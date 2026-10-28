// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

// Eden Duo: the Vulkan presenter for the aux ("second screen") window. RendererVulkan owns one
// instance and calls into it from Composite (Route) and SetAuxWindow; everything else about the
// second screen -- its surface, swapchain, present manager, the mod UI upload/blit path and the
// GPU-composite quad pipeline -- lives here so the upstream renderer only carries hook calls.

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

#include <boost/container/small_vector.hpp>

#include "common/common_types.h"
#include "video_core/dsmod/aux_routing.h"
#include "video_core/framebuffer_config.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/renderer_vulkan/vk_blit_screen.h"
#include "video_core/renderer_vulkan/vk_present_manager.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Core::Frontend {
class EmuWindow;
}

namespace Tegra {
class GPU;
}

namespace Vulkan {

class Device;
class MemoryAllocator;
class RasterizerVulkan;
class Scheduler;

class DSModAuxPresenter {
public:
    explicit DSModAuxPresenter(Tegra::GPU& gpu, Tegra::MaxwellDeviceMemoryManager& device_memory,
                               const vk::Instance& instance, const Device& device,
                               MemoryAllocator& memory_allocator, Scheduler& scheduler,
                               RasterizerVulkan& rasterizer);
    ~DSModAuxPresenter();

    DSModAuxPresenter(const DSModAuxPresenter&) = delete;
    DSModAuxPresenter& operator=(const DSModAuxPresenter&) = delete;

    /// Attach (or detach with nullptr) the aux window. See RendererBase::SetAuxWindow.
    void SetAuxWindow(Core::Frontend::EmuWindow* aux_window_);

    /// Presents this frame's aux output (mod UI, GPU composite, bound layer or mirror) and returns
    /// the layers that stay on the primary screen. The returned span is valid until the next call.
    std::span<const Tegra::FramebufferConfig> Route(
        std::span<const Tegra::FramebufferConfig> framebuffers);

    /// EDEN_NO_PRESENT (headless verification): never hand a primary frame to the window.
    static bool PrimaryPresentDisabled();

    /// Set from the frontend: false on a single-screen device, where Duo has nothing to present.
    /// While disabled Route() hands the guest's layers straight back and no aux work is done.
    static void SetDuoEnabled(bool enabled);
    static bool DuoEnabled();

private:
    void SyncAuxWindow();
    void RenderAuxWindow(std::span<const Tegra::FramebufferConfig> layers);
    /// Presents the mod runtime's UI image on the aux window. Returns false when there is none.
    bool RenderAuxModUi();

    Tegra::GPU& gpu;
    Tegra::MaxwellDeviceMemoryManager& device_memory;
    const vk::Instance& instance;
    const Device& device;
    MemoryAllocator& memory_allocator;
    Scheduler& scheduler;
    RasterizerVulkan& rasterizer;

    /// Storage for the primary layers when a bound layer is routed away (Route's result).
    boost::container::small_vector<Tegra::FramebufferConfig, 4> primary_layers;

    // DSMod aux output. Attach/detach requests are applied on the GPU thread (SyncAuxWindow);
    // a detach additionally blocks the caller until the surface is no longer in use, or force-
    // destroys under aux_mutex when the GPU thread is idle (paused), so the frontend may release
    // the native window right after SetAuxWindow(nullptr) returns.
    void DestroyAuxLocked();
    std::atomic<Core::Frontend::EmuWindow*> pending_aux_window{nullptr};
    std::atomic<bool> aux_dirty{false};
    std::mutex aux_mutex;
    std::condition_variable aux_cv;
    Core::Frontend::EmuWindow* aux_window{nullptr};
    std::optional<vk::SurfaceKHR> aux_surface;
    std::optional<Swapchain> aux_swapchain;
    std::optional<PresentManager> aux_present_manager;
    std::optional<BlitScreen> blit_aux;
    vk::RenderPass aux_ui_render_pass;
    vk::Image aux_ui_image;
    // Persistent staging buffer for the aux-UI upload. The old path allocated a throwaway staging
    // buffer per republish and called scheduler.Finish() to keep it alive until the GPU copy ran --
    // a full blocking GPU drain on the shared scheduler, on the render thread, ahead of the guest
    // frame's own present in Composite(). Keeping the buffer alive across frames lets the copy be
    // recorded and left in flight (no Finish); aux_ui_upload_tick fences reuse so we never scribble
    // over a copy the GPU has not consumed yet.
    vk::Buffer aux_ui_staging;
    size_t aux_ui_staging_bytes{};
    u64 aux_ui_upload_tick{};
    u64 aux_ui_blit_tick{}; ///< last blit that read aux_ui_image; retire before replacing it
    u32 aux_ui_w{};
    u32 aux_ui_h{};
    u64 aux_ui_serial{};
    bool aux_ui_presented{}; ///< the current UI has been blitted+presented at least once
    u32 aux_ui_last_w{};
    u32 aux_ui_last_h{};
    bool aux_ui_last_full{true}; ///< Eden Duo: the last present filled the frame (no Fit bars)
    // Reused compact scratch buffer for a partial (dirty-sub-rect) upload: RenderAuxModUi extracts
    // just the dirty rows out of the full canvas into here (mirrors AuxRouting::PublishUiPartial's
    // own row-bounded copy), so UploadImageAsync's staging copy is proportional to the dirty area,
    // not the whole ~5 MB canvas. Grows only, like aux_ui_staging itself.
    std::vector<u32> aux_ui_partial_scratch;

    // ------------- GPU composite path (EDEN_DSMOD_GPU_COMPOSITE) -------------
    // One coverage-blend pipeline that draws a display list of textured quads over up to NumAuxTex
    // cached source textures (0=map area, 1=icon atlas, 2=HUD overlay) into the aux frame's render
    // pass. Textures re-upload async (persistent per-slot staging, no Finish) only when their
    // serial moves; the marker/map-pan is just new quad geometry per frame.
    static constexpr u32 NumAuxTex = VideoCore::DSMod::AuxRouting::NumAuxTex;
    void EnsureAuxCompositeResources();
    bool RenderAuxModUiGpu();
    bool aux_c_init{false};
    vk::DescriptorSetLayout aux_c_set_layout;
    vk::DescriptorSetLayout aux_c_map_set_layout;
    vk::PipelineLayout aux_c_pipe_layout;
    vk::PipelineLayout aux_c_map_pipe_layout;
    vk::ShaderModule aux_c_vert;
    vk::ShaderModule aux_c_frag;
    vk::ShaderModule aux_c_map_frag;
    vk::RenderPass aux_c_render_pass;
    vk::Pipeline aux_c_pipeline;
    vk::Pipeline aux_c_map_pipeline;
    vk::Sampler aux_c_sampler_nearest;
    vk::DescriptorPool aux_c_pool;
    vk::DescriptorPool aux_c_map_pool;
    vk::DescriptorSets aux_c_sets;
    vk::DescriptorSets aux_c_map_set;
    bool aux_c_map_desc_ready{false};
    std::array<vk::Image, NumAuxTex> aux_c_img;
    std::array<vk::ImageView, NumAuxTex> aux_c_view;
    std::array<vk::Buffer, NumAuxTex> aux_c_staging;
    std::array<size_t, NumAuxTex> aux_c_staging_bytes{};
    std::array<u32, NumAuxTex> aux_c_w{};
    std::array<u32, NumAuxTex> aux_c_h{};
    std::array<u64, NumAuxTex> aux_c_serial{};
    std::array<u64, NumAuxTex> aux_c_upload_tick{};
    u64 aux_c_draw_tick{}; ///< last recorded quad draw; retire before replacing a sampled image
    // Last composite taken from the mod: kept so the panel can be re-presented (resize, reattach)
    // without a new publish, and held untouched when nothing changed.
    std::vector<VideoCore::DSMod::AuxRouting::Quad> aux_c_quads;
    u32 aux_c_cw{};
    u32 aux_c_ch{};
    u32 aux_c_bg{};
    u64 aux_c_comp_serial{};
    bool aux_c_dirty{false}; ///< quads or a texture changed since the last present
    bool aux_c_presented{false};
    u32 aux_c_last_w{}; ///< panel size at the last present
    u32 aux_c_last_h{};
    bool aux_c_last_full{true}; ///< Eden Duo: the last present filled the frame (no Fit bars)
    u64 aux_c_present_count{};
    std::array<u64, NumAuxTex> aux_c_upload_count{};
    vk::Buffer aux_c_readback_buf;
    u32 aux_c_readback_w{};
    u32 aux_c_readback_h{};
};

} // namespace Vulkan
