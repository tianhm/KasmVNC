/* Copyright (C) 2025 Kasm.  All Rights Reserved.
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
#pragma once

#include "KasmVideoConstants.h"
#include "VideoEncoder.h"
#include "rfb/Encoder.h"
#include "rfb/ffmpeg.h"

namespace rfb {
    VideoEncoder *create_encoder(const Screen &layout, const FFmpeg *ffmpeg, SConnection *conn, KasmVideoEncoders::Encoder video_encoder, const char *dri_node, VideoEncoderParams params);

} // namespace rfb
