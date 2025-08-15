#include "ScreenEncoderManager.h"
#include <cassert>
#include "VideoEncoder.h"
#include "VideoEncoderFactory.h"
#include "rfb/benchmark/benchmark.h"
#include "rfb/encodings.h"

namespace rfb {
    ScreenEncoderManager::ScreenEncoderManager(const FFmpeg &ffmpeg_, KasmVideoEncoders::Encoder encoder,
                                               const std::vector<KasmVideoEncoders::Encoder>& encoders, SConnection *conn, uint8_t frame_rate,
                                               uint16_t bit_rate) :
        Encoder(conn, encodingKasmVideo, static_cast<EncoderFlags>(EncoderUseNativePF | EncoderLossy), -1), ffmpeg(ffmpeg_),
        frame_rate(frame_rate), bit_rate(bit_rate), base_video_encoder(encoder), available_encoders(encoders) {}

    ScreenEncoderManager::~ScreenEncoderManager() = default;

    Encoder *ScreenEncoderManager::add_screen(u_int32_t id) const {
        Encoder *encoder{};
        try {
            encoder = create_encoder(id, &ffmpeg, base_video_encoder, conn, frame_rate, bit_rate);
        } catch (const std::exception &e) {
            if (base_video_encoder != KasmVideoEncoders::Encoder::h264_software) {
                vlog.error("Attempting fallback to software encoder due to error: %s", e.what());
                try {
                    encoder = create_encoder(id, &ffmpeg, KasmVideoEncoders::Encoder::h264_software, conn, frame_rate, bit_rate);
                } catch (const std::exception &exception) {
                    vlog.error("Failed to create software encoder: %s", exception.what());
                }
            } else
                vlog.error("Failed to create software encoder: %s", e.what());
        }

        return encoder;
    }

    size_t ScreenEncoderManager::get_screen_count() const {
        return encoders.size();
    }

    void ScreenEncoderManager::remove_screen(Encoder *encoder) {
        std::erase(encoders, encoder);
    }

    Encoder *ScreenEncoderManager::get_screen(size_t idx) const {
        assert(idx < encoders.size());
        return encoders[idx];
    }
    VideoEncoder *ScreenEncoderManager::get_video_encoder(size_t index) const {
        assert(index < encoders.size());
        return reinterpret_cast<VideoEncoder *>(encoders[index]);
    }

    void ScreenEncoderManager::sync_layout(const ScreenSet &layout) {
        const auto count = layout.num_screens();
        const auto encoder_size = encoders.size();

        for (int i = 0; i < count; ++i) {
            const auto &screen = layout.screens[i];

        }
        for (int i = 0; auto &screen: layout) {
            const auto encoder = encoders[i];
            const auto video_encoder = reinterpret_cast<VideoEncoder *>(encoder);
            assert(video_encoder);

            if (video_encoder->getId() != screen.id) {
                delete encoder;
                encoders[i] = add_screen(screen.id);
            }
            ++i;
        }
    }

    bool ScreenEncoderManager::isSupported() {
        auto *base_encoder = encoders.front();
        assert(!base_encoder);
        return base_encoder->isSupported();
    }

    void ScreenEncoderManager::writeRect(const PixelBuffer *pb, const Palette &palette) {
        auto *base_encoder = encoders.front();
        assert(!base_encoder);
        base_encoder->writeRect(pb, palette);
    }

    void ScreenEncoderManager::writeSolidRect(int width, int height, const PixelFormat &pf, const rdr::U8 *colour) {
        auto *base_encoder = encoders.front();
        assert(!base_encoder);
        base_encoder->writeSolidRect(width, height, pf, colour);
    }

} // namespace rfb
