#pragma once

#include <vector>
#include "KasmVideoConstants.h"
#include "rfb/ffmpeg.h"

namespace rfb::video_encoders {
    class EncoderProbe {
        KasmVideoEncoders::Encoder best_encoder{KasmVideoEncoders::Encoder::h264_software};
        std::vector<KasmVideoEncoders::Encoder> available_encoders;
        FFmpeg &ffmpeg;

        explicit EncoderProbe(FFmpeg &ffmpeg);

    public:
        EncoderProbe(const EncoderProbe &) = delete;
        EncoderProbe &operator=(const EncoderProbe &) = delete;
        EncoderProbe(EncoderProbe &&) = delete;
        EncoderProbe &operator=(EncoderProbe &&) = delete;

        static EncoderProbe &get(FFmpeg &ffmpeg) {
            static EncoderProbe instance{ffmpeg};

            return instance;
        }

        [[nodiscard]] static bool is_acceleration_available();

        [[nodiscard]] KasmVideoEncoders::Encoder select_best_encoder() const {
            return best_encoder;
        }

        [[nodiscard]] const std::vector<KasmVideoEncoders::Encoder> &get_available_encoders() const {
            return available_encoders;
        }
    };

    inline static const std::vector<KasmVideoEncoders::Encoder>& available_encoders = EncoderProbe::get(FFmpeg::get()).get_available_encoders();
    inline static const KasmVideoEncoders::Encoder best_encoder = EncoderProbe::get(FFmpeg::get()).select_best_encoder();
} // namespace rfb::video_encoders
