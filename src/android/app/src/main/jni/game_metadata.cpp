// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <fcntl.h>
#include <unistd.h>
#include <format>
#include "common/android/android_common.h"
#include "common/common_funcs.h"
#include "common/hex_util.h"
#include "common/fs/file.h"
#include "common/fs/path_util.h"
#include "common/fs/fs.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "core/core.h"
#include "core/file_sys/card_image.h"
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/fs_filesystem.h"
#include "core/file_sys/nup_writer.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/program_metadata.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/savedata_factory.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/file_sys/submission_package.h"
#include "core/loader/loader.h"
#include "core/loader/nro.h"
#include "core/loader/nso.h"
#include "core/loader/nup.h"
#include "native.h"
#include "native_save_binding.h"

struct RomMetadata {
    std::string title;
    u64 programId;
    std::string developer;
    std::string version;
    std::vector<u8> icon;
    bool isHomebrew;
};
static ::Common::unordered_map<std::string, RomMetadata> m_rom_metadata_cache;

static RomMetadata CacheRomMetadata(const std::string& path) {
    auto& instance = EmulationSession::GetInstance();
    const auto file = Core::GetGameFileFromPath(instance.System().GetFilesystem(), path);
    if (auto loader = Loader::GetLoader(instance.System(), file, 0, 0); loader) {
        RomMetadata entry;
        loader->ReadTitle(entry.title);
        loader->ReadProgramId(entry.programId);
        loader->ReadIcon(entry.icon);

        const FileSys::PatchManager pm{
            entry.programId,
            instance.System().GetFileSystemController(),
            instance.System().GetContentProvider()
        };
        const auto control = pm.GetControlMetadata();

        if (control.first != nullptr) {
            entry.developer = control.first->GetDeveloperName();
            entry.version = control.first->GetVersionString();
        } else {
            FileSys::NACP nacp{};
            entry.developer = loader->ReadControlData(nacp) == Loader::ResultStatus::Success
                ? nacp.GetDeveloperName()
                : "";
            entry.version = "1.0.0";
        }
        if (loader->GetFileType() == Loader::FileType::NRO) {
            auto loader_nro = reinterpret_cast<Loader::AppLoader_NRO*>(loader.get());
            entry.isHomebrew = loader_nro->IsHomebrew();
        } else {
            entry.isHomebrew = false;
        }
        m_rom_metadata_cache[path] = entry;
        return entry;
    }
    return {};
}

static RomMetadata GetRomMetadata(const std::string& path, bool reload = false) {
    if (reload)
        return CacheRomMetadata(path);
    if (auto it = m_rom_metadata_cache.find(path); it != m_rom_metadata_cache.end())
        return it->second;
    return CacheRomMetadata(path);
}

extern "C" {

jboolean Java_org_yuzu_yuzu_1emu_utils_GameMetadata_getIsValid(JNIEnv* env, jobject obj, jstring jpath) {
    if (auto const file = EmulationSession::GetInstance().System().GetFilesystem()->OpenFile(Common::Android::GetJString(env, jpath), FileSys::OpenMode::Read); file) {
        if (auto loader = Loader::GetLoader(EmulationSession::GetInstance().System(), file); loader) {
            auto const file_type = loader->GetFileType();
            if (file_type == Loader::FileType::Unknown || file_type == Loader::FileType::Error)
                return false;
            if ((file_type == Loader::FileType::NSP || file_type == Loader::FileType::XCI ||
                 file_type == Loader::FileType::NSZ || file_type == Loader::FileType::XCZ) &&
                !Loader::IsBootableGameContainer(file, file_type))
                return false;
            u64 program_id = 0;
            return loader->ReadProgramId(program_id) == Loader::ResultStatus::Success;
        }
    }
    return false;
}

jstring Java_org_yuzu_yuzu_1emu_utils_GameMetadata_getTitle(JNIEnv* env, jobject obj, jstring jpath) {
    return Common::Android::ToJString(env, GetRomMetadata(Common::Android::GetJString(env, jpath)).title);
}

jstring Java_org_yuzu_yuzu_1emu_utils_GameMetadata_getProgramId(JNIEnv* env, jobject obj, jstring jpath) {
    return Common::Android::ToJString(env, std::to_string(GetRomMetadata(Common::Android::GetJString(env, jpath)).programId));
}

jstring Java_org_yuzu_yuzu_1emu_utils_GameMetadata_getDeveloper(JNIEnv* env, jobject obj, jstring jpath) {
    return Common::Android::ToJString(env, GetRomMetadata(Common::Android::GetJString(env, jpath)).developer);
}

jstring Java_org_yuzu_yuzu_1emu_utils_GameMetadata_getVersion(JNIEnv* env, jobject obj, jstring jpath, jboolean jreload) {
    return Common::Android::ToJString(env, GetRomMetadata(Common::Android::GetJString(env, jpath), jreload).version);
}

jbyteArray Java_org_yuzu_yuzu_1emu_utils_GameMetadata_getIcon(JNIEnv* env, jobject obj, jstring jpath) {
    auto icon_data = GetRomMetadata(Common::Android::GetJString(env, jpath)).icon;
    jbyteArray icon = env->NewByteArray(jsize(icon_data.size()));
    env->SetByteArrayRegion(icon, 0, env->GetArrayLength(icon), reinterpret_cast<jbyte*>(icon_data.data()));
    return icon;
}

jboolean Java_org_yuzu_yuzu_1emu_utils_GameMetadata_getIsHomebrew(JNIEnv* env, jobject obj, jstring jpath) {
    return jboolean(GetRomMetadata(Common::Android::GetJString(env, jpath)).isHomebrew);
}

void Java_org_yuzu_yuzu_1emu_utils_GameMetadata_resetMetadata(JNIEnv* env, jobject obj) {
    m_rom_metadata_cache.clear();
}

} // extern "C"

namespace {

using Json = nlohmann::json;
using Modules = std::map<std::string, std::string>;
namespace fs = std::filesystem;

void RequirePrepare(bool condition, const char* error) {
    if (!condition) {
        throw std::runtime_error(error);
    }
}

std::string Digest(const std::string& bytes) {
    std::array<u8, 32> hash{};
    unsigned int length{};
    RequirePrepare(EVP_Digest(bytes.data(), bytes.size(), hash.data(), &length, EVP_sha256(),
                              nullptr) == 1 && length == hash.size(),
                   "Cannot fingerprint effective content");
    return Common::HexToString(hash, false);
}

std::optional<u32> ReadContentVersion(const FileSys::NCA& meta, u64 title_id,
                                     FileSys::TitleType type) {
    if (meta.GetStatus() != Loader::ResultStatus::Success) {
        return std::nullopt;
    }
    for (const auto& dir : meta.GetSubdirectories()) {
        for (const auto& file : dir->GetFiles()) {
            FileSys::CNMTHeader header{};
            if (file->GetExtension() == "cnmt" && file->ReadObject(&header) == sizeof(header) &&
                header.type == type && header.title_id == title_id) {
                return header.title_version;
            }
        }
    }
    return std::nullopt;
}

// These are profile contracts, not a platform-bit conversion. CNMT records below must
// independently prove the original application and each selected content source.
std::optional<u64> IndexedApplication(u64 program_id) {
    switch (program_id) {
    case 0x040015100B514001ULL:
        return 0x010015100B514000ULL;
    case 0x04004D300C5AE001ULL:
        return 0x01004D300C5AE000ULL;
    default:
        return std::nullopt;
    }
}

std::optional<FileSys::ContentRecord> ProgramRecord(
    const FileSys::NCA& meta, u64 application_id, FileSys::TitleType type, u8 index) {
    if (meta.GetStatus() != Loader::ResultStatus::Success) {
        return std::nullopt;
    }
    for (const auto& dir : meta.GetSubdirectories()) {
        for (const auto& file : dir->GetFiles()) {
            FileSys::CNMTHeader header{};
            if (file->GetExtension() != "cnmt" ||
                file->ReadObject(&header) != sizeof(header) ||
                header.title_id != application_id || header.type != type) {
                continue;
            }
            const size_t offset = sizeof(header) + static_cast<u16>(header.table_offset);
            const size_t count = header.number_content_entries;
            RequirePrepare(offset <= file->GetSize() &&
                               count <= (file->GetSize() - offset) / sizeof(FileSys::ContentRecord),
                           "Application CNMT content records are truncated");
            std::optional<FileSys::ContentRecord> result;
            for (size_t i = 0; i < count; ++i) {
                FileSys::ContentRecord record{};
                RequirePrepare(file->ReadObject(&record, offset + i * sizeof(record)) == sizeof(record),
                               "Cannot read application CNMT content record");
                if (record.type == FileSys::ContentRecordType::Program && record.id_offset == index) {
                    RequirePrepare(!result, "Application CNMT has duplicate program records");
                    result = record;
                }
            }
            return result;
        }
    }
    return std::nullopt;
}

bool MatchesProgramRecord(const FileSys::VirtualFile& raw,
                          const FileSys::ContentRecord& record) {
    if (!raw) {
        return false;
    }
    u64 size{};
    for (size_t i = 0; i < record.size.size(); ++i) {
        size |= static_cast<u64>(record.size[i]) << (8 * i);
    }
    const auto name = Common::ToLower(raw->GetName());
    const auto id = Common::HexToString(record.nca_id, false);
    return raw->GetSize() == size && (name == id + ".nca" || name == id + ".ncz");
}

FileSys::PatchManager::SelectedUpdate ResolveIndexedUpdate(
    const FileSys::ContentProviderUnion& provider, u64 application_id, u64 program_id,
    const FileSys::PatchManager::SelectedUpdate& application_update) {
    auto selected = application_update;
    if (!selected.selected) {
        return selected;
    }
    RequirePrepare(selected.metadata_raw && selected.version && selected.source,
                   "Indexed update has no exact application metadata/provider");
    const FileSys::NCA meta{selected.metadata_raw};
    const auto record = ProgramRecord(meta, FileSys::GetUpdateTitleID(application_id),
                                     FileSys::TitleType::Update, 1);
    RequirePrepare(record.has_value(), "Selected application update has no program one record");
    const auto original_record = ProgramRecord(meta, FileSys::GetUpdateTitleID(application_id),
                                              FileSys::TitleType::Update, 0);
    RequirePrepare(original_record && MatchesProgramRecord(application_update.raw, *original_record),
                   "Selected original update does not match its application CNMT program zero");
    const auto* source = provider.GetSlotProvider(*selected.source);
    RequirePrepare(source != nullptr, "Indexed update provider is unavailable");
    selected.raw = nullptr;
    const auto consider = [&](const FileSys::VirtualFile& raw) {
        if (MatchesProgramRecord(raw, *record)) {
            selected.raw = raw;
        }
    };
    // Providers may key a secondary NCA by its real program ID (manual/external), or
    // by the application's CNMT id_offset (installed cache). The CNMT content ID decides.
    for (const auto id : {FileSys::GetUpdateTitleID(program_id),
                          FileSys::GetUpdateTitleID(application_id + 1)}) {
        if (*selected.source == FileSys::ContentProviderUnionSlot::External) {
            for (const auto& entry :
                 static_cast<const FileSys::ExternalContentProvider*>(source)->ListUpdateVersions(id)) {
                consider(entry.files[static_cast<size_t>(FileSys::ContentRecordType::Program)]);
            }
        } else if (*selected.source == FileSys::ContentProviderUnionSlot::FrontendManual) {
            for (const auto& entry :
                 static_cast<const FileSys::ManualContentProvider*>(source)->ListUpdateVersions(id)) {
                consider(entry.files[static_cast<size_t>(FileSys::ContentRecordType::Program)]);
            }
        } else {
            consider(source->GetEntryRaw(id, FileSys::ContentRecordType::Program));
        }
    }
    RequirePrepare(selected.raw != nullptr, "Exact indexed update program content is missing");
    return selected;
}

std::optional<u64> AddonApplication(const FileSys::NCA& meta, u64 title_id) {
    if (meta.GetStatus() != Loader::ResultStatus::Success) {
        return std::nullopt;
    }
    for (const auto& dir : meta.GetSubdirectories()) {
        for (const auto& file : dir->GetFiles()) {
            FileSys::CNMTHeader header{};
            FileSys::OptionalHeader optional{};
            if (file->GetExtension() == "cnmt" &&
                file->ReadObject(&header) == sizeof(header) &&
                header.title_id == title_id && header.type == FileSys::TitleType::AOC &&
                header.table_offset >= sizeof(optional) &&
                file->ReadObject(&optional, sizeof(header)) == sizeof(optional)) {
                return optional.title_id;
            }
        }
    }
    return std::nullopt;
}

std::shared_ptr<FileSys::NSP> OpenRetailPackage(const FileSys::VirtualFile& file,
                                              size_t program_index) {
    // Probe bytes, not the URI's display suffix. PFS-shaped XCZ identifies as NSP.
    switch (Loader::IdentifyFile(file)) {
    case Loader::FileType::NSP:
    case Loader::FileType::NSZ:
        return std::make_shared<FileSys::NSP>(file, 0, program_index, false);
    case Loader::FileType::XCI:
    case Loader::FileType::XCZ: {
        FileSys::XCI card{file, 0, program_index, false};
        return card.GetStatus() == Loader::ResultStatus::Success ? card.GetSecurePartitionNSP()
                                                                : nullptr;
    }
    default:
        return nullptr;
    }
}

u64 SelectedRetailProgram(const std::shared_ptr<FileSys::NSP>& package, jint index) {
    if (index == 1) {
        u64 selected{};
        for (const auto& [id, entries] : package->GetNCAs()) {
            if (IndexedApplication(id) &&
                entries.contains({FileSys::TitleType::Application, FileSys::ContentRecordType::Program})) {
                RequirePrepare(selected == 0, "Container contains multiple indexed native programs");
                selected = id;
            }
        }
        if (selected != 0) {
            return selected;
        }
    }
    return package->GetProgramTitleID();
}

Modules ReadModules(const FileSys::VirtualDir& exefs, u64 program_id) {
    RequirePrepare(exefs != nullptr, "Effective executable is missing");
    FileSys::ProgramMetadata metadata;
    RequirePrepare(metadata.Load(exefs->GetFile("main.npdm")) == Loader::ResultStatus::Success &&
                       metadata.GetTitleID() == program_id,
                   "Effective NPDM does not identify the selected program");
    Modules modules;
    for (const auto& module : exefs->GetFiles()) {
        const auto name = module->GetName();
        if (name != "main" && name != "sdk" && name != "rtld" && !name.starts_with("subsdk")) {
            continue;
        }
        Loader::NSOHeader header{};
        RequirePrepare(module->ReadObject(&header) == sizeof(header) &&
                           header.magic == Common::MakeMagic('N', 'S', 'O', '0'),
                       "An effective NSO module is missing or unreadable");
        RequirePrepare(modules.emplace(name, Common::HexToString(header.build_id, false)).second,
                       "Duplicate effective NSO module");
    }
    RequirePrepare(modules.contains("main"), "Effective executable has no main NSO");
    return modules;
}

struct EffectiveInput {
    u64 program_id{};
    u64 application_id{};
    jint program_index{};
    u32 version{};
    u64 input_bytes{};
    Modules modules;
    Json context = Json::object();
    Json dlc = Json::array();
    Json programs = Json::array();
    std::optional<FileSys::ContentRecord> original_program_record;
    std::vector<FileSys::Nup::ResolvedPackage> packages;
    // Explicitly retain sources, including CNMT provenance, throughout serialization.
    std::vector<FileSys::VirtualFile> sources;
    std::string content_key;

    std::vector<std::string> Fields() const {
        std::vector<std::string> fields{std::format("{:016X}", program_id),
                                       std::to_string(version), content_key,
                                       std::to_string(input_bytes)};
        for (const auto& [name, id] : modules) {
            fields.push_back(name + "=" + id);
        }
        return fields;
    }
};

std::vector<std::string> ReadExpectedFields(JNIEnv* env, jobjectArray fields) {
    std::vector<std::string> expected;
    const auto count = env->GetArrayLength(fields);
    expected.reserve(count);
    for (jsize i = 0; i < count; ++i) {
        const auto field = static_cast<jstring>(env->GetObjectArrayElement(fields, i));
        expected.push_back(Common::Android::GetJString(env, field));
        env->DeleteLocalRef(field);
    }
    return expected;
}

class ContentIdentity {
public:
    ContentIdentity(JNIEnv* env_, jobject support_, const std::function<void()>& pulse_)
        : env{env_}, pulse{pulse_} {
        type = support_ ? env->GetObjectClass(support_) :
            env->FindClass("org/yuzu/yuzu_emu/duo/nativeplay/NativeGameSupport");
        RequirePrepare(type != nullptr, "Cannot find native content metadata support");
        stamp = env->GetStaticMethodID(type, "sourceStamp", "(Ljava/lang/String;)Ljava/lang/String;");
        RequirePrepare(stamp != nullptr, "Cannot read source freshness metadata");
    }
    ~ContentIdentity() {
        env->DeleteLocalRef(type);
    }

    Json Capture(const FileSys::VirtualFile& file, bool nup = false) {
        if (pulse) {
            pulse();
        }
        RequirePrepare(file && file->IsReadable() && file->GetSize() > 0,
                       "Selected content source is missing or unreadable");
        const size_t size = std::min<size_t>(file->GetSize(), nup ? 0x80 : 0xC00);
        const auto header = file->ReadBytes(size);
        RequirePrepare(header.size() == size, "Selected content source returned a short read");
        // NCA headers contain section master hashes; NUP superblocks contain the full TOC hash.
        // Combined with all backing-container stamps this avoids hashing ROM bodies per launch.
        Json result{{"path", file->GetFullPath()}, {"size", file->GetSize()},
                    {"header", Digest(std::string{header.begin(), header.end()})},
                    {"sources", Json::array()}};
        const auto paths = file->GetSourcePaths();
        RequirePrepare(!paths.empty(), "Selected content has no stable source provenance");
        for (const auto& path : paths) {
            if (pulse) {
                pulse();
            }
            const auto jpath = Common::Android::ToJString(env, path);
            const auto value = static_cast<jstring>(env->CallStaticObjectMethod(type, stamp, jpath));
            env->DeleteLocalRef(jpath);
            RequirePrepare(!env->ExceptionCheck() && value != nullptr,
                           "Selected content needs readable size and modification metadata");
            result["sources"].push_back(
                Json{{"path", path}, {"stamp", Common::Android::GetJString(env, value)}});
            env->DeleteLocalRef(value);
        }
        return result;
    }

private:
    JNIEnv* env;
    jclass type;
    jmethodID stamp;
    const std::function<void()>& pulse;
};

void CaptureUpdate(EffectiveInput& input, ContentIdentity& identity,
                   const FileSys::PatchManager::SelectedUpdate& update, u64 title_id,
                   const std::string& key) {
    if (!update.selected) {
        input.context[key] = nullptr;
        return;
    }
    RequirePrepare(update.raw && update.metadata_raw && update.version,
                   "Selected update is unreadable or has no authoritative numeric version");
    FileSys::NCA meta{update.metadata_raw};
    const auto version = ReadContentVersion(meta, FileSys::GetUpdateTitleID(title_id),
                                             FileSys::TitleType::Update);
    RequirePrepare(version && version == update.version,
                   "Selected update CNMT does not prove the exact chosen version");
    input.sources.push_back(update.raw);
    input.sources.push_back(update.metadata_raw);
    input.context[key] = Json{{"version", *version},
                              {"slot", update.source ? static_cast<int>(*update.source) : -1},
                              {"content", identity.Capture(update.raw)},
                              {"metadata", identity.Capture(update.metadata_raw)}};
}
Json ResolveSaveControl(EffectiveInput& input, ContentIdentity& identity,
                        const FileSys::ContentProviderUnion& provider) {
    auto& system = EmulationSession::GetInstance().System();
    u64 control_id = input.program_index == 1 && IndexedApplication(input.program_id) ?
        input.application_id : input.program_id;
    auto base = provider.GetEntry(control_id, FileSys::ContentRecordType::Control);
    if (!base) {
        const auto parent = provider.GetParentApplicationId(control_id);
        RequirePrepare(parent && *parent != control_id,
                       "Original game has no authoritative control metadata; NUP display metadata is not sufficient");
        control_id = provider.HasEntry(*parent, FileSys::ContentRecordType::Control) ?
            *parent : FileSys::GetUpdateTitleID(*parent);
        base = provider.GetEntry(control_id, FileSys::ContentRecordType::Control);
    }
    RequirePrepare(base && base->GetStatus() == Loader::ResultStatus::Success && base->GetRomFS(),
                   "Original control NCA is missing or unreadable");
    const FileSys::PatchManager patches{
        control_id, system.GetFileSystemController(), provider};
    input.context["control_base"] = identity.Capture(base->GetBaseFile());
    const auto update = patches.ResolveUpdate(FileSys::ContentRecordType::Control);
    CaptureUpdate(input, identity, update, control_id, "control_update");
    if (update.selected) {
        const FileSys::NCA effective{update.raw, base.get()};
        RequirePrepare(effective.GetStatus() == Loader::ResultStatus::Success && effective.GetRomFS(),
                       "Exact selected control update is unreadable");
    }
    // Production never applies LayeredFS to Control. Disable that no-op explicitly because
    // its setup calls GetOrCreateDirectoryRelative on the mod roots before checking the type.
    const auto romfs = patches.PatchRomFS(base.get(), base->GetRomFS(),
                                         FileSys::ContentRecordType::Control, nullptr, false);
    const auto directory = romfs ? FileSys::ExtractRomFS(romfs) : nullptr;
    RequirePrepare(directory != nullptr, "Effective control RomFS is unreadable");
    auto file = directory->GetFile("control.nacp");
    if (!file) {
        file = directory->GetFile("Control.nacp");
    }
    RequirePrepare(file && file->GetSize() >= sizeof(FileSys::RawNACP),
                   "Effective authoritative NACP is missing or truncated");
    const auto bytes = file->ReadBytes(sizeof(FileSys::RawNACP));
    RequirePrepare(bytes.size() == sizeof(FileSys::RawNACP), "Cannot read effective NACP");
    // Only the fixed save declarations are needed; do not decode or allocate title strings.
    FileSys::RawNACP raw{};
    std::memcpy(&raw, bytes.data(), sizeof(raw));
    Json unsupported = Json::array();
    if (static_cast<u64>(raw.bcat_delivery_cache_storage_size) != 0) {
        unsupported.push_back("bcat");
    }
    if (static_cast<u64>(raw.temporary_storage_size) != 0) {
        unsupported.push_back("temporary");
    }
    if (static_cast<u64>(raw.cache_storage_size) != 0 ||
        static_cast<u64>(raw.cache_storage_journal_size) != 0 ||
        static_cast<u64>(raw.cache_storage_data_and_journal_max_size) != 0 ||
        static_cast<u16>(raw.cache_storage_max_index) != 0) {
        unsupported.push_back("cache");
    }
    const Json provenance{
        {"control_id", std::format("{:016X}", control_id)},
        {"base", input.context.at("control_base")},
        {"update", input.context.at("control_update")},
        {"nacp_sha256", Digest(std::string{bytes.begin(), bytes.end()})}};
    return Json{
        {"save_owner_id", std::format("{:016X}", static_cast<u64>(raw.save_data_owner_id))},
        {"account_size", std::to_string(static_cast<u64>(raw.user_account_save_data_size))},
        {"account_journal_size", std::to_string(static_cast<u64>(raw.user_account_save_data_journal_size))},
        {"device_size", std::to_string(static_cast<u64>(raw.device_save_data_size))},
        {"device_journal_size", std::to_string(static_cast<u64>(raw.device_save_data_journal_size))},
        {"control_source", provenance.dump()},
        {"supported", unsupported.empty()},
        {"unsupported_save_kinds", unsupported},
        {"unsupported_saves", {
            {"bcat_delivery_cache_size", std::to_string(static_cast<u64>(raw.bcat_delivery_cache_storage_size))},
            {"temporary_size", std::to_string(static_cast<u64>(raw.temporary_storage_size))},
            {"cache_size", std::to_string(static_cast<u64>(raw.cache_storage_size))},
            {"cache_journal_size", std::to_string(static_cast<u64>(raw.cache_storage_journal_size))},
            {"cache_data_and_journal_max_size", std::to_string(static_cast<u64>(raw.cache_storage_data_and_journal_max_size))},
            {"cache_max_index", std::to_string(static_cast<u16>(raw.cache_storage_max_index))}}},
        {"account_max_size", std::to_string(static_cast<u64>(raw.user_account_save_data_max_size))},
        {"account_max_journal_size", std::to_string(static_cast<u64>(raw.user_account_save_data_max_journal_size))},
        {"device_max_size", std::to_string(static_cast<u64>(raw.device_save_data_max_size))},
        {"device_max_journal_size", std::to_string(static_cast<u64>(raw.device_save_data_max_journal_size))}};
}


EffectiveInput ResolveEffectiveInput(JNIEnv* env, jobject support, const std::string& path,
                                     jint program_index,
                                     const std::function<void()>& pulse = {},
                                     Json* save_control = nullptr) {
    RequirePrepare(program_index >= 0, "Invalid program index");
    auto& session = EmulationSession::GetInstance();
    auto& system = session.System();
    const auto file = Core::GetGameFileFromPath(system.GetFilesystem(), path);
    RequirePrepare(file != nullptr, "Cannot open selected source");
    auto provider = system.GetContentProviderUnion();
    FileSys::ManualContentProvider manual{*session.GetContentProvider()};
    provider.SetSlot(FileSys::ContentProviderUnionSlot::FrontendManual, &manual);
    ContentIdentity identity{env, support, pulse};
    EffectiveInput input;
    input.program_index = program_index;
    input.sources.push_back(file);
    input.context["source"] = identity.Capture(file, FileSys::NupContainer::IsNupFile(file));
    FileSys::VirtualDir exefs;
    FileSys::VirtualDir romfs;
    std::shared_ptr<FileSys::NCA> base;
    std::unique_ptr<FileSys::NupContainer> nup;

    if (FileSys::NupContainer::IsNupFile(file)) {
        RequirePrepare(program_index == 0, "Multi-program NUP input is unsupported");
        Loader::AppLoader_NUP loader{file};
        const auto packaged = loader.ReadPackagedExeFS();
        RequirePrepare(packaged.has_value(), "NUP has no coherent executable/RomFS version");
        input.version = packaged->first;
        exefs = packaged->second;
        FileSys::ProgramMetadata metadata;
        RequirePrepare(metadata.Load(exefs->GetFile("main.npdm")) == Loader::ResultStatus::Success,
                       "NUP NPDM is unreadable");
        input.program_id = metadata.GetTitleID();
        nup = std::make_unique<FileSys::NupContainer>(file);
        RequirePrepare(nup->GetStatus() == Loader::ResultStatus::Success,
                       "NUP metadata or table hash is invalid");
        const auto base_index = nup->FindBasePackage();
        const auto update_index = nup->FindBestUpdatePackage();
        for (const auto index : {base_index, update_index}) {
            RequirePrepare(!index || nup->GetPackages()[*index].MatchesProgramId(input.program_id),
                           "NUP executable and package program identities disagree");
        }
        romfs = FileSys::NupContainer::MergeShadowDirs(
            base_index ? nup->GetRomFSDir(*base_index) : nullptr,
            update_index ? nup->GetRomFSDir(*update_index) : nullptr);
        // Embedded NUP AoC is not mounted by Duet; do not turn it on during export.
        input.context["nup_embedded_aoc"] = "not-emulated";
    } else {
        const auto package = OpenRetailPackage(file, program_index);
        RequirePrepare(package && package->GetStatus() == Loader::ResultStatus::Success &&
                           !package->IsExtractedType(),
                       "Prepare needs a readable retail NSP/XCI or coherent NUP");
        input.program_id = SelectedRetailProgram(package, program_index);
        const auto indexed_application = program_index == 1 ?
            IndexedApplication(input.program_id) : std::nullopt;
        input.application_id = indexed_application.value_or(FileSys::GetBaseTitleID(input.program_id));
        RequirePrepare(input.program_id != 0 &&
                           (input.program_id & FileSys::AOC_TITLE_ID_MASK) ==
                               static_cast<u64>(program_index),
                       "Selected retail program index is not available");
        base = package->GetNCA(input.program_id, FileSys::ContentRecordType::Program);
        const auto meta = package->GetNCA(input.application_id, FileSys::ContentRecordType::Meta);
        const auto version = meta ? ReadContentVersion(*meta,
            input.application_id, FileSys::TitleType::Application) : std::nullopt;
        RequirePrepare(base && base->GetStatus() == Loader::ResultStatus::Success && version,
                       "Selected base NCA or authoritative application version is missing");
        input.version = *version;
        if (indexed_application) {
            const auto record = ProgramRecord(*meta, input.application_id,
                                             FileSys::TitleType::Application, 1);
            input.original_program_record = ProgramRecord(*meta, input.application_id,
                FileSys::TitleType::Application, 0);
            RequirePrepare(record && input.original_program_record &&
                               MatchesProgramRecord(base->GetBaseFile(), *record) &&
                               base->GetTitleId() == input.program_id,
                           "Application CNMT does not bind both edition programs");
            input.context["application_id"] = std::format("{:016X}", input.application_id);
            input.context["program_index"] = program_index;
        }
        // Include the selected launch container even when game-directory discovery is disabled.
        RequirePrepare(manual.AddEntriesFromContainer(package),
                       "Cannot snapshot selected container content registration");
        input.sources.push_back(base->GetBaseFile());
        input.sources.push_back(meta->GetBaseFile());
        input.context["base"] = identity.Capture(base->GetBaseFile());
        input.context["base_metadata"] = identity.Capture(meta->GetBaseFile());
        auto registered_base = provider.GetEntryRaw(
            input.program_id, FileSys::ContentRecordType::Program);
        if (!registered_base && indexed_application) {
            registered_base = provider.GetEntryRaw(
                input.application_id + 1, FileSys::ContentRecordType::Program);
        }
        RequirePrepare(registered_base != nullptr, "Boot provider has no selected base content");
        input.context["boot_base"] = identity.Capture(registered_base);
        RequirePrepare(input.context["boot_base"]["header"] == input.context["base"]["header"] &&
                           registered_base->GetSize() == base->GetBaseFile()->GetSize(),
                       "Boot provider base conflicts with the selected container");
        exefs = base->GetExeFS();
        RequirePrepare(base->GetRomFS() != nullptr, "Selected base RomFS is missing");
    }
    if (input.application_id == 0) {
        input.application_id = FileSys::GetBaseTitleID(input.program_id);
    }

    const FileSys::PatchManager patches{
        input.program_id, system.GetFileSystemController(), provider};
    if (!save_control) {
        RequirePrepare(!patches.HasEnabledMods(),
                       "Native preparation cannot use enabled game mods. Turn them off in this game's settings; emulation settings have not been changed.");
    }
    auto update = patches.ResolveUpdate();
    FileSys::PatchManager::SelectedUpdate original_update;
    const bool indexed = program_index == 1 && IndexedApplication(input.program_id).has_value();
    if (indexed) {
        const FileSys::PatchManager original_patches{
            input.application_id, system.GetFileSystemController(), provider};
        if (!save_control) {
            RequirePrepare(!original_patches.HasEnabledMods(), "Original application mods are unsupported");
        }
        original_update = original_patches.ResolveUpdate();
        RequirePrepare(!update.selected || original_update.selected,
                       "Selected edition update has no authoritative original application update");
        update = ResolveIndexedUpdate(provider, input.application_id, input.program_id, original_update);
    }
    CaptureUpdate(input, identity, update,
                  indexed ? input.application_id : input.program_id, "update");
    if (save_control) {
        // Save metadata needs the real selected source/provider and update policy, not an
        // executable/RomFS snapshot. Avoid mod factory queries that can create directories.
        RequirePrepare(!nup || !update.selected,
                       "NUP premerged RomFS cannot be combined with an external executable update");
        *save_control = ResolveSaveControl(input, identity, provider);
        return input;
    }
    if (nup) {
        RequirePrepare(!update.selected,
                       "NUP premerged RomFS cannot be combined with an external executable update");
    } else {
        std::shared_ptr<FileSys::NCA> effective = base;
        if (update.selected) {
            effective = std::make_shared<FileSys::NCA>(update.raw, base.get());
            RequirePrepare(effective->GetStatus() == Loader::ResultStatus::Success &&
                               effective->GetExeFS() && effective->GetRomFS(),
                           "Exact selected update cannot resolve complete ExeFS and RomFS");
            input.version = *update.version;
        }
        exefs = effective->GetExeFS();
        RequirePrepare(effective->GetRomFS() != nullptr, "Effective RomFS is missing");
        romfs = FileSys::ExtractRomFS(effective->GetRomFS());
        input.input_bytes += effective->GetRomFS()->GetSize();
    }
    RequirePrepare(romfs != nullptr, "Effective RomFS cannot be read");
    input.modules = ReadModules(exefs, input.program_id);
    for (const auto& module : exefs->GetFiles()) {
        input.input_bytes += module->GetSize();
    }
    if (nup) {
        input.input_bytes += file->GetSize();
    }
    using Kind = FileSys::NupContainer::PackageKind;
    input.packages.push_back({input.version == 0 ? Kind::Base : Kind::Update,
                              static_cast<u8>(input.version == 0 ? 0 : 1),
                              input.program_id, input.version, exefs, romfs});
    if (indexed) {
        const auto slot = provider.GetSlotForEntry(input.application_id, FileSys::ContentRecordType::Program);
        RequirePrepare(slot.has_value(), "Original program zero has no selected provider");
        const auto* source = provider.GetSlotProvider(*slot);
        auto original = source->GetEntry(input.application_id, FileSys::ContentRecordType::Program);
        const auto meta = source->GetEntry(input.application_id, FileSys::ContentRecordType::Meta);
        const auto version = meta ? ReadContentVersion(*meta, input.application_id,
            FileSys::TitleType::Application) : std::nullopt;
        const auto record = meta ? ProgramRecord(*meta, input.application_id,
            FileSys::TitleType::Application, 0) : std::nullopt;
        RequirePrepare(original && original->GetStatus() == Loader::ResultStatus::Success &&
                           original->GetTitleId() == input.application_id && version && record &&
                           MatchesProgramRecord(original->GetBaseFile(), *record) &&
                           input.original_program_record &&
                           MatchesProgramRecord(original->GetBaseFile(), *input.original_program_record),
                       "Original program zero is missing or its CNMT conflicts with the selected edition");
        input.context["original_base"] = identity.Capture(original->GetBaseFile());
        input.context["original_metadata"] = identity.Capture(meta->GetBaseFile());
        input.context["original_slot"] = static_cast<int>(*slot);
        input.sources.push_back(original->GetBaseFile());
        input.sources.push_back(meta->GetBaseFile());
        CaptureUpdate(input, identity, original_update, input.application_id, "original_update");
        if (original_update.selected) {
            original = std::make_unique<FileSys::NCA>(original_update.raw, original.get());
        }
        RequirePrepare(original->GetStatus() == Loader::ResultStatus::Success &&
                           original->GetRomFS() && original->GetExeFS(),
                       "Exact original program zero update cannot be read");
        const auto original_modules = ReadModules(original->GetExeFS(), input.application_id);
        input.packages.front().program0_romfs = FileSys::ExtractRomFS(original->GetRomFS());
        RequirePrepare(input.packages.front().program0_romfs != nullptr,
                       "Original program zero RomFS cannot be read");
        input.input_bytes += original->GetRomFS()->GetSize();
        input.programs.push_back(Json{{"index", 0},
            {"title_id", std::format("{:016X}", input.application_id)},
            {"version", original_update.selected ? *original_update.version : *version},
            {"build_ids", original_modules}});
    }
    input.programs.push_back(Json{{"index", program_index},
        {"title_id", std::format("{:016X}", input.program_id)}, {"version", input.version},
        {"build_ids", input.modules}});

    const auto base_id = indexed ? input.application_id : FileSys::GetBaseTitleID(input.program_id);
    Json disabled = Json::object();
    const auto parent_id = provider.GetParentApplicationId(input.program_id);
    for (const auto id : {base_id, input.program_id, parent_id.value_or(base_id)}) {
        if (const auto it = Settings::values.disabled_addons.find(id);
            it != Settings::values.disabled_addons.end()) {
            disabled[std::format("{:016X}", id)] = it->second;
        }
    }
    input.context["disabled_addons"] = disabled;
    const auto disabled_it = Settings::values.disabled_addons.find(base_id);
    bool dlc_disabled = disabled_it != Settings::values.disabled_addons.end() &&
        std::ranges::find(disabled_it->second, "DLC") != disabled_it->second.end();
    if (indexed) {
        for (const auto id : {input.program_id, FileSys::GetBaseTitleID(input.program_id)}) {
            if (const auto it = Settings::values.disabled_addons.find(id);
                it != Settings::values.disabled_addons.end()) {
                disabled[std::format("{:016X}", id)] = it->second;
                dlc_disabled |= std::ranges::find(it->second, "DLC") != it->second.end();
            }
        }
        input.context["disabled_addons"] = disabled;
    }
    if (!dlc_disabled) {
        for (const auto& entry : provider.ListEntriesFilter(
                 FileSys::TitleType::AOC, FileSys::ContentRecordType::Data, std::nullopt)) {
            if (!indexed && FileSys::GetBaseTitleID(entry.title_id) != base_id) {
                continue;
            }
            const auto slot = provider.GetSlotForEntry(entry.title_id, FileSys::ContentRecordType::Data);
            RequirePrepare(slot.has_value(), "Enabled DLC has no selected provider");
            const auto* source = provider.GetSlotProvider(*slot);
            auto data = source->GetEntry(entry.title_id, FileSys::ContentRecordType::Data);
            auto meta = source->GetEntry(entry.title_id, FileSys::ContentRecordType::Meta);
            if (indexed) {
                const auto application = meta ? AddonApplication(*meta, entry.title_id) : std::nullopt;
                RequirePrepare(application || FileSys::GetBaseTitleID(entry.title_id) != base_id,
                               "Original application DLC has no authoritative association");
                if (application != input.application_id) {
                    continue;
                }
            }
            const auto version = meta ? ReadContentVersion(*meta, entry.title_id,
                FileSys::TitleType::AOC) : std::nullopt;
            RequirePrepare(data && data->GetStatus() == Loader::ResultStatus::Success &&
                               data->GetRomFS() && version,
                           "Enabled DLC data or exact metadata is unreadable");
            const FileSys::PatchManager data_patches{
                entry.title_id, system.GetFileSystemController(), provider};
            RequirePrepare(!data_patches.HasEnabledMods(), "Enabled DLC mods are unsupported");
            const auto data_update = data_patches.ResolveUpdate(FileSys::ContentRecordType::Data);
            const auto key = std::format("dlc_{:016X}", entry.title_id);
            CaptureUpdate(input, identity, data_update, entry.title_id, key + "_update");
            Json dlc_identity{{"title_id", std::format("{:016X}", entry.title_id)},
                               {"version", data_update.selected ? *data_update.version : *version},
                               {"slot", static_cast<int>(*slot)},
                               {"source", identity.Capture(data->GetBaseFile())},
                               {"metadata", identity.Capture(meta->GetBaseFile())}};
            input.sources.push_back(data->GetBaseFile());
            input.sources.push_back(meta->GetBaseFile());
            if (data_update.selected) {
                auto patched = std::make_unique<FileSys::NCA>(data_update.raw, data.get());
                RequirePrepare(patched->GetStatus() == Loader::ResultStatus::Success &&
                                   patched->GetRomFS(),
                               "Exact selected DLC data patch cannot be read");
                data = std::move(patched);
            }
            const auto tree = FileSys::ExtractRomFS(data->GetRomFS());
            RequirePrepare(tree != nullptr, "Enabled DLC RomFS cannot be read");
            input.input_bytes += data->GetRomFS()->GetSize();
            input.packages.push_back({Kind::Addon, 0, entry.title_id,
                data_update.selected ? *data_update.version : *version, nullptr, tree});
            input.context[key] = dlc_identity;
            input.dlc.push_back(std::move(dlc_identity));
        }
    }
    input.context["program_id"] = std::format("{:016X}", input.program_id);
    input.context["version"] = input.version;
    input.context["modules"] = input.modules;
    if (indexed) {
        input.context["programs"] = input.programs;
    }
    input.context["mods"] = "none";
    input.content_key = Digest(input.context.dump());
    return input;
}

class PrepareProgress {
public:
    PrepareProgress(JNIEnv* env_, jobject callback_) : env{env_}, callback{callback_} {
        const auto type = env->GetObjectClass(callback);
        update = env->GetMethodID(type, "update", "(JJ)Z");
        env->DeleteLocalRef(type);
        RequirePrepare(update != nullptr, "Prepare progress callback is unavailable");
    }

    void Report(u64 done, u64 total) {
        if (!cancel.load() &&
            !env->CallBooleanMethod(callback, update, static_cast<jlong>(done),
                                     static_cast<jlong>(total))) {
            cancel.store(true);
        }
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            cancel.store(true);
        }
    }
    void Check() const {
        RequirePrepare(!cancel.load(), "Prepare cancelled");
    }
    std::atomic_bool cancel{false};

private:
    JNIEnv* env;
    jobject callback;
    jmethodID update;
};

void ValidateResolvedTree(const FileSys::VirtualDir& expected, const FileSys::VirtualDir& observed) {
    RequirePrepare(expected && observed, "Prepared indexed RomFS tree is missing");
    auto expected_files = expected->GetFiles();
    auto observed_files = observed->GetFiles();
    auto expected_dirs = expected->GetSubdirectories();
    auto observed_dirs = observed->GetSubdirectories();
    const auto by_name = [](const auto& left, const auto& right) {
        return left->GetName() < right->GetName();
    };
    std::ranges::sort(expected_files, by_name);
    std::ranges::sort(observed_files, by_name);
    std::ranges::sort(expected_dirs, by_name);
    std::ranges::sort(observed_dirs, by_name);
    RequirePrepare(expected_files.size() == observed_files.size() &&
                       expected_dirs.size() == observed_dirs.size(),
                   "Prepared indexed RomFS tree is incomplete");
    for (size_t i = 0; i < expected_files.size(); ++i) {
        RequirePrepare(expected_files[i]->GetName() == observed_files[i]->GetName() &&
                           expected_files[i]->GetSize() == observed_files[i]->GetSize(),
                       "Prepared indexed RomFS member mismatch");
    }
    for (size_t i = 0; i < expected_dirs.size(); ++i) {
        RequirePrepare(expected_dirs[i]->GetName() == observed_dirs[i]->GetName(),
                       "Prepared indexed RomFS directory mismatch");
        ValidateResolvedTree(expected_dirs[i], observed_dirs[i]);
    }
}

void ValidateOutput(const EffectiveInput& input, const FileSys::VirtualFile& file) {
    FileSys::NupContainer output{file};
    RequirePrepare(output.GetStatus() == Loader::ResultStatus::Success &&
                       output.GetPackages().size() == input.packages.size(),
                   "Prepared snapshot metadata validation failed");
    for (size_t i = 0; i < input.packages.size(); ++i) {
        const auto& expected = input.packages[i];
        const auto& observed = output.GetPackages()[i];
        RequirePrepare(observed.kind == expected.kind && observed.title_id == expected.title_id &&
                           observed.version == expected.version &&
                           observed.standalone_update == ((expected.flags & 1) != 0) &&
                           output.GetRomFSDir(i),
                       "Prepared snapshot content identity mismatch");
        if (i == 0) {
            RequirePrepare(ReadModules(output.GetExeFSDir(i), input.program_id) == input.modules,
                           "Prepared snapshot full module identity mismatch");
            if (expected.program0_romfs) {
                RequirePrepare(output.HasIndexedProgramLayout(i),
                               "Prepared indexed executable aliases are missing or incoherent");
                ValidateResolvedTree(expected.program0_romfs,
                    output.GetPackageDir(i)->GetDirectoryRelative("program/0/romfs"));
            }
        }
    }
}

std::string OutputHeader(const FileSys::VirtualFile& file) {
    const auto bytes = file->ReadBytes(0x80);
    RequirePrepare(bytes.size() == 0x80, "Prepared snapshot header is unreadable");
    return Digest(std::string{bytes.begin(), bytes.end()});
}

void ValidateExistingSnapshot(const EffectiveInput& input, const std::string& destination) {
    const fs::path sidecar{destination + ".json"};
    RequirePrepare(fs::is_regular_file(destination) && fs::is_regular_file(sidecar) &&
                       fs::file_size(sidecar) <= 1024 * 1024,
                   "Prepare input generation is incomplete; choose a fresh generation");
    const auto metadata = Json::parse(Common::FS::ReadStringFromFile(
        sidecar, Common::FS::FileType::BinaryFile));
    const auto existing = Core::GetGameFileFromPath(
        EmulationSession::GetInstance().System().GetFilesystem(), destination);
    RequirePrepare(existing && metadata.value("title_id", "") ==
                       std::format("{:016X}", input.program_id) &&
                       metadata.value("version", u64{0x100000000}) == input.version &&
                       metadata.value("content_key", "") == input.content_key &&
                       metadata.at("build_ids") == Json(input.modules) &&
                       metadata.at("enabled_dlc") == input.dlc &&
                       metadata.value("input_bytes", u64{}) == existing->GetSize() &&
                       metadata.value("nup_header", "") == OutputHeader(existing),
                   "Prepare input generation is corrupt or mismatched; do not overwrite it");
    if (input.program_index == 1 || metadata.contains("program_index")) {
        RequirePrepare(metadata.at("program_index") == input.program_index &&
                           metadata.at("application_id") == std::format("{:016X}", input.application_id) &&
                           metadata.at("programs") == input.programs,
                       "Prepare indexed program identity metadata is missing or mismatched");
    }
    ValidateOutput(input, existing);
}

void CommitPrepareFile(const fs::path& path) {
    // Immutable hard links retain read-only permissions; bypass IOFile's read-only mmap path.
    const int file = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    RequirePrepare(file >= 0, "Cannot open Prepare generation file for data sync");
    const bool committed = fsync(file) == 0;
    const bool closed = close(file) == 0;
    RequirePrepare(committed && closed, "Cannot commit and close Prepare generation file");
}

void SyncPrepareDirectory(const fs::path& output) {
    if (const auto error = Common::FS::SyncDirectory(output.parent_path())) {
        throw std::runtime_error("Cannot sync Prepare generation directory: " + error.message());
    }
    if (const auto error = Common::FS::SyncDirectory(output.parent_path().parent_path())) {
        throw std::runtime_error("Cannot sync Prepare generation parent: " + error.message());
    }
}

void PublishMetadata(const fs::path& part, const fs::path& destination) {
    // This export exclusively reserved the enclosing generation, not just the staging file.
    // No other producer may write here; FUSE supports rename but may reject NOREPLACE.
    RequirePrepare(!fs::exists(destination), "Prepare input metadata already exists");
    std::error_code error;
    fs::rename(part, destination, error);
    RequirePrepare(!error, "Cannot atomically publish Prepare input metadata");
}

} // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_org_yuzu_yuzu_1emu_NativeLibrary_getNativeSaveBinding(
    JNIEnv* env, jobject, jstring jpath, jint program_index) {
    try {
        RequirePrepare(gettid() != getpid(), "Resolve native saves on a worker thread");
        auto& session = EmulationSession::GetInstance();
        RequirePrepare(!session.IsRunning(), "Resolve native saves before starting a game");
        auto& system = session.System();
        // Reuse the frontend's live profile list. A new ProfileManager can create profiles
        // and write profiles.dat, which is forbidden in this read-only resolver.
        const auto& manager = system.GetProfileManager();
        const auto selected = Settings::values.current_user.GetValue();
        RequirePrepare(selected >= 0, "No real user profile is selected");
        const auto user = manager.GetUser(static_cast<std::size_t>(selected));
        Service::Account::ProfileBase profile{};
        RequirePrepare(user && user->IsValid() && manager.GetProfileBase(*user, profile),
                       "Selected user profile is missing or invalid");
        const auto root = NativeSaveBinding::PhysicalRoot(
            Common::FS::GetEdenPath(Common::FS::EdenPath::SaveDir));
        FileSys::RealVfsFilesystem filesystem;
        const auto directory = filesystem.OpenDirectory(
            Common::FS::PathToUTF8String(root), FileSys::OpenMode::Read);
        RequirePrepare(directory != nullptr, "Cannot open physical SaveDir read-only");
        Json binding;
        const auto input = ResolveEffectiveInput(
            env, nullptr, Common::Android::GetJString(env, jpath), program_index, {}, &binding);
        const auto owner = std::stoull(binding.at("save_owner_id").get<std::string>(), nullptr, 16);
        RequirePrepare(owner != 0, "Authoritative NACP does not declare a save owner");
        const auto path = [&](FileSys::SaveDataType kind, u128 uid) {
            return Common::FS::PathToUTF8String(NativeSaveBinding::PhysicalPath(
                root, FileSys::SaveDataFactory::GetFullPath(
                    {}, directory, FileSys::SaveDataSpaceId::User, kind, owner, uid, 0)));
        };
        binding["format"] = 1;
        binding["program_id"] = std::format("{:016X}", input.program_id);
        binding["user_id"] = NativeSaveBinding::UserFolder(*user);
        binding["user_uid_words"] = NativeSaveBinding::UserUidWords(*user);
        binding["user_name"] = Common::StringFromFixedZeroTerminatedBuffer(
            reinterpret_cast<const char*>(profile.username.data()), profile.username.size());
        binding["save_root"] = Common::FS::PathToUTF8String(root);
        binding["account_path"] = nullptr;
        binding["device_path"] = nullptr;
        if (binding.at("account_size") != "0" || binding.at("account_journal_size") != "0") {
            binding["account_path"] = path(FileSys::SaveDataType::Account, user->AsU128());
        }
        if (binding.at("device_size") != "0" || binding.at("device_journal_size") != "0") {
            binding["device_path"] = path(FileSys::SaveDataType::Device, {});
        }
        RequirePrepare(Settings::values.current_user.GetValue() == selected &&
                           manager.GetUser(static_cast<std::size_t>(selected)) == user &&
                           NativeSaveBinding::PhysicalRoot(
                               Common::FS::GetEdenPath(Common::FS::EdenPath::SaveDir)) == root,
                       "Selected profile or save root changed while resolving native saves");
        return Common::Android::ToJString(env, binding.dump());
    } catch (const std::exception& error) {
        if (env->ExceptionCheck()) {
            return nullptr;
        }
        const auto exception = env->FindClass("java/lang/IllegalStateException");
        if (exception) {
            env->ThrowNew(exception, error.what());
            env->DeleteLocalRef(exception);
        }
        return nullptr;
    }
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_yuzu_yuzu_1emu_duo_nativeplay_NativeGameSupport_readPrepareProgramId(
    JNIEnv* env, jobject, jstring jpath, jint program_index) {
    if (program_index < 0) {
        return nullptr;
    }
    try {
        const auto file = Core::GetGameFileFromPath(
            EmulationSession::GetInstance().System().GetFilesystem(),
            Common::Android::GetJString(env, jpath));
        if (!file) {
            return nullptr;
        }
        u64 program_id{};
        if (FileSys::NupContainer::IsNupFile(file)) {
            Loader::AppLoader_NUP loader{file};
            if (program_index != 0 ||
                loader.ReadProgramId(program_id) != Loader::ResultStatus::Success) {
                return nullptr;
            }
        } else {
            const auto package = OpenRetailPackage(file, program_index);
            if (!package || package->GetStatus() != Loader::ResultStatus::Success) {
                return nullptr;
            }
            program_id = SelectedRetailProgram(package, program_index);
        }
        return program_id == 0 ? nullptr :
            Common::Android::ToJString(env, std::to_string(program_id));
    } catch (const std::exception&) {
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
        return nullptr;
    }
}

extern "C" JNIEXPORT jobjectArray JNICALL
Java_org_yuzu_yuzu_1emu_duo_nativeplay_NativeGameSupport_readGameBuild(
    JNIEnv* env, jobject support, jstring jpath, jint program_index) {
    try {
        const auto input = ResolveEffectiveInput(
            env, support, Common::Android::GetJString(env, jpath), program_index);
        return Common::Android::ToJStringArray(env, input.Fields());
    } catch (const std::exception& error) {
        if (env->ExceptionCheck()) {
            return nullptr;
        }
        const auto exception = env->FindClass("java/lang/IllegalStateException");
        if (exception) {
            env->ThrowNew(exception, error.what());
            env->DeleteLocalRef(exception);
        }
        return nullptr;
    }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_yuzu_yuzu_1emu_duo_nativeplay_NativeGameSupport_isPrepareInputCompleteNative(
    JNIEnv* env, jobject support, jstring jpath, jint program_index, jobjectArray jexpected,
    jstring jdestination) {
    try {
        RequirePrepare(program_index == 0 || program_index == 1, "Unsupported native program index");
        const auto input = ResolveEffectiveInput(
            env, support, Common::Android::GetJString(env, jpath), program_index);
        RequirePrepare(input.Fields() == ReadExpectedFields(env, jexpected) &&
                           ((program_index == 0 && (input.program_id & 0xFFF) == 0) ||
                            (program_index == 1 && IndexedApplication(input.program_id))),
                       "Selected effective content does not match the expected input");
        ValidateExistingSnapshot(input, Common::Android::GetJString(env, jdestination));
        return true;
    } catch (const std::exception&) {
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
        return false;
    }
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_yuzu_yuzu_1emu_duo_nativeplay_NativeGameSupport_exportPrepareInputNative(
    JNIEnv* env, jobject support, jstring jpath, jint program_index, jobjectArray jexpected,
    jstring jdestination, jobject callback, jboolean immutable_local_nup) {
    const auto destination = Common::Android::GetJString(env, jdestination);
    const fs::path output{destination};
    const fs::path sidecar{destination + ".json"};
    const fs::path sidecar_part{destination + ".json.part"};
    bool owns_output = false;
    bool owns_sidecar = false;
    bool owns_sidecar_part = false;
    bool owns_generation = false;
    try {
        RequirePrepare(program_index == 0 || program_index == 1, "Unsupported native program index");
        PrepareProgress progress{env, callback};
        progress.Report(0, 0);
        progress.Check();
        const auto path = Common::Android::GetJString(env, jpath);
        auto input = ResolveEffectiveInput(env, support, path, program_index, [&] {
            progress.Report(0, 0);
            progress.Check();
        });
        const auto expected = ReadExpectedFields(env, jexpected);
        RequirePrepare(input.Fields() == expected,
                       "Selected code, update, DLC, mods or source changed; choose native again");
        RequirePrepare((program_index == 0 && (input.program_id & 0xFFF) == 0) ||
                           (program_index == 1 && IndexedApplication(input.program_id)),
                       "Native consumer does not support this selected program");
        if (fs::exists(output) || fs::exists(sidecar)) {
            ValidateExistingSnapshot(input, destination);
            progress.Report(input.input_bytes, input.input_bytes);
            progress.Check();
            // Old entries may predate durable publication. Validation alone cannot establish
            // durability; do not delete these unowned names if committing them fails.
            CommitPrepareFile(output);
            CommitPrepareFile(sidecar);
            SyncPrepareDirectory(output);
            return nullptr;
        }
        RequirePrepare(!fs::exists(sidecar_part), "Prepare input metadata staging already exists");
        bool linked = false;
        if (immutable_local_nup && FileSys::NupContainer::IsNupFile(input.sources.front())) {
            try {
                // Only an already complete single-version stock snapshot is reusable byte-for-byte.
                ValidateOutput(input, input.sources.front());
                std::error_code error;
                RequirePrepare(fs::create_directory(output.parent_path(), error),
                               "Cannot reserve Prepare input generation");
                owns_generation = true;
                fs::create_hard_link(fs::path{path}, output, error);
                linked = !error;
                if (!linked) {
                    RequirePrepare(fs::remove(output.parent_path(), error),
                                   "Cannot release unused Prepare input generation");
                    owns_generation = false;
                }
            } catch (const std::exception&) {
                // The source needs flattening/filtering; serialize its effective trees below.
            }
        }
        if (!linked) {
            FileSys::Nup::PackOptions options;
            // Raw members remain bounded-read views during validation and native gameplay.
            options.zstd_level = 0;
            std::string error;
            const auto result = FileSys::Nup::PackResolved(
                input.packages, output, options,
                [&](const FileSys::Nup::PackProgress& state) {
                    progress.Report(state.bytes_done, state.bytes_total);
                }, &progress.cancel, &error);
            RequirePrepare(result == FileSys::Nup::PackResult::Success,
                           result == FileSys::Nup::PackResult::Cancelled ? "Prepare cancelled" :
                               error.empty() ? "Cannot write effective Prepare input" : error.c_str());
            owns_generation = true;
        }
        owns_output = true;
        if (linked) {
            // A hard link persists no file data by itself, even for an immutable source.
            CommitPrepareFile(output);
        }
        progress.Check();
        const auto exported = Core::GetGameFileFromPath(
            EmulationSession::GetInstance().System().GetFilesystem(), destination);
        RequirePrepare(exported != nullptr, "Cannot reopen completed Prepare input");
        ValidateOutput(input, exported);
        RequirePrepare(ResolveEffectiveInput(env, support, path, program_index, [&] {
            progress.Report(input.input_bytes, input.input_bytes);
            progress.Check();
        }).Fields() == expected,
                       "Selected source or effective content changed during Prepare export");
        progress.Report(input.input_bytes, input.input_bytes);
        progress.Check();
        const Json metadata{{"title_id", std::format("{:016X}", input.program_id)},
                             {"version", input.version}, {"content_key", input.content_key},
                             {"build_ids", input.modules}, {"enabled_dlc", input.dlc},
                             {"program_index", input.program_index},
                             {"application_id", std::format("{:016X}", input.application_id)},
                             {"programs", input.programs},
                             {"input_bytes", exported->GetSize()},
                             {"nup_header", OutputHeader(exported)}};
        const auto bytes = metadata.dump();
        const int reservation = open(sidecar_part.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        RequirePrepare(reservation >= 0, "Cannot reserve Prepare input metadata staging");
        owns_sidecar_part = true;
        RequirePrepare(close(reservation) == 0, "Cannot close Prepare metadata reservation");
        Common::FS::IOFile meta{sidecar_part, Common::FS::FileAccessMode::Write};
        RequirePrepare(meta.IsOpen() && meta.WriteString(bytes) == bytes.size() && meta.Commit(),
                       "Cannot sync Prepare input identity metadata");
        RequirePrepare(meta.Close(), "Cannot close Prepare input identity metadata");
        progress.Report(input.input_bytes, input.input_bytes);
        progress.Check();
        PublishMetadata(sidecar_part, sidecar);
        owns_sidecar_part = false;
        owns_sidecar = true;
        SyncPrepareDirectory(output);
        return nullptr;
    } catch (const std::exception& error) {
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
        std::error_code ignored;
        if (owns_sidecar_part) {
            fs::remove(sidecar_part, ignored);
        }
        if (owns_sidecar) {
            fs::remove(sidecar, ignored);
        }
        if (owns_output) {
            fs::remove(output, ignored);
        }
        if (owns_generation) {
            fs::remove(output.parent_path(), ignored);
        }
        return Common::Android::ToJString(env, error.what());
    }
}
