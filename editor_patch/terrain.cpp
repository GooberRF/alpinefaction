#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <algorithm>
#include <chrono>
#include <format>
#include <memory>
#include <new>
#include <random>
#include <span>
#include <vector>
#include <zlib.h>
#include <stb_image.h>
#include <common/rfl_chunk_reader.h>
#include <common/terrain/alpine_terrain.h>
#include <common/terrain/alpine_terrain_reader.h>
#include <common/utils/string-utils.h>
#include <xlog/xlog.h>
#include "alpine_spinner.h"
#include "brush_import.h"
#include "dialog_tooltips.h"
#include "file_dialogs.h"
#include "headless_bake.h"
#include "mesh_browser.h"
#include "terrain.h"
#include "terrain_build.h"
#include "terrain_decorations.h"
#include "terrain_generate.h"
#include "terrain_paint.h"
#include "terrain_paint_math.h"
#include "terrain_preview.h"
#include "level.h"
#include "resources.h"
#include "vtypes.h"
#include "alpine_obj.h"
#include "textures.h"

namespace at = alpine_terrain;

// ─── Globals ─────────────────────────────────────────────────────────────────

static int g_terrain_icon_handle = -1;
static std::vector<DedTerrain*> g_terrain_clipboard;

// The level's texture name limit, which the terrain format's own must match.
static_assert(at::max_texture_name_len == MAX_TEXTURE_NAME_LEN);

void terrain_report(const std::string& msg, bool popup)
{
    editor_report(EditorReportLevel::warn, "Terrain", msg, true);
    if (popup && !headless_bake_active()) show_error_message(msg.c_str());
}

static void terrain_load_icon()
{
    if (g_terrain_icon_handle < 0) {
        g_terrain_icon_handle = bm_load("Icon_AFTerrain.tga", -1, 1);
    }
}

// ─── Grid helpers ────────────────────────────────────────────────────────────

static std::shared_ptr<TerrainGrid> terrain_make_flat_grid(uint32_t nx, uint32_t nz, uint32_t mul)
{
    auto g = std::make_shared<TerrainGrid>();
    g->nx = nx;
    g->nz = nz;
    g->weight_res_mul = mul;
    g->heights.assign(at::vertex_count(nx, nz), 0);
    const std::size_t map = at::weight_map_bytes(nx, nz, mul);
    g->weights.assign(map * 2, 0);
    for (std::size_t i = 0; i < map; i += 4) {
        g->weights[i] = 255;
    }
    const std::size_t mask = at::bitmask_bytes(at::cells(nx), at::cells(nz));
    g->holes.assign(mask, 0);
    g->diag.assign(mask, 0);
    return g;
}

static bool terrain_grid_consistent(const TerrainGrid& g)
{
    if (g.nx < at::min_verts || g.nx > at::max_verts || g.nz < at::min_verts || g.nz > at::max_verts) {
        return false;
    }
    if (!at::is_allowed_weight_res_mul(g.weight_res_mul)) return false;
    const std::size_t mask = at::bitmask_bytes(at::cells(g.nx), at::cells(g.nz));
    const std::size_t plane = at::decoration_plane_bytes(g.nx, g.nz, g.weight_res_mul);
    return g.heights.size() == at::vertex_count(g.nx, g.nz) &&
           g.weights.size() == at::blob_weights_bytes(g.nx, g.nz, g.weight_res_mul) &&
           g.holes.size() == mask && g.diag.size() == mask &&
           (g.overlay.empty() || g.overlay.size() == at::overlay_map_bytes(g.nx, g.nz, g.weight_res_mul)) &&
           g.decoration.size() % plane == 0 && g.decoration.size() / plane <= at::max_decorations;
}

// The grid carries a coverage map exactly while the terrain has overlays; a new one starts at zero.
static void terrain_match_overlay_map(DedTerrainData& d)
{
    if (!d.grid || d.overlays.empty() == d.grid->overlay.empty()) return;
    auto g = std::make_shared<TerrainGrid>(*d.grid);
    if (d.overlays.empty()) {
        g->overlay.clear();
    }
    else {
        g->overlay.assign(at::overlay_map_bytes(g->nx, g->nz, g->weight_res_mul), 0);
    }
    d.grid = std::move(g);
}

// The grid carries one coverage plane per decoration; a new one starts full when its decoration follows a
// texture layer, so it appears where that layer is painted, else empty.
static void terrain_match_decoration_planes(DedTerrainData& d)
{
    if (!d.grid) return;
    const std::size_t plane = at::decoration_plane_bytes(d.grid->nx, d.grid->nz, d.grid->weight_res_mul);
    const std::size_t have = terrain_decoration_plane_count(*d.grid);
    if (have == d.decorations.size() && d.grid->decoration.size() % plane == 0) return;
    auto g = std::make_shared<TerrainGrid>(*d.grid);
    g->decoration.resize(std::min(have, d.decorations.size()) * plane);
    for (std::size_t i = g->decoration.size() / plane; i < d.decorations.size(); i++) {
        const bool linked = d.decorations[i].link_layer != at::decoration_link_none;
        g->decoration.insert(g->decoration.end(), plane, linked ? at::decoration_coverage_full : uint8_t{0});
    }
    d.grid = std::move(g);
}

static std::size_t terrain_texel_count(const TerrainGrid& g)
{
    return at::weight_texel_count(g.nx, g.nz, g.weight_res_mul);
}

static void terrain_get_weights(const TerrainGrid& g, std::size_t texel, uint8_t (&w)[at::max_layers])
{
    at::texel_weights(g.weights.data(), at::weight_map_bytes(g.nx, g.nz, g.weight_res_mul), texel, w);
}

static void terrain_set_weights(TerrainGrid& g, std::size_t texel, const uint8_t (&w)[at::max_layers])
{
    at::set_texel_weights(g.weights.data(), at::weight_map_bytes(g.nx, g.nz, g.weight_res_mul), texel, w);
}

// Per destination sample, the source taps on one axis and weights summing to 1: bilinear when upsampling,
// else a box. `lattice` maps end onto end (vertices); otherwise sample centres spread evenly.
struct TerrainAxisTaps
{
    std::vector<uint32_t> start; // destination i uses [start[i], start[i + 1])
    std::vector<uint32_t> index;
    std::vector<float> weight;

    uint32_t count() const { return static_cast<uint32_t>(start.size() - 1); }
};

static TerrainAxisTaps terrain_axis_taps(uint32_t sn, uint32_t dn, bool lattice)
{
    TerrainAxisTaps t;
    t.start.reserve(dn + 1);
    const double step = lattice ? static_cast<double>(sn - 1) / (dn - 1) : static_cast<double>(sn) / dn;
    const double last = static_cast<double>(sn - 1);
    for (uint32_t i = 0; i < dn; i++) {
        const auto first = static_cast<uint32_t>(t.index.size());
        t.start.push_back(first);
        const double c = lattice ? i * step : (i + 0.5) * step - 0.5;
        if (step > 1.0) {
            const double lo = c - step * 0.5, hi = c + step * 0.5;
            const auto k0 = static_cast<uint32_t>(std::clamp(std::floor(lo + 0.5), 0.0, last));
            const auto k1 = static_cast<uint32_t>(std::clamp(std::floor(hi + 0.5), 0.0, last));
            for (uint32_t k = k0; k <= k1; k++) {
                const double overlap = std::min(hi, k + 0.5) - std::max(lo, k - 0.5);
                if (overlap > 0.0) {
                    t.index.push_back(k);
                    t.weight.push_back(static_cast<float>(overlap));
                }
            }
        }
        else {
            const double u = std::clamp(c, 0.0, last);
            const auto k0 = std::min(static_cast<uint32_t>(u), sn - 1);
            const float f = static_cast<float>(u - k0);
            t.index.push_back(k0);
            t.weight.push_back(1.0f - f);
            if (f > 0.0f && k0 + 1 < sn) {
                t.index.push_back(k0 + 1);
                t.weight.push_back(f);
            }
        }
        float sum = 0.0f;
        for (std::size_t k = first; k < t.weight.size(); k++) sum += t.weight[k];
        for (std::size_t k = first; k < t.weight.size(); k++) t.weight[k] = sum > 0.0f ? t.weight[k] / sum : 0.0f;
    }
    t.start.push_back(static_cast<uint32_t>(t.index.size()));
    return t;
}

// Separable filter of a source with `channels` values per sample: src(col, row, ch) reads the source,
// dst(i, j, values) receives each destination sample.
template<typename Src, typename Dst>
static void terrain_filter(const TerrainAxisTaps& tx, const TerrainAxisTaps& tz, uint32_t src_width,
                           uint32_t channels, Src&& src, Dst&& dst)
{
    std::vector<float> row(static_cast<std::size_t>(src_width) * channels);
    float out[at::max_layers];
    for (uint32_t j = 0; j < tz.count(); j++) {
        std::fill(row.begin(), row.end(), 0.0f);
        for (uint32_t a = tz.start[j]; a < tz.start[j + 1]; a++) {
            const uint32_t sr = tz.index[a];
            const float wz = tz.weight[a];
            float* r = row.data();
            for (uint32_t c = 0; c < src_width; c++) {
                for (uint32_t ch = 0; ch < channels; ch++) *r++ += wz * src(c, sr, ch);
            }
        }
        for (uint32_t i = 0; i < tx.count(); i++) {
            std::fill(out, out + channels, 0.0f);
            for (uint32_t b = tx.start[i]; b < tx.start[i + 1]; b++) {
                const float* s = row.data() + static_cast<std::size_t>(tx.index[b]) * channels;
                for (uint32_t ch = 0; ch < channels; ch++) out[ch] += tx.weight[b] * s[ch];
            }
            dst(i, j, out);
        }
    }
}

static uint8_t terrain_weight_byte(float v)
{
    return static_cast<uint8_t>(std::clamp(std::lround(v), 0L, 255L));
}

// Heights filtered on the vertex lattice, weights over texel centres and renormalized, overlay coverage and
// decoration planes as the weights without renormalizing, holes and diagonals nearest by cell centre.
static std::shared_ptr<TerrainGrid> terrain_resample_grid(const TerrainGrid& src, uint32_t nx,
                                                          uint32_t nz, uint32_t mul)
{
    auto g = std::make_shared<TerrainGrid>();
    g->nx = nx;
    g->nz = nz;
    g->weight_res_mul = mul;

    g->heights.resize(at::vertex_count(nx, nz));
    terrain_filter(
        terrain_axis_taps(src.nx, nx, true), terrain_axis_taps(src.nz, nz, true), src.nx, 1,
        [&](uint32_t c, uint32_t r, uint32_t) {
            return static_cast<float>(src.heights[static_cast<std::size_t>(r) * src.nx + c]);
        },
        [&](uint32_t i, uint32_t j, const float* v) {
            g->heights[static_cast<std::size_t>(j) * nx + i] = at::encode_height01(v[0] / 65535.0f);
        });

    const uint32_t sw = at::weight_width(src.nx, src.weight_res_mul);
    const uint32_t sh = at::weight_height(src.nz, src.weight_res_mul);
    const uint32_t dw = at::weight_width(nx, mul);
    const uint32_t dh = at::weight_height(nz, mul);
    const std::size_t src_map = at::weight_map_bytes(src.nx, src.nz, src.weight_res_mul);
    g->weights.assign(at::blob_weights_bytes(nx, nz, mul), 0);
    terrain_filter(
        terrain_axis_taps(sw, dw, false), terrain_axis_taps(sh, dh, false), sw, at::max_layers,
        [&](uint32_t c, uint32_t r, uint32_t ch) {
            const std::size_t texel = static_cast<std::size_t>(r) * sw + c;
            return static_cast<float>(src.weights[(ch < 4 ? 0 : src_map) + texel * 4 + (ch & 3)]);
        },
        [&](uint32_t i, uint32_t j, const float* v) {
            uint8_t w[at::max_layers];
            for (std::size_t c = 0; c < at::max_layers; c++) w[c] = terrain_weight_byte(v[c]);
            at::normalize_weights(w);
            terrain_set_weights(*g, static_cast<std::size_t>(j) * dw + i, w);
        });

    if (!src.overlay.empty()) {
        g->overlay.assign(at::overlay_map_bytes(nx, nz, mul), 0);
        terrain_filter(
            terrain_axis_taps(sw, dw, false), terrain_axis_taps(sh, dh, false), sw, 4,
            [&](uint32_t c, uint32_t r, uint32_t ch) {
                return static_cast<float>(src.overlay[(static_cast<std::size_t>(r) * sw + c) * 4 + ch]);
            },
            [&](uint32_t i, uint32_t j, const float* v) {
                for (uint32_t c = 0; c < 4; c++) {
                    g->overlay[(static_cast<std::size_t>(j) * dw + i) * 4 + c] = terrain_weight_byte(v[c]);
                }
            });
    }

    const std::size_t planes = terrain_decoration_plane_count(src);
    if (planes > 0) {
        g->decoration.assign(planes * at::decoration_plane_bytes(nx, nz, mul), 0);
        const TerrainAxisTaps tx = terrain_axis_taps(sw, dw, false), tz = terrain_axis_taps(sh, dh, false);
        for (std::size_t p = 0; p < planes; p++) {
            const uint8_t* in = terrain_decoration_plane(src, p).data();
            uint8_t* out = terrain_decoration_plane(*g, p).data();
            terrain_filter(
                tx, tz, sw, 1,
                [&](uint32_t c, uint32_t r, uint32_t) {
                    return static_cast<float>(in[static_cast<std::size_t>(r) * sw + c]);
                },
                [&](uint32_t i, uint32_t j, const float* v) {
                    out[static_cast<std::size_t>(j) * dw + i] = terrain_weight_byte(v[0]);
                });
        }
    }

    const uint32_t scx = at::cells(src.nx), scz = at::cells(src.nz);
    const uint32_t dcx = at::cells(nx), dcz = at::cells(nz);
    const std::size_t mask = at::bitmask_bytes(dcx, dcz);
    g->holes.assign(mask, 0);
    g->diag.assign(mask, 0);
    for (uint32_t z = 0; z < dcz; z++) {
        const uint32_t sz = std::min(static_cast<uint32_t>((z + 0.5f) * scz / dcz), scz - 1);
        for (uint32_t x = 0; x < dcx; x++) {
            const uint32_t sx = std::min(static_cast<uint32_t>((x + 0.5f) * scx / dcx), scx - 1);
            at::set_cell_bit(g->holes.data(), dcx, x, z, at::get_cell_bit(src.holes.data(), scx, sx, sz));
            at::set_cell_bit(g->diag.data(), dcx, x, z, at::get_cell_bit(src.diag.data(), scx, sx, sz));
        }
    }
    return g;
}

// A copy of `old` at nx x nz vertices, resampled only when that changes its resolution.
static std::shared_ptr<TerrainGrid> terrain_grid_at_resolution(const TerrainGrid& old, uint32_t nx, uint32_t nz)
{
    return nx == old.nx && nz == old.nz ? std::make_shared<TerrainGrid>(old)
                                        : terrain_resample_grid(old, nx, nz, old.weight_res_mul);
}

// Every weight channel up to `highest` gets a layer behind it.
static void terrain_grow_layers(DedTerrainData& d, int highest)
{
    while (static_cast<int>(d.layers.size()) <= highest) d.layers.emplace_back();
}

// ─── Budget ──────────────────────────────────────────────────────────────────

static at::Header terrain_wire_header(const Vector3& pos, const DedTerrainData& d, std::vector<uint8_t>& geo);

static uint64_t terrain_data_raw_bytes(const DedTerrainData& d)
{
    std::vector<uint8_t> geo;
    return d.grid ? at::header_raw_size(terrain_wire_header({}, d, geo)) : 0;
}

static uint64_t terrain_level_raw_bytes_except(CDedLevel* level, const DedTerrain* except)
{
    uint64_t total = 0;
    if (!level) return total;
    for (auto* t : level->GetAlpineLevelProperties().terrain_objects) {
        if (t != except) total += terrain_data_raw_bytes(t->data);
    }
    return total;
}

// The game drops the whole terrain chunk past this budget, so RED never lets a level reach it.
static bool terrain_budget_allows(HWND owner, const DedTerrain* except, uint32_t nx, uint32_t nz,
                                  uint32_t mul, bool overlays, std::size_t decorations)
{
    // Whether the geo mask is written depends on the edit's chunk layout, so it is always counted, at its
    // largest (the finest chunk grid).
    const auto deco_count = static_cast<uint32_t>(std::min<std::size_t>(decorations, at::max_decorations));
    const uint32_t flags = at::flag_chunk_geo_mask | (overlays ? at::flag_overlays : 0) |
                           (deco_count ? at::flag_decorations : 0);
    const uint64_t total = terrain_level_raw_bytes_except(CDedLevel::Get(), except) +
                           at::wire_raw_size(nx, nz, mul, at::chunk_edge_options[0], flags, deco_count);
    if (total <= at::max_level_raw_bytes) return true;
    char msg[256];
    std::snprintf(msg, sizeof(msg),
                  "This would bring the level's terrain data to %.1f MB, over the %.0f MB limit.\n"
                  "Lower the resolution or the weight map resolution.",
                  static_cast<double>(total) / (1024.0 * 1024.0),
                  static_cast<double>(at::max_level_raw_bytes) / (1024.0 * 1024.0));
    MessageBoxA(owner, msg, "Terrain", MB_OK | MB_ICONWARNING);
    return false;
}

// ─── Property sanitising ─────────────────────────────────────────────────────

static void terrain_sanitize_texture(std::string& name, const char* fallback)
{
    if (!at::texture_name_valid(name.c_str(), name.size())) name = fallback;
}

// Clamps every field into the ranges validate_header accepts, so the writer can never emit a record
// the readers reject.
static void terrain_clamp_properties(DedTerrainData& d)
{
    d.cell_size = at::clamp_finite(d.cell_size, at::min_cell_size, at::max_cell_size, at::default_cell_size);
    d.height_min = at::clamp_finite(d.height_min, -at::max_coord, at::max_coord, 0.0f);
    d.height_range = at::clamp_finite(d.height_range, at::min_height_range, at::max_height_range,
                            at::default_height_range);
    if (!at::is_allowed_chunk_cells(d.chunk_cells)) d.chunk_cells = at::default_chunk_cells;
    d.lightmap_density = std::clamp(d.lightmap_density, at::lightmap_density_min, at::lightmap_density_max);
    d.flags &= at::flag_mask;
    d.thickness = at::clamp_finite(d.thickness, at::min_thickness, at::max_thickness, at::default_thickness);
    d.skirt_depth = at::clamp_finite(d.skirt_depth, 0.0f, at::max_skirt_depth, at::default_skirt_depth);
    terrain_sanitize_texture(d.underside_texture, at::default_layer_texture);
    terrain_sanitize_texture(d.crater_texture, "");
    if (d.layers.empty()) d.layers.emplace_back();
    if (d.layers.size() > at::max_layers) d.layers.resize(at::max_layers);
    for (auto& layer : d.layers) {
        terrain_sanitize_texture(layer.texture, at::default_layer_texture);
        layer.uv_scale = at::clamp_finite(layer.uv_scale, at::min_uv_scale, at::max_uv_scale, at::default_uv_scale);
    }
    if (d.overlays.size() > at::max_overlays) d.overlays.resize(at::max_overlays);
    for (auto& overlay : d.overlays) {
        terrain_sanitize_texture(overlay.texture, "");
        overlay.uv_scale = at::clamp_finite(overlay.uv_scale, at::min_uv_scale, at::max_uv_scale, at::default_uv_scale);
    }
    if (d.decorations.size() > at::max_decorations) d.decorations.resize(at::max_decorations);
    for (auto& deco : d.decorations) {
        if (!at::decoration_mesh_valid(deco.mesh.c_str(), deco.mesh.size())) deco.mesh.clear();
        deco.density = at::clamp_finite(deco.density, 0.0f, at::max_decoration_density, at::default_decoration_density);
        deco.scale_min = at::clamp_finite(deco.scale_min, at::min_decoration_scale, at::max_decoration_scale,
                                          at::default_decoration_scale);
        deco.scale_max = at::clamp_finite(deco.scale_max, at::min_decoration_scale, at::max_decoration_scale,
                                          at::default_decoration_scale);
        deco.scale_max = std::max(deco.scale_max, deco.scale_min);
        deco.max_slope = at::clamp_finite(deco.max_slope, 0.0f, at::max_decoration_slope_deg,
                                          at::default_decoration_slope_deg);
        deco.draw_distance = at::clamp_finite(deco.draw_distance, at::min_decoration_draw_distance,
                                              at::max_decoration_draw_distance, at::default_decoration_draw_distance);
        deco.vertical_offset =
            at::clamp_finite(deco.vertical_offset, -at::max_decoration_offset, at::max_decoration_offset, 0.0f);
        if (deco.link_layer >= d.layers.size()) deco.link_layer = at::decoration_link_none;
    }
    if (!d.grid || !terrain_grid_consistent(*d.grid)) {
        d.grid = terrain_make_flat_grid(at::default_verts, at::default_verts, at::default_weight_res_mul);
    }
    terrain_match_overlay_map(d);
    terrain_match_decoration_planes(d);
}

// The header as written: flag_chunk_geo_mask added when a chunk is not geoable, `geo` then the mask,
// flag_overlays when the terrain has overlays and flag_decorations when it has decorations.
static at::Header terrain_wire_header(const Vector3& pos, const DedTerrainData& d, std::vector<uint8_t>& geo)
{
    at::Header h = terrain_header(pos, d, d.grid.get());
    geo.clear();
    if ((d.flags & at::flag_geoable) && d.grid) {
        geo = terrain_geo_chunks(d);
        if (!at::chunk_mask_full(geo.data(), at::header_chunk_count(h))) h.flags |= at::flag_chunk_geo_mask;
    }
    if (!d.overlays.empty()) {
        h.flags |= at::flag_overlays;
        h.overlay_count = static_cast<uint32_t>(d.overlays.size());
    }
    if (!d.decorations.empty()) {
        h.flags |= at::flag_decorations;
        h.decoration_count = static_cast<uint32_t>(d.decorations.size());
    }
    return h;
}

void terrain_prepare(DedTerrain& terrain)
{
    terrain_clamp_properties(terrain.data);
    terrain.orient = identity_orient;
}

// ─── Cleanup ─────────────────────────────────────────────────────────────────

void DestroyDedTerrain(DedTerrain* terrain)
{
    if (!terrain) return;
    terrain_paint_forget(terrain);
    terrain_preview_forget(terrain);
    terrain_decorations_forget(terrain);
    terrain->field_4.free();
    terrain->script_name.free();
    terrain->class_name.free();
    delete terrain;
}

static DedTerrain* terrain_alloc()
{
    auto* terrain = new DedTerrain();
    memset(static_cast<DedObject*>(terrain), 0, sizeof(DedObject));
    terrain->vtbl = reinterpret_cast<void*>(ded_object_vtbl_addr);
    terrain->type = DedObjectType::DED_TERRAIN;
    terrain->orient = identity_orient;
    return terrain;
}

// ─── Serialization ──────────────────────────────────────────────────────────

// Every problem goes to the log; outside a headless bake or an autosave they also share one message box.
// A terrain that was never built is in the box only the first time this session.
static void terrain_report_all(const std::vector<std::string>& problems,
                               const std::vector<std::pair<std::string, DedTerrain*>>& unbuilt = {})
{
    std::string all;
    for (const std::string& p : problems) {
        terrain_report(p, false);
        all += p + "\n";
    }
    for (const auto& [text, terrain] : unbuilt) {
        terrain_report(text, false);
        if (!terrain->unbuilt_save_warned) all += text + "\n";
    }
    if (all.empty() || headless_bake_active() || level_autosave_in_progress()) return;
    for (const auto& entry : unbuilt) entry.second->unbuilt_save_warned = true;
    show_error_message(all.c_str());
}

void terrain_serialize_chunk(CDedLevel& level, rf::File& file, bool group)
{
    auto& terrains = level.GetAlpineLevelProperties().terrain_objects;
    std::vector<std::string> problems;
    std::vector<std::pair<std::string, DedTerrain*>> unbuilt;
    if (!group) {
        try {
            terrain_build_strip_leftovers(level);
            if (std::string orphans = terrain_build_orphan_note(level); !orphans.empty()) {
                problems.push_back(std::move(orphans));
            }
        }
        catch (const std::bad_alloc&) {
            xlog::error("[Terrain] out of memory checking the terrain build before saving");
        }
    }
    if (terrains.empty()) {
        try {
            terrain_report_all(problems);
        }
        catch (const std::bad_alloc&) {
            xlog::error("[Terrain] out of memory reporting terrain save problems");
        }
        return;
    }

    struct Record
    {
        DedTerrain* terrain;
        std::string script_name;
        uint8_t chunk_cells;
        uint8_t flags;
        bool write_mapping;
        uint32_t raw_size;
        std::vector<uint8_t> comp;
    };
    std::vector<Record> records;
    std::vector<uint8_t> raw, geo;
    uint64_t total_raw = 0;

    for (auto* terrain : terrains) {
        if (records.size() >= at::max_terrains) {
            problems.push_back(std::format("Only the first {} terrains were saved.", at::max_terrains));
            break;
        }
        try {
            DedTerrainData& d = terrain->data;
            terrain_clamp_properties(d);
            terrain->orient = identity_orient;
            const TerrainGrid& g = *d.grid;

            Vector3& pos = terrain->pos;
            if (std::isfinite(pos.x) && std::isfinite(pos.y) && std::isfinite(pos.z) &&
                (std::abs(pos.x) > at::max_coord || std::abs(pos.y) > at::max_coord ||
                 std::abs(pos.z) > at::max_coord)) {
                pos.x = std::clamp(pos.x, -at::max_coord, at::max_coord);
                pos.y = std::clamp(pos.y, -at::max_coord, at::max_coord);
                pos.z = std::clamp(pos.z, -at::max_coord, at::max_coord);
                problems.push_back(std::format("Terrain uid {} was outside the world limit and was moved to "
                                               "({:.6g}, {:.6g}, {:.6g}).",
                                               terrain->uid, pos.x, pos.y, pos.z));
            }
            const at::Header header = terrain_wire_header(pos, d, geo);
            const char* err = at::validate_header(header);
            if (!err) err = at::validate_overlay_count(header);
            if (!err) err = at::validate_decoration_count(header);
            if (err) {
                problems.push_back(std::format("Terrain uid {} was not saved: {}.", terrain->uid, err));
                continue;
            }
            const std::size_t raw_size = at::header_raw_size(header);
            if (total_raw + raw_size > at::max_level_raw_bytes) {
                problems.push_back(std::format("Terrain uid {} was not saved: the level's terrain data would "
                                               "exceed {} MB.",
                                               terrain->uid, at::max_level_raw_bytes / (1024 * 1024)));
                continue;
            }
            // A group carries no build: the compiled rooms belong to this level.
            bool write_mapping = false;
            if (!group) {
                const bool never_built = d.built_room_uids.empty() || !level.solid;
                std::string problem = terrain_build_fill_mapping(level, *terrain);
                if (!problem.empty() && never_built) unbuilt.emplace_back(std::move(problem), terrain);
                else if (!problem.empty()) problems.push_back(std::move(problem));
                // A mapping that no longer matches the chunk layout is stale; the game treats absent as
                // "not built".
                write_mapping = at::mapping_count_valid(header, static_cast<uint32_t>(d.build_mapping.size()));
            }

            raw.resize(raw_size);
            std::memcpy(raw.data() + at::blob_heights_offset(), g.heights.data(),
                        at::blob_heights_bytes(g.nx, g.nz));
            std::memcpy(raw.data() + at::blob_weights_offset(g.nx, g.nz), g.weights.data(), g.weights.size());
            std::memcpy(raw.data() + at::blob_holes_offset(g.nx, g.nz, g.weight_res_mul), g.holes.data(),
                        g.holes.size());
            std::memcpy(raw.data() + at::blob_diag_offset(g.nx, g.nz, g.weight_res_mul), g.diag.data(),
                        g.diag.size());
            if (header.flags & at::flag_chunk_geo_mask) {
                uint8_t* dst = raw.data() + at::blob_geo_mask_offset(g.nx, g.nz, g.weight_res_mul);
                const std::size_t n = at::blob_geo_mask_bytes(g.nx, g.nz, header.chunk_cells, header.flags);
                std::memset(dst, 0, n);
                std::memcpy(dst, geo.data(), std::min(geo.size(), n));
            }
            if (header.flags & at::flag_overlays) {
                std::memcpy(raw.data() + at::blob_overlay_offset(g.nx, g.nz, g.weight_res_mul, header.chunk_cells,
                                                                 header.flags),
                            g.overlay.data(), g.overlay.size());
            }
            if (header.flags & at::flag_decorations) {
                uint8_t* dst = raw.data() + at::blob_decoration_offset(g.nx, g.nz, g.weight_res_mul,
                                                                       header.chunk_cells, header.flags);
                const std::size_t n =
                    at::blob_decoration_bytes(g.nx, g.nz, g.weight_res_mul, header.flags, header.decoration_count);
                std::memset(dst, 0, n);
                std::memcpy(dst, g.decoration.data(), std::min(g.decoration.size(), n));
            }

            Record rec{terrain, terrain->script_name.c_str(), static_cast<uint8_t>(header.chunk_cells),
                       static_cast<uint8_t>(header.flags), write_mapping, static_cast<uint32_t>(raw_size), {}};
            if (rec.script_name.size() > at::max_script_name_len) rec.script_name.resize(at::max_script_name_len);
            uLongf comp_len = compressBound(static_cast<uLong>(raw_size));
            rec.comp.resize(comp_len);
            if (compress2(rec.comp.data(), &comp_len, raw.data(), static_cast<uLong>(raw_size),
                          Z_DEFAULT_COMPRESSION) != Z_OK) {
                problems.push_back(std::format("Terrain uid {} was not saved: compression failed.", terrain->uid));
                continue;
            }
            rec.comp.resize(comp_len);
            rec.comp.shrink_to_fit();
            records.push_back(std::move(rec));
            total_raw += raw_size;
        }
        catch (const std::bad_alloc&) {
            problems.push_back(std::format("Terrain uid {} was not saved: out of memory.", terrain->uid));
        }
    }
    std::vector<uint8_t>().swap(raw);

    // Counted as the game places them: terrains saved with a build mapping, in record order, within the level
    // budget; one over the cap tells "more than" from "exactly".
    at::DecorationBudget budget;
    budget.instances++;
    uint32_t instances = 0;
    for (const auto& rec : records) {
        if (!rec.write_mapping || budget.spent()) continue;
        const DedTerrain& t = *rec.terrain;
        instances += terrain_decoration_instances(t.uid, t.pos, t.data, budget);
    }
    try {
        if (instances > at::max_level_decoration_instances) {
            problems.push_back(std::format("The level's terrain decorations make more than {0} instances; the game "
                                           "draws the first {0}.",
                                           at::max_level_decoration_instances));
        }
        else if (budget.candidates == 0) {
            problems.push_back(std::format("The level's terrain decorations reach the limit of {} placement tries "
                                           "(most on ground steeper than their slope limit); the game draws the "
                                           "{} instances found first.",
                                           at::max_level_decoration_candidates, instances));
        }
    }
    catch (const std::bad_alloc&) {
        xlog::error("[Terrain] out of memory reporting the decoration instance count");
    }

    auto start_pos = level.BeginRflSection(file, alpine_terrain_chunk_id);

    // No chunk version: growth appends per-record fields gated on the RFL version.
    file.write<uint32_t>(static_cast<uint32_t>(records.size()));

    for (const auto& rec : records) {
        const DedTerrain* terrain = rec.terrain;
        const DedTerrainData& d = terrain->data;
        const TerrainGrid& g = *d.grid;

        file.write<int32_t>(terrain->uid);
        file.write<float>(terrain->pos.x);
        file.write<float>(terrain->pos.y);
        file.write<float>(terrain->pos.z);
        write_rfl_string(file, rec.script_name);
        file.write<float>(d.cell_size);
        file.write<uint16_t>(static_cast<uint16_t>(g.nx));
        file.write<uint16_t>(static_cast<uint16_t>(g.nz));
        file.write<float>(d.height_min);
        file.write<float>(d.height_range);
        file.write<uint8_t>(rec.chunk_cells);
        file.write<uint8_t>(static_cast<uint8_t>(g.weight_res_mul));
        file.write<uint8_t>(d.lightmap_density);
        file.write<uint8_t>(rec.flags);
        file.write<float>(d.thickness);
        file.write<float>(d.skirt_depth);
        write_rfl_string(file, d.underside_texture);
        write_rfl_string(file, d.crater_texture);
        file.write<uint8_t>(static_cast<uint8_t>(d.layers.size()));
        for (const auto& layer : d.layers) {
            write_rfl_string(file, layer.texture);
            file.write<float>(layer.uv_scale);
            file.write<uint8_t>(layer.triplanar ? at::layer_flag_triplanar : 0);
        }
        if (rec.flags & at::flag_overlays) {
            file.write<uint8_t>(static_cast<uint8_t>(d.overlays.size()));
            for (const auto& overlay : d.overlays) {
                write_rfl_string(file, overlay.texture);
                file.write<float>(overlay.uv_scale);
                file.write<uint8_t>(static_cast<uint8_t>((overlay.triplanar ? at::overlay_flag_triplanar : 0) |
                                                         (overlay.break_tiling ? at::overlay_flag_break_tiling : 0)));
            }
        }
        if (rec.flags & at::flag_decorations) {
            file.write<uint8_t>(static_cast<uint8_t>(d.decorations.size()));
            for (const auto& deco : d.decorations) {
                write_rfl_string(file, deco.mesh);
                file.write<float>(deco.density);
                file.write<float>(deco.scale_min);
                file.write<float>(deco.scale_max);
                file.write<float>(deco.max_slope);
                file.write<float>(deco.draw_distance);
                file.write<float>(deco.vertical_offset);
                file.write<uint8_t>(deco.link_layer);
                file.write<uint8_t>(static_cast<uint8_t>(at::decoration_flags(deco)));
            }
        }

        file.write<uint32_t>(rec.write_mapping ? static_cast<uint32_t>(d.build_mapping.size()) : 0);
        if (rec.write_mapping) {
            for (const auto& m : d.build_mapping) {
                file.write<int32_t>(m.room_uid);
                file.write<uint32_t>(m.vertex_count);
                file.write<uint64_t>(m.pos_hash);
            }
        }

        file.write<uint32_t>(rec.raw_size);
        file.write<uint32_t>(static_cast<uint32_t>(rec.comp.size()));
        file.write(rec.comp.data(), rec.comp.size());
    }

    level.EndRflSection(file, start_pos);

    try {
        terrain_report_all(problems, unbuilt);
    }
    catch (const std::bad_alloc&) {
        xlog::error("[Terrain] out of memory reporting terrain save problems");
    }
}

static void terrain_from_record(at::Record& rec, DedTerrain* terrain)
{
    DedTerrainData& d = terrain->data;
    const at::Header& h = rec.header;
    auto g = std::make_shared<TerrainGrid>();
    g->nx = h.nx;
    g->nz = h.nz;
    g->weight_res_mul = h.weight_res_mul;
    g->heights = std::move(rec.heights);
    g->weights = std::move(rec.weights);
    g->holes = std::move(rec.holes);
    g->diag = std::move(rec.diag);
    g->overlay = std::move(rec.overlay_coverage);
    g->decoration = std::move(rec.decoration_coverage);

    terrain->uid = rec.uid;
    terrain->pos = {h.origin[0], h.origin[1], h.origin[2]};
    terrain->script_name.assign_0(rec.script_name.c_str());
    d.cell_size = h.cell_size;
    d.height_min = h.height_min;
    d.height_range = h.height_range;
    d.chunk_cells = h.chunk_cells;
    d.lightmap_density = static_cast<uint8_t>(h.lightmap_density);
    d.flags = static_cast<uint8_t>(h.flags & at::flag_mask);
    d.fullbright = (h.flags & at::flag_fullbright) != 0;
    d.thickness = h.thickness;
    d.skirt_depth = h.skirt_depth;
    d.underside_texture = std::move(rec.underside_texture);
    d.crater_texture = std::move(rec.crater_texture);
    d.layers.reserve(rec.layers.size());
    for (at::RecordLayer& layer : rec.layers) {
        d.layers.push_back({std::move(layer.texture), layer.uv_scale, layer.triplanar});
    }
    d.overlays.resize(rec.overlays.size());
    for (std::size_t i = 0; i < rec.overlays.size(); i++) {
        d.overlays[i].texture = std::move(rec.overlays[i].texture);
        d.overlays[i].uv_scale = rec.overlays[i].uv_scale;
        d.overlays[i].triplanar = rec.overlays[i].triplanar;
        d.overlays[i].break_tiling = rec.overlays[i].break_tiling;
    }
    d.decorations.resize(rec.decorations.size());
    for (std::size_t i = 0; i < rec.decorations.size(); i++) {
        at::RecordDecoration& src = rec.decorations[i];
        DedTerrainDecoration& deco = d.decorations[i];
        deco.mesh = std::move(src.mesh);
        deco.density = src.density;
        deco.scale_min = src.scale_min;
        deco.scale_max = src.scale_max;
        deco.max_slope = src.max_slope;
        deco.draw_distance = src.draw_distance;
        deco.vertical_offset = src.vertical_offset;
        deco.link_layer = src.link_layer;
        deco.align_to_slope = src.align_to_slope;
        deco.random_yaw = src.random_yaw;
        deco.casts_shadows = src.casts_shadows;
    }
    d.build_mapping = std::move(rec.build_mapping);
    d.grid = std::move(g);
    if (!rec.geo_chunks.empty() && !at::chunk_mask_full(rec.geo_chunks.data(), at::header_chunk_count(h))) {
        d.geo_chunks = std::move(rec.geo_chunks);
        d.geo_chunks_layout = at::header_chunk_layout(h);
    }
}

bool terrain_deserialize_chunk(CDedLevel& level, rf::File& file, std::size_t chunk_len, bool group)
{
    std::size_t remaining = chunk_len;
    rf::File::ChunkGuard chunk_guard{file, remaining};
    RflChunkReader<rf::File> reader{file, remaining};
    auto& terrains = level.GetAlpineLevelProperties().terrain_objects;
    auto fail = [&](const std::string& why) {
        terrain_report("Terrain data could not be loaded and will be missing from the next save: " + why,
                       !group);
        return false;
    };

    uint32_t count = 0;
    if (!reader.read(count)) return fail("the chunk is truncated.");
    if (count > 0 && (terrains.size() >= at::max_terrains || count > at::max_terrains - terrains.size())) {
        return fail(std::format("{} more terrains would exceed the limit of {}.", count, at::max_terrains));
    }

    // All or nothing: a record that fails leaves no way to trust the rest of the chunk.
    std::vector<DedTerrain*> parsed;
    DedTerrain* terrain = nullptr;
    const char* err = nullptr;
    uint32_t record = 0;
    try {
        uint64_t total_raw = terrain_level_raw_bytes_except(&level, nullptr);
        for (; record < count; record++) {
            terrain = terrain_alloc();
            at::Record rec;
            if ((err = at::read_record(reader, rec, total_raw))) break;
            terrain_from_record(rec, terrain);
            parsed.push_back(terrain);
            terrain = nullptr;
        }
        if (!err) {
            for (auto* t : parsed) {
                if (group) {
                    terrain_reset_built_state(*t);
                }
                else {
                    terrain_build_note_loaded(level, *t);
                }
            }
            terrains.reserve(terrains.size() + parsed.size());
        }
    }
    catch (const std::bad_alloc&) {
        err = "out of memory";
    }
    if (err) {
        DestroyDedTerrain(terrain);
        for (auto* t : parsed) DestroyDedTerrain(t);
        return fail(record < count ? std::format("record {}: {}.", record, err) : std::format("{}.", err));
    }

    for (auto* t : parsed) {
        terrains.push_back(t);
        level.master_objects.add(static_cast<DedObject*>(t));
    }
    if (group && !parsed.empty()) mark_level_modified();
    xlog::info("[Terrain] Loaded {} terrain object(s)", parsed.size());
    return true;
}

// ─── Heightmap / splat files ─────────────────────────────────────────────────

static bool terrain_read_file(const char* path, std::vector<uint8_t>& out)
{
    constexpr long max_file_size = 256L * 1024 * 1024;
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    bool ok = std::fseek(f, 0, SEEK_END) == 0;
    const long size = ok ? std::ftell(f) : -1;
    ok = ok && size > 0 && size <= max_file_size && std::fseek(f, 0, SEEK_SET) == 0;
    if (ok) {
        out.resize(static_cast<std::size_t>(size));
        ok = std::fread(out.data(), 1, out.size(), f) == out.size();
    }
    std::fclose(f);
    return ok;
}

// Import cap per image edge: 16x the largest grid, and small enough that decoding never threatens
// RED's address space.
static constexpr uint32_t terrain_max_image_edge = 4097;

static bool terrain_is_raw16_path(const char* path)
{
    return string_iends_with(path, ".r16") || string_iends_with(path, ".raw");
}

// Greyscale samples, row 0 at the top of the image (the terrain's +Z edge).
static bool terrain_load_heightmap(const char* path, std::vector<uint16_t>& out, uint32_t& w,
                                   uint32_t& h, std::string& err)
{
    std::vector<uint8_t> bytes;
    if (!terrain_read_file(path, bytes)) {
        err = "The file could not be read.";
        return false;
    }

    if (terrain_is_raw16_path(path)) {
        // RAW16 carries no header: little-endian u16, square, size inferred from the length.
        if (bytes.size() % 2 != 0) {
            err = "A RAW16 file must hold whole 16-bit samples.";
            return false;
        }
        const std::size_t n = bytes.size() / 2;
        const auto side = static_cast<uint32_t>(std::lround(std::sqrt(static_cast<double>(n))));
        if (static_cast<std::size_t>(side) * side != n || side < at::min_verts || side > terrain_max_image_edge) {
            err = std::format("A RAW16 heightmap must be square ({0}x{0} to {1}x{1}).", at::min_verts,
                              terrain_max_image_edge);
            return false;
        }
        w = h = side;
        out.resize(n);
        for (std::size_t i = 0; i < n; i++) {
            out[i] = static_cast<uint16_t>(bytes[i * 2] | (bytes[i * 2 + 1] << 8));
        }
        return true;
    }

    int iw = 0, ih = 0, comp = 0;
    if (!stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &iw, &ih, &comp)) {
        err = "The image format is not supported.";
        return false;
    }
    if (iw < static_cast<int>(at::min_verts) || ih < static_cast<int>(at::min_verts) ||
        iw > static_cast<int>(terrain_max_image_edge) || ih > static_cast<int>(terrain_max_image_edge)) {
        err = std::format("The image must be between {0}x{0} and {1}x{1} pixels.", at::min_verts,
                          terrain_max_image_edge);
        return false;
    }
    // 8-bit images come back widened to 16 bits (v * 257), so both depths share one path.
    stbi_us* pixels = stbi_load_16_from_memory(bytes.data(), static_cast<int>(bytes.size()), &iw, &ih,
                                               &comp, 1);
    if (!pixels) {
        err = "The image could not be decoded.";
        return false;
    }
    w = static_cast<uint32_t>(iw);
    h = static_cast<uint32_t>(ih);
    out.assign(pixels, pixels + static_cast<std::size_t>(w) * h);
    stbi_image_free(pixels);
    return true;
}

// Resamples image samples onto the grid's vertex lattice.
static void terrain_heights_from_image(TerrainGrid& g, const std::vector<uint16_t>& img, uint32_t w,
                                       uint32_t h)
{
    terrain_filter(
        terrain_axis_taps(w, g.nx, true), terrain_axis_taps(h, g.nz, true), w, 1,
        [&](uint32_t c, uint32_t r, uint32_t) {
            return static_cast<float>(img[static_cast<std::size_t>(at::image_row(r, h)) * w + c]);
        },
        [&](uint32_t i, uint32_t j, const float* v) {
            g.heights[static_cast<std::size_t>(j) * g.nx + i] = at::encode_height01(v[0] / 65535.0f);
        });
}

static void terrain_png_chunk(std::vector<uint8_t>& out, const char* type, const uint8_t* data,
                              std::size_t len)
{
    auto be32 = [&](uint32_t v) {
        out.push_back(static_cast<uint8_t>(v >> 24));
        out.push_back(static_cast<uint8_t>(v >> 16));
        out.push_back(static_cast<uint8_t>(v >> 8));
        out.push_back(static_cast<uint8_t>(v));
    };
    be32(static_cast<uint32_t>(len));
    const std::size_t crc_start = out.size();
    out.insert(out.end(), type, type + 4);
    if (len) out.insert(out.end(), data, data + len);
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, out.data() + crc_start, static_cast<uInt>(out.size() - crc_start));
    be32(static_cast<uint32_t>(crc));
}

// 16-bit greyscale PNG (no stb_image_write in vendor/): one IDAT, filter 0 on every row.
static bool terrain_export_heightmap(const char* path, const TerrainGrid& g, std::string& err)
{
    const uint32_t w = g.nx, h = g.nz;
    std::vector<uint8_t> file;
    if (terrain_is_raw16_path(path)) {
        file.reserve(static_cast<std::size_t>(w) * h * 2);
        for (uint32_t r = 0; r < h; r++) {
            const uint32_t z = at::image_row(r, h);
            for (uint32_t x = 0; x < w; x++) {
                const uint16_t v = g.heights[static_cast<std::size_t>(z) * w + x];
                file.push_back(static_cast<uint8_t>(v));
                file.push_back(static_cast<uint8_t>(v >> 8));
            }
        }
    }
    else {
        std::vector<uint8_t> scan;
        scan.reserve((static_cast<std::size_t>(w) * 2 + 1) * h);
        for (uint32_t r = 0; r < h; r++) {
            const uint32_t z = at::image_row(r, h);
            scan.push_back(0);
            for (uint32_t x = 0; x < w; x++) {
                const uint16_t v = g.heights[static_cast<std::size_t>(z) * w + x];
                scan.push_back(static_cast<uint8_t>(v >> 8));
                scan.push_back(static_cast<uint8_t>(v));
            }
        }
        uLongf comp_len = compressBound(static_cast<uLong>(scan.size()));
        std::vector<uint8_t> idat(comp_len);
        if (compress2(idat.data(), &comp_len, scan.data(), static_cast<uLong>(scan.size()), 9) != Z_OK) {
            err = "zlib failed.";
            return false;
        }
        static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
        file.assign(signature, signature + 8);
        const uint8_t ihdr[13] = {
            static_cast<uint8_t>(w >> 24), static_cast<uint8_t>(w >> 16), static_cast<uint8_t>(w >> 8),
            static_cast<uint8_t>(w), static_cast<uint8_t>(h >> 24), static_cast<uint8_t>(h >> 16),
            static_cast<uint8_t>(h >> 8), static_cast<uint8_t>(h),
            16, 0, 0, 0, 0, // bit depth 16, greyscale, deflate, filter 0, no interlace
        };
        terrain_png_chunk(file, "IHDR", ihdr, sizeof(ihdr));
        terrain_png_chunk(file, "IDAT", idat.data(), comp_len);
        terrain_png_chunk(file, "IEND", nullptr, 0);
    }

    FILE* f = std::fopen(path, "wb");
    if (!f) {
        err = "The file could not be created.";
        return false;
    }
    const bool ok = std::fwrite(file.data(), 1, file.size(), f) == file.size();
    if (std::fclose(f) != 0 || !ok) {
        err = "The file could not be written.";
        return false;
    }
    return true;
}

// Writes the RGBA image into four weight channels starting at `base` and renormalizes every texel.
// Returns the highest channel any texel now uses.
static int terrain_apply_splat(TerrainGrid& g, const uint8_t* rgba, uint32_t w, uint32_t h, int base)
{
    const uint32_t dw = at::weight_width(g.nx, g.weight_res_mul);
    const uint32_t dh = at::weight_height(g.nz, g.weight_res_mul);
    int highest = 0;
    terrain_filter(
        terrain_axis_taps(w, dw, false), terrain_axis_taps(h, dh, false), w, 4,
        [&](uint32_t c, uint32_t r, uint32_t ch) {
            return static_cast<float>(rgba[(static_cast<std::size_t>(at::image_row(r, h)) * w + c) * 4 + ch]);
        },
        [&](uint32_t i, uint32_t j, const float* v) {
            const std::size_t texel = static_cast<std::size_t>(j) * dw + i;
            uint8_t wt[at::max_layers];
            terrain_get_weights(g, texel, wt);
            for (int c = 0; c < 4; c++) wt[base + c] = terrain_weight_byte(v[c]);
            at::normalize_weights(wt);
            terrain_set_weights(g, texel, wt);
            for (int c = static_cast<int>(at::max_layers) - 1; c > highest; c--) {
                if (wt[c]) {
                    highest = c;
                    break;
                }
            }
        });
    return highest;
}

static void terrain_remove_weight_channel(TerrainGrid& g, std::size_t k)
{
    for (std::size_t t = 0; t < terrain_texel_count(g); t++) {
        uint8_t w[at::max_layers];
        terrain_get_weights(g, t, w);
        for (std::size_t c = k; c + 1 < at::max_layers; c++) w[c] = w[c + 1];
        w[at::max_layers - 1] = 0;
        at::normalize_weights(w);
        terrain_set_weights(g, t, w);
    }
}

static void terrain_swap_weight_channels(TerrainGrid& g, std::size_t a, std::size_t b)
{
    for (std::size_t t = 0; t < terrain_texel_count(g); t++) {
        uint8_t w[at::max_layers];
        terrain_get_weights(g, t, w);
        std::swap(w[a], w[b]);
        terrain_set_weights(g, t, w);
    }
}

// Overlay k's coverage goes; the channels above it move down and the last one clears.
static void terrain_remove_overlay_channel(TerrainGrid& g, std::size_t k)
{
    for (std::size_t t = 0; t + 4 <= g.overlay.size(); t += 4) {
        for (std::size_t c = k; c + 1 < at::max_overlays; c++) g.overlay[t + c] = g.overlay[t + c + 1];
        g.overlay[t + at::max_overlays - 1] = 0;
    }
}

static void terrain_swap_overlay_channels(TerrainGrid& g, std::size_t a, std::size_t b)
{
    for (std::size_t t = 0; t + 4 <= g.overlay.size(); t += 4) std::swap(g.overlay[t + a], g.overlay[t + b]);
}

static void terrain_remove_decoration_plane(TerrainGrid& g, std::size_t k)
{
    const std::span<uint8_t> p = terrain_decoration_plane(g, k);
    if (p.empty()) return;
    const auto first = g.decoration.begin() + (p.data() - g.decoration.data());
    g.decoration.erase(first, first + static_cast<std::ptrdiff_t>(p.size()));
}

static void terrain_swap_decoration_planes(TerrainGrid& g, std::size_t a, std::size_t b)
{
    const std::span<uint8_t> pa = terrain_decoration_plane(g, a), pb = terrain_decoration_plane(g, b);
    if (pa.empty() || pb.empty()) return;
    std::swap_ranges(pa.begin(), pa.end(), pb.begin());
}

static bool terrain_decoration_plane_is(const TerrainGrid& g, std::size_t k, uint8_t value)
{
    const std::span<const uint8_t> p = terrain_decoration_plane(g, k);
    return !p.empty() && std::all_of(p.begin(), p.end(), [value](uint8_t v) { return v == value; });
}

// Copy-on-write: the staged grid may be shared with the terrain object and the clipboard.
static TerrainGrid& terrain_mutable_grid(DedTerrainData& d)
{
    auto g = std::make_shared<TerrainGrid>(*d.grid);
    d.grid = g;
    return *g;
}

// Sets decoration k's link layer. Linking from none fills a plane still empty, so the decoration appears where
// that layer is painted; unlinking empties a plane still full. `g` is d's unshared grid, or null to copy it first
// when a plane changes.
static void terrain_set_decoration_link(DedTerrainData& d, std::size_t k, uint8_t link, TerrainGrid* g = nullptr)
{
    const bool was_linked = d.decorations[k].link_layer != at::decoration_link_none;
    const bool linked = link != at::decoration_link_none;
    d.decorations[k].link_layer = link;
    if (was_linked == linked) return;
    const uint8_t from = linked ? uint8_t{0} : at::decoration_coverage_full;
    if (!terrain_decoration_plane_is(*d.grid, k, from)) return;
    const std::span<uint8_t> p = terrain_decoration_plane(g ? *g : terrain_mutable_grid(d), k);
    std::fill(p.begin(), p.end(), linked ? at::decoration_coverage_full : uint8_t{0});
}

// Layer `removed` is gone: decorations linked to it are unlinked (a plane still full from linking is emptied) and
// later links shift down. `g` is d's unshared grid.
static void terrain_unlink_removed_layer(DedTerrainData& d, TerrainGrid& g, std::size_t removed)
{
    for (std::size_t k = 0; k < d.decorations.size(); k++) {
        DedTerrainDecoration& deco = d.decorations[k];
        if (deco.link_layer == at::decoration_link_none) continue;
        if (deco.link_layer == removed) {
            terrain_set_decoration_link(d, k, at::decoration_link_none, &g);
        }
        else if (deco.link_layer > removed) {
            deco.link_layer--;
        }
    }
}

static void terrain_swap_layer_links(DedTerrainData& d, std::size_t a, std::size_t b)
{
    for (auto& deco : d.decorations) {
        if (deco.link_layer == a) deco.link_layer = static_cast<uint8_t>(b);
        else if (deco.link_layer == b) deco.link_layer = static_cast<uint8_t>(a);
    }
}

// ─── Properties Dialog ──────────────────────────────────────────────────────

// The dialog edits base layers, overlays and decorations through the same list controls.
enum TerrainListKind
{
    terrain_list_layers = 0,
    terrain_list_overlays = 1,
    terrain_list_decorations = 2,
    terrain_list_kinds = 3,
};

struct TerrainListUi
{
    int list, add, remove, up, down, texture, browse, uv_scale, uv_scale_spin, triplanar, break_tiling, preview;
    int min_count, max_count;
};

static constexpr TerrainListUi terrain_list_ui[terrain_list_kinds] = {
    {IDC_TERRAIN_LAYER_LIST, IDC_TERRAIN_LAYER_ADD, IDC_TERRAIN_LAYER_REMOVE, IDC_TERRAIN_LAYER_UP,
     IDC_TERRAIN_LAYER_DOWN, IDC_TERRAIN_LAYER_TEXTURE, IDC_TERRAIN_LAYER_BROWSE, IDC_TERRAIN_LAYER_UV_SCALE,
     IDC_TERRAIN_LAYER_UV_SCALE_SPIN, IDC_TERRAIN_LAYER_TRIPLANAR, 0, IDC_TERRAIN_LAYER_PREVIEW, 1,
     static_cast<int>(at::max_layers)},
    {IDC_TERRAIN_OVERLAY_LIST, IDC_TERRAIN_OVERLAY_ADD, IDC_TERRAIN_OVERLAY_REMOVE, IDC_TERRAIN_OVERLAY_UP,
     IDC_TERRAIN_OVERLAY_DOWN, IDC_TERRAIN_OVERLAY_TEXTURE, IDC_TERRAIN_OVERLAY_BROWSE, IDC_TERRAIN_OVERLAY_UV_SCALE,
     IDC_TERRAIN_OVERLAY_UV_SCALE_SPIN, IDC_TERRAIN_OVERLAY_TRIPLANAR, IDC_TERRAIN_OVERLAY_BREAK_TILING,
     IDC_TERRAIN_OVERLAY_PREVIEW, 0, static_cast<int>(at::max_overlays)},
    // Its fields are its own (terrain_deco_fields and the controls around them); 0 = none.
    {IDC_TERRAIN_DECO_LIST, IDC_TERRAIN_DECO_ADD, IDC_TERRAIN_DECO_REMOVE, IDC_TERRAIN_DECO_UP, IDC_TERRAIN_DECO_DOWN,
     0, 0, 0, 0, 0, 0, 0, 0, static_cast<int>(at::max_decorations)},
};

// A decoration's float fields, stepped by `step` within [lo, hi].
struct TerrainDecoField
{
    int edit, spin;
    float DedTerrainDecoration::*value;
    float step, lo, hi;
    int decimals;
};

static constexpr TerrainDecoField terrain_deco_fields[] = {
    {IDC_TERRAIN_DECO_DENSITY, IDC_TERRAIN_DECO_DENSITY_SPIN, &DedTerrainDecoration::density, 0.1f, 0.0f,
     at::max_decoration_density, 3},
    {IDC_TERRAIN_DECO_SCALE_MIN, IDC_TERRAIN_DECO_SCALE_MIN_SPIN, &DedTerrainDecoration::scale_min, 0.1f,
     at::min_decoration_scale, at::max_decoration_scale, 2},
    {IDC_TERRAIN_DECO_SCALE_MAX, IDC_TERRAIN_DECO_SCALE_MAX_SPIN, &DedTerrainDecoration::scale_max, 0.1f,
     at::min_decoration_scale, at::max_decoration_scale, 2},
    {IDC_TERRAIN_DECO_SLOPE, IDC_TERRAIN_DECO_SLOPE_SPIN, &DedTerrainDecoration::max_slope, 1.0f, 0.0f,
     at::max_decoration_slope_deg, 1},
    {IDC_TERRAIN_DECO_DRAW_DIST, IDC_TERRAIN_DECO_DRAW_DIST_SPIN, &DedTerrainDecoration::draw_distance, 5.0f,
     at::min_decoration_draw_distance, at::max_decoration_draw_distance, 1},
    {IDC_TERRAIN_DECO_OFFSET, IDC_TERRAIN_DECO_OFFSET_SPIN, &DedTerrainDecoration::vertical_offset, 0.05f,
     -at::max_decoration_offset, at::max_decoration_offset, 3},
};

struct TerrainDecoCheck
{
    int check;
    bool DedTerrainDecoration::*value;
};

static constexpr TerrainDecoCheck terrain_deco_checks[] = {
    {IDC_TERRAIN_DECO_ALIGN, &DedTerrainDecoration::align_to_slope},
    {IDC_TERRAIN_DECO_RANDOM_YAW, &DedTerrainDecoration::random_yaw},
    {IDC_TERRAIN_DECO_CASTS_SHADOWS, &DedTerrainDecoration::casts_shadows},
};

// Enabled while there is a decoration to edit.
static constexpr int terrain_deco_controls[] = {
    IDC_TERRAIN_DECO_MESH,       IDC_TERRAIN_DECO_MESH_BROWSE,    IDC_TERRAIN_DECO_DENSITY,
    IDC_TERRAIN_DECO_DENSITY_SPIN, IDC_TERRAIN_DECO_SCALE_MIN,    IDC_TERRAIN_DECO_SCALE_MIN_SPIN,
    IDC_TERRAIN_DECO_SCALE_MAX,  IDC_TERRAIN_DECO_SCALE_MAX_SPIN, IDC_TERRAIN_DECO_SLOPE,
    IDC_TERRAIN_DECO_SLOPE_SPIN, IDC_TERRAIN_DECO_DRAW_DIST,      IDC_TERRAIN_DECO_DRAW_DIST_SPIN,
    IDC_TERRAIN_DECO_OFFSET,     IDC_TERRAIN_DECO_OFFSET_SPIN,    IDC_TERRAIN_DECO_LINK,
    IDC_TERRAIN_DECO_ALIGN,      IDC_TERRAIN_DECO_RANDOM_YAW,     IDC_TERRAIN_DECO_CASTS_SHADOWS,
};

// Staging for the open dialog: only IDOK writes the terrain. The viewport box follows the staged
// dimensions while the dialog is up.
struct TerrainDialogState
{
    bool active = false;
    bool loading_layer = false;
    DedTerrain* terrain = nullptr;
    DedTerrainData data;
    int sel[terrain_list_kinds] = {};
    std::string preview_name[terrain_list_kinds];
    int preview_handle[terrain_list_kinds];

    TerrainDialogState()
    {
        std::fill(std::begin(preview_handle), std::end(preview_handle), -1);
    }
};
static TerrainDialogState g_terrain_dlg;

static int terrain_dlg_count(int kind)
{
    const DedTerrainData& d = g_terrain_dlg.data;
    switch (kind) {
    case terrain_list_overlays: return static_cast<int>(d.overlays.size());
    case terrain_list_decorations: return static_cast<int>(d.decorations.size());
    default: return static_cast<int>(d.layers.size());
    }
}

// A base layer or an overlay.
static DedTerrainLayer& terrain_dlg_item(int kind, int index)
{
    if (kind == terrain_list_overlays) return g_terrain_dlg.data.overlays[index];
    return g_terrain_dlg.data.layers[index];
}

static std::string terrain_get_text(HWND hdlg, int idc)
{
    char buf[MAX_PATH] = {};
    GetDlgItemTextA(hdlg, idc, buf, sizeof(buf));
    return buf;
}

enum TerrainDlgTimer : UINT_PTR
{
    // Composites and decorations left for a later paint are otherwise serviced by RED's idle loop, which the
    // dialog's modal loop does not run.
    terrain_dlg_repaint_timer = 1,
    terrain_dlg_status_timer = 2,
};
static constexpr UINT terrain_dlg_status_delay_ms = 250;

static void terrain_dlg_update_status(HWND hdlg)
{
    const DedTerrainData& d = g_terrain_dlg.data;
    const TerrainGrid& g = *d.grid;
    char buf[128];
    const int n = std::snprintf(buf, sizeof(buf), "Data %.2f MB, weights %u x %u",
                                static_cast<double>(terrain_data_raw_bytes(d)) / (1024.0 * 1024.0),
                                at::weight_width(g.nx, g.weight_res_mul), at::weight_height(g.nz, g.weight_res_mul));
    if (!d.decorations.empty() && n > 0 && static_cast<std::size_t>(n) < sizeof(buf)) {
        const DedTerrain& t = *g_terrain_dlg.terrain;
        const uint32_t cap = at::max_level_decoration_instances;
        at::DecorationBudget budget;
        budget.instances = cap + 1;
        const uint32_t instances = terrain_decoration_instances(t.uid, t.pos, d, budget);
        const char* format = instances > cap           ? ". Decorations: over %u instances"
                             : budget.candidates == 0 ? ". Decorations: %u instances (placement limit)"
                                                      : ". Decorations: %u instances";
        std::snprintf(buf + n, sizeof(buf) - n, format, std::min(instances, cap));
    }
    SetDlgItemTextA(hdlg, IDC_TERRAIN_STATUS, buf);
}

static void terrain_dlg_update_readouts(HWND hdlg)
{
    const DedTerrainData& d = g_terrain_dlg.data;
    const TerrainGrid& g = *d.grid;
    const uint32_t cx = at::cells(g.nx), cz = at::cells(g.nz);
    char buf[128];

    std::snprintf(buf, sizeof(buf), "%u x %u vertices (%u x %u cells)", g.nx, g.nz, cx, cz);
    SetDlgItemTextA(hdlg, IDC_TERRAIN_RESOLUTION, buf);

    std::snprintf(buf, sizeof(buf), "Extent %.6g x %.6g m", at::extent(g.nx, d.cell_size),
                  at::extent(g.nz, d.cell_size));
    SetDlgItemTextA(hdlg, IDC_TERRAIN_EXTENT, buf);

    const uint32_t edge = terrain_effective_chunk_cells(d);
    const uint32_t chunks = terrain_chunk_count(d);
    if (edge != d.chunk_cells) {
        std::snprintf(buf, sizeof(buf), "%s to %u (%u chunks)", edge > d.chunk_cells ? "raised" : "lowered",
                      edge, chunks);
    }
    else {
        std::snprintf(buf, sizeof(buf), "%u chunk%s", chunks, chunks == 1 ? "" : "s");
    }
    SetDlgItemTextA(hdlg, IDC_TERRAIN_CHUNK_INFO, buf);

    // Counting decorations can take a while, so while there are any the status line follows once edits pause.
    if (d.decorations.empty()) {
        KillTimer(hdlg, terrain_dlg_status_timer);
        terrain_dlg_update_status(hdlg);
    }
    else {
        SetTimer(hdlg, terrain_dlg_status_timer, terrain_dlg_status_delay_ms, nullptr);
    }
}

static void terrain_dlg_refresh_viewports()
{
    if (g_terrain_dlg.active) redraw_all_viewports();
}

static void terrain_dlg_format_layer(int kind, int index, char* buf, std::size_t size)
{
    if (kind == terrain_list_decorations) {
        const DedTerrainDecoration& deco = g_terrain_dlg.data.decorations[index];
        char link[24] = "";
        if (deco.link_layer != at::decoration_link_none) {
            std::snprintf(link, sizeof(link), ", layer %d", deco.link_layer + 1);
        }
        char offset[24] = "";
        if (deco.vertical_offset != 0.0f) {
            std::snprintf(offset, sizeof(offset), ", offset %+.3g", deco.vertical_offset);
        }
        std::snprintf(buf, size, "%d: %s  (%.3g/m2, x%.3g-%.3g%s%s%s)", index + 1,
                      deco.mesh.empty() ? "(none)" : deco.mesh.c_str(), deco.density, deco.scale_min, deco.scale_max,
                      link, offset, deco.casts_shadows ? ", shadows" : "");
        return;
    }
    const DedTerrainLayer& layer = terrain_dlg_item(kind, index);
    const bool break_tiling = kind == terrain_list_overlays && g_terrain_dlg.data.overlays[index].break_tiling;
    std::snprintf(buf, size, "%d: %s  (tile %.4g m%s%s)", index + 1,
                  layer.texture.empty() ? "(none)" : layer.texture.c_str(), layer.uv_scale,
                  layer.triplanar ? ", triplanar" : "", break_tiling ? ", break-up" : "");
}

static void terrain_dlg_fill_layer_list(HWND hdlg, int kind)
{
    HWND list = GetDlgItem(hdlg, terrain_list_ui[kind].list);
    SendMessageA(list, LB_RESETCONTENT, 0, 0);
    char buf[128];
    for (int i = 0; i < terrain_dlg_count(kind); i++) {
        terrain_dlg_format_layer(kind, i, buf, sizeof(buf));
        SendMessageA(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(buf));
    }
    SendMessageA(list, LB_SETCURSEL, g_terrain_dlg.sel[kind], 0);
}

static void terrain_dlg_refresh_layer_row(HWND hdlg, int kind, int index)
{
    HWND list = GetDlgItem(hdlg, terrain_list_ui[kind].list);
    char buf[128];
    terrain_dlg_format_layer(kind, index, buf, sizeof(buf));
    SendMessageA(list, LB_DELETESTRING, index, 0);
    SendMessageA(list, LB_INSERTSTRING, index, reinterpret_cast<LPARAM>(buf));
    SendMessageA(list, LB_SETCURSEL, g_terrain_dlg.sel[kind], 0);
}

static void terrain_dlg_update_layer_preview(HWND hdlg, int kind, bool force)
{
    if (!terrain_list_ui[kind].preview) return;
    const std::string name = terrain_get_text(hdlg, terrain_list_ui[kind].texture);
    if (!force && g_terrain_dlg.preview_name[kind] == name) return;
    g_terrain_dlg.preview_name[kind] = name;
    g_terrain_dlg.preview_handle[kind] = alpine_dlg_resolve_bitmap(name.c_str());
    InvalidateRect(GetDlgItem(hdlg, terrain_list_ui[kind].preview), nullptr, TRUE);
}

static void terrain_dlg_update_state(HWND hdlg)
{
    const bool geoable = IsDlgButtonChecked(hdlg, IDC_TERRAIN_GEOABLE) == BST_CHECKED;
    const bool skirts = IsDlgButtonChecked(hdlg, IDC_TERRAIN_SKIRTS) == BST_CHECKED;

    static const int thickness_controls[] = {IDC_TERRAIN_THICKNESS, IDC_TERRAIN_THICKNESS_SPIN,
                                             IDC_TERRAIN_THICKNESS_LABEL};
    for (int id : thickness_controls) EnableWindow(GetDlgItem(hdlg, id), geoable);

    // A geoable terrain is emitted as closed chunks, which already have walls.
    EnableWindow(GetDlgItem(hdlg, IDC_TERRAIN_SKIRTS), !geoable);
    static const int skirt_controls[] = {IDC_TERRAIN_SKIRT_DEPTH, IDC_TERRAIN_SKIRT_DEPTH_SPIN,
                                         IDC_TERRAIN_SKIRT_DEPTH_LABEL};
    for (int id : skirt_controls) EnableWindow(GetDlgItem(hdlg, id), skirts && !geoable);

    static const int underside_controls[] = {IDC_TERRAIN_UNDERSIDE_TEXTURE, IDC_TERRAIN_UNDERSIDE_BROWSE,
                                             IDC_TERRAIN_UNDERSIDE_LABEL};
    for (int id : underside_controls) EnableWindow(GetDlgItem(hdlg, id), geoable || skirts);

    // A fullbright terrain gets no baked light.
    const bool lit = IsDlgButtonChecked(hdlg, IDC_TERRAIN_FULLBRIGHT) != BST_CHECKED;
    for (int id : {IDC_TERRAIN_LM_DENSITY, IDC_TERRAIN_LM_DENSITY_SPIN, IDC_TERRAIN_LM_DENSITY_LABEL,
                   IDC_TERRAIN_LM_DENSITY_UNIT}) {
        EnableWindow(GetDlgItem(hdlg, id), lit);
    }

    for (int kind = 0; kind < terrain_list_kinds; kind++) {
        const TerrainListUi& ui = terrain_list_ui[kind];
        const int count = terrain_dlg_count(kind);
        const int sel = g_terrain_dlg.sel[kind];
        EnableWindow(GetDlgItem(hdlg, ui.add), count < ui.max_count);
        EnableWindow(GetDlgItem(hdlg, ui.remove), count > ui.min_count);
        EnableWindow(GetDlgItem(hdlg, ui.up), sel > 0 && sel < count);
        EnableWindow(GetDlgItem(hdlg, ui.down), sel + 1 < count);
        for (int id : {ui.texture, ui.browse, ui.uv_scale, ui.uv_scale_spin, ui.triplanar, ui.break_tiling}) {
            if (id) EnableWindow(GetDlgItem(hdlg, id), count > 0);
        }
    }
    const bool decorations = terrain_dlg_count(terrain_list_decorations) > 0;
    for (int id : terrain_deco_controls) EnableWindow(GetDlgItem(hdlg, id), decorations);
}

// "None", then every base layer.
static void terrain_dlg_fill_link_combo(HWND hdlg, uint8_t selected)
{
    SendDlgItemMessageA(hdlg, IDC_TERRAIN_DECO_LINK, CB_RESETCONTENT, 0, 0);
    alpine_dlg_combo_add(hdlg, IDC_TERRAIN_DECO_LINK, "None", at::decoration_link_none);
    const auto& layers = g_terrain_dlg.data.layers;
    for (std::size_t i = 0; i < layers.size(); i++) {
        char buf[64];
        const std::string& name = layers[i].texture;
        std::snprintf(buf, sizeof(buf), "%zu: %s", i + 1, name.empty() ? "(none)" : name.c_str());
        alpine_dlg_combo_add(hdlg, IDC_TERRAIN_DECO_LINK, buf, static_cast<LPARAM>(i));
    }
    alpine_dlg_combo_select(hdlg, IDC_TERRAIN_DECO_LINK, selected);
}

static const DedTerrainDecoration* terrain_dlg_selected_decoration()
{
    const int sel = g_terrain_dlg.sel[terrain_list_decorations];
    const auto& decorations = g_terrain_dlg.data.decorations;
    return sel >= 0 && static_cast<std::size_t>(sel) < decorations.size() ? &decorations[sel] : nullptr;
}

static void terrain_dlg_load_decoration_fields(HWND hdlg)
{
    const DedTerrainDecoration* deco = terrain_dlg_selected_decoration();
    SetDlgItemTextA(hdlg, IDC_TERRAIN_DECO_MESH, deco ? deco->mesh.c_str() : "");
    for (const TerrainDecoField& f : terrain_deco_fields) {
        if (deco) {
            alpine_dlg_set_float_field_exact(hdlg, f.edit, deco->*f.value);
        }
        else {
            SetDlgItemTextA(hdlg, f.edit, "");
        }
    }
    terrain_dlg_fill_link_combo(hdlg, deco ? deco->link_layer : at::decoration_link_none);
    for (const TerrainDecoCheck& c : terrain_deco_checks) {
        CheckDlgButton(hdlg, c.check, deco && deco->*c.value ? BST_CHECKED : BST_UNCHECKED);
    }
}

static void terrain_dlg_load_layer_fields(HWND hdlg, int kind)
{
    const TerrainListUi& ui = terrain_list_ui[kind];
    const int count = terrain_dlg_count(kind);
    g_terrain_dlg.sel[kind] = std::clamp(g_terrain_dlg.sel[kind], 0, std::max(count - 1, 0));
    const int sel = g_terrain_dlg.sel[kind];
    g_terrain_dlg.loading_layer = true;
    if (kind == terrain_list_decorations) {
        terrain_dlg_load_decoration_fields(hdlg);
    }
    else if (count > 0) {
        const DedTerrainLayer& layer = terrain_dlg_item(kind, sel);
        SetDlgItemTextA(hdlg, ui.texture, layer.texture.c_str());
        alpine_dlg_set_float_field_exact(hdlg, ui.uv_scale, layer.uv_scale);
        CheckDlgButton(hdlg, ui.triplanar, layer.triplanar ? BST_CHECKED : BST_UNCHECKED);
        if (ui.break_tiling) {
            CheckDlgButton(hdlg, ui.break_tiling,
                           g_terrain_dlg.data.overlays[sel].break_tiling ? BST_CHECKED : BST_UNCHECKED);
        }
    }
    else {
        SetDlgItemTextA(hdlg, ui.texture, "");
        SetDlgItemTextA(hdlg, ui.uv_scale, "");
        CheckDlgButton(hdlg, ui.triplanar, BST_UNCHECKED);
        if (ui.break_tiling) CheckDlgButton(hdlg, ui.break_tiling, BST_UNCHECKED);
    }
    g_terrain_dlg.loading_layer = false;
    terrain_dlg_update_layer_preview(hdlg, kind, true);
    terrain_dlg_update_state(hdlg);
}

static void terrain_dlg_reselect_layer(HWND hdlg, int kind, int sel)
{
    g_terrain_dlg.sel[kind] = sel;
    terrain_dlg_fill_layer_list(hdlg, kind);
    terrain_dlg_load_layer_fields(hdlg, kind);
}

// The base layers changed: the decorations show their links again.
static void terrain_dlg_layers_changed(HWND hdlg)
{
    terrain_dlg_reselect_layer(hdlg, terrain_list_decorations, g_terrain_dlg.sel[terrain_list_decorations]);
}

// The texture browser runs its own modal loop off the main frame; same disable/re-activate dance the
// rope dialog does. It opens on the field's texture's category, else on the one texture mode starts on.
static bool terrain_browse_texture(HWND hdlg, int field_idc)
{
    const std::string current = terrain_get_text(hdlg, field_idc);
    const char* category = texture_category_of(current.c_str());
    EnableWindow(hdlg, FALSE);
    const int picked = texture_browser_pick(category ? category : "Root", alpine_dlg_resolve_bitmap(current.c_str()));
    EnableWindow(hdlg, TRUE);
    SetActiveWindow(hdlg);
    if (picked < 0) return false;
    const char* name = bm_get_filename(picked);
    SetDlgItemTextA(hdlg, field_idc, name ? name : "");
    return true;
}

// ─── Resolution / splat prompts ─────────────────────────────────────────────

struct TerrainResolutionPrompt
{
    const char* title = "";
    std::string info;
    uint32_t nx = at::default_verts;
    uint32_t nz = at::default_verts;
};
static TerrainResolutionPrompt g_terrain_res_prompt;

// Whether both vertex count fields are in range; says so when not.
static bool terrain_dlg_vertex_counts_valid(HWND hdlg, int nx_idc, int nz_idc)
{
    for (int idc : {nx_idc, nz_idc}) {
        const int n = alpine_dlg_get_int_field(hdlg, idc);
        if (n < static_cast<int>(at::min_verts) || n > static_cast<int>(at::max_verts)) {
            const std::string msg =
                std::format("Each vertex count must be between {} and {}.", at::min_verts, at::max_verts);
            MessageBoxA(hdlg, msg.c_str(), "Terrain", MB_OK | MB_ICONWARNING);
            return false;
        }
    }
    return true;
}

static INT_PTR CALLBACK TerrainResolutionDialogProc(HWND hdlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG:
        alpine_center_dialog_on_owner(hdlg);
        SetWindowTextA(hdlg, g_terrain_res_prompt.title);
        SetDlgItemTextA(hdlg, IDC_TERRAIN_RES_INFO, g_terrain_res_prompt.info.c_str());
        SetDlgItemInt(hdlg, IDC_TERRAIN_RES_NX, g_terrain_res_prompt.nx, FALSE);
        SetDlgItemInt(hdlg, IDC_TERRAIN_RES_NZ, g_terrain_res_prompt.nz, FALSE);
        alpine_spinner_init_int(hdlg, IDC_TERRAIN_RES_NX, IDC_TERRAIN_RES_NX_SPIN, 1,
                                static_cast<int>(at::min_verts), static_cast<int>(at::max_verts));
        alpine_spinner_init_int(hdlg, IDC_TERRAIN_RES_NZ, IDC_TERRAIN_RES_NZ_SPIN, 1,
                                static_cast<int>(at::min_verts), static_cast<int>(at::max_verts));
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK: {
            if (!terrain_dlg_vertex_counts_valid(hdlg, IDC_TERRAIN_RES_NX, IDC_TERRAIN_RES_NZ)) return TRUE;
            g_terrain_res_prompt.nx = static_cast<uint32_t>(alpine_dlg_get_int_field(hdlg, IDC_TERRAIN_RES_NX));
            g_terrain_res_prompt.nz = static_cast<uint32_t>(alpine_dlg_get_int_field(hdlg, IDC_TERRAIN_RES_NZ));
            EndDialog(hdlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(hdlg, IDCANCEL);
            return TRUE;
        }
        break;
    case WM_NOTIFY:
        if (alpine_spinner_handle_notify(hdlg, lp)) return TRUE;
        break;
    }
    return FALSE;
}

static bool terrain_prompt_resolution(HWND parent, const char* title, std::string info, uint32_t& nx,
                                      uint32_t& nz)
{
    g_terrain_res_prompt.title = title;
    g_terrain_res_prompt.info = std::move(info);
    g_terrain_res_prompt.nx = nx;
    g_terrain_res_prompt.nz = nz;
    const INT_PTR result = DialogBoxParam(reinterpret_cast<HINSTANCE>(&__ImageBase),
                                          MAKEINTRESOURCE(IDD_ALPINE_TERRAIN_RESOLUTION), parent,
                                          TerrainResolutionDialogProc, 0);
    if (result != IDOK) return false;
    nx = g_terrain_res_prompt.nx;
    nz = g_terrain_res_prompt.nz;
    return true;
}

static int g_terrain_splat_base = 0;

static INT_PTR CALLBACK TerrainSplatDialogProc(HWND hdlg, UINT msg, WPARAM wp, LPARAM /*lp*/)
{
    switch (msg) {
    case WM_INITDIALOG:
        alpine_center_dialog_on_owner(hdlg);
        CheckRadioButton(hdlg, IDC_TERRAIN_SPLAT_LAYERS_1_4, IDC_TERRAIN_SPLAT_LAYERS_5_8,
                         g_terrain_splat_base == 4 ? IDC_TERRAIN_SPLAT_LAYERS_5_8 : IDC_TERRAIN_SPLAT_LAYERS_1_4);
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK:
            g_terrain_splat_base =
                IsDlgButtonChecked(hdlg, IDC_TERRAIN_SPLAT_LAYERS_5_8) == BST_CHECKED ? 4 : 0;
            EndDialog(hdlg, IDOK);
            return TRUE;
        case IDCANCEL:
            EndDialog(hdlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// False when cancelled. A save dialog asks before overwriting and appends `def_ext`.
static bool terrain_pick_file(HWND owner, const char* title, const char* filter, char (&path)[MAX_PATH], bool save,
                              const char* def_ext = nullptr)
{
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = def_ext;
    ofn.Flags = (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST) | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrTitle = title;
    return (save ? alpine_get_save_file_name(&ofn) : alpine_get_open_file_name(&ofn)) != FALSE;
}

// ─── Dialog commands ────────────────────────────────────────────────────────

static void terrain_dlg_new_flat(HWND hdlg)
{
    const TerrainGrid& old = *g_terrain_dlg.data.grid;
    uint32_t nx = old.nx, nz = old.nz;
    if (!terrain_prompt_resolution(hdlg, "New Flat Terrain",
                                   "Resets the heights to Height Min and clears the layer weights, "
                                   "overlay and decoration coverage, holes and cell diagonals.",
                                   nx, nz)) {
        return;
    }
    if (!terrain_budget_allows(hdlg, g_terrain_dlg.terrain, nx, nz, old.weight_res_mul,
                               !g_terrain_dlg.data.overlays.empty(), g_terrain_dlg.data.decorations.size())) {
        return;
    }
    DedTerrainData next = g_terrain_dlg.data;
    next.grid = terrain_make_flat_grid(nx, nz, old.weight_res_mul);
    terrain_match_overlay_map(next);
    terrain_match_decoration_planes(next);
    g_terrain_dlg.data = std::move(next);
    terrain_dlg_update_readouts(hdlg);
    terrain_dlg_refresh_viewports();
}

static void terrain_dlg_import_heightmap(HWND hdlg)
{
    char path[MAX_PATH] = {};
    if (!terrain_pick_file(hdlg, "Import Heightmap",
                                "Heightmaps (*.png;*.r16;*.raw)\0*.png;*.r16;*.raw\0"
                                "All Files (*.*)\0*.*\0",
                                path, false)) {
        return;
    }

    std::vector<uint16_t> img;
    uint32_t w = 0, h = 0;
    std::string err;
    if (!terrain_load_heightmap(path, img, w, h, err)) {
        MessageBoxA(hdlg, err.c_str(), "Import Heightmap", MB_OK | MB_ICONWARNING);
        return;
    }

    // One vertex per pixel by default; an oversized image is proposed scaled down to fit.
    uint32_t nx = w, nz = h;
    const uint32_t longest = std::max(w, h);
    if (longest > at::max_verts) {
        const double scale = static_cast<double>(at::max_verts - 1) / (longest - 1);
        const auto scaled = [scale](uint32_t n) {
            return std::clamp<uint32_t>(static_cast<uint32_t>(std::lround((n - 1) * scale)) + 1, at::min_verts,
                                        at::max_verts);
        };
        nx = scaled(w);
        nz = scaled(h);
    }
    char info[160];
    std::snprintf(info, sizeof(info),
                  "The image is %u x %u. Black maps to Height Min, white to Height Min + Height Range.",
                  w, h);
    if (!terrain_prompt_resolution(hdlg, "Import Heightmap", info, nx, nz)) return;

    const TerrainGrid& old = *g_terrain_dlg.data.grid;
    if (!terrain_budget_allows(hdlg, g_terrain_dlg.terrain, nx, nz, old.weight_res_mul,
                               !g_terrain_dlg.data.overlays.empty(), g_terrain_dlg.data.decorations.size())) {
        return;
    }

    std::shared_ptr<TerrainGrid> g = terrain_grid_at_resolution(old, nx, nz);
    terrain_heights_from_image(*g, img, w, h);
    g_terrain_dlg.data.grid = std::move(g);
    terrain_dlg_update_readouts(hdlg);
    terrain_dlg_refresh_viewports();
}

static void terrain_dlg_export_heightmap(HWND hdlg)
{
    char path[MAX_PATH] = "heightmap.png";
    if (!terrain_pick_file(hdlg, "Export Heightmap", "16-bit PNG (*.png)\0*.png\0RAW16 (*.r16;*.raw)\0*.r16;*.raw\0",
                           path, true, "png")) {
        return;
    }

    const TerrainGrid& g = *g_terrain_dlg.data.grid;
    std::string err;
    if (!terrain_export_heightmap(path, g, err)) {
        MessageBoxA(hdlg, err.c_str(), "Export Heightmap", MB_OK | MB_ICONWARNING);
        return;
    }
    if (terrain_is_raw16_path(path) && g.nx != g.nz) {
        MessageBoxA(hdlg, "RAW16 files carry no dimensions and import back only as a square; this "
                          "terrain is not square.",
                    "Export Heightmap", MB_OK | MB_ICONINFORMATION);
    }
}

static void terrain_dlg_import_splat(HWND hdlg)
{
    char path[MAX_PATH] = {};
    if (!terrain_pick_file(hdlg, "Import Splat Map",
                           "Images (*.png;*.tga;*.bmp)\0*.png;*.tga;*.bmp\0All Files (*.*)\0*.*\0", path,
                           false)) {
        return;
    }
    if (DialogBoxParam(reinterpret_cast<HINSTANCE>(&__ImageBase), MAKEINTRESOURCE(IDD_ALPINE_TERRAIN_SPLAT),
                       hdlg, TerrainSplatDialogProc, 0) != IDOK) {
        return;
    }

    std::vector<uint8_t> bytes;
    int w = 0, h = 0, comp = 0;
    if (!terrain_read_file(path, bytes) ||
        !stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &comp)) {
        MessageBoxA(hdlg, "The image could not be read.", "Import Splat Map", MB_OK | MB_ICONWARNING);
        return;
    }
    if (w < 1 || h < 1 || w > static_cast<int>(terrain_max_image_edge) ||
        h > static_cast<int>(terrain_max_image_edge)) {
        const std::string msg =
            std::format("The image must be at most {0}x{0} pixels.", terrain_max_image_edge);
        MessageBoxA(hdlg, msg.c_str(), "Import Splat Map", MB_OK | MB_ICONWARNING);
        return;
    }
    std::unique_ptr<stbi_uc, void (*)(void*)> pixels{
        stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &comp, 4), stbi_image_free};
    if (!pixels) {
        MessageBoxA(hdlg, "The image could not be decoded.", "Import Splat Map", MB_OK | MB_ICONWARNING);
        return;
    }
    // Without an alpha channel (grey or RGB) the fourth layer of the block receives nothing.
    if (comp == 1 || comp == 3) {
        for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; i++) pixels.get()[i * 4 + 3] = 0;
    }

    DedTerrainData next = g_terrain_dlg.data;
    auto g = std::make_shared<TerrainGrid>(*next.grid);
    const int highest = terrain_apply_splat(*g, pixels.get(), static_cast<uint32_t>(w),
                                            static_cast<uint32_t>(h), g_terrain_splat_base);
    next.grid = std::move(g);
    terrain_grow_layers(next, highest);
    g_terrain_dlg.data = std::move(next);
    terrain_dlg_reselect_layer(hdlg, terrain_list_layers, g_terrain_dlg.sel[terrain_list_layers]);
    terrain_dlg_layers_changed(hdlg);
    terrain_dlg_update_readouts(hdlg);
    terrain_dlg_refresh_viewports();
}

static void terrain_dlg_set_weight_res(HWND hdlg)
{
    const LRESULT data = alpine_dlg_combo_data(hdlg, IDC_TERRAIN_WEIGHT_RES, CB_ERR);
    if (data == CB_ERR) return;
    const auto mul = static_cast<uint32_t>(data);
    const TerrainGrid& old = *g_terrain_dlg.data.grid;
    if (!at::is_allowed_weight_res_mul(mul) || mul == old.weight_res_mul) return;
    if (!terrain_budget_allows(hdlg, g_terrain_dlg.terrain, old.nx, old.nz, mul,
                               !g_terrain_dlg.data.overlays.empty(), g_terrain_dlg.data.decorations.size())) {
        alpine_dlg_combo_select(hdlg, IDC_TERRAIN_WEIGHT_RES, old.weight_res_mul);
        return;
    }
    g_terrain_dlg.data.grid = terrain_resample_grid(old, old.nx, old.nz, mul);
    terrain_dlg_update_readouts(hdlg);
    terrain_dlg_refresh_viewports();
}

static void terrain_dlg_layer_add(HWND hdlg, int kind)
{
    if (terrain_dlg_count(kind) >= terrain_list_ui[kind].max_count) return;
    DedTerrainData d = g_terrain_dlg.data;
    const TerrainGrid& g = *d.grid;
    if (kind == terrain_list_overlays) {
        if (d.overlays.empty() && !terrain_budget_allows(hdlg, g_terrain_dlg.terrain, g.nx, g.nz, g.weight_res_mul,
                                                         true, d.decorations.size())) {
            return;
        }
        d.overlays.emplace_back();
        terrain_match_overlay_map(d);
        g_terrain_dlg.data = std::move(d);
        terrain_dlg_update_readouts(hdlg);
    }
    else if (kind == terrain_list_decorations) {
        if (!terrain_budget_allows(hdlg, g_terrain_dlg.terrain, g.nx, g.nz, g.weight_res_mul, !d.overlays.empty(),
                                   d.decorations.size() + 1)) {
            return;
        }
        d.decorations.emplace_back();
        terrain_match_decoration_planes(d);
        g_terrain_dlg.data = std::move(d);
        terrain_dlg_update_readouts(hdlg);
    }
    else {
        d.layers.emplace_back();
        g_terrain_dlg.data = std::move(d);
        terrain_dlg_layers_changed(hdlg);
    }
    terrain_dlg_reselect_layer(hdlg, kind, terrain_dlg_count(kind) - 1);
    terrain_dlg_refresh_viewports();
}

static void terrain_dlg_layer_remove(HWND hdlg, int kind)
{
    const int sel = g_terrain_dlg.sel[kind];
    if (terrain_dlg_count(kind) <= terrain_list_ui[kind].min_count || sel < 0 || sel >= terrain_dlg_count(kind)) {
        return;
    }
    DedTerrainData d = g_terrain_dlg.data;
    if (kind == terrain_list_overlays) {
        terrain_remove_overlay_channel(terrain_mutable_grid(d), static_cast<std::size_t>(sel));
        d.overlays.erase(d.overlays.begin() + sel);
        terrain_match_overlay_map(d);
        g_terrain_dlg.data = std::move(d);
        terrain_dlg_update_readouts(hdlg);
    }
    else if (kind == terrain_list_decorations) {
        terrain_remove_decoration_plane(terrain_mutable_grid(d), static_cast<std::size_t>(sel));
        d.decorations.erase(d.decorations.begin() + sel);
        terrain_match_decoration_planes(d);
        g_terrain_dlg.data = std::move(d);
        terrain_dlg_update_readouts(hdlg);
    }
    else {
        // Its weight goes to the remaining layers in proportion; layer 0 takes texels left empty.
        TerrainGrid& g = terrain_mutable_grid(d);
        terrain_remove_weight_channel(g, static_cast<std::size_t>(sel));
        d.layers.erase(d.layers.begin() + sel);
        terrain_unlink_removed_layer(d, g, static_cast<std::size_t>(sel));
        g_terrain_dlg.data = std::move(d);
        terrain_dlg_layers_changed(hdlg);
        terrain_dlg_update_readouts(hdlg);
    }
    terrain_dlg_reselect_layer(hdlg, kind, std::min(sel, std::max(terrain_dlg_count(kind) - 1, 0)));
    terrain_dlg_refresh_viewports();
}

static void terrain_dlg_layer_move(HWND hdlg, int kind, int delta)
{
    const int a = g_terrain_dlg.sel[kind];
    const int b = a + delta;
    const int count = terrain_dlg_count(kind);
    if (a < 0 || b < 0 || a >= count || b >= count) return;
    const auto ca = static_cast<std::size_t>(a), cb = static_cast<std::size_t>(b);
    DedTerrainData d = g_terrain_dlg.data;
    if (kind == terrain_list_overlays) {
        terrain_swap_overlay_channels(terrain_mutable_grid(d), ca, cb);
        std::swap(d.overlays[ca], d.overlays[cb]);
    }
    else if (kind == terrain_list_decorations) {
        terrain_swap_decoration_planes(terrain_mutable_grid(d), ca, cb);
        std::swap(d.decorations[ca], d.decorations[cb]);
    }
    else {
        terrain_swap_weight_channels(terrain_mutable_grid(d), ca, cb);
        std::swap(d.layers[ca], d.layers[cb]);
        terrain_swap_layer_links(d, ca, cb);
    }
    g_terrain_dlg.data = std::move(d);
    if (kind == terrain_list_layers) terrain_dlg_layers_changed(hdlg);
    terrain_dlg_reselect_layer(hdlg, kind, b);
    terrain_dlg_refresh_viewports();
}

static void terrain_dlg_capture_shape(HWND hdlg)
{
    DedTerrainData& d = g_terrain_dlg.data;
    auto field = [hdlg](int idc, float shown) { return alpine_dlg_get_float_field_exact(hdlg, idc, shown); };
    d.cell_size = at::clamp_finite(field(IDC_TERRAIN_CELL_SIZE, d.cell_size), at::min_cell_size, at::max_cell_size,
                                   d.cell_size);
    d.height_min = at::clamp_finite(field(IDC_TERRAIN_HEIGHT_MIN, d.height_min), -at::max_coord, at::max_coord,
                                    d.height_min);
    d.height_range = at::clamp_finite(field(IDC_TERRAIN_HEIGHT_RANGE, d.height_range), at::min_height_range,
                                      at::max_height_range, d.height_range);
    d.chunk_cells = static_cast<uint32_t>(alpine_dlg_combo_data(hdlg, IDC_TERRAIN_CHUNK_SIZE, d.chunk_cells));
    // Geoable and skirted terrains cap the effective chunk size, so the readout follows the checkboxes.
    d.flags = static_cast<uint8_t>(d.flags & ~(at::flag_geoable | at::flag_skirts));
    if (IsDlgButtonChecked(hdlg, IDC_TERRAIN_GEOABLE) == BST_CHECKED) d.flags |= at::flag_geoable;
    if (IsDlgButtonChecked(hdlg, IDC_TERRAIN_SKIRTS) == BST_CHECKED) d.flags |= at::flag_skirts;
    d.thickness = at::clamp_finite(field(IDC_TERRAIN_THICKNESS, d.thickness), at::min_thickness, at::max_thickness,
                                   d.thickness);
    d.skirt_depth = at::clamp_finite(field(IDC_TERRAIN_SKIRT_DEPTH, d.skirt_depth), 0.0f, at::max_skirt_depth,
                                     d.skirt_depth);
}

static void terrain_dlg_store_decoration_fields(HWND hdlg)
{
    if (g_terrain_dlg.loading_layer) return;
    const int sel = g_terrain_dlg.sel[terrain_list_decorations];
    DedTerrainData& d = g_terrain_dlg.data;
    if (sel < 0 || static_cast<std::size_t>(sel) >= d.decorations.size()) return;
    DedTerrainDecoration& deco = d.decorations[sel];
    deco.mesh = terrain_get_text(hdlg, IDC_TERRAIN_DECO_MESH);
    for (const TerrainDecoField& f : terrain_deco_fields) {
        const float v = alpine_dlg_get_float_field_exact(hdlg, f.edit, deco.*f.value);
        if (std::isfinite(v)) deco.*f.value = std::clamp(v, f.lo, f.hi);
    }
    const LRESULT link = alpine_dlg_combo_data(hdlg, IDC_TERRAIN_DECO_LINK, deco.link_layer);
    const auto new_link = static_cast<uint8_t>(link >= 0 && link < static_cast<LRESULT>(d.layers.size())
                                                   ? link
                                                   : at::decoration_link_none);
    terrain_set_decoration_link(d, static_cast<std::size_t>(sel), new_link);
    for (const TerrainDecoCheck& c : terrain_deco_checks) {
        deco.*c.value = IsDlgButtonChecked(hdlg, c.check) == BST_CHECKED;
    }
    terrain_dlg_refresh_layer_row(hdlg, terrain_list_decorations, sel);
    terrain_dlg_update_readouts(hdlg);
    terrain_dlg_refresh_viewports();
}

static void terrain_dlg_browse_mesh(HWND hdlg)
{
    std::string name = terrain_get_text(hdlg, IDC_TERRAIN_DECO_MESH);
    // Static geometry only, so the browser offers no animation; the edit's EN_CHANGE stores the pick.
    if (alpine_browse_mesh(hdlg, name, ALPINE_MESH_V3M)) SetDlgItemTextA(hdlg, IDC_TERRAIN_DECO_MESH, name.c_str());
}

static void terrain_dlg_store_layer_fields(HWND hdlg, int kind)
{
    if (g_terrain_dlg.loading_layer) return;
    if (kind == terrain_list_decorations) {
        terrain_dlg_store_decoration_fields(hdlg);
        return;
    }
    const TerrainListUi& ui = terrain_list_ui[kind];
    const int sel = g_terrain_dlg.sel[kind];
    if (sel < 0 || sel >= terrain_dlg_count(kind)) return;
    DedTerrainLayer& layer = terrain_dlg_item(kind, sel);
    layer.texture = terrain_get_text(hdlg, ui.texture);
    const float uv = alpine_dlg_get_float_field_exact(hdlg, ui.uv_scale, layer.uv_scale);
    if (std::isfinite(uv)) layer.uv_scale = std::clamp(uv, at::min_uv_scale, at::max_uv_scale);
    layer.triplanar = IsDlgButtonChecked(hdlg, ui.triplanar) == BST_CHECKED;
    if (ui.break_tiling) {
        g_terrain_dlg.data.overlays[sel].break_tiling = IsDlgButtonChecked(hdlg, ui.break_tiling) == BST_CHECKED;
    }
    terrain_dlg_refresh_layer_row(hdlg, kind, sel);
    if (kind == terrain_list_layers) {
        const DedTerrainDecoration* deco = terrain_dlg_selected_decoration();
        terrain_dlg_fill_link_combo(hdlg, deco ? deco->link_layer : at::decoration_link_none);
    }
    terrain_dlg_refresh_viewports();
}

// A command from either list's controls; false when `id` is none of them.
static bool terrain_dlg_list_command(HWND hdlg, int id, int code)
{
    for (int kind = 0; kind < terrain_list_kinds; kind++) {
        const TerrainListUi& ui = terrain_list_ui[kind];
        if (id == ui.list) {
            if (code == LBN_SELCHANGE) {
                const LRESULT sel = SendDlgItemMessageA(hdlg, ui.list, LB_GETCURSEL, 0, 0);
                if (sel != LB_ERR) {
                    g_terrain_dlg.sel[kind] = static_cast<int>(sel);
                    terrain_dlg_load_layer_fields(hdlg, kind);
                }
            }
        }
        else if (ui.texture && id == ui.texture) {
            if (code == EN_CHANGE) {
                terrain_dlg_store_layer_fields(hdlg, kind);
                terrain_dlg_update_layer_preview(hdlg, kind, false);
            }
        }
        else if (ui.uv_scale && id == ui.uv_scale) {
            if (code == EN_CHANGE) terrain_dlg_store_layer_fields(hdlg, kind);
        }
        else if ((ui.triplanar && id == ui.triplanar) || (ui.break_tiling && id == ui.break_tiling)) {
            terrain_dlg_store_layer_fields(hdlg, kind);
        }
        else if (ui.browse && id == ui.browse) {
            // The edit's EN_CHANGE stores the pick into the layer.
            terrain_browse_texture(hdlg, ui.texture);
        }
        else if (id == ui.add) {
            terrain_dlg_layer_add(hdlg, kind);
        }
        else if (id == ui.remove) {
            terrain_dlg_layer_remove(hdlg, kind);
        }
        else if (id == ui.up || id == ui.down) {
            terrain_dlg_layer_move(hdlg, kind, id == ui.up ? -1 : 1);
        }
        else {
            continue;
        }
        return true;
    }
    return false;
}

static bool terrain_dlg_commit(HWND hdlg)
{
    DedTerrainData& d = g_terrain_dlg.data;
    terrain_dlg_capture_shape(hdlg);
    d.lightmap_density = static_cast<uint8_t>(std::clamp(alpine_dlg_get_int_field(hdlg, IDC_TERRAIN_LM_DENSITY),
                                                         static_cast<int>(at::lightmap_density_min),
                                                         static_cast<int>(at::lightmap_density_max)));
    d.flags = 0;
    if (IsDlgButtonChecked(hdlg, IDC_TERRAIN_GEOABLE) == BST_CHECKED) d.flags |= at::flag_geoable;
    if (IsDlgButtonChecked(hdlg, IDC_TERRAIN_SKIRTS) == BST_CHECKED) d.flags |= at::flag_skirts;
    d.fullbright = IsDlgButtonChecked(hdlg, IDC_TERRAIN_FULLBRIGHT) == BST_CHECKED;
    d.underside_texture = terrain_get_text(hdlg, IDC_TERRAIN_UNDERSIDE_TEXTURE);
    d.crater_texture = terrain_get_text(hdlg, IDC_TERRAIN_CRATER_TEXTURE);

    std::vector<std::string> names = {d.underside_texture, d.crater_texture};
    for (const auto& layer : d.layers) names.push_back(layer.texture);
    for (const auto& overlay : d.overlays) names.push_back(overlay.texture);
    for (const auto& name : names) {
        if (!at::texture_name_valid(name.c_str(), name.size())) {
            char msg[128];
            std::snprintf(msg, sizeof(msg), "'%s' is too long for a level texture name.", name.c_str());
            MessageBoxA(hdlg, msg, "Terrain", MB_OK | MB_ICONWARNING);
            return false;
        }
    }
    for (const auto& deco : d.decorations) {
        if (!at::decoration_mesh_valid(deco.mesh.c_str(), deco.mesh.size())) {
            const std::string msg = std::format("Decoration mesh '{}' must be a .v3m or .vfx file name without a "
                                                "folder, at most {} characters.",
                                                deco.mesh, at::max_texture_name_len);
            MessageBoxA(hdlg, msg.c_str(), "Terrain", MB_OK | MB_ICONWARNING);
            return false;
        }
    }
    terrain_clamp_properties(d);

    DedTerrain* terrain = g_terrain_dlg.terrain;
    std::string script_name = terrain_get_text(hdlg, IDC_TERRAIN_SCRIPT_NAME);
    if (script_name.size() > at::max_script_name_len) script_name.resize(at::max_script_name_len);
    DedTerrainData committed = d;
    terrain->script_name.assign_0(script_name.c_str());
    terrain->data = std::move(committed);
    return true;
}

// RED's 32-bit address space can run out on a large grid or image: the exception must not unwind into the
// dialog manager, and every edit either completes or leaves the staged data as it was.
template<typename F>
static void terrain_dlg_guard(HWND hdlg, const char* message, F&& edit, const char* caption = "Terrain")
{
    try {
        edit();
    }
    catch (const std::bad_alloc&) {
        MessageBoxA(hdlg, message, caption, MB_OK | MB_ICONWARNING);
    }
}

// ─── Generate Terrain ───────────────────────────────────────────────────────

namespace
{

// Fields shown as `scale` times the setting (percentages), within [lo, hi] as shown.
struct TerrainGenFloatField
{
    int edit, spin;
    float TerrainGenSettings::*value;
    float scale, step, lo, hi;
    int decimals;
};

constexpr TerrainGenFloatField terrain_gen_float_fields[] = {
    {IDC_TGEN_FEATURE, IDC_TGEN_FEATURE_SPIN, &TerrainGenSettings::feature_size, 1.0f, 4.0f, 1.0f, 65536.0f, 1},
    {IDC_TGEN_ROUGHNESS, IDC_TGEN_ROUGHNESS_SPIN, &TerrainGenSettings::roughness, 1.0f, 0.05f, 0.1f, 0.9f, 2},
    {IDC_TGEN_LACUNARITY, IDC_TGEN_LACUNARITY_SPIN, &TerrainGenSettings::lacunarity, 1.0f, 0.05f, 1.25f, 4.0f, 2},
    {IDC_TGEN_WARP, IDC_TGEN_WARP_SPIN, &TerrainGenSettings::warp, 1.0f, 0.05f, 0.0f, 2.0f, 2},
    {IDC_TGEN_EXPONENT, IDC_TGEN_EXPONENT_SPIN, &TerrainGenSettings::exponent, 1.0f, 0.05f, 0.2f, 5.0f, 2},
    {IDC_TGEN_OFFSET, IDC_TGEN_OFFSET_SPIN, &TerrainGenSettings::offset, 100.0f, 1.0f, -50.0f, 50.0f, 0},
    {IDC_TGEN_FALLOFF_WIDTH, IDC_TGEN_FALLOFF_WIDTH_SPIN, &TerrainGenSettings::falloff_width, 100.0f, 1.0f, 2.0f,
     100.0f, 0},
    {IDC_TGEN_TALUS, IDC_TGEN_TALUS_SPIN, &TerrainGenSettings::talus_deg, 1.0f, 1.0f, 5.0f, 75.0f, 1},
    {IDC_TGEN_STRENGTH, IDC_TGEN_STRENGTH_SPIN, &TerrainGenSettings::erosion_strength, 100.0f, 5.0f, 0.0f, 100.0f, 0},
    {IDC_TGEN_SLOPE, IDC_TGEN_SLOPE_SPIN, &TerrainGenSettings::slope_deg, 1.0f, 1.0f, 0.0f, 89.0f, 1},
    {IDC_TGEN_SLOPE_BLEND, IDC_TGEN_SLOPE_BLEND_SPIN, &TerrainGenSettings::slope_blend_deg, 1.0f, 1.0f, 0.0f, 45.0f, 1},
    {IDC_TGEN_HIGH, IDC_TGEN_HIGH_SPIN, &TerrainGenSettings::high_start, 100.0f, 1.0f, 0.0f, 100.0f, 0},
    {IDC_TGEN_HIGH_BLEND, IDC_TGEN_HIGH_BLEND_SPIN, &TerrainGenSettings::high_blend, 100.0f, 1.0f, 0.0f, 100.0f, 0},
    {IDC_TGEN_VARIATION, IDC_TGEN_VARIATION_SPIN, &TerrainGenSettings::variation, 100.0f, 5.0f, 0.0f, 100.0f, 0},
    {IDC_TGEN_VARIATION_SIZE, IDC_TGEN_VARIATION_SIZE_SPIN, &TerrainGenSettings::variation_size, 1.0f, 1.0f, 1.0f,
     65536.0f, 1},
    {IDC_TGEN_RIDGE, IDC_TGEN_RIDGE_SPIN, &TerrainGenSettings::ridge_emphasis, 100.0f, 5.0f, 0.0f, 100.0f, 0},
};

struct TerrainGenIntField
{
    int edit, spin;
    int TerrainGenSettings::*value;
    int step, lo, hi;
};

constexpr TerrainGenIntField terrain_gen_int_fields[] = {
    {IDC_TGEN_OCTAVES, IDC_TGEN_OCTAVES_SPIN, &TerrainGenSettings::octaves, 1, 1, 10},
    {IDC_TGEN_TERRACES, IDC_TGEN_TERRACES_SPIN, &TerrainGenSettings::terraces, 1, 0, 64},
    {IDC_TGEN_THERMAL, IDC_TGEN_THERMAL_SPIN, &TerrainGenSettings::thermal_iterations, 5, 0, 100},
    {IDC_TGEN_DROPLETS, IDC_TGEN_DROPLETS_SPIN, &TerrainGenSettings::droplets, 5000, 0, 100000},
    {IDC_TGEN_SMOOTH, IDC_TGEN_SMOOTH_SPIN, &TerrainGenSettings::smooth_passes, 1, 0, 10},
};

constexpr int terrain_gen_splat_controls[] = {
    IDC_TGEN_SPLAT_LAYERS,        IDC_TGEN_SLOPE,     IDC_TGEN_SLOPE_SPIN,     IDC_TGEN_SLOPE_BLEND,
    IDC_TGEN_SLOPE_BLEND_SPIN,    IDC_TGEN_HIGH,      IDC_TGEN_HIGH_SPIN,      IDC_TGEN_HIGH_BLEND,
    IDC_TGEN_HIGH_BLEND_SPIN,     IDC_TGEN_VARIATION, IDC_TGEN_VARIATION_SPIN, IDC_TGEN_VARIATION_SIZE,
    IDC_TGEN_VARIATION_SIZE_SPIN, IDC_TGEN_RIDGE,     IDC_TGEN_RIDGE_SPIN,     IDC_TGEN_PREVIEW_SPLAT};

// Option changes regenerate once typing pauses.
constexpr UINT_PTR terrain_gen_timer = 1;
constexpr UINT terrain_gen_delay_ms = 250;

struct TerrainGenerateState
{
    // The options last used, kept for the session; sizes default from the first terrain's extent.
    TerrainGenSettings settings;
    bool sized = false;
    int splat_base = 0;
    TerrainGenPreview preview = TerrainGenPreview::shaded;

    // The open dialog: its grid and height mapping, and the result OK applies.
    uint32_t nx = 0, nz = 0;
    float height_min = 0.0f, height_range = at::default_height_range;
    bool pending = false;
    bool result_valid = false;
    TerrainGenResult result;
    std::vector<uint32_t> pixels;
    int pixels_size = 0;
};
TerrainGenerateState g_terrain_gen;

} // namespace

static void terrain_gen_default_sizes(TerrainGenSettings& s)
{
    const TerrainGrid& g = *g_terrain_dlg.data.grid;
    const float extent =
        std::max(at::extent(g.nx, g_terrain_dlg.data.cell_size), at::extent(g.nz, g_terrain_dlg.data.cell_size));
    s.feature_size = std::max(std::round(extent * 0.5f), 1.0f);
    s.variation_size = std::max(std::round(extent * 0.1f), 1.0f);
}

static void terrain_gen_load_fields(HWND hdlg)
{
    const TerrainGenSettings& s = g_terrain_gen.settings;
    SetDlgItemInt(hdlg, IDC_TGEN_NX, g_terrain_gen.nx, FALSE);
    SetDlgItemInt(hdlg, IDC_TGEN_NZ, g_terrain_gen.nz, FALSE);
    alpine_dlg_set_float_field_exact(hdlg, IDC_TGEN_HEIGHT_MIN, g_terrain_gen.height_min);
    alpine_dlg_set_float_field_exact(hdlg, IDC_TGEN_HEIGHT_RANGE, g_terrain_gen.height_range);
    alpine_dlg_combo_select(hdlg, IDC_TGEN_TYPE, static_cast<LPARAM>(s.type));
    SetDlgItemTextA(hdlg, IDC_TGEN_SEED, std::to_string(s.seed).c_str());
    for (const TerrainGenFloatField& f : terrain_gen_float_fields) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.6g", static_cast<double>(s.*f.value * f.scale));
        SetDlgItemTextA(hdlg, f.edit, buf);
    }
    for (const TerrainGenIntField& f : terrain_gen_int_fields) SetDlgItemInt(hdlg, f.edit, s.*f.value, TRUE);
    alpine_dlg_combo_select(hdlg, IDC_TGEN_FALLOFF, static_cast<LPARAM>(s.falloff));
    CheckDlgButton(hdlg, IDC_TGEN_SPLAT, s.splat ? BST_CHECKED : BST_UNCHECKED);
    alpine_dlg_combo_select(hdlg, IDC_TGEN_SPLAT_LAYERS, g_terrain_gen.splat_base);
}

static void terrain_gen_read_fields(HWND hdlg)
{
    TerrainGenSettings& s = g_terrain_gen.settings;
    const auto verts = [&](int idc) {
        return static_cast<uint32_t>(std::clamp(alpine_dlg_get_int_field(hdlg, idc), static_cast<int>(at::min_verts),
                                                static_cast<int>(at::max_verts)));
    };
    g_terrain_gen.nx = verts(IDC_TGEN_NX);
    g_terrain_gen.nz = verts(IDC_TGEN_NZ);
    g_terrain_gen.height_min =
        at::clamp_finite(alpine_dlg_get_float_field_exact(hdlg, IDC_TGEN_HEIGHT_MIN, g_terrain_gen.height_min),
                         -at::max_coord, at::max_coord, g_terrain_gen.height_min);
    g_terrain_gen.height_range =
        at::clamp_finite(alpine_dlg_get_float_field_exact(hdlg, IDC_TGEN_HEIGHT_RANGE, g_terrain_gen.height_range),
                         at::min_height_range, at::max_height_range, g_terrain_gen.height_range);
    s.type = static_cast<TerrainNoiseType>(alpine_dlg_combo_data(hdlg, IDC_TGEN_TYPE, static_cast<LRESULT>(s.type)));
    s.seed = static_cast<uint32_t>(std::strtoul(terrain_get_text(hdlg, IDC_TGEN_SEED).c_str(), nullptr, 10));
    for (const TerrainGenFloatField& f : terrain_gen_float_fields) {
        const float shown = alpine_dlg_get_float_field(hdlg, f.edit);
        if (std::isfinite(shown)) {
            s.*f.value = std::clamp(shown, f.lo, f.hi) / f.scale;
        }
    }
    for (const TerrainGenIntField& f : terrain_gen_int_fields) {
        s.*f.value = std::clamp(alpine_dlg_get_int_field(hdlg, f.edit), f.lo, f.hi);
    }
    s.falloff =
        static_cast<TerrainEdgeFalloff>(alpine_dlg_combo_data(hdlg, IDC_TGEN_FALLOFF, static_cast<LRESULT>(s.falloff)));
    s.splat = IsDlgButtonChecked(hdlg, IDC_TGEN_SPLAT) == BST_CHECKED;
    g_terrain_gen.splat_base = alpine_dlg_combo_data(hdlg, IDC_TGEN_SPLAT_LAYERS, 0) == 4 ? 4 : 0;
}

static void terrain_gen_update_state(HWND hdlg)
{
    const bool splat = IsDlgButtonChecked(hdlg, IDC_TGEN_SPLAT) == BST_CHECKED;
    for (int id : terrain_gen_splat_controls) EnableWindow(GetDlgItem(hdlg, id), splat);
    if (!splat && g_terrain_gen.preview == TerrainGenPreview::splat) {
        g_terrain_gen.preview = TerrainGenPreview::shaded;
    }
    const int preview_ids[] = {IDC_TGEN_PREVIEW_HEIGHT, IDC_TGEN_PREVIEW_SHADED, IDC_TGEN_PREVIEW_SPLAT};
    CheckRadioButton(hdlg, IDC_TGEN_PREVIEW_HEIGHT, IDC_TGEN_PREVIEW_SPLAT,
                     preview_ids[static_cast<int>(g_terrain_gen.preview)]);
}

static void terrain_gen_render_preview(HWND hdlg)
{
    HWND ctrl = GetDlgItem(hdlg, IDC_TGEN_PREVIEW);
    RECT rc{};
    GetClientRect(ctrl, &rc);
    g_terrain_gen.pixels_size = std::max<int>(std::min(rc.right - rc.left, rc.bottom - rc.top), 0);
    if (g_terrain_gen.result_valid) {
        terrain_generate_preview(g_terrain_gen.result, g_terrain_gen.preview, g_terrain_gen.pixels_size,
                                 g_terrain_gen.pixels);
    }
    else {
        g_terrain_gen.pixels.clear();
    }
    InvalidateRect(ctrl, nullptr, TRUE);
}

static void terrain_gen_draw_preview(const DRAWITEMSTRUCT& dis)
{
    FillRect(dis.hDC, &dis.rcItem, GetSysColorBrush(COLOR_BTNFACE));
    const int size = g_terrain_gen.pixels_size;
    if (size <= 0 || g_terrain_gen.pixels.size() != static_cast<std::size_t>(size) * size) return;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    const int x = dis.rcItem.left + (dis.rcItem.right - dis.rcItem.left - size) / 2;
    const int y = dis.rcItem.top + (dis.rcItem.bottom - dis.rcItem.top - size) / 2;
    SetDIBitsToDevice(dis.hDC, x, y, size, size, 0, 0, 0, size, g_terrain_gen.pixels.data(), &info, DIB_RGB_COLORS);
}

static void terrain_gen_regenerate(HWND hdlg)
{
    KillTimer(hdlg, terrain_gen_timer);
    g_terrain_gen.pending = false;
    terrain_gen_read_fields(hdlg);
    const TerrainGenSettings& s = g_terrain_gen.settings;
    const float cell_size = g_terrain_dlg.data.cell_size;
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%.3g cells", s.feature_size / cell_size);
    SetDlgItemTextA(hdlg, IDC_TGEN_FEATURE_INFO, buf);

    TerrainGenInput in;
    in.nx = g_terrain_gen.nx;
    in.nz = g_terrain_gen.nz;
    in.weight_res_mul = g_terrain_dlg.data.grid->weight_res_mul;
    in.cell_size = cell_size;
    in.height_range = g_terrain_gen.height_range;
    HCURSOR cursor = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    const auto start = std::chrono::steady_clock::now();
    g_terrain_gen.result_valid = false;
    try {
        terrain_generate(s, in, g_terrain_gen.result);
        g_terrain_gen.result_valid = true;
    }
    catch (const std::bad_alloc&) {
        g_terrain_gen.result = {};
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    SetCursor(cursor);

    const TerrainGenResult& r = g_terrain_gen.result;
    if (!g_terrain_gen.result_valid) {
        std::snprintf(buf, sizeof(buf), "There is not enough memory to generate this terrain.");
    }
    else {
        int n = std::snprintf(buf, sizeof(buf),
                              "Heights span %.6g to %.6g (Height Min to Height Min + Height Range).\n\n"
                              "Slope: mean %.1f deg, max %.1f deg; %.0f%% steeper than %.4g deg.\n\n",
                              g_terrain_gen.height_min, g_terrain_gen.height_min + g_terrain_gen.height_range,
                              r.slope_mean_deg, r.slope_max_deg, r.steep_share * 100.0f, s.slope_deg);
        if (!r.splat.empty()) {
            const int first = g_terrain_gen.splat_base + 1;
            n += std::snprintf(buf + n, sizeof(buf) - n,
                               "Layers %d-%d: base %.0f%%, slope %.0f%%, high %.0f%%, variation %.0f%%.\n\n", first,
                               first + 3, r.coverage[terrain_gen_base] * 100.0f, r.coverage[terrain_gen_slope] * 100.0f,
                               r.coverage[terrain_gen_high] * 100.0f, r.coverage[terrain_gen_variation] * 100.0f);
        }
        std::snprintf(buf + n, sizeof(buf) - n, "%u x %u vertices, generated in %lld ms.", r.nx, r.nz,
                      static_cast<long long>(ms.count()));
    }
    SetDlgItemTextA(hdlg, IDC_TGEN_STATS, buf);
    terrain_gen_render_preview(hdlg);
}

// Writes the result into the staged terrain: the grid is resampled to the new resolution and its heights
// replaced, as heightmap import does, and Height Min, Height Range and any splat map are applied as well.
// Holes, diagonals, overlay coverage and decoration planes are kept.
static bool terrain_gen_apply(HWND hdlg)
{
    if (!terrain_dlg_vertex_counts_valid(hdlg, IDC_TGEN_NX, IDC_TGEN_NZ)) return false;
    if (g_terrain_gen.pending || !g_terrain_gen.result_valid) {
        terrain_gen_regenerate(hdlg);
    }
    if (!g_terrain_gen.result_valid) return false;
    const TerrainGenResult& r = g_terrain_gen.result;
    const TerrainGrid& old = *g_terrain_dlg.data.grid;
    if (!terrain_budget_allows(hdlg, g_terrain_dlg.terrain, r.nx, r.nz, old.weight_res_mul,
                               !g_terrain_dlg.data.overlays.empty(), g_terrain_dlg.data.decorations.size())) {
        return false;
    }
    DedTerrainData next = g_terrain_dlg.data;
    std::shared_ptr<TerrainGrid> g = terrain_grid_at_resolution(old, r.nx, r.nz);
    g->heights = r.heights;
    int highest = -1;
    if (!r.splat.empty()) {
        highest = terrain_apply_splat(*g, r.splat.data(), at::weight_width(r.nx, r.weight_res_mul),
                                      at::weight_height(r.nz, r.weight_res_mul), g_terrain_gen.splat_base);
    }
    next.grid = std::move(g);
    next.height_min = g_terrain_gen.height_min;
    next.height_range = g_terrain_gen.height_range;
    terrain_grow_layers(next, highest);
    terrain_match_overlay_map(next);
    terrain_match_decoration_planes(next);
    g_terrain_dlg.data = std::move(next);
    return true;
}

static INT_PTR CALLBACK TerrainGenerateDialogProc(HWND hdlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        alpine_center_dialog_on_owner(hdlg);
        const TerrainGrid& g = *g_terrain_dlg.data.grid;
        g_terrain_gen.nx = g.nx;
        g_terrain_gen.nz = g.nz;
        g_terrain_gen.height_min = g_terrain_dlg.data.height_min;
        g_terrain_gen.height_range = g_terrain_dlg.data.height_range;
        if (!g_terrain_gen.sized) {
            terrain_gen_default_sizes(g_terrain_gen.settings);
            g_terrain_gen.sized = true;
        }
        alpine_dlg_combo_add(hdlg, IDC_TGEN_TYPE, "fBm (hills)", static_cast<LPARAM>(TerrainNoiseType::fbm));
        alpine_dlg_combo_add(hdlg, IDC_TGEN_TYPE, "Ridged", static_cast<LPARAM>(TerrainNoiseType::ridged));
        alpine_dlg_combo_add(hdlg, IDC_TGEN_TYPE, "Billow", static_cast<LPARAM>(TerrainNoiseType::billow));
        alpine_dlg_combo_add(hdlg, IDC_TGEN_TYPE, "Hybrid", static_cast<LPARAM>(TerrainNoiseType::hybrid));
        alpine_dlg_combo_add(hdlg, IDC_TGEN_FALLOFF, "None", static_cast<LPARAM>(TerrainEdgeFalloff::none));
        alpine_dlg_combo_add(hdlg, IDC_TGEN_FALLOFF, "Island", static_cast<LPARAM>(TerrainEdgeFalloff::island));
        alpine_dlg_combo_add(hdlg, IDC_TGEN_FALLOFF, "Basin", static_cast<LPARAM>(TerrainEdgeFalloff::basin));
        alpine_dlg_combo_add(hdlg, IDC_TGEN_SPLAT_LAYERS, "Layers 1-4", 0);
        alpine_dlg_combo_add(hdlg, IDC_TGEN_SPLAT_LAYERS, "Layers 5-8", 4);
        SendDlgItemMessageA(hdlg, IDC_TGEN_SEED, EM_LIMITTEXT, 10, 0);
        alpine_spinner_init_int(hdlg, IDC_TGEN_NX, IDC_TGEN_NX_SPIN, 1, static_cast<int>(at::min_verts),
                                static_cast<int>(at::max_verts));
        alpine_spinner_init_int(hdlg, IDC_TGEN_NZ, IDC_TGEN_NZ_SPIN, 1, static_cast<int>(at::min_verts),
                                static_cast<int>(at::max_verts));
        alpine_spinner_init(hdlg, IDC_TGEN_HEIGHT_MIN, IDC_TGEN_HEIGHT_MIN_SPIN, 1.0f, -at::max_coord, at::max_coord,
                            2);
        alpine_spinner_init(hdlg, IDC_TGEN_HEIGHT_RANGE, IDC_TGEN_HEIGHT_RANGE_SPIN, 1.0f, at::min_height_range,
                            at::max_height_range, 2);
        for (const TerrainGenFloatField& f : terrain_gen_float_fields) {
            alpine_spinner_init(hdlg, f.edit, f.spin, f.step, f.lo, f.hi, f.decimals);
        }
        for (const TerrainGenIntField& f : terrain_gen_int_fields) {
            alpine_spinner_init_int(hdlg, f.edit, f.spin, f.step, f.lo, f.hi);
        }
        terrain_gen_load_fields(hdlg);
        terrain_gen_update_state(hdlg);
        terrain_dlg_guard(hdlg, "There is not enough memory to generate this terrain.",
                          [&] { terrain_gen_regenerate(hdlg); });
        return TRUE;
    }
    case WM_TIMER:
        if (wp != terrain_gen_timer) break;
        terrain_dlg_guard(hdlg, "There is not enough memory to generate this terrain.",
                          [&] { terrain_gen_regenerate(hdlg); });
        return TRUE;
    case WM_COMMAND: {
        const int id = LOWORD(wp), code = HIWORD(wp);
        switch (id) {
        case IDOK: {
            bool applied = false;
            terrain_dlg_guard(hdlg, "There is not enough memory to generate this terrain; it was left as it was.",
                              [&] { applied = terrain_gen_apply(hdlg); });
            if (applied) {
                EndDialog(hdlg, IDOK);
            }
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(hdlg, IDCANCEL);
            return TRUE;
        case IDC_TGEN_RANDOMIZE:
            SetDlgItemTextA(hdlg, IDC_TGEN_SEED, std::to_string(std::random_device{}()).c_str());
            return TRUE;
        case IDC_TGEN_DEFAULTS:
            g_terrain_gen.settings = {};
            terrain_gen_default_sizes(g_terrain_gen.settings);
            g_terrain_gen.splat_base = 0;
            terrain_gen_load_fields(hdlg);
            terrain_gen_update_state(hdlg);
            return TRUE;
        case IDC_TGEN_PREVIEW_HEIGHT:
        case IDC_TGEN_PREVIEW_SHADED:
        case IDC_TGEN_PREVIEW_SPLAT:
            g_terrain_gen.preview = static_cast<TerrainGenPreview>(id - IDC_TGEN_PREVIEW_HEIGHT);
            terrain_dlg_guard(hdlg, "There is not enough memory to draw the preview.",
                              [&] { terrain_gen_render_preview(hdlg); });
            return TRUE;
        case IDC_TGEN_SPLAT:
            terrain_gen_update_state(hdlg);
            break;
        }
        if (code == EN_CHANGE || code == CBN_SELCHANGE || code == BN_CLICKED) {
            g_terrain_gen.pending = true;
            SetTimer(hdlg, terrain_gen_timer, terrain_gen_delay_ms, nullptr);
        }
        return TRUE;
    }
    case WM_NOTIFY:
        if (alpine_spinner_handle_notify(hdlg, lp)) return TRUE;
        break;
    case WM_DRAWITEM: {
        const auto* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
        if (!dis || static_cast<int>(dis->CtlID) != IDC_TGEN_PREVIEW) break;
        terrain_gen_draw_preview(*dis);
        return TRUE;
    }
    }
    return FALSE;
}

// Generate opens over Terrain Properties and, like the imports, changes only the staged copy.
static void terrain_dlg_generate(HWND hdlg)
{
    const INT_PTR result =
        DialogBoxParam(reinterpret_cast<HINSTANCE>(&__ImageBase), MAKEINTRESOURCE(IDD_ALPINE_TERRAIN_GENERATE), hdlg,
                       TerrainGenerateDialogProc, 0);
    g_terrain_gen.result = {};
    g_terrain_gen.result_valid = false;
    std::vector<uint32_t>().swap(g_terrain_gen.pixels);
    if (result != IDOK) return;
    // Each field's EN_CHANGE re-stages both from their text, so both values are taken before either is shown.
    const float height_min = g_terrain_dlg.data.height_min;
    const float height_range = g_terrain_dlg.data.height_range;
    alpine_dlg_set_float_field_exact(hdlg, IDC_TERRAIN_HEIGHT_MIN, height_min);
    alpine_dlg_set_float_field_exact(hdlg, IDC_TERRAIN_HEIGHT_RANGE, height_range);
    terrain_dlg_reselect_layer(hdlg, terrain_list_layers, g_terrain_dlg.sel[terrain_list_layers]);
    terrain_dlg_layers_changed(hdlg);
    terrain_dlg_update_readouts(hdlg);
    terrain_dlg_refresh_viewports();
}

static constexpr const char* terrain_import_out_of_memory = "There is not enough memory to import this file.";

static INT_PTR terrain_dlg_command(HWND hdlg, WPARAM wp)
{
    switch (LOWORD(wp)) {
    case IDC_TERRAIN_CELL_SIZE:
    case IDC_TERRAIN_HEIGHT_MIN:
    case IDC_TERRAIN_HEIGHT_RANGE:
    case IDC_TERRAIN_THICKNESS:
    case IDC_TERRAIN_SKIRT_DEPTH:
        if (HIWORD(wp) == EN_CHANGE && g_terrain_dlg.active) {
            terrain_dlg_capture_shape(hdlg);
            terrain_dlg_update_readouts(hdlg);
            terrain_dlg_refresh_viewports();
        }
        break;
    case IDC_TERRAIN_CHUNK_SIZE:
        if (HIWORD(wp) == CBN_SELCHANGE) {
            terrain_dlg_capture_shape(hdlg);
            terrain_dlg_update_readouts(hdlg);
            terrain_dlg_refresh_viewports();
        }
        break;
    case IDC_TERRAIN_WEIGHT_RES:
        if (HIWORD(wp) == CBN_SELCHANGE) terrain_dlg_set_weight_res(hdlg);
        break;
    case IDC_TERRAIN_GEOABLE:
        // Geoable terrains default to finer chunks, which keep each carve's boolean small.
        if (IsDlgButtonChecked(hdlg, IDC_TERRAIN_GEOABLE) == BST_CHECKED &&
            alpine_dlg_combo_data(hdlg, IDC_TERRAIN_CHUNK_SIZE, 0) == at::default_chunk_cells) {
            alpine_dlg_combo_select(hdlg, IDC_TERRAIN_CHUNK_SIZE, at::default_geoable_chunk_cells);
        }
        terrain_dlg_capture_shape(hdlg);
        terrain_dlg_update_readouts(hdlg);
        terrain_dlg_update_state(hdlg);
        terrain_dlg_refresh_viewports();
        return TRUE;
    case IDC_TERRAIN_SKIRTS:
        terrain_dlg_capture_shape(hdlg);
        terrain_dlg_update_readouts(hdlg);
        terrain_dlg_update_state(hdlg);
        terrain_dlg_refresh_viewports();
        return TRUE;
    case IDC_TERRAIN_FULLBRIGHT:
        g_terrain_dlg.data.fullbright = IsDlgButtonChecked(hdlg, IDC_TERRAIN_FULLBRIGHT) == BST_CHECKED;
        terrain_dlg_update_state(hdlg);
        terrain_dlg_refresh_viewports();
        return TRUE;
    case IDC_TERRAIN_UNDERSIDE_TEXTURE:
        if (HIWORD(wp) == EN_CHANGE && g_terrain_dlg.active) {
            g_terrain_dlg.data.underside_texture = terrain_get_text(hdlg, IDC_TERRAIN_UNDERSIDE_TEXTURE);
            terrain_dlg_refresh_viewports();
        }
        break;
    case IDC_TERRAIN_UNDERSIDE_BROWSE:
        terrain_browse_texture(hdlg, IDC_TERRAIN_UNDERSIDE_TEXTURE);
        return TRUE;
    case IDC_TERRAIN_CRATER_BROWSE:
        terrain_browse_texture(hdlg, IDC_TERRAIN_CRATER_TEXTURE);
        return TRUE;
    case IDC_TERRAIN_DECO_MESH:
    case IDC_TERRAIN_DECO_DENSITY:
    case IDC_TERRAIN_DECO_SCALE_MIN:
    case IDC_TERRAIN_DECO_SCALE_MAX:
    case IDC_TERRAIN_DECO_SLOPE:
    case IDC_TERRAIN_DECO_DRAW_DIST:
    case IDC_TERRAIN_DECO_OFFSET:
        if (HIWORD(wp) == EN_CHANGE && g_terrain_dlg.active) terrain_dlg_store_decoration_fields(hdlg);
        break;
    case IDC_TERRAIN_DECO_LINK:
        if (HIWORD(wp) == CBN_SELCHANGE && g_terrain_dlg.active) terrain_dlg_store_decoration_fields(hdlg);
        break;
    case IDC_TERRAIN_DECO_ALIGN:
    case IDC_TERRAIN_DECO_RANDOM_YAW:
    case IDC_TERRAIN_DECO_CASTS_SHADOWS:
        terrain_dlg_store_decoration_fields(hdlg);
        return TRUE;
    case IDC_TERRAIN_DECO_MESH_BROWSE:
        terrain_dlg_browse_mesh(hdlg);
        return TRUE;
    case IDC_TERRAIN_NEW_FLAT:
        terrain_dlg_new_flat(hdlg);
        return TRUE;
    case IDC_TERRAIN_GENERATE:
        terrain_dlg_generate(hdlg);
        return TRUE;
    case IDC_TERRAIN_IMPORT_HEIGHTMAP:
        terrain_dlg_guard(hdlg, terrain_import_out_of_memory, [&] { terrain_dlg_import_heightmap(hdlg); },
                          "Import Heightmap");
        return TRUE;
    case IDC_TERRAIN_EXPORT_HEIGHTMAP:
        terrain_dlg_export_heightmap(hdlg);
        return TRUE;
    case IDC_TERRAIN_IMPORT_SPLAT:
        terrain_dlg_guard(hdlg, terrain_import_out_of_memory, [&] { terrain_dlg_import_splat(hdlg); },
                          "Import Splat Map");
        return TRUE;
    case IDOK:
    case IDC_TERRAIN_TOOLS:
    case IDC_TERRAIN_CONVERT:
        // Tools and Convert apply the dialog's changes as OK does, then run once it is closed.
        if (terrain_dlg_commit(hdlg)) EndDialog(hdlg, LOWORD(wp));
        return TRUE;
    case IDCANCEL:
        EndDialog(hdlg, IDCANCEL);
        return TRUE;
    default:
        if (terrain_dlg_list_command(hdlg, LOWORD(wp), HIWORD(wp))) return TRUE;
        break;
    }
    return FALSE;
}

static constexpr const char* terrain_tip_cell_size = "Distance between height samples (m).";
static constexpr const char* terrain_tip_tile = "World size (m) of one texture repeat.";
static constexpr const char* terrain_tip_triplanar =
    "Projects from 3 axes; no stretching on steep slopes.";
static constexpr const char* terrain_tip_weights = "Paint texels per cell edge, for layers and overlays.";
static constexpr const char* terrain_tip_chunk_size =
    "Cells per chunk edge, one room each; smaller = cheaper craters, more rooms.";
static constexpr const char* terrain_tip_lightmap = "Baked light texels per cell edge.";
static constexpr const char* terrain_tip_thickness =
    "Solid depth below Height Min; keep it deeper than craters or they punch through.";
static constexpr const char* terrain_tip_skirt_depth = "How far skirts hang below the edges and holes.";
static constexpr const char* terrain_tip_deco_density = "Instances per m2 where coverage is full.";
static constexpr const char* terrain_tip_deco_scale = "Each instance's size is picked at random in this range.";
static constexpr const char* terrain_tip_deco_link = "Also scaled by this texture layer's painted weight.";
static constexpr const char* terrain_tip_deco_slope = "No instances on ground steeper than this (degrees).";
static constexpr const char* terrain_tip_deco_draw_dist = "Instances fade out by this distance (m).";
static constexpr const char* terrain_tip_deco_offset =
    "Raises each mesh along its up axis (mesh units, scaled). For meshes centred on their origin.";

static constexpr DialogTooltip terrain_dlg_tooltips[] = {
    {IDC_TERRAIN_CELL_SIZE, terrain_tip_cell_size},
    {IDC_TERRAIN_CELL_SIZE_LABEL, terrain_tip_cell_size},
    {IDC_TERRAIN_CHUNK_SIZE, terrain_tip_chunk_size},
    {IDC_TERRAIN_CHUNK_SIZE_LABEL, terrain_tip_chunk_size},
    {IDC_TERRAIN_LM_DENSITY, terrain_tip_lightmap},
    {IDC_TERRAIN_LM_DENSITY_LABEL, terrain_tip_lightmap},
    {IDC_TERRAIN_FULLBRIGHT,
     "Draws the textures at full brightness, ignoring ambient, sun and baked lighting. Not baked by Calculate "
     "Lighting."},
    {IDC_TERRAIN_THICKNESS, terrain_tip_thickness},
    {IDC_TERRAIN_THICKNESS_LABEL, terrain_tip_thickness},
    {IDC_TERRAIN_SKIRT_DEPTH, terrain_tip_skirt_depth},
    {IDC_TERRAIN_SKIRT_DEPTH_LABEL, terrain_tip_skirt_depth},
    {IDC_TERRAIN_LAYER_UV_SCALE, terrain_tip_tile},
    {IDC_TERRAIN_LAYER_UV_SCALE_LABEL, terrain_tip_tile},
    {IDC_TERRAIN_LAYER_TRIPLANAR, terrain_tip_triplanar},
    {IDC_TERRAIN_WEIGHT_RES, terrain_tip_weights},
    {IDC_TERRAIN_WEIGHT_RES_LABEL, terrain_tip_weights},
    {IDC_TERRAIN_OVERLAY_UV_SCALE, terrain_tip_tile},
    {IDC_TERRAIN_OVERLAY_UV_SCALE_LABEL, terrain_tip_tile},
    {IDC_TERRAIN_OVERLAY_TRIPLANAR, terrain_tip_triplanar},
    {IDC_TERRAIN_OVERLAY_BREAK_TILING, "Rotates and shifts each repeat to hide tiling."},
    {IDC_TERRAIN_DECO_DENSITY, terrain_tip_deco_density},
    {IDC_TERRAIN_DECO_DENSITY_LABEL, terrain_tip_deco_density},
    {IDC_TERRAIN_DECO_SCALE_LABEL, terrain_tip_deco_scale},
    {IDC_TERRAIN_DECO_SCALE_MIN, terrain_tip_deco_scale},
    {IDC_TERRAIN_DECO_SCALE_MAX, terrain_tip_deco_scale},
    {IDC_TERRAIN_DECO_LINK, terrain_tip_deco_link},
    {IDC_TERRAIN_DECO_LINK_LABEL, terrain_tip_deco_link},
    {IDC_TERRAIN_DECO_SLOPE, terrain_tip_deco_slope},
    {IDC_TERRAIN_DECO_SLOPE_LABEL, terrain_tip_deco_slope},
    {IDC_TERRAIN_DECO_DRAW_DIST, terrain_tip_deco_draw_dist},
    {IDC_TERRAIN_DECO_DRAW_DIST_LABEL, terrain_tip_deco_draw_dist},
    {IDC_TERRAIN_DECO_OFFSET, terrain_tip_deco_offset},
    {IDC_TERRAIN_DECO_OFFSET_LABEL, terrain_tip_deco_offset},
    {IDC_TERRAIN_DECO_CASTS_SHADOWS, "Shadows are baked by Calculate Lighting."},
    {IDC_TERRAIN_DECO_ALIGN, "Tilt to the ground slope."},
};

static INT_PTR CALLBACK TerrainDialogProc(HWND hdlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        DedTerrain* terrain = g_terrain_dlg.terrain;
        const DedTerrainData& d = g_terrain_dlg.data;

        SendDlgItemMessageA(hdlg, IDC_TERRAIN_SCRIPT_NAME, EM_LIMITTEXT, at::max_script_name_len, 0);
        SetDlgItemTextA(hdlg, IDC_TERRAIN_SCRIPT_NAME, terrain->script_name.c_str());
        alpine_dlg_set_float_field_exact(hdlg, IDC_TERRAIN_CELL_SIZE, d.cell_size);
        alpine_dlg_set_float_field_exact(hdlg, IDC_TERRAIN_HEIGHT_MIN, d.height_min);
        alpine_dlg_set_float_field_exact(hdlg, IDC_TERRAIN_HEIGHT_RANGE, d.height_range);
        SetDlgItemInt(hdlg, IDC_TERRAIN_LM_DENSITY, d.lightmap_density, FALSE);
        alpine_dlg_set_float_field_exact(hdlg, IDC_TERRAIN_THICKNESS, d.thickness);
        alpine_dlg_set_float_field_exact(hdlg, IDC_TERRAIN_SKIRT_DEPTH, d.skirt_depth);
        SetDlgItemTextA(hdlg, IDC_TERRAIN_UNDERSIDE_TEXTURE, d.underside_texture.c_str());
        SetDlgItemTextA(hdlg, IDC_TERRAIN_CRATER_TEXTURE, d.crater_texture.c_str());
        CheckDlgButton(hdlg, IDC_TERRAIN_GEOABLE, (d.flags & at::flag_geoable) ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hdlg, IDC_TERRAIN_SKIRTS, (d.flags & at::flag_skirts) ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hdlg, IDC_TERRAIN_FULLBRIGHT, d.fullbright ? BST_CHECKED : BST_UNCHECKED);

        for (uint32_t edge : at::chunk_edge_options) {
            // An edge over the flagless cap is always lowered.
            if (edge > at::max_chunk_cells(0)) continue;
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%u cells", edge);
            alpine_dlg_combo_add(hdlg, IDC_TERRAIN_CHUNK_SIZE, buf, edge);
        }
        alpine_dlg_combo_select(hdlg, IDC_TERRAIN_CHUNK_SIZE, d.chunk_cells);

        for (uint32_t mul : at::weight_res_options) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%ux cells", mul);
            alpine_dlg_combo_add(hdlg, IDC_TERRAIN_WEIGHT_RES, buf, mul);
        }
        alpine_dlg_combo_select(hdlg, IDC_TERRAIN_WEIGHT_RES, d.grid->weight_res_mul);

        alpine_spinner_init(hdlg, IDC_TERRAIN_CELL_SIZE, IDC_TERRAIN_CELL_SIZE_SPIN, 0.25f, at::min_cell_size,
                            at::max_cell_size, 3);
        alpine_spinner_init(hdlg, IDC_TERRAIN_HEIGHT_MIN, IDC_TERRAIN_HEIGHT_MIN_SPIN, 1.0f, -at::max_coord,
                            at::max_coord, 2);
        alpine_spinner_init(hdlg, IDC_TERRAIN_HEIGHT_RANGE, IDC_TERRAIN_HEIGHT_RANGE_SPIN, 1.0f,
                            at::min_height_range, at::max_height_range, 2);
        alpine_spinner_init_int(hdlg, IDC_TERRAIN_LM_DENSITY, IDC_TERRAIN_LM_DENSITY_SPIN, 1,
                                at::lightmap_density_min, at::lightmap_density_max);
        alpine_spinner_init(hdlg, IDC_TERRAIN_THICKNESS, IDC_TERRAIN_THICKNESS_SPIN, 1.0f, at::min_thickness,
                            at::max_thickness, 2);
        alpine_spinner_init(hdlg, IDC_TERRAIN_SKIRT_DEPTH, IDC_TERRAIN_SKIRT_DEPTH_SPIN, 1.0f, 0.0f,
                            at::max_skirt_depth, 2);
        for (const TerrainDecoField& f : terrain_deco_fields) {
            alpine_spinner_init(hdlg, f.edit, f.spin, f.step, f.lo, f.hi, f.decimals);
        }
        SendDlgItemMessageA(hdlg, IDC_TERRAIN_DECO_MESH, EM_LIMITTEXT, at::max_texture_name_len, 0);
        for (int kind = 0; kind < terrain_list_kinds; kind++) {
            const TerrainListUi& ui = terrain_list_ui[kind];
            if (ui.uv_scale) {
                alpine_spinner_init(hdlg, ui.uv_scale, ui.uv_scale_spin, 0.5f, at::min_uv_scale, at::max_uv_scale, 3);
            }
            g_terrain_dlg.sel[kind] = 0;
            terrain_dlg_fill_layer_list(hdlg, kind);
            terrain_dlg_load_layer_fields(hdlg, kind);
        }
        terrain_dlg_update_readouts(hdlg);
        KillTimer(hdlg, terrain_dlg_status_timer);
        terrain_dlg_update_status(hdlg);
        alpine_dlg_add_tooltips(hdlg, terrain_dlg_tooltips);
        g_terrain_dlg.active = true;
        terrain_dlg_refresh_viewports();
        SetTimer(hdlg, terrain_dlg_repaint_timer, 50, nullptr);
        return TRUE;
    }
    case WM_TIMER:
        if (wp == terrain_dlg_status_timer) {
            KillTimer(hdlg, terrain_dlg_status_timer);
            terrain_dlg_update_status(hdlg);
            return TRUE;
        }
        if (wp != terrain_dlg_repaint_timer) break;
        {
            const bool decorations = terrain_decorations_take_pending_work();
            if (terrain_preview_take_pending_work() || decorations) terrain_dlg_refresh_viewports();
        }
        return TRUE;
    case WM_COMMAND: {
        INT_PTR handled = FALSE;
        terrain_dlg_guard(hdlg, "There is not enough memory for this change; the terrain was left as it was.",
                          [&] { handled = terrain_dlg_command(hdlg, wp); });
        return handled;
    }
    case WM_NOTIFY:
        if (alpine_spinner_handle_notify(hdlg, lp)) return TRUE;
        break;
    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        for (int kind = 0; dis && kind < terrain_list_kinds; kind++) {
            const int preview = terrain_list_ui[kind].preview;
            if (preview && static_cast<int>(dis->CtlID) == preview) {
                alpine_dlg_draw_bitmap_preview(dis->hwndItem, dis->rcItem, g_terrain_dlg.preview_handle[kind]);
                return TRUE;
            }
        }
        break;
    }
    }
    return FALSE;
}

// ─── Convert to Brushes ─────────────────────────────────────────────────────

struct TerrainConvertPrompt
{
    std::string info;
    std::string warning;
    bool keep = false;
};
static TerrainConvertPrompt g_terrain_convert;

static INT_PTR CALLBACK TerrainConvertDialogProc(HWND hdlg, UINT msg, WPARAM wp, LPARAM /*lp*/)
{
    switch (msg) {
    case WM_INITDIALOG:
        alpine_center_dialog_on_owner(hdlg);
        SetDlgItemTextA(hdlg, IDC_TCONVERT_INFO, g_terrain_convert.info.c_str());
        SetDlgItemTextA(hdlg, IDC_TCONVERT_WARNING, g_terrain_convert.warning.c_str());
        CheckDlgButton(hdlg, IDC_TCONVERT_KEEP, g_terrain_convert.keep ? BST_CHECKED : BST_UNCHECKED);
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK:
            g_terrain_convert.keep = IsDlgButtonChecked(hdlg, IDC_TCONVERT_KEEP) == BST_CHECKED;
            EndDialog(hdlg, IDOK);
            return TRUE;
        case IDCANCEL:
            EndDialog(hdlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// Bakes the terrain into permanent detail brushes, one per chunk, the way Build Geometry emits it.
// Like To Brush this makes no undo record: the terrain's deletion could not be undone with it.
static void terrain_convert_to_brushes(CDedLevel* level, DedTerrain* terrain)
{
    auto& terrains = level->GetAlpineLevelProperties().terrain_objects;
    if (std::find(terrains.begin(), terrains.end(), terrain) == terrains.end()) return;

    TerrainBakeEstimate e;
    try {
        e = terrain_bake_estimate(*level, *terrain);
    }
    catch (const std::bad_alloc&) {
        show_error_message("There is not enough memory to convert this terrain.");
        return;
    }
    const uint64_t after = static_cast<uint64_t>(e.level_surfaces) + e.new_surfaces;
    if (e.level_surfaces_measured && after > red_max_level_surfaces) {
        show_error_message(std::format("Converting would bring the level to about {} lightmap surfaces ({} now, "
                                       "about {} from the terrain), over RED's limit of {}. Lower the terrain's "
                                       "resolution or convert a smaller terrain.",
                                       after, e.level_surfaces, e.new_surfaces, red_max_level_surfaces)
                               .c_str());
        return;
    }

    g_terrain_convert.info = std::format(
        "Creates {} detail brush(es), one per chunk, with {} faces textured by each cell's dominant layer. "
        "They are ordinary brushes: they lose the layer blend and terrain lighting and get stock lightmaps, "
        "where every non-coplanar triangle becomes its own lightmap surface (about {} here).\n\n"
        "This cannot be undone, the same as To Brush.",
        e.chunks, e.faces, e.new_surfaces);
    if (e.level_surfaces_measured) {
        g_terrain_convert.warning = std::format("The level would have about {} of RED's {} lightmap surfaces.", after,
                                                red_max_level_surfaces);
    }
    else if (after > red_max_level_surfaces) {
        g_terrain_convert.warning =
            std::format("Warning: the level may exceed RED's {} lightmap surfaces (up to {} estimated before Calculate "
                        "Lighting has numbered them).",
                        red_max_level_surfaces, after);
    }
    else {
        g_terrain_convert.warning = std::format("The level would have at most about {} of RED's {} lightmap surfaces.",
                                                after, red_max_level_surfaces);
    }
    if (DialogBoxParam(reinterpret_cast<HINSTANCE>(&__ImageBase), MAKEINTRESOURCE(IDD_ALPINE_TERRAIN_CONVERT),
                       GetMainFrameHandle(), TerrainConvertDialogProc, 0) != IDOK) {
        return;
    }

    std::vector<BrushNode*> brushes;
    try {
        brushes = terrain_bake_to_brushes(*level, *terrain);
    }
    catch (const std::bad_alloc&) {
        terrain_report("Out of memory converting the terrain; the brushes made so far stay in the level.", true);
    }

    // The new brushes take the selection over, as To Brush does.
    level->clear_selection();
    if (!g_terrain_convert.keep && !brushes.empty()) DeleteTerrainObject(terrain);
    select_inserted_brushes(level, brushes);
}

static bool terrain_layers_equal(const DedTerrainLayer& a, const DedTerrainLayer& b)
{
    return a.texture == b.texture && a.uv_scale == b.uv_scale && a.triplanar == b.triplanar;
}

enum class TerrainEdit
{
    none,
    look_only, // overlays, decorations or fullbright
    other,
};

// What differs between `a` and `b`: nothing, only their overlays, decorations (the lists and the coverage) or
// fullbright, or more.
static TerrainEdit terrain_edit_kind(const DedTerrainData& a, const DedTerrainData& b)
{
    if (!a.grid || !b.grid) return TerrainEdit::other;
    const TerrainGrid& ga = *a.grid;
    const TerrainGrid& gb = *b.grid;
    const bool same_rest =
        a.cell_size == b.cell_size && a.height_min == b.height_min && a.height_range == b.height_range &&
        a.chunk_cells == b.chunk_cells && a.lightmap_density == b.lightmap_density && a.flags == b.flags &&
        a.thickness == b.thickness && a.skirt_depth == b.skirt_depth && a.underside_texture == b.underside_texture &&
        a.crater_texture == b.crater_texture &&
        std::equal(a.layers.begin(), a.layers.end(), b.layers.begin(), b.layers.end(), terrain_layers_equal) &&
        a.geo_chunks == b.geo_chunks && a.geo_chunks_layout == b.geo_chunks_layout &&
        (&ga == &gb || (ga.nx == gb.nx && ga.nz == gb.nz && ga.weight_res_mul == gb.weight_res_mul &&
                        ga.heights == gb.heights && ga.weights == gb.weights && ga.holes == gb.holes &&
                        ga.diag == gb.diag));
    if (!same_rest) return TerrainEdit::other;
    const bool same_overlays =
        std::equal(a.overlays.begin(), a.overlays.end(), b.overlays.begin(), b.overlays.end(),
                   [](const DedTerrainOverlay& x, const DedTerrainOverlay& y) {
                       return terrain_layers_equal(x, y) && x.break_tiling == y.break_tiling;
                   }) &&
        (&ga == &gb || ga.overlay == gb.overlay);
    const bool same_decorations = a.decorations == b.decorations && (&ga == &gb || ga.decoration == gb.decoration);
    return same_overlays && same_decorations && a.fullbright == b.fullbright ? TerrainEdit::none
                                                                             : TerrainEdit::look_only;
}

void terrain_show_properties(CDedLevel* level, DedTerrain* terrain)
{
    if (!level || !terrain) return;
    terrain_paint_end_stroke(nullptr);
    terrain_clamp_properties(terrain->data);
    g_terrain_dlg = TerrainDialogState{};
    g_terrain_dlg.terrain = terrain;
    g_terrain_dlg.data = terrain->data;
    const DedTerrainData before = terrain->data;
    const std::string before_name = terrain->script_name.c_str();
    const uint64_t before_light = terrain_decoration_lighting_hash(terrain->uid, terrain->pos, before);

    const INT_PTR result = DialogBoxParam(reinterpret_cast<HINSTANCE>(&__ImageBase),
                                          MAKEINTRESOURCE(IDD_ALPINE_TERRAIN_PROPERTIES), GetActiveWindow(),
                                          TerrainDialogProc, 0);
    if (result == IDOK || result == IDC_TERRAIN_TOOLS || result == IDC_TERRAIN_CONVERT) {
        // Overlays, decorations and fullbright never need a rebuild; nothing changed marks nothing.
        const TerrainEdit edit = before_name == terrain->script_name.c_str() ? terrain_edit_kind(before, terrain->data)
                                                                              : TerrainEdit::other;
        if (edit == TerrainEdit::look_only) mark_level_modified();
        else if (edit == TerrainEdit::other) level->mark_geometry_dirty();
        if (terrain_decoration_lighting_hash(terrain->uid, terrain->pos, terrain->data) != before_light) {
            terrain_preview_lighting_changed(terrain);
        }
    }

    g_terrain_dlg = TerrainDialogState{};
    redraw_all_viewports();

    if (result == IDC_TERRAIN_TOOLS) terrain_paint_open(level, terrain);
    else if (result == IDC_TERRAIN_CONVERT) terrain_convert_to_brushes(level, terrain);
}

// The heightmap and weights are per-terrain data, so the dialog edits the first selected terrain.
void ShowTerrainPropertiesDialog(CDedLevel* level)
{
    auto& sel = level->selection;
    for (int i = 0; i < sel.get_size(); i++) {
        DedObject* obj = sel[i];
        if (obj && obj->type == DedObjectType::DED_TERRAIN) {
            terrain_show_properties(level, static_cast<DedTerrain*>(obj));
            return;
        }
    }
}

// ─── Object Lifecycle ───────────────────────────────────────────────────────

static bool terrain_can_add(CDedLevel* level, const DedTerrainData& d, bool interactive)
{
    auto& terrains = level->GetAlpineLevelProperties().terrain_objects;
    std::string why;
    if (terrains.size() >= at::max_terrains) {
        why = std::format("A level holds at most {} terrains.", at::max_terrains);
    }
    else if (terrain_level_raw_bytes_except(level, nullptr) + terrain_data_raw_bytes(d) >
             at::max_level_raw_bytes) {
        why = "The level's terrain data limit has been reached.";
    }
    if (why.empty()) return true;
    terrain_report(why, interactive);
    return false;
}

static void terrain_place_new(CDedLevel* level)
{
    auto& terrains = level->GetAlpineLevelProperties().terrain_objects;
    DedTerrainData data;
    data.layers.emplace_back();
    data.grid = terrain_make_flat_grid(at::default_verts, at::default_verts, at::default_weight_res_mul);
    if (!terrain_can_add(level, data, true)) return;
    terrains.reserve(terrains.size() + 1);

    auto* terrain = terrain_alloc();
    terrain->data = std::move(data);
    terrain->script_name.assign_0("Terrain");

    // Centred under the camera, a little below eye level so the flat surface is not seen edge on.
    auto* viewport = get_active_viewport();
    if (viewport && viewport->view_data) {
        const Vector3& cam = viewport->view_data->camera_pos;
        const float half = at::extent(at::default_verts, terrain->data.cell_size) * 0.5f;
        terrain->pos = {cam.x - half, cam.y - 4.0f, cam.z - half};
    }

    terrain->uid = generate_uid();

    terrains.push_back(terrain);
    level->master_objects.add(static_cast<DedObject*>(terrain));

    level->clear_selection();
    level->add_to_selection(static_cast<DedObject*>(terrain));
    level->update_console_display();
}

void PlaceNewTerrainObject()
{
    auto* level = CDedLevel::Get();
    if (!level) return;
    try {
        terrain_place_new(level);
    }
    catch (const std::bad_alloc&) {
        terrain_report("There is not enough memory to create a terrain.", true);
    }
}

DedTerrain* CloneTerrainObject(DedTerrain* source, bool add_to_level)
{
    if (!source) return nullptr;
    // The clone shares the grid, which a stroke in progress would go on editing in place.
    terrain_paint_end_stroke(source);

    // Everything that can run out of memory comes before the clone exists.
    CDedLevel* level = add_to_level ? CDedLevel::Get() : nullptr;
    if (level) {
        auto& terrains = level->GetAlpineLevelProperties().terrain_objects;
        terrains.reserve(terrains.size() + 1);
    }
    // The grid is shared until either side edits it.
    DedTerrainData data = source->data;

    auto* terrain = terrain_alloc();
    terrain->pos = source->pos;
    terrain->script_name.assign_0(source->script_name.c_str());
    terrain->data = std::move(data);
    // The compiled rooms belong to the source.
    terrain_reset_built_state(*terrain);
    terrain->uid = generate_uid();

    if (level) {
        level->GetAlpineLevelProperties().terrain_objects.push_back(terrain);
        level->master_objects.add(static_cast<DedObject*>(terrain));
    }

    return terrain;
}

void DeleteTerrainObject(DedTerrain* terrain)
{
    if (!terrain) return;
    auto* level = CDedLevel::Get();
    if (!level) return;

    auto& terrains = level->GetAlpineLevelProperties().terrain_objects;
    auto it = std::find(terrains.begin(), terrains.end(), terrain);
    if (it != terrains.end()) {
        terrains.erase(it);
    }
    alpine_remove_from_groups(level, static_cast<DedObject*>(terrain));
    level->master_objects.remove_by_value(static_cast<DedObject*>(terrain));
    DestroyDedTerrain(terrain);
}

// ─── Rendering ──────────────────────────────────────────────────────────────

static constexpr float terrain_icon_size = 1.0f;

// Centre of the bounding box of the footprint and the surface's height span: where the icon sits.
static Vector3 terrain_icon_pos(const Vector3& pos, const DedTerrainData& d)
{
    const TerrainGrid* g = d.grid.get();
    if (!g || g->heights.empty()) return pos;
    const auto [lo, hi] = std::minmax_element(g->heights.begin(), g->heights.end());
    const float y_lo = at::height_offset(*lo, d.height_min, d.height_range);
    const float y_hi = at::height_offset(*hi, d.height_min, d.height_range);
    return {pos.x + at::extent(g->nx, d.cell_size) * 0.5f, pos.y + (y_lo + y_hi) * 0.5f,
            pos.z + at::extent(g->nz, d.cell_size) * 0.5f};
}

Vector3 terrain_icon_pos(const DedTerrain& terrain)
{
    return terrain_icon_pos(terrain.pos, terrain.data);
}

// What the viewport draws of `terrain`: the dialog's staged copy while the dialog edits it, which also selects it.
static const DedTerrainData& terrain_shown_data(CDedLevel* level, DedTerrain* terrain, bool& selected)
{
    const bool preview = g_terrain_dlg.active && g_terrain_dlg.terrain == terrain;
    selected = preview || is_object_selected(level, terrain);
    return preview ? g_terrain_dlg.data : terrain->data;
}

void terrain_render_surfaces(CDedLevel* level)
{
    for (auto* terrain : level->GetAlpineLevelProperties().terrain_objects) {
        if (terrain->hidden_in_editor) continue;

        // Axis-aligned by definition: a rotate tool pass leaves nothing behind.
        terrain->orient = identity_orient;

        bool selected = false;
        const DedTerrainData& data = terrain_shown_data(level, terrain, selected);
        terrain_preview_draw(*level, *terrain, data, selected);
        terrain_decorations_collect(*terrain, data);
    }
    terrain_decorations_frame_end(*level, g_terrain_dlg.active ? &g_terrain_dlg.data : nullptr);
    terrain_preview_frame_end(*level);
}

void terrain_render(CDedLevel* level)
{
    auto& terrains = level->GetAlpineLevelProperties().terrain_objects;
    if (!terrains.empty()) terrain_load_icon();
    const float cam_param = gr_cam_param;

    for (auto* terrain : terrains) {
        if (terrain->hidden_in_editor) continue;
        bool selected = false;
        const DedTerrainData& data = terrain_shown_data(level, terrain, selected);

        const auto& rgb = selected ? terrain_selected_rgb : terrain_unselected_rgb;
        set_draw_color(rgb[0], rgb[1], rgb[2], 0xff);
        if (g_terrain_icon_handle >= 0) {
            gr_set_bitmap(g_terrain_icon_handle, -1);
        }
        Vector3 icon = terrain_icon_pos(terrain->pos, data);
        gr_render_billboard(&icon, 0, terrain_icon_size, cam_param);
    }
    terrain_paint_draw_cursor(*level);
}

// Marquee: the icon inside the box, or the whole footprint.
void terrain_pick(CDedLevel* level, int param1, int param2)
{
    auto& terrains = level->GetAlpineLevelProperties().terrain_objects;
    for (auto* terrain : terrains) {
        if (terrain->hidden_in_editor) continue;
        const Vector3 icon = terrain_icon_pos(*terrain);
        bool hit = level->hit_test_point(param1, param2, &icon);
        const DedTerrainData& d = terrain->data;
        if (!hit && d.grid && !d.layers.empty()) {
            const at::GridView v = terrain_grid_view(terrain->pos, d, *d.grid);
            const uint32_t corners[4][2] = {{0, 0}, {d.grid->nx - 1, 0}, {0, d.grid->nz - 1},
                                            {d.grid->nx - 1, d.grid->nz - 1}};
            hit = true;
            for (const auto& c : corners) {
                float p[3];
                at::grid_position(v, c[0], c[1], p);
                const Vector3 corner{p[0], p[1], p[2]};
                hit = hit && level->hit_test_point(param1, param2, &corner);
            }
        }
        if (hit) {
            level->select_object(static_cast<DedObject*>(terrain));
        }
    }
}

DedTerrain* terrain_click_pick(CDedLevel* level, float click_x, float click_y)
{
    return alpine_click_pick_point(level->GetAlpineLevelProperties().terrain_objects, click_x, click_y,
                                   alpine_click_pick_radius_sq,
                                   [](const DedTerrain& terrain) { return terrain_icon_pos(terrain); });
}

void terrain_tree_populate(EditorTreeCtrl* tree, int master_groups, CDedLevel* level)
{
    auto& terrains = level->GetAlpineLevelProperties().terrain_objects;

    char buf[64];
    snprintf(buf, sizeof(buf), "Terrains (%d)", static_cast<int>(terrains.size()));
    int parent = tree->insert_item(buf, master_groups, 0xffff0002);

    for (auto* terrain : terrains) {
        const char* name = terrain->script_name.c_str();
        if (!name || name[0] == '\0') {
            name = "(unnamed terrain)";
        }
        int child = tree->insert_item(name, parent, 0xffff0002);
        tree->set_item_data(child, terrain->uid);
    }
}

void terrain_tree_add_object_type(EditorTreeCtrl* tree)
{
    tree->insert_item("Terrain", 0xffff0000, 0xffff0002);
}

bool terrain_copy_object(DedObject* source)
{
    if (!source || source->type != DedObjectType::DED_TERRAIN) return false;
    try {
        g_terrain_clipboard.reserve(g_terrain_clipboard.size() + 1);
        auto* staged = CloneTerrainObject(static_cast<DedTerrain*>(source), false);
        if (staged) {
            g_terrain_clipboard.push_back(staged);
            return true;
        }
    }
    catch (const std::bad_alloc&) {
        terrain_report("There is not enough memory to copy the terrain.", true);
    }
    return false;
}

void terrain_paste_objects(CDedLevel* level)
{
    int skipped = 0, failed = 0;
    for (auto* staged : g_terrain_clipboard) {
        try {
            if (!terrain_can_add(level, staged->data, false)) {
                skipped++;
                continue;
            }
            auto* clone = CloneTerrainObject(staged, true);
            if (clone) {
                level->add_to_selection(static_cast<DedObject*>(clone));
                mark_level_modified();
            }
        }
        catch (const std::bad_alloc&) {
            failed++;
        }
    }
    if (skipped) {
        terrain_report(std::format("{} terrain(s) were not pasted: the level would exceed its terrain count or "
                                   "data limit.",
                                   skipped),
                       true);
    }
    if (failed) {
        terrain_report(std::format("{} terrain(s) were not pasted: there is not enough memory.", failed), true);
    }
}

void terrain_clear_clipboard()
{
    for (auto* terrain : g_terrain_clipboard) {
        DestroyDedTerrain(terrain);
    }
    g_terrain_clipboard.clear();
}

void terrain_handle_delete_or_cut(DedObject* obj)
{
    if (!obj || obj->type != DedObjectType::DED_TERRAIN) return;
    auto* level = CDedLevel::Get();
    if (!level) return;

    auto& terrains = level->GetAlpineLevelProperties().terrain_objects;
    auto it = std::find(terrains.begin(), terrains.end(), static_cast<DedTerrain*>(obj));
    if (it != terrains.end()) {
        terrains.erase(it);
    }
}

void terrain_handle_delete_selection(CDedLevel* level)
{
    alpine_compact_selection<DedTerrain>(level, DedObjectType::DED_TERRAIN, DeleteTerrainObject);
}

void terrain_ensure_uid(int& uid)
{
    auto* level = CDedLevel::Get();
    if (!level) return;
    alpine_ensure_uid(level->GetAlpineLevelProperties().terrain_objects, uid);
}
