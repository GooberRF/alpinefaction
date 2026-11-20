#pragma once

#include <cstdint>

// Signed distance a - b between two wrapping 16-bit counters (ms ticks, sequence numbers); valid within +-32767
inline int wrapped_diff16(uint16_t a, uint16_t b)
{
    return static_cast<int16_t>(static_cast<uint16_t>(a - b));
}

// `value` stepped by `step` within 0..count-1, wrapping at either end; `count` > 0
inline int wrap_step(int value, int step, int count)
{
    return ((value + step) % count + count) % count;
}
