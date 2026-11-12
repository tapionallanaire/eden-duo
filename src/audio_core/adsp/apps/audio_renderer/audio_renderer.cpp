// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <chrono>
#include <thread>

#include "audio_core/adsp/apps/audio_renderer/audio_renderer.h"
#include "audio_core/audio_core.h"
#include "audio_core/common/common.h"
#include "audio_core/sink/sink.h"
#include "common/logging.h"
#include "common/thread.h"
#include "core/core.h"
#include "core/core_timing.h"

namespace AudioCore::ADSP::AudioRenderer {

AudioRenderer::AudioRenderer(Core::System& system_, Sink::Sink& sink_)
    : system{system_}, sink{sink_} {}

AudioRenderer::~AudioRenderer() {
    Stop();
}

void AudioRenderer::Start() {
    CreateSinkStreams();

    mailbox.Initialize(AppMailboxId::AudioRenderer);

    main_thread = std::jthread([this](std::stop_token stop_token) { Main(stop_token); });

    mailbox.Send(Direction::DSP, Message::InitializeOK);
    if (mailbox.Receive(Direction::Host) != Message::InitializeOK) {
        LOG_ERROR(Service_Audio, "Host Audio Renderer -- Failed to receive shutdown "
                                 "message response from ADSP!");
        return;
    }
    running = true;
}

void AudioRenderer::Stop() {
    if (!running) {
        return;
    }

    mailbox.Send(Direction::DSP, Message::Shutdown);
    if (mailbox.Receive(Direction::Host) != Message::Shutdown) {
        LOG_ERROR(Service_Audio, "Host Audio Renderer -- Failed to receive shutdown "
                                 "message response from ADSP!");
    }
    main_thread.request_stop();
    main_thread.join();

    for (auto& stream : streams) {
        if (stream) {
            stream->Stop();
            sink.CloseStream(stream);
            stream = nullptr;
        }
    }
    running = false;
}

void AudioRenderer::Signal() {
    signalled_tick = system.CoreTiming().GetGlobalTimeNs().count();
    Send(Direction::DSP, Message::Render);
}

void AudioRenderer::Wait() {
    auto msg = Receive(Direction::Host);
    if (msg != Message::RenderResponse) {
        LOG_ERROR(Service_Audio,
                  "Did not receive the expected render response from the AudioRenderer! Expected "
                  "{}, got {}",
                  Message::RenderResponse, msg);
    }
    PostDSPClearCommandBuffer();
}

void AudioRenderer::Send(Direction dir, u32 message) {
    mailbox.Send(dir, std::move(message));
}

u32 AudioRenderer::Receive(Direction dir) {
    return mailbox.Receive(dir);
}

void AudioRenderer::SetCommandBuffer(s32 session_id, CpuAddr buffer, u64 size, u64 time_limit,
                                     u64 applet_resource_user_id, Kernel::KProcess* process,
                                     bool reset) noexcept {
    command_buffers[session_id].buffer = buffer;
    command_buffers[session_id].size = size;
    command_buffers[session_id].time_limit = time_limit;
    command_buffers[session_id].applet_resource_user_id = applet_resource_user_id;
    command_buffers[session_id].process = process;
    command_buffers[session_id].reset_buffer = reset;
}

void AudioRenderer::PostDSPClearCommandBuffer() noexcept {
    for (auto& buffer : command_buffers) {
        buffer.buffer = 0;
        buffer.size = 0;
        buffer.reset_buffer = false;
    }
}

u32 AudioRenderer::GetRemainCommandCount(s32 session_id) const noexcept {
    return command_buffers[session_id].remaining_command_count;
}

void AudioRenderer::ClearRemainCommandCount(s32 session_id) noexcept {
    command_buffers[session_id].remaining_command_count = 0;
}

u64 AudioRenderer::GetRenderingStartTick(s32 session_id) const noexcept {
    return (1000 * command_buffers[session_id].render_time_taken_us) + signalled_tick;
}

void AudioRenderer::CreateSinkStreams() {
    u32 channels{sink.GetDeviceChannels()};
    for (u32 i = 0; i < MaxRendererSessions; i++) {
        std::string name{fmt::format("ADSP_RenderStream-{}", i)};
        streams[i] =
            sink.AcquireSinkStream(system, channels, name, ::AudioCore::Sink::StreamType::Render);
        streams[i]->SetRingSize(4);
    }
}

void AudioRenderer::Main(std::stop_token stop_token) {
    Common::SetCurrentThreadName("DSP_AudioRenderer_Main");
    // The emulated CPU cores and the GPU thread run at Critical. At High this thread lost the
    // core to them while a level loaded and the sink ran dry: renders came up to 54 ms apart for
    // 5 ms of audio each (Retroid Pocket Flip 2, Crash Team Racing). It uses about 1 % of a core.
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);

    // TODO: Create buffer map/unmap thread + mailbox
    // TODO: Create gMix devices, initialize them here

    if (mailbox.Receive(Direction::DSP) != Message::InitializeOK) {
        LOG_ERROR(Service_Audio, "ADSP Audio Renderer -- Failed to receive initialize message from host!");
        return;
    }

    mailbox.Send(Direction::Host, Message::InitializeOK);

    // 0.12 seconds (2,304,000 / 19,200,000)
    constexpr u64 max_process_time{2'304'000ULL};

    // When the next render is due on the host clock, see the pacing below.
    auto next_render{std::chrono::steady_clock::now()};
    while (!stop_token.stop_requested()) {
        auto msg{mailbox.Receive(Direction::DSP)};
        switch (msg) {
        case Message::Shutdown:
            mailbox.Send(Direction::Host, Message::Shutdown);
            return;

        case Message::Render: {
            if (system.IsShuttingDown()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                mailbox.Send(Direction::Host, Message::RenderResponse);
                continue;
            }
            std::array<bool, MaxRendererSessions> buffers_reset{};
            std::array<u64, MaxRendererSessions> render_times_taken{};
            const auto start_time{system.CoreTiming().GetGlobalTimeUs().count()};

            // Every session gets one buffer per render and its own host stream drains it, so the
            // render rate has to follow the stream with the least queued. Pacing on the first
            // session alone left the second one a buffer or two lower, and its device callback
            // (1024 frames on Android) came up short several times a minute: Crash Team Racing
            // plays through two sessions (Retroid Pocket Flip 2).
            u32 wait_index{MaxRendererSessions};
            u32 lowest_queue{~0u};
            for (u32 index = 0; index < MaxRendererSessions; index++) {
                if (command_buffers[index].buffer != 0 && streams[index]->IsDraining() &&
                    streams[index]->GetQueueSize() < lowest_queue) {
                    lowest_queue = streams[index]->GetQueueSize();
                    wait_index = index;
                }
            }
            if (wait_index == MaxRendererSessions && command_buffers[0].buffer != 0) {
                wait_index = 0; // nothing is playing yet: the first session paces, as before
            }
            if (wait_index < MaxRendererSessions) {
                auto& pace_stream{*streams[wait_index]};
                const u32 need{pace_stream.DeviceRequestBuffers()};
                if (need == 0) {
                    pace_stream.WaitFreeSpace(stop_token);
                } else {
                    // The game's audio thread wakes on the event each render signals and feeds
                    // one 5 ms frame per wake-up. Rendering whenever the stream had room meant
                    // four renders back to back after every device callback (1024 frames on
                    // Android) and then nothing for 21 ms: the wake-ups merged, the game made
                    // about 880 updates in 5 s instead of 1000 and its sound came out in pieces
                    // (Crash Team Racing, Retroid Pocket Flip 2, at a steady 30 FPS). Render on
                    // a 5 ms clock instead; the queue only bounds it: at once when it runs low,
                    // held back while a callback's worth plus a margin is already waiting.
                    // A callback takes `need` buffers at once, so that many stay queued behind
                    // it (25 ms on Android): with three, a 10 ms delay of this thread ran the
                    // stream dry a few times in three minutes.
                    const u32 low{need};
                    const u32 high{low + need + 2};
                    constexpr std::chrono::microseconds period{5000};
                    auto now{std::chrono::steady_clock::now()};
                    if (pace_stream.GetQueueSize() < low) {
                        next_render = now;
                    } else {
                        if (next_render > now) {
                            std::this_thread::sleep_until(next_render);
                        }
                        pace_stream.WaitBelow(high, stop_token);
                        now = std::chrono::steady_clock::now();
                        if (now - next_render > 4 * period) {
                            next_render = now; // held back or late: do not replay the missed ticks
                        }
                    }
                    next_render += period;
                }
            }

            for (u32 index = 0; index < MaxRendererSessions; index++) {
                auto& command_buffer{command_buffers[index]};
                auto& command_list_processor{command_list_processors[index]};

                // Check this buffer is valid, as it may not be used.
                if (command_buffer.buffer != 0) {
                    // If there are no remaining commands (from the previous list),
                    // this is a new command list, initialize it.
                    if (command_buffer.remaining_command_count == 0) {
                        command_list_processor.Initialize(system, *command_buffer.process,
                            command_buffer.buffer,
                            command_buffer.size, streams[index]);
                    }

                    if (command_buffer.reset_buffer && !buffers_reset[index]) {
                        streams[index]->ClearQueue();
                        buffers_reset[index] = true;
                    }

                    u64 max_time{max_process_time};
                    if (index == 1 && command_buffer.applet_resource_user_id ==
                                          command_buffers[0].applet_resource_user_id) {
                        max_time = max_process_time - render_times_taken[0];
                        if (render_times_taken[0] > max_process_time) {
                            max_time = 0;
                        }
                    }

                    max_time = (std::min)(command_buffer.time_limit, max_time);
                    command_list_processor.SetProcessTimeMax(max_time);

                    // Process the command list
                    {
                        render_times_taken[index] =
                            command_list_processor.Process(index) - start_time;
                    }

                    const auto end_time{system.CoreTiming().GetGlobalTimeUs().count()};

                    command_buffer.remaining_command_count =
                        command_list_processor.GetRemainingCommandCount();
                    command_buffer.render_time_taken_us = end_time - start_time;
                }
            }
            mailbox.Send(Direction::Host, Message::RenderResponse);
        } break;
        default:
            LOG_WARNING(Service_Audio, "ADSP AudioRenderer received an invalid message, msg={:02X}!", msg);
            break;
        }
    }
}

} // namespace AudioCore::ADSP::AudioRenderer
