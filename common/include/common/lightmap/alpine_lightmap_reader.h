#pragma once

// Alpine Lightmaps — the reader half of the 0x0AFBAE09 contract: the validation order every
// consumer must run in, and the GPU index buffer layout the D3D11 pixel shader indexes with.
// Pure and engine free, so the game and RED run the same code.
// Every derived quantity still comes from alpine_lightmap.h; nothing is re-derived here.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "alpine_lightmap.h"
#include "../terrain/alpine_terrain.h"

namespace alpine_lightmap {

// Stock lightmap fragment dimensions of one geometry surface, border ring included.
struct SurfaceDims
{
    std::uint32_t w;
    std::uint32_t h;
};

struct LayerRef
{
    std::uint32_t payload_offset;      // from the start of the section
    std::uint32_t stored_size;
    std::uint32_t uncompressed_size;
    std::uint16_t codec;
    std::uint8_t compression;
};

// The surface charts and the terrain charts are validated independently: a section whose surface
// fingerprint no longer matches (or whose reader has no surfaces to check against) still carries
// usable terrain charts, and the other way round. ok means at least one half is usable.
struct ReadResult
{
    bool ok = false;
    const char* reason = "";
    SectionHeader head{};
    std::uint32_t page_size = 0;
    std::uint32_t tile_step = 0;
    std::uint32_t gutter = 0;
    bool surfaces_ok = false;
    const char* surface_reason = "";
    std::vector<Chart> charts;         // as stored, positional over the surfaces
    std::vector<ChartGeometry> geoms;  // derived, one per chart; empty unless surfaces_ok
    std::vector<std::uint32_t> bases;  // first tile index of each chart; empty unless surfaces_ok
    std::vector<TerrainChart> terrain; // as stored
    std::vector<ChartGeometry> terrain_geoms;
    std::vector<std::uint32_t> terrain_bases;
    std::vector<std::uint8_t> terrain_ok; // per record: usable
    std::vector<Tile> tiles;
    LayerRef layer{};
};

// Checks a terrain record's own fields: its tiling is derived and checked with the table.
inline bool terrain_chart_fields_ok(const TerrainChart& t)
{
    return std::isfinite(t.origin_x) && std::isfinite(t.origin_z) && std::isfinite(t.texel_size)
        && t.texel_size > 0.0f && !terrain_chart_geometry(t.w, t.h).empty();
}

// Checked before a section body is allocated: it can never be longer than what is left of the file.
inline constexpr bool section_len_plausible(std::size_t chunk_len, std::int64_t bytes_left)
{
    return chunk_len > 0 && bytes_left >= 0
        && static_cast<std::uint64_t>(chunk_len) <= static_cast<std::uint64_t>(bytes_left);
}

// The header checks every consumer runs before trusting anything after the header. The atlas
// geometry must be exactly the one the writer emits.
inline const char* header_invalid_reason(const SectionHeader& head)
{
    if (head.version != section_version) {
        return "unsupported container version";
    }
    if (head.page_size != page_size || head.tile_step != tile_step || head.gutter != gutter) {
        return "bad atlas geometry";
    }
    if (head.num_pages == 0 || head.num_pages > max_pages) {
        return "bad page count";
    }
    return nullptr;
}

// Only these combinations are decoded. Anything else is an unknown layer and is skipped, which
// is how future semantics/codecs land without breaking this build. raw_rgb8 is the writer's
// -rawlightmaps debug codec and is kept readable so a debug bake renders like a shipping one.
inline constexpr bool layer_supported(std::uint16_t semantic, std::uint16_t codec,
                                      std::uint16_t version, std::uint8_t colorspace,
                                      std::uint8_t compression)
{
    return semantic == static_cast<std::uint16_t>(Semantic::radiance_ldr)
        && (codec == static_cast<std::uint16_t>(Codec::bc7_unorm)
            || codec == static_cast<std::uint16_t>(Codec::raw_rgb8))
        && version == layer_version
        && colorspace == static_cast<std::uint8_t>(Colorspace::rf_lightmap_x2)
        && (compression == static_cast<std::uint8_t>(Compression::none)
            || compression == static_cast<std::uint8_t>(Compression::zlib));
}

namespace detail {

inline ReadResult fail(const char* reason)
{
    ReadResult r;
    r.ok = false;
    r.reason = reason;
    return r;
}

template<typename T>
inline void load(T& dst, const std::uint8_t* p)
{
    std::memcpy(&dst, p, sizeof(T));
}

} // namespace detail

// Every tile of chart geometry `g` from `base` stays on a page of the section.
inline bool chart_tiles_on_pages(const ReadResult& r, const ChartGeometry& g, std::uint32_t base)
{
    for (std::uint32_t ty = 0; ty < g.ny; ty++) {
        for (std::uint32_t tx = 0; tx < g.nx; tx++) {
            const std::uint32_t idx = tile_index(base, g, tx, ty);
            if (idx >= r.tiles.size()) {
                return false;
            }
            const Tile& t = r.tiles[idx];
            const TileDims td = tile_dims(g, tx, ty, r.tile_step);
            if (t.page >= r.head.num_pages || t.x + td.w_t > r.page_size
                || t.y + td.h_t > r.page_size) {
                return false;
            }
        }
    }
    return true;
}

namespace detail {

// The surface half: nullptr when it is usable, else why not. terrain_tiles is the tile count the
// terrain charts own at the end of the tile table.
inline const char* validate_surfaces(ReadResult& r, const SurfaceDims* dims, std::uint32_t num_surfaces,
                                     std::uint32_t expect_surface_hash, bool have_hash,
                                     std::uint64_t terrain_tiles)
{
    if (r.head.num_charts != num_surfaces) {
        return "chart count does not match the loaded geometry";
    }
    if (r.head.src_num_surfaces != num_surfaces) {
        return "surface count fingerprint";
    }
    if (!have_hash) {
        return "surface fingerprint unavailable";
    }
    if (r.head.src_surface_hash != expect_surface_hash) {
        return "surface fingerprint";
    }
    if (num_surfaces > 0 && !dims) {
        return "surface dimensions unavailable";
    }
    if (terrain_tiles > r.head.num_tiles) {
        return "tile count does not match the derived atlas";
    }
    const std::uint64_t surface_tiles = r.head.num_tiles - terrain_tiles;
    // sized only now: the count check above bounds them by the loaded geometry, not by the file
    r.geoms.assign(r.head.num_charts, ChartGeometry{0, 0, 0, 0, 0, 0});
    r.bases.assign(r.head.num_charts, 0);
    std::uint64_t derived_tiles = 0;
    for (std::uint32_t i = 0; i < r.head.num_charts; i++) {
        if (r.charts[i].k_u == 0 || r.charts[i].k_v == 0) {
            continue;
        }
        r.geoms[i] = chart_geometry(dims[i].w, dims[i].h, r.charts[i].k_u, r.charts[i].k_v,
                                    r.tile_step, r.gutter);
        // Tile counts accumulate in u32 elsewhere; bound them in u64 so a huge chart cannot wrap.
        derived_tiles += static_cast<std::uint64_t>(r.geoms[i].nx) * r.geoms[i].ny;
        if (derived_tiles > surface_tiles) {
            return "tile count does not match the derived atlas";
        }
    }
    if (derived_tiles != surface_tiles) {
        return "tile count does not match the derived atlas";
    }
    if (compute_tile_bases(r.geoms.data(), r.head.num_charts, r.bases.data()) != surface_tiles) {
        return "tile count does not match the derived atlas";
    }
    for (std::uint32_t i = 0; i < r.head.num_charts; i++) {
        if (!chart_tiles_on_pages(r, r.geoms[i], r.bases[i])) {
            return "tile rect leaves its page";
        }
    }
    return nullptr;
}

} // namespace detail

// `expect_surface_hash` is the xxhash32 the reader computed over the same bytes the writer
// hashed (the serialized geometry::surfaces records). Pass have_hash false only where the
// reader could not obtain it, which is itself a reason to refuse the surface charts; dims may be
// null when the caller only wants the terrain charts.
inline ReadResult read_section(const std::uint8_t* data, std::size_t len, const SurfaceDims* dims,
                               std::uint32_t num_surfaces, std::uint32_t expect_surface_hash,
                               bool have_hash)
{
    using detail::fail;
    using detail::load;

    if (!data || len < sizeof(SectionHeader)) {
        return fail("truncated header");
    }

    ReadResult r;
    std::memcpy(&r.head, data, sizeof(SectionHeader));

    if (const char* reason = header_invalid_reason(r.head)) {
        return fail(reason);
    }
    r.page_size = r.head.page_size;
    r.tile_step = r.head.tile_step;
    r.gutter = r.head.gutter;

    const std::uint64_t charts_end =
        sizeof(SectionHeader) + static_cast<std::uint64_t>(r.head.num_charts) * sizeof(Chart);
    if (charts_end + sizeof(std::uint32_t) > len) {
        return fail("truncated chart/tile tables");
    }
    std::uint32_t num_terrain = 0;
    load(num_terrain, data + charts_end);
    if (num_terrain > max_terrain_charts) {
        return fail("too many terrain charts");
    }
    const std::uint64_t tables = charts_end + sizeof(std::uint32_t)
                               + static_cast<std::uint64_t>(num_terrain) * sizeof(TerrainChart)
                               + static_cast<std::uint64_t>(r.head.num_tiles) * sizeof(Tile);
    if (tables > len) {
        return fail("truncated chart/tile tables");
    }
    // every stored tile is at least one 4x4 block, so no more than (P/4)^2 fit on a page
    const std::uint64_t tiles_per_page = static_cast<std::uint64_t>(r.page_size / 4) * (r.page_size / 4);
    if (r.head.num_tiles > static_cast<std::uint64_t>(r.head.num_pages) * tiles_per_page) {
        return fail("tile count exceeds what the pages can hold");
    }

    std::size_t off = sizeof(SectionHeader);
    r.charts.resize(r.head.num_charts);
    for (std::uint32_t i = 0; i < r.head.num_charts; i++) {
        load(r.charts[i], data + off);
        off += sizeof(Chart);
    }
    off += sizeof(std::uint32_t);
    r.terrain.resize(num_terrain);
    for (std::uint32_t i = 0; i < num_terrain; i++) {
        load(r.terrain[i], data + off);
        off += sizeof(TerrainChart);
    }
    r.tiles.resize(r.head.num_tiles);
    for (std::uint32_t i = 0; i < r.head.num_tiles; i++) {
        load(r.tiles[i], data + off);
        off += sizeof(Tile);
    }

    // Terrain tiles close the tile table, so their layout needs nothing from the surfaces: a
    // record with an unreadable size makes the whole table's layout unknown.
    r.terrain_geoms.assign(num_terrain, ChartGeometry{0, 0, 0, 0, 0, 0});
    r.terrain_bases.assign(num_terrain, 0);
    r.terrain_ok.assign(num_terrain, 0);
    const char* terrain_reason = nullptr;
    std::uint64_t terrain_tiles = 0;
    std::uint64_t terrain_texels = 0;
    for (std::uint32_t i = 0; i < num_terrain && !terrain_reason; i++) {
        r.terrain_geoms[i] = terrain_chart_geometry(r.terrain[i].w, r.terrain[i].h, r.tile_step, r.gutter);
        if (r.terrain_geoms[i].empty()) {
            terrain_reason = "terrain chart size";
        }
        terrain_tiles += static_cast<std::uint64_t>(r.terrain_geoms[i].nx) * r.terrain_geoms[i].ny;
        terrain_texels += static_cast<std::uint64_t>(r.terrain[i].w) * r.terrain[i].h;
    }
    if (!terrain_reason && terrain_tiles > r.head.num_tiles) {
        terrain_reason = "terrain tiles exceed the tile table";
    }
    // a chart's texels are the interiors of its tiles, which cannot outgrow the pages they occupy
    if (!terrain_reason
        && (terrain_texels > max_terrain_chart_texels
            || terrain_texels > static_cast<std::uint64_t>(r.head.num_pages) * r.page_size * r.page_size)) {
        terrain_reason = "terrain charts exceed the page budget";
    }
    if (!terrain_reason) {
        std::uint32_t base = r.head.num_tiles - static_cast<std::uint32_t>(terrain_tiles);
        for (std::uint32_t i = 0; i < num_terrain; i++) {
            r.terrain_bases[i] = base;
            base += r.terrain_geoms[i].tile_count();
            bool usable = terrain_chart_fields_ok(r.terrain[i])
                       && chart_tiles_on_pages(r, r.terrain_geoms[i], r.terrain_bases[i]);
            // a uid charted twice identifies neither chart
            for (std::uint32_t j = 0; j < num_terrain; j++) {
                if (j != i && r.terrain[j].terrain_uid == r.terrain[i].terrain_uid) {
                    usable = false;
                }
            }
            r.terrain_ok[i] = usable ? 1 : 0;
        }
    }
    else {
        terrain_tiles = 0;
    }

    const char* surface_reason = detail::validate_surfaces(r, dims, num_surfaces, expect_surface_hash,
                                                           have_hash, terrain_tiles);
    r.surfaces_ok = surface_reason == nullptr;
    r.surface_reason = surface_reason ? surface_reason : "";
    if (!r.surfaces_ok) {
        r.geoms.clear();
        r.bases.clear();
    }
    bool any_terrain = false;
    for (std::uint8_t v : r.terrain_ok) {
        any_terrain = any_terrain || v != 0;
    }
    if (!r.surfaces_ok && !any_terrain) {
        return fail(num_terrain && terrain_reason ? terrain_reason : r.surface_reason);
    }

    if (off + sizeof(LayerDirHeader) > len) {
        return fail("truncated layer directory");
    }
    LayerDirHeader dir{};
    load(dir, data + off);
    off += sizeof(LayerDirHeader);
    if (dir.num_layers == 0) {
        return fail("no layers");
    }
    if (off + static_cast<std::uint64_t>(dir.num_layers) * sizeof(LayerDirEntry) > len) {
        return fail("truncated layer directory");
    }

    std::vector<LayerDirEntry> entries(dir.num_layers);
    for (std::uint8_t i = 0; i < dir.num_layers; i++) {
        load(entries[i], data + off);
        off += sizeof(LayerDirEntry);
    }

    bool picked = false;
    for (std::uint8_t i = 0; i < dir.num_layers; i++) {
        const LayerDirEntry& e = entries[i];
        if (off + e.stored_size > len || off + e.stored_size < off) {
            return fail("truncated layer payload");
        }
        if (!picked
            && layer_supported(e.semantic, e.codec, e.layer_version, e.colorspace, e.compression)) {
            const std::uint64_t want =
                layer_payload_size(static_cast<Codec>(e.codec), r.head.num_pages, r.page_size);
            if (want == 0 || want > max_layer_bytes || want != e.uncompressed_size) {
                return fail("layer payload size");
            }
            if (e.compression == static_cast<std::uint8_t>(Compression::none)
                && e.stored_size != e.uncompressed_size) {
                return fail("layer payload size");
            }
            r.layer = LayerRef{static_cast<std::uint32_t>(off), e.stored_size, e.uncompressed_size,
                               e.codec, e.compression};
            picked = true;
        }
        off += e.stored_size;
    }
    if (!picked) {
        return fail("no supported layer");
    }

    r.ok = true;
    r.reason = "";
    return r;
}

// GPU index buffer, a typed Buffer<uint4> (R32G32B32A32_UINT, so it stays inside shader
// model 4, which is what both pixel shader permutations compile to). S is gpu_surface_records(),
// T the terrain chart count:
//   [0, S)              one record per geometry surface, positionally (none unless surfaces_ok)
//   [S, S + T)          one record per terrain chart, table order (gpu_terrain_chart)
//       .x = nx, .y = ny                  0 means no usable chart
//       .z = absolute index IN THIS BUFFER of the chart's first tile record
//       .w = pad_u | (pad_v << 16)
//   [S + T, S + T + num_tiles)  one record per tile, chart order, row major
//       .x = page, .y = x, .z = y, .w = 0
// Charts and tiles share one buffer so the pixel shader needs a single SRV slot; .z is already
// biased past the chart records so the shader adds ty * nx + tx and nothing else.
inline std::uint32_t gpu_surface_records(const ReadResult& r)
{
    return r.surfaces_ok ? r.head.num_charts : 0u;
}

inline std::uint32_t gpu_terrain_chart(const ReadResult& r, std::uint32_t terrain_index)
{
    return gpu_surface_records(r) + terrain_index;
}

inline std::vector<std::uint32_t> build_gpu_index(const ReadResult& r)
{
    const std::uint32_t num_surface = gpu_surface_records(r);
    const std::uint32_t num_terrain = static_cast<std::uint32_t>(r.terrain.size());
    const std::uint32_t num_records = num_surface + num_terrain;
    const std::uint32_t num_tiles = r.head.num_tiles;
    std::vector<std::uint32_t> out(static_cast<std::size_t>(num_records + num_tiles) * 4, 0);
    auto put_chart = [&](std::uint32_t record, const ChartGeometry& g, std::uint32_t base) {
        std::uint32_t* rec = out.data() + static_cast<std::size_t>(record) * 4;
        if (g.empty()) {
            return;
        }
        rec[0] = g.nx;
        rec[1] = g.ny;
        rec[2] = num_records + base;
        rec[3] = (g.pad_u & 0xffffu) | (g.pad_v << 16);
    };
    for (std::uint32_t i = 0; i < num_surface; i++) {
        put_chart(i, r.geoms[i], r.bases[i]);
    }
    for (std::uint32_t i = 0; i < num_terrain; i++) {
        if (r.terrain_ok[i]) {
            put_chart(gpu_terrain_chart(r, i), r.terrain_geoms[i], r.terrain_bases[i]);
        }
    }
    for (std::uint32_t i = 0; i < num_tiles; i++) {
        std::uint32_t* rec = out.data() + static_cast<std::size_t>(num_records + i) * 4;
        rec[0] = r.tiles[i].page;
        rec[1] = r.tiles[i].x;
        rec[2] = r.tiles[i].y;
    }
    return out;
}

// Unpacks one 16 byte BC7 block into 16 RGBA8 pixels, row major.
using Bc7BlockDecoder = bool (*)(const void* block, std::uint8_t* rgba);

// Decodes usable terrain chart `index` into out, w * h RGB8 texels row major, reading every texel
// from the tile whose interior owns it. `layer` is the decompressed layer the section's layer
// directory describes. False when the chart is unusable or the layer does not have its size.
inline bool decode_terrain_chart(const ReadResult& r, const std::uint8_t* layer, std::size_t layer_len,
                                 std::uint32_t index, Bc7BlockDecoder decode_bc7,
                                 std::vector<std::uint8_t>& out)
{
    if (!r.ok || index >= r.terrain.size() || !r.terrain_ok[index] || !layer) {
        return false;
    }
    const auto codec = static_cast<Codec>(r.layer.codec);
    if (layer_len != layer_payload_size(codec, r.head.num_pages, r.page_size)
        || (codec == Codec::bc7_unorm && !decode_bc7)) {
        return false;
    }
    const ChartGeometry& g = r.terrain_geoms[index];
    const std::uint32_t p = r.page_size;
    const std::uint32_t bpr = p / 4;
    out.assign(static_cast<std::size_t>(g.cw) * g.ch * 3, 0);
    std::uint8_t block[64];
    for (std::uint32_t ty = 0; ty < g.ny; ty++) {
        for (std::uint32_t tx = 0; tx < g.nx; tx++) {
            const Tile& t = r.tiles[tile_index(r.terrain_bases[index], g, tx, ty)];
            const TileDims td = tile_dims(g, tx, ty, r.tile_step);
            const ChartCoord origin = tile_origin_chart(g, tx, ty, r.tile_step);
            // the interior this tile owns, in chart and in page texels
            const std::uint32_t u0 = tx * r.tile_step, v0 = ty * r.tile_step;
            const std::uint32_t px0 = t.x + static_cast<std::uint32_t>(u0 - origin.u);
            const std::uint32_t py0 = t.y + static_cast<std::uint32_t>(v0 - origin.v);
            if (codec == Codec::raw_rgb8) {
                for (std::uint32_t y = 0; y < td.ih_t; y++) {
                    for (std::uint32_t x = 0; x < td.iw_t; x++) {
                        const std::size_t src =
                            ((static_cast<std::size_t>(t.page) * p + py0 + y) * p + px0 + x) * 3;
                        std::memcpy(&out[(static_cast<std::size_t>(v0 + y) * g.cw + u0 + x) * 3],
                                    layer + src, 3);
                    }
                }
                continue;
            }
            for (std::uint32_t by = py0 / 4; by * 4 < py0 + td.ih_t; by++) {
                for (std::uint32_t bx = px0 / 4; bx * 4 < px0 + td.iw_t; bx++) {
                    const std::size_t b = (static_cast<std::size_t>(t.page) * bpr + by) * bpr + bx;
                    if (!decode_bc7(layer + b * 16, block)) {
                        std::memset(block, 0, sizeof(block));
                    }
                    for (std::uint32_t y = 0; y < 4; y++) {
                        const std::uint32_t py = by * 4 + y;
                        if (py < py0 || py >= py0 + td.ih_t) {
                            continue;
                        }
                        for (std::uint32_t x = 0; x < 4; x++) {
                            const std::uint32_t px = bx * 4 + x;
                            if (px < px0 || px >= px0 + td.iw_t) {
                                continue;
                            }
                            const std::size_t dst =
                                (static_cast<std::size_t>(v0 + py - py0) * g.cw + u0 + px - px0) * 3;
                            std::memcpy(&out[dst], block + (y * 4 + x) * 4, 3);
                        }
                    }
                }
            }
        }
    }
    return true;
}

// Box filters a decoded w x h chart down by `reduction` (terrain_chart_reduction) in place. False,
// leaving it untouched, when the reduction does not divide the chart.
inline bool reduce_terrain_chart(std::vector<std::uint8_t>& rgb, std::uint32_t w, std::uint32_t h,
                                 std::uint32_t reduction)
{
    const std::uint32_t r = reduction;
    if (r == 0 || w % r != 0 || h % r != 0 || rgb.size() != static_cast<std::size_t>(w) * h * 3) {
        return false;
    }
    if (r == 1) {
        return true;
    }
    const std::uint32_t rw = w / r, rh = h / r, n = r * r;
    // reduced texel i reads only chart texels at or past index i, so nothing is overwritten unread
    for (std::uint32_t y = 0; y < rh; y++) {
        for (std::uint32_t x = 0; x < rw; x++) {
            std::uint32_t sum[3] = {0, 0, 0};
            for (std::uint32_t dy = 0; dy < r; dy++) {
                const std::uint8_t* src = &rgb[((static_cast<std::size_t>(y) * r + dy) * w + x * r) * 3];
                for (std::uint32_t i = 0; i < r * 3; i++) {
                    sum[i % 3] += src[i];
                }
            }
            std::uint8_t* dst = &rgb[(static_cast<std::size_t>(y) * rw + x) * 3];
            for (int c = 0; c < 3; c++) {
                dst[c] = static_cast<std::uint8_t>((sum[c] + n / 2) / n);
            }
        }
    }
    rgb.resize(static_cast<std::size_t>(rw) * rh * 3);
    rgb.shrink_to_fit();
    return true;
}

// decode_terrain_chart then reduce_terrain_chart by the reduction of the chart's density over a grid
// cells_x cells across, which lands in `reduction`. False when either step fails.
inline bool decode_reduced_terrain_chart(const ReadResult& r, const std::uint8_t* layer, std::size_t layer_len,
                                         std::uint32_t index, std::uint32_t cells_x, Bc7BlockDecoder decode_bc7,
                                         std::vector<std::uint8_t>& out, std::uint32_t& reduction)
{
    if (index >= r.terrain.size()) {
        return false;
    }
    const TerrainChart& c = r.terrain[index];
    const std::uint32_t red = terrain_chart_reduction(terrain_chart_density(c, cells_x));
    if (!decode_terrain_chart(r, layer, layer_len, index, decode_bc7, out) || !reduce_terrain_chart(out, c.w, c.h, red)) {
        return false;
    }
    reduction = red;
    return true;
}

// Bilinear read of chart `c` reduced by `reduction` (decode_reduced_terrain_chart) at world (x, z).
inline void terrain_reduced_chart_sample(const std::uint8_t* rgb, const TerrainChart& c, std::uint32_t reduction,
                                         float world_x, float world_z, float (&out)[3])
{
    chart_sample_bilinear(rgb, c.w / reduction, c.h / reduction,
                          terrain_reduced_chart_coord(c.origin_x, c.texel_size, reduction, world_x),
                          terrain_reduced_chart_coord(c.origin_z, c.texel_size, reduction, world_z), out);
}

// Whether chart `c` lights terrain grid `g` as it is now: it covers the grid's cells at a whole density
// of 1..lightmap_density_max and was baked for its lighting_fingerprint. Matching the uid is the caller's.
enum class TerrainChartFit : std::uint8_t {
    match,
    other_grid,
    stale,
};

inline TerrainChartFit terrain_chart_fit(const TerrainChart& c, const alpine_terrain::GridView& g)
{
    namespace at = alpine_terrain;
    if (!terrain_chart_fits_grid(c, at::cells(g.nx), at::cells(g.nz), at::lightmap_density_max)) {
        return TerrainChartFit::other_grid;
    }
    if (c.geometry_fingerprint != at::lighting_fingerprint(g)) {
        return TerrainChartFit::stale;
    }
    return TerrainChartFit::match;
}

inline bool terrain_chart_matches(const TerrainChart& c, const alpine_terrain::GridView& g)
{
    return terrain_chart_fit(c, g) == TerrainChartFit::match;
}

} // namespace alpine_lightmap
