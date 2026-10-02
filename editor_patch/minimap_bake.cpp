#include <windows.h>
#include <shlwapi.h>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <dds.h>
#include <rgbcx.h>
#include <xlog/xlog.h>
#include <patch_common/MemUtils.h>
#include <common/lighting/alpine_lighting.h>
#include <common/terrain/alpine_terrain.h>
#include <common/utils/string-utils.h>
#include "minimap_bake.h"
#include "alpine_lightmaps.h"
#include "alpine_obj.h"
#include "level.h"
#include "meshes.h"
#include "mfc_types.h"
#include "terrain_build.h"
#include "terrain_decorations.h"
#include "terrain_preview.h"
#include "textures.h"
#include "vtypes.h"

namespace at = alpine_terrain;

namespace
{

constexpr float min_up_normal_y = 0.05f;
constexpr int max_texture_dim = 128;
constexpr int max_source_texture_dim = 8192;
constexpr uint32_t fallback_texel = 0xFF808080u;
constexpr int alpha_test_ref = 128;
// Decorations reaching less than this many pixels from their origin are blended in, not rasterized.
constexpr float min_raster_reach_px = 1.0f;
// Below this reach a decoration is drawn from its lowest LOD.
constexpr float far_lod_reach_px = 8.0f;
// Past this many decoration triangles the smallest decorations are blended in instead.
constexpr std::size_t max_deco_raster_tris = 4'000'000;

struct TexLevel
{
    int w = 1;
    int h = 1;
    std::vector<uint32_t> px;
};

struct Texture
{
    std::vector<TexLevel> levels;
    bool alpha_tested = false;
    // Average colour of the texels that pass the alpha test, and the fraction that do
    float opaque_rgb[3] = {128.0f, 128.0f, 128.0f};
    float opaque_fraction = 1.0f;
};

uint32_t argb(int a, int r, int g, int b)
{
    return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

int expand5(int v) { return (v << 3) | (v >> 2); }
int expand6(int v) { return (v << 2) | (v >> 4); }

int bytes_per_texel(int format)
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

// Channel layouts match the D3D texture upload 0x004F4C50.
void decode_texels(int format, const uint8_t* src, const uint8_t* pal, int count, uint32_t* out)
{
    switch (format) {
    case BitmapEntry::FORMAT_8_PALETTED:
        for (int i = 0; i < count; ++i) {
            const uint8_t* c = pal + src[i] * 3;
            out[i] = argb(255, c[0], c[1], c[2]);
        }
        break;
    case BitmapEntry::FORMAT_8_ALPHA:
        for (int i = 0; i < count; ++i) out[i] = argb(src[i], 255, 255, 255);
        break;
    case BitmapEntry::FORMAT_565_RGB:
    case BitmapEntry::FORMAT_4444_ARGB:
    case BitmapEntry::FORMAT_1555_ARGB:
        for (int i = 0; i < count; ++i) {
            uint16_t p;
            std::memcpy(&p, src + i * 2, 2);
            if (format == BitmapEntry::FORMAT_565_RGB) {
                out[i] = argb(255, expand5(p >> 11), expand6((p >> 5) & 0x3F), expand5(p & 0x1F));
            }
            else if (format == BitmapEntry::FORMAT_4444_ARGB) {
                out[i] = argb(((p >> 12) & 0xF) * 17, ((p >> 8) & 0xF) * 17, ((p >> 4) & 0xF) * 17,
                              (p & 0xF) * 17);
            }
            else {
                out[i] = argb((p & 0x8000) ? 255 : 0, expand5((p >> 10) & 0x1F), expand5((p >> 5) & 0x1F),
                              expand5(p & 0x1F));
            }
        }
        break;
    case BitmapEntry::FORMAT_888_RGB:
        for (int i = 0; i < count; ++i) {
            const uint8_t* c = src + i * 3;
            out[i] = argb(255, c[2], c[1], c[0]);
        }
        break;
    case BitmapEntry::FORMAT_8888_ARGB:
        std::memcpy(out, src, static_cast<std::size_t>(count) * 4);
        break;
    default:
        break;
    }
}

TexLevel downsample(const TexLevel& s)
{
    TexLevel d;
    d.w = std::max(1, s.w / 2);
    d.h = std::max(1, s.h / 2);
    d.px.resize(static_cast<std::size_t>(d.w) * d.h);
    for (int y = 0; y < d.h; ++y) {
        const int y0 = std::min(y * 2, s.h - 1);
        const int y1 = std::min(y * 2 + 1, s.h - 1);
        for (int x = 0; x < d.w; ++x) {
            const int x0 = std::min(x * 2, s.w - 1);
            const int x1 = std::min(x * 2 + 1, s.w - 1);
            const uint32_t q[4] = {s.px[y0 * s.w + x0], s.px[y0 * s.w + x1], s.px[y1 * s.w + x0],
                                   s.px[y1 * s.w + x1]};
            uint32_t out = 0;
            for (int shift = 0; shift < 32; shift += 8) {
                uint32_t sum = 0;
                for (uint32_t t : q) sum += (t >> shift) & 0xFF;
                out |= ((sum + 2) / 4) << shift;
            }
            d.px[y * d.w + x] = out;
        }
    }
    return d;
}

// Decodes one source row at a time straight into box averages at out's size, so a large texture
// costs a row of scratch rather than a full-resolution copy.
class BoxReducer
{
public:
    BoxReducer(int src_w, int src_h, TexLevel& out) :
        src_w_{src_w}, src_h_{src_h}, out_{out}, row_(src_w), sums_(static_cast<std::size_t>(out.w) * 4),
        col_count_(out.w, 0)
    {
        out_.px.assign(static_cast<std::size_t>(out.w) * out.h, 0u);
        for (int x = 0; x < src_w_; ++x) ++col_count_[column_of(x)];
    }

    bool run(int format, const uint8_t* src, const uint8_t* pal, bool& alpha_tested)
    {
        const int bpp = bytes_per_texel(format);
        if (bpp == 0 || (format == BitmapEntry::FORMAT_8_PALETTED && !pal)) return false;
        const std::size_t stride = static_cast<std::size_t>(src_w_) * bpp;
        int band_rows = 0;
        for (int y = 0; y < src_h_; ++y) {
            decode_texels(format, src + y * stride, pal, src_w_, row_.data());
            for (int x = 0; x < src_w_; ++x) {
                const uint32_t t = row_[x];
                if (static_cast<int>(t >> 24) < alpha_test_ref) alpha_tested = true;
                uint32_t* sum = &sums_[static_cast<std::size_t>(column_of(x)) * 4];
                for (int c = 0; c < 4; ++c) sum[c] += (t >> (8 * c)) & 0xFF;
            }
            ++band_rows;
            const int oy = row_of(y);
            if (y + 1 == src_h_ || row_of(y + 1) != oy) {
                flush_band(oy, band_rows);
                band_rows = 0;
            }
        }
        return true;
    }

private:
    int column_of(int x) const { return static_cast<int>(static_cast<int64_t>(x) * out_.w / src_w_); }
    int row_of(int y) const { return static_cast<int>(static_cast<int64_t>(y) * out_.h / src_h_); }

    void flush_band(int oy, int band_rows)
    {
        for (int ox = 0; ox < out_.w; ++ox) {
            uint32_t* sum = &sums_[static_cast<std::size_t>(ox) * 4];
            const uint32_t n = static_cast<uint32_t>(col_count_[ox] * band_rows);
            uint32_t texel = 0;
            for (int c = 0; c < 4; ++c) {
                texel |= (n ? (sum[c] + n / 2) / n : 0u) << (8 * c);
                sum[c] = 0;
            }
            out_.px[static_cast<std::size_t>(oy) * out_.w + ox] = texel;
        }
    }

    int src_w_;
    int src_h_;
    TexLevel& out_;
    std::vector<uint32_t> row_;
    std::vector<uint32_t> sums_;
    std::vector<int> col_count_;
};

Texture load_texture(int handle)
{
    Texture tex;
    TexLevel base;
    bool decoded = false;
    int w = 0, h = 0, num_pixels = 0, num_levels = 0;
    if (handle >= 0 && BitmapEntry::handle_to_index(handle) >= 0) {
        bm_get_mipmap_info(handle, &w, &h, &num_pixels, &num_levels);
    }
    if (w > 0 && h > 0 && w <= max_source_texture_dim && h <= max_source_texture_dim) {
        int factor = 1;
        while (w / factor > max_texture_dim || h / factor > max_texture_dim) factor *= 2;
        base.w = std::max(1, w / factor);
        base.h = std::max(1, h / factor);
        // Everything allocates before the lock, so nothing between lock and unlock can throw.
        BoxReducer reducer{w, h, base};
        void* pixels = nullptr;
        void* palette = nullptr;
        const int format = bm_lock(handle, &pixels, &palette);
        if (format != 0) {
            if (pixels) {
                decoded = reducer.run(format, static_cast<const uint8_t*>(pixels),
                                      static_cast<const uint8_t*>(palette), tex.alpha_tested);
            }
            bm_unlock(handle);
        }
    }
    if (!decoded) {
        base.w = base.h = 1;
        base.px.assign(1, fallback_texel);
        tex.alpha_tested = false;
    }
    uint64_t sum[3] = {};
    std::size_t opaque = 0;
    for (const uint32_t t : base.px) {
        if (tex.alpha_tested && static_cast<int>(t >> 24) < alpha_test_ref) continue;
        for (int c = 0; c < 3; ++c) sum[c] += (t >> (16 - 8 * c)) & 0xFF;
        ++opaque;
    }
    if (opaque > 0) {
        for (int c = 0; c < 3; ++c) tex.opaque_rgb[c] = static_cast<float>(sum[c]) / static_cast<float>(opaque);
    }
    tex.opaque_fraction = static_cast<float>(opaque) / static_cast<float>(base.px.size());
    tex.levels.push_back(std::move(base));
    while (tex.levels.back().w > 1 || tex.levels.back().h > 1) {
        tex.levels.push_back(downsample(tex.levels.back()));
    }
    return tex;
}

uint32_t lerp_texel(uint32_t a, uint32_t b, float t)
{
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        const float ca = static_cast<float>((a >> shift) & 0xFF);
        const float cb = static_cast<float>((b >> shift) & 0xFF);
        out |= static_cast<uint32_t>(ca + (cb - ca) * t + 0.5f) << shift;
    }
    return out;
}

float wrap01(float f)
{
    if (!std::isfinite(f)) return 0.0f;
    return f - std::floor(f);
}

uint32_t sample_texture(const TexLevel& t, float u, float v)
{
    const float x = wrap01(u) * t.w - 0.5f;
    const float y = wrap01(v) * t.h - 0.5f;
    const float fx = std::floor(x);
    const float fy = std::floor(y);
    const int x0 = (static_cast<int>(fx) + t.w) % t.w;
    const int y0 = (static_cast<int>(fy) + t.h) % t.h;
    const int x1 = (x0 + 1) % t.w;
    const int y1 = (y0 + 1) % t.h;
    const uint32_t top = lerp_texel(t.px[y0 * t.w + x0], t.px[y0 * t.w + x1], x - fx);
    const uint32_t bottom = lerp_texel(t.px[y1 * t.w + x0], t.px[y1 * t.w + x1], x - fx);
    return lerp_texel(top, bottom, y - fy);
}

void sample_lightmap(const GLightmap& p, float lu, float lv, float out[3])
{
    float x = std::isfinite(lu) ? lu * p.w - 0.5f : 0.0f;
    float y = std::isfinite(lv) ? lv * p.h - 0.5f : 0.0f;
    x = std::clamp(x, 0.0f, static_cast<float>(p.w - 1));
    y = std::clamp(y, 0.0f, static_cast<float>(p.h - 1));
    const int x0 = static_cast<int>(x);
    const int y0 = static_cast<int>(y);
    const int x1 = std::min(x0 + 1, p.w - 1);
    const int y1 = std::min(y0 + 1, p.h - 1);
    const float fx = x - x0;
    const float fy = y - y0;
    for (int c = 0; c < 3; ++c) {
        const float t00 = p.pixels[(y0 * p.w + x0) * 3 + c];
        const float t10 = p.pixels[(y0 * p.w + x1) * 3 + c];
        const float t01 = p.pixels[(y1 * p.w + x0) * 3 + c];
        const float t11 = p.pixels[(y1 * p.w + x1) * 3 + c];
        const float top = t00 + (t10 - t00) * fx;
        const float bottom = t01 + (t11 - t01) * fx;
        out[c] = top + (bottom - top) * fy;
    }
}

struct WorldVert
{
    float x, y, z, u, v, lu, lv;
};

WorldVert lerp_vert(const WorldVert& a, const WorldVert& b, float t)
{
    return {a.x + (b.x - a.x) * t,   a.y + (b.y - a.y) * t,   a.z + (b.z - a.z) * t,
            a.u + (b.u - a.u) * t,   a.v + (b.v - a.v) * t,   a.lu + (b.lu - a.lu) * t,
            a.lv + (b.lv - a.lv) * t};
}

void clip_below(const std::vector<WorldVert>& in, float cut, std::vector<WorldVert>& out)
{
    out.clear();
    const std::size_t n = in.size();
    for (std::size_t i = 0; i < n; ++i) {
        const WorldVert& cur = in[i];
        const WorldVert& nxt = in[(i + 1) % n];
        const bool cur_in = cur.y <= cut;
        const bool nxt_in = nxt.y <= cut;
        if (cur_in) out.push_back(cur);
        if (cur_in != nxt_in) out.push_back(lerp_vert(cur, nxt, (cut - cur.y) / (nxt.y - cur.y)));
    }
}

struct ScreenVert
{
    float sx, sy;
    float attr[5]; // y, u, v, lu, lv
};

// Evaluated from the edge's endpoints in a fixed order, so the triangle on the other side of a
// shared edge gets the exact negation; the top-left rule then gives each centre on it to one side.
class EdgeFn
{
public:
    EdgeFn(const ScreenVert& p, const ScreenVert& q)
    {
        const bool swap = q.sx < p.sx || (q.sx == p.sx && q.sy < p.sy);
        const ScreenVert& s = swap ? q : p;
        const ScreenVert& e = swap ? p : q;
        px_ = s.sx;
        py_ = s.sy;
        dx_ = static_cast<double>(e.sx) - s.sx;
        dy_ = static_cast<double>(e.sy) - s.sy;
        sign_ = swap ? -1.0 : 1.0;
        const float ddx = q.sx - p.sx;
        const float ddy = q.sy - p.sy;
        top_left_ = ddy < 0.0f || (ddy == 0.0f && ddx > 0.0f);
    }

    bool covers(double x, double y) const
    {
        const double w = sign_ * (dx_ * (y - py_) - dy_ * (x - px_));
        return w > 0.0 || (w == 0.0 && top_left_);
    }

private:
    double px_, py_, dx_, dy_, sign_;
    bool top_left_;
};

int lit_channel(int c, float light)
{
    return std::min(255, static_cast<int>(static_cast<float>(c) * light + 0.5f));
}

struct FaceShade
{
    const Texture* tex = nullptr;
    const GLightmap* lightmap = nullptr;
    // The light the texels are multiplied by across the face, when it has no lightmap
    const float* light = nullptr;
    // 1 + the index of the terrain whose shading replaces the face's
    uint8_t terrain = 0;
    bool liquid = false;
    float liquid_rgb[3] = {};
    float liquid_alpha = 0.0f;
};

class Raster
{
public:
    Raster(int res, float min_x, float max_z, float scale_x, float scale_z) :
        res_{res}, min_x_{min_x}, max_z_{max_z}, scale_x_{scale_x}, scale_z_{scale_z},
        depth_(static_cast<std::size_t>(res) * res, -std::numeric_limits<float>::infinity()),
        color_(static_cast<std::size_t>(res) * res, 0u), terrain_(static_cast<std::size_t>(res) * res, 0)
    {}

    float pixel_world_size() const { return std::max(1.0f / scale_x_, 1.0f / scale_z_); }
    float pixels_per_unit() const { return std::min(scale_x_, scale_z_); }
    float pixel_area_per_unit2() const { return scale_x_ * scale_z_; }

    // Replaces each terrain-marked pixel with shade(terrain index, world x, y, z); returns how many.
    template<typename Fn>
    int shade_terrain(Fn&& shade)
    {
        int shaded = 0;
        for (int py = 0; py < res_; ++py) {
            const float z = max_z_ - (py + 0.5f) / scale_z_;
            for (int px = 0; px < res_; ++px) {
                const std::size_t i = static_cast<std::size_t>(py) * res_ + px;
                if (!terrain_[i]) continue;
                color_[i] = shade(terrain_[i] - 1, min_x_ + (px + 0.5f) / scale_x_, depth_[i], z);
                terrain_[i] = 0;
                ++shaded;
            }
        }
        return shaded;
    }

    // Blends rgb by `weight`, split bilinearly over the pixels around world (x, z), into those whose
    // surface top_y is above.
    void splat(float x, float z, float top_y, const float (&rgb)[3], float weight)
    {
        const float fx = (x - min_x_) * scale_x_ - 0.5f;
        const float fy = (max_z_ - z) * scale_z_ - 0.5f;
        if (!(fx > -1.0f && fy > -1.0f && fx < res_ && fy < res_)) return;
        const float bx = std::floor(fx);
        const float by = std::floor(fy);
        const float tx = fx - bx;
        const float ty = fy - by;
        for (int k = 0; k < 4; ++k) {
            const int px = static_cast<int>(bx) + (k & 1);
            const int py = static_cast<int>(by) + (k >> 1);
            if (px < 0 || py < 0 || px >= res_ || py >= res_) continue;
            const float w = weight * ((k & 1) ? tx : 1.0f - tx) * ((k >> 1) ? ty : 1.0f - ty);
            const std::size_t i = static_cast<std::size_t>(py) * res_ + px;
            if (!(w > 0.0f) || !std::isfinite(depth_[i]) || !(top_y > depth_[i])) continue;
            uint32_t& dst = color_[i];
            int out[3];
            for (int c = 0; c < 3; ++c) {
                const float under = static_cast<float>((dst >> (16 - 8 * c)) & 0xFF);
                out[c] = static_cast<int>(under + (rgb[c] - under) * w + 0.5f);
            }
            dst = argb(255, out[0], out[1], out[2]);
        }
    }

    // Returns true if any triangle of the polygon covered a pixel center.
    bool draw_polygon(const std::vector<WorldVert>& poly, const FaceShade& shade)
    {
        if (poly.size() < 3) return false;
        screen_.clear();
        for (const WorldVert& v : poly) {
            screen_.push_back({(v.x - min_x_) * scale_x_, (max_z_ - v.z) * scale_z_, {v.y, v.u, v.v, v.lu, v.lv}});
        }
        bool drew = false;
        for (std::size_t i = 1; i + 1 < screen_.size(); ++i) {
            drew |= draw_triangle(screen_[0], screen_[i], screen_[i + 1], shade);
        }
        return drew;
    }

    const std::vector<uint32_t>& pixels() const { return color_; }

private:
    bool draw_triangle(const ScreenVert& a, ScreenVert b, ScreenVert c, const FaceShade& shade)
    {
        float area = (b.sx - a.sx) * (c.sy - a.sy) - (b.sy - a.sy) * (c.sx - a.sx);
        if (!std::isfinite(area) || std::fabs(area) < 1e-6f) return false;
        if (area < 0.0f) {
            std::swap(b, c);
            area = -area;
        }

        float dx[5], dy[5];
        for (int k = 0; k < 5; ++k) {
            const float ab = b.attr[k] - a.attr[k];
            const float ac = c.attr[k] - a.attr[k];
            dx[k] = (ab * (c.sy - a.sy) - ac * (b.sy - a.sy)) / area;
            dy[k] = (ac * (b.sx - a.sx) - ab * (c.sx - a.sx)) / area;
        }

        const float fmin_x = std::min({a.sx, b.sx, c.sx});
        const float fmax_x = std::max({a.sx, b.sx, c.sx});
        const float fmin_y = std::min({a.sy, b.sy, c.sy});
        const float fmax_y = std::max({a.sy, b.sy, c.sy});
        if (fmax_x < 0.0f || fmax_y < 0.0f || fmin_x > res_ || fmin_y > res_) return false;
        const int x0 = std::max(0, static_cast<int>(std::floor(fmin_x)));
        const int x1 = std::min(res_ - 1, static_cast<int>(std::ceil(fmax_x)));
        const int y0 = std::max(0, static_cast<int>(std::floor(fmin_y)));
        const int y1 = std::min(res_ - 1, static_cast<int>(std::ceil(fmax_y)));

        const TexLevel* level = nullptr;
        if (shade.tex && !shade.terrain) {
            const TexLevel& top = shade.tex->levels.front();
            const float rho = std::max(std::hypot(dx[1] * top.w, dx[2] * top.h),
                                       std::hypot(dy[1] * top.w, dy[2] * top.h));
            int lod = rho > 1.0f && std::isfinite(rho) ? static_cast<int>(std::log2(rho)) : 0;
            lod = std::clamp(lod, 0, static_cast<int>(shade.tex->levels.size()) - 1);
            level = &shade.tex->levels[lod];
        }

        const EdgeFn e0{a, b};
        const EdgeFn e1{b, c};
        const EdgeFn e2{c, a};

        bool drew = false;
        for (int py = y0; py <= y1; ++py) {
            const float cy = py + 0.5f;
            for (int px = x0; px <= x1; ++px) {
                const float cx = px + 0.5f;
                if (!e0.covers(cx, cy) || !e1.covers(cx, cy) || !e2.covers(cx, cy)) continue;
                const float rx = cx - a.sx;
                const float ry = cy - a.sy;
                const float y = a.attr[0] + dx[0] * rx + dy[0] * ry;
                const std::size_t i = static_cast<std::size_t>(py) * res_ + px;
                if (!(y > depth_[i])) continue;
                drew = true;
                if (shade.liquid) {
                    shade_liquid(i, y, shade);
                    continue;
                }
                if (shade.terrain) {
                    depth_[i] = y;
                    terrain_[i] = shade.terrain;
                    continue;
                }
                uint32_t texel = fallback_texel;
                if (level) {
                    texel = sample_texture(*level, a.attr[1] + dx[1] * rx + dy[1] * ry,
                                           a.attr[2] + dx[2] * rx + dy[2] * ry);
                    if (shade.tex->alpha_tested && static_cast<int>(texel >> 24) < alpha_test_ref) continue;
                }
                int r = (texel >> 16) & 0xFF;
                int g = (texel >> 8) & 0xFF;
                int bl = texel & 0xFF;
                if (shade.lightmap) {
                    float lm[3];
                    sample_lightmap(*shade.lightmap, a.attr[3] + dx[3] * rx + dy[3] * ry,
                                    a.attr[4] + dx[4] * rx + dy[4] * ry, lm);
                    r = std::min(255, static_cast<int>(r * lm[0] * (2.0f / 255.0f) + 0.5f));
                    g = std::min(255, static_cast<int>(g * lm[1] * (2.0f / 255.0f) + 0.5f));
                    bl = std::min(255, static_cast<int>(bl * lm[2] * (2.0f / 255.0f) + 0.5f));
                }
                else if (shade.light) {
                    r = lit_channel(r, shade.light[0]);
                    g = lit_channel(g, shade.light[1]);
                    bl = lit_channel(bl, shade.light[2]);
                }
                depth_[i] = y;
                color_[i] = argb(255, r, g, bl);
                terrain_[i] = 0;
            }
        }
        return drew;
    }

    void shade_liquid(std::size_t i, float y, const FaceShade& shade)
    {
        uint32_t& dst = color_[i];
        if (!std::isfinite(depth_[i])) {
            dst = argb(255, static_cast<int>(shade.liquid_rgb[0]), static_cast<int>(shade.liquid_rgb[1]),
                       static_cast<int>(shade.liquid_rgb[2]));
        }
        else {
            const float a = shade.liquid_alpha;
            int rgb[3];
            for (int c = 0; c < 3; ++c) {
                const float under = static_cast<float>((dst >> (16 - 8 * c)) & 0xFF);
                rgb[c] = static_cast<int>(under + (shade.liquid_rgb[c] - under) * a + 0.5f);
            }
            dst = argb(255, rgb[0], rgb[1], rgb[2]);
        }
        depth_[i] = y;
    }

    int res_;
    float min_x_;
    float max_z_;
    float scale_x_;
    float scale_z_;
    std::vector<float> depth_;
    std::vector<uint32_t> color_;
    std::vector<uint8_t> terrain_;
    std::vector<ScreenVert> screen_;
};

std::string level_file_stem()
{
    auto* frame = static_cast<CMainFrame*>(GetMainFrame());
    CDedDoc* doc = frame ? frame->doc : nullptr;
    const char* path = doc ? doc->_d.m_strPathName.m_pchData : nullptr;
    if (!path || !path[0]) return {};
    return std::string{get_filename_without_ext(PathFindFileNameA(path))};
}

uint32_t fnv1a_lower(const std::string& s)
{
    uint32_t hash = 2166136261u;
    for (const char ch : s) {
        hash ^= static_cast<uint8_t>(std::tolower(static_cast<unsigned char>(ch)));
        hash *= 16777619u;
    }
    return hash;
}

bool ensure_dir(const std::string& dir)
{
    return CreateDirectoryA(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

// A square RGBA (R first) level's 2x2 box halving. Colour is averaged over the covered texels only, so
// the empty background does not darken the level's edges.
std::vector<uint8_t> halve_rgba(int size, const std::vector<uint8_t>& s)
{
    const int half = size / 2;
    std::vector<uint8_t> d(static_cast<std::size_t>(half) * half * 4);
    for (int y = 0; y < half; ++y) {
        for (int x = 0; x < half; ++x) {
            int sum_a = 0;
            int sum_ca[3] = {};
            for (int k = 0; k < 4; ++k) {
                const std::size_t row = static_cast<std::size_t>(2 * y + (k >> 1)) * size;
                const uint8_t* t = &s[(row + 2 * x + (k & 1)) * 4];
                sum_a += t[3];
                for (int c = 0; c < 3; ++c) sum_ca[c] += t[c] * t[3];
            }
            uint8_t* o = &d[(static_cast<std::size_t>(y) * half + x) * 4];
            for (int c = 0; c < 3; ++c) {
                o[c] = sum_a > 0 ? static_cast<uint8_t>((sum_ca[c] + sum_a / 2) / sum_a) : 0;
            }
            o[3] = static_cast<uint8_t>((sum_a + 2) / 4);
        }
    }
    return d;
}

constexpr uint32_t bc1_quality = 10;

// 16 RGBA texels to a BC1 block. Texels with alpha below alpha_test_ref become 3-colour mode's
// transparent black.
void encode_bc1_punch_through(const uint8_t* px, uint8_t* dst)
{
    alignas(4) uint8_t opaque[16 * 4];
    int n = 0;
    for (int i = 0; i < 16; ++i) {
        if (px[i * 4 + 3] >= alpha_test_ref) {
            std::memcpy(opaque + n * 4, px + i * 4, 4);
            ++n;
        }
    }
    if (n == 16) {
        rgbcx::encode_bc1(bc1_quality, dst, px, true, false);
        return;
    }
    if (n == 0) {
        constexpr uint8_t transparent[8] = {0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF};
        std::memcpy(dst, transparent, sizeof(transparent));
        return;
    }
    // rgbcx has no punch-through mode: fit the endpoints to the opaque texels alone, then order them
    // for 3-colour mode and pick the selectors again.
    for (int i = n; i < 16; ++i) {
        std::memcpy(opaque + i * 4, opaque + (i % n) * 4, 4);
    }
    rgbcx::encode_bc1(bc1_quality, dst, opaque, true, false);
    const uint16_t e0 = static_cast<uint16_t>(dst[0] | dst[1] << 8);
    const uint16_t e1 = static_cast<uint16_t>(dst[2] | dst[3] << 8);
    const uint16_t lo = std::min(e0, e1);
    const uint16_t hi = std::max(e0, e1);
    dst[0] = static_cast<uint8_t>(lo);
    dst[1] = static_cast<uint8_t>(lo >> 8);
    dst[2] = static_cast<uint8_t>(hi);
    dst[3] = static_cast<uint8_t>(hi >> 8);
    rgbcx::color32 pal[4];
    rgbcx::unpack_bc1_block_colors(dst, pal);
    uint32_t sels = 0;
    for (int i = 0; i < 16; ++i) {
        const uint8_t* t = px + i * 4;
        uint32_t sel = 3;
        if (t[3] >= alpha_test_ref) {
            int best = std::numeric_limits<int>::max();
            for (uint32_t k = 0; k < 3; ++k) {
                const int dr = pal[k].r - t[0];
                const int dg = pal[k].g - t[1];
                const int db = pal[k].b - t[2];
                const int err = dr * dr + dg * dg + db * db;
                if (err < best) {
                    best = err;
                    sel = k;
                }
            }
        }
        sels |= sel << (2 * i);
    }
    for (int k = 0; k < 4; ++k) {
        dst[4 + k] = static_cast<uint8_t>(sels >> (8 * k));
    }
}

// The file bytes of a DXT1 DDS with a full mip chain, of a square ARGB image.
std::vector<uint8_t> encode_dds_dxt1(int size, const std::vector<uint32_t>& argb)
{
    static const bool rgbcx_ready = (rgbcx::init(), true);
    (void)rgbcx_ready;

    int levels = 1;
    for (int s = size; s > 1; s /= 2) ++levels;
    auto level_bytes = [](int s) { return static_cast<std::size_t>((s + 3) / 4) * ((s + 3) / 4) * 8; };
    std::size_t total = sizeof(DDS_MAGIC) + sizeof(DDS_HEADER);
    for (int k = 0; k < levels; ++k) {
        total += level_bytes(size >> k);
    }
    std::vector<uint8_t> out(total);

    DDS_HEADER hdr{};
    hdr.size = sizeof(DDS_HEADER);
    hdr.flags = DDS_HEADER_FLAGS_TEXTURE | DDS_HEADER_FLAGS_MIPMAP | DDS_HEADER_FLAGS_LINEARSIZE;
    hdr.height = static_cast<uint32_t>(size);
    hdr.width = static_cast<uint32_t>(size);
    hdr.pitchOrLinearSize = static_cast<uint32_t>(level_bytes(size));
    hdr.mipMapCount = static_cast<uint32_t>(levels);
    hdr.ddspf = DDSPF_DXT1;
    hdr.caps = DDS_SURFACE_FLAGS_TEXTURE | DDS_SURFACE_FLAGS_MIPMAP;
    std::memcpy(out.data(), &DDS_MAGIC, sizeof(DDS_MAGIC));
    std::memcpy(out.data() + sizeof(DDS_MAGIC), &hdr, sizeof(hdr));

    std::vector<uint8_t> level(static_cast<std::size_t>(size) * size * 4);
    for (std::size_t i = 0; i < level.size() / 4; ++i) {
        const uint32_t p = argb[i];
        level[i * 4 + 0] = static_cast<uint8_t>(p >> 16);
        level[i * 4 + 1] = static_cast<uint8_t>(p >> 8);
        level[i * 4 + 2] = static_cast<uint8_t>(p);
        level[i * 4 + 3] = static_cast<uint8_t>(p >> 24);
    }

    uint8_t* dst = out.data() + sizeof(DDS_MAGIC) + sizeof(hdr);
    alignas(4) uint8_t block[16 * 4];
    for (int s = size;; s /= 2) {
        const uint8_t* src = level.data();
        for (int by = 0; by < s; by += 4) {
            for (int bx = 0; bx < s; bx += 4) {
                // Levels under 4x4 repeat their edge texels to fill the block.
                for (int i = 0; i < 16; ++i) {
                    const int x = std::min(bx + (i & 3), s - 1);
                    const int y = std::min(by + (i >> 2), s - 1);
                    std::memcpy(block + i * 4, src + (static_cast<std::size_t>(y) * s + x) * 4, 4);
                }
                encode_bc1_punch_through(block, dst);
                dst += 8;
            }
        }
        if (s <= 1) break;
        level = halve_rgba(s, level);
    }
    return out;
}

bool write_file(const std::string& path, const std::vector<uint8_t>& bytes)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    ok = (std::fclose(f) == 0) && ok;
    return ok;
}

bool gather_face(GFace* face, std::vector<WorldVert>& out)
{
    out.clear();
    GFaceVertex* start = face->edge_loop;
    GFaceVertex* fv = start;
    int guard = 0;
    do {
        if (!fv->vertex) return false;
        const Vector3& p = fv->vertex->pos;
        out.push_back({p.x, p.y, p.z, fv->u, fv->v, fv->lm_u, fv->lm_v});
        fv = fv->next;
    } while (fv && fv != start && ++guard < 256);
    return out.size() >= 3;
}

struct MipLevel
{
    int size = 0;
    std::vector<uint8_t> px;
};

// An n-channel square tile and its box-filtered halvings.
std::vector<MipLevel> tile_mips(const std::vector<uint8_t>& base, int size, int n)
{
    std::vector<MipLevel> out;
    if (size <= 0 || base.size() < static_cast<std::size_t>(size) * size * n) return out;
    out.push_back({size, base});
    while (out.back().size > 1 && out.back().size % 2 == 0) {
        const MipLevel& s = out.back();
        MipLevel d;
        d.size = s.size / 2;
        d.px.resize(static_cast<std::size_t>(d.size) * d.size * n);
        auto texel = [&](int x, int y, int c) { return s.px[(static_cast<std::size_t>(y) * s.size + x) * n + c]; };
        for (int y = 0; y < d.size; ++y) {
            for (int x = 0; x < d.size; ++x) {
                for (int c = 0; c < n; ++c) {
                    const int sum = texel(2 * x, 2 * y, c) + texel(2 * x + 1, 2 * y, c) + texel(2 * x, 2 * y + 1, c) +
                                    texel(2 * x + 1, 2 * y + 1, c);
                    d.px[(static_cast<std::size_t>(y) * d.size + x) * n + c] = static_cast<uint8_t>((sum + 2) / 4);
                }
            }
        }
        out.push_back(std::move(d));
    }
    return out;
}

// Bilinear and wrapping, 0..255 per channel, from the level with about one texel per pixel.
template<int N>
void sample_mips(const std::vector<MipLevel>& mips, float texels_per_pixel, float u, float v, float (&out)[N])
{
    const int lod = texels_per_pixel > 1.0f && std::isfinite(texels_per_pixel)
                        ? static_cast<int>(std::log2(texels_per_pixel))
                        : 0;
    const MipLevel& m = mips[std::clamp(lod, 0, static_cast<int>(mips.size()) - 1)];
    const int size = m.size;
    const float fu = wrap01(u) * size - 0.5f;
    const float fv = wrap01(v) * size - 0.5f;
    const float bu = std::floor(fu);
    const float bv = std::floor(fv);
    const float wu = fu - bu;
    const float wv = fv - bv;
    const int u0 = (static_cast<int>(bu) + size) % size;
    const int v0 = (static_cast<int>(bv) + size) % size;
    const int u1 = (u0 + 1) % size;
    const int v1 = (v0 + 1) % size;
    const uint8_t* r0 = &m.px[static_cast<std::size_t>(v0) * size * N];
    const uint8_t* r1 = &m.px[static_cast<std::size_t>(v1) * size * N];
    for (int k = 0; k < N; ++k) {
        const float top = r0[u0 * N + k] + (r0[u1 * N + k] - r0[u0 * N + k]) * wu;
        const float bottom = r1[u0 * N + k] + (r1[u1 * N + k] - r1[u0 * N + k]) * wu;
        out[k] = top + (bottom - top) * wv;
    }
}

struct TileMips
{
    std::vector<MipLevel> rgb;
    std::vector<MipLevel> rgba_premul;
};

struct TerrainTexture
{
    const TileMips* mips = nullptr;
    float inv_uv_scale = 1.0f;
    bool triplanar = false;
};

// Planar XZ, plus the two upright wall planes by the triplanar weights when the layer is triplanar
// (standard_ps.hlsl TER_ADD).
template<int N>
void sample_terrain_texture(const std::vector<MipLevel>& mips, const TerrainTexture& t, float pixel_world, float x,
                            float y, float z, const float (&tw)[3], float (&out)[N])
{
    const float tpp = static_cast<float>(mips.front().size) * pixel_world * t.inv_uv_scale;
    sample_mips(mips, tpp, x * t.inv_uv_scale, z * t.inv_uv_scale, out);
    if (!t.triplanar) return;
    float a[N], b[N];
    sample_mips(mips, tpp, z * t.inv_uv_scale, -y * t.inv_uv_scale, a);
    sample_mips(mips, tpp, x * t.inv_uv_scale, -y * t.inv_uv_scale, b);
    for (int k = 0; k < N; ++k) out[k] = out[k] * tw[1] + a[k] * tw[0] + b[k] * tw[2];
}

// The level ambient and sun as the game's light_get_ambient and gr_get_sun_state give them, and the light
// scale and overbright range the D3D11 renderer draws a multiplayer static mesh with (RenderModeBufferData).
struct LevelLight
{
    float ambient[3] = {};
    alpine_lighting::SunState sun;
    float mesh_light_scale = 3.2f;
    // The default of the player's setting; a level_info.tbl override is not visible to the editor
    float overbright = 0.5f;
};

LevelLight make_level_light(CDedLevel& level)
{
    LevelLight l;
    const Color& a = level.ambient_color;
    l.ambient[0] = alpine_lighting::level_ambient_channel(a.r);
    l.ambient[1] = alpine_lighting::level_ambient_channel(a.g);
    l.ambient[2] = alpine_lighting::level_ambient_channel(a.b);
    const auto& props = level.GetAlpineLevelProperties();
    l.sun = alpine_lighting::sun_state(props);
    if (props.override_static_mesh_ambient_light_modifier) {
        const float m = props.static_mesh_ambient_light_modifier;
        l.mesh_light_scale = std::isfinite(m) ? std::max(m, 0.0f) : 0.0f;
    }
    return l;
}

// alpine_terrain_sample_light with no baked chart
void fallback_light(const LevelLight& l, const float (&n)[3], float (&texel)[3])
{
    alpine_lighting::terrain_fallback_texel(l.ambient, l.sun.travel_dir, l.sun.color, n, 1.0f, texel);
}

// What instance_light packs into a decoration instance for the D3D11 pass: its mesh ambient and sun scale.
struct DecoLight
{
    float ambient[3];
    float sun_scale;
};

DecoLight make_deco_light(const LevelLight& l, float (&texel)[3])
{
    for (float& c : texel) c = std::clamp(c, 0.0f, alpine_lighting::max_mesh_ambient_texel);
    DecoLight d{};
    alpine_lighting::mesh_blend_ambient(l.ambient, texel, d.ambient);
    for (float& c : d.ambient) c = std::clamp(c, 0.0f, 1.0f);
    d.sun_scale = alpine_lighting::sun_mesh_scale(l.sun.enabled && l.sun.affects_meshes, l.sun.mesh_mode, d.ambient);
    // The instance buffer carries them as R8G8B8A8_UNORM.
    auto unorm8 = [](float v) {
        return static_cast<float>(static_cast<int>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f)) / 255.0f;
    };
    for (float& c : d.ambient) c = unorm8(c);
    d.sun_scale = unorm8(d.sun_scale);
    return d;
}

// A decoration pixel's static light for world normal n (standard_ps.hlsl main, INSTANCE_LIGHT): the instance
// ambient plus its share of the sun's N.L, times light_scale and compressed by luminance (add_scene_lights),
// floored at the batch's self-illumination (finish_fragment). Dynamic lights are left out.
void deco_pixel_light(const LevelLight& l, const DecoLight& d, const float (&n)[3], float self_illum, float (&out)[3])
{
    const float* t = l.sun.travel_dir;
    const float n_dot_l = std::clamp(-(n[0] * t[0] + n[1] * t[1] + n[2] * t[2]), 0.0f, 1.0f);
    for (int c = 0; c < 3; ++c) {
        out[c] = (d.ambient[c] + l.sun.color[c] * d.sun_scale * n_dot_l) * l.mesh_light_scale;
    }
    const float lum = out[0] * 0.2126f + out[1] * 0.7152f + out[2] * 0.0722f;
    if (lum > 1.0f) {
        const float range = l.overbright;
        const float excess = lum - 1.0f;
        const float compressed = range > 0.0f ? 1.0f + excess * range / (excess + range) : 1.0f;
        for (float& c : out) c *= compressed / lum;
    }
    if (self_illum > 0.0f) {
        for (float& c : out) c = std::max(c, self_illum);
    }
}

struct TerrainShade
{
    const DedTerrain* terrain = nullptr;
    at::GridView v{};
    bool mapped = false;
    bool weighted = false;
    TerrainTexture layers[at::max_layers];
    uint32_t layer_count = 0;
    TerrainTexture overlays[at::max_overlays];
    uint32_t overlay_count = 0;
    const uint8_t* coverage = nullptr;
    const TerrainBakedLight* light = nullptr;
    bool fullbright = false;
    bool needs_normal = false;
};

TerrainShade make_terrain_shade(const DedTerrain& t, std::unordered_map<const TerrainLayerTile*, TileMips>& cache)
{
    TerrainShade s;
    const DedTerrainData& d = t.data;
    const TerrainGrid& g = *d.grid;
    s.terrain = &t;
    s.v = terrain_grid_view(t.pos, d, g);
    const uint32_t mul = g.weight_res_mul;
    s.mapped = mul > 0 && g.nx >= at::min_verts && g.nz >= at::min_verts;
    s.weighted = s.mapped && g.weights.size() >= 2 * at::weight_map_bytes(g.nx, g.nz, mul);
    auto texture = [&](const DedTerrainLayer& layer) {
        TerrainTexture out;
        if (const TerrainLayerTile* tile = terrain_preview_layer_tile(layer.texture)) {
            auto it = cache.find(tile);
            if (it == cache.end()) {
                it = cache.emplace(tile, TileMips{tile_mips(tile->rgb, tile->size, 3),
                                                  tile_mips(tile->rgba_premul, tile->size, 4)})
                         .first;
            }
            if (!it->second.rgb.empty()) out.mips = &it->second;
        }
        out.inv_uv_scale = 1.0f / std::max(layer.uv_scale, at::min_uv_scale);
        out.triplanar = layer.triplanar;
        s.needs_normal = s.needs_normal || (out.triplanar && out.mips);
        return out;
    };
    s.layer_count = static_cast<uint32_t>(std::min<std::size_t>(d.layers.size(), at::max_layers));
    for (uint32_t l = 0; l < s.layer_count; ++l) s.layers[l] = texture(d.layers[l]);
    if (s.mapped && !g.overlay.empty() && g.overlay.size() == at::overlay_map_bytes(g.nx, g.nz, mul)) {
        s.coverage = g.overlay.data();
        s.overlay_count = static_cast<uint32_t>(std::min<std::size_t>(d.overlays.size(), at::max_overlays));
        for (uint32_t o = 0; o < s.overlay_count; ++o) s.overlays[o] = texture(d.overlays[o]);
    }
    s.fullbright = d.fullbright;
    s.light = terrain_baked_light_find(t.uid, t.pos, d);
    s.needs_normal = s.needs_normal || (!s.fullbright && !s.light);
    return s;
}

// The preview's composite (terrain_preview.cpp composite_chunk) at one point, lit as the game lights it.
uint32_t shade_terrain_pixel(const TerrainShade& s, const LevelLight& ll, float pixel_world, float x, float y, float z)
{
    const at::GridView& v = s.v;
    float n[3] = {0.0f, 1.0f, 0.0f};
    if (s.needs_normal) at::heightmap_normal(v, x, z, n);
    float tw[3];
    float tw_sum = 0.0f;
    for (int c = 0; c < 3; ++c) {
        tw[c] = n[c] * n[c] * n[c] * n[c];
        tw_sum += tw[c];
    }
    for (float& w : tw) w /= std::max(tw_sum, 1e-4f);

    std::size_t t00 = 0, t10 = 0, t01 = 0, t11 = 0;
    float fx = 0.0f, fz = 0.0f;
    if (s.mapped) {
        const uint32_t mul = v.weight_res_mul;
        const uint32_t ww = at::weight_width(v.nx, mul);
        const uint32_t wh = at::weight_height(v.nz, mul);
        auto tap = [&](float world, float origin, uint32_t count, uint32_t& i0, uint32_t& i1, float& f) {
            const float cell = (world - origin) / v.cell_size * static_cast<float>(mul) - 0.5f;
            const float t = std::clamp(std::isfinite(cell) ? cell : 0.0f, 0.0f, static_cast<float>(count - 1));
            i0 = std::min(static_cast<uint32_t>(t), count - 1);
            i1 = std::min(i0 + 1, count - 1);
            f = t - static_cast<float>(i0);
        };
        uint32_t x0, x1, z0, z1;
        tap(x, v.origin[0], ww, x0, x1, fx);
        tap(z, v.origin[2], wh, z0, z1, fz);
        t00 = (static_cast<std::size_t>(z0) * ww + x0) * 4;
        t10 = (static_cast<std::size_t>(z0) * ww + x1) * 4;
        t01 = (static_cast<std::size_t>(z1) * ww + x0) * 4;
        t11 = (static_cast<std::size_t>(z1) * ww + x1) * 4;
    }
    auto bilinear = [&](const uint8_t* map, std::size_t off) {
        const float a = map[t00 + off] + (map[t10 + off] - map[t00 + off]) * fx;
        const float b = map[t01 + off] + (map[t11 + off] - map[t01 + off]) * fx;
        return a + (b - a) * fz;
    };

    float w[at::max_layers] = {};
    float total = 0.0f;
    if (s.weighted) {
        const std::size_t map1 = at::weight_map_bytes(v.nx, v.nz, v.weight_res_mul);
        for (uint32_t l = 0; l < s.layer_count; ++l) {
            w[l] = bilinear(v.weights, (l < 4 ? 0 : map1) + (l & 3));
            total += w[l];
        }
    }
    if (total <= 0.0f) {
        w[0] = 1.0f;
        total = 1.0f;
    }
    float c[3] = {};
    for (uint32_t l = 0; l < s.layer_count; ++l) {
        if (w[l] <= 0.0f) continue;
        const float share = w[l] / total;
        const TileMips* mips = s.layers[l].mips ? s.layers[l].mips : s.layers[0].mips;
        float t[3] = {160.0f, 160.0f, 160.0f};
        if (mips) sample_terrain_texture(mips->rgb, s.layers[l], pixel_world, x, y, z, tw, t);
        for (int ch = 0; ch < 3; ++ch) c[ch] += t[ch] * share;
    }
    for (uint32_t o = 0; o < s.overlay_count; ++o) {
        const TerrainTexture& t = s.overlays[o];
        if (!t.mips || t.mips->rgba_premul.empty()) continue;
        const float cov = bilinear(s.coverage, o) / 255.0f;
        if (cov <= 0.0f) continue;
        float p[4];
        sample_terrain_texture(t.mips->rgba_premul, t, pixel_world, x, y, z, tw, p);
        const float keep = 1.0f - p[3] / 255.0f * cov;
        for (int ch = 0; ch < 3; ++ch) c[ch] = c[ch] * keep + p[ch] * cov;
    }

    float texel[3] = {0.5f, 0.5f, 0.5f};
    if (!s.fullbright) {
        if (s.light) {
            terrain_baked_light_sample(*s.light, x, z, texel);
        }
        else {
            fallback_light(ll, n, texel);
        }
    }
    int rgb[3];
    for (int ch = 0; ch < 3; ++ch) {
        rgb[ch] = static_cast<int>(std::clamp(c[ch] * 2.0f * texel[ch], 0.0f, 255.0f) + 0.5f);
    }
    return argb(255, rgb[0], rgb[1], rgb[2]);
}

struct DecoTri
{
    Vector3 p[3];
    float uv[3][2];
    const Texture* tex;
    // Unit, from its vertex normals where the mesh has them
    float n[3];
    float self_illum;
};

// A decoration mesh's LOD 0 triangles in the space the editor renders it, and what a top-down view of it
// shows when it is smaller than a pixel.
struct DecoMesh
{
    std::vector<DecoTri> tris;
    // Lowest LOD; empty when the mesh has only LOD 0
    std::vector<DecoTri> far_tris;
    Vector3 lo{};
    Vector3 hi{};
    float reach_xz = 0.0f;
    float radius = 0.0f;
    // Top-down area its opaque texels cover at scale 1, and their average colour, normal and self-illumination
    float cover = 0.0f;
    float rgb[3] = {128.0f, 128.0f, 128.0f};
    float n[3] = {0.0f, 1.0f, 0.0f};
    float self_illum = 0.0f;
};

int chunk_bitmap(const EditorV3dMesh& sub, const EditorVifMesh& vm, const EditorVifChunk& chunk)
{
    const int idx = chunk.texture_idx;
    if (idx < 0 || idx >= 7 || idx >= vm.num_texture_handles) return -1;
    if (vm.tex_handles[idx] != -1) return vm.tex_handles[idx];
    const int material = vm.tex_ids[idx];
    if (sub.materials && material < sub.num_materials) {
        return alpine_dlg_resolve_bitmap(sub.materials[material].texture_maps[0].name);
    }
    return -1;
}

// The floor the D3D11 renderer gives a chunk's light (gr_d3d11_decoration.cpp render, gr_d3d11_mesh.cpp batches).
float chunk_self_illumination(const EditorV3dMesh& sub, const EditorVifMesh& vm, const EditorVifChunk& chunk)
{
    if (((chunk.mode >> gr_mode_color_shift) & gr_mode_color_mask) == gr_mode_color_texture) return 1.0f;
    const int idx = chunk.texture_idx;
    if (!sub.materials || sub.num_materials <= 0 || idx < 0 || idx >= 7) return 0.0f;
    const int material = vm.tex_ids[idx];
    if (material >= sub.num_materials) return 0.0f;
    const float* si = sub.materials[material].self_illumination;
    return si && si[0] > 0.0f ? std::min(si[0], 1.0f) : 0.0f;
}

bool normalize3(float (&v)[3])
{
    const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!(len > 1e-12f) || !std::isfinite(len)) return false;
    for (float& c : v) c /= len;
    return true;
}

template<typename GetTexture>
void gather_deco_tris(const EditorV3d& v3d, int level, GetTexture& get_texture, std::vector<DecoTri>& out)
{
    for (int s = 0; s < v3d.num_meshes; ++s) {
        const EditorV3dMesh& sub = v3d.meshes[s];
        vmesh_for_each_lod_chunk(sub.lod_mesh, level, [&](const EditorVifMesh& vm, const EditorVifChunk& chunk,
                                                          auto&& vertex) {
            const int bm = chunk_bitmap(sub, vm, chunk);
            const Texture* tex = bm >= 0 ? get_texture(bm) : nullptr;
            const float self_illum = chunk_self_illumination(sub, vm, chunk);
            const auto* uvs = static_cast<const float*>(chunk.uvs);
            for (int f = 0; f < chunk.num_faces; ++f) {
                const EditorVifFace& face = chunk.faces[f];
                if (!vmesh_face_valid(chunk, face)) continue;
                const uint16_t idx[3] = {face.vindex1, face.vindex2, face.vindex3};
                DecoTri tri{};
                tri.tex = tex;
                tri.self_illum = self_illum;
                bool finite = true;
                for (int k = 0; k < 3; ++k) {
                    tri.p[k] = vertex(idx[k]);
                    finite = finite && std::isfinite(tri.p[k].x) && std::isfinite(tri.p[k].y) &&
                             std::isfinite(tri.p[k].z);
                    if (uvs) {
                        tri.uv[k][0] = uvs[idx[k] * 2];
                        tri.uv[k][1] = uvs[idx[k] * 2 + 1];
                    }
                    if (chunk.norms) {
                        tri.n[0] += chunk.norms[idx[k]].x;
                        tri.n[1] += chunk.norms[idx[k]].y;
                        tri.n[2] += chunk.norms[idx[k]].z;
                    }
                }
                if (!finite) continue;
                if (!normalize3(tri.n)) {
                    const Vector3 e1{tri.p[1].x - tri.p[0].x, tri.p[1].y - tri.p[0].y, tri.p[1].z - tri.p[0].z};
                    const Vector3 e2{tri.p[2].x - tri.p[0].x, tri.p[2].y - tri.p[0].y, tri.p[2].z - tri.p[0].z};
                    tri.n[0] = e1.y * e2.z - e1.z * e2.y;
                    tri.n[1] = e1.z * e2.x - e1.x * e2.z;
                    tri.n[2] = e1.x * e2.y - e1.y * e2.x;
                    if (chunk.face_planes) {
                        const Vector3& pn = chunk.face_planes[f].normal;
                        if (tri.n[0] * pn.x + tri.n[1] * pn.y + tri.n[2] * pn.z < 0.0f) {
                            for (float& c : tri.n) c = -c;
                        }
                    }
                    if (!normalize3(tri.n)) {
                        tri.n[0] = 0.0f;
                        tri.n[1] = 1.0f;
                        tri.n[2] = 0.0f;
                    }
                }
                out.push_back(tri);
            }
        });
    }
}

template<typename GetTexture>
DecoMesh load_deco_mesh(const std::string& name, GetTexture&& get_texture)
{
    DecoMesh m;
    EditorVMesh* vmesh = terrain_decorations_mesh(name);
    const auto* v3d = vmesh ? static_cast<const EditorV3d*>(vmesh->instance) : nullptr;
    if (!v3d || v3d->num_meshes <= 0 || !v3d->meshes) return m;
    gather_deco_tris(*v3d, 0, get_texture, m.tris);
    if (m.tris.empty()) return m;
    gather_deco_tris(*v3d, vmesh_lowest_lod, get_texture, m.far_tris);
    if (m.far_tris.size() >= m.tris.size()) m.far_tris.clear();

    m.lo = {FLT_MAX, FLT_MAX, FLT_MAX};
    m.hi = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    double top_weight = 0.0, any_weight = 0.0;
    double top_rgb[3] = {}, any_rgb[3] = {};
    double top_n[3] = {}, any_n[3] = {};
    double top_si = 0.0, any_si = 0.0;
    for (const DecoTri& t : m.tris) {
        for (const Vector3& p : t.p) {
            m.lo = {std::min(m.lo.x, p.x), std::min(m.lo.y, p.y), std::min(m.lo.z, p.z)};
            m.hi = {std::max(m.hi.x, p.x), std::max(m.hi.y, p.y), std::max(m.hi.z, p.z)};
            m.reach_xz = std::max(m.reach_xz, std::sqrt(p.x * p.x + p.z * p.z));
            m.radius = std::max(m.radius, std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z));
        }
        const double e1[3] = {double(t.p[1].x) - t.p[0].x, double(t.p[1].y) - t.p[0].y, double(t.p[1].z) - t.p[0].z};
        const double e2[3] = {double(t.p[2].x) - t.p[0].x, double(t.p[2].y) - t.p[0].y, double(t.p[2].z) - t.p[0].z};
        const double cx = e1[1] * e2[2] - e1[2] * e2[1];
        const double cy = e1[2] * e2[0] - e1[0] * e2[2];
        const double cz = e1[0] * e2[1] - e1[1] * e2[0];
        const double opaque = t.tex ? t.tex->opaque_fraction : 1.0;
        const double top = 0.5 * std::fabs(cy) * opaque;
        const double any = 0.5 * std::sqrt(cx * cx + cy * cy + cz * cz) * opaque;
        for (int c = 0; c < 3; ++c) {
            const double col = t.tex ? t.tex->opaque_rgb[c] : 128.0;
            top_rgb[c] += col * top;
            any_rgb[c] += col * any;
            // Only faces turned up show from above.
            top_n[c] += t.n[1] > 0.0f ? t.n[c] * top : 0.0;
            any_n[c] += t.n[c] * any;
        }
        top_si += t.self_illum * top;
        any_si += t.self_illum * any;
        top_weight += top;
        any_weight += any;
    }
    const bool from_top = top_weight > 1e-9;
    const double weight = from_top ? top_weight : any_weight;
    if (weight > 1e-12) {
        for (int c = 0; c < 3; ++c) {
            m.rgb[c] = static_cast<float>((from_top ? top_rgb[c] : any_rgb[c]) / weight);
            m.n[c] = static_cast<float>(top_n[c]);
        }
        m.self_illum = static_cast<float>((from_top ? top_si : any_si) / weight);
        if (!normalize3(m.n)) {
            for (int c = 0; c < 3; ++c) m.n[c] = static_cast<float>(any_n[c]);
            if (!normalize3(m.n)) {
                m.n[0] = 0.0f;
                m.n[1] = 1.0f;
                m.n[2] = 0.0f;
            }
        }
    }
    m.cover = static_cast<float>(std::min(top_weight, double(m.hi.x - m.lo.x) * double(m.hi.z - m.lo.z)));
    return m;
}

} // namespace

bool minimap_bake_target(std::string& out_bitmap_name, std::string& out_path, std::string& out_error)
{
    std::string stem = level_file_stem();
    if (stem.empty()) {
        out_error = "Save the level first: the minimap bitmap is named after the level file.";
        return false;
    }
    // The file scan takes the extension at the first dot, so a dotted stem would not register.
    std::replace(stem.begin(), stem.end(), '.', '_');
    constexpr std::string_view suffix = "_mm.dds";
    const std::size_t max_stem = rfl_name_max_len - suffix.size();
    if (stem.size() > max_stem) {
        char hash[8];
        std::snprintf(hash, sizeof(hash), "%06x", fnv1a_lower(stem) & 0xFFFFFFu);
        stem = stem.substr(0, max_stem - 7) + "_" + hash;
    }
    out_bitmap_name = stem + std::string{suffix};

    const std::string root{file_root_path};
    if (root.empty()) {
        out_error = "The RF root directory is unknown.";
        return false;
    }
    out_path = root + "user_maps\\textures\\" + out_bitmap_name;
    return true;
}

bool minimap_bake(CDedLevel& level, const MinimapBakeParams& p, MinimapBakeResult& out, std::string& out_error)
{
    const auto started = std::chrono::steady_clock::now();
    const int res = p.resolution;
    if (res != 512 && res != 1024 && res != 2048) {
        out_error = "Unsupported minimap resolution.";
        return false;
    }
    if (p.use_bounds) {
        const bool finite = std::isfinite(p.bounds_min.x) && std::isfinite(p.bounds_min.z) &&
                            std::isfinite(p.bounds_max.x) && std::isfinite(p.bounds_max.z);
        if (!finite || !(p.bounds_max.x - p.bounds_min.x >= 1.0f) || !(p.bounds_max.z - p.bounds_min.z >= 1.0f)) {
            out_error = "The bounds need Max X and Max Z at least 1 unit above Min X and Min Z.";
            return false;
        }
    }

    std::string bitmap_name;
    std::string path;
    if (!minimap_bake_target(bitmap_name, path, out_error)) {
        return false;
    }

    GSolid* solid = level.solid;
    if (!solid || !solid->face_list_head) {
        out_error = "The level has no geometry. Build the level geometry first.";
        return false;
    }

    try {
        std::vector<GFace*> faces;
        std::unordered_set<GRoom*> rooms;
        Vector3 bb_min{FLT_MAX, FLT_MAX, FLT_MAX};
        Vector3 bb_max{-FLT_MAX, -FLT_MAX, -FLT_MAX};
        auto grow = [&](const Vector3& mn, const Vector3& mx) {
            bb_min = {std::min(bb_min.x, mn.x), std::min(bb_min.y, mn.y), std::min(bb_min.z, mn.z)};
            bb_max = {std::max(bb_max.x, mx.x), std::max(bb_max.y, mx.y), std::max(bb_max.z, mx.z)};
        };

        // Skybox decoration is detail geometry in rooms of its own, which carry no sky flag; the room
        // builder lists them in the sky room's detail_rooms.
        std::unordered_set<GRoom*> sky_rooms;
        int guard = 0;
        for (GFace* face = solid->face_list_head; face && guard < (1 << 22); face = face->next_solid, ++guard) {
            if (GRoom* room = face->which_room; room && room->is_sky) {
                sky_rooms.insert(room);
            }
        }
        const std::vector<GRoom*> sky_parents(sky_rooms.begin(), sky_rooms.end());
        for (GRoom* sky : sky_parents) {
            for (int i = 0; i < sky->detail_rooms.size; ++i) {
                if (GRoom* detail = sky->detail_rooms.data_ptr[i]) {
                    sky_rooms.insert(detail);
                }
            }
        }
        std::unordered_set<int32_t> sky_room_uids;

        guard = 0;
        for (GFace* face = solid->face_list_head; face && guard < (1 << 22); face = face->next_solid, ++guard) {
            GRoom* room = face->which_room;
            if (room && sky_rooms.count(room)) {
                if (room->uid >= 0) sky_room_uids.insert(room->uid);
                continue;
            }
            if (room) {
                if (rooms.insert(room).second) grow(room->bbox_min, room->bbox_max);
            }
            else {
                grow(face->bounding_box_min, face->bounding_box_max);
            }
            if (!face->edge_loop || face->portal_id != 0 || (face->flags & (FACE_INVISIBLE | FACE_SHOW_SKY))) {
                continue;
            }
            const Vector3& n = face->plane.normal;
            const float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
            if (!(len > 0.0f) || n.y / len <= min_up_normal_y) continue;
            faces.push_back(face);
        }

        if (!(bb_min.x <= bb_max.x) || !(bb_min.z <= bb_max.z) || !std::isfinite(bb_max.x - bb_min.x) ||
            !std::isfinite(bb_max.z - bb_min.z)) {
            out_error = "Could not determine the level bounds.";
            return false;
        }

        const bool cut_applied = p.cut_height != 0.0f && std::isfinite(p.cut_height) &&
                                 p.cut_height > bb_min.y && p.cut_height < bb_max.y;
        const float cut = cut_applied ? p.cut_height : FLT_MAX;

        Vector3 world_min;
        Vector3 world_max;
        if (p.use_bounds) {
            world_min = {p.bounds_min.x, bb_min.y, p.bounds_min.z};
            world_max = {p.bounds_max.x, cut_applied ? cut : bb_max.y, p.bounds_max.z};
        }
        else {
            const float cx = std::round((bb_min.x + bb_max.x) * 50.0f) / 100.0f;
            const float cz = std::round((bb_min.z + bb_max.z) * 50.0f) / 100.0f;
            float half = std::max(bb_max.x - bb_min.x, bb_max.z - bb_min.z) * 0.5f;
            half += std::max(1.0f, half * 0.02f);
            half = std::ceil(half * 100.0f) / 100.0f;
            world_min = {cx - half, bb_min.y, cz - half};
            world_max = {cx + half, cut_applied ? cut : bb_max.y, cz + half};
        }

        const VArray<GSurface*>& surfaces = solid->surfaces;
        const auto& props = level.GetAlpineLevelProperties();
        // A level loaded without stock lightmaps points every surface at one placeholder page until
        // Calculate Lighting repacks them.
        const bool lightmaps_placeholder = lightmap_stock_layout_synthesized();
        const LevelLight level_light = make_level_light(level);

        std::unordered_map<int, Texture> textures;
        auto get_texture = [&](int handle) -> const Texture* {
            auto it = textures.find(handle);
            if (it == textures.end()) {
                it = textures.emplace(handle, load_texture(handle)).first;
            }
            return &it->second;
        };

        // A terrain changed since its build saves no build mapping: the game draws its faces as plain faces
        // and places none of its decorations.
        std::vector<bool> resolves(props.terrain_objects.size(), false);
        std::unordered_set<int32_t> stale_rooms;
        int terrains_stale = 0;
        for (std::size_t k = 0; k < resolves.size(); ++k) {
            const DedTerrain* t = props.terrain_objects[k];
            if (!t || !t->data.grid) continue;
            resolves[k] = terrain_build_resolves(level, *t);
            if (resolves[k] || t->data.built_room_uids.empty()) continue;
            ++terrains_stale;
            for (const int32_t uid : t->data.built_room_uids) {
                if (uid >= 0) stale_rooms.insert(uid);
            }
        }

        // By index in terrain_objects, the order the game places decorations in
        std::unordered_map<const TerrainLayerTile*, TileMips> tile_cache;
        std::vector<TerrainShade> terrains(props.terrain_objects.size());
        std::unordered_map<int32_t, uint8_t> room_terrain;
        for (std::size_t k = 0; k < terrains.size(); ++k) {
            const DedTerrain* t = props.terrain_objects[k];
            if (!resolves[k] || t->data.layers.empty()) continue;
            terrains[k] = make_terrain_shade(*t, tile_cache);
            if (k >= UINT8_MAX) continue;
            for (const int32_t uid : t->data.built_room_uids) {
                if (uid >= 0) room_terrain.emplace(uid, static_cast<uint8_t>(k + 1));
            }
        }
        // A split chunk's other rooms map to no terrain: the one whose surface passes nearest the face.
        auto terrain_of = [&](const GRoom& room, const std::vector<WorldVert>& face) -> uint8_t {
            if (auto it = room_terrain.find(room.uid); it != room_terrain.end()) return it->second;
            float c[3] = {};
            for (const WorldVert& v : face) {
                c[0] += v.x;
                c[1] += v.y;
                c[2] += v.z;
            }
            for (float& f : c) f /= static_cast<float>(face.size());
            uint8_t best = 0;
            float best_dy = FLT_MAX;
            for (std::size_t k = 0; k < terrains.size() && k < UINT8_MAX; ++k) {
                if (!terrains[k].terrain) continue;
                const at::GridView& v = terrains[k].v;
                if (c[0] < v.origin[0] || c[0] > v.origin[0] + at::extent(v.nx, v.cell_size) ||
                    c[2] < v.origin[2] || c[2] > v.origin[2] + at::extent(v.nz, v.cell_size)) {
                    continue;
                }
                const float dy = std::fabs(at::height_at(v, c[0], c[2]) - c[1]);
                if (dy <= std::max(1.0f, v.cell_size) && dy < best_dy) {
                    best = static_cast<uint8_t>(k + 1);
                    best_dy = dy;
                }
            }
            return best;
        };

        Raster raster{res, world_min.x, world_max.z, res / (world_max.x - world_min.x),
                      res / (world_max.z - world_min.z)};
        std::vector<WorldVert> poly, clipped;
        std::vector<std::pair<GFace*, FaceShade>> liquids;
        int drawn = 0;
        int lightmapped = 0;

        for (GFace* face : faces) {
            if (cut_applied && face->bounding_box_min.y > cut) continue;
            GRoom* room = face->which_room;
            FaceShade shade;
            if ((face->flags & FACE_LIQUID) && room && room->contains_liquid) {
                shade.liquid = true;
                shade.liquid_rgb[0] = room->liquid_color.r;
                shade.liquid_rgb[1] = room->liquid_color.g;
                shade.liquid_rgb[2] = room->liquid_color.b;
                shade.liquid_alpha = std::clamp(room->liquid_alpha, 0, 255) / 255.0f;
                liquids.emplace_back(face, shade);
                continue;
            }
            if (!gather_face(face, poly)) continue;

            if (room && !stale_rooms.count(room->uid) &&
                (props.is_terrain_room(room->uid) || props.is_terrain_split_room(room->uid))) {
                shade.terrain = terrain_of(*room, poly);
            }
            if (!shade.terrain) {
                if (face->bitmap_id >= 0) shade.tex = get_texture(face->bitmap_id);
                const int surface_index = face->surface_index;
                if (!lightmaps_placeholder && !(face->flags & FACE_FULL_BRIGHT) && surface_index >= 0 &&
                    surface_index < surfaces.size && surfaces.data_ptr) {
                    const GSurface* surface = surfaces.data_ptr[surface_index];
                    const GLightmap* page = surface ? surface->lightmap : nullptr;
                    if (page && page->pixels && page->w > 0 && page->h > 0 && page->w <= 4096 && page->h <= 4096) {
                        shade.lightmap = page;
                    }
                }
            }

            const std::vector<WorldVert>* src = &poly;
            if (cut_applied) {
                clip_below(poly, cut, clipped);
                src = &clipped;
            }
            if (raster.draw_polygon(*src, shade)) {
                ++drawn;
                if (shade.lightmap) ++lightmapped;
            }
        }

        const float pixel_world = raster.pixel_world_size();
        std::vector<bool> terrain_shown(terrains.size(), false);
        const int terrain_pixels = raster.shade_terrain([&](int k, float x, float y, float z) {
            terrain_shown[k] = true;
            return shade_terrain_pixel(terrains[k], level_light, pixel_world, x, y, z);
        });
        int terrains_unlit = 0;
        for (std::size_t k = 0; k < terrains.size(); ++k) {
            if (terrain_shown[k] && !terrains[k].fullbright && !terrains[k].light) ++terrains_unlit;
        }

        // Placed as the game places them: resolvable terrains in record order, one level budget.
        std::unordered_map<std::string, DecoMesh> deco_meshes;
        uint32_t deco_placed = 0;
        int deco_drawn = 0;
        int deco_blended = 0;
        std::size_t deco_tris = 0;
        const float px_per_unit = raster.pixels_per_unit();
        const float px_area = raster.pixel_area_per_unit2();
        const float half_x = (world_max.x - world_min.x) * 0.5f;
        const float half_z = (world_max.z - world_min.z) * 0.5f;
        const float mid_x = world_min.x + half_x;
        const float mid_z = world_min.z + half_z;
        // fn(k, mesh, inst) for every instance in the bounds; a skybox terrain's instances only spend the budget.
        auto for_each_decoration = [&](auto&& fn) {
            at::DecorationBudget budget;
            for (std::size_t k = 0; k < props.terrain_objects.size() && !budget.spent(); ++k) {
                const DedTerrain* t = props.terrain_objects[k];
                TerrainDecorationPlacement placement;
                if (!resolves[k] || t->data.decorations.empty() ||
                    !terrain_decoration_placement(t->pos, t->data, placement)) {
                    continue;
                }
                const bool in_sky = std::any_of(t->data.built_room_uids.begin(), t->data.built_room_uids.end(),
                                                [&](int32_t uid) { return sky_room_uids.count(uid) != 0; });
                const DecoMesh* meshes[at::max_decorations] = {};
                for (uint32_t d = 0; d < placement.count && !in_sky; ++d) {
                    if (!at::decoration_active(placement.views[d])) continue;
                    const std::string& name = t->data.decorations[d].mesh;
                    std::string key = string_to_lower(name);
                    auto it = deco_meshes.find(key);
                    if (it == deco_meshes.end()) {
                        it = deco_meshes.emplace(std::move(key), load_deco_mesh(name, get_texture)).first;
                    }
                    if (!it->second.tris.empty()) meshes[d] = &it->second;
                }
                at::for_each_terrain_decoration(
                    placement.grid, t->uid, placement.layout, placement.views, placement.count, budget,
                    [&](uint32_t, uint32_t d, const at::DecorationInstance& inst) {
                        ++deco_placed;
                        const DecoMesh* mesh = meshes[d];
                        if (!mesh) return true;
                        const float reach = mesh->radius * inst.scale;
                        if (std::fabs(inst.pos[0] - mid_x) <= half_x + reach &&
                            std::fabs(inst.pos[2] - mid_z) <= half_z + reach) {
                            fn(k, *mesh, inst);
                        }
                        return true;
                    });
            }
        };
        auto deco_reach_px = [&](const DecoMesh& mesh, const at::DecorationInstance& inst) {
            return mesh.reach_xz * inst.scale * px_per_unit;
        };
        auto deco_lod = [&](const DecoMesh& mesh, float reach_px) -> const std::vector<DecoTri>& {
            return reach_px < far_lod_reach_px && !mesh.far_tris.empty() ? mesh.far_tris : mesh.tris;
        };

        // Over the triangle budget, the smallest decorations blend instead, evenly across the map.
        std::vector<std::pair<float, uint32_t>> raster_costs;
        std::size_t raster_total = 0;
        for_each_decoration([&](std::size_t, const DecoMesh& mesh, const at::DecorationInstance& inst) {
            const float reach_px = deco_reach_px(mesh, inst);
            if (!(reach_px >= min_raster_reach_px)) return;
            const auto n = static_cast<uint32_t>(deco_lod(mesh, reach_px).size());
            raster_costs.emplace_back(reach_px, n);
            raster_total += n;
        });
        float raster_min_reach_px = min_raster_reach_px;
        bool raster_over_budget = false;
        if (raster_total > max_deco_raster_tris) {
            std::sort(raster_costs.begin(), raster_costs.end(),
                      [](const auto& a, const auto& b) { return a.first > b.first; });
            std::size_t sum = 0;
            for (const auto& [reach_px, n] : raster_costs) {
                if (sum + n > max_deco_raster_tris) {
                    raster_min_reach_px = reach_px;
                    raster_over_budget = true;
                    break;
                }
                sum += n;
            }
            xlog::info("[Minimap] {} decoration triangles over the {} budget: blending decorations reaching {:.1f} px "
                       "or less",
                       raster_total, max_deco_raster_tris, raster_min_reach_px);
        }
        raster_costs = {};

        deco_placed = 0;
        for_each_decoration([&](std::size_t k, const DecoMesh& mesh, const at::DecorationInstance& inst) {
            const float sc = inst.scale;
            auto to_world = [&](const Vector3& p, const float (&uv)[2]) {
                WorldVert w{};
                w.x = inst.pos[0] + (inst.rvec[0] * p.x + inst.uvec[0] * p.y + inst.fvec[0] * p.z) * sc;
                w.y = inst.pos[1] + (inst.rvec[1] * p.x + inst.uvec[1] * p.y + inst.fvec[1] * p.z) * sc;
                w.z = inst.pos[2] + (inst.rvec[2] * p.x + inst.uvec[2] * p.y + inst.fvec[2] * p.z) * sc;
                w.u = uv[0];
                w.v = uv[1];
                return w;
            };
            auto to_world_normal = [&](const float (&n)[3], float (&out)[3]) {
                for (int c = 0; c < 3; ++c) out[c] = inst.rvec[c] * n[0] + inst.uvec[c] * n[1] + inst.fvec[c] * n[2];
                if (!normalize3(out)) {
                    out[0] = 0.0f;
                    out[1] = 1.0f;
                    out[2] = 0.0f;
                }
            };
            float texel[3];
            if (terrains[k].terrain && terrains[k].light) {
                terrain_baked_light_sample(*terrains[k].light, inst.base[0], inst.base[2], texel);
            }
            else {
                fallback_light(level_light, inst.normal, texel);
            }
            const DecoLight deco_light = make_deco_light(level_light, texel);

            const float reach_px = deco_reach_px(mesh, inst);
            if (!(reach_px >= min_raster_reach_px) || (raster_over_budget && reach_px <= raster_min_reach_px)) {
                if (inst.base[1] > cut) return;
                static const float no_uv[2] = {};
                float top = -FLT_MAX;
                for (int c = 0; c < 8; ++c) {
                    const Vector3 corner{(c & 1) ? mesh.hi.x : mesh.lo.x, (c & 2) ? mesh.hi.y : mesh.lo.y,
                                         (c & 4) ? mesh.hi.z : mesh.lo.z};
                    top = std::max(top, to_world(corner, no_uv).y);
                }
                float n[3], light[3], rgb[3];
                to_world_normal(mesh.n, n);
                deco_pixel_light(level_light, deco_light, n, mesh.self_illum, light);
                for (int c = 0; c < 3; ++c) rgb[c] = std::min(mesh.rgb[c] * light[c], 255.0f);
                // A slope rises up to a pixel's width across the pixel the instance lands in.
                raster.splat(inst.pos[0], inst.pos[2], std::min(top, cut) + pixel_world, rgb,
                             std::min(1.0f, mesh.cover * sc * sc * px_area));
                ++deco_blended;
                return;
            }

            const std::vector<DecoTri>& tris = deco_lod(mesh, reach_px);
            float n[3], light[3];
            FaceShade shade;
            shade.light = light;
            bool drew = false;
            for (const DecoTri& tri : tris) {
                to_world_normal(tri.n, n);
                deco_pixel_light(level_light, deco_light, n, tri.self_illum, light);
                shade.tex = tri.tex;
                poly.assign({to_world(tri.p[0], tri.uv[0]), to_world(tri.p[1], tri.uv[1]),
                             to_world(tri.p[2], tri.uv[2])});
                const std::vector<WorldVert>* src = &poly;
                if (cut_applied) {
                    clip_below(poly, cut, clipped);
                    src = &clipped;
                }
                drew |= raster.draw_polygon(*src, shade);
            }
            if (drew) {
                ++deco_drawn;
                deco_tris += tris.size();
            }
        });

        for (auto& [face, shade] : liquids) {
            if (!gather_face(face, poly)) continue;
            const std::vector<WorldVert>* src = &poly;
            if (cut_applied) {
                clip_below(poly, cut, clipped);
                src = &clipped;
            }
            if (raster.draw_polygon(*src, shade)) ++drawn;
        }

        const std::vector<uint8_t> dds = encode_dds_dxt1(res, raster.pixels());
        const std::string dir{std::string{file_root_path} + "user_maps"};
        const std::string tex_dir = dir + "\\textures";
        if (!ensure_dir(dir) || !ensure_dir(tex_dir)) {
            out_error = "Could not create " + tex_dir + ".";
            return false;
        }
        if (!write_file(path, dds)) {
            out_error = "Could not write " + path + ".";
            return false;
        }
        xlog::info("[Minimap] Baked {} ({}x{}, {} faces, {} lightmapped{}, {} terrain pixels, {} decorations placed: "
                   "{} rasterized ({} triangles), {} blended)",
                   path, res, res, drawn, lightmapped, lightmaps_placeholder ? ", placeholder lightmaps ignored" : "",
                   terrain_pixels, deco_placed, deco_drawn, deco_tris, deco_blended);

        register_written_file(path.c_str());
        if (!reload_bitmap_in_place(bitmap_name.c_str())) {
            xlog::warn("[Minimap] Could not reload bitmap '{}'", bitmap_name);
        }

        out.bitmap_name = bitmap_name;
        out.world_min = world_min;
        out.world_max = world_max;
        out.faces_drawn = drawn;
        out.faces_lightmapped = lightmapped;
        out.lightmaps_placeholder = lightmaps_placeholder;
        out.terrains_unlit = terrains_unlit;
        out.terrains_stale = terrains_stale;
        out.decorations_drawn = deco_drawn;
        out.decorations_blended = deco_blended;
        out.cut_applied = cut_applied;
        out.cut_height = cut_applied ? cut : 0.0f;
        out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        return true;
    }
    catch (const std::bad_alloc&) {
        out_error = "Out of memory while baking the minimap.";
        return false;
    }
}
