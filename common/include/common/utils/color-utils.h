#pragma once

#include <cstdint>
#include <tuple>

// A colour packed as 0xRRGGBBAA, as 0-255 ints
inline std::tuple<int, int, int, int> extract_color_components(uint32_t color)
{
    return std::make_tuple(
        (color >> 24) & 0xFF, // Red
        (color >> 16) & 0xFF, // Green
        (color >> 8) & 0xFF,  // Blue
        color & 0xFF          // Alpha
    );
}
