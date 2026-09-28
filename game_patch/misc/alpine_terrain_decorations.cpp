#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <xlog/xlog.h>
#include "alpine_settings.h"
#include "alpine_terrain.h"
#include "alpine_terrain_decorations.h"
#include "../graphics/gr.h"
#include "../multi/multi.h"
#include "../os/console.h"
#include "../rf/gr/gr.h"
#include "../rf/math/matrix.h"
#include "../rf/math/vector.h"
#include "../rf/multi.h"
#include "../rf/os/frametime.h"
#include "../rf/v3d.h"
#include "../rf/vmesh.h"

namespace at = alpine_terrain;

namespace
{

// Absurd geometry must not blow a chunk's cull box out to cover the level.
constexpr float max_mesh_radius = 100.0f;
// The legacy renderers draw each instance as its own mesh, so only the nearest ones.
constexpr float legacy_max_distance = 40.0f;
constexpr std::size_t legacy_max_draws = 1024;
// In range instances considered per frame, the nearest legacy_max_draws of them drawn
constexpr std::size_t legacy_max_candidates = 16384;

struct Crater
{
    rf::Vector3 pos;
    float radius;
    // The terrain chunks it carves: only their instances go
    std::vector<AlpineTerrainRoomRef> chunks;
};

bool g_draw_enabled = true;
std::uint32_t g_placed = 0;
std::vector<TerrainDecorations> g_decorations;
DecorationMeshCache g_meshes{max_mesh_radius};
// Every crater of the level so far, for those a savegame replays before the instances exist
std::vector<Crater> g_craters;
DecorationFrameStats g_stats;

struct LegacyDraw
{
    float dist_sq;
    const GpuDecorationInstance* inst;
    int mesh_slot;
};
struct LegacyChunk
{
    float dist_sq;
    const TerrainDecorations* td;
    const DecorationChunk* chunk;
};
// Reserved at level init, so the render pass never allocates
std::vector<LegacyDraw> g_legacy_draws;
std::vector<LegacyChunk> g_legacy_chunks;

bool level_decorated()
{
    const auto& terrains = alpine_terrain_get_all();
    return std::any_of(terrains.begin(), terrains.end(), [](const AlpineTerrain& t) {
        return t.resolved && !t.decorations.empty();
    });
}

std::uint32_t pack_rgba(const float (&rgb)[3], float a)
{
    auto byte = [](float v) {
        return static_cast<std::uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return byte(rgb[0]) | byte(rgb[1]) << 8 | byte(rgb[2]) << 16 | byte(a) << 24;
}

// The ambient MeshRenderer gives a mesh standing on this texel (MRF_CUSTOM_AMBIENT_COLOR), with its sun scale.
std::uint32_t instance_light(int terrain, const at::DecorationInstance& inst, bool d3d11)
{
    float texel[3];
    alpine_terrain_sample_light(terrain, at::FaceKind::top, inst.base, inst.normal, texel);
    // The clamp GSolid_get_ambient_color_hook applies, which keeps it from reading as "no lightmap"
    for (float& c : texel) {
        c = std::clamp(c, 0.0f, 254.0f / 255.0f);
    }
    if (!d3d11) {
        return pack_rgba(texel, 1.0f);
    }
    float rgb[3];
    gr_mesh_blend_ambient(texel, rgb);
    for (float& c : rgb) {
        c = std::clamp(c, 0.0f, 1.0f);
    }
    return pack_rgba(rgb, gr_sun_get_mesh_scale(rgb));
}

rf::Vector3 instance_origin(const GpuDecorationInstance& g)
{
    return {g.row0[3], g.row1[3], g.row2[3]};
}

// Where it stands: the origin lowered by the decoration's vertical offset along the scaled uvec.
rf::Vector3 instance_base(const GpuDecorationInstance& g, float vertical_offset)
{
    return {g.row0[3] - g.row0[1] * vertical_offset, g.row1[3] - g.row1[1] * vertical_offset,
            g.row2[3] - g.row2[1] * vertical_offset};
}

// Removes the instances standing within the crater on the chunks it carves. Returns how many.
std::uint32_t apply_crater(const Crater& crater)
{
    const float r_sq = crater.radius * crater.radius;
    std::uint32_t removed = 0;
    for (const AlpineTerrainRoomRef& ref : crater.chunks) {
        if (ref.terrain < 0 || static_cast<std::size_t>(ref.terrain) >= g_decorations.size()) continue;
        TerrainDecorations& td = g_decorations[static_cast<std::size_t>(ref.terrain)];
        if (ref.chunk < 0 || static_cast<std::size_t>(ref.chunk) >= td.chunks.size()) continue;
        const auto c = static_cast<std::size_t>(ref.chunk);
        DecorationChunk& chunk = td.chunks[c];
        if (!(chunk.dist_sq(crater.pos) <= r_sq)) continue;
        bool changed = false;
        for (std::uint32_t d = 0; d < at::max_decorations; d++) {
            GpuDecorationInstance* first = td.inst.data() + chunk.first[d];
            GpuDecorationInstance* last = first + chunk.count[d];
            const float offset = td.vertical_offset[d];
            GpuDecorationInstance* kept = std::remove_if(first, last, [&](const GpuDecorationInstance& g) {
                return (instance_base(g, offset) - crater.pos).len_sq() <= r_sq;
            });
            const auto n = static_cast<std::uint32_t>(last - kept);
            if (n > 0) {
                chunk.count[d] -= n;
                removed += n;
                changed = true;
            }
        }
        const auto index = static_cast<std::uint32_t>(c);
        if (changed && std::find(td.dirty_chunks.begin(), td.dirty_chunks.end(), index) == td.dirty_chunks.end()) {
            td.dirty_chunks.push_back(index);
        }
    }
    return removed;
}

// Places terrain `index`'s decorations within the level's `budget`, which it lowers.
void build_terrain(int index, const AlpineTerrain& t, TerrainDecorations& td, at::DecorationBudget& budget,
                   bool d3d11)
{
    const at::GridView g = alpine_terrain_grid(t);
    at::DecorationView views[at::max_decorations];
    const std::uint32_t count = alpine_terrain_decoration_views(t, views);
    float radius[at::max_decorations] = {};
    for (std::uint32_t d = 0; d < count; d++) {
        td.draw_distance[d] = t.decorations[d].draw_distance;
        td.vertical_offset[d] = t.decorations[d].vertical_offset;
        if (at::decoration_active(views[d])) {
            const std::string& mesh = t.decorations[d].mesh;
            if (at::decoration_mesh_is_vfx(mesh.c_str(), mesh.size())) {
                if (g_meshes.reject(mesh)) {
                    xlog::warn("[AlpineTerrain] Animated decoration mesh '{}' is not supported yet", mesh);
                }
                td.mesh_slot[d] = -1;
                continue;
            }
            td.mesh_slot[d] = g_meshes.resolve(mesh, "AlpineTerrain");
            radius[d] = td.mesh_slot[d] >= 0 ? g_meshes[td.mesh_slot[d]].radius : 0.0f;
        }
    }

    const at::ChunkLayout layout = at::header_chunk_layout(t.header);
    DecorationChunk empty{};
    std::fill(std::begin(empty.lo), std::end(empty.lo), std::numeric_limits<float>::max());
    std::fill(std::begin(empty.hi), std::end(empty.hi), -std::numeric_limits<float>::max());
    td.chunks.assign(at::layout_chunk_count(layout), empty);

    // A layer whose mesh failed still spends the budget, so every reader places the same set.
    at::for_each_terrain_decoration(
        g, t.uid, layout, views, count, budget,
        [&](std::uint32_t c, std::uint32_t d, const at::DecorationInstance& inst) {
            if (td.mesh_slot[d] < 0) return true;
            DecorationChunk& chunk = td.chunks[c];
            if (chunk.count[d] == 0) {
                chunk.first[d] = static_cast<std::uint32_t>(td.inst.size());
            }
            chunk.count[d]++;
            const float s = inst.scale;
            GpuDecorationInstance& gi = td.inst.emplace_back();
            std::array<float, 4>* rows[3] = {&gi.row0, &gi.row1, &gi.row2};
            const float r = radius[d] * s;
            for (int k = 0; k < 3; k++) {
                *rows[k] = {inst.rvec[k] * s, inst.uvec[k] * s, inst.fvec[k] * s, inst.pos[k]};
                chunk.lo[k] = std::min({chunk.lo[k], inst.pos[k] - r, inst.base[k]});
                chunk.hi[k] = std::max({chunk.hi[k], inst.pos[k] + r, inst.base[k]});
            }
            gi.light = instance_light(index, inst, d3d11);
            return true;
        });
}

// Adds chunk's instances within the legacy range of `eye` to g_legacy_draws, up to its capacity.
void collect_legacy_chunk(const TerrainDecorations& td, const DecorationChunk& chunk, const rf::Vector3& eye)
{
    for (std::uint32_t d = 0; d < at::max_decorations; d++) {
        if (chunk.count[d] == 0 || td.mesh_slot[d] < 0) continue;
        const float max_dist = std::min(td.draw_distance[d], legacy_max_distance);
        for (std::uint32_t i = 0; i < chunk.count[d]; i++) {
            if (g_legacy_draws.size() == g_legacy_draws.capacity()) return;
            const GpuDecorationInstance& g = td.inst[chunk.first[d] + i];
            const float dist_sq = (instance_origin(g) - eye).len_sq();
            if (dist_sq <= max_dist * max_dist) {
                g_legacy_draws.push_back({dist_sq, &g, td.mesh_slot[d]});
            }
        }
    }
}

void draw_legacy(const LegacyDraw& draw)
{
    const GpuDecorationInstance& g = *draw.inst;
    rf::Vector3 pos = instance_origin(g);
    rf::Matrix3 orient;
    orient.rvec = {g.row0[0], g.row1[0], g.row2[0]};
    orient.uvec = {g.row0[1], g.row1[1], g.row2[1]};
    orient.fvec = {g.row0[2], g.row1[2], g.row2[2]};
    const DecorationMesh& mesh = g_meshes[draw.mesh_slot];
    if (rf::gr::cull_sphere(pos, mesh.radius * orient.rvec.len())) return;
    // params.orient also turns the stock key and fill lights (0x0052DAD0); its scale there is tolerated.
    rf::MeshRenderParams params{};
    params.init_defaults();
    params.flags = rf::MRF_CUSTOM_AMBIENT_COLOR;
    params.ambient_color.set(static_cast<rf::ubyte>(g.light), static_cast<rf::ubyte>(g.light >> 8),
                             static_cast<rf::ubyte>(g.light >> 16), 255);
    params.orient = orient;
    rf::vmesh_render(mesh.mesh, &pos, &orient, &params);
}

ConsoleCommand2 dbg_terrain_decorations_cmd{
    "dbg_terrain_decorations",
    [](std::optional<int> value) {
        g_draw_enabled = value ? value.value() != 0 : !g_draw_enabled;
        rf::console::print("Terrain decorations are {}: {} placed; last frame {} chunk(s), {} draw(s), {} instance(s), "
                           "{:.3f} ms",
                           g_draw_enabled ? "on" : "off", g_placed, g_stats.visible_chunks, g_stats.draws,
                           g_stats.instances, g_stats.cpu_ms);
    },
    "Toggles terrain mesh decorations and prints their draw statistics",
    "dbg_terrain_decorations [0|1]",
};

} // namespace

void alpine_terrain_decorations_level_init()
{
    const auto& terrains = alpine_terrain_get_all();
    if (level_decorated()) {
        const bool d3d11 = is_d3d11();
        try {
            g_decorations.resize(terrains.size());
            at::DecorationBudget budget;
            for (std::size_t i = 0; i < terrains.size() && !budget.spent(); i++) {
                const AlpineTerrain& t = terrains[i];
                if (t.resolved && !t.decorations.empty()) {
                    build_terrain(static_cast<int>(i), t, g_decorations[i], budget, d3d11);
                }
            }
            std::uint32_t removed = 0;
            for (const Crater& crater : g_craters) {
                removed += apply_crater(crater);
            }
            std::size_t chunks = 0;
            for (TerrainDecorations& td : g_decorations) {
                td.dirty_chunks.clear();
                g_placed += static_cast<std::uint32_t>(td.inst.size());
                chunks += td.chunks.size();
            }
            g_placed -= removed;
            if (!d3d11) {
                g_legacy_draws.reserve(legacy_max_candidates);
                g_legacy_chunks.reserve(chunks);
            }
            xlog::info("[AlpineTerrain] Placed {} decoration instance(s), {} removed by craters", g_placed, removed);
            if (budget.instances == 0) {
                xlog::warn("[AlpineTerrain] Terrain decorations reached the level limit of {} instances",
                           at::max_level_decoration_instances);
            }
            else if (budget.candidates == 0) {
                xlog::warn("[AlpineTerrain] Terrain decorations reached the level limit of {} placement candidates",
                           at::max_level_decoration_candidates);
            }
        }
        catch (const std::bad_alloc&) {
            g_decorations.clear();
            g_decorations.shrink_to_fit();
            g_placed = 0;
            xlog::warn("[AlpineTerrain] Out of memory placing terrain decorations; none are drawn");
        }
    }
    alpine_terrain_release_decoration_maps();
}

void alpine_terrain_decorations_clear_state()
{
    g_decorations.clear();
    g_decorations.shrink_to_fit();
    g_meshes.clear();
    g_craters.clear();
    g_legacy_draws.clear();
    g_legacy_draws.shrink_to_fit();
    g_legacy_chunks.clear();
    g_legacy_chunks.shrink_to_fit();
    g_placed = 0;
    g_stats = {};
}

void alpine_terrain_decorations_notify_crater(const rf::Vector3& pos, float radius,
                                              const std::vector<rf::GRoom*>& rooms)
{
    if (rf::is_dedicated_server || is_headless_mode() || !(radius > 0.0f) || !std::isfinite(radius) ||
        !level_decorated()) {
        return;
    }
    std::uint32_t removed = 0;
    try {
        Crater crater{pos, radius, {}};
        for (const rf::GRoom* room : rooms) {
            if (const AlpineTerrainRoomRef* ref = alpine_terrain_find_room(room)) {
                crater.chunks.push_back(*ref);
            }
        }
        if (crater.chunks.empty()) {
            return;
        }
        g_craters.push_back(std::move(crater));
        removed = apply_crater(g_craters.back());
    }
    catch (const std::bad_alloc&) {
        xlog::warn("[AlpineTerrain] Out of memory recording a crater; its decorations may stay");
    }
    if (removed > 0) {
        g_placed -= removed;
        xlog::info("[AlpineTerrain] A crater removed {} decoration instance(s)", removed);
    }
}

void alpine_terrain_decorations_render_legacy()
{
    if (g_placed == 0 || !g_draw_enabled || is_d3d11() || rf::is_dedicated_server || is_headless_mode()) {
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    const rf::Vector3 eye = rf::gr::eye_pos;
    DecorationFrameStats& stats = alpine_terrain_decorations_frame_stats();
    g_legacy_draws.clear();
    g_legacy_chunks.clear();
    for (const TerrainDecorations& td : g_decorations) {
        for (const DecorationChunk& chunk : td.chunks) {
            const float dist_sq = chunk.dist_sq(eye);
            if (dist_sq <= legacy_max_distance * legacy_max_distance &&
                !rf::gr::cull_bounding_box(chunk.lo_vec(), chunk.hi_vec()) &&
                g_legacy_chunks.size() < g_legacy_chunks.capacity()) {
                g_legacy_chunks.push_back({dist_sq, &td, &chunk});
            }
        }
    }
    // Nearest chunks first, so those the candidate cap cuts off are the farthest
    std::sort(g_legacy_chunks.begin(), g_legacy_chunks.end(),
              [](const LegacyChunk& a, const LegacyChunk& b) { return a.dist_sq < b.dist_sq; });
    for (const LegacyChunk& c : g_legacy_chunks) {
        if (g_legacy_draws.size() == g_legacy_draws.capacity()) break;
        collect_legacy_chunk(*c.td, *c.chunk, eye);
        stats.visible_chunks++;
    }
    if (g_legacy_draws.size() > legacy_max_draws) {
        std::nth_element(g_legacy_draws.begin(), g_legacy_draws.begin() + legacy_max_draws, g_legacy_draws.end(),
                         [](const LegacyDraw& a, const LegacyDraw& b) { return a.dist_sq < b.dist_sq; });
        g_legacy_draws.resize(legacy_max_draws);
    }
    for (const LegacyDraw& draw : g_legacy_draws) {
        draw_legacy(draw);
    }
    stats.draws += static_cast<std::uint32_t>(g_legacy_draws.size());
    stats.instances += static_cast<std::uint32_t>(g_legacy_draws.size());
    if (!g_legacy_draws.empty()) {
        rf::gr::set_color(255, 255, 255, 255);
        rf::gr::set_texture(-1, -1);
    }
    stats.cpu_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void alpine_terrain_decorations_apply_patch()
{
    dbg_terrain_decorations_cmd.register_cmd();
}

bool alpine_terrain_decorations_active()
{
    return g_draw_enabled && g_placed > 0;
}

std::vector<TerrainDecorations>& alpine_terrain_decorations_get_all()
{
    return g_decorations;
}

const DecorationChunk* alpine_terrain_decorations_chunk(const AlpineTerrainRoomRef& ref)
{
    if (ref.terrain < 0 || static_cast<std::size_t>(ref.terrain) >= g_decorations.size()) return nullptr;
    const TerrainDecorations& td = g_decorations[static_cast<std::size_t>(ref.terrain)];
    if (ref.chunk < 0 || static_cast<std::size_t>(ref.chunk) >= td.chunks.size()) return nullptr;
    const DecorationChunk& chunk = td.chunks[static_cast<std::size_t>(ref.chunk)];
    return std::any_of(std::begin(chunk.count), std::end(chunk.count), [](std::uint32_t n) { return n > 0; })
               ? &chunk
               : nullptr;
}

const DecorationMesh& alpine_terrain_decorations_mesh(int slot)
{
    return g_meshes[slot];
}

float DecorationChunk::dist_sq(const rf::Vector3& p) const
{
    const float d[3] = {std::max({lo[0] - p.x, 0.0f, p.x - hi[0]}), std::max({lo[1] - p.y, 0.0f, p.y - hi[1]}),
                        std::max({lo[2] - p.z, 0.0f, p.z - hi[2]})};
    return d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
}

rf::Vector3 DecorationChunk::lo_vec() const
{
    return {lo[0], lo[1], lo[2]};
}

rf::Vector3 DecorationChunk::hi_vec() const
{
    return {hi[0], hi[1], hi[2]};
}

DecorationFrameStats& alpine_terrain_decorations_frame_stats()
{
    if (g_stats.frame != rf::frame_count) {
        g_stats = {};
        g_stats.frame = rf::frame_count;
    }
    return g_stats;
}
