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

#include <string_view>
#include <array>
#include <fcntl.h>

namespace rfb {
    // Compression control
    static constexpr unsigned int kasmVideoH264 = 0x01; // H.264 encoding
    static constexpr unsigned int kasmVideoSkip = 0x00; // Skip frame

    static constexpr int GroupOfPictureSize = 10; // interval between I-frames

    static auto render_path = "/dev/dri/renderD128";

    inline bool is_acceleration_available() {
        if (access(render_path, R_OK | W_OK) != 0)
            return false;

        const int fd = open(render_path, O_RDWR);
        if (fd < 0)
            return false;

        close(fd);

        return true;
    }

    inline static bool hw_accel = is_acceleration_available();

    struct VideoEncoders {
        enum class Codecs : uint8_t
        {
            H264
        };

        static inline std::array<std::string_view, 1> CodecNames = {"h264"};

        static std::string_view to_string(Codecs codec) {
            return CodecNames[static_cast<uint8_t>(codec)];
        }
    };
} // namespace rfb
