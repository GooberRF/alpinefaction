#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <common/terrain/alpine_terrain.h>
#include <common/utils/string-utils.h>
#include <xlog/xlog.h>
#include "level.h"
#include "mesh.h"
#include "mfc_types.h"
#include "terrain_build.h"
#include "terrain_decorations.h"
#include "terrain_preview.h"
#include "vtypes.h"

namespace at = alpine_terrain;

namespace
{

// The viewport draws at most this many instances, the nearest ones within this reach.
constexpr std::size_t max_drawn_instances = 400;
constexpr float preview_reach = 100.0f;
// Placement work per paint before the rest waits for the next one
constexpr double build_budget_ms = 25.0;
constexpr int mesh_loads_per_paint = 1;
// Paints after which a chunk another view showed counts as idle again
constexpr uint32_t shown_lapse_paints = 64;
// Absurd geometry must not blow a chunk's light reach out to cover the level.
constexpr float max_mesh_radius = 1000.0f;

bool g_visible = true;

// ─── Meshes ─────────────────────────────────────────────────────────────────

struct MeshEntry
{
    EditorVMesh* vmesh = nullptr; // null: could not be loaded
    float radius = 0.0f;          // bounding sphere radius about the mesh origin
    uint32_t last_used = 0;
};

// By lowercased file name
std::unordered_map<std::string, MeshEntry> g_meshes;
int g_loads_left = mesh_loads_per_paint;

MeshEntry load_mesh(const std::string& name)
{
    MeshEntry e;
    // Reserved; the loader would animate a .vfx mid-paint.
    if (at::decoration_mesh_is_vfx(name.c_str(), name.size())) {
        xlog::warn("[Terrain] animated decoration mesh '{}' is not supported yet", name);
        return e;
    }
    // A missing file must not reach the loader.
    rf::File file;
    if (!file.open(name.c_str())) {
        xlog::warn("[Terrain] decoration mesh '{}' not found", name);
        return e;
    }
    EditorVMesh* vm = mesh_load_vmesh_file(name.c_str());
    if (vm && (vmesh_get_type(vm) != VMESH_TYPE_STATIC || !vm->instance)) {
        vmesh_free(vm);
        vm = nullptr;
    }
    if (!vm) {
        xlog::warn("[Terrain] decoration mesh '{}' could not be loaded", name);
        return e;
    }
    float center[3] = {};
    float radius = 0.0f;
    vmesh_get_bound_sphere(vm, center, &radius);
    const float offset = std::sqrt(center[0] * center[0] + center[1] * center[1] + center[2] * center[2]);
    e.vmesh = vm;
    e.radius = std::isfinite(radius + offset) ? std::clamp(radius + offset, 0.0f, max_mesh_radius) : max_mesh_radius;
    return e;
}

// A name typed partway is never loaded, nor is a .vfx (load_mesh turns it away).
bool loadable(const std::string& name)
{
    return !name.empty() && at::decoration_mesh_valid(name.c_str(), name.size());
}

// The entry for loadable `name`, loading it when `may_load` or the paint's load budget allows; null while it
// waits.
MeshEntry* find_mesh(const std::string& name, bool may_load)
{
    std::string key = string_to_lower(name);
    auto it = g_meshes.find(key);
    if (it == g_meshes.end()) {
        if (!may_load && g_loads_left <= 0) return nullptr;
        if (!may_load) g_loads_left--;
        it = g_meshes.emplace(std::move(key), load_mesh(name)).first;
    }
    return &it->second;
}

void free_mesh(MeshEntry& e)
{
    if (e.vmesh) vmesh_free(e.vmesh);
    e.vmesh = nullptr;
}

// ─── Instances ──────────────────────────────────────────────────────────────

struct Instance
{
    Vector3 pos;
    Matrix3 orient; // scaled
    float scale;
    uint8_t deco;
};

struct Chunk
{
    bool built = false;
    uint32_t last_used = 0;
    // Views whose latest paint had the chunk in reach; a chunk another view still shows is not idle
    uint32_t shown_in = 0;
    // Room under the instance budget that did not hold the chunk whole; tried again once there is more
    std::size_t too_big_for = 0;
    Vector3 lo, hi; // instance origins
    std::vector<Instance> instances;
};

// One terrain's instances by chunk, built as the camera nears them and dropped when what places them
// changes.
struct Cache
{
    const DedTerrain* owner = nullptr;
    int uid = 0;
    std::shared_ptr<const TerrainGrid> grid;
    Vector3 pos;
    float cell_size = 0.0f;
    float height_min = 0.0f;
    float height_range = 0.0f;
    uint32_t edge = 0;
    std::size_t layer_count = 0;
    std::vector<DedTerrainDecoration> decorations;
    std::vector<Chunk> chunks;
};

std::vector<std::unique_ptr<Cache>> g_caches;
// Instances held across every cache, kept within the game's level budget
std::size_t g_instances = 0;
uint32_t g_frame = 1;
double g_build_left_ms = build_budget_ms;
bool g_pending = false;
bool g_work_outstanding = false;
uint32_t g_idle_released_frame = 0;

// What a terrain's collect left for the paint's frame_end to build and draw
struct Pass
{
    Cache* cache;
    at::GridView v;
    at::DecorationView views[at::max_decorations];
    uint32_t count;
    float reach[at::max_decorations];
    const MeshEntry* meshes[at::max_decorations];
};
std::vector<Pass> g_passes;

struct Candidate
{
    float dist_sq;
    const Instance* inst;
    const MeshEntry* mesh;
    const Chunk* chunk;
};
std::vector<Candidate> g_candidates;

struct ChunkOrder
{
    float dist_sq;
    uint32_t pass;
    uint32_t k;
};
// Every terrain's chunks in reach, nearest first, so the instance budget goes to those nearest the camera
std::vector<ChunkOrder> g_chunk_order;

uint32_t painting_view_bit()
{
    return painting_view_index >= 0 && painting_view_index < std::numeric_limits<uint32_t>::digits
               ? 1u << painting_view_index
               : 0u;
}

void release_chunk(Chunk& c)
{
    // An unbuilt chunk's instances were never counted: a placement cut short by out of memory
    if (c.built) g_instances -= c.instances.size();
    c.instances.clear();
    c.instances.shrink_to_fit();
    c.built = false;
    c.shown_in = 0;
    c.too_big_for = 0;
}

void release_cache(Cache& c)
{
    for (Chunk& ch : c.chunks) release_chunk(ch);
}

void release_terrain(const DedTerrain* terrain)
{
    for (auto& c : g_caches) {
        if (c->owner == terrain) release_cache(*c);
    }
}

// Frees built chunks outside this paint's reach that no other view shows; true if that freed any. At most once
// per paint: every terrain marks its reach before any chunk is built.
bool release_idle_chunks()
{
    if (g_idle_released_frame == g_frame) return false;
    g_idle_released_frame = g_frame;
    const std::size_t before = g_instances;
    const uint32_t others = ~painting_view_bit();
    for (auto& c : g_caches) {
        for (Chunk& ch : c->chunks) {
            // A view that stops painting (hidden, layout change) keeps its bits, so they lapse after a while
            const bool shown = (ch.shown_in & others) && g_frame - ch.last_used <= shown_lapse_paints;
            if (ch.built && ch.last_used != g_frame && !shown) release_chunk(ch);
        }
    }
    return g_instances < before;
}

Cache& sync_cache(const DedTerrain& t, const DedTerrainData& d)
{
    Cache* c = nullptr;
    for (auto& p : g_caches) {
        if (p->owner == &t) c = p.get();
    }
    if (!c) {
        g_caches.push_back(std::make_unique<Cache>());
        c = g_caches.back().get();
        c->owner = &t;
    }
    const uint32_t edge = terrain_effective_chunk_cells(d);
    const uint32_t count = terrain_chunk_count(d);
    if (c->grid != d.grid || c->uid != t.uid || c->pos.x != t.pos.x || c->pos.y != t.pos.y || c->pos.z != t.pos.z ||
        c->cell_size != d.cell_size || c->height_min != d.height_min || c->height_range != d.height_range ||
        c->edge != edge || c->chunks.size() != count || c->layer_count != d.layers.size() ||
        c->decorations != d.decorations) {
        release_cache(*c);
        c->chunks.assign(count, Chunk{});
        c->grid = d.grid;
        c->uid = t.uid;
        c->pos = t.pos;
        c->cell_size = d.cell_size;
        c->height_min = d.height_min;
        c->height_range = d.height_range;
        c->edge = edge;
        c->layer_count = d.layers.size();
        c->decorations = d.decorations;
    }
    return *c;
}

// Places chunk k's instances as the game does, decoration by decoration; false when the paint's time is
// spent. A chunk the instance budget cannot hold whole stays unbuilt.
bool build_chunk(Cache& c, const at::GridView& v, const at::DecorationView* views, uint32_t count, uint32_t k)
{
    if (g_build_left_ms <= 0.0) return false;
    constexpr std::size_t cap = at::max_level_decoration_instances;
    Chunk& ch = c.chunks[k];
    if (g_instances >= cap || ch.too_big_for > 0) release_idle_chunks();
    std::size_t room = cap - std::min(cap, g_instances);
    if (room <= ch.too_big_for) return true;

    LARGE_INTEGER start;
    QueryPerformanceCounter(&start);
    const at::TexelRect r =
        at::chunk_texel_rect(at::chunk_rect(at::cells(v.nx), at::cells(v.nz), c.edge, k), v.weight_res_mul);
    // False when the chunk does not fit in `room`
    auto place = [&]() {
        ch.instances.clear();
        ch.lo = {1e30f, 1e30f, 1e30f};
        ch.hi = {-1e30f, -1e30f, -1e30f};
        uint32_t deco = 0;
        auto add = [&](const at::DecorationInstance& di) {
            const float s = di.scale;
            Instance inst;
            inst.pos = {di.pos[0], di.pos[1], di.pos[2]};
            inst.orient.rvec = {di.rvec[0] * s, di.rvec[1] * s, di.rvec[2] * s};
            inst.orient.uvec = {di.uvec[0] * s, di.uvec[1] * s, di.uvec[2] * s};
            inst.orient.fvec = {di.fvec[0] * s, di.fvec[1] * s, di.fvec[2] * s};
            inst.scale = s;
            inst.deco = static_cast<uint8_t>(deco);
            ch.instances.push_back(inst);
            ch.lo = {std::min(ch.lo.x, inst.pos.x), std::min(ch.lo.y, inst.pos.y), std::min(ch.lo.z, inst.pos.z)};
            ch.hi = {std::max(ch.hi.x, inst.pos.x), std::max(ch.hi.y, inst.pos.y), std::max(ch.hi.z, inst.pos.z)};
            return true;
        };
        // One over the room tells a chunk that does not fit from one that fills it exactly.
        at::DecorationBudget budget;
        budget.instances = static_cast<uint32_t>(room + 1);
        for (; deco < count && !budget.spent(); deco++) {
            at::for_each_decoration_instance(v, views[deco], at::decoration_seed(c.uid, deco), r, budget, add);
        }
        return budget.instances != 0;
    };
    bool fits = place();
    // Chunks out of reach may hold the room it lacks.
    if (!fits && release_idle_chunks()) {
        room = cap - std::min(cap, g_instances);
        fits = place();
    }
    if (!fits) {
        ch.instances.clear();
        ch.instances.shrink_to_fit();
        ch.too_big_for = room;
    }
    else {
        ch.instances.shrink_to_fit();
        g_instances += ch.instances.size();
        ch.built = true;
    }
    g_build_left_ms -= elapsed_ms(start);
    return true;
}

void collect(const DedTerrain& t, const DedTerrainData& d)
{
    const TerrainGrid& g = *d.grid;
    at::DecorationView views[at::max_decorations];
    const uint32_t count = terrain_decoration_views(d, g, views);
    float reach[at::max_decorations];
    const MeshEntry* meshes[at::max_decorations] = {};
    float max_reach = -1.0f;
    bool active = false;
    for (uint32_t k = 0; k < count; k++) {
        reach[k] = -1.0f;
        if (!at::decoration_active(views[k]) || !loadable(d.decorations[k].mesh)) continue;
        active = true;
        MeshEntry* e = find_mesh(d.decorations[k].mesh, false);
        if (!e) {
            g_pending = true;
            continue;
        }
        e->last_used = g_frame;
        if (!e->vmesh) continue;
        meshes[k] = e;
        reach[k] = std::min(d.decorations[k].draw_distance, preview_reach);
        max_reach = std::max(max_reach, reach[k]);
    }
    if (!active) {
        release_terrain(&t);
        return;
    }
    if (max_reach < 0.0f) return;

    Cache& c = sync_cache(t, d);
    Pass& p = g_passes.emplace_back();
    p.cache = &c;
    p.v = terrain_grid_view(t.pos, d, g);
    p.count = count;
    std::copy_n(views, count, p.views);
    std::copy_n(reach, count, p.reach);
    std::copy_n(meshes, count, p.meshes);
    const at::GridView& v = p.v;
    const uint32_t pass = static_cast<uint32_t>(g_passes.size() - 1);
    const uint32_t cx = at::cells(v.nx), cz = at::cells(v.nz);
    const float cam[3] = {ed_cam_pos[0], ed_cam_pos[1], ed_cam_pos[2]};
    for (uint32_t k = 0; k < c.chunks.size(); k++) {
        const at::ChunkRect r = at::chunk_rect(cx, cz, c.edge, k);
        const float x0 = v.origin[0] + r.x0 * v.cell_size, x1 = v.origin[0] + r.x1 * v.cell_size;
        const float z0 = v.origin[2] + r.z0 * v.cell_size, z1 = v.origin[2] + r.z1 * v.cell_size;
        const float dx = std::max({x0 - cam[0], 0.0f, cam[0] - x1});
        const float dz = std::max({z0 - cam[2], 0.0f, cam[2] - z1});
        if (dx * dx + dz * dz > max_reach * max_reach) continue;
        // In use before any is built, so making room frees only chunks out of reach
        c.chunks[k].last_used = g_frame;
        g_chunk_order.push_back({dx * dx + dz * dz, pass, k});
    }
}

// Builds the chunks every terrain has in reach, nearest first, and gathers their instances in reach.
void place_collected()
{
    std::sort(g_chunk_order.begin(), g_chunk_order.end(),
              [](const ChunkOrder& a, const ChunkOrder& b) { return a.dist_sq < b.dist_sq; });
    const float cam[3] = {ed_cam_pos[0], ed_cam_pos[1], ed_cam_pos[2]};
    for (const ChunkOrder& order : g_chunk_order) {
        const Pass& p = g_passes[order.pass];
        Chunk& ch = p.cache->chunks[order.k];
        if (!ch.built && !build_chunk(*p.cache, p.v, p.views, p.count, order.k)) {
            g_pending = true;
            continue;
        }
        // Unbuilt: over the budget, or a placement cut short by out of memory
        if (!ch.built) continue;
        for (const Instance& inst : ch.instances) {
            const float rk = p.reach[inst.deco];
            if (rk < 0.0f) continue;
            const float ex = inst.pos.x - cam[0], ey = inst.pos.y - cam[1], ez = inst.pos.z - cam[2];
            const float dist_sq = ex * ex + ey * ey + ez * ez;
            if (dist_sq <= rk * rk) g_candidates.push_back({dist_sq, &inst, p.meshes[inst.deco], &ch});
        }
    }
}

// vmesh_render with a scaled orient: each submesh gets the position that puts its centre where the engine's
// camera transform needs it (see vmesh_render_submesh). Placement is exact, but with a scale other than 1 the
// engine picks LOD and lights from that moved position, so those are approximate.
void render_scaled(EditorVMesh* vm, const Instance& inst)
{
    const auto* v3d = static_cast<const EditorV3d*>(vm->instance);
    if (!v3d || v3d->num_meshes <= 0 || !v3d->meshes) return;
    const float k = 1.0f / (inst.scale * inst.scale);
    const Matrix3& o = inst.orient;
    EditorRenderParams params = editor_mesh_render_params();
    for (int i = 0; i < v3d->num_meshes; i++) {
        Vector3 pos = inst.pos;
        if (inst.scale != 1.0f) {
            const EditorVifLodMesh* lod = v3d->meshes[i].lod_mesh;
            const Vector3 c = lod ? lod->center : Vector3{};
            const float mc[3] = {o.rvec.x * c.x + o.uvec.x * c.y + o.fvec.x * c.z,
                                 o.rvec.y * c.x + o.uvec.y * c.y + o.fvec.y * c.z,
                                 o.rvec.z * c.x + o.uvec.z * c.y + o.fvec.z * c.z};
            float* p = &pos.x;
            for (int a = 0; a < 3; a++) {
                const float centre = p[a] + mc[a];
                p[a] = ed_cam_pos[a] + (centre - ed_cam_pos[a]) * k - mc[a];
            }
        }
        vmesh_render_submesh(vm, i, &pos, &o, &params);
    }
}

// The nearest candidates, a chunk's at a time under the lights reaching it.
void draw_candidates()
{
    const std::size_t n = std::min(g_candidates.size(), max_drawn_instances);
    auto nearer = [](const Candidate& a, const Candidate& b) { return a.dist_sq < b.dist_sq; };
    if (n < g_candidates.size()) {
        std::nth_element(g_candidates.begin(), g_candidates.begin() + n, g_candidates.end(), nearer);
    }
    std::sort(g_candidates.begin(), g_candidates.begin() + n, [](const Candidate& a, const Candidate& b) {
        return a.chunk != b.chunk ? std::less<const Chunk*>{}(a.chunk, b.chunk) : a.mesh < b.mesh;
    });
    set_draw_color(0xff, 0xff, 0xff, 0xff);
    for (std::size_t i = 0; i < n;) {
        const Chunk& ch = *g_candidates[i].chunk;
        std::size_t end = i;
        float reach = 0.0f;
        for (; end < n && g_candidates[end].chunk == &ch; end++) {
            reach = std::max(reach, g_candidates[end].mesh->radius * g_candidates[end].inst->scale);
        }
        const Vector3 center{(ch.lo.x + ch.hi.x) * 0.5f, (ch.lo.y + ch.hi.y) * 0.5f, (ch.lo.z + ch.hi.z) * 0.5f};
        const float hx = ch.hi.x - center.x, hy = ch.hi.y - center.y, hz = ch.hi.z - center.z;
        room_setup(nullptr, &center, std::sqrt(hx * hx + hy * hy + hz * hz) + reach, 1, 1);
        for (; i < end; i++) render_scaled(g_candidates[i].mesh->vmesh, *g_candidates[i].inst);
        room_cleanup();
    }
}

// Drops the caches of terrains that left the level and the meshes no decoration names, the dialog's staged
// ones included.
void release_unused(CDedLevel& level, const DedTerrainData* staged)
{
    const auto& terrains = level.GetAlpineLevelProperties().terrain_objects;
    for (auto it = g_caches.begin(); it != g_caches.end();) {
        if (std::find(terrains.begin(), terrains.end(), (*it)->owner) == terrains.end()) {
            release_cache(**it);
            it = g_caches.erase(it);
        }
        else {
            ++it;
        }
    }
    if (g_meshes.empty()) return;
    std::unordered_set<std::string> named;
    auto add_names = [&](const DedTerrainData& d) {
        for (const DedTerrainDecoration& deco : d.decorations) {
            if (!deco.mesh.empty()) named.insert(string_to_lower(deco.mesh));
        }
    };
    for (const DedTerrain* t : terrains) add_names(t->data);
    if (staged) add_names(*staged);
    for (auto it = g_meshes.begin(); it != g_meshes.end();) {
        if (it->second.last_used != g_frame && !named.contains(it->first)) {
            free_mesh(it->second);
            it = g_meshes.erase(it);
        }
        else {
            ++it;
        }
    }
}

// What a paint collected points into caches about to be released.
void drop_collected()
{
    g_chunk_order.clear();
    g_passes.clear();
    g_candidates.clear();
}

void release_all_caches()
{
    drop_collected();
    for (auto& c : g_caches) release_cache(*c);
}

} // namespace

void terrain_decorations_collect(const DedTerrain& terrain, const DedTerrainData& data)
{
    if (!g_visible || !terrain_view_draws_solid()) return;
    if (data.decorations.empty() || !data.grid || data.layers.empty()) {
        release_terrain(&terrain);
        return;
    }
    try {
        collect(terrain, data);
    }
    catch (const std::bad_alloc&) {
        xlog::error("[Terrain] out of memory placing the decoration preview");
    }
}

void terrain_decorations_frame_end(CDedLevel& level, const DedTerrainData* staged)
{
    if (!g_chunk_order.empty()) {
        try {
            place_collected();
        }
        catch (const std::bad_alloc&) {
            xlog::error("[Terrain] out of memory placing the decoration preview");
        }
    }
    if (!g_candidates.empty()) {
        try {
            draw_candidates();
        }
        catch (const std::bad_alloc&) {
            xlog::error("[Terrain] out of memory drawing the decoration preview");
        }
    }
    drop_collected();
    // Only a paint that draws the preview marks what it uses.
    if (terrain_view_draws_solid() && (!g_caches.empty() || !g_meshes.empty())) {
        try {
            release_unused(level, staged);
        }
        catch (const std::bad_alloc&) {
            xlog::error("[Terrain] out of memory releasing unused decoration previews");
        }
    }
    // Chunks or meshes left for the budget: this view paints again on the next idle tick.
    if (g_pending) editor_view_mark_repaint(editor_view_at(painting_view_index));
    g_work_outstanding = g_work_outstanding || g_pending;
    g_pending = false;
    if (const uint32_t bit = painting_view_bit()) {
        for (auto& c : g_caches) {
            for (Chunk& ch : c->chunks) {
                ch.shown_in = ch.last_used == g_frame ? ch.shown_in | bit : ch.shown_in & ~bit;
            }
        }
    }
    g_loads_left = mesh_loads_per_paint;
    g_build_left_ms = build_budget_ms;
    g_frame++;
}

bool terrain_decorations_take_pending_work()
{
    const bool pending = g_work_outstanding;
    g_work_outstanding = false;
    return pending;
}

void terrain_decorations_invalidate(const DedTerrain* terrain, const TerrainCellRect* cells)
{
    for (auto& c : g_caches) {
        if (c->owner != terrain) continue;
        if (!c->grid || !c->edge) return;
        const uint32_t cx = at::cells(c->grid->nx), cz = at::cells(c->grid->nz);
        for (uint32_t k = 0; k < c->chunks.size(); k++) {
            const at::ChunkRect r = at::chunk_rect(cx, cz, c->edge, k);
            // Surface normals read the cells either side.
            if (!cells || (r.x0 <= cells->x1 && cells->x0 <= r.x1 && r.z0 <= cells->z1 && cells->z0 <= r.z1)) {
                release_chunk(c->chunks[k]);
            }
        }
        return;
    }
}

void terrain_decorations_rebind_grid(const DedTerrain* terrain, const TerrainGrid* old_grid,
                                     const std::shared_ptr<const TerrainGrid>& new_grid)
{
    for (auto& c : g_caches) {
        if (c->owner == terrain && c->grid.get() == old_grid) c->grid = new_grid;
    }
}

void terrain_decorations_forget(const DedTerrain* terrain)
{
    drop_collected();
    for (auto it = g_caches.begin(); it != g_caches.end(); ++it) {
        if ((*it)->owner == terrain) {
            release_cache(**it);
            g_caches.erase(it);
            return;
        }
    }
}

void terrain_decorations_level_reset()
{
    release_all_caches();
    g_caches.clear();
    for (auto& [name, e] : g_meshes) free_mesh(e);
    g_meshes.clear();
}

bool terrain_decorations_visible()
{
    return g_visible;
}

void terrain_decorations_set_visible(bool visible)
{
    g_visible = visible;
    if (!visible) release_all_caches();
}

EditorVMesh* terrain_decorations_mesh(const std::string& name)
{
    if (!loadable(name)) return nullptr;
    MeshEntry* e = find_mesh(name, true);
    return e ? e->vmesh : nullptr;
}
