#pragma once

// Shared bitmap format helpers used by both game_patch and editor_patch. These wrap the few
// bits of logic that are genuinely identical across binaries (DDS pixel-format classification,
// stb_image channel-count → bm format mapping, file-extension predicates). Engine-glue code
// like file IO, hooks, and bm system calls intentionally stays per-binary because of address
// and behavioural differences between RF.exe and RED.exe.
//
// Format constants are returned/expressed as plain `int`. Each binary's own enum
// (rf::bm::Format on the game side, BitmapEntry::FORMAT_* on the editor side) uses the same
// numeric values, so callers cast freely.

#include <cstdint>
#include <string_view>

#include <dds.h>
#include <common/utils/string-utils.h>

// ─── Format constants ─────────────────────────────────────────────────────────
// Match RED.exe's and RF.exe's bm enum values.

inline constexpr int BM_FORMAT_NONE       = 0;
inline constexpr int BM_FORMAT_8_PALETTED = 1;
inline constexpr int BM_FORMAT_8_ALPHA    = 2;
inline constexpr int BM_FORMAT_565_RGB    = 3;
inline constexpr int BM_FORMAT_4444_ARGB  = 4;
inline constexpr int BM_FORMAT_1555_ARGB  = 5;
inline constexpr int BM_FORMAT_888_RGB    = 6;
inline constexpr int BM_FORMAT_8888_ARGB  = 7;
// Alpine extensions (game-only):
inline constexpr int BM_FORMAT_DXT1       = 0x11;
inline constexpr int BM_FORMAT_DXT2       = 0x12;
inline constexpr int BM_FORMAT_DXT3       = 0x13;
inline constexpr int BM_FORMAT_DXT4       = 0x14;
inline constexpr int BM_FORMAT_DXT5       = 0x15;

// ─── Format helpers ──────────────────────────────────────────────────────────

// Returns the bm format integer for a DDS pixel format header, or BM_FORMAT_NONE if the
// pixel format is unsupported.
inline int bm_format_from_dds(const DDS_PIXELFORMAT& ddspf)
{
    if (ddspf.flags & DDS_RGB) {
        switch (ddspf.RGBBitCount) {
            case 32: return ddspf.ABitMask ? BM_FORMAT_8888_ARGB : BM_FORMAT_NONE;
            case 24: return BM_FORMAT_888_RGB;
            case 16:
                if (ddspf.ABitMask == 0x8000) return BM_FORMAT_1555_ARGB;
                if (ddspf.ABitMask)           return BM_FORMAT_4444_ARGB;
                return BM_FORMAT_565_RGB;
        }
    }
    else if (ddspf.flags & DDS_FOURCC) {
        switch (ddspf.fourCC) {
            case MAKEFOURCC('D','X','T','1'): return BM_FORMAT_DXT1;
            case MAKEFOURCC('D','X','T','2'): return BM_FORMAT_DXT2;
            case MAKEFOURCC('D','X','T','3'): return BM_FORMAT_DXT3;
            case MAKEFOURCC('D','X','T','4'): return BM_FORMAT_DXT4;
            case MAKEFOURCC('D','X','T','5'): return BM_FORMAT_DXT5;
        }
    }
    return BM_FORMAT_NONE;
}

// Map stb_image's reported source channel count to the appropriate bm format.
// 1 (greyscale) and 3 (RGB) → BM_FORMAT_888_RGB (no alpha).
// 2 (gray+alpha) and 4 (RGBA) → BM_FORMAT_8888_ARGB.
inline int bm_format_from_stb_channels(int channels)
{
    return (channels == 2 || channels == 4) ? BM_FORMAT_8888_ARGB : BM_FORMAT_888_RGB;
}

// ─── Filename extension predicates ────────────────────────────────────────────
// Used by both game-side and editor-side bm hooks to decide which decode path applies.
// Centralising these means a new format is added in exactly one place.

inline bool is_stb_filename(std::string_view name)
{
    return string_iends_with(name, ".png")
        || string_iends_with(name, ".jpg")
        || string_iends_with(name, ".jpeg");
}
inline bool is_dds_filename(std::string_view name) { return string_iends_with(name, ".dds"); }
inline bool is_atx_filename(std::string_view name) { return string_iends_with(name, ".atx"); }

// ─── Format predicates / mappings ─────────────────────────────────────────────

// True for the set of uncompressed direct-color formats: 565, 1555, 4444, 888, 8888.
// Excludes paletted (indexed-color) and block-compressed (DXT*). Useful for any pipeline
// that reads or writes pixel bytes directly.
inline bool bm_format_is_uncompressed_rgb(int fmt)
{
    switch (fmt) {
        case BM_FORMAT_565_RGB:
        case BM_FORMAT_4444_ARGB:
        case BM_FORMAT_1555_ARGB:
        case BM_FORMAT_888_RGB:
        case BM_FORMAT_8888_ARGB:
            return true;
        default:
            return false;
    }
}

// If `fmt` lacks an alpha channel, return an analogous format with one. Used to auto-promote
// when an alpha mask is supplied without an explicit target format.
inline int bm_promote_to_alpha(int fmt)
{
    switch (fmt) {
        case BM_FORMAT_565_RGB: return BM_FORMAT_4444_ARGB;
        case BM_FORMAT_888_RGB: return BM_FORMAT_8888_ARGB;
        default:                return fmt;
    }
}

// ─── Alpha mask overlay ───────────────────────────────────────────────────────
// Overlay an 8-bit greyscale `mask` as the alpha channel of a destination buffer in
// `dst_fmt`. Caller guarantees dst_fmt is one of the alpha-bearing supported formats
// (8888_ARGB, 4444_ARGB, or 1555_ARGB). Buffer layout matches D3D's *_ARGB: the alpha
// component lives in the highest-order bits of each pixel (memory: BGRA / RGBA-MSB-A).
inline void bm_overlay_alpha_mask(uint8_t* dst, int dst_fmt, const uint8_t* mask, int num_pixels)
{
    switch (dst_fmt) {
        case BM_FORMAT_8888_ARGB:
            // Memory order is BGRA on little-endian; alpha is the 4th byte per pixel.
            for (int i = 0; i < num_pixels; ++i) {
                dst[i * 4 + 3] = mask[i];
            }
            break;
        case BM_FORMAT_4444_ARGB:
            // Two bytes per pixel little-endian; alpha lives in the high nibble of byte 1.
            for (int i = 0; i < num_pixels; ++i) {
                dst[i * 2 + 1] = static_cast<uint8_t>((dst[i * 2 + 1] & 0x0F) | (mask[i] & 0xF0));
            }
            break;
        case BM_FORMAT_1555_ARGB:
            // Single alpha bit at the top of byte 1; threshold the mask at 0x80.
            for (int i = 0; i < num_pixels; ++i) {
                if (mask[i] >= 128) dst[i * 2 + 1] |= 0x80;
                else                dst[i * 2 + 1] &= 0x7F;
            }
            break;
        default:
            break;
    }
}
