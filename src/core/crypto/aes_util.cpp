// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <openssl/err.h>
#include <openssl/evp.h>
#include "common/assert.h"
#include "common/logging.h"
#include "core/crypto/aes_util.h"
#include "core/crypto/key_manager.h"

#if defined(__aarch64__) && defined(__ARM_FEATURE_AES)
#define EDEN_AES_ARM64_CE 1
#include <arm_neon.h>
#if defined(__linux__) || defined(__ANDROID__)
#include <sys/auxv.h>
#endif
#else
#define EDEN_AES_ARM64_CE 0
#endif

namespace Core::Crypto {
namespace {
using NintendoTweak = std::array<u8, 16>;
constexpr std::size_t AesBlockBytes = 16;

NintendoTweak CalculateNintendoTweak(std::size_t sector_id) {
    NintendoTweak out{};
    for (std::size_t i = 0xF; i <= 0xF; --i) {
        out[i] = sector_id & 0xFF;
        sector_id >>= 8;
    }
    return out;
}

#if EDEN_AES_ARM64_CE
// AES with the ARMv8 Cryptography Extensions. The OpenSSL this project ships for Android is built
// without assembly, so every NCA and RomFS byte went through its C AES on the one FS thread; the
// instructions below run the same ciphers 10-20x faster. Checked against OpenSSL on first use.
namespace CE {
constexpr std::array<u8, 256> SBOX = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

struct RoundKeys {
    std::array<uint8x16_t, 15> enc{};
    std::array<uint8x16_t, 15> dec{};
    int rounds{};
};

u32 SubWord(u32 w) {
    return (u32(SBOX[(w >> 24) & 0xFF]) << 24) | (u32(SBOX[(w >> 16) & 0xFF]) << 16) |
           (u32(SBOX[(w >> 8) & 0xFF]) << 8) | u32(SBOX[w & 0xFF]);
}

u32 RotWord(u32 w) {
    return (w << 8) | (w >> 24);
}

u32 LoadBE(const u8* p) {
    return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | u32(p[3]);
}

RoundKeys ExpandKey(const u8* key, std::size_t key_bytes) {
    RoundKeys rk{};
    const int nk = static_cast<int>(key_bytes / 4);
    rk.rounds = nk + 6;
    std::array<u32, 60> w{};
    for (int i = 0; i < nk; ++i) {
        w[i] = LoadBE(key + 4 * i);
    }
    u32 rcon = 0x01000000;
    for (int i = nk; i < 4 * (rk.rounds + 1); ++i) {
        u32 t = w[i - 1];
        if (i % nk == 0) {
            t = SubWord(RotWord(t)) ^ rcon;
            rcon = (rcon << 1) ^ ((rcon & 0x80000000) ? 0x1b000000 : 0);
        } else if (nk > 6 && i % nk == 4) {
            t = SubWord(t);
        }
        w[i] = w[i - nk] ^ t;
    }
    for (int r = 0; r <= rk.rounds; ++r) {
        std::array<u8, 16> bytes{};
        for (int c = 0; c < 4; ++c) {
            const u32 word = w[4 * r + c];
            bytes[4 * c + 0] = static_cast<u8>(word >> 24);
            bytes[4 * c + 1] = static_cast<u8>(word >> 16);
            bytes[4 * c + 2] = static_cast<u8>(word >> 8);
            bytes[4 * c + 3] = static_cast<u8>(word);
        }
        rk.enc[r] = vld1q_u8(bytes.data());
    }
    // Equivalent inverse cipher: reversed keys, InvMixColumns on the middle ones.
    rk.dec[0] = rk.enc[rk.rounds];
    for (int r = 1; r < rk.rounds; ++r) {
        rk.dec[r] = vaesimcq_u8(rk.enc[rk.rounds - r]);
    }
    rk.dec[rk.rounds] = rk.enc[0];
    return rk;
}

inline uint8x16_t EncryptBlock(const RoundKeys& rk, uint8x16_t x) {
    for (int r = 0; r < rk.rounds - 1; ++r) {
        x = vaesmcq_u8(vaeseq_u8(x, rk.enc[r]));
    }
    return veorq_u8(vaeseq_u8(x, rk.enc[rk.rounds - 1]), rk.enc[rk.rounds]);
}

inline uint8x16_t DecryptBlock(const RoundKeys& rk, uint8x16_t x) {
    for (int r = 0; r < rk.rounds - 1; ++r) {
        x = vaesimcq_u8(vaesdq_u8(x, rk.dec[r]));
    }
    return veorq_u8(vaesdq_u8(x, rk.dec[rk.rounds - 1]), rk.dec[rk.rounds]);
}

// Big-endian 128-bit increment, as OpenSSL's AES-CTR does it.
inline void IncrementCounter(std::array<u8, 16>& ctr) {
    for (int i = 15; i >= 0; --i) {
        if (++ctr[i] != 0) {
            break;
        }
    }
}

void Ecb(const RoundKeys& rk, const u8* src, std::size_t size, u8* dst, bool encrypt) {
    for (std::size_t i = 0; i + AesBlockBytes <= size; i += AesBlockBytes) {
        const uint8x16_t x = vld1q_u8(src + i);
        vst1q_u8(dst + i, encrypt ? EncryptBlock(rk, x) : DecryptBlock(rk, x));
    }
}

void Ctr(const RoundKeys& rk, std::array<u8, 16> ctr, const u8* src, std::size_t size, u8* dst) {
    std::size_t i = 0;
    // Four independent blocks keep the AES pipeline full.
    for (; i + 4 * AesBlockBytes <= size; i += 4 * AesBlockBytes) {
        std::array<uint8x16_t, 4> ks;
        for (auto& k : ks) {
            k = EncryptBlock(rk, vld1q_u8(ctr.data()));
            IncrementCounter(ctr);
        }
        for (std::size_t b = 0; b < 4; ++b) {
            vst1q_u8(dst + i + b * AesBlockBytes,
                     veorq_u8(vld1q_u8(src + i + b * AesBlockBytes), ks[b]));
        }
    }
    for (; i < size; i += AesBlockBytes) {
        const uint8x16_t ks = EncryptBlock(rk, vld1q_u8(ctr.data()));
        IncrementCounter(ctr);
        const std::size_t n = std::min(AesBlockBytes, size - i);
        if (n == AesBlockBytes) {
            vst1q_u8(dst + i, veorq_u8(vld1q_u8(src + i), ks));
        } else {
            std::array<u8, 16> k{};
            vst1q_u8(k.data(), ks);
            for (std::size_t b = 0; b < n; ++b) {
                dst[i + b] = src[i + b] ^ k[b];
            }
        }
    }
}

// Multiply the tweak by x in GF(2^128), IEEE P1619 (little-endian carry chain).
inline uint8x16_t NextTweak(uint8x16_t t) {
    std::array<u8, 16> b;
    vst1q_u8(b.data(), t);
    u8 carry = 0;
    for (int i = 0; i < 16; ++i) {
        const u8 next = b[i] >> 7;
        b[i] = static_cast<u8>((b[i] << 1) | carry);
        carry = next;
    }
    if (carry) {
        b[0] ^= 0x87;
    }
    return vld1q_u8(b.data());
}

void Xts(const RoundKeys& data_key, const RoundKeys& tweak_key, const std::array<u8, 16>& iv,
         const u8* src, std::size_t size, u8* dst, bool encrypt) {
    uint8x16_t tweak = EncryptBlock(tweak_key, vld1q_u8(iv.data()));
    for (std::size_t i = 0; i + AesBlockBytes <= size; i += AesBlockBytes) {
        const uint8x16_t x = veorq_u8(vld1q_u8(src + i), tweak);
        const uint8x16_t y = encrypt ? EncryptBlock(data_key, x) : DecryptBlock(data_key, x);
        vst1q_u8(dst + i, veorq_u8(y, tweak));
        tweak = NextTweak(tweak);
    }
}

bool CpuHasAes() {
#if defined(__APPLE__)
    return true;
#elif defined(__linux__) || defined(__ANDROID__)
    constexpr unsigned long hwcap_aes = 1UL << 3; // HWCAP_AES
    return (getauxval(AT_HWCAP) & hwcap_aes) != 0;
#else
    return false;
#endif
}
} // namespace CE
#endif
} // Anonymous namespace

struct CipherContext {
    EVP_CIPHER_CTX* encryption_context = nullptr;
    EVP_CIPHER_CTX* decryption_context = nullptr;
    EVP_CIPHER* cipher = nullptr;
    Mode mode{};
    std::array<u8, 16> iv{};
#if EDEN_AES_ARM64_CE
    bool use_ce = false;
    CE::RoundKeys ce_key{};   // ECB / CTR key, or the XTS data key
    CE::RoundKeys ce_tweak{}; // XTS tweak key
#endif
};

static inline const std::string GetCipherName(Mode mode, u32 key_size) {
    std::string cipher;
    std::size_t effective_bits = key_size * 8;
    switch (mode) {
    case Mode::CTR:
        cipher = "CTR";
        break;
    case Mode::ECB:
        cipher = "ECB";
        break;
    case Mode::XTS:
        cipher = "XTS";
        effective_bits /= 2;
        break;
    default:
        UNREACHABLE();
    }
    return std::format("AES-{}-{}", effective_bits, cipher);
};

static EVP_CIPHER *GetCipher(Mode mode, u32 key_size) {
    static auto const fetch_cipher = [](Mode m, u32 k) {
        return EVP_CIPHER_fetch(nullptr, GetCipherName(m, k).c_str(), nullptr);
    };
    static const struct {
        EVP_CIPHER* ctr_16 = fetch_cipher(Mode::CTR, 16);
        EVP_CIPHER* ecb_16 = fetch_cipher(Mode::ECB, 16);
        EVP_CIPHER* xts_16 = fetch_cipher(Mode::XTS, 16);
        EVP_CIPHER* ctr_32 = fetch_cipher(Mode::CTR, 32);
        EVP_CIPHER* ecb_32 = fetch_cipher(Mode::ECB, 32);
        EVP_CIPHER* xts_32 = fetch_cipher(Mode::XTS, 32);
    } ciphers = {};
    switch (mode) {
    case Mode::CTR:
        return key_size == 16 ? ciphers.ctr_16 : ciphers.ctr_32;
    case Mode::ECB:
        return key_size == 16 ? ciphers.ecb_16 : ciphers.ecb_32;
    case Mode::XTS:
        return key_size == 16 ? ciphers.xts_16 : ciphers.xts_32;
    default:
        UNIMPLEMENTED();
    }
    return nullptr;
}

namespace {
void OpenSslTranscode(CipherContext& ctx, const u8* src, std::size_t size, u8* dest, Op op) {
    auto* const context = op == Op::Encrypt ? ctx.encryption_context : ctx.decryption_context;

    // reset
    ASSERT(EVP_CipherInit_ex(context, nullptr, nullptr, nullptr, nullptr, -1));

    const int block_size = EVP_CIPHER_CTX_get_block_size(context);
    ASSERT(block_size > 0 && block_size <= int(AesBlockBytes));

    const std::size_t whole_block_bytes = size - (size % block_size);
    int written = 0;

    if (whole_block_bytes != 0) {
        ASSERT(EVP_CipherUpdate(context, dest, &written, src, static_cast<int>(whole_block_bytes)));

        if (std::size_t(written) != whole_block_bytes) {
            LOG_WARNING(Crypto, "Not all data was processed requested={:016X}, actual={:016X}.",
                        whole_block_bytes, written);
        }
    }

    // tail
    const std::size_t tail = size - whole_block_bytes;
    if (tail == 0)
        return;

    std::array<u8, AesBlockBytes> tail_buffer{};
    std::memcpy(tail_buffer.data(), src + whole_block_bytes, tail);

    int tail_written = 0;

    ASSERT(EVP_CipherUpdate(context, tail_buffer.data(), &tail_written, tail_buffer.data(), block_size));

    if (tail_written != block_size) {
        LOG_WARNING(Crypto, "Tail block not fully processed requested={:016X}, actual={:016X}.",
                    block_size, tail_written);
    }

    std::memcpy(dest + whole_block_bytes, tail_buffer.data(), tail);
}

#if EDEN_AES_ARM64_CE
void CeTranscode(CipherContext& ctx, const u8* src, std::size_t size, u8* dest, Op op) {
    const bool encrypt = op == Op::Encrypt;
    switch (ctx.mode) {
    case Mode::CTR:
        CE::Ctr(ctx.ce_key, ctx.iv, src, size, dest);
        break;
    case Mode::XTS:
        CE::Xts(ctx.ce_key, ctx.ce_tweak, ctx.iv, src, size, dest, encrypt);
        break;
    case Mode::ECB: {
        const std::size_t whole = size - (size % AesBlockBytes);
        CE::Ecb(ctx.ce_key, src, whole, dest, encrypt);
        if (const std::size_t tail = size - whole; tail != 0) {
            // Same as the OpenSSL path: the tail is processed as a zero-padded block.
            std::array<u8, AesBlockBytes> block{};
            std::memcpy(block.data(), src + whole, tail);
            CE::Ecb(ctx.ce_key, block.data(), AesBlockBytes, block.data(), encrypt);
            std::memcpy(dest + whole, block.data(), tail);
        }
        break;
    }
    }
}

// One comparison against OpenSSL per mode and key size; a mismatch keeps that cipher on OpenSSL.
bool CeMatchesOpenSsl(CipherContext& ctx) {
    std::array<u8, 4096 + 7> plain{};
    for (std::size_t i = 0; i < plain.size(); ++i) {
        plain[i] = static_cast<u8>(i * 131 + 17);
    }
    std::array<u8, 16> iv{};
    for (std::size_t i = 0; i < iv.size(); ++i) {
        iv[i] = static_cast<u8>(0xA0 + i);
    }
    ctx.iv = iv;
    ASSERT(EVP_CipherInit_ex(ctx.encryption_context, nullptr, nullptr, nullptr, iv.data(), -1));
    ASSERT(EVP_CipherInit_ex(ctx.decryption_context, nullptr, nullptr, nullptr, iv.data(), -1));
    const std::size_t size = ctx.mode == Mode::CTR ? plain.size() : 4096;
    std::array<u8, 4096 + 7> a{}, b{}, back{};
    OpenSslTranscode(ctx, plain.data(), size, a.data(), Op::Encrypt);
    CeTranscode(ctx, plain.data(), size, b.data(), Op::Encrypt);
    if (std::memcmp(a.data(), b.data(), size) != 0) {
        return false;
    }
    CeTranscode(ctx, a.data(), size, back.data(), Op::Decrypt);
    return std::memcmp(back.data(), plain.data(), size) == 0;
}
#endif
} // Anonymous namespace

template <typename Key>
Crypto::AESCipher<Key>::AESCipher(Key key, Mode mode) : ctx(std::make_unique<CipherContext>()) {
    ctx->encryption_context = EVP_CIPHER_CTX_new();
    ctx->decryption_context = EVP_CIPHER_CTX_new();
    ctx->cipher = GetCipher(mode, sizeof(Key));
    ctx->mode = mode;
    if (ctx->cipher) {
        EVP_CIPHER_up_ref(ctx->cipher);
    } else {
        UNIMPLEMENTED();
    }

    ASSERT(ctx->encryption_context && ctx->decryption_context && ctx->cipher && "OpenSSL cipher context failed init!");
    // now init ciphers
    ASSERT(EVP_CipherInit_ex2(ctx->encryption_context, ctx->cipher, key.data(), NULL, 1, NULL));
    ASSERT(EVP_CipherInit_ex2(ctx->decryption_context, ctx->cipher, key.data(), NULL, 0, NULL));

    EVP_CIPHER_CTX_set_padding(ctx->encryption_context, 0);
    EVP_CIPHER_CTX_set_padding(ctx->decryption_context, 0);

#if EDEN_AES_ARM64_CE
    static const bool cpu_has_aes = CE::CpuHasAes();
    if (!cpu_has_aes) {
        return;
    }
    if (mode == Mode::XTS) {
        ctx->ce_key = CE::ExpandKey(key.data(), sizeof(Key) / 2);
        ctx->ce_tweak = CE::ExpandKey(key.data() + sizeof(Key) / 2, sizeof(Key) / 2);
    } else {
        ctx->ce_key = CE::ExpandKey(key.data(), sizeof(Key));
    }
    // Verified once per (mode, key size): index 0..5.
    static std::array<int, 6> verified{}; // 0 = untested, 1 = ok, -1 = mismatch
    const std::size_t slot = static_cast<std::size_t>(mode == Mode::CTR ? 0 : mode == Mode::ECB ? 1 : 2) +
                             (sizeof(Key) == 16 ? 0 : 3);
    if (verified[slot] == 0) {
        verified[slot] = CeMatchesOpenSsl(*ctx) ? 1 : -1;
        // Back to the zero IV a fresh context has.
        SetIV(std::array<u8, 16>{});
        if (verified[slot] < 0) {
            LOG_ERROR(Crypto, "ARMv8 AES {} with a {}-byte key does not match OpenSSL; using OpenSSL",
                      GetCipherName(mode, sizeof(Key)), sizeof(Key));
        } else {
            LOG_INFO(Crypto, "ARMv8 AES in use for {}", GetCipherName(mode, sizeof(Key)));
        }
    }
    ctx->use_ce = verified[slot] > 0;
#endif
}

template <typename Key>
AESCipher<Key>::~AESCipher() {
    EVP_CIPHER_CTX_free(ctx->encryption_context);
    EVP_CIPHER_CTX_free(ctx->decryption_context);
    EVP_CIPHER_free(ctx->cipher);
}

template <typename Key>
void AESCipher<Key>::Transcode(const u8* src, std::size_t size, u8* dest, Op op) const {
    if (size == 0)
        return;
#if EDEN_AES_ARM64_CE
    if (ctx->use_ce) {
        CeTranscode(*ctx, src, size, dest, op);
        return;
    }
#endif
    OpenSslTranscode(*ctx, src, size, dest, op);
}

template <typename Key>
void AESCipher<Key>::XTSTranscode(const u8* src, std::size_t size, u8* dest, std::size_t sector_id, std::size_t sector_size, Op op) {
    ASSERT(size % sector_size == 0 && "XTS decryption size must be a multiple of sector size.");
    for (std::size_t i = 0; i < size; i += sector_size) {
        SetIV(CalculateNintendoTweak(sector_id++));
        Transcode(src + i, sector_size, dest + i, op);
    }
}

template <typename Key>
void AESCipher<Key>::SetIV(std::span<const u8> data) {
    if (data.size() >= ctx->iv.size()) {
        std::memcpy(ctx->iv.data(), data.data(), ctx->iv.size());
    }
    const int ret_enc = EVP_CipherInit_ex(ctx->encryption_context, nullptr, nullptr, nullptr, data.data(), -1);
    const int ret_dec = EVP_CipherInit_ex(ctx->decryption_context, nullptr, nullptr, nullptr, data.data(), -1);
    ASSERT(ret_enc == 1 && ret_dec == 1 && "Failed to set IV on OpenSSL contexts");
}

template class AESCipher<Key128>;
template class AESCipher<Key256>;
} // namespace Core::Crypto
