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
#include <xlog/xlog.h>
#include <patch_common/MemUtils.h>
#include <common/utils/string-utils.h>
#include "minimap_bake.h"
#include "level.h"
#include "meshes.h"
#include "textures.h"
#include "vtypes.h"

namespace
{

constexpr float min_up_normal_y = 0.05f;
constexpr int max_texture_dim = 128;
constexpr int max_source_texture_dim = 8192;
constexpr uint32_t fallback_texel = 0xFF808080u;
constexpr int alpha_test_ref = 128;

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

struct FaceShade
{
    const Texture* tex = nullptr;
    const GLightmap* lightmap = nullptr;
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
        color_(static_cast<std::size_t>(res) * res, 0u)
    {}

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
        if (shade.tex) {
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
                depth_[i] = y;
                color_[i] = argb(255, r, g, bl);
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

bool write_tga(const std::string& path, int res, const std::vector<uint32_t>& pixels)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const uint8_t header[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                static_cast<uint8_t>(res & 0xFF), static_cast<uint8_t>(res >> 8),
                                static_cast<uint8_t>(res & 0xFF), static_cast<uint8_t>(res >> 8),
                                32, 8};
    bool ok = std::fwrite(header, sizeof(header), 1, f) == 1;
    // Bottom-left origin: rows go out bottom-up.
    for (int row = res - 1; ok && row >= 0; --row) {
        ok = std::fwrite(pixels.data() + static_cast<std::size_t>(row) * res, 4, res, f) ==
             static_cast<std::size_t>(res);
    }
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
    constexpr std::string_view suffix = "_minimap.tga";
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

        guard = 0;
        for (GFace* face = solid->face_list_head; face && guard < (1 << 22); face = face->next_solid, ++guard) {
            GRoom* room = face->which_room;
            if (room && sky_rooms.count(room)) continue;
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

        std::unordered_map<int, Texture> textures;
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

            if (face->bitmap_id >= 0) {
                auto it = textures.find(face->bitmap_id);
                if (it == textures.end()) {
                    it = textures.emplace(face->bitmap_id, load_texture(face->bitmap_id)).first;
                }
                shade.tex = &it->second;
            }
            const int surface_index = face->surface_index;
            if (!(face->flags & FACE_FULL_BRIGHT) && surface_index >= 0 && surface_index < surfaces.size &&
                surfaces.data_ptr) {
                const GSurface* surface = surfaces.data_ptr[surface_index];
                const GLightmap* page = surface ? surface->lightmap : nullptr;
                if (page && page->pixels && page->w > 0 && page->h > 0 && page->w <= 4096 && page->h <= 4096) {
                    shade.lightmap = page;
                }
            }

            if (!gather_face(face, poly)) continue;
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

        for (auto& [face, shade] : liquids) {
            if (!gather_face(face, poly)) continue;
            const std::vector<WorldVert>* src = &poly;
            if (cut_applied) {
                clip_below(poly, cut, clipped);
                src = &clipped;
            }
            if (raster.draw_polygon(*src, shade)) ++drawn;
        }

        const std::string dir{std::string{file_root_path} + "user_maps"};
        const std::string tex_dir = dir + "\\textures";
        if (!ensure_dir(dir) || !ensure_dir(tex_dir)) {
            out_error = "Could not create " + tex_dir + ".";
            return false;
        }
        if (!write_tga(path, res, raster.pixels())) {
            out_error = "Could not write " + path + ".";
            return false;
        }
        xlog::info("[Minimap] Baked {} ({}x{}, {} faces, {} lightmapped)", path, res, res, drawn, lightmapped);

        register_written_file(path.c_str());
        if (!reload_bitmap_in_place(bitmap_name.c_str())) {
            xlog::warn("[Minimap] Could not reload bitmap '{}'", bitmap_name);
        }

        out.bitmap_name = bitmap_name;
        out.world_min = world_min;
        out.world_max = world_max;
        out.faces_drawn = drawn;
        out.faces_lightmapped = lightmapped;
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
