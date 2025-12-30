#include "EncoderConfiguration.h"

namespace rfb {
    static inline std::array<EncoderConfiguration, static_cast<size_t>(KasmVideoEncoders::Encoder::unavailable) + 1> EncoderConfigurations =
        {
            // AV1
            // av1_vaapi
            EncoderConfiguration{0, 0, {}},
            // av1_ffmpeg_vaapi
            EncoderConfiguration{0, 0, {}},
            // av1_nvenc
            EncoderConfiguration{0, 0, {}},
            // av1_software
            EncoderConfiguration{0, 0, {}},

            // H.265
            // h265_vaapi
            EncoderConfiguration{0, 51, {18, 23, 28, 39, 51}},
            // h265_ffmpeg_vaapi
            EncoderConfiguration{0, 51, {18, 23, 28, 39, 51}},
            // h265_nvenc
            EncoderConfiguration{0, 51, {18, 23, 28, 39, 51}},
            // h265_software
            EncoderConfiguration{0, 51, {18, 23, 28, 39, 51}},

            // H.264
            // h264_vaapi
            EncoderConfiguration{0, 51, {18, 23, 28, 33, 51}},
            // h264_ffmpeg_vaapi
            EncoderConfiguration{0, 51, {18, 23, 28, 39, 51}},
            // h264_nvenc
            EncoderConfiguration{0, 51, {18, 23, 28, 39, 51}},
            // h264_software
            EncoderConfiguration{0, 51, {18, 23, 28, 39, 51}},

            EncoderConfiguration{}
    };

    // Compile-time check: EncoderConfigurations must match Encoder enum count (excluding unavailable)
    static_assert(EncoderConfigurations.size() == static_cast<size_t>(KasmVideoEncoders::Encoder::unavailable) + 1,
        "EncoderSettingsArray size must match KasmVideoEncoders::Encoder enum count.");

    const EncoderConfiguration &EncoderConfiguration::get_configuration(KasmVideoEncoders::Encoder encoder) {
        return EncoderConfigurations[static_cast<uint8_t>(encoder)];
    }
} // namespace rfb
