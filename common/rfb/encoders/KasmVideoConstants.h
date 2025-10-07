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
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace rfb {
    // Compression control
    static constexpr unsigned int kasmVideoH264 = 0x01; // H.264 encoding
    static constexpr unsigned int kasmVideoSkip = 0x00; // Skip frame

    static constexpr int GroupOfPictureSize = 10; // interval between I-frames

    static constexpr std::array<std::string_view, 4> drm_device_paths = {
            "/dev/dri/renderD128",
            "/dev/dri/card0",
            "/dev/dri/renderD129",
            "/dev/dri/card1",
    };

    struct SupportedVideoEncoders {
        enum class Codecs : uint8_t
        {
            H264,
            H265,
            AV1
        };

        static inline std::array<std::string_view, 3> CodecNames = {"h264", "h265", "av1"};

        static std::string_view to_string(Codecs codec) {
            return CodecNames[static_cast<uint8_t>(codec)];
        }

        static bool is_supported(std::string_view codec) {
            if (codec.empty())
                return false;

            for (auto supported_codec: CodecNames)
                if (supported_codec == codec)
                    return true;

            if (codec == "hevc")
                return true;

            return false;
        }
    };

    struct KasmVideoEncoders {
        enum class Encoder : uint8_t
        {
            av1_vaapi,
            av1_software,
            hevc_vaapi, // h265
            h265_software,
            h264_vaapi,
            h264_ffmpeg_vaapi,
            h264_nvenc,
            h264_software,
            unavailable
        };

        static inline std::array<std::string_view, 9> EncoderNames = {
                "av1_vaapi", "av1_software", "hevc_vaapi", "libx265", "h264_vaapi", "h264_vaapi", "h264_nvenc", "libx264", "unavailable"};

        static std::string_view to_string(Encoder encoder) {
            return EncoderNames[static_cast<uint8_t>(encoder)];
        }
    };


} // namespace rfb
