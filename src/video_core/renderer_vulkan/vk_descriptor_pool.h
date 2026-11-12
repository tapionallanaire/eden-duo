// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <shared_mutex>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "shader_recompiler/shader_info.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class Scheduler;

struct DescriptorBank;
class DescriptorLayoutBuilder;

struct DescriptorBankInfo {
    [[nodiscard]] bool IsSuperset(const DescriptorBankInfo& subset) const noexcept;

    u32 uniform_buffers{}; ///< Number of uniform buffer descriptors
    u32 storage_buffers{}; ///< Number of storage buffer descriptors
    u32 texture_buffers{}; ///< Number of texture buffer descriptors
    u32 image_buffers{};   ///< Number of image buffer descriptors
    u32 textures{};        ///< Number of texture descriptors
    u32 images{};          ///< Number of image descriptors
    u32 sampled_images{};  ///< Number of sampled image descriptors (without a sampler)
    u32 samplers{};        ///< Number of sampler descriptors
    s32 score{};           ///< Number of descriptors in total
};

class DescriptorAllocator final : public ResourcePool {
    friend class DescriptorPool;

public:
    explicit DescriptorAllocator() = default;
    ~DescriptorAllocator() override = default;

    DescriptorAllocator& operator=(DescriptorAllocator&&) noexcept = default;
    DescriptorAllocator(DescriptorAllocator&&) noexcept = default;

    DescriptorAllocator& operator=(const DescriptorAllocator&) = delete;
    DescriptorAllocator(const DescriptorAllocator&) = delete;

    VkDescriptorSet Commit();

private:
    explicit DescriptorAllocator(const Device& device_, MasterSemaphore& master_semaphore_,
                                 DescriptorBank& bank_, VkDescriptorSetLayout layout_);

    void Allocate(size_t begin, size_t end) override;

    vk::DescriptorSets AllocateDescriptors(size_t count);

    const Device* device{};
    DescriptorBank* bank{};
    VkDescriptorSetLayout layout{};

    std::vector<vk::DescriptorSets> sets;
    std::vector<VkDescriptorSet> handles; ///< Every set in sets, in commit index order
};

class DescriptorPool {
public:
    explicit DescriptorPool(const Device& device, Scheduler& scheduler);
    ~DescriptorPool();

    DescriptorPool& operator=(const DescriptorPool&) = delete;
    DescriptorPool(const DescriptorPool&) = delete;

    DescriptorAllocator Allocator(const Device& device, Scheduler& scheduler, VkDescriptorSetLayout layout, std::span<const Shader::Info> infos);
    DescriptorAllocator Allocator(const Device& device, Scheduler& scheduler, VkDescriptorSetLayout layout, const Shader::Info& info);
    DescriptorAllocator Allocator(const Device& device, Scheduler& scheduler, VkDescriptorSetLayout layout, const DescriptorBankInfo& info);

    /// One allocator per distinct set layout, used by every pipeline with that layout: sets are
    /// then held for the draws in flight, not for every pipeline ever drawn. Commit() on it only
    /// from the scheduler's command recording.
    DescriptorAllocator& SharedAllocator(const Device& device, Scheduler& scheduler,
                                         const DescriptorLayoutBuilder& builder,
                                         std::span<const Shader::Info> infos);

private:
    struct SharedAllocatorEntry {
        vk::DescriptorSetLayout layout;
        std::unique_ptr<DescriptorAllocator> allocator;
    };

    DescriptorBank& Bank(const Device& device, const DescriptorBankInfo& reqs);
    std::shared_mutex banks_mutex;
    std::vector<DescriptorBankInfo> bank_infos;
    std::vector<std::unique_ptr<DescriptorBank>> banks;
    std::mutex shared_mutex;
    std::unordered_map<std::string, SharedAllocatorEntry> shared_allocators;
};

} // namespace Vulkan
