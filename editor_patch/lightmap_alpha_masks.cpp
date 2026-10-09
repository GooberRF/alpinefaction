#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>
#include <xlog/xlog.h>
#include "lightmap_alpha_masks.h"
#include "bitmap_texels.h"
#include "textures.h"
#include "vtypes.h"

namespace
{

// Larger textures are box reduced to this edge, finer than any lightmap texel resolves.
constexpr int max_mask_dim = 1024;
// Taps per axis over a footprint wider than a levels[0] texel. Each samples a mip this many times finer than
// the footprint, so an atlas neighbour bleeds in no further than a fraction of it.
constexpr int footprint_taps = 4;
constexpr int max_source_dim = 8192;

std::map<int, std::unique_ptr<LightmapAlphaMask>> g_masks;

LightmapAlphaMask::Level downsample(const LightmapAlphaMask::Level& s)
{
    LightmapAlphaMask::Level d;
    d.w = std::max(1, s.w / 2);
    d.h = std::max(1, s.h / 2);
    d.a.resize(static_cast<std::size_t>(d.w) * d.h);
    for (int y = 0; y < d.h; ++y) {
        const int y0 = std::min(y * 2, s.h - 1);
        const int y1 = std::min(y * 2 + 1, s.h - 1);
        for (int x = 0; x < d.w; ++x) {
            const int x0 = std::min(x * 2, s.w - 1);
            const int x1 = std::min(x * 2 + 1, s.w - 1);
            const unsigned sum = s.a[y0 * s.w + x0] + s.a[y0 * s.w + x1] + s.a[y1 * s.w + x0] + s.a[y1 * s.w + x1];
            d.a[y * d.w + x] = static_cast<std::uint8_t>((sum + 2) / 4);
        }
    }
    return d;
}

std::unique_ptr<LightmapAlphaMask> decode(int handle)
{
    if (handle < 0 || BitmapEntry::handle_to_index(handle) < 0 || bm_has_alpha(handle) == 0) {
        return nullptr;
    }
    int w = 0, h = 0, num_pixels = 0, num_levels = 0;
    bm_get_mipmap_info(handle, &w, &h, &num_pixels, &num_levels);
    if (w <= 0 || h <= 0 || w > max_source_dim || h > max_source_dim) {
        return nullptr;
    }
    int factor = 1;
    while (w / factor > max_mask_dim || h / factor > max_mask_dim) {
        factor *= 2;
    }
    auto mask = std::make_unique<LightmapAlphaMask>();
    LightmapAlphaMask::Level base;
    base.w = std::max(1, w / factor);
    base.h = std::max(1, h / factor);
    // everything allocates before the lock, so nothing between lock and unlock can throw
    std::vector<std::uint32_t> sums(static_cast<std::size_t>(base.w) * base.h, 0u);
    std::vector<std::uint32_t> counts(sums.size(), 0u);
    std::vector<std::uint32_t> row(static_cast<std::size_t>(w));
    base.a.resize(sums.size());
    bool decoded = false;
    void* pixels = nullptr;
    void* palette = nullptr;
    const int format = bm_lock(handle, &pixels, &palette);
    if (format != 0) {
        const int bpp = bitmap_bytes_per_texel(format);
        if (pixels && bpp != 0 && (format != BitmapEntry::FORMAT_8_PALETTED || palette)) {
            const auto* src = static_cast<const std::uint8_t*>(pixels);
            const auto* pal = static_cast<const std::uint8_t*>(palette);
            for (int y = 0; y < h; ++y) {
                bitmap_decode_texels(format, src + static_cast<std::size_t>(y) * w * bpp, pal, w, row.data());
                const int oy = std::min(y / factor, base.h - 1);
                for (int x = 0; x < w; ++x) {
                    const std::size_t o = static_cast<std::size_t>(oy) * base.w + std::min(x / factor, base.w - 1);
                    sums[o] += row[x] >> 24;
                    counts[o]++;
                }
            }
            decoded = true;
        }
        bm_unlock(handle);
    }
    if (!decoded) {
        const char* name = bm_get_filename(handle);
        xlog::warn("Lightmap: cannot read the alpha of texture '{}' for alpha-tested light occlusion",
                   name ? name : "?");
        return nullptr;
    }
    for (std::size_t i = 0; i < sums.size(); ++i) {
        base.a[i] = static_cast<std::uint8_t>(counts[i] ? (sums[i] + counts[i] / 2) / counts[i] : 255u);
    }
    mask->levels.push_back(std::move(base));
    while (mask->levels.back().w > 1 || mask->levels.back().h > 1) {
        mask->levels.push_back(downsample(mask->levels.back()));
    }
    return mask;
}

float wrap01(float f)
{
    if (!std::isfinite(f)) {
        return 0.0f;
    }
    return f - std::floor(f);
}

// The texels of a level whose centres lie in a rect, at least the one nearest its centre.
struct TexelRange
{
    int x0, x1, y0, y1;
};

TexelRange texels_in(const LightmapAlphaMask::Level& l, const LightmapAlphaMask::Rect& r)
{
    auto axis = [](float a, float b, int n, int& lo, int& hi) {
        const float fn = static_cast<float>(n);
        lo = std::clamp(static_cast<int>(std::ceil(a * fn - 0.5f)), 0, n - 1);
        hi = std::clamp(static_cast<int>(std::floor(b * fn - 0.5f)), 0, n - 1);
        if (lo > hi) {
            lo = hi = std::clamp(static_cast<int>((a + b) * 0.5f * fn), 0, n - 1);
        }
    };
    TexelRange t;
    axis(r.u0, r.u1, l.w, t.x0, t.x1);
    axis(r.v0, r.v1, l.h, t.y0, t.y1);
    return t;
}

float bilinear_in(const LightmapAlphaMask::Level& l, float u, float v, const TexelRange& t)
{
    auto axis = [](float c, int n, int lo, int hi, int& i0, int& i1, float& f) {
        float x = c * static_cast<float>(n) - 0.5f;
        x = std::isfinite(x) ? std::clamp(x, static_cast<float>(lo), static_cast<float>(hi)) : static_cast<float>(lo);
        i0 = static_cast<int>(x);
        i1 = std::min(i0 + 1, hi);
        f = x - static_cast<float>(i0);
    };
    int x0, x1, y0, y1;
    float fx, fy;
    axis(u, l.w, t.x0, t.x1, x0, x1, fx);
    axis(v, l.h, t.y0, t.y1, y0, y1, fy);
    const float a00 = l.a[y0 * l.w + x0];
    const float a01 = l.a[y0 * l.w + x1];
    const float a10 = l.a[y1 * l.w + x0];
    const float a11 = l.a[y1 * l.w + x1];
    const float top = a00 + (a01 - a00) * fx;
    const float bottom = a10 + (a11 - a10) * fx;
    return top + (bottom - top) * fy;
}

float bilinear(const LightmapAlphaMask::Level& l, float u, float v)
{
    const float x = wrap01(u) * static_cast<float>(l.w) - 0.5f;
    const float y = wrap01(v) * static_cast<float>(l.h) - 0.5f;
    const float fx0 = std::floor(x);
    const float fy0 = std::floor(y);
    const float fx = x - fx0;
    const float fy = y - fy0;
    const int x0 = (static_cast<int>(fx0) + l.w) % l.w;
    const int y0 = (static_cast<int>(fy0) + l.h) % l.h;
    const int x1 = (x0 + 1) % l.w;
    const int y1 = (y0 + 1) % l.h;
    const float a00 = l.a[y0 * l.w + x0];
    const float a01 = l.a[y0 * l.w + x1];
    const float a10 = l.a[y1 * l.w + x0];
    const float a11 = l.a[y1 * l.w + x1];
    const float top = a00 + (a01 - a00) * fx;
    const float bottom = a10 + (a11 - a10) * fx;
    return top + (bottom - top) * fy;
}

} // namespace

float LightmapAlphaMask::coverage(float u, float v, float lod, const Rect* rect) const
{
    if (levels.empty()) {
        return 1.0f;
    }
    const float top = static_cast<float>(levels.size() - 1);
    lod = lod > 0.0f ? std::min(lod, top) : 0.0f;
    const int taps = lod > 0.0f ? footprint_taps : 1;
    // a rect keeps at least a texel of every level read, so a coarse texel never stands in for it
    float finest = top;
    if (rect) {
        const float extent = std::min((rect->u1 - rect->u0) * static_cast<float>(levels.front().w),
                                      (rect->v1 - rect->v0) * static_cast<float>(levels.front().h));
        finest = std::log2(std::max(extent, 1.0f));
    }
    const float tap_lod = std::min(std::max(0.0f, lod - std::log2(static_cast<float>(taps))), finest);
    const int l0 = static_cast<int>(tap_lod);
    const int l1 = static_cast<float>(l0 + 1) <= finest ? l0 + 1 : l0;
    const float frac = tap_lod - static_cast<float>(l0);
    const TexelRange r0 = rect ? texels_in(levels[l0], *rect) : TexelRange{};
    const TexelRange r1 = rect ? texels_in(levels[l1], *rect) : TexelRange{};
    auto tap = [&](int l, const TexelRange& r, float tu, float tv) {
        return rect ? bilinear_in(levels[l], tu, tv, r) : bilinear(levels[l], tu, tv);
    };
    const float span_u = std::exp2(lod) / static_cast<float>(levels.front().w);
    const float span_v = std::exp2(lod) / static_cast<float>(levels.front().h);
    float sum = 0.0f;
    for (int j = 0; j < taps; j++) {
        const float tv = v + ((static_cast<float>(j) + 0.5f) / static_cast<float>(taps) - 0.5f) * span_v;
        for (int i = 0; i < taps; i++) {
            const float tu = u + ((static_cast<float>(i) + 0.5f) / static_cast<float>(taps) - 0.5f) * span_u;
            float a = tap(l0, r0, tu, tv);
            if (frac > 0.0f && l1 != l0) {
                a += (tap(l1, r1, tu, tv) - a) * frac;
            }
            sum += a;
        }
    }
    return sum / static_cast<float>(taps * taps) * (1.0f / 255.0f);
}

const LightmapAlphaMask* lightmap_alpha_mask(int bm_handle)
{
    auto it = g_masks.find(bm_handle);
    if (it == g_masks.end()) {
        it = g_masks.emplace(bm_handle, decode(bm_handle)).first;
    }
    return it->second.get();
}

void lightmap_alpha_masks_release()
{
    g_masks.clear();
}
