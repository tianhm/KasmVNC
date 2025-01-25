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
#ifndef __RFB_KASMVIDEOENCODER_H__
#define __RFB_KASMVIDEOENCODER_H__

#include <rfb/Encoder.h>
#include <stdint.h>
#include <vector>

struct hevc_t;

namespace rfb {

  // Kasm Video Encodings
  const int kasmVideoVP8 = 0;  // VP8 encoding
  const int kasmVideoHEVC = 1; // HEVC encoding
  const int kasmVideoSkip = 2; // Skip frame
  
  class KasmVideoEncoder : public Encoder {
  public:
    KasmVideoEncoder(SConnection* conn);
    virtual ~KasmVideoEncoder();

    virtual bool isSupported();

    virtual void writeRect(const PixelBuffer* pb, const Palette& palette);
    virtual void writeSkipRect();
    virtual void writeSolidRect(int width, int height,
                                const PixelFormat& pf,
                                const rdr::U8* colour);

  protected:
    void writeCompact(rdr::U32 value, rdr::OutStream* os) const;

  private:
    bool init;
    uint32_t sw, sh;

    hevc_t *hevc;
  };
}
#endif
