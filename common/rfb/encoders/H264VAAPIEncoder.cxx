#include "H264VAAPIEncoder.h"

#include <fmt/format.h>
#include "rfb/LogWriter.h"

extern "C" {
#include <libavutil/opt.h>
}

#include "KasmVideoConstants.h"
#include "rfb/encodings.h"

static rfb::LogWriter vlog("H264VAAPIEncoder");

namespace rfb {
    H264VAAPIEncoder::H264VAAPIEncoder(const FFmpeg &ffmpeg_, SConnection *conn, uint8_t frame_rate_, uint16_t bit_rate_) :
        Encoder(conn, encodingKasmVideo, static_cast<EncoderFlags>(EncoderUseNativePF | EncoderLossy), -1), ffmpeg(ffmpeg_),
        frame_rate(frame_rate_), bit_rate(bit_rate_) {
        AVBufferRef *hw_device_ctx{};
        int err{};

        // TODO: change render_path to nullptr
        if (err = ffmpeg.av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, render_path, nullptr, 0); err < 0) {
            throw std::runtime_error(fmt::format("Failed to create VAAPI device context {}", ffmpeg.get_error_description(err)));
        }

        hw_device_ctx_guard.reset(hw_device_ctx);
        codec = ffmpeg.avcodec_find_encoder_by_name("h264_vaapi");
        if (!codec)
            throw std::runtime_error("Could not find h264_vaapi encoder");

        auto *ctx = ffmpeg.avcodec_alloc_context3(codec);
        if (!ctx)
            throw std::runtime_error("Cannot allocate AVCodecContext");
        ctx_guard.reset(ctx);

        auto *frame = ffmpeg.av_frame_alloc();
        if (!frame) {
            throw std::runtime_error("Cannot allocate AVFrame");
        }
        frame->pts = 0;
        sw_frame_guard.reset(frame);

        ctx->time_base = {1, frame_rate};
        ctx->framerate = {frame_rate, 1};
        ctx->gop_size = GroupOfPictureSize; // interval between I-frames
        ctx->max_b_frames = 0; // No B-frames for immediate output
        ctx->pix_fmt = AV_PIX_FMT_VAAPI;

        frame = ffmpeg.av_frame_alloc();
        if (!frame)
            throw std::runtime_error("Cannot allocate hw AVFrame");
        hw_frame_guard.reset(frame);

        auto *pkt = ffmpeg.av_packet_alloc();
        if (!pkt) {
            throw std::runtime_error("Could not allocate packet");
        }
        pkt_guard.reset(pkt);
    }

    void H264VAAPIEncoder::write_compact(rdr::OutStream *os, int value) {
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

    bool H264VAAPIEncoder::init(int width, int height, int dst_width, int dst_height) {
        AVHWFramesContext *frames_ctx{};
        int err{};

        ctx_guard->width = dst_width;
        ctx_guard->height = dst_height;

        auto *hw_frames_ctx = ffmpeg.av_hwframe_ctx_alloc(hw_device_ctx_guard.get());
        if (!hw_frames_ctx) {
            vlog.error("Failed to create VAAPI frame context");
            return false;
        }

        hw_frames_ref_guard.reset(hw_frames_ctx);

        frames_ctx = reinterpret_cast<AVHWFramesContext *>(hw_frames_ctx->data);
        frames_ctx->format = AV_PIX_FMT_VAAPI;
        frames_ctx->sw_format = AV_PIX_FMT_NV12;
        frames_ctx->width = dst_width;
        frames_ctx->height = dst_height;
        frames_ctx->initial_pool_size = 20;
        if (err = ffmpeg.av_hwframe_ctx_init(hw_frames_ctx); err < 0) {
            vlog.error("Failed to initialize VAAPI frame context (%s). Error code: %d", ffmpeg.get_error_description(err).c_str(), err);
            return false;
        }

        ctx_guard->hw_frames_ctx = ffmpeg.av_buffer_ref(hw_frames_ctx);
        if (!ctx_guard->hw_frames_ctx) {
            vlog.error("Failed to create buffer reference");
            return false;
        }


        if (err = ffmpeg.avcodec_open2(ctx_guard.get(), codec, nullptr); err < 0) {
            vlog.error("Failed to open codec (%s). Error code: %d", ffmpeg.get_error_description(err).c_str(), err);
            return false;
        }

        auto *sws_ctx = ffmpeg.sws_getContext(
                width, height, AV_PIX_FMT_RGB32, dst_width, dst_height, AV_PIX_FMT_NV12, SWS_BILINEAR, nullptr, nullptr, nullptr);

        sws_guard.reset(sws_ctx);

        auto *frame = sw_frame_guard.get();
        frame->format = AV_PIX_FMT_NV12;
        frame->width = dst_width;
        frame->height = dst_height;
        frame->pict_type = AV_PICTURE_TYPE_I;

        if (ffmpeg.av_frame_get_buffer(frame, 0) < 0) {
            vlog.error("Could not allocate sw-frame data");
            return false;
        }

        if (err = ffmpeg.av_hwframe_get_buffer(ctx_guard->hw_frames_ctx, hw_frame_guard.get(), 0); err < 0) {
            vlog.error("Could not allocate hw-frame data (%s). Error code: %d", ffmpeg.get_error_description(err).c_str(), err);
            return false;
        }

        return true;
    }

    bool H264VAAPIEncoder::isSupported() {
        return conn->cp.supportsEncoding(encodingKasmVideo);
    }

    void H264VAAPIEncoder::writeRect(const PixelBuffer *pb, const Palette &palette) {
        // compress
        int stride;
        const auto rect = pb->getRect();
        const auto *buffer = pb->getBuffer(rect, &stride);

        const int width = rect.width();
        const int height = rect.height();
        auto *frame = sw_frame_guard.get();

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

        int err{};
        if (err = ffmpeg.sws_scale(sws_guard.get(), src_data, src_line_size, 0, height, frame->data, frame->linesize); err < 0) {
            vlog.error("Error (%s) while scaling image. Error code: %d", ffmpeg.get_error_description(err).c_str(), err);
            return;
        }

        if (err = ffmpeg.av_hwframe_transfer_data(hw_frame_guard.get(), frame, 0); err < 0) {
            vlog.error(
                    "Error while transferring frame data to surface (%s). Error code: %d", ffmpeg.get_error_description(err).c_str(), err);
        }

        if (err = ffmpeg.avcodec_send_frame(ctx_guard.get(), hw_frame_guard.get()); err < 0) {
            vlog.error("Error sending frame to codec (%s). Error code: %d", ffmpeg.get_error_description(err).c_str(), err);
            return;
        }

        auto *pkt = pkt_guard.get();

        err = ffmpeg.avcodec_receive_packet(ctx_guard.get(), pkt);
        if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) {
            // Trying again
            ffmpeg.avcodec_send_frame(ctx_guard.get(), hw_frame_guard.get());
            err = ffmpeg.avcodec_receive_packet(ctx_guard.get(), pkt);
        }

        if (err < 0) {
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
        vlog.debug("Frame size:  %d", pkt->size);

        ++frame->pts;
        ffmpeg.av_packet_unref(pkt);
    }

    void H264VAAPIEncoder::writeSolidRect(int width, int height, const PixelFormat &pf, const rdr::U8 *colour) {}

    void H264VAAPIEncoder::writeSkipRect() {
        auto *os = conn->getOutStream(conn->cp.supportsUdp);
        os->writeU8(kasmVideoSkip << 4);
    }
} // namespace rfb
