#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <patch_common/FunHook.h>
#include <patch_common/MemUtils.h>
#include <common/bitmap/formats.h>
#include <common/scope_guard.h>
#include <common/terrain/alpine_terrain.h>
#include <common/utils/string-utils.h>
#include <xlog/xlog.h>
#include "alpine_lightmaps.h"
#include "alpine_obj.h"
#include "level.h"
#include "mfc_types.h"
#include "terrain.h"
#include "terrain_build.h"
#include "terrain_decorations.h"
#include "terrain_paint.h"
#include "terrain_preview.h"
#include "textures.h"
#include "vtypes.h"

namespace at = alpine_terrain;

namespace
{

// ─── RED entry points ───────────────────────────────────────────────────────

// Render Everything's per-face drawer
void level_face_draw_hooked(GSolid* solid, GFace* face, char outline);
FunHook<decltype(level_face_draw_hooked)> level_face_draw_hook{0x004E94B0, level_face_draw_hooked};

// RED makes 8888 bitmaps A4R4G4B4 textures; 565 gets its best opaque 16-bit format.
constexpr int composite_format = BM_FORMAT_565_RGB;

// Composite budget: texels per terrain and across the level, bitmaps across the level
constexpr uint64_t max_terrain_texels = 1u << 20;
constexpr uint64_t max_level_texels = 8u << 20;
constexpr int max_level_bitmaps = 1024;
constexpr double composite_budget_ms = 20.0;

const uint8_t chunk_line_rgb[3] = {0x90, 0x00, 0x00};

// A lightmapped texel as RED draws a brush (texture x lightmap, MODULATE2X) and the game's terrain shader
// draws the chart (albedo x 2 x chart texel, saturated), dynamic lights aside: albedo 0..255, texel a
// stock lightmap texel 0..1, result 0..255.
float terrain_preview_lit(float albedo, float texel)
{
    return std::min(albedo * 2.0f * texel, 255.0f);
}

uint8_t terrain_preview_byte(float v)
{
    return static_cast<uint8_t>(std::clamp(v, 0.0f, 255.0f) + 0.5f);
}

// The level-wide composite budget has room for one more res x res composite.
bool composite_budget_fits(int bitmaps, uint64_t texels, uint32_t res, int max_bitmaps, uint64_t max_texels)
{
    return bitmaps < max_bitmaps && texels + static_cast<uint64_t>(res) * res <= max_texels;
}

// A composite last drawn in paint `last_drawn` may be freed for another in paint `frame`.
bool composite_evictable(uint32_t last_drawn, uint32_t frame, uint32_t keep_frames)
{
    return frame - last_drawn >= keep_frames;
}

// ─── Layer tiles ────────────────────────────────────────────────────────────

// A layer texture box-filtered down to at most tile_max texels square: RGB, and for overlays RGB
// premultiplied by alpha plus alpha.
struct LayerTile
{
    int size = 0;
    std::vector<uint8_t> rgb;
    std::vector<uint8_t> rgba_premul;
};
constexpr int tile_max = 128;

std::unordered_map<std::string, std::shared_ptr<const LayerTile>> g_tiles;

// c[3] is the alpha, 255 for formats without one.
bool read_texel(int fmt, const uint8_t* px, const uint8_t* pal, int w, int x, int y, uint32_t (&c)[4])
{
    const std::size_t i = static_cast<std::size_t>(y) * w + x;
    auto u16 = [&] { return static_cast<uint32_t>(px[i * 2] | (px[i * 2 + 1] << 8)); };
    c[3] = 255;
    switch (fmt) {
    case BM_FORMAT_8_PALETTED:
        if (!pal) return false;
        for (int k = 0; k < 3; k++) c[k] = pal[px[i] * 3 + k];
        return true;
    case BM_FORMAT_8_ALPHA:
        c[0] = c[1] = c[2] = px[i];
        return true;
    case BM_FORMAT_565_RGB: {
        const uint32_t v = u16();
        c[0] = ((v >> 11) & 31) * 255 / 31;
        c[1] = ((v >> 5) & 63) * 255 / 63;
        c[2] = (v & 31) * 255 / 31;
        return true;
    }
    case BM_FORMAT_4444_ARGB: {
        const uint32_t v = u16();
        c[0] = ((v >> 8) & 15) * 17;
        c[1] = ((v >> 4) & 15) * 17;
        c[2] = (v & 15) * 17;
        c[3] = ((v >> 12) & 15) * 17;
        return true;
    }
    case BM_FORMAT_1555_ARGB: {
        const uint32_t v = u16();
        c[0] = ((v >> 10) & 31) * 255 / 31;
        c[1] = ((v >> 5) & 31) * 255 / 31;
        c[2] = (v & 31) * 255 / 31;
        c[3] = (v & 0x8000u) ? 255 : 0;
        return true;
    }
    case BM_FORMAT_888_RGB:
        c[0] = px[i * 3 + 2];
        c[1] = px[i * 3 + 1];
        c[2] = px[i * 3];
        return true;
    case BM_FORMAT_8888_ARGB:
        c[0] = px[i * 4 + 2];
        c[1] = px[i * 4 + 1];
        c[2] = px[i * 4];
        c[3] = px[i * 4 + 3];
        return true;
    default:
        return false;
    }
}

std::shared_ptr<const LayerTile> decode_tile(const std::string& name)
{
    const int handle = alpine_dlg_resolve_bitmap(name.c_str());
    if (handle < 0) return nullptr;
    int w = 0, h = 0, num_pixels = 0, levels = 0;
    bm_get_mipmap_info(handle, &w, &h, &num_pixels, &levels);
    if (w <= 0 || h <= 0) return nullptr;
    void* pixels = nullptr;
    void* palette = nullptr;
    const int fmt = bm_lock(handle, &pixels, &palette);
    ScopeGuard unlock{[handle] { bm_unlock(handle); }};
    std::shared_ptr<LayerTile> tile;
    if (pixels) {
        tile = std::make_shared<LayerTile>();
        int size = tile_max;
        while (size > 1 && (size > w || size > h)) size /= 2;
        tile->size = size;
        tile->rgb.resize(static_cast<std::size_t>(size) * size * 3);
        tile->rgba_premul.resize(static_cast<std::size_t>(size) * size * 4);
        const auto* px = static_cast<const uint8_t*>(pixels);
        const auto* pal = static_cast<const uint8_t*>(palette);
        bool ok = true;
        for (int ty = 0; ty < size && ok; ty++) {
            const int y0 = ty * h / size, y1 = std::max(y0 + 1, (ty + 1) * h / size);
            for (int tx = 0; tx < size && ok; tx++) {
                const int x0 = tx * w / size, x1 = std::max(x0 + 1, (tx + 1) * w / size);
                uint64_t sum[3] = {}, premul[4] = {};
                uint32_t c[4];
                for (int y = y0; y < y1 && ok; y++) {
                    for (int x = x0; x < x1; x++) {
                        if (!(ok = read_texel(fmt, px, pal, w, x, y, c))) break;
                        for (int k = 0; k < 3; k++) {
                            sum[k] += c[k];
                            premul[k] += c[k] * c[3];
                        }
                        premul[3] += c[3];
                    }
                }
                const uint64_t n = static_cast<uint64_t>((y1 - y0) * (x1 - x0));
                const std::size_t t = static_cast<std::size_t>(ty) * size + tx;
                for (int k = 0; k < 3; k++) {
                    tile->rgb[t * 3 + k] = static_cast<uint8_t>(sum[k] / n);
                    tile->rgba_premul[t * 4 + k] = static_cast<uint8_t>(premul[k] / (n * 255));
                }
                tile->rgba_premul[t * 4 + 3] = static_cast<uint8_t>(premul[3] / n);
            }
        }
        if (!ok) tile.reset();
    }
    return tile;
}

const LayerTile* get_tile(const std::string& name)
{
    std::string key = string_to_lower(name);
    auto it = g_tiles.find(key);
    if (it == g_tiles.end()) it = g_tiles.emplace(key, decode_tile(name)).first;
    return it->second.get();
}

// Bilinear, wrapping, in 0..255 per channel of a tile's N channel texels (rgb, or rgba_premul).
template<int N>
void sample_tile(const std::vector<uint8_t>& px, int size, float u, float v, float (&out)[N])
{
    const float fu = (u - std::floor(u)) * size - 0.5f;
    const float fv = (v - std::floor(v)) * size - 0.5f;
    const float bu = std::floor(fu), bv = std::floor(fv);
    const float wu = fu - bu, wv = fv - bv;
    auto wrap = [&](int i) { return ((i % size) + size) % size; };
    const int u0 = wrap(static_cast<int>(bu)), u1 = wrap(static_cast<int>(bu) + 1);
    const int v0 = wrap(static_cast<int>(bv)), v1 = wrap(static_cast<int>(bv) + 1);
    const uint8_t* r0 = &px[static_cast<std::size_t>(v0) * size * N];
    const uint8_t* r1 = &px[static_cast<std::size_t>(v1) * size * N];
    for (int k = 0; k < N; k++) {
        const float top = r0[u0 * N + k] + (r0[u1 * N + k] - r0[u0 * N + k]) * wu;
        const float bottom = r1[u0 * N + k] + (r1[u1 * N + k] - r1[u0 * N + k]) * wu;
        out[k] = top + (bottom - top) * wv;
    }
}

// ─── Preview cache ──────────────────────────────────────────────────────────

struct PreviewChunk
{
    int bm = -1;
    bool dirty = true;
    bool has_hole = false;
    bool bounds_stale = true;
    // The composite carries the baked light (drawn with white vertices)
    bool lit = false;
    uint32_t last_drawn = 0;
    float y_lo = 0.0f, y_hi = 0.0f;
};

// Shading inputs, compared as a whole.
struct ShadeKey
{
    int style = 0;
    bool sun = false;
    float dir[3] = {};
    float color[3] = {};
    uint8_t ambient[3] = {};
    bool fullbright = false;
    const TerrainBakedLight* light = nullptr;
    uint32_t light_gen = 0;

    bool operator==(const ShadeKey& o) const
    {
        return style == o.style && sun == o.sun && std::memcmp(dir, o.dir, sizeof(dir)) == 0 &&
               std::memcmp(color, o.color, sizeof(color)) == 0 &&
               std::memcmp(ambient, o.ambient, sizeof(ambient)) == 0 && fullbright == o.fullbright &&
               light == o.light && light_gen == o.light_gen;
    }
};

// Whether the preview shows the terrain's baked lighting.
enum class BakedLight : uint8_t
{
    unknown, // to be checked against the terrain
    recheck, // geometry edited in place: not shown, checked again once no stroke is running
    none,
    shown,
};

// Vertex colour and texturing per RED view style.
enum ShadeStyle
{
    style_lit = 0,          // Textures w/ Lightmaps
    style_textures = 1,     // Show Just Textures: fullbright
    style_lightmaps = 2,    // Show Just Lightmaps: untextured, lit
    style_room_colors = 3,  // Rooms in Different Colors: untextured, tinted
};

struct Preview
{
    const DedTerrain* owner = nullptr;
    std::shared_ptr<const TerrainGrid> grid;
    Vector3 pos;
    float cell_size = 0.0f;
    float height_min = 0.0f;
    float height_range = 0.0f;
    uint32_t edge = 0;
    uint32_t res = 0;
    std::vector<std::pair<std::string, float>> layers;
    std::vector<std::pair<std::string, float>> overlays;
    std::string underside_name;
    int underside_bm = -1;
    bool bounds_dirty = true;
    bool shade_dirty = true;
    // Vertices whose shading a height edit left stale, when shade_dirty is not set
    bool shade_partial = false;
    TerrainCellRect shade_rect{};
    bool textures_failed = false;
    ShadeKey shade_key;
    std::vector<uint8_t> shade;
    std::vector<PreviewChunk> chunks;
    // The rest of what lighting_fingerprint covers
    uint8_t flags = 0;
    float thickness = 0.0f;
    float skirt_depth = 0.0f;
    // The decorations that cast shadows, by index, as decoration_lighting_hash reads them
    std::vector<std::pair<std::size_t, DedTerrainDecoration>> casting;
    BakedLight baked = BakedLight::unknown;
    const TerrainBakedLight* light = nullptr;
    uint32_t light_gen = 0;
    // The light the composites are being made with
    const TerrainBakedLight* composite_light = nullptr;
    uint32_t composite_light_gen = 0;
};

std::vector<std::unique_ptr<Preview>> g_previews;
int g_level_bitmaps = 0;
uint64_t g_level_texels = 0;
bool g_work_pending = false;
bool g_work_outstanding = false;
double g_budget_left_ms = composite_budget_ms;
// Viewport paints so far; a chunk drawn within the last keep_frames paints keeps its composite.
uint32_t g_frame = 1;
constexpr uint32_t keep_frames = 8;
// The paint in which no composite could be evicted for the level budget
uint32_t g_budget_full_frame = 0;
bool g_budget_logged = false;

// Released composites by resolution (32 to 256), reused across levels: bm_release would leak their textures.
std::vector<int> g_spare_bitmaps[4];

std::vector<int>& spare_bitmaps(uint32_t res)
{
    return g_spare_bitmaps[res >= 256 ? 3 : res >= 128 ? 2 : res >= 64 ? 1 : 0];
}

bool preview_bitmap_valid(int bm, uint32_t res)
{
    if (bm < 0) return false;
    const int index = BitmapEntry::handle_to_index(bm);
    if (index < 0) return false;
    const BitmapEntry& e = BitmapEntry::entries[index];
    return e.handle == bm && e.bm_type == BitmapEntry::TYPE_USER && e.width == res && e.height == res;
}

// gr_lock's texture setup, done first so a failed CreateTexture never reaches gr_lock. A failure
// frees the slot, so a pooled bitmap is tried again at the next recomposite.
bool preview_texture_ready(int bm)
{
    GrTextureSlot* slot = gr_texture_slot_of(bm);
    if (!slot) return false;
    const bool created = (slot->section_count > 0 && slot->bm_handle == bm) || gr_texture_create(bm, slot);
    if (created && slot->section_count == 1 && slot->sections && slot->sections[0].texture) return true;
    gr_texture_free(slot);
    return false;
}

void release_chunk_bitmap(PreviewChunk& c, uint32_t res)
{
    if (c.bm < 0) return;
    if (preview_bitmap_valid(c.bm, res)) {
        try {
            spare_bitmaps(res).push_back(c.bm);
        }
        catch (const std::bad_alloc&) {
            bm_release(c.bm);
        }
    }
    c.bm = -1;
    c.lit = false;
    g_level_bitmaps--;
    g_level_texels -= static_cast<uint64_t>(res) * res;
}

void release_preview(Preview& p)
{
    for (PreviewChunk& c : p.chunks) release_chunk_bitmap(c, p.res);
}

uint32_t choose_res(uint32_t edge, uint32_t chunk_count)
{
    uint32_t res = 256;
    while (res > 32 && (res > edge * 8 || static_cast<uint64_t>(chunk_count) * res * res > max_terrain_texels)) {
        res /= 2;
    }
    return res;
}

// Whether `cast` lists `d`'s shadow casting decorations as they are (their draw distance aside).
bool same_casting(const std::vector<std::pair<std::size_t, DedTerrainDecoration>>& cast, const DedTerrainData& d)
{
    std::size_t n = 0;
    for (std::size_t i = 0; i < d.decorations.size(); i++) {
        if (!terrain_decoration_casts(d.decorations[i])) continue;
        if (n >= cast.size() || cast[n].first != i) return false;
        DedTerrainDecoration deco = d.decorations[i];
        deco.draw_distance = 0.0f;
        if (cast[n++].second != deco) return false;
    }
    return n == cast.size();
}

std::vector<std::pair<std::size_t, DedTerrainDecoration>> casting_decorations(const DedTerrainData& d)
{
    std::vector<std::pair<std::size_t, DedTerrainDecoration>> out;
    for (std::size_t i = 0; i < d.decorations.size(); i++) {
        if (!terrain_decoration_casts(d.decorations[i])) continue;
        out.emplace_back(i, d.decorations[i]);
        out.back().second.draw_distance = 0.0f;
    }
    return out;
}

Preview& find_preview(const DedTerrain* owner)
{
    for (auto& p : g_previews) {
        if (p->owner == owner) return *p;
    }
    g_previews.push_back(std::make_unique<Preview>());
    g_previews.back()->owner = owner;
    return *g_previews.back();
}

// Brings the cache in line with what is about to be drawn: anything the composites depend on marks
// every chunk, anything the shading depends on the vertex colours.
Preview& sync_preview(const DedTerrain& terrain, const DedTerrainData& d)
{
    Preview& p = find_preview(&terrain);
    const uint32_t edge = terrain_effective_chunk_cells(d);
    const uint32_t count = terrain_chunk_count(d);
    const uint32_t res = choose_res(edge, count);

    bool recomposite = false;
    if (p.grid != d.grid || p.edge != edge || p.res != res || p.chunks.size() != count) {
        if (p.edge != edge || p.res != res || p.chunks.size() != count) {
            release_preview(p);
            p.chunks.assign(count, PreviewChunk{});
            p.edge = edge;
            p.res = res;
        }
        p.grid = d.grid;
        recomposite = true;
        p.bounds_dirty = p.shade_dirty = true;
        p.baked = BakedLight::unknown;
    }
    if (p.pos.x != terrain.pos.x || p.pos.y != terrain.pos.y || p.pos.z != terrain.pos.z ||
        p.cell_size != d.cell_size) {
        recomposite = true;
        p.bounds_dirty = p.shade_dirty = true;
        p.baked = BakedLight::unknown;
        p.pos = terrain.pos;
        p.cell_size = d.cell_size;
    }
    if (p.height_min != d.height_min || p.height_range != d.height_range) {
        p.bounds_dirty = p.shade_dirty = true;
        p.baked = BakedLight::unknown;
        p.height_min = d.height_min;
        p.height_range = d.height_range;
    }
    if (p.flags != d.flags || p.thickness != d.thickness || p.skirt_depth != d.skirt_depth) {
        p.baked = BakedLight::unknown;
        p.flags = d.flags;
        p.thickness = d.thickness;
        p.skirt_depth = d.skirt_depth;
    }
    if (!same_casting(p.casting, d)) {
        p.baked = BakedLight::unknown;
        p.casting = casting_decorations(d);
    }
    bool same_layers = p.layers.size() == d.layers.size();
    for (std::size_t i = 0; same_layers && i < d.layers.size(); i++) {
        same_layers = p.layers[i].first == d.layers[i].texture && p.layers[i].second == d.layers[i].uv_scale;
    }
    if (!same_layers) {
        p.layers.clear();
        for (const auto& layer : d.layers) p.layers.emplace_back(layer.texture, layer.uv_scale);
        recomposite = true;
    }
    bool same_overlays = p.overlays.size() == d.overlays.size();
    for (std::size_t i = 0; same_overlays && i < d.overlays.size(); i++) {
        same_overlays = p.overlays[i].first == d.overlays[i].texture && p.overlays[i].second == d.overlays[i].uv_scale;
    }
    if (!same_overlays) {
        p.overlays.clear();
        for (const auto& overlay : d.overlays) p.overlays.emplace_back(overlay.texture, overlay.uv_scale);
        recomposite = true;
    }
    if (recomposite) {
        p.textures_failed = false;
        for (PreviewChunk& c : p.chunks) c.dirty = true;
    }
    return p;
}

void update_bounds(Preview& p, const at::GridView& v)
{
    if (p.bounds_dirty) {
        for (PreviewChunk& c : p.chunks) c.bounds_stale = true;
        p.bounds_dirty = false;
    }
    const uint32_t cx = at::cells(v.nx), cz = at::cells(v.nz);
    for (uint32_t k = 0; k < p.chunks.size(); k++) {
        if (!p.chunks[k].bounds_stale) continue;
        p.chunks[k].bounds_stale = false;
        const at::ChunkRect r = at::chunk_rect(cx, cz, p.edge, k);
        uint16_t lo = UINT16_MAX, hi = 0;
        for (uint32_t z = r.z0; z <= r.z1; z++) {
            for (uint32_t x = r.x0; x <= r.x1; x++) {
                const uint16_t h = v.heights[static_cast<std::size_t>(z) * v.nx + x];
                lo = std::min(lo, h);
                hi = std::max(hi, h);
            }
        }
        p.chunks[k].y_lo = at::world_y(v.origin[1], lo, v.height_min, v.height_range);
        p.chunks[k].y_hi = at::world_y(v.origin[1], hi, v.height_min, v.height_range);
        bool hole = false;
        for (uint32_t z = r.z0; z < r.z1 && !hole; z++) {
            for (uint32_t x = r.x0; x < r.x1 && !hole; x++) hole = !at::cell_solid(v, x, z);
        }
        p.chunks[k].has_hole = hole;
    }
}

ShadeKey current_shade_key(CDedLevel& level, int style)
{
    ShadeKey key;
    key.style = style;
    const auto& props = level.GetAlpineLevelProperties();
    key.sun = props.enable_sun;
    const Vector3 dir = key.sun ? props.sun_to_light_dir() : Vector3{0.35f, 1.0f, 0.25f};
    const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    key.dir[0] = dir.x / len;
    key.dir[1] = dir.y / len;
    key.dir[2] = dir.z / len;
    if (key.sun) {
        key.color[0] = props.sun_color_r / 255.0f * props.sun_intensity;
        key.color[1] = props.sun_color_g / 255.0f * props.sun_intensity;
        key.color[2] = props.sun_color_b / 255.0f * props.sun_intensity;
    }
    const Color& ambient = level.ambient_color;
    key.ambient[0] = ambient.r;
    key.ambient[1] = ambient.g;
    key.ambient[2] = ambient.b;
    return key;
}

// Looks the terrain's baked lighting up again only when something it depends on changed: a bake or level
// load (the generation), an edit sync_preview saw, or an in-place geometry edit once its stroke is over.
void update_baked_light(Preview& p, const DedTerrain& terrain, const DedTerrainData& d)
{
    const uint32_t gen = terrain_baked_light_generation();
    if (p.light_gen != gen) {
        p.light_gen = gen;
        p.baked = BakedLight::unknown;
    }
    if (p.baked != BakedLight::unknown && p.baked != BakedLight::recheck) return;
    // never a heightmap hash per dab
    if (terrain_paint_stroke_active()) {
        p.baked = BakedLight::recheck;
        return;
    }
    p.light = terrain_baked_light_find(terrain.uid, terrain.pos, d);
    p.baked = p.light ? BakedLight::shown : BakedLight::none;
}

// The baked light the preview draws: none while a geometry edit is unchecked.
const TerrainBakedLight* shown_light(const Preview& p)
{
    return p.baked == BakedLight::shown ? p.light : nullptr;
}

// Per-vertex colour: level ambient plus the sun (or, with no sun, a fixed editor light) by the
// heightmap normal, floored so unlit slopes keep their shape. Fullbright shows the neutral lightmap in the
// lightmaps style, as the game does, and white otherwise.
constexpr uint8_t neutral_lightmap_byte = 128;

void light_color(const ShadeKey& key, const float (&n)[3], uint8_t* out)
{
    if (key.style == style_textures || key.fullbright) {
        out[0] = out[1] = out[2] = key.style == style_lightmaps ? neutral_lightmap_byte : 255;
        return;
    }
    static const float room_tint[3] = {0.55f, 0.8f, 0.55f};
    const float ndl = std::max(0.0f, n[0] * key.dir[0] + n[1] * key.dir[1] + n[2] * key.dir[2]);
    for (int k = 0; k < 3; k++) {
        float c = key.ambient[k] / 255.0f;
        c += key.sun ? key.color[k] * ndl : 0.3f + 0.7f * ndl;
        c = std::max(c, 0.12f);
        if (key.style == style_room_colors) c *= room_tint[k];
        out[k] = static_cast<uint8_t>(std::clamp(c, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
}

// With baked light: the chart texel at the vertex, doubled as a lightmap is on a white (untextured) face,
// raw when lightmaps are shown alone.
void vertex_color(const ShadeKey& key, const at::GridView& v, uint32_t x, uint32_t z, uint8_t* out)
{
    if (key.light && (key.style == style_lit || key.style == style_lightmaps)) {
        float pos[3], texel[3];
        at::grid_position(v, x, z, pos);
        terrain_baked_light_sample(*key.light, pos[0], pos[2], texel);
        for (int k = 0; k < 3; k++) {
            out[k] = terrain_preview_byte(key.style == style_lit ? terrain_preview_lit(255.0f, texel[k])
                                                                 : texel[k] * 255.0f);
        }
        return;
    }
    float n[3];
    at::vertex_normal(v, x, z, n);
    light_color(key, n, out);
}

void update_shade(Preview& p, const at::GridView& v, const ShadeKey& key)
{
    const std::size_t size = at::vertex_count(v.nx, v.nz) * 3;
    TerrainCellRect r{0, 0, v.nx, v.nz};
    if (!p.shade_dirty && p.shade_key == key && p.shade.size() == size) {
        if (!p.shade_partial) return;
        r = {p.shade_rect.x0, p.shade_rect.z0, std::min(p.shade_rect.x1, v.nx), std::min(p.shade_rect.z1, v.nz)};
    }
    p.shade_key = key;
    p.shade_dirty = false;
    p.shade_partial = false;
    p.shade.resize(size);
    for (uint32_t z = r.z0; z < r.z1; z++) {
        for (uint32_t x = r.x0; x < r.x1; x++) {
            vertex_color(key, v, x, z, &p.shade[(static_cast<std::size_t>(z) * v.nx + x) * 3]);
        }
    }
}

void write_texel(uint8_t* dst, int fmt, const float (&c)[3])
{
    const uint32_t r = static_cast<uint32_t>(std::clamp(c[0], 0.0f, 255.0f) + 0.5f);
    const uint32_t g = static_cast<uint32_t>(std::clamp(c[1], 0.0f, 255.0f) + 0.5f);
    const uint32_t b = static_cast<uint32_t>(std::clamp(c[2], 0.0f, 255.0f) + 0.5f);
    auto put16 = [&](uint32_t v) {
        dst[0] = static_cast<uint8_t>(v);
        dst[1] = static_cast<uint8_t>(v >> 8);
    };
    switch (fmt) {
    case BM_FORMAT_565_RGB: put16((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)); break;
    case BM_FORMAT_4444_ARGB: put16(0xF000u | (r >> 4) << 8 | (g >> 4) << 4 | (b >> 4)); break;
    case BM_FORMAT_1555_ARGB: put16(0x8000u | (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3)); break;
    case BM_FORMAT_888_RGB:
        dst[0] = static_cast<uint8_t>(b);
        dst[1] = static_cast<uint8_t>(g);
        dst[2] = static_cast<uint8_t>(r);
        break;
    case BM_FORMAT_8888_ARGB:
        dst[0] = static_cast<uint8_t>(b);
        dst[1] = static_cast<uint8_t>(g);
        dst[2] = static_cast<uint8_t>(r);
        dst[3] = 0xFF;
        break;
    default: break;
    }
}

int texel_bytes(int fmt)
{
    switch (fmt) {
    case BM_FORMAT_565_RGB:
    case BM_FORMAT_4444_ARGB:
    case BM_FORMAT_1555_ARGB: return 2;
    case BM_FORMAT_888_RGB: return 3;
    case BM_FORMAT_8888_ARGB: return 4;
    default: return 0;
    }
}

// A chunk's texture spans its cells edge to edge: texel i sits on cell coordinate x0 + i * w / (res - 1),
// so neighbouring chunks share their border texels.
float chunk_tex_coord(uint32_t local, uint32_t span, uint32_t res)
{
    return (0.5f + static_cast<float>(local) / static_cast<float>(span) * static_cast<float>(res - 1)) /
           static_cast<float>(res);
}

enum class Composite
{
    ok,
    over_budget, // the level's composite budget is full: this chunk draws untextured and tries again
    failed,      // the texture could not be made or written: the terrain draws untextured
};

// Frees the composite drawn longest ago, when no paint in the last keep_frames drew it.
bool evict_idle_composite()
{
    Preview* owner = nullptr;
    PreviewChunk* oldest = nullptr;
    for (auto& p : g_previews) {
        for (PreviewChunk& c : p->chunks) {
            if (c.bm < 0 || !composite_evictable(c.last_drawn, g_frame, keep_frames)) continue;
            if (!oldest || c.last_drawn < oldest->last_drawn) {
                oldest = &c;
                owner = p.get();
            }
        }
    }
    if (!oldest) return false;
    release_chunk_bitmap(*oldest, owner->res);
    oldest->dirty = true;
    return true;
}

Composite create_chunk_bitmap(PreviewChunk& chunk, uint32_t res)
{
    while (!composite_budget_fits(g_level_bitmaps, g_level_texels, res, max_level_bitmaps, max_level_texels)) {
        if (g_budget_full_frame == g_frame || !evict_idle_composite()) {
            g_budget_full_frame = g_frame;
            return Composite::over_budget;
        }
    }
    chunk.bm = -1;
    for (auto& spare = spare_bitmaps(res); chunk.bm < 0 && !spare.empty(); spare.pop_back()) {
        if (preview_bitmap_valid(spare.back(), res)) chunk.bm = spare.back();
    }
    if (chunk.bm < 0) chunk.bm = bm_create(composite_format, static_cast<int>(res), static_cast<int>(res));
    if (chunk.bm < 0) return Composite::failed;
    g_level_bitmaps++;
    g_level_texels += static_cast<uint64_t>(res) * res;
    return Composite::ok;
}

// Blends the layer tiles by the weight maps into chunk k's bitmap, then lays the overlay tiles over
// them by their coverage and alpha, lit by `light` when given.
Composite composite_chunk(Preview& p, const at::GridView& v, uint32_t k,
                          const LayerTile* const (&tiles)[at::max_layers],
                          const LayerTile* const (&overlay_tiles)[at::max_overlays], const TerrainBakedLight* light)
{
    PreviewChunk& chunk = p.chunks[k];
    const uint32_t res = p.res;
    if (!preview_bitmap_valid(chunk.bm, res)) {
        if (chunk.bm >= 0) release_chunk_bitmap(chunk, res);
        if (const Composite made = create_chunk_bitmap(chunk, res); made != Composite::ok) return made;
    }
    if (!preview_texture_ready(chunk.bm)) {
        release_chunk_bitmap(chunk, res);
        return Composite::failed;
    }
    GrLockInfo lock{};
    if (!gr_lock(chunk.bm, 0, &lock, 2)) return Composite::failed;
    const int bpp = texel_bytes(lock.format);
    if (!bpp || !lock.data || lock.w < static_cast<int>(res) || lock.h < static_cast<int>(res)) {
        gr_unlock(&lock);
        return Composite::failed;
    }

    const uint32_t cx = at::cells(v.nx), cz = at::cells(v.nz);
    const at::ChunkRect r = at::chunk_rect(cx, cz, p.edge, k);
    const uint32_t mul = v.weight_res_mul;
    const uint32_t ww = at::weight_width(v.nx, mul), wh = at::weight_height(v.nz, mul);
    const std::size_t map1 = at::weight_map_bytes(v.nx, v.nz, mul);
    const uint32_t layers = std::min<uint32_t>(v.layer_count, at::max_layers);
    const std::vector<uint8_t>& coverage = p.grid->overlay;
    const uint32_t overlays = coverage.size() == at::overlay_map_bytes(v.nx, v.nz, mul)
                                  ? static_cast<uint32_t>(std::min<std::size_t>(p.overlays.size(), at::max_overlays))
                                  : 0;
    const float inv = 1.0f / static_cast<float>(res - 1);

    struct Tap
    {
        uint32_t i0, i1;
        float f;
        float world;
    };
    auto tap = [&](float cell, uint32_t count, float origin) {
        const float t = std::clamp(cell * mul - 0.5f, 0.0f, static_cast<float>(count - 1));
        const uint32_t i0 = std::min(static_cast<uint32_t>(t), count - 1);
        return Tap{i0, std::min(i0 + 1, count - 1), t - static_cast<float>(i0), origin + cell * v.cell_size};
    };
    std::vector<Tap> xs(res);
    for (uint32_t i = 0; i < res; i++) {
        xs[i] = tap(static_cast<float>(r.x0) + static_cast<float>(r.x1 - r.x0) * i * inv, ww, v.origin[0]);
    }

    static const float fallback[3] = {160.0f, 160.0f, 160.0f};
    for (uint32_t j = 0; j < res; j++) {
        const Tap tz = tap(static_cast<float>(r.z0) + static_cast<float>(r.z1 - r.z0) * j * inv, wh, v.origin[2]);
        uint8_t* row = lock.data + static_cast<std::size_t>(j) * lock.stride_in_bytes;
        for (uint32_t i = 0; i < res; i++) {
            const Tap& tx = xs[i];
            const std::size_t t00 = (static_cast<std::size_t>(tz.i0) * ww + tx.i0) * 4;
            const std::size_t t10 = (static_cast<std::size_t>(tz.i0) * ww + tx.i1) * 4;
            const std::size_t t01 = (static_cast<std::size_t>(tz.i1) * ww + tx.i0) * 4;
            const std::size_t t11 = (static_cast<std::size_t>(tz.i1) * ww + tx.i1) * 4;
            float w[at::max_layers] = {};
            float total = 0.0f;
            for (uint32_t l = 0; l < layers; l++) {
                const std::size_t off = (l < 4 ? 0 : map1) + (l & 3);
                const float a = v.weights[t00 + off] + (v.weights[t10 + off] - v.weights[t00 + off]) * tx.f;
                const float b = v.weights[t01 + off] + (v.weights[t11 + off] - v.weights[t01 + off]) * tx.f;
                w[l] = a + (b - a) * tz.f;
                total += w[l];
            }
            if (total <= 0.0f) {
                w[0] = 1.0f;
                total = 1.0f;
            }
            float c[3] = {};
            for (uint32_t l = 0; l < layers; l++) {
                if (w[l] <= 0.0f) continue;
                const float s = w[l] / total;
                const LayerTile* tile = tiles[l] ? tiles[l] : tiles[0];
                float t[3];
                if (tile) {
                    float uv[2];
                    at::layer_uv(tx.world, tz.world, v.layer_uv_scale[l], uv);
                    sample_tile(tile->rgb, tile->size, uv[0], uv[1], t);
                }
                else {
                    std::memcpy(t, fallback, sizeof(t));
                }
                for (int ch = 0; ch < 3; ch++) c[ch] += t[ch] * s;
            }
            // lerp(c, overlay, alpha * coverage), with the overlay premultiplied by its alpha
            for (uint32_t o = 0; o < overlays; o++) {
                const LayerTile* tile = overlay_tiles[o];
                if (!tile) continue;
                const float a = coverage[t00 + o] + (coverage[t10 + o] - coverage[t00 + o]) * tx.f;
                const float b = coverage[t01 + o] + (coverage[t11 + o] - coverage[t01 + o]) * tx.f;
                const float cov = (a + (b - a) * tz.f) / 255.0f;
                if (cov <= 0.0f) continue;
                float uv[2], s[4];
                at::layer_uv(tx.world, tz.world, p.overlays[o].second, uv);
                sample_tile(tile->rgba_premul, tile->size, uv[0], uv[1], s);
                const float keep = 1.0f - s[3] / 255.0f * cov;
                for (int ch = 0; ch < 3; ch++) c[ch] = c[ch] * keep + s[ch] * cov;
            }
            if (light) {
                float texel[3];
                terrain_baked_light_sample(*light, tx.world, tz.world, texel);
                for (int ch = 0; ch < 3; ch++) c[ch] = terrain_preview_lit(c[ch], texel[ch]);
            }
            write_texel(row + static_cast<std::size_t>(i) * bpp, lock.format, c);
        }
    }
    gr_unlock(&lock);
    chunk.lit = light != nullptr;
    return Composite::ok;
}

// ─── Drawing ────────────────────────────────────────────────────────────────

std::vector<GrVertex> g_verts;
std::vector<uint32_t> g_vert_stamp;
uint32_t g_stamp = 0;

Vector3 grid_point(const at::GridView& v, uint32_t x, uint32_t z, float lift = 0.0f)
{
    float p[3];
    at::grid_position(v, x, z, p);
    return {p[0], p[1] + lift, p[2]};
}

bool walls_closed(const at::GridView& v)
{
    return (v.flags & at::flag_geoable) != 0;
}

bool walls_skirted(const at::GridView& v)
{
    return !walls_closed(v) && (v.flags & at::flag_skirts) && v.skirt_depth >= at::min_skirt_depth;
}

void set_color(const uint8_t (&rgb)[3])
{
    set_draw_color(rgb[0], rgb[1], rgb[2], 0xff);
}

// A polyline over grid vertices from (x0, z0) toward (x1, z1) in `step`s, always ending on (x1, z1).
void draw_grid_polyline(const at::GridView& v, uint32_t x0, uint32_t z0, uint32_t x1, uint32_t z1, uint32_t step,
                        float lift)
{
    const uint32_t len = std::max(x1 - x0, z1 - z0);
    if (len == 0) return;
    Vector3 prev = grid_point(v, x0, z0, lift);
    for (uint32_t s = std::min(step, len);; s = std::min(s + step, len)) {
        const uint32_t x = x1 > x0 ? x0 + s : x0, z = z1 > z0 ? z0 + s : z0;
        const Vector3 cur = grid_point(v, x, z, lift);
        gr_line_3d(&prev, &cur, editor_line_mode());
        prev = cur;
        if (s == len) break;
    }
}

void draw_outline(const at::GridView& v, uint32_t edge, bool chunk_lines)
{
    const uint32_t cx = at::cells(v.nx), cz = at::cells(v.nz);
    const float lift = std::max(0.02f, v.cell_size * 0.01f);
    if (chunk_lines) {
        set_color(chunk_line_rgb);
        for (uint32_t x = edge; x < cx; x += edge) draw_grid_polyline(v, x, 0, x, cz, 1, lift);
        for (uint32_t z = edge; z < cz; z += edge) draw_grid_polyline(v, 0, z, cx, z, 1, lift);
    }
    set_color(terrain_selected_rgb);
    draw_grid_polyline(v, 0, 0, cx, 0, 1, lift);
    draw_grid_polyline(v, 0, cz, cx, cz, 1, lift);
    draw_grid_polyline(v, 0, 0, 0, cz, 1, lift);
    draw_grid_polyline(v, cx, 0, cx, cz, 1, lift);
}

// Brush-style wireframe for the ortho views and the brush-only view modes: every k-th grid line,
// at most 64 per axis.
void draw_wireframe(const at::GridView& v, bool selected)
{
    const uint32_t cx = at::cells(v.nx), cz = at::cells(v.nz);
    const uint32_t k = std::max<uint32_t>(1, (std::max(cx, cz) + 63) / 64);
    set_color(selected ? terrain_selected_rgb : terrain_unselected_rgb);
    for (uint32_t z = 0;; z = std::min(z + k, cz)) {
        draw_grid_polyline(v, 0, z, cx, z, k, 0.0f);
        if (z == cz) break;
    }
    for (uint32_t x = 0;; x = std::min(x + k, cx)) {
        draw_grid_polyline(v, x, 0, x, cz, k, 0.0f);
        if (x == cx) break;
    }
}

struct ChunkDraw
{
    const at::GridView& v;
    const Preview& p;
    at::ChunkRect r;
    uint32_t tmap_flags;
    uint32_t mode;
    bool outer_dense;
    // The composite already carries the light
    bool white;

    uint32_t span_x() const { return r.x1 - r.x0; }
    uint32_t span_z() const { return r.z1 - r.z0; }

    GrVertex* vert(uint32_t x, uint32_t z)
    {
        const std::size_t i = static_cast<std::size_t>(z - r.z0) * (span_x() + 1) + (x - r.x0);
        GrVertex& gv = g_verts[i];
        if (g_vert_stamp[i] != g_stamp) {
            g_vert_stamp[i] = g_stamp;
            const Vector3 pos = grid_point(v, x, z);
            project_to_screen(&gv, &pos);
            gv.u = chunk_tex_coord(x - r.x0, span_x(), p.res);
            gv.v = chunk_tex_coord(z - r.z0, span_z(), p.res);
            static const uint8_t white_rgb[3] = {0xff, 0xff, 0xff};
            const uint8_t* c = white ? white_rgb : &p.shade[(static_cast<std::size_t>(z) * v.nx + x) * 3];
            gv.r = c[0];
            gv.g = c[1];
            gv.b = c[2];
            gv.a = 0xff;
        }
        return &gv;
    }

    // Fanned from one end of the cell's diagonal: (a, b, c, d) splits along a-c, (b, c, d, a) along
    // b-d, as emit_chunk splits it.
    void cell(uint32_t x, uint32_t z)
    {
        if (!at::cell_solid(v, x, z)) return;
        GrVertex* a = vert(x, z);
        GrVertex* b = vert(x + 1, z);
        GrVertex* c = vert(x + 1, z + 1);
        GrVertex* d = vert(x, z + 1);
        GrVertex* poly[4] = {a, b, c, d};
        if (at::get_cell_bit(v.diag, at::cells(v.nx), x, z)) {
            poly[0] = b;
            poly[1] = c;
            poly[2] = d;
            poly[3] = a;
        }
        gr_poly_render(4, poly, tmap_flags, mode, 0, 0.0f);
    }

    void cells(uint32_t x0, uint32_t z0, uint32_t x1, uint32_t z1)
    {
        for (uint32_t z = z0; z < z1; z++) {
            for (uint32_t x = x0; x < x1; x++) cell(x, z);
        }
    }

    // A coarse quad of solid cells. Its sides on a chunk border keep every grid vertex where another
    // chunk or a wall meets them (outer_dense), so they match that edge whatever its step; the fan
    // starts at a corner away from those sides so no triangle collapses onto one.
    void quad(uint32_t x, uint32_t z, uint32_t x2, uint32_t z2)
    {
        const uint32_t cx = at::cells(v.nx), cz = at::cells(v.nz);
        const bool dense[4] = {z == r.z0 && (z > 0 || outer_dense), x2 == r.x1 && (x2 < cx || outer_dense),
                               z2 == r.z1 && (z2 < cz || outer_dense), x == r.x0 && (x > 0 || outer_dense)};
        GrVertex* loop[16];
        int n = 0, corner[4];
        corner[0] = n;
        loop[n++] = vert(x, z);
        if (dense[0]) for (uint32_t i = x + 1; i < x2; i++) loop[n++] = vert(i, z);
        corner[1] = n;
        loop[n++] = vert(x2, z);
        if (dense[1]) for (uint32_t i = z + 1; i < z2; i++) loop[n++] = vert(x2, i);
        corner[2] = n;
        loop[n++] = vert(x2, z2);
        if (dense[2]) for (uint32_t i = x2 - 1; i > x; i--) loop[n++] = vert(i, z2);
        corner[3] = n;
        loop[n++] = vert(x, z2);
        if (dense[3]) for (uint32_t i = z2 - 1; i > z; i--) loop[n++] = vert(x, i);
        // Corner c touches sides c and c - 1.
        int start = -1;
        for (int c = 0; c < 4 && start < 0; c++) {
            if (!dense[c] && !dense[(c + 3) & 3]) start = corner[c];
        }
        if (start < 0) {
            cells(x, z, x2, z2);
            return;
        }
        GrVertex* poly[16];
        for (int i = 0; i < n; i++) poly[i] = loop[(start + i) % n];
        gr_poly_render(n, poly, tmap_flags, mode, 0, 0.0f);
    }

    void draw(uint32_t step)
    {
        const std::size_t count = static_cast<std::size_t>(span_x() + 1) * (span_z() + 1);
        if (g_verts.size() < count) {
            g_verts.resize(count);
            g_vert_stamp.resize(count, 0);
        }
        if (++g_stamp == 0) {
            std::fill(g_vert_stamp.begin(), g_vert_stamp.end(), 0);
            g_stamp = 1;
        }
        if (step <= 1) {
            cells(r.x0, r.z0, r.x1, r.z1);
            return;
        }
        for (uint32_t z = r.z0; z < r.z1; z += step) {
            for (uint32_t x = r.x0; x < r.x1; x += step) {
                const uint32_t x2 = std::min(x + step, r.x1), z2 = std::min(z + step, r.z1);
                if (x2 - x == 1 && z2 - z == 1) {
                    cell(x, z);
                }
                else {
                    quad(x, z, x2, z2);
                }
            }
        }
    }
};

// The outer walls of a closed or skirted terrain, flat shaded, with the underside texture mapped as
// emit_chunk maps it. Walls around holes are left out.
void draw_walls(Preview& p, const at::GridView& v, bool textured, const std::string& underside)
{
    const bool closed = walls_closed(v);
    if (!closed && !walls_skirted(v)) return;
    int bm = -1;
    if (textured) {
        const std::string& name = underside.empty() && !p.layers.empty() ? p.layers[0].first : underside;
        if (name != p.underside_name) {
            p.underside_name = name;
            p.underside_bm = alpine_dlg_resolve_bitmap(name.c_str());
        }
        bm = p.underside_bm;
    }
    gr_set_bitmap(bm, -1);
    const uint32_t flags = bm >= 0 ? tmap_uv | tmap_rgb : tmap_rgb;
    const uint32_t mode = bm >= 0 ? mode_textured_wrap : mode_vertex;
    const float s0 = v.layer_uv_scale[0];

    auto wall = [&](uint32_t px, uint32_t pz, uint32_t qx, uint32_t qz, float nx, float nz) {
        const bool along_x = nz != 0.0f;
        const float n[3] = {nx, 0.0f, nz};
        uint8_t rgb[3];
        light_color(p.shade_key, n, rgb);
        Vector3 pts[4] = {grid_point(v, px, pz), grid_point(v, qx, qz), grid_point(v, qx, qz), grid_point(v, px, pz)};
        pts[2].y = closed ? at::bottom_y(v) : pts[1].y - v.skirt_depth;
        pts[3].y = closed ? at::bottom_y(v) : pts[0].y - v.skirt_depth;
        GrVertex gv[4] = {};
        GrVertex* poly[4];
        for (int i = 0; i < 4; i++) {
            project_to_screen(&gv[i], &pts[i]);
            float uv[2];
            at::wall_uv(along_x ? pts[i].x : pts[i].z, pts[i].y, s0, uv);
            gv[i].u = uv[0];
            gv[i].v = uv[1];
            gv[i].r = rgb[0];
            gv[i].g = rgb[1];
            gv[i].b = rgb[2];
            gv[i].a = 0xff;
            poly[i] = &gv[i];
        }
        gr_poly_render(4, poly, flags, mode, 0, 0.0f);
    };

    const uint32_t cx = at::cells(v.nx), cz = at::cells(v.nz);
    for (uint32_t x = 0; x < cx; x++) {
        if (at::cell_solid(v, x, 0)) wall(x, 0, x + 1, 0, 0.0f, -1.0f);
        if (at::cell_solid(v, x, cz - 1)) wall(x + 1, cz, x, cz, 0.0f, 1.0f);
    }
    for (uint32_t z = 0; z < cz; z++) {
        if (at::cell_solid(v, 0, z)) wall(0, z + 1, 0, z, -1.0f, 0.0f);
        if (at::cell_solid(v, cx - 1, z)) wall(cx, z, cx, z + 1, 1.0f, 0.0f);
    }
}

bool chunk_culled(const at::GridView& v, const at::ChunkRect& r, const PreviewChunk& c)
{
    const float xs[2] = {v.origin[0] + r.x0 * v.cell_size, v.origin[0] + r.x1 * v.cell_size};
    const float zs[2] = {v.origin[2] + r.z0 * v.cell_size, v.origin[2] + r.z1 * v.cell_size};
    const float ys[2] = {c.y_lo, c.y_hi};
    uint8_t all = 0xff;
    for (int i = 0; i < 8; i++) {
        const Vector3 p{xs[i & 1], ys[(i >> 1) & 1], zs[(i >> 2) & 1]};
        GrVertex gv{};
        project_to_screen(&gv, &p);
        all &= gv.clip_flags;
    }
    return all != 0;
}

// Grid step for a chunk by the on-screen size of a cell at its nearest point.
uint32_t chunk_step(const at::GridView& v, const at::ChunkRect& r, const PreviewChunk& c)
{
    const float lo[3] = {v.origin[0] + r.x0 * v.cell_size, c.y_lo, v.origin[2] + r.z0 * v.cell_size};
    const float hi[3] = {v.origin[0] + r.x1 * v.cell_size, c.y_hi, v.origin[2] + r.z1 * v.cell_size};
    float dist_sq = 0.0f;
    for (int i = 0; i < 3; i++) {
        const float d = std::max({lo[i] - ed_cam_pos[i], 0.0f, ed_cam_pos[i] - hi[i]});
        dist_sq += d * d;
    }
    const float cell_px = v.cell_size * gr_half_width / std::max(std::sqrt(dist_sq), 1e-3f);
    if (cell_px < 1.5f) return 4;
    if (cell_px < 3.0f) return 2;
    return 1;
}

void draw_surface(CDedLevel& level, Preview& p, const DedTerrain& terrain, const DedTerrainData& d,
                  const at::GridView& v)
{
    int style = style_lit;
    if (view_lightmaps_only) style = style_lightmaps;
    else if (view_room_colors) style = style_room_colors;
    else if (editor_textures_enabled) style = style_textures;
    const bool textured = style == style_lit || style == style_textures;

    // The baked light as RED draws brush lightmaps in this style: in the lit style it goes into the
    // composites (texture x lightmap x 2), with lightmaps alone into the vertex colours.
    update_baked_light(p, terrain, d);
    ShadeKey key = current_shade_key(level, style);
    // A fullbright terrain shows no light in the styles that show it.
    const bool light_style = style == style_lit || style == style_lightmaps;
    key.fullbright = light_style && d.fullbright;
    if (light_style && !key.fullbright) {
        key.light = shown_light(p);
        key.light_gen = key.light ? p.light_gen : 0;
    }
    const TerrainBakedLight* composite_light = style == style_lit ? key.light : nullptr;
    if (composite_light != p.composite_light || (composite_light && p.composite_light_gen != p.light_gen)) {
        p.composite_light = composite_light;
        p.composite_light_gen = p.light_gen;
        for (PreviewChunk& c : p.chunks) c.dirty = true;
    }

    update_bounds(p, v);
    update_shade(p, v, key);

    const LayerTile* tiles[at::max_layers] = {};
    const LayerTile* overlay_tiles[at::max_overlays] = {};
    if (textured) {
        for (std::size_t l = 0; l < p.layers.size() && l < at::max_layers; l++) {
            if (!p.layers[l].first.empty()) tiles[l] = get_tile(p.layers[l].first);
        }
        for (std::size_t o = 0; o < p.overlays.size() && o < at::max_overlays; o++) {
            if (!p.overlays[o].first.empty()) overlay_tiles[o] = get_tile(p.overlays[o].first);
        }
    }

    const uint32_t cx = at::cells(v.nx), cz = at::cells(v.nz);
    std::vector<uint32_t> visible;
    for (uint32_t k = 0; k < p.chunks.size(); k++) {
        if (chunk_culled(v, at::chunk_rect(cx, cz, p.edge, k), p.chunks[k])) continue;
        visible.push_back(k);
        p.chunks[k].last_drawn = g_frame;
    }

    // Composites first, while none of this terrain's polygons are queued.
    if (textured) {
        for (uint32_t k : visible) {
            PreviewChunk& chunk = p.chunks[k];
            if (!chunk.dirty || p.textures_failed) continue;
            if (g_budget_left_ms <= 0.0) {
                g_work_pending = true;
                break;
            }
            LARGE_INTEGER start;
            QueryPerformanceCounter(&start);
            const Composite result = composite_chunk(p, v, k, tiles, overlay_tiles, composite_light);
            if (result == Composite::ok) chunk.dirty = false;
            else if (result == Composite::failed) p.textures_failed = true;
            else if (!g_budget_logged) {
                g_budget_logged = true;
                terrain_report("The terrain preview's texture budget is full: chunks that get no texture draw "
                               "untextured until other terrain leaves the view.",
                               false);
            }
            g_budget_left_ms -= elapsed_ms(start);
        }
    }

    set_draw_color(0xff, 0xff, 0xff, 0xff);
    const bool walls = walls_closed(v) || walls_skirted(v);
    for (uint32_t k : visible) {
        PreviewChunk& chunk = p.chunks[k];
        const at::ChunkRect r = at::chunk_rect(cx, cz, p.edge, k);
        // An outdated composite stands in until its turn comes.
        const bool use_texture = textured && !p.textures_failed && preview_bitmap_valid(chunk.bm, p.res);
        gr_set_bitmap(use_texture ? chunk.bm : -1, -1);
        ChunkDraw draw{v, p, r, use_texture ? tmap_uv | tmap_rgb : tmap_rgb, use_texture ? mode_textured : mode_vertex,
                       walls, use_texture && chunk.lit};
        draw.draw(chunk.has_hole ? 1 : chunk_step(v, r, chunk));
    }
    draw_walls(p, v, textured, d.underside_texture);
}

void level_face_draw_hooked(GSolid* solid, GFace* face, char outline)
{
    if (face && terrain_preview_hides_room(face->which_room)) {
        CDedLevel* level = CDedLevel::Get();
        if (level && solid == level->solid) return;
    }
    level_face_draw_hook.call_target(solid, face, outline);
}

} // namespace

bool terrain_view_draws_solid()
{
    return gr_perspective && level_render_mode != LEVEL_RENDER_BRUSHES_ONLY && !view_see_through;
}

void terrain_preview_draw(CDedLevel& level, const DedTerrain& terrain, const DedTerrainData& data, bool selected)
{
    if (!data.grid || data.layers.empty()) return;
    try {
        Preview& p = sync_preview(terrain, data);
        const at::GridView v = terrain_grid_view(terrain.pos, data, *data.grid);
        if (!terrain_view_draws_solid()) {
            draw_wireframe(v, selected);
            return;
        }
        draw_surface(level, p, terrain, data, v);
        if (selected) draw_outline(v, p.edge, true);
    }
    catch (const std::bad_alloc&) {
        xlog::error("[Terrain] out of memory drawing the terrain preview");
    }
}

void terrain_preview_frame_end(CDedLevel& level)
{
    const auto& terrains = level.GetAlpineLevelProperties().terrain_objects;
    for (auto it = g_previews.begin(); it != g_previews.end();) {
        if (std::find(terrains.begin(), terrains.end(), (*it)->owner) == terrains.end()) {
            release_preview(**it);
            it = g_previews.erase(it);
        }
        else {
            ++it;
        }
    }
    if (g_previews.empty()) g_tiles.clear();

    // Composites left over for the budget: this view paints again on the next idle tick.
    if (g_work_pending) editor_view_mark_repaint(editor_view_at(painting_view_index));
    g_work_outstanding = g_work_outstanding || g_work_pending;
    g_work_pending = false;
    g_budget_left_ms = composite_budget_ms;
    g_frame++;
}

bool terrain_preview_take_pending_work()
{
    const bool pending = g_work_outstanding;
    g_work_outstanding = false;
    return pending;
}

void terrain_preview_invalidate(const DedTerrain* terrain, const TerrainCellRect* cells, bool heights_changed)
{
    terrain_decorations_invalidate(terrain, cells);
    for (auto& p : g_previews) {
        if (p->owner != terrain) continue;
        p->shade_dirty = p->shade_dirty || heights_changed;
        p->bounds_dirty = true;
        if (!p->grid || !p->edge) return;
        const uint32_t cx = at::cells(p->grid->nx), cz = at::cells(p->grid->nz);
        for (uint32_t k = 0; k < p->chunks.size(); k++) {
            const at::ChunkRect r = at::chunk_rect(cx, cz, p->edge, k);
            // Weight texels blend across one cell, so a stroke reaches the chunks next to it.
            if (!cells || (r.x0 <= cells->x1 && cells->x0 <= r.x1 && r.z0 <= cells->z1 && cells->z0 <= r.z1)) {
                p->chunks[k].dirty = true;
            }
        }
        return;
    }
}

void terrain_preview_heights_changed(const DedTerrain* terrain, const TerrainCellRect* verts)
{
    terrain_decorations_invalidate(terrain, verts);
    terrain_preview_lighting_changed(terrain);
    for (auto& p : g_previews) {
        if (p->owner != terrain) continue;
        if (!verts || !p->grid || !p->edge || p->shade_dirty) {
            p->shade_dirty = p->bounds_dirty = true;
            return;
        }
        // A vertex normal reads the vertices either side of it.
        const TerrainCellRect r{verts->x0 > 0 ? verts->x0 - 1 : 0, verts->z0 > 0 ? verts->z0 - 1 : 0, verts->x1 + 1,
                                verts->z1 + 1};
        if (p->shade_partial) {
            p->shade_rect = {std::min(p->shade_rect.x0, r.x0), std::min(p->shade_rect.z0, r.z0),
                             std::max(p->shade_rect.x1, r.x1), std::max(p->shade_rect.z1, r.z1)};
        }
        else {
            p->shade_rect = r;
            p->shade_partial = true;
        }
        const uint32_t cx = at::cells(p->grid->nx), cz = at::cells(p->grid->nz);
        for (uint32_t k = 0; k < p->chunks.size(); k++) {
            // A chunk spans vertices [x0, x1] x [z0, z1].
            const at::ChunkRect c = at::chunk_rect(cx, cz, p->edge, k);
            if (c.x0 < verts->x1 && verts->x0 <= c.x1 && c.z0 < verts->z1 && verts->z0 <= c.z1) {
                p->chunks[k].bounds_stale = true;
            }
        }
        return;
    }
}

void terrain_preview_lighting_changed(const DedTerrain* terrain)
{
    for (auto& p : g_previews) {
        if (p->owner == terrain) p->baked = BakedLight::recheck;
    }
}

void terrain_preview_textures_reloaded()
{
    g_tiles.clear();
    for (auto& p : g_previews) {
        p->textures_failed = false;
        p->underside_name.clear();
        p->underside_bm = -1;
        for (PreviewChunk& c : p->chunks) c.dirty = true;
    }
    editor_views_mark_repaint_all();
}

void terrain_preview_rebind_grid(const DedTerrain* terrain, const TerrainGrid* old_grid,
                                 const std::shared_ptr<const TerrainGrid>& new_grid)
{
    terrain_decorations_rebind_grid(terrain, old_grid, new_grid);
    for (auto& p : g_previews) {
        if (p->owner == terrain && p->grid.get() == old_grid) p->grid = new_grid;
    }
}

bool terrain_preview_layer_color(const std::string& texture, std::uint8_t (&rgb)[3])
{
    if (texture.empty()) return false;
    const LayerTile* tile = nullptr;
    try {
        tile = get_tile(texture);
    }
    catch (const std::bad_alloc&) {
        return false;
    }
    if (!tile || tile->size <= 0) return false;
    uint64_t sum[3] = {};
    const std::size_t n = static_cast<std::size_t>(tile->size) * tile->size;
    for (std::size_t i = 0; i < n; i++) {
        for (int k = 0; k < 3; k++) sum[k] += tile->rgb[i * 3 + k];
    }
    for (int k = 0; k < 3; k++) rgb[k] = static_cast<std::uint8_t>(sum[k] / n);
    return true;
}

void terrain_preview_forget(const DedTerrain* terrain)
{
    for (auto it = g_previews.begin(); it != g_previews.end(); ++it) {
        if ((*it)->owner == terrain) {
            release_preview(**it);
            g_previews.erase(it);
            break;
        }
    }
    if (g_previews.empty()) g_tiles.clear();
}

bool terrain_preview_hides_room(const GRoom* room)
{
    if (!room) return false;
    CDedLevel* level = CDedLevel::Get();
    if (!level) return false;
    const auto& props = level->GetAlpineLevelProperties();
    return props.is_terrain_room(room->uid) || props.is_terrain_split_room(room->uid);
}

TerrainRay terrain_screen_ray(float screen_x, float screen_y)
{
    float ray[8] = {};
    screen_to_ray(ray, screen_x, screen_y);
    return TerrainRay{{ray[0], ray[1], ray[2]}, {ray[3], ray[4], ray[5]}};
}

bool terrain_ray_hit(const DedTerrain& t, const TerrainRay& ray, float t_max, float& hit_t, bool ignore_holes)
{
    if (!t.data.grid) return false;
    const at::GridView v = terrain_grid_view(t.pos, t.data, *t.data.grid);
    return at::raycast(v, ray.o, ray.d, 0.0f, t_max, hit_t, ignore_holes);
}

DedTerrain* terrain_surface_pick(CDedLevel& level, float screen_x, float screen_y)
{
    const TerrainRay ray = terrain_screen_ray(screen_x, screen_y);
    DedTerrain* best = nullptr;
    float best_t = terrain_pick_reach;
    for (DedTerrain* t : level.GetAlpineLevelProperties().terrain_objects) {
        if (!t || t->hidden_in_editor || !t->data.grid || t->data.layers.empty()) continue;
        float hit = 0.0f;
        if (terrain_ray_hit(*t, ray, best_t, hit) && (!best || hit < best_t)) {
            best = t;
            best_t = hit;
        }
    }
    // Level faces the view draws in front of it; ortho, see-through and brush-only views draw none.
    if (best && terrain_view_draws_solid() && terrain_build_level_ray_hit(level, ray.o, ray.d, best_t) < best_t) {
        return nullptr;
    }
    return best;
}

void ApplyTerrainPreviewPatches()
{
    level_face_draw_hook.install();
}
