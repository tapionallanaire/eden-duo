// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <map>
#include <mutex>
#include <tuple>

#include "common/common_types.h"

// EDEN_NX2_KSTATS=1: kernel hand-off counters (diagnostics, off by default). SVC counts and time
// per SVC, and the wake latency of guest threads: from the moment a waiting thread is made
// runnable to the moment a core enters guest code with it again, by how it was woken. The guest
// sampler thread logs them every 10 s per presented frame.
namespace Kernel::Nx2KStats {

/// Read again at each game load (StartGuestSampler): Android sets the environment from env.txt
/// after the library loads.
inline bool g_enabled = std::getenv("EDEN_NX2_KSTATS") != nullptr;

inline u64 NowNs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

/// How a thread was woken: by a thread on the core it is queued on, by another core's thread
/// while its core idled, or while its core ran another thread, or by a host thread (timer, HLE).
/// A busy core's woken thread either has a higher priority than the running one (it should
/// preempt it at once) or not (it is queued behind it).
enum WakeKind : u32 { SameCore, OtherCoreIdle, OtherCoreBusy, HostThread, OtherCoreQueued, WakeKinds };

constexpr size_t Buckets = 12; ///< Microseconds: <1, <2, <4 ... <1024, more

struct Stats {
    std::array<std::atomic<u64>, 0x100> svc_count{};
    std::array<std::atomic<u64>, 0x100> svc_ns{};
    std::array<std::array<std::atomic<u64>, Buckets>, WakeKinds> wake_hist{};
    std::array<std::atomic<u64>, WakeKinds> wake_ns{};    ///< Ready to guest code
    std::array<std::atomic<u64>, WakeKinds> switch_ns{};  ///< Ready to switched in by a core
    std::atomic<u64> interrupts_idle{};  ///< Cross-core reschedule requests to an idle core
    std::atomic<u64> interrupts_busy{};  ///< ... to a core running a guest thread
    std::atomic<u64> idle_entries{};
    std::atomic<u64> idle_blocked{};     ///< Of those, ones that spun out and slept on the host
    std::atomic<u64> idle_blocked_ns{};  ///< Time slept on the host
    std::atomic<u64> switches{};         ///< Thread switches on cores
    std::array<std::atomic<u64>, 2> yields{};    ///< SleepThread(0, -1, -2): calls, ns in the call
    std::array<std::atomic<u64>, 3> sleeps{};    ///< SleepThread(>0): calls, ns asked, ns in the call
    std::array<std::atomic<u64>, Buckets> sleep_hist{}; ///< Of those, time in the call
    std::array<std::atomic<u64>, Buckets> sleep_asked_hist{}; ///< ... and time asked
    /// Wakes onto a busy core: the woken thread's priority above, equal to, below the running one
    std::array<std::atomic<u64>, 3> busy_priority{};
    /// PhysicalCore::Interrupt: calls, ns in it; idle cores that saw one while spinning: count,
    /// ns from the interrupt to leaving the spin.
    std::array<std::atomic<u64>, 4> interrupt{}; ///< EDEN_NX2_FAST_YIELD: yields that skipped the lock
    /// The scheduler lock: acquisitions, contended ones, ns waited, ns held (outermost holds,
    /// the unlock's scheduling update included, the reschedule after it not).
    std::array<std::atomic<u64>, 4> sched_lock{};
    /// SVC round trip on a core: from the JIT stopping for the SVC to it entering guest code again
    /// with the same thread, when no other thread ran in between: calls, ns.
    std::array<std::atomic<u64>, 0x100> svc_rt_count{};
    std::array<std::atomic<u64>, 0x100> svc_rt_ns{};
};

inline Stats g_stats;

/// The main and sdk modules (set by the NSO loader): SVC callers are logged relative to them.
inline std::atomic<u64> g_main_base{}, g_main_size{}, g_sdk_base{}, g_sdk_size{};

/// EDEN_NX2_SVC_CALLERS=1: for every 4th waiting, yielding or signalling SVC of each core, the
/// frame-pointer chain is walked to the first two return addresses inside main; calls and time
/// in the call are summed per (SVC, thread, sdk return address, main return addresses).
inline bool g_callers_enabled = std::getenv("EDEN_NX2_SVC_CALLERS") != nullptr;
using CallerKey = std::tuple<u32, u64, u64, u64, u64>;
inline std::mutex g_callers_mutex;
inline std::map<CallerKey, std::pair<u64, u64>> g_callers; ///< Calls, ns

/// Wake latency per thread id: wakes, ns, wakes over 32 us (under g_callers_mutex).
inline std::map<u64, std::array<u64, 3>> g_thread_wakes;

inline size_t Bucket(u64 ns) {
    size_t bucket = 0;
    for (u64 us = ns / 1000; us != 0 && bucket < Buckets - 1; us >>= 1) {
        ++bucket;
    }
    return bucket;
}

inline void Add(std::atomic<u64>& counter, u64 value = 1) {
    counter.fetch_add(value, std::memory_order_relaxed);
}

} // namespace Kernel::Nx2KStats
