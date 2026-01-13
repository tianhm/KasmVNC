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
#include "VideoEncoderFactory.h"

#include <cstdint>
#include "FFMPEGVAAPIEncoder.h"
#include "SoftwareEncoder.h"
#include "VAAPIEncoder.h"

namespace rfb {
    class EncoderBuilderBase {
    public:
        virtual VideoEncoder *build() = 0;
        virtual ~EncoderBuilderBase() = default;
    };

    template<typename T>
    struct is_ffmpeg_based {
        static constexpr auto value = true;
    };

    template<>
    struct is_ffmpeg_based<VAAPIEncoder> {
        static constexpr auto value = false;
    };

    template<typename T>
    class EncoderBuilder : public EncoderBuilderBase {
        static constexpr uint32_t INVALID_ID{std::numeric_limits<uint32_t>::max()};
        Screen layout;
        const FFmpeg *ffmpeg{};
        KasmVideoEncoders::Encoder encoder{};
        VideoEncoderParams params{};
        SConnection *conn{};
        const char *dri_node{};

        explicit EncoderBuilder(const FFmpeg *ffmpeg_) :
            ffmpeg(ffmpeg_) {
            layout.id = INVALID_ID;
        }
        EncoderBuilder() = default;

    public:
        static EncoderBuilder create(const FFmpeg *ffmpeg) {
            return EncoderBuilder{ffmpeg};
        }

        static EncoderBuilder create() {
            return EncoderBuilder{};
        }

        EncoderBuilder &with_params(VideoEncoderParams value) {
            params = value;

            return *this;
        }

        EncoderBuilder &with_encoder(KasmVideoEncoders::Encoder value) {
            encoder = value;

            return *this;
        }

        EncoderBuilder &with_connection(SConnection *value) {
            conn = value;

            return *this;
        }

        EncoderBuilder &with_id(uint32_t value) {
            layout.id = value;

            return *this;
        }

        EncoderBuilder &with_layout(const Screen &layout_) {
            layout = layout_;

            return *this;
        }

        EncoderBuilder &with_dri_node(const char *path) {
            dri_node = path;

            return *this;
        }

        VideoEncoder *build() override {
            if (layout.id == INVALID_ID)
                throw std::runtime_error("Encoder does not have a valid id");

            if (!conn)
                throw std::runtime_error("Connection is required");

            if constexpr (is_ffmpeg_based<T>::value) {
                if (!ffmpeg)
                    throw std::runtime_error("FFmpeg is required");

                if constexpr (std::is_same_v<T, FFMPEGVAAPIEncoder>) {
                    return new T(layout, *ffmpeg, conn, encoder, dri_node, params);
                } else
                    return new T(layout, *ffmpeg, conn, encoder, params);
            } else {
                return new T(conn, encoder, params);
            }
        }
    };

    using FFMPEGVAAPIEncoderBuilder = EncoderBuilder<FFMPEGVAAPIEncoder>;
    using VAAPIEncoderBuilder = EncoderBuilder<VAAPIEncoder>;
    using SoftwareEncoderBuilder = EncoderBuilder<SoftwareEncoder>;

    VideoEncoder *create_encoder(const Screen &layout, const FFmpeg *ffmpeg, SConnection *conn, KasmVideoEncoders::Encoder video_encoder,
        const char *dri_node, VideoEncoderParams params) {
        switch (video_encoder) {
            case KasmVideoEncoders::Encoder::h264_vaapi:
            case KasmVideoEncoders::Encoder::h265_vaapi:
            case KasmVideoEncoders::Encoder::av1_vaapi:
                // return
                // H264VAAPIEncoderBuilder::create().with_connection(conn).with_frame_rate(frame_rate).with_bit_rate(bit_rate).build();
            case KasmVideoEncoders::Encoder::h264_ffmpeg_vaapi:
            case KasmVideoEncoders::Encoder::h265_ffmpeg_vaapi:
            case KasmVideoEncoders::Encoder::av1_ffmpeg_vaapi:
                return FFMPEGVAAPIEncoderBuilder::create(ffmpeg)
                    .with_layout(layout)
                    .with_connection(conn)
                    .with_encoder(video_encoder)
                    .with_params(params)
                    .with_dri_node(dri_node)
                    .build();
            case KasmVideoEncoders::Encoder::h264_nvenc:
            case KasmVideoEncoders::Encoder::h265_nvenc:
            case KasmVideoEncoders::Encoder::av1_nvenc:
                throw std::runtime_error("NVENC is not supported yet");
            default:
                return SoftwareEncoderBuilder::create(ffmpeg)
                    .with_layout(layout)
                    .with_connection(conn)
                    .with_encoder(video_encoder)
                    .with_params(params)
                    .build();
        }
    }
} // namespace rfb
