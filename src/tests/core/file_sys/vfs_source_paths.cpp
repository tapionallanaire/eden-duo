// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/zstd_compression.h"
#include "android/app/src/main/jni/native_save_binding.h"
#include "common/fs/path_util.h"
#include "core/file_sys/savedata_factory.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/crypto/aes_util.h"
#include "core/crypto/ctr_encryption_layer.h"
#include "core/crypto/ncz_decompressor.h"
#include "core/file_sys/vfs/vfs_concat.h"
#include "core/file_sys/vfs/vfs_offset.h"
#include "core/file_sys/vfs/vfs_vector.h"

TEST_CASE("Offset and split files retain their backing source paths", "[core][vfs]") {
    const auto physical = std::make_shared<FileSys::VectorVfsDirectory>(
        std::vector<FileSys::VirtualFile>{}, std::vector<FileSys::VirtualDir>{}, "/games");
    const auto saf = std::make_shared<FileSys::VectorVfsDirectory>(
        std::vector<FileSys::VirtualFile>{}, std::vector<FileSys::VirtualDir>{},
        "content://provider/tree/games");
    const auto first = std::make_shared<FileSys::VectorVfsFile>(
        std::vector<u8>{1, 2}, "game.nsp.00", physical);
    const auto second = std::make_shared<FileSys::VectorVfsFile>(
        std::vector<u8>{3, 4}, "game.nsp.01", saf);
    REQUIRE(first->GetSourcePaths() == std::vector<std::string>{"/games/game.nsp.00"});
    REQUIRE(second->GetSourcePaths() ==
            std::vector<std::string>{"content://provider/tree/games/game.nsp.01"});

    const auto split = FileSys::ConcatenatedVfsFile::MakeConcatenatedFile(
        "game.nsp", std::vector<FileSys::VirtualFile>{first, second});
    const auto entry = std::make_shared<FileSys::OffsetVfsFile>(split, 3, 1, "program.nca");
    const auto nested = std::make_shared<FileSys::OffsetVfsFile>(entry, 2, 1, "romfs");
    const std::vector<std::string> expected{
        "/games/game.nsp.00", "content://provider/tree/games/game.nsp.01"};
    REQUIRE(split->GetSourcePaths() == expected);
    REQUIRE(entry->GetSourcePaths() == expected);
    REQUIRE(nested->GetSourcePaths() == expected);
    REQUIRE(entry->GetFullPath() == "/program.nca");
    REQUIRE(nested->ReadAllBytes() == std::vector<u8>{3, 4});
    const Core::Crypto::CTREncryptionLayer decrypted{entry, Core::Crypto::Key128{}, 0};
    REQUIRE(decrypted.GetSourcePaths() == expected);
}

TEST_CASE("NCZ wrappers retain compressed container source paths", "[core][vfs][ncz]") {
    const Core::Crypto::SolidNczUnpackScope unpack; // as a launch: the solid pass unpacks
    constexpr std::size_t HeaderSize = 0x4000;
    const std::vector<u8> body(HeaderSize, 0x5a);
    const auto compressed = Common::Compression::CompressDataZSTDDefault(body.data(), body.size());
    REQUIRE(compressed.size() < body.size());

    for (const bool block : {false, true}) {
        INFO("NCZBLOCK=" << block);
        constexpr std::size_t BlockStart = HeaderSize + 16 + 0x40;
        std::vector<u8> bytes(BlockStart + (block ? 0x18 + 4 : 0));
        const auto put_le = [&](std::size_t offset, u64 value, std::size_t size) {
            for (std::size_t i = 0; i < size; ++i) {
                bytes[offset + i] = static_cast<u8>(value >> (8 * i));
            }
        };
        constexpr std::string_view sections_magic = "NCZSECTN";
        std::copy(sections_magic.begin(), sections_magic.end(), bytes.begin() + HeaderSize);
        put_le(HeaderSize + 8, 1, 8);
        put_le(HeaderSize + 16, HeaderSize, 8);
        put_le(HeaderSize + 24, body.size(), 8);
        if (block) {
            constexpr std::string_view block_magic = "NCZBLOCK";
            std::copy(block_magic.begin(), block_magic.end(), bytes.begin() + BlockStart);
            bytes[BlockStart + 11] = 14;
            put_le(BlockStart + 12, 1, 4);
            put_le(BlockStart + 16, body.size(), 8);
            put_le(BlockStart + 24, compressed.size(), 4);
        }
        bytes.insert(bytes.end(), compressed.begin(), compressed.end());
        const auto source = std::make_shared<FileSys::VectorVfsFile>(std::move(bytes), "game.nsz");
        const auto entry = std::make_shared<FileSys::OffsetVfsFile>(
            source, source->GetSize(), 0, "program.ncz");
        const auto decoded = Core::Crypto::DecompressNCZ(entry, "program.nca");
        REQUIRE(decoded != nullptr);
        REQUIRE(decoded->GetSourcePaths() == std::vector<std::string>{"/game.nsz"});
        REQUIRE(decoded->GetFullPath() == "/program.nca");
        REQUIRE(decoded->ReadBytes(body.size(), HeaderSize) == body);
    }
    std::error_code error; // the solid pass unpacked into the cache
    std::filesystem::remove(
        Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) / "ncz" / "program.nca", error);
}

TEST_CASE("Solid NCZ unpacks to the original NCA bytes", "[core][vfs][ncz]") {
    // A made-up NCA: header copy, a plain range, then an AES-CTR range. nsz stores the CTR range
    // decrypted; the solid unpack must encrypt it again and give back the NCA byte for byte.
    constexpr u64 HeaderSize = 0x4000, PlainSize = 0x3000, CtrSize = 0x5010;
    std::vector<u8> nca(HeaderSize + PlainSize + CtrSize);
    for (std::size_t i = 0; i < nca.size(); ++i) nca[i] = static_cast<u8>(i * 131 + (i >> 9));
    Core::Crypto::Key128 key{}, counter{};
    for (u8 i = 0; i < 16; ++i) key[i] = static_cast<u8>(0xa0 + i);
    for (u8 i = 0; i < 8; ++i) counter[i] = static_cast<u8>(0x10 + i);
    const u64 ctr_at = HeaderSize + PlainSize;
    Core::Crypto::Key128 iv = counter;
    for (std::size_t i = 0, blk = ctr_at >> 4; i < 8; ++i, blk >>= 8) iv[15 - i] = static_cast<u8>(blk);
    std::vector<u8> body(nca.begin() + HeaderSize, nca.end());
    Core::Crypto::AESCipher<Core::Crypto::Key128> cipher{key, Core::Crypto::Mode::CTR};
    cipher.SetIV(iv);
    cipher.Transcode(body.data() + PlainSize, CtrSize, body.data() + PlainSize,
                     Core::Crypto::Op::Decrypt);
    REQUIRE(!std::equal(body.begin() + PlainSize, body.end(), nca.begin() + ctr_at));

    std::vector<u8> ncz(nca.begin(), nca.begin() + HeaderSize);
    const auto put = [&](u64 value, std::size_t size) {
        for (std::size_t i = 0; i < size; ++i) ncz.push_back(static_cast<u8>(value >> (8 * i)));
    };
    for (const char c : std::string_view{"NCZSECTN"}) ncz.push_back(static_cast<u8>(c));
    put(2, 8);
    for (const auto [offset, size, type] : {std::array<u64, 3>{HeaderSize, PlainSize, 1},
                                            std::array<u64, 3>{ctr_at, CtrSize, 3}}) {
        put(offset, 8), put(size, 8), put(type, 8), put(0, 8);
        ncz.insert(ncz.end(), key.begin(), key.end());
        ncz.insert(ncz.end(), counter.begin(), counter.end());
    }
    const auto frame = Common::Compression::CompressDataZSTDDefault(body.data(), body.size());
    ncz.insert(ncz.end(), frame.begin(), frame.end());

    const std::string name = "ncz-solid-test-" + Common::UUID::MakeRandom().RawString() + ".nca";
    const auto cached = Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) / "ncz" / name;
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove(path, error);
        }
    } cleanup{cached};
    const auto source = std::make_shared<FileSys::VectorVfsFile>(std::move(ncz), "game.nsz");
    REQUIRE_FALSE(Core::Crypto::DecompressNCZ(source, "nested/program.nca"));
    auto stale = cached;
    stale += "-old.part"; // left by an unpack that was cut short
    std::filesystem::create_directories(cached.parent_path());
    std::ofstream{stale} << "x";
    { // a library scan: the header only, nothing written
        const auto decoded = Core::Crypto::DecompressNCZ(source, name);
        REQUIRE(decoded != nullptr);
        REQUIRE(Core::Crypto::IsNczPendingUnpack(decoded));
        REQUIRE(decoded->GetSize() == nca.size());
        REQUIRE(decoded->ReadBytes(0x20, HeaderSize - 0x10) ==
                std::vector<u8>(nca.begin() + HeaderSize - 0x10, nca.begin() + HeaderSize));
        REQUIRE(decoded->ReadAllBytes() == std::vector<u8>(nca.begin(), nca.begin() + HeaderSize));
        REQUIRE(!std::filesystem::exists(cached));
        REQUIRE(std::filesystem::exists(stale));
    }
    for (int pass = 0; pass < 2; ++pass) { // a launch unpacks; then even a scan reuses the copy
        INFO("pass " << pass);
        std::optional<Core::Crypto::SolidNczUnpackScope> launch;
        if (pass == 0) launch.emplace();
        const auto decoded = Core::Crypto::DecompressNCZ(source, name);
        REQUIRE(decoded != nullptr);
        REQUIRE(decoded->GetName() == name);
        REQUIRE(decoded->GetSourcePaths() == std::vector<std::string>{"/game.nsz"});
        REQUIRE(decoded->ReadAllBytes() == nca);
        REQUIRE(decoded->ReadBytes(0x20, ctr_at - 8) ==
                std::vector<u8>(nca.begin() + ctr_at - 8, nca.begin() + ctr_at + 0x18));
        REQUIRE(!Core::Crypto::IsNczPendingUnpack(decoded));
    }
    REQUIRE(std::filesystem::file_size(cached) == nca.size());
    REQUIRE(!std::filesystem::exists(stale));
}

TEST_CASE("Solid NCZ too large for the free space is not unpacked", "[core][vfs][ncz]") {
    constexpr u64 HeaderSize = 0x4000, Huge = 1ULL << 50; // 1 PiB: no test machine has it free
    std::vector<u8> ncz(HeaderSize, 0x11);
    const auto put = [&](u64 value) {
        for (std::size_t i = 0; i < 8; ++i) ncz.push_back(static_cast<u8>(value >> (8 * i)));
    };
    for (const char c : std::string_view{"NCZSECTN"}) ncz.push_back(static_cast<u8>(c));
    put(1), put(HeaderSize), put(Huge), put(1), put(0);
    ncz.insert(ncz.end(), 32, 0);
    const std::vector<u8> frame{0x28, 0xb5, 0x2f, 0xfd, 0, 0x48, 1, 0, 0}; // empty zstd frame
    ncz.insert(ncz.end(), frame.begin(), frame.end());
    const std::string name = "ncz-huge-test-" + Common::UUID::MakeRandom().RawString() + ".nca";
    const auto cached = Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) / "ncz" / name;
    const Core::Crypto::SolidNczUnpackScope launch;
    const auto decoded = Core::Crypto::DecompressNCZ(
        std::make_shared<FileSys::VectorVfsFile>(std::move(ncz), "huge.nsz"), name);
    REQUIRE(decoded != nullptr);
    std::vector<u8> out(0x20);
    REQUIRE(decoded->Read(out.data(), out.size(), HeaderSize - 0x10) == 0x10);
    auto part = cached;
    part += ".part";
    REQUIRE(!std::filesystem::exists(cached));
    REQUIRE(!std::filesystem::exists(part));
}

TEST_CASE("Native save bindings preserve UUID words and production layout selection",
          "[core][vfs][native-save-binding]") {
    namespace fs = std::filesystem;
    const Common::UUID user{"00112233445566778899aabbccddeeff"};
    REQUIRE(NativeSaveBinding::UserUidWords(user) ==
            std::array<std::string, 2>{"7766554433221100", "ffeeddccbbaa9988"});
    REQUIRE(NativeSaveBinding::UserFolder(user) == "FFEEDDCCBBAA99887766554433221100");
    REQUIRE(user.RawString() == "00112233445566778899aabbccddeeff");
    REQUIRE(NativeSaveBinding::UserFolder(user) != "00112233445566778899AABBCCDDEEFF");
    const auto temporary = fs::temp_directory_path() /
        ("native-save-binding-" + Common::UUID::MakeRandom().RawString());
    struct Cleanup {
        fs::path path;
        ~Cleanup() {
            std::error_code error;
            fs::remove_all(path, error);
        }
    } cleanup{temporary};
    fs::create_directories(temporary / "nand");
    const auto root = NativeSaveBinding::PhysicalRoot(temporary / "nand");
    FileSys::RealVfsFilesystem filesystem;
    const auto directory = filesystem.OpenDirectory(
        Common::FS::PathToUTF8String(root), FileSys::OpenMode::Read);
    REQUIRE(directory != nullptr);
    constexpr u64 Owner = 0x0100123400000002;
    const auto account = [&] {
        return FileSys::SaveDataFactory::GetFullPath(
            {}, directory, FileSys::SaveDataSpaceId::User, FileSys::SaveDataType::Account,
            Owner, user.AsU128(), 0);
    };
    const std::string legacy =
        "/user/save/0000000000000000/FFEEDDCCBBAA99887766554433221100/0100123400000002";
    REQUIRE(account() == legacy);
    REQUIRE(NativeSaveBinding::PhysicalPath(root, legacy) == root / fs::path{legacy}.relative_path());
    REQUIRE_FALSE(fs::exists(NativeSaveBinding::PhysicalPath(root, legacy)));
    const std::string future =
        "/user/save/account/00112233445566778899aabbccddeeff/0100123400000000/0";
    fs::create_directories(root / fs::path{future}.relative_path());
    REQUIRE(account() == future);
    REQUIRE(NativeSaveBinding::PhysicalPath(root, account()) ==
            root / fs::path{future}.relative_path());
    const auto device = FileSys::SaveDataFactory::GetFullPath(
        {}, directory, FileSys::SaveDataSpaceId::User, FileSys::SaveDataType::Device,
        Owner, {}, 0);
    REQUIRE(device ==
            "/user/save/0000000000000000/00000000000000000000000000000000/0100123400000002");
    REQUIRE_THROWS(NativeSaveBinding::PhysicalRoot("content://provider/tree/saves"));
    REQUIRE_THROWS(NativeSaveBinding::PhysicalRoot("relative/save"));
    REQUIRE_THROWS(NativeSaveBinding::PhysicalPath(root, "/../outside"));
    fs::create_directories(temporary / "outside");
    std::error_code error;
    fs::create_directory_symlink(temporary / "outside", root / "escape", error);
    if (!error) {
        REQUIRE_THROWS(NativeSaveBinding::PhysicalPath(root, "/escape/new-save"));
    } else {
        WARN("Host cannot create directory symlinks; symlink escape regression not exercised");
    }
}
