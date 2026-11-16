// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdexcept>

#include "video_core/textures/decoders.h"

// Every byte of a rect must fall inside the runs CalculateSubrectRuns reports for it.
int main() {
    using namespace Tegra::Texture;
    const u32 width = 988, height = 986, extent = 34;
    for (const u32 bpp : {1u, 4u, 16u}) {
        for (const u32 block_height : {0u, 2u, 4u}) {
            for (u32 x0 = 0; x0 + extent <= width; x0 += 53) {
                for (u32 y0 = 0; y0 + extent <= height; y0 += 47) {
                    const auto runs =
                        CalculateSubrectRuns(bpp, width, x0, y0, extent, extent, block_height);
                    for (u32 y = y0; y < y0 + extent; ++y) {
                        for (u32 byte = x0 * bpp; byte < (x0 + extent) * bpp; ++byte) {
                            // Block-linear address of the 512-byte GOB holding this byte
                            const u32 gob_y = y >> GOB_SIZE_Y_SHIFT;
                            const u32 gob =
                                (gob_y >> block_height) * runs.stride +
                                ((byte >> GOB_SIZE_X_SHIFT) << (GOB_SIZE_SHIFT + block_height)) +
                                ((gob_y & ((1u << block_height) - 1)) << GOB_SIZE_SHIFT);
                            bool found = false;
                            for (u32 row = 0; row < runs.rows; ++row) {
                                const u32 begin = runs.offset + row * runs.stride;
                                found |= gob >= begin && gob + GOB_SIZE <= begin + runs.size;
                            }
                            if (!found) {
                                throw std::runtime_error("rect byte outside its runs");
                            }
                        }
                    }
                }
            }
        }
    }
}
