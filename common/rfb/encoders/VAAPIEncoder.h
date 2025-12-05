#pragma once

#include <unistd.h>
#include <va/va.h>


#include "rdr/OutStream.h"
#include "rfb/Encoder.h"
#include "rfb/encoders/VideoEncoder.h"

struct fd_handle {
    int fd;

    fd_handle(int descriptor = -1) : fd(descriptor) {}

    ~fd_handle() {
        if (fd >= 0)
            ::close(fd);
    }

    fd_handle(const fd_handle &) = delete;

    fd_handle &operator=(const fd_handle &) = delete;

    fd_handle(fd_handle &&other) noexcept : fd(other.fd) {
        other.fd = -1;
    }

    fd_handle &operator=(fd_handle &&other) noexcept {
        if (this != &other) {
            if (fd >= 0)
                ::close(fd);
            fd = other.fd;
            other.fd = -1;
        }
        return *this;
    }

    operator int() const noexcept {
        return fd;
    } // implicit cast to int if you want
};

namespace rfb {
    class VAAPIEncoder final : public VideoEncoder {
        static inline VASurfaceAttrib surface_attribs[] = {
                {VASurfaceAttribPixelFormat, VA_SURFACE_ATTRIB_SETTABLE, {VAGenericValueTypeInteger, {VA_FOURCC_RGBX}}},
                {VASurfaceAttribPixelFormat, VA_SURFACE_ATTRIB_SETTABLE, {VAGenericValueTypeInteger, {VA_FOURCC_NV12}}}};

        uint8_t frame_rate{};
        int bpp{};

        fd_handle fd;
        VADisplay dpy;
        VAConfigID config_id;
        VASurfaceID rgb_surface;
        VASurfaceID yuv_surface;

        static void write_compact(rdr::OutStream *os, int value);
        [[nodiscard]] bool init(int width, int height, int dst_width, int dst_height);

    public:
        VAAPIEncoder(uint32_t id, SConnection *conn, uint8_t frame_rate);
        bool isSupported() const override;
        void writeRect(const PixelBuffer *pb, const Palette &palette) override;
        void writeSolidRect(int width, int height, const PixelFormat &pf, const rdr::U8 *colour) override;
        void writeSkipRect() override;
    };
} // namespace rfb
