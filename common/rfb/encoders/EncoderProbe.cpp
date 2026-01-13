/* Copyright (C) 2025 Kasm.  All Rights Reserved.
*
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 * USA.
 */
#include "EncoderProbe.h"
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <vector>
#include "KasmVideoConstants.h"
#include <rfb/LogWriter.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
}
#include "rfb/ffmpeg.h"

namespace rfb::video_encoders {
    static LogWriter vlog("EncoderProbe");

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

    EncoderProbe::EncoderProbe(FFmpeg &ffmpeg_, const std::vector<std::string_view> &parsed_encoders, const char *dri_node) :
        ffmpeg(ffmpeg_) {
        if (!ffmpeg.is_available()) {
            available_encoders.push_back(KasmVideoEncoders::Encoder::unavailable);
        } else {
            auto debug_encoders = [] (const char *msg, const KasmVideoEncoders::Encoders &encoders) {
                std::string encoder_names;

                for (const auto encoder: encoders)
                    encoder_names.append(KasmVideoEncoders::to_string(encoder)).append(" ");

                if (!encoder_names.empty())
                    vlog.debug("%s: %s",msg, encoder_names.c_str());
            };

            const auto encoders = SupportedVideoEncoders::map_encoders(parsed_encoders);
            debug_encoders("CLI-specified video codecs", encoders);

            available_encoders = probe(dri_node);
            debug_encoders("Available encoders", available_encoders);

            available_encoders = SupportedVideoEncoders::filter_available_encoders(encoders, available_encoders);
            debug_encoders("Using CLI-specified video codecs (supported subset)", available_encoders);
        }

        available_encoders.shrink_to_fit();
        if (available_encoders.empty())
            best_encoder = KasmVideoEncoders::Encoder::unavailable;
        else
            best_encoder = available_encoders.front();
    }

    KasmVideoEncoders::Encoders EncoderProbe::probe(const char *dri_node) {
        KasmVideoEncoders::Encoders result{};
        for (const auto &encoder_candidate: candidates) {
            const AVCodec *codec = ffmpeg.avcodec_find_encoder_by_name(KasmVideoEncoders::to_string(encoder_candidate.encoder));
            if (!codec || codec->type != AVMEDIA_TYPE_VIDEO)
                continue;

            if (encoder_candidate.hw_type != AV_HWDEVICE_TYPE_NONE) {
                if (!ffmpeg.av_codec_is_encoder(codec))
                    continue;

                FFmpeg::BufferGuard hw_ctx_guard;
                AVBufferRef *hw_ctx{};

                if (dri_node) {
                    const auto err = ffmpeg.av_hwdevice_ctx_create(&hw_ctx, encoder_candidate.hw_type, dri_node, nullptr, 0);
                    if (err == 0) {
                        hw_ctx_guard.reset(hw_ctx);
                        drm_device_path = dri_node;
                        result.push_back(encoder_candidate.encoder);
                    } else
                        vlog.error("%s", ffmpeg.get_error_description(err).c_str());

                } else {
                    vlog.debug("Trying to open all DRM devices");
                    for (const auto *drm_dev_path: drm_device_paths) {
                        const auto err = ffmpeg.av_hwdevice_ctx_create(&hw_ctx, encoder_candidate.hw_type, drm_dev_path, nullptr, 0);
                        if (err < 0) {
                            vlog.error("%s", ffmpeg.get_error_description(err).c_str());

                            continue;
                        }

                        hw_ctx_guard.reset(hw_ctx);
                        drm_device_path = drm_dev_path;

                        vlog.info("Found DRM device %s", drm_dev_path);

                        if (encoder_candidate.hw_type == AV_HWDEVICE_TYPE_VAAPI) {
                            vlog.debug("DEBUG: Codec: %s\n", codec->name);
                            const FFmpeg::ContextGuard ctx_guard{ffmpeg.avcodec_alloc_context3(codec)};

                            const AVOption *opt{};
                            while (opt = ffmpeg.av_opt_next(ctx_guard->priv_data, opt), opt) {
                                vlog.debug("DEBUG: Option: %s.%s (help: %s)\n", codec->name, opt->name, opt->help ? opt->help : "n/a");
                            }
                        }

                        result.push_back(encoder_candidate.encoder);

                        break;
                    }
                }
            }
        }

        result.push_back(KasmVideoEncoders::Encoder::h264_software);
        result.push_back(KasmVideoEncoders::Encoder::h265_software);
        // result.push_back(KasmVideoEncoders::Encoder::av1_software);

        return result;
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
