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
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>

#include <webp/encode.h>

using namespace rfb;

static LogWriter vlog("KasmVideoEncoder");
static const PixelFormat pfRGBX(32, 24, false, true, 255, 255, 255, 0, 8, 16);
static const PixelFormat pfBGRX(32, 24, false, true, 255, 255, 255, 16, 8, 0);

struct hevc_t {
  AVCodecContext *codecCtx;
  AVBufferRef *hw_device_ctx;
  AVFrame *hw_frame;
  uint32_t frame;
};

KasmVideoEncoder::KasmVideoEncoder(SConnection* conn) :
  Encoder(conn, encodingKasmVideo, (EncoderFlags)(EncoderUseNativePF | EncoderLossy), -1),
  init(false), sw(0), sh(0), hevc(NULL)
{
  hevc = new hevc_t;
}

KasmVideoEncoder::~KasmVideoEncoder()
{
  if (hevc->codecCtx) {
    avcodec_free_context(&hevc->codecCtx);
  }
  if (hevc->hw_device_ctx) {
    av_buffer_unref(&hevc->hw_device_ctx);
  }
  if (hevc->hw_frame) {
    av_frame_free(&hevc->hw_frame);
  }
  delete hevc;
}

bool KasmVideoEncoder::isSupported()
{
  return conn->cp.supportsEncoding(encodingKasmVideo);
}

static void init_hevc_vaapi(hevc_t *hevc, const uint32_t w, const uint32_t h, const uint32_t fps, const uint32_t bitrate) {
  // Initialize VAAPI hardware context
  int ret = av_hwdevice_ctx_create(&hevc->hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0);
  if (ret < 0) {
    vlog.error("Failed to create VAAPI hardware context");
    return;
  }

  // Find the HEVC VAAPI encoder
  const AVCodec *codec = avcodec_find_encoder_by_name("hevc_vaapi");
  if (!codec) {
    vlog.error("HEVC VAAPI encoder not found");
    return;
  }

  // Allocate codec context
  hevc->codecCtx = avcodec_alloc_context3(codec);
  if (!hevc->codecCtx) {
    vlog.error("Failed to allocate codec context");
    return;
  }

  // Set codec parameters
  hevc->codecCtx->width = w;
  hevc->codecCtx->height = h;
  hevc->codecCtx->time_base = (AVRational){1, fps};
  hevc->codecCtx->bit_rate = bitrate;
  hevc->codecCtx->pix_fmt = AV_PIX_FMT_VAAPI;
  hevc->codecCtx->hw_device_ctx = av_buffer_ref(hevc->hw_device_ctx);

  // Allocate hardware frames context
  AVBufferRef *hw_frames_ctx = av_hwframe_ctx_alloc(hevc->hw_device_ctx);
  AVHWFramesContext *frames_ctx = (AVHWFramesContext *)hw_frames_ctx->data;
  frames_ctx->format = AV_PIX_FMT_VAAPI;
  frames_ctx->sw_format = AV_PIX_FMT_NV12;
  frames_ctx->width = w;
  frames_ctx->height = h;
  frames_ctx->initial_pool_size = 20;
  av_hwframe_ctx_init(hw_frames_ctx);
  hevc->codecCtx->hw_frames_ctx = av_buffer_ref(hw_frames_ctx);

  // Open the codec
  if (avcodec_open2(hevc->codecCtx, codec, nullptr) < 0) {
    vlog.error("Failed to open codec");
    return;
  }

  // Allocate hardware frame
  hevc->hw_frame = av_frame_alloc();
  hevc->hw_frame->format = AV_PIX_FMT_VAAPI;
  hevc->hw_frame->width = w;
  hevc->hw_frame->height = h;
  av_frame_get_buffer(hevc->hw_frame, 0);

  hevc->frame = 0;
}

static void deinit_hevc_vaapi(hevc_t *hevc) {
  if (hevc->codecCtx) {
    avcodec_free_context(&hevc->codecCtx);
  }
  if (hevc->hw_device_ctx) {
    av_buffer_unref(&hevc->hw_device_ctx);
  }
  if (hevc->hw_frame) {
    av_frame_free(&hevc->hw_frame);
  }
}

void KasmVideoEncoder::writeRect(const PixelBuffer* pb, const Palette& palette)
{
  const rdr::U8* buffer;
  int stride;

  rdr::OutStream* os;

  buffer = pb->getBuffer(pb->getRect(), &stride);

  // compress
  WebPPicture pic;

  WebPPictureInit(&pic);
  pic.width = pb->getRect().width();
  pic.height = pb->getRect().height();

  if (pfRGBX.equal(pb->getPF())) {
    WebPPictureImportRGBX(&pic, buffer, stride * 4);
  } else if (pfBGRX.equal(pb->getPF())) {
    WebPPictureImportBGRX(&pic, buffer, stride * 4);
  } else {
    rdr::U8* tmpbuf = new rdr::U8[pic.width * pic.height * 3];
    pb->getPF().rgbFromBuffer(tmpbuf, (const rdr::U8 *) buffer, pic.width, stride, pic.height);
    stride = pic.width * 3;

    WebPPictureImportRGB(&pic, tmpbuf, stride);
    delete [] tmpbuf;
  }

  os = conn->getOutStream(conn->cp.supportsUdp);

  if (!strcmp(Server::videoCodec, "hevc")) {
    os->writeU8(kasmVideoHEVC << 4);

    if (!init) {
      init_hevc_vaapi(hevc, pb->getRect().width(), pb->getRect().height(), Server::frameRate, Server::videoBitrate);
      init = true;
      sw = pb->getRect().width();
      sh = pb->getRect().height();
    } else if (sw != (uint32_t) pb->getRect().width() || sh != (uint32_t) pb->getRect().height()) {
      deinit_hevc_vaapi(hevc);
      init_hevc_vaapi(hevc, pb->getRect().width(), pb->getRect().height(), Server::frameRate, Server::videoBitrate);
      sw = pb->getRect().width();
      sh = pb->getRect().height();
    }

    // Convert YUV to VAAPI hardware frame
    av_hwframe_get_buffer(hevc->codecCtx->hw_frames_ctx, hevc->hw_frame, 0);
    hevc->hw_frame->data[0] = pic.y;
    hevc->hw_frame->data[1] = pic.u;
    hevc->hw_frame->data[2] = pic.v;
    hevc->hw_frame->linesize[0] = pic.y_stride;
    hevc->hw_frame->linesize[1] = pic.uv_stride;

    // Encode the frame
    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data = nullptr;
    pkt.size = 0;

    int ret = avcodec_send_frame(hevc->codecCtx, hevc->hw_frame);
    if (ret < 0) {
      vlog.error("Error sending frame to encoder");
      return;
    }

    while (ret >= 0) {
      ret = avcodec_receive_packet(hevc->codecCtx, &pkt);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        break;
      } else if (ret < 0) {
        vlog.error("Error during encoding");
        break;
      }

      // Ensure the packet is formatted with NAL units for WebCodecs compatibility
      const uint8_t keyframe = (pkt.flags & AV_PKT_FLAG_KEY) != 0;
      writeCompact(pkt.size + 1, os);
      os->writeBytes(&keyframe, 1);
      os->writeBytes(pkt.data, pkt.size);

      av_packet_unref(&pkt);
    }
  } else {
    vlog.error("Unknown videoCodec %s", (const char *) Server::videoCodec);
  }

  WebPPictureFree(&pic);
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
