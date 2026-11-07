// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/game_settings.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "common/logging.h"
#include "common/settings.h"
#include "video_core/renderer_base.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace Core::GameSettings {

static GPUVendor GetGPU(const std::string& gpu_vendor_string) {
    struct Entry { const char* name; GPUVendor vendor; };
    static constexpr Entry GpuVendor[] = {
        // NVIDIA
        {"NVIDIA",   GPUVendor::Nvidia},
        {"Nouveau",  GPUVendor::Nvidia},
        {"NVK",      GPUVendor::Nvidia},
        {"Tegra",    GPUVendor::Nvidia},
        // AMD
        {"AMD",       GPUVendor::AMD},
        {"RadeonSI",  GPUVendor::AMD},
        {"RADV",      GPUVendor::AMD},
        {"AMDVLK",    GPUVendor::AMD},
        {"R600",      GPUVendor::AMD},
        // Intel
        {"Intel",     GPUVendor::Intel},
        {"ANV",       GPUVendor::Intel},
        {"i965",      GPUVendor::Intel},
        {"i915",      GPUVendor::Intel},
        {"OpenSWR",   GPUVendor::Intel},
        // Apple
        {"Apple",     GPUVendor::Apple},
        {"MoltenVK",  GPUVendor::Apple},
        // Qualcomm / Adreno
        {"Qualcomm",  GPUVendor::Qualcomm},
        {"Turnip",    GPUVendor::Qualcomm},
        // ARM / Mali
        {"Mali",      GPUVendor::ARM},
        {"PanVK",     GPUVendor::ARM},
        // Imagination / PowerVR
        {"PowerVR",   GPUVendor::Imagination},
        {"PVR",       GPUVendor::Imagination},
        // Microsoft / WARP / D3D12 GL
        {"D3D12",     GPUVendor::Microsoft},
        {"Microsoft", GPUVendor::Microsoft},
        {"WARP",      GPUVendor::Microsoft},
    };

    for (const auto& entry : GpuVendor) {
        if (gpu_vendor_string == entry.name) {
            return entry.vendor;
        }
    }

    // legacy (shouldn't be needed anymore, but just in case)
    std::string gpu = gpu_vendor_string;
    std::transform(gpu.begin(), gpu.end(), gpu.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    if (gpu.find("geforce") != std::string::npos) {
        return GPUVendor::Nvidia;
    }
    if (gpu.find("radeon") != std::string::npos || gpu.find("ati") != std::string::npos) {
        return GPUVendor::AMD;
    }

    return GPUVendor::Unknown;
}

static OS DetectOS() {
#if defined(_WIN32)
    return OS::Windows;
#elif defined(__FIREOS__)
    return OS::FireOS;
#elif defined(__ANDROID__)
    return OS::Android;
#elif defined(__OHOS__)
    return OS::HarmonyOS;
#elif defined(__HAIKU__)
    return OS::HaikuOS;
#elif defined(__DragonFly__)
    return OS::DragonFlyBSD;
#elif defined(__NetBSD__)
    return OS::NetBSD;
#elif defined(__OpenBSD__)
    return OS::OpenBSD;
#elif defined(_AIX)
    return OS::AIX;
#elif defined(__managarm__)
    return OS::Managarm;
#elif defined(__redox__)
    return OS::RedoxOS;
#elif defined(__APPLE__) && defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
    return OS::IOS;
#elif defined(__APPLE__)
    return OS::MacOS;
#elif defined(__FreeBSD__)
    return OS::FreeBSD;
#elif defined(__sun) && defined(__SVR4)
    return OS::Solaris;
#elif defined(__linux__)
    return OS::Linux;
#else
    return OS::Unknown;
#endif
}

EnvironmentInfo DetectEnvironment(const VideoCore::RendererBase& renderer) {
    EnvironmentInfo env{};
    env.os = DetectOS();
    env.vendor_string = renderer.GetDeviceVendor();
    env.vendor = GetGPU(env.vendor_string);
    return env;
}

namespace {
#ifdef __ANDROID__
/// The GPU's family from the kernel driver, readable before any Vulkan device exists.
bool IsAdreno6xx() {
    std::FILE* const file = std::fopen("/sys/class/kgsl/kgsl-3d0/gpu_model", "r");
    if (file == nullptr) {
        return false;
    }
    char model[32]{};
    const bool read = std::fgets(model, sizeof(model), file) != nullptr;
    std::fclose(file);
    return read && std::strncmp(model, "Adreno6", 7) == 0;
}
#endif

/// At boot ([force] false) a title value only fills a setting the user left on global; applying
/// the optimal settings ([force] true) replaces the game's own value too.
template <typename T, typename Setting>
void DefaultForTitle(Setting& setting, T value, bool force) {
    if (force || setting.UsingGlobal()) {
        setting.SetGlobal(false);
        setting.SetValue(value);
    }
}

bool TitleSettings(std::uint64_t program_id, bool force) {
    const auto DefaultForTitle = [force](auto& setting, auto value) {
        GameSettings::DefaultForTitle(setting, value, force);
    };
    switch (static_cast<TitleID>(program_id)) {
        case TitleID::FinalFantasyResonance:
            // The bloom hack scales sampled RGBA values and corrupts this game's sprite colours.
            DefaultForTitle(Settings::values.fix_bloom_effects, false);
            // Synchronous GPU emulation ran the game's render thread and the GPU thread in
            // series (each under half a core on the Odin 2), so a frame cost their sum:
            // 23 FPS on the Chapter 1 overworld in handheld. Asynchronous GPU overlaps them:
            // 29-30 FPS there and in battle on the same capped clocks, with the HUD present in
            // every recorded frame and the party drawn in its own colours. The dropouts and
            // panics once seen with it were the composite and lightmap bugs fixed since 1.1.3.
            DefaultForTitle(Settings::values.use_asynchronous_gpu_emulation, true);
            // Handheld or docked is the user's choice: the game holds 30 FPS in both on the Odin 2
            // (Adreno 740), so no console mode is forced here or by "Apply optimal settings".
            DefaultForTitle(Settings::values.cpu_clock, Settings::CpuClock::Boost);
            DefaultForTitle(Settings::values.vsync_mode, Settings::VSyncMode::Mailbox);
            DefaultForTitle(Settings::values.gpu_accuracy, Settings::GpuAccuracy::High);
            DefaultForTitle(Settings::values.dma_accuracy, Settings::DmaAccuracy::Safe);
            DefaultForTitle(Settings::values.sync_memory_operations, true);
            DefaultForTitle(Settings::values.use_disk_shader_cache, true);
            DefaultForTitle(Settings::values.use_reactive_flushing, true);
            DefaultForTitle(Settings::values.enable_buffer_history, true);
#ifdef __ANDROID__
            DefaultForTitle(Settings::values.use_optimized_vertex_buffers, true);
#endif
            DefaultForTitle(Settings::values.use_asynchronous_shaders, true);
            DefaultForTitle(Settings::values.dyna_state, Settings::ExtendedDynamicState::EDS3);
            DefaultForTitle(Settings::values.vertex_input_dynamic_state, true);
            // FXAA smears its character faces; native resolution keeps them sharp without it.
            DefaultForTitle(Settings::values.anti_aliasing, Settings::AntiAliasing::None);
            DefaultForTitle(Settings::values.gpu_fence_behavior, Settings::GpuFenceBehavior::Accurate);
            LOG_INFO(Core, "Title defaults: async GPU {}, vertex input dynamic state {}, fence {}",
                     Settings::values.use_asynchronous_gpu_emulation.GetValue(),
                     Settings::values.vertex_input_dynamic_state.GetValue(),
                     Settings::CanonicalizeEnum(Settings::values.gpu_fence_behavior.GetValue()));
            return true;
        case TitleID::PokemonScarlet:
        case TitleID::PokemonViolet:
            // Every new area and cutscene builds dozens of pipelines; compiled synchronously, the
            // game freezes on them (Scarlet's intro: 78 s from the enrollment form to Clavell on a
            // fresh cache, 63 s with asynchronous shaders, the photo still drawn).
            DefaultForTitle(Settings::values.use_asynchronous_shaders, true);
            // The set measured on the Retroid Pocket Flip 2 (Snapdragon 865, 1x): 29.5-30 FPS in
            // the overworld. The GPU-write fixes it relies on are for async GPU at low accuracy.
            DefaultForTitle(Settings::values.use_asynchronous_gpu_emulation, true);
            DefaultForTitle(Settings::values.gpu_accuracy, Settings::GpuAccuracy::Low);
            DefaultForTitle(Settings::values.dma_accuracy, Settings::DmaAccuracy::Default);
            DefaultForTitle(Settings::values.gpu_fence_behavior, Settings::GpuFenceBehavior::Default);
            DefaultForTitle(Settings::values.cpu_clock, Settings::CpuClock::Boost);
            // Boosted GPU timing looked the same and was slower in the overworld.
            DefaultForTitle(Settings::values.gpu_clock, Settings::GpuClock::Normal);
            return true;
        case TitleID::TrailsInTheSky1stChapter:
        case TitleID::TrailsInTheSky2ndChapter:
        case TitleID::YsXNordics:
            // Its towns build new pipelines as NPCs, effects and streets come into view; compiled
            // synchronously each one held the frame (Rolent on the AYN Thor, 1x handheld: a 59 ms
            // frame for 2 new shaders, dozens per second on entering the town) with the GPU under
            // half busy. Asynchronous shaders keep the frame going while they build.
            DefaultForTitle(Settings::values.use_asynchronous_shaders, true);
            DefaultForTitle(Settings::values.use_disk_shader_cache, true);
            // Its frames go to Eden's GPU thread and the guest waits on it, so the thread's cost
            // sets the frame rate (it asks for 60 and gets 30 in Rolent, 45 on the roads). Extended
            // dynamic state makes that thread set every state on every draw: EDS3 took the same
            // square from 30 to 20 FPS (GPU thread 16 -> 30 ms a frame).
            DefaultForTitle(Settings::values.dyna_state, Settings::ExtendedDynamicState::Disabled);
            DefaultForTitle(Settings::values.use_asynchronous_gpu_emulation, true);
            // Low accuracy releases its fences when the GPU thread reaches them; nothing in its
            // picture needs more.
            DefaultForTitle(Settings::values.gpu_accuracy, Settings::GpuAccuracy::Low);
            return true;
        case TitleID::HogwartsLegacy:
            // At low accuracy its fences release before the GPU work behind them, and its scenes
            // render washed out to white (the London street, the cave; AYN Thor, docked 1x).
            // High keeps the picture and still holds 30.
            DefaultForTitle(Settings::values.gpu_accuracy, Settings::GpuAccuracy::High);
            DefaultForTitle(Settings::values.use_asynchronous_gpu_emulation, true);
            return true;
        case TitleID::FireEmblemEngage:
        case TitleID::DragonQuestVIIReimagined:
            // Their effect shaders divide by fp16 subnormals (see LoadOverrides). The load-time
            // default yields to a value set for the game; applied, the fix is the game's own value.
            DefaultForTitle(Settings::values.fix_fp16_denormals, true);
            return true;
        case TitleID::XenobladeChronicles2:
        case TitleID::XenobladeChronicles3:
            // Their optimal GPU driver (Turnip v27.0.0-R1) is set by the Android frontend
            // (OptimalDriver), which owns driver_path.
            return true;
        default:
            return false;
    }
}
} // namespace

void LoadBootOverrides(std::uint64_t program_id) {
    TitleSettings(program_id, false);
#ifdef __ANDROID__
    if (static_cast<TitleID>(program_id) == TitleID::FinalFantasyResonance && IsAdreno6xx()) {
        // Turnip's binned (GMEM) rendering hangs the Adreno 6xx in this game's post-processing
        // passes: the GPU sits in vertex fetch and visibility-stream decode until KGSL resets it
        // (VK_ERROR_DEVICE_LOST within minutes on the Retroid Pocket Flip 2, Adreno 650).
        // Rendering straight to memory draws the same image, at about the same frame rate there.
        // Turnip reads TU_DEBUG when the GPU is created, which comes after this.
        // ponytail: stays set for the rest of the process (a later game in it also renders to
        // memory, a little slower); unset it per launch if that ever matters.
        const std::string current = std::getenv("TU_DEBUG") ? std::getenv("TU_DEBUG") : "";
        if (current.find("sysmem") == std::string::npos) {
            const std::string value = current.empty() ? "sysmem" : current + ",sysmem";
            setenv("TU_DEBUG", value.c_str(), 1);
            LOG_INFO(Core, "Title defaults: TU_DEBUG={}", value);
        }
    }
#endif
}

bool HasOptimalSettings(std::uint64_t program_id) {
    switch (static_cast<TitleID>(program_id)) {
        case TitleID::FinalFantasyResonance:
        case TitleID::PokemonScarlet:
        case TitleID::PokemonViolet:
        case TitleID::TrailsInTheSky1stChapter:
        case TitleID::TrailsInTheSky2ndChapter:
        case TitleID::YsXNordics:
        case TitleID::FireEmblemEngage:
        case TitleID::DragonQuestVIIReimagined:
        case TitleID::XenobladeChronicles2:
        case TitleID::XenobladeChronicles3:
        case TitleID::HogwartsLegacy:
            return true;
        default:
            return false;
    }
}

bool ApplyOptimalSettings(std::uint64_t program_id) {
    return TitleSettings(program_id, true);
}

void LoadOverrides(std::uint64_t program_id, const VideoCore::RendererBase& renderer) {
    const auto env = DetectEnvironment(renderer);

    Settings::values.delay_fence_reports_by_title = false;
    Settings::values.fix_fp16_denormals_by_title = false;
    Settings::values.disable_macro_hle_by_title = false;
    Settings::values.skip_buffer_downloads_by_title = false;
    Settings::values.bindless_table_max_by_title = 0;
    switch (static_cast<TitleID>(program_id)) {
        case TitleID::NinjaGaidenRagebound:
            Settings::values.use_squashed_iterated_blend = true;
            break;
        case TitleID::DoubleDragonRevive:
            // Its Unreal occlusion culling reads the query reports once the frame's fence
            // signals. Released on the CPU, the fence signals before the GPU has written them,
            // so stale results cull visible buildings and props for single frames.
            Settings::values.delay_fence_reports_by_title = true;
            break;
        case TitleID::FireEmblemEngage:
        case TitleID::DragonQuestVIIReimagined:
            // Its effect shaders divide by fp16 subnormals; flushed to zero the result goes
            // inf/NaN and the bloom blows characters and lights out to white.
            Settings::values.fix_fp16_denormals_by_title = true;
            break;
        case TitleID::FinalFantasyResonance:
            // As Fire Emblem Engage and Dragon Quest VII above, and in addition: its battle
            // characters are drawn through a GPU macro whose HLE version inherits the parameters
            // of the macros the game issues before its argument register arrives, which blows
            // their materials out to white. The microcode interpreter reads only the parameters
            // its program uses and draws them correctly, at the same frame rate, so give this
            // title the interpreter.
            Settings::values.fix_fp16_denormals_by_title = true;
            Settings::values.disable_macro_hle_by_title = true;
            break;
        case TitleID::TrailsInTheSky1stChapter:
            // The same engine as 2nd Chapter, the same cost: about 1 GB/s of buffer downloads
            // docked outside the Bright house on the AYN Thor, 36 FPS with them.
        case TitleID::YsXNordics:
            // Falcom's engine again: the fencing thread spent 30 ms a frame copying GPU-written
            // buffers back on its title screen (AYN Thor, docked).
        case TitleID::TrailsInTheSky2ndChapter:
            // It marks large buffers GPU-written every frame (about 150 MB/s docked on the Odin 2) and
            // never reads them back on the CPU. Copying them to guest memory took the GPU thread to
            // 30 ms and the fencing thread to 23 ms a frame: 19 FPS in Rolent docked, 49 without.
            Settings::values.skip_buffer_downloads_by_title = true;
            break;
        case TitleID::HogwartsLegacy:
            // Its materials index texture handle tables in 256-384 byte constant buffers (at most
            // 48 handles). At 1024 slots per table the GPU thread spent most of each frame writing
            // empty ones: 17-25 FPS uncapped in the opening cave on a desktop, 40-52 at 64.
            Settings::values.bindless_table_max_by_title = 64;
            break;
        default:
            break;
    }

    LOG_INFO(Core, "Applied game settings for title ID {:016X} on OS {}, GPU vendor {} ({})",
             program_id,
             static_cast<int>(env.os),
             static_cast<int>(env.vendor),
             env.vendor_string);
}

} // namespace Core::GameSettings
