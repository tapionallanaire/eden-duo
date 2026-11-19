// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <filesystem>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>

#include "common/uuid.h"
#include "core/loader/nup.h"

namespace NativeSaveBinding {

inline u64 NupSaveOwner(Loader::AppLoader_NUP& loader, u64 program_id) {
    u64 owner{};
    if (!loader.ReadPackagedExeFS() ||
        loader.ReadProgramId(owner) != Loader::ResultStatus::Success ||
        owner == 0 || owner != program_id) {
        throw std::runtime_error("NUP emulated save owner does not match the selected program");
    }
    return owner;
}

inline std::array<std::string, 2> UserUidWords(const Common::UUID& user) {
    const auto words = user.AsU128();
    return {std::format("{:016x}", words[0]), std::format("{:016x}", words[1])};
}

inline std::string UserFolder(const Common::UUID& user) {
    const auto words = user.AsU128();
    return std::format("{:016X}{:016X}", words[1], words[0]);
}

inline std::filesystem::path PhysicalRoot(const std::filesystem::path& configured) {
    // RealVfs also supports Android document URIs. Native hosts need actual POSIX backing.
    if (!configured.is_absolute() || configured.generic_string().find("://") != std::string::npos) {
        throw std::runtime_error("Native saves require an absolute physical SaveDir, not a document URI");
    }
    const auto root = std::filesystem::canonical(configured);
    if (!std::filesystem::is_directory(root)) {
        throw std::runtime_error("Native SaveDir backing is not an existing physical directory");
    }
    return root;
}

inline std::filesystem::path PhysicalPath(const std::filesystem::path& root,
                                          std::string_view save_path) {
    namespace fs = std::filesystem;
    // GetFullPath returns a VFS-rooted path, not a host absolute path.
    const auto relative = fs::path{save_path}.relative_path();
    if (relative.empty()) {
        throw std::runtime_error("Native save path is empty");
    }
    auto path = root;
    for (const auto& component : relative) {
        if (component == "." || component == ".." || component.has_root_path()) {
            throw std::runtime_error("Native save path escapes SaveDir");
        }
        path /= component;
        std::error_code error;
        const auto status = fs::symlink_status(path, error);
        if (error && error != std::errc::no_such_file_or_directory) {
            throw fs::filesystem_error("Cannot inspect native save backing", path, error);
        }
        if (fs::exists(status)) {
            // Also rejects dangling links and files. Do not create absent save directories.
            if (!fs::is_directory(path)) {
                throw std::runtime_error("Native save backing contains a non-directory component");
            }
            path = fs::canonical(path);
            const auto within = path.lexically_relative(root);
            if (within.empty() || within == "." || *within.begin() == ".." ||
                within.has_root_path()) {
                throw std::runtime_error("Native save backing escapes physical SaveDir");
            }
        }
    }
    return path;
}

} // namespace NativeSaveBinding
