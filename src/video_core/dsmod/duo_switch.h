// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>

namespace VideoCore::DSMod {

/// Eden Duo: whether this device has a second panel to present on. The frontend sets it once at
/// start-up from the display list; while it is false the aux presenter hands the guest's layers
/// straight back, so a single-screen device runs none of the Duo machinery.
inline std::atomic_bool g_duo_enabled{true};

inline void SetDuoEnabled(bool enabled) {
    g_duo_enabled.store(enabled, std::memory_order_relaxed);
}

inline bool DuoEnabled() {
    return g_duo_enabled.load(std::memory_order_relaxed);
}

} // namespace VideoCore::DSMod
