// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <format>
#include <string>
#include <thread>

#include <ranges>
#if defined(__ANDROID__) || defined(__linux__)
#include <dlfcn.h>
#include <unwind.h>
#endif
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/gpu_split.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

constexpr u64 FENCE_RESERVE_SIZE = 8;

namespace {

#if defined(__ANDROID__) || defined(__linux__)
// EDEN_NX2_BACK_WAIT_CALLERS=1 (diagnostic): the GPU back thread's blocking waits on the host GPU
// by call chain (module offsets, for addr2line), every 5 s. Frame-pointer samplers lose the
// chain at MasterSemaphore::Wait; this unwinds from the unwind tables.
struct BackWaitCallers {
    static constexpr int Depth = 16;
    struct Chain {
        std::array<uintptr_t, Depth> pcs{};
        u64 count{};
        u64 ns{};
    };
    std::vector<Chain> chains;
    std::chrono::steady_clock::time_point next_report{};
    u64 total_ns{};

    static bool Enabled() {
        static const bool enabled = std::getenv("EDEN_NX2_BACK_WAIT_CALLERS") != nullptr;
        return enabled;
    }

    struct Capture {
        std::array<uintptr_t, Depth + 2> pcs{};
        int count{};
    };

    static _Unwind_Reason_Code Trace(_Unwind_Context* context, void* arg) {
        auto* capture = static_cast<Capture*>(arg);
        if (capture->count >= static_cast<int>(capture->pcs.size())) {
            return _URC_END_OF_STACK;
        }
        capture->pcs[capture->count++] = _Unwind_GetIP(context);
        return _URC_NO_REASON;
    }

    void Note(u64 ns) {
        Capture capture;
        _Unwind_Backtrace(Trace, &capture);
        Chain chain{};
        // Skip this function and Wait itself.
        for (int i = 2; i < capture.count && i - 2 < Depth; ++i) {
            chain.pcs[i - 2] = capture.pcs[i];
        }
        total_ns += ns;
        auto it = std::ranges::find_if(chains, [&](const Chain& c) { return c.pcs == chain.pcs; });
        if (it == chains.end()) {
            chain.count = 1;
            chain.ns = ns;
            chains.push_back(chain);
        } else {
            ++it->count;
            it->ns += ns;
        }
        const auto now = std::chrono::steady_clock::now();
        if (next_report.time_since_epoch().count() == 0) {
            next_report = now + std::chrono::seconds{5};
        }
        if (now < next_report) {
            return;
        }
        next_report = now + std::chrono::seconds{5};
        std::ranges::sort(chains, [](const Chain& a, const Chain& b) { return a.ns > b.ns; });
        std::string text;
        for (size_t i = 0; i < chains.size() && i < 6; ++i) {
            text += std::format("\n  {} waits {} ms:", chains[i].count, chains[i].ns / 1000000);
            for (const uintptr_t pc : chains[i].pcs) {
                if (pc == 0) {
                    break;
                }
                Dl_info info{};
                if (dladdr(reinterpret_cast<void*>(pc), &info) != 0 && info.dli_fbase != nullptr) {
                    const char* slash = info.dli_fname ? std::strrchr(info.dli_fname, '/') : nullptr;
                    text += std::format(" {}+{:#x}", slash ? slash + 1 : "?",
                                        pc - reinterpret_cast<uintptr_t>(info.dli_fbase));
                } else {
                    text += std::format(" {:#x}", pc);
                }
            }
        }
        LOG_INFO(Render_Vulkan, "GPU back thread waits on the host GPU (last 5 s, {} ms):{}",
                 total_ns / 1000000, text);
        chains.clear();
        total_ns = 0;
    }
};

thread_local BackWaitCallers t_back_wait_callers;

class BackWaitScope {
public:
    BackWaitScope()
        : active{BackWaitCallers::Enabled() && VideoCommon::GpuSplit::OnBackThread()},
          start{active ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}} {}
    ~BackWaitScope() {
        if (active) {
            t_back_wait_callers.Note(static_cast<u64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - start)
                    .count()));
        }
    }

private:
    bool active;
    std::chrono::steady_clock::time_point start;
};
#else
struct BackWaitScope {};
#endif

} // Anonymous namespace

MasterSemaphore::MasterSemaphore(const Device& device_) : device(device_) {
    if (!device.HasTimelineSemaphore()) {
        static constexpr VkFenceCreateInfo fence_ci{
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = nullptr, .flags = 0};
        free_queue.resize(FENCE_RESERVE_SIZE);
        std::ranges::generate(free_queue,
                              [&] { return device.GetLogical().CreateFence(fence_ci); });
        wait_thread = std::jthread([this](std::stop_token token) { WaitThread(token); });
        return;
    }

    static constexpr VkSemaphoreTypeCreateInfo semaphore_type_ci{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .pNext = nullptr,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = 0,
    };
    static constexpr VkSemaphoreCreateInfo semaphore_ci{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &semaphore_type_ci,
        .flags = 0,
    };
    semaphore = device.GetLogical().CreateSemaphore(semaphore_ci);

    if (!Settings::values.renderer_debug) {
        return;
    }
    // Validation layers have a bug where they fail to track resource usage when using timeline
    // semaphores and synchronizing with GetSemaphoreCounterValue. To workaround this issue, have
    // a separate thread waiting for each timeline semaphore value.
    debug_thread = std::jthread([this](std::stop_token stop_token) {
        u64 counter = 0;
        while (!stop_token.stop_requested()) {
            if (semaphore.Wait(counter, 10'000'000)) {
                ++counter;
            }
        }
    });
}

MasterSemaphore::~MasterSemaphore() = default;

void MasterSemaphore::Refresh() {
    if (!semaphore) {
        // If we don't support timeline semaphores, there's nothing to refresh
        return;
    }

    u64 this_tick{};
    u64 counter{};
    do {
        this_tick = gpu_tick.load(std::memory_order_acquire);
        counter = semaphore.GetCounter();
        if (counter < this_tick) {
            return;
        }
    } while (!gpu_tick.compare_exchange_weak(this_tick, counter, std::memory_order_release,
                                             std::memory_order_relaxed));
}

void MasterSemaphore::Wait(u64 tick) {
    if (!semaphore) {
        // Fast check: already reached the requested tick?
        if (gpu_tick.load(std::memory_order_acquire) >= tick) {
            return;
        }

        [[maybe_unused]] BackWaitScope back_wait_scope;
        u64 last_tick = gpu_tick.load(std::memory_order_relaxed);
        while (gpu_tick.load(std::memory_order_acquire) < tick) {
            gpu_tick.wait(last_tick, std::memory_order_acquire);
            last_tick = gpu_tick.load(std::memory_order_relaxed);
        }
        return;
    }

    // No need to wait if the GPU is ahead of the tick
    if (IsFree(tick)) {
        return;
    }

    // Update the GPU tick and try again
    Refresh();

    if (IsFree(tick)) {
        return;
    }

    // If none of the above is hit, fallback to a regular wait
    [[maybe_unused]] BackWaitScope back_wait_scope;
    try {
        while (!semaphore.Wait(tick)) {
        }
    } catch (const vk::Exception& exception) {
        // The throw ends the process; say why first (a device loss reached nothing else here).
        LOG_CRITICAL(Render_Vulkan, "Waiting for GPU tick {} failed: {}", tick, exception.what());
        throw;
    }

    Refresh();
}

VkResult MasterSemaphore::SubmitQueue(vk::CommandBuffer& cmdbuf, vk::CommandBuffer& upload_cmdbuf,
                                      VkSemaphore signal_semaphore, VkSemaphore wait_semaphore,
                                      u64 host_tick) {
    if (semaphore) {
        return SubmitQueueTimeline(cmdbuf, upload_cmdbuf, signal_semaphore, wait_semaphore,
                                   host_tick);
    } else {
        return SubmitQueueFence(cmdbuf, upload_cmdbuf, signal_semaphore, wait_semaphore, host_tick);
    }
}

static constexpr VkPipelineStageFlags wait_stage_mask = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

VkResult MasterSemaphore::SubmitQueueTimeline(vk::CommandBuffer& cmdbuf,
                                              vk::CommandBuffer& upload_cmdbuf,
                                              VkSemaphore signal_semaphore,
                                              VkSemaphore wait_semaphore, u64 host_tick) {
    const VkSemaphore timeline_semaphore = *semaphore;

    if (device.HasSynchronization2()) {
        const std::array<VkCommandBufferSubmitInfo, 2> cmdbuffer_infos{{
            {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                .pNext = nullptr,
                .commandBuffer = *upload_cmdbuf,
                .deviceMask = 0,
            },
            {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                .pNext = nullptr,
                .commandBuffer = *cmdbuf,
                .deviceMask = 0,
            },
        }};

        std::array<VkSemaphoreSubmitInfo, 2> signal_infos{{
            {
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                .pNext = nullptr,
                .semaphore = timeline_semaphore,
                .value = host_tick,
                .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                .deviceIndex = 0,
            },
            {},
        }};
        u32 num_signal_semaphores = 1;
        if (signal_semaphore) {
            signal_infos[1] = VkSemaphoreSubmitInfo{
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                .pNext = nullptr,
                .semaphore = signal_semaphore,
                .value = 0,
                .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                .deviceIndex = 0,
            };
            num_signal_semaphores = 2;
        }

        const u32 num_wait_semaphores = wait_semaphore ? 1 : 0;
        const VkSemaphoreSubmitInfo wait_info{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext = nullptr,
            .semaphore = wait_semaphore,
            .value = 0,
            .stageMask = static_cast<VkPipelineStageFlags2>(wait_stage_mask),
            .deviceIndex = 0,
        };

        const VkSubmitInfo2 submit_info2{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .pNext = nullptr,
            .flags = 0,
            .waitSemaphoreInfoCount = num_wait_semaphores,
            .pWaitSemaphoreInfos = num_wait_semaphores ? &wait_info : nullptr,
            .commandBufferInfoCount = static_cast<u32>(cmdbuffer_infos.size()),
            .pCommandBufferInfos = cmdbuffer_infos.data(),
            .signalSemaphoreInfoCount = num_signal_semaphores,
            .pSignalSemaphoreInfos = signal_infos.data(),
        };
        return device.GetGraphicsQueue().Submit2(submit_info2);
    }

    const u32 num_signal_semaphores = signal_semaphore ? 2 : 1;
    const std::array signal_values{host_tick, u64(0)};
    const std::array signal_semaphores{timeline_semaphore, signal_semaphore};

    const std::array cmdbuffers{*upload_cmdbuf, *cmdbuf};

    const u32 num_wait_semaphores = wait_semaphore ? 1 : 0;
    // Pointers must be null when the count is zero (best-practices)
    const VkSemaphore* p_wait_sems =
        (num_wait_semaphores > 0) ? &wait_semaphore : nullptr;
    const VkPipelineStageFlags* p_wait_masks =
        (num_wait_semaphores > 0) ? &wait_stage_mask : nullptr;
    const VkSemaphore* p_signal_sems =
        (num_signal_semaphores > 0) ? signal_semaphores.data() : nullptr;
    const u64 wait_zero = 0; // dummy for binary wait
    const VkTimelineSemaphoreSubmitInfo timeline_si{
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreValueCount = num_wait_semaphores,
        .pWaitSemaphoreValues    = num_wait_semaphores ? &wait_zero : nullptr,
        .signalSemaphoreValueCount = num_signal_semaphores,
        .pSignalSemaphoreValues = signal_values.data(),
    };
    const VkSubmitInfo submit_info{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = &timeline_si,
        .waitSemaphoreCount = num_wait_semaphores,
        .pWaitSemaphores = p_wait_sems,
        .pWaitDstStageMask = p_wait_masks,
        .commandBufferCount = static_cast<u32>(cmdbuffers.size()),
        .pCommandBuffers = cmdbuffers.data(),
        .signalSemaphoreCount = num_signal_semaphores,
        .pSignalSemaphores = p_signal_sems,
    };

    return device.GetGraphicsQueue().Submit(submit_info);
}

VkResult MasterSemaphore::SubmitQueueFence(vk::CommandBuffer& cmdbuf,
                                           vk::CommandBuffer& upload_cmdbuf,
                                           VkSemaphore signal_semaphore, VkSemaphore wait_semaphore,
                                           u64 host_tick) {
    if (device.HasSynchronization2()) {
        const std::array<VkCommandBufferSubmitInfo, 2> cmdbuffer_infos{{
            {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                .pNext = nullptr,
                .commandBuffer = *upload_cmdbuf,
                .deviceMask = 0,
            },
            {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                .pNext = nullptr,
                .commandBuffer = *cmdbuf,
                .deviceMask = 0,
            },
        }};

        const u32 num_signal_semaphores = signal_semaphore ? 1 : 0;
        const VkSemaphoreSubmitInfo signal_info{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext = nullptr,
            .semaphore = signal_semaphore,
            .value = 0,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .deviceIndex = 0,
        };

        const u32 num_wait_semaphores = wait_semaphore ? 1 : 0;
        const VkSemaphoreSubmitInfo wait_info{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext = nullptr,
            .semaphore = wait_semaphore,
            .value = 0,
            .stageMask = static_cast<VkPipelineStageFlags2>(wait_stage_mask),
            .deviceIndex = 0,
        };

        const VkSubmitInfo2 submit_info2{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .pNext = nullptr,
            .flags = 0,
            .waitSemaphoreInfoCount = num_wait_semaphores,
            .pWaitSemaphoreInfos = num_wait_semaphores ? &wait_info : nullptr,
            .commandBufferInfoCount = static_cast<u32>(cmdbuffer_infos.size()),
            .pCommandBufferInfos = cmdbuffer_infos.data(),
            .signalSemaphoreInfoCount = num_signal_semaphores,
            .pSignalSemaphoreInfos = num_signal_semaphores ? &signal_info : nullptr,
        };

        auto fence = GetFreeFence();
        auto result = device.GetGraphicsQueue().Submit2(submit_info2, *fence);

        if (result == VK_SUCCESS) {
            std::scoped_lock lock{wait_mutex};
            wait_queue.emplace(host_tick, std::move(fence));
            wait_cv.notify_one();
        }

        return result;
    }

    const u32 num_signal_semaphores = signal_semaphore ? 1 : 0;
    const u32 num_wait_semaphores = wait_semaphore ? 1 : 0;

    const VkSemaphore* p_wait_sems =
            (num_wait_semaphores > 0) ? &wait_semaphore : nullptr;
    const VkPipelineStageFlags* p_wait_masks =
        (num_wait_semaphores > 0) ? &wait_stage_mask : nullptr;
    const VkSemaphore* p_signal_sems =
        (num_signal_semaphores > 0) ? &signal_semaphore : nullptr;
    const std::array cmdbuffers{*upload_cmdbuf, *cmdbuf};

    const VkSubmitInfo submit_info{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreCount = num_wait_semaphores,
        .pWaitSemaphores = p_wait_sems,
        .pWaitDstStageMask = p_wait_masks,
        .commandBufferCount = static_cast<u32>(cmdbuffers.size()),
        .pCommandBuffers = cmdbuffers.data(),
        .signalSemaphoreCount = num_signal_semaphores,
        .pSignalSemaphores = p_signal_sems,
    };

    auto fence = GetFreeFence();
    auto result = device.GetGraphicsQueue().Submit(submit_info, *fence);

    if (result == VK_SUCCESS) {
        std::scoped_lock lock{wait_mutex};
        wait_queue.emplace(host_tick, std::move(fence));
        wait_cv.notify_one();
    }

    return result;
}

void MasterSemaphore::WaitThread(std::stop_token token) {
    while (!token.stop_requested()) {
        u64 host_tick;
        vk::Fence fence;
        {
            std::unique_lock lock{wait_mutex};
            wait_cv.wait(lock, token, [this] { return !wait_queue.empty(); });
            if (token.stop_requested()) {
                return;
            }
            std::tie(host_tick, fence) = std::move(wait_queue.front());
            wait_queue.pop();
        }

        fence.Wait();
        fence.Reset();

        {
            std::scoped_lock lock{free_mutex};
            free_queue.push_front(std::move(fence));
            gpu_tick.store(host_tick, std::memory_order_release);
        }
        gpu_tick.notify_one();
    }
}

vk::Fence MasterSemaphore::GetFreeFence() {
    std::scoped_lock lock{free_mutex};
    if (free_queue.empty()) {
        static constexpr VkFenceCreateInfo fence_ci{
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = nullptr, .flags = 0};
        return device.GetLogical().CreateFence(fence_ci);
    }

    auto fence = std::move(free_queue.back());
    free_queue.pop_back();
    return fence;
}

} // namespace Vulkan
