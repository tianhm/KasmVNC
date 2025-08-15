#pragma once
#include "KasmVideoConstants.h"
#include "VideoEncoder.h"
#include "rfb/Encoder.h"
#include "rfb/ffmpeg.h"

namespace rfb {
    class ScreenEncoderManager final : public Encoder {
        const FFmpeg &ffmpeg;
        uint8_t frame_rate{};
        uint16_t bit_rate{};

        KasmVideoEncoders::Encoder base_video_encoder;
        std::vector<KasmVideoEncoders::Encoder> available_encoders;
        std::vector<Encoder *> encoders;

        Encoder *add_screen(u_int32_t id) const;
        [[nodiscard]] size_t get_screen_count() const;
        void remove_screen(Encoder *encoder);
        Encoder *get_screen(size_t screen_id) const;
        VideoEncoder *get_video_encoder(size_t index) const;

    public:
        // Iterators
        using iterator = std::vector<Encoder *>::iterator;
        using const_iterator = std::vector<Encoder *>::const_iterator;
        iterator begin() {
            return encoders.begin();
        }
        iterator end() {
            return encoders.end();
        }

        [[nodiscard]] const_iterator cbegin() const {
            return encoders.begin();
        }
        [[nodiscard]] const_iterator cend() const {
            return encoders.end();
        }

        explicit ScreenEncoderManager(const FFmpeg &ffmpeg, KasmVideoEncoders::Encoder encoder,
                                      const std::vector<KasmVideoEncoders::Encoder> &encoders, SConnection *conn, uint8_t frame_rate,
                                      uint16_t bit_rate);
        ~ScreenEncoderManager() override;

        ScreenEncoderManager(const ScreenEncoderManager &) = delete;
        ScreenEncoderManager &operator=(const ScreenEncoderManager &) = delete;
        ScreenEncoderManager(ScreenEncoderManager &&) = delete;
        ScreenEncoderManager &operator=(ScreenEncoderManager &&) = delete;

        void sync_layout(const ScreenSet &layout);

        // Encoder
        bool isSupported() override;

        void writeRect(const PixelBuffer *pb, const Palette &palette) override;
        void writeSolidRect(int width, int height, const PixelFormat &pf, const rdr::U8 *colour) override;
    };
} // namespace rfb
