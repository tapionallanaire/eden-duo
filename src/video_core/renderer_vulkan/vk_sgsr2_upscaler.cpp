// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cmath>
#include <cstring>
#include <numbers>
#include <span>

#include "common/alignment.h"
#include "common/div_ceil.h"
#include "common/logging.h"
#include "video_core/host_shaders/sgsr2_convert_comp_spv.h"
#include "video_core/host_shaders/sgsr2_upscale_comp_spv.h"
#include "video_core/renderer_vulkan/vk_compute_pass.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_sgsr2_upscaler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

namespace Vulkan {
namespace {

// Params in sgsr2_convert.comp / sgsr2_upscale.comp (std140).
struct Params {
    std::array<u32, 2> render_size;
    std::array<u32, 2> display_size;
    std::array<f32, 2> render_size_rcp;
    std::array<f32, 2> display_size_rcp;
    std::array<f32, 2> jitter;
    std::array<f32, 2> motion_scale;
    f32 pre_exposure;
    f32 camera_fov_angle_hor;
    f32 camera_near;
    f32 min_lerp_contribution;
    u32 same_camera_frames;
    u32 reset;
};
static_assert(sizeof(Params) == 72);

// DLSS takes no camera. The field of view only scales SGSR's depth-edge weight: a typical one.
constexpr f32 CAMERA_FOV = 60.0f * std::numbers::pi_v<f32> / 180.0f;

/// Binding order of both passes: the parameters, three textures, two storage images; the point
/// and linear samplers at 1000 and 1001.
constexpr std::array<VkDescriptorType, 6> BINDINGS{
    VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
    VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
};

constexpr VkMemoryBarrier MemoryBarrier(VkAccessFlags src, VkAccessFlags dst) {
    return VkMemoryBarrier{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = src,
        .dstAccessMask = dst,
    };
}

struct Layout {
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    std::vector<VkDescriptorUpdateTemplateEntry> templates;
    DescriptorBankInfo bank;
};

Layout MakeLayout(const std::array<VkSampler, 2>& samplers) {
    Layout result;
    for (u32 binding = 0; binding < BINDINGS.size(); ++binding) {
        const VkDescriptorType type = BINDINGS[binding];
        switch (type) {
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            ++result.bank.uniform_buffers;
            break;
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            ++result.bank.sampled_images;
            break;
        default:
            ++result.bank.images;
            break;
        }
        result.bindings.push_back({
            .binding = binding,
            .descriptorType = type,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .pImmutableSamplers = nullptr,
        });
        result.templates.push_back({
            .dstBinding = binding,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = type,
            .offset = binding * sizeof(DescriptorUpdateEntry),
            .stride = sizeof(DescriptorUpdateEntry),
        });
    }
    for (u32 index = 0; index < samplers.size(); ++index) {
        result.bindings.push_back({
            .binding = 1000 + index,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .pImmutableSamplers = &samplers[index],
        });
        ++result.bank.samplers;
    }
    result.bank.score = static_cast<s32>(result.bindings.size());
    return result;
}

} // Anonymous namespace

class Sgsr2Upscaler::Pass final : public ComputePass {
public:
    explicit Pass(const Device& device_, Scheduler& scheduler, DescriptorPool& descriptor_pool,
                  const Layout& pass_layout, std::span<const u32> code)
        : ComputePass(device_, scheduler, descriptor_pool, pass_layout.bindings,
                      pass_layout.templates, pass_layout.bank, {}, code) {}

    /// Binds the parameters, three textures and two storage images, in binding order.
    void Dispatch(Scheduler& scheduler_, ComputePassDescriptorQueue& queue, VkBuffer params,
                  VkDeviceSize offset, VkDeviceSize size, std::array<VkImageView, 5> views,
                  u32 groups_x, u32 groups_y) {
        queue.Acquire(scheduler_, 6);
        queue.AddBuffer(params, offset, size);
        for (const VkImageView view : views) {
            queue.AddImage(view);
        }
        const void* const descriptor_data{queue.UpdateData()};
        scheduler_.Record([this, descriptor_data, groups_x, groups_y](vk::CommandBuffer cmdbuf) {
            const VkDescriptorSet set = descriptor_allocator.Commit();
            device.GetLogical().UpdateDescriptorSet(set, *descriptor_template, descriptor_data);
            cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline);
            cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *layout, 0, set, {});
            cmdbuf.Dispatch(groups_x, groups_y, 1);
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                   MemoryBarrier(VK_ACCESS_SHADER_WRITE_BIT,
                                                 VK_ACCESS_SHADER_READ_BIT |
                                                     VK_ACCESS_SHADER_WRITE_BIT));
        });
        scheduler_.InvalidateDescriptorBuffer();
    }
};

Sgsr2Upscaler::Sgsr2Upscaler(const Device& device_, Scheduler& scheduler_,
                             MemoryAllocator& memory_allocator_,
                             StagingBufferPool& staging_buffer_pool_,
                             DescriptorPool& descriptor_pool,
                             ComputePassDescriptorQueue& descriptor_queue_)
    : device{device_}, scheduler{scheduler_}, memory_allocator{memory_allocator_},
      staging_buffer_pool{staging_buffer_pool_}, descriptor_queue{descriptor_queue_} {
    const auto sampler = [&](VkFilter filter) {
        return device.GetLogical().CreateSampler(VkSamplerCreateInfo{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .magFilter = filter,
            .minFilter = filter,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .mipLodBias = 0.0f,
            .anisotropyEnable = VK_FALSE,
            .maxAnisotropy = 1.0f,
            .compareEnable = VK_FALSE,
            .compareOp = VK_COMPARE_OP_NEVER,
            .minLod = 0.0f,
            .maxLod = 0.0f,
            .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
            .unnormalizedCoordinates = VK_FALSE,
        });
    };
    point_sampler = sampler(VK_FILTER_NEAREST);
    linear_sampler = sampler(VK_FILTER_LINEAR);
    const Layout layout = MakeLayout({*point_sampler, *linear_sampler});
    convert = std::make_unique<Pass>(device, scheduler, descriptor_pool, layout,
                                     SGSR2_CONVERT_COMP_SPV);
    upscale = std::make_unique<Pass>(device, scheduler, descriptor_pool, layout,
                                     SGSR2_UPSCALE_COMP_SPV);
}

Sgsr2Upscaler::~Sgsr2Upscaler() = default;

bool Sgsr2Upscaler::IsSupported(const Device& device) {
    constexpr VkFormatFeatureFlags storage =
        VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    return device.IsFormatSupported(VK_FORMAT_R16G16B16A16_SFLOAT,
                                    storage | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT,
                                    FormatType::Optimal) &&
           device.IsFormatSupported(VK_FORMAT_R32_UINT, storage, FormatType::Optimal);
}

Sgsr2Upscaler::Texture Sgsr2Upscaler::CreateTexture(VkFormat format, u32 width, u32 height) {
    Texture texture;
    texture.image = memory_allocator.CreateImage(VkImageCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {(std::max)(width, 1U), (std::max)(height, 1U), 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    });
    texture.view = device.GetLogical().CreateImageView(VkImageViewCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = *texture.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .components{},
        .subresourceRange{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    });
    return texture;
}

void Sgsr2Upscaler::CreateResources(VkExtent2D render_size, VkExtent2D upscale_size) {
    motion_depth_clip =
        CreateTexture(VK_FORMAT_R16G16B16A16_SFLOAT, render_size.width, render_size.height);
    ycocg = CreateTexture(VK_FORMAT_R32_UINT, render_size.width, render_size.height);
    for (Texture& texture : history) {
        texture =
            CreateTexture(VK_FORMAT_R16G16B16A16_SFLOAT, upscale_size.width, upscale_size.height);
    }
    output = CreateTexture(VK_FORMAT_R16G16B16A16_SFLOAT, upscale_size.width, upscale_size.height);
    render_extent = render_size;
    upscale_extent = upscale_size;
    first_execution = true;
    frame_parity = 0;
    LOG_INFO(Render_Vulkan, "SGSR 2 upscaler: {}x{} -> {}x{}", render_size.width,
             render_size.height, upscale_size.width, upscale_size.height);
}

void Sgsr2Upscaler::Upscale(const Inputs& in) {
    if (!output.image || in.render_size.width != render_extent.width ||
        in.render_size.height != render_extent.height ||
        in.upscale_size.width != upscale_extent.width ||
        in.upscale_size.height != upscale_extent.height) {
        if (output.image) {
            scheduler.Finish(); // the old images may still be in use
        }
        CreateResources(in.render_size, in.upscale_size);
    }
    const bool first = std::exchange(first_execution, false);
    const bool reset = in.reset || first;

    const f32 rw = static_cast<f32>(render_extent.width);
    const f32 rh = static_cast<f32>(render_extent.height);
    const f32 uw = static_cast<f32>(upscale_extent.width);
    const f32 uh = static_cast<f32>(upscale_extent.height);
    // The game's vectors point to the previous frame (previous UV = UV + vector * scale / size);
    // SGSR's motion is current minus previous in NDC, two units across the screen.
    const Params params{
        .render_size = {render_extent.width, render_extent.height},
        .display_size = {upscale_extent.width, upscale_extent.height},
        .render_size_rcp = {1.0f / rw, 1.0f / rh},
        .display_size_rcp = {1.0f / uw, 1.0f / uh},
        .jitter = in.jitter,
        .motion_scale = {-2.0f * in.motion_vector_scale[0] / rw,
                         -2.0f * in.motion_vector_scale[1] / rh},
        .pre_exposure = 1.0f,
        .camera_fov_angle_hor = std::tan(0.5f * CAMERA_FOV) * rw / rh,
        .camera_near = 0.1f,
        .min_lerp_contribution = 0.0f,
        .same_camera_frames = 0,
        .reset = reset ? 1U : 0U,
    };
    const VkDeviceSize size = Common::AlignUp(
        VkDeviceSize{sizeof(Params)},
        (std::max<VkDeviceSize>)(device.GetUniformBufferAlignment(), 16));
    const StagingBufferRef uniforms = staging_buffer_pool.Request(size, MemoryUsage::Upload);
    std::memcpy(uniforms.mapped_span.data(), &params, sizeof(params));

    const size_t p = frame_parity;
    const size_t q = p ^ 1;
    std::vector<VkImage> initialize;
    if (first) {
        initialize = {*motion_depth_clip.image, *ycocg.image, *history[0].image,
                      *history[1].image, *output.image};
    }
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([initialize](vk::CommandBuffer cmdbuf) {
        // The inputs come from the game's draws and dispatches; our images from the last frame.
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               0,
                               MemoryBarrier(VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                                 VK_ACCESS_SHADER_WRITE_BIT |
                                                 VK_ACCESS_TRANSFER_WRITE_BIT,
                                             VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                                                 VK_ACCESS_TRANSFER_WRITE_BIT));
        if (initialize.empty()) {
            return;
        }
        constexpr VkImageSubresourceRange range{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        };
        std::vector<VkImageMemoryBarrier> barriers;
        for (const VkImage image : initialize) {
            barriers.push_back(VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = 0,
                .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image,
                .subresourceRange = range,
            });
        }
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, {}, {}, barriers);
        for (const VkImage image : initialize) {
            cmdbuf.ClearColorImage(image, VK_IMAGE_LAYOUT_GENERAL, VkClearColorValue{}, range);
        }
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               0,
                               MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT,
                                             VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT));
    });

    convert->Dispatch(scheduler, descriptor_queue, uniforms.buffer, uniforms.offset, size,
                      {in.color, in.depth, in.motion_vectors, *motion_depth_clip.view,
                       *ycocg.view},
                      Common::DivCeil(render_extent.width, 8U),
                      Common::DivCeil(render_extent.height, 8U));
    upscale->Dispatch(scheduler, descriptor_queue, uniforms.buffer, uniforms.offset, size,
                      {*history[p].view, *motion_depth_clip.view, *ycocg.view, *output.view,
                       *history[q].view},
                      Common::DivCeil(upscale_extent.width, 8U),
                      Common::DivCeil(upscale_extent.height, 8U));
    scheduler.Record([](vk::CommandBuffer cmdbuf) {
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                               MemoryBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT));
    });
    frame_parity = q;
}

} // namespace Vulkan
