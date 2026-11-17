// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>

namespace Vulkan::DriverBuildReport {

enum class Kind { Graphics, Compute };

/// Records one vkCreate*Pipelines call. Every 10 s with builds, logs how many there were and how
/// many took over 10 ms: a driver that finds the pipeline in its cache builds it in about 1 ms, one
/// that compiles it takes tens of ms (NVIDIA: 0.6-1.5 ms against ~48 ms), so tester logs show
/// whether the driver's own cache works on their system. Thread safe.
void Record(Kind kind, std::chrono::steady_clock::duration time);

/// Builds recorded on this thread while it lives are the shader precache's: they are counted
/// apart, so the game's own builds show whether the driver's cache served them.
class PrecacheScope {
public:
    PrecacheScope();
    ~PrecacheScope();
    PrecacheScope(const PrecacheScope&) = delete;
    PrecacheScope& operator=(const PrecacheScope&) = delete;
};

/// True on a thread inside a PrecacheScope.
[[nodiscard]] bool OnPrecacheThread();

/// A shader stage (its runtime hash) the boot-time pair build compiled this session, and whether
/// a hash is one: a slow build in play names its stages with that, to tell a shader the game
/// files' pairs do not hold from one whose build differed from the pair's.
void NotePairStage(unsigned long long hash);
[[nodiscard]] bool IsPairStage(unsigned long long hash);
[[nodiscard]] unsigned long long PairStageCount();

/// Logs the builds since the last summary (the pipelines built at boot) and starts over.
void LogSummary(const char* when);

} // namespace Vulkan::DriverBuildReport
