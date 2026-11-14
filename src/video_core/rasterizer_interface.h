// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <functional>
#include <optional>
#include <span>
#include <utility>
#include "common/common_types.h"
#include "common/polyfill_thread.h"
#include "common/settings.h"
#include "video_core/cache_types.h"
#include "video_core/dsmod/texture_readback.h"
#include "video_core/engines/fermi_2d.h"
#include "video_core/game_dlss_status.h"
#include "video_core/gpu.h"
#include "video_core/query_cache/types.h"
#include "video_core/rasterizer_download_area.h"

namespace Tegra {
class MemoryManager;
namespace Engines {
class AccelerateDMAInterface;
}
namespace Control {
struct ChannelState;
}
namespace Texture {
struct TICEntry;
}
} // namespace Tegra

namespace VideoCore {

enum class LoadCallbackStage {
    Prepare,
    Build,
    Complete,
};
using DiskResourceLoadCallback = std::function<void(LoadCallbackStage, std::size_t, std::size_t)>;

class RasterizerInterface {
public:
    virtual ~RasterizerInterface() = default;

    /// Dispatches a draw invocation
    virtual void Draw(bool is_indexed, u32 instance_count) = 0;

    /// Dispatches an indirect draw invocation
    virtual void DrawIndirect() {}

    /// Dispatches an draw texture invocation
    virtual void DrawTexture() = 0;

    /// Clear the current framebuffer
    virtual void Clear(u32 layer_count) = 0;

    /// Dispatches a compute shader invocation
    virtual void DispatchCompute() = 0;

    /// Resets the counter of a query
    virtual void ResetCounter(VideoCommon::QueryType type) = 0;

    /// Records a GPU query and caches it
    virtual void Query(GPUVAddr gpu_addr, VideoCommon::QueryType type,
                       VideoCommon::QueryPropertiesFlags flags, u32 payload, u32 subreport) = 0;

    /// Signal an uniform buffer binding
    virtual void BindGraphicsUniformBuffer(size_t stage, u32 index, GPUVAddr gpu_addr,
                                           u32 size) = 0;

    /// Signal disabling of a uniform buffer
    virtual void DisableGraphicsUniformBuffer(size_t stage, u32 index) = 0;

    /// Signal a GPU based semaphore as a fence. force_completion also waits at low accuracy.
    virtual void SignalFence(std::function<void()>&& func, bool force_completion = false) = 0;

    /// Send an operation to be done after a certain amount of flushes.
    virtual void SyncOperation(std::function<void()>&& func) = 0;

    /// Signal a GPU based syncpoint as a fence
    virtual void SignalSyncPoint(u32 value) = 0;

    /// Signal a GPU based reference as point
    virtual void SignalReference() = 0;

    /// Release all pending fences.
    virtual void ReleaseFences(bool force = true) = 0;

    /// Notify rasterizer that all caches should be flushed to Switch memory
    virtual void FlushAll() = 0;

    /// Notify rasterizer that any caches of the specified region should be flushed to Switch memory
    virtual void FlushRegion(DAddr addr, u64 size,
                             VideoCommon::CacheType which = VideoCommon::CacheType::All) = 0;

    /// Writes the region's query reports to guest memory, waiting for pending host results.
    virtual void FlushQueryResults(DAddr addr, u64 size) {
        FlushRegion(addr, size, VideoCommon::CacheType::QueryCache);
    }

    /// Switch 2, from the GPU thread's front: where a GPU-written command list's words are.
    enum class GpuWrittenList {
        Unknown, ///< Neither: the caller drains, flushes the page and notes it synced
        Copied,  ///< `out` (within one 4 KiB page) is filled from the copy made after its writer
        Synced,  ///< Guest memory holds the page: no GPU work wrote it since it was flushed
        /// Draws write the page: ask again with `past_sync` once the split's back ran the work
        /// before the last host-level wait (GpuSplit::WaitForSync)
        NeedsSync,
    };
    virtual GpuWrittenList ReadGpuWrittenCommandList([[maybe_unused]] DAddr addr,
                                                     [[maybe_unused]] std::span<u8> out,
                                                     [[maybe_unused]] bool past_sync) {
        return GpuWrittenList::Unknown;
    }

    /// Switch 2: the front flushed this command list page with the split drained.
    virtual void NoteGpuWrittenCommandListSynced([[maybe_unused]] DAddr page) {}

    /// Check if the the specified memory area requires flushing to CPU Memory.
    virtual bool MustFlushRegion(DAddr addr, u64 size,
                                 VideoCommon::CacheType which = VideoCommon::CacheType::All) = 0;

    virtual RasterizerDownloadArea GetFlushArea(DAddr addr, u64 size) = 0;

    /// Notify rasterizer that any caches of the specified region should be invalidated
    virtual void InvalidateRegion(DAddr addr, u64 size,
                                  VideoCommon::CacheType which = VideoCommon::CacheType::All) = 0;

    /// Called before guest memory in the region is overwritten, ahead of InvalidateRegion
    virtual void PrepareRegionWrite([[maybe_unused]] DAddr addr, [[maybe_unused]] u64 size,
                                    [[maybe_unused]] VideoCommon::CacheType which) {}

    virtual void InnerInvalidation(std::span<const std::pair<DAddr, std::size_t>> sequences) {
        if (!Settings::values.skip_cpu_inner_invalidation.GetValue()) {
            for (const auto& [cpu_addr, size] : sequences) {
                InvalidateRegion(cpu_addr, size);
            }
        }
    }

    /// Notify rasterizer that any caches of the specified region are desync with guest
    virtual void OnCacheInvalidation(PAddr addr, u64 size) = 0;

    /// OnCacheInvalidation for each of the CPU writes InvalidateGPUCache collected.
    virtual void OnCacheInvalidations(std::span<const std::pair<DAddr, u64>> ranges) {
        for (const auto& [addr, size] : ranges) {
            OnCacheInvalidation(addr, size);
        }
    }

    /// Return true when an image in the region holds GPU writes guest memory does not have
    virtual bool HasUnsavedImageWrites(DAddr addr, u64 size) {
        return MustFlushRegion(addr, size, VideoCommon::CacheType::TextureCache);
    }

    /// Invalidates a region the CPU overwrote as a whole (an NCE page fault), unless the buffer
    /// cache or an image owns GPU-written data inside it, which must be flushed first. Returns
    /// false, having done nothing, in that case. Implementations decide and invalidate atomically,
    /// so a GPU write recorded concurrently cannot be disowned without its download.
    virtual bool OnCacheInvalidationIfNotGpuModified(DAddr addr, u64 size) {
        if (MustFlushRegion(addr, size, VideoCommon::CacheType::BufferCache) ||
            HasUnsavedImageWrites(addr, size)) {
            return false;
        }
        OnCacheInvalidation(addr, size);
        return true;
    }

    /// Waits for an async download already recorded with the GPU writes in a region to reach guest
    /// memory, so a CPU write to the region needs no download of its own. Returns false, without
    /// waiting, when no such download holds them all.
    virtual bool WaitForPendingDownload(DAddr addr, u64 size) {
        return false;
    }

    /// True when an async download already recorded holds every GPU write in the region.
    virtual bool IsRegionAwaitingDownload(DAddr addr, u64 size) {
        return false;
    }

    /// The commands being parsed carry arguments the GPU wrote, and they are consumed without
    /// waiting for the GPU. Order every earlier GPU write before the work that follows.
    virtual void OnGpuWrittenCommandData() {}

    virtual bool OnCPUWrite(PAddr addr, u64 size) = 0;

    /// Sync memory between guest and host.
    virtual void InvalidateGPUCache() = 0;

    /// Unmap memory range
    virtual void UnmapMemory(DAddr addr, u64 size) = 0;

    /// Remap GPU memory range. This means underneath backing memory changed
    virtual void ModifyGPUMemory(size_t as_id, GPUVAddr addr, u64 size) = 0;

    /// Notify rasterizer that any caches of the specified region should be flushed to Switch memory
    /// and invalidated
    virtual void FlushAndInvalidateRegion(
        DAddr addr, u64 size, VideoCommon::CacheType which = VideoCommon::CacheType::All) = 0;

    /// Notify the host renderer to wait for previous primitive and compute operations.
    virtual void WaitForIdle() = 0;

    /// Notify the host renderer to wait for reads and writes to render targets and flush caches.
    virtual void FragmentBarrier() = 0;

    /// Notify the host renderer to make available previous render target writes.
    virtual void TiledCacheBarrier() = 0;

    /// Notify the rasterizer to send all written commands to the host GPU.
    virtual void FlushCommands() = 0;

    /// Submits the recorded commands and waits for the host GPU to finish them (diagnostics).
    virtual void FinishCommands() {}

    /// Barrier mode 2: the GPU thread has run a frame's command lists.
    virtual void Nx2FrameDone() {}

    /// Notify rasterizer that a frame is about to finish
    virtual void TickFrame() = 0;

    /// Eden Duo: give back host memory that is no longer needed (cache garbage collection) when the
    /// device reports low memory. Runs on the GPU thread; the default does nothing.
    virtual void RecoverMemory() {}

    virtual bool AccelerateConditionalRendering() {
        return false;
    }

    /// Attempt to use a faster method to perform a surface copy
    [[nodiscard]] virtual bool AccelerateSurfaceCopy(
        const Tegra::Engines::Fermi2D::Surface& src, const Tegra::Engines::Fermi2D::Surface& dst,
        const Tegra::Engines::Fermi2D::Config& copy_config) {
        return false;
    }

    /// Called around a software surface copy into dst (done: false before it, true after), so
    /// the rasterizer can update dst's texture in place instead of reloading it
    virtual void SoftwareSurfaceCopy(const Tegra::Engines::Fermi2D::Surface& dst,
                                     const Tegra::Engines::Fermi2D::Config& copy_config,
                                     bool done) {}
    /// Stand-in for a DLSS evaluation: temporal upscale of src into dst along motion_vectors
    /// (jitter in src pixels; motion_scale turns a motion vector into a UV offset to the previous
    /// frame), with the frame's depth when known. False when unsupported; the caller then falls
    /// back to AccelerateSurfaceCopy.
    [[nodiscard]] virtual bool AccelerateTemporalUpscale(
        const Tegra::Engines::Fermi2D::Surface& src, const Tegra::Engines::Fermi2D::Surface& dst,
        const Tegra::Engines::Fermi2D::Surface& motion_vectors,
        const Tegra::Texture::TICEntry* depth, std::array<f32, 2> jitter,
        std::array<f32, 2> motion_scale) {
        return false;
    }

    /// NVIDIA DLSS (DLAA or Super Resolution) in place of a game's own anti-aliasing / upscaling
    /// of src into dst (Ocarina of Time; ocarina_upscale.h), with jitter and camera motion from
    /// the host. False when it can't run; the caller retains its native dispatch.
    [[nodiscard]] virtual bool AccelerateCameraMotionUpscale(
        const Tegra::Engines::Fermi2D::Surface& src, const Tegra::Engines::Fermi2D::Surface& dst) {
        return false;
    }

    /// Whether a game's own DLSS network can run here: its matrix multiplies are emulated with
    /// 32-wide shuffles, and its output must be checkable (ProbeGameDlssOutput).
    [[nodiscard]] virtual bool SupportsGameDlssNetwork() const {
        return false;
    }

    /// Whether NVIDIA DLSS (the "Use NVIDIA DLSS" setting) would draw a game's DLSS evaluation.
    [[nodiscard]] virtual bool UsesNvidiaDlss() const {
        return false;
    }

    /// Whether ProbeGameDlssOutput checks anything on this backend.
    [[nodiscard]] virtual bool CanProbeGameDlssOutput() const {
        return false;
    }

    /// Records a health check of a game's DLSS network output (dst) against its input (src) and
    /// returns the result of an earlier check once the GPU has finished it.
    [[nodiscard]] virtual std::optional<VideoCore::GameDlssProbe> ProbeGameDlssOutput(
        const Tegra::Engines::Fermi2D::Surface& src, const Tegra::Engines::Fermi2D::Surface& dst) {
        return std::nullopt;
    }

    [[nodiscard]] virtual Tegra::Engines::AccelerateDMAInterface& AccessAccelerateDMA() = 0;

    virtual void AccelerateInlineToMemory(GPUVAddr address, size_t copy_size,
                                          std::span<const u8> memory) = 0;

    /// Initialize disk cached resources for the game being emulated
    virtual void LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                                   const DiskResourceLoadCallback& callback) {}

    virtual void InitializeChannel(Tegra::Control::ChannelState& channel) {}

    virtual void BindChannel(Tegra::Control::ChannelState& channel) {}

    virtual void ReleaseChannel(s32 channel_id) {}

    /// Register the address as a Transform Feedback Object
    virtual void RegisterTransformFeedback(GPUVAddr tfb_object_addr) {}

    /// Returns true when the rasterizer has Draw Transform Feedback capabilities
    virtual bool HasDrawTransformFeedback() {
        return false;
    }

    /// Eden Duo (runtime 24): whether DSModReadbackTexture can read guest textures back.
    [[nodiscard]] virtual bool DSModSupportsTextureReadback() const {
        return false;
    }

    /// Eden Duo: on the GPU thread, start a bounded CPU copy of one guest texture
    /// (video_core/dsmod/texture_readback.h). True = taken: `callback` runs exactly once, now or
    /// when the copy lands. False = this renderer cannot do it; `callback` is left untouched.
    virtual bool DSModReadbackTexture(const DSMod::ReadbackRequest& request,
                                      DSMod::ReadbackCallback& callback) {
        return false;
    }
};
} // namespace VideoCore
