#pragma once

#include "KasmVideoConstants.h"
#include "VideoEncoder.h"
#include "rfb/Encoder.h"
#include "rfb/ffmpeg.h"

namespace rfb {
    VideoEncoder *create_encoder(const Screen &layout, const FFmpeg *ffmpeg, SConnection *conn, KasmVideoEncoders::Encoder video_encoder, const char *dri_node, VideoEncoderParams params);

} // namespace rfb
