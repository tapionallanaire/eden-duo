// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <optional>

#include "common/common_types.h"

namespace Vulkan {

struct FrameGenPlan {
    size_t generations{};
    bool warm{};
};

class FrameGenPacer {
public:
    /// `panel_refresh_hz`: the display's measured refresh, 0 when unknown.
    [[nodiscard]] FrameGenPlan Plan(size_t capacity, f32 panel_refresh_hz);

    void Reset();

private:
    using Clock = std::chrono::steady_clock;

    void Stabilize(Clock::time_point now);
    void DeferEvaluations(Clock::duration amount);
    void UpdateLimit(Clock::time_point now, f32 base_rate, f32 target_rate, size_t ceiling);

    std::optional<Clock::time_point> last_frame;
    std::optional<Clock::time_point> stable_until;
    std::optional<Clock::time_point> probe_until;
    std::optional<Clock::time_point> next_probe;
    std::optional<Clock::time_point> deficit_since;
    std::optional<Clock::time_point> slowed_since;
    f32 smoothed_interval{};
    f32 output_credit{};
    f32 probe_base_rate{};
    f32 unloaded_base_rate{};
    f32 base_retention{};
    size_t issued_generations{};
    size_t probe_previous_limit{};
    size_t limit{};
    u32 probe_failures{};
};

} // namespace Vulkan
