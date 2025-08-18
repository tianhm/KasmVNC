#include "VideoEncoderFactory.h"

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
        uint32_t id{std::numeric_limits<u_int32_t>::max()};
        const FFmpeg *ffmpeg{};
        int frame_rate{};
        int bit_rate{};
        SConnection *conn{};
        explicit H264EncoderBuilder(const FFmpeg *ffmpeg_) : ffmpeg(ffmpeg_) {}
        H264EncoderBuilder() = default;

    public:
        static H264EncoderBuilder create(const FFmpeg *ffmpeg) {
            return H264EncoderBuilder{ffmpeg};
        }

        static H264EncoderBuilder create() {
            return H264EncoderBuilder{};
        }

        H264EncoderBuilder &with_frame_rate(int value) {
            frame_rate = value;

            return *this;
        }

        H264EncoderBuilder &with_bit_rate(int value) {
            bit_rate = value;

            return *this;
        }

        H264EncoderBuilder &with_connection(SConnection *value) {
            conn = value;

            return *this;
        }

        H264EncoderBuilder &with_id(uint32_t value) {
            id = value;

            return *this;
        }

        Encoder *build() override {
            if (id == std::numeric_limits<u_int32_t>::max())
                throw std::runtime_error("Encoder does not have a valid id");

            if (!conn)
                throw std::runtime_error("Connection is required");

            if constexpr (is_ffmpeg_based<T>::value) {
                if (!ffmpeg)
                    throw std::runtime_error("FFmpeg is required");

                return new T(id, *ffmpeg, conn, frame_rate, bit_rate);
            } else {
                return new T(conn, frame_rate, bit_rate);
            }
        }
    };

    using H264FFMPEGVAAPIEncoderBuilder = H264EncoderBuilder<H264FFMPEGVAAPIEncoder>;
    using H264VAAPIEncoderBuilder = H264EncoderBuilder<H264VAAPIEncoder>;
    using H264SoftwareEncoderBuilder = H264EncoderBuilder<H264SoftwareEncoder>;

    Encoder *create_encoder(u_int32_t id, const FFmpeg *ffmpeg, KasmVideoEncoders::Encoder video_encoder, SConnection *conn,
                            uint8_t frame_rate, uint16_t bit_rate) {
        switch (video_encoder) {
            case KasmVideoEncoders::Encoder::h264_vaapi:
                // return
                // H264VAAPIEncoderBuilder::create().with_connection(conn).with_frame_rate(frame_rate).with_bit_rate(bit_rate).build();
            case KasmVideoEncoders::Encoder::h264_ffmpeg_vaapi:
                return H264FFMPEGVAAPIEncoderBuilder::create(ffmpeg)
                        .with_id(id)
                        .with_connection(conn)
                        .with_frame_rate(frame_rate)
                        .with_bit_rate(bit_rate)
                        .build();
            default:
                return H264SoftwareEncoderBuilder::create(ffmpeg)
                        .with_id(id)
                        .with_connection(conn)
                        .with_frame_rate(frame_rate)
                        .with_bit_rate(bit_rate)
                        .build();
        }
    }
} // namespace rfb
