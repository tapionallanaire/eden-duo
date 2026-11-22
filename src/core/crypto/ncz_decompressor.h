// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string_view>

#include "core/file_sys/vfs/vfs_types.h"

namespace Core::Crypto {

/**
 * Opens an NCZ (the compressed NCA of an NSZ/XCZ) as the NCA it holds. A block-compressed one
 * (NCZBLOCK, nsz -B) is decoded on demand, its CTR sections served decrypted. A solid one serves
 * its NCA header at once; the rest comes from <cache>/ncz/<output_name>, which a thread inside a
 * SolidNczUnpackScope fills on its first read (one full unpack, after a free-space check).
 *
 * @param ncz_file    The raw NCZ payload (e.g. "<id>.ncz" entry from an NSZ/XCZ PFS0).
 * @param output_name Name to give the returned VirtualFile (e.g. "<id>.nca").
 *
 * @return a VirtualFile for the NCA, or nullptr if the NCZ is malformed or cannot be unpacked.
 */
[[nodiscard]] FileSys::VirtualFile DecompressNCZ(const FileSys::VirtualFile& ncz_file,
                                                 std::string_view output_name);

/// While alive, this thread may unpack solid NCZs it reads (a launch, an install). Library scans
/// and metadata reads run without one and never write the unpacked NCA.
class SolidNczUnpackScope {
public:
    SolidNczUnpackScope();
    ~SolidNczUnpackScope();
    SolidNczUnpackScope(const SolidNczUnpackScope&) = delete;
    SolidNczUnpackScope& operator=(const SolidNczUnpackScope&) = delete;

private:
    bool previous;
};

/// True for a solid NCZ's NCA (from DecompressNCZ) that is not unpacked and may not be on this
/// thread: only its header can be read.
[[nodiscard]] bool IsNczPendingUnpack(const FileSys::VirtualFile& file);

} // namespace Core::Crypto