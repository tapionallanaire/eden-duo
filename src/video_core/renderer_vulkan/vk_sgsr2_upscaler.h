// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <memory>

#include "common/common_types.h"
#include "video_core/renderer_vulkan/vk_fsr3_upscaler.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class DescriptorPool;
class Device;
class MemoryAllocator;
class Scheduler;
class StagingBufferPool;

/// Snapdragon Game Super Resolution 2 (Qualcomm's temporal upscaler for Adreno, the 2-pass compute
/// variant; shaders in host_shaders/sgsr2_*.comp) in place of a game's DLSS evaluation. It takes
/// the same inputs as the FSR 3.1 upscaler and costs two passes instead of nine.
class Sgsr2Upscaler {
public:
    using Inputs = Fsr3Upscaler::Inputs;

    explicit Sgsr2Upscaler(const Device& device, Scheduler& scheduler,
                           MemoryAllocator& memory_allocator,
                           StagingBufferPool& staging_buffer_pool, DescriptorPool& descriptor_pool,
                           ComputePassDescriptorQueue& descriptor_queue);
    ~Sgsr2Upscaler();

    /// Whether the device has the storage formats the passes write.
    [[nodiscard]] static bool IsSupported(const Device& device);

    /// Records one evaluation into the output: RGBA16F at the upscale size, GENERAL layout, ready
    /// to be sampled by a fragment shader. Inputs::sharpen and sharpness are not used.
    void Upscale(const Inputs& inputs);

    [[nodiscard]] VkImage OutputImage() const noexcept {
        return *output.image;
    }
    [[nodiscard]] VkImageView OutputView() const noexcept {
        return *output.view;
    }
    [[nodiscard]] VkSampler PointSampler() const noexcept {
        return *point_sampler;
    }

private:
    class Pass;
    struct Texture {
        vk::Image image;
        vk::ImageView view;
    };

    void CreateResources(VkExtent2D render_size, VkExtent2D upscale_size);
    Texture CreateTexture(VkFormat format, u32 width, u32 height);

    const Device& device;
    Scheduler& scheduler;
    MemoryAllocator& memory_allocator;
    StagingBufferPool& staging_buffer_pool;
    ComputePassDescriptorQueue& descriptor_queue;

    vk::Sampler point_sampler;
    vk::Sampler linear_sampler;
    std::unique_ptr<Pass> convert;
    std::unique_ptr<Pass> upscale;

    Texture motion_depth_clip; ///< Render size: motion, depth clip, colour max.
    Texture ycocg;             ///< Render size: packed YCoCg colour.
    std::array<Texture, 2> history; ///< Display size, ping-pong.
    Texture output;

    VkExtent2D render_extent{};
    VkExtent2D upscale_extent{};
    u32 frame_parity{};
    bool first_execution{true};
};

} // namespace Vulkan
