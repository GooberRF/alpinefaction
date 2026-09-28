#pragma once

// Alpine Terrain — RFL chunk 0x0AFBAE0B, shared by the RED writer/reader and the game reader.
// This header owns every size, index and range derivation of the terrain data model.
// The record parse both readers run is in alpine_terrain_reader.h.
//
// POD types and free functions only: nothing here may allocate, throw, or pass a non-trivial type by value.
//
// Wire layout, per terrain after a u32 count (no chunk version; growth appends fields gated on the RFL
// version; wire flags (flag_chunk_geo_mask, flag_overlays, flag_decorations) gate optional parts, and
// flag_fullbright adds none):
//   i32 uid, f32x3 origin, vstring script_name, f32 cell_size, u16 nx, u16 nz,
//   f32 height_min, f32 height_range, u8 chunk_cells (the edge a build uses), u8 weight_res_mul,
//   u8 lightmap_density, u8 flags, f32 thickness, f32 skirt_depth, vstring underside_texture,
//   vstring crater_texture, u8 layer_count, per layer {vstring texture, f32 uv_scale, u8 layer_flags},
//   [flag_overlays: u8 overlay_count, per overlay {vstring texture, f32 uv_scale, u8 overlay_flags}],
//   [flag_decorations: u8 decoration_count, per decoration {vstring mesh (.v3m/.vfx), f32 density, f32 scale_min,
//    f32 scale_max, f32 max_slope_deg, f32 draw_distance, f32 vertical_offset, u8 link_layer,
//    u8 decoration_flags}],
//   u32 mapping_count, per mapping {i32 room_uid, u32 vertex_count, u64 pos_hash},
//   u32 raw_size, u32 comp_size, comp_size bytes of zlib holding the blob described by blob_*().
//
// Grid conventions: vertex (x, z) sits at origin + (x * cell_size, height, z * cell_size) and is
// heights[z * nx + x]. Cells, weight texels and mask bits are row-major in z the same way.

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <bit>

namespace alpine_terrain
{

inline constexpr std::uint32_t chunk_id = 0x0AFBAE0Bu;

// ─── Limits ───────────────────────────────────────────────────────────────────

inline constexpr std::uint32_t min_verts = 2;
inline constexpr std::uint32_t max_verts = 257;
inline constexpr std::uint32_t max_layers = 8;
inline constexpr std::uint32_t max_overlays = 4;
inline constexpr std::uint32_t max_terrains = 64;
inline constexpr std::uint32_t max_chunks = 256;
inline constexpr std::uint32_t chunk_edge_options[] = {16, 32, 64, 128};
inline constexpr std::uint32_t weight_res_options[] = {1, 2, 4};
inline constexpr std::uint32_t max_script_name_len = 64;
// Stem and extension caps every RFL texture name obeys (editor rfl_name_max_len / rfl_ext_max_len).
inline constexpr std::size_t max_texture_name_len = 31;
inline constexpr std::size_t max_texture_ext_len = 14;

inline constexpr std::uint8_t lightmap_density_min = 1;
inline constexpr std::uint8_t lightmap_density_max = 8;
inline constexpr std::uint8_t lightmap_density_default = 2;

inline constexpr float max_coord = 65536.0f;
inline constexpr float min_cell_size = 0.125f;
inline constexpr float max_cell_size = 64.0f;
inline constexpr float min_height_range = 0.01f;
inline constexpr float max_height_range = 16384.0f;
inline constexpr float min_thickness = 0.5f;
inline constexpr float max_thickness = 4096.0f;
inline constexpr float max_skirt_depth = 4096.0f;
inline constexpr float min_uv_scale = 0.001f;
inline constexpr float max_uv_scale = 1000.0f;

inline constexpr std::uint32_t max_room_vertices = 32768;
// A chunk's worst case under the D3D8/9 room cache limits, with room left for craters and decals.
inline constexpr std::uint32_t legacy_chunk_vertex_budget = 6000;
inline constexpr std::uint32_t legacy_chunk_face_budget = 12288;
// RED warns past this many emitted terrain triangles per level.
inline constexpr std::uint32_t level_triangle_budget = 250000;

// ─── Flags ────────────────────────────────────────────────────────────────────
// Append-only: never reuse or renumber a value. Readers reject unknown bits.

inline constexpr std::uint8_t flag_geoable = 0x1;
inline constexpr std::uint8_t flag_skirts = 0x2;
// A terrain's own flags; the shape and every fingerprint use only these.
inline constexpr std::uint8_t flag_mask = flag_geoable | flag_skirts;
// Wire only, and only with flag_geoable: the blob ends with a chunk geo mask. The writer sets it when at
// least one chunk is not geoable; without it every chunk is.
inline constexpr std::uint8_t flag_chunk_geo_mask = 0x4;
// Wire only: the header lists overlays after the layers and the blob ends with their coverage map. The
// writer sets it when the terrain has at least one overlay.
inline constexpr std::uint8_t flag_overlays = 0x8;
// Wire only: the header lists decorations after the overlays and the blob ends with their coverage planes.
// The writer sets it when the terrain has at least one decoration.
inline constexpr std::uint8_t flag_decorations = 0x10;
// Drawn at full brightness, without ambient, sun or baked light; Calculate Lighting bakes no chart for it.
// Outside flag_mask:
// it changes neither the shape nor any fingerprint.
inline constexpr std::uint8_t flag_fullbright = 0x20;
inline constexpr std::uint8_t wire_flag_mask =
    flag_mask | flag_chunk_geo_mask | flag_overlays | flag_decorations | flag_fullbright;

inline constexpr std::uint8_t layer_flag_triplanar = 0x1;
inline constexpr std::uint8_t layer_flag_mask = layer_flag_triplanar;

// Overlays: textures with alpha composited over the layer blend in order, each by its own painted
// coverage (not normalized): albedo = lerp(albedo, overlay.rgb, overlay.a * coverage).
inline constexpr std::uint8_t overlay_flag_triplanar = layer_flag_triplanar;
// Hex-tiled with a random rotation and offset per tile, so the repeat does not show.
inline constexpr std::uint8_t overlay_flag_break_tiling = 0x2;
inline constexpr std::uint8_t overlay_flag_mask = overlay_flag_triplanar | overlay_flag_break_tiling;

// Decorations: a static mesh scattered over the surface by its painted coverage plane, optionally scaled by a
// texture layer's weight. Visual only.
inline constexpr std::uint32_t max_decorations = 8;
inline constexpr std::uint8_t decoration_link_none = 0xFF;
inline constexpr std::uint8_t decoration_coverage_full = 255;
inline constexpr std::uint8_t decoration_flag_align_to_slope = 0x1;
inline constexpr std::uint8_t decoration_flag_random_yaw = 0x2;
inline constexpr std::uint8_t decoration_flag_casts_shadows = 0x4;
inline constexpr std::uint8_t decoration_flag_mask =
    decoration_flag_align_to_slope | decoration_flag_random_yaw | decoration_flag_casts_shadows;
// Instances per m² at full coverage
inline constexpr float max_decoration_density = 16.0f;
inline constexpr float min_decoration_scale = 0.01f;
inline constexpr float max_decoration_scale = 16.0f;
inline constexpr float max_decoration_slope_deg = 90.0f;
inline constexpr float min_decoration_draw_distance = 1.0f;
inline constexpr float max_decoration_draw_distance = 1024.0f;
// Mesh units along the instance's up axis, so scaled with it
inline constexpr float max_decoration_offset = 16.0f;
inline constexpr std::uint32_t max_decoration_instances_per_texel = 256;
// The game places a level's instances in for_each_terrain_decoration order until it has placed this many or
// tried this many candidates, placed or dropped by the slope limit (DecorationBudget).
inline constexpr std::uint32_t max_level_decoration_instances = 250000;
inline constexpr std::uint32_t max_level_decoration_candidates = 4 * max_level_decoration_instances;

// Decompressed bytes summed over every 0x0AFBAE0B chunk; a chunk that would pass it is dropped whole.
inline constexpr std::uint64_t max_level_raw_bytes = 128ull * 1024 * 1024;

// ─── Defaults ─────────────────────────────────────────────────────────────────

inline constexpr std::uint32_t default_verts = 129;
inline constexpr float default_cell_size = 2.0f;
inline constexpr float default_height_range = 64.0f;
inline constexpr std::uint32_t default_chunk_cells = 32;
inline constexpr std::uint32_t default_geoable_chunk_cells = 16;
inline constexpr std::uint32_t default_weight_res_mul = 2;
inline constexpr float default_thickness = 16.0f;
inline constexpr float default_skirt_depth = 8.0f;
// World units per texture repeat (layer_uv)
inline constexpr float default_uv_scale = 4.0f;
// RED's own default face texture (RED.exe string at 0x005781C8)
inline constexpr const char* default_layer_texture = "Rck_default.tga";
inline constexpr float default_decoration_density = 0.5f;
inline constexpr float default_decoration_scale = 1.0f;
inline constexpr float default_decoration_slope_deg = 35.0f;
inline constexpr float default_decoration_draw_distance = 80.0f;

// ─── Parsed header ────────────────────────────────────────────────────────────

struct Header
{
    float origin[3];
    float cell_size;
    std::uint32_t nx;
    std::uint32_t nz;
    float height_min;
    float height_range;
    std::uint32_t chunk_cells;
    std::uint32_t weight_res_mul;
    std::uint32_t lightmap_density;
    std::uint32_t flags; // as on the wire, the wire-only flags included
    float thickness;
    float skirt_depth;
    std::uint32_t layer_count;
    std::uint32_t overlay_count; // 0 without flag_overlays
    std::uint32_t decoration_count; // 0 without flag_decorations
};

struct ChunkMapping
{
    std::int32_t room_uid;
    std::uint32_t vertex_count;
    std::uint64_t pos_hash;
};

// room_uid of a chunk that emits no faces (every cell a hole); its vertex_count and pos_hash are 0.
inline constexpr std::int32_t no_room_uid = -1;

// ─── Grid derivations ─────────────────────────────────────────────────────────

inline constexpr std::uint32_t cells(std::uint32_t verts)
{
    return verts > 0 ? verts - 1 : 0;
}

inline constexpr bool is_allowed_chunk_cells(std::uint32_t v)
{
    for (std::uint32_t e : chunk_edge_options) {
        if (e == v) return true;
    }
    return false;
}

inline constexpr bool is_allowed_weight_res_mul(std::uint32_t v)
{
    for (std::uint32_t m : weight_res_options) {
        if (m == v) return true;
    }
    return false;
}

inline constexpr std::uint32_t chunk_count(std::uint32_t cells_x, std::uint32_t cells_z,
                                           std::uint32_t edge)
{
    if (edge == 0) return 0;
    return ((cells_x + edge - 1) / edge) * ((cells_z + edge - 1) / edge);
}

// Upper bounds for an edge x edge chunk over every hole pattern (emit_chunk): the top grid, plus
// the lower grid once walls or bottoms exist; two triangles per cell, at most one wall per cell
// edge, and a bottom per cell when closed. Vertices are the distinct slots faces reference.
inline constexpr std::uint32_t chunk_max_vertices(std::uint32_t edge, std::uint32_t flags)
{
    const std::uint32_t grid = (edge + 1) * (edge + 1);
    return (flags & (flag_geoable | flag_skirts)) ? 2 * grid : grid;
}

inline constexpr std::uint32_t chunk_max_faces(std::uint32_t edge, std::uint32_t flags)
{
    const std::uint32_t top = 2 * edge * edge, walls = 2 * edge * (edge + 1);
    if (flags & flag_geoable) return top + walls + edge * edge;
    return (flags & flag_skirts) ? top + walls : top;
}

inline constexpr bool chunk_edge_fits(std::uint32_t edge, std::uint32_t flags)
{
    const std::uint32_t verts = chunk_max_vertices(edge, flags);
    return verts < max_room_vertices && verts <= legacy_chunk_vertex_budget &&
           chunk_max_faces(edge, flags) <= legacy_chunk_face_budget;
}

// The largest allowed edge whose every chunk fits RED's room and a legacy room cache: 64 for a bare
// heightfield, 32 with skirts or closed chunks.
inline constexpr std::uint32_t max_chunk_cells(std::uint32_t flags)
{
    std::uint32_t best = chunk_edge_options[0];
    for (std::uint32_t e : chunk_edge_options) {
        if (chunk_edge_fits(e, flags)) best = e;
    }
    return best;
}

static_assert(chunk_edge_fits(chunk_edge_options[0], flag_mask));
static_assert(chunk_count(cells(max_verts), cells(max_verts), chunk_edge_options[0]) <= max_chunks);

// The smallest allowed edge at or above `requested` that keeps the terrain within max_chunks, held
// at or below max_chunk_cells(flags). Always satisfiable (the asserts above): the smallest edge fits
// every flag combination and cuts the largest terrain into max_chunks chunks. RED's choice for a new
// build only: a record stores the edge it was built with, and readers never recompute it.
inline constexpr std::uint32_t effective_chunk_cells(std::uint32_t cells_x, std::uint32_t cells_z,
                                                     std::uint32_t requested, std::uint32_t flags)
{
    const std::uint32_t cap = max_chunk_cells(flags);
    const std::uint32_t want = requested < cap ? requested : cap;
    for (std::uint32_t e : chunk_edge_options) {
        if (e >= want && e <= cap && chunk_count(cells_x, cells_z, e) <= max_chunks) return e;
    }
    return cap;
}

inline constexpr std::uint32_t chunks_along(std::uint32_t cells_axis, std::uint32_t edge)
{
    return edge == 0 ? 0 : (cells_axis + edge - 1) / edge;
}

// Cells [x0, x1) x [z0, z1) of chunk `index`; chunks are numbered row-major, z then x.
struct ChunkRect
{
    std::uint32_t x0, z0, x1, z1;
};

inline constexpr ChunkRect chunk_rect(std::uint32_t cells_x, std::uint32_t cells_z, std::uint32_t edge,
                                      std::uint32_t index)
{
    const std::uint32_t across = chunks_along(cells_x, edge);
    const std::uint32_t x0 = (index % across) * edge;
    const std::uint32_t z0 = (index / across) * edge;
    return {x0, z0, std::min(x0 + edge, cells_x), std::min(z0 + edge, cells_z)};
}

inline constexpr std::uint32_t weight_width(std::uint32_t nx, std::uint32_t mul)
{
    return cells(nx) * mul;
}

inline constexpr std::uint32_t weight_height(std::uint32_t nz, std::uint32_t mul)
{
    return cells(nz) * mul;
}

inline constexpr std::size_t vertex_count(std::uint32_t nx, std::uint32_t nz)
{
    return static_cast<std::size_t>(nx) * nz;
}

inline constexpr std::size_t weight_texel_count(std::uint32_t nx, std::uint32_t nz, std::uint32_t mul)
{
    return static_cast<std::size_t>(weight_width(nx, mul)) * weight_height(nz, mul);
}

// World size along one axis of `verts` vertices.
inline constexpr float extent(std::uint32_t verts, float cell_size)
{
    return static_cast<float>(cells(verts)) * cell_size;
}

// Heightmap and splat images put row 0 at the terrain's +Z edge, so grid row z is image row
// image_row(z, rows), and the other way round.
inline constexpr std::uint32_t image_row(std::uint32_t z, std::uint32_t rows)
{
    return rows - 1 - z;
}

// ─── Heights ──────────────────────────────────────────────────────────────────

// Height above origin.y: 0 maps to height_min, 65535 to height_min + height_range.
inline float height_offset(std::uint16_t h, float height_min, float height_range)
{
    return height_min + (static_cast<float>(h) / 65535.0f) * height_range;
}

inline float world_y(float origin_y, std::uint16_t h, float height_min, float height_range)
{
    return origin_y + height_offset(h, height_min, height_range);
}

// Normalised 0..1 height to storage, rounded and clamped.
inline std::uint16_t encode_height01(float v)
{
    if (!(v > 0.0f)) return 0;
    if (v >= 1.0f) return 65535;
    return static_cast<std::uint16_t>(std::lround(v * 65535.0f));
}

// ─── Sculpting: height mapping growth ─────────────────────────────────────────
// An edit that needs offsets outside [height_min, height_min + height_range] widens the mapping and
// re-quantizes every height to it. The mapping never shrinks, and stays within validate_header.

// World units per storage step.
inline double height_step(float height_range)
{
    return static_cast<double>(height_range) / 65535.0;
}

// Offset above origin.y to storage, rounded and clamped to the mapping.
inline std::uint16_t encode_height(double offset, float height_min, float height_range)
{
    const double v = (offset - height_min) / height_range;
    if (!(v > 0.0)) return 0;
    if (v >= 1.0) return 65535;
    return static_cast<std::uint16_t>(std::lround(v * 65535.0));
}

inline double height_offset_exact(std::uint16_t h, float height_min, float height_range)
{
    return height_min + static_cast<double>(h) / 65535.0 * height_range;
}

// A side that has to grow gets this share of the old range beyond what it needs, so growing again
// is rare while a stroke keeps pushing the same way.
inline constexpr float height_growth_headroom = 0.25f;

struct HeightMapping
{
    float height_min;
    float height_range;
    bool clamped; // [need_lo, need_hi] did not fit the limits
};

// The mapping for offsets spanning [need_lo, need_hi] given the current one: unchanged when they fit,
// otherwise grown on the side(s) that need it by the need plus height_growth_headroom x range, the
// headroom below stopping at headroom_floor (growth_headroom_floor). Within height_min >= -max_coord and
// height_range <= max_height_range the old mapping is always kept inside; headroom is given up before
// need, and a need past the limits is shared by how far each side asked.
inline HeightMapping grow_height_mapping(float height_min, float height_range, float need_lo, float need_hi,
                                         double headroom_floor = -INFINITY)
{
    HeightMapping m{height_min, height_range, false};
    if (!std::isfinite(need_lo) || !std::isfinite(need_hi)) return m;
    const double old_lo = height_min, old_hi = static_cast<double>(height_min) + height_range;
    const double head = static_cast<double>(height_growth_headroom) * height_range;
    double need_l = old_lo, need_h = old_hi, want_lo = old_lo, want_hi = old_hi;
    if (need_lo < old_lo) {
        need_l = need_lo;
        want_lo = std::max(need_l - head, std::min(need_l, headroom_floor));
    }
    if (need_hi > old_hi) {
        need_h = need_hi;
        want_hi = need_h + head;
    }
    if (want_lo == old_lo && want_hi == old_hi) return m;

    const double floor_lo = -static_cast<double>(max_coord);
    want_lo = std::max(want_lo, std::min(floor_lo, old_lo));
    if (need_l < floor_lo) {
        need_l = std::min(floor_lo, old_lo);
        m.clamped = true;
    }
    const double cap = max_height_range;
    if (want_hi - want_lo > cap) {
        if (need_h - need_l <= cap) {
            const double head_lo = need_l - want_lo, head_hi = want_hi - need_h;
            const double keep = (cap - (need_h - need_l)) / (head_lo + head_hi);
            want_lo = need_l - head_lo * keep;
            want_hi = need_h + head_hi * keep;
        }
        else {
            m.clamped = true;
            const double spare = std::max(cap - (old_hi - old_lo), 0.0);
            const double grow_lo = old_lo - need_l, grow_hi = need_h - old_hi;
            want_lo = old_lo - spare * grow_lo / (grow_lo + grow_hi);
            want_hi = old_hi + spare * grow_hi / (grow_lo + grow_hi);
        }
    }
    m.height_min = std::min(static_cast<float>(want_lo), height_min);
    const double top = std::max(want_hi, old_hi);
    m.height_range = static_cast<float>(std::min(top - m.height_min, cap));
    m.height_range = std::max(m.height_range, height_range);
    return m;
}

// A closed (geoable) terrain's bottom, height_min - thickness (bottom_y), stays where it is while a
// growth lowers height_min: the thickness shrinks by as much (thickness_after_growth), and the headroom
// stops min_thickness above the bottom, so the bottom only moves once the surface is lowered through
// it, and then to min_thickness below the surface. Other terrains keep their thickness (skirts hang
// from the top edge).
inline double growth_headroom_floor(std::uint32_t flags, float height_min, float thickness)
{
    if (!(flags & flag_geoable)) return -INFINITY;
    return static_cast<double>(height_min) - thickness + min_thickness;
}

inline float thickness_after_growth(std::uint32_t flags, float thickness, float old_height_min,
                                    float new_height_min)
{
    if (!(flags & flag_geoable) || !(new_height_min < old_height_min)) return thickness;
    const double t = static_cast<double>(thickness) - (static_cast<double>(old_height_min) - new_height_min);
    return static_cast<float>(std::clamp(t, static_cast<double>(min_thickness), static_cast<double>(max_thickness)));
}

// ─── Per-cell bitmasks (holes, diagonals) ─────────────────────────────────────
// Bit (z * cells_x + x), LSB first within each byte. Padding bits are written clear and kept as read
// (lighting_fingerprint hashes them).
// Diagonal bit 0 splits the cell along (x, z)-(x+1, z+1); 1 along (x+1, z)-(x, z+1).

inline constexpr std::size_t cell_bit_index(std::uint32_t cells_x, std::uint32_t x, std::uint32_t z)
{
    return static_cast<std::size_t>(z) * cells_x + x;
}

inline constexpr std::size_t bitmask_bytes(std::uint32_t cells_x, std::uint32_t cells_z)
{
    return (static_cast<std::size_t>(cells_x) * cells_z + 7) / 8;
}

inline bool get_bit(const std::uint8_t* mask, std::size_t i)
{
    return (mask[i >> 3] >> (i & 7)) & 1u;
}

inline void set_bit(std::uint8_t* mask, std::size_t i, bool value)
{
    const std::uint8_t bit = static_cast<std::uint8_t>(1u << (i & 7));
    mask[i >> 3] = static_cast<std::uint8_t>(value ? (mask[i >> 3] | bit) : (mask[i >> 3] & ~bit));
}

inline bool get_cell_bit(const std::uint8_t* mask, std::uint32_t cells_x, std::uint32_t x,
                         std::uint32_t z)
{
    return get_bit(mask, cell_bit_index(cells_x, x, z));
}

inline void set_cell_bit(std::uint8_t* mask, std::uint32_t cells_x, std::uint32_t x, std::uint32_t z,
                         bool value)
{
    set_bit(mask, cell_bit_index(cells_x, x, z), value);
}

// ─── Chunk geo mask ───────────────────────────────────────────────────────────
// Which chunks of a geoable terrain the game marks geoable: bit k for chunk k in build mapping order,
// LSB first, padding bits clear in memory. Every chunk is emitted closed either way, so the mask is
// neither geometry nor lighting.

// The chunk grid a mask covers.
struct ChunkLayout
{
    std::uint32_t cells_x, cells_z, edge;

    bool operator==(const ChunkLayout& o) const
    {
        return cells_x == o.cells_x && cells_z == o.cells_z && edge == o.edge;
    }
    bool operator!=(const ChunkLayout& o) const { return !(*this == o); }
};

// RED's chunk grid for a new build of the terrain with flag_geoable set, whatever `flags` holds: the
// build mapping's layout whenever the mask matters.
inline constexpr ChunkLayout geo_chunk_layout(std::uint32_t nx, std::uint32_t nz, std::uint32_t chunk_cells,
                                              std::uint32_t flags)
{
    const std::uint32_t cx = cells(nx), cz = cells(nz);
    return {cx, cz, effective_chunk_cells(cx, cz, chunk_cells, flags | flag_geoable)};
}

inline constexpr std::uint32_t layout_chunk_count(const ChunkLayout& l)
{
    return chunk_count(l.cells_x, l.cells_z, l.edge);
}

inline constexpr std::size_t chunk_mask_bytes(std::uint32_t chunk_count)
{
    return (static_cast<std::size_t>(chunk_count) + 7) / 8;
}

inline void fill_chunk_mask(std::uint8_t* mask, std::uint32_t count)
{
    const std::size_t n = chunk_mask_bytes(count);
    for (std::size_t i = 0; i < n; i++) mask[i] = 0;
    for (std::uint32_t k = 0; k < count; k++) set_bit(mask, k, true);
}

inline bool chunk_mask_full(const std::uint8_t* mask, std::uint32_t count)
{
    for (std::uint32_t k = 0; k < count; k++) {
        if (!get_bit(mask, k)) return false;
    }
    return true;
}

// Clears the bits past `count` in the mask's last byte.
inline void clear_chunk_mask_padding(std::uint8_t* mask, std::uint32_t count)
{
    const std::size_t n = chunk_mask_bytes(count);
    for (std::size_t i = count; i < n * 8; i++) set_bit(mask, i, false);
}

// The chunk of `from` holding the centre of chunk `index` of `to`, each grid scaled to the other's cell
// count per axis so the mask follows content that a resample stretched; -1 when either grid is empty.
inline std::int64_t remap_chunk_index(const ChunkLayout& from, const ChunkLayout& to, std::uint32_t index)
{
    const std::uint32_t across = chunks_along(from.cells_x, from.edge), down = chunks_along(from.cells_z, from.edge);
    if (across == 0 || down == 0 || to.cells_x == 0 || to.cells_z == 0) return -1;
    const ChunkRect r = chunk_rect(to.cells_x, to.cells_z, to.edge, index);
    // Doubled, so the centre stays an integer.
    const std::uint64_t x2 = static_cast<std::uint64_t>(r.x0) + r.x1, z2 = static_cast<std::uint64_t>(r.z0) + r.z1;
    const std::uint64_t col = std::min<std::uint64_t>(x2 * from.cells_x / (2ull * from.edge * to.cells_x), across - 1);
    const std::uint64_t row = std::min<std::uint64_t>(z2 * from.cells_z / (2ull * from.edge * to.cells_z), down - 1);
    return static_cast<std::int64_t>(row * across + col);
}

// Chunk `index` of layout `to` read from a mask over layout `from`: the bit of the old chunk holding its
// centre, set when there is none. A null mask has every chunk set.
inline bool remapped_chunk_bit(const std::uint8_t* mask, const ChunkLayout& from, const ChunkLayout& to,
                               std::uint32_t index)
{
    if (!mask) return true;
    const std::int64_t k = from == to ? static_cast<std::int64_t>(index) : remap_chunk_index(from, to, index);
    return k < 0 || get_bit(mask, static_cast<std::size_t>(k));
}

// `out` (chunk_mask_bytes over `to`) from a mask over `from`, by remapped_chunk_bit.
inline void remap_chunk_mask(const std::uint8_t* mask, const ChunkLayout& from, std::uint8_t* out,
                             const ChunkLayout& to)
{
    const std::uint32_t n = layout_chunk_count(to);
    const std::size_t bytes = chunk_mask_bytes(n);
    for (std::size_t i = 0; i < bytes; i++) out[i] = 0;
    for (std::uint32_t k = 0; k < n; k++) set_bit(out, k, remapped_chunk_bit(mask, from, to, k));
}

// ─── Weights ──────────────────────────────────────────────────────────────────
// Two RGBA8 maps of weight_width x weight_height texels; map 0 holds layers 0-3, map 1 layers
// 4-7. Texel (i, j) covers cells [i/mul, (i+1)/mul) along x.

inline constexpr std::size_t weight_map_bytes(std::uint32_t nx, std::uint32_t nz, std::uint32_t mul)
{
    return weight_texel_count(nx, nz, mul) * 4;
}

// The 8 layer weights of texel `texel` of the two maps at `weights`, each map_bytes (weight_map_bytes) long.
inline void texel_weights(const std::uint8_t* weights, std::size_t map_bytes, std::size_t texel,
                          std::uint8_t (&w)[max_layers])
{
    for (std::size_t c = 0; c < 4; c++) {
        w[c] = weights[texel * 4 + c];
        w[4 + c] = weights[map_bytes + texel * 4 + c];
    }
}

// Where layer `layer`'s weight of texel 0 sits in the two maps; texel t's is 4 * t further.
inline constexpr std::size_t layer_weight_offset(std::uint32_t layer, std::size_t map_bytes)
{
    return (layer < 4 ? 0 : map_bytes) + (layer & 3);
}

inline void set_texel_weights(std::uint8_t* weights, std::size_t map_bytes, std::size_t texel,
                              const std::uint8_t (&w)[max_layers])
{
    for (std::size_t c = 0; c < 4; c++) {
        weights[texel * 4 + c] = w[c];
        weights[map_bytes + texel * 4 + c] = w[4 + c];
    }
}

// Rescales the 8 channels of one texel to sum exactly 255; an all-zero texel becomes layer 0.
inline void normalize_weights(std::uint8_t (&w)[max_layers])
{
    unsigned sum = 0;
    for (std::uint8_t v : w) sum += v;
    if (sum == 255) return;
    if (sum == 0) {
        w[0] = 255;
        return;
    }
    unsigned total = 0;
    std::size_t largest = 0;
    for (std::size_t i = 0; i < max_layers; i++) {
        w[i] = static_cast<std::uint8_t>((w[i] * 255u + sum / 2) / sum);
        total += w[i];
        if (w[i] > w[largest]) largest = i;
    }
    const int fix = 255 - static_cast<int>(total);
    w[largest] = static_cast<std::uint8_t>(std::clamp(static_cast<int>(w[largest]) + fix, 0, 255));
}

// Overlay coverage: one RGBA8 map laid out as a weight map, channel i for overlay i, channels past the
// overlay count zero.

inline constexpr std::size_t overlay_map_bytes(std::uint32_t nx, std::uint32_t nz, std::uint32_t mul)
{
    return weight_map_bytes(nx, nz, mul);
}

inline void clear_unused_overlay_channels(std::uint8_t* map, std::size_t bytes, std::uint32_t overlay_count)
{
    for (std::size_t i = 0; i < bytes; i += 4) {
        for (std::uint32_t c = overlay_count; c < max_overlays; c++) map[i + c] = 0;
    }
}

// ─── Decompressed blob ────────────────────────────────────────────────────────
// heights u16[nx*nz] (little-endian) | weight map 0 | weight map 1 | holes mask | diagonal mask
// [| chunk geo mask (blob_geo_mask_bytes), with flag_chunk_geo_mask]
// [| overlay coverage map (overlay_map_bytes), with flag_overlays]
// [| decoration_count coverage planes (decoration_plane_bytes each, in list order), with flag_decorations]

inline constexpr std::size_t blob_heights_offset()
{
    return 0;
}

inline constexpr std::size_t blob_heights_bytes(std::uint32_t nx, std::uint32_t nz)
{
    return vertex_count(nx, nz) * 2;
}

inline constexpr std::size_t blob_weights_offset(std::uint32_t nx, std::uint32_t nz)
{
    return blob_heights_bytes(nx, nz);
}

inline constexpr std::size_t blob_weights_bytes(std::uint32_t nx, std::uint32_t nz, std::uint32_t mul)
{
    return weight_map_bytes(nx, nz, mul) * 2;
}

inline constexpr std::size_t blob_holes_offset(std::uint32_t nx, std::uint32_t nz, std::uint32_t mul)
{
    return blob_weights_offset(nx, nz) + blob_weights_bytes(nx, nz, mul);
}

inline constexpr std::size_t blob_diag_offset(std::uint32_t nx, std::uint32_t nz, std::uint32_t mul)
{
    return blob_holes_offset(nx, nz, mul) + bitmask_bytes(cells(nx), cells(nz));
}

inline constexpr std::size_t blob_raw_size(std::uint32_t nx, std::uint32_t nz, std::uint32_t mul)
{
    return blob_diag_offset(nx, nz, mul) + bitmask_bytes(cells(nx), cells(nz));
}

inline constexpr std::size_t blob_geo_mask_offset(std::uint32_t nx, std::uint32_t nz, std::uint32_t mul)
{
    return blob_raw_size(nx, nz, mul);
}

// `chunk_cells` and `flags` as on the wire: the mask covers the record's stored chunk grid, padding bits
// written clear and ignored on read.
inline constexpr std::size_t blob_geo_mask_bytes(std::uint32_t nx, std::uint32_t nz, std::uint32_t chunk_cells,
                                                 std::uint32_t flags)
{
    return (flags & flag_chunk_geo_mask) ? chunk_mask_bytes(chunk_count(cells(nx), cells(nz), chunk_cells)) : 0;
}

inline constexpr std::size_t blob_overlay_offset(std::uint32_t nx, std::uint32_t nz, std::uint32_t mul,
                                                 std::uint32_t chunk_cells, std::uint32_t flags)
{
    return blob_geo_mask_offset(nx, nz, mul) + blob_geo_mask_bytes(nx, nz, chunk_cells, flags);
}

inline constexpr std::size_t blob_overlay_bytes(std::uint32_t nx, std::uint32_t nz, std::uint32_t mul,
                                                std::uint32_t flags)
{
    return (flags & flag_overlays) ? overlay_map_bytes(nx, nz, mul) : 0;
}

// One byte of coverage per weight texel, laid out as a weight map.
inline constexpr std::size_t decoration_plane_bytes(std::uint32_t nx, std::uint32_t nz, std::uint32_t mul)
{
    return weight_texel_count(nx, nz, mul);
}

inline constexpr std::size_t blob_decoration_offset(std::uint32_t nx, std::uint32_t nz, std::uint32_t mul,
                                                    std::uint32_t chunk_cells, std::uint32_t flags)
{
    return blob_overlay_offset(nx, nz, mul, chunk_cells, flags) + blob_overlay_bytes(nx, nz, mul, flags);
}

inline constexpr std::size_t blob_decoration_bytes(std::uint32_t nx, std::uint32_t nz, std::uint32_t mul,
                                                   std::uint32_t flags, std::uint32_t decoration_count)
{
    return (flags & flag_decorations) ? decoration_plane_bytes(nx, nz, mul) * decoration_count : 0;
}

static_assert(blob_raw_size(max_verts, max_verts, 4) + chunk_mask_bytes(max_chunks) +
                  overlay_map_bytes(max_verts, max_verts, 4) +
                  max_decorations * decoration_plane_bytes(max_verts, max_verts, 4) < 0xFFFFFFFFull);

// ─── Validation (every reader, and the editor before it writes) ──────────────

inline bool finite_in(float v, float lo, float hi)
{
    return std::isfinite(v) && v >= lo && v <= hi;
}

// v clamped into [lo, hi], or def when v is not finite.
inline float clamp_finite(float v, float lo, float hi, float def)
{
    return std::isfinite(v) ? std::clamp(v, lo, hi) : def;
}

inline bool texture_name_valid(const char* name, std::size_t len)
{
    if (len > max_texture_name_len) return false;
    for (std::size_t i = len; i > 0; i--) {
        if (name[i - 1] == '.') return len - (i - 1) <= max_texture_ext_len;
    }
    return true;
}

// nullptr when the header is usable, else the reason.
inline const char* validate_header(const Header& h)
{
    for (float c : h.origin) {
        if (!finite_in(c, -max_coord, max_coord)) return "origin out of range";
    }
    if (!finite_in(h.cell_size, min_cell_size, max_cell_size)) return "cell size out of range";
    if (h.nx < min_verts || h.nx > max_verts || h.nz < min_verts || h.nz > max_verts) {
        return "vertex count out of range";
    }
    if (!finite_in(h.height_min, -max_coord, max_coord)) return "height min out of range";
    if (!finite_in(h.height_range, min_height_range, max_height_range)) {
        return "height range out of range";
    }
    if (!is_allowed_chunk_cells(h.chunk_cells)) return "chunk size not allowed";
    if (chunk_count(cells(h.nx), cells(h.nz), h.chunk_cells) > max_chunks) return "too many chunks";
    if (!is_allowed_weight_res_mul(h.weight_res_mul)) return "weight resolution not allowed";
    if (h.lightmap_density < lightmap_density_min || h.lightmap_density > lightmap_density_max) {
        return "lightmap density out of range";
    }
    if ((h.flags & ~static_cast<std::uint32_t>(wire_flag_mask)) != 0) return "unknown flags";
    if ((h.flags & flag_chunk_geo_mask) && !(h.flags & flag_geoable)) return "chunk geo mask without geoable";
    if (!finite_in(h.thickness, min_thickness, max_thickness)) return "thickness out of range";
    if (!finite_in(h.skirt_depth, 0.0f, max_skirt_depth)) return "skirt depth out of range";
    if (h.layer_count < 1 || h.layer_count > max_layers) return "layer count out of range";
    return nullptr;
}

inline const char* validate_layer(float uv_scale, std::uint32_t layer_flags)
{
    if (!finite_in(uv_scale, min_uv_scale, max_uv_scale)) return "layer uv scale out of range";
    if ((layer_flags & ~static_cast<std::uint32_t>(layer_flag_mask)) != 0) return "unknown layer flags";
    return nullptr;
}

// 1..max_overlays with flag_overlays, else 0.
inline const char* validate_overlay_count(const Header& h)
{
    const bool listed = (h.flags & flag_overlays) != 0;
    if (listed ? h.overlay_count < 1 || h.overlay_count > max_overlays : h.overlay_count != 0) {
        return "overlay count out of range";
    }
    return nullptr;
}

inline const char* validate_overlay(float uv_scale, std::uint32_t overlay_flags)
{
    if (!finite_in(uv_scale, min_uv_scale, max_uv_scale)) return "overlay uv scale out of range";
    if ((overlay_flags & ~static_cast<std::uint32_t>(overlay_flag_mask)) != 0) return "unknown overlay flags";
    return nullptr;
}

// 1..max_decorations with flag_decorations, else 0.
inline const char* validate_decoration_count(const Header& h)
{
    const bool listed = (h.flags & flag_decorations) != 0;
    if (listed ? h.decoration_count < 1 || h.decoration_count > max_decorations : h.decoration_count != 0) {
        return "decoration count out of range";
    }
    return nullptr;
}

inline constexpr char ascii_lower(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

// True when `name` (len >= 4) ends in `ext` (a lowercase ".xyz"), ignoring case.
inline bool decoration_mesh_ext_is(const char* name, std::size_t len, const char* ext)
{
    for (std::size_t i = 0; i < 4; i++) {
        if (ascii_lower(name[len - 4 + i]) != ext[i]) return false;
    }
    return true;
}

// A decoration's mesh: empty (none picked yet), or a .v3m or .vfx file name within the texture name caps and
// without a path. .vfx (animated) is reserved: readers treat it as a mesh that fails to load.
inline bool decoration_mesh_valid(const char* name, std::size_t len)
{
    if (len == 0) return true;
    if (!texture_name_valid(name, len) || len < 4) return false;
    for (std::size_t i = 0; i < len; i++) {
        const char c = name[i];
        if (c == '/' || c == '\\' || c == ':' || c == '\0') return false;
    }
    return decoration_mesh_ext_is(name, len, ".v3m") || decoration_mesh_ext_is(name, len, ".vfx");
}

// A valid decoration mesh name that names an animated (.vfx) mesh.
inline bool decoration_mesh_is_vfx(const char* name, std::size_t len)
{
    return len >= 4 && decoration_mesh_ext_is(name, len, ".vfx");
}

inline const char* validate_decoration(float density, float scale_min, float scale_max, float max_slope_deg,
                                       float draw_distance, float vertical_offset, std::uint32_t link_layer,
                                       std::uint32_t flags, std::uint32_t layer_count)
{
    if (!finite_in(density, 0.0f, max_decoration_density)) return "decoration density out of range";
    if (!finite_in(scale_min, min_decoration_scale, max_decoration_scale)) return "decoration scale min out of range";
    if (!finite_in(scale_max, min_decoration_scale, max_decoration_scale)) return "decoration scale max out of range";
    if (!finite_in(max_slope_deg, 0.0f, max_decoration_slope_deg)) return "decoration max slope out of range";
    if (!finite_in(draw_distance, min_decoration_draw_distance, max_decoration_draw_distance)) {
        return "decoration draw distance out of range";
    }
    if (!finite_in(vertical_offset, -max_decoration_offset, max_decoration_offset)) {
        return "decoration vertical offset out of range";
    }
    if (scale_max < scale_min) return "decoration scale max below scale min";
    if (link_layer != decoration_link_none && link_layer >= layer_count) return "decoration link layer out of range";
    if ((flags & ~static_cast<std::uint32_t>(decoration_flag_mask)) != 0) return "unknown decoration flags";
    return nullptr;
}

// A decoration's wire flags from its align_to_slope, random_yaw and casts_shadows.
template<typename Decoration>
constexpr std::uint32_t decoration_flags(const Decoration& d)
{
    return (d.align_to_slope ? decoration_flag_align_to_slope : 0u) | (d.random_yaw ? decoration_flag_random_yaw : 0u) |
           (d.casts_shadows ? decoration_flag_casts_shadows : 0u);
}

// The chunk grid a record was built with, its stored chunk_cells: the build mapping and the chunk geo
// mask cover it.
inline ChunkLayout header_chunk_layout(const Header& h)
{
    return {cells(h.nx), cells(h.nz), h.chunk_cells};
}

inline std::uint32_t header_chunk_count(const Header& h)
{
    return layout_chunk_count(header_chunk_layout(h));
}

// A build mapping is either absent or covers every chunk.
inline bool mapping_count_valid(const Header& h, std::uint32_t mapping_count)
{
    return mapping_count == 0 || mapping_count == header_chunk_count(h);
}

// A mapping count a reader may read past: one that is not mapping_count_valid is read and dropped, and
// the terrain loads unbuilt.
inline bool mapping_count_acceptable(std::uint32_t mapping_count)
{
    return mapping_count <= max_chunks;
}

// The decompressed blob of a terrain with this wire chunk_cells, these wire flags and this decoration count.
inline constexpr std::size_t wire_raw_size(std::uint32_t nx, std::uint32_t nz, std::uint32_t mul,
                                           std::uint32_t chunk_cells, std::uint32_t flags,
                                           std::uint32_t decoration_count)
{
    return blob_decoration_offset(nx, nz, mul, chunk_cells, flags) +
           blob_decoration_bytes(nx, nz, mul, flags, decoration_count);
}

inline std::size_t header_raw_size(const Header& h)
{
    return wire_raw_size(h.nx, h.nz, h.weight_res_mul, h.chunk_cells, h.flags, h.decoration_count);
}

// ─── Texturing ────────────────────────────────────────────────────────────────

// Planar XZ layer mapping: uv_scale is world units per texture repeat, u = x / uv_scale and
// v = z / uv_scale. The terrain shader samples layers with the same function.
inline void layer_uv(float world_x, float world_z, float uv_scale, float (&uv)[2])
{
    uv[0] = world_x / uv_scale;
    uv[1] = world_z / uv_scale;
}

// Walls use the underside texture at layer 0's scale, planar on the wall's own plane: u runs along
// the wall (world x for walls facing +-Z, world z for walls facing +-X), v = -y / uv_scale so the
// texture stands upright.
inline void wall_uv(float along, float world_y, float uv_scale, float (&uv)[2])
{
    uv[0] = along / uv_scale;
    uv[1] = -world_y / uv_scale;
}

// ─── Chunk emission ───────────────────────────────────────────────────────────
// Every derivation of the geometry Build Geometry compiles for a terrain. Positions are world
// space; faces wind so Newell's normal, (p1 - p0) x (p2 - p0), points out of the solid.

struct GridView
{
    const std::uint16_t* heights;
    const std::uint8_t* weights; // weight map 0 then weight map 1, as in the blob
    const std::uint8_t* holes;
    const std::uint8_t* diag;
    std::uint32_t nx, nz, weight_res_mul;
    float origin[3];
    float cell_size, height_min, height_range;
    std::uint32_t flags;
    float thickness, skirt_depth;
    std::uint32_t layer_count;
    std::uint32_t textured_layers; // bit i set when layer i names a texture
    float layer_uv_scale[max_layers];
};

// The view of a terrain's arrays with placement and shape from `h` (flags masked to flag_mask). `layers`
// holds h.layer_count elements with .texture (.empty()) and .uv_scale; uv slots past them stay 0, which only
// material_fingerprint reads.
template<typename Layer>
GridView make_grid_view(const Header& h, const std::uint16_t* heights, const std::uint8_t* weights,
                        const std::uint8_t* holes, const std::uint8_t* diag, const Layer* layers)
{
    GridView v{};
    v.heights = heights;
    v.weights = weights;
    v.holes = holes;
    v.diag = diag;
    v.nx = h.nx;
    v.nz = h.nz;
    v.weight_res_mul = h.weight_res_mul;
    std::memcpy(v.origin, h.origin, sizeof(v.origin));
    v.cell_size = h.cell_size;
    v.height_min = h.height_min;
    v.height_range = h.height_range;
    v.flags = h.flags & flag_mask;
    v.thickness = h.thickness;
    v.skirt_depth = h.skirt_depth;
    v.layer_count = h.layer_count;
    for (std::uint32_t l = 0; l < h.layer_count && l < max_layers; l++) {
        if (!layers[l].texture.empty()) v.textured_layers |= 1u << l;
        v.layer_uv_scale[l] = layers[l].uv_scale;
    }
    return v;
}

// A face's material: a layer index, or the underside texture.
inline constexpr std::uint32_t material_underside = max_layers;

struct EmitFace
{
    std::uint32_t count; // 3 or 4
    std::uint32_t slot[4];
    float uv[4][2];
    std::uint32_t material;
};

// Skirts shorter than this would be degenerate, so they are not emitted.
inline constexpr float min_skirt_depth = 0.01f;

// The layer a cell's faces carry: the argmax of its summed weights (lowest index on a tie); an untextured
// layer falls back to layer 0.
inline std::uint32_t dominant_layer(const GridView& g, std::uint32_t x, std::uint32_t z)
{
    const std::uint32_t m = g.weight_res_mul;
    const std::size_t row = weight_width(g.nx, m);
    const std::size_t map = weight_map_bytes(g.nx, g.nz, m);
    std::uint32_t sum[max_layers] = {};
    for (std::uint32_t j = z * m; j < z * m + m; j++) {
        for (std::uint32_t i = x * m; i < x * m + m; i++) {
            std::uint8_t w[max_layers];
            texel_weights(g.weights, map, j * row + i, w);
            for (std::uint32_t c = 0; c < max_layers; c++) sum[c] += w[c];
        }
    }
    std::uint32_t best = 0;
    const std::uint32_t n = std::min(g.layer_count, max_layers);
    for (std::uint32_t l = 1; l < n; l++) {
        if (sum[l] > sum[best]) best = l;
    }
    return ((g.textured_layers >> best) & 1u) ? best : 0;
}

inline float top_y(const GridView& g, std::uint32_t x, std::uint32_t z)
{
    return world_y(g.origin[1], g.heights[static_cast<std::size_t>(z) * g.nx + x], g.height_min,
                   g.height_range);
}

// World position of grid vertex (x, z) on the top surface.
inline void grid_position(const GridView& g, std::uint32_t x, std::uint32_t z, float (&out)[3])
{
    out[0] = g.origin[0] + static_cast<float>(x) * g.cell_size;
    out[1] = top_y(g, x, z);
    out[2] = g.origin[2] + static_cast<float>(z) * g.cell_size;
}

// Relative slack of a few float ulps, scaled by the magnitude of the coordinates involved.
inline constexpr float coord_ulps = 4.8e-7f;

// World y of the emitted surface at world (x, z), on the triangle the cell's diagonal bit picks, so
// it is exact at every grid vertex and follows the compiled faces between them. Clamped to the grid; NaN
// for a NaN coordinate.
inline float height_at(const GridView& g, float world_x, float world_z)
{
    const std::uint32_t cx = cells(g.nx), cz = cells(g.nz);
    float fx = std::clamp((world_x - g.origin[0]) / g.cell_size, 0.0f, static_cast<float>(cx));
    float fz = std::clamp((world_z - g.origin[2]) / g.cell_size, 0.0f, static_cast<float>(cz));
    if (std::isnan(fx) || std::isnan(fz)) return NAN;
    // A vertex written as origin + i * cell_size does not divide back to exactly i: allow a few ulps
    // of the coordinates involved, in cells.
    const float ulps = coord_ulps / g.cell_size;
    const float tol_x = std::clamp((std::fabs(world_x) + std::fabs(g.origin[0])) * ulps, 1e-4f, 0.1f);
    const float tol_z = std::clamp((std::fabs(world_z) + std::fabs(g.origin[2])) * ulps, 1e-4f, 0.1f);
    if (std::fabs(fx - std::round(fx)) < tol_x) fx = std::round(fx);
    if (std::fabs(fz - std::round(fz)) < tol_z) fz = std::round(fz);
    const std::uint32_t x = std::min(static_cast<std::uint32_t>(fx), cx - 1);
    const std::uint32_t z = std::min(static_cast<std::uint32_t>(fz), cz - 1);
    const float u = fx - static_cast<float>(x), v = fz - static_cast<float>(z);
    const float h00 = top_y(g, x, z), h10 = top_y(g, x + 1, z);
    const float h11 = top_y(g, x + 1, z + 1), h01 = top_y(g, x, z + 1);
    if (!get_cell_bit(g.diag, cx, x, z)) {
        return u >= v ? h00 + u * (h10 - h00) + v * (h11 - h10) : h00 + v * (h01 - h00) + u * (h11 - h01);
    }
    return u + v <= 1.0f ? h00 + u * (h10 - h00) + v * (h01 - h00)
                         : h11 + (1.0f - u) * (h01 - h11) + (1.0f - v) * (h10 - h11);
}

// How far a point may sit from the surface and still be on it; below min_skirt_depth and min_thickness.
inline constexpr float surface_epsilon = 0.005f;
static_assert(surface_epsilon < min_skirt_depth && surface_epsilon < min_thickness);

// on_surface's cap for a closed chunk, whose underside is at least min_thickness below its top.
inline constexpr float max_surface_tolerance = 0.1f;
static_assert(max_surface_tolerance >= surface_epsilon && max_surface_tolerance < min_thickness);

// on_surface's slack at a point. RF2 carving re-splits a closed chunk's top in float, so there it is
// the ulps of the coordinates plus their lateral error (the point's own ulps and height_at's snap to a
// grid line) times the steepest cell edge around the nearest grid vertex. Other chunks keep
// surface_epsilon: their skirts reach only min_skirt_depth below.
inline float surface_tolerance(const GridView& g, float world_x, float world_y, float world_z)
{
    if (!(g.flags & flag_geoable)) return surface_epsilon;
    const float ax = std::fabs(world_x), ay = std::fabs(world_y), az = std::fabs(world_z);
    if (!(ax + ay + az < 2.0f * max_coord)) return surface_epsilon;
    const float lat_x = (2.0f * ax + std::fabs(g.origin[0])) * coord_ulps + 1e-4f * g.cell_size;
    const float lat_z = (2.0f * az + std::fabs(g.origin[2])) * coord_ulps + 1e-4f * g.cell_size;
    const std::uint32_t cx = cells(g.nx), cz = cells(g.nz);
    auto nearest = [&](float c, float o, std::uint32_t n) {
        const float f = std::clamp((c - o) / g.cell_size, 0.0f, static_cast<float>(n));
        return static_cast<std::uint32_t>(f + 0.5f);
    };
    const std::uint32_t vx = nearest(world_x, g.origin[0], cx), vz = nearest(world_z, g.origin[2], cz);
    const std::uint32_t x0 = vx > 0 ? vx - 1 : 0, x1 = std::min(vx + 1, cx);
    const std::uint32_t z0 = vz > 0 ? vz - 1 : 0, z1 = std::min(vz + 1, cz);
    float rise_x = 0.0f, rise_z = 0.0f;
    for (std::uint32_t z = z0; z <= z1; z++) {
        for (std::uint32_t x = x0; x <= x1; x++) {
            if (x < x1) rise_x = std::max(rise_x, std::fabs(top_y(g, x + 1, z) - top_y(g, x, z)));
            if (z < z1) rise_z = std::max(rise_z, std::fabs(top_y(g, x, z + 1) - top_y(g, x, z)));
        }
    }
    const float t = (ax + ay + az) * coord_ulps + (rise_x * lat_x + rise_z * lat_z) / g.cell_size;
    return std::clamp(t, surface_epsilon, max_surface_tolerance);
}

inline bool on_surface(const GridView& g, float world_x, float world_y, float world_z)
{
    if (!std::isfinite(world_x) || !std::isfinite(world_y) || !std::isfinite(world_z)) return false;
    const float tol = surface_tolerance(g, world_x, world_y, world_z);
    const float ex = extent(g.nx, g.cell_size), ez = extent(g.nz, g.cell_size);
    const float lx = world_x - g.origin[0], lz = world_z - g.origin[2];
    if (lx < -tol || lz < -tol || lx > ex + tol || lz > ez + tol) {
        return false;
    }
    return std::fabs(world_y - height_at(g, world_x, world_z)) <= tol;
}

// Closed chunks share one flat bottom.
inline float bottom_y(const GridView& g)
{
    return g.origin[1] + g.height_min - g.thickness;
}

inline bool cell_solid(const GridView& g, std::int64_t x, std::int64_t z)
{
    const std::uint32_t cx = cells(g.nx), cz = cells(g.nz);
    if (x < 0 || z < 0 || x >= cx || z >= cz) return false;
    return !get_cell_bit(g.holes, cx, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(z));
}

// Vertex slots of a chunk: the top grid, then a lower grid (the bottom of a closed chunk, or the
// foot of a skirt) at the same x/z. Only slots a face references are ever emitted.
inline std::uint32_t chunk_slot_count(const ChunkRect& r)
{
    return 2 * (r.x1 - r.x0 + 1) * (r.z1 - r.z0 + 1);
}

inline std::uint32_t top_slot(const ChunkRect& r, std::uint32_t x, std::uint32_t z)
{
    return (z - r.z0) * (r.x1 - r.x0 + 1) + (x - r.x0);
}

inline std::uint32_t low_slot(const ChunkRect& r, std::uint32_t x, std::uint32_t z)
{
    return top_slot(r, x, z) + (r.x1 - r.x0 + 1) * (r.z1 - r.z0 + 1);
}

inline void slot_position(const GridView& g, const ChunkRect& r, std::uint32_t slot, float (&out)[3])
{
    const std::uint32_t grid = (r.x1 - r.x0 + 1) * (r.z1 - r.z0 + 1);
    const bool low = slot >= grid;
    const std::uint32_t s = low ? slot - grid : slot;
    const std::uint32_t x = r.x0 + s % (r.x1 - r.x0 + 1);
    const std::uint32_t z = r.z0 + s / (r.x1 - r.x0 + 1);
    grid_position(g, x, z, out);
    if (low) out[1] = (g.flags & flag_geoable) ? bottom_y(g) : out[1] - g.skirt_depth;
}

// A closed chunk with no hole and at least two cells each way: its bottom is one fan from an interior
// lower-grid vertex instead of a quad per cell.
inline bool bottom_is_fan(const GridView& g, const ChunkRect& r)
{
    if (!(g.flags & flag_geoable) || r.x1 - r.x0 < 2 || r.z1 - r.z0 < 2) return false;
    for (std::uint32_t z = r.z0; z < r.z1; z++) {
        for (std::uint32_t x = r.x0; x < r.x1; x++) {
            if (!cell_solid(g, x, z)) return false;
        }
    }
    return true;
}

// Emits every face of chunk `r` to sink(const EmitFace&):
//  - top: two triangles per non-hole cell, split by the cell's diagonal bit, textured with its
//    dominant layer;
//  - geoable: a closed solid, with a wall quad on every edge between a non-hole cell of the chunk and
//    anything that is not one (the chunk's sides and every hole), and a bottom at bottom_y sharing the
//    walls' lower vertices: a quad under every non-hole cell, or with bottom_is_fan a triangle from the
//    middle lower vertex to each lower wall edge;
//  - otherwise with skirts: wall quads only on the terrain's outer edges and around holes, reaching
//    skirt_depth below the top edge;
//  - otherwise: the top alone.
// Walls and bottoms carry material_underside.
template<typename Sink>
void emit_chunk(const GridView& g, const ChunkRect& r, Sink&& sink)
{
    const bool closed = (g.flags & flag_geoable) != 0;
    const bool skirts = !closed && (g.flags & flag_skirts) && g.skirt_depth >= min_skirt_depth;
    const bool fan = bottom_is_fan(g, r);
    const std::uint32_t cx = cells(g.nx);
    const float s0 = g.layer_uv_scale[0];

    auto pos = [&](std::uint32_t slot, float (&p)[3]) { slot_position(g, r, slot, p); };

    auto bottom = [&](std::uint32_t count, const std::uint32_t (&slots)[4]) {
        EmitFace f{};
        f.count = count;
        f.material = material_underside;
        for (std::uint32_t k = 0; k < count; k++) {
            f.slot[k] = slots[k];
            float p[3];
            pos(f.slot[k], p);
            layer_uv(p[0], p[2], s0, f.uv[k]);
        }
        sink(static_cast<const EmitFace&>(f));
    };

    auto wall = [&](std::uint32_t px, std::uint32_t pz, std::uint32_t qx, std::uint32_t qz, bool along_x) {
        EmitFace f{};
        f.count = 4;
        f.material = material_underside;
        f.slot[0] = top_slot(r, px, pz);
        f.slot[1] = top_slot(r, qx, qz);
        f.slot[2] = low_slot(r, qx, qz);
        f.slot[3] = low_slot(r, px, pz);
        for (std::uint32_t k = 0; k < 4; k++) {
            float p[3];
            pos(f.slot[k], p);
            wall_uv(along_x ? p[0] : p[2], p[1], s0, f.uv[k]);
        }
        sink(static_cast<const EmitFace&>(f));
    };

    // A solid cell's edge gets a wall when the cell across it is not part of the same solid.
    auto walled = [&](std::int64_t nx_, std::int64_t nz_) {
        if (closed) {
            const bool inside = nx_ >= r.x0 && nx_ < r.x1 && nz_ >= r.z0 && nz_ < r.z1;
            return !inside || !cell_solid(g, nx_, nz_);
        }
        return skirts && !cell_solid(g, nx_, nz_);
    };

    for (std::uint32_t z = r.z0; z < r.z1; z++) {
        for (std::uint32_t x = r.x0; x < r.x1; x++) {
            if (!cell_solid(g, x, z)) continue;

            const std::uint32_t a = top_slot(r, x, z), b = top_slot(r, x + 1, z);
            const std::uint32_t c = top_slot(r, x + 1, z + 1), d = top_slot(r, x, z + 1);
            const std::uint32_t layer = dominant_layer(g, x, z);
            const float scale = g.layer_uv_scale[layer];
            const bool diag = get_cell_bit(g.diag, cx, x, z);
            const std::uint32_t tris[2][3] = {{a, diag ? d : c, b}, {diag ? b : a, d, c}};
            for (const auto& t : tris) {
                EmitFace f{};
                f.count = 3;
                f.material = layer;
                for (std::uint32_t k = 0; k < 3; k++) {
                    f.slot[k] = t[k];
                    float p[3];
                    pos(t[k], p);
                    layer_uv(p[0], p[2], scale, f.uv[k]);
                }
                sink(static_cast<const EmitFace&>(f));
            }

            const std::int64_t sx = x, sz = z;
            if (walled(sx, sz - 1)) wall(x, z, x + 1, z, true);                 // faces -z
            if (walled(sx, sz + 1)) wall(x + 1, z + 1, x, z + 1, true);         // faces +z
            if (walled(sx - 1, sz)) wall(x, z + 1, x, z, false);                // faces -x
            if (walled(sx + 1, sz)) wall(x + 1, z, x + 1, z + 1, false);        // faces +x

            if (closed && !fan) {
                bottom(4, {low_slot(r, x, z), low_slot(r, x + 1, z), low_slot(r, x + 1, z + 1), low_slot(r, x, z + 1)});
            }
        }
    }

    if (fan) {
        // The lower perimeter in the bottom quads' winding: -z side, +x, +z, -x.
        const std::uint32_t mid = low_slot(r, r.x0 + (r.x1 - r.x0) / 2, r.z0 + (r.z1 - r.z0) / 2);
        for (std::uint32_t x = r.x0; x < r.x1; x++) bottom(3, {mid, low_slot(r, x, r.z0), low_slot(r, x + 1, r.z0)});
        for (std::uint32_t z = r.z0; z < r.z1; z++) bottom(3, {mid, low_slot(r, r.x1, z), low_slot(r, r.x1, z + 1)});
        for (std::uint32_t x = r.x1; x > r.x0; x--) bottom(3, {mid, low_slot(r, x, r.z1), low_slot(r, x - 1, r.z1)});
        for (std::uint32_t z = r.z1; z > r.z0; z--) bottom(3, {mid, low_slot(r, r.x0, z), low_slot(r, r.x0, z - 1)});
    }
}

// ─── Normals and ray queries ──────────────────────────────────────────────────

// Unit normal at vertex (x, z) by central differences of the world heights, one-sided at the border:
// the derivation the terrain pixel shader (ter_normal) samples at a vertex.
inline void vertex_normal(const GridView& g, std::uint32_t x, std::uint32_t z, float (&n)[3])
{
    const std::uint32_t lx = x > 0 ? x - 1 : 0, hx = x + 1 < g.nx ? x + 1 : x;
    const std::uint32_t lz = z > 0 ? z - 1 : 0, hz = z + 1 < g.nz ? z + 1 : z;
    const float sx = static_cast<float>(hx > lx ? hx - lx : 1) * g.cell_size;
    const float sz = static_cast<float>(hz > lz ? hz - lz : 1) * g.cell_size;
    n[0] = (top_y(g, lx, z) - top_y(g, hx, z)) / sx;
    n[1] = 1.0f;
    n[2] = (top_y(g, x, lz) - top_y(g, x, hz)) / sz;
    const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    for (float& c : n) c /= len;
}

// The normalised height at heightmap texel coordinates (u, v), vertex (x, z) being texel centre
// (x + 0.5, z + 0.5): what the terrain shader's clamped bilinear read of its R16 height map returns.
inline float height01_bilinear(const GridView& g, float u, float v)
{
    auto axis = [](float c, std::uint32_t n, std::uint32_t& i0, std::uint32_t& i1, float& t) {
        const float f = std::isfinite(c) ? std::clamp(c - 0.5f, -1.0f, static_cast<float>(n)) : 0.0f;
        const float fl = std::floor(f);
        t = f - fl;
        const std::int64_t i = static_cast<std::int64_t>(fl);
        const std::int64_t hi = static_cast<std::int64_t>(n) - 1;
        i0 = static_cast<std::uint32_t>(std::clamp<std::int64_t>(i, 0, hi));
        i1 = static_cast<std::uint32_t>(std::clamp<std::int64_t>(i + 1, 0, hi));
    };
    std::uint32_t x0, x1, z0, z1;
    float tx, tz;
    axis(u, g.nx, x0, x1, tx);
    axis(v, g.nz, z0, z1, tz);
    auto h = [&](std::uint32_t x, std::uint32_t z) {
        return static_cast<float>(g.heights[static_cast<std::size_t>(z) * g.nx + x]) / 65535.0f;
    };
    const float a = h(x0, z0) + (h(x1, z0) - h(x0, z0)) * tx;
    const float b = h(x0, z1) + (h(x1, z1) - h(x0, z1)) * tx;
    return a + (b - a) * tz;
}

// The shading normal: central differences one cell either side (one-sided at the grid edge). HLSL copy:
// ter_normal.
inline void heightmap_normal(const GridView& g, float world_x, float world_z, float (&n)[3])
{
    const float tx = (world_x - g.origin[0]) / g.cell_size + 0.5f;
    const float tz = (world_z - g.origin[2]) / g.cell_size + 0.5f;
    const float lo_x = std::max(tx - 1.0f, 0.5f), hi_x = std::min(tx + 1.0f, static_cast<float>(g.nx) - 0.5f);
    const float lo_z = std::max(tz - 1.0f, 0.5f), hi_z = std::min(tz + 1.0f, static_cast<float>(g.nz) - 0.5f);
    const float hl = height01_bilinear(g, lo_x, tz), hr = height01_bilinear(g, hi_x, tz);
    const float hd = height01_bilinear(g, tx, lo_z), hu = height01_bilinear(g, tx, hi_z);
    const float kx = g.height_range / (std::max(hi_x - lo_x, 1e-3f) * g.cell_size);
    const float kz = g.height_range / (std::max(hi_z - lo_z, 1e-3f) * g.cell_size);
    n[0] = (hl - hr) * kx;
    n[1] = 1.0f;
    n[2] = (hd - hu) * kz;
    const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    for (float& c : n) c /= len;
}

// ─── Chunk faces with no surface ──────────────────────────────────────────────
// top: every vertex on the heightfield. underside: every vertex on one wall plane (a grid line of x
// or z: chunk sides, hole walls, skirts) or on a closed chunk's bottom. crater: anything else in a
// closed chunk, which only RF2 carving adds.

enum class FaceKind : std::uint8_t
{
    top,
    underside,
    crater,
};

// A world coordinate's slack: surface_epsilon, or a few ulps of the coordinates involved.
inline float coord_tolerance(float a, float b)
{
    return std::max(surface_epsilon, (std::fabs(a) + std::fabs(b)) * coord_ulps);
}

// for_each_vertex(visit) calls visit(x, y, z) once per vertex of the face.
template<typename ForEachVertex>
FaceKind face_kind(const GridView& g, ForEachVertex&& for_each_vertex)
{
    const bool closed = (g.flags & flag_geoable) != 0;
    const float by = bottom_y(g);
    const float max_line = static_cast<float>(std::max(g.nx, g.nz));
    bool top = true, bottom = closed, wall_x = true, wall_z = true;
    std::int64_t line_x = 0, line_z = 0;
    std::uint32_t n = 0;
    auto on_line = [&](float c, float o, std::int64_t& line) {
        const float f = (c - o) / g.cell_size;
        if (!(f > -1.0f && f < max_line)) return false;
        const std::int64_t k = static_cast<std::int64_t>(std::lround(f));
        if (n > 0 && k != line) return false;
        line = k;
        return std::fabs(c - (o + static_cast<float>(k) * g.cell_size)) <= coord_tolerance(c, o);
    };
    for_each_vertex([&](float x, float y, float z) {
        top = top && on_surface(g, x, y, z);
        bottom = bottom && std::fabs(y - by) <= coord_tolerance(y, by);
        wall_x = wall_x && on_line(x, g.origin[0], line_x);
        wall_z = wall_z && on_line(z, g.origin[2], line_z);
        n++;
    });
    if (n == 0) return FaceKind::underside;
    if (top) return FaceKind::top;
    return !closed || bottom || wall_x || wall_z ? FaceKind::underside : FaceKind::crater;
}

// The original surface's world y at (x, z) as the terrain shader reads its height map (bilinear).
// HLSL copy: ter_surface_y.
inline float surface_y_bilinear(const GridView& g, float world_x, float world_z)
{
    const float u = (world_x - g.origin[0]) / g.cell_size + 0.5f;
    const float v = (world_z - g.origin[2]) / g.cell_size + 0.5f;
    return g.origin[1] + g.height_min + height01_bilinear(g, u, v) * g.height_range;
}

// Crater faces take the top's light at their x/z, dimmed linearly with depth below the original
// surface down to crater_light_floor at crater_dark_depth. HLSL copy: ter_crater_factor.
inline constexpr float crater_dark_depth = 6.0f;
inline constexpr float crater_light_floor = 0.35f;

inline float crater_light_factor(float depth)
{
    const float t = depth > 0.0f ? std::min(depth / crater_dark_depth, 1.0f) : 0.0f;
    return 1.0f - (1.0f - crater_light_floor) * t;
}

// Two-sided ray/triangle test (Moller-Trumbore); t within [t_lo, t_hi]. Edges are inclusive by a few
// ulps so a ray through an edge shared by two cells hits at least one of them.
inline bool ray_triangle(const float (&o)[3], const float (&d)[3], const float (&a)[3], const float (&b)[3],
                         const float (&c)[3], float t_lo, float t_hi, float& t_out)
{
    const double e1[3] = {static_cast<double>(b[0]) - a[0], static_cast<double>(b[1]) - a[1],
                          static_cast<double>(b[2]) - a[2]};
    const double e2[3] = {static_cast<double>(c[0]) - a[0], static_cast<double>(c[1]) - a[1],
                          static_cast<double>(c[2]) - a[2]};
    const double p[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
    const double det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
    if (std::fabs(det) < 1e-12) return false;
    const double inv = 1.0 / det;
    const double s[3] = {static_cast<double>(o[0]) - a[0], static_cast<double>(o[1]) - a[1],
                         static_cast<double>(o[2]) - a[2]};
    constexpr double edge_eps = 1e-6;
    const double u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
    if (u < -edge_eps || u > 1.0 + edge_eps) return false;
    const double q[3] = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0]};
    const double v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
    if (v < -edge_eps || u + v > 1.0 + edge_eps) return false;
    const double t = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
    if (t < t_lo || t > t_hi) return false;
    t_out = static_cast<float>(t);
    return true;
}

// The two top triangles of cell (x, z), split by its diagonal bit as emit_chunk splits them.
inline void cell_triangles(const GridView& g, std::uint32_t x, std::uint32_t z, float (&tri)[2][3][3])
{
    float a[3], b[3], c[3], d[3];
    grid_position(g, x, z, a);
    grid_position(g, x + 1, z, b);
    grid_position(g, x + 1, z + 1, c);
    grid_position(g, x, z + 1, d);
    const bool diag = get_cell_bit(g.diag, cells(g.nx), x, z);
    const float* order[2][3] = {{a, diag ? d : c, b}, {diag ? b : a, d, c}};
    for (int t = 0; t < 2; t++) {
        for (int k = 0; k < 3; k++) std::memcpy(tri[t][k], order[t][k], sizeof(float) * 3);
    }
}

// The first top face (holes skipped unless ignore_holes, both sides) hit by origin + t * dir for t in
// [t_min, t_max]: a 2D DDA over the cells the ray crosses inside the terrain's box, exact triangle tests per
// cell. No hit for a non-finite origin or direction, or a NaN t range.
inline bool raycast(const GridView& g, const float (&o)[3], const float (&d)[3], float t_min, float t_max,
                    float& t_hit, bool ignore_holes = false)
{
    const std::uint32_t cx = cells(g.nx), cz = cells(g.nz);
    if (cx == 0 || cz == 0) return false;
    for (int i = 0; i < 3; i++) {
        if (!std::isfinite(o[i]) || !std::isfinite(d[i])) return false;
    }
    if (std::isnan(t_min) || std::isnan(t_max)) return false;
    const float lo[3] = {g.origin[0], g.origin[1] + g.height_min, g.origin[2]};
    const float hi[3] = {g.origin[0] + extent(g.nx, g.cell_size), lo[1] + g.height_range,
                         g.origin[2] + extent(g.nz, g.cell_size)};
    float t0 = t_min, t1 = t_max;
    for (int i = 0; i < 3; i++) {
        const float margin = (i == 1 ? g.height_range : g.cell_size) * 1e-3f + 1e-3f;
        const float blo = lo[i] - margin, bhi = hi[i] + margin;
        if (std::fabs(d[i]) < 1e-12f) {
            if (o[i] < blo || o[i] > bhi) return false;
            continue;
        }
        float ta = (blo - o[i]) / d[i], tb = (bhi - o[i]) / d[i];
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
        if (t0 > t1) return false;
    }

    auto cell_of = [&](float t, int axis, std::uint32_t count) {
        const float f = (o[axis] + t * d[axis] - g.origin[axis]) / g.cell_size;
        const float c = std::floor(f);
        return c >= 0.0f ? static_cast<std::int64_t>(std::min(c, static_cast<float>(count - 1))) : 0;
    };
    std::int64_t x = cell_of(t0, 0, cx), z = cell_of(t0, 2, cz);
    const int step_x = d[0] > 0.0f ? 1 : (d[0] < 0.0f ? -1 : 0);
    const int step_z = d[2] > 0.0f ? 1 : (d[2] < 0.0f ? -1 : 0);
    auto next_t = [&](std::int64_t c, int step, int axis) {
        if (step == 0) return INFINITY;
        const float edge = g.origin[axis] + static_cast<float>(c + (step > 0 ? 1 : 0)) * g.cell_size;
        return (edge - o[axis]) / d[axis];
    };
    float tx = next_t(x, step_x, 0), tz = next_t(z, step_z, 2);
    const float dtx = step_x ? g.cell_size / std::fabs(d[0]) : INFINITY;
    const float dtz = step_z ? g.cell_size / std::fabs(d[2]) : INFINITY;

    // A hit may lie a hair outside the cell's own t span when it is on a shared edge.
    const float slack = (t1 - t0) * 1e-5f + 1e-4f;
    for (std::uint32_t guard = 0; guard <= cx + cz + 2; guard++) {
        if (ignore_holes || cell_solid(g, x, z)) {
            float tri[2][3][3];
            cell_triangles(g, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(z), tri);
            float best = INFINITY, t = 0.0f;
            for (const auto& tr : tri) {
                if (ray_triangle(o, d, tr[0], tr[1], tr[2], std::max(t_min, t0 - slack), std::min(t_max, t1 + slack),
                                 t) &&
                    t < best) {
                    best = t;
                }
            }
            if (best != INFINITY) {
                t_hit = best;
                return true;
            }
        }
        if (tx < tz) {
            if (tx > t1) break;
            x += step_x;
            tx += dtx;
        }
        else {
            if (tz > t1 || tz == INFINITY) break;
            z += step_z;
            tz += dtz;
        }
        if (x < 0 || z < 0 || x >= cx || z >= cz) break;
    }
    return false;
}

// ─── Build mapping hash ───────────────────────────────────────────────────────
// A chunk's compiled room is identified by its distinct vertex positions (by bit pattern, -0.0 as +0.0):
// vertex_count of them and pos_hash, the order-independent sum (mod 2^64) of position_hash_term.

struct PositionKey
{
    std::uint32_t x, y, z;

    bool operator==(const PositionKey& o) const { return x == o.x && y == o.y && z == o.z; }
    bool operator<(const PositionKey& o) const
    {
        if (x != o.x) return x < o.x;
        if (y != o.y) return y < o.y;
        return z < o.z;
    }
};

inline constexpr std::uint32_t position_bits(float f)
{
    if (f == 0.0f) f = 0.0f;
    return std::bit_cast<std::uint32_t>(f);
}

inline PositionKey position_key(float x, float y, float z)
{
    return {position_bits(x), position_bits(y), position_bits(z)};
}

inline constexpr std::uint64_t splitmix64(std::uint64_t v)
{
    v += 0x9E3779B97F4A7C15ull;
    v = (v ^ (v >> 30)) * 0xBF58476D1CE4E5B9ull;
    v = (v ^ (v >> 27)) * 0x94D049BB133111EBull;
    return v ^ (v >> 31);
}

// splitmix64(splitmix64(x | y << 32) ^ z)
inline constexpr std::uint64_t position_hash_term(const PositionKey& k)
{
    return splitmix64(splitmix64(static_cast<std::uint64_t>(k.x) | (static_cast<std::uint64_t>(k.y) << 32)) ^
                      static_cast<std::uint64_t>(k.z));
}

// Stored in every build mapping, so pinned.
static_assert(position_hash_term({0x3F800000u, 0xC2280000u, 0x00000000u}) == 0x3E086D6D9F723AD7ull);

// A room's vertex_count and pos_hash from the keys of all its face vertices, duplicates included.
// Sorts `keys` in place.
inline void position_set_hash(PositionKey* keys, std::size_t n, std::uint32_t& vertex_count,
                              std::uint64_t& pos_hash)
{
    std::sort(keys, keys + n);
    const std::size_t distinct = static_cast<std::size_t>(std::unique(keys, keys + n) - keys);
    std::uint64_t hash = 0;
    for (std::size_t i = 0; i < distinct; i++) hash += position_hash_term(keys[i]);
    vertex_count = static_cast<std::uint32_t>(distinct);
    pos_hash = hash;
}

// ─── Decorations ──────────────────────────────────────────────────────────────
// Instances are placed per weight texel from integer hashes of the terrain uid, the decoration's index and
// the texel, so every reader places the same ones whatever chunk grid it walks. The game draws them; RED
// counts and bakes them.

struct DecorationView
{
    const std::uint8_t* coverage; // decoration_plane_bytes
    const char* mesh;
    float density, scale_min, scale_max, max_slope_deg, vertical_offset;
    std::uint32_t link_layer, flags;
};

// `deco` has .mesh (std::string), .density, .scale_min, .scale_max, .max_slope, .vertical_offset, .link_layer
// and the flag bools decoration_flags reads; `plane` is its coverage plane.
template<typename Decoration>
DecorationView make_decoration_view(const Decoration& deco, const std::uint8_t* plane)
{
    return {plane, deco.mesh.c_str(), deco.density, deco.scale_min, deco.scale_max, deco.max_slope,
            deco.vertical_offset, deco.link_layer, decoration_flags(deco)};
}

// Views of the first max_decorations of `decos` over `coverage`, which holds their planes of plane_bytes each in
// list order; one without a plane gets a null coverage. Returns how many.
template<typename Decorations, typename Coverage>
std::uint32_t make_decoration_views(const Decorations& decos, const Coverage& coverage, std::size_t plane_bytes,
                                    DecorationView (&out)[max_decorations])
{
    const auto count = static_cast<std::uint32_t>(std::min<std::size_t>(decos.size(), max_decorations));
    for (std::uint32_t i = 0; i < count; i++) {
        const bool has_plane = coverage.size() >= (i + 1) * plane_bytes;
        out[i] = make_decoration_view(decos[i], has_plane ? coverage.data() + i * plane_bytes : nullptr);
    }
    return count;
}

// Places nothing, casts nothing and hashes nothing when inactive.
inline constexpr bool decoration_active(const DecorationView& d)
{
    return d.coverage && d.mesh && d.mesh[0] && d.density > 0.0f;
}

inline constexpr bool decoration_casts(const DecorationView& d)
{
    return decoration_active(d) && (d.flags & decoration_flag_casts_shadows) != 0;
}

inline constexpr double deg_to_rad = 3.14159265358979 / 180.0;
inline constexpr float two_pi = 6.28318531f;

// Weight texels [x0, x1) x [z0, z1).
struct TexelRect
{
    std::uint32_t x0, z0, x1, z1;
};

inline constexpr TexelRect chunk_texel_rect(const ChunkRect& r, std::uint32_t mul)
{
    return {r.x0 * mul, r.z0 * mul, r.x1 * mul, r.z1 * mul};
}

// One placed mesh: its base on the surface, the surface normal there, its origin (the base raised along uvec
// by vertical_offset * scale) and its unit orientation, scaled by `scale`.
struct DecorationInstance
{
    float base[3], normal[3], pos[3], rvec[3], uvec[3], fvec[3];
    float scale;
};

inline constexpr std::uint64_t decoration_seed(std::int32_t uid, std::uint32_t index)
{
    return splitmix64(splitmix64(static_cast<std::uint32_t>(uid)) ^ (0xDEC0ull << 48 | index));
}

inline constexpr std::uint64_t decoration_texel_key(std::uint64_t seed, std::uint32_t i, std::uint32_t j)
{
    return splitmix64(seed ^ (static_cast<std::uint64_t>(j) << 32 | i));
}

// Where every reader's instances come from, so pinned.
static_assert(decoration_texel_key(decoration_seed(-7, 3), 12, 34) == 0xE541EBED09662C5Full);

// The top 24 bits of `h` as a float in [0, 1).
inline constexpr float hash_u01(std::uint64_t h)
{
    return static_cast<float>(h >> 40) * (1.0f / 16777216.0f);
}

// What is left of a level's placement work, carried across terrains in record order: instances placed, and
// candidates tried whether placed or dropped by the slope limit, so steep ground cannot make it unbounded.
struct DecorationBudget
{
    std::uint32_t instances = max_level_decoration_instances;
    std::uint32_t candidates = max_level_decoration_candidates;

    constexpr bool spent() const
    {
        return instances == 0 || candidates == 0;
    }
};

// Calls fn(const DecorationInstance&) for decoration `d`'s instances on weight texels `r` (clamped to the grid),
// row by row, within `budget`, which it lowers; fn returns false to stop. Returns how many it was given. A
// texel expects density x texel area x coverage (x the linked layer's weight) instances; the fraction is one
// more by chance. Holes, and ground steeper than the slope limit, drop their instances without moving any
// other.
// Its output is baked into decoration shadows: change it only together with decoration_lighting_hash.
template<typename Fn>
std::uint32_t for_each_decoration_instance(const GridView& g, const DecorationView& d, std::uint64_t seed,
                                           TexelRect r, DecorationBudget& budget, Fn&& fn)
{
    const bool linked = d.link_layer != decoration_link_none;
    if (!decoration_active(d) || budget.spent()) return 0;
    if (linked && (!g.weights || d.link_layer >= std::min(g.layer_count, max_layers))) return 0;
    const std::uint32_t mul = g.weight_res_mul;
    const std::uint32_t ww = weight_width(g.nx, mul), wh = weight_height(g.nz, mul);
    r.x1 = std::min(r.x1, ww);
    r.z1 = std::min(r.z1, wh);
    if (mul == 0 || r.x0 >= r.x1 || r.z0 >= r.z1) return 0;

    const double step = static_cast<double>(g.cell_size) / mul;
    const double expected = std::min(static_cast<double>(d.density) * step * step,
                                     static_cast<double>(max_decoration_instances_per_texel));
    const std::uint64_t expected_q16 = static_cast<std::uint64_t>(std::llround(expected * 65536.0));
    const float cos_max = static_cast<float>(std::cos(static_cast<double>(d.max_slope_deg) * deg_to_rad));
    const std::size_t map_bytes = weight_map_bytes(g.nx, g.nz, mul);
    const std::size_t link_base = linked ? layer_weight_offset(d.link_layer, map_bytes) : 0;
    const bool align = (d.flags & decoration_flag_align_to_slope) != 0;
    const bool random_yaw = (d.flags & decoration_flag_random_yaw) != 0;

    std::uint32_t count = 0;
    for (std::uint32_t j = r.z0; j < r.z1; j++) {
        for (std::uint32_t i = r.x0; i < r.x1; i++) {
            const std::size_t t = static_cast<std::size_t>(j) * ww + i;
            const std::uint32_t cov = d.coverage[t];
            if (cov == 0) continue;
            const std::uint32_t weight = linked ? g.weights[link_base + t * 4] : 255u;
            if (weight == 0 || !cell_solid(g, i / mul, j / mul)) continue;
            const std::uint64_t n_q16 = expected_q16 * cov * weight / 65025u;
            std::uint32_t n = static_cast<std::uint32_t>(n_q16 >> 16);
            const std::uint64_t key = decoration_texel_key(seed, i, j);
            if ((splitmix64(key ^ 0xF4AC7105ull) & 0xFFFFu) < (n_q16 & 0xFFFFu)) n++;

            for (std::uint32_t k = 0; k < n; k++) {
                if (budget.candidates == 0) return count;
                budget.candidates--;
                const std::uint64_t s = splitmix64(key + (k + 1ull) * 0x9E3779B97F4A7C15ull);
                const std::uint64_t r1 = splitmix64(s), r2 = splitmix64(r1), r3 = splitmix64(r2), r4 = splitmix64(r3);
                DecorationInstance inst;
                inst.base[0] = g.origin[0] + static_cast<float>((i + static_cast<double>(hash_u01(r1))) * step);
                inst.base[2] = g.origin[2] + static_cast<float>((j + static_cast<double>(hash_u01(r2))) * step);
                inst.base[1] = height_at(g, inst.base[0], inst.base[2]);
                heightmap_normal(g, inst.base[0], inst.base[2], inst.normal);
                if (!(inst.normal[1] >= cos_max)) continue;
                const float yaw = random_yaw ? hash_u01(r3) * two_pi : 0.0f;
                inst.scale = d.scale_min + (d.scale_max - d.scale_min) * hash_u01(r4);

                const float up[3] = {align ? inst.normal[0] : 0.0f, align ? inst.normal[1] : 1.0f,
                                     align ? inst.normal[2] : 0.0f};
                const float sy = std::sin(yaw), cy = std::cos(yaw);
                // rvec = up x (sin yaw, 0, cos yaw), or up x (cos yaw, 0, -sin yaw) where up lies along the first
                float rv[3] = {up[1] * cy, up[2] * sy - up[0] * cy, -up[1] * sy};
                float len = std::sqrt(rv[0] * rv[0] + rv[1] * rv[1] + rv[2] * rv[2]);
                if (!(len >= 1e-6f)) {
                    rv[0] = -up[1] * sy;
                    rv[1] = up[2] * cy + up[0] * sy;
                    rv[2] = -up[1] * cy;
                    len = std::sqrt(rv[0] * rv[0] + rv[1] * rv[1] + rv[2] * rv[2]);
                }
                const float lift = d.vertical_offset * inst.scale;
                for (int c = 0; c < 3; c++) {
                    inst.rvec[c] = rv[c] / len;
                    inst.uvec[c] = up[c];
                    inst.pos[c] = inst.base[c] + up[c] * lift;
                }
                inst.fvec[0] = inst.rvec[1] * up[2] - inst.rvec[2] * up[1];
                inst.fvec[1] = inst.rvec[2] * up[0] - inst.rvec[0] * up[2];
                inst.fvec[2] = inst.rvec[0] * up[1] - inst.rvec[1] * up[0];
                count++;
                budget.instances--;
                if (!fn(static_cast<const DecorationInstance&>(inst)) || budget.instances == 0) return count;
            }
        }
    }
    return count;
}

// Every decoration instance of one terrain in the order the game places them: chunk by chunk of `layout`,
// within a chunk decoration by decoration, within `budget` (the level's, carried across terrains in record
// order), which it lowers. fn(chunk, decoration, const DecorationInstance&) returns false to stop. Returns
// how many it was given.
template<typename Fn>
std::uint32_t for_each_terrain_decoration(const GridView& g, std::int32_t uid, const ChunkLayout& layout,
                                          const DecorationView* decos, std::uint32_t count,
                                          DecorationBudget& budget, Fn&& fn)
{
    count = std::min(count, max_decorations);
    std::uint64_t seeds[max_decorations];
    for (std::uint32_t k = 0; k < count; k++) seeds[k] = decoration_seed(uid, k);
    const std::uint32_t chunks = layout_chunk_count(layout);
    std::uint32_t visited = 0;
    bool stop = false;
    for (std::uint32_t c = 0; c < chunks && !stop && !budget.spent(); c++) {
        const TexelRect r =
            chunk_texel_rect(chunk_rect(layout.cells_x, layout.cells_z, layout.edge, c), g.weight_res_mul);
        for (std::uint32_t k = 0; k < count && !stop && !budget.spent(); k++) {
            visited += for_each_decoration_instance(g, decos[k], seeds[k], r, budget,
                                                    [&](const DecorationInstance& inst) {
                                                        stop = !fn(c, k, inst);
                                                        return !stop;
                                                    });
        }
    }
    return visited;
}

// ─── Lighting fingerprint ─────────────────────────────────────────────────────
// What a terrain's baked chart (TerrainChart::geometry_fingerprint) depends on: placement, heights, holes,
// triangulation and shape. Painting keeps the chart; geometry edits drop it.

// Mixes the byte length of p[0, count), then its bytes as little-endian 64-bit words, the last one
// zero-padded.
template<typename Mix, typename T>
constexpr void mix_le_words(Mix&& mix, const T* p, std::size_t count)
{
    constexpr std::size_t per = 8 / sizeof(T), bits = 8 * sizeof(T);
    mix(count * sizeof(T));
    std::size_t i = 0;
    for (; i + per <= count; i += per) {
        std::uint64_t w = 0;
        for (std::size_t k = 0; k < per; k++) w |= static_cast<std::uint64_t>(p[i + k]) << (bits * k);
        mix(w);
    }
    std::uint64_t tail = 0;
    for (std::size_t k = 0; i + k < count; k++) tail |= static_cast<std::uint64_t>(p[i + k]) << (bits * k);
    mix(tail);
}

// Its output must never change for an existing record: a new input is mixed in only when non-default.
inline constexpr std::uint64_t lighting_fingerprint(const GridView& g)
{
    std::uint64_t h = 0xcbf29ce484222325ull;
    auto mix = [&h](std::uint64_t v) { h = splitmix64(h ^ v); };
    for (float c : g.origin) mix(position_bits(c));
    mix(position_bits(g.cell_size));
    mix(position_bits(g.height_min));
    mix(position_bits(g.height_range));
    mix(g.nx);
    mix(g.nz);
    mix(g.flags);
    mix(position_bits(g.thickness));
    mix(position_bits(g.skirt_depth));
    const std::size_t mask = bitmask_bytes(cells(g.nx), cells(g.nz));
    mix_le_words(mix, g.heights, vertex_count(g.nx, g.nz));
    mix_le_words(mix, g.holes, mask);
    mix_le_words(mix, g.diag, mask);
    return h;
}

// Stored in every baked terrain chart, so pinned.
static_assert([] {
    const std::uint16_t heights[9] = {0, 1, 0xFFFF, 0x1234, 0x8000, 0xABCD, 7, 0x0100, 0x7FFF};
    const std::uint8_t holes[1] = {0x04}, diag[1] = {0x09};
    const GridView g{heights, nullptr, holes, diag, 3, 3, 1, {-12.5f, -0.0f, 1024.0f}, 2.0f, -3.0f, 64.0f,
                     flag_geoable, 16.0f, 8.0f, 1, 0, {}};
    return lighting_fingerprint(g);
}() == 0xEE420A033E6AF829ull);

// What a terrain's baked light depends on beyond lighting_fingerprint: the decorations that cast shadows,
// by everything that places them (draw_distance does not). 0 when none casts, so terrains without one keep
// their charts. A linked layer's weights must be present.
inline constexpr std::uint64_t decoration_lighting_hash(std::int32_t uid, const GridView& g, const DecorationView* d,
                                                        std::uint32_t count)
{
    count = std::min(count, max_decorations);
    bool any = false;
    for (std::uint32_t i = 0; i < count; i++) any = any || decoration_casts(d[i]);
    if (!any) return 0;

    std::uint64_t h = 0x6A09E667F3BCC908ull;
    auto mix = [&h](std::uint64_t v) { h = splitmix64(h ^ v); };
    // Bytes as material_fingerprint mixes them: the length, then little-endian words, the last zero-padded.
    auto bytes = [&mix](std::size_t n, auto&& byte_at) {
        mix(n);
        std::uint64_t w = 0;
        std::size_t k = 0;
        for (std::size_t i = 0; i < n; i++) {
            w |= static_cast<std::uint64_t>(byte_at(i)) << (8 * k);
            if (++k == 8) {
                mix(w);
                w = 0;
                k = 0;
            }
        }
        mix(w);
    };
    const std::size_t texels = weight_texel_count(g.nx, g.nz, g.weight_res_mul);
    const std::size_t map_bytes = weight_map_bytes(g.nx, g.nz, g.weight_res_mul);
    mix(static_cast<std::uint32_t>(uid));
    mix(g.weight_res_mul);
    for (std::uint32_t i = 0; i < count; i++) {
        const DecorationView& v = d[i];
        if (!decoration_casts(v)) continue;
        mix(i);
        std::size_t len = 0;
        while (v.mesh[len]) len++;
        bytes(len, [&](std::size_t k) { return static_cast<std::uint8_t>(ascii_lower(v.mesh[k])); });
        mix(position_bits(v.density));
        mix(position_bits(v.scale_min));
        mix(position_bits(v.scale_max));
        mix(position_bits(v.max_slope_deg));
        mix(position_bits(v.vertical_offset));
        mix(v.flags);
        mix(v.link_layer);
        mix_le_words(mix, v.coverage, texels);
        if (v.link_layer != decoration_link_none && g.weights && v.link_layer < max_layers) {
            const std::size_t base = layer_weight_offset(v.link_layer, map_bytes);
            bytes(texels, [&](std::size_t t) { return g.weights[base + t * 4]; });
        }
    }
    return h == 0 ? 1 : h;
}

// The fingerprint a baked terrain chart stores: lighting_fingerprint, mixed with decoration_lighting_hash when
// a decoration casts shadows.
inline constexpr std::uint64_t chart_fingerprint(const GridView& g, std::uint64_t decoration_hash)
{
    return decoration_hash == 0 ? lighting_fingerprint(g) : splitmix64(lighting_fingerprint(g) ^ decoration_hash);
}

// Stored in every baked terrain chart, so pinned.
static_assert([] {
    const std::uint16_t heights[9] = {0, 1, 0xFFFF, 0x1234, 0x8000, 0xABCD, 7, 0x0100, 0x7FFF};
    const std::uint8_t holes[1] = {0x04}, diag[1] = {0x09}, plane[4] = {255, 0, 17, 128};
    const GridView g{heights, nullptr, holes, diag, 3, 3, 1, {-12.5f, -0.0f, 1024.0f}, 2.0f, -3.0f, 64.0f,
                     flag_geoable, 16.0f, 8.0f, 1, 0, {}};
    const DecorationView d{plane, "rock.v3m", 0.5f, 0.75f, 1.5f, 35.0f, 0.5f, decoration_link_none,
                           decoration_flag_random_yaw | decoration_flag_casts_shadows};
    return chart_fingerprint(g, decoration_lighting_hash(-7, g, &d, 1));
}() == 0x2BD621293A0026F6ull);

// ─── Build fingerprints ───────────────────────────────────────────────────────
// Geometry: every input of the compiled positions and chunk -> room layout (the build mapping is valid
// while it matches). Material: face textures and UVs only; a material-only change keeps the mapping.
// Overlays are in neither: the compiled faces never carry them, and the game reads them from the chunk.

inline std::uint64_t geometry_fingerprint(const GridView& g, std::uint32_t chunk_cells)
{
    return splitmix64(lighting_fingerprint(g) ^
                      effective_chunk_cells(cells(g.nx), cells(g.nz), chunk_cells, g.flags));
}

// layer_textures[i] names layer i for i < g.layer_count; null or empty names count as untextured.
inline std::uint64_t material_fingerprint(const GridView& g, const char* const* layer_textures,
                                          const char* underside_texture)
{
    std::uint64_t h = 0x84222325cbf29ce4ull;
    auto mix = [&h](std::uint64_t v) { h = splitmix64(h ^ v); };
    auto bytes = [&](const std::uint8_t* p, std::size_t n) {
        mix(n);
        std::size_t i = 0;
        for (; i + 8 <= n; i += 8) {
            std::uint64_t w;
            std::memcpy(&w, p + i, 8);
            mix(w);
        }
        std::uint64_t tail = 0;
        if (n > i) std::memcpy(&tail, p + i, n - i);
        mix(tail);
    };
    auto str = [&](const char* s) { bytes(reinterpret_cast<const std::uint8_t*>(s ? s : ""), s ? std::strlen(s) : 0); };
    const std::uint32_t n = std::min(g.layer_count, max_layers);
    mix(g.weight_res_mul);
    mix(n);
    mix(g.textured_layers);
    for (std::uint32_t i = 0; i < max_layers; i++) mix(position_bits(g.layer_uv_scale[i]));
    for (std::uint32_t i = 0; i < n; i++) str(layer_textures[i]);
    str(underside_texture);
    bytes(g.weights, weight_map_bytes(g.nx, g.nz, g.weight_res_mul) * 2);
    return h;
}

} // namespace alpine_terrain
