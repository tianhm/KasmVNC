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
#include <tbb/spin_mutex.h>
#include <vector>
#include "KasmVideoConstants.h"
#include "VideoEncoder.h"
#include "rfb/Encoder.h"
#include "rfb/ffmpeg.h"

inline constexpr uint8_t MAX_SCREENS = 8;

namespace rfb {
    template<uint8_t T = MAX_SCREENS>
    class ScreenEncoderManager final : public Encoder {
        static_assert(
            T <= std::numeric_limits<uint64_t>::digits, "ScreenEncoderManager mask should be changed as current mask supports T <= 64");
        struct screen_t {
            Screen layout{};
            VideoEncoder *encoder{};
            bool dirty{};
        };

        uint8_t head{};
        uint8_t count{};

        uint64_t mask{};
        std::vector<uint8_t> screens_to_refresh;
        tbb::spin_mutex conn_mutex;

        std::array<screen_t, T> screens{};
        const FFmpeg &ffmpeg;
        VideoEncoderParams current_params;

        KasmVideoEncoders::Encoder base_video_encoder;
        std::vector<KasmVideoEncoders::Encoder> available_encoders;
        const char *dri_node{};

        [[nodiscard]] VideoEncoder *add_encoder(const Screen &layout) const;
        bool add_screen(uint8_t index, const Screen &layout);
        [[nodiscard]] size_t get_screen_count() const;
        void remove_screen(uint8_t index);
        void rebuild_screens_to_refresh();

    public:
        struct stats_t {
            uint64_t rects{};
            uint64_t pixels{};
            uint64_t bytes{};
            uint64_t equivalent{};
        };
        [[nodiscard]] stats_t get_stats() const;
        // Iterator
        using iterator = typename std::array<screen_t, T>::iterator;
        using const_iterator = typename std::array<screen_t, T>::const_iterator;

        iterator begin() {
            return screens.begin();
        }
        iterator end() {
            return screens.end();
        }

        [[nodiscard]] const_iterator cbegin() const {
            return screens.begin();
        }
        [[nodiscard]] const_iterator cend() const {
            return screens.end();
        }

        explicit ScreenEncoderManager(const FFmpeg &ffmpeg_, KasmVideoEncoders::Encoder encoder,
            const std::vector<KasmVideoEncoders::Encoder> &encoders, SConnection *conn, const char *dri_node, VideoEncoderParams params);
        ~ScreenEncoderManager() override;

        ScreenEncoderManager(const ScreenEncoderManager &) = delete;
        ScreenEncoderManager &operator=(const ScreenEncoderManager &) = delete;
        ScreenEncoderManager(ScreenEncoderManager &&) = delete;
        ScreenEncoderManager &operator=(ScreenEncoderManager &&) = delete;

        bool sync_layout(const ScreenSet &layout, const Region &region);
        [[nodiscard]] KasmVideoEncoders::Encoder get_encoder() const {
            return base_video_encoder;
        }

        // Encoder
        [[nodiscard]] bool isSupported() const override;

        void writeRect(const PixelBuffer *pb, const Palette &palette) override;
        void writeSolidRect(int width, int height, const PixelFormat &pf, const rdr::U8 *colour) override;

    private:
        stats_t stats{};
    };

    template class ScreenEncoderManager<>;
} // namespace rfb
