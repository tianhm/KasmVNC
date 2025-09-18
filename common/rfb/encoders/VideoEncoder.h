#pragma once

#include <cstdint>
#include <rfb/Encoder.h>

namespace rfb {
    class VideoEncoder {
        uint32_t id;

    public:
        explicit VideoEncoder(uint32_t id_) : id(id_) {}
        virtual Encoder *clone(uint32_t id) = 0;
        void setId(uint32_t id_) {
            id = id_;
        }
        [[nodiscard]] uint32_t getId() const {
            return id;
        }
        virtual void writeSkipRect() = 0;
        virtual ~VideoEncoder() = default;
    };
} // namespace rfb
