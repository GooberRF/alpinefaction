#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <format>
#include <map>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <patch_common/CodeInjection.h>
#include <patch_common/FunHook.h>
#include <patch_common/MemUtils.h>
#include <common/terrain/alpine_terrain.h>
#include <xlog/xlog.h>
#include "alpine_lightmaps.h"
#include "alpine_obj.h"
#include "brush_import.h"
#include "face_list_cache.h"
#include "headless_bake.h"
#include "level.h"
#include "mfc_types.h"
#include "terrain.h"
#include "terrain_build.h"
#include "vtypes.h"

namespace at = alpine_terrain;

namespace
{

// built_room_uids entry of a chunk that has faces but no compiled room
constexpr int32_t missing_room_uid = -2;

template<typename F>
void for_each_face_vertex(const GFace* face, F&& f)
{
    const GFaceVertex* head = face->edge_loop;
    for (const GFaceVertex* fv = head; fv;) {
        if (fv->vertex) f(fv->vertex->pos);
        fv = fv->next;
        if (fv == head) break;
    }
}

DedTerrain* find_terrain(CDedLevel& level, int32_t uid)
{
    for (DedTerrain* t : level.GetAlpineLevelProperties().terrain_objects) {
        if (t && t->uid == uid) return t;
    }
    return nullptr;
}

// ─── Fingerprints (alpine_terrain.h) ────────────────────────────────────────
// Computed from the terrain as it is, position included, so any edit or move makes it stale.

uint64_t terrain_geometry_fingerprint(const DedTerrain& t)
{
    const DedTerrainData& d = t.data;
    if (!d.grid || d.layers.empty()) return 0;
    return at::geometry_fingerprint(terrain_grid_view(t.pos, d, *d.grid), d.chunk_cells);
}

uint64_t terrain_material_fingerprint(const DedTerrain& t)
{
    const DedTerrainData& d = t.data;
    if (!d.grid || d.layers.empty()) return 0;
    const char* names[at::max_layers] = {};
    for (std::size_t i = 0; i < d.layers.size() && i < at::max_layers; i++) names[i] = d.layers[i].texture.c_str();
    return at::material_fingerprint(terrain_grid_view(t.pos, d, *d.grid), names, d.underside_texture.c_str());
}

// ─── Temporary chunk brushes ────────────────────────────────────────────────

struct TempChunk
{
    BrushNode* brush;
    int32_t brush_uid;
    int32_t terrain_uid;
    uint32_t chunk;
};

struct BuiltTerrain
{
    int32_t uid;
    uint64_t geometry_fingerprint;
    uint64_t material_fingerprint;
    uint32_t chunk_count;
    std::vector<uint32_t> failed_chunks;
};

std::vector<TempChunk> g_temp_chunks;
std::vector<BuiltTerrain> g_built_terrains;
// Warnings found while inserting: the driver clears the log on every later tick, so they are reported
// when the build finishes.
std::vector<std::string> g_build_notes;
uint64_t g_build_triangles = 0;

void unlink_brush(CDedLevel& level, BrushNode* brush)
{
    if (brush->next == brush) {
        level.brush_list = nullptr;
    }
    else {
        brush->prev->next = brush->next;
        brush->next->prev = brush->prev;
        if (level.brush_list == brush) level.brush_list = brush->next;
    }
    brush->next = nullptr;
    brush->prev = nullptr;
}

// Only while no build is running: the build dialog's work list points at the brushes until the
// driver's finish or cancel empties it. Also on a build's first tick, before insert_temp_brushes
// adds this build's own.
int remove_temp_brushes(CDedLevel& level, std::size_t first = 0)
{
    if (g_temp_chunks.size() <= first) return 0;
    std::unordered_set<BrushNode*> live;
    if (BrushNode* head = level.brush_list) {
        BrushNode* node = head;
        do {
            live.insert(node);
            node = node->next;
        } while (node && node != head);
    }
    int removed = 0;
    for (std::size_t i = first; i < g_temp_chunks.size(); i++) {
        const TempChunk& tc = g_temp_chunks[i];
        if (!live.count(tc.brush) || tc.brush->uid != tc.brush_uid) continue;
        unlink_brush(level, tc.brush);
        BrushNode::destroy(tc.brush);
        removed++;
    }
    g_temp_chunks.resize(first);
    return removed;
}

struct TerrainTextures
{
    int layer[at::max_layers];
    int underside;
};

int resolve_texture(const std::string& name, const DedTerrain& t, int fallback, std::vector<std::string>& notes)
{
    if (name.empty()) return fallback;
    const int handle = alpine_dlg_resolve_bitmap(name.c_str());
    if (handle >= 0) return handle;
    notes.push_back(
        std::format("{}: texture '{}' was not found; a fallback texture is used.", terrain_label(t), name));
    return fallback;
}

TerrainTextures resolve_textures(const DedTerrain& t, std::vector<std::string>& notes)
{
    const DedTerrainData& d = t.data;
    TerrainTextures tex{};
    const int base =
        resolve_texture(d.layers[0].texture, t, alpine_dlg_resolve_bitmap(at::default_layer_texture), notes);
    for (std::size_t i = 0; i < at::max_layers; i++) {
        tex.layer[i] = (i > 0 && i < d.layers.size()) ? resolve_texture(d.layers[i].texture, t, base, notes) : base;
    }
    tex.underside = resolve_texture(d.underside_texture, t, base, notes);
    return tex;
}

at::GridView make_view(const DedTerrain& t, const TerrainGrid& g)
{
    return terrain_grid_view(t.pos, t.data, g);
}

// Emits every chunk of a prepared terrain as a brush solid with its vertices relative to `base` and
// hands it to sink(k, solid, triangles), which takes ownership and returns whether the brush went in.
// Chunks that cannot be built are listed in `failed`, with a note saying why.
template<typename Sink>
void build_chunk_solids(const DedTerrain& t, const Vector3& base, std::vector<std::string>& notes,
                        std::vector<uint32_t>& failed, Sink&& sink)
{
    const std::shared_ptr<const TerrainGrid> grid = t.data.grid;
    const at::GridView view = make_view(t, *grid);
    const TerrainTextures tex = resolve_textures(t, notes);
    const uint32_t cx = at::cells(grid->nx), cz = at::cells(grid->nz);
    const uint32_t edge = terrain_effective_chunk_cells(t.data);
    const uint32_t count = at::chunk_count(cx, cz, edge);
    const std::string who = terrain_label(t);

    std::vector<int> slot_index;
    std::vector<Vector3> positions;
    std::vector<BrushSolidFace> faces;
    for (uint32_t k = 0; k < count; k++) {
        const at::ChunkRect rect = at::chunk_rect(cx, cz, edge, k);
        slot_index.assign(at::chunk_slot_count(rect), -1);
        positions.clear();
        faces.clear();
        uint64_t triangles = 0;
        at::emit_chunk(view, rect, [&](const at::EmitFace& f) {
            BrushSolidFace bf{};
            bf.bitmap_id = f.material == at::material_underside ? tex.underside : tex.layer[f.material];
            bf.count = static_cast<int>(f.count);
            for (uint32_t i = 0; i < f.count; i++) {
                int& index = slot_index[f.slot[i]];
                if (index < 0) {
                    float p[3];
                    at::slot_position(view, rect, f.slot[i], p);
                    index = static_cast<int>(positions.size());
                    positions.emplace_back(p[0] - base.x, p[1] - base.y, p[2] - base.z);
                }
                bf.v[i] = index;
                bf.uv[i][0] = f.uv[i][0];
                bf.uv[i][1] = f.uv[i][1];
            }
            faces.push_back(bf);
            triangles += f.count - 2;
        });
        if (faces.empty()) continue;
        if (positions.size() >= at::max_room_vertices) {
            notes.push_back(std::format("{}: chunk {} has {} vertices, but a room holds at most {}; it "
                                        "was left out.",
                                        who, k, positions.size(), at::max_room_vertices - 1));
            failed.push_back(k);
            continue;
        }

        int dropped = 0;
        GSolid* solid = build_brush_solid(positions, faces, dropped);
        if (dropped > 0) {
            notes.push_back(std::format("{}: chunk {} lost {} degenerate face(s).", who, k, dropped));
        }
        if (!solid || !sink(k, solid, triangles)) {
            notes.push_back(std::format("{}: chunk {} could not be built.", who, k));
            failed.push_back(k);
        }
    }
}

void insert_terrain(CDedLevel& level, DedTerrain& t)
{
    terrain_prepare(t);
    const uint32_t count = terrain_chunk_count(t.data);
    // No allocation may fail between a brush entering the level and it being tracked
    g_temp_chunks.reserve(g_temp_chunks.size() + count);
    const uint64_t geometry = terrain_geometry_fingerprint(t);
    const uint64_t material = terrain_material_fingerprint(t);

    std::vector<uint32_t> failed;
    // World-space vertices under a brush at the origin: the compiled positions are the emitted ones
    // bit for bit, which the build mapping hash relies on.
    build_chunk_solids(t, Vector3{}, g_build_notes, failed, [&](uint32_t k, GSolid* solid, uint64_t triangles) {
        BrushNode* brush = insert_detail_solid_brush(&level, solid, Vector3{}, identity_orient);
        if (!brush) return false;
        brush->state = BRUSH_STATE_HIDDEN;
        g_temp_chunks.push_back({brush, brush->uid, t.uid, k});
        g_build_triangles += triangles;
        return true;
    });
    g_built_terrains.push_back({t.uid, geometry, material, count, std::move(failed)});
}

void insert_temp_brushes(CDedLevel& level)
{
    remove_temp_brushes(level);
    g_built_terrains.clear();
    g_build_notes.clear();
    g_build_triangles = 0;
    for (DedTerrain* t : level.GetAlpineLevelProperties().terrain_objects) {
        if (!t) continue;
        const std::size_t first_chunk = g_temp_chunks.size();
        const uint64_t triangles = g_build_triangles;
        try {
            insert_terrain(level, *t);
        }
        catch (const std::bad_alloc&) {
            remove_temp_brushes(level, first_chunk);
            g_build_triangles = triangles;
            g_build_notes.push_back(std::format("{}: out of memory while building its chunks; it was left out.",
                                                terrain_label(*t)));
        }
    }
}

// ─── Build finish: chunk -> room capture and validation ─────────────────────

struct ChunkCounts
{
    int no_parent = 0;
    int outside = 0;
    int sky = 0;
    int split = 0;
    int missing = 0;
};

bool point_in_box(const Vector3& p, const Vector3& lo, const Vector3& hi)
{
    constexpr float eps = 0.001f;
    return p.x >= lo.x - eps && p.x <= hi.x + eps && p.y >= lo.y - eps && p.y <= hi.y + eps &&
           p.z >= lo.z - eps && p.z <= hi.z + eps;
}

bool boxes_overlap(const Vector3& a_lo, const Vector3& a_hi, const Vector3& b_lo, const Vector3& b_hi)
{
    return a_lo.x <= b_hi.x && a_hi.x >= b_lo.x && a_lo.y <= b_hi.y && a_hi.y >= b_lo.y && a_lo.z <= b_hi.z &&
           a_hi.z >= b_lo.z;
}

// Roughly the reach of stock weapon craters with the Big Craters mutator
constexpr float geoable_floor_warn_depth = 8.0f;

void check_geo_regions(CDedLevel& level, const DedTerrain& t, const std::vector<at::ChunkRect>& geo_chunks)
{
    const DedTerrainData& d = t.data;
    const float lo_y = at::bottom_y(make_view(t, *d.grid));
    const float hi_y = at::world_y(t.pos.y, UINT16_MAX, d.height_min, d.height_range);
    const auto& regions = level.geo_regions;
    for (int i = 0; i < regions.get_size(); i++) {
        auto* r = static_cast<DedGeoRegion*>(regions.data_ptr[i]);
        if (!r || r->type != DedObjectType::DED_GEO_REGION) continue;
        const GeoRegionShape shape = r->shape;
        Vector3 half;
        if (shape == GeoRegionShape::sphere) {
            const float radius = r->radius;
            half = {radius, radius, radius};
        }
        else if (shape == GeoRegionShape::box) {
            const float ex = r->width * 0.5f;
            const float ey = r->height * 0.5f;
            const float ez = r->depth * 0.5f;
            const Matrix3& m = r->orient;
            half = {std::abs(m.rvec.x) * ex + std::abs(m.uvec.x) * ey + std::abs(m.fvec.x) * ez,
                    std::abs(m.rvec.y) * ex + std::abs(m.uvec.y) * ey + std::abs(m.fvec.y) * ez,
                    std::abs(m.rvec.z) * ex + std::abs(m.uvec.z) * ey + std::abs(m.fvec.z) * ez};
        }
        else {
            continue;
        }
        const Vector3 r_lo{r->pos.x - half.x, r->pos.y - half.y, r->pos.z - half.z};
        const Vector3 r_hi{r->pos.x + half.x, r->pos.y + half.y, r->pos.z + half.z};
        const bool hit = std::any_of(geo_chunks.begin(), geo_chunks.end(), [&](const at::ChunkRect& c) {
            const Vector3 lo{t.pos.x + static_cast<float>(c.x0) * d.cell_size, lo_y,
                             t.pos.z + static_cast<float>(c.z0) * d.cell_size};
            const Vector3 hi{t.pos.x + static_cast<float>(c.x1) * d.cell_size, hi_y,
                             t.pos.z + static_cast<float>(c.z1) * d.cell_size};
            return boxes_overlap(lo, hi, r_lo, r_hi);
        });
        if (hit) {
            terrain_report(std::format("{} is geoable but overlaps Geo Region {}; it cannot be carved there.",
                                       terrain_label(t), r->uid),
                           false);
        }
    }
}

void finish_build(CDedLevel& level)
{
    auto& props = level.GetAlpineLevelProperties();
    for (DedTerrain* t : props.terrain_objects) {
        if (!t) continue;
        t->data.built_room_uids.clear();
        t->data.built_geometry_fingerprint = 0;
        t->data.built_material_fingerprint = 0;
    }
    props.terrain_room_uids.clear();
    props.terrain_split_room_uids.clear();
    GSolid* solid = level.solid;
    if (!solid || (g_built_terrains.empty() && g_temp_chunks.empty())) return;

    // Compiled faces keep the face ids the driver's first tick gave the brush faces they were cloned
    // from, so each chunk's faces are found by the ids of its brush's faces.
    std::unordered_map<int, std::size_t> face_owner;
    for (std::size_t i = 0; i < g_temp_chunks.size(); i++) {
        const auto* geometry = static_cast<const GSolid*>(g_temp_chunks[i].brush->geometry);
        for (const GFace* f = geometry ? geometry->face_list_head : nullptr; f; f = f->next_solid) {
            if (f->face_id >= 0) face_owner[f->face_id] = i;
        }
    }
    std::vector<std::vector<std::pair<GRoom*, int>>> rooms(g_temp_chunks.size());
    for (GFace* f = solid->face_list_head; f; f = f->next_solid) {
        auto it = face_owner.find(f->face_id);
        if (it == face_owner.end() || !f->which_room) continue;
        auto& list = rooms[it->second];
        auto r = std::find_if(list.begin(), list.end(), [&](const auto& e) { return e.first == f->which_room; });
        if (r == list.end()) {
            list.emplace_back(f->which_room, 1);
        }
        else {
            r->second++;
        }
    }

    std::map<int32_t, ChunkCounts> counts;
    std::vector<GRoom*> chunk_room(g_temp_chunks.size(), nullptr);
    for (std::size_t i = 0; i < rooms.size(); i++) {
        const auto& list = rooms[i];
        auto best = std::max_element(list.begin(), list.end(),
                                     [](const auto& a, const auto& b) { return a.second < b.second; });
        if (best == list.end()) {
            counts[g_temp_chunks[i].terrain_uid].missing++;
            continue;
        }
        if (list.size() > 1) counts[g_temp_chunks[i].terrain_uid].split++;
        GRoom* room = best->first;
        groom_assign_uid_if_missing(*room);
        chunk_room[i] = room;
    }
    std::vector<int32_t> room_uids;
    for (const GRoom* room : chunk_room) {
        if (room) room_uids.push_back(room->uid);
    }
    std::sort(room_uids.begin(), room_uids.end());
    props.terrain_room_uids = std::move(room_uids);

    // The preview draws a split chunk whole, so its other rooms are hidden too when nothing else is in them.
    std::vector<int32_t> split_uids;
    for (std::size_t i = 0; i < rooms.size(); i++) {
        for (const auto& entry : rooms[i]) {
            GRoom* room = entry.first;
            if (room == chunk_room[i]) continue;
            bool terrain_only = true;
            for (const GFace* f = room->face_list_head; f && terrain_only; f = f->next_room) {
                terrain_only = face_owner.count(f->face_id) != 0;
            }
            if (!terrain_only) continue;
            groom_assign_uid_if_missing(*room);
            if (!props.is_terrain_room(room->uid)) {
                split_uids.push_back(room->uid);
            }
        }
    }
    std::sort(split_uids.begin(), split_uids.end());
    split_uids.erase(std::unique(split_uids.begin(), split_uids.end()), split_uids.end());
    props.terrain_split_room_uids = std::move(split_uids);

    for (const BuiltTerrain& bt : g_built_terrains) {
        if (DedTerrain* t = find_terrain(level, bt.uid)) {
            t->data.built_room_uids.assign(bt.chunk_count, at::no_room_uid);
            for (uint32_t k : bt.failed_chunks) t->data.built_room_uids[k] = missing_room_uid;
            t->data.built_geometry_fingerprint = bt.geometry_fingerprint;
            t->data.built_material_fingerprint = bt.material_fingerprint;
        }
    }
    for (std::size_t i = 0; i < g_temp_chunks.size(); i++) {
        const TempChunk& tc = g_temp_chunks[i];
        DedTerrain* t = find_terrain(level, tc.terrain_uid);
        if (!t || tc.chunk >= t->data.built_room_uids.size()) continue;
        t->data.built_room_uids[tc.chunk] = chunk_room[i] ? chunk_room[i]->uid : missing_room_uid;
    }

    // Collision reaches a detail room only through a parent room whose bbox the query touches.
    std::unordered_map<const GRoom*, std::vector<const GRoom*>> parents;
    for (GRoom* room : chunk_room) {
        if (room) parents[room];
    }
    for (int i = 0; i < solid->all_rooms.get_size(); i++) {
        const GRoom* p = solid->all_rooms.data_ptr[i];
        if (!p || p->is_detail) continue;
        for (int j = 0; j < p->detail_rooms.get_size(); j++) {
            auto it = parents.find(p->detail_rooms.data_ptr[j]);
            if (it != parents.end()) it->second.push_back(p);
        }
    }
    for (std::size_t i = 0; i < chunk_room.size(); i++) {
        const GRoom* room = chunk_room[i];
        if (!room) continue;
        ChunkCounts& c = counts[g_temp_chunks[i].terrain_uid];
        const auto& ps = parents[room];
        if (ps.empty()) {
            c.no_parent++;
            continue;
        }
        if (std::any_of(ps.begin(), ps.end(), [](const GRoom* p) { return p->is_sky; })) c.sky++;
        bool inside = true;
        for (const GFace* f = room->face_list_head; f && inside; f = f->next_room) {
            for_each_face_vertex(f, [&](const Vector3& v) {
                if (inside && std::none_of(ps.begin(), ps.end(), [&](const GRoom* p) {
                        return point_in_box(v, p->bbox_min, p->bbox_max);
                    })) {
                    inside = false;
                }
            });
        }
        if (!inside) c.outside++;
    }

    for (const std::string& note : g_build_notes) terrain_report(note, false);
    for (const auto& [uid, c] : counts) {
        DedTerrain* t = find_terrain(level, uid);
        const std::string who = t ? terrain_label(*t) : std::format("Terrain {}", uid);
        if (c.missing) {
            terrain_report(std::format("{}: {} chunk(s) produced no compiled room.", who, c.missing), false);
        }
        if (c.split) {
            terrain_report(std::format("{}: {} chunk(s) were split across several rooms.", who, c.split), false);
        }
        if (c.no_parent) {
            terrain_report(std::format("{}: {} chunk(s) are in no room, so nothing collides with them; enclose "
                                       "the terrain in air.",
                                       who, c.no_parent),
                           false);
        }
        if (c.outside) {
            terrain_report(std::format("{}: {} chunk(s) reach outside the bounding boxes of the rooms around "
                                       "them, where collision is skipped; extend the air around the terrain.",
                                       who, c.outside),
                           false);
        }
        if (c.sky) {
            terrain_report(std::format("{}: {} chunk(s) are in the sky room, which terrain does not support.", who,
                                       c.sky),
                           false);
        }
    }
    for (const BuiltTerrain& bt : g_built_terrains) {
        DedTerrain* t = find_terrain(level, bt.uid);
        if (!t || !(t->data.flags & at::flag_geoable)) continue;
        const DedTerrainData& d = t->data;
        const at::ChunkLayout layout = terrain_geo_chunk_layout(d);
        std::vector<at::ChunkRect> geo_chunks;
        uint16_t lowest = UINT16_MAX;
        for (uint32_t k = 0; k < at::layout_chunk_count(layout); k++) {
            if (!terrain_chunk_geoable(d, k)) continue;
            const at::ChunkRect r = at::chunk_rect(layout.cells_x, layout.cells_z, layout.edge, k);
            geo_chunks.push_back(r);
            for (uint32_t z = r.z0; z <= r.z1; z++) {
                for (uint32_t x = r.x0; x <= r.x1; x++) {
                    lowest = std::min(lowest, d.grid->heights[static_cast<std::size_t>(z) * d.grid->nx + x]);
                }
            }
        }
        if (geo_chunks.empty()) {
            terrain_report(std::format("{} is geoable, but none of its chunks are (Terrain Tools, Geoable Chunks); "
                                       "nothing on it can be carved.",
                                       terrain_label(*t)),
                           false);
            continue;
        }
        if (!props.rf2_style_geomod) {
            terrain_report(std::format("{} is geoable, but RF2-style geomod is off in Level Properties; it must be "
                                       "enabled for the terrain to be carved.",
                                       terrain_label(*t)),
                           false);
        }
        check_geo_regions(level, *t, geo_chunks);
        const float floor = at::world_y(t->pos.y, lowest, d.height_min, d.height_range) -
                            at::bottom_y(make_view(*t, *d.grid));
        if (floor < geoable_floor_warn_depth) {
            terrain_report(std::format("{}: the lowest point of its geoable chunks is only {:.1f} above its bottom, "
                                       "so craters may punch through there; raise Thickness in its properties.",
                                       terrain_label(*t), floor),
                           false);
        }
    }
    if (g_build_triangles > at::level_triangle_budget) {
        terrain_report(std::format("The level's terrains make {} triangles, over the budget of {}.", g_build_triangles,
                                   at::level_triangle_budget),
                       false);
    }
}

// ─── Hooks ──────────────────────────────────────────────────────────────────

// GeoBuild_Driver, once per idle tick: chunk brushes go in before its first tick, out once it clears build_running.
void __fastcall geobuild_driver_hooked(CDedLevel* level);
FunHook<decltype(geobuild_driver_hooked)> geobuild_driver_hook{0x004399b0, geobuild_driver_hooked};
void __fastcall geobuild_driver_hooked(CDedLevel* level)
{
    FaceListCacheWindow face_list_cache;
    const bool cancelling = level->build_cancelling();
    if (!cancelling && g_build_first_tick_pending) {
        try {
            insert_temp_brushes(*level);
        }
        catch (const std::bad_alloc&) {
            xlog::error("[Terrain] out of memory inserting terrain chunks");
        }
    }
    geobuild_driver_hook.call_target(level);
    if (level->build_running) return;
    try {
        if (!cancelling) finish_build(*level);
    }
    catch (const std::bad_alloc&) {
        terrain_report("Out of memory finishing the terrain build; rebuild before saving.", false);
    }
    try {
        remove_temp_brushes(*level);
    }
    catch (const std::bad_alloc&) {
        xlog::error("[Terrain] out of memory removing temporary chunk brushes; saving strips them");
    }
    g_built_terrains.clear();
    g_build_notes.clear();
}

// Sorted uids of the level solid's terrain rooms while FUN_004aa610 builds its surfaces, else null.
// They outlive a deleted terrain until the next Build Geometry, as its rooms do.
const std::vector<int32_t>* g_terrain_gate_uids = nullptr;

// The solid's faces grouped by surface index in list order: surface i owns faces[offsets[i], offsets[i + 1]).
struct SurfaceFaceBuckets
{
    bool built = false;
    std::vector<uint32_t> offsets;
    std::vector<GFace*> faces;
};

// Set for the duration of one FUN_004aa610 call, else null (also when building the buckets ran out of memory).
SurfaceFaceBuckets* g_surface_face_buckets = nullptr;

bool in_terrain_room(const GFace* face, const std::vector<int32_t>& uids)
{
    return face->which_room && std::binary_search(uids.begin(), uids.end(), face->which_room->uid);
}

// FUN_004aa610: the lightmap surface builder (thiscall on a GSolid, 4 stack arguments, RET 0x10),
// called by Calculate Lighting for the level solid (0x00448d8a) and for each mover brush (0x00448e0a).
int __fastcall surface_build_hooked(GSolid* solid, void* edx, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4);
FunHook<decltype(surface_build_hooked)> surface_build_hook{0x004aa610, surface_build_hooked};
int __fastcall surface_build_hooked(GSolid* solid, void* edx, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4)
{
    CDedLevel* level = CDedLevel::Get();
    const bool level_solid = level && solid && solid == level->solid;
    g_terrain_gate_uids = level_solid ? &level->GetAlpineLevelProperties().terrain_room_uids : nullptr;
    SurfaceFaceBuckets buckets;
    g_surface_face_buckets = &buckets;
    const int result = surface_build_hook.call_target(solid, edx, a1, a2, a3, a4);
    g_surface_face_buckets = nullptr;
    g_terrain_gate_uids = nullptr;
    return result;
}

// FUN_004aa610 keeps a surface index in a short and sizes its per-surface arrays at 0x10000 (0x0144ac28),
// so a surface past red_max_level_surfaces would write outside them. Faces past the limit keep surface -1.
CodeInjection surface_build_cap{
    0x004aa751, // a new surface is about to start; EBP = surfaces so far
    [](auto& regs) {
        if (static_cast<uint32_t>(regs.ebp) >= red_max_level_surfaces) regs.eip = 0x004aaa1c;
    },
};

void build_surface_face_buckets(SurfaceFaceBuckets& buckets, const GSolid* solid, int surface_count)
{
    auto& offsets = buckets.offsets;
    offsets.assign(surface_count + 1, 0);
    for (GFace* f = solid->face_list_head; f; f = f->next_solid) {
        if (f->surface_index >= 0 && f->surface_index < surface_count)
            offsets[f->surface_index + 1]++;
    }
    for (int i = 0; i < surface_count; i++) offsets[i + 1] += offsets[i];
    buckets.faces.resize(offsets[surface_count]);
    for (GFace* f = solid->face_list_head; f; f = f->next_solid) {
        if (f->surface_index >= 0 && f->surface_index < surface_count)
            buckets.faces[offsets[f->surface_index]++] = f;
    }
    // each offset now holds its bucket's end, which is the next bucket's start
    std::copy_backward(offsets.begin(), offsets.end() - 1, offsets.end());
    offsets[0] = 0;
}

// FUN_004aa610's last loop fills surface EBP's face array at [ESP+0x60] by walking every face of the solid
// (0x004aaaf7-0x004aab28), which is quadratic. Nothing in that loop changes a face's surface index or the face
// list, so one bucketing pass yields the same faces in the same order. Replaces the 12 bytes of MOVs before the walk.
CodeInjection surface_faces_collect{
    0x004aaaeb,
    [](auto& regs) {
        SurfaceFaceBuckets* buckets = g_surface_face_buckets;
        if (!buckets) return;
        const uintptr_t esp = regs.esp;
        if (!buckets->built) {
            buckets->built = true;
            try {
                build_surface_face_buckets(*buckets, *reinterpret_cast<GSolid**>(esp + 0x20),
                                           *reinterpret_cast<int*>(esp + 0x10));
            }
            catch (const std::bad_alloc&) {
                *buckets = SurfaceFaceBuckets{};
                g_surface_face_buckets = nullptr;
                return;
            }
        }
        // the skipped MOV's unwind state, which marks the face array live
        *reinterpret_cast<int*>(esp + 0x74) = 4;
        auto& surface_faces = *reinterpret_cast<VArray<GFace*>*>(esp + 0x60);
        const int surface = regs.ebp;
        for (uint32_t k = buckets->offsets[surface]; k < buckets->offsets[surface + 1]; k++) {
            surface_faces.push_back(buckets->faces[k]);
        }
        regs.eip = 0x004aab2a;
    },
};

// FUN_004abac0 (ECX = &face->flags): refusing a face keeps it out of the surfaces only; it still occludes.
bool __fastcall face_gets_surface_hooked(const int* flags);
FunHook<decltype(face_gets_surface_hooked)> face_gets_surface_hook{0x004abac0, face_gets_surface_hooked};
bool __fastcall face_gets_surface_hooked(const int* flags)
{
    if (!face_gets_surface_hook.call_target(flags)) return false;
    if (!g_terrain_gate_uids || g_terrain_gate_uids->empty()) return true;
    const auto* face =
        reinterpret_cast<const GFace*>(reinterpret_cast<const std::byte*>(flags) - offsetof(GFace, flags));
    return !in_terrain_room(face, *g_terrain_gate_uids);
}

bool gets_stock_surface(GFace* face, const std::vector<int32_t>& terrain_uids)
{
    return face_gets_surface_hook.call_target(&face->flags) && !in_terrain_room(face, terrain_uids);
}

// After the surface pass: faces left without a surface by the caps above.
void report_surface_overflow(CDedLevel& level)
{
    const GSolid* solid = level.solid;
    if (!solid) return;
    const auto& terrain_uids = level.GetAlpineLevelProperties().terrain_room_uids;
    uint32_t unlit = 0;
    for (GFace* f = solid->face_list_head; f; f = f->next_solid) {
        if (f->surface_index == -1 && gets_stock_surface(f, terrain_uids)) unlit++;
    }
    if (!unlit) return;
    const std::string msg = std::format("The level needs more than RED's {} lightmap surfaces; {} faces got no "
                                        "lightmap. Leftover geometry of a deleted, moved or converted terrain is "
                                        "the usual cause: run Build Geometry, then Calculate Lighting again.",
                                        red_max_level_surfaces, unlit);
    lighting_calc_report_refusal(msg.c_str());
}

// A terrain with no build state may still have an older build in the compiled solid (saved stale,
// then reloaded) whose rooms nothing identifies, so they would get one stock surface per triangle.
// Returns true when the surface pass must not run.
bool terrain_lighting_refused(CDedLevel& level)
{
    const auto& props = level.GetAlpineLevelProperties();
    std::string unbuilt;
    int unbuilt_count = 0;
    for (const DedTerrain* t : props.terrain_objects) {
        if (!t || !t->data.built_room_uids.empty()) continue;
        if (unbuilt_count++ < 4) unbuilt += (unbuilt.empty() ? "" : ", ") + terrain_label(*t);
    }
    if (!unbuilt_count) return false;
    if (unbuilt_count > 4) unbuilt += std::format(" and {} more", unbuilt_count - 4);
    const std::string head = std::format("{} {} not built into the level geometry.", unbuilt,
                                         unbuilt_count == 1 ? "is" : "are");

    if (headless_bake_active()) {
        terrain_report("Calculate Lighting was not run: " + head + " Build Geometry and save the level first.",
                       false);
        headless_bake_mark_refused();
        return true;
    }

    uint32_t candidates = 0;
    if (const GSolid* solid = level.solid) {
        for (GFace* f = solid->face_list_head; f; f = f->next_solid) {
            if (gets_stock_surface(f, props.terrain_room_uids)) candidates++;
        }
    }
    if (candidates > red_max_level_surfaces) {
        terrain_report(std::format("Calculate Lighting was not run: {} If the level still holds an older build of "
                                   "it, its {} faces would overflow RED's {} lightmap surfaces. Run Build "
                                   "Geometry first.",
                                   head, candidates, red_max_level_surfaces),
                       true);
        return true;
    }
    terrain_report(head + " Run Build Geometry before Calculate Lighting: an older build of it left in the level "
                          "would get stock lightmaps.",
                   true);
    return false;
}

// FUN_00448ca0: Calculate Lighting's surface pass (thiscall, no stack arguments), alone or before a bake.
// It frees the level's lightmaps, so the memory check comes first.
void __fastcall lighting_surfaces_hooked(void* self);
FunHook<decltype(lighting_surfaces_hooked)> lighting_surfaces_hook{0x00448ca0, lighting_surfaces_hooked};
void __fastcall lighting_surfaces_hooked(void* self)
{
    bool refused = !lighting_calc_memory_admits();
    if (CDedLevel* level = refused ? nullptr : CDedLevel::Get()) {
        try {
            refused = terrain_lighting_refused(*level);
        }
        catch (const std::bad_alloc&) {
            xlog::error("[Terrain] out of memory checking terrain build state before Calculate Lighting");
        }
    }
    alpine_lm_note_lighting_refused(refused);
    if (refused) return;
    lighting_surfaces_hook.call_target(self);
    if (CDedLevel* level = CDedLevel::Get()) report_surface_overflow(*level);
}

// A stored mask sized for another layout than its own is ignored (every chunk).
const uint8_t* terrain_stored_geo_chunks(const DedTerrainData& d)
{
    const bool sized = d.geo_chunks.size() == at::chunk_mask_bytes(at::layout_chunk_count(d.geo_chunks_layout));
    return !d.geo_chunks.empty() && sized ? d.geo_chunks.data() : nullptr;
}

using RoomsByUid = std::unordered_map<int32_t, const GRoom*>;

// Why the terrain would be saved without a build mapping, else empty with `rooms` holding the compiled rooms
// by uid.
std::string terrain_build_mapping_problem(CDedLevel& level, const DedTerrain& terrain, RoomsByUid& rooms)
{
    const DedTerrainData& d = terrain.data;
    const std::string who = terrain_label(terrain);
    if (d.built_room_uids.empty() || !level.solid) {
        return who + " has no compiled geometry - run Build Geometry before saving.";
    }
    if (d.built_room_uids.size() != terrain_chunk_count(d) ||
        terrain_geometry_fingerprint(terrain) != d.built_geometry_fingerprint) {
        return who + " changed since the last Build Geometry - rebuild before saving.";
    }
    const GSolid* solid = level.solid;
    for (int i = 0; i < solid->all_rooms.get_size(); i++) {
        const GRoom* room = solid->all_rooms.data_ptr[i];
        if (room && room->uid != -1) rooms.emplace(room->uid, room);
    }
    for (std::size_t k = 0; k < d.built_room_uids.size(); k++) {
        const int32_t uid = d.built_room_uids[k];
        if (uid == at::no_room_uid) continue;
        auto it = uid != missing_room_uid ? rooms.find(uid) : rooms.end();
        if (it == rooms.end() || !it->second->face_list_head) {
            return std::format("{}: chunk {} has no compiled room - rebuild before saving.", who, k);
        }
    }
    return {};
}

// Decoration k's plane of `g`, empty when `g` has none for it.
template<typename Grid>
auto decoration_plane(Grid& g, std::size_t k)
{
    const std::size_t plane = at::decoration_plane_bytes(g.nx, g.nz, g.weight_res_mul);
    const bool has_plane = (k + 1) * plane <= g.decoration.size();
    return std::span{g.decoration.data() + (has_plane ? k * plane : 0), has_plane ? plane : 0};
}

} // namespace

std::string terrain_label(const DedTerrain& t)
{
    const char* name = t.script_name.c_str();
    return name[0] ? std::format("Terrain {} '{}'", t.uid, name) : std::format("Terrain {}", t.uid);
}

uint32_t terrain_effective_chunk_cells(const DedTerrainData& d)
{
    return at::effective_chunk_cells(at::cells(d.grid->nx), at::cells(d.grid->nz), d.chunk_cells, d.flags);
}

uint32_t terrain_chunk_count(const DedTerrainData& d)
{
    return at::chunk_count(at::cells(d.grid->nx), at::cells(d.grid->nz), terrain_effective_chunk_cells(d));
}

at::ChunkLayout terrain_geo_chunk_layout(const DedTerrainData& d)
{
    return d.grid ? at::geo_chunk_layout(d.grid->nx, d.grid->nz, d.chunk_cells, d.flags) : at::ChunkLayout{};
}

bool terrain_chunk_geoable(const DedTerrainData& d, uint32_t index)
{
    return at::remapped_chunk_bit(terrain_stored_geo_chunks(d), d.geo_chunks_layout, terrain_geo_chunk_layout(d),
                                  index);
}

std::vector<uint8_t> terrain_geo_chunks(const DedTerrainData& d)
{
    const at::ChunkLayout now = terrain_geo_chunk_layout(d);
    std::vector<uint8_t> out(at::chunk_mask_bytes(at::layout_chunk_count(now)));
    at::remap_chunk_mask(terrain_stored_geo_chunks(d), d.geo_chunks_layout, out.data(), now);
    return out;
}

at::Header terrain_header(const Vector3& pos, const DedTerrainData& d, const TerrainGrid* g)
{
    at::Header h{};
    h.origin[0] = pos.x;
    h.origin[1] = pos.y;
    h.origin[2] = pos.z;
    h.cell_size = d.cell_size;
    h.nx = g ? g->nx : 0;
    h.nz = g ? g->nz : 0;
    h.height_min = d.height_min;
    h.height_range = d.height_range;
    h.chunk_cells = g ? at::effective_chunk_cells(at::cells(g->nx), at::cells(g->nz), d.chunk_cells, d.flags)
                      : d.chunk_cells;
    h.weight_res_mul = g ? g->weight_res_mul : 0;
    h.lightmap_density = d.lightmap_density;
    h.flags = d.flags | (d.fullbright ? at::flag_fullbright : 0);
    h.thickness = d.thickness;
    h.skirt_depth = d.skirt_depth;
    h.layer_count = static_cast<uint32_t>(d.layers.size());
    return h;
}

at::GridView terrain_grid_view(const Vector3& pos, const DedTerrainData& d, const TerrainGrid& g)
{
    return at::make_grid_view(terrain_header(pos, d, &g), g.heights.data(), g.weights.data(), g.holes.data(),
                              g.diag.data(), d.layers.data());
}

std::span<uint8_t> terrain_decoration_plane(TerrainGrid& g, std::size_t k)
{
    return decoration_plane(g, k);
}

std::span<const uint8_t> terrain_decoration_plane(const TerrainGrid& g, std::size_t k)
{
    return decoration_plane(g, k);
}

std::size_t terrain_decoration_plane_count(const TerrainGrid& g)
{
    return g.decoration.size() / at::decoration_plane_bytes(g.nx, g.nz, g.weight_res_mul);
}

uint32_t terrain_decoration_views(const DedTerrainData& d, const TerrainGrid& g,
                                  at::DecorationView (&out)[at::max_decorations])
{
    return at::make_decoration_views(d.decorations, g.decoration,
                                     at::decoration_plane_bytes(g.nx, g.nz, g.weight_res_mul), out);
}

uint64_t terrain_decoration_lighting_hash(int32_t uid, const Vector3& pos, const DedTerrainData& d)
{
    if (!d.grid || !terrain_decorations_cast(d)) return 0;
    at::DecorationView views[at::max_decorations];
    const uint32_t count = terrain_decoration_views(d, *d.grid, views);
    return at::decoration_lighting_hash(uid, terrain_grid_view(pos, d, *d.grid), views, count);
}

bool terrain_decoration_casts(const DedTerrainDecoration& deco)
{
    return deco.casts_shadows && !deco.mesh.empty() && deco.density > 0.0f;
}

bool terrain_decorations_cast(const DedTerrainData& d)
{
    return std::any_of(d.decorations.begin(), d.decorations.end(), terrain_decoration_casts);
}

bool terrain_decoration_placement(const Vector3& pos, const DedTerrainData& d, TerrainDecorationPlacement& out)
{
    if (!d.grid || d.decorations.empty()) return false;
    const TerrainGrid& g = *d.grid;
    out.grid = terrain_grid_view(pos, d, g);
    out.layout = {at::cells(g.nx), at::cells(g.nz), terrain_effective_chunk_cells(d)};
    out.count = terrain_decoration_views(d, g, out.views);
    return true;
}

void terrain_build_isolated_brush_uids(std::unordered_set<int32_t>& uids)
{
    for (const TempChunk& tc : g_temp_chunks) uids.insert(tc.brush_uid);
}

void terrain_build_strip_leftovers(CDedLevel& level)
{
    if (g_temp_chunks.empty()) return;
    if (level.build_running) {
        terrain_report("Saving during Build Geometry: temporary terrain brushes are saved with the level.", false);
        return;
    }
    const int removed = remove_temp_brushes(level);
    xlog::warn("[Terrain] removed {} temporary chunk brush(es) left by Build Geometry", removed);
}

bool terrain_build_resolves(CDedLevel& level, const DedTerrain& terrain)
{
    if (!terrain.data.grid) return false;
    try {
        RoomsByUid rooms;
        return terrain_build_mapping_problem(level, terrain, rooms).empty();
    }
    catch (const std::bad_alloc&) {
        return false;
    }
}

std::string terrain_build_fill_mapping(CDedLevel& level, DedTerrain& terrain)
{
    DedTerrainData& d = terrain.data;
    d.build_mapping.clear();
    if (!d.grid) return {};
    const std::string who = terrain_label(terrain);
    const uint32_t count = terrain_chunk_count(d);

    try {
        RoomsByUid rooms;
        std::string problem = terrain_build_mapping_problem(level, terrain, rooms);
        if (!problem.empty()) return problem;
        if (terrain_material_fingerprint(terrain) != d.built_material_fingerprint) {
            terrain_report(who + " was painted since the last Build Geometry - footstep materials and legacy "
                                 "(D3D8/9) textures update on the next build.",
                           false);
        }

        std::vector<at::ChunkMapping> mapping(count);
        std::vector<at::PositionKey> keys;
        for (uint32_t k = 0; k < count; k++) {
            const int32_t uid = d.built_room_uids[k];
            if (uid == at::no_room_uid) {
                mapping[k] = {at::no_room_uid, 0, 0};
                continue;
            }
            const GFace* head = rooms.at(uid)->face_list_head;
            // Distinct positions of the faces as the RFL stores them (alpine_terrain.h, build mapping hash)
            keys.clear();
            for (const GFace* f = head; f; f = f->next_room) {
                for_each_face_vertex(f, [&](const Vector3& p) { keys.push_back(at::position_key(p.x, p.y, p.z)); });
            }
            uint32_t vertex_count = 0;
            uint64_t hash = 0;
            at::position_set_hash(keys.data(), keys.size(), vertex_count, hash);
            mapping[k] = {uid, vertex_count, hash};
        }
        d.build_mapping = std::move(mapping);
    }
    catch (const std::bad_alloc&) {
        d.build_mapping.clear();
        return who + ": out of memory writing its build mapping; it was saved without one.";
    }
    return {};
}

void terrain_build_note_loaded(CDedLevel& level, DedTerrain& terrain)
{
    DedTerrainData& d = terrain.data;
    d.built_room_uids.clear();
    d.built_geometry_fingerprint = 0;
    d.built_material_fingerprint = 0;
    if (d.build_mapping.empty()) return;
    terrain_prepare(terrain);
    auto& room_uids = level.GetAlpineLevelProperties().terrain_room_uids;
    for (const auto& m : d.build_mapping) {
        d.built_room_uids.push_back(m.room_uid);
        if (m.room_uid >= 0) room_uids.push_back(m.room_uid);
    }
    std::sort(room_uids.begin(), room_uids.end());
    d.built_geometry_fingerprint = terrain_geometry_fingerprint(terrain);
    d.built_material_fingerprint = terrain_material_fingerprint(terrain);
}

std::string terrain_build_orphan_note(CDedLevel& level)
{
    const auto& props = level.GetAlpineLevelProperties();
    std::unordered_set<int32_t> owned;
    for (const DedTerrain* t : props.terrain_objects) {
        if (t) owned.insert(t->data.built_room_uids.begin(), t->data.built_room_uids.end());
    }
    const auto orphans = std::count_if(props.terrain_room_uids.begin(), props.terrain_room_uids.end(),
                                       [&](int32_t uid) { return !owned.count(uid); });
    if (!orphans) return {};
    return std::format("The compiled level geometry still holds {} room(s) of deleted terrain - rebuild before "
                       "saving. Once the level is reloaded nothing marks them as terrain, and Calculate Lighting "
                       "would give them stock lightmaps.",
                       orphans);
}

void terrain_reset_built_state(DedTerrain& terrain)
{
    terrain.data.build_mapping.clear();
    terrain.data.built_room_uids.clear();
    terrain.data.built_geometry_fingerprint = 0;
    terrain.data.built_material_fingerprint = 0;
}

bool terrain_build_is_current(const DedTerrain& terrain)
{
    const DedTerrainData& d = terrain.data;
    return d.grid && !d.built_room_uids.empty() &&
           d.built_geometry_fingerprint == terrain_geometry_fingerprint(terrain);
}

float terrain_build_level_ray_hit(CDedLevel& level, const float (&o)[3], const float (&d)[3], float t_max)
{
    float best = t_max;
    const GSolid* solid = level.solid;
    if (!solid) return best;
    const auto& props = level.GetAlpineLevelProperties();
    struct Point
    {
        float v[3];
    };
    std::vector<Point> loop;
    for (const GFace* f = solid->face_list_head; f; f = f->next_solid) {
        if (f->portal_id > 0 || (f->flags & (FACE_SHOW_SKY | FACE_LIQUID | FACE_SEE_THRU | FACE_INVISIBLE))) continue;
        // Back faces, as the viewport culls them (0x004ee4c0)
        const Vector3& n = f->plane.normal;
        if (n.x * o[0] + n.y * o[1] + n.z * o[2] + f->plane.dist <= 0.0f) continue;
        if (f->which_room &&
            (props.is_terrain_room(f->which_room->uid) || props.is_terrain_split_room(f->which_room->uid))) {
            continue;
        }
        loop.clear();
        float lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
        for_each_face_vertex(f, [&](const Vector3& p) {
            const Point pt{{p.x, p.y, p.z}};
            loop.push_back(pt);
            for (int i = 0; i < 3; i++) {
                lo[i] = std::min(lo[i], pt.v[i]);
                hi[i] = std::max(hi[i], pt.v[i]);
            }
        });
        if (loop.size() < 3) continue;
        constexpr float margin = 1e-3f;
        float t0 = 0.0f, t1 = best;
        for (int i = 0; i < 3 && t0 <= t1; i++) {
            if (std::fabs(d[i]) < 1e-12f) {
                if (o[i] < lo[i] - margin || o[i] > hi[i] + margin) t0 = INFINITY;
                continue;
            }
            float ta = (lo[i] - margin - o[i]) / d[i], tb = (hi[i] + margin - o[i]) / d[i];
            if (ta > tb) std::swap(ta, tb);
            t0 = std::max(t0, ta);
            t1 = std::min(t1, tb);
        }
        if (t0 > t1) continue;
        for (std::size_t k = 1; k + 1 < loop.size(); k++) {
            float t = 0.0f;
            if (at::ray_triangle(o, d, loop[0].v, loop[k].v, loop[k + 1].v, 0.0f, best, t)) best = t;
        }
    }
    return best;
}

TerrainBakeEstimate terrain_bake_estimate(CDedLevel& level, DedTerrain& terrain)
{
    TerrainBakeEstimate e;
    terrain_prepare(terrain);
    const DedTerrainData& d = terrain.data;
    const at::GridView view = make_view(terrain, *d.grid);
    const uint32_t cx = at::cells(d.grid->nx), cz = at::cells(d.grid->nz);
    const uint32_t edge = terrain_effective_chunk_cells(d);
    e.chunks = at::chunk_count(cx, cz, edge);
    // RED gives coplanar faces of one texture a shared surface, so a chunk costs one surface per
    // distinct (plane, material) among its faces.
    std::vector<std::array<int64_t, 5>> planes;
    for (uint32_t k = 0; k < e.chunks; k++) {
        const at::ChunkRect rect = at::chunk_rect(cx, cz, edge, k);
        planes.clear();
        at::emit_chunk(view, rect, [&](const at::EmitFace& f) {
            e.faces++;
            float p[3][3];
            for (int i = 0; i < 3; i++) at::slot_position(view, rect, f.slot[i], p[i]);
            const double u[3] = {double(p[1][0]) - p[0][0], double(p[1][1]) - p[0][1], double(p[1][2]) - p[0][2]};
            const double v[3] = {double(p[2][0]) - p[0][0], double(p[2][1]) - p[0][1], double(p[2][2]) - p[0][2]};
            double n[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
            const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (len <= 0.0) return;
            for (double& c : n) c /= len;
            const double dist = -(n[0] * p[0][0] + n[1] * p[0][1] + n[2] * p[0][2]);
            planes.push_back({std::llround(n[0] * 1e4), std::llround(n[1] * 1e4), std::llround(n[2] * 1e4),
                              std::llround(dist * 1e3), static_cast<int64_t>(f.material)});
        });
        std::sort(planes.begin(), planes.end());
        e.new_surfaces += static_cast<uint32_t>(std::unique(planes.begin(), planes.end()) - planes.begin());
    }

    // The level's surfaces as the last Calculate Lighting numbered them, or, before any, every face
    // the stock surface gate would take (an upper bound: coplanar neighbours share a surface).
    const GSolid* solid = level.solid;
    if (!solid) return e;
    const auto& props = level.GetAlpineLevelProperties();
    std::vector<bool> used;
    uint32_t candidates = 0;
    for (GFace* f = solid->face_list_head; f; f = f->next_solid) {
        if (in_terrain_room(f, props.terrain_room_uids)) continue;
        const int index = f->surface_index;
        if (index >= 0) {
            if (used.size() <= static_cast<std::size_t>(index)) used.resize(index + 1, false);
            used[index] = true;
        }
        if (face_gets_surface_hook.call_target(&f->flags)) candidates++;
    }
    const auto numbered = static_cast<uint32_t>(std::count(used.begin(), used.end(), true));
    e.level_surfaces_measured = numbered > 0;
    e.level_surfaces = numbered > 0 ? numbered : candidates;
    return e;
}

std::vector<BrushNode*> terrain_bake_to_brushes(CDedLevel& level, DedTerrain& terrain)
{
    terrain_prepare(terrain);
    std::vector<BrushNode*> brushes;
    std::vector<std::string> notes;
    std::vector<uint32_t> failed;
    brushes.reserve(terrain_chunk_count(terrain.data));
    // One shared pivot, the terrain's origin, so a vertex two chunks share lands on the same world
    // position in both brushes.
    const Vector3 base = terrain.pos;
    build_chunk_solids(terrain, base, notes, failed, [&](uint32_t, GSolid* solid, uint64_t) {
        BrushNode* brush = insert_detail_solid_brush(&level, solid, base, identity_orient);
        if (!brush) return false;
        brushes.push_back(brush);
        return true;
    });
    for (const std::string& note : notes) terrain_report(note, false);
    terrain_report(std::format("{} was converted into {} detail brush(es).", terrain_label(terrain), brushes.size()),
                   false);
    return brushes;
}

void ApplyTerrainBuildPatches()
{
    geobuild_driver_hook.install();
    surface_build_hook.install();
    surface_build_cap.install();
    surface_faces_collect.install();
    lighting_surfaces_hook.install();
    face_gets_surface_hook.install();
}
