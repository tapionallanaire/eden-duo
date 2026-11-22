// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <span>

#include "common/common_types.h"

namespace VideoCommon {

/// A single contiguous Vulkan readback. An intervening GPU or host write invalidates only its page.
struct Nx2ListPageBatch {
    static constexpr std::size_t PageSize = 4096;
    static constexpr std::size_t MaxPages = 8;
    DAddr first{};
    std::array<u64, MaxPages> writes{};
    std::size_t count{};

    [[nodiscard]] std::span<const u8> Page(std::span<const u8> bytes, DAddr page,
                                           u64 generation) const {
        if (page < first || (page - first) % PageSize != 0) {
            return {};
        }
        const u64 index = (page - first) / PageSize;
        if (index >= count || writes[index] != generation) {
            return {};
        }
        return bytes.subspan(index * PageSize, PageSize);
    }
};

} // namespace VideoCommon
