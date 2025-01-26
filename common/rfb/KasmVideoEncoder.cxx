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
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
using namespace rfb;
static LogWriter vlog("KasmVideoEncoder");
static const PixelFormat pfRGBX(32, 24, false, true, 255, 255, 255, 0, 8, 16);
static const PixelFormat pfBGRX(32, 24, false, true, 255, 255, 255, 16, 8, 0);
struct hevc_t {
  AVCodecContext *ctx;
  AVFrame *frame;
  AVPacket pkt;
};
KasmVideoEncoder::KasmVideoEncoder(SConnection* conn) :
  Encoder(conn, encodingKasmVideo, (EncoderFlags)(EncoderUseNativePF | EncoderLossy), -1),
  init(false), sw(0), sh(0), hevc(NULL)
{
  hevc = new hevc_t;
}
KasmVideoEncoder::~KasmVideoEncoder()
{
  delete hevc;
}
bool KasmVideoEncoder::isSupported()
{
  return conn->cp.supportsEncoding(encodingKasmVideo);
}
static void init_hevc(hevc_t *hevc,
                    const uint32_t w, const uint32_t h, const uint32_t fps,
                    const uint32_t bitrate) {
  hevc->ctx = avcodec_alloc_context3(avcodec_find_encoder(AV_CODEC_ID_HEVC));
  if (!hevc->ctx) {
    vlog.error("Can't allocate AVCodecContext");
    return;
  }
  hevc->frame = av_frame_alloc();
  if (!hevc->frame) {
    vlog.error("Can't allocate AVFrame");
    avcodec_free_context(&hevc->ctx);
    return;
  }
  hevc->frame->format = AV_PIX_FMT_YUV420P;
  hevc->frame->width = w;
  hevc->frame->height = h;
  if (av_image_alloc(hevc->frame->data, hevc->frame->linesize, w, h, AV_PIX_FMT_YUV420P, 32) < 0) {
    vlog.error("Can't allocate image");
    avcodec_free_context(&hevc->ctx);
    av_frame_free(&hevc->frame);
    return;
  }
  hevc->ctx->bit_rate = bitrate * 1000;
  hevc->ctx->framerate = (AVRational){fps, 1};
  hevc->ctx->time_base = (AVRational){1, fps};
  hevc->ctx->gop_size = 10; // arbitrary
  hevc->ctx->max_b_frames = 1;
  hevc->ctx->pix_fmt = AV_PIX_FMT_YUV420P;
  if (avcodec_open2(hevc->ctx, avcodec_find_encoder(AV_CODEC_ID_HEVC), nullptr) < 0) {
    vlog.error("Failed to open codec");
    avcodec_free_context(&hevc->ctx);
    av_frame_free(&hevc->frame);
    return;
  }
}
static void deinit_hevc(hevc_t *hevc) {
  avcodec_free_context(&hevc->ctx);
  av_frame_free(&hevc->frame);
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
  pFrame->format = AV_PIX_FMT_YUV420P;
  pFrame->width = pb->getRect().width();
  pFrame->height = pb->getRect().height();
  if (av_image_fill_arrays(pFrame->data, pFrame->linesize, buffer, AV_PIX_FMT_BGR24, pb->getRect().width(), pb->getRect().height(), 1) < 0) {
    vlog.error("Can't fill image arrays");
    av_frame_free(&pFrame);
    return;
  }
  os = conn->getOutStream(conn->cp.supportsUdp);
  if (!strcmp(Server::videoCodec, "hevc")) {
    os->writeU8(kasmVideoHEVC << 4);
    if (!init) {
      init_hevc(hevc, pb->getRect().width(), pb->getRect().height(), Server::frameRate,
              Server::videoBitrate);
      init = true;
      sw = pb->getRect().width();
      sh = pb->getRect().height();
    } else if (sw != (uint32_t) pb->getRect().width() || sh != (uint32_t) pb->getRect().height()) {
      deinit_hevc(hevc);
      init_hevc(hevc, pb->getRect().width(), pb->getRect().height(), Server::frameRate,
              Server::videoBitrate);
      sw = pb->getRect().width();
      sh = pb->getRect().height();
    }
    int ret = avcodec_send_frame(hevc->ctx, pFrame);
    if (ret < 0) {
      vlog.error("Error sending frame to codec");
      return;
    }
    while (ret >= 0) {
      AVPacket pkt;
      ret = avcodec_receive_packet(hevc->ctx, &pkt);
      if (ret < 0) {
        vlog.error("Error receiving packet from codec");
        break;
      }
      writeCompact(pkt.size + 1, os);
      os->writeBytes(&pkt.data[0], pkt.size);
      av_packet_unref(&pkt);
    }
  } else {
    vlog.error("Unknown videoCodec %s", (const char *) Server::videoCodec);
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
