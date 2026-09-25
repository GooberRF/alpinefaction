#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <algorithm>
#include <format>
#include <memory>
#include <new>
#include <vector>
#include <zlib.h>
#include <stb_image.h>
#include <common/rfl_chunk_reader.h>
#include <common/terrain/alpine_terrain.h>
#include <common/terrain/alpine_terrain_reader.h>
#include <common/utils/string-utils.h>
#include <xlog/xlog.h>
#include "alpine_spinner.h"
#include "file_dialogs.h"
#include "headless_bake.h"
#include "terrain.h"
#include "terrain_build.h"
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

static void terrain_set_identity(Matrix3& orient)
{
    orient.rvec = {1.0f, 0.0f, 0.0f};
    orient.uvec = {0.0f, 1.0f, 0.0f};
    orient.fvec = {0.0f, 0.0f, 1.0f};
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
    return g.heights.size() == at::vertex_count(g.nx, g.nz) &&
           g.weights.size() == at::blob_weights_bytes(g.nx, g.nz, g.weight_res_mul) &&
           g.holes.size() == mask && g.diag.size() == mask &&
           (g.overlay.empty() || g.overlay.size() == at::overlay_map_bytes(g.nx, g.nz, g.weight_res_mul));
}

// The grid carries a coverage map exactly while the terrain has overlays; a new one starts at zero.
static void terrain_match_overlay_map(DedTerrainData& d)
{
    if (!d.grid || d.overlays.empty() == d.grid->overlay.empty()) return;
    auto g = std::make_shared<TerrainGrid>(*d.grid);
    if (d.overlays.empty()) g->overlay.clear();
    else g->overlay.assign(at::overlay_map_bytes(g->nx, g->nz, g->weight_res_mul), 0);
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

// Heights filtered on the vertex lattice, weights over texel centres and renormalized, overlay coverage
// as the weights without renormalizing, holes and diagonals nearest by cell centre.
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
                                  uint32_t mul, bool overlays)
{
    // Whether the geo mask is written depends on the edit's chunk layout, so it is always counted.
    const uint64_t total =
        terrain_level_raw_bytes_except(CDedLevel::Get(), except) +
        at::wire_raw_size(nx, nz, mul, at::flag_chunk_geo_mask | (overlays ? at::flag_overlays : 0));
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
    if (!d.grid || !terrain_grid_consistent(*d.grid)) {
        d.grid = terrain_make_flat_grid(at::default_verts, at::default_verts, at::default_weight_res_mul);
    }
    terrain_match_overlay_map(d);
}

// The header as written: flag_chunk_geo_mask added when a chunk is not geoable, `geo` then the mask, and
// flag_overlays when the terrain has overlays.
static at::Header terrain_wire_header(const Vector3& pos, const DedTerrainData& d, std::vector<uint8_t>& geo)
{
    at::Header h = terrain_header(pos, d, d.grid.get());
    geo.clear();
    if ((d.flags & at::flag_geoable) && d.grid) {
        geo = terrain_geo_chunks(d);
        if (!at::chunk_mask_full(geo.data(), at::header_geo_chunk_count(h))) h.flags |= at::flag_chunk_geo_mask;
    }
    if (!d.overlays.empty()) {
        h.flags |= at::flag_overlays;
        h.overlay_count = static_cast<uint32_t>(d.overlays.size());
    }
    return h;
}

void terrain_prepare(DedTerrain& terrain)
{
    terrain_clamp_properties(terrain.data);
    terrain_set_identity(terrain.orient);
}

// ─── Cleanup ─────────────────────────────────────────────────────────────────

void DestroyDedTerrain(DedTerrain* terrain)
{
    if (!terrain) return;
    terrain_paint_forget(terrain);
    terrain_preview_forget(terrain);
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
    terrain_set_identity(terrain->orient);
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
            terrain_set_identity(terrain->orient);
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
                std::memcpy(dst, geo.data(), geo.size());
                std::memset(dst + geo.size(), 0, at::chunk_geo_mask_wire_bytes - geo.size());
            }
            if (header.flags & at::flag_overlays) {
                std::memcpy(raw.data() + at::blob_overlay_offset(g.nx, g.nz, g.weight_res_mul, header.flags),
                            g.overlay.data(), g.overlay.size());
            }

            Record rec{terrain, terrain->script_name.c_str(), static_cast<uint8_t>(header.flags), write_mapping,
                       static_cast<uint32_t>(raw_size), {}};
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
        file.write<uint8_t>(static_cast<uint8_t>(d.chunk_cells));
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

    terrain->uid = rec.uid;
    terrain->pos = {h.origin[0], h.origin[1], h.origin[2]};
    terrain->script_name.assign_0(rec.script_name.c_str());
    d.cell_size = h.cell_size;
    d.height_min = h.height_min;
    d.height_range = h.height_range;
    d.chunk_cells = h.chunk_cells;
    d.lightmap_density = static_cast<uint8_t>(h.lightmap_density);
    d.flags = static_cast<uint8_t>(h.flags & at::flag_mask);
    d.thickness = h.thickness;
    d.skirt_depth = h.skirt_depth;
    d.underside_texture = std::move(rec.underside_texture);
    d.crater_texture = std::move(rec.crater_texture);
    d.layers.reserve(rec.layers.size());
    for (at::RecordLayer& layer : rec.layers) d.layers.push_back({std::move(layer.texture), layer.uv_scale, layer.triplanar});
    d.overlays.resize(rec.overlays.size());
    for (std::size_t i = 0; i < rec.overlays.size(); i++) {
        d.overlays[i].texture = std::move(rec.overlays[i].texture);
        d.overlays[i].uv_scale = rec.overlays[i].uv_scale;
        d.overlays[i].triplanar = rec.overlays[i].triplanar;
        d.overlays[i].break_tiling = rec.overlays[i].break_tiling;
    }
    d.build_mapping = std::move(rec.build_mapping);
    d.grid = std::move(g);
    if (!rec.geo_chunks.empty() && !at::chunk_mask_full(rec.geo_chunks.data(), at::header_geo_chunk_count(h))) {
        d.geo_chunks = std::move(rec.geo_chunks);
        d.geo_chunks_layout = at::geo_chunk_layout(h.nx, h.nz, h.chunk_cells, h.flags);
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
                if (group) terrain_reset_built_state(*t);
                else terrain_build_note_loaded(level, *t);
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
            err = "A RAW16 heightmap must be square (2x2 to 4097x4097).";
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
        err = "The image must be between 2x2 and 4097x4097 pixels.";
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

// ─── Properties Dialog ──────────────────────────────────────────────────────

// The dialog edits base layers and overlays through the same list controls.
enum TerrainListKind
{
    terrain_list_layers = 0,
    terrain_list_overlays = 1,
    terrain_list_kinds = 2,
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
    int preview_handle[terrain_list_kinds] = {-1, -1};
};
static TerrainDialogState g_terrain_dlg;

static int terrain_dlg_count(int kind)
{
    return static_cast<int>(kind == terrain_list_overlays ? g_terrain_dlg.data.overlays.size()
                                                          : g_terrain_dlg.data.layers.size());
}

static DedTerrainLayer& terrain_dlg_item(int kind, int index)
{
    if (kind == terrain_list_overlays) return g_terrain_dlg.data.overlays[index];
    return g_terrain_dlg.data.layers[index];
}

// More digits than alpine_dlg_set_float_field where needed: OK re-reads every field, and a value a
// sculpt stroke grew (height min/range, thickness) must read back bit for bit.
static void terrain_set_float_field(HWND hdlg, int idc, float value)
{
    alpine_dlg_set_float_field_exact(hdlg, idc, value);
}

// `shown` while the field still holds its text, else what was typed.
static float terrain_get_float_field(HWND hdlg, int idc, float shown)
{
    char text[32] = {}, fmt[32];
    GetDlgItemTextA(hdlg, idc, text, sizeof(text));
    alpine_format_float_exact(fmt, shown);
    return std::strcmp(text, fmt) == 0 ? shown : std::strtof(text, nullptr);
}

static std::string terrain_get_text(HWND hdlg, int idc)
{
    char buf[MAX_PATH] = {};
    GetDlgItemTextA(hdlg, idc, buf, sizeof(buf));
    return buf;
}

// Copy-on-write: the staged grid may be shared with the terrain object and the clipboard.
static TerrainGrid& terrain_mutable_grid(DedTerrainData& d)
{
    auto g = std::make_shared<TerrainGrid>(*d.grid);
    d.grid = g;
    return *g;
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

    const uint32_t edge = at::effective_chunk_cells(cx, cz, d.chunk_cells, d.flags);
    const uint32_t chunks = at::chunk_count(cx, cz, edge);
    if (edge != d.chunk_cells) {
        std::snprintf(buf, sizeof(buf), "%s to %u (%u chunks)", edge > d.chunk_cells ? "raised" : "lowered",
                      edge, chunks);
    }
    else {
        std::snprintf(buf, sizeof(buf), "%u chunk%s", chunks, chunks == 1 ? "" : "s");
    }
    SetDlgItemTextA(hdlg, IDC_TERRAIN_CHUNK_INFO, buf);

    std::snprintf(buf, sizeof(buf), "Data %.2f MB, weights %u x %u",
                  static_cast<double>(terrain_data_raw_bytes(d)) / (1024.0 * 1024.0),
                  at::weight_width(g.nx, g.weight_res_mul), at::weight_height(g.nz, g.weight_res_mul));
    SetDlgItemTextA(hdlg, IDC_TERRAIN_STATUS, buf);
}

static void terrain_dlg_refresh_viewports()
{
    if (g_terrain_dlg.active) redraw_all_viewports();
}

// Composites left for a later paint are otherwise serviced by RED's idle loop, which the dialog's
// modal loop does not run.
static constexpr UINT_PTR terrain_dlg_repaint_timer = 1;

static void terrain_dlg_format_layer(int kind, int index, char* buf, std::size_t size)
{
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
}

static void terrain_dlg_load_layer_fields(HWND hdlg, int kind)
{
    const TerrainListUi& ui = terrain_list_ui[kind];
    const int count = terrain_dlg_count(kind);
    g_terrain_dlg.sel[kind] = std::clamp(g_terrain_dlg.sel[kind], 0, std::max(count - 1, 0));
    const int sel = g_terrain_dlg.sel[kind];
    g_terrain_dlg.loading_layer = true;
    if (count > 0) {
        const DedTerrainLayer& layer = terrain_dlg_item(kind, sel);
        SetDlgItemTextA(hdlg, ui.texture, layer.texture.c_str());
        terrain_set_float_field(hdlg, ui.uv_scale, layer.uv_scale);
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

// Browser categories to open on, first match wins: a user's own terrain folder, then the closest
// stock one.
static constexpr const char* terrain_surface_categories[] = {"Terrain", "Custom - Terrain", "Floor - Rock"};
static constexpr const char* terrain_underside_categories[] = {"Terrain", "Custom - Terrain", "Wall - Rock"};

// The texture browser runs its own modal loop off the main frame; same disable/re-activate dance the
// rope dialog does.
template<std::size_t N>
static bool terrain_browse_texture(HWND hdlg, int field_idc, const char* const (&categories)[N])
{
    const std::string current = terrain_get_text(hdlg, field_idc);
    EnableWindow(hdlg, FALSE);
    const int picked = texture_browser_pick_first(categories, N, alpine_dlg_resolve_bitmap(current.c_str()));
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
            const int nx = alpine_dlg_get_int_field(hdlg, IDC_TERRAIN_RES_NX);
            const int nz = alpine_dlg_get_int_field(hdlg, IDC_TERRAIN_RES_NZ);
            if (nx < static_cast<int>(at::min_verts) || nx > static_cast<int>(at::max_verts) ||
                nz < static_cast<int>(at::min_verts) || nz > static_cast<int>(at::max_verts)) {
                MessageBoxA(hdlg, "Each vertex count must be between 2 and 257.", "Terrain",
                            MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            g_terrain_res_prompt.nx = static_cast<uint32_t>(nx);
            g_terrain_res_prompt.nz = static_cast<uint32_t>(nz);
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
                                   "overlay coverage, holes and cell diagonals.",
                                   nx, nz)) {
        return;
    }
    if (!terrain_budget_allows(hdlg, g_terrain_dlg.terrain, nx, nz, old.weight_res_mul,
                               !g_terrain_dlg.data.overlays.empty())) {
        return;
    }
    DedTerrainData next = g_terrain_dlg.data;
    next.grid = terrain_make_flat_grid(nx, nz, old.weight_res_mul);
    terrain_match_overlay_map(next);
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
        nx = std::clamp<uint32_t>(static_cast<uint32_t>(std::lround((w - 1) * scale)) + 1, at::min_verts, at::max_verts);
        nz = std::clamp<uint32_t>(static_cast<uint32_t>(std::lround((h - 1) * scale)) + 1, at::min_verts, at::max_verts);
    }
    char info[160];
    std::snprintf(info, sizeof(info),
                  "The image is %u x %u. Black maps to Height Min, white to Height Min + Height Range.",
                  w, h);
    if (!terrain_prompt_resolution(hdlg, "Import Heightmap", info, nx, nz)) return;

    const TerrainGrid& old = *g_terrain_dlg.data.grid;
    if (!terrain_budget_allows(hdlg, g_terrain_dlg.terrain, nx, nz, old.weight_res_mul,
                               !g_terrain_dlg.data.overlays.empty())) {
        return;
    }

    std::shared_ptr<TerrainGrid> g = (nx == old.nx && nz == old.nz)
                                         ? std::make_shared<TerrainGrid>(old)
                                         : terrain_resample_grid(old, nx, nz, old.weight_res_mul);
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
        MessageBoxA(hdlg, "The image must be at most 4097x4097 pixels.", "Import Splat Map",
                    MB_OK | MB_ICONWARNING);
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
    // Every channel that now carries weight gets a layer behind it.
    while (static_cast<int>(next.layers.size()) <= highest) next.layers.emplace_back();
    g_terrain_dlg.data = std::move(next);
    terrain_dlg_reselect_layer(hdlg, terrain_list_layers, g_terrain_dlg.sel[terrain_list_layers]);
    terrain_dlg_update_readouts(hdlg);
    terrain_dlg_refresh_viewports();
}

// A large image can exhaust RED's 32-bit address space; the exception must not unwind into the dialog
// manager.
static void terrain_dlg_run_import(HWND hdlg, const char* title, void (*import)(HWND))
{
    try {
        import(hdlg);
    }
    catch (const std::bad_alloc&) {
        MessageBoxA(hdlg, "There is not enough memory to import this file.", title, MB_OK | MB_ICONWARNING);
    }
}

static void terrain_dlg_set_weight_res(HWND hdlg)
{
    const LRESULT data = alpine_dlg_combo_data(hdlg, IDC_TERRAIN_WEIGHT_RES, CB_ERR);
    if (data == CB_ERR) return;
    const auto mul = static_cast<uint32_t>(data);
    const TerrainGrid& old = *g_terrain_dlg.data.grid;
    if (!at::is_allowed_weight_res_mul(mul) || mul == old.weight_res_mul) return;
    if (!terrain_budget_allows(hdlg, g_terrain_dlg.terrain, old.nx, old.nz, mul,
                               !g_terrain_dlg.data.overlays.empty())) {
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
    if (kind == terrain_list_overlays) {
        const TerrainGrid& g = *d.grid;
        if (d.overlays.empty() &&
            !terrain_budget_allows(hdlg, g_terrain_dlg.terrain, g.nx, g.nz, g.weight_res_mul, true)) {
            return;
        }
        d.overlays.emplace_back();
        terrain_match_overlay_map(d);
        g_terrain_dlg.data = std::move(d);
        terrain_dlg_update_readouts(hdlg);
    }
    else {
        d.layers.emplace_back();
        g_terrain_dlg.data = std::move(d);
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
    else {
        // Its weight goes to the remaining layers in proportion; layer 0 takes texels left empty.
        terrain_remove_weight_channel(terrain_mutable_grid(d), static_cast<std::size_t>(sel));
        d.layers.erase(d.layers.begin() + sel);
        g_terrain_dlg.data = std::move(d);
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
    else {
        terrain_swap_weight_channels(terrain_mutable_grid(d), ca, cb);
        std::swap(d.layers[ca], d.layers[cb]);
    }
    g_terrain_dlg.data = std::move(d);
    terrain_dlg_reselect_layer(hdlg, kind, b);
    terrain_dlg_refresh_viewports();
}

static void terrain_dlg_capture_shape(HWND hdlg)
{
    DedTerrainData& d = g_terrain_dlg.data;
    d.cell_size = at::clamp_finite(terrain_get_float_field(hdlg, IDC_TERRAIN_CELL_SIZE, d.cell_size), at::min_cell_size,
                         at::max_cell_size, d.cell_size);
    d.height_min = at::clamp_finite(terrain_get_float_field(hdlg, IDC_TERRAIN_HEIGHT_MIN, d.height_min), -at::max_coord,
                          at::max_coord, d.height_min);
    d.height_range = at::clamp_finite(terrain_get_float_field(hdlg, IDC_TERRAIN_HEIGHT_RANGE, d.height_range),
                            at::min_height_range, at::max_height_range, d.height_range);
    d.chunk_cells = static_cast<uint32_t>(alpine_dlg_combo_data(hdlg, IDC_TERRAIN_CHUNK_SIZE, d.chunk_cells));
    // Geoable and skirted terrains cap the effective chunk size, so the readout follows the checkboxes.
    d.flags = static_cast<uint8_t>(d.flags & ~(at::flag_geoable | at::flag_skirts));
    if (IsDlgButtonChecked(hdlg, IDC_TERRAIN_GEOABLE) == BST_CHECKED) d.flags |= at::flag_geoable;
    if (IsDlgButtonChecked(hdlg, IDC_TERRAIN_SKIRTS) == BST_CHECKED) d.flags |= at::flag_skirts;
    d.thickness = at::clamp_finite(terrain_get_float_field(hdlg, IDC_TERRAIN_THICKNESS, d.thickness), at::min_thickness,
                         at::max_thickness, d.thickness);
    d.skirt_depth = at::clamp_finite(terrain_get_float_field(hdlg, IDC_TERRAIN_SKIRT_DEPTH, d.skirt_depth), 0.0f,
                           at::max_skirt_depth, d.skirt_depth);
}

static void terrain_dlg_store_layer_fields(HWND hdlg, int kind)
{
    if (g_terrain_dlg.loading_layer) return;
    const TerrainListUi& ui = terrain_list_ui[kind];
    const int sel = g_terrain_dlg.sel[kind];
    if (sel < 0 || sel >= terrain_dlg_count(kind)) return;
    DedTerrainLayer& layer = terrain_dlg_item(kind, sel);
    layer.texture = terrain_get_text(hdlg, ui.texture);
    const float uv = terrain_get_float_field(hdlg, ui.uv_scale, layer.uv_scale);
    if (std::isfinite(uv)) layer.uv_scale = std::clamp(uv, at::min_uv_scale, at::max_uv_scale);
    layer.triplanar = IsDlgButtonChecked(hdlg, ui.triplanar) == BST_CHECKED;
    if (ui.break_tiling) {
        g_terrain_dlg.data.overlays[sel].break_tiling = IsDlgButtonChecked(hdlg, ui.break_tiling) == BST_CHECKED;
    }
    terrain_dlg_refresh_layer_row(hdlg, kind, sel);
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
        else if (id == ui.texture) {
            if (code == EN_CHANGE) {
                terrain_dlg_store_layer_fields(hdlg, kind);
                terrain_dlg_update_layer_preview(hdlg, kind, false);
            }
        }
        else if (id == ui.uv_scale) {
            if (code == EN_CHANGE) terrain_dlg_store_layer_fields(hdlg, kind);
        }
        else if (id == ui.triplanar || (ui.break_tiling && id == ui.break_tiling)) {
            terrain_dlg_store_layer_fields(hdlg, kind);
        }
        else if (id == ui.browse) {
            // The edit's EN_CHANGE stores the pick into the layer.
            terrain_browse_texture(hdlg, ui.texture, terrain_surface_categories);
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
    terrain_clamp_properties(d);

    DedTerrain* terrain = g_terrain_dlg.terrain;
    std::string script_name = terrain_get_text(hdlg, IDC_TERRAIN_SCRIPT_NAME);
    if (script_name.size() > at::max_script_name_len) script_name.resize(at::max_script_name_len);
    DedTerrainData committed = d;
    terrain->script_name.assign_0(script_name.c_str());
    terrain->data = std::move(committed);
    return true;
}

// RED's 32-bit address space can run out on a large grid: the exception must not unwind into the dialog
// manager, and every edit either completes or leaves the staged data as it was.
template<typename F>
static void terrain_dlg_guard(HWND hdlg, const char* message, F&& edit)
{
    try {
        edit();
    }
    catch (const std::bad_alloc&) {
        MessageBoxA(hdlg, message, "Terrain", MB_OK | MB_ICONWARNING);
    }
}

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
    case IDC_TERRAIN_UNDERSIDE_TEXTURE:
        if (HIWORD(wp) == EN_CHANGE && g_terrain_dlg.active) {
            g_terrain_dlg.data.underside_texture = terrain_get_text(hdlg, IDC_TERRAIN_UNDERSIDE_TEXTURE);
            terrain_dlg_refresh_viewports();
        }
        break;
    case IDC_TERRAIN_UNDERSIDE_BROWSE:
        terrain_browse_texture(hdlg, IDC_TERRAIN_UNDERSIDE_TEXTURE, terrain_underside_categories);
        return TRUE;
    case IDC_TERRAIN_CRATER_BROWSE:
        terrain_browse_texture(hdlg, IDC_TERRAIN_CRATER_TEXTURE, terrain_surface_categories);
        return TRUE;
    case IDC_TERRAIN_NEW_FLAT:
        terrain_dlg_new_flat(hdlg);
        return TRUE;
    case IDC_TERRAIN_IMPORT_HEIGHTMAP:
        terrain_dlg_run_import(hdlg, "Import Heightmap", terrain_dlg_import_heightmap);
        return TRUE;
    case IDC_TERRAIN_EXPORT_HEIGHTMAP:
        terrain_dlg_export_heightmap(hdlg);
        return TRUE;
    case IDC_TERRAIN_IMPORT_SPLAT:
        terrain_dlg_run_import(hdlg, "Import Splat Map", terrain_dlg_import_splat);
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

static INT_PTR CALLBACK TerrainDialogProc(HWND hdlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        DedTerrain* terrain = g_terrain_dlg.terrain;
        const DedTerrainData& d = g_terrain_dlg.data;

        SendDlgItemMessageA(hdlg, IDC_TERRAIN_SCRIPT_NAME, EM_LIMITTEXT, at::max_script_name_len, 0);
        SetDlgItemTextA(hdlg, IDC_TERRAIN_SCRIPT_NAME, terrain->script_name.c_str());
        terrain_set_float_field(hdlg, IDC_TERRAIN_CELL_SIZE, d.cell_size);
        terrain_set_float_field(hdlg, IDC_TERRAIN_HEIGHT_MIN, d.height_min);
        terrain_set_float_field(hdlg, IDC_TERRAIN_HEIGHT_RANGE, d.height_range);
        SetDlgItemInt(hdlg, IDC_TERRAIN_LM_DENSITY, d.lightmap_density, FALSE);
        terrain_set_float_field(hdlg, IDC_TERRAIN_THICKNESS, d.thickness);
        terrain_set_float_field(hdlg, IDC_TERRAIN_SKIRT_DEPTH, d.skirt_depth);
        SetDlgItemTextA(hdlg, IDC_TERRAIN_UNDERSIDE_TEXTURE, d.underside_texture.c_str());
        SetDlgItemTextA(hdlg, IDC_TERRAIN_CRATER_TEXTURE, d.crater_texture.c_str());
        CheckDlgButton(hdlg, IDC_TERRAIN_GEOABLE, (d.flags & at::flag_geoable) ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hdlg, IDC_TERRAIN_SKIRTS, (d.flags & at::flag_skirts) ? BST_CHECKED : BST_UNCHECKED);

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
        for (int kind = 0; kind < terrain_list_kinds; kind++) {
            const TerrainListUi& ui = terrain_list_ui[kind];
            alpine_spinner_init(hdlg, ui.uv_scale, ui.uv_scale_spin, 0.5f, at::min_uv_scale, at::max_uv_scale, 3);
            g_terrain_dlg.sel[kind] = 0;
            terrain_dlg_fill_layer_list(hdlg, kind);
            terrain_dlg_load_layer_fields(hdlg, kind);
        }
        terrain_dlg_update_readouts(hdlg);
        g_terrain_dlg.active = true;
        terrain_dlg_refresh_viewports();
        SetTimer(hdlg, terrain_dlg_repaint_timer, 50, nullptr);
        return TRUE;
    }
    case WM_TIMER:
        if (wp != terrain_dlg_repaint_timer) break;
        if (terrain_preview_take_pending_work()) terrain_dlg_refresh_viewports();
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
            if (static_cast<int>(dis->CtlID) == terrain_list_ui[kind].preview) {
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

    // Brush selection lives in BrushNode::state; the new brushes take it over, as To Brush does.
    level->clear_selection();
    if (BrushNode* head = level->brush_list) {
        BrushNode* b = head;
        do {
            if (b->state == BRUSH_STATE_SELECTED) b->state = BRUSH_STATE_NORMAL;
            b = b->next;
        } while (b && b != head);
    }
    for (BrushNode* brush : brushes) brush->state = BRUSH_STATE_SELECTED;
    if (!g_terrain_convert.keep && !brushes.empty()) DeleteTerrainObject(terrain);

    level->mark_geometry_dirty();
    level->update_console_display();
    redraw_all_viewports();
}

static bool terrain_layers_equal(const DedTerrainLayer& a, const DedTerrainLayer& b)
{
    return a.texture == b.texture && a.uv_scale == b.uv_scale && a.triplanar == b.triplanar;
}

enum class TerrainEdit
{
    none,
    overlays_only,
    other,
};

// What differs between `a` and `b`: nothing, only their overlays (the list and the coverage), or more.
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
    return same_overlays ? TerrainEdit::none : TerrainEdit::overlays_only;
}

// The heightmap and weights are per-terrain data, so the dialog edits the first selected terrain.
void ShowTerrainPropertiesDialog(CDedLevel* level)
{
    DedTerrain* terrain = nullptr;
    auto& sel = level->selection;
    for (int i = 0; i < sel.get_size(); i++) {
        DedObject* obj = sel[i];
        if (obj && obj->type == DedObjectType::DED_TERRAIN) {
            terrain = static_cast<DedTerrain*>(obj);
            break;
        }
    }
    if (!terrain) return;

    terrain_paint_end_stroke(nullptr);
    terrain_clamp_properties(terrain->data);
    g_terrain_dlg = TerrainDialogState{};
    g_terrain_dlg.terrain = terrain;
    g_terrain_dlg.data = terrain->data;
    const DedTerrainData before = terrain->data;
    const std::string before_name = terrain->script_name.c_str();

    const INT_PTR result = DialogBoxParam(reinterpret_cast<HINSTANCE>(&__ImageBase),
                                          MAKEINTRESOURCE(IDD_ALPINE_TERRAIN_PROPERTIES), GetActiveWindow(),
                                          TerrainDialogProc, 0);
    if (result == IDOK || result == IDC_TERRAIN_TOOLS || result == IDC_TERRAIN_CONVERT) {
        // Overlays never need a rebuild; nothing changed marks nothing.
        const TerrainEdit edit = before_name == terrain->script_name.c_str() ? terrain_edit_kind(before, terrain->data)
                                                                              : TerrainEdit::other;
        if (edit == TerrainEdit::overlays_only) mark_level_modified();
        else if (edit == TerrainEdit::other) level->mark_geometry_dirty();
    }

    g_terrain_dlg = TerrainDialogState{};
    redraw_all_viewports();

    if (result == IDC_TERRAIN_TOOLS) terrain_paint_open(level, terrain);
    else if (result == IDC_TERRAIN_CONVERT) terrain_convert_to_brushes(level, terrain);
}

// ─── Object Lifecycle ───────────────────────────────────────────────────────

static bool terrain_can_add(CDedLevel* level, const DedTerrainData& d, bool interactive)
{
    auto& terrains = level->GetAlpineLevelProperties().terrain_objects;
    const char* why = nullptr;
    if (terrains.size() >= at::max_terrains) {
        why = "A level holds at most 64 terrains.";
    }
    else if (terrain_level_raw_bytes_except(level, nullptr) + terrain_data_raw_bytes(d) >
             at::max_level_raw_bytes) {
        why = "The level's terrain data limit has been reached.";
    }
    if (!why) return true;
    xlog::warn("[Terrain] {}", why);
    if (interactive) show_error_message(why);
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

void terrain_render(CDedLevel* level)
{
    auto& terrains = level->GetAlpineLevelProperties().terrain_objects;
    if (!terrains.empty()) terrain_load_icon();
    const float cam_param = gr_cam_param;

    for (auto* terrain : terrains) {
        if (terrain->hidden_in_editor) continue;

        // Axis-aligned by definition: a rotate tool pass leaves nothing behind.
        terrain_set_identity(terrain->orient);

        const bool preview = g_terrain_dlg.active && g_terrain_dlg.terrain == terrain;
        const bool selected = preview || is_object_selected(level, terrain);
        terrain_preview_draw(*level, *terrain, preview ? g_terrain_dlg.data : terrain->data, selected);

        const auto& rgb = selected ? terrain_selected_rgb : terrain_unselected_rgb;
        set_draw_color(rgb[0], rgb[1], rgb[2], 0xff);
        if (g_terrain_icon_handle >= 0) {
            gr_set_bitmap(g_terrain_icon_handle, -1);
        }
        gr_render_billboard(&terrain->pos, 0, 0.25f, cam_param);
    }
    terrain_paint_draw_cursor(*level);
    terrain_preview_frame_end(*level);
}

// Marquee: the origin handle inside the box, or the whole footprint.
void terrain_pick(CDedLevel* level, int param1, int param2)
{
    auto& terrains = level->GetAlpineLevelProperties().terrain_objects;
    for (auto* terrain : terrains) {
        if (terrain->hidden_in_editor) continue;
        bool hit = level->hit_test_point(param1, param2, &terrain->pos);
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
                                   alpine_click_pick_radius_sq);
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
        show_error_message(std::format("{} terrain(s) were not pasted: the level would exceed its terrain "
                                       "count or data limit.",
                                       skipped)
                               .c_str());
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
