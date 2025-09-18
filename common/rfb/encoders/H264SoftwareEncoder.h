#pragma once

#include "rdr/OutStream.h"
#include "rfb/Encoder.h"
#include "rfb/encoders/VideoEncoder.h"
#include "rfb/ffmpeg.h"

namespace rfb {
    class H264SoftwareEncoder final : public Encoder, public VideoEncoder {
        uint32_t id;
        const FFmpeg &ffmpeg;
        const AVCodec *codec{};

        FFmpeg::FrameGuard frame_guard;
        FFmpeg::PacketGuard pkt_guard;
        FFmpeg::ContextGuard ctx_guard;
        FFmpeg::SwsContextGuard sws_guard;

        uint8_t frame_rate{};
        uint16_t bit_rate{};
        int bpp{};
        static void write_compact(rdr::OutStream *os, int value);
        [[nodiscard]] bool init(int src_width, int src_height, int dst_width, int dst_height);

    public:
        H264SoftwareEncoder(uint32_t id, const FFmpeg &ffmpeg, SConnection *conn, uint8_t frame_rate, uint16_t bit_rate);
        bool isSupported() override;
        void writeRect(const PixelBuffer *pb, const Palette &palette) override;
        void writeSolidRect(int width, int height, const PixelFormat &pf, const rdr::U8 *colour) override;
        Encoder *clone(uint32_t id) override;
        void writeSkipRect() override;
    };
} // namespace rfb
