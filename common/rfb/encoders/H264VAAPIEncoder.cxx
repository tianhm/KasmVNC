#include "H264VAAPIEncoder.h"

#include <fcntl.h>
#include <fmt/format.h>
#include <unistd.h>
#include <utility>
#include "rfb/LogWriter.h"

#include <fcntl.h>
#include <sys/utsname.h>
#include <xf86drm.h>

#include "KasmVideoConstants.h"
#include "rfb/encodings.h"
#include <va/va_drm.h>
#include <memory>
#include <fmt/format.h>

struct drm_version_handler {
    drmVersionPtr version;

    drm_version_handler(drmVersionPtr ptr) : version(ptr) {
    }

    ~drm_version_handler() { drmFreeVersion(version); }

    drm_version_handler(const drm_version_handler &) = delete;

    drm_version_handler &operator=(const drm_version_handler &) = delete;

    drm_version_handler(drm_version_handler &&other) noexcept
        : version(std::exchange(other.version, nullptr)) {
    }

    drm_version_handler &operator=(drm_version_handler &&other) noexcept {
        if (this != &other) {
            drmFreeVersion(std::exchange(version, other.version));
        }
        return *this;
    }

    drmVersionPtr operator->() const { return version; }
    drmVersionPtr operator*() const { return version; }
    operator bool() const { return version ? true : false; }
};

using version_handler = std::unique_ptr<drmVersion, decltype([](drmVersionPtr ptr) { drmFreeVersion(ptr); })>;

static rfb::LogWriter vlog("H264VAAPIEncoder");

namespace rfb {
    H264VAAPIEncoder::H264VAAPIEncoder(uint32_t id, SConnection *conn, uint8_t frame_rate_,
                                       uint16_t bit_rate_) : Encoder(conn, encodingKasmVideo,
                                                                     static_cast<EncoderFlags>(
                                                                         EncoderUseNativePF | EncoderLossy), -1),
                                                             frame_rate(frame_rate_), bit_rate(bit_rate_) {
        static constexpr std::array<std::string_view, 4> drm_device_paths = {
            "/dev/dri/renderD128",
            "/dev/dri/card0",
            "/dev/dri/renderD129",
            "/dev/dri/card1",
        };

        for (auto &path: drm_device_paths) {
            vlog.info("Trying to open %s", path.data());
            fd = open(path.data(), O_RDWR);
            if (fd < 0)
                continue;

            drm_version_handler ver{drmGetVersion(fd)};
            if (!ver)
                continue;

            /* On normal Linux platforms we do not want vgem.
            *  Yet Windows subsystem for linux uses vgem,
            *  while also providing a fallback VA driver.
            *  See https://github.com/intel/libva/pull/688
            */
            utsname sysinfo = {};
            if (!strncmp(ver->name, "vgem", 4) && uname(&sysinfo) >= 0 &&
                !strstr(sysinfo.release, "WSL")) {
                continue;
            }

            dpy = vaGetDisplayDRM(fd);
            int major, minor;

            if (auto res = vaInitialize(dpy, &major, &minor); res != VA_STATUS_SUCCESS)
                throw std::runtime_error("vaInitialize failed");

            break;
        }


        VAConfigAttrib config_attrib = {VAConfigAttribRTFormat, VA_RT_FORMAT_RGB32 | VA_RT_FORMAT_YUV420};

        if (VAStatus va_status = vaCreateConfig(dpy, VAProfileNone, VAEntrypointVLD, &config_attrib, 1, &config_id); va_status != VA_STATUS_SUCCESS) {
            throw std::runtime_error(fmt::format("H264 VA-API Encoder vaCreateConfig failed: {}", vaErrorStr(va_status)));
        }



        ;
        if (VAStatus va_status = vaCreateConfig(dpy, VAProfileNone, VAEntrypointVLD, &config_attrib, 1, &config_id);
            va_status != VA_STATUS_SUCCESS) {
            throw std::runtime_error(
                fmt::format("H264 VA-API Encoder vaCreateConfig failed: {}", vaErrorStr(va_status)));
        };

        /*
        {
            int create_surfaces() {
                VAConfigAttrib config_attrib = { VAConfigAttribRTFormat, VA_RT_FORMAT_RGB32 | VA_RT_FORMAT_YUV420 };
                VAConfigID config_id;

                va_status = vaCreateConfig(va_display, VAProfileNone, VAEntrypointVLD, &config_attrib, 1, &config_id);
                if (va_status != VA_STATUS_SUCCESS) {
                    fprintf(stderr, "Failed to create config: %s\n", vaErrorStr(va_status));
                    return -1;
                }

                VASurfaceAttrib surface_attribs[] = {
                    { VASurfaceAttribPixelFormat, sizeof(int), 0, VA_FOURCC_RGBX },
                    { VASurfaceAttribPixelFormat, sizeof(int), 0, VA_FOURCC_NV12 }
                };

                va_status = vaCreateSurfaces(va_display, VA_RT_FORMAT_RGB32, width, height, &rgb_surface, 1, &surface_attribs[0], 1);
                if (va_status != VA_STATUS_SUCCESS) {
                    fprintf(stderr, "Failed to create RGB surface: %s\n", vaErrorStr(va_status));
                    return -1;
                }

                va_status = vaCreateSurfaces(va_display, VA_RT_FORMAT_YUV420, width, height, &yuv_surface, 1, &surface_attribs[1], 1);
                if (va_status != VA_STATUS_SUCCESS) {
                    fprintf(stderr, "Failed to create YUV surface: %s\n", vaErrorStr(va_status));
                    return -1;
                }

                return 0;
            }
        }*/

        /*frame->pts = 0;
        sw_frame_guard.reset(frame);

        ctx->time_base = {1, frame_rate};
        ctx->framerate = {frame_rate, 1};
        ctx->gop_size = GroupOfPictureSize; // interval between I-frames
        ctx->max_b_frames = 0; // No B-frames for immediate output
        ctx->pix_fmt = AV_PIX_FMT_VAAPI;

///
        auto *hw_frames_ctx = ffmpeg.av_hwframe_ctx_alloc(hw_device_ctx);
        if (!hw_frames_ctx)
            throw std::runtime_error("Failed to create VAAPI frame context");
        hw_frames_ref_guard.reset(hw_frames_ctx);
///
        frame = ffmpeg.av_frame_alloc();
        if (!frame)
            throw std::runtime_error("Cannot allocate hw AVFrame");
        hw_frame_guard.reset(frame);

        auto *pkt = ffmpeg.av_packet_alloc();
        if (!pkt) {
            throw std::runtime_error("Could not allocate packet");
        }
        pkt_guard.reset(pkt);*/
    }

    void H264VAAPIEncoder::write_compact(rdr::OutStream *os, int value) {
        auto b = value & 0x7F;
        if (value <= 0x7F) {
            os->writeU8(b);
        } else {
            os->writeU8(b | 0x80);
            b = value >> 7 & 0x7F;
            if (value <= 0x3FFF) {
                os->writeU8(b);
            } else {
                os->writeU8(b | 0x80);
                os->writeU8(value >> 14 & 0xFF);
            }
        }
    }

    bool H264VAAPIEncoder::init(int width, int height, int dst_width, int dst_height) {
        VAStatus va_status = vaCreateSurfaces(dpy, VA_RT_FORMAT_RGB32, width, height, &rgb_surface, 1,
                                              &surface_attribs[0], 1);
        if (va_status != VA_STATUS_SUCCESS) {
            fprintf(stderr, "Failed to create RGB surface: %s\n", vaErrorStr(va_status));
            return false;
        }

        va_status = vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, width, height, &yuv_surface, 1, &surface_attribs[1], 1);
        if (va_status != VA_STATUS_SUCCESS) {
            fprintf(stderr, "Failed to create YUV surface: %s\n", vaErrorStr(va_status));
            return false;
        }
        return false;
        /*AVHWFramesContext *frames_ctx{};
        int err{};

        ctx_guard->width = dst_width;
        ctx_guard->height = dst_height;

        auto *hw_frames_ctx = ffmpeg.av_hwframe_ctx_alloc(hw_device_ctx_guard.get());
        if (!hw_frames_ctx) {
            vlog.error("Failed to create VAAPI frame context");
            return false;
        }

        hw_frames_ref_guard.reset(hw_frames_ctx);

        //auto hw_frames_ctx = hw_frames_ref_guard.get();
        frames_ctx = reinterpret_cast<AVHWFramesContext *>(hw_frames_ctx->data);
        frames_ctx->format = AV_PIX_FMT_VAAPI;
        frames_ctx->sw_format = AV_PIX_FMT_NV12;
        frames_ctx->width = dst_width;
        frames_ctx->height = dst_height;
        frames_ctx->initial_pool_size = 20;
        if (err = ffmpeg.av_hwframe_ctx_init(hw_frames_ctx); err < 0) {
            vlog.error("Failed to initialize VAAPI frame context (%s). Error code: %d", ffmpeg.get_error_description(err).c_str(), err);
            return false;
        }

        ctx_guard->hw_frames_ctx = ffmpeg.av_buffer_ref(hw_frames_ctx);
        if (!ctx_guard->hw_frames_ctx) {
            vlog.error("Failed to create buffer reference");
            return false;
        }


        if (err = ffmpeg.avcodec_open2(ctx_guard.get(), codec, nullptr); err < 0) {
            vlog.error("Failed to open codec (%s). Error code: %d", ffmpeg.get_error_description(err).c_str(), err);
            return false;
        }

        auto *sws_ctx = ffmpeg.sws_getContext(
                width, height, AV_PIX_FMT_RGB32, dst_width, dst_height, AV_PIX_FMT_NV12, SWS_BILINEAR, nullptr, nullptr, nullptr);

        sws_guard.reset(sws_ctx);

        auto *frame = sw_frame_guard.get();
        frame->format = AV_PIX_FMT_NV12;
        frame->width = dst_width;
        frame->height = dst_height;
        frame->pict_type = AV_PICTURE_TYPE_I;

        if (ffmpeg.av_frame_get_buffer(frame, 0) < 0) {
            vlog.error("Could not allocate sw-frame data");
            return false;
        }

        if (err = ffmpeg.av_hwframe_get_buffer(ctx_guard->hw_frames_ctx, hw_frame_guard.get(), 0); err < 0) {
            vlog.error("Could not allocate hw-frame data (%s). Error code: %d", ffmpeg.get_error_description(err).c_str(), err);
            return false;
        }

        return true;*/
    }

    bool H264VAAPIEncoder::isSupported() {
        return conn->cp.supportsEncoding(encodingKasmVideo);
    }

    void H264VAAPIEncoder::writeRect(const PixelBuffer *pb, const Palette &palette) {
        // compress
        /*int stride;
        const auto rect = pb->getRect();
        const auto *buffer = pb->getBuffer(rect, &stride);

        const int width = rect.width();
        const int height = rect.height();
        auto *frame = sw_frame_guard.get();

        int dst_width = width;
        int dst_height = height;

        if (width % 2 != 0)
            dst_width = width & ~1;

        if (height % 2 != 0)
            dst_height = height & ~1;

        if (frame->width != dst_width || frame->height != dst_height) {
            bpp = pb->getPF().bpp >> 3;
            if (!init(width, height, dst_width, dst_height)) {
                vlog.error("Failed to initialize encoder");
                return;
            }
        } else {
            frame->pict_type = AV_PICTURE_TYPE_NONE;
        }

        const uint8_t *src_data[1] = {buffer};
        const int src_line_size[1] = {stride * bpp}; // RGB has bpp bytes per pixel

        int err{};
        if (err = ffmpeg.sws_scale(sws_guard.get(), src_data, src_line_size, 0, dst_height, frame->data, frame->linesize); err < 0) {
            vlog.error("Error (%s) while scaling image. Error code: %d", ffmpeg.get_error_description(err).c_str(), err);
            return;
        }

        if (err = ffmpeg.av_hwframe_transfer_data(hw_frame_guard.get(), frame, 0); err < 0) {
            vlog.error(
                    "Error while transferring frame data to surface (%s). Error code: %d", ffmpeg.get_error_description(err).c_str(), err);
        }

        if (err = ffmpeg.avcodec_send_frame(ctx_guard.get(), hw_frame_guard.get()); err < 0) {
            vlog.error("Error sending frame to codec (%s). Error code: %d", ffmpeg.get_error_description(err).c_str(), err);
            return;
        }

        auto *pkt = pkt_guard.get();

        err = ffmpeg.avcodec_receive_packet(ctx_guard.get(), pkt);
        if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) {
            // Trying again
            ffmpeg.avcodec_send_frame(ctx_guard.get(), hw_frame_guard.get());
            err = ffmpeg.avcodec_receive_packet(ctx_guard.get(), pkt);
        }

        if (err < 0) {
            vlog.error("Error receiving packet from codec");
            return;
        }

        if (pkt->flags & AV_PKT_FLAG_KEY)
            vlog.debug("Key frame %ld", frame->pts);

        auto *os = conn->getOutStream(conn->cp.supportsUdp);
        os->writeU8(kasmVideoH264 << 4);
        os->writeU8(pkt->flags & AV_PKT_FLAG_KEY);
        write_compact(os, pkt->size);
        os->writeBytes(&pkt->data[0], pkt->size);
        vlog.debug("Frame size:  %d", pkt->size);

        ++frame->pts;
        ffmpeg.av_packet_unref(pkt);*/
    }

    void H264VAAPIEncoder::writeSolidRect(int width, int height, const PixelFormat &pf, const rdr::U8 *colour) {
    }

    void H264VAAPIEncoder::writeSkipRect() {
        auto *os = conn->getOutStream(conn->cp.supportsUdp);
        os->writeU8(kasmVideoSkip << 4);
    }
} // namespace rfb

