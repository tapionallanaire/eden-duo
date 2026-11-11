// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>

#include "common/common_types.h"
#include "video_core/textures/texture.h"

namespace Tegra::Texture {

constexpr u32 GOB_SIZE_X = 64;
constexpr u32 GOB_SIZE_Y = 8;
constexpr u32 GOB_SIZE_Z = 1;
constexpr u32 GOB_SIZE = GOB_SIZE_X * GOB_SIZE_Y * GOB_SIZE_Z;

constexpr u32 GOB_SIZE_X_SHIFT = 6;
constexpr u32 GOB_SIZE_Y_SHIFT = 3;
constexpr u32 GOB_SIZE_Z_SHIFT = 0;
constexpr u32 GOB_SIZE_SHIFT = GOB_SIZE_X_SHIFT + GOB_SIZE_Y_SHIFT + GOB_SIZE_Z_SHIFT;

constexpr u32 SWIZZLE_X_BITS = 0b100101111;
constexpr u32 SWIZZLE_Y_BITS = 0b011010000;

/// Unswizzles a block linear texture into linear memory.
void UnswizzleTexture(std::span<u8> output, std::span<const u8> input, u32 bytes_per_pixel,
                      u32 width, u32 height, u32 depth, u32 block_height, u32 block_depth,
                      u32 stride_alignment = 1);

/// Swizzles linear memory into a block linear texture.
void SwizzleTexture(std::span<u8> output, std::span<const u8> input, u32 bytes_per_pixel, u32 width,
                    u32 height, u32 depth, u32 block_height, u32 block_depth,
                    u32 stride_alignment = 1);

/// This function calculates the correct size of a texture depending if it's tiled or not.
std::size_t CalculateSize(bool tiled, u32 bytes_per_pixel, u32 width, u32 height, u32 depth,
                          u32 block_height, u32 block_depth);

/// Copies an untiled subrectangle into a tiled surface.
void SwizzleSubrect(std::span<u8> output, std::span<const u8> input, u32 bytes_per_pixel, u32 width,
                    u32 height, u32 depth, u32 origin_x, u32 origin_y, u32 extent_x, u32 extent_y,
                    u32 block_height, u32 block_depth, u32 pitch_linear);

/// Where a rect of a block-linear layer with block depth 0 is stored: `rows` runs of `size` bytes,
/// the first at `offset`, each `stride` bytes after the one before. Units as in SwizzleSubrect;
/// the rect must not be empty.
struct SubrectRuns {
    u32 offset;
    u32 size;
    u32 stride;
    u32 rows;
};
constexpr SubrectRuns CalculateSubrectRuns(u32 bytes_per_pixel, u32 width, u32 origin_x,
                                           u32 origin_y, u32 extent_x, u32 extent_y,
                                           u32 block_height) {
    const u32 x_shift = GOB_SIZE_SHIFT + block_height;
    const u32 y_shift = GOB_SIZE_Y_SHIFT + block_height;
    const u32 stride = ((width * bytes_per_pixel + GOB_SIZE_X - 1) >> GOB_SIZE_X_SHIFT) << x_shift;
    const u32 first_gob = (origin_x * bytes_per_pixel) >> GOB_SIZE_X_SHIFT;
    const u32 last_gob = ((origin_x + extent_x) * bytes_per_pixel - 1) >> GOB_SIZE_X_SHIFT;
    const u32 first_row = origin_y >> y_shift;
    const u32 last_row = (origin_y + extent_y - 1) >> y_shift;
    return {first_row * stride + (first_gob << x_shift), (last_gob - first_gob + 1) << x_shift,
            stride, last_row - first_row + 1};
}

/// Copies a tiled subrectangle into a linear surface.
void UnswizzleSubrect(std::span<u8> output, std::span<const u8> input, u32 bytes_per_pixel,
                      u32 width, u32 height, u32 depth, u32 origin_x, u32 origin_y, u32 extent_x,
                      u32 extent_y, u32 block_height, u32 block_depth, u32 pitch_linear);

/// Obtains the offset of the gob for positions 'dst_x' & 'dst_y'
u64 GetGOBOffset(u32 width, u32 height, u32 dst_x, u32 dst_y, u32 block_height,
                 u32 bytes_per_pixel);

} // namespace Tegra::Texture
