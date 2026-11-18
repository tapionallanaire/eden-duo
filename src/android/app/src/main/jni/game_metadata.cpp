// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <format>
#include "common/android/android_common.h"
#include "common/common_funcs.h"
#include "common/hex_util.h"
#include "core/core.h"
#include "core/file_sys/fs_filesystem.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/program_metadata.h"
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

extern "C" JNIEXPORT jobjectArray JNICALL
Java_org_yuzu_yuzu_1emu_duo_nativeplay_NativeGameSupport_readNupBuild(JNIEnv* env, jobject,
                                                                    jstring jpath,
                                                                    jint program_index) {
    if (program_index != 0) {
        return nullptr;
    }
    auto& system = EmulationSession::GetInstance().System();
    const auto path = Common::Android::GetJString(env, jpath);
    const auto file = Core::GetGameFileFromPath(system.GetFilesystem(), path);
    if (!file || !FileSys::NupContainer::IsNupFile(file)) {
        return nullptr;
    }

    Loader::AppLoader_NUP loader{file};
    const auto packaged = loader.ReadPackagedExeFS();
    if (!packaged) {
        return nullptr;
    }
    const auto& [version, exefs] = *packaged;
    FileSys::ProgramMetadata metadata;
    if (metadata.Load(exefs->GetFile("main.npdm")) != Loader::ResultStatus::Success) {
        return nullptr;
    }

    std::vector<std::string> fields{
        std::format("{:016X}", metadata.GetTitleID()), std::to_string(version)};
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
