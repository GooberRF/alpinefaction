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

// Samples of `count` along one axis whose centres, at (i + 0.5) / per_cell cells, lie within the dab.
inline void dab_span(float c, float r, std::uint32_t per_cell, std::uint32_t count, std::uint32_t& lo,
                     std::uint32_t& hi)
{
    const float a = (c - r) * static_cast<float>(per_cell) - 0.5f;
    const float b = (c + r) * static_cast<float>(per_cell) - 0.5f;
    lo = static_cast<std::uint32_t>(std::clamp(std::ceil(a), 0.0f, static_cast<float>(count)));
    hi = static_cast<std::uint32_t>(std::clamp(std::floor(b) + 1.0f, 0.0f, static_cast<float>(count)));
}

inline Rect dab_rect(const Dab& d, std::uint32_t per_cell, std::uint32_t w, std::uint32_t h)
{
    Rect r;
    dab_span(d.cx, d.radius, per_cell, w, r.x0, r.x1);
    dab_span(d.cz, d.radius, per_cell, h, r.z0, r.z1);
    return r;
}

inline float dab_amount(const Dab& d, std::uint32_t per_cell, std::uint32_t i, std::uint32_t j)
{
    const float x = (static_cast<float>(i) + 0.5f) / static_cast<float>(per_cell) - d.cx;
    const float z = (static_cast<float>(j) + 0.5f) / static_cast<float>(per_cell) - d.cz;
    const float t = std::sqrt(x * x + z * z) / std::max(d.radius, 1e-6f);
    return std::clamp(d.strength, 0.0f, 1.0f) * falloff_weight(d.falloff, t);
}

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
inline Rect paint_layer(WeightMaps& m, const Dab& d, std::uint32_t layer)
{
    if (layer >= at::max_layers || layer >= m.layer_count) return {};
    const Rect r = dab_rect(d, m.mul, m.w, m.h);
    Rect changed;
    for (std::uint32_t j = r.z0; j < r.z1; j++) {
        for (std::uint32_t i = r.x0; i < r.x1; i++) {
            const float a = dab_amount(d, m.mul, i, j);
            if (a < min_amount) continue;
            const std::size_t t = static_cast<std::size_t>(j) * m.w + i;
            std::uint8_t v[at::max_layers];
            m.get(t, v);
            std::uint8_t out[at::max_layers];
            unsigned rest = 0;
            for (std::uint32_t c = 0; c < at::max_layers; c++) {
                if (c == layer) continue;
                out[c] = static_cast<std::uint8_t>(std::floor(static_cast<float>(v[c]) * (1.0f - a)));
                rest += out[c];
            }
            out[layer] = static_cast<std::uint8_t>(255u - std::min(rest, 255u));
            at::normalize_weights(out);
            if (std::memcmp(out, v, sizeof(v)) == 0) continue;
            m.set(t, out);
            changed = rect_union(changed, {i, j, i + 1, j + 1});
        }
    }
    return changed;
}

// Blends each texel toward the 3x3 average of the texels around it (as they were before the dab),
// then renormalizes. Returns the texels changed.
inline Rect smooth_weights(WeightMaps& m, const Dab& d)
{
    const Rect r = dab_rect(d, m.mul, m.w, m.h);
    if (r.empty()) return {};
    const std::uint32_t sx0 = r.x0 > 0 ? r.x0 - 1 : 0, sz0 = r.z0 > 0 ? r.z0 - 1 : 0;
    const std::uint32_t sx1 = std::min(r.x1 + 1, m.w), sz1 = std::min(r.z1 + 1, m.h);
    const std::uint32_t sw = sx1 - sx0;
    std::vector<std::uint8_t> src(static_cast<std::size_t>(sw) * (sz1 - sz0) * at::max_layers);
    for (std::uint32_t j = sz0; j < sz1; j++) {
        for (std::uint32_t i = sx0; i < sx1; i++) {
            std::uint8_t v[at::max_layers];
            m.get(static_cast<std::size_t>(j) * m.w + i, v);
            std::memcpy(&src[(static_cast<std::size_t>(j - sz0) * sw + (i - sx0)) * at::max_layers], v, sizeof(v));
        }
    }
    Rect changed;
    for (std::uint32_t j = r.z0; j < r.z1; j++) {
        for (std::uint32_t i = r.x0; i < r.x1; i++) {
            const float a = dab_amount(d, m.mul, i, j);
            if (a < min_amount) continue;
            float avg[at::max_layers] = {};
            int n = 0;
            for (std::uint32_t z = (j > sz0 ? j - 1 : j); z <= std::min(j + 1, sz1 - 1); z++) {
                for (std::uint32_t x = (i > sx0 ? i - 1 : i); x <= std::min(i + 1, sx1 - 1); x++) {
                    const std::uint8_t* s = &src[(static_cast<std::size_t>(z - sz0) * sw + (x - sx0)) * at::max_layers];
                    for (std::uint32_t c = 0; c < at::max_layers; c++) avg[c] += s[c];
                    n++;
                }
            }
            const std::uint8_t* cur = &src[(static_cast<std::size_t>(j - sz0) * sw + (i - sx0)) * at::max_layers];
            std::uint8_t out[at::max_layers];
            for (std::uint32_t c = 0; c < at::max_layers; c++) {
                const float target = avg[c] / static_cast<float>(n);
                const float v = static_cast<float>(cur[c]) + (target - static_cast<float>(cur[c])) * a;
                out[c] = static_cast<std::uint8_t>(std::clamp(std::lround(v), 0L, 255L));
            }
            at::normalize_weights(out);
            if (std::memcmp(out, cur, sizeof(out)) == 0) continue;
            m.set(static_cast<std::size_t>(j) * m.w + i, out);
            changed = rect_union(changed, {i, j, i + 1, j + 1});
        }
    }
    return changed;
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
inline Rect paint_coverage(CoverageMap& m, const Dab& d, bool raise)
{
    if (m.channel >= m.stride) return {};
    const Rect r = dab_rect(d, m.mul, m.w, m.h);
    Rect changed;
    for (std::uint32_t j = r.z0; j < r.z1; j++) {
        for (std::uint32_t i = r.x0; i < r.x1; i++) {
            const float a = dab_amount(d, m.mul, i, j);
            if (a < min_amount) continue;
            std::uint8_t& v = m.data[(static_cast<std::size_t>(j) * m.w + i) * m.stride + m.channel];
            const float rest = raise ? 255.0f - v : static_cast<float>(v);
            const auto left = static_cast<std::uint8_t>(std::floor(rest * (1.0f - a)));
            const std::uint8_t out = raise ? static_cast<std::uint8_t>(255 - left) : left;
            if (out == v) continue;
            v = out;
            changed = rect_union(changed, {i, j, i + 1, j + 1});
        }
    }
    return changed;
}

// Blends each texel's coverage toward the 3x3 average around it (as it was before the dab), as
// smooth_weights does. Returns the texels changed.
inline Rect smooth_coverage(CoverageMap& m, const Dab& d)
{
    if (m.channel >= m.stride) return {};
    const Rect r = dab_rect(d, m.mul, m.w, m.h);
    if (r.empty()) return {};
    const std::uint32_t sx0 = r.x0 > 0 ? r.x0 - 1 : 0, sz0 = r.z0 > 0 ? r.z0 - 1 : 0;
    const std::uint32_t sx1 = std::min(r.x1 + 1, m.w), sz1 = std::min(r.z1 + 1, m.h);
    const std::uint32_t sw = sx1 - sx0;
    auto at_ = [&](std::uint32_t i, std::uint32_t j) -> std::uint8_t& {
        return m.data[(static_cast<std::size_t>(j) * m.w + i) * m.stride + m.channel];
    };
    std::vector<std::uint8_t> src(static_cast<std::size_t>(sw) * (sz1 - sz0));
    for (std::uint32_t j = sz0; j < sz1; j++) {
        for (std::uint32_t i = sx0; i < sx1; i++) src[static_cast<std::size_t>(j - sz0) * sw + (i - sx0)] = at_(i, j);
    }
    Rect changed;
    for (std::uint32_t j = r.z0; j < r.z1; j++) {
        for (std::uint32_t i = r.x0; i < r.x1; i++) {
            const float a = dab_amount(d, m.mul, i, j);
            if (a < min_amount) continue;
            float sum = 0.0f;
            int n = 0;
            for (std::uint32_t z = (j > sz0 ? j - 1 : j); z <= std::min(j + 1, sz1 - 1); z++) {
                for (std::uint32_t x = (i > sx0 ? i - 1 : i); x <= std::min(i + 1, sx1 - 1); x++) {
                    sum += src[static_cast<std::size_t>(z - sz0) * sw + (x - sx0)];
                    n++;
                }
            }
            const float cur = src[static_cast<std::size_t>(j - sz0) * sw + (i - sx0)];
            const float v = cur + (sum / static_cast<float>(n) - cur) * a;
            const auto out = static_cast<std::uint8_t>(std::clamp(std::lround(v), 0L, 255L));
            if (out == at_(i, j)) continue;
            at_(i, j) = out;
            changed = rect_union(changed, {i, j, i + 1, j + 1});
        }
    }
    return changed;
}

// Sets or clears the hole bit of every cell whose centre is inside the dab (strength and falloff do
// not apply to holes). Returns the cells changed.
inline Rect paint_holes(std::uint8_t* holes, std::uint32_t cells_x, std::uint32_t cells_z, const Dab& d, bool hole)
{
    const Rect r = dab_rect(d, 1, cells_x, cells_z);
    Rect changed;
    for (std::uint32_t z = r.z0; z < r.z1; z++) {
        for (std::uint32_t x = r.x0; x < r.x1; x++) {
            const float dx = static_cast<float>(x) + 0.5f - d.cx, dz = static_cast<float>(z) + 0.5f - d.cz;
            if (dx * dx + dz * dz >= d.radius * d.radius) continue;
            if (at::get_cell_bit(holes, cells_x, x, z) == hole) continue;
            at::set_cell_bit(holes, cells_x, x, z, hole);
            changed = rect_union(changed, {x, z, x + 1, z + 1});
        }
    }
    return changed;
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

// Vertices inside [x0, x1] x [z0, z1] (in cells), as a rect of vertex indices.
inline Rect vertex_box(float x0, float z0, float x1, float z1, std::uint32_t nx, std::uint32_t nz)
{
    if (!std::isfinite(x0) || !std::isfinite(z0) || !std::isfinite(x1) || !std::isfinite(z1)) return {};
    auto span = [](float a, float b, std::uint32_t n, std::uint32_t& lo, std::uint32_t& hi) {
        lo = static_cast<std::uint32_t>(std::clamp(std::ceil(a), 0.0f, static_cast<float>(n)));
        hi = static_cast<std::uint32_t>(std::clamp(std::floor(b) + 1.0f, 0.0f, static_cast<float>(n)));
    };
    Rect r;
    span(x0, x1, nx, r.x0, r.x1);
    span(z0, z1, nz, r.z0, r.z1);
    return r;
}

// Vertices within the dab's radius, as a rect of vertex indices.
inline Rect dab_vertex_rect(const Dab& d, std::uint32_t nx, std::uint32_t nz)
{
    if (!std::isfinite(d.cx) || !std::isfinite(d.cz) || !std::isfinite(d.radius)) return {};
    return vertex_box(d.cx - d.radius, d.cz - d.radius, d.cx + d.radius, d.cz + d.radius, nx, nz);
}

inline float vertex_falloff(const Dab& d, std::uint32_t i, std::uint32_t j)
{
    const float x = static_cast<float>(i) - d.cx, z = static_cast<float>(j) - d.cz;
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
inline float sculpt_noise(std::uint32_t i, std::uint32_t j, float feature_cells, std::uint32_t seed)
{
    const float x = static_cast<float>(i) / feature_cells, z = static_cast<float>(j) / feature_cells;
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
};

struct HeightWrite
{
    Rect verts;              // vertices whose stored height changed
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
    // stroke has reached (nx * nz, zero at its start). A vertex sits at base + amount * noise * coverage.
    const std::uint16_t* base = nullptr;
    float base_min = 0.0f, base_range = 1.0f;
    float* coverage = nullptr;
    std::uint32_t seed = 0;
    float feature_cells = 1.0f;
    // Per vertex (nx * nz) the offset the stroke has built up, NaN where it has not touched: dabs
    // accumulate here and the stored heights are encoded from it, so moves under a storage step add up.
    double* exact = nullptr;
};

inline double plane_target(const SculptDab& s, std::uint32_t i, std::uint32_t j)
{
    return static_cast<double>(s.target) + static_cast<double>(s.slope_x) * (static_cast<double>(i) - s.plane_x) +
           static_cast<double>(s.slope_z) * (static_cast<double>(j) - s.plane_z);
}

inline bool segment_long_enough(const SculptDab& s)
{
    return std::sqrt(s.seg_x * s.seg_x + s.seg_z * s.seg_z) >= min_segment_cells;
}

// Bridge points: a vertex's falloff by its distance from the segment, and the target where it
// projects onto it.
inline float segment_falloff(const SculptDab& s, std::uint32_t i, std::uint32_t j, double& target)
{
    float dist = 0.0f;
    const float t =
        segment_t(s.dab.cx, s.dab.cz, s.seg_x, s.seg_z, static_cast<float>(i), static_cast<float>(j), dist);
    target = static_cast<double>(s.target) + (static_cast<double>(s.target_end) - s.target) * t;
    return falloff_weight(s.dab.falloff, dist / std::max(s.dab.radius, 1e-6f));
}

// The vertices a dab can reach: its circle, or for a Bridge Points stroke the segment's capsule.
inline Rect sculpt_vertex_rect(const SculptDab& s, std::uint32_t nx, std::uint32_t nz)
{
    if (s.tool != Tool::ramp_between) return dab_vertex_rect(s.dab, nx, nz);
    if (!segment_long_enough(s) || !std::isfinite(s.dab.radius)) return {};
    const float bx = s.dab.cx + s.seg_x, bz = s.dab.cz + s.seg_z, r = s.dab.radius;
    return vertex_box(std::min(s.dab.cx, bx) - r, std::min(s.dab.cz, bz) - r, std::max(s.dab.cx, bx) + r,
                      std::max(s.dab.cz, bz) + r, nx, nz);
}

struct SculptScratch
{
    std::vector<HeightTarget> targets;
    std::vector<double> src;
};

inline HeightWrite sculpt_dab(HeightGrid& g, const SculptDab& s, SculptScratch& scratch)
{
    scratch.targets.clear();
    const Rect r = sculpt_vertex_rect(s, g.nx, g.nz);
    if (r.empty() || !tool_edits_heights(s.tool)) return {};
    const float strength = std::clamp(s.dab.strength, 0.0f, 1.0f);

    // Smooth reads the heights as they were before this dab.
    std::uint32_t k = 0, sx0 = 0, sz0 = 0, sx1 = 0, sz1 = 0;
    if (s.tool == Tool::smooth_heights) {
        k = smooth_kernel(s.dab.radius);
        sx0 = r.x0 > k ? r.x0 - k : 0;
        sz0 = r.z0 > k ? r.z0 - k : 0;
        sx1 = std::min(r.x1 + k, g.nx);
        sz1 = std::min(r.z1 + k, g.nz);
        scratch.src.resize(static_cast<std::size_t>(sx1 - sx0) * (sz1 - sz0));
        for (std::uint32_t j = sz0; j < sz1; j++) {
            for (std::uint32_t i = sx0; i < sx1; i++) {
                scratch.src[static_cast<std::size_t>(j - sz0) * (sx1 - sx0) + (i - sx0)] =
                    sculpt_offset(g, s.exact, static_cast<std::size_t>(j) * g.nx + i);
            }
        }
    }

    for (std::uint32_t j = r.z0; j < r.z1; j++) {
        for (std::uint32_t i = r.x0; i < r.x1; i++) {
            double seg_target = 0.0;
            const float w =
                s.tool == Tool::ramp_between ? segment_falloff(s, i, j, seg_target) : vertex_falloff(s.dab, i, j);
            if (!(w > 0.0f)) continue;
            const std::size_t idx = static_cast<std::size_t>(j) * g.nx + i;
            const double cur = sculpt_offset(g, s.exact, idx);
            double next = cur;
            switch (s.tool) {
            case Tool::raise: next = cur + static_cast<double>(s.amount) * w; break;
            case Tool::lower: next = cur - static_cast<double>(s.amount) * w; break;
            case Tool::smooth_heights: {
                double sum = 0.0;
                std::uint32_t n = 0;
                for (std::uint32_t z = (j > sz0 + k ? j - k : sz0); z <= std::min(j + k, sz1 - 1); z++) {
                    for (std::uint32_t x = (i > sx0 + k ? i - k : sx0); x <= std::min(i + k, sx1 - 1); x++) {
                        sum += scratch.src[static_cast<std::size_t>(z - sz0) * (sx1 - sx0) + (x - sx0)];
                        n++;
                    }
                }
                next = cur + (sum / n - cur) * strength * w;
                break;
            }
            case Tool::flatten:
            case Tool::set_height:
            case Tool::ramp: next = cur + (plane_target(s, i, j) - cur) * strength * w; break;
            case Tool::ramp_between: next = cur + (seg_target - cur) * strength * w; break;
            case Tool::noise: {
                if (!s.base || !s.coverage || !(w > s.coverage[idx])) continue;
                s.coverage[idx] = w;
                next = at::height_offset_exact(s.base[idx], s.base_min, s.base_range) +
                       static_cast<double>(s.amount) * sculpt_noise(i, j, s.feature_cells, s.seed) * w;
                break;
            }
            default: continue;
            }
            if (!std::isfinite(next) || next == cur) continue;
            scratch.targets.push_back({static_cast<std::uint32_t>(idx), next});
            if (s.exact) s.exact[idx] = next;
        }
    }
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

// ─── Undo diffs ───────────────────────────────────────────────────────────────
// A stroke's before and after state inside the rectangles it touched: 8 weight bytes per texel
// (map 0 channels then map 1 channels), then the texel's 4 overlay coverage bytes when the terrain has
// overlays and a byte per decoration plane when it has decorations; one byte per hole cell and the heights
// of the vertices. A stroke that grew the height mapping covers every vertex and records both mappings.

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

    bool mapping_changed() const
    {
        return height_min_before != height_min_after || height_range_before != height_range_after ||
               thickness_before != thickness_after;
    }

    std::size_t bytes() const
    {
        return sizeof(*this) + weights_before.capacity() + weights_after.capacity() + holes_before.capacity() +
               holes_after.capacity() + (heights_before.capacity() + heights_after.capacity()) * sizeof(std::uint16_t);
    }
};

// Applies one side of a diff to a grid's weight maps, overlay coverage, decoration planes and hole mask.
inline void apply_diff(const StrokeDiff& d, bool after, std::uint8_t* weights, std::uint32_t w, std::uint32_t h,
                       std::uint8_t* holes, std::uint32_t cells_x, std::uint8_t* overlay = nullptr,
                       std::uint8_t* planes = nullptr)
{
    if (!d.texels.empty()) {
        restore_weights(weights, w, h, d.texels, after ? d.weights_after : d.weights_before,
                        d.overlay ? overlay : nullptr, planes, d.deco_planes);
    }
    if (!d.cells.empty()) restore_holes(holes, cells_x, d.cells, after ? d.holes_after : d.holes_before);
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
