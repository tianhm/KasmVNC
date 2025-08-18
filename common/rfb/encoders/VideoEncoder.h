#pragma once

#include <rfb/Encoder.h>

namespace rfb {
    class VideoEncoder {
        u_int32_t id;

    public:
        explicit VideoEncoder(u_int32_t id_) : id(id_) {}
        virtual Encoder *clone(u_int32_t id) = 0;
        void setId(uint32_t id_) {
            id = id_;
        }
        [[nodiscard]] u_int32_t getId() const {
            return id;
        }
        virtual void writeSkipRect() = 0;
        virtual ~VideoEncoder() = default;
    };
} // namespace rfb
