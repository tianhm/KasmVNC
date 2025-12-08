#include "ScreenEncoderManager.h"
#include <cassert>
#include <rfb/LogWriter.h>
#include <rfb/Region.h>
#include <rfb/SMsgWriter.h>
#include <rfb/encodings.h>
#include <tbb/parallel_for_each.h>
#include <sys/stat.h>
#include "VideoEncoder.h"
#include "VideoEncoderFactory.h"

namespace rfb {
    static LogWriter vlog("ScreenEncoderManager");

    template<int T>
    ScreenEncoderManager<T>::ScreenEncoderManager(const FFmpeg &ffmpeg_, KasmVideoEncoders::Encoder encoder,
        const std::vector<KasmVideoEncoders::Encoder> &encoders, SConnection *conn, const char *dri_node, VideoEncoderParams params) :
        Encoder(conn, encodingKasmVideo, static_cast<EncoderFlags>(EncoderUseNativePF | EncoderLossy), -1),
        ffmpeg(ffmpeg_),
        current_params(params),
        base_video_encoder(encoder),
        available_encoders(encoders) {
        active_screens.reserve(T);
    }

    template<int T>
    ScreenEncoderManager<T>::~ScreenEncoderManager() {
        for (uint8_t i = 0; i < get_screen_count(); ++i)
            remove_screen(i);
    };

    template<int T>
    VideoEncoder *ScreenEncoderManager<T>::add_encoder(const Screen &layout) const {
        VideoEncoder *encoder{};
        try {
            encoder = create_encoder(layout, &ffmpeg, conn, base_video_encoder, dri_node, current_params);
        } catch (const std::exception &e) {
            if (base_video_encoder != KasmVideoEncoders::Encoder::h264_software) {
                vlog.error("Attempting fallback to software encoder due to error: %s", e.what());
                try {
                    encoder = create_encoder(layout, &ffmpeg, conn, KasmVideoEncoders::Encoder::h264_software, nullptr, current_params);
                } catch (const std::exception &exception) {
                    vlog.error("Failed to create software encoder: %s", exception.what());
                }
            } else
                vlog.error("Failed to create software encoder: %s", e.what());
        }

        return encoder;
    }

    template<int T>
    bool ScreenEncoderManager<T>::add_screen(uint8_t index, const Screen &layout) {
        printf("SCREEN ADDED: %d (%d, %d, %d, %d)\n",
            index,
            layout.dimensions.tl.x,
            layout.dimensions.tl.y,
            layout.dimensions.br.x,
            layout.dimensions.br.y);
        auto *encoder = add_encoder(layout);
        if (!encoder)
            return false;

        mask |= 1 << index;

        screens[index] = {layout, encoder, true};
        head = std::min(head, index);
        tail = std::max(tail, index);
        update_active_screens();

        return true;
    }

    template<int T>
    size_t ScreenEncoderManager<T>::get_screen_count() const {
        return active_screens.size();
    }

    template<int T>
    void ScreenEncoderManager<T>::remove_screen(uint8_t index) {
        if (screens[index].encoder) {
            delete screens[index].encoder;
            screens[index].encoder = nullptr;

            mask &= ~(1 << index);
            update_active_screens();
        }
        screens[index] = {};
    }

    template<int T>
    void ScreenEncoderManager<T>::update_active_screens() {
        active_screens.clear();

        for (uint8_t i = 0; i < T; ++i) {
            if (mask & 1ULL << i)
                active_screens.push_back(i);
        }
    }

    template<int T>
    ScreenEncoderManager<T>::stats_t ScreenEncoderManager<T>::get_stats() const {
        return stats;
    }

    template<int T>
    bool ScreenEncoderManager<T>::sync_layout(const ScreenSet &layout, const Region &region) {
        const auto bounds = region.get_bounding_rect();

        for (uint8_t i = 0; i < layout.num_screens(); ++i) {
            const auto &screen = layout.screens[i];
            auto id = screen.id;
            if (id > T) {
                assert("Wrong  id");
                id = 0;
            }

            if (!screens[id].layout.dimensions.equals(screen.dimensions)) {
                remove_screen(id);
                if (!add_screen(id, screen))
                    return false;
            }

            if (screen.dimensions.overlaps(bounds)) {
                screens[id].dirty = true;
            }
        }

        return true;
    }

    template<int T>
    bool ScreenEncoderManager<T>::isSupported() const {
        if (const auto *encoder = screens[head].encoder; encoder)
            return encoder->isSupported();

        return false;
    }

    template<int T>
    void ScreenEncoderManager<T>::writeRect(const PixelBuffer *pb, const Palette &palette) {
        if (active_screens.empty())
            return;

        const auto send_frame = [this, pb, &palette](const screen_t &screen) {
            ++stats.rects;
            const auto &rect = screen.layout.dimensions;
            const auto area = rect.area();
            stats.pixels += area;
            const auto before = conn->getOutStream(conn->cp.supportsUdp)->length();

            const int equiv = 12 + (area * conn->cp.pf().bpp >> 3);
            stats.equivalent += equiv;

            const auto &encoder = screen.encoder;

            conn->writer()->startRect(rect, encoder->encoding);
            encoder->writeRect(pb, palette);
            conn->writer()->endRect();

            const auto after = conn->getOutStream(conn->cp.supportsUdp)->length();
            stats.bytes += after - before;
        };

        if (active_screens.size() > 1) {
            tbb::parallel_for_each(active_screens.begin(), active_screens.end(), [this, pb, &send_frame](uint8_t index) {
                auto &screen = screens[index];
                if (auto *encoder = screen.encoder; encoder) {
                    if (encoder->render(pb)) {
                        screen.dirty = false;
                        std::lock_guard lock(conn_mutex);
                        send_frame(screen);
                    }
                }
            });
        } else {
            if (auto encoder = screens[head].encoder; encoder) {
                if (encoder->render(pb))
                    send_frame(screens[head]);
            }
        }
    }

    template<int T>
    void ScreenEncoderManager<T>::writeSolidRect(int width, int height, const PixelFormat &pf, const rdr::U8 *colour) {
        for (const auto &screen: screens) {
            if (auto *encoder = screen.encoder; encoder)
                encoder->writeSolidRect(width, height, pf, colour);
        }
    }

} // namespace rfb
