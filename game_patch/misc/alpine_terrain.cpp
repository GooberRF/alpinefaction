#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <new>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>
#include <xlog/xlog.h>
#include <common/lighting/alpine_lighting.h>
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

namespace
{

using SeamPoint = std::array<double, 2>;

// A chunk wall's outline in its seam plane: (u, y), u being z on an x line and x on a z line.
struct SeamWall
{
    std::vector<SeamPoint> pts;
    double lo_u, hi_u, lo_y, hi_y;
};

// What an AlpineTerrainSeamScope gathers once: the live chunk rooms, and a room's walls on a boundary line.
struct SeamCache
{
    bool rooms_built = false;
    std::unordered_map<std::uint64_t, rf::GRoom*> chunk_rooms; // (terrain << 32) | chunk
    std::map<std::tuple<const rf::GRoom*, int, std::int64_t, int>, std::vector<SeamWall>> walls;
    // Scratch, reused so a rebuild's hundreds of seam faces don't each allocate.
    std::vector<SeamPoint> outline;
    std::vector<SeamPoint> clip_a;
    std::vector<SeamPoint> clip_b;
};
SeamCache* g_seam_cache = nullptr;

std::uint64_t seam_chunk_key(int terrain, std::int64_t chunk)
{
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(terrain)) << 32) |
        static_cast<std::uint32_t>(chunk);
}

// The faces of `room` lying on grid line `line` of `axis` (0 x, 2 z) whose normal points `sign` along it.
std::vector<SeamWall> gather_seam_walls(rf::GRoom* room, const at::GridView& g, int axis, std::int64_t line, int sign)
{
    std::vector<SeamWall> out;
    const float line_c = g.origin[axis] + static_cast<float>(line) * g.cell_size;
    for (rf::GFace& f : room->face_list) {
        const float n_axis = axis == 0 ? f.plane.normal.x : f.plane.normal.z;
        if (!(n_axis * static_cast<float>(sign) > 0.999f)) continue;
        SeamWall w;
        w.lo_u = w.lo_y = std::numeric_limits<double>::max();
        w.hi_u = w.hi_y = -std::numeric_limits<double>::max();
        bool on_line = true;
        int n = 0;
        for (const rf::GFaceVertex* fv = f.edge_loop; fv;) {
            if (++n > rf::max_face_vertices || !fv->vertex) {
                on_line = false;
                break;
            }
            const rf::Vector3& p = fv->vertex->pos;
            const float c = axis == 0 ? p.x : p.z;
            if (!(std::fabs(c - line_c) <= at::coord_tolerance(c, line_c))) {
                on_line = false;
                break;
            }
            const SeamPoint q{axis == 0 ? p.z : p.x, p.y};
            w.pts.push_back(q);
            w.lo_u = std::min(w.lo_u, q[0]);
            w.hi_u = std::max(w.hi_u, q[0]);
            w.lo_y = std::min(w.lo_y, q[1]);
            w.hi_y = std::max(w.hi_y, q[1]);
            fv = fv->next;
            if (fv == f.edge_loop) break;
        }
        if (on_line && w.pts.size() >= 3) out.push_back(std::move(w));
    }
    return out;
}

double seam_signed_area(const std::vector<SeamPoint>& p)
{
    double a = 0.0;
    for (std::size_t i = 0, n = p.size(); i < n; i++) {
        const SeamPoint& u = p[i];
        const SeamPoint& v = p[(i + 1) % n];
        a += u[0] * v[1] - v[0] * u[1];
    }
    return a * 0.5;
}

// Sutherland-Hodgman: `subject` clipped to the convex, counter-clockwise `clip`, each edge pushed out by 0.01 mm
// so a subject lying exactly on `clip` (a carve's sliver and its twin in the next chunk) is not rounded away.
// Returns the clipped area; `source` is moved by -`origin` first. `subject` and `input` are scratch.
double seam_clipped_area(const std::vector<SeamPoint>& source, const SeamPoint& origin,
                         const std::vector<SeamPoint>& clip, std::vector<SeamPoint>& subject,
                         std::vector<SeamPoint>& input)
{
    constexpr double edge_slack = 1e-5;
    subject.clear();
    for (const SeamPoint& q : source) subject.push_back({q[0] - origin[0], q[1] - origin[1]});
    for (std::size_t i = 0, m = clip.size(); i < m && !subject.empty(); i++) {
        const SeamPoint& a = clip[i];
        const SeamPoint& b = clip[(i + 1) % m];
        const double slack = edge_slack * std::hypot(b[0] - a[0], b[1] - a[1]);
        const auto side = [&](const SeamPoint& p) {
            return (b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0]) + slack;
        };
        input.swap(subject);
        subject.clear();
        for (std::size_t j = 0, n = input.size(); j < n; j++) {
            const SeamPoint& cur = input[j];
            const SeamPoint& prev = input[(j + n - 1) % n];
            const double sc = side(cur), sp = side(prev);
            if ((sc >= 0.0) != (sp >= 0.0)) {
                const double t = sp / (sp - sc);
                subject.push_back({prev[0] + (cur[0] - prev[0]) * t, prev[1] + (cur[1] - prev[1]) * t});
            }
            if (sc >= 0.0) subject.push_back(cur);
        }
    }
    return std::fabs(seam_signed_area(subject));
}

// 0 inside the convex wall `w`, else the distance from `p` to its outline.
double seam_distance_to_wall(const SeamWall& w, const SeamPoint& p)
{
    bool inside = false;
    double best = std::numeric_limits<double>::max();
    for (std::size_t i = 0, n = w.pts.size(); i < n; i++) {
        const SeamPoint& a = w.pts[i];
        const SeamPoint& b = w.pts[(i + 1) % n];
        if ((a[1] > p[1]) != (b[1] > p[1]) && p[0] < a[0] + (p[1] - a[1]) / (b[1] - a[1]) * (b[0] - a[0])) {
            inside = !inside;
        }
        const double du = b[0] - a[0], dy = b[1] - a[1];
        const double len_sq = du * du + dy * dy;
        const double t = len_sq > 0.0 ? std::clamp(((p[0] - a[0]) * du + (p[1] - a[1]) * dy) / len_sq, 0.0, 1.0) : 0.0;
        best = std::min(best, std::hypot(a[0] + du * t - p[0], a[1] + dy * t - p[1]));
    }
    return inside ? 0.0 : best;
}

bool seam_face_is_backed(const rf::GFace& face, SeamCache& cache)
{
    const rf::GRoom* room = face.which_room;
    const AlpineTerrainRoomRef* ref = alpine_terrain_find_room(room);
    if (!ref || !room->is_geoable) return false;
    const AlpineTerrain& t = g_terrains[static_cast<std::size_t>(ref->terrain)];
    if (!(t.header.flags & at::flag_geoable)) return false;
    const at::ChunkLayout layout = at::header_chunk_layout(t.header);
    const std::uint32_t across = at::chunks_along(layout.cells_x, layout.edge);
    if (across == 0) return false;
    const auto chunk = static_cast<std::uint32_t>(ref->chunk);
    const at::ChunkRect r = at::chunk_rect(layout.cells_x, layout.cells_z, layout.edge, chunk);
    const at::GridView g = alpine_terrain_grid(t);
    if (!(g.cell_size > 0.0f)) return false;
    const float max_line = static_cast<float>(std::max(g.nx, g.nz));

    // Every vertex on one grid line of `axis` (0 x, 2 z); the line's cell index, or -1.
    auto line_of = [&](int axis) -> std::int64_t {
        std::int64_t line = -1;
        int n = 0;
        for (const rf::GFaceVertex* fv = face.edge_loop; fv;) {
            if (++n > rf::max_face_vertices || !fv->vertex) return -1;
            const float c = axis == 0 ? fv->vertex->pos.x : fv->vertex->pos.z;
            const float o = g.origin[axis];
            const float f = (c - o) / g.cell_size;
            if (!(f > -1.0f && f < max_line)) return -1;
            const auto k = static_cast<std::int64_t>(std::lround(f));
            if ((line >= 0 && k != line) || k < 0
                || std::fabs(c - (o + static_cast<float>(k) * g.cell_size)) > at::coord_tolerance(c, o)) {
                return -1;
            }
            line = k;
            fv = fv->next;
            if (fv == face.edge_loop) break;
        }
        return n >= 3 ? line : -1;
    };
    std::int64_t neighbour = -1;
    int axis = 0;
    std::int64_t line = line_of(0);
    if (line >= 0) {
        if (line == r.x0 && r.x0 > 0) neighbour = chunk - 1;
        else if (line == r.x1 && r.x1 < layout.cells_x) neighbour = chunk + 1;
    }
    else {
        axis = 2;
        line = line_of(2);
        if (line >= 0 && line == r.z0 && r.z0 > 0) neighbour = chunk - across;
        else if (line >= 0 && line == r.z1 && r.z1 < layout.cells_z) neighbour = chunk + across;
    }
    if (neighbour < 0 || !rf::level.geometry) return false;
    const float n_axis = axis == 0 ? face.plane.normal.x : face.plane.normal.z;
    if (!(std::fabs(n_axis) > 0.999f)) return false;

    // From the live room list: the engine frees a room a carve emptied (0x004D0590), so a stored pointer may dangle.
    if (!cache.rooms_built) {
        for (rf::GRoom* candidate : rf::level.geometry->all_rooms) {
            if (const AlpineTerrainRoomRef* c = alpine_terrain_find_room(candidate)) {
                cache.chunk_rooms.emplace(seam_chunk_key(c->terrain, c->chunk), candidate);
            }
        }
        cache.rooms_built = true;
    }
    const auto it = cache.chunk_rooms.find(seam_chunk_key(ref->terrain, neighbour));
    if (it == cache.chunk_rooms.end() || !it->second->is_geoable) return false;

    // The neighbour's opposite walls on this line must cover the whole face: a carve that reached one chunk and
    // not the other leaves the other's wall partly or wholly open.
    const int back_sign = n_axis > 0.0f ? -1 : 1;
    const auto key = std::make_tuple(static_cast<const rf::GRoom*>(it->second), axis, line, back_sign);
    auto walls_it = cache.walls.find(key);
    if (walls_it == cache.walls.end()) {
        walls_it = cache.walls.emplace(key, gather_seam_walls(it->second, g, axis, line, back_sign)).first;
    }

    // In (u, y) about the face's first vertex, counter-clockwise. line_of checked every vertex.
    std::vector<SeamPoint>& outline = cache.outline;
    outline.clear();
    SeamPoint origin{};
    for (const rf::GFaceVertex* fv = face.edge_loop; fv;) {
        const SeamPoint q{axis == 0 ? fv->vertex->pos.z : fv->vertex->pos.x, fv->vertex->pos.y};
        if (outline.empty()) origin = q;
        outline.push_back({q[0] - origin[0], q[1] - origin[1]});
        fv = fv->next;
        if (fv == face.edge_loop || outline.size() >= static_cast<std::size_t>(rf::max_face_vertices)) break;
    }
    double area = seam_signed_area(outline);
    if (area < 0.0) {
        std::reverse(outline.begin(), outline.end());
        area = -area;
    }
    // A sliver thinner than any hull, as a carve leaves where the two chunks' cuts differ by float error: its
    // degenerate triangles give two-sided collision bogus normals. Dropped only while every vertex is within the
    // same width of the neighbour's wall, so the slit is that thin and a run of exposed slivers can't add up.
    constexpr double sliver_width = 0.01;
    double perimeter = 0.0;
    for (std::size_t i = 0, n = outline.size(); i < n; i++) {
        const SeamPoint& u = outline[i];
        const SeamPoint& v = outline[(i + 1) % n];
        perimeter += std::hypot(v[0] - u[0], v[1] - u[1]);
    }
    if (!(2.0 * area >= sliver_width * perimeter)) {
        for (const SeamPoint& q : outline) {
            const SeamPoint p{q[0] + origin[0], q[1] + origin[1]};
            bool by_wall = false;
            for (const SeamWall& w : walls_it->second) {
                if (p[0] < w.lo_u - sliver_width || p[0] > w.hi_u + sliver_width || p[1] < w.lo_y - sliver_width ||
                    p[1] > w.hi_y + sliver_width) {
                    continue;
                }
                if (seam_distance_to_wall(w, p) <= sliver_width) {
                    by_wall = true;
                    break;
                }
            }
            if (!by_wall) return false;
        }
        return true;
    }
    double lo_u = 0.0, hi_u = 0.0, lo_y = 0.0, hi_y = 0.0;
    for (const SeamPoint& q : outline) {
        lo_u = std::min(lo_u, q[0]);
        hi_u = std::max(hi_u, q[0]);
        lo_y = std::min(lo_y, q[1]);
        hi_y = std::max(hi_y, q[1]);
    }
    // At most 0.1% of the face, and never more than 10 cm^2, may be open.
    const double needed = area - std::min(area * 1e-3, 1e-3);
    double covered = 0.0;
    for (const SeamWall& w : walls_it->second) {
        if (w.hi_u - origin[0] < lo_u || w.lo_u - origin[0] > hi_u || w.hi_y - origin[1] < lo_y ||
            w.lo_y - origin[1] > hi_y) {
            continue;
        }
        covered += seam_clipped_area(w.pts, origin, outline, cache.clip_a, cache.clip_b);
        if (covered >= needed) {
            break;
        }
    }
    return covered >= needed;
}

} // namespace

AlpineTerrainSeamScope::AlpineTerrainSeamScope()
{
    if (!g_seam_cache) {
        try {
            g_seam_cache = new SeamCache;
            owner_ = true;
        }
        catch (const std::bad_alloc&) {
        }
    }
}

AlpineTerrainSeamScope::~AlpineTerrainSeamScope()
{
    if (owner_) {
        delete g_seam_cache;
        g_seam_cache = nullptr;
    }
}

bool alpine_terrain_is_interior_seam_face(const rf::GFace& face)
{
    // A chunk boundary wall faces along x or z; this turns away nearly every face before any lookup.
    if (!(std::fabs(face.plane.normal.x) > 0.999f || std::fabs(face.plane.normal.z) > 0.999f)
        || !alpine_terrain_is_chunk_room(face.which_room)) {
        return false;
    }
    try {
        if (g_seam_cache) {
            return seam_face_is_backed(face, *g_seam_cache);
        }
        SeamCache local;
        return seam_face_is_backed(face, local);
    }
    catch (const std::bad_alloc&) {
        return false;
    }
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

    // Otherwise identical to ter_base_light without a chart.
    float light[3];
    rf::gr::light_get_ambient(&light[0], &light[1], &light[2]);
    const SunLightState sun = gr_get_sun_state();
    const float travel[3] = {sun.travel_dir.x, sun.travel_dir.y, sun.travel_dir.z};
    alpine_lighting::terrain_fallback_texel(light, travel, sun.color, n, scale, texel);
}
