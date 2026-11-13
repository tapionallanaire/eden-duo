// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstring>
#include <optional>
#include <set>

#include "common/logging.h"
#include "common/settings.h"
#include "video_core/host1x/ffmpeg.h"
#include "video_core/host1x/h264_packet.h"
#include "video_core/host1x/nx2_video_decoder.h"

#ifdef __ANDROID__
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#endif

extern "C" {
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#endif

#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
}

namespace Tegra::Host1x {

namespace {

std::optional<NvdecCommon::VideoCodec> CodecFromMime(std::string_view mime) {
    if (mime.find("vp9") != std::string_view::npos) {
        return NvdecCommon::VideoCodec::VP9;
    }
    if (mime.find("vp8") != std::string_view::npos) {
        return NvdecCommon::VideoCodec::VP8;
    }
    // "video/av01" or "video/av1". FFmpeg decodes AV1 with libdav1d.
    if (mime.find("av01") != std::string_view::npos || mime.find("av1") != std::string_view::npos) {
        return NvdecCommon::VideoCodec::AV1;
    }
    if (mime.find("avc") != std::string_view::npos || mime.find("264") != std::string_view::npos) {
        return NvdecCommon::VideoCodec::H264;
    }
    return std::nullopt;
}

#ifdef __ANDROID__
// AV1 through the platform's decoder (NDK MediaCodec): the Android FFmpeg package has no AV1
// decoder, and Switch 2 titles' movies are AV1 (Ocarina of Time's title screen played black).
// Output buffers are copied into FFmpeg frames, so Render() treats them like any other picture.
class PlatformAv1Decoder {
public:
    PlatformAv1Decoder(u32 width, u32 height) {
        codec = AMediaCodec_createDecoderByType("video/av01");
        if (codec == nullptr) {
            return;
        }
        AMediaFormat* format = AMediaFormat_new();
        AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "video/av01");
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH,
                              static_cast<int32_t>(width != 0 ? width : 1920));
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT,
                              static_cast<int32_t>(height != 0 ? height : 1080));
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, ColorFlexible);
        const bool configured =
            AMediaCodec_configure(codec, format, nullptr, nullptr, 0) == AMEDIA_OK;
        AMediaFormat_delete(format);
        if (!configured || AMediaCodec_start(codec) != AMEDIA_OK) {
            AMediaCodec_delete(codec);
            codec = nullptr;
            return;
        }
        if (char* codec_name{}; AMediaCodec_getName(codec, &codec_name) == AMEDIA_OK) {
            name = codec_name;
            AMediaCodec_releaseName(codec, codec_name);
        }
    }

    ~PlatformAv1Decoder() {
        if (codec != nullptr) {
            AMediaCodec_stop(codec);
            AMediaCodec_delete(codec);
        }
    }

    PlatformAv1Decoder(const PlatformAv1Decoder&) = delete;
    PlatformAv1Decoder& operator=(const PlatformAv1Decoder&) = delete;

    bool IsOpen() const {
        return codec != nullptr;
    }

    const std::string& CodecName() const {
        return name;
    }

    void Decode(std::span<const u8> data, u64 pts, std::vector<Nx2Picture>& pictures) {
        // An input buffer frees up as the decoder consumes one: take its output meanwhile.
        for (int attempt = 0; attempt < 100; ++attempt) {
            const ssize_t index = AMediaCodec_dequeueInputBuffer(codec, 10'000);
            if (index < 0) {
                Receive(pictures, 0);
                continue;
            }
            size_t capacity{};
            u8* const buffer =
                AMediaCodec_getInputBuffer(codec, static_cast<size_t>(index), &capacity);
            if (buffer == nullptr || capacity < data.size()) {
                LOG_ERROR(HW_GPU,
                          "nx2 video: AV1 frame of {} bytes does not fit the decoder's {} byte "
                          "input buffer, dropped",
                          data.size(), capacity);
                AMediaCodec_queueInputBuffer(codec, static_cast<size_t>(index), 0, 0, 0, 0);
            } else {
                std::memcpy(buffer, data.data(), data.size());
                AMediaCodec_queueInputBuffer(codec, static_cast<size_t>(index), 0, data.size(),
                                             static_cast<int64_t>(pts), 0);
                queued = true;
            }
            Receive(pictures, 0);
            return;
        }
        LOG_ERROR(HW_GPU, "nx2 video: the AV1 decoder took no input for 1 s, frame dropped");
    }

    void Drain(std::vector<Nx2Picture>& pictures) {
        if (queued) {
            const ssize_t index = AMediaCodec_dequeueInputBuffer(codec, 1'000'000);
            if (index >= 0) {
                AMediaCodec_queueInputBuffer(codec, static_cast<size_t>(index), 0, 0, 0,
                                             AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                // Until the decoder says it has no more, at most 3 s.
                for (int attempt = 0; attempt < 30 && !Receive(pictures, 100'000); ++attempt) {
                }
            }
        }
        Reset();
    }

    void Reset() {
        // Also what lets the decoder take input again after an end of stream.
        AMediaCodec_flush(codec);
        queued = false;
    }

private:
    // MediaCodecInfo.CodecCapabilities colour formats.
    static constexpr int32_t ColorPlanar = 19;                    // I420
    static constexpr int32_t ColorSemiPlanar = 21;                // NV12
    static constexpr int32_t ColorP010 = 54;                      // 10 bit in 16, NV12 layout
    static constexpr int32_t ColorFlexible = 0x7F420888;          // any 8 bit 4:2:0, as a request
    static constexpr int32_t ColorQcomSemiPlanar32m = 0x7FA30C04; // NV12 with padded stride

    /// Takes the output that is ready (waiting up to timeout_us for the first one). Returns
    /// whether the decoder reached the end of the stream.
    bool Receive(std::vector<Nx2Picture>& pictures, int64_t timeout_us) {
        while (true) {
            AMediaCodecBufferInfo info{};
            const ssize_t index = AMediaCodec_dequeueOutputBuffer(codec, &info, timeout_us);
            if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED ||
                index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
                continue;
            }
            if (index < 0) {
                return false;
            }
            timeout_us = 0;
            if (info.size > 0) {
                size_t capacity{};
                const u8* const buffer =
                    AMediaCodec_getOutputBuffer(codec, static_cast<size_t>(index), &capacity);
                AMediaFormat* const format = AMediaCodec_getOutputFormat(codec);
                if (buffer != nullptr && format != nullptr && info.offset >= 0 &&
                    static_cast<size_t>(info.offset) + static_cast<size_t>(info.size) <= capacity) {
                    if (auto frame = MakeFrame(
                            std::span<const u8>{buffer + info.offset,
                                                static_cast<size_t>(info.size)},
                            format)) {
                        const AVFrame* const f = frame->GetFrame();
                        pictures.push_back({static_cast<u32>(f->width),
                                            static_cast<u32>(f->height),
                                            static_cast<u64>(info.presentationTimeUs),
                                            std::move(frame)});
                    }
                }
                if (format != nullptr) {
                    AMediaFormat_delete(format);
                }
            }
            AMediaCodec_releaseOutputBuffer(codec, static_cast<size_t>(index), false);
            if ((info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0) {
                return true;
            }
        }
    }

    /// Copies a decoder output buffer (layout described by the output format) into an FFmpeg
    /// frame.
    std::shared_ptr<FFmpeg::Frame> MakeFrame(std::span<const u8> data, AMediaFormat* format) {
        int32_t width{}, height{}, color{};
        if (!AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_WIDTH, &width) ||
            !AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_HEIGHT, &height) ||
            !AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, &color) || width <= 0 ||
            height <= 0) {
            return Unsupported("no size or colour format", 0);
        }
        int32_t stride{width}, slice_height{height};
        AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_STRIDE, &stride);
        AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_SLICE_HEIGHT, &slice_height);
        if (stride <= 0) {
            stride = width;
        }
        if (slice_height <= 0) {
            slice_height = height;
        }
        // The visible part of the decoded picture (right and bottom inclusive).
        int32_t left{}, top{}, right{width - 1}, bottom{height - 1};
        if (int32_t l{}, t{}, r{}, b{};
            AMediaFormat_getRect(format, AMEDIAFORMAT_KEY_DISPLAY_CROP, &l, &t, &r, &b) &&
            l >= 0 && t >= 0 && r >= l && b >= t && r < width && b < height) {
            left = l & ~1;
            top = t & ~1;
            right = r;
            bottom = b;
        }
        const int out_width = right - left + 1;
        const int out_height = bottom - top + 1;
        const int chroma_height = (out_height + 1) / 2;

        AVPixelFormat pixel_format{};
        size_t sample{1}; // bytes per sample
        bool semi_planar{true};
        switch (color) {
        case ColorPlanar:
            pixel_format = AV_PIX_FMT_YUV420P;
            semi_planar = false;
            break;
        case ColorSemiPlanar:
        case ColorQcomSemiPlanar32m:
            pixel_format = AV_PIX_FMT_NV12;
            break;
        case ColorP010:
            pixel_format = AV_PIX_FMT_P010LE;
            sample = 2;
            break;
        default:
            return Unsupported("colour format", color);
        }
        // Planar: two planes of half-width rows; semi-planar: one plane of interleaved rows.
        const size_t luma_stride = static_cast<size_t>(stride);
        const size_t chroma_stride = semi_planar ? luma_stride : (luma_stride + 1) / 2;
        const size_t chroma_slice = (static_cast<size_t>(slice_height) + 1) / 2;
        const size_t planes = semi_planar ? 1 : 2;
        const size_t luma_row = static_cast<size_t>(out_width) * sample;
        const size_t chroma_row =
            static_cast<size_t>((out_width + 1) / 2) * (semi_planar ? 2 * sample : 1);
        const size_t luma_start =
            static_cast<size_t>(top) * luma_stride + static_cast<size_t>(left) * sample;
        const size_t chroma_base = luma_stride * static_cast<size_t>(slice_height);
        const size_t chroma_start =
            static_cast<size_t>(top / 2) * chroma_stride +
            (semi_planar ? static_cast<size_t>(left) * sample : static_cast<size_t>(left / 2));
        const size_t end = chroma_base + (planes - 1) * chroma_stride * chroma_slice +
                           chroma_start + static_cast<size_t>(chroma_height - 1) * chroma_stride +
                           chroma_row;
        if (luma_row > luma_stride || chroma_row > chroma_stride || end > data.size()) {
            return Unsupported("buffer layout", color);
        }

        auto frame = std::make_shared<FFmpeg::Frame>();
        AVFrame* const f = frame->GetFrame();
        f->format = pixel_format;
        f->width = out_width;
        f->height = out_height;
        if (av_frame_get_buffer(f, 0) < 0) {
            return nullptr;
        }
        for (int y = 0; y < out_height; ++y) {
            std::memcpy(f->data[0] + static_cast<size_t>(y) * static_cast<size_t>(f->linesize[0]),
                        data.data() + luma_start + static_cast<size_t>(y) * luma_stride, luma_row);
        }
        for (size_t plane = 0; plane < planes; ++plane) {
            const u8* const source =
                data.data() + chroma_base + plane * chroma_stride * chroma_slice + chroma_start;
            for (int y = 0; y < chroma_height; ++y) {
                std::memcpy(f->data[plane + 1] +
                                static_cast<size_t>(y) * static_cast<size_t>(f->linesize[plane + 1]),
                            source + static_cast<size_t>(y) * chroma_stride, chroma_row);
            }
        }
        // MediaFormat.COLOR_STANDARD_* and COLOR_RANGE_*; left unspecified when not given.
        if (int32_t standard{};
            AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_COLOR_STANDARD, &standard)) {
            f->colorspace = standard == 1                    ? AVCOL_SPC_BT709
                            : standard == 6                  ? AVCOL_SPC_BT2020_NCL
                            : standard == 2 || standard == 4 ? AVCOL_SPC_BT470BG
                                                             : AVCOL_SPC_UNSPECIFIED;
        }
        if (int32_t range{}; AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_COLOR_RANGE, &range)) {
            f->color_range = range == 1   ? AVCOL_RANGE_JPEG
                             : range == 2 ? AVCOL_RANGE_MPEG
                                          : AVCOL_RANGE_UNSPECIFIED;
        }
        if (!logged_output) {
            logged_output = true;
            LOG_INFO(HW_GPU,
                     "nx2 video: AV1 output {}x{} (buffer {}x{}, stride {}, slice height {}, "
                     "colour format {:#x})",
                     out_width, out_height, width, height, stride, slice_height, color);
        }
        return frame;
    }

    std::shared_ptr<FFmpeg::Frame> Unsupported(const char* what, int32_t color) {
        if (!logged_unsupported) {
            logged_unsupported = true;
            LOG_ERROR(HW_GPU, "nx2 video: AV1 decoder output not usable ({}, colour format {:#x})",
                      what, color);
        }
        return nullptr;
    }

    AMediaCodec* codec{};
    std::string name;
    bool queued{}; ///< Input sent since the last flush
    bool logged_output{};
    bool logged_unsupported{};
};
#endif

} // namespace

bool IsH264Stream(std::span<const u8> data) {
    if (data.size() >= 4) {
        // Annex-B 3-byte start code: 00 00 01 <nal_header>
        if (data[0] == 0 && data[1] == 0 && data[2] == 1) {
            const u8 nal_type = data[3] & 0x1F;
            if ((data[3] & 0x80) == 0 && nal_type >= 1 && nal_type <= 23) {
                return true;
            }
        }
    }
    if (data.size() >= 5) {
        // Annex-B 4-byte start code: 00 00 00 01 <nal_header>
        if (data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) {
            const u8 nal_type = data[4] & 0x1F;
            if ((data[4] & 0x80) == 0 && nal_type >= 1 && nal_type <= 23) {
                return true;
            }
        }
        // avcC / MP4 length-prefixed NAL unit: [u32 be length] <nal_header>
        const u32 nalu_len = (static_cast<u32>(data[0]) << 24) |
                             (static_cast<u32>(data[1]) << 16) |
                             (static_cast<u32>(data[2]) << 8) |
                             static_cast<u32>(data[3]);
        if (nalu_len > 0 && nalu_len <= data.size() - 4) {
            const u8 nal_type = data[4] & 0x1F;
            if ((data[4] & 0x80) == 0 && nal_type >= 1 && nal_type <= 23) {
                return true;
            }
        }
    }
    if (data.size() >= 7) {
        // avcC configuration record: version=1, profile, compat, level, lengthSizeMinusOne | 0xFC, numSPS | 0xE0
        if (data[0] == 1 && (data[4] & 0xFC) == 0xFC && (data[5] & 0xE0) == 0xE0 && (data[5] & 0x1F) > 0) {
            return true;
        }
    }
    return false;
}

struct Nx2VideoDecoder::Impl {
    std::string name;
    std::optional<FFmpeg::Decoder> decoder;
    std::optional<FFmpeg::HardwareContext> hardware;
    std::optional<FFmpeg::DecoderContext> context;
    SwsContext* scaler{};
    bool open{};
    H264PacketConverter h264_packets;
    /// Timestamps of submitted frames not yet output. Frames copied back from a host GPU decoder
    /// lose theirs and take the earliest one instead.
    std::multiset<u64> pending_pts;
    std::vector<u8> packet; ///< FFmpeg reads up to AV_INPUT_BUFFER_PADDING_SIZE past the data
#ifdef __ANDROID__
    std::optional<PlatformAv1Decoder> platform; ///< AV1, when FFmpeg has no decoder for it
#endif

    ~Impl() {
        sws_freeContext(scaler);
        context.reset();
        hardware.reset();
    }

    void Receive(std::vector<Nx2Picture>& pictures) {
        while (auto frame = context->ReceiveFrame()) {
            const AVFrame* f = frame->GetFrame();
            const s64 stamp = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp
                                                                         : f->pts;
            u64 pts{};
            if (stamp != AV_NOPTS_VALUE) {
                pts = static_cast<u64>(stamp);
                if (const auto it = pending_pts.find(pts); it != pending_pts.end()) {
                    pending_pts.erase(it);
                }
            } else if (!pending_pts.empty()) {
                pts = *pending_pts.begin();
                pending_pts.erase(pending_pts.begin());
            }
            pictures.push_back({static_cast<u32>(f->width), static_cast<u32>(f->height), pts,
                                std::move(frame)});
        }
    }
};

Nx2VideoDecoder::Nx2VideoDecoder(std::string_view mime, [[maybe_unused]] u32 width,
                                 [[maybe_unused]] u32 height)
    : impl{std::make_unique<Impl>()} {
    impl->name = std::string{mime};
    const auto codec = CodecFromMime(mime);
    if (!codec) {
        LOG_ERROR(HW_GPU, "nx2 video: no decoder for codec \"{}\"", mime);
        return;
    }
    impl->decoder.emplace(*codec);
    if (impl->decoder->GetCodec() == nullptr) {
#ifdef __ANDROID__
        if (*codec == NvdecCommon::VideoCodec::AV1) {
            impl->platform.emplace(width, height);
            impl->open = impl->platform->IsOpen();
            if (impl->open) {
                LOG_INFO(HW_GPU, "nx2 video: decoder for \"{}\" open (MediaCodec {}, {}x{})", mime,
                         impl->platform->CodecName(), width, height);
            } else {
                impl->platform.reset();
                LOG_ERROR(HW_GPU,
                          "nx2 video: no FFmpeg or platform (MediaCodec) decoder for \"{}\"",
                          mime);
            }
            return;
        }
#endif
        LOG_ERROR(HW_GPU, "nx2 video: FFmpeg has no decoder for \"{}\"", mime);
        return;
    }
    impl->context.emplace(*impl->decoder);
    if (Settings::values.nvdec_emulation.GetValue() == Settings::NvdecEmulation::Gpu) {
        impl->hardware.emplace();
        impl->hardware->InitializeForDecoder(*impl->context, *impl->decoder);
    }
    impl->open = impl->context->OpenContext(*impl->decoder);
    LOG_INFO(HW_GPU, "nx2 video: decoder for \"{}\" {}", mime, impl->open ? "open" : "failed to open");
}

Nx2VideoDecoder::~Nx2VideoDecoder() = default;

bool Nx2VideoDecoder::IsOpen() const {
    return impl->open;
}

const std::string& Nx2VideoDecoder::Name() const {
    return impl->name;
}

void Nx2VideoDecoder::Decode(std::span<const u8> data, u64 pts, std::vector<Nx2Picture>& pictures) {
    if (!impl->open || data.empty()) {
        return;
    }
#ifdef __ANDROID__
    if (impl->platform) {
        impl->platform->Decode(data, pts, pictures);
        return;
    }
#endif
    impl->packet.assign(data.size() + AV_INPUT_BUFFER_PADDING_SIZE, 0);
    std::memcpy(impl->packet.data(), data.data(), data.size());

    // Convert MP4/avcC length-prefixed NALUs to Annex-B start codes for FFmpeg if needed.
    if (impl->name.find("avc") != std::string::npos || impl->name.find("264") != std::string::npos) {
        impl->h264_packets.Convert(std::span<u8>{impl->packet.data(), data.size()});
    }

    FFmpeg::Packet packet{std::span<const u8>{impl->packet.data(), data.size()}};
    packet.GetPacket()->pts = static_cast<s64>(pts);
    // Hidden VP9 frames never come out: keep the set from growing over a long movie.
    if (impl->pending_pts.size() >= 64) {
        impl->pending_pts.erase(impl->pending_pts.begin());
    }
    impl->pending_pts.insert(pts);
    impl->context->SendPacket(packet);
    impl->Receive(pictures);
}

void Nx2VideoDecoder::Drain(std::vector<Nx2Picture>& pictures) {
    if (!impl->open) {
        return;
    }
#ifdef __ANDROID__
    if (impl->platform) {
        impl->platform->Drain(pictures);
        return;
    }
#endif
    avcodec_send_packet(impl->context->GetCodecContext(), nullptr);
    impl->Receive(pictures);
    Reset();
}

void Nx2VideoDecoder::Reset() {
#ifdef __ANDROID__
    if (impl->platform) {
        impl->platform->Reset();
        return;
    }
#endif
    if (impl->open) {
        avcodec_flush_buffers(impl->context->GetCodecContext());
    }
    impl->pending_pts.clear();
}

void Nx2VideoDecoder::Render(const Nx2Picture& picture, Nx2PictureFormat format, u32 width,
                             u32 height, u64 pitch, u64 chroma_offset, std::span<u8> out) {
    const AVFrame* f = picture.frame->GetFrame();
    const u64 needed = format == Nx2PictureFormat::Nv12
                           ? chroma_offset + pitch * ((u64{height} + 1) / 2)
                           : pitch * height;
    if (width == 0 || height == 0 || out.size() < needed) {
        return;
    }
    const AVPixelFormat target = format == Nx2PictureFormat::Nv12    ? AV_PIX_FMT_NV12
                                 : format == Nx2PictureFormat::Rgba8 ? AV_PIX_FMT_RGBA
                                                                     : AV_PIX_FMT_BGRA;
    // Frames copied back from a host GPU decoder are already NV12. Copy them as they are:
    // FFmpeg 6.1's swscale leaves the chroma plane of an NV12 to NV12 copy at zero, which shows
    // the movies green.
    if (target == AV_PIX_FMT_NV12 && f->format == AV_PIX_FMT_NV12 &&
        static_cast<u32>(f->width) == width && static_cast<u32>(f->height) == height) {
        for (u32 y = 0; y < height; ++y) {
            std::memcpy(out.data() + y * pitch, f->data[0] + y * f->linesize[0], width);
        }
        for (u32 y = 0; y < (height + 1) / 2; ++y) {
            std::memcpy(out.data() + chroma_offset + y * pitch, f->data[1] + y * f->linesize[1],
                        width);
        }
        return;
    }
    impl->scaler = sws_getCachedContext(impl->scaler, f->width, f->height,
                                        static_cast<AVPixelFormat>(f->format),
                                        static_cast<int>(width), static_cast<int>(height), target,
                                        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (impl->scaler == nullptr) {
        LOG_ERROR(HW_GPU, "nx2 video: cannot convert {} to {}",
                  av_get_pix_fmt_name(static_cast<AVPixelFormat>(f->format)),
                  av_get_pix_fmt_name(target));
        return;
    }
    if (format != Nx2PictureFormat::Nv12) {
        // The stream's matrix and range; untagged HD video is BT.709.
        int space = SWS_CS_ITU601;
        if (f->colorspace == AVCOL_SPC_BT709 ||
            (f->colorspace == AVCOL_SPC_UNSPECIFIED && f->height >= 720)) {
            space = SWS_CS_ITU709;
        } else if (f->colorspace == AVCOL_SPC_BT2020_NCL || f->colorspace == AVCOL_SPC_BT2020_CL) {
            space = SWS_CS_BT2020;
        }
        const int* coefficients = sws_getCoefficients(space);
        sws_setColorspaceDetails(impl->scaler, coefficients, f->color_range == AVCOL_RANGE_JPEG,
                                 coefficients, 1, 0, 1 << 16, 1 << 16);
    }
    std::array<u8*, 4> planes{out.data(), nullptr, nullptr, nullptr};
    std::array<int, 4> strides{static_cast<int>(pitch), 0, 0, 0};
    if (format == Nx2PictureFormat::Nv12) {
        planes[1] = out.data() + chroma_offset;
        strides[1] = static_cast<int>(pitch);
    }
    sws_scale(impl->scaler, f->data, f->linesize, 0, f->height, planes.data(), strides.data());
}

} // namespace Tegra::Host1x
