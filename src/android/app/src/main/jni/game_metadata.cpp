// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <format>
#include "common/android/android_common.h"
#include "common/common_funcs.h"
#include "common/hex_util.h"
#include "core/core.h"
#include "core/file_sys/card_image.h"
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/fs_filesystem.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/program_metadata.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/submission_package.h"
#include "core/loader/loader.h"
#include "core/loader/nro.h"
#include "core/loader/nso.h"
#include "core/loader/nup.h"
#include "native.h"

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

using GameExeFS = std::pair<u32, FileSys::VirtualDir>;

std::optional<u32> ReadApplicationVersion(const FileSys::NSP& package, u64 program_id) {
    const auto application_id = FileSys::GetBaseTitleID(program_id);
    const auto meta = package.GetNCA(application_id, FileSys::ContentRecordType::Meta);
    if (!meta) {
        return std::nullopt;
    }
    for (const auto& dir : meta->GetSubdirectories()) {
        for (const auto& file : dir->GetFiles()) {
            FileSys::CNMTHeader header{};
            if (file->GetExtension() == "cnmt" && file->ReadObject(&header) == sizeof(header) &&
                header.type == FileSys::TitleType::Application && header.title_id == application_id) {
                return header.title_version;
            }
        }
    }
    return std::nullopt;
}

std::optional<GameExeFS> ReadRetailExeFS(const FileSys::NSP& package, size_t program_index) {
    const auto program_id = package.GetProgramTitleID();
    if (package.GetStatus() != Loader::ResultStatus::Success || package.IsExtractedType() ||
        program_id == 0 || (program_id & FileSys::AOC_TITLE_ID_MASK) != program_index) {
        return std::nullopt;
    }

    const auto program = package.GetNCA(program_id, FileSys::ContentRecordType::Program);
    const auto version = ReadApplicationVersion(package, program_id);
    if (!program || !program->GetExeFS() || !version) {
        return std::nullopt;
    }
    return GameExeFS{*version, program->GetExeFS()};
}

std::shared_ptr<FileSys::NSP> OpenRetailPackage(const FileSys::VirtualFile& file,
                                               size_t program_index) {
    switch (Loader::GuessFromFilename(file->GetName())) {
    case Loader::FileType::NSP:
    case Loader::FileType::NSZ:
        return std::make_shared<FileSys::NSP>(file, 0, program_index, false);
    case Loader::FileType::XCI:
    case Loader::FileType::XCZ: {
        FileSys::XCI card{file, 0, program_index, false};
        if (card.GetStatus() == Loader::ResultStatus::Success) {
            return card.GetSecurePartitionNSP();
        }
        if (Loader::GuessFromFilename(file->GetName()) == Loader::FileType::XCZ) {
            return std::make_shared<FileSys::NSP>(file, 0, program_index, false);
        }
        return nullptr;
    }
    default:
        return nullptr;
    }
}

} // namespace

extern "C" JNIEXPORT jobjectArray JNICALL
Java_org_yuzu_yuzu_1emu_duo_nativeplay_NativeGameSupport_readGameBuild(JNIEnv* env, jobject,
                                                                     jstring jpath,
                                                                     jint program_index) {
    if (program_index < 0) {
        return nullptr;
    }
    auto& session = EmulationSession::GetInstance();
    auto& system = session.System();
    const auto path = Common::Android::GetJString(env, jpath);
    const auto file = Core::GetGameFileFromPath(system.GetFilesystem(), path);
    if (!file) {
        return nullptr;
    }

    auto provider = system.GetContentProviderUnion();
    std::optional<FileSys::ManualContentProvider> manual;
    std::optional<GameExeFS> packaged;
    if (FileSys::NupContainer::IsNupFile(file)) {
        if (program_index != 0) {
            return nullptr;
        }
        packaged = Loader::AppLoader_NUP{file}.ReadPackagedExeFS();
    } else if (const auto package = OpenRetailPackage(file, program_index)) {
        packaged = ReadRetailExeFS(*package, program_index);
        if (packaged) {
            // Eden registers this container's content at launch. Use a local snapshot so lookup
            // has the same update choices without changing the library's registered content.
            manual.emplace(*session.GetContentProvider());
            if (!manual->AddEntriesFromContainer(package)) {
                return nullptr;
            }
            provider.SetSlot(FileSys::ContentProviderUnionSlot::FrontendManual, &*manual);
        }
    }
    if (!packaged) {
        return nullptr;
    }

    auto& [version, exefs] = *packaged;
    FileSys::ProgramMetadata metadata;
    if (metadata.Load(exefs->GetFile("main.npdm")) != Loader::ResultStatus::Success) {
        return nullptr;
    }
    const auto program_id = metadata.GetTitleID();
    const FileSys::PatchManager patches{program_id, system.GetFileSystemController(), provider};
    if (auto [updated_exefs, update_version] = patches.ReadExeFSUpdate(); updated_exefs) {
        if (!update_version ||
            metadata.Load(updated_exefs->GetFile("main.npdm")) != Loader::ResultStatus::Success ||
            metadata.GetTitleID() != program_id) {
            return nullptr;
        }
        version = *update_version;
        exefs = std::move(updated_exefs);
    }

    std::vector<std::string> fields{
        std::format("{:016X}", program_id), std::to_string(version)};
    for (const auto& module : exefs->GetFiles()) {
        const auto name = module->GetName();
        if (name != "main" && name != "sdk" && name != "rtld" && !name.starts_with("subsdk")) {
            continue;
        }
        Loader::NSOHeader header{};
        if (module->ReadObject(&header) == sizeof(header) &&
            header.magic == Common::MakeMagic('N', 'S', 'O', '0')) {
            fields.emplace_back(name + "=" + Common::HexToString(header.build_id, false));
        }
    }
    return Common::Android::ToJStringArray(env, fields);
}
