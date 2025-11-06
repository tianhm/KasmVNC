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

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <rfb/encodings.h>
#include <string_view>
#include <type_traits>
#include <vector>

template<typename E>
class EnumRange {
public:
    using val_t = std::underlying_type_t<E>;

    class EnumIterator {
        val_t value;

    public:
        explicit EnumIterator(E v) :
            value(static_cast<std::underlying_type_t<E>>(v)) {}
        E operator*() const {
            return static_cast<E>(value);
        }
        EnumIterator &operator++() {
            ++value;
            return *this;
        }
        bool operator!=(const EnumIterator &other) const {
            return value != other.value;
        }
    };

    EnumRange(E begin, E end) :
        begin_iter(EnumIterator(begin)),
        end_iter(++EnumIterator(end)) {}
    EnumIterator begin() const {
        return begin_iter;
    }
    EnumIterator end() const {
        return end_iter;
    }
    EnumIterator begin() {
        return begin_iter;
    }
    EnumIterator end() {
        return end_iter;
    }

private:
    EnumIterator begin_iter;
    EnumIterator end_iter;
};

template<typename T>
auto enum_range(T begin, T end) {
    return EnumRange<T>(begin, end);
}

namespace rfb {
    // Compression control
    static constexpr unsigned int kasmVideoH264 = 0x01 << 4; // H.264 encoding
    static constexpr unsigned int kasmVideoH265 = 0x02 << 4; // H.265 encoding
    static constexpr unsigned int kasmVideoAV1 = 0x03 << 4; // AV1 encoding
    static constexpr unsigned int kasmVideoSkip = 0x00 << 4; // Skip frame

    static constexpr auto drm_device_paths = std::to_array<std::string_view>({
        "/dev/dri/renderD128",
        "/dev/dri/card0",
        "/dev/dri/renderD129",
        "/dev/dri/card1",
    });

    struct KasmVideoEncoders {
        // Codecs are ordered by preferred usage quality
        enum class Encoder : uint8_t
        {
            av1_vaapi,
            av1_ffmpeg_vaapi,
            av1_nvenc,
            av1_software,

            h265_vaapi, // h265
            h265_ffmpeg_vaapi,
            h265_nvenc,
            h265_software,

            h264_vaapi,
            h264_ffmpeg_vaapi,
            h264_nvenc,
            h264_software,

            unavailable
        };

        using Encoders = std::vector<Encoder>;

        static inline auto EncoderNames = std::to_array<std::string_view>({"av1_vaapi",
            "av1_vaapi",
            "av1_nvenc",
            "libsvtav1",

            "hevc_vaapi",
            "hevc_vaapi",
            "hevc_nvenc",
            "libx265",

            "h264_vaapi",
            "h264_vaapi",
            "h264_nvenc",
            "libx264",
            "unavailable"});

        static inline auto Encodings = std::to_array<int>({pseudoEncodingStreamingModeAV1VAAPI,
            pseudoEncodingStreamingModeAV1VAAPI,
            pseudoEncodingStreamingModeAV1NVENC,
            pseudoEncodingStreamingModeAV1SW,

            pseudoEncodingStreamingModeHEVCVAAPI,
            pseudoEncodingStreamingModeHEVCVAAPI,
            pseudoEncodingStreamingModeHEVCNVENC,
            pseudoEncodingStreamingModeHEVCSW,

            pseudoEncodingStreamingModeAVCVAAPI,
            pseudoEncodingStreamingModeAVCVAAPI,
            pseudoEncodingStreamingModeAVCNVENC,
            pseudoEncodingStreamingModeAVCSW,

            pseudoEncodingStreamingModeJpegWebp});

        static bool is_accelerated(Encoder encoder) {
            return encoder != Encoder::h264_software && encoder != Encoder::h265_software && encoder != Encoder::av1_software;
        }

        static std::string_view to_string(Encoder encoder) {
            return EncoderNames[static_cast<uint8_t>(encoder)];
        }

        static int to_encoding(Encoder encoder) {
            return Encodings[static_cast<uint8_t>(encoder)];
        }

        static Encoder from_encoding(int encoding) {
            for (auto encoder: enum_range(Encoder::av1_vaapi, Encoder::unavailable)) {
                if (to_encoding(encoder) == encoding) {
                    switch (encoder) {
                        case Encoder::av1_vaapi:
                            return Encoder::av1_ffmpeg_vaapi;
                        case Encoder::h265_vaapi:
                            return Encoder::h265_ffmpeg_vaapi;
                        case Encoder::h264_vaapi:
                            return Encoder::h264_ffmpeg_vaapi;
                        default:
                            return encoder;
                    }
                }
            }

            return Encoder::unavailable;
        }

        static unsigned int to_msg_id(Encoder encoder) {
            switch (encoder) {
                case Encoder::av1_vaapi:
                case Encoder::av1_ffmpeg_vaapi:
                case Encoder::av1_nvenc:
                case Encoder::av1_software:
                    return kasmVideoAV1;
                case Encoder::h265_vaapi: // h265
                case Encoder::h265_ffmpeg_vaapi:
                case Encoder::h265_nvenc:
                case Encoder::h265_software:
                    return kasmVideoH265;
                case Encoder::h264_vaapi:
                case Encoder::h264_ffmpeg_vaapi:
                case Encoder::h264_nvenc:
                case Encoder::h264_software:
                    return kasmVideoH264;
                default:
                    assert(false);
            }
        }

        static int32_t to_streaming_mode(Encoder encoder) {
            switch (encoder) {
                case Encoder::av1_vaapi:
                case Encoder::av1_ffmpeg_vaapi:
                case Encoder::av1_nvenc:
                case Encoder::av1_software:
                    return pseudoEncodingStreamingModeAV1;
                case Encoder::h265_vaapi: // h265
                case Encoder::h265_ffmpeg_vaapi:
                case Encoder::h265_nvenc:
                case Encoder::h265_software:
                    return pseudoEncodingStreamingModeHEVC;
                case Encoder::h264_vaapi:
                case Encoder::h264_ffmpeg_vaapi:
                case Encoder::h264_nvenc:
                case Encoder::h264_software:
                    return pseudoEncodingStreamingModeAVC;
                default:
                    return pseudoEncodingStreamingModeJpegWebp;
            }
        }
    };

    struct SupportedVideoEncoders {
        enum class Codecs : uint8_t
        {
            h264,
            h264_vaapi,
            h264_nvenc,
            avc,
            avc_vaapi,
            avc_nvenc,

            h265,
            h265_vaapi,
            h265_nvenc,
            hevc,
            hevc_vaapi,
            hevc_nvenc,

            av1,
            av1_vaapi,
            av1_nvenc,
            auto_detect,
            unavailable
        };

        static constexpr auto MappedCodecs = std::to_array<KasmVideoEncoders::Encoder>({KasmVideoEncoders::Encoder::h264_software,
            KasmVideoEncoders::Encoder::h264_ffmpeg_vaapi,
            KasmVideoEncoders::Encoder::h264_nvenc,
            KasmVideoEncoders::Encoder::h264_software,
            KasmVideoEncoders::Encoder::h264_ffmpeg_vaapi,
            KasmVideoEncoders::Encoder::h264_nvenc,

            KasmVideoEncoders::Encoder::h265_software,
            KasmVideoEncoders::Encoder::h265_ffmpeg_vaapi,
            KasmVideoEncoders::Encoder::h265_nvenc,
            KasmVideoEncoders::Encoder::h265_software,
            KasmVideoEncoders::Encoder::h265_ffmpeg_vaapi,
            KasmVideoEncoders::Encoder::h265_nvenc,

            KasmVideoEncoders::Encoder::av1_software,
            KasmVideoEncoders::Encoder::av1_ffmpeg_vaapi,
            KasmVideoEncoders::Encoder::av1_nvenc,
            KasmVideoEncoders::Encoder::h264_software,
            KasmVideoEncoders::Encoder::unavailable});

        static inline auto CodecNames = std::to_array<std::string_view>({"h264",
            "h264_vaapi",
            "h264_nvenc",
            "avc",
            "avc_vaapi",
            "avc_nvenc",

            "h265",
            "h265_vaapi",
            "h265_nvenc",
            "hevc",
            "hevc_vaapi",
            "hevc_nvenc",

            "av1",
            "av1_vaapi",
            "av1_nvenc",
            "auto"});

        static std::string_view to_string(Codecs codec) {
            return CodecNames[static_cast<uint8_t>(codec)];
        }

        static bool is_supported(std::string_view codec) {
            if (codec.empty())
                return false;

            for (const auto supported_codec: CodecNames)
                if (supported_codec == codec)
                    return true;

            return false;
        }

        static auto get_codec(std::string_view codec) {
            for (auto codec_impl: enum_range(Codecs::h264, Codecs::auto_detect)) {
                if (to_string(codec_impl) == codec)
                    return codec_impl;
            }

            return Codecs::unavailable;
        }

        static constexpr auto map_encoder(Codecs impl) {
            return MappedCodecs[static_cast<uint8_t>(impl)];
        }

        static std::vector<std::string_view> parse(std::string_view codecs) {
            std::vector<std::string_view> result;

            if (codecs.empty())
                return {};

            size_t pos{};
            size_t start{};

            while (pos < codecs.size()) {
                pos = codecs.find_first_of(',', pos);
                if (pos == std::string_view::npos)
                    pos = codecs.size();

                result.push_back(codecs.substr(start, pos - start));

                start = ++pos;
            }

            return result;
        }

        static KasmVideoEncoders::Encoders map_encoders(const std::vector<std::string_view> &codecs) {
            KasmVideoEncoders::Encoders result;

            if (codecs.empty())
                return {};

            for (auto codec_name: codecs) {
                const auto codec = get_codec(codec_name);

                switch (codec) {
                    case Codecs::auto_detect:
                        if (!result.empty())
                            result.clear();

                        result.push_back(map_encoder(Codecs::av1_nvenc));
                        result.push_back(map_encoder(Codecs::av1_vaapi));
                        result.push_back(map_encoder(Codecs::av1));
                        result.push_back(map_encoder(Codecs::h265_nvenc));
                        result.push_back(map_encoder(Codecs::h265_vaapi));
                        result.push_back(map_encoder(Codecs::h265));
                        result.push_back(map_encoder(Codecs::h264_nvenc));
                        result.push_back(map_encoder(Codecs::h264_vaapi));
                        result.push_back(map_encoder(Codecs::h264));

                        return result;
                    default:
                    {
                        const auto encoder = map_encoder(codec);
                        if (std::find(result.begin(), result.end(), encoder) == result.end())
                            result.push_back(encoder);
                    }
                }
            }

            return result;
        }

        static KasmVideoEncoders::Encoders filter_available_encoders(
            const KasmVideoEncoders::Encoders &encoders, const KasmVideoEncoders::Encoders &available) {
            KasmVideoEncoders::Encoders result;

            for (auto encoder: available) {
                if (std::ranges::find(encoders.begin(), encoders.end(), encoder) != encoders.end())
                    result.push_back(encoder);
            }

            return result;
        }
    };

} // namespace rfb
