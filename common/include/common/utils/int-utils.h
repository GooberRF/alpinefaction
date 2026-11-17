#pragma once

#include <cstdint>

// Signed distance a - b between two wrapping 16-bit counters (ms ticks, sequence numbers); valid within +-32767
inline int wrapped_diff16(uint16_t a, uint16_t b)
{
    return static_cast<int16_t>(static_cast<uint16_t>(a - b));
}
