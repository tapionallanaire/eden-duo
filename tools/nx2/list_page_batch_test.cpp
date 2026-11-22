// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include "video_core/buffer_cache/nx2_list_page_batch.h"

int main() {
    // A's mandatory wait captured B and C. The guest/GPU then rewrote B while C stayed current.
    const VideoCommon::Nx2ListPageBatch batch{.first = 0x1000, .writes = {1, 4, 9}, .count = 3};
    std::array<u8, 3 * 4096> staging{};
    staging[4096] = 0xba;
    staging[8192] = 0xcc;
    const auto b = batch.Page(staging, 0x2000, 4);
    const auto c = batch.Page(staging, 0x3000, 9);
    return b.size() == 4096 && b.front() == 0xba && batch.Page(staging, 0x2000, 5).empty() &&
                   c.size() == 4096 && c.front() == 0xcc && batch.Page(staging, 0x4000, 0).empty()
               ? 0
               : 1;
}
