// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <functional>

#include "common/common_types.h"

namespace Core {

/// Frames the guest has presented (Switch 2 vi2 present calls); defined in core.cpp.
extern std::atomic<u64> g_nx2_present_count;

/// Write barrier for a present that returns before the GPU thread has run its frame (nx2_gsv.h,
/// EDEN_NX2_BARRIER). The game prepares its next frame meanwhile, but its writes into buffers the
/// GPU also writes (draw arguments its compute dispatches fill in) must not reach the caches
/// before the previous frame's dispatches ran. Those writes are collected in two sets
/// (GPUDirtyMemoryManager, one per frame parity): the guest collects into g_nx2_collect_set,
/// which the present flips, and the GPU thread gathers only the set of the frame it is running.
/// 0: off. 1: such a write waits for the GPU thread to finish the frame before. 2: held back as
/// described (faster, but a rare 0.3 s brightness step was recorded with it). 3: 2 with the
/// page copies below (no late upload left, but slower than no barrier at all so far).
extern std::atomic<u32> g_nx2_barrier;
extern std::atomic<u32> g_nx2_collect_set;
struct Nx2BarrierStats {
    std::atomic<u64> waits{}, wait_ns{}, timeouts{};
};
extern Nx2BarrierStats g_nx2_barrier_stats;
/// GPU thread: every command list submitted before present number `present` has run.
void Nx2NoteGpuThreadDone(u64 present);
/// The last present the GPU thread has finished (Nx2NoteGpuThreadDone).
[[nodiscard]] u64 Nx2GpuThreadDone();
/// Guest threads: waits (bounded) until the GPU thread has run the lists of present `present`.
void Nx2WaitGpuThreadDone(u64 present);
/// A new emulation session starts.
void Nx2BarrierReset();

/// Barrier mode 3: the pages of buffer data the guest wrote during a frame, copied when it
/// presents that frame. While the guest prepares the next frame the GPU thread still uploads
/// some of them, and must get what the presented frame had, not what the next one writes.
namespace Nx2PageStore {
constexpr u64 PageSize = 0x1000;
/// Guest threads: a buffer page was handed to the CPU (its first write of the frame).
void Record(u64 device_address, u64 size);
/// Guest thread, at present number "present": copies the recorded pages and the ones known to
/// be rewritten every frame. "read" returns the page's memory or null.
void Snapshot(u64 present, const std::function<const u8*(u64)>& read);
/// GPU thread: the copy of a page taken at the present of the frame it is running, or null
/// (no copy, or the guest has not presented that frame: guest memory is current).
[[nodiscard]] const u8* Find(u64 device_page);
/// GPU thread: the guest rewrote the page since its copy was taken: copy it at every present.
void NoteRewritten(u64 device_page);
struct Stats {
    std::atomic<u64> copied{}, served{}, rewritten{};
};
extern Stats g_stats;
} // namespace Nx2PageStore

} // namespace Core
