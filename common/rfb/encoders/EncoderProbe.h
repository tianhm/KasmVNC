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

#include <vector>
#include "KasmVideoConstants.h"
#include "SupportedVideoEncoders.h"
#include "rfb/ffmpeg.h"

namespace rfb::video_encoders {
    class EncoderProbe {
        KasmVideoEncoders::Encoder best_encoder{KasmVideoEncoders::Encoder::h264_software};
        KasmVideoEncoders::Encoders available_encoders;
        std::string drm_device_path;
        FFmpeg &ffmpeg;

        explicit EncoderProbe(FFmpeg &ffmpeg, const std::vector<std::string_view> &parsed_encoders, const char *dri_node);
        KasmVideoEncoders::Encoders probe(const char *dri_node);

    public:
        EncoderProbe(const EncoderProbe &) = delete;
        EncoderProbe &operator=(const EncoderProbe &) = delete;
        EncoderProbe(EncoderProbe &&) = delete;
        EncoderProbe &operator=(EncoderProbe &&) = delete;

        static EncoderProbe &get(FFmpeg &ffmpeg, const std::vector<std::string_view> &parsed_encoders, const char *dri_node) {
            static EncoderProbe instance{ffmpeg, parsed_encoders, dri_node};

            return instance;
        }

        // [[nodiscard]] static bool is_acceleration_available();

        [[nodiscard]] KasmVideoEncoders::Encoder get_best_encoder() const {
            return best_encoder;
        }

        [[nodiscard]] const KasmVideoEncoders::Encoders &get_available_encoders() const {
            return available_encoders;
        }

        [[nodiscard]] const KasmVideoEncoders::Encoders &update_encoders(const std::vector<std::string_view> &codecs) {
            available_encoders = SupportedVideoEncoders::filter_available_encoders(SupportedVideoEncoders::map_encoders(codecs), available_encoders);
            return available_encoders;
        }

        [[nodiscard]] const char *get_drm_device_path() const {
            return drm_device_path.c_str();
        }
    };

} // namespace rfb::video_encoders
