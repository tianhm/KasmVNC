#pragma once

#include "rdr/OutStream.h"

namespace rfb::encoders {
    void write_compact(rdr::OutStream *os, int value);
} // namespace rfb::encoders
