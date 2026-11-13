#pragma once

// Terrain paint brush math: dabs on the raw weight maps, overlay and decoration coverage and hole mask, and the
// per-stroke undo diffs. No RED dependencies, so the standalone self-check compiles it as is.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>
#include <common/terrain/alpine_terrain.h>

namespace terrain_paint
{

namespace at = alpine_terrain;

enum class Falloff : int
{
    smooth = 0,
    linear = 1,
    constant = 2,
};

enum class Tool : int
{
    paint_layer = 0,
    erase = 1,
    smooth = 2,
    paint_holes = 3,
    clear_holes = 4,
    raise = 5,
    lower = 6,
    smooth_heights = 7,
    flatten = 8,
    set_height = 9,
    noise = 10,
    geo_chunks = 11,
    ramp = 12,
    ramp_between = 13,
};

inline constexpr int tool_count = 14;

inline bool tool_edits_holes(Tool t)
{
    return t == Tool::paint_holes || t == Tool::clear_holes;
}

inline bool tool_edits_heights(Tool t)
{
    return (t >= Tool::raise && t <= Tool::noise) || t == Tool::ramp || t == Tool::ramp_between;
}

// Held still, these keep dabbing at the last spot; noise reaches its full shape in one pass, and a
// Bridge Points stroke is applied once, on release.
inline bool tool_repeats_in_place(Tool t)
{
    return tool_edits_heights(t) && t != Tool::noise && t != Tool::ramp_between;
}

// ─── Geoable Chunks ───────────────────────────────────────────────────────────
// Positions in cells from the grid origin, clamped to the grid.

inline std::uint32_t chunk_at(const at::ChunkLayout& l, float x, float z)
{
    const std::uint32_t across = at::chunks_along(l.cells_x, l.edge), down = at::chunks_along(l.cells_z, l.edge);
    if (across == 0 || down == 0) return 0;
    auto axis = [&](float c, std::uint32_t n, std::uint32_t count) {
        const float f = std::isfinite(c) ? std::clamp(c, 0.0f, static_cast<float>(n)) : 0.0f;
        return std::min(static_cast<std::uint32_t>(f) / l.edge, count - 1);
    };
    return axis(z, l.cells_z, down) * across + axis(x, l.cells_x, across);
}

// Clips the segment (x0, z0)-(x1, z1) to [0, w] x [0, h] (Liang-Barsky). False when none of it is inside.
inline bool clip_segment(float& x0, float& z0, float& x1, float& z1, float w, float h)
{
    const float dx = x1 - x0, dz = z1 - z0;
    float t0 = 0.0f, t1 = 1.0f;
    // p * t <= q for t in [t0, t1]
    auto edge = [&](float p, float q) {
        if (p == 0.0f) return q >= 0.0f;
        const float t = q / p;
        if (p < 0.0f) t0 = std::max(t0, t);
        else t1 = std::min(t1, t);
        return t0 <= t1;
    };
    if (!(edge(-dx, x0) && edge(dx, w - x0) && edge(-dz, z0) && edge(dz, h - z0))) return false;
    const float ax = x0, az = z0;
    x0 = ax + t0 * dx;
    z0 = az + t0 * dz;
    x1 = ax + t1 * dx;
    z1 = az + t1 * dz;
    return true;
}

// visit(k) for every chunk whose closed cell rect the segment (x0, z0)-(x1, z1) touches.
template<typename Visit>
void chunks_on_segment(const at::ChunkLayout& l, float x0, float z0, float x1, float z1, Visit&& visit)
{
    auto clampc = [](float c, std::uint32_t n) {
        return std::isfinite(c) ? std::clamp(c, 0.0f, static_cast<float>(n)) : 0.0f;
    };
    x0 = clampc(x0, l.cells_x);
    x1 = clampc(x1, l.cells_x);
    z0 = clampc(z0, l.cells_z);
    z1 = clampc(z1, l.cells_z);
    const std::uint32_t n = at::layout_chunk_count(l);
    for (std::uint32_t k = 0; k < n; k++) {
        const at::ChunkRect r = at::chunk_rect(l.cells_x, l.cells_z, l.edge, k);
        // Slab clip of t in [0, 1] against the rect on each axis.
        float t0 = 0.0f, t1 = 1.0f;
        auto slab = [&](float a, float b, float lo, float hi) {
            const float d = b - a;
            if (d == 0.0f) return a >= lo && a <= hi;
            float ta = (lo - a) / d, tb = (hi - a) / d;
            if (ta > tb) std::swap(ta, tb);
            t0 = std::max(t0, ta);
            t1 = std::min(t1, tb);
            return t0 <= t1;
        };
        if (slab(x0, x1, static_cast<float>(r.x0), static_cast<float>(r.x1)) &&
            slab(z0, z1, static_cast<float>(r.z0), static_cast<float>(r.z1))) {
            visit(k);
        }
    }
}

// ─── Layer list ───────────────────────────────────────────────────────────────
// The panel lists the base layers, then the overlays, then the decorations; its selection is one index into all.

struct LayerListShape
{
    int layers = 0;
    std::vector<std::string> overlays;    // texture names
    std::vector<std::string> decorations; // mesh names
};

// Entry k of a named list after it changed from `before` to `after`: it stays while the entry keeps its name,
// follows its name if exactly one entry has it (a reorder), else keeps its index if no entry was removed (its
// name was replaced); otherwise -1.
inline int remap_named_entry(int k, const std::vector<std::string>& before, const std::vector<std::string>& after)
{
    const int n = static_cast<int>(after.size());
    if (k >= static_cast<int>(before.size())) return -1;
    const std::string& name = before[k];
    if (k < n && after[k] == name) return k;
    if (std::count(after.begin(), after.end(), name) == 1) {
        return static_cast<int>(std::find(after.begin(), after.end(), name) - after.begin());
    }
    return k < n && n >= static_cast<int>(before.size()) ? k : -1;
}

// The selection after the lists changed from `before` to `after`, never moving between a base layer, an
// overlay and a decoration: a base layer keeps its index (clamped); an overlay or a decoration follows
// remap_named_entry within its own list; otherwise base layer 0 is selected.
inline int remap_layer_selection(int sel, const LayerListShape& before, const LayerListShape& after)
{
    if (sel < 0 || after.layers < 1) return 0;
    if (sel < before.layers) return std::min(sel, after.layers - 1);
    const int k = sel - before.layers;
    const int overlays = static_cast<int>(before.overlays.size());
    if (k < overlays) {
        const int r = remap_named_entry(k, before.overlays, after.overlays);
        return r < 0 ? 0 : after.layers + r;
    }
    const int r = remap_named_entry(k - overlays, before.decorations, after.decorations);
    return r < 0 ? 0 : after.layers + static_cast<int>(after.overlays.size()) + r;
}

// Brush weight at t = distance / radius: 1 at the centre, 0 at and past the rim.
inline float falloff_weight(Falloff f, float t)
{
    if (!(t < 1.0f)) return 0.0f;
    t = std::max(t, 0.0f);
    switch (f) {
    case Falloff::linear: return 1.0f - t;
    case Falloff::constant: return 1.0f;
    default: return 0.5f + 0.5f * std::cos(t * 3.14159265f);
    }
}

// [x0, x1) x [z0, z1); empty when x0 >= x1 or z0 >= z1.
struct Rect
{
    std::uint32_t x0 = 0, z0 = 0, x1 = 0, z1 = 0;

    bool empty() const { return x0 >= x1 || z0 >= z1; }
    std::size_t area() const { return empty() ? 0 : static_cast<std::size_t>(x1 - x0) * (z1 - z0); }
};

inline Rect rect_union(const Rect& a, const Rect& b)
{
    if (a.empty()) return b;
    if (b.empty()) return a;
    return {std::min(a.x0, b.x0), std::min(a.z0, b.z0), std::max(a.x1, b.x1), std::max(a.z1, b.z1)};
}

// A dab in cell units: centre (cells from the grid origin along x and z) and radius.
struct Dab
{
    float cx = 0.0f, cz = 0.0f;
    float radius = 1.0f;
    float strength = 1.0f; // 0..1
    Falloff falloff = Falloff::smooth;
};

// At sample (i, j), which may lie off the grid.
inline float dab_amount(const Dab& d, std::uint32_t per_cell, float i, float j)
{
    const float x = (i + 0.5f) / static_cast<float>(per_cell) - d.cx;
    const float z = (j + 0.5f) / static_cast<float>(per_cell) - d.cz;
    const float t = std::sqrt(x * x + z * z) / std::max(d.radius, 1e-6f);
    return std::clamp(d.strength, 0.0f, 1.0f) * falloff_weight(d.falloff, t);
}

inline bool rect_has(const Rect& r, std::uint32_t i, std::uint32_t j)
{
    return i >= r.x0 && i < r.x1 && j >= r.z0 && j < r.z1;
}

// ─── Mirror painting ──────────────────────────────────────────────────────────
// A dab also applies at each mirror image: image k flips x on bit 0 and z on bit 1 (image 0 is the dab).
// Samples are worked folded into the dab's frame, and overlapping images apply in an order that depends only
// on their contributions, so mirror partners get the same bits.

inline constexpr int max_images = 4;

struct Mirror
{
    bool x = false, z = false;
    std::int32_t x2 = 0, z2 = 0; // twice each line's position, in cells from the grid origin
};

inline bool mirror_on(const Mirror* m)
{
    return m && (m->x || m->z);
}

inline bool image_on(const Mirror* m, int k)
{
    return k == 0 || (mirror_on(m) && (!(k & 1) || m->x) && (!(k & 2) || m->z));
}

// A sample lattice: `per_cell` samples per cell, centred in their share of a cell, or on the grid lines
// (vertices).
struct Lattice
{
    std::uint32_t per_cell = 1;
    bool centred = true;
};

// Sample i's mirror across a line at line2 / 2 cells is fold - i.
inline std::int64_t lattice_fold(std::int32_t line2, const Lattice& l)
{
    return static_cast<std::int64_t>(line2) * l.per_cell - (l.centred ? 1 : 0);
}

// Signed sample indices [x0, x1) x [z0, z1), not clipped to the grid.
struct Span
{
    std::int64_t x0 = 0, z0 = 0, x1 = 0, z1 = 0;

    bool has(std::int64_t i, std::int64_t j) const { return i >= x0 && i < x1 && j >= z0 && j < z1; }
};

// Indices ceil(a) through floor(b); none for a bound that is not finite.
inline void signed_span(float a, float b, std::int64_t& lo, std::int64_t& hi)
{
    lo = hi = 0;
    if (!std::isfinite(a) || !std::isfinite(b)) return;
    constexpr float lim = 1073741824.0f;
    lo = static_cast<std::int64_t>(std::clamp(std::ceil(a), -lim, lim));
    hi = static_cast<std::int64_t>(std::clamp(std::floor(b) + 1.0f, -lim, lim));
}

// Samples whose centres, at (i + 0.5) / per_cell cells, lie within the dab.
inline Span dab_reach(const Dab& d, std::uint32_t per_cell)
{
    const float pc = static_cast<float>(per_cell);
    Span s;
    signed_span((d.cx - d.radius) * pc - 0.5f, (d.cx + d.radius) * pc - 0.5f, s.x0, s.x1);
    signed_span((d.cz - d.radius) * pc - 0.5f, (d.cz + d.radius) * pc - 0.5f, s.z0, s.z1);
    return s;
}

// The samples a dab reaches in each image, on a w x h lattice.
struct ImageSet
{
    const Mirror* mirror = nullptr; // null when no line is on
    std::int64_t fold_x = 0, fold_z = 0;
    Span reach;                     // in the dab's own frame
    Rect rects[max_images];         // empty for images that are off
};

inline ImageSet image_set(const Mirror* m, const Lattice& l, const Span& reach, std::uint32_t w, std::uint32_t h)
{
    ImageSet s;
    s.mirror = mirror_on(m) ? m : nullptr;
    if (s.mirror) {
        s.fold_x = lattice_fold(m->x2, l);
        s.fold_z = lattice_fold(m->z2, l);
    }
    s.reach = reach;
    auto clip = [](std::int64_t v, std::uint32_t n) {
        return static_cast<std::uint32_t>(std::clamp<std::int64_t>(v, 0, n));
    };
    for (int k = 0; k < max_images; k++) {
        if (!image_on(s.mirror, k)) continue;
        std::int64_t x0 = reach.x0, x1 = reach.x1, z0 = reach.z0, z1 = reach.z1;
        if (k & 1) {
            x0 = s.fold_x - reach.x1 + 1;
            x1 = s.fold_x - reach.x0 + 1;
        }
        if (k & 2) {
            z0 = s.fold_z - reach.z1 + 1;
            z1 = s.fold_z - reach.z0 + 1;
        }
        s.rects[k] = {clip(x0, w), clip(z0, h), clip(x1, w), clip(z1, h)};
    }
    return s;
}

inline bool image_set_empty(const ImageSet& s)
{
    for (const Rect& r : s.rects) {
        if (!r.empty()) return false;
    }
    return true;
}

// A sample as image k's dab sees it: folded into the dab's frame, and whether the dab reaches it there.
struct Fold
{
    std::int64_t i = 0, j = 0;
    bool reached = false;
};

// visit(i, j, k, folds) once for every sample some image reaches, k being the first image whose rect
// holds it; true when it changed the sample. Returns the samples changed and adds each to per_image[k].
template<typename Visit>
Rect visit_images(const ImageSet& s, Rect* per_image, Visit&& visit)
{
    Rect changed;
    for (int k = 0; k < max_images; k++) {
        const Rect& r = s.rects[k];
        for (std::uint32_t j = r.z0; j < r.z1; j++) {
            for (std::uint32_t i = r.x0; i < r.x1; i++) {
                bool seen = false;
                for (int e = 0; e < k && !seen; e++) seen = rect_has(s.rects[e], i, j);
                if (seen) continue;
                Fold f[max_images];
                for (int e = 0; e < max_images; e++) {
                    if (!image_on(s.mirror, e)) continue;
                    f[e].i = (e & 1) ? s.fold_x - i : i;
                    f[e].j = (e & 2) ? s.fold_z - j : j;
                    f[e].reached = s.reach.has(f[e].i, f[e].j);
                }
                if (!visit(i, j, k, f)) continue;
                changed = rect_union(changed, {i, j, i + 1, j + 1});
                if (per_image) per_image[k] = rect_union(per_image[k], {i, j, i + 1, j + 1});
            }
        }
    }
    return changed;
}

// One image's share of a sample: its weight, its target (sculpting), and where the sample sits in its frame.
struct Contrib
{
    float w = 0.0f;
    double target = 0.0;
    std::int64_t i = 0, j = 0;
    int image = 0;
};

inline bool contrib_before(const Contrib& a, const Contrib& b)
{
    if (a.w != b.w) return a.w > b.w;
    if (a.i != b.i) return a.i < b.i;
    if (a.j != b.j) return a.j < b.j;
    return a.target < b.target;
}

inline void order_contribs(Contrib* c, int n)
{
    for (int a = 1; a < n; a++) {
        for (int b = a; b > 0 && contrib_before(c[b], c[b - 1]); b--) std::swap(c[b], c[b - 1]);
    }
}

// The images that reach a sample, weighed by weigh(fold, image, target) and put in application order.
template<typename Weigh>
int gather_contribs(const ImageSet& s, const Fold (&f)[max_images], Contrib (&out)[max_images], Weigh&& weigh)
{
    int n = 0;
    for (int e = 0; e < max_images; e++) {
        if (!image_on(s.mirror, e) || !f[e].reached) continue;
        Contrib& c = out[n++];
        c = Contrib{};
        c.i = f[e].i;
        c.j = f[e].j;
        c.image = e;
        c.w = weigh(f[e], e, c.target);
    }
    order_contribs(out, n);
    return n;
}

// Per image, the samples a dab reached, padded by `pad` and clipped to the grid, as they were before it.
template<typename T>
struct Snapshots
{
    Rect box[max_images];
    std::vector<T> data[max_images];

    template<typename Read>
    void take(const ImageSet& s, std::uint32_t pad, std::uint32_t w, std::uint32_t h, std::size_t stride, Read&& read)
    {
        for (int k = 0; k < max_images; k++) {
            const Rect& r = s.rects[k];
            box[k] = {};
            if (r.empty()) continue;
            box[k] = {r.x0 > pad ? r.x0 - pad : 0, r.z0 > pad ? r.z0 - pad : 0, std::min(r.x1 + pad, w),
                      std::min(r.z1 + pad, h)};
            data[k].resize(box[k].area() * stride);
            for (std::uint32_t j = box[k].z0; j < box[k].z1; j++) {
                for (std::uint32_t i = box[k].x0; i < box[k].x1; i++) read(i, j, at(k, i, j, stride));
            }
        }
    }

    T* at(int k, std::uint32_t i, std::uint32_t j, std::size_t stride)
    {
        const Rect& b = box[k];
        return &data[k][(static_cast<std::size_t>(j - b.z0) * (b.x1 - b.x0) + (i - b.x0)) * stride];
    }
};

// Both RGBA8 weight maps of a grid, as the blob stores them.
struct WeightMaps
{
    std::uint8_t* data;
    std::uint32_t w, h;     // weight_width / weight_height
    std::uint32_t mul;      // weight texels per cell
    std::uint32_t layer_count;

    std::size_t map_bytes() const { return static_cast<std::size_t>(w) * h * 4; }

    void get(std::size_t texel, std::uint8_t (&v)[at::max_layers]) const
    {
        at::texel_weights(data, map_bytes(), texel, v);
    }

    void set(std::size_t texel, const std::uint8_t (&v)[at::max_layers])
    {
        at::set_texel_weights(data, map_bytes(), texel, v);
    }
};

// Below this a dab leaves a texel alone, so the rim of a soft brush does not creep.
inline constexpr float min_amount = 0.5f / 255.0f;

// Moves each texel toward pure `layer` by the dab amount: every other channel (layers past
// layer_count included) is scaled down and rounded down, and `layer` takes the rest, so the texel
// sums to 255 and any amount above min_amount makes progress. Returns the texels changed.
inline Rect paint_layer(WeightMaps& m, const Dab& d, std::uint32_t layer, const Mirror* mirror = nullptr,
                        Rect* per_image = nullptr)
{
    if (layer >= at::max_layers || layer >= m.layer_count) return {};
    const ImageSet s = image_set(mirror, {m.mul, true}, dab_reach(d, m.mul), m.w, m.h);
    return visit_images(s, per_image, [&](std::uint32_t i, std::uint32_t j, int, const Fold(&f)[max_images]) {
        Contrib c[max_images];
        const int n = gather_contribs(s, f, c, [&](const Fold& p, int, double&) {
            return dab_amount(d, m.mul, static_cast<float>(p.i), static_cast<float>(p.j));
        });
        const std::size_t t = static_cast<std::size_t>(j) * m.w + i;
        std::uint8_t v[at::max_layers];
        m.get(t, v);
        std::uint8_t cur[at::max_layers];
        std::memcpy(cur, v, sizeof(v));
        for (int q = 0; q < n; q++) {
            const float a = c[q].w;
            if (a < min_amount) continue;
            std::uint8_t out[at::max_layers];
            unsigned rest = 0;
            for (std::uint32_t ch = 0; ch < at::max_layers; ch++) {
                if (ch == layer) continue;
                out[ch] = static_cast<std::uint8_t>(std::floor(static_cast<float>(cur[ch]) * (1.0f - a)));
                rest += out[ch];
            }
            out[layer] = static_cast<std::uint8_t>(255u - std::min(rest, 255u));
            at::normalize_weights(out);
            std::memcpy(cur, out, sizeof(out));
        }
        if (std::memcmp(cur, v, sizeof(v)) == 0) return false;
        m.set(t, cur);
        return true;
    });
}

// Blends each texel toward the 3x3 average of the texels around it (as they were before the dab, kept in
// `src`, the stroke's scratch), then renormalizes. Returns the texels changed.
inline Rect smooth_weights(WeightMaps& m, const Dab& d, Snapshots<std::uint8_t>& src, const Mirror* mirror = nullptr,
                           Rect* per_image = nullptr)
{
    const ImageSet s = image_set(mirror, {m.mul, true}, dab_reach(d, m.mul), m.w, m.h);
    if (image_set_empty(s)) return {};
    src.take(s, 1, m.w, m.h, at::max_layers, [&](std::uint32_t i, std::uint32_t j, std::uint8_t* out) {
        std::uint8_t v[at::max_layers];
        m.get(static_cast<std::size_t>(j) * m.w + i, v);
        std::memcpy(out, v, sizeof(v));
    });
    return visit_images(s, per_image, [&](std::uint32_t i, std::uint32_t j, int k, const Fold(&f)[max_images]) {
        Contrib c[max_images];
        const int n = gather_contribs(s, f, c, [&](const Fold& p, int, double&) {
            return dab_amount(d, m.mul, static_cast<float>(p.i), static_cast<float>(p.j));
        });
        const Rect& b = src.box[k];
        float avg[at::max_layers] = {};
        int count = 0;
        for (std::uint32_t z = (j > b.z0 ? j - 1 : j); z <= std::min(j + 1, b.z1 - 1); z++) {
            for (std::uint32_t x = (i > b.x0 ? i - 1 : i); x <= std::min(i + 1, b.x1 - 1); x++) {
                const std::uint8_t* p = src.at(k, x, z, at::max_layers);
                for (std::uint32_t ch = 0; ch < at::max_layers; ch++) avg[ch] += p[ch];
                count++;
            }
        }
        const std::uint8_t* was = src.at(k, i, j, at::max_layers);
        std::uint8_t cur[at::max_layers];
        std::memcpy(cur, was, sizeof(cur));
        for (int q = 0; q < n; q++) {
            const float a = c[q].w;
            if (a < min_amount) continue;
            std::uint8_t out[at::max_layers];
            for (std::uint32_t ch = 0; ch < at::max_layers; ch++) {
                const float target = avg[ch] / static_cast<float>(count);
                const float v = static_cast<float>(cur[ch]) + (target - static_cast<float>(cur[ch])) * a;
                out[ch] = static_cast<std::uint8_t>(std::clamp(std::lround(v), 0L, 255L));
            }
            at::normalize_weights(out);
            std::memcpy(cur, out, sizeof(out));
        }
        if (std::memcmp(cur, was, sizeof(cur)) == 0) return false;
        m.set(static_cast<std::size_t>(j) * m.w + i, cur);
        return true;
    });
}

// One channel of a coverage map laid out as a weight map with `stride` channels per texel: the overlay
// coverage map, or a decoration's plane (stride 1).
struct CoverageMap
{
    std::uint8_t* data;
    std::uint32_t w, h;   // weight_width / weight_height
    std::uint32_t mul;    // texels per cell
    std::uint32_t channel;
    std::uint32_t stride = at::max_overlays;
};

// Moves each texel's coverage toward 255 (raise) or 0 by the dab amount, rounding in favour of the
// move as paint_layer does, so any amount above min_amount makes progress. No normalization. Returns
// the texels changed.
inline Rect paint_coverage(CoverageMap& m, const Dab& d, bool raise, const Mirror* mirror = nullptr,
                           Rect* per_image = nullptr)
{
    if (m.channel >= m.stride) return {};
    const ImageSet s = image_set(mirror, {m.mul, true}, dab_reach(d, m.mul), m.w, m.h);
    return visit_images(s, per_image, [&](std::uint32_t i, std::uint32_t j, int, const Fold(&f)[max_images]) {
        Contrib c[max_images];
        const int n = gather_contribs(s, f, c, [&](const Fold& p, int, double&) {
            return dab_amount(d, m.mul, static_cast<float>(p.i), static_cast<float>(p.j));
        });
        std::uint8_t& v = m.data[(static_cast<std::size_t>(j) * m.w + i) * m.stride + m.channel];
        std::uint8_t cur = v;
        for (int q = 0; q < n; q++) {
            const float a = c[q].w;
            if (a < min_amount) continue;
            const float rest = raise ? 255.0f - cur : static_cast<float>(cur);
            const auto left = static_cast<std::uint8_t>(std::floor(rest * (1.0f - a)));
            cur = raise ? static_cast<std::uint8_t>(255 - left) : left;
        }
        if (cur == v) return false;
        v = cur;
        return true;
    });
}

// Blends each texel's coverage toward the 3x3 average around it (as it was before the dab), as
// smooth_weights does. Returns the texels changed.
inline Rect smooth_coverage(CoverageMap& m, const Dab& d, Snapshots<std::uint8_t>& src, const Mirror* mirror = nullptr,
                            Rect* per_image = nullptr)
{
    if (m.channel >= m.stride) return {};
    const ImageSet s = image_set(mirror, {m.mul, true}, dab_reach(d, m.mul), m.w, m.h);
    if (image_set_empty(s)) return {};
    auto at_ = [&](std::uint32_t i, std::uint32_t j) -> std::uint8_t& {
        return m.data[(static_cast<std::size_t>(j) * m.w + i) * m.stride + m.channel];
    };
    src.take(s, 1, m.w, m.h, 1, [&](std::uint32_t i, std::uint32_t j, std::uint8_t* out) { *out = at_(i, j); });
    return visit_images(s, per_image, [&](std::uint32_t i, std::uint32_t j, int k, const Fold(&f)[max_images]) {
        Contrib c[max_images];
        const int n = gather_contribs(s, f, c, [&](const Fold& p, int, double&) {
            return dab_amount(d, m.mul, static_cast<float>(p.i), static_cast<float>(p.j));
        });
        const Rect& b = src.box[k];
        float sum = 0.0f;
        int count = 0;
        for (std::uint32_t z = (j > b.z0 ? j - 1 : j); z <= std::min(j + 1, b.z1 - 1); z++) {
            for (std::uint32_t x = (i > b.x0 ? i - 1 : i); x <= std::min(i + 1, b.x1 - 1); x++) {
                sum += *src.at(k, x, z, 1);
                count++;
            }
        }
        std::uint8_t out = *src.at(k, i, j, 1);
        for (int q = 0; q < n; q++) {
            const float a = c[q].w;
            if (a < min_amount) continue;
            const float cur = out;
            const float v = cur + (sum / static_cast<float>(count) - cur) * a;
            out = static_cast<std::uint8_t>(std::clamp(std::lround(v), 0L, 255L));
        }
        if (out == at_(i, j)) return false;
        at_(i, j) = out;
        return true;
    });
}

// Sets or clears the hole bit of every cell whose centre is inside the dab (strength and falloff do
// not apply to holes). Returns the cells changed.
inline Rect paint_holes(std::uint8_t* holes, std::uint32_t cells_x, std::uint32_t cells_z, const Dab& d, bool hole,
                        const Mirror* mirror = nullptr, Rect* per_image = nullptr)
{
    const ImageSet s = image_set(mirror, {1, true}, dab_reach(d, 1), cells_x, cells_z);
    return visit_images(s, per_image, [&](std::uint32_t x, std::uint32_t z, int, const Fold(&f)[max_images]) {
        bool inside = false;
        for (int e = 0; e < max_images && !inside; e++) {
            if (!image_on(s.mirror, e) || !f[e].reached) continue;
            const float dx = static_cast<float>(f[e].i) + 0.5f - d.cx, dz = static_cast<float>(f[e].j) + 0.5f - d.cz;
            inside = dx * dx + dz * dz < d.radius * d.radius;
        }
        if (!inside || at::get_cell_bit(holes, cells_x, x, z) == hole) return false;
        at::set_cell_bit(holes, cells_x, x, z, hole);
        return true;
    });
}

// ─── Sculpting ────────────────────────────────────────────────────────────────
// Dabs on the vertex heights, worked in offsets above origin.y. A vertex's falloff is by its distance
// from the dab centre in cells.

// Raise / lower: offset per dab at the brush centre, per unit of brush radius at full strength.
inline constexpr float raise_rate_per_radius = 0.05f;
// Noise: peak displacement per unit of brush radius at full strength.
inline constexpr float noise_amplitude_per_radius = 0.25f;

inline float raise_step(float radius_world, float strength)
{
    return std::clamp(strength, 0.0f, 1.0f) * radius_world * raise_rate_per_radius;
}

inline float noise_amplitude(float radius_world, float strength)
{
    return std::clamp(strength, 0.0f, 1.0f) * radius_world * noise_amplitude_per_radius;
}

// The noise pattern's feature size, fixed for a stroke from its brush radius (both in cells).
inline float noise_feature_cells(float radius_cells)
{
    return std::max(radius_cells * 0.5f, 1.0f);
}

inline constexpr float max_ramp_angle = 80.0f;
// Bridge points: a shorter drag does nothing.
inline constexpr float min_segment_cells = 0.25f;

// Ramp: the height change per cell along x and z of a slope of `angle_deg` rising along the unit
// vector (dir_x, dir_z).
inline void ramp_slope(float angle_deg, float dir_x, float dir_z, float cell_size, float& slope_x, float& slope_z)
{
    const float a = std::clamp(angle_deg, -max_ramp_angle, max_ramp_angle) * (3.14159265f / 180.0f);
    const float rise = std::tan(a) * cell_size;
    slope_x = rise * dir_x;
    slope_z = rise * dir_z;
}

// Ramp: a stroke's direction locks once the cursor is this many cells from where it started. Capped,
// so a brush wider than the terrain still locks.
inline float ramp_lock_cells(float radius_cells)
{
    return std::clamp(radius_cells * 0.5f, 1.0f, 8.0f);
}

// Where (x, z) projects onto the segment from (ax, az) along (sx, sz), clamped to [0, 1], and in
// `dist` its distance from the nearest point of the segment; all in cells.
inline float segment_t(float ax, float az, float sx, float sz, float x, float z, float& dist)
{
    const float px = x - ax, pz = z - az;
    const float len2 = sx * sx + sz * sz;
    const float t = len2 > 0.0f ? std::clamp((px * sx + pz * sz) / len2, 0.0f, 1.0f) : 0.0f;
    const float dx = px - sx * t, dz = pz - sz * t;
    dist = std::sqrt(dx * dx + dz * dz);
    return t;
}

// Half-width in vertices of the smooth tool's box average.
inline std::uint32_t smooth_kernel(float radius_cells)
{
    return static_cast<std::uint32_t>(std::clamp(std::lround(radius_cells / 8.0f), 1L, 3L));
}

struct HeightGrid
{
    std::uint16_t* heights;
    std::uint32_t nx, nz;
    float height_min, height_range;
    double headroom_floor = -INFINITY; // at::growth_headroom_floor
};

inline double grid_offset(const HeightGrid& g, std::size_t i)
{
    return at::height_offset_exact(g.heights[i], g.height_min, g.height_range);
}

// A vertex's offset: the stroke's exact one once it has one (NaN until then), else the stored height.
inline double sculpt_offset(const HeightGrid& g, const double* exact, std::size_t i)
{
    return exact && !std::isnan(exact[i]) ? exact[i] : grid_offset(g, i);
}

// Vertices inside [x0, x1] x [z0, z1] (in cells).
inline Span vertex_span(float x0, float z0, float x1, float z1)
{
    Span r;
    if (!std::isfinite(x0) || !std::isfinite(z0) || !std::isfinite(x1) || !std::isfinite(z1)) return r;
    signed_span(x0, x1, r.x0, r.x1);
    signed_span(z0, z1, r.z0, r.z1);
    return r;
}

inline float vertex_falloff(const Dab& d, float i, float j)
{
    const float x = i - d.cx, z = j - d.cz;
    return falloff_weight(d.falloff, std::sqrt(x * x + z * z) / std::max(d.radius, 1e-6f));
}

// Lattice value in [-1, 1).
inline float lattice_noise(std::int64_t x, std::int64_t z, std::uint32_t seed)
{
    const std::uint64_t key = static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) |
                              static_cast<std::uint64_t>(static_cast<std::uint32_t>(z)) << 32;
    const std::uint64_t h = at::splitmix64(at::splitmix64(key) ^ seed);
    return static_cast<float>(h >> 40) / static_cast<float>(1u << 23) - 1.0f;
}

inline float value_noise(float x, float z, std::uint32_t seed)
{
    const float fx = std::floor(x), fz = std::floor(z);
    const std::int64_t ix = static_cast<std::int64_t>(fx), iz = static_cast<std::int64_t>(fz);
    auto fade = [](float t) { return t * t * (3.0f - 2.0f * t); };
    const float u = fade(x - fx), v = fade(z - fz);
    const float a = lattice_noise(ix, iz, seed), b = lattice_noise(ix + 1, iz, seed);
    const float c = lattice_noise(ix, iz + 1, seed), e = lattice_noise(ix + 1, iz + 1, seed);
    const float top = a + (b - a) * u, bottom = c + (e - c) * u;
    return top + (bottom - top) * v;
}

// Three octaves of value noise at a vertex, in [-1, 1].
inline float sculpt_noise(float i, float j, float feature_cells, std::uint32_t seed)
{
    const float x = i / feature_cells, z = j / feature_cells;
    float sum = 0.0f, amp = 1.0f, freq = 1.0f;
    for (std::uint32_t o = 0; o < 3; o++) {
        sum += amp * value_noise(x * freq, z * freq, seed + o * 0x9E3779B9u);
        amp *= 0.5f;
        freq *= 2.0f;
    }
    return std::clamp(sum / 1.75f, -1.0f, 1.0f);
}

struct HeightTarget
{
    std::uint32_t index;
    double offset;
    std::uint8_t image = 0; // the mirror image it was reached under
};

struct HeightWrite
{
    Rect verts;              // vertices whose stored height changed
    Rect image_verts[max_images]; // the same by target image, leaving out a re-quantization
    bool requantized = false; // the mapping grew: every height was re-encoded
    bool clamped = false;     // an offset did not fit the mapping limits
};

// Writes offsets to their vertices. When one falls outside the mapping by more than half a step, the
// mapping grows (at::grow_height_mapping) and every height is re-quantized to it first, from its
// exact offset; with `exact`, a vertex without one takes its old height as one, so later growths in
// the stroke do not round it again.
inline HeightWrite write_heights(HeightGrid& g, const std::vector<HeightTarget>& targets, double* exact = nullptr)
{
    HeightWrite out;
    if (targets.empty()) return out;
    double lo = INFINITY, hi = -INFINITY;
    for (const HeightTarget& t : targets) {
        lo = std::min(lo, t.offset);
        hi = std::max(hi, t.offset);
    }
    const double half = at::height_step(g.height_range) * 0.5;
    if (lo < g.height_min - half || hi > static_cast<double>(g.height_min) + g.height_range + half) {
        const at::HeightMapping m = at::grow_height_mapping(g.height_min, g.height_range, static_cast<float>(lo),
                                                            static_cast<float>(hi), g.headroom_floor);
        out.clamped = m.clamped;
        if (m.height_min != g.height_min || m.height_range != g.height_range) {
            const std::size_t count = at::vertex_count(g.nx, g.nz);
            for (std::size_t i = 0; i < count; i++) {
                const double off = sculpt_offset(g, exact, i);
                if (exact) exact[i] = off;
                g.heights[i] = at::encode_height(off, m.height_min, m.height_range);
            }
            g.height_min = m.height_min;
            g.height_range = m.height_range;
            out.requantized = true;
            out.verts = {0, 0, g.nx, g.nz};
        }
    }
    for (const HeightTarget& t : targets) {
        const std::uint16_t v = at::encode_height(t.offset, g.height_min, g.height_range);
        if (g.heights[t.index] == v) continue;
        g.heights[t.index] = v;
        const std::uint32_t x = t.index % g.nx, z = t.index / g.nx;
        out.verts = rect_union(out.verts, {x, z, x + 1, z + 1});
        Rect& image = out.image_verts[t.image < max_images ? t.image : 0];
        image = rect_union(image, {x, z, x + 1, z + 1});
    }
    return out;
}

struct SculptDab
{
    Tool tool = Tool::raise;
    Dab dab;              // centre and radius in cells; strength is the blend for every tool but raise, lower and noise
    float amount = 0.0f;  // raise / lower: raise_step; noise: noise_amplitude
    // Flatten, set height and ramp: heights blend toward the plane through offset `target` at
    // (plane_x, plane_z) cells, rising by slope_x / slope_z per cell (ramp_slope; zero for the others).
    float target = 0.0f;
    float plane_x = 0.0f, plane_z = 0.0f;
    float slope_x = 0.0f, slope_z = 0.0f;
    // Bridge points: the segment from the dab centre to (seg_x, seg_z) cells past it, along
    // which the target runs from `target` to `target_end`. The falloff is by distance from it.
    float seg_x = 0.0f, seg_z = 0.0f;
    float target_end = 0.0f;
    // Noise: the heights and mapping at the stroke's start, and per vertex the largest falloff the
    // stroke has reached (nx * nz, zero at its start; one such plane per image when mirrored). A vertex
    // sits at base + amount * noise * coverage, summed over the images.
    const std::uint16_t* base = nullptr;
    float base_min = 0.0f, base_range = 1.0f;
    float* coverage = nullptr;
    std::uint32_t seed = 0;
    float feature_cells = 1.0f;
    // Per vertex (nx * nz) the offset the stroke has built up, NaN where it has not touched: dabs
    // accumulate here and the stored heights are encoded from it, so moves under a storage step add up.
    double* exact = nullptr;
    // Mirrored flatten, ramp and bridge points: image k > 0's own `target` and `target_end`.
    const Mirror* mirror = nullptr;
    float image_target[max_images] = {};
    float image_target_end[max_images] = {};
};

inline double plane_target(const SculptDab& s, float target, double i, double j)
{
    return static_cast<double>(target) + static_cast<double>(s.slope_x) * (i - s.plane_x) +
           static_cast<double>(s.slope_z) * (j - s.plane_z);
}

inline bool segment_long_enough(const SculptDab& s)
{
    return std::sqrt(s.seg_x * s.seg_x + s.seg_z * s.seg_z) >= min_segment_cells;
}

// Bridge points: a vertex's falloff by its distance from the segment, and the target where it
// projects onto it.
inline float segment_falloff(const SculptDab& s, float from, float to, float i, float j, double& target)
{
    float dist = 0.0f;
    const float t = segment_t(s.dab.cx, s.dab.cz, s.seg_x, s.seg_z, i, j, dist);
    target = static_cast<double>(from) + (static_cast<double>(to) - from) * t;
    return falloff_weight(s.dab.falloff, dist / std::max(s.dab.radius, 1e-6f));
}

// The vertices a dab can reach: its circle, or for a Bridge Points stroke the segment's capsule.
inline Span sculpt_vertex_span(const SculptDab& s)
{
    const Dab& d = s.dab;
    if (s.tool != Tool::ramp_between) {
        if (!std::isfinite(d.cx) || !std::isfinite(d.cz) || !std::isfinite(d.radius)) return {};
        return vertex_span(d.cx - d.radius, d.cz - d.radius, d.cx + d.radius, d.cz + d.radius);
    }
    if (!segment_long_enough(s) || !std::isfinite(d.radius)) return {};
    const float bx = d.cx + s.seg_x, bz = d.cz + s.seg_z, r = d.radius;
    return vertex_span(std::min(d.cx, bx) - r, std::min(d.cz, bz) - r, std::max(d.cx, bx) + r,
                       std::max(d.cz, bz) + r);
}

struct SculptScratch
{
    std::vector<HeightTarget> targets;
    Snapshots<double> src;
};

inline HeightWrite sculpt_dab(HeightGrid& g, const SculptDab& s, SculptScratch& scratch)
{
    scratch.targets.clear();
    if (!tool_edits_heights(s.tool)) return {};
    const ImageSet set = image_set(s.mirror, {1, false}, sculpt_vertex_span(s), g.nx, g.nz);
    if (image_set_empty(set)) return {};
    const float strength = std::clamp(s.dab.strength, 0.0f, 1.0f);
    const std::size_t count = at::vertex_count(g.nx, g.nz);

    // Smooth reads the heights as they were before this dab.
    std::uint32_t k = 0;
    if (s.tool == Tool::smooth_heights) {
        k = smooth_kernel(s.dab.radius);
        scratch.src.take(set, k, g.nx, g.nz, 1, [&](std::uint32_t i, std::uint32_t j, double* out) {
            *out = sculpt_offset(g, s.exact, static_cast<std::size_t>(j) * g.nx + i);
        });
    }
    // Mirrored, the box is summed in pairs either side of the vertex so a mirrored box gives the same bits.
    auto box_average = [&](int image, std::uint32_t i, std::uint32_t j) {
        const Rect& b = scratch.src.box[image];
        const std::uint32_t z0 = j > b.z0 + k ? j - k : b.z0, z1 = std::min(j + k, b.z1 - 1);
        const std::uint32_t x0 = i > b.x0 + k ? i - k : b.x0, x1 = std::min(i + k, b.x1 - 1);
        auto at_ = [&](std::uint32_t x, std::uint32_t z) { return *scratch.src.at(image, x, z, 1); };
        double sum = 0.0;
        std::uint32_t n = 0;
        if (!set.mirror) {
            for (std::uint32_t z = z0; z <= z1; z++) {
                for (std::uint32_t x = x0; x <= x1; x++) {
                    sum += at_(x, z);
                    n++;
                }
            }
            return sum / n;
        }
        auto row = [&](std::uint32_t z) {
            double r = at_(i, z);
            for (std::uint32_t d = 1; d <= k; d++) {
                r += (i >= x0 + d ? at_(i - d, z) : 0.0) + (i + d <= x1 ? at_(i + d, z) : 0.0);
            }
            return r;
        };
        sum = row(j);
        for (std::uint32_t d = 1; d <= k; d++) {
            sum += (j >= z0 + d ? row(j - d) : 0.0) + (j + d <= z1 ? row(j + d) : 0.0);
        }
        n = (x1 - x0 + 1) * (z1 - z0 + 1);
        return sum / n;
    };
    auto image_target = [&](int e) { return e == 0 ? s.target : s.image_target[e]; };
    auto image_target_end = [&](int e) { return e == 0 ? s.target_end : s.image_target_end[e]; };

    visit_images(set, nullptr, [&](std::uint32_t i, std::uint32_t j, int image, const Fold(&f)[max_images]) {
        Contrib c[max_images];
        const int n = gather_contribs(set, f, c, [&](const Fold& p, int e, double& target) {
            const float fi = static_cast<float>(p.i), fj = static_cast<float>(p.j);
            switch (s.tool) {
            case Tool::ramp_between: return segment_falloff(s, image_target(e), image_target_end(e), fi, fj, target);
            case Tool::flatten:
            case Tool::ramp:
                target = plane_target(s, image_target(e), static_cast<double>(p.i), static_cast<double>(p.j));
                break;
            case Tool::set_height:
                target = plane_target(s, s.target, static_cast<double>(p.i), static_cast<double>(p.j));
                break;
            default: break;
            }
            return vertex_falloff(s.dab, fi, fj);
        });
        const std::size_t idx = static_cast<std::size_t>(j) * g.nx + i;
        const double cur = sculpt_offset(g, s.exact, idx);
        double next = cur;
        if (s.tool == Tool::noise) {
            if (!s.base || !s.coverage) return false;
            bool raised = false;
            for (int q = 0; q < n; q++) {
                float& cov = s.coverage[c[q].image * count + idx];
                if (!(c[q].w > 0.0f) || !(c[q].w > cov)) continue;
                cov = c[q].w;
                raised = true;
            }
            if (!raised) return false;
            Contrib parts[max_images];
            int m = 0;
            for (int e = 0; e < max_images; e++) {
                if (!image_on(set.mirror, e) || !(s.coverage[e * count + idx] > 0.0f)) continue;
                parts[m++] = {s.coverage[e * count + idx], 0.0, f[e].i, f[e].j, e};
            }
            order_contribs(parts, m);
            next = at::height_offset_exact(s.base[idx], s.base_min, s.base_range);
            for (int q = 0; q < m; q++) {
                next += static_cast<double>(s.amount) *
                        sculpt_noise(static_cast<float>(parts[q].i), static_cast<float>(parts[q].j), s.feature_cells,
                                     s.seed) *
                        parts[q].w;
            }
        }
        else {
            const double avg = s.tool == Tool::smooth_heights ? box_average(image, i, j) : 0.0;
            for (int q = 0; q < n; q++) {
                const float w = c[q].w;
                if (!(w > 0.0f)) continue;
                switch (s.tool) {
                case Tool::raise: next = next + static_cast<double>(s.amount) * w; break;
                case Tool::lower: next = next - static_cast<double>(s.amount) * w; break;
                case Tool::smooth_heights: next = next + (avg - next) * strength * w; break;
                default: next = next + (c[q].target - next) * strength * w; break;
                }
            }
        }
        if (!std::isfinite(next) || next == cur) return false;
        scratch.targets.push_back({static_cast<std::uint32_t>(idx), next, static_cast<std::uint8_t>(image)});
        if (s.exact) s.exact[idx] = next;
        return true;
    });
    const HeightWrite w = write_heights(g, scratch.targets, s.exact);
    if (s.exact) {
        // Past the mapping's limits an offset stops within half a step of them, so pushing on does not
        // bank height that coming back would first have to undo.
        const double half = at::height_step(g.height_range) * 0.5;
        const double lo = g.height_min - half, hi = static_cast<double>(g.height_min) + g.height_range + half;
        for (const HeightTarget& t : scratch.targets) s.exact[t.index] = std::clamp(s.exact[t.index], lo, hi);
    }
    return w;
}

// Mirrored sculpting keeps cell splits mirrored: each cell next to `verts` off the dab's side takes the
// diagonal of its mirror on that side (or the nearest in-grid one), flipped once per line between them.
// A line through a cell's centre leaves that axis out. Returns the cells changed.
inline Rect mirror_diagonals(std::uint8_t* diag, std::uint32_t cells_x, std::uint32_t cells_z, const Mirror& m,
                             float cx, float cz, const Rect& verts)
{
    if (!mirror_on(&m) || verts.empty() || cells_x == 0 || cells_z == 0) return {};
    const std::uint32_t x0 = verts.x0 > 0 ? verts.x0 - 1 : 0, z0 = verts.z0 > 0 ? verts.z0 - 1 : 0;
    const std::uint32_t x1 = std::min(verts.x1, cells_x), z1 = std::min(verts.z1, cells_z);
    // The side of each line the dab is on: +1 above it, -1 below or on it.
    const int side_x = 2.0f * cx > static_cast<float>(m.x2) ? 1 : -1;
    const int side_z = 2.0f * cz > static_cast<float>(m.z2) ? 1 : -1;
    auto side = [](std::int64_t cell, std::int32_t line2) {
        const std::int64_t d = 2 * cell + 1 - line2;
        return d > 0 ? 1 : d < 0 ? -1 : 0;
    };
    Rect changed;
    for (std::uint32_t z = z0; z < z1; z++) {
        for (std::uint32_t x = x0; x < x1; x++) {
            const int sx = side(x, m.x2), sz = side(z, m.z2);
            const bool on_x = m.x && sx != 0, on_z = m.z && sz != 0;
            const bool dx = on_x && sx != side_x, dz = on_z && sz != side_z;
            if (!dx && !dz) continue;
            const std::int64_t ax = dx ? static_cast<std::int64_t>(m.x2) - 1 - x : x;
            const std::int64_t az = dz ? static_cast<std::int64_t>(m.z2) - 1 - z : z;
            for (int u = 0; u < max_images; u++) {
                if (((u & 1) && !on_x) || ((u & 2) && !on_z)) continue;
                const std::int64_t px = (u & 1) ? static_cast<std::int64_t>(m.x2) - 1 - ax : ax;
                const std::int64_t pz = (u & 2) ? static_cast<std::int64_t>(m.z2) - 1 - az : az;
                if (px < 0 || pz < 0 || px >= cells_x || pz >= cells_z) continue;
                const int flips = (((u & 1) != 0) != dx ? 1 : 0) + (((u & 2) != 0) != dz ? 1 : 0);
                if (flips == 0) break;
                const bool want = at::get_cell_bit(diag, cells_x, static_cast<std::uint32_t>(px),
                                                   static_cast<std::uint32_t>(pz)) != (flips == 1);
                if (at::get_cell_bit(diag, cells_x, x, z) != want) {
                    at::set_cell_bit(diag, cells_x, x, z, want);
                    changed = rect_union(changed, {x, z, x + 1, z + 1});
                }
                break;
            }
        }
    }
    return changed;
}

// at::height_at at image k's mirror of world (x, z), worked in (x, z)'s frame (exactly height_at(x, z) on a
// mirrored terrain); mirrored samples off the grid clamp to it.
inline float mirrored_height_at(const at::GridView& g, const Mirror& m, int k, float world_x, float world_z)
{
    const std::uint32_t cx = at::cells(g.nx), cz = at::cells(g.nz);
    float fx = std::clamp((world_x - g.origin[0]) / g.cell_size, 0.0f, static_cast<float>(cx));
    float fz = std::clamp((world_z - g.origin[2]) / g.cell_size, 0.0f, static_cast<float>(cz));
    if (std::isnan(fx) || std::isnan(fz)) return NAN;
    const float ulps = at::coord_ulps / g.cell_size;
    const float tol_x = std::clamp((std::fabs(world_x) + std::fabs(g.origin[0])) * ulps, 1e-4f, 0.1f);
    const float tol_z = std::clamp((std::fabs(world_z) + std::fabs(g.origin[2])) * ulps, 1e-4f, 0.1f);
    if (std::fabs(fx - std::round(fx)) < tol_x) fx = std::round(fx);
    if (std::fabs(fz - std::round(fz)) < tol_z) fz = std::round(fz);
    const std::uint32_t x = std::min(static_cast<std::uint32_t>(fx), cx - 1);
    const std::uint32_t z = std::min(static_cast<std::uint32_t>(fz), cz - 1);
    const float u = fx - static_cast<float>(x), v = fz - static_cast<float>(z);
    auto fold = [](bool flip, std::int64_t line, std::int64_t i, std::uint32_t n) {
        return static_cast<std::uint32_t>(std::clamp<std::int64_t>(flip ? line - i : i, 0, n - 1));
    };
    auto y = [&](std::uint32_t a, std::uint32_t b) {
        return at::top_y(g, fold(k & 1, m.x2, a, g.nx), fold(k & 2, m.z2, b, g.nz));
    };
    const float h00 = y(x, z), h10 = y(x + 1, z), h11 = y(x + 1, z + 1), h01 = y(x, z + 1);
    const bool diag = at::get_cell_bit(g.diag, cx, fold(k & 1, m.x2 - 1, x, cx), fold(k & 2, m.z2 - 1, z, cz)) !=
                      (k == 1 || k == 2);
    if (!diag) {
        return u >= v ? h00 + u * (h10 - h00) + v * (h11 - h10) : h00 + v * (h01 - h00) + u * (h11 - h01);
    }
    return u + v <= 1.0f ? h00 + u * (h10 - h00) + v * (h01 - h00)
                         : h11 + (1.0f - u) * (h01 - h11) + (1.0f - v) * (h10 - h11);
}

// ─── Undo diffs ───────────────────────────────────────────────────────────────
// A stroke's before and after state inside the rectangles it touched: 8 weight bytes per texel
// (map 0 channels then map 1 channels), then the texel's 4 overlay coverage bytes when the terrain has
// overlays and a byte per decoration plane when it has decorations; one byte per hole cell, the heights
// of the vertices and one byte per cell diagonal. A stroke that grew the height mapping covers every vertex
// and records both mappings. A mirrored stroke is kept as a few such parts, one per area it reached.

// The captured bytes per texel.
inline std::size_t weight_diff_stride(bool overlay, std::uint32_t deco_planes)
{
    return 8 + (overlay ? 4 : 0) + deco_planes;
}

// `planes` holds `deco_planes` decoration planes of w x h bytes.
inline void capture_weights(const std::uint8_t* weights, std::uint32_t w, std::uint32_t h, const Rect& r,
                            std::vector<std::uint8_t>& out, const std::uint8_t* overlay = nullptr,
                            const std::uint8_t* planes = nullptr, std::uint32_t deco_planes = 0)
{
    const std::size_t plane = static_cast<std::size_t>(w) * h, map = plane * 4;
    if (!planes) deco_planes = 0;
    const std::size_t first_plane = weight_diff_stride(overlay != nullptr, 0);
    const std::size_t stride = weight_diff_stride(overlay != nullptr, deco_planes);
    out.resize(r.area() * stride);
    std::size_t k = 0;
    for (std::uint32_t j = r.z0; j < r.z1; j++) {
        for (std::uint32_t i = r.x0; i < r.x1; i++) {
            const std::size_t texel = static_cast<std::size_t>(j) * w + i, t = texel * 4;
            std::memcpy(&out[k], weights + t, 4);
            std::memcpy(&out[k + 4], weights + map + t, 4);
            if (overlay) std::memcpy(&out[k + 8], overlay + t, 4);
            for (std::uint32_t p = 0; p < deco_planes; p++) out[k + first_plane + p] = planes[p * plane + texel];
            k += stride;
        }
    }
}

// Restores what capture_weights took with the same overlay presence and plane count; anything else is left alone.
inline void restore_weights(std::uint8_t* weights, std::uint32_t w, std::uint32_t h, const Rect& r,
                            const std::vector<std::uint8_t>& in, std::uint8_t* overlay = nullptr,
                            std::uint8_t* planes = nullptr, std::uint32_t deco_planes = 0)
{
    const std::size_t plane = static_cast<std::size_t>(w) * h, map = plane * 4;
    if (!planes) deco_planes = 0;
    const std::size_t first_plane = weight_diff_stride(overlay != nullptr, 0);
    const std::size_t stride = weight_diff_stride(overlay != nullptr, deco_planes);
    if (in.size() != r.area() * stride) return;
    std::size_t k = 0;
    for (std::uint32_t j = r.z0; j < r.z1; j++) {
        for (std::uint32_t i = r.x0; i < r.x1; i++) {
            const std::size_t texel = static_cast<std::size_t>(j) * w + i, t = texel * 4;
            std::memcpy(weights + t, &in[k], 4);
            std::memcpy(weights + map + t, &in[k + 4], 4);
            if (overlay) std::memcpy(overlay + t, &in[k + 8], 4);
            for (std::uint32_t p = 0; p < deco_planes; p++) planes[p * plane + texel] = in[k + first_plane + p];
            k += stride;
        }
    }
}

inline void capture_holes(const std::uint8_t* holes, std::uint32_t cells_x, const Rect& r,
                          std::vector<std::uint8_t>& out)
{
    out.resize(r.area());
    std::size_t k = 0;
    for (std::uint32_t z = r.z0; z < r.z1; z++) {
        for (std::uint32_t x = r.x0; x < r.x1; x++) out[k++] = at::get_cell_bit(holes, cells_x, x, z) ? 1 : 0;
    }
}

inline void restore_holes(std::uint8_t* holes, std::uint32_t cells_x, const Rect& r,
                          const std::vector<std::uint8_t>& in)
{
    std::size_t k = 0;
    for (std::uint32_t z = r.z0; z < r.z1; z++) {
        for (std::uint32_t x = r.x0; x < r.x1; x++) at::set_cell_bit(holes, cells_x, x, z, in[k++] != 0);
    }
}

inline void capture_heights(const std::uint16_t* heights, std::uint32_t nx, const Rect& r,
                            std::vector<std::uint16_t>& out)
{
    out.resize(r.area());
    std::size_t k = 0;
    for (std::uint32_t z = r.z0; z < r.z1; z++) {
        for (std::uint32_t x = r.x0; x < r.x1; x++) out[k++] = heights[static_cast<std::size_t>(z) * nx + x];
    }
}

inline void restore_heights(std::uint16_t* heights, std::uint32_t nx, const Rect& r,
                            const std::vector<std::uint16_t>& in)
{
    std::size_t k = 0;
    for (std::uint32_t z = r.z0; z < r.z1; z++) {
        for (std::uint32_t x = r.x0; x < r.x1; x++) heights[static_cast<std::size_t>(z) * nx + x] = in[k++];
    }
}

struct StrokeDiff
{
    Rect texels;
    std::vector<std::uint8_t> weights_before, weights_after;
    // What the weight bytes carry after the weights (capture_weights)
    bool overlay = false;
    std::uint32_t deco_planes = 0;
    Rect cells;
    std::vector<std::uint8_t> holes_before, holes_after;
    Rect verts;
    std::vector<std::uint16_t> heights_before, heights_after;
    float height_min_before = 0.0f, height_range_before = 0.0f;
    float height_min_after = 0.0f, height_range_after = 0.0f;
    // A geoable terrain's thickness follows a downward growth (at::thickness_after_growth).
    float thickness_before = 0.0f, thickness_after = 0.0f;
    Rect diag_cells;
    std::vector<std::uint8_t> diag_before, diag_after;

    bool mapping_changed() const
    {
        return height_min_before != height_min_after || height_range_before != height_range_after ||
               thickness_before != thickness_after;
    }

    std::size_t bytes() const
    {
        return sizeof(*this) + weights_before.capacity() + weights_after.capacity() + holes_before.capacity() +
               holes_after.capacity() + (heights_before.capacity() + heights_after.capacity()) * sizeof(std::uint16_t) +
               diag_before.capacity() + diag_after.capacity();
    }
};

// Applies one side of a diff to a grid's weight maps, overlay coverage, decoration planes, hole mask and
// diagonals.
inline void apply_diff(const StrokeDiff& d, bool after, std::uint8_t* weights, std::uint32_t w, std::uint32_t h,
                       std::uint8_t* holes, std::uint32_t cells_x, std::uint8_t* overlay = nullptr,
                       std::uint8_t* planes = nullptr, std::uint8_t* diag = nullptr)
{
    if (!d.texels.empty()) {
        restore_weights(weights, w, h, d.texels, after ? d.weights_after : d.weights_before,
                        d.overlay ? overlay : nullptr, planes, d.deco_planes);
    }
    if (!d.cells.empty()) restore_holes(holes, cells_x, d.cells, after ? d.holes_after : d.holes_before);
    if (diag && !d.diag_cells.empty()) {
        restore_holes(diag, cells_x, d.diag_cells, after ? d.diag_after : d.diag_before);
    }
}

// Applies one side of a diff's heights and, when the stroke grew it, the height mapping and thickness.
inline void apply_height_diff(const StrokeDiff& d, bool after, std::uint16_t* heights, std::uint32_t nx,
                              float& height_min, float& height_range, float& thickness)
{
    if (!d.verts.empty()) restore_heights(heights, nx, d.verts, after ? d.heights_after : d.heights_before);
    if (d.mapping_changed()) {
        height_min = after ? d.height_min_after : d.height_min_before;
        height_range = after ? d.height_range_after : d.height_range_before;
        thickness = after ? d.thickness_after : d.thickness_before;
    }
}

} // namespace terrain_paint
