#pragma once

// TGA validation and pixel decoding shared by the game and editor patches of the stock TGA
// loaders (RF.exe 0x0055A6D0/0x0055ABF0, RED.exe 0x004F3910/0x004F3E30). File IO and hooks stay
// per-binary.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

inline constexpr std::size_t tga_header_size = 18;

inline constexpr uint8_t tga_type_color_mapped = 1;
inline constexpr uint8_t tga_type_true_color = 2;
inline constexpr uint8_t tga_type_greyscale = 3;
inline constexpr uint8_t tga_type_rle_color_mapped = 9;
inline constexpr uint8_t tga_type_rle_true_color = 10;
inline constexpr uint8_t tga_type_rle_greyscale = 11;

inline constexpr uint8_t tga_descriptor_alpha_bits_mask = 0x0F;
inline constexpr uint8_t tga_descriptor_top_down = 0x20;

inline constexpr uint8_t tga_rle_run_packet = 0x80;
inline constexpr uint8_t tga_rle_count_mask = 0x7F;

// Image type, bits per pixel and alpha bits the stock pixel loaders accept, and dimensions the
// stock header reader would store as positive.
inline bool tga_header_supported(const uint8_t* hdr)
{
    const uint8_t image_type = hdr[2];
    const auto width = static_cast<int16_t>(hdr[12] | (hdr[13] << 8));
    const auto height = static_cast<int16_t>(hdr[14] | (hdr[15] << 8));
    const uint8_t bpp = hdr[16];
    const uint8_t alpha_bits = hdr[17] & tga_descriptor_alpha_bits_mask;
    const bool type_ok = image_type == tga_type_color_mapped || image_type == tga_type_true_color
        || image_type == tga_type_greyscale || image_type == tga_type_rle_color_mapped
        || image_type == tga_type_rle_true_color || image_type == tga_type_rle_greyscale;
    const bool bpp_ok = bpp == 8 || bpp == 16 || bpp == 24 || bpp == 32;
    const bool alpha_ok = alpha_bits == 0 || alpha_bits == 1 || alpha_bits == 8;
    return type_ok && width > 0 && height > 0 && bpp_ok && alpha_ok;
}

// The stock loaders copy the colormap into the palette unchecked, but the palette is only
// allocated (256 entries) for 8-bit formats.
inline bool tga_colormap_fits(const void* palette, int16_t num_entries)
{
    return palette && num_entries <= 256;
}

// Bounded equivalent of the stock row decode. Rows are stored bottom-up unless top_down is set,
// and an RLE packet may run past its row into the adjacent one as in stock, but nothing is read
// past src_size or written past the image. Pixels a truncated file lacks are left untouched.
inline void tga_decode_pixels(uint8_t* dst, const uint8_t* src, std::size_t src_size, int width, int height,
    int bytes_per_pixel, uint8_t image_type, bool top_down)
{
    const bool rle = image_type == tga_type_rle_color_mapped || image_type == tga_type_rle_true_color;
    const bool raw = image_type == tga_type_color_mapped || image_type == tga_type_true_color;
    if ((!rle && !raw) || width <= 0 || height <= 0 || bytes_per_pixel <= 0) {
        return;
    }
    const std::size_t bpp = static_cast<std::size_t>(bytes_per_pixel);
    const std::size_t pitch = static_cast<std::size_t>(width) * bpp;
    const std::size_t dst_size = pitch * static_cast<std::size_t>(height);
    std::size_t src_pos = 0;

    auto write = [&](std::size_t dst_pos, const uint8_t* data, std::size_t len) {
        if (dst_pos < dst_size) {
            std::memcpy(dst + dst_pos, data, std::min(len, dst_size - dst_pos));
        }
    };

    for (int i = 0; i < height; ++i) {
        std::size_t dst_pos = pitch * static_cast<std::size_t>(top_down ? i : height - 1 - i);
        if (!rle) {
            const std::size_t len = std::min(pitch, src_size - src_pos);
            write(dst_pos, src + src_pos, len);
            src_pos += len;
            if (len < pitch) {
                return;
            }
            continue;
        }
        for (int done = 0; done < width;) {
            if (src_pos == src_size) {
                return;
            }
            const uint8_t packet = src[src_pos++];
            const int count = (packet & tga_rle_count_mask) + 1;
            done += count;
            if (packet & tga_rle_run_packet) {
                if (src_size - src_pos < bpp) {
                    return;
                }
                const uint8_t* pixel = src + src_pos;
                src_pos += bpp;
                for (int k = 0; k < count; ++k, dst_pos += bpp) {
                    write(dst_pos, pixel, bpp);
                }
            }
            else {
                const std::size_t len = static_cast<std::size_t>(count) * bpp;
                const std::size_t avail = std::min(len, src_size - src_pos);
                write(dst_pos, src + src_pos, avail);
                src_pos += avail;
                dst_pos += len;
                if (avail < len) {
                    return;
                }
            }
        }
    }
}
