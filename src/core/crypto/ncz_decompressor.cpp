// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <limits>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include <zstd.h>

#include "common/common_types.h"
#include "common/fs/file.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/zstd_compression.h"
#include "core/crypto/aes_util.h"
#include "core/crypto/key_manager.h"
#include "core/crypto/ncz_decompressor.h"
#include "core/file_sys/vfs/vfs.h"

namespace Core::Crypto {

namespace {

constexpr u64 HeaderSize = 0x4000;
constexpr std::size_t SectionWire = 0x40;
constexpr std::size_t BlockFixed = 0x18;
constexpr std::size_t Chunk = 0x100000;
// Upper bound on NCZ section entries (64 bytes each, so the table is at most 4 MiB). A base NCA
// has a handful of sections, but nsz stores an update's patch (BKTR) data one entry per
// subsection, which is thousands of them.
constexpr u64 MaxSectionCount = 0x10000;
constexpr std::array<u8, 8> SectionsMagic{'N', 'C', 'Z', 'S', 'E', 'C', 'T', 'N'};
constexpr std::array<u8, 8> BlockMagic{'N', 'C', 'Z', 'B', 'L', 'O', 'C', 'K'};

struct Section {
    s64 offset, size;
    u64 crypto_type;
    Key128 key, counter;
    bool Ctr() const { return crypto_type == 3 || crypto_type == 4; }
};

struct BlockHeader {
    u32 exponent;
    u64 decompressed_size;
    std::vector<u32> sizes;
};

u64 Le(const u8* p, std::size_t n) {
    u64 v = 0;
    for (std::size_t i = n; i-- > 0;) v = (v << 8) | p[i];
    return v;
}

// Mirrors nsz AESCTR.seek(): upper 8 bytes = stored counter, lower 8 = absolute NCA offset / 16.
void SetIv(Key128& iv, const Key128& base, u64 off) {
    iv = base;
    u64 blk = off >> 4;
    for (std::size_t i = 0; i < 8; ++i) { iv[15 - i] = static_cast<u8>(blk & 0xFF); blk >>= 8; }
}

// Parse header + section table + optional block header. Returns payload_start.
std::optional<std::pair<std::vector<Section>, std::optional<BlockHeader>>> ParseHeader(
    const FileSys::VirtualFile& f, u64& payload_start) {
    const u64 fsize = f->GetSize();
    if (fsize < HeaderSize + 16) return std::nullopt;

    const auto head = f->ReadBytes(16, HeaderSize);
    if (head.size() != 16 || !std::equal(head.begin(), head.begin() + 8, SectionsMagic.begin()))
        return std::nullopt;
    const u64 count = Le(head.data() + 8, 8);
    if (count == 0 || count > MaxSectionCount || count * SectionWire > fsize - HeaderSize - 16)
        return std::nullopt;

    const auto table = f->ReadBytes(count * SectionWire, HeaderSize + 16);
    if (table.size() != count * SectionWire) return std::nullopt;

    std::vector<Section> sections;
    sections.reserve(count + 1);
    for (u64 i = 0; i < count; ++i) {
        const u8* e = table.data() + i * SectionWire;
        Section s{};
        s.offset = static_cast<s64>(Le(e, 8));
        s.size = static_cast<s64>(Le(e + 8, 8));
        if (s.offset < 0 || s.size < 0 || s.offset > std::numeric_limits<s64>::max() - s.size)
            return std::nullopt;
        s.crypto_type = Le(e + 16, 8);
        std::memcpy(s.key.data(), e + 32, 16);
        std::memcpy(s.counter.data(), e + 48, 16);
        sections.push_back(s);
    }

    const u64 block_start = HeaderSize + 16 + count * SectionWire;
    const auto fixed = f->ReadBytes(BlockFixed, block_start);
    std::optional<BlockHeader> bh;
    if (fixed.size() == BlockFixed &&
        std::equal(fixed.begin(), fixed.begin() + 8, BlockMagic.begin())) {
        BlockHeader b{};
        b.exponent = fixed[11];
        const s32 n = static_cast<s32>(Le(fixed.data() + 12, 4));
        if (n <= 0) return std::nullopt;
        b.decompressed_size = Le(fixed.data() + 16, 8);
        const auto sizes = f->ReadBytes(static_cast<u64>(n) * 4, block_start + BlockFixed);
        if (sizes.size() != static_cast<u64>(n) * 4) return std::nullopt;
        b.sizes.resize(n);
        for (s32 i = 0; i < n; ++i) b.sizes[i] = static_cast<u32>(Le(sizes.data() + i * 4, 4));
        payload_start = block_start + BlockFixed + static_cast<u64>(n) * 4;
        bh = std::move(b);
    } else {
        payload_start = block_start;
    }

    if (!sections.empty() && sections[0].offset > static_cast<s64>(HeaderSize)) {
        Section fake{};
        fake.offset = static_cast<s64>(HeaderSize);
        fake.size = sections[0].offset - static_cast<s64>(HeaderSize);
        sections.insert(sections.begin(), fake);
    }
    return std::make_pair(std::move(sections), std::move(bh));
}

// Sequential reader over independently-compressed blocks. Uses a direct-mapped cache (like
// NCZVirtualFile) instead of an LRU list: block_index % CacheSlots picks the slot, so a hit is
// O(1). Reuses one ZSTD_DCtx for the lifetime of the reader.
class BlockReader {
public:
    static constexpr std::size_t CacheSlots = 32;

    BlockReader(FileSys::VirtualFile f, const BlockHeader& bh, u64 payload_start)
        : source{std::move(f)}, block_size{1ULL << bh.exponent}, sizes{bh.sizes},
          decompressed_size{bh.decompressed_size} {
        if (bh.exponent < 14 || bh.exponent > 32 || sizes.empty())
            throw std::runtime_error("bad NCZBLOCK header");
        u64 off = payload_start;
        offsets.reserve(sizes.size());
        for (u32 s : sizes) { offsets.push_back(off); off += s; }
        dctx = ZSTD_createDCtx();
        if (dctx == nullptr) throw std::runtime_error("ZSTD_createDCtx failed");
        cache.resize(CacheSlots);
        for (auto& slot : cache) slot.id = ~0ULL;
    }

    ~BlockReader() {
        if (dctx != nullptr) ZSTD_freeDCtx(dctx);
    }

    BlockReader(const BlockReader&) = delete;
    BlockReader& operator=(const BlockReader&) = delete;

    std::vector<std::string> GetSourcePaths() const {
        return source->GetSourcePaths();
    }

    std::size_t ReadAt(u64 position, std::span<u8> out) {
        std::size_t done = 0;
        while (done < out.size() && position < decompressed_size) {
            const u64 id = position / block_size;
            if (id >= sizes.size()) break;
            const std::vector<u8>* block = GetBlock(id);
            if (block == nullptr) break;
            const u64 in = position - id * block_size;
            if (in >= block->size()) break;
            const std::size_t n = static_cast<std::size_t>(
                std::min<u64>(out.size() - done, block->size() - in));
            std::memcpy(out.data() + done, block->data() + in, n);
            done += n;
            position += n;
        }
        return done;
    }

private:
    struct Slot {
        u64 id = ~0ULL;
        std::vector<u8> data;
    };

    const std::vector<u8>* GetBlock(u64 id) {
        Slot& slot = cache[id % CacheSlots];
        if (slot.id == id) {
            return &slot.data;  // O(1) hit
        }
        std::vector<u8> buffer;
        buffer.swap(slot.data);  // reuse existing allocation
        if (!DecodeBlock(id, buffer)) {
            return nullptr;
        }
        slot.data = std::move(buffer);
        slot.id = id;
        return &slot.data;
    }

    bool DecodeBlock(u64 id, std::vector<u8>& buffer) {
        u64 want = block_size;
        if (id + 1 == sizes.size()) {
            const u64 rem = decompressed_size % block_size;
            if (rem > 0) want = rem;  // avoid the 0-size bug from the original nsz
        }
        try { buffer.resize(static_cast<std::size_t>(want)); }
        catch (const std::bad_alloc&) { return false; }
        const u32 csize = sizes[id];
        if (csize < want) {
            const auto comp = source->ReadBytes(csize, offsets[id]);
            if (comp.size() != csize) return false;
            const std::size_t res = ZSTD_decompressDCtx(
                dctx, buffer.data(), buffer.size(), comp.data(), comp.size());
            if (ZSTD_isError(res) || res != want) {
                LOG_ERROR(Crypto, "Failed to decompress NCZBLOCK block {}: {}", id,
                          ZSTD_isError(res) ? ZSTD_getErrorName(res) : "size mismatch");
                return false;
            }
        } else if (source->Read(buffer.data(), buffer.size(), offsets[id]) != buffer.size()) {
            return false;
        }
        return true;
    }

    FileSys::VirtualFile source;
    u64 block_size;
    std::vector<u64> offsets;
    std::vector<u32> sizes;
    u64 decompressed_size;
    ZSTD_DCtx* dctx = nullptr;
    std::vector<Slot> cache;  // direct-mapped, CacheSlots entries
};

class BlockNczFile final : public FileSys::VfsFile {
public:
    BlockNczFile(std::string name_, std::vector<u8> hdr_, std::vector<Section> sections_,
                 std::unique_ptr<BlockReader> body_)
        : name{std::move(name_)}, hdr{std::move(hdr_)}, sections{std::move(sections_)},
          body{std::move(body_)} {
        nca_size = HeaderSize;
        for (const auto& s : sections)
            nca_size = std::max<u64>(nca_size, static_cast<u64>(s.offset + s.size));
    }

    std::string GetName() const override { return name; }
    std::size_t GetSize() const override { return static_cast<std::size_t>(nca_size); }
    bool Resize(std::size_t) override { return false; }
    FileSys::VirtualDir GetContainingDirectory() const override { return nullptr; }
    std::vector<std::string> GetSourcePaths() const override {
        return body->GetSourcePaths();
    }
    bool IsWritable() const override { return false; }
    bool IsReadable() const override { return true; }

    // NCZ stores CTR sections decrypted; this file serves them as-is, and NcaFileSystemDriver
    // skips its AES layers because of this flag.
    bool HasDecryptedSections() const override { return true; }

    std::size_t Read(u8* data, std::size_t len, std::size_t off) const override {
        if (off >= nca_size) return 0;
        len = static_cast<std::size_t>(std::min<u64>(len, nca_size - off));
        const u64 end = off + len;
        u64 pos = off;
        if (pos < HeaderSize) {
            const u64 n = std::min<u64>(end, HeaderSize) - pos;
            std::memcpy(data, hdr.data() + pos, static_cast<std::size_t>(n));
            pos += n;
        }
        std::scoped_lock lock{mutex};
        // Sections are sorted by offset; find the first one whose stop > pos.
        auto it = std::lower_bound(
            sections.begin(), sections.end(), pos, [](const Section& s, u64 p) {
                const u64 b = std::max<u64>(static_cast<u64>(s.offset), HeaderSize);
                const u64 e = std::max<u64>(static_cast<u64>(s.offset) + static_cast<u64>(s.size), b);
                return e <= p;
            });
        for (; it != sections.end() && pos < end; ++it) {
            const auto& s = *it;
            const u64 begin = std::max<u64>(static_cast<u64>(s.offset), HeaderSize);
            const u64 stop = std::max<u64>(static_cast<u64>(s.offset + s.size), begin);
            if (stop <= pos) continue;
            if (pos < begin) {
                const u64 g = std::min(end, begin);
                std::memset(data + (pos - off), 0, static_cast<std::size_t>(g - pos));
                pos = g;
                if (pos >= end) break;
            }
            const u64 pe = std::min(end, stop);
            const std::span<u8> piece(data + (pos - off), static_cast<std::size_t>(pe - pos));
            if (body->ReadAt(pos - HeaderSize, piece) != piece.size())
                return static_cast<std::size_t>(pos - off);
            // No CTR here: NCZ body is already decrypted, and NcaFileSystemDriver skips AES.
            pos = pe;
        }
        return len;
    }

    std::size_t Write(const u8*, std::size_t, std::size_t) override { return 0; }
    bool Rename(std::string_view) override { return false; }

private:
    const std::string name;
    const std::vector<u8> hdr;
    const std::vector<Section> sections;
    const std::unique_ptr<BlockReader> body;
    mutable std::mutex mutex;
    u64 nca_size{};
};

// The NCA unpacked from a solid NCZ, under the NCZ's name and source paths.
class CachedNcaFile final : public FileSys::VfsFile {
public:
    CachedNcaFile(std::string name_, std::vector<std::string> sources_,
                  const std::filesystem::path& path)
        : name{std::move(name_)}, sources{std::move(sources_)},
          file{path, Common::FS::FileAccessMode::Read, Common::FS::FileType::BinaryFile},
          size{file.IsOpen() ? file.GetSize() : 0} {}

    bool IsOpen() const { return file.IsOpen(); }
    std::string GetName() const override { return name; }
    std::size_t GetSize() const override { return static_cast<std::size_t>(size); }
    bool Resize(std::size_t) override { return false; }
    FileSys::VirtualDir GetContainingDirectory() const override { return nullptr; }
    std::vector<std::string> GetSourcePaths() const override { return sources; }
    bool IsWritable() const override { return false; }
    bool IsReadable() const override { return true; }

    std::size_t Read(u8* data, std::size_t len, std::size_t off) const override {
        std::scoped_lock lock{mutex};
        if (off >= size || !file.Seek(static_cast<s64>(off))) return 0;
        return file.ReadSpan(
            std::span<u8>{data, static_cast<std::size_t>(std::min<u64>(len, size - off))});
    }

    std::size_t Write(const u8*, std::size_t, std::size_t) override { return 0; }
    bool Rename(std::string_view) override { return false; }

private:
    const std::string name;
    const std::vector<std::string> sources;
    const Common::FS::IOFile file;
    const u64 size;
    mutable std::mutex mutex;
};

// Writes the NCA a solid NCZ holds to path: its header copy, then each section's bytes from the
// one zstd stream, encrypted again where the section is AES-CTR, so the file is the NCA as dumped.
bool UnpackSolid(const FileSys::VirtualFile& f, const std::vector<Section>& sections,
                 u64 payload_start, const std::filesystem::path& path) {
    Common::FS::IOFile out{path, Common::FS::FileAccessMode::Write,
                           Common::FS::FileType::BinaryFile};
    const auto header = f->ReadBytes(HeaderSize, 0);
    if (!out.IsOpen() || header.size() != HeaderSize ||
        out.WriteSpan(std::span<const u8>{header}) != HeaderSize) {
        return false;
    }
    const std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> dctx{ZSTD_createDCtx(),
                                                                   ZSTD_freeDCtx};
    if (!dctx) return false;
    ZSTD_DCtx_setParameter(dctx.get(), ZSTD_d_windowLogMax, 31);
    std::vector<u8> in_buf(Chunk), out_buf(Chunk * 4);
    ZSTD_inBuffer in{in_buf.data(), 0, 0};
    u64 read_pos = payload_start;
    const u64 file_size = f->GetSize();
    for (const auto& s : sections) {
        const u64 begin = std::max<u64>(static_cast<u64>(s.offset), HeaderSize);
        const u64 end = std::max<u64>(static_cast<u64>(s.offset + s.size), begin);
        std::optional<AESCipher<Key128>> cipher;
        if (s.Ctr()) cipher.emplace(s.key, Mode::CTR);
        for (u64 pos = begin; pos < end;) {
            ZSTD_outBuffer o{out_buf.data(),
                             static_cast<std::size_t>(std::min<u64>(end - pos, out_buf.size())),
                             0};
            while (o.pos < o.size) {
                if (in.pos == in.size) {
                    const auto want = std::min<u64>(Chunk, file_size - std::min(read_pos, file_size));
                    const std::size_t n =
                        want == 0 ? 0
                                  : f->Read(in_buf.data(), static_cast<std::size_t>(want),
                                            static_cast<std::size_t>(read_pos));
                    if (n == 0) {
                        LOG_ERROR(Crypto, "Solid NCZ {}: truncated", f->GetName());
                        return false;
                    }
                    read_pos += n;
                    in.size = n;
                    in.pos = 0;
                }
                if (const auto r = ZSTD_decompressStream(dctx.get(), &o, &in); ZSTD_isError(r)) {
                    LOG_ERROR(Crypto, "Solid NCZ {}: {}", f->GetName(), ZSTD_getErrorName(r));
                    return false;
                }
            }
            if (cipher) {
                Key128 iv{};
                SetIv(iv, s.counter, pos);
                cipher->SetIV(iv);
                cipher->Transcode(out_buf.data(), o.pos, out_buf.data(), Op::Encrypt);
            }
            if (out.WriteSpan(std::span<const u8>{out_buf.data(), o.pos}) != o.pos) return false;
            pos += o.pos;
        }
    }
    return out.Flush();
}

thread_local bool unpack_allowed = false;

// ponytail: a solid NCZ (nsz without -B) has no random access, so a launch or an install unpacks
// the whole NCA once into <cache>/ncz/<id>.nca: as much disk as the NCA, kept for later launches
// until the cache is cleared, and that first launch waits for it. Block NCZ (nsz -B) needs no
// copy. Nothing bounds the folder; evict per game if cache space matters.
std::shared_ptr<CachedNcaFile> UnpackSolidCached(const FileSys::VirtualFile& f,
                                                 const std::string& name,
                                                 const std::vector<Section>& sections,
                                                 u64 payload_start, u64 nca_size, bool unpack) {
    const auto dir = Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) / "ncz";
    const auto path = dir / name;
    static std::mutex unpack_mutex; // one unpack at a time; a second opener reuses its result
    std::scoped_lock lock{unpack_mutex};
    std::error_code ec;
    if (std::filesystem::file_size(path, ec) != nca_size || ec) {
        if (!unpack) return nullptr;
        // An unpack cut short (killed, crashed) leaves a .part: nothing reads one, so drop them all.
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (entry.path().extension() == ".part") std::filesystem::remove(entry.path(), ec);
        }
        std::filesystem::create_directories(dir, ec);
        constexpr u64 Reserve = 2ULL << 30; // leave the device room to keep working
        const auto space = std::filesystem::space(dir, ec);
        if (ec || space.available < Reserve || nca_size > space.available - Reserve) {
            LOG_CRITICAL(Crypto,
                         "Not enough free space to unpack solid NCZ {}: needs {} MiB plus 2048 "
                         "MiB to spare in {}, {} MiB free. Free space or recompress with nsz -B.",
                         name, nca_size >> 20, Common::FS::PathToUTF8String(dir),
                         ec ? 0 : space.available >> 20);
            return nullptr;
        }
        auto part = path;
        part += ".part";
        LOG_INFO(Crypto, "Unpacking solid NCZ {} ({} bytes) into {}", name, nca_size,
                 Common::FS::PathToUTF8String(dir));
        const auto start = std::chrono::steady_clock::now();
        bool ok = UnpackSolid(f, sections, payload_start, part);
        if (ok) {
            std::filesystem::rename(part, path, ec);
            ok = !ec;
        }
        if (!ok) {
            LOG_ERROR(Crypto, "Could not unpack solid NCZ {} into {}", name,
                      Common::FS::PathToUTF8String(dir));
            std::filesystem::remove(part, ec);
            return nullptr;
        }
        LOG_INFO(Crypto, "Unpacked {} in {} ms", name,
                 std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - start)
                     .count());
    }
    auto file = std::make_shared<CachedNcaFile>(name, f->GetSourcePaths(), path);
    return file->IsOpen() && file->GetSize() == nca_size ? file : nullptr;
}

// Live SolidNczFiles, so IsNczPendingUnpack can tell one apart without RTTI.
std::mutex solid_files_mutex;
std::set<const FileSys::VfsFile*> solid_files;

// A solid NCZ's NCA: its header copy at once, the rest from the unpacked cache file. Until that
// exists, only a thread inside a SolidNczUnpackScope (launch, install) unpacks it; elsewhere
// (library scans, metadata) reads past the header come back short and nothing is written.
class SolidNczFile final : public FileSys::VfsFile {
public:
    SolidNczFile(std::string name_, FileSys::VirtualFile source_, std::vector<u8> header_,
                 std::vector<Section> sections_, u64 payload_start_)
        : name{std::move(name_)}, source{std::move(source_)}, header{std::move(header_)},
          sections{std::move(sections_)}, payload_start{payload_start_} {
        for (const auto& s : sections)
            nca_size = std::max<u64>(nca_size, static_cast<u64>(s.offset + s.size));
        std::scoped_lock lock{solid_files_mutex};
        solid_files.insert(this);
    }

    ~SolidNczFile() override {
        std::scoped_lock lock{solid_files_mutex};
        solid_files.erase(this);
    }

    std::string GetName() const override { return name; }
    std::size_t GetSize() const override { return static_cast<std::size_t>(nca_size); }
    bool Resize(std::size_t) override { return false; }
    FileSys::VirtualDir GetContainingDirectory() const override { return nullptr; }
    std::vector<std::string> GetSourcePaths() const override { return source->GetSourcePaths(); }
    bool IsWritable() const override { return false; }
    bool IsReadable() const override { return true; }

    std::size_t Read(u8* data, std::size_t len, std::size_t off) const override {
        if (off >= nca_size) return 0;
        len = static_cast<std::size_t>(std::min<u64>(len, nca_size - off));
        std::size_t done = 0;
        if (off < HeaderSize) {
            done = static_cast<std::size_t>(std::min<u64>(len, HeaderSize - off));
            std::memcpy(data, header.data() + off, done);
        }
        if (done == len) return len;
        const auto body = Body(unpack_allowed);
        return body ? done + body->Read(data + done, len - done, off + done) : done;
    }

    std::size_t Write(const u8*, std::size_t, std::size_t) override { return 0; }
    bool Rename(std::string_view) override { return false; }

    bool Pending() const { return !unpack_allowed && Body(false) == nullptr; }

private:
    std::shared_ptr<CachedNcaFile> Body(bool unpack) const {
        std::scoped_lock lock{mutex};
        if (!body)
            body = UnpackSolidCached(source, name, sections, payload_start, nca_size, unpack);
        return body;
    }

    const std::string name;
    const FileSys::VirtualFile source;
    const std::vector<u8> header;
    const std::vector<Section> sections;
    const u64 payload_start;
    u64 nca_size = HeaderSize;
    mutable std::mutex mutex;
    mutable std::shared_ptr<CachedNcaFile> body;
};

} // Anonymous namespace

FileSys::VirtualFile DecompressNCZ(const FileSys::VirtualFile& f, std::string_view name) {
    if (!f) return nullptr;
    u64 ps = 0;
    auto parsed = ParseHeader(f, ps);
    if (!parsed) return nullptr;
    auto& [sections, block] = *parsed;

    if (block) {
        auto hdr = f->ReadBytes(HeaderSize, 0);
        if (hdr.size() != HeaderSize) return nullptr;
        std::unique_ptr<BlockReader> body;
        try { body = std::make_unique<BlockReader>(f, *block, ps); }
        catch (const std::exception& e) {
            LOG_ERROR(Crypto, "Failed to open NCZBLOCK: {}", e.what());
            return nullptr;
        }
        return std::make_shared<BlockNczFile>(std::string{name}, std::move(hdr),
                                              std::move(sections), std::move(body));
    }

    // Only solid entries become host files. Never interpret archive-supplied names as paths.
    if (!name.ends_with(".nca") || name.size() <= 4 || name.front() == '.' ||
        name.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-_.") !=
            std::string_view::npos)
        return nullptr;
    auto hdr = f->ReadBytes(HeaderSize, 0);
    if (hdr.size() != HeaderSize) return nullptr;
    return std::make_shared<SolidNczFile>(std::string{name}, f, std::move(hdr),
                                          std::move(sections), ps);
}

SolidNczUnpackScope::SolidNczUnpackScope() : previous{unpack_allowed} {
    unpack_allowed = true;
}

SolidNczUnpackScope::~SolidNczUnpackScope() {
    unpack_allowed = previous;
}

bool IsNczPendingUnpack(const FileSys::VirtualFile& file) {
    {
        std::scoped_lock lock{solid_files_mutex};
        if (!solid_files.contains(file.get())) return false;
    }
    return static_cast<const SolidNczFile*>(file.get())->Pending();
}

} // namespace Core::Crypto