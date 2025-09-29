#include "VideoEncoderFactory.h"

#include <cstdint>
#include "H264FFMPEGVAAPIEncoder.h"
#include "H264SoftwareEncoder.h"
#include "H264VAAPIEncoder.h"

namespace rfb {
    class EncoderBuilder {
    public:
        virtual Encoder *build() = 0;
        virtual ~EncoderBuilder() = default;
    };

    template<typename T>
    struct is_ffmpeg_based {
        static constexpr auto value = true;
    };

    template<>
    struct is_ffmpeg_based<H264VAAPIEncoder> {
        static constexpr auto value = false;
    };

    template<typename T>
    class H264EncoderBuilder : public EncoderBuilder {
        static constexpr uint32_t INVALID_ID{std::numeric_limits<uint32_t>::max()};
        Screen layout{};
        const FFmpeg *ffmpeg{};
        KasmVideoEncoders::Encoder encoder{};
        VideoEncoderParams params{};
        SConnection *conn{};
        explicit H264EncoderBuilder(const FFmpeg *ffmpeg_) : ffmpeg(ffmpeg_) {
            layout.id = INVALID_ID;
        }
        H264EncoderBuilder() = default;

    public:
        static H264EncoderBuilder create(const FFmpeg *ffmpeg) {
            return H264EncoderBuilder{ffmpeg};
        }

        static H264EncoderBuilder create() {
            return H264EncoderBuilder{};
        }

        H264EncoderBuilder &with_params(VideoEncoderParams value) {
            params = value;

            return *this;
        }

        H264EncoderBuilder &with_encoder(KasmVideoEncoders::Encoder value) {
            encoder = value;

            return *this;
        }

        H264EncoderBuilder &with_connection(SConnection *value) {
            conn = value;

            return *this;
        }

        H264EncoderBuilder &with_id(uint32_t value) {
            layout.id = value;

            return *this;
        }

        H264EncoderBuilder &with_layout(const Screen &layout_) {
            layout = layout_;

            return *this;
        }

        Encoder *build() override {
            if (layout.id == INVALID_ID)
                throw std::runtime_error("Encoder does not have a valid id");

            if (!conn)
                throw std::runtime_error("Connection is required");

            if constexpr (is_ffmpeg_based<T>::value) {
                if (!ffmpeg)
                    throw std::runtime_error("FFmpeg is required");

                return new T(layout, *ffmpeg, conn, encoder, params);
            } else {
                return new T(conn, encoder, params);
            }
        }
    };

    using H264FFMPEGVAAPIEncoderBuilder = H264EncoderBuilder<H264FFMPEGVAAPIEncoder>;
    using H264VAAPIEncoderBuilder = H264EncoderBuilder<H264VAAPIEncoder>;
    using H264SoftwareEncoderBuilder = H264EncoderBuilder<H264SoftwareEncoder>;

    Encoder *create_encoder(const Screen &layout, const FFmpeg *ffmpeg, SConnection *conn, KasmVideoEncoders::Encoder video_encoder,
                            VideoEncoderParams params) {
        switch (video_encoder) {
            case KasmVideoEncoders::Encoder::h264_vaapi:
                // return
                // H264VAAPIEncoderBuilder::create().with_connection(conn).with_frame_rate(frame_rate).with_bit_rate(bit_rate).build();
            case KasmVideoEncoders::Encoder::h264_ffmpeg_vaapi:
                return H264FFMPEGVAAPIEncoderBuilder::create(ffmpeg)
                        .with_layout(layout)
                        .with_connection(conn)
                        .with_encoder(video_encoder)
                        .with_params(params)
                        .build();
            default:
                return H264SoftwareEncoderBuilder::create(ffmpeg)
                        .with_layout(layout)
                        .with_connection(conn)
                        .with_encoder(video_encoder)
                        .with_params(params)
                        .build();
        }
    }
} // namespace rfb
