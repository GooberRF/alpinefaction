#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include <unordered_map>
#include <utility>
#include <xlog/xlog.h>
#include <common/terrain/alpine_terrain_reader.h>
#include "alpine_settings.h"
#include "alpine_terrain.h"
#include "level.h"
#include "../rf/geometry.h"
#include "../rf/level.h"
#include "../rf/multi.h"
#include "../rf/gr/gr_light.h"
#include "../graphics/gr.h"
#include "../graphics/af_lightmap.h"
#include "../multi/multi.h"

namespace at = alpine_terrain;

namespace
{

std::vector<AlpineTerrain> g_terrains;

// Indexed by GRoom::room_index; the room pointer guards against a stale or reused index.
struct RoomSlot
{
    const rf::GRoom* room = nullptr;
    AlpineTerrainRoomRef ref{-1, -1};
};
std::vector<RoomSlot> g_room_slots;

// Far above any real level's room count, so a garbage index cannot size the table.
constexpr int max_room_index = 1 << 20;

// vertex_count and pos_hash of a compiled room, as alpine_terrain.h defines them for the mapping.
bool room_position_hash(rf::GRoom* room, std::vector<at::PositionKey>& keys, std::uint32_t& count,
                        std::uint64_t& hash)
{
    keys.clear();
    for (rf::GFace& face : room->face_list) {
        const rf::GFaceVertex* head = face.edge_loop;
        int n = 0;
        for (const rf::GFaceVertex* fv = head; fv;) {
            if (++n > rf::max_face_vertices) return false;
            if (fv->vertex) keys.push_back(at::position_key(fv->vertex->pos.x, fv->vertex->pos.y, fv->vertex->pos.z));
            fv = fv->next;
            if (fv == head) break;
        }
    }
    at::position_set_hash(keys.data(), keys.size(), count, hash);
    return true;
}

// nullptr when every chunk of `t` matches its compiled room, else why not.
const char* resolve_terrain(const AlpineTerrain& t, const std::unordered_map<int, rf::GRoom*>& rooms,
                            std::vector<at::PositionKey>& keys, std::vector<rf::GRoom*>& out)
{
    if (t.build_mapping.empty()) return "it was saved without Build Geometry";
    out.assign(t.build_mapping.size(), nullptr);
    for (std::size_t k = 0; k < t.build_mapping.size(); k++) {
        const at::ChunkMapping& m = t.build_mapping[k];
        if (m.room_uid == at::no_room_uid) {
            if (m.vertex_count != 0 || m.pos_hash != 0) return "an empty chunk has a hash";
            continue;
        }
        auto it = rooms.find(m.room_uid);
        if (it == rooms.end() || !it->second) return "a chunk room is missing";
        rf::GRoom* room = it->second;
        if (!room->is_detail || room->is_sky) return "a chunk room is not a detail room";
        if (room->room_index < 0 || room->room_index >= max_room_index) return "a chunk room has no index";
        std::uint32_t count = 0;
        std::uint64_t hash = 0;
        if (!room_position_hash(room, keys, count, hash)) return "a chunk room has a corrupt face";
        if (count != m.vertex_count || hash != m.pos_hash) return "the compiled geometry does not match";
        out[k] = room;
    }
    return nullptr;
}

// Before anything is freed: it reads the decoration planes and the weights of linked layers.
std::uint64_t decoration_lighting_hash(const AlpineTerrain& t)
{
    at::DecorationView views[at::max_decorations];
    const std::uint32_t count = alpine_terrain_decoration_views(t, views);
    return at::decoration_lighting_hash(t.uid, alpine_terrain_grid(t), views, count);
}

// The paint maps are texture sources for the D3D11 terrain renderer, which cannot be switched to mid-session.
bool keeps_paint_maps()
{
    return !rf::is_dedicated_server && is_d3d11();
}

} // namespace

void alpine_terrain_load_chunk(rf::File& file, std::size_t chunk_len)
{
    std::size_t remaining = chunk_len;
    rf::File::ChunkGuard chunk_guard{file, remaining};
    AlpineChunkReader reader{file, remaining};

    std::uint32_t count = 0;
    if (!reader.read_bytes(&count, sizeof(count))) {
        xlog::warn("[AlpineTerrain] Ignoring the terrain chunk: truncated");
        return;
    }
    if (count > at::max_terrains - g_terrains.size()) {
        xlog::warn("[AlpineTerrain] Ignoring the terrain chunk: {} more terrains would exceed {}", count,
                   at::max_terrains);
        return;
    }

    // The weights and decoration planes also place decorations, which only a client that renders draws.
    const bool keep_paint_maps = keeps_paint_maps();
    const bool renders = !rf::is_dedicated_server && !is_headless_mode();

    // All or nothing: after a bad record nothing later in the chunk can be trusted.
    std::vector<AlpineTerrain> parsed;
    std::uint64_t total_raw = 0;
    for (const auto& t : g_terrains) total_raw += at::header_raw_size(t.header);
    // Sized by the file, inside an engine call nothing may unwind through.
    try {
        for (std::uint32_t i = 0; i < count; ++i) {
            at::Record rec;
            if (const char* err = at::read_record(reader, rec, total_raw)) {
                xlog::warn("[AlpineTerrain] Ignoring the terrain chunk: record {} {}", i, err);
                return;
            }
            AlpineTerrain& t = parsed.emplace_back(AlpineTerrain{std::move(rec)});
            t.decoration_lighting_hash = decoration_lighting_hash(t);
            const bool decorated = renders && !t.decorations.empty();
            if (!keep_paint_maps && !decorated) {
                t.weights.clear();
                t.weights.shrink_to_fit();
            }
            if (!keep_paint_maps) {
                t.overlay_coverage.clear();
                t.overlay_coverage.shrink_to_fit();
            }
            if (!decorated) {
                t.decoration_coverage.clear();
                t.decoration_coverage.shrink_to_fit();
            }
        }
        g_terrains.reserve(g_terrains.size() + parsed.size());
    }
    catch (const std::bad_alloc&) {
        xlog::warn("[AlpineTerrain] Ignoring the terrain chunk: out of memory");
        return;
    }

    for (auto& t : parsed) g_terrains.push_back(std::move(t));
    xlog::info("[AlpineTerrain] Loaded {} terrain(s)", parsed.size());
}

void alpine_terrain_clear_state()
{
    g_terrains.clear();
    g_terrains.shrink_to_fit();
    g_room_slots.clear();
    g_room_slots.shrink_to_fit();
}

void alpine_terrain_release_decoration_maps()
{
    const bool keep_paint_maps = keeps_paint_maps();
    for (AlpineTerrain& t : g_terrains) {
        t.decoration_coverage.clear();
        t.decoration_coverage.shrink_to_fit();
        if (!keep_paint_maps) {
            t.weights.clear();
            t.weights.shrink_to_fit();
        }
    }
}

void alpine_terrain_resolve_rooms()
{
    g_room_slots.clear();
    try {
        rf::GSolid* solid = rf::level.geometry;
        if (g_terrains.empty() || !solid) return;

        // A uid two rooms share cannot identify either.
        std::unordered_map<int, rf::GRoom*> rooms;
        for (rf::GRoom* room : solid->all_rooms) {
            if (!room || room->uid == -1) continue;
            auto [it, inserted] = rooms.emplace(room->uid, room);
            if (!inserted) it->second = nullptr;
        }

        std::vector<at::PositionKey> keys;
        std::vector<rf::GRoom*> chunk_rooms;
        int resolved = 0;
        for (std::size_t i = 0; i < g_terrains.size(); i++) {
            AlpineTerrain& t = g_terrains[i];
            t.resolved = false;
            const char* err = resolve_terrain(t, rooms, keys, chunk_rooms);
            // Two chunks, or two terrains, claiming one room means the mapping is not this level's.
            if (!err) {
                std::vector<int> indices;
                for (const rf::GRoom* room : chunk_rooms) {
                    if (room) indices.push_back(room->room_index);
                }
                std::sort(indices.begin(), indices.end());
                const bool shared = std::adjacent_find(indices.begin(), indices.end()) != indices.end() ||
                    std::any_of(indices.begin(), indices.end(), [](int idx) {
                        return static_cast<std::size_t>(idx) < g_room_slots.size() && g_room_slots[idx].room;
                    });
                if (shared) err = "a chunk room is claimed twice";
            }
            if (err) {
                xlog::warn("[AlpineTerrain] Terrain {} renders as plain geometry: {}", t.uid, err);
                continue;
            }
            for (std::size_t k = 0; k < chunk_rooms.size(); k++) {
                const rf::GRoom* room = chunk_rooms[k];
                if (!room) continue;
                const auto idx = static_cast<std::size_t>(room->room_index);
                if (idx >= g_room_slots.size()) g_room_slots.resize(idx + 1);
                g_room_slots[idx] = {room, {static_cast<int>(i), static_cast<int>(k)}};
            }
            t.resolved = true;
            resolved++;
        }
        xlog::info("[AlpineTerrain] {} of {} terrain(s) matched their compiled geometry", resolved, g_terrains.size());
    }
    catch (const std::bad_alloc&) {
        g_room_slots.clear();
        for (AlpineTerrain& t : g_terrains) t.resolved = false;
        xlog::warn("[AlpineTerrain] Out of memory matching terrain rooms; terrain renders as plain geometry");
    }
}

const AlpineTerrainRoomRef* alpine_terrain_find_room(const rf::GRoom* room)
{
    if (!room || room->room_index < 0) return nullptr;
    const auto idx = static_cast<std::size_t>(room->room_index);
    if (idx >= g_room_slots.size() || g_room_slots[idx].room != room) return nullptr;
    return &g_room_slots[idx].ref;
}

bool alpine_terrain_is_separate_chunk(const rf::GRoom* parent, const rf::GRoom* detail_room)
{
    return parent && !parent->is_sky && alpine_terrain_is_chunk_room(detail_room);
}

at::GridView alpine_terrain_grid(const AlpineTerrain& t)
{
    return at::make_grid_view(t.header, t.heights.data(), t.weights.empty() ? nullptr : t.weights.data(),
                              t.holes.data(), t.diag.data(), t.layers.data());
}

std::uint32_t alpine_terrain_decoration_views(const AlpineTerrain& t, at::DecorationView (&out)[at::max_decorations])
{
    return at::make_decoration_views(
        t.decorations, t.decoration_coverage,
        at::decoration_plane_bytes(t.header.nx, t.header.nz, t.header.weight_res_mul), out);
}

const std::vector<AlpineTerrain>& alpine_terrain_get_all()
{
    return g_terrains;
}

at::FaceKind alpine_terrain_face_kind(const at::GridView& g, const rf::GFace& face)
{
    return at::face_kind(g, [&](auto&& visit) {
        int n = 0;
        for (const rf::GFaceVertex* fv = face.edge_loop; fv && fv->vertex && n < rf::max_face_vertices; n++) {
            visit(fv->vertex->pos.x, fv->vertex->pos.y, fv->vertex->pos.z);
            fv = fv->next;
            if (fv == face.edge_loop) break;
        }
    });
}

void alpine_terrain_sample_light(int terrain, at::FaceKind kind, const float (&pos)[3],
                                 const float (&face_normal)[3], float (&texel)[3])
{
    const AlpineTerrain& t = g_terrains[static_cast<std::size_t>(terrain)];
    const at::GridView g = alpine_terrain_grid(t);
    float n[3] = {face_normal[0], face_normal[1], face_normal[2]};
    if (kind == at::FaceKind::top) {
        at::heightmap_normal(g, pos[0], pos[2], n);
    }
    const float scale = kind == at::FaceKind::crater && !t.fullbright()
                            ? at::crater_light_factor(at::surface_y_bilinear(g, pos[0], pos[2]) - pos[1])
                            : 1.0f;

    // The baked terrain chart where the level carries one, the texel ter_base_light samples. It holds
    // the top surface's light, which craters take dimmed and the underside does not use.
    if (kind != at::FaceKind::underside && af_lightmap_terrain_sample(terrain, pos[0], pos[2], texel)) {
        for (float& c : texel) c *= scale;
        return;
    }

    // Otherwise identical to ter_base_light without a chart: level ambient plus the sun's N.L. The
    // shader draws that light as is, a lightmap texel doubled, hence the half.
    float light[3];
    rf::gr::light_get_ambient(&light[0], &light[1], &light[2]);
    const SunLightState sun = gr_get_sun_state();
    const float n_dot_l =
        std::clamp(-(n[0] * sun.travel_dir.x + n[1] * sun.travel_dir.y + n[2] * sun.travel_dir.z), 0.0f, 1.0f);
    for (int i = 0; i < 3; i++) {
        texel[i] = (light[i] + sun.color[i] * n_dot_l) * 0.5f * scale;
    }
}
