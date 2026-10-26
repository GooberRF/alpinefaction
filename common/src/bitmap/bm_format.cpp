#include <cassert>
#include <stdexcept>
#include <vector>
#include <xlog/xlog.h>
#include <common/config/BuildConfig.h>
#include <common/scope_guard.h>
#include <common/utils/perf-utils.h>
#include <gr_d3d11_engine.h>
#include <common/bitmap/bm_format.h>
#include <common/bitmap/fmt_conv_templates.h>

int bm_calculate_pitch(int w, rf::bm::Format format)
{
    switch (format) {
        case rf::bm::FORMAT_8888_ARGB:
            return w * 4;
        case rf::bm::FORMAT_888_RGB:
            return w * 3;
        case rf::bm::FORMAT_565_RGB:
        case rf::bm::FORMAT_1555_ARGB:
        case rf::bm::FORMAT_4444_ARGB:
            return w * 2;
        case rf::bm::FORMAT_8_ALPHA:
        case rf::bm::FORMAT_8_PALETTED:
            return w;
        case rf::bm::FORMAT_DXT1:
            // 4x4 pixels per block, 64 bits per block
            return (w + 3) / 4 * 64 / 8;
        case rf::bm::FORMAT_DXT2:
        case rf::bm::FORMAT_DXT3:
        case rf::bm::FORMAT_DXT4:
        case rf::bm::FORMAT_DXT5:
            // 4x4 pixels per block, 128 bits per block
            return (w + 3) / 4 * 128 / 8;
        default:
            xlog::warn("Unknown format {} in bm_calculate_pitch", static_cast<int>(format));
            return -1;
    }
}

int bm_calculate_rows(int h, rf::bm::Format format)
{
    switch (format) {
        case rf::bm::FORMAT_DXT1:
        case rf::bm::FORMAT_DXT2:
        case rf::bm::FORMAT_DXT3:
        case rf::bm::FORMAT_DXT4:
        case rf::bm::FORMAT_DXT5:
            // 4x4 pixels per block
            return (h + 3) / 4;
        default:
            return h;
    }
}

bool bm_is_compressed_format(rf::bm::Format format)
{
    switch (format) {
        case rf::bm::FORMAT_DXT1:
        case rf::bm::FORMAT_DXT2:
        case rf::bm::FORMAT_DXT3:
        case rf::bm::FORMAT_DXT4:
        case rf::bm::FORMAT_DXT5:
            return true;
        default:
            return false;
    }
}

bool bm_convert_format(void* dst_bits_ptr, rf::bm::Format dst_fmt, const void* src_bits_ptr,
                           rf::bm::Format src_fmt, int width, int height, int dst_pitch, int src_pitch,
                           const uint8_t* palette)
{
#if DEBUG_PERF
    static auto& color_conv_perf = PerfAggregator::create("bm_convert_format");
    ScopedPerfMonitor mon{color_conv_perf};
#endif
    try {
        call_with_format(src_fmt, [=](auto s) {
            call_with_format(dst_fmt, [=](auto d) {
                SurfacePixelFormatConverter<decltype(s)::value, decltype(d)::value> conv{
                    src_bits_ptr, dst_bits_ptr,
                    static_cast<size_t>(src_pitch), static_cast<size_t>(dst_pitch),
                    static_cast<size_t>(width), static_cast<size_t>(height),
                    palette,
                };
                conv();
            });
        });
        return true;
    }
    catch (const std::exception& e) {
        xlog::error("Pixel format conversion failed ({} -> {}): {}", static_cast<int>(src_fmt), static_cast<int>(dst_fmt), e.what());
        return false;
    }
}

rf::gr::Color bm_get_pixel(uint8_t* data, rf::bm::Format format, int stride_in_bytes, int x, int y)
{
    if (bm_is_compressed_format(format)) {
        constexpr int block_w = 4;
        constexpr int block_h = 4;
        auto bytes_per_block = bm_get_bytes_per_compressed_block(format);
        auto* block = data + (y / block_h) * stride_in_bytes + x / block_w * bytes_per_block;
        return decode_block_compressed_pixel(block, format, x % block_w, y % block_h);
    }
    auto* ptr = data + y * stride_in_bytes + x * bm_bytes_per_pixel(format);
    rf::Color result{0, 0, 0, 0};
    call_with_format(format, [&](auto s) {
        if constexpr (decltype(s)::value == rf::bm::FORMAT_8_PALETTED) {
            assert(false);
            return;
        }
        PixelsReader<decltype(s)::value> rdr{ptr};
        PixelColor<rf::bm::FORMAT_8888_ARGB> color = rdr.read();
        result.set(
            static_cast<uint8_t>(color.r.value),
            static_cast<uint8_t>(color.g.value),
            static_cast<uint8_t>(color.b.value),
            static_cast<uint8_t>(color.a.value)
        );
    });
    return result;
}

bool bm_read_8888(int bm_handle, int w, int h, std::vector<uint8_t>& out)
{
    rf::ubyte* bits = nullptr;
    rf::ubyte* pal = nullptr;
    const rf::bm::Format fmt = rf::bm::lock(bm_handle, &bits, &pal);
    if (fmt == rf::bm::FORMAT_NONE || !bits) {
        return false;
    }
    ScopeGuard unlock{[bm_handle] { rf::bm::unlock(bm_handle); }};

    out.resize(static_cast<size_t>(w) * h * 4);
    const int pitch = bm_calculate_pitch(w, fmt);
    if (pitch <= 0) {
        return false;
    }
    if (fmt == rf::bm::FORMAT_DXT1 || fmt == rf::bm::FORMAT_DXT3 || fmt == rf::bm::FORMAT_DXT5) {
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                const rf::gr::Color c = bm_get_pixel(bits, fmt, pitch, x, y);
                uint8_t* p = &out[(static_cast<size_t>(y) * w + x) * 4];
                p[0] = c.blue;
                p[1] = c.green;
                p[2] = c.red;
                p[3] = c.alpha;
            }
        }
        return true;
    }
    return !bm_is_compressed_format(fmt)
        && bm_convert_format(out.data(), rf::bm::FORMAT_8888_ARGB, bits, fmt, w, h, w * 4, pitch, pal);
}
