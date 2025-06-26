#include "EncoderProbe.h"
#include "KasmVideoConstants.h"
#include <fcntl.h>
#include <unistd.h>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}
#include "rfb/ffmpeg.h"

namespace rfb::video_encoders {
    struct EncoderCandidate {
        KasmVideoEncoders::Encoder encoder;
        AVCodecID codec_id;
        AVHWDeviceType hw_type;
    };

    static std::array<EncoderCandidate, 2> candidates = {
        {{KasmVideoEncoders::Encoder::h264_vaapi, AV_CODEC_ID_H264, AV_HWDEVICE_TYPE_VAAPI},
         {KasmVideoEncoders::Encoder::h264_software, AV_CODEC_ID_H264, AV_HWDEVICE_TYPE_NONE}}};

    EncoderProbe::EncoderProbe(FFmpeg &ffmpeg_) : ffmpeg(ffmpeg_) {
        for (const auto &encoder_candidate: candidates) {
            const AVCodec *codec =
                    ffmpeg.avcodec_find_encoder_by_name(KasmVideoEncoders::to_string(encoder_candidate.encoder).data());

            if (!codec)
                continue;

            if (encoder_candidate.hw_type != AV_HWDEVICE_TYPE_NONE) {
                FFmpeg::BufferGuard hw_ctx_guard;
                AVBufferRef *hw_ctx{};
                hw_ctx_guard.reset(hw_ctx);

                if (ffmpeg.av_hwdevice_ctx_create(&hw_ctx, encoder_candidate.hw_type, nullptr, nullptr, 0) < 0) {
                    continue;
                }

                best_encoder = encoder_candidate.encoder;

                break;
            }
        }
    }

    bool EncoderProbe::is_acceleration_available() {
        if (access(render_path, R_OK | W_OK) != 0)
            return false;

        const int fd = open(render_path, O_RDWR);
        if (fd < 0)
            return false;

        close(fd);

        return true;
    }
} // namespace rfb::video_encoders
