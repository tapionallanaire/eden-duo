// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdlib>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/logging.h"
#include "video_core/renderer_vulkan/vk_descriptor_buffer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {

DescriptorBufferRing::DescriptorBufferRing(const Device& device_,
                                           MemoryAllocator& memory_allocator)
    : device{device_} {
    if (!device.IsExtDescriptorBufferSupported() || !device.IsBufferDeviceAddressSupported()) {
        return;
    }
    const VkPhysicalDeviceDescriptorBufferPropertiesEXT& props{device.DescriptorBufferProperties()};
    alignment = std::max<VkDeviceSize>(props.descriptorBufferOffsetAlignment, 1);

    const VkDeviceSize max_bound{(std::min)({props.maxSamplerDescriptorBufferRange,
                                             props.maxResourceDescriptorBufferRange,
                                             props.samplerDescriptorBufferAddressSpaceSize,
                                             props.resourceDescriptorBufferAddressSpaceSize,
                                             props.descriptorBufferAddressSpaceSize})};
    VkDeviceSize frame_size{device.IsTiler() ? TILER_FRAME_SIZE : DESKTOP_FRAME_SIZE};
    // EDEN_NX2_DESC_FRAME_MB=<n>: descriptor bytes per frame before the ring waits for the GPU.
    if (const char* value = std::getenv("EDEN_NX2_DESC_FRAME_MB"); value != nullptr) {
        frame_size = (std::max)(VkDeviceSize{1}, static_cast<VkDeviceSize>(std::atoll(value))) *
                     1024 * 1024;
    }
    const VkDeviceSize chunk_size{
        Common::AlignDown((std::min)(frame_size, max_bound), alignment)};
    if (chunk_size <= alignment) {
        LOG_DEBUG(Render_Vulkan, "Descriptor buffer binding limit of {} is unusable, disabling",
                  max_bound);
        return;
    }
    chunk_capacity = chunk_size - alignment;
    chunks_per_frame = static_cast<size_t>(frame_size / chunk_size);

    const VkBufferCreateInfo buffer_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = chunk_size,
        .usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
                 VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    const size_t total_chunks{chunks_per_frame * FRAMES_IN_FLIGHT};
    chunks.reserve(total_chunks);
    chunk_addresses.reserve(total_chunks);
    chunk_hosts.reserve(total_chunks);
    for (size_t index = 0; index < total_chunks; ++index) {
        vk::Buffer buffer{memory_allocator.CreateBuffer(buffer_ci, MemoryUsage::Upload)};
        if (!buffer.IsHostVisible()) {
            LOG_DEBUG(Render_Vulkan, "Descriptor buffer is not host visible, disabling");
            chunks.clear();
            return;
        }
        if (!buffer.IsHostCoherent()) {
            LOG_DEBUG(Render_Vulkan, "Descriptor buffer is not host coherent, disabling");
            chunks.clear();
            return;
        }
        if (device.HasDebuggingToolAttached()) {
            buffer.SetObjectNameEXT("Descriptor buffer");
        }
        const VkDeviceAddress raw_address{device.GetLogical().GetBufferDeviceAddress(*buffer)};
        const VkDeviceAddress address{Common::AlignUp(raw_address, alignment)};
        chunk_addresses.push_back(address);
        chunk_hosts.push_back(buffer.Mapped().data() + (address - raw_address));
        chunks.push_back(std::move(buffer));
    }
}

DescriptorBufferRing::~DescriptorBufferRing() = default;

void DescriptorBufferRing::TickFrame() {
    peak_frame_bytes = (std::max)(peak_frame_bytes, frame_bytes);
    frame_bytes = 0;
    if (++frames >= next_report_frame) {
        next_report_frame = frames + 600;
        LOG_INFO(Render_Vulkan,
                 "Descriptor buffer: {} GPU stalls for a full frame in the last 600 frames, peak "
                 "{} KiB of {} KiB a frame",
                 exhausted_stalls, peak_frame_bytes / 1024,
                 chunks_per_frame * (chunk_capacity + alignment) / 1024);
        exhausted_stalls = 0;
        peak_frame_bytes = 0;
    }
    if (++frame_index >= FRAMES_IN_FLIGHT) {
        frame_index = 0;
    }
    chunk_cursor = 0;
    cursor = 0;
    ++generation;
    frame_reused = true;
}

void DescriptorBufferRing::TouchFrame(Scheduler& scheduler) {
    frame_ticks[frame_index] = scheduler.CurrentTick();
}

DescriptorBufferRing::Allocation DescriptorBufferRing::Allocate(Scheduler& scheduler,
                                                                VkDeviceSize size) {
    ASSERT(!chunks.empty());
    if (!CanAllocate(size)) {
        LOG_DEBUG(Render_Vulkan, "Descriptor set of {} bytes exceeds chunk capacity {}", size,
                  chunk_capacity);
        return Allocation{};
    }
    const VkDeviceSize needed{Common::AlignUp(size, alignment)};
    if (frame_reused) {
        frame_reused = false;
        scheduler.Wait(frame_ticks[frame_index]);
    }
    if (cursor + needed > chunk_capacity) {
        if (chunk_cursor + 1 < chunks_per_frame) {
            ++chunk_cursor;
        } else {
            LOG_DEBUG(Render_Vulkan, "Descriptor buffer frame exhausted, stalling on the GPU");
            ++exhausted_stalls;
            scheduler.Finish();
            chunk_cursor = 0;
            ++generation;
        }
        cursor = 0;
    }
    const size_t chunk{frame_index * chunks_per_frame + chunk_cursor};
    const VkDeviceSize offset{cursor};
    cursor += needed;
    frame_bytes += needed;
    frame_ticks[frame_index] = scheduler.CurrentTick();
    return Allocation{
        .host = chunk_hosts[chunk] + offset,
        .offset = offset,
        .chunk = static_cast<u32>(chunk),
        .generation = generation,
    };
}

} // namespace Vulkan
