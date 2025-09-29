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
    H264SoftwareEncoder::H264SoftwareEncoder(Screen layout_, const FFmpeg &ffmpeg_, SConnection *conn, KasmVideoEncoders::Encoder encoder_,
                                             VideoEncoderParams params) :
        Encoder(conn, encodingKasmVideo, static_cast<EncoderFlags>(EncoderUseNativePF | EncoderLossy), -1), layout(layout_),
        ffmpeg(ffmpeg_), encoder(encoder_), current_params(params) {
        codec = ffmpeg.avcodec_find_encoder(AV_CODEC_ID_H264);
        if (!codec)
            throw std::runtime_error("Could not find H264 encoder");

        auto *frame = ffmpeg.av_frame_alloc();
        if (!frame) {
            throw std::runtime_error("Cannot allocate AVFrame");
        }
        frame_guard.reset(frame);

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

        const auto rect = layout.dimensions;
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

        VideoEncoderParams params{dst_width,
                                  dst_height,
                                  static_cast<uint8_t>(Server::frameRate),
                                  static_cast<uint8_t>(Server::groupOfPicture),
                                  static_cast<uint8_t>(Server::videoQualityCRFCQP)};

        if (current_params != params) {
            bpp = pb->getPF().bpp >> 3;
            if (!init(width, height, params)) {
                vlog.error("Failed to initialize encoder");
                return;
            }

            frame = frame_guard.get();
        } else {
            frame->pict_type = AV_PICTURE_TYPE_NONE;
        }

        const uint8_t *src_data[1] = {buffer};
        const int src_line_size[1] = {stride * bpp}; // RGB has bpp bytes per pixel

        if (ffmpeg.sws_scale(sws_guard.get(), src_data, src_line_size, 0, height, frame->data, frame->linesize) < 0) {
            vlog.error("Error while scaling image");
            return;
        }

        frame->pts = pts++;

        int err = ffmpeg.avcodec_send_frame(ctx_guard.get(), frame);
        if (err < 0) {
            vlog.error("Error sending frame to codec (%s). Error code: %d", ffmpeg.get_error_description(err).c_str(), err);
            return;
        }

        auto *pkt = pkt_guard.get();

        err = ffmpeg.avcodec_receive_packet(ctx_guard.get(), pkt);
        if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) {
            // Trying one more time
            err = ffmpeg.avcodec_send_frame(ctx_guard.get(), nullptr);
            err = ffmpeg.avcodec_receive_packet(ctx_guard.get(), pkt);
        }

        if (err < 0) {
            vlog.error("Error receiving packet from codec");
            writeSkipRect();
            return;
        }

        if (pkt->flags & AV_PKT_FLAG_KEY)
            vlog.debug("Key frame %ld", frame->pts);

        auto *os = conn->getOutStream(conn->cp.supportsUdp);
        os->writeU8(kasmVideoH264 << 4);
        os->writeU8(pkt->flags & AV_PKT_FLAG_KEY);
        write_compact(os, pkt->size);
        os->writeBytes(&pkt->data[0], pkt->size);

        ffmpeg.av_packet_unref(pkt);
    }

    void H264SoftwareEncoder::writeSolidRect(int width, int height, const PixelFormat &pf, const rdr::U8 *colour) {}

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

    bool H264SoftwareEncoder::init(int width, int height, VideoEncoderParams params) {
        current_params = params;
        printf("FRAME RESIZE!!!!!!!!!!!!!!!!!!\n");

        auto *ctx = ffmpeg.avcodec_alloc_context3(codec);
        if (!ctx)
            return false;

        ctx_guard.reset(ctx);

        ctx->time_base = {1, params.frame_rate};
        ctx->framerate = {params.frame_rate, 1};
        ctx->gop_size = params.group_of_picture; // interval between I-frames
        //  best
        // ctx->pix_fmt = AV_PIX_FMT_YUV444P; // AV_PIX_FMT_YUV420P;
        ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        ctx->max_b_frames = 0; // No B-frames for immediate output

        // HIGH
        // if (ffmpeg.av_opt_set(ctx->priv_data, "tune", "zerolatency,stillimage", 0) != 0)
        //     return false;
        //
        // // start here, lower (20–22) = better quality,
        // // higher (24–28) = lower bitrate
        // if (ffmpeg.av_opt_set(ctx->priv_data, "crf", "18", 0) != 0)
        //     return false;
        //
        // // Preset: speed vs. compression efficiency
        // if (ffmpeg.av_opt_set(ctx->priv_data, "preset", "medium", 0) != 0)
        //     return false;

        if (ffmpeg.av_opt_set(ctx->priv_data, "async_depth", "1", 0) < 0) {
            vlog.info("Cannot set async_depth");
        }

        if (ffmpeg.av_opt_set(ctx->priv_data, "tune", "zerolatency", 0) < 0) {
            vlog.info("Cannot set tune to zerolatency");
        }

        if (ffmpeg.av_opt_set(ctx->priv_data, "preset", "ultrafast", 0) < 0) {
            vlog.info("Cannot set preset to ultrafast");
        }

        // start here, lower (20–22) = better quality,
        // higher (24–28) = lower bitrate
        if (ffmpeg.av_opt_set_int(ctx->priv_data, "crf", current_params.quality, 0) < 0) {
            vlog.info("Cannot set crf to %d", current_params.quality);
        }


        // // Preset: speed vs. compression efficiency
        // if (ffmpeg.av_opt_set(ctx->priv_data, "preset", "medium", 0) != 0)
        //     return false;

        /*if (ffmpeg.av_opt_set(ctx->priv_data, "preset", "ultrafast", 0) != 0)
            throw std::runtime_error("Could not set codec setting");*/
        // "ultrafast" = lowest latency but bigger bitrate
        // "veryfast" = good balance for realtime
        // "medium+" = too slow for live

        // H.264 profile for better compression
        // if (ffmpeg.av_opt_set(ctx->priv_data, "profile", "high", 0) != 0)
        //     throw std::runtime_error("Could not set codec setting");

        ctx_guard->width = current_params.width;
        ctx_guard->height = current_params.height;
        ctx_guard->coded_width = current_params.width;
        ctx_guard->coded_height = current_params.height;

        auto *sws_ctx = ffmpeg.sws_getContext(width,
                                              height,
                                              AV_PIX_FMT_RGB32,
                                              current_params.width,
                                              current_params.height,
                                              ctx_guard->pix_fmt,
                                              SWS_BILINEAR,
                                              nullptr,
                                              nullptr,
                                              nullptr);

        sws_guard.reset(sws_ctx);

        auto *frame = frame_guard.get();

        ffmpeg.av_frame_unref(frame);
        frame->format = ctx_guard->pix_fmt;
        frame->width = current_params.width;
        frame->height = current_params.height;
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
