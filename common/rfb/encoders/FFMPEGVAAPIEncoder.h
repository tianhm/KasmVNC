#pragma once

#include "KasmVideoConstants.h"
#include "rdr/OutStream.h"
#include "rfb/Encoder.h"
#include "rfb/encoders/VideoEncoder.h"
#include "rfb/ffmpeg.h"

namespace rfb {
class FFMPEGVAAPIEncoder final : public VideoEncoder {
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
    uint8_t msg_codec_id;

    int64_t pts{};
    int bpp{};
    const char *dri_node{};

    static void write_compact(rdr::OutStream *os, int value);
    [[nodiscard]] bool init(int width, int height, VideoEncoderParams params);

    template<typename T>
    friend class EncoderBuilder;
    FFMPEGVAAPIEncoder(Screen layout, const FFmpeg &ffmpeg, SConnection *conn, KasmVideoEncoders::Encoder encoder,
        const char *dri_node, VideoEncoderParams params);

public:
    bool isSupported() const override;
    void writeRect(const PixelBuffer *pb, const Palette &palette) override;
    void writeSolidRect(int width, int height, const PixelFormat &pf, const rdr::U8 *colour) override;
    bool render(const PixelBuffer *pb) override;
    void writeSkipRect() override;
};
} // namespace rfb
