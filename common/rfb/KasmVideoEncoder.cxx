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

#include <vpx/vpx_encoder.h>
#include <vpx/vp8cx.h>

#include <webp/encode.h>

using namespace rfb;

static LogWriter vlog("KasmVideoEncoder");
static const PixelFormat pfRGBX(32, 24, false, true, 255, 255, 255, 0, 8, 16);
static const PixelFormat pfBGRX(32, 24, false, true, 255, 255, 255, 16, 8, 0);

struct vp8_t {
  vpx_codec_iface_t *cx;
  vpx_image_t raw;
  vpx_codec_enc_cfg_t cfg;
  vpx_codec_ctx_t codec;
  uint32_t frame;
};

KasmVideoEncoder::KasmVideoEncoder(SConnection* conn) :
  Encoder(conn, encodingKasmVideo, (EncoderFlags)(EncoderUseNativePF | EncoderLossy), -1),
  init(false), sw(0), sh(0), vp8(NULL)
{
  vp8 = new vp8_t;
}

KasmVideoEncoder::~KasmVideoEncoder()
{
  delete vp8;
}

bool KasmVideoEncoder::isSupported()
{
  return conn->cp.supportsEncoding(encodingKasmVideo);
}

static void init_vp8(vp8_t *vp8,
                    const uint32_t w, const uint32_t h, const uint32_t fps,
                    const uint32_t bitrate) {
  vp8->cx = vpx_codec_vp8_cx();
  if (!vpx_img_alloc(&vp8->raw, VPX_IMG_FMT_I420, w, h, 1))
    vlog.error("Can't allocate vp8 img");

  if (vpx_codec_enc_config_default(vp8->cx, &vp8->cfg, 0))
    vlog.error("VP8 config");

  vp8->cfg.g_w = w;
  vp8->cfg.g_h = h;
  vp8->cfg.g_timebase.num = 1;
  vp8->cfg.g_timebase.den = fps;
  vp8->cfg.rc_target_bitrate = bitrate;
  vp8->cfg.g_error_resilient = 0;

  vp8->cfg.g_lag_in_frames = 0; // realtime

  if (vpx_codec_enc_init(&vp8->codec, vp8->cx, &vp8->cfg, 0))
    vlog.error("VP8 init");

  vp8->frame = 0;
}

static void deinit_vp8(vp8_t *vp8) {
  //vpx_img_free(&vp8->raw);
  vpx_codec_destroy(&vp8->codec);
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

  if (!strcmp(Server::videoCodec, "vp8")) {
    os->writeU8(kasmVideoVP8 << 4);

    if (!init) {
      init_vp8(vp8, pb->getRect().width(), pb->getRect().height(), Server::frameRate,
              Server::videoBitrate);
      init = true;
      sw = pb->getRect().width();
      sh = pb->getRect().height();
    } else if (sw != (uint32_t) pb->getRect().width() || sh != (uint32_t) pb->getRect().height()) {
      deinit_vp8(vp8);
      init_vp8(vp8, pb->getRect().width(), pb->getRect().height(), Server::frameRate,
              Server::videoBitrate);
      sw = pb->getRect().width();
      sh = pb->getRect().height();
    }

    vp8->raw.planes[0] = pic.y;
    vp8->raw.planes[1] = pic.u;
    vp8->raw.planes[2] = pic.v;

    vp8->raw.stride[0] = pic.y_stride;
    vp8->raw.stride[1] = vp8->raw.stride[2] = pic.uv_stride;

    int flags = 0;
    // VPX_EFLAG_FORCE_KF ?
    // flush? raw NULL, frame -1

    vpx_codec_iter_t iter = NULL;
    const vpx_codec_cx_pkt_t *pkt = NULL;
    const vpx_codec_err_t res =
      vpx_codec_encode(&vp8->codec, &vp8->raw, vp8->frame++, 1, flags, VPX_DL_REALTIME);
    if (res != VPX_CODEC_OK)
      vlog.error("VP8 encoding error %u", res);

    while ((pkt = vpx_codec_get_cx_data(&vp8->codec, &iter)) != NULL) {
      if (pkt->kind == VPX_CODEC_CX_FRAME_PKT) {
        const uint8_t keyframe = (pkt->data.frame.flags & VPX_FRAME_IS_KEY) != 0;
        writeCompact(pkt->data.frame.sz + 1, os);
        os->writeBytes(&keyframe, 1);
        os->writeBytes(pkt->data.frame.buf, pkt->data.frame.sz);
      }
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
