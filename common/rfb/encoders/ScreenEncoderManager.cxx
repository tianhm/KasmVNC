#include "ScreenEncoderManager.h"
#include <cassert>
#include "VideoEncoder.h"
#include "VideoEncoderFactory.h"
#include "rfb/benchmark/benchmark.h"
#include "rfb/encodings.h"

namespace rfb {
    ScreenEncoderManager::ScreenEncoderManager(const FFmpeg &ffmpeg_, KasmVideoEncoders::Encoder encoder,
                                               const std::vector<KasmVideoEncoders::Encoder> &encoders, SConnection *conn,
                                               VideoEncoderParams params) :
        Encoder(conn, encodingKasmVideo, static_cast<EncoderFlags>(EncoderUseNativePF | EncoderLossy), -1), ffmpeg(ffmpeg_),
        current_params(params), base_video_encoder(encoder), available_encoders(encoders) {}

    ScreenEncoderManager::~ScreenEncoderManager() = default;

    Encoder *ScreenEncoderManager::add_screen(const Screen &layout) const {
        Encoder *encoder{};
        try {
            encoder = create_encoder(layout, &ffmpeg, conn, base_video_encoder, current_params);
        } catch (const std::exception &e) {
            if (base_video_encoder != KasmVideoEncoders::Encoder::h264_software) {
                vlog.error("Attempting fallback to software encoder due to error: %s", e.what());
                try {
                    encoder = create_encoder(layout, &ffmpeg, conn, KasmVideoEncoders::Encoder::h264_software, current_params);
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
        const auto new_count = layout.num_screens();
        const auto current_count = static_cast<int>(encoders.size());
        const auto min_count = std::min(new_count, current_count);

        for (size_t i = 0; i < min_count; ++i) {
            auto *encoder = encoders[i];
            if (!encoder || layout.screens[i].id != encoder->getId()) {
                delete encoders[i];
                encoders[i] = add_screen(layout.screens[i]);
            }
        }

        if (new_count < current_count) {
            encoders.erase(encoders.begin() + new_count, encoders.end());
        } else if (new_count > current_count) {
            encoders.reserve(new_count);

            for (size_t i = current_count; i < new_count; ++i) {
                encoders.emplace_back(add_screen(layout.screens[i]));
            }
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
