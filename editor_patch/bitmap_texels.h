#pragma once

#include <cstdint>
#include <cstring>
#include "textures.h"

// CPU decoding of the pixels bm_lock hands out, for the bakes that read textures.

inline std::uint32_t texel_argb(int a, int r, int g, int b)
{
    return (static_cast<std::uint32_t>(a) << 24) | (static_cast<std::uint32_t>(r) << 16) |
           (static_cast<std::uint32_t>(g) << 8) | static_cast<std::uint32_t>(b);
}

// 0 for a format the decoder does not read
inline int bitmap_bytes_per_texel(int format)
{
    switch (format) {
    case BitmapEntry::FORMAT_8_PALETTED:
    case BitmapEntry::FORMAT_8_ALPHA:
        return 1;
    case BitmapEntry::FORMAT_565_RGB:
    case BitmapEntry::FORMAT_4444_ARGB:
    case BitmapEntry::FORMAT_1555_ARGB:
        return 2;
    case BitmapEntry::FORMAT_888_RGB:
        return 3;
    case BitmapEntry::FORMAT_8888_ARGB:
        return 4;
    default:
        return 0;
    }
}

// Decodes `count` texels to 0xAARRGGBB. Channel layouts match the D3D texture upload 0x004F4C50.
inline void bitmap_decode_texels(int format, const std::uint8_t* src, const std::uint8_t* pal, int count,
                                 std::uint32_t* out)
{
    auto expand5 = [](int v) { return (v << 3) | (v >> 2); };
    auto expand6 = [](int v) { return (v << 2) | (v >> 4); };
    switch (format) {
    case BitmapEntry::FORMAT_8_PALETTED:
        for (int i = 0; i < count; ++i) {
            const std::uint8_t* c = pal + src[i] * 3;
            out[i] = texel_argb(255, c[0], c[1], c[2]);
        }
        break;
    case BitmapEntry::FORMAT_8_ALPHA:
        for (int i = 0; i < count; ++i) out[i] = texel_argb(src[i], 255, 255, 255);
        break;
    case BitmapEntry::FORMAT_565_RGB:
    case BitmapEntry::FORMAT_4444_ARGB:
    case BitmapEntry::FORMAT_1555_ARGB:
        for (int i = 0; i < count; ++i) {
            std::uint16_t p;
            std::memcpy(&p, src + i * 2, 2);
            if (format == BitmapEntry::FORMAT_565_RGB) {
                out[i] = texel_argb(255, expand5(p >> 11), expand6((p >> 5) & 0x3F), expand5(p & 0x1F));
            }
            else if (format == BitmapEntry::FORMAT_4444_ARGB) {
                out[i] = texel_argb(((p >> 12) & 0xF) * 17, ((p >> 8) & 0xF) * 17, ((p >> 4) & 0xF) * 17,
                                    (p & 0xF) * 17);
            }
            else {
                out[i] = texel_argb((p & 0x8000) ? 255 : 0, expand5((p >> 10) & 0x1F), expand5((p >> 5) & 0x1F),
                                    expand5(p & 0x1F));
            }
        }
        break;
    case BitmapEntry::FORMAT_888_RGB:
        for (int i = 0; i < count; ++i) {
            const std::uint8_t* c = src + i * 3;
            out[i] = texel_argb(255, c[2], c[1], c[0]);
        }
        break;
    case BitmapEntry::FORMAT_8888_ARGB:
        std::memcpy(out, src, static_cast<std::size_t>(count) * 4);
        break;
    default:
        break;
    }
}
