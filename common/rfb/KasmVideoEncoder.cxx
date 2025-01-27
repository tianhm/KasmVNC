/* Copyright (C) 2024 Kasm.  All Rights Reserved.
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 * USA.
 */
#include <rdr/OutStream.h>
#include <rfb/encodings.h>
#include <rfb/LogWriter.h>
#include <rfb/SConnection.h>
#include <rfb/ServerCore.h>
#include <rfb/PixelBuffer.h>
#include <rfb/KasmVideoEncoder.h>
#include <rfb/KasmVideoConstants.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/hwcontext_vaapi.h>
}
using namespace rfb;
static LogWriter vlog("KasmVideoEncoder");
static const PixelFormat pfRGBX(32, 24, false, true, 255, 255, 255, 0, 8, 16);
static const PixelFormat pfBGRX(32, 24, false, true, 255, 255, 255, 16, 8, 0);
struct h264_t {
  AVCodecContext *ctx;
  AVFrame *frame;
  AVPacket pkt;
};
KasmVideoEncoder::KasmVideoEncoder(SConnection* conn) :
  Encoder(conn, encodingKasmVideo, (EncoderFlags)(EncoderUseNativePF | EncoderLossy), -1),
  init(false), sw(0), sh(0), h264(NULL)
{
  h264 = new h264_t;
}
KasmVideoEncoder::~KasmVideoEncoder()
{
  delete h264;
}
bool KasmVideoEncoder::isSupported()
{
  return conn->cp.supportsEncoding(encodingKasmVideo);
}
static void init_h264(h264_t *h264,
                    const uint32_t w, const uint32_t h, const uint32_t fps,
                    const uint32_t bitrate) {
  int err;
  AVHWDeviceType devtype = AV_HWDEVICE_TYPE_VAAPI;
  AVBufferRef *hw_device_ctx = NULL;

  av_hwdevice_ctx_create(&hw_device_ctx, devtype, "/dev/dri/renderD128", 0, NULL);
  if (!hw_device_ctx) {
    vlog.error("Failed to create VAAPI device context");
    return;
  }

  h264->ctx = avcodec_alloc_context3(avcodec_find_encoder_by_name("h264_vaapi"));
  if (!h264->ctx) {
    vlog.error("Can't allocate AVCodecContext");
    av_buffer_unref(&hw_device_ctx);
    return;
  }

  h264->ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
  if (!h264->ctx->hw_device_ctx) {
    vlog.error("Failed to reference VAAPI device context");
    avcodec_free_context(&h264->ctx);
    av_buffer_unref(&hw_device_ctx);
    return;
  }

  h264->frame = av_frame_alloc();
  if (!h264->frame) {
    vlog.error("Can't allocate AVFrame");
    avcodec_free_context(&h264->ctx);
    return;
  }

  h264->frame->format = AV_PIX_FMT_VAAPI;
  h264->frame->width = w;
  h264->frame->height = h;
  if (av_image_alloc(h264->frame->data, h264->frame->linesize, h264->frame->width, h264->frame->height, AV_PIX_FMT_NV12, 32) < 0) {
    vlog.error("Failed to allocate image");
    avcodec_free_context(&h264->ctx);
    av_frame_free(&h264->frame);
    return;
  }

  h264->ctx->width = w;
  h264->ctx->height = h;
  h264->ctx->time_base = (AVRational){1, fps};
  h264->ctx->gop_size = 10;
  h264->ctx->max_b_frames = 0;
  h264->ctx->pix_fmt = AV_PIX_FMT_VAAPI;
  if (avcodec_open2(h264->ctx, avcodec_find_encoder_by_name("h264_vaapi"), nullptr) < 0) {
    vlog.error("Failed to open codec");
    avcodec_free_context(&h264->ctx);
    av_frame_free(&h264->frame);
    return;
  }
}
static void deinit_h264(h264_t *h264) {
  if (h264->ctx && h264->ctx->hw_device_ctx) {
    av_buffer_unref(&h264->ctx->hw_device_ctx);
  }
  avcodec_free_context(&h264->ctx);
  av_frame_free(&h264->frame);
}
void KasmVideoEncoder::writeRect(const PixelBuffer* pb, const Palette& palette)
{
  const rdr::U8* buffer;
  int stride;
  rdr::OutStream* os;
  buffer = pb->getBuffer(pb->getRect(), &stride);
  // compress
  AVFrame *pFrame = av_frame_alloc();
  if (!pFrame) {
    vlog.error("Can't allocate AVFrame");
    return;
  }
  pFrame->format = AV_PIX_FMT_VAAPI;
  pFrame->width = pb->getRect().width(); // Use the width from the PixelBuffer
  pFrame->height = pb->getRect().height(); // Use the height from the PixelBuffer
  if (av_image_fill_arrays(pFrame->data, pFrame->linesize, buffer, AV_PIX_FMT_BGR24, pb->getRect().width(), pb->getRect().height(), 1) < 0) {
    vlog.error("Can't fill image arrays");
    av_frame_free(&pFrame);
    return;
  }
  os = conn->getOutStream(conn->cp.supportsUdp);
  if (!strcmp(Server::videoCodec, "h264")) {
    os->writeU8(kasmVideoH264 << 4);
    if (!init) {
      init_h264(h264, pb->getRect().width(), pb->getRect().height(), Server::frameRate,
                Server::videoBitrate);
      init = true;
      sw = pb->getRect().width();
      sh = pb->getRect().height();
    } else if (sw != (uint32_t) pb->getRect().width() || sh != (uint32_t) pb->getRect().height()) {
      deinit_h264(h264);
      init_h264(h264, pb->getRect().width(), pb->getRect().height(), Server::frameRate,
                Server::videoBitrate);
      sw = pb->getRect().width();
      sh = pb->getRect().height();
    }
    int ret = avcodec_send_frame(h264->ctx, pFrame);
    if (ret < 0) {
      vlog.error("Error sending frame to codec");
      return;
    }
    while (ret >= 0) {
      AVPacket pkt;
      ret = avcodec_receive_packet(h264->ctx, &pkt);
      if (ret < 0) {
        vlog.error("Error receiving packet from codec");
        break;
      }
      writeCompact(pkt.size + 1, os);
      os->writeBytes(&pkt.data[0], pkt.size);
      av_packet_unref(&pkt);
    }
  } else {
    vlog.error("Unknown test videoCodec %s", (const char *) Server::videoCodec);
  }
  av_frame_free(&pFrame);
}
void KasmVideoEncoder::writeSkipRect()
{
  rdr::OutStream* os;
  os = conn->getOutStream(conn->cp.supportsUdp);
  os->writeU8(kasmVideoSkip << 4);
}
void KasmVideoEncoder::writeCompact(rdr::U32 value, rdr::OutStream* os) const
{
  // Copied from TightEncoder as it's overkill to inherit just for this
  rdr::U8 b;
  b = value & 0x7F;
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
void KasmVideoEncoder::writeSolidRect(int width, int height,
                                const PixelFormat& pf,
                                const rdr::U8* colour)
{
// nop
}
