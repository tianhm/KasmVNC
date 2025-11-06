#pragma once

#include <vector>
#include "KasmVideoConstants.h"
#include "rfb/ffmpeg.h"

namespace rfb::video_encoders {
    class EncoderProbe {
        KasmVideoEncoders::Encoder best_encoder{KasmVideoEncoders::Encoder::h264_software};
        std::vector<KasmVideoEncoders::Encoder> available_encoders;
        std::string_view drm_device_path;
        FFmpeg &ffmpeg;

        explicit EncoderProbe(FFmpeg &ffmpeg);
        void probe();
    public:
        EncoderProbe(const EncoderProbe &) = delete;
        EncoderProbe &operator=(const EncoderProbe &) = delete;
        EncoderProbe(EncoderProbe &&) = delete;
        EncoderProbe &operator=(EncoderProbe &&) = delete;

        static EncoderProbe &get(FFmpeg &ffmpeg) {
            static EncoderProbe instance{ffmpeg};

            return instance;
        }

        // [[nodiscard]] static bool is_acceleration_available();

        [[nodiscard]] KasmVideoEncoders::Encoder select_best_encoder() const {
            return best_encoder;
        }

        [[nodiscard]] const std::vector<KasmVideoEncoders::Encoder> &get_available_encoders() const {
            return available_encoders;
        }

        [[nodiscard]] std::string_view get_drm_device_path() const {
            return drm_device_path;
        }
    };

    extern const std::vector<KasmVideoEncoders::Encoder>& available_encoders; // = EncoderProbe::get(FFmpeg::get()).get_available_encoders();
    extern const KasmVideoEncoders::Encoder best_encoder; // = EncoderProbe::get(FFmpeg::get()).select_best_encoder();
    extern const std::string_view drm_device_path; // = EncoderProbe::get(FFmpeg::get()).get_drm_device_path();

} // namespace rfb::video_encoders
