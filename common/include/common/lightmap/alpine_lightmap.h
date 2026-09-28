#pragma once

// Alpine Lightmaps, RFL section 0x0AFBAE09: the wire spec and the only atlas derivation, shared by
// the RED writer and the game reader.
//
// POD types and free functions only. Both binaries include this and both hand the results to
// engine code, so nothing here may allocate, throw, or pass a non-trivial type by value.

#include <cstddef>
#include <cstdint>
#include <cmath>
#include <algorithm>

namespace alpine_lightmap
{

// ─── Container constants ──────────────────────────────────────────────────────
// The section grows only through section_version, a table's version or layer_version, never the RFL
// version: RED re-emits a retained section byte for byte into files stamped with newer RFL versions.

inline constexpr std::uint32_t chunk_id       = 0x0AFBAE09u;
inline constexpr std::uint32_t section_version = 2u;
inline constexpr std::uint16_t layer_version   = 1u;

inline constexpr std::uint32_t page_size = 256; // P, edge length of every page
inline constexpr std::uint32_t tile_step = 244; // S, chart texel step between tile origins
inline constexpr std::uint32_t gutter    = 4;   // G, overlap stored on each side of a tiled chart

static_assert(tile_step % 4 == 0 && gutter % 4 == 0 && page_size % 4 == 0);
static_assert(tile_step + 2 * gutter <= page_size);

// RED's page budget for one section, and the most pages any reader accepts.
inline constexpr std::uint32_t max_pages = 1024;

// Edge of the stock lightmap pages that surface uv_scale/uv_add normalize against (highres doubles it).
inline constexpr std::uint32_t stock_page_size(bool highres)
{
    return highres ? 256u : 128u;
}

// SectionHeader::stock_page_log2: the stock page edge the surfaces were packed and normalized at.
inline constexpr std::uint8_t stock_page_log2_min = 7;
inline constexpr std::uint8_t stock_page_log2_max = 8;

inline constexpr std::uint8_t stock_page_log2_of(std::uint32_t edge)
{
    return edge == stock_page_size(true) ? stock_page_log2_max : stock_page_log2_min;
}

inline constexpr bool stock_page_edge_valid(std::uint32_t edge)
{
    return edge == stock_page_size(false) || edge == stock_page_size(true);
}

// ─── Level property `d3d11_only_lightmaps` (u8 bit field) ─────────────────────
// bit0: the file carries no stock 0x1200 lightmaps section, which is all the game reads.
// bit1: the editor setting, kept apart so a save that still wrote the stock section keeps it.
// bits 2-7: reserved, written as 0.

inline constexpr std::uint8_t d3d11_only_stock_omitted = 1u << 0;
inline constexpr std::uint8_t d3d11_only_setting       = 1u << 1;

// ─── Density ──────────────────────────────────────────────────────────────────
// Level property `lightmap_density`: 0 = the default, 1-128 = texels per world unit, 129-254 clamp to
// 128, density_off (255) = Off, no alpine surface charts. The game never reads it.

inline constexpr std::uint8_t density_default = 8;
inline constexpr std::uint8_t density_min     = 1;
inline constexpr std::uint8_t density_max     = 128;
inline constexpr std::uint8_t density_off     = 255;
static_assert(density_off > density_max);

inline constexpr std::uint8_t effective_density(std::uint8_t stored)
{
    return stored == 0 ? density_default : std::clamp(stored, density_min, density_max);
}

// ─── Wire enums ───────────────────────────────────────────────────────────────
// Append-only: never reuse or renumber a value.

enum class Semantic : std::uint16_t
{
    radiance_ldr = 0,
    radiance_hdr = 1,
    directional_l1 = 2,
    ambient_occlusion = 3,
};

enum class Codec : std::uint16_t
{
    raw_rgb8 = 0,
    bc7_unorm = 1,
    bc6h_uf16 = 2,
};

enum class Colorspace : std::uint8_t
{
    rf_lightmap_x2 = 0,
    srgb = 1,
    linear = 2,
};

enum class Compression : std::uint8_t
{
    none = 0,
    zlib = 1,
};

// TableHeader::tag. Append-only: never reuse or renumber a value.
enum class TableTag : std::uint32_t
{
    movers = 1,
    terrain = 2,
};

// ─── Wire records ─────────────────────────────────────────────────────────────

#pragma pack(push, 1)

struct SectionHeader
{
    std::uint32_t version;
    std::uint32_t flags; // written 0, ignored by readers
    std::uint16_t page_size;
    std::uint16_t tile_step;
    std::uint8_t  gutter;
    std::uint8_t  base_density;
    std::uint8_t  stock_page_log2;
    std::uint8_t  reserved; // written 0, ignored by readers
    std::uint16_t num_pages;
    std::uint32_t num_charts;
    std::uint32_t num_tiles;
    std::uint32_t src_num_faces;
    std::uint32_t src_num_surfaces;
    std::uint32_t src_surface_hash;
};

// One per geometry surface, positionally. k_u == 0 or k_v == 0 means this surface has no
// Alpine chart and contributes no tiles.
struct Chart
{
    std::uint16_t k_u;
    std::uint16_t k_v;
};

// Terrain table (TableTag::terrain) body: u32 num_terrain_charts, then this record per terrain. A
// terrain chart is w x h texels over the terrain's XZ footprint (terrain_texel_center); its tiles run
// in record order. geometry_fingerprint is alpine_terrain::chart_fingerprint of the terrain the chart
// was baked for (its lighting_fingerprint while no decoration casts shadows).
struct TerrainChart
{
    std::int32_t terrain_uid;
    std::uint16_t w;
    std::uint16_t h;
    float origin_x;
    float origin_z;
    float texel_size;
    std::uint64_t geometry_fingerprint;
};

// The table directory follows the surface charts: u32 num_tables, then per table this header and
// byte_len bytes of body. Every table owns num_tiles tiles of the tile table, after the surface
// tiles and in directory order, so a reader skips a table it does not know without losing the
// layout. A tag present more than once, or a known tag at another version, is skipped the same way.
struct TableHeader
{
    std::uint32_t tag;
    std::uint16_t version;
    std::uint16_t reserved; // written 0, ignored by readers
    std::uint32_t byte_len;
    std::uint32_t num_tiles;
};

// Mover table (TableTag::movers) body: u32 num_movers, then per mover this record followed by
// num_surfaces MoverSurfaceChart, positional over the mover solid's surfaces. surface_hash is the
// XXH32 of that solid's surface records as written in the movers section (0x2000), XXH32("") for
// none. Tiles run in record order, surface by surface, row major per chart.
struct MoverChart
{
    std::int32_t mover_uid;
    std::uint32_t num_surfaces;
    std::uint32_t surface_hash;
};

// k_u == 0 or k_v == 0 means no chart and no tiles; w, h are the stock fragment the chart refines.
struct MoverSurfaceChart
{
    std::uint16_t k_u;
    std::uint16_t k_v;
    std::uint16_t w;
    std::uint16_t h;
};

struct Tile
{
    std::uint16_t page;
    std::uint16_t x;
    std::uint16_t y;
};

struct LayerDirHeader
{
    std::uint8_t num_layers;
    std::uint8_t reserved2[3]; // written 0, ignored by readers
};

struct LayerDirEntry
{
    std::uint16_t semantic;
    std::uint16_t codec;
    std::uint16_t layer_version;
    std::uint8_t  colorspace;
    std::uint8_t  compression;
    std::uint32_t uncompressed_size;
    std::uint32_t stored_size;
};

#pragma pack(pop)

static_assert(sizeof(SectionHeader) == 38);
static_assert(sizeof(Chart) == 4);
static_assert(sizeof(TerrainChart) == 28);
static_assert(sizeof(TableHeader) == 16);
static_assert(sizeof(MoverChart) == 12);
static_assert(sizeof(MoverSurfaceChart) == 8);
static_assert(sizeof(Tile) == 6);

// A reader cap, not a wire value: it can be raised freely.
inline constexpr std::uint32_t max_tables = 16;

inline constexpr std::uint16_t mover_table_version = 1;
inline constexpr std::uint16_t terrain_table_version = 1;

// A face's surface index is a short, and the RFL stores a fragment's w and h as u8.
inline constexpr std::uint32_t max_mover_charts = 8192;
inline constexpr std::uint32_t max_mover_surfaces = 32767;
inline constexpr std::uint32_t max_mover_surfaces_total = 262144;
inline constexpr std::uint32_t max_fragment_dim = 255;

// Bytes one GSurface record occupies in a solid as the RFL stores it; the fingerprints hash these.
inline constexpr std::uint32_t surface_record_size = 96;

inline constexpr std::uint64_t mover_table_bytes(std::uint64_t num_movers, std::uint64_t num_surfaces)
{
    return sizeof(std::uint32_t) + num_movers * sizeof(MoverChart) + num_surfaces * sizeof(MoverSurfaceChart);
}

// A terrain is at most 256 cells per axis at 8 texels per cell. The texel total a section's terrain
// charts may declare is what the page budget can hold, which bounds a reader's decoded copies.
inline constexpr std::uint32_t max_terrain_charts = 64;
inline constexpr std::uint32_t max_terrain_chart_dim = 2048;
inline constexpr std::uint64_t max_terrain_chart_texels =
    static_cast<std::uint64_t>(max_pages) * tile_step * tile_step;

inline constexpr std::uint64_t terrain_table_bytes(std::uint64_t num_terrain)
{
    return sizeof(std::uint32_t) + num_terrain * sizeof(TerrainChart);
}

static_assert(sizeof(LayerDirHeader) == 4);
static_assert(sizeof(LayerDirEntry) == 16);

// ─── THE derivation ───────────────────────────────────────────────────────────

inline constexpr std::uint32_t align4(std::uint32_t v)
{
    return (v + 3u) & ~3u;
}

inline constexpr std::uint32_t ceil_div(std::uint32_t a, std::uint32_t b)
{
    return b == 0 ? 0 : (a + b - 1) / b;
}

// Everything about a chart that is derived rather than stored.
struct ChartGeometry
{
    std::uint32_t cw;    // chart texel width  = (surface.w - 2) * k_u
    std::uint32_t ch;    // chart texel height = (surface.h - 2) * k_v
    std::uint32_t nx;    // tiles across
    std::uint32_t ny;    // tiles down
    std::uint32_t pad_u; // stored texels of border/overlap on each side, horizontally
    std::uint32_t pad_v;

    constexpr std::uint32_t tile_count() const { return nx * ny; }
    constexpr bool empty() const { return cw == 0 || ch == 0; }
};

// Charts above this are invalid; at the cap every derived quantity (nx <= 4298, nx * ny <= 18.5M) fits u32.
inline constexpr std::uint64_t max_chart_dim = 1u << 20;

// The tiling of a cw x ch chart, whatever it covers. A single tile stores a one texel ring past the
// chart, a tiled axis the gutter.
inline constexpr ChartGeometry chart_geometry_from_extent(std::uint64_t cw, std::uint64_t ch,
                                                          std::uint32_t step = tile_step,
                                                          std::uint32_t g = gutter)
{
    ChartGeometry r{};
    if (cw == 0 || ch == 0 || cw > max_chart_dim || ch > max_chart_dim) {
        return r;
    }
    r.cw = static_cast<std::uint32_t>(cw);
    r.ch = static_cast<std::uint32_t>(ch);
    r.nx = ceil_div(r.cw, step);
    r.ny = ceil_div(r.ch, step);
    r.pad_u = (r.nx == 1) ? 1u : g;
    r.pad_v = (r.ny == 1) ? 1u : g;
    return r;
}

// surface_w/surface_h are the stock lightmap fragment dimensions (which always include the
// one texel border ring), k_u/k_v the stored integer refinement factors.
inline constexpr ChartGeometry chart_geometry(std::uint32_t surface_w, std::uint32_t surface_h,
                                              std::uint32_t k_u, std::uint32_t k_v,
                                              std::uint32_t step = tile_step,
                                              std::uint32_t g = gutter)
{
    if (surface_w <= 2 || surface_h <= 2 || k_u == 0 || k_v == 0) {
        return ChartGeometry{};
    }
    return chart_geometry_from_extent(static_cast<std::uint64_t>(surface_w - 2) * k_u,
                                      static_cast<std::uint64_t>(surface_h - 2) * k_v, step, g);
}

// Empty for no chart, and for a fragment size no RFL can store.
inline constexpr ChartGeometry mover_chart_geometry(const MoverSurfaceChart& c, std::uint32_t step = tile_step,
                                                    std::uint32_t g = gutter)
{
    if (c.w > max_fragment_dim || c.h > max_fragment_dim) {
        return ChartGeometry{};
    }
    return chart_geometry(c.w, c.h, c.k_u, c.k_v, step, g);
}

// Empty for a record whose size no reader accepts.
inline constexpr ChartGeometry terrain_chart_geometry(std::uint32_t w, std::uint32_t h,
                                                      std::uint32_t step = tile_step,
                                                      std::uint32_t g = gutter)
{
    if (w > max_terrain_chart_dim || h > max_terrain_chart_dim) {
        return ChartGeometry{};
    }
    return chart_geometry_from_extent(w, h, step, g);
}

// Chart texel coordinate of a tile's stored texel (0, 0): tile-local (sx, sy) is chart
// (u + sx, v + sy), padding included.
struct ChartCoord
{
    std::int64_t u;
    std::int64_t v;
};

inline constexpr ChartCoord tile_origin_chart(const ChartGeometry& g, std::uint32_t tx, std::uint32_t ty,
                                              std::uint32_t step = tile_step)
{
    return ChartCoord{static_cast<std::int64_t>(tx) * step - g.pad_u,
                      static_cast<std::int64_t>(ty) * step - g.pad_v};
}

struct TileDims
{
    std::uint32_t iw_t; // interior texels of the chart this tile owns
    std::uint32_t ih_t;
    std::uint32_t w_t;  // stored size, interior plus padding, rounded up to a BC7 block
    std::uint32_t h_t;
};

inline constexpr TileDims tile_dims(const ChartGeometry& g, std::uint32_t tx, std::uint32_t ty,
                                    std::uint32_t step = tile_step)
{
    TileDims r{};
    if (tx >= g.nx || ty >= g.ny) {
        return r;
    }
    r.iw_t = std::min(step, g.cw - tx * step);
    r.ih_t = std::min(step, g.ch - ty * step);
    r.w_t = align4(r.iw_t + 2 * g.pad_u);
    r.h_t = align4(r.ih_t + 2 * g.pad_v);
    return r;
}

// Tiles are stored row major per chart and the charts' runs are concatenated in chart order,
// so a chart's first tile index is the running sum of tile_count() over the charts before it.
// Fills out_bases[0..num_charts) and returns the total tile count.
inline std::uint32_t compute_tile_bases(const ChartGeometry* geoms, std::uint32_t num_charts,
                                        std::uint32_t* out_bases)
{
    std::uint32_t base = 0;
    for (std::uint32_t i = 0; i < num_charts; ++i) {
        if (out_bases) {
            out_bases[i] = base;
        }
        base += geoms[i].tile_count();
    }
    return base;
}

inline constexpr std::uint32_t tile_index(std::uint32_t base, const ChartGeometry& g,
                                          std::uint32_t tx, std::uint32_t ty)
{
    return base + ty * g.nx + tx;
}

// Stock page UV -> chart texel coordinate. page_u/page_v come from the stock lighting
// projection (pos[u_coefficient] * uv_scale.u + uv_add.u); lm_w/lm_h are the dimensions of
// the stock lightmap the surface's fragment lives in; surf_x/surf_y its fragment origin.
struct ChartTexel
{
    float u;
    float v;
};

inline ChartTexel chart_texel_from_page_uv(float page_u, float page_v,
                                           std::uint32_t lm_w, std::uint32_t lm_h,
                                           std::uint32_t surf_x, std::uint32_t surf_y,
                                           std::uint32_t k_u, std::uint32_t k_v)
{
    ChartTexel r;
    r.u = (page_u * static_cast<float>(lm_w) - static_cast<float>(surf_x) - 1.0f) * static_cast<float>(k_u);
    r.v = (page_v * static_cast<float>(lm_h) - static_cast<float>(surf_y) - 1.0f) * static_cast<float>(k_v);
    return r;
}

// Which tile of a chart a chart texel coordinate belongs to.
struct TileSelect
{
    std::uint32_t tx;
    std::uint32_t ty;
};

// Clamping happens in the float domain so NaN, infinities and out-of-range coordinates are all
// defined (NaN fails both comparisons and lands in tile 0) rather than relying on lround.
inline TileSelect select_tile(const ChartGeometry& g, float u, float v, std::uint32_t step = tile_step)
{
    TileSelect r{};
    if (g.nx == 0 || g.ny == 0) {
        return r;
    }
    const float fs = static_cast<float>(step);
    const auto axis = [fs](float c, std::uint32_t n) -> std::uint32_t {
        if (!(c >= fs)) {
            return 0;
        }
        const float t = std::floor(c / fs);
        if (!(t < static_cast<float>(n - 1))) {
            return n - 1;
        }
        return static_cast<std::uint32_t>(t);
    };
    r.tx = axis(u, g.nx);
    r.ty = axis(v, g.ny);
    return r;
}

// Absolute texel position inside the page a chart texel coordinate samples from.
struct PageSample
{
    std::uint16_t page;
    float x;
    float y;
};

// num_tiles bounds the array: a section whose stored num_tiles disagrees with the derived sum
// is invalid and must be rejected, but this must never index past the array while finding out.
inline PageSample sample_page_coords(const ChartGeometry& g, const Tile* tiles, std::uint32_t base,
                                     std::uint32_t num_tiles, float u, float v,
                                     std::uint32_t step = tile_step)
{
    const TileSelect sel = select_tile(g, u, v, step);
    const std::uint32_t idx = tile_index(base, g, sel.tx, sel.ty);
    if (!tiles || idx >= num_tiles) {
        return PageSample{};
    }
    const Tile& t = tiles[idx];
    PageSample r;
    r.page = t.page;
    r.x = static_cast<float>(t.x + g.pad_u) + (u - static_cast<float>(sel.tx * step));
    r.y = static_cast<float>(t.y + g.pad_v) + (v - static_cast<float>(sel.ty * step));
    return r;
}

// Where an integer chart texel (padding included) is stored; the owner is chosen on the coordinate clamped
// into the chart. valid is false when that tile does not store the coordinate.
struct TileSlot
{
    std::uint32_t index;
    std::int64_t sx;
    std::int64_t sy;
    bool valid;
};

inline TileSlot chart_tile_slot(const ChartGeometry& g, std::uint32_t base, std::int64_t cu,
                                std::int64_t cv, std::uint32_t step = tile_step)
{
    TileSlot r{};
    if (g.nx == 0 || g.ny == 0) {
        return r;
    }
    const std::int64_t ccu = std::clamp<std::int64_t>(cu, 0, static_cast<std::int64_t>(g.cw) - 1);
    const std::int64_t ccv = std::clamp<std::int64_t>(cv, 0, static_cast<std::int64_t>(g.ch) - 1);
    const std::uint32_t tx =
        std::min<std::uint32_t>(static_cast<std::uint32_t>(ccu / step), g.nx - 1);
    const std::uint32_t ty =
        std::min<std::uint32_t>(static_cast<std::uint32_t>(ccv / step), g.ny - 1);
    const TileDims td = tile_dims(g, tx, ty, step);
    const ChartCoord origin = tile_origin_chart(g, tx, ty, step);
    r.index = tile_index(base, g, tx, ty);
    r.sx = cu - origin.u;
    r.sy = cv - origin.v;
    r.valid = r.sx >= 0 && r.sy >= 0 && r.sx < td.w_t && r.sy < td.h_t;
    return r;
}

// ─── Surface charts: the stock projection ─────────────────────────────────────
// A surface's chart is its stock fragment interior refined k times per axis: stock interior texel i
// (page texel start + 1 + i) covers chart texels [i * k, (i + 1) * k), so in a page scaled by k the
// chart's texel c sits at k * (start + 1) + c. chart_texel_from_page_uv is the inverse.

inline constexpr std::uint32_t stock_texel_chart_coord(std::uint32_t interior_texel, std::uint32_t k,
                                                        std::uint32_t sub)
{
    return interior_texel * k + sub;
}

inline constexpr std::int64_t stock_view_coord(std::uint32_t k, std::int32_t fragment_start,
                                               std::int64_t chart_coord)
{
    return static_cast<std::int64_t>(k) * (static_cast<std::int64_t>(fragment_start) + 1) + chart_coord;
}

// World position -> chart texel of a surface: the surface's lightmap projection keeps the world axes
// u_coefficient and v_coefficient (GSurface +0x60/+0x64), then the stock affine and the refinement.
inline ChartTexel surface_chart_texel(const float* pos, int axis_u, int axis_v, float scale_u,
                                      float scale_v, float add_u, float add_v, std::uint32_t lm_w,
                                      std::uint32_t lm_h, std::uint32_t surf_x, std::uint32_t surf_y,
                                      std::uint32_t k_u, std::uint32_t k_v)
{
    return chart_texel_from_page_uv(pos[axis_u] * scale_u + add_u, pos[axis_v] * scale_v + add_v, lm_w,
                                    lm_h, surf_x, surf_y, k_u, k_v);
}

// ─── Terrain charts: the XZ mapping ───────────────────────────────────────────
// A terrain chart is cells * density texels per axis over the terrain's footprint. Texel i spans
// world [origin + i * texel_size, origin + (i + 1) * texel_size), so chart coordinate c (texel units,
// af_lm_sample's chart_uv, texel centres at i + 0.5) is (world - origin) / texel_size. The terrain
// pixel shader (ter_base_light) mirrors terrain_chart_coord.

inline constexpr std::uint32_t terrain_chart_extent(std::uint32_t cells, std::uint32_t density)
{
    return cells * density;
}

inline float terrain_chart_texel_size(float cell_size, std::uint32_t density)
{
    return cell_size / static_cast<float>(density);
}

// Texels per cell of chart `c` over a grid cells_x cells across, 0 for no cells.
inline std::uint32_t terrain_chart_density(const TerrainChart& c, std::uint32_t cells_x)
{
    return cells_x ? c.w / cells_x : 0;
}

// Whether chart `c` covers a cells_x x cells_z grid at a whole density of 1..max_density texels per
// cell (the bake may have lowered the terrain's own density to fit the page budget).
inline bool terrain_chart_fits_grid(const TerrainChart& c, std::uint32_t cells_x, std::uint32_t cells_z,
                                    std::uint32_t max_density)
{
    if (cells_x == 0 || cells_z == 0) {
        return false;
    }
    const std::uint32_t density = terrain_chart_density(c, cells_x);
    return density >= 1 && density <= max_density && c.w == terrain_chart_extent(cells_x, density)
        && c.h == terrain_chart_extent(cells_z, density);
}

inline float terrain_texel_center(float origin, float texel_size, std::uint32_t i)
{
    return origin + (static_cast<float>(i) + 0.5f) * texel_size;
}

inline float terrain_chart_coord(float origin, float texel_size, float world)
{
    return (world - origin) / texel_size;
}

// Bilinear read of a decoded w x h RGB8 chart at chart coordinate (cu, cv), clamped to the edge
// texels as the padding a stored tile carries past the chart replicates them. 0..1 per channel.
inline void chart_sample_bilinear(const std::uint8_t* rgb, std::uint32_t w, std::uint32_t h, float cu,
                                  float cv, float (&out)[3])
{
    out[0] = out[1] = out[2] = 0.0f;
    if (!rgb || w == 0 || h == 0) {
        return;
    }
    const auto axis = [](float c, std::uint32_t n, std::uint32_t& i0, std::uint32_t& i1, float& t) {
        float f = c - 0.5f;
        const float hi = static_cast<float>(n - 1);
        if (!(f > 0.0f)) {
            f = 0.0f;
        }
        if (f > hi) {
            f = hi;
        }
        const float fl = std::floor(f);
        i0 = static_cast<std::uint32_t>(fl);
        i1 = std::min(i0 + 1, n - 1);
        t = f - fl;
    };
    std::uint32_t x0, x1, y0, y1;
    float tx, ty;
    axis(cu, w, x0, x1, tx);
    axis(cv, h, y0, y1, ty);
    const auto px = [&](std::uint32_t x, std::uint32_t y, int c) {
        return static_cast<float>(rgb[(static_cast<std::size_t>(y) * w + x) * 3 + c]);
    };
    for (int c = 0; c < 3; c++) {
        const float top = px(x0, y0, c) + (px(x1, y0, c) - px(x0, y0, c)) * tx;
        const float bot = px(x0, y1, c) + (px(x1, y1, c) - px(x0, y1, c)) * tx;
        out[c] = (top + (bot - top) * ty) / 255.0f;
    }
}

// ─── Terrain charts: reduced CPU copies ───────────────────────────────────────
// A CPU light sampler keeps a chart box filtered down by a reduction r, the smallest divisor of its
// density leaving at most terrain_reduced_texels_per_cell texels per cell, so r divides w and h.
// Reduced texel j averages chart texels [j * r, (j + 1) * r), which makes a reduced coordinate the
// chart coordinate over r.

inline constexpr std::uint32_t terrain_reduced_texels_per_cell = 2;

inline constexpr std::uint32_t terrain_chart_reduction(std::uint32_t density)
{
    std::uint32_t r = 1;
    while (r < density && (density % r != 0 || density / r > terrain_reduced_texels_per_cell)) {
        r++;
    }
    return r;
}

inline float terrain_reduced_chart_coord(float origin, float texel_size, std::uint32_t reduction, float world)
{
    return terrain_chart_coord(origin, texel_size, world) / static_cast<float>(reduction);
}

// Final step of the sampling recipe: the normalized page coordinate a shader or CPU sampler
// actually reads with.
struct PageUv
{
    float u;
    float v;
};

inline PageUv page_uv(const PageSample& s, std::uint32_t p = page_size)
{
    return PageUv{s.x / static_cast<float>(p), s.y / static_cast<float>(p)};
}

// Refinement of one stock axis (surface_dim texels incl. border over `extent` world units) reaching
// `density`. Never 0, which marks "no chart" on the wire.
inline std::uint16_t k_from_density(std::uint32_t surface_dim, float extent, float density)
{
    if (surface_dim <= 2 || !(extent > 0.0f) || !(density > 0.0f)) {
        return 1;
    }
    const double d_stock = static_cast<double>(surface_dim - 2) / static_cast<double>(extent);
    if (!(d_stock > 0.0)) {
        return 1;
    }
    const long k = std::lround(static_cast<double>(density) / d_stock);
    return static_cast<std::uint16_t>(std::clamp<long>(k, 1, 65535));
}

// ─── Layer payload sizing ─────────────────────────────────────────────────────

inline constexpr std::uint64_t bc7_block_count(std::uint32_t num_pages, std::uint32_t p = page_size)
{
    return static_cast<std::uint64_t>(num_pages) * (p / 4) * (p / 4);
}

// Exact decoded size of one layer, i.e. what LayerDirEntry::uncompressed_size must hold.
// That field is u32 while this returns u64: raw_rgb8 overflows it at 21846 pages, so a writer
// must reject a page count whose payload does not fit before narrowing.
inline constexpr std::uint64_t layer_payload_size(Codec codec, std::uint32_t num_pages,
                                                  std::uint32_t p = page_size)
{
    switch (codec) {
    case Codec::raw_rgb8:
        return static_cast<std::uint64_t>(num_pages) * p * p * 3u;
    case Codec::bc7_unorm:
    case Codec::bc6h_uf16:
        return bc7_block_count(num_pages, p) * 16u;
    }
    return 0;
}

// Decoded bytes one layer may hold whatever its codec: a full BC7 atlas. The raw debug codec is
// widened again on upload, so it gets fewer pages rather than more memory.
inline constexpr std::uint64_t max_layer_bytes = layer_payload_size(Codec::bc7_unorm, max_pages);

inline constexpr std::uint32_t max_layer_pages(Codec codec)
{
    const std::uint64_t per_page = layer_payload_size(codec, 1);
    return per_page == 0 ? 0u
                         : static_cast<std::uint32_t>(std::min<std::uint64_t>(max_pages, max_layer_bytes / per_page));
}

// ─── Compression modes (level property `lightmap_compression`) ────────────────

enum class CompressionMode : std::uint8_t
{
    quality = 0,  // plain BC7, RDO off
    balanced = 1,
    compact = 2,
};

inline constexpr CompressionMode compression_mode_from_wire(std::uint8_t v)
{
    return v <= static_cast<std::uint8_t>(CompressionMode::compact)
        ? static_cast<CompressionMode>(v)
        : CompressionMode::quality;
}

// The only encoder configuration. Both passes are RNG-free, so within one toolchain (GNU builds use
// x87 float math) the bytes depend only on these, the pixels and block order: bc7enc threads over
// fixed ranges only, and ert runs single threaded in index order.
struct EncoderSettings
{
    // ert::reduce_entropy_params. rdo_lambda == 0 disables the RDO pass entirely.
    float rdo_lambda;
    std::uint32_t rdo_lookback_window_size;
    float rdo_max_allowed_rms_increase_ratio;
    float rdo_max_smooth_block_std_dev;
    float rdo_smooth_block_max_mse_scale;
    bool rdo_try_two_matches;
    bool rdo_allow_relative_movement;
    bool rdo_skip_zero_mse_blocks;

    // bc7enc_compress_block_params.
    std::uint32_t bc7_uber_level;
    std::uint32_t bc7_max_partitions;
    bool bc7_perceptual;
    bool bc7_try_least_squares;
    bool bc7_mode17_partition_estimation_filterbank;
    bool bc7_quant_mode6_endpoints;
    float bc7_pbit1_weight;
    float bc7_mode6_error_weight;
    float bc7_low_frequency_partition_weight;
};

// The no-alpha branch of bc7enc_rdo's rdo_bc_encoder.cpp; perceptual off since texels are radiance.
inline constexpr EncoderSettings encoder_settings(CompressionMode mode)
{
    switch (mode) {
    case CompressionMode::balanced:
        return EncoderSettings{
            0.5f, 128u, 10.0f, 18.0f, 15.0f, true, false, false,
            4u, 64u, false, true, false, true, 1.3f, 0.4f, 0.9999f,
        };
    case CompressionMode::compact:
        return EncoderSettings{
            2.0f, 256u, 10.0f, 18.0f, 25.0f, true, false, false,
            4u, 64u, false, true, false, true, 1.3f, 0.4f, 0.9999f,
        };
    case CompressionMode::quality:
        break;
    }
    return EncoderSettings{
        0.0f, 128u, 10.0f, 18.0f, 15.0f, false, false, false,
        4u, 64u, false, true, true, false, 1.0f, 1.0f, 1.0f,
    };
}

} // namespace alpine_lightmap
