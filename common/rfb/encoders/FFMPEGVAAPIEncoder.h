#pragma once

#include "KasmVideoConstants.h"
#include "rdr/OutStream.h"
#include "rfb/Encoder.h"
#include "rfb/encoders/VideoEncoder.h"
#include "rfb/ffmpeg.h"

namespace rfb {
class FFMPEGVAAPIEncoder final : public Encoder, public VideoEncoder {
    Screen layout;
    const FFmpeg &ffmpeg;

    FFmpeg::FrameGuard sw_frame_guard;
    FFmpeg::FrameGuard hw_frame_guard;
    FFmpeg::PacketGuard pkt_guard;
    FFmpeg::ContextGuard ctx_guard;
    FFmpeg::SwsContextGuard sws_guard;
    FFmpeg::BufferGuard hw_device_ctx_guard;
    FFmpeg::BufferGuard hw_frames_ref_guard;

    const AVCodec *codec{};

    KasmVideoEncoders::Encoder encoder;
    VideoEncoderParams current_params{};

    int64_t pts{};
    int bpp{};

    static void write_compact(rdr::OutStream *os, int value);
    [[nodiscard]] bool init(int width, int height, VideoEncoderParams params);

    template<typename T>
    friend class EncoderBuilder;
    FFMPEGVAAPIEncoder(Screen layout, const FFmpeg &ffmpeg, SConnection *conn, KasmVideoEncoders::Encoder encoder,
                           VideoEncoderParams params);

public:
    bool isSupported() override;
    void writeRect(const PixelBuffer *pb, const Palette &palette) override;
    void writeSolidRect(int width, int height, const PixelFormat &pf, const rdr::U8 *colour) override;
    void writeSkipRect() override;
};
} // namespace rfb
