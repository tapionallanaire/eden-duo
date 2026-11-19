// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include "common/common_funcs.h"
#include "common/fs/path_util.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/loader/nso.h"

namespace {

constexpr u64 OcarinaTitleId = 0x0400486024DEC000;
constexpr u64 OtherTitleId = 0x0100000000010000;
constexpr std::string_view MaxResolution = "Max Resolution (built-in)";
constexpr std::string_view OcarinaBuildIdHex = "E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348";
constexpr FileSys::PatchManager::BuildID OcarinaBuildId{
    0xE0, 0xF3, 0xCC, 0xFA, 0xF5, 0x00, 0x65, 0xAE, 0x4E, 0xA0,
    0x1E, 0xB5, 0xAE, 0xB1, 0x3B, 0x1A, 0x45, 0xDA, 0x33, 0x48,
};
constexpr size_t PatchOffset = sizeof(Loader::NSOHeader) + 0x805BD0;
constexpr u32 TbzInstruction = 0x360041E8;
constexpr u32 BranchInstruction = 0x1400020F;
constexpr u32 NopInstruction = 0xD503201F;

constexpr std::array<std::pair<Common::FS::EdenPath, const char*>, 4> TestDirectories{{
    {Common::FS::EdenPath::NANDDir, "nand"},
    {Common::FS::EdenPath::SDMCDir, "sdmc"},
    {Common::FS::EdenPath::LoadDir, "load"},
    {Common::FS::EdenPath::DumpDir, "dump"},
}};

void WriteIps(const std::filesystem::path& path, u32 word) {
    std::vector<u8> ips{'P', 'A', 'T', 'C', 'H'};
    ips.push_back(static_cast<u8>(PatchOffset >> 16));
    ips.push_back(static_cast<u8>(PatchOffset >> 8));
    ips.push_back(static_cast<u8>(PatchOffset));
    ips.push_back(0);
    ips.push_back(sizeof(word));
    for (size_t i = 0; i < sizeof(word); ++i) {
        ips.push_back(static_cast<u8>(word >> (i * 8)));
    }
    ips.insert(ips.end(), {'E', 'O', 'F'});
    std::ofstream file{path, std::ios::binary};
    file.write(reinterpret_cast<const char*>(ips.data()), static_cast<std::streamsize>(ips.size()));
}

class PatchManagerTestContext {
public:
    explicit PatchManagerTestContext(std::optional<u32> user_mod_word = std::nullopt) {
        for (size_t i = 0; i < TestDirectories.size(); ++i) {
            const auto& [path, name] = TestDirectories[i];
            previous[i] = Common::FS::GetEdenPath(path);
            std::filesystem::create_directories(root / name);
            Common::FS::SetEdenPath(path, root / name);
        }
        const auto title_dir = root / "load" / fmt::format("{:016X}", OcarinaTitleId);
        std::filesystem::create_directories(title_dir);
        if (user_mod_word) {
            const auto exefs = title_dir / "UserMod" / "exefs";
            std::filesystem::create_directories(exefs);
            WriteIps(exefs / fmt::format("{}.ips", OcarinaBuildIdHex), *user_mod_word);
        }
        system.SetFilesystem(std::make_shared<FileSys::RealVfsFilesystem>());
        system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
        system.GetFileSystemController().CreateFactories(*system.GetFilesystem());
    }

    ~PatchManagerTestContext() {
        for (size_t i = 0; i < TestDirectories.size(); ++i) {
            Common::FS::SetEdenPath(TestDirectories[i].first, previous[i]);
        }
        Settings::values.disabled_addons.erase(OcarinaTitleId);
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    PatchManagerTestContext(const PatchManagerTestContext&) = delete;
    PatchManagerTestContext& operator=(const PatchManagerTestContext&) = delete;

    FileSys::PatchManager Manager(u64 title_id = OcarinaTitleId) {
        return FileSys::PatchManager{title_id, system.GetFileSystemController(),
                                     system.GetContentProvider()};
    }

    FileSys::PatchManager Manager(const FileSys::ContentProvider& provider,
                                 u64 title_id = OcarinaTitleId) {
        return FileSys::PatchManager{title_id, system.GetFileSystemController(), provider};
    }

private:
    std::array<std::filesystem::path, TestDirectories.size()> previous;
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("builtin-patch-test-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Core::System system;
};

std::vector<u8> MakeMain(const FileSys::PatchManager::BuildID& build_id, u32 word) {
    std::vector<u8> nso(PatchOffset + 0x100);
    Loader::NSOHeader header{};
    header.magic = Common::MakeMagic('N', 'S', 'O', '0');
    header.build_id = build_id;
    std::memcpy(nso.data(), &header, sizeof(header));
    std::memcpy(nso.data() + PatchOffset, &word, sizeof(word));
    return nso;
}

u32 WordAt(const std::vector<u8>& nso) {
    REQUIRE(nso.size() >= PatchOffset + sizeof(u32));
    u32 word;
    std::memcpy(&word, nso.data() + PatchOffset, sizeof(word));
    return word;
}

u32 PatchMain(PatchManagerTestContext& context, const FileSys::PatchManager::BuildID& build_id,
              u32 word, const std::string& name = "main", u64 title_id = OcarinaTitleId) {
    return WordAt(context.Manager(title_id).PatchNSO(MakeMain(build_id, word), name));
}

} // Anonymous namespace

TEST_CASE("Built-in Max Resolution patch is gated on build id and original word",
          "[patch_manager]") {
    PatchManagerTestContext context;

    SECTION("Matching build id and original word applies") {
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction) == BranchInstruction);
    }

    SECTION("Wrong original word skips") {
        REQUIRE(PatchMain(context, OcarinaBuildId, NopInstruction) == NopInstruction);
    }

    SECTION("Other build id skips") {
        auto other = OcarinaBuildId;
        other[0] ^= 1;
        REQUIRE(PatchMain(context, other, TbzInstruction) == TbzInstruction);
    }

    SECTION("Other module skips") {
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction, "sdk") == TbzInstruction);
    }

    SECTION("Other title skips") {
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction, "main", OtherTitleId) ==
                TbzInstruction);
    }

    SECTION("Disabled in Add-Ons skips") {
        Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution}};
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction) == TbzInstruction);
    }
}

TEST_CASE("Built-in Max Resolution patch defers to a user mod at the same offset",
          "[patch_manager]") {
    SECTION("A different word from the mod is kept") {
        PatchManagerTestContext context{NopInstruction};
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction) == NopInstruction);
    }

    SECTION("The same word from the mod is applied once") {
        PatchManagerTestContext context{BranchInstruction};
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction) == BranchInstruction);
    }

    SECTION("Disabling the built-in patch keeps the mod") {
        PatchManagerTestContext context{BranchInstruction};
        Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution}};
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction) == BranchInstruction);
    }
}

namespace {

constexpr std::string_view RealTime = "Real-time speed (built-in)";
constexpr std::array<std::pair<size_t, u32>, 4> RealTimeOriginals{{
    {0x3C461E0, 0x00000000},
    {0x3C461E4, 0x00000000},
    {0x3C461E8, 0x00000000},
    {0x6E6CC4, 0x1E201828},
}};

std::vector<u8> MakeRealTimeMain() {
    std::vector<u8> nso(sizeof(Loader::NSOHeader) + 0x3C47000);
    Loader::NSOHeader header{};
    header.magic = Common::MakeMagic('N', 'S', 'O', '0');
    header.build_id = OcarinaBuildId;
    std::memcpy(nso.data(), &header, sizeof(header));
    for (const auto& [offset, word] : RealTimeOriginals) {
        std::memcpy(nso.data() + sizeof(Loader::NSOHeader) + offset, &word, sizeof(word));
    }
    return nso;
}

u32 ModuleWord(const std::vector<u8>& nso, size_t offset) {
    u32 word;
    std::memcpy(&word, nso.data() + sizeof(Loader::NSOHeader) + offset, sizeof(word));
    return word;
}

} // Anonymous namespace

TEST_CASE("Built-in real-time patch applies all of its words or none", "[patch_manager]") {
    PatchManagerTestContext context;
    auto nso = MakeRealTimeMain();

    SECTION("All words apply, on by default") {
        const auto patched = context.Manager().PatchNSO(nso, "main");
        REQUIRE(ModuleWord(patched, 0x3C461E0) == 0x3F800000);
        REQUIRE(ModuleWord(patched, 0x3C461E4) == 0x1CFFFFE8);
        REQUIRE(ModuleWord(patched, 0x3C461E8) == 0x172A82B8);
        REQUIRE(ModuleWord(patched, 0x6E6CC4) == 0x14D57D48);
    }

    SECTION("One changed word leaves the hook out") {
        const u32 other = 0xD503201F;
        std::memcpy(nso.data() + sizeof(Loader::NSOHeader) + 0x3C461E4, &other, sizeof(other));
        const auto patched = context.Manager().PatchNSO(nso, "main");
        REQUIRE(ModuleWord(patched, 0x3C461E0) == 0x00000000);
        REQUIRE(ModuleWord(patched, 0x6E6CC4) == 0x1E201828);
    }

    SECTION("Disabled in Add-Ons skips") {
        Settings::values.disabled_addons[OcarinaTitleId] = {std::string{RealTime}};
        const auto patched = context.Manager().PatchNSO(nso, "main");
        REQUIRE(ModuleWord(patched, 0x6E6CC4) == 0x1E201828);
    }

    SECTION("It is one Add-Ons entry") {
        const auto patches = context.Manager().GetPatches();
        REQUIRE(std::ranges::count(patches, RealTime, &FileSys::Patch::name) == 1);
    }
}

TEST_CASE("Built-in Max Resolution patch is listed and loaded for Ocarina's main",
          "[patch_manager]") {
    PatchManagerTestContext context;
    const auto manager = context.Manager();

    REQUIRE(manager.HasNSOPatch(OcarinaBuildId, "main"));
    REQUIRE_FALSE(manager.HasNSOPatch(OcarinaBuildId, "sdk"));
    REQUIRE_FALSE(context.Manager(OtherTitleId).HasNSOPatch(OcarinaBuildId, "main"));

    const auto builtin_enabled = [&] {
        const auto patches = manager.GetPatches();
        const auto it = std::ranges::find(patches, MaxResolution, &FileSys::Patch::name);
        REQUIRE(it != patches.end());
        REQUIRE(it->type == FileSys::PatchType::Mod);
        return it->enabled;
    };
    REQUIRE(builtin_enabled());

    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution}};
    REQUIRE_FALSE(builtin_enabled());
}

TEST_CASE("Selected update retains exact source and never falls back from missing content",
          "[patch_manager]") {
    PatchManagerTestContext context;
    FileSys::ContentProviderUnion providers{};
    FileSys::ManualContentProvider manual;
    FileSys::ManualContentProvider nand;
    providers.SetSlot(FileSys::ContentProviderUnionSlot::FrontendManual, &manual);
    providers.SetSlot(FileSys::ContentProviderUnionSlot::UserNAND, &nand);
    constexpr u64 UpdateTitleId = OcarinaTitleId | 0x800;
    const auto newest = std::make_shared<FileSys::VectorVfsFile>(std::vector<u8>{1});
    const auto older = std::make_shared<FileSys::VectorVfsFile>(std::vector<u8>{2});
    const auto installed = std::make_shared<FileSys::VectorVfsFile>(std::vector<u8>{3});
    const auto newest_meta = std::make_shared<FileSys::VectorVfsFile>(std::vector<u8>{4});
    const auto older_meta = std::make_shared<FileSys::VectorVfsFile>(std::vector<u8>{5});
    const auto installed_meta = std::make_shared<FileSys::VectorVfsFile>(std::vector<u8>{6});
    manual.AddEntryWithVersion(FileSys::TitleType::Update, FileSys::ContentRecordType::Program,
                               UpdateTitleId, 131072, "", newest);
    manual.AddEntryWithVersion(FileSys::TitleType::Update, FileSys::ContentRecordType::Program,
                               UpdateTitleId, 65536, "", older);
    nand.AddEntry(FileSys::TitleType::Update, FileSys::ContentRecordType::Program, UpdateTitleId,
                  installed);
    manual.AddEntryWithVersion(FileSys::TitleType::Update, FileSys::ContentRecordType::Meta,
                               UpdateTitleId, 131072, "", newest_meta);
    manual.AddEntryWithVersion(FileSys::TitleType::Update, FileSys::ContentRecordType::Meta,
                               UpdateTitleId, 65536, "", older_meta);
    nand.AddEntry(FileSys::TitleType::Update, FileSys::ContentRecordType::Meta, UpdateTitleId,
                  installed_meta);
    const auto manager = context.Manager(providers);

    SECTION("Newest manual version wins over NAND") {
        const auto update = manager.ResolveUpdate();
        REQUIRE(update.selected);
        REQUIRE(update.raw == newest);
        REQUIRE(update.metadata_raw == newest_meta);
        REQUIRE(update.version == 131072);
        REQUIRE(update.source == FileSys::ContentProviderUnionSlot::FrontendManual);
    }
    SECTION("Disabling one version selects the next enabled version") {
        Settings::values.disabled_addons[OcarinaTitleId] = {"Update@131072"};
        const auto update = manager.ResolveUpdate();
        REQUIRE(update.raw == older);
        REQUIRE(update.metadata_raw == older_meta);
        REQUIRE(update.version == 65536);
    }
    SECTION("Missing selected record stays selected instead of using another source") {
        nand.AddEntry(FileSys::TitleType::Update, FileSys::ContentRecordType::Data, UpdateTitleId,
                      installed);
        const auto update = manager.ResolveUpdate(FileSys::ContentRecordType::Data);
        REQUIRE(update.selected);
        REQUIRE(update.raw == nullptr);
        REQUIRE(update.metadata_raw == newest_meta);
        REQUIRE(update.version == 131072);
        REQUIRE(update.source == FileSys::ContentProviderUnionSlot::FrontendManual);
    }
    SECTION("All manual versions disabled select the allowed NAND slot, not disabled SDMC") {
        FileSys::ManualContentProvider sdmc;
        providers.SetSlot(FileSys::ContentProviderUnionSlot::SDMC, &sdmc);
        sdmc.AddEntry(FileSys::TitleType::Update, FileSys::ContentRecordType::Program, UpdateTitleId,
                      older);
        Settings::values.disabled_addons[OcarinaTitleId] = {
            "Update@131072", "Update@65536", "Update (SDMC)"};
        const auto update = manager.ResolveUpdate();
        REQUIRE(update.selected);
        REQUIRE(update.raw == installed);
        REQUIRE(update.metadata_raw == installed_meta);
        REQUIRE_FALSE(update.version.has_value());
        REQUIRE(update.source == FileSys::ContentProviderUnionSlot::UserNAND);
    }
    SECTION("Installed fallback reads allowed SDMC rather than the disabled NAND winner") {
        FileSys::ManualContentProvider sdmc;
        providers.SetSlot(FileSys::ContentProviderUnionSlot::SDMC, &sdmc);
        sdmc.AddEntry(FileSys::TitleType::Update, FileSys::ContentRecordType::Program, UpdateTitleId,
                      older);
        Settings::values.disabled_addons[OcarinaTitleId] = {
            "Update@131072", "Update@65536", "Update (NAND)"};
        const auto update = manager.ResolveUpdate();
        REQUIRE(update.selected);
        REQUIRE(update.raw == older);
        REQUIRE(update.metadata_raw == nullptr);
        REQUIRE(update.source == FileSys::ContentProviderUnionSlot::SDMC);
    }
    SECTION("Disabling every source yields no update") {
        Settings::values.disabled_addons[OcarinaTitleId] = {
            "Update@131072", "Update@65536", "Update (NAND)", "Update (SDMC)"};
        REQUIRE_FALSE(manager.ResolveUpdate().selected);
    }
    SECTION("Unusable NCA does not materialize an executable or erase provenance") {
        const auto [directory, version] = manager.ReadExeFSUpdate();
        REQUIRE(directory == nullptr);
        REQUIRE(version == 131072);
        REQUIRE(manager.ResolveUpdate().raw == newest);
    }
    SECTION("Missing selected metadata never borrows another version or provider") {
        manual.AddEntryWithVersion(FileSys::TitleType::Update, FileSys::ContentRecordType::Meta,
                                   UpdateTitleId, 131072, "", nullptr);
        const auto update = manager.ResolveUpdate();
        REQUIRE(update.selected);
        REQUIRE(update.raw == newest);
        REQUIRE(update.version == 131072);
        REQUIRE(update.metadata_raw == nullptr);
    }
}

TEST_CASE("Packed updates obey the same update disable setting", "[patch_manager]") {
    PatchManagerTestContext context;
    FileSys::ContentProviderUnion providers{};
    const auto packed = std::make_shared<FileSys::VectorVfsFile>(std::vector<u8>{1});
    const auto manager = context.Manager(providers);
    REQUIRE_FALSE(manager.ResolveUpdate().selected);
    const auto update = manager.ResolveUpdate(FileSys::ContentRecordType::Program, packed);
    REQUIRE(update.selected);
    REQUIRE(update.raw == packed);
    REQUIRE(update.metadata_raw == nullptr);
    REQUIRE_FALSE(update.version.has_value());
    REQUIRE_FALSE(update.source.has_value());
    Settings::values.disabled_addons[OcarinaTitleId] = {"Update"};
    REQUIRE_FALSE(manager.ResolveUpdate(FileSys::ContentRecordType::Program, packed).selected);
}

TEST_CASE("Strict export can refuse enabled built-in and user mods", "[patch_manager]") {
    PatchManagerTestContext context{NopInstruction};
    const auto manager = context.Manager();
    REQUIRE(manager.HasEnabledMods());
    Settings::values.disabled_addons[OcarinaTitleId] = {
        std::string{MaxResolution}, "Real-time speed (built-in)"};
    REQUIRE(manager.HasEnabledMods());
    Settings::values.disabled_addons[OcarinaTitleId].push_back("UserMod");
    REQUIRE_FALSE(manager.HasEnabledMods());
}

TEST_CASE("Strict mod refusal honors inherited roots and disabled settings", "[patch_manager]") {
    PatchManagerTestContext context{NopInstruction};
    FileSys::ContentProviderUnion providers{};
    FileSys::ManualContentProvider manual;
    providers.SetSlot(FileSys::ContentProviderUnionSlot::FrontendManual, &manual);
    const auto file = std::make_shared<FileSys::VectorVfsFile>(std::vector<u8>{1});
    manual.AddEntry(FileSys::TitleType::Application, FileSys::ContentRecordType::Meta,
                    OcarinaTitleId, file);
    manual.AddEntry(FileSys::TitleType::Application, FileSys::ContentRecordType::Program,
                    OcarinaTitleId + 1, file);
    const auto manager = context.Manager(providers, OcarinaTitleId + 1);
    REQUIRE(manager.HasEnabledMods());
    Settings::values.disabled_addons[OcarinaTitleId] = {"UserMod"};
    REQUIRE_FALSE(manager.HasEnabledMods());
}
