#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <new>

#include <zlib.h>
#include <xxhash.h>
#include <bc7enc.h>
#include <ert.h>

#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/FunHook.h>
#include <patch_common/MemUtils.h>
#include <xlog/xlog.h>

#include <common/scope_guard.h>
#include <common/lightmap/alpine_lightmap.h>
#include <common/lightmap/alpine_lightmap_decode.h>
#include <common/terrain/alpine_terrain.h>

#include "alpine_lightmaps.h"
#include "work_pool.h"
#include "level.h"
#include "mfc_types.h"
#include "terrain_build.h"

using namespace alpine_lightmap;
namespace at = alpine_terrain;

static_assert(at::max_terrains <= max_terrain_charts);
static_assert(static_cast<std::uint64_t>(at::max_verts - 1) * at::lightmap_density_max <= max_terrain_chart_dim);

namespace
{

constexpr std::uint32_t P = page_size;
constexpr std::uint32_t S = tile_step;
constexpr std::uint32_t G = gutter;

// A tile view's scratch buffer is (h_t - 1) * k_u * lm_w * 3 + w_t * 3 bytes, so a surface whose
// stock page is wide and whose refinement is large can ask for a lot at once. Charts over the cap
// are dropped rather than shrunk, so the atlas the header derives is the one that was shaded.
constexpr std::size_t max_tile_view_bytes = 64u * 1024u * 1024u;

void af_log(const std::string& line)
{
    editor_report(EditorReportLevel::info, "AlpineLightmaps", line, false);
}

void af_warn(const std::string& line)
{
    editor_report(EditorReportLevel::warn, "AlpineLightmaps", line, false);
}

void af_error(const std::string& line)
{
    editor_report(EditorReportLevel::error, "AlpineLightmaps", line, false);
}

// ─── engine record accessors ──────────────────────────────────────────────────

int surf_i(std::uintptr_t s, std::uint32_t off)
{
    return *reinterpret_cast<int*>(s + off);
}

float surf_f(std::uintptr_t s, std::uint32_t off)
{
    return *reinterpret_cast<float*>(s + off);
}

std::uintptr_t surf_lm(std::uintptr_t s)
{
    return *reinterpret_cast<std::uintptr_t*>(s + 0xc);
}

struct EngineLightmap
{
    int unknown0;
    int w;
    int h;
    std::uint8_t* pixels;
    int bm_handle;
    int index;
};

// ─── module state ─────────────────────────────────────────────────────────────

struct AfChart
{
    std::uint16_t k_u = 0;
    std::uint16_t k_v = 0;
    std::uint16_t w = 0; // the stock fragment, which a mover record stores
    std::uint16_t h = 0;
    ChartGeometry geom{};
    std::uint32_t tile_base = 0;
};

// One mover record of the bake: charts[first_chart, first_chart + num_surfaces) are its surfaces'.
struct AfMover
{
    std::int32_t uid = 0;
    std::uintptr_t solid = 0;
    std::uint32_t num_surfaces = 0;
    std::uint32_t first_chart = 0;
    std::uint32_t signature = 0;
    std::uint32_t hash_offset = 0; // of its surface_hash in the body
};

struct AfSurfaceRef
{
    std::uint32_t chart;
    std::uintptr_t solid;
};

// One terrain chart of the bake: the terrain it lights, at the density it got.
struct AfTerrain
{
    const DedTerrain* terrain = nullptr;
    std::uint32_t cells_x = 0;
    std::uint32_t cells_z = 0;
    std::uint32_t density = 0;
    TerrainChart wire{};
};

// One buffer per page, so no allocation grows with the page count.
using PageBuffers = std::vector<std::vector<std::uint8_t>>;

std::size_t page_buffers_size(const PageBuffers& pages)
{
    std::size_t size = 0;
    for (const auto& page : pages) {
        size += page.size();
    }
    return size;
}

struct AfBake
{
    bool active = false;      // charts derived, tiles being shaded
    bool complete = false;    // pages encoded, body ready
    std::uintptr_t solid = 0; // the static solid the charts are positional over
    std::uint32_t signature = 0;
    std::uint8_t base_density = 0;
    // charts[0, num_surface_charts) are the surfaces' charts, positionally; the terrain charts
    // follow in terrains order, then the movers' in movers order: the reader's tile order.
    std::uint32_t num_surface_charts = 0;
    bool has_surface_charts = false;
    std::vector<AfTerrain> terrains;
    std::vector<AfMover> movers;
    std::uint32_t mover_surfaces = 0; // over all movers
    std::uint32_t mover_density = 0;
    std::vector<AfChart> charts;
    std::vector<Tile> tiles;
    std::uint32_t num_pages = 0;
    PageBuffers pages; // num_pages buffers of P * P * 3, RGB8
    std::unordered_map<std::uintptr_t, AfSurfaceRef> surface_index;
    std::vector<std::uint8_t> body; // serialised section, fingerprints still zero
};

AfBake g_af;

// Drops the baked terrain lighting the viewport preview keeps.
void terrain_light_clear();

// Retained on load so re-saving a level nobody re-baked keeps its alpine lightmaps. The
// fingerprint it carries covers the surface records as they were written then, lightmap index
// included, so it is only valid to re-emit while d3d11-only lightmaps is what it was at bake time.
std::vector<std::uint8_t> g_retained;
bool g_retained_suppressed = false;
std::uint32_t g_retained_signature = 0;

bool g_tile_pass = false;
std::vector<std::uint8_t> g_tile_view;

// -rawlightmaps stores the atlas as raw RGB8 instead of BC7, which is what makes the writer's
// output exactly comparable with the stock lightmaps it is derived from.
bool g_raw_codec = false;

// Slices of 256x256 the bake may spend before it halves the density and starts over.
std::uint32_t page_budget()
{
    return max_layer_pages(g_raw_codec ? Codec::raw_rgb8 : Codec::bc7_unorm);
}

// Save-time state.
bool g_suppress_stock = false;
bool g_emit_af = false;
bool g_emit_retained = false;
bool g_emit_surface_charts = false;
std::uint32_t g_save_num_faces = 0;
std::uint32_t g_save_num_surfaces = 0;
std::uint32_t g_save_surface_hash = 0;
bool g_hash_armed = false;
bool g_hash_capturing = false;
XXH32_state_t* g_hash_state = nullptr;

// The movers section's solid fingerprints, by mover uid, as this save wrote them.
struct SaveMover
{
    std::uint32_t hash = 0;
    std::uint32_t num_surfaces = 0;
    std::uintptr_t solid = 0;
    bool duplicate = false; // another brush wrote the same uid
};
std::unordered_map<std::int32_t, SaveMover> g_save_movers;
// The brush the movers section is writing while its BrushNode::write runs (0x004316c6 only).
const BrushNode* g_mover_save_brush = nullptr;

AlpineLevelProperties* level_props()
{
    auto* level = CDedLevel::Get();
    return level ? &level->GetAlpineLevelProperties() : nullptr;
}

// Not a wire value: a token that says the surfaces the charts were derived from are still the ones
// about to be written, so a Build Geometry between the bake and the save drops the section.
std::uint32_t surfaces_signature(std::uintptr_t solid)
{
    const auto [count, elems] = solid_surfaces(solid);
    if (count <= 0 || !elems) {
        return 0;
    }
    std::vector<std::uint32_t> token;
    token.reserve(static_cast<std::size_t>(count) * 6);
    for (int i = 0; i < count; i++) {
        const auto surface = *reinterpret_cast<std::uintptr_t*>(elems + i * 4);
        if (!surface) {
            token.insert(token.end(), {0u, 0u, 0u, 0u, 0u, 0u});
            continue;
        }
        const std::uintptr_t lm = surf_lm(surface);
        token.push_back(static_cast<std::uint32_t>(surf_i(surface, 0x10)));
        token.push_back(static_cast<std::uint32_t>(surf_i(surface, 0x14)));
        token.push_back(static_cast<std::uint32_t>(surf_i(surface, 0x18)));
        token.push_back(static_cast<std::uint32_t>(surf_i(surface, 0x1c)));
        token.push_back(lm ? static_cast<std::uint32_t>(*reinterpret_cast<int*>(lm + 4)) : 0u);
        token.push_back(lm ? static_cast<std::uint32_t>(*reinterpret_cast<int*>(lm + 8)) : 0u);
    }
    return XXH32(token.data(), token.size() * sizeof(std::uint32_t), 0);
}

// ─── chart derivation and packing ─────────────────────────────────────────────

// Deterministic skyline packer over one open page at a time: a tile that does not fit closes the
// page. Tiles arrive already ordered, so the placement is a pure function of the chart table.
class SkylinePacker
{
public:
    void reset()
    {
        pages_ = 0;
        new_page();
    }

    std::uint32_t pages() const { return pages_; }

    bool place(std::uint32_t w, std::uint32_t h, std::uint16_t& out_x, std::uint16_t& out_y,
               std::uint16_t& out_page)
    {
        if (w == 0 || h == 0 || w > P || h > P) {
            return false;
        }
        if (!try_place(w, h, out_x, out_y)) {
            new_page();
            if (!try_place(w, h, out_x, out_y)) {
                return false;
            }
        }
        out_page = static_cast<std::uint16_t>(pages_ - 1);
        return true;
    }

private:
    struct Node
    {
        std::uint32_t x, y, w;
    };

    void new_page()
    {
        nodes_.clear();
        nodes_.push_back({0, 0, P});
        pages_++;
    }

    // Lowest y wins, then leftmost x; both are read off the skyline in order, so the result never
    // depends on anything but the sequence of sizes.
    bool try_place(std::uint32_t w, std::uint32_t h, std::uint16_t& out_x, std::uint16_t& out_y)
    {
        std::size_t best = nodes_.size();
        std::uint32_t best_y = P + 1;
        for (std::size_t i = 0; i < nodes_.size(); i++) {
            std::uint32_t y = 0;
            if (!fits(i, w, y) || y + h > P) {
                continue;
            }
            if (y < best_y) {
                best = i;
                best_y = y;
            }
        }
        if (best == nodes_.size()) {
            return false;
        }
        out_x = static_cast<std::uint16_t>(nodes_[best].x);
        out_y = static_cast<std::uint16_t>(best_y);
        add_node(best, nodes_[best].x, best_y + h, w);
        return true;
    }

    bool fits(std::size_t index, std::uint32_t w, std::uint32_t& y) const
    {
        if (nodes_[index].x + w > P) {
            return false;
        }
        std::uint32_t left = w;
        y = nodes_[index].y;
        for (std::size_t i = index; i < nodes_.size() && left > 0; i++) {
            y = std::max(y, nodes_[i].y);
            left = nodes_[i].w >= left ? 0 : left - nodes_[i].w;
        }
        return left == 0;
    }

    void add_node(std::size_t index, std::uint32_t x, std::uint32_t y, std::uint32_t w)
    {
        nodes_.insert(nodes_.begin() + static_cast<std::ptrdiff_t>(index), Node{x, y, w});
        for (std::size_t i = index + 1; i < nodes_.size();) {
            const std::uint32_t prev_end = nodes_[i - 1].x + nodes_[i - 1].w;
            if (nodes_[i].x >= prev_end) {
                break;
            }
            const std::uint32_t shrink = prev_end - nodes_[i].x;
            if (nodes_[i].w <= shrink) {
                nodes_.erase(nodes_.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            nodes_[i].x += shrink;
            nodes_[i].w -= shrink;
            break;
        }
        for (std::size_t i = 0; i + 1 < nodes_.size();) {
            if (nodes_[i].y == nodes_[i + 1].y) {
                nodes_[i].w += nodes_[i + 1].w;
                nodes_.erase(nodes_.begin() + static_cast<std::ptrdiff_t>(i) + 1);
                continue;
            }
            i++;
        }
    }

    std::vector<Node> nodes_;
    std::uint32_t pages_ = 0;
};

// 64 bit: k_u is a u16 off the wire, so h_t * k_u * lm_w * 3 runs past 2^32 well before the cap
// this feeds would reject the chart.
std::uint64_t tile_view_bytes(const AfChart& c, std::uint32_t lm_w)
{
    std::uint64_t worst = 0;
    const std::uint64_t stride = static_cast<std::uint64_t>(c.k_u) * lm_w * 3u;
    for (std::uint32_t ty = 0; ty < c.geom.ny; ty++) {
        for (std::uint32_t tx = 0; tx < c.geom.nx; tx++) {
            const TileDims td = tile_dims(c.geom, tx, ty);
            worst = std::max(worst, (td.h_t - 1) * stride + td.w_t * 3u);
        }
    }
    return worst;
}

enum class PackResult
{
    ok,
    no_charts,
    over_budget,
};

// The surfaces' charts at `density`; false when no surface qualifies for one. `owner` names the solid
// in the log. A mover's charts are the ones its record can describe (mover_chart_geometry).
bool derive_surface_charts(std::uintptr_t solid, std::uint32_t surface_count, float density,
                           std::vector<AfChart>& charts, const std::string& owner = {}, bool mover = false)
{
    const auto surfaces = solid_surfaces(solid).elems;
    charts.assign(surface_count, AfChart{});
    bool any = false;

    for (std::uint32_t i = 0; i < surface_count; i++) {
        const auto surface = *reinterpret_cast<std::uintptr_t*>(surfaces + i * 4);
        if (!surface) {
            continue;
        }
        const int w = surf_i(surface, 0x18);
        const int h = surf_i(surface, 0x1c);
        charts[i].w = static_cast<std::uint16_t>(std::clamp(w, 0, 0xffff));
        charts[i].h = static_cast<std::uint16_t>(std::clamp(h, 0, 0xffff));
        const std::uintptr_t lm = surf_lm(surface);
        if (!lm) {
            continue;
        }
        const int lm_w = *reinterpret_cast<int*>(lm + 4);
        const int u_coef = surf_i(surface, 0x60);
        const int v_coef = surf_i(surface, 0x64);
        if (w <= 2 || h <= 2 || lm_w <= 0 || u_coef < 0 || u_coef > 2 || v_coef < 0 || v_coef > 2) {
            continue;
        }
        const float extent_u =
            surf_f(surface, 0x40 + u_coef * 4) - surf_f(surface, 0x34 + u_coef * 4);
        const float extent_v =
            surf_f(surface, 0x40 + v_coef * 4) - surf_f(surface, 0x34 + v_coef * 4);

        AfChart c = charts[i];
        c.k_u = k_from_density(static_cast<std::uint32_t>(w), extent_u, density);
        c.k_v = k_from_density(static_cast<std::uint32_t>(h), extent_v, density);
        c.geom = mover ? mover_chart_geometry(MoverSurfaceChart{c.k_u, c.k_v, c.w, c.h})
                       : chart_geometry(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), c.k_u, c.k_v);
        if (c.geom.empty()) {
            continue;
        }
        if (tile_view_bytes(c, static_cast<std::uint32_t>(lm_w)) > max_tile_view_bytes) {
            xlog::warn("[AlpineLightmaps] {}surface {} would need a {} MB tile view at k {}x{}, "
                       "leaving it on the stock lightmap only",
                       owner, i, tile_view_bytes(c, static_cast<std::uint32_t>(lm_w)) >> 20, c.k_u, c.k_v);
            continue;
        }
        charts[i] = c;
        any = true;
    }
    return any;
}

// Lays every chart's tiles out in the pages: surfaces first, then terrain, as the reader derives it.
PackResult pack_charts(std::vector<AfChart>& charts, std::vector<Tile>& tiles, std::uint32_t& out_pages)
{
    const auto chart_count = static_cast<std::uint32_t>(charts.size());
    tiles.clear();
    out_pages = 0;

    // tile_base is the running sum over the charts in order, exactly as the reader derives it
    std::vector<ChartGeometry> geoms(chart_count);
    std::vector<std::uint32_t> bases(chart_count);
    for (std::uint32_t i = 0; i < chart_count; i++) {
        geoms[i] = charts[i].geom;
    }
    const std::uint32_t total = compute_tile_bases(geoms.data(), chart_count, bases.data());
    for (std::uint32_t i = 0; i < chart_count; i++) {
        charts[i].tile_base = bases[i];
    }
    if (total == 0) {
        return PackResult::no_charts;
    }
    tiles.assign(total, Tile{0, 0, 0});

    // Tall tiles first so the skyline fills from the bottom; ties keep chart order, so the whole
    // placement is a deterministic function of the chart table.
    struct Entry
    {
        std::uint32_t index;
        std::uint32_t w, h;
    };
    std::vector<Entry> order;
    order.reserve(total);
    for (std::uint32_t i = 0; i < chart_count; i++) {
        const AfChart& c = charts[i];
        for (std::uint32_t ty = 0; ty < c.geom.ny; ty++) {
            for (std::uint32_t tx = 0; tx < c.geom.nx; tx++) {
                const TileDims td = tile_dims(c.geom, tx, ty);
                order.push_back({tile_index(c.tile_base, c.geom, tx, ty), td.w_t, td.h_t});
            }
        }
    }
    std::stable_sort(order.begin(), order.end(), [](const Entry& a, const Entry& b) {
        if (a.h != b.h) {
            return a.h > b.h;
        }
        if (a.w != b.w) {
            return a.w > b.w;
        }
        return a.index < b.index;
    });

    SkylinePacker packer;
    packer.reset();
    for (const Entry& e : order) {
        Tile t{};
        if (!packer.place(e.w, e.h, t.x, t.y, t.page)) {
            return PackResult::over_budget;
        }
        tiles[e.index] = t;
    }
    out_pages = packer.pages();
    if (out_pages > page_budget() || out_pages > 0xffffu) {
        return PackResult::over_budget;
    }
    return PackResult::ok;
}

std::string surface_label(std::uint32_t chart)
{
    return "surface " + std::to_string(chart);
}

template<typename Label = std::string (*)(std::uint32_t)>
void report_offenders(const std::vector<AfChart>& charts, Label label = surface_label)
{
    struct Off
    {
        std::uint32_t index;
        std::uint64_t texels;
    };
    std::vector<Off> offs;
    for (std::uint32_t i = 0; i < charts.size(); i++) {
        const ChartGeometry& g = charts[i].geom;
        if (!g.empty()) {
            offs.push_back({i, static_cast<std::uint64_t>(g.cw) * g.ch});
        }
    }
    std::stable_sort(offs.begin(), offs.end(),
                     [](const Off& a, const Off& b) { return a.texels > b.texels; });
    for (std::size_t i = 0; i < offs.size() && i < 20; i++) {
        const ChartGeometry& g = charts[offs[i].index].geom;
        af_log("  offender " + label(offs[i].index) + ": chart " +
               std::to_string(g.cw) + "x" + std::to_string(g.ch) + ", " +
               std::to_string(g.tile_count()) + " tiles");
    }
}

// ─── page addressing ──────────────────────────────────────────────────────────

std::uint8_t* page_texel(std::uint32_t page, std::uint32_t x, std::uint32_t y)
{
    if (page >= g_af.pages.size() || x >= P || y >= P) {
        return nullptr;
    }
    return g_af.pages[page].data() + (static_cast<std::size_t>(y) * P + x) * 3;
}

// Reaches the padding as well as the chart, so the blend's bilinear taps and the edge replication
// address the tile's whole stored rect the way the reconciliation does.
std::uint8_t* chart_texel_padded(const AfChart& c, std::int64_t cu, std::int64_t cv)
{
    const TileSlot slot = chart_tile_slot(c.geom, c.tile_base, cu, cv);
    if (!slot.valid || slot.index >= g_af.tiles.size()) {
        return nullptr;
    }
    const Tile& t = g_af.tiles[slot.index];
    return page_texel(t.page, t.x + static_cast<std::uint32_t>(slot.sx),
                      t.y + static_cast<std::uint32_t>(slot.sy));
}

std::uint8_t* chart_texel(const AfChart& c, std::uint32_t cu, std::uint32_t cv)
{
    if (cu >= c.geom.cw || cv >= c.geom.ch) {
        return nullptr;
    }
    return chart_texel_padded(c, cu, cv);
}

} // namespace

// ─── bake driver ──────────────────────────────────────────────────────────────

bool alpine_lm_tile_pass_active()
{
    return g_tile_pass;
}

static bool g_lighting_refused = false;

void alpine_lm_note_lighting_refused(bool refused)
{
    g_lighting_refused = refused;
}

bool alpine_lm_take_lighting_refused()
{
    return std::exchange(g_lighting_refused, false);
}

namespace
{

// The chart of `a` at its current density.
AfChart terrain_chart(AfTerrain& a)
{
    a.wire.w = static_cast<std::uint16_t>(terrain_chart_extent(a.cells_x, a.density));
    a.wire.h = static_cast<std::uint16_t>(terrain_chart_extent(a.cells_z, a.density));
    a.wire.texel_size = terrain_chart_texel_size(a.terrain->data.cell_size, a.density);
    AfChart c;
    c.geom = terrain_chart_geometry(a.wire.w, a.wire.h);
    return c;
}

// Every terrain whose compiled geometry is current. The rest keep no baked lighting; the Calculate
// Lighting pre-check has already said why.
std::vector<AfTerrain> collect_terrains(const AlpineLevelProperties& props)
{
    std::vector<AfTerrain> out;
    for (const DedTerrain* t : props.terrain_objects) {
        if (!t || !t->data.grid) {
            continue;
        }
        if (!terrain_build_is_current(*t)) {
            af_log(terrain_label(*t) + " is not built as it is now, its lighting is not baked");
            continue;
        }
        if (std::any_of(out.begin(), out.end(), [&](const AfTerrain& a) { return a.terrain->uid == t->uid; })) {
            af_log(terrain_label(*t) + " shares its uid with another terrain, its lighting is not baked");
            continue;
        }
        if (out.size() >= max_terrain_charts) {
            break;
        }
        const TerrainGrid& g = *t->data.grid;
        AfTerrain a;
        a.terrain = t;
        a.cells_x = at::cells(g.nx);
        a.cells_z = at::cells(g.nz);
        a.density = std::clamp<std::uint32_t>(t->data.lightmap_density, at::lightmap_density_min,
                                              at::lightmap_density_max);
        if (a.cells_x == 0 || a.cells_z == 0) {
            continue;
        }
        a.wire.terrain_uid = t->uid;
        a.wire.origin_x = t->pos.x;
        a.wire.origin_z = t->pos.z;
        a.wire.geometry_fingerprint = at::lighting_fingerprint(terrain_grid_view(t->pos, t->data, g));
        out.push_back(a);
    }
    return out;
}

// Every brush of every moving group, in the order the movers section (0x2000) and the bake visit them.
template<typename F>
void for_each_mover_brush(CDedLevel& level, F&& fn)
{
    const auto& groups = level.moving_groups;
    for (int g = 0; g < groups.size; g++) {
        const GroupEntry* group = groups.data_ptr[g];
        if (!group || !group->is_moving_group()) {
            continue;
        }
        for (int b = 0; b < group->brushes.size; b++) {
            const BrushNode* brush = group->brushes.data_ptr[b];
            if (brush && brush->geometry) {
                fn(*brush);
            }
        }
    }
}

// The movers the bake charts: one record per uid (a brush in several groups is one mover), none for a
// uid two brushes share, none for a solid without surfaces or with more than a record can hold.
std::vector<AfMover> collect_movers(CDedLevel& level)
{
    std::vector<AfMover> out;
    std::unordered_map<std::int32_t, std::uintptr_t> seen;
    std::vector<std::int32_t> shared;
    for_each_mover_brush(level, [&](const BrushNode& brush) {
        const auto solid = reinterpret_cast<std::uintptr_t>(brush.geometry);
        const auto [it, fresh] = seen.try_emplace(brush.uid, solid);
        if (!fresh) {
            if (it->second != solid && std::find(shared.begin(), shared.end(), brush.uid) == shared.end()) {
                shared.push_back(brush.uid);
            }
            return;
        }
        const int count = solid_surfaces(solid).count;
        if (count <= 0) {
            return;
        }
        if (static_cast<std::uint32_t>(count) > max_mover_surfaces) {
            af_warn("mover " + std::to_string(brush.uid) + " has " + std::to_string(count) +
                    " lightmap surfaces, more than a mover chart can hold; it keeps its stock lightmaps");
            return;
        }
        AfMover m;
        m.uid = brush.uid;
        m.solid = solid;
        m.num_surfaces = static_cast<std::uint32_t>(count);
        out.push_back(m);
    });
    for (std::int32_t uid : shared) {
        af_warn("two mover brushes share uid " + std::to_string(uid) + ", neither gets alpine lighting");
        out.erase(std::remove_if(out.begin(), out.end(), [uid](const AfMover& m) { return m.uid == uid; }), out.end());
    }
    if (out.size() > max_mover_charts) {
        af_warn("the level has " + std::to_string(out.size()) + " movers, only the first " +
                std::to_string(max_mover_charts) + " get alpine lighting");
        out.resize(max_mover_charts);
    }
    return out;
}

// The movers' charts at `density`, appended to `out` in order; drops the movers that get none and
// sets each kept mover's first_chart relative to the first mover chart.
void derive_mover_charts(std::vector<AfMover>& movers, std::uint32_t density, std::vector<AfChart>& out)
{
    out.clear();
    std::vector<AfMover> kept;
    std::vector<AfChart> charts;
    for (AfMover m : movers) {
        if (!derive_surface_charts(m.solid, m.num_surfaces, static_cast<float>(density), charts,
                                   "mover " + std::to_string(m.uid) + " ", true)) {
            continue;
        }
        m.first_chart = static_cast<std::uint32_t>(out.size());
        out.insert(out.end(), charts.begin(), charts.end());
        kept.push_back(m);
    }
    movers = std::move(kept);
}

} // namespace

std::uint32_t alpine_lm_bake_begin()
{
    // a re-bake supersedes the loaded section even when it ends up emitting nothing
    g_retained.clear();
    g_retained.shrink_to_fit();
    terrain_light_clear();
    g_af = AfBake{};
    g_tile_pass = false;

    auto* props = level_props();
    auto* level = CDedLevel::Get();
    if (!props || !level || !level->solid) {
        return 0;
    }

    const auto solid = reinterpret_cast<std::uintptr_t>(level->solid);
    const auto surface_count = static_cast<std::uint32_t>(std::max(0, solid_surfaces(solid).count));

    // The surfaces' charts first, at the level's density or below, exactly as if the level had no
    // terrain: the terrain charts are fitted around them afterwards and never cost them density.
    const bool charts_off = props->lightmap_density == lightmap_density_off;
    std::uint32_t density = effective_density(charts_off ? 0 : props->lightmap_density);
    std::vector<AfChart> surface_charts(surface_count);
    bool has_surface = false;
    if (props->legacy_lighting) {
        af_log("legacy lighting is set, surfaces keep their stock lightmaps only");
    }
    else if (charts_off) {
        af_log("alpine surface lightmaps are off, surfaces keep their stock lightmaps only");
    }
    else if (surface_count > 0) {
        std::vector<Tile> tiles;
        std::uint32_t pages = 0;
        while (true) {
            if (!derive_surface_charts(solid, surface_count, static_cast<float>(density), surface_charts)) {
                af_log("no surface qualifies for an alpine chart");
                break;
            }
            if (pack_charts(surface_charts, tiles, pages) == PackResult::ok) {
                has_surface = true;
                break;
            }
            af_log("density " + std::to_string(density) + " needs more than the " +
                   std::to_string(page_budget()) + " page budget");
            if (density <= density_min) {
                af_error("the level does not fit the page budget even at density 1, surfaces keep "
                         "their stock lightmaps only");
                report_offenders(surface_charts);
                break;
            }
            density = std::max<std::uint32_t>(density_min, density / 2);
        }
        if (!has_surface) {
            surface_charts.assign(surface_count, AfChart{});
        }
    }

    // The movers next, around the surfaces' charts: static density is never reduced for them.
    std::vector<AfMover> movers;
    std::vector<AfChart> mover_charts;
    std::uint32_t mover_density = 0;
    if (props->surface_charts_enabled()) {
        const std::vector<AfMover> all_movers = collect_movers(*level);
        std::uint32_t dm = has_surface ? density : effective_density(props->lightmap_density);
        while (!all_movers.empty()) {
            movers = all_movers;
            derive_mover_charts(movers, dm, mover_charts);
            if (movers.empty()) {
                af_log("no mover surface qualifies for an alpine chart");
                break;
            }
            std::vector<AfChart> fit = surface_charts;
            fit.insert(fit.end(), mover_charts.begin(), mover_charts.end());
            std::vector<Tile> tiles;
            std::uint32_t pages = 0;
            if (pack_charts(fit, tiles, pages) == PackResult::ok) {
                mover_density = dm;
                break;
            }
            if (dm <= density_min) {
                af_warn("the mover charts do not fit the page budget next to the surfaces, movers keep their "
                        "stock lightmaps (fullbright on d3d11-only levels)");
                const std::size_t base = surface_charts.size();
                report_offenders(fit, [&](std::uint32_t c) {
                    if (c < base) {
                        return surface_label(c);
                    }
                    const std::uint32_t rel = c - static_cast<std::uint32_t>(base);
                    for (const AfMover& m : movers) {
                        if (rel >= m.first_chart && rel < m.first_chart + m.num_surfaces) {
                            return "mover " + std::to_string(m.uid) + " surface " + std::to_string(rel - m.first_chart);
                        }
                    }
                    return surface_label(c);
                });
                movers.clear();
                mover_charts.clear();
                break;
            }
            dm = std::max<std::uint32_t>(density_min, dm / 2);
        }
    }

    std::vector<AfTerrain> terrains = collect_terrains(*props);
    if (!has_surface && terrains.empty() && movers.empty()) {
        af_log("no alpine lightmap section will be written");
        return 0;
    }

    std::vector<AfChart> charts;
    std::vector<Tile> tiles;
    std::uint32_t pages = 0;
    while (true) {
        charts = surface_charts;
        std::uint64_t terrain_texels = 0;
        for (AfTerrain& a : terrains) {
            charts.push_back(terrain_chart(a));
            terrain_texels += static_cast<std::uint64_t>(a.wire.w) * a.wire.h;
        }
        charts.insert(charts.end(), mover_charts.begin(), mover_charts.end());
        // the reader's cap on the section's terrain texels
        const PackResult res = terrain_texels > max_terrain_chart_texels ? PackResult::over_budget
                                                                         : pack_charts(charts, tiles, pages);
        if (res == PackResult::ok) {
            break;
        }
        if (res == PackResult::no_charts) {
            return 0;
        }
        if (std::any_of(terrains.begin(), terrains.end(), [](const AfTerrain& a) { return a.density > 1; })) {
            af_warn("the terrain charts do not fit the page budget next to the surface charts, "
                    "halving terrain lightmap density");
            for (AfTerrain& a : terrains) {
                a.density = std::max<std::uint32_t>(1, a.density / 2);
            }
            continue;
        }
        af_error("the terrain charts do not fit the page budget even at one texel per cell, terrain "
                 "lighting is not baked");
        terrains.clear();
        if (!has_surface && movers.empty()) {
            return 0;
        }
    }

    const auto first_mover_chart = static_cast<std::uint32_t>(surface_count + terrains.size());
    for (AfMover& m : movers) {
        m.first_chart += first_mover_chart;
    }
    g_af.solid = solid;
    g_af.base_density = static_cast<std::uint8_t>(density);
    g_af.num_surface_charts = surface_count;
    g_af.has_surface_charts = has_surface;
    g_af.terrains = std::move(terrains);
    g_af.movers = std::move(movers);
    g_af.mover_density = mover_density;
    g_af.charts = std::move(charts);
    g_af.tiles = std::move(tiles);
    g_af.num_pages = pages;

    const auto index_surfaces = [](std::uintptr_t owner, std::uint32_t count, std::uint32_t first_chart) {
        const auto surfaces = solid_surfaces(owner).elems;
        for (std::uint32_t i = 0; i < count; i++) {
            const auto surface = *reinterpret_cast<std::uintptr_t*>(surfaces + i * 4);
            if (surface) {
                g_af.surface_index[surface] = AfSurfaceRef{first_chart + i, owner};
            }
        }
    };
    if (has_surface) {
        index_surfaces(solid, surface_count, 0);
    }
    for (const AfMover& m : g_af.movers) {
        index_surfaces(m.solid, m.num_surfaces, m.first_chart);
        g_af.mover_surfaces += m.num_surfaces;
    }

    std::uint32_t tiled = 0;
    for (const AfChart& c : g_af.charts) {
        if (c.geom.tile_count() > 1) {
            tiled++;
        }
    }
    g_af.active = true;
    af_log("density " + std::to_string(density) + ": " + std::to_string(surface_count) + " surfaces" +
           (has_surface ? "" : " (no charts)") + ", " + std::to_string(g_af.terrains.size()) + " terrains, " +
           std::to_string(g_af.movers.size()) + " movers, " + std::to_string(g_af.mover_surfaces) +
           " mover surfaces at density " + std::to_string(mover_density) + ", " +
           std::to_string(g_af.tiles.size()) + " tiles (" + std::to_string(tiled) +
           " charts tiled), " + std::to_string(pages) + " pages");
    for (const AfTerrain& a : g_af.terrains) {
        af_log(terrain_label(*a.terrain) + ": " + std::to_string(a.wire.w) + "x" + std::to_string(a.wire.h) +
               " texels, " + std::to_string(a.density) + " per cell");
    }
    return pages;
}

void alpine_lm_bake_allocate()
{
    if (!g_af.active) {
        return;
    }
    g_af.pages.resize(g_af.num_pages);
    for (auto& page : g_af.pages) {
        page.assign(static_cast<std::size_t>(P) * P * 3, 0);
    }
}

namespace
{

// Runs the stock per-surface shading against a per-tile view of the surface. lm_w' = k_u * lm_w and
// lm_h' = k_v * lm_h leave uv_scale and uv_add untouched, which is what makes k = 1 arithmetically
// identical to the stock fragment and keeps the faces' stored lightmap UVs (which the out-of-face
// gutter fill addresses texels with) valid for the view.
void shade_tile(std::uintptr_t solid, std::uintptr_t surface, int mode, const AfChart& c,
                std::uint32_t tx, std::uint32_t ty, int lm_w, int lm_h, int xstart, int ystart)
{
    const TileDims td = tile_dims(c.geom, tx, ty);
    if (td.w_t == 0 || td.h_t == 0) {
        return;
    }
    const std::uint32_t idx = tile_index(c.tile_base, c.geom, tx, ty);
    if (idx >= g_af.tiles.size()) {
        return;
    }
    const Tile tile = g_af.tiles[idx];

    // Shade exactly the interior plus its padding. The align4 rounding a stored tile carries on top
    // of that is filled by edge replication below, because widening the rect the engine shades
    // changes what its 3x3 filter and its out-of-face gutter fill see: at k = 1 a chart's core is
    // the stock fragment rect to the texel, and that is what makes the two agree byte for byte.
    const std::uint32_t cw_t = td.iw_t + 2 * c.geom.pad_u;
    const std::uint32_t ch_t = td.ih_t + 2 * c.geom.pad_v;
    const std::uint64_t stride64 = static_cast<std::uint64_t>(c.k_u) * lm_w * 3u;
    const std::uint64_t need64 = (ch_t - 1) * stride64 + cw_t * 3u;
    if (need64 > max_tile_view_bytes) {
        return;
    }
    const std::size_t stride = static_cast<std::size_t>(stride64);
    const std::size_t need = static_cast<std::size_t>(need64);
    if (g_tile_view.size() < need) {
        g_tile_view.assign(need, 0);
    }
    std::memset(g_tile_view.data(), 0, need);

    const ChartCoord origin = tile_origin_chart(c.geom, tx, ty);
    const std::int64_t view_x = stock_view_coord(c.k_u, xstart, origin.u);
    const std::int64_t view_y = stock_view_coord(c.k_v, ystart, origin.v);

    EngineLightmap view{};
    view.w = static_cast<int>(c.k_u) * lm_w;
    view.h = static_cast<int>(c.k_v) * lm_h;
    // The engine addresses ((view_y + row) * view.w + view_x + col) * 3 off this pointer, so biasing
    // it by the rect origin maps that window onto a buffer that only covers the tile. The bias is
    // deliberate 32 bit wraparound; every address the engine then forms is still exact.
    view.pixels = reinterpret_cast<std::uint8_t*>(
        reinterpret_cast<std::uintptr_t>(g_tile_view.data()) -
        static_cast<std::uintptr_t>(
            static_cast<std::int64_t>(view_y) * view.w * 3 + view_x * 3));
    view.bm_handle = -1;
    view.index = -1;

    {
        // The surface goes back to describing its stock fragment however this scope is left
        struct Restore
        {
            std::uintptr_t surface;
            std::uintptr_t lm;
            int x, y, w, h;
            std::uint8_t state;
            ~Restore()
            {
                *reinterpret_cast<std::uintptr_t*>(surface + 0xc) = lm;
                *reinterpret_cast<int*>(surface + 0x10) = x;
                *reinterpret_cast<int*>(surface + 0x14) = y;
                *reinterpret_cast<int*>(surface + 0x18) = w;
                *reinterpret_cast<int*>(surface + 0x1c) = h;
                *reinterpret_cast<std::uint8_t*>(surface + 8) = state;
                g_tile_pass = false;
            }
        } restore{surface,
                  surf_lm(surface),
                  surf_i(surface, 0x10),
                  surf_i(surface, 0x14),
                  surf_i(surface, 0x18),
                  surf_i(surface, 0x1c),
                  *reinterpret_cast<std::uint8_t*>(surface + 8)};

        *reinterpret_cast<std::uintptr_t*>(surface + 0xc) = reinterpret_cast<std::uintptr_t>(&view);
        *reinterpret_cast<int*>(surface + 0x10) = static_cast<int>(view_x);
        *reinterpret_cast<int*>(surface + 0x14) = static_cast<int>(view_y);
        *reinterpret_cast<int*>(surface + 0x18) = static_cast<int>(cw_t);
        *reinterpret_cast<int*>(surface + 0x1c) = static_cast<int>(ch_t);
        *reinterpret_cast<std::uint8_t*>(surface + 8) = 7;

        g_tile_pass = true;
        AddrCaller{0x004ac470}.this_call(reinterpret_cast<void*>(surface),
                                         reinterpret_cast<void*>(solid), mode);
    }

    for (std::uint32_t row = 0; row < td.h_t; row++) {
        std::uint8_t* dst = page_texel(tile.page, tile.x, tile.y + row);
        if (!dst) {
            break;
        }
        const std::uint32_t src_row = std::min(row, ch_t - 1);
        const std::uint32_t copy_w = std::min<std::uint32_t>(cw_t, P - tile.x);
        std::memcpy(dst, g_tile_view.data() + src_row * stride, copy_w * 3u);
        for (std::uint32_t col = copy_w; col < td.w_t && tile.x + col < P; col++) {
            std::memcpy(dst + col * 3u, dst + (copy_w - 1) * 3u, 3);
        }
    }
}

// Every stock interior texel becomes the box mean of the k_u x k_v chart texels covering it, so the
// fallback stays inside the range its footprint spans and keeps its mean to within an LSB. At k = 1
// this copies back bytes the chart already holds a bit-identical copy of.
void downsample_into_stock(std::uintptr_t surface, const AfChart& c, int lm_w, int xstart,
                           int ystart, std::uint8_t* page)
{
    const int w = surf_i(surface, 0x18);
    const int h = surf_i(surface, 0x1c);
    const std::uint32_t n = static_cast<std::uint32_t>(c.k_u) * c.k_v;
    for (int j = 0; j + 2 < h; j++) {
        for (int i = 0; i + 2 < w; i++) {
            std::uint32_t sum[3] = {0, 0, 0};
            std::uint32_t taken = 0;
            for (std::uint32_t dv = 0; dv < c.k_v; dv++) {
                for (std::uint32_t du = 0; du < c.k_u; du++) {
                    const std::uint8_t* src =
                        chart_texel(c, stock_texel_chart_coord(static_cast<std::uint32_t>(i), c.k_u, du),
                                    stock_texel_chart_coord(static_cast<std::uint32_t>(j), c.k_v, dv));
                    if (!src) {
                        continue;
                    }
                    sum[0] += src[0];
                    sum[1] += src[1];
                    sum[2] += src[2];
                    taken++;
                }
            }
            if (taken == 0 || taken != n) {
                continue;
            }
            std::uint8_t* dst =
                page + ((static_cast<std::size_t>(ystart + 1 + j) * lm_w) + xstart + 1 + i) * 3;
            for (int ch = 0; ch < 3; ch++) {
                dst[ch] = static_cast<std::uint8_t>((sum[ch] + taken / 2) / taken);
            }
        }
    }
}

// At k = 1 a single-tile chart's shaded rect is the stock fragment rect to the texel (shade_tile), so
// the tile can take the fragment the stock pass just shaded instead of shading it again.
bool chart_is_stock_fragment(const AfChart& c)
{
    return c.k_u == 1 && c.k_v == 1 && c.geom.nx == 1 && c.geom.ny == 1;
}

// Stores the fragment as shade_tile stores its view.
void store_fragment_as_tile(const AfChart& c, int lm_w, int xstart, int ystart, const std::uint8_t* page)
{
    const std::uint32_t idx = tile_index(c.tile_base, c.geom, 0, 0);
    if (idx >= g_af.tiles.size()) {
        return;
    }
    const Tile tile = g_af.tiles[idx];
    const TileDims td = tile_dims(c.geom, 0, 0);
    const std::uint32_t cw_t = td.iw_t + 2 * c.geom.pad_u;
    const std::uint32_t ch_t = td.ih_t + 2 * c.geom.pad_v;
    const std::uint32_t copy_w = std::min<std::uint32_t>(cw_t, P - tile.x);
    for (std::uint32_t row = 0; row < td.h_t; row++) {
        std::uint8_t* dst = page_texel(tile.page, tile.x, tile.y + row);
        if (!dst) {
            break;
        }
        const std::uint32_t src_row = std::min(row, ch_t - 1);
        const std::uint8_t* src = page + ((static_cast<std::size_t>(ystart) + src_row) * lm_w + xstart) * 3;
        for (std::uint32_t col = 0; col < td.w_t && tile.x + col < P; col++) {
            std::memcpy(dst + col * 3u, src + std::min(col, copy_w - 1) * 3u, 3);
        }
    }
}

// The remaining surfaces skip the alpine bake and alpine_lm_bake_end drops it.
void af_bake_failed(const char* during) noexcept
{
    g_af.active = false;
    try {
        af_error(std::string{"out of memory "} + during + ", the alpine lightmap section is dropped");
    }
    catch (...) {
    }
}

} // namespace

void alpine_lm_shade_surface(std::uintptr_t solid, std::uintptr_t surface, int mode)
{
    if (!g_af.active || g_tile_pass) {
        return;
    }
    auto it = g_af.surface_index.find(surface);
    if (it == g_af.surface_index.end() || it->second.solid != solid || it->second.chart >= g_af.charts.size()) {
        return;
    }
    const AfChart c = g_af.charts[it->second.chart];
    if (c.geom.empty()) {
        return;
    }
    const std::uintptr_t lm = surf_lm(surface);
    if (!lm) {
        return;
    }
    const int lm_w = *reinterpret_cast<int*>(lm + 4);
    const int lm_h = *reinterpret_cast<int*>(lm + 8);
    auto* page = *reinterpret_cast<std::uint8_t**>(lm + 0xc);
    const int xstart = surf_i(surface, 0x10);
    const int ystart = surf_i(surface, 0x14);
    if (lm_w <= 0 || lm_h <= 0 || !page) {
        return;
    }

    // At k = 1 downsample_into_stock would only copy the same bytes back.
    if (chart_is_stock_fragment(c)) {
        store_fragment_as_tile(c, lm_w, xstart, ystart, page);
        return;
    }

    // Nothing may unwind into the engine's lighting frames: out of memory drops the alpine half of the
    // bake, the stock bake goes on.
    try {
        for (std::uint32_t ty = 0; ty < c.geom.ny; ty++) {
            for (std::uint32_t tx = 0; tx < c.geom.nx; tx++) {
                shade_tile(solid, surface, mode, c, tx, ty, lm_w, lm_h, xstart, ystart);
            }
        }
        downsample_into_stock(surface, c, lm_w, xstart, ystart, page);
    }
    catch (...) {
        af_bake_failed("shading the surface charts");
    }
}

// ─── cross-surface blend at chart resolution ──────────────────────────────────

namespace
{

constexpr float af_blend_own = 9.0f / 16.0f;
constexpr float af_blend_other = 7.0f / 16.0f;
constexpr float af_blend_samples_per_texel = 128.0f;
constexpr int af_blend_max_steps = 1 << 18;

struct AfBlendSide
{
    const AfChart* chart = nullptr;
    float lm_w = 0.0f, lm_h = 0.0f;
    float scale_x = 0.0f, scale_y = 0.0f, add_x = 0.0f, add_y = 0.0f;
    int u_coeff = 0, v_coeff = 0;
    int xstart = 0, ystart = 0;
    struct Accum
    {
        float sum[3];
        int count;
    };
    std::unordered_map<std::uint32_t, Accum> acc;
};

bool af_blend_init(AfBlendSide& s, std::uintptr_t surface)
{
    s.acc.clear();
    s.chart = nullptr;
    auto it = g_af.surface_index.find(surface);
    if (it == g_af.surface_index.end() || it->second.chart >= g_af.charts.size()) {
        return false;
    }
    const AfChart& c = g_af.charts[it->second.chart];
    if (c.geom.empty()) {
        return false;
    }
    const std::uintptr_t lm = surf_lm(surface);
    if (!lm) {
        return false;
    }
    s.scale_x = surf_f(surface, 0x4c);
    s.scale_y = surf_f(surface, 0x50);
    if (s.scale_x == 0.0f || s.scale_y == 0.0f) {
        return false;
    }
    s.chart = &c;
    s.lm_w = static_cast<float>(*reinterpret_cast<int*>(lm + 4));
    s.lm_h = static_cast<float>(*reinterpret_cast<int*>(lm + 8));
    s.add_x = surf_f(surface, 0x54);
    s.add_y = surf_f(surface, 0x58);
    s.u_coeff = surf_i(surface, 0x60);
    s.v_coeff = surf_i(surface, 0x64);
    if (s.u_coeff < 0 || s.u_coeff > 2 || s.v_coeff < 0 || s.v_coeff > 2) {
        return false;
    }
    s.xstart = surf_i(surface, 0x10);
    s.ystart = surf_i(surface, 0x14);
    return true;
}

// World position -> chart texel, the projection the game places the surface's vertices with.
ChartTexel af_world_to_chart(const AfBlendSide& s, const float* p)
{
    return surface_chart_texel(p, s.u_coeff, s.v_coeff, s.scale_x, s.scale_y, s.add_x, s.add_y,
                               static_cast<std::uint32_t>(s.lm_w), static_cast<std::uint32_t>(s.lm_h),
                               static_cast<std::uint32_t>(s.xstart), static_cast<std::uint32_t>(s.ystart),
                               s.chart->k_u, s.chart->k_v);
}

int af_blend_index(const AfBlendSide& s, float u, float v)
{
    int cu = static_cast<int>(std::floor(u));
    int cv = static_cast<int>(std::floor(v));
    cu = std::min(std::max(cu, 0), static_cast<int>(s.chart->geom.cw) - 1);
    cv = std::min(std::max(cv, 0), static_cast<int>(s.chart->geom.ch) - 1);
    return cv * static_cast<int>(s.chart->geom.cw) + cu;
}

void af_blend_sample(const AfBlendSide& s, float u, float v, float* out)
{
    const float fx = u - 0.5f;
    const float fy = v - 0.5f;
    const int x0 = static_cast<int>(std::floor(fx));
    const int y0 = static_cast<int>(std::floor(fy));
    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);
    out[0] = out[1] = out[2] = 0.0f;
    // The stock blend clamps its taps to the fragment INCLUDING the one texel border ring
    // (lightmap.cpp blend_side_sample), so the chart clamps one texel past its own edge too;
    // clamping to the chart interior instead is what used to make k = 1 disagree with stock.
    for (int dy = 0; dy < 2; dy++) {
        const int yy = std::min(std::max(y0 + dy, -1), static_cast<int>(s.chart->geom.ch));
        const float wy = dy ? ty : 1.0f - ty;
        for (int dx = 0; dx < 2; dx++) {
            const int xx = std::min(std::max(x0 + dx, -1), static_cast<int>(s.chart->geom.cw));
            const float wgt = wy * (dx ? tx : 1.0f - tx);
            const std::uint8_t* px = chart_texel_padded(*s.chart, xx, yy);
            if (!px) {
                continue;
            }
            out[0] += px[0] * wgt;
            out[1] += px[1] * wgt;
            out[2] += px[2] * wgt;
        }
    }
}

void af_blend_add(AfBlendSide& s, int index, const float* other)
{
    auto& a = s.acc[static_cast<std::uint32_t>(index)];
    a.sum[0] += other[0];
    a.sum[1] += other[1];
    a.sum[2] += other[2];
    a.count++;
}

void af_blend_apply(AfBlendSide& s)
{
    for (const auto& e : s.acc) {
        std::uint8_t* dst = chart_texel(*s.chart, e.first % s.chart->geom.cw,
                                        e.first / s.chart->geom.cw);
        if (!dst || e.second.count == 0) {
            continue;
        }
        const float inv = 1.0f / static_cast<float>(e.second.count);
        for (int ch = 0; ch < 3; ch++) {
            const int val = static_cast<int>(static_cast<float>(dst[ch]) * af_blend_own +
                                             e.second.sum[ch] * inv * af_blend_other + 0.5f);
            dst[ch] = static_cast<std::uint8_t>(std::min(std::max(val, 0), 255));
        }
    }
    s.acc.clear();
}

void af_blend_edge(std::uintptr_t surf_a, std::uintptr_t surf_b, const float* p0, const float* p1)
{
    // the blend pass is serial, so the scratch accumulators are kept across edges
    static AfBlendSide a, b;
    if (!af_blend_init(a, surf_a) || !af_blend_init(b, surf_b)) {
        return;
    }
    const ChartTexel a0 = af_world_to_chart(a, p0);
    const ChartTexel a1 = af_world_to_chart(a, p1);
    const ChartTexel b0 = af_world_to_chart(b, p0);
    const ChartTexel b1 = af_world_to_chart(b, p1);
    const float ext = std::max(std::max(std::abs(a1.u - a0.u), std::abs(a1.v - a0.v)),
                               std::max(std::abs(b1.u - b0.u), std::abs(b1.v - b0.v)));
    if (!(ext >= 0.0f) || ext > 1.0e7f) {
        return;
    }
    int steps = static_cast<int>(std::min(ext * af_blend_samples_per_texel, 1.0e7f)) + 1;
    steps = std::min(std::max(steps, 256), af_blend_max_steps);
    for (int k = 0; k <= steps; k++) {
        const float t = static_cast<float>(k) / static_cast<float>(steps);
        const float au = a0.u + (a1.u - a0.u) * t;
        const float av = a0.v + (a1.v - a0.v) * t;
        const float bu = b0.u + (b1.u - b0.u) * t;
        const float bv = b0.v + (b1.v - b0.v) * t;
        float sa[3], sb[3];
        af_blend_sample(a, au, av, sa);
        af_blend_sample(b, bu, bv, sb);
        af_blend_add(a, af_blend_index(a, au, av), sb);
        af_blend_add(b, af_blend_index(b, bu, bv), sa);
    }
    af_blend_apply(a);
    af_blend_apply(b);
}

} // namespace

void alpine_lm_blend_edge(std::uintptr_t surf_a, std::uintptr_t surf_b, const float* p0,
                          const float* p1)
{
    if (!g_af.active || g_tile_pass || !p0 || !p1) {
        return;
    }
    try {
        af_blend_edge(surf_a, surf_b, p0, p1);
    }
    catch (...) {
        af_bake_failed("blending the surface charts");
    }
}

// ─── gutter reconciliation, encode and serialisation ──────────────────────────

namespace
{

// Blocks holding a texel reconciled with a sibling tile; RDO leaves them alone so seams stay exact.
std::vector<std::uint8_t> g_shared_blocks;

void mark_shared_block(std::uint32_t page, std::uint32_t x, std::uint32_t y)
{
    constexpr std::uint32_t bpr = P / 4;
    const std::size_t i = (static_cast<std::size_t>(page) * bpr + y / 4) * bpr + x / 4;
    if (i < g_shared_blocks.size()) {
        g_shared_blocks[i] = 1;
    }
}

// Off-chart texels copy the nearest in-chart texel, like the stock border ring (FUN_004abad0).
std::uint64_t replicate_chart_edges()
{
    std::uint64_t copied = 0;
    for (const AfChart& c : g_af.charts) {
        if (c.geom.empty()) {
            continue;
        }
        const auto clamp_chart = [](std::int64_t v, std::int64_t hi) {
            return v < 0 ? std::int64_t{0} : (v > hi ? hi : v);
        };
        for (std::uint32_t ty = 0; ty < c.geom.ny; ty++) {
            for (std::uint32_t tx = 0; tx < c.geom.nx; tx++) {
                const std::uint32_t idx = tile_index(c.tile_base, c.geom, tx, ty);
                if (idx >= g_af.tiles.size()) {
                    continue;
                }
                const Tile& t = g_af.tiles[idx];
                const TileDims td = tile_dims(c.geom, tx, ty);
                const ChartCoord origin = tile_origin_chart(c.geom, tx, ty);
                for (std::uint32_t sy = 0; sy < td.h_t; sy++) {
                    const std::int64_t cv = origin.v + sy;
                    const bool row_in = cv >= 0 && cv < c.geom.ch;
                    for (std::uint32_t sx = 0; sx < td.w_t; sx++) {
                        const std::int64_t cu = origin.u + sx;
                        if (row_in && cu >= 0 && cu < c.geom.cw) {
                            continue;
                        }
                        const std::uint8_t* src =
                            chart_texel_padded(c, clamp_chart(cu, c.geom.cw - 1),
                                               clamp_chart(cv, c.geom.ch - 1));
                        std::uint8_t* dst = page_texel(t.page, t.x + sx, t.y + sy);
                        if (!src || !dst) {
                            continue;
                        }
                        dst[0] = src[0];
                        dst[1] = src[1];
                        dst[2] = src[2];
                        copied++;
                    }
                }
            }
        }
    }
    return copied;
}

// Texels several tiles store become byte copies of the owning tile's value (owner by clamped chart coordinate).
std::uint64_t reconcile_gutters()
{
    g_shared_blocks.assign(static_cast<std::size_t>(bc7_block_count(g_af.num_pages)), 0);
    std::uint64_t copied = 0;
    for (const AfChart& c : g_af.charts) {
        if (c.geom.empty() || c.geom.tile_count() <= 1) {
            continue;
        }
        for (std::uint32_t ty = 0; ty < c.geom.ny; ty++) {
            for (std::uint32_t tx = 0; tx < c.geom.nx; tx++) {
                const std::uint32_t idx = tile_index(c.tile_base, c.geom, tx, ty);
                if (idx >= g_af.tiles.size()) {
                    continue;
                }
                const Tile& t = g_af.tiles[idx];
                const TileDims td = tile_dims(c.geom, tx, ty);
                const ChartCoord origin = tile_origin_chart(c.geom, tx, ty);
                for (std::uint32_t sy = 0; sy < td.h_t; sy++) {
                    const std::int64_t cv = origin.v + sy;
                    for (std::uint32_t sx = 0; sx < td.w_t; sx++) {
                        const std::int64_t cu = origin.u + sx;
                        const TileSlot owner = chart_tile_slot(c.geom, c.tile_base, cu, cv);
                        if (owner.index == idx || !owner.valid ||
                            owner.index >= g_af.tiles.size()) {
                            continue;
                        }
                        const Tile& ot = g_af.tiles[owner.index];
                        const std::uint8_t* src =
                            page_texel(ot.page, ot.x + static_cast<std::uint32_t>(owner.sx),
                                       ot.y + static_cast<std::uint32_t>(owner.sy));
                        std::uint8_t* dst = page_texel(t.page, t.x + sx, t.y + sy);
                        if (!src || !dst) {
                            continue;
                        }
                        dst[0] = src[0];
                        dst[1] = src[1];
                        dst[2] = src[2];
                        mark_shared_block(t.page, t.x + sx, t.y + sy);
                        mark_shared_block(ot.page, ot.x + static_cast<std::uint32_t>(owner.sx),
                                          ot.y + static_cast<std::uint32_t>(owner.sy));
                        copied++;
                    }
                }
            }
        }
    }
    return copied;
}

// Shadow rays leave a terrain texel this far along its normal: a ray lift like a face's, plus a share
// of the texel, so a slope's own facets cannot catch the ray of a texel centre next to their edge.
float terrain_ray_lift(float texel_size)
{
    return 0.02f + 0.05f * texel_size;
}

constexpr std::uint32_t terrain_block = 64;

// Lights every texel of terrain chart `c` at its world position on the heightfield and heightmap
// normal, converts the chart as the fixed pipeline converts a fragment, and stores it into every tile
// texel that falls inside the chart; replicate_chart_edges fills the padding past it.
bool shade_terrain_chart(const AfTerrain& a, const AfChart& c)
{
    const DedTerrain& t = *a.terrain;
    const at::GridView g = terrain_grid_view(t.pos, t.data, *t.data.grid);
    const std::uint32_t w = c.geom.cw;
    const std::uint32_t h = c.geom.ch;
    const float ts = a.wire.texel_size;
    const float lift = terrain_ray_lift(ts);
    const std::size_t texels = static_cast<std::size_t>(w) * h;
    std::vector<float> r(texels), gr(texels), b(texels);
    std::vector<LightmapPoint> pts;
    std::vector<float> br, bg, bb;
    for (std::uint32_t by = 0; by < h; by += terrain_block) {
        for (std::uint32_t bx = 0; bx < w; bx += terrain_block) {
            const std::uint32_t bw = std::min(terrain_block, w - bx);
            const std::uint32_t bh = std::min(terrain_block, h - by);
            pts.resize(static_cast<std::size_t>(bw) * bh);
            for (std::uint32_t j = 0; j < bh; j++) {
                for (std::uint32_t i = 0; i < bw; i++) {
                    LightmapPoint& p = pts[static_cast<std::size_t>(j) * bw + i];
                    p.pos[0] = terrain_texel_center(a.wire.origin_x, ts, bx + i);
                    p.pos[2] = terrain_texel_center(a.wire.origin_z, ts, by + j);
                    p.pos[1] = at::height_at(g, p.pos[0], p.pos[2]);
                    at::heightmap_normal(g, p.pos[0], p.pos[2], p.normal);
                }
            }
            br.resize(pts.size());
            bg.resize(pts.size());
            bb.resize(pts.size());
            if (!lightmap_light_terrain_points(pts.data(), static_cast<int>(pts.size()), lift, br.data(), bg.data(),
                                               bb.data())) {
                return false;
            }
            for (std::uint32_t j = 0; j < bh; j++) {
                for (std::uint32_t i = 0; i < bw; i++) {
                    const std::size_t src = static_cast<std::size_t>(j) * bw + i;
                    const std::size_t dst = static_cast<std::size_t>(by + j) * w + bx + i;
                    r[dst] = br[src];
                    gr[dst] = bg[src];
                    b[dst] = bb[src];
                }
            }
        }
    }
    std::vector<std::uint8_t> bytes(texels * 3);
    lightmap_encode_float_texels(r.data(), gr.data(), b.data(), static_cast<int>(w), static_cast<int>(h),
                                 bytes.data());

    for (std::uint32_t ty = 0; ty < c.geom.ny; ty++) {
        for (std::uint32_t tx = 0; tx < c.geom.nx; tx++) {
            const std::uint32_t idx = tile_index(c.tile_base, c.geom, tx, ty);
            if (idx >= g_af.tiles.size()) {
                continue;
            }
            const Tile& tile = g_af.tiles[idx];
            const TileDims td = tile_dims(c.geom, tx, ty);
            const ChartCoord origin = tile_origin_chart(c.geom, tx, ty);
            for (std::uint32_t sy = 0; sy < td.h_t; sy++) {
                const std::int64_t cv = origin.v + sy;
                for (std::uint32_t sx = 0; sx < td.w_t; sx++) {
                    const std::int64_t cu = origin.u + sx;
                    if (cu < 0 || cv < 0 || cu >= w || cv >= h) {
                        continue;
                    }
                    std::uint8_t* dst = page_texel(tile.page, tile.x + sx, tile.y + sy);
                    if (dst) {
                        std::memcpy(dst, &bytes[(static_cast<std::size_t>(cv) * w + static_cast<std::size_t>(cu)) * 3], 3);
                    }
                }
            }
        }
    }
    return true;
}

void shade_terrain_charts()
{
    if (g_af.terrains.empty()) {
        return;
    }
    lightmap_prepare_terrain_bake();
    for (std::size_t k = 0; k < g_af.terrains.size(); k++) {
        AfTerrain& a = g_af.terrains[k];
        const std::size_t ci = g_af.num_surface_charts + k;
        const DWORD t0 = GetTickCount();
        bool ok = false;
        try {
            ok = ci < g_af.charts.size() && shade_terrain_chart(a, g_af.charts[ci]);
        }
        catch (const std::bad_alloc&) {
        }
        if (!ok) {
            // a fingerprint no terrain hashes to, so the game keeps its placeholder lighting
            a.wire.geometry_fingerprint = 0;
            af_error(terrain_label(*a.terrain) + " could not be lit, it is saved without baked lighting");
            continue;
        }
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s: lit %ux%u texels in %.1fs", terrain_label(*a.terrain).c_str(),
                      static_cast<unsigned>(a.wire.w), static_cast<unsigned>(a.wire.h),
                      (GetTickCount() - t0) / 1000.0);
        af_log(buf);
    }
}

bool unpack_bc7_cb(const void* block, ert::color_rgba* pixels, uint32_t, void*)
{
    return unpack_bc7_rgba(block, reinterpret_cast<std::uint8_t*>(pixels));
}

void fill_block_pixels(std::uint32_t block, ert::color_rgba* out)
{
    constexpr std::uint32_t blocks_per_row = P / 4;
    constexpr std::uint32_t blocks_per_page = blocks_per_row * blocks_per_row;
    const std::uint32_t page = block / blocks_per_page;
    const std::uint32_t within = block % blocks_per_page;
    const std::uint32_t bx = (within % blocks_per_row) * 4;
    const std::uint32_t by = (within / blocks_per_row) * 4;
    for (std::uint32_t y = 0; y < 4; y++) {
        const std::uint8_t* row = g_af.pages[page].data() + (static_cast<std::size_t>(by + y) * P + bx) * 3;
        for (std::uint32_t x = 0; x < 4; x++) {
            ert::color_rgba& c = out[y * 4 + x];
            c.m_c[0] = row[x * 3];
            c.m_c[1] = row[x * 3 + 1];
            c.m_c[2] = row[x * 3 + 2];
            c.m_c[3] = 255;
        }
    }
}

// The pages' BC7 blocks, one buffer per page.
PageBuffers encode_bc7(const EncoderSettings& s, std::uint32_t& out_modified)
{
    constexpr std::uint32_t page_blocks = (P / 4) * (P / 4);
    const std::uint32_t total = static_cast<std::uint32_t>(bc7_block_count(g_af.num_pages));
    PageBuffers blocks(g_af.num_pages);
    for (auto& page : blocks) {
        page.resize(static_cast<std::size_t>(page_blocks) * BC7ENC_BLOCK_SIZE);
    }
    const auto block_at = [&blocks](std::uint32_t b) {
        return blocks[b / page_blocks].data() + static_cast<std::size_t>(b % page_blocks) * BC7ENC_BLOCK_SIZE;
    };
    out_modified = 0;
    if (total == 0) {
        return blocks;
    }

    bc7enc_compress_block_params bp;
    bc7enc_compress_block_params_init(&bp);
    bc7enc_compress_block_params_init_linear_weights(&bp);
    bp.m_uber_level = s.bc7_uber_level;
    bp.m_max_partitions = s.bc7_max_partitions;
    bp.m_perceptual = s.bc7_perceptual;
    bp.m_try_least_squares = s.bc7_try_least_squares;
    bp.m_mode17_partition_estimation_filterbank = s.bc7_mode17_partition_estimation_filterbank;
    bp.m_quant_mode6_endpoints = s.bc7_quant_mode6_endpoints;
    bp.m_pbit1_weight = s.bc7_pbit1_weight;
    bp.m_mode6_error_weight = s.bc7_mode6_error_weight;
    bp.m_low_frequency_partition_weight = s.bc7_low_frequency_partition_weight;

    auto run = [&](std::uint32_t first, std::uint32_t last) {
        ert::color_rgba pixels[16];
        for (std::uint32_t b = first; b < last; b++) {
            fill_block_pixels(b, pixels);
            bc7enc_compress_block(block_at(b), pixels, &bp);
        }
    };

    // Fixed contiguous ranges, never work stealing: the encoder is an RNG free exhaustive search, so
    // identical pixels and identical ranges give identical bytes whatever the thread count.
    unsigned workers = std::thread::hardware_concurrency();
    workers = std::max(1u, std::min(workers, 32u));
    const std::uint32_t chunk = (total + workers - 1) / workers;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges;
    for (std::uint32_t start = 0; start < total; start += chunk) {
        ranges.emplace_back(start, std::min(total, start + chunk));
    }
    work_pool_run(static_cast<int>(ranges.size()), [&](int i) {
        const auto r = ranges[static_cast<std::size_t>(i)];
        run(r.first, r.second);
    });

    if (s.rdo_lambda > 0.0f) {
        ert::reduce_entropy_params ep;
        ep.m_lambda = s.rdo_lambda;
        ep.m_lookback_window_size = s.rdo_lookback_window_size;
        ep.m_max_allowed_rms_increase_ratio = s.rdo_max_allowed_rms_increase_ratio;
        ep.m_max_smooth_block_std_dev = s.rdo_max_smooth_block_std_dev;
        ep.m_smooth_block_max_mse_scale = s.rdo_smooth_block_max_mse_scale;
        ep.m_try_two_matches = s.rdo_try_two_matches;
        ep.m_allow_relative_movement = s.rdo_allow_relative_movement;
        ep.m_skip_zero_mse_blocks = s.rdo_skip_zero_mse_blocks;
        // Three components: ert refuses outright (returns false, having done nothing) unless the
        // weights of the components it is not told about are zero.
        ep.m_color_weights[3] = 0;

        // the RDO pass must leave these blocks as encoded
        std::vector<std::uint32_t> kept;
        std::vector<std::uint8_t> kept_bytes;
        for (std::uint32_t b = 0; b < total && b < g_shared_blocks.size(); b++) {
            if (g_shared_blocks[b]) {
                kept.push_back(b);
                kept_bytes.insert(kept_bytes.end(), block_at(b), block_at(b) + BC7ENC_BLOCK_SIZE);
            }
        }
        // blocks [first, first + n) laid out contiguously at `data`
        const auto rdo = [&](std::uint8_t* data, std::uint32_t first, std::uint32_t n, std::uint32_t& modified) {
            std::vector<ert::color_rgba> pixels(static_cast<std::size_t>(n) * 16);
            for (std::uint32_t b = 0; b < n; b++) {
                fill_block_pixels(first + b, pixels.data() + static_cast<std::size_t>(b) * 16);
            }
            ert::reduce_entropy(data, n, BC7ENC_BLOCK_SIZE, BC7ENC_BLOCK_SIZE, 4, 4, 3, pixels.data(), ep, modified,
                                unpack_bc7_cb, nullptr);
        };
        // One page per task on fixed block ranges, so the output is deterministic; no match crosses a page.
        const int pages = static_cast<int>(blocks.size());
        std::vector<std::uint32_t> modified(static_cast<std::size_t>(pages), 0);
        work_pool_run(pages, [&](int page) {
            const std::uint32_t first = static_cast<std::uint32_t>(page) * page_blocks;
            rdo(blocks[static_cast<std::size_t>(page)].data(), first, std::min(page_blocks, total - first),
                modified[static_cast<std::size_t>(page)]);
        });
        for (const std::uint32_t m : modified) {
            out_modified += m;
        }
        for (std::size_t k = 0; k < kept.size(); k++) {
            std::memcpy(block_at(kept[k]), kept_bytes.data() + k * BC7ENC_BLOCK_SIZE, BC7ENC_BLOCK_SIZE);
        }
        const auto restored = static_cast<std::uint32_t>(kept.size());
        if (restored) {
            af_log("kept " + std::to_string(restored) + " of " + std::to_string(total) +
                   " blocks out of the RDO pass, they carry a reconciled tile gutter");
        }
    }
    return blocks;
}

// The pages as one zlib stream, the same bytes compress2 makes of them laid end to end, in chunks of
// at most 1 MB. False when zlib fails.
bool deflate_pages(const PageBuffers& pages, PageBuffers& out, std::size_t& out_size)
{
    constexpr std::size_t chunk = 1u << 20;
    out.clear();
    out_size = 0;
    z_stream zs{};
    if (deflateInit(&zs, Z_DEFAULT_COMPRESSION) != Z_OK) {
        return false;
    }
    ScopeGuard end{[&zs] { deflateEnd(&zs); }};
    std::vector<std::uint8_t> buf(chunk);
    zs.next_out = buf.data();
    zs.avail_out = static_cast<uInt>(chunk);
    bool ok = true;
    for (std::size_t i = 0; ok && i <= pages.size(); i++) {
        const bool finish = i == pages.size();
        zs.next_in = finish ? nullptr : const_cast<Bytef*>(pages[i].data());
        zs.avail_in = finish ? 0u : static_cast<uInt>(pages[i].size());
        while (true) {
            const int zr = deflate(&zs, finish ? Z_FINISH : Z_NO_FLUSH);
            if (zr == Z_STREAM_ERROR || (finish && zr == Z_BUF_ERROR && zs.avail_out != 0)) {
                ok = false;
                break;
            }
            if (finish && zr == Z_STREAM_END) {
                break;
            }
            if (zs.avail_out == 0) {
                out.push_back(std::move(buf));
                buf.assign(chunk, 0);
                zs.next_out = buf.data();
                zs.avail_out = static_cast<uInt>(chunk);
                continue;
            }
            if (!finish && zs.avail_in == 0) {
                break;
            }
        }
    }
    if (ok) {
        buf.resize(chunk - zs.avail_out);
        out.push_back(std::move(buf));
        out_size = zs.total_out;
    }
    return ok;
}

void build_body(const PageBuffers& raw, Codec codec)
{
    const std::size_t raw_size = page_buffers_size(raw);
    PageBuffers payload;
    std::size_t payload_size = 0;
    std::uint8_t compression = static_cast<std::uint8_t>(Compression::zlib);
    if (!deflate_pages(raw, payload, payload_size)) {
        payload.clear();
        payload_size = raw_size;
        compression = static_cast<std::uint8_t>(Compression::none);
        af_warn("zlib failed, storing the layer uncompressed");
    }
    const PageBuffers& stored = compression == static_cast<std::uint8_t>(Compression::none) ? raw : payload;

    SectionHeader head{};
    head.version = section_version;
    head.flags = 0;
    head.page_size = static_cast<std::uint16_t>(P);
    head.tile_step = static_cast<std::uint16_t>(S);
    head.gutter = static_cast<std::uint8_t>(G);
    head.base_density = g_af.base_density;
    head.num_pages = static_cast<std::uint16_t>(g_af.num_pages);
    head.num_charts = g_af.num_surface_charts;
    head.num_tiles = static_cast<std::uint32_t>(g_af.tiles.size());

    LayerDirHeader dir{};
    dir.num_layers = 1;

    LayerDirEntry layer{};
    layer.semantic = static_cast<std::uint16_t>(Semantic::radiance_ldr);
    layer.codec = static_cast<std::uint16_t>(codec);
    layer.layer_version = layer_version;
    layer.colorspace = static_cast<std::uint8_t>(Colorspace::rf_lightmap_x2);
    layer.compression = compression;
    layer.uncompressed_size = static_cast<std::uint32_t>(raw_size);
    layer.stored_size = static_cast<std::uint32_t>(payload_size);

    std::uint32_t mover_tiles = 0;
    for (const AfMover& m : g_af.movers) {
        for (std::uint32_t s = 0; s < m.num_surfaces; s++) {
            mover_tiles += g_af.charts[m.first_chart + s].geom.tile_count();
        }
    }
    const std::uint64_t mover_bytes =
        g_af.movers.empty() ? 0 : mover_table_bytes(g_af.movers.size(), g_af.mover_surfaces);

    auto& body = g_af.body;
    body.clear();
    body.reserve(sizeof(head) + g_af.charts.size() * sizeof(Chart) + sizeof(std::uint32_t) +
                 g_af.terrains.size() * sizeof(TerrainChart) + sizeof(std::uint32_t) +
                 (g_af.movers.empty() ? 0 : sizeof(TableHeader) + static_cast<std::size_t>(mover_bytes)) +
                 g_af.tiles.size() * sizeof(Tile) +
                 sizeof(dir) + sizeof(layer) + payload_size);
    auto append = [&body](const void* p, std::size_t n) {
        const auto* b = static_cast<const std::uint8_t*>(p);
        body.insert(body.end(), b, b + n);
    };
    append(&head, sizeof(head));
    for (std::uint32_t i = 0; i < g_af.num_surface_charts; i++) {
        const AfChart& c = g_af.charts[i];
        const Chart wire{c.geom.empty() ? std::uint16_t{0} : c.k_u,
                         c.geom.empty() ? std::uint16_t{0} : c.k_v};
        append(&wire, sizeof(wire));
    }
    const auto num_terrain = static_cast<std::uint32_t>(g_af.terrains.size());
    append(&num_terrain, sizeof(num_terrain));
    for (const AfTerrain& a : g_af.terrains) {
        append(&a.wire, sizeof(a.wire));
    }
    const std::uint32_t num_tables = g_af.movers.empty() ? 0 : 1;
    append(&num_tables, sizeof(num_tables));
    if (num_tables) {
        const TableHeader table{table_tag_movers, mover_table_version, 0, static_cast<std::uint32_t>(mover_bytes),
                                mover_tiles};
        append(&table, sizeof(table));
        const auto num_movers = static_cast<std::uint32_t>(g_af.movers.size());
        append(&num_movers, sizeof(num_movers));
        for (AfMover& m : g_af.movers) {
            // stamped when the save knows what the movers section wrote
            const MoverChart record{m.uid, m.num_surfaces, 0};
            m.hash_offset = static_cast<std::uint32_t>(body.size() + offsetof(MoverChart, surface_hash));
            append(&record, sizeof(record));
            for (std::uint32_t s = 0; s < m.num_surfaces; s++) {
                const AfChart& c = g_af.charts[m.first_chart + s];
                const bool charted = !c.geom.empty();
                const MoverSurfaceChart wire{charted ? c.k_u : std::uint16_t{0}, charted ? c.k_v : std::uint16_t{0},
                                             c.w, c.h};
                append(&wire, sizeof(wire));
            }
        }
    }
    for (const Tile& t : g_af.tiles) {
        append(&t, sizeof(t));
    }
    append(&dir, sizeof(dir));
    append(&layer, sizeof(layer));
    for (const auto& piece : stored) {
        append(piece.data(), piece.size());
    }
}

} // namespace

// ─── decoded terrain charts for the viewport preview ──────────────────────────

struct TerrainBakedLight
{
    TerrainChart chart{};
    // The chart box-filtered to at most terrain_reduced_texels_per_cell texels per cell, as the game keeps
    // it: the preview needs no more, and a full-density copy of every terrain can reach 150 MB.
    std::uint32_t reduction = 1;
    std::vector<std::uint8_t> rgb;
};

namespace
{

struct TerrainLightMatch
{
    std::uint32_t index;
    std::uint32_t cells_x;
};

// By terrain uid, from the last bake or the section the level was loaded with.
std::unordered_map<std::int32_t, TerrainBakedLight> g_terrain_light;
std::uint32_t g_terrain_light_generation = 0;

// The terrain previews reshade on the views' next paint.
void terrain_light_clear()
{
    g_terrain_light.clear();
    g_terrain_light_generation++;
    for (int i = 0; i < editor_num_views; i++) {
        editor_view_mark_repaint(editor_view_at(i));
    }
}

// The section's terrain and mover halves only: the editor has no surface fingerprint to hand, and needs none.
ReadResult read_unfingerprinted(const std::vector<std::uint8_t>& body)
{
    return read_section(body.data(), body.size(), nullptr, 0, 0, false);
}

// Whether chart `c` still lights terrain `uid` with `data` placed at `pos`.
bool terrain_chart_current(const TerrainChart& c, std::int32_t uid, const Vector3& pos, const DedTerrainData& data)
{
    const TerrainGrid* g = data.grid.get();
    return g && c.terrain_uid == uid && terrain_chart_matches(c, terrain_grid_view(pos, data, *g));
}

// The usable records of `r` that light a terrain of the level as it is now, the only ones decoded.
std::vector<TerrainLightMatch> terrain_light_matches(const ReadResult& r, CDedLevel* level)
{
    std::vector<TerrainLightMatch> out;
    if (!r.ok || !level) {
        return out;
    }
    for (const DedTerrain* t : level->GetAlpineLevelProperties().terrain_objects) {
        for (std::uint32_t i = 0; t && i < r.terrain.size(); i++) {
            if (r.terrain_ok[i] && terrain_chart_current(r.terrain[i], t->uid, t->pos, t->data)) {
                out.push_back({i, at::cells(t->data.grid->nx)});
                break;
            }
        }
    }
    return out;
}

// decode(match, light) runs decode_reduced_terrain_chart for the match into the light's rgb and reduction.
template<typename Decode>
void terrain_light_decode(const ReadResult& r, const std::vector<TerrainLightMatch>& matches, Decode&& decode)
{
    for (const TerrainLightMatch& m : matches) {
        TerrainBakedLight tl;
        tl.chart = r.terrain[m.index];
        if (decode(m, tl)) {
            g_terrain_light[tl.chart.terrain_uid] = std::move(tl);
        }
    }
}

// Replaces the decoded charts with those of `body`, whose decompressed layer is `layer`, a page per buffer.
void terrain_light_store(const std::vector<std::uint8_t>& body, const PageBuffers& layer)
{
    terrain_light_clear();
    const ReadResult r = read_unfingerprinted(body);
    const auto page_len =
        static_cast<std::size_t>(layer_payload_size(static_cast<Codec>(r.layer.codec), 1, r.page_size));
    const auto page_data = [&](std::uint32_t page) -> const std::uint8_t* {
        return page < layer.size() && layer[page].size() == page_len ? layer[page].data() : nullptr;
    };
    terrain_light_decode(r, terrain_light_matches(r, CDedLevel::Get()),
                         [&](const TerrainLightMatch& m, TerrainBakedLight& tl) {
                             return decode_reduced_terrain_chart_paged(r, page_data, m.index, m.cells_x,
                                                                       unpack_bc7_rgba, tl.rgb, tl.reduction);
                         });
}

// The same from a section as stored, decompressing its layer only when a chart matches a terrain.
void terrain_light_store_section(const std::vector<std::uint8_t>& body, CDedLevel& level)
{
    terrain_light_clear();
    const ReadResult r = read_unfingerprinted(body);
    const std::vector<TerrainLightMatch> matches = terrain_light_matches(r, &level);
    if (matches.empty()) {
        return;
    }
    std::vector<std::uint8_t> layer;
    if (!decompress_layer(body.data(), body.size(), r.layer, layer)) {
        return;
    }
    terrain_light_decode(r, matches, [&](const TerrainLightMatch& m, TerrainBakedLight& tl) {
        return decode_reduced_terrain_chart(r, layer.data(), layer.size(), m.index, m.cells_x, unpack_bc7_rgba,
                                            tl.rgb, tl.reduction);
    });
}

} // namespace

std::uint32_t terrain_baked_light_generation()
{
    return g_terrain_light_generation;
}

const TerrainBakedLight* terrain_baked_light_find(int uid, const Vector3& pos, const DedTerrainData& data)
{
    const auto it = g_terrain_light.find(uid);
    if (it == g_terrain_light.end() || it->second.rgb.empty()) {
        return nullptr;
    }
    // placement is in the fingerprint; a moved terrain is told apart without hashing its heights
    const TerrainChart& c = it->second.chart;
    if (c.origin_x != pos.x || c.origin_z != pos.z) {
        return nullptr;
    }
    return terrain_chart_current(c, uid, pos, data) ? &it->second : nullptr;
}

void terrain_baked_light_sample(const TerrainBakedLight& light, float world_x, float world_z, float (&texel)[3])
{
    terrain_reduced_chart_sample(light.rgb.data(), light.chart, light.reduction, world_x, world_z, texel);
}

void alpine_lm_bake_end()
{
    g_tile_pass = false;
    g_tile_view.clear();
    g_tile_view.shrink_to_fit();
    if (!g_af.active) {
        g_af = AfBake{};
        return;
    }

    shade_terrain_charts();

    const DWORD t0 = GetTickCount();
    const std::uint64_t replicated = replicate_chart_edges();
    const std::uint64_t copied = reconcile_gutters();

    auto* props = level_props();
    const CompressionMode mode = compression_mode_from_wire(props ? props->lightmap_compression : 0);
    const EncoderSettings settings = encoder_settings(mode);
    const Codec codec = g_raw_codec ? Codec::raw_rgb8 : Codec::bc7_unorm;
    std::uint32_t rdo_modified = 0;
    PageBuffers raw;
    if (g_raw_codec) {
        raw = std::move(g_af.pages);
    }
    else {
        raw = encode_bc7(settings, rdo_modified);
        // the encoder is the last reader of the RGB pages, and at the page budget they are 200 MB
        g_af.pages.clear();
        g_af.pages.shrink_to_fit();
    }
    const std::uint64_t raw_size = page_buffers_size(raw);
    if (raw_size != layer_payload_size(codec, g_af.num_pages) || raw_size > 0xffffffffull) {
        af_error("the encoded layer does not fit its size field, dropping the section");
        g_af = AfBake{};
        return;
    }
    build_body(raw, codec);
    try {
        terrain_light_store(g_af.body, raw);
    }
    catch (const std::bad_alloc&) {
        terrain_light_clear();
        af_error("out of memory keeping the baked terrain lighting for the editor view, the saved section "
                 "is unaffected");
    }
    g_af.signature = surfaces_signature(g_af.solid);
    for (AfMover& m : g_af.movers) {
        m.signature = surfaces_signature(m.solid);
    }
    g_af.complete = true;
    g_af.active = false;
    g_af.pages.clear();
    g_af.pages.shrink_to_fit();
    g_shared_blocks.clear();
    g_shared_blocks.shrink_to_fit();

    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "encoded %u pages as %s, %llu edge texels replicated, %llu gutter texels copied, "
                  "compression mode %u, rdo modified %u blocks, section %u bytes in %.1fs",
                  g_af.num_pages, g_raw_codec ? "raw rgb8" : "bc7",
                  static_cast<unsigned long long>(replicated),
                  static_cast<unsigned long long>(copied), static_cast<unsigned>(mode),
                  rdo_modified, static_cast<unsigned>(g_af.body.size()),
                  (GetTickCount() - t0) / 1000.0);
    af_log(buf);
}

void alpine_lm_bake_abort()
{
    g_tile_pass = false;
    g_tile_view.clear();
    g_tile_view.shrink_to_fit();
    g_shared_blocks.clear();
    g_shared_blocks.shrink_to_fit();
    g_af = AfBake{};
}

// ─── save and load ────────────────────────────────────────────────────────────

namespace
{

bool retained_header(std::uint32_t& faces, std::uint32_t& surfaces)
{
    if (g_retained.size() < sizeof(SectionHeader)) {
        return false;
    }
    SectionHeader head{};
    std::memcpy(&head, g_retained.data(), sizeof(head));
    if (header_invalid_reason(head)) {
        return false;
    }
    faces = head.src_num_faces;
    surfaces = head.src_num_surfaces;
    return true;
}

// Whether a stored section carries any surface chart, from its chart table.
bool section_has_surface_charts(const std::vector<std::uint8_t>& body)
{
    if (body.size() < sizeof(SectionHeader)) {
        return false;
    }
    SectionHeader head{};
    std::memcpy(&head, body.data(), sizeof(head));
    if (body.size() - sizeof(SectionHeader) < static_cast<std::uint64_t>(head.num_charts) * sizeof(Chart)) {
        return false;
    }
    for (std::uint32_t i = 0; i < head.num_charts; i++) {
        Chart c{};
        std::memcpy(&c, body.data() + sizeof(SectionHeader) + i * sizeof(Chart), sizeof(c));
        if (c.k_u != 0 && c.k_v != 0) {
            return true;
        }
    }
    return false;
}

// Terrain lighting the save is about to carry that no longer matches its terrain, and terrains that
// have none: both render with the placeholder lighting in game until the level is re-baked.
void warn_terrain_lighting(const std::vector<std::uint8_t>* body, const AlpineLevelProperties& props)
{
    ReadResult r;
    if (body) {
        r = read_unfingerprinted(*body);
    }
    for (const DedTerrain* t : props.terrain_objects) {
        if (!t) {
            continue;
        }
        const TerrainChart* chart = nullptr;
        for (std::uint32_t i = 0; r.ok && i < r.terrain.size(); i++) {
            if (r.terrain_ok[i] && r.terrain[i].terrain_uid == t->uid) {
                chart = &r.terrain[i];
            }
        }
        if (!chart) {
            af_warn(terrain_label(*t) + " has no baked lighting, run Calculate Lighting");
        }
        else if (!terrain_chart_current(*chart, t->uid, t->pos, t->data)) {
            af_warn(terrain_label(*t) + " changed since its lighting was baked, run Calculate "
                   "Lighting");
        }
    }
}

void warn_if_stock_layout_synthesized()
{
    if (lightmap_stock_layout_synthesized() && !g_suppress_stock) {
        af_warn("this level was loaded without stock lightmaps and has not been rebuilt, so "
               "the stock lightmaps section being written has overlapping surfaces; run Build "
               "Geometry and Calculate Lighting before saving");
    }
}

} // namespace

void alpine_lm_drop_retained()
{
    g_retained.clear();
    g_retained.shrink_to_fit();
    g_retained_suppressed = false;
    g_retained_signature = 0;
    g_af = AfBake{};
    terrain_light_clear();
}

bool alpine_lm_stock_suppressed()
{
    return g_suppress_stock;
}

namespace
{

void save_decide(CDedLevel& level)
{
    const auto s = reinterpret_cast<std::uintptr_t>(level.solid);
    g_save_num_surfaces = static_cast<std::uint32_t>(std::max(0, solid_surfaces(s).count));
    g_save_num_faces = static_cast<std::uint32_t>(
        std::max(0, AddrCaller{0x0043cc40}.this_call<int>(reinterpret_cast<void*>(s + 0x70))));

    // Surface charts replace the stock lightmaps only with the fixed pipeline, so a section that
    // carries them is not emitted for a legacy-lit level or one with alpine surface lightmaps off; one
    // with terrain charts alone always is.
    // Mover charts are brushwork too: they follow the same rule, but only static charts stand in for the
    // stock section.
    const auto& props = level.GetAlpineLevelProperties();
    const bool charts_allowed = props.surface_charts_enabled();
    bool has_surface = false;
    const bool fresh_geometry = g_af.complete && g_af.solid == s && g_af.num_surface_charts == g_save_num_surfaces &&
                                g_af.signature == surfaces_signature(s);
    const bool fresh_brush = g_af.has_surface_charts || !g_af.movers.empty();
    std::vector<std::int32_t> charted_movers;
    if (fresh_geometry && !(fresh_brush && !charts_allowed)) {
        g_emit_af = true;
        has_surface = g_af.has_surface_charts;
        for (const AfMover& m : g_af.movers) {
            charted_movers.push_back(m.uid);
        }
    }
    else {
        std::uint32_t faces = 0, surfaces = 0;
        const ReadResult retained = read_unfingerprinted(g_retained);
        const bool retained_surface = section_has_surface_charts(g_retained);
        const bool retained_brush = retained_surface || !retained.movers.empty();
        const bool retained_geometry = retained_header(faces, surfaces) && surfaces == g_save_num_surfaces &&
                                       faces == g_save_num_faces && g_retained_signature == surfaces_signature(s);
        const bool has_terrain = g_af.complete ? !g_af.terrains.empty() : !retained.terrain.empty();
        for (std::size_t i = 0; i < retained.movers.size(); i++) {
            if (retained.mover_ok[i]) {
                charted_movers.push_back(retained.movers[i].mover_uid);
            }
        }
        if (retained_geometry && !(retained_brush && !charts_allowed) &&
            g_retained_suppressed == (retained_surface && props.d3d11_only_lightmaps)) {
            g_emit_af = true;
            g_emit_retained = true;
            has_surface = retained_surface;
            af_log("re-emitting the alpine lightmap section this level was loaded with; re-bake if "
                   "the geometry changed");
        }
        else if (!charts_allowed && ((fresh_geometry && fresh_brush) ||
                                     (!g_af.complete && retained_geometry && retained_brush))) {
            const char* why = props.legacy_lighting ? "Legacy lighting is set, so the alpine lightmap section baked "
                                                      "without it is being dropped"
                                                    : "Alpine surface lightmaps are off, so the alpine lightmap "
                                                      "section baked with them is being dropped";
            af_warn(has_terrain ? std::string{why} + ", terrain lighting included; run Calculate Lighting again" : why);
        }
        else if (g_af.complete || !g_retained.empty()) {
            af_warn(has_terrain ? "the alpine lightmap section no longer matches the geometry being "
                                  "saved and is being dropped, terrain lighting included: terrain lighting must "
                                  "be re-baked after Build Geometry, run Calculate Lighting"
                                : "the alpine lightmap section no longer matches the geometry being "
                                  "saved and is being dropped, re-bake the level");
        }
    }
    // bit0 of the saved property says the stock section is gone, which only surface charts can stand in for
    g_suppress_stock = g_emit_af && has_surface && props.d3d11_only_lightmaps;
    g_emit_surface_charts = has_surface;
    if (g_suppress_stock) {
        af_log("d3d11 only lightmaps: the stock lightmaps section is omitted and every surface "
               "records lightmap index -1");
        std::vector<std::int32_t> warned;
        for_each_mover_brush(level, [&](const BrushNode& brush) {
            if (solid_surfaces(reinterpret_cast<std::uintptr_t>(brush.geometry)).count <= 0
                || std::find(charted_movers.begin(), charted_movers.end(), brush.uid) != charted_movers.end()
                || std::find(warned.begin(), warned.end(), brush.uid) != warned.end()) {
                return;
            }
            warned.push_back(brush.uid);
            af_warn("mover " + std::to_string(brush.uid) +
                    " has no alpine lighting and renders fullbright on this d3d11-only level");
        });
    }
    warn_if_stock_layout_synthesized();
    if (!props.terrain_objects.empty()) {
        warn_terrain_lighting(g_emit_af ? (g_emit_retained ? &g_retained : &g_af.body) : nullptr, props);
    }
}

} // namespace

void alpine_lm_save_begin(CDedLevel& level)
{
    g_emit_af = false;
    g_emit_retained = false;
    g_emit_surface_charts = false;
    g_suppress_stock = false;
    g_save_num_faces = 0;
    g_save_num_surfaces = 0;
    g_save_surface_hash = 0;
    g_hash_armed = false;
    g_hash_capturing = false;
    g_save_movers.clear();
    g_mover_save_brush = nullptr;

    if (!level.solid) {
        return;
    }
    try {
        save_decide(level);
    }
    catch (const std::bad_alloc&) {
        // the stock section is written and nothing stands in for it
        g_emit_af = false;
        g_emit_retained = false;
        g_emit_surface_charts = false;
        g_suppress_stock = false;
        xlog::error("[AlpineLightmaps] out of memory preparing the save, the alpine lightmap section is dropped");
    }
}

namespace
{

// The section header as saved: with `restamp`, the fingerprint fields describe the surfaces just written.
void section_header_as_written(const std::uint8_t* body, bool restamp, std::uint32_t num_faces,
                               std::uint32_t num_surfaces, std::uint32_t surface_hash,
                               std::uint8_t (&head)[sizeof(SectionHeader)])
{
    std::memcpy(head, body, sizeof(head));
    if (restamp) {
        std::memcpy(head + offsetof(SectionHeader, src_num_faces), &num_faces, 4);
        std::memcpy(head + offsetof(SectionHeader, src_num_surfaces), &num_surfaces, 4);
        std::memcpy(head + offsetof(SectionHeader, src_surface_hash), &surface_hash, 4);
    }
}

// A mover record's surface_hash as saved: the fingerprint the movers section just recorded for it.
struct MoverStamp
{
    std::size_t offset;
    std::uint32_t hash;
};

// A fresh bake's records, by body offset. A mover whose solid changed since the bake gets a hash no
// solid can have, so it keeps its stock lightmap in game rather than a chart for other surfaces.
std::vector<MoverStamp> fresh_mover_stamps()
{
    std::vector<MoverStamp> out;
    out.reserve(g_af.movers.size());
    for (const AfMover& m : g_af.movers) {
        const auto it = g_save_movers.find(m.uid);
        std::uint32_t hash = 0;
        if (it == g_save_movers.end()) {
            af_log("mover " + std::to_string(m.uid) + " was not saved, its baked lighting matches nothing");
        }
        else if (!it->second.duplicate && it->second.solid == m.solid && it->second.num_surfaces == m.num_surfaces
                 && surfaces_signature(m.solid) == m.signature) {
            hash = it->second.hash;
        }
        else {
            hash = ~it->second.hash;
            af_warn("mover " + std::to_string(m.uid) + " changed since Calculate Lighting, re-bake");
        }
        out.push_back({m.hash_offset, hash});
    }
    std::sort(out.begin(), out.end(), [](const MoverStamp& a, const MoverStamp& b) { return a.offset < b.offset; });
    return out;
}

// A retained section's records keep the hashes they were baked with; each that no longer matches what
// the movers section just wrote is reported.
void check_retained_movers(const std::vector<std::uint8_t>& body)
{
    const ReadResult r = read_unfingerprinted(body);
    for (std::size_t i = 0; i < r.movers.size(); i++) {
        if (!r.mover_ok[i]) {
            continue;
        }
        const auto it = g_save_movers.find(r.movers[i].mover_uid);
        if (it == g_save_movers.end() || it->second.duplicate || it->second.hash != r.movers[i].surface_hash) {
            af_warn("mover " + std::to_string(r.movers[i].mover_uid) +
                    "'s baked lighting no longer matches, it keeps its stock lightmaps (fullbright on d3d11-only) "
                    "until re-baked");
        }
    }
}

} // namespace

void alpine_lm_serialize_chunk(CDedLevel& level, rf::File& file)
{
    if (!g_emit_af) {
        return;
    }
    const std::vector<std::uint8_t>& body = g_emit_retained ? g_retained : g_af.body;
    if (body.size() < sizeof(SectionHeader)) {
        return;
    }
    // With no surface charts the fingerprint guards nothing, so a retained terrain-only section is
    // restamped with the surfaces just written like a fresh one.
    const bool restamp = !g_emit_retained || !g_emit_surface_charts;
    if (!restamp) {
        // The stock section is already behind us by now, so this can only refuse the stale copy.
        std::uint32_t retained_hash = 0;
        std::memcpy(&retained_hash, body.data() + offsetof(SectionHeader, src_surface_hash), 4);
        if (retained_hash != g_save_surface_hash) {
            try {
                af_warn("the retained alpine lightmap section does not match the surfaces just "
                        "written and was NOT saved, and the stock section was already omitted: the level "
                        "now has NO lightmaps, re-bake it");
            }
            catch (const std::bad_alloc&) {
            }
            return;
        }
    }
    std::vector<MoverStamp> stamps;
    try {
        if (g_emit_retained) {
            check_retained_movers(body);
        }
        else {
            stamps = fresh_mover_stamps();
        }
    }
    catch (const std::bad_alloc&) {
        // unstamped mover records match no mover, which then keeps its stock lighting
        stamps.clear();
    }
    std::uint8_t head[sizeof(SectionHeader)];
    section_header_as_written(body.data(), restamp, g_save_num_faces, g_save_num_surfaces, g_save_surface_hash,
                              head);
    auto start_pos = level.BeginRflSection(file, alpine_lightmaps_chunk_id);
    file.write(head, sizeof(head));
    std::size_t pos = sizeof(head);
    for (const MoverStamp& st : stamps) {
        if (st.offset < pos || st.offset + sizeof(st.hash) > body.size()) {
            continue;
        }
        if (st.offset > pos) {
            file.write(body.data() + pos, st.offset - pos);
        }
        file.write(&st.hash, sizeof(st.hash));
        pos = st.offset + sizeof(st.hash);
    }
    if (body.size() > pos) {
        file.write(body.data() + pos, body.size() - pos);
    }
    level.EndRflSection(file, start_pos);
    try {
        af_log("wrote the alpine lightmap section, " + std::to_string(body.size()) + " bytes");
    }
    catch (const std::bad_alloc&) {
    }
}

void alpine_lm_deserialize_chunk(CDedLevel& level, rf::File& file, std::size_t chunk_len)
{
    std::size_t remaining = chunk_len;
    rf::File::ChunkGuard chunk_guard{file, remaining};
    g_retained.clear();
    // the alpine props chunk is written before the geometry section and so is always read first
    g_retained_suppressed = level.GetAlpineLevelProperties().stock_lightmaps_omitted;
    const std::int64_t bytes_left = static_cast<std::int64_t>(file.get_size()) - file.tell();
    if (!section_len_plausible(chunk_len, bytes_left)) {
        // remaining is left alone so the guard seeks past the chunk; zeroing it would leave the
        // cursor inside the payload and the next chunk header would be read out of it.
        return;
    }
    try {
        g_retained.resize(chunk_len);
    }
    catch (const std::bad_alloc&) {
        g_retained.clear();
        af_error("out of memory reading the alpine lightmap section, it is dropped");
        return;
    }
    const int got = file.read(g_retained.data(), chunk_len);
    if (got <= 0 || file.error() || static_cast<std::size_t>(got) < chunk_len) {
        if (got > 0) {
            remaining -= static_cast<std::size_t>(got);
        }
        g_retained.clear();
        return;
    }
    remaining = 0;
    g_retained_signature =
        level.solid ? surfaces_signature(reinterpret_cast<std::uintptr_t>(level.solid)) : 0;
    af_log("retained the level's " + std::to_string(chunk_len) +
           " byte alpine lightmap section, it is re-emitted unchanged if nothing re-bakes it");
    try {
        terrain_light_store_section(g_retained, level);
    }
    catch (const std::bad_alloc&) {
        terrain_light_clear();
    }
}

// ─── engine hooks ─────────────────────────────────────────────────────────────

namespace
{

// solid_write(file, solid, brush_only). The static solid's surface records are the last thing it
// writes, so the fingerprint hash covers exactly them: the tap arms here and starts capturing on
// the first surface's lightmap index load.
void __cdecl solid_write_new(void* file, void* solid, char brush_only);
FunHook<void __cdecl(void*, void*, char)> solid_write_hook{0x004a3fc0, solid_write_new};

void __cdecl solid_write_new(void* file, void* solid, char brush_only)
{
    auto* level = CDedLevel::Get();
    const bool ours = level && solid && solid == level->solid && brush_only == 0;
    const bool mover = g_mover_save_brush && solid && solid == g_mover_save_brush->geometry && brush_only == 0;
    if (ours || mover) {
        if (!g_hash_state) {
            g_hash_state = XXH32_createState();
        }
        g_hash_armed = g_hash_state != nullptr;
    }
    solid_write_hook.call_target(file, solid, brush_only);
    if (ours || mover) {
        // A zero surface solid writes no surface bytes, so the fingerprint is the digest of the
        // empty message and not 0 - which is what a reader hashing zero bytes will compute.
        const std::uint8_t empty = 0;
        const std::uint32_t hash = g_hash_capturing ? XXH32_digest(g_hash_state) : XXH32(&empty, 0, 0);
        g_hash_armed = false;
        g_hash_capturing = false;
        if (ours) {
            g_save_surface_hash = hash;
        }
        else {
            const auto s = reinterpret_cast<std::uintptr_t>(solid);
            const SaveMover capture{hash, static_cast<std::uint32_t>(std::max(0, solid_surfaces(s).count)), s, false};
            try {
                const auto [it, fresh] = g_save_movers.try_emplace(g_mover_save_brush->uid, capture);
                if (!fresh && it->second.solid != s) {
                    it->second.duplicate = true;
                }
            }
            catch (const std::bad_alloc&) {
                // an uncaptured mover is stamped as not written and matches nothing
            }
        }
    }
}

// BrushNode::write(file, 0), called only by the movers section's loop (0x2000): the brush whose solid
// the next solid_write writes.
void __fastcall mover_brush_write_new(BrushNode* brush, int edx, void* file, int arg);
CallHook<void __fastcall(BrushNode*, int, void*, int)> mover_brush_write_hook{0x004316c6, mover_brush_write_new};

void __fastcall mover_brush_write_new(BrushNode* brush, int edx, void* file, int arg)
{
    g_mover_save_brush = brush;
    mover_brush_write_hook.call_target(brush, edx, file, arg);
    g_mover_save_brush = nullptr;
}

// rf::File::write, the single funnel every RFL field write goes through.
void __fastcall file_write_new(void* self, int edx, const void* data, std::size_t len);
FunHook<void __fastcall(void*, int, const void*, std::size_t)> file_write_hook{0x004d13f0,
                                                                              file_write_new};

void __fastcall file_write_new(void* self, int edx, const void* data, std::size_t len)
{
    if (g_hash_capturing && data && len) {
        XXH32_update(g_hash_state, data, len);
    }
    file_write_hook.call_target(self, edx, data, len);
}

// Replaces "MOV ECX,[EDI+0xc]; MOV EDX,[ECX+0x14]" (6 bytes), the lightmap index load at the head
// of solid_write's surface record loop. Both loaders clamp an out of range index into the array and
// synthesise a page when the level carries none, so -1 is exactly what a level with no lightmaps
// section should record.
CodeInjection surface_lightmap_index_injection{
    0x004a46dc,
    [](auto& regs) {
        if (g_hash_armed && !g_hash_capturing) {
            g_hash_capturing = true;
            XXH32_reset(g_hash_state, 0);
        }
        if (g_suppress_stock) {
            regs.ecx = 0;
            regs.edx = 0xffffffffu;
        }
        else {
            const auto lm = *reinterpret_cast<std::uintptr_t*>(static_cast<uintptr_t>(regs.edi) + 0xc);
            regs.ecx = lm;
            regs.edx = *reinterpret_cast<std::uint32_t*>(lm + 0x14);
        }
        regs.eip = 0x004a46e2;
    },
    false, // no trampoline: the injection fully replaces the two loads
};

// Replaces "MOV ECX,0x1128678" (5 bytes) ahead of the lightmaps section. 0x00430ddf is where stock
// resumes when the level has no lightmap pages at all, which is the same file shape this produces.
CodeInjection stock_lightmaps_section_injection{
    0x00430d55,
    [](auto& regs) {
        regs.ecx = 0x1128678;
        regs.eip = g_suppress_stock ? 0x00430ddf : 0x00430d5a;
    },
    false, // no trampoline: the injection fully replaces the 5 byte load
};

// At "PUSH 0; LEA ECX,[ESP+0x44]" (6 bytes), just past the movers section (0x2000) and the target of
// its skip when there are none: both the geometry's and every mover's fingerprint are recorded by now,
// and no object chunk has been written yet.
CodeInjection alpine_lightmaps_section_injection{
    0x004316dd,
    [](auto& regs) {
        auto& level = *reinterpret_cast<CDedLevel*>(static_cast<uintptr_t>(regs.edi));
        auto& file = *reinterpret_cast<rf::File*>(static_cast<uintptr_t>(regs.esi));
        alpine_lm_serialize_chunk(level, file);
    },
};

} // namespace

void ApplyAlpineLightmapPatches()
{
    // GetCommandLineA rather than argv, which the CRT has not populated during DLL init
    g_raw_codec = std::strstr(GetCommandLineA(), "-rawlightmaps") != nullptr;
    bc7enc_compress_block_init();
    solid_write_hook.install();
    file_write_hook.install();
    surface_lightmap_index_injection.install();
    stock_lightmaps_section_injection.install();
    mover_brush_write_hook.install();
    alpine_lightmaps_section_injection.install();
}
