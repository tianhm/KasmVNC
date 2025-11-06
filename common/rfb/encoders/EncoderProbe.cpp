#include "EncoderProbe.h"
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <vector>
#include "KasmVideoConstants.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
}
#include "rfb/ffmpeg.h"

namespace rfb::video_encoders {
    const std::vector<KasmVideoEncoders::Encoder> &available_encoders = EncoderProbe::get(FFmpeg::get()).get_available_encoders();
    const KasmVideoEncoders::Encoder best_encoder = EncoderProbe::get(FFmpeg::get()).select_best_encoder();
    const std::string_view drm_device_path = EncoderProbe::get(FFmpeg::get()).get_drm_device_path();

    struct EncoderCandidate {
        KasmVideoEncoders::Encoder encoder;
        AVCodecID codec_id;
        AVHWDeviceType hw_type;
    };

    static std::array<EncoderCandidate, 6> candidates = {
        {
         //{KasmVideoEncoders::Encoder::h264_nvenc, AV_CODEC_ID_H264, AV_HWDEVICE_TYPE_VAAPI}
            //{KasmVideoEncoders::Encoder::av1_vaapi, AV_CODEC_ID_AV1, AV_HWDEVICE_TYPE_VAAPI},
            //{KasmVideoEncoders::Encoder::hevc_vaapi, AV_CODEC_ID_HEVC, AV_HWDEVICE_TYPE_VAAPI}, // h265
            EncoderCandidate{KasmVideoEncoders::Encoder::h264_ffmpeg_vaapi, AV_CODEC_ID_H264, AV_HWDEVICE_TYPE_VAAPI},
         // EncoderCandidate{KasmVideoEncoders::Encoder::h264_software, AV_CODEC_ID_H264, AV_HWDEVICE_TYPE_NONE}
            //{KasmVideoEncoders::Encoder::av1_software, AV_CODEC_ID_AV1, AV_HWDEVICE_TYPE_NONE},
            //{KasmVideoEncoders::Encoder::h265_software, AV_CODEC_ID_HEVC, AV_HWDEVICE_TYPE_NONE},
        }
    };

    EncoderProbe::EncoderProbe(FFmpeg &ffmpeg_) :
        ffmpeg(ffmpeg_) {
        if (!ffmpeg.is_available()) {
            available_encoders.push_back(KasmVideoEncoders::Encoder::unavailable);
        } else
            probe();


        available_encoders.shrink_to_fit();
        best_encoder = available_encoders.front();
    }

    void EncoderProbe::probe() {
        for (const auto &encoder_candidate: candidates) {
            const AVCodec *codec = ffmpeg.avcodec_find_encoder_by_name(KasmVideoEncoders::to_string(encoder_candidate.encoder));
            if (!codec || codec->type != AVMEDIA_TYPE_VIDEO)
                continue;

            if (encoder_candidate.hw_type != AV_HWDEVICE_TYPE_NONE) {
                if (!ffmpeg.av_codec_is_encoder(codec))
                    continue;

                FFmpeg::BufferGuard hw_ctx_guard;
                AVBufferRef *hw_ctx{};
                hw_ctx_guard.reset(hw_ctx);

                for (const auto *drm_dev_path: drm_device_paths) {
                    const auto err = ffmpeg.av_hwdevice_ctx_create(&hw_ctx, encoder_candidate.hw_type, drm_dev_path, nullptr, 0);
                    if (err < 0) {
                        puts(ffmpeg.get_error_description(err).c_str());

                        continue;
                    }
                    drm_device_path = drm_dev_path;

                    if (encoder_candidate.hw_type == AV_HWDEVICE_TYPE_VAAPI) {
                        printf("DEBUG: Codec: %s\n", codec->name);
                        const FFmpeg::ContextGuard ctx_guard{ffmpeg.avcodec_alloc_context3(codec)};

                        const AVOption *opt{};
                        while (opt = ffmpeg.av_opt_next(ctx_guard->priv_data, opt), opt) {
                            printf("DEBUG: Option: %s.%s (help: %s)\n", codec->name, opt->name, opt->help ? opt->help : "n/a");
                        }
                    }

                    available_encoders.push_back(encoder_candidate.encoder);

                    break;
                }
            }
        }

        available_encoders.push_back(KasmVideoEncoders::Encoder::h264_software);
        available_encoders.push_back(KasmVideoEncoders::Encoder::h265_software);
        // available_encoders.push_back(KasmVideoEncoders::Encoder::av1_software);
    }

    /*bool EncoderProbe::is_acceleration_available() {
        if (access(render_path, R_OK | W_OK) != 0)
            return false;

        const int fd = open(render_path, O_RDWR);
        if (fd < 0)
            return false;

        close(fd);

        return true;
    }*/
} // namespace rfb::video_encoders
