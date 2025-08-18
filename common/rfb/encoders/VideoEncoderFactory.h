#pragma once

#include <cstdint>
#include "KasmVideoConstants.h"
#include "rfb/Encoder.h"
#include "rfb/ffmpeg.h"

namespace rfb {
    Encoder *create_encoder(uint32_t id, const FFmpeg *ffmpeg, KasmVideoEncoders::Encoder video_encoder, SConnection *conn, uint8_t frame_rate,
                            uint16_t bit_rate);

} // namespace rfb
