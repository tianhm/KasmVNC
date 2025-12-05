#include "ScreenEncoderManager.h"
#include <cassert>
#include <rfb/LogWriter.h>
#include <rfb/SMsgWriter.h>
#include <rfb/encodings.h>
#include <tbb/parallel_for_each.h>
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
        available_encoders(encoders) {}

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

        screens[index] = {layout, encoder};
        head = std::min(head, index);
        tail = std::max(tail, index);
        ++count;

        return true;
    }

    template<int T>
    size_t ScreenEncoderManager<T>::get_screen_count() const {
        return count;
    }

    template<int T>
    void ScreenEncoderManager<T>::remove_screen(uint8_t index) {
        if (screens[index].encoder) {
            delete screens[index].encoder;
            screens[index].encoder = nullptr;
            --count;
        }
        screens[index].layout = {};
    }

    template<int T>
    ScreenEncoderManager<T>::stats_t ScreenEncoderManager<T>::get_stats() const {
        return {};
    }

    template<int T>
    bool ScreenEncoderManager<T>::sync_layout(const ScreenSet &layout) {
        for (uint8_t i = 0; i < layout.num_screens(); ++i) {
            const auto &screen = layout.screens[i];
            auto id = screen.id;
            if (id > ScreenSet::MAX_SCREENS) {
                assert("Wrong  id");
                id = 0;
            }

            if (!screens[id].layout.dimensions.equals(screen.dimensions)) {
                remove_screen(id);
                if (!add_screen(id, screen))
                    return false;
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
        if (count > 1) {
            tbb::parallel_for_each(screens.begin(), screens.end(), [&pb](auto &screen) {
                if (auto *encoder = screen.encoder; encoder) {
                    auto *video_encoder = static_cast<VideoEncoder *>(encoder);
                    screen.failed = !video_encoder->render(pb);
                }
            });
        } else {
            auto *video_encoder = static_cast<VideoEncoder *>(screens[head].encoder);
            screens[head].failed = !video_encoder->render(pb);
        }

        for (int index = head; index <= tail; ++index) {
            auto &screen = screens[index];
            if (screen.failed)
                continue;

            if (auto *encoder = screen.encoder; encoder) {

                ++stats.rects;
                const auto &rect = screen.layout.dimensions;
                const auto &area = rect.area();
                stats.pixels += area;
                stats.bytes = static_cast<int>(conn->getOutStream(conn->cp.supportsUdp)->length());

                const int equiv = 12 + (area * conn->cp.pf().bpp >> 3);
                stats.equivalent += equiv;

                conn->writer()->startRect(rect, encoder->encoding);

                encoder->writeRect(pb, palette);

                conn->writer()->endRect();

                stats.bytes += conn->getOutStream(conn->cp.supportsUdp)->length() - stats.bytes;
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
