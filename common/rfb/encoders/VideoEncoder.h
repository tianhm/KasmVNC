#pragma once

#include <rfb/PixelBuffer.h>
#include "rfb/Encoder.h"

namespace rfb {
    struct VideoEncoderParams {
        int width{};
        int height{};
        uint8_t frame_rate{};
        uint8_t group_of_picture{};
        uint8_t quality{};

        bool operator==(const VideoEncoderParams &rhs) const noexcept {
            return width == rhs.width && height == rhs.height && frame_rate == rhs.frame_rate && group_of_picture == rhs.group_of_picture &&
                   quality == rhs.quality;
        }
        bool operator!=(const VideoEncoderParams &rhs) const noexcept {
            return !(*this == rhs);
        }
    };

    class VideoEncoder : public Encoder {
    public:
        VideoEncoder(Id id, SConnection *conn) :
            Encoder(id, conn, encodingKasmVideo, static_cast<EncoderFlags>(EncoderUseNativePF | EncoderLossy), -1) {}
        virtual bool render(const PixelBuffer *pb) = 0;
        virtual void writeSkipRect() = 0;
        ~VideoEncoder() override = default;
    };
} // namespace rfb
