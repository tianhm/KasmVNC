#include "H264SoftwareEncoder.h"
extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}
#include "KasmVideoConstants.h"
#include "rfb/LogWriter.h"
#include "rfb/SConnection.h"
#include "rfb/ServerCore.h"
#include "rfb/encodings.h"
#include "rfb/ffmpeg.h"

static rfb::LogWriter vlog("H264SoftwareEncoder");

namespace rfb {
    H264SoftwareEncoder::H264SoftwareEncoder(uint32_t id, const FFmpeg &ffmpeg_, SConnection *conn, uint8_t frame_rate_, uint16_t bit_rate_) :
        Encoder(conn, encodingKasmVideo, static_cast<EncoderFlags>(EncoderUseNativePF | EncoderLossy), -1), VideoEncoder(id), ffmpeg(ffmpeg_),
        frame_rate(frame_rate_), bit_rate(bit_rate_) {
        codec = ffmpeg.avcodec_find_encoder(AV_CODEC_ID_H264);
        if (!codec)
            throw std::runtime_error("Could not find H264 encoder");

        auto *ctx = ffmpeg.avcodec_alloc_context3(codec);
        if (!ctx) {
            throw std::runtime_error("Cannot allocate AVCodecContext");
        }
        ctx_guard.reset(ctx);

        auto *frame = ffmpeg.av_frame_alloc();
        if (!frame) {
            throw std::runtime_error("Cannot allocate AVFrame");
        }
        frame->pts = 0;
        frame_guard.reset(frame);

        ctx->time_base = {1, frame_rate};
        ctx->framerate = {frame_rate, 1};
        ctx->gop_size = GroupOfPictureSize; // interval between I-frames
        ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        ctx->max_b_frames = 0; // No B-frames for immediate output

        if (ffmpeg.av_opt_set(ctx->priv_data, "tune", "zerolatency", 0) != 0)
            throw std::runtime_error("Could not set codec setting");
        if (ffmpeg.av_opt_set(ctx->priv_data, "preset", "ultrafast", 0) != 0)
            throw std::runtime_error("Could not set codec setting");

        auto *pkt = ffmpeg.av_packet_alloc();
        if (!pkt)
            throw std::runtime_error("Could not allocate packet");

        pkt_guard.reset(pkt);
    }

    bool H264SoftwareEncoder::isSupported() {
        return conn->cp.supportsEncoding(encodingKasmVideo);
    }

    void H264SoftwareEncoder::writeRect(const PixelBuffer *pb, const Palette &palette) {
        // compress
        int stride;
        const auto rect = pb->getRect();
        const auto *buffer = pb->getBuffer(rect, &stride);

        const int width = rect.width();
        const int height = rect.height();
        auto *frame = frame_guard.get();

        int dst_width = width;
        int dst_height = height;

        if (width % 2 != 0)
            dst_width = width & ~1;

        if (height % 2 != 0)
            dst_height = height & ~1;

        if (frame->width != dst_width || frame->height != dst_height) {
            bpp = pb->getPF().bpp >> 3;
            if (!init(width, height, dst_width, dst_height)) {
                vlog.error("Failed to initialize encoder");
                return;
            }
        } else {
            frame->pict_type = AV_PICTURE_TYPE_NONE;
        }

        const uint8_t *src_data[1] = {buffer};
        const int src_line_size[1] = {stride * bpp}; // RGB has bpp bytes per pixel

        if (ffmpeg.sws_scale(sws_guard.get(), src_data, src_line_size, 0, height, frame->data, frame->linesize) < 0) {
            vlog.error("Error while scaling image");
            return;
        }

        int ret = ffmpeg.avcodec_send_frame(ctx_guard.get(), frame);
        if (ret < 0) {
            vlog.error("Error sending frame to codec");
            return;
        }

        auto *pkt = pkt_guard.get();

        ret = ffmpeg.avcodec_receive_packet(ctx_guard.get(), pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            // Trying one more time
            ret = ffmpeg.avcodec_receive_packet(ctx_guard.get(), pkt);
        }

        if (ret < 0) {
            vlog.error("Error receiving packet from codec");
            return;
        }

        if (pkt->flags & AV_PKT_FLAG_KEY)
            vlog.debug("Key frame %ld", frame->pts);

        auto *os = conn->getOutStream(conn->cp.supportsUdp);
        os->writeU8(kasmVideoH264 << 4);
        os->writeU8(pkt->flags & AV_PKT_FLAG_KEY);
        write_compact(os, pkt->size);
        os->writeBytes(&pkt->data[0], pkt->size);

        ++frame->pts;
        ffmpeg.av_packet_unref(pkt);
    }

    void H264SoftwareEncoder::writeSolidRect(int width, int height, const PixelFormat &pf, const rdr::U8 *colour) {}

    Encoder *H264SoftwareEncoder::clone(uint32_t id) {
        return new H264SoftwareEncoder(id, ffmpeg, conn, frame_rate, bit_rate);
    }

    void H264SoftwareEncoder::writeSkipRect() {
        auto *os = conn->getOutStream(conn->cp.supportsUdp);
        os->writeU8(kasmVideoSkip << 4);
    }

    void H264SoftwareEncoder::write_compact(rdr::OutStream *os, int value) {
        auto b = value & 0x7F;
        if (value <= 0x7F) {
            os->writeU8(b);
        } else {
            os->writeU8(b | 0x80);
            b = value >> 7 & 0x7F;
            if (value <= 0x3FFF) {
                os->writeU8(b);
            } else {
                os->writeU8(b | 0x80);
                os->writeU8(value >> 14 & 0xFF);
            }
        }
    }

    bool H264SoftwareEncoder::init(int src_width, int src_height, int dst_width, int dst_height) {
        auto *sws_ctx = ffmpeg.sws_getContext(src_width,
                                              src_height,
                                              AV_PIX_FMT_RGB32,
                                              dst_width,
                                              dst_height,
                                              AV_PIX_FMT_YUV420P,
                                              SWS_BILINEAR,
                                              nullptr,
                                              nullptr,
                                              nullptr);

        sws_guard.reset(sws_ctx);

        ctx_guard->width = dst_width;
        ctx_guard->height = dst_height;

        auto *frame = frame_guard.get();
        frame->format = ctx_guard->pix_fmt;
        frame->width = dst_width;
        frame->height = dst_height;
        frame->pict_type = AV_PICTURE_TYPE_I;

        if (ffmpeg.av_frame_get_buffer(frame, 0) < 0) {
            vlog.error("Could not allocate frame data");
            return false;
        }

        if (ffmpeg.avcodec_open2(ctx_guard.get(), codec, nullptr) < 0) {
            vlog.error("Failed to open codec");
            return false;
        }

        return true;
    }
} // namespace rfb
