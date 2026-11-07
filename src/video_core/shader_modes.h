// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

namespace VideoCore {

/// "Record shaders": while set, every graphics pipeline the game builds for the first time in a
/// session is also written to the title's recording.bin (a plan file) with a line per pipeline in
/// recording.log. That is what a later compile can warm, for effects only play reveals.
inline std::atomic<bool> g_shader_record{false};

/// "Compile shaders": while set, the next run loads the title's plan and builds it, then stops.
/// It also makes the load happen when the disk shader cache setting is off, so an explicit compile
/// is never silently turned into ordinary play.
inline std::atomic<bool> g_shader_compile{false};

/// "Max compile": while set, a compile run also builds every vertex/fragment shader combination
/// the title's plan knows, not only the pairs it has drawn, so more of the pipelines a game will
/// ask for later already exist in the driver's cache.
inline std::atomic<bool> g_shader_max_compile{false};

inline bool ShaderRecording() {
    return g_shader_record.load(std::memory_order_relaxed);
}

inline bool CompileRun() {
    return g_shader_compile.load(std::memory_order_relaxed);
}

inline bool MaxCompile() {
    return g_shader_max_compile.load(std::memory_order_relaxed);
}

inline void SetCompileRun(bool enabled) {
    g_shader_compile.store(enabled, std::memory_order_relaxed);
}

inline void SetShaderRecording(bool enabled) {
    g_shader_record.store(enabled, std::memory_order_relaxed);
}

inline void SetMaxCompile(bool enabled) {
    g_shader_max_compile.store(enabled, std::memory_order_relaxed);
}

} // namespace VideoCore
