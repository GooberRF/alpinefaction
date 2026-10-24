#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <new>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include <xlog/xlog.h>
#include "bake_progress.h"
#include "build_repair.h"
#include "editor_settings.h"
#include "level.h"
#include "vtypes.h"

namespace
{

// The boolean places cut points to 1e-4 and BuildFinalRenderSolid welds only below that, so cracks run wider
constexpr double repair_tolerance = 1e-3;
// RED's geometry epsilon (0x00554714)
constexpr double red_epsilon = 1e-4;
constexpr double parallel_cosine = 0.999;
// Square units: cut slivers and small missing pieces, never a doorway
constexpr double max_fill_area = 1.0;
// FUN_004a6950 splits a face with more corners than this
constexpr std::size_t max_face_corners = 16;
constexpr int max_repair_rounds = 8;
constexpr int max_logged_examples = 10;

// In double: float spacing passes these tolerances a few thousand units from the origin
struct Vec
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

Vec to_vec(const Vector3& v)
{
    return {v.x, v.y, v.z};
}

Vec operator-(const Vec& a, const Vec& b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec operator+(const Vec& a, const Vec& b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

Vec operator*(const Vec& a, double s)
{
    return {a.x * s, a.y * s, a.z * s};
}

double dot(const Vec& a, const Vec& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec cross(const Vec& a, const Vec& b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

double length(const Vec& a)
{
    return std::sqrt(dot(a, a));
}

double plane_distance(const Plane& plane, const Vector3& p)
{
    return dot(to_vec(plane.normal), to_vec(p)) + plane.dist;
}

bool is_portal(const GFace& face)
{
    return face.portal_id > 0;
}

bool is_detail(const GFace& face)
{
    return (face.flags & FACE_IS_DETAIL) != 0;
}

// As FUN_0043cc90, the room builder's regular face
bool is_regular(const GFace& face)
{
    return (face.flags & (FACE_LIQUID | FACE_IS_DETAIL)) == 0 && !is_portal(face);
}

bool has_face_ref(const GVertex& vertex, const GFace* face)
{
    for (int i = 0; i < vertex.face_refs.size; i++) {
        if (vertex.face_refs.data_ptr[i] == face) {
            return true;
        }
    }
    return false;
}

bool no_detail_faces(const GVertex& vertex)
{
    for (int i = 0; i < vertex.face_refs.size; i++) {
        if (is_detail(*vertex.face_refs.data_ptr[i])) {
            return false;
        }
    }
    return true;
}

template<typename Fn>
void for_each_edge(const GFace& face, Fn&& fn)
{
    for_each_face_corner(face, [&](GFaceVertex& fv) { fn(fv.vertex, fv.next->vertex); });
}

bool is_edge(const GFace& face, const GVertex* a, const GVertex* b)
{
    bool found = false;
    for_each_edge(face, [&](const GVertex* p, const GVertex* q) {
        found = found || (p == a && q == b) || (p == b && q == a);
    });
    return found;
}

// Twice the area, along the winding normal
Vec newell(const std::vector<Vec>& points)
{
    Vec n;
    for (std::size_t i = 0; i < points.size(); i++) {
        const Vec& a = points[i];
        const Vec& b = points[(i + 1) % points.size()];
        n.x += (a.y - b.y) * (a.z + b.z);
        n.y += (a.z - b.z) * (a.x + b.x);
        n.z += (a.x - b.x) * (a.y + b.y);
    }
    return n;
}

std::vector<Vec> positions(const std::vector<GVertex*>& vertices)
{
    std::vector<Vec> out;
    for (const GVertex* v : vertices) {
        out.push_back(to_vec(v->pos));
    }
    return out;
}

Vec newell(const GFace& face)
{
    std::vector<Vec> points;
    for_each_face_corner(face, [&](GFaceVertex& fv) { points.push_back(to_vec(fv.vertex->pos)); });
    return newell(points);
}

GFaceAttributes attributes_of(const GFace& face)
{
    return {face.flags,         face.group_id, face.bitmap_id,       face.portal_id,
            face.surface_index, face.face_id,  face.smoothing_groups};
}

std::vector<GVertex*> sorted_vertices(const GFace& face)
{
    std::vector<GVertex*> out;
    for_each_face_corner(face, [&](GFaceVertex& fv) { out.push_back(fv.vertex); });
    std::sort(out.begin(), out.end());
    return out;
}

template<typename Fn>
void for_each_room_face(GRoom* room, Fn&& fn)
{
    for (GFace* face = room->face_list_head; face; face = face->next_room) {
        fn(*face);
    }
}

std::vector<GRoom*> normal_rooms(GSolid* solid)
{
    std::vector<GRoom*> out;
    for (int i = 0; i < solid->all_rooms.size; i++) {
        GRoom* room = solid->all_rooms.data_ptr[i];
        if (room && !room->is_detail && room->face_list_head) {
            out.push_back(room);
        }
    }
    return out;
}

std::vector<GFace*> portal_faces(GSolid* solid)
{
    std::vector<GFace*> out;
    solid->for_each_face([&](GFace& face) {
        if (is_portal(face) && !is_detail(face)) {
            out.push_back(&face);
        }
        return false;
    });
    return out;
}

template<typename T>
class DisjointSets
{
public:
    T find(T x)
    {
        if (parent_.try_emplace(x, x).second) {
            order_.push_back(x);
        }
        T root = x;
        while (parent_[root] != root) {
            root = parent_[root];
        }
        while (parent_[x] != root) {
            x = std::exchange(parent_[x], root);
        }
        return root;
    }

    void join(T a, T b)
    {
        const T ra = find(a);
        const T rb = find(b);
        if (ra != rb) {
            parent_[rb] = ra;
        }
    }

    // Sets of two or more, in first-seen order so the build is deterministic
    std::vector<std::vector<T>> groups()
    {
        std::vector<std::vector<T>> out;
        std::unordered_map<T, std::size_t> slot;
        for (T x : order_) {
            const auto [it, added] = slot.try_emplace(find(x), out.size());
            if (added) {
                out.emplace_back();
            }
            out[it->second].push_back(x);
        }
        out.erase(std::remove_if(out.begin(), out.end(), [](const auto& g) { return g.size() < 2; }), out.end());
        return out;
    }

private:
    std::unordered_map<T, T> parent_;
    std::vector<T> order_;
};

struct PortalBox
{
    Plane plane;
    Vector3 min;
    Vector3 max;
};

PortalBox box_of(const GFace& face)
{
    return {face.plane, face.bounding_box_min, face.bounding_box_max};
}

bool near_box(const Vector3& v, const Vector3& min, const Vector3& max)
{
    return v.x >= min.x - repair_tolerance && v.x <= max.x + repair_tolerance && v.y >= min.y - repair_tolerance
        && v.y <= max.y + repair_tolerance && v.z >= min.z - repair_tolerance && v.z <= max.z + repair_tolerance;
}

struct Stats
{
    int planes = 0;
    int twins = 0;
    int rooms_merged = 0;
    int welds = 0;
    int splits = 0;
    int faces_removed = 0;
    int portal_fills = 0;
    int wall_fills = 0;
    int portals_deleted = 0;
    int holes_after_rooms = 0;
    // out of memory before portal linking's deletions were known, so a wall gap could be a lost doorway
    bool portals_unknown = false;
};

Stats g_stats;

// From inside the repairs' std::bad_alloc handlers, which must not throw again
void log_out_of_memory(const char* doing)
{
    try {
        editor_report(EditorReportLevel::warn, "BuildRepair", std::string{"Out of memory "} + doing, true);
    }
    catch (...) {
    }
}

// To find the portal faces portal linking deletes
std::vector<std::pair<GFace*, PortalBox>> g_flood_portals;
// Every portal of the build, deleted ones included, so the wall fill leaves doorways open
std::vector<PortalBox> g_portal_boxes;

void report(const std::string& text)
{
    editor_report(EditorReportLevel::info, "BuildRepair", text, true);
}

// ─── Portal planes ───

// A sliver's own plane can be tilted, which misleads the side test and the point lookup, so each coplanar group of
// pieces takes its largest piece's plane. Before the room builder, coplanar is judged by its own side test.
int unify_portal_planes(GSolid* solid, bool before_rooms)
{
    std::vector<std::pair<double, GFace*>> portals;
    for (GFace* face : portal_faces(solid)) {
        portals.emplace_back(length(newell(*face)), face);
    }
    std::stable_sort(portals.begin(), portals.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

    int changed = 0;
    std::unordered_set<GFace*> done;
    std::vector<GFace*> queue;
    for (const auto& [area, seed] : portals) {
        if (done.count(seed)) continue;
        const Plane ref = seed->plane;
        const Vec ref_normal = to_vec(ref.normal);
        auto in_plane = [&](const GFace& face) {
            bool inside = true;
            for_each_face_corner(face, [&](GFaceVertex& fv) {
                const Vector3& pos = fv.vertex->pos;
                inside = inside && (before_rooms ? ref.side_of(pos) == PlaneSide::on
                                                 : std::abs(plane_distance(ref, pos)) <= repair_tolerance);
            });
            return inside;
        };
        done.insert(seed);
        queue.assign(1, seed);
        while (!queue.empty()) {
            GFace* face = queue.back();
            queue.pop_back();
            // The winding gives the facing even under a tilted plane, unless the piece is too thin for it
            const Vec winding = newell(*face);
            const double facing = dot(winding, ref_normal);
            const bool same = std::abs(facing) > 1e-6 * length(winding)
                                  ? facing > 0.0
                                  : dot(to_vec(face->plane.normal), ref_normal) >= 0.0;
            const Plane plane = same ? ref : Plane{{-ref.normal.x, -ref.normal.y, -ref.normal.z}, -ref.dist};
            if (std::memcmp(&face->plane, &plane, sizeof(Plane)) != 0) {
                face->plane = plane;
                changed++;
            }
            for_each_face_corner(*face, [&](GFaceVertex& fv) {
                const GVertex& vertex = *fv.vertex;
                for (int i = 0; i < vertex.face_refs.size; i++) {
                    GFace* other = vertex.face_refs.data_ptr[i];
                    if (is_portal(*other) && !is_detail(*other) && !done.count(other) && in_plane(*other)) {
                        done.insert(other);
                        queue.push_back(other);
                    }
                }
            });
        }
    }
    return changed;
}

// ─── After the flood fill ───

// Null unless exactly one room's bordering faces are in front; edge neighbours decide before corner neighbours
GRoom* room_in_front(GFace& portal)
{
    auto vote = [&](bool along_edges) {
        GRoom* found = nullptr;
        bool ambiguous = false;
        std::unordered_set<const GFace*> seen;
        for_each_edge(portal, [&](GVertex* a, GVertex* b) {
            for (int i = 0; i < a->face_refs.size; i++) {
                GFace* other = a->face_refs.data_ptr[i];
                if (other == &portal || !is_regular(*other) || !other->which_room || other->which_room->is_detail
                    || (along_edges && !is_edge(*other, a, b)) || !seen.insert(other).second
                    || !other->in_front_of(portal)) {
                    continue;
                }
                ambiguous = ambiguous || (found && found != other->which_room);
                found = other->which_room;
            }
        });
        return std::pair{found, ambiguous};
    };
    auto [room, ambiguous] = vote(true);
    if (!room && !ambiguous) {
        std::tie(room, ambiguous) = vote(false);
    }
    return ambiguous ? nullptr : room;
}

// A copy the flood fill put in the wrong room leaves the other room open
int fix_portal_twins(GSolid* solid)
{
    std::map<std::vector<GVertex*>, std::size_t> slot;
    std::vector<std::vector<GFace*>> pairs;
    for (GFace* face : portal_faces(solid)) {
        const auto [it, added] = slot.try_emplace(sorted_vertices(*face), pairs.size());
        if (added) {
            pairs.emplace_back();
        }
        pairs[it->second].push_back(face);
    }
    int moved = 0;
    for (const std::vector<GFace*>& faces : pairs) {
        if (faces.size() != 2) continue;
        GFace* a = faces[0];
        GFace* b = faces[1];
        if (dot(to_vec(a->plane.normal), to_vec(b->plane.normal)) >= 0.0) {
            continue;
        }
        if (a->which_room && b->which_room && a->which_room != b->which_room) {
            continue;
        }
        GRoom* room_a = room_in_front(*a);
        GRoom* room_b = room_in_front(*b);
        // one room in front of both is a portal inside one room, which portal linking removes
        if (!room_a || !room_b || room_a == room_b) {
            continue;
        }
        for (auto [face, room] : {std::pair{a, room_a}, std::pair{b, room_b}}) {
            if (face->which_room != room) {
                room->add_face(face);
                moved++;
            }
        }
    }
    return moved;
}

// One edge along a portal's boundary can span several of its pieces, so extents count as well as corners
bool on_portal(const GVertex& a, const GVertex& b, const std::vector<PortalBox>& portals)
{
    auto in_plane = [&](const Plane& plane) {
        return std::abs(plane_distance(plane, a.pos)) <= repair_tolerance
            && std::abs(plane_distance(plane, b.pos)) <= repair_tolerance;
    };
    for (const GVertex* v : {&a, &b}) {
        for (int i = 0; i < v->face_refs.size; i++) {
            const GFace* face = v->face_refs.data_ptr[i];
            if (is_portal(*face) && !is_detail(*face) && in_plane(face->plane)) {
                return true;
            }
        }
    }
    for (const PortalBox& p : portals) {
        if ((near_box(a.pos, p.min, p.max) || near_box(b.pos, p.min, p.max)) && in_plane(p.plane)) {
            return true;
        }
    }
    return false;
}

bool portal_between(GRoom* a, GRoom* b)
{
    std::vector<const GFace*> in_b;
    for_each_room_face(b, [&](GFace& f) {
        if (is_portal(f)) {
            in_b.push_back(&f);
        }
    });
    bool found = false;
    for_each_room_face(a, [&](GFace& p) {
        if (found || !is_portal(p)) {
            return;
        }
        for (const GFace* q : in_b) {
            const bool overlap = p.bounding_box_min.x <= q->bounding_box_max.x + repair_tolerance
                && q->bounding_box_min.x <= p.bounding_box_max.x + repair_tolerance
                && p.bounding_box_min.y <= q->bounding_box_max.y + repair_tolerance
                && q->bounding_box_min.y <= p.bounding_box_max.y + repair_tolerance
                && p.bounding_box_min.z <= q->bounding_box_max.z + repair_tolerance
                && q->bounding_box_min.z <= p.bounding_box_max.z + repair_tolerance;
            if (!overlap || dot(to_vec(p.plane.normal), to_vec(q->plane.normal)) > -parallel_cosine) {
                continue;
            }
            bool coplanar = true;
            for_each_face_corner(*q, [&](GFaceVertex& fv) {
                coplanar = coplanar && std::abs(plane_distance(p.plane, fv.vertex->pos)) <= repair_tolerance;
            });
            if (coplanar) {
                found = true;
                return;
            }
        }
    });
    return found;
}

// Rooms with no portal between them that share an edge away from every portal are one space the flood fill split
int merge_fragment_rooms(GSolid* solid)
{
    std::vector<PortalBox> portals;
    for (GFace* face : portal_faces(solid)) {
        portals.push_back(box_of(*face));
    }
    DisjointSets<GRoom*> sets;
    std::map<std::pair<GRoom*, GRoom*>, bool> separated;
    solid->for_each_face([&](GFace& face) {
        GRoom* room = face.which_room;
        if (!is_regular(face) || !room || room->is_detail) {
            return false;
        }
        for_each_edge(face, [&](GVertex* a, GVertex* b) {
            for (int i = 0; i < a->face_refs.size; i++) {
                const GFace* other = a->face_refs.data_ptr[i];
                GRoom* other_room = other->which_room;
                if (other == &face || !is_regular(*other) || !other_room || other_room->is_detail
                    || sets.find(other_room) == sets.find(room) || !has_face_ref(*b, other)
                    || on_portal(*a, *b, portals)) {
                    continue;
                }
                const auto key = room < other_room ? std::pair{room, other_room} : std::pair{other_room, room};
                auto known = separated.find(key);
                if (known == separated.end()) {
                    known = separated.emplace(key, portal_between(room, other_room)).first;
                }
                if (!known->second) {
                    sets.join(room, other_room);
                }
            }
        });
        return false;
    });

    int merged = 0;
    for (const std::vector<GRoom*>& rooms : sets.groups()) {
        GRoom* primary = *std::max_element(rooms.begin(), rooms.end(), [](const GRoom* x, const GRoom* y) {
            return x->face_list_count < y->face_list_count;
        });
        for (GRoom* room : rooms) {
            if (room == primary) continue;
            std::vector<GFace*> faces;
            for_each_room_face(room, [&](GFace& f) { faces.push_back(&f); });
            unlink_room_faces(room, {faces.begin(), faces.end()});
            for (GFace* f : faces) {
                primary->add_face(f);
            }
            const Vector3 where = room->bbox_min;
            // As merge_geoable_interior_rooms leaves one: skip_empty_detail_rooms_in_loop2 and lookups pass it by
            room->bbox_min = {0.0f, 0.0f, 0.0f};
            room->bbox_max = {0.0f, 0.0f, 0.0f};
            room->set_detail(solid, 1);
            merged++;
            xlog::info("[BuildRepair] joined a split room ({} faces) at ({:.3f} {:.3f} {:.3f})", faces.size(), where.x,
                       where.y, where.z);
        }
        recompute_room_bbox(primary);
    }
    return merged;
}

// ─── Open edges ───

struct OpenEdge
{
    GFace* face;
    GVertex* a;
    GVertex* b;
};

using RoomOpenEdges = std::vector<std::pair<GRoom*, std::vector<OpenEdge>>>;

// CheckForHoles' rule (0x0043c420): an edge is open unless another face of its room has both ends
RoomOpenEdges open_edges(GSolid* solid)
{
    RoomOpenEdges out;
    for (GRoom* room : normal_rooms(solid)) {
        std::vector<OpenEdge> list;
        for_each_room_face(room, [&](GFace& face) {
            for_each_edge(face, [&](GVertex* a, GVertex* b) {
                bool closed = false;
                for (int i = 0; i < a->face_refs.size && !closed; i++) {
                    const GFace* other = a->face_refs.data_ptr[i];
                    closed = other != &face && other->which_room == room && has_face_ref(*b, other);
                }
                if (!closed) {
                    list.push_back({&face, a, b});
                }
            });
        });
        if (!list.empty()) {
            out.emplace_back(room, std::move(list));
        }
    }
    return out;
}

std::vector<GVertex*> open_edge_ends(const std::vector<OpenEdge>& list)
{
    std::vector<GVertex*> ends;
    for (const OpenEdge& e : list) {
        ends.push_back(e.a);
        ends.push_back(e.b);
    }
    std::sort(ends.begin(), ends.end());
    ends.erase(std::unique(ends.begin(), ends.end()), ends.end());
    std::stable_sort(ends.begin(), ends.end(), [](const GVertex* a, const GVertex* b) {
        return std::tie(a->pos.x, a->pos.y, a->pos.z) < std::tie(b->pos.x, b->pos.y, b->pos.z);
    });
    return ends;
}

// Open edges sharing a vertex make one hole, as CheckForHoles counts them
int count_holes(const RoomOpenEdges& edges)
{
    int holes = 0;
    for (const auto& [room, list] : edges) {
        DisjointSets<GVertex*> sets;
        for (const OpenEdge& e : list) {
            sets.join(e.a, e.b);
        }
        std::unordered_set<GVertex*> roots;
        for (const OpenEdge& e : list) {
            roots.insert(sets.find(e.a));
        }
        holes += static_cast<int>(roots.size());
    }
    return holes;
}

// ─── Spatial queries ───

// As the point lookup tests a face: the stored plane, then the corners in its dominant projection
bool segment_crosses(const GFace& face, const Vec& a, const Vec& b)
{
    const Vec n = to_vec(face.plane.normal);
    const double da = dot(n, a) + face.plane.dist;
    const double db = dot(n, b) + face.plane.dist;
    if ((da > 0.0) == (db > 0.0)) {
        return false;
    }
    const Vec p = a + (b - a) * (da / (da - db));
    const double ax = std::abs(n.x);
    const double ay = std::abs(n.y);
    const double az = std::abs(n.z);
    auto project = [&](const Vec& v) {
        if (ax >= ay && ax >= az) {
            return std::pair{v.y, v.z};
        }
        if (ay >= az) {
            return std::pair{v.x, v.z};
        }
        return std::pair{v.x, v.y};
    };
    const auto [px, py] = project(p);
    bool inside = false;
    for_each_face_corner(face, [&](GFaceVertex& fv) {
        const auto [x0, y0] = project(to_vec(fv.prev->vertex->pos));
        const auto [x1, y1] = project(to_vec(fv.vertex->pos));
        if ((y1 > py) != (y0 > py) && px < (x0 - x1) * (py - y1) / (y0 - y1) + x1) {
            inside = !inside;
        }
    });
    return inside;
}

// Faces too large for the 1-unit grid are kept apart and tested by bounding box first
class FaceGrid
{
public:
    explicit FaceGrid(const std::vector<GFace*>& faces)
    {
        for (GFace* face : faces) {
            const auto lo = cell_of(face->bounding_box_min);
            const auto hi = cell_of(face->bounding_box_max);
            long long count = 1;
            for (int k = 0; k < 3; k++) {
                count *= static_cast<long long>(hi[k]) - lo[k] + 1;
            }
            if (count > max_cells_per_face) {
                large_.push_back(face);
                continue;
            }
            for (int x = lo[0]; x <= hi[0]; x++) {
                for (int y = lo[1]; y <= hi[1]; y++) {
                    for (int z = lo[2]; z <= hi[2]; z++) {
                        cells_[key(x, y, z)].push_back(face);
                    }
                }
            }
        }
    }

    std::vector<GFace*> around(const Vector3& p) const
    {
        std::vector<GFace*> out;
        for (GFace* face : large_) {
            if (near_box(p, face->bounding_box_min, face->bounding_box_max)) {
                out.push_back(face);
            }
        }
        const float pad = static_cast<float>(repair_tolerance);
        visit({p.x - pad, p.y - pad, p.z - pad}, {p.x + pad, p.y + pad, p.z + pad}, [&](GFace* face) {
            out.push_back(face);
            return false;
        });
        return out;
    }

    bool crosses(const Vec& a, const Vec& b, const GFace* skip) const
    {
        const Vector3 lo{static_cast<float>(std::min(a.x, b.x)), static_cast<float>(std::min(a.y, b.y)),
                         static_cast<float>(std::min(a.z, b.z))};
        const Vector3 hi{static_cast<float>(std::max(a.x, b.x)), static_cast<float>(std::max(a.y, b.y)),
                         static_cast<float>(std::max(a.z, b.z))};
        auto test = [&](const GFace* face) {
            const Vector3& fmin = face->bounding_box_min;
            const Vector3& fmax = face->bounding_box_max;
            return face != skip && lo.x <= fmax.x && hi.x >= fmin.x && lo.y <= fmax.y && hi.y >= fmin.y
                && lo.z <= fmax.z && hi.z >= fmin.z && segment_crosses(*face, a, b);
        };
        for (const GFace* face : large_) {
            if (test(face)) {
                return true;
            }
        }
        return visit(lo, hi, test);
    }

private:
    static constexpr long long max_cells_per_face = 512;
    static constexpr double max_cell = 1e6;

    static std::array<int, 3> cell_of(const Vector3& v)
    {
        // a corrupt coordinate must not reach the integer cast
        auto cell = [](float c) {
            const double f = std::floor(static_cast<double>(c));
            return static_cast<int>(f >= -max_cell && f <= max_cell ? f : 0.0);
        };
        return {cell(v.x), cell(v.y), cell(v.z)};
    }

    static long long key(int x, int y, int z)
    {
        return (static_cast<long long>(x & 0x1FFFFF) << 42) | (static_cast<long long>(y & 0x1FFFFF) << 21)
            | static_cast<long long>(z & 0x1FFFFF);
    }

    template<typename Fn>
    bool visit(const Vector3& lo, const Vector3& hi, Fn&& fn) const
    {
        const auto cl = cell_of(lo);
        const auto ch = cell_of(hi);
        std::unordered_set<const GFace*> seen;
        for (int x = cl[0]; x <= ch[0]; x++) {
            for (int y = cl[1]; y <= ch[1]; y++) {
                for (int z = cl[2]; z <= ch[2]; z++) {
                    const auto it = cells_.find(key(x, y, z));
                    if (it == cells_.end()) continue;
                    for (GFace* face : it->second) {
                        if (seen.insert(face).second && fn(face)) {
                            return true;
                        }
                    }
                }
            }
        }
        return false;
    }

    std::unordered_map<long long, std::vector<GFace*>> cells_;
    std::vector<GFace*> large_;
};

// ─── Cracks ───

// False if fewer than three corners remain
bool drop_repeated_corners(GFace& face)
{
    int count = face_corner_count(face);
    GFaceVertex* fv = face.edge_loop;
    for (int unchanged = 0; count > 1 && unchanged < count;) {
        GFaceVertex* next = fv->next;
        if (fv->vertex != next->vertex) {
            fv = next;
            unchanged++;
            continue;
        }
        fv->next = next->next;
        next->next->prev = fv;
        if (face.edge_loop == next) {
            face.edge_loop = fv;
        }
        GFaceVertex::free(next);
        count--;
        unchanged = 0;
    }
    return count >= 3;
}

void remove_face(GSolid* solid, GFace* face)
{
    solid->remove_face(face);
    GFace::destroy(face);
}

// Within BuildFinalRenderSolid's weld distance of the line through the two corners farthest apart
bool is_line(const GFace& face)
{
    const std::vector<GFaceVertex*> ring = face_corner_list(face);
    Vec a;
    Vec b;
    double longest = 0.0;
    for (std::size_t i = 0; i < ring.size(); i++) {
        for (std::size_t j = i + 1; j < ring.size(); j++) {
            const Vec p = to_vec(ring[i]->vertex->pos);
            const Vec q = to_vec(ring[j]->vertex->pos);
            if (length(q - p) > longest) {
                longest = length(q - p);
                a = p;
                b = q;
            }
        }
    }
    if (!(longest > 0.0)) {
        return true;
    }
    const Vec dir = (b - a) * (1.0 / longest);
    for (const GFaceVertex* fv : ring) {
        const Vec v = to_vec(fv->vertex->pos) - a;
        if (length(v - dir * dot(v, dir)) > red_epsilon) {
            return false;
        }
    }
    return true;
}

// The boolean can leave a zero-area face along a seam whose long edge counts as a hole. It goes only when every
// edge it closes for another face of the room is closed by a third face too.
int remove_line_faces(GSolid* solid, std::unordered_set<GRoom*>& touched)
{
    int removed = 0;
    for (const auto& [room, list] : open_edges(solid)) {
        // a face's open edges are listed together
        std::vector<GFace*> candidates;
        for (const OpenEdge& edge : list) {
            if (candidates.empty() || candidates.back() != edge.face) {
                candidates.push_back(edge.face);
            }
        }
        for (GFace* face : candidates) {
            // a room's last face stays, so no room is left empty
            if (!is_regular(*face) || !is_line(*face) || (room->face_list_head == face && !face->next_room)) {
                continue;
            }
            bool safe = true;
            for_each_face_corner(*face, [&](GFaceVertex& fv) {
                const GVertex& a = *fv.vertex;
                for (int i = 0; i < a.face_refs.size; i++) {
                    const GFace* other = a.face_refs.data_ptr[i];
                    if (other == face || other->which_room != room) {
                        continue;
                    }
                    for_each_edge(*other, [&](const GVertex* p, const GVertex* q) {
                        if (p != &a || !has_face_ref(*q, face)) {
                            return;
                        }
                        bool closed = false;
                        for (int k = 0; k < p->face_refs.size && !closed; k++) {
                            const GFace* third = p->face_refs.data_ptr[k];
                            closed = third != face && third != other && third->which_room == room
                                  && has_face_ref(*q, third);
                        }
                        safe = safe && closed;
                    });
                }
            });
            if (!safe) continue;
            touched.insert(room);
            remove_face(solid, face);
            removed++;
        }
    }
    return removed;
}

// A face that would lose two corners that are not neighbours keeps them. The dropped vertex is not freed.
int weld_cracks(GSolid* solid, std::unordered_set<GRoom*>& touched)
{
    DisjointSets<GVertex*> sets;
    for (const auto& [room, list] : open_edges(solid)) {
        const std::vector<GVertex*> ends = open_edge_ends(list);
        for (std::size_t i = 0; i < ends.size(); i++) {
            // ordered by x, so only ends within the tolerance in x need a distance test
            for (std::size_t j = i + 1; j < ends.size() && ends[j]->pos.x - ends[i]->pos.x <= repair_tolerance; j++) {
                if (length(to_vec(ends[i]->pos) - to_vec(ends[j]->pos)) <= repair_tolerance
                    && no_detail_faces(*ends[i]) && no_detail_faces(*ends[j])) {
                    sets.join(ends[i], ends[j]);
                }
            }
        }
    }

    int welds = 0;
    for (const std::vector<GVertex*>& members : sets.groups()) {
        const std::unordered_set<GVertex*> member_set{members.begin(), members.end()};
        std::vector<GFace*> faces;
        std::unordered_set<GFace*> seen;
        for (GVertex* v : members) {
            for (int i = 0; i < v->face_refs.size; i++) {
                if (seen.insert(v->face_refs.data_ptr[i]).second) {
                    faces.push_back(v->face_refs.data_ptr[i]);
                }
            }
        }
        bool safe = true;
        for (GFace* face : faces) {
            const std::vector<GFaceVertex*> ring = face_corner_list(*face);
            int count = 0;
            int runs = 0;
            for (std::size_t k = 0; k < ring.size(); k++) {
                const bool in = member_set.count(ring[k]->vertex) > 0;
                const bool prev_in = member_set.count(ring[(k + ring.size() - 1) % ring.size()]->vertex) > 0;
                count += in;
                runs += in && !prev_in;
            }
            if (count > 1 && runs != 1 && count != static_cast<int>(ring.size())) {
                safe = false;
            }
        }
        if (!safe) continue;
        for (GFace* face : faces) {
            if (face->which_room) {
                touched.insert(face->which_room);
            }
        }
        GVertex* keep = *std::max_element(members.begin(), members.end(), [](const GVertex* x, const GVertex* y) {
            return x->face_refs.size < y->face_refs.size;
        });
        for (GVertex* v : members) {
            if (v == keep) continue;
            for (int i = 0; i < v->face_refs.size; i++) {
                GFace* face = v->face_refs.data_ptr[i];
                for_each_face_corner(*face, [&](GFaceVertex& fv) {
                    if (fv.vertex == v) {
                        fv.vertex = keep;
                    }
                });
                keep->face_refs.add(face);
            }
            v->face_refs.size = 0;
            solid->remove_vertex(v);
            welds++;
        }
        for (GFace* face : faces) {
            if (!drop_repeated_corners(*face) || !face->compute_plane_and_bbox()) {
                remove_face(solid, face);
                g_stats.faces_removed++;
            }
        }
    }
    return welds;
}

double segment_distance(const Vec& p, const Vec& a, const Vec& b, double& along)
{
    const Vec ab = b - a;
    const double len = length(ab);
    if (len <= 0.0) {
        along = 0.0;
        return length(p - a);
    }
    along = dot(p - a, ab) / len;
    return length(p - (a + ab * (along / len)));
}

// The edge split need not be open: CheckForHoles counts an edge closed by any other face that has both its ends
int split_tjunctions(GSolid* solid, std::unordered_set<GRoom*>& touched)
{
    int splits = 0;
    for (const auto& [room, list] : open_edges(solid)) {
        std::vector<GFace*> faces;
        for_each_room_face(room, [&](GFace& f) { faces.push_back(&f); });
        const FaceGrid grid{faces};
        for (GVertex* c : open_edge_ends(list)) {
            if (!no_detail_faces(*c)) continue;
            const Vec p = to_vec(c->pos);
            GVertex* edge_a = nullptr;
            GVertex* edge_b = nullptr;
            for (GFace* face : grid.around(c->pos)) {
                if (edge_a) break;
                if (is_detail(*face) || has_face_ref(*c, face)) {
                    continue;
                }
                for_each_edge(*face, [&](GVertex* a, GVertex* b) {
                    const double len = length(to_vec(b->pos) - to_vec(a->pos));
                    double along = 0.0;
                    if (!edge_a && segment_distance(p, to_vec(a->pos), to_vec(b->pos), along) <= repair_tolerance
                        && along > repair_tolerance && along < len - repair_tolerance) {
                        edge_a = a;
                        edge_b = b;
                    }
                });
            }
            if (!edge_a) continue;
            std::vector<GFace*> sharing;
            for (int i = 0; i < edge_a->face_refs.size; i++) {
                if (has_face_ref(*edge_b, edge_a->face_refs.data_ptr[i])) {
                    sharing.push_back(edge_a->face_refs.data_ptr[i]);
                }
            }
            for (GFace* face : sharing) {
                if (face->which_room) {
                    touched.insert(face->which_room);
                }
            }
            solid->insert_vertex_on_edge(edge_a, edge_b, c);
            // the vertex can sit up to the tolerance off the edge, past the faces' bounding boxes
            for (GFace* face : sharing) {
                face->compute_plane_and_bbox();
            }
            splits++;
        }
    }
    return splits;
}

// ─── Fills ───

struct Loop
{
    std::vector<GVertex*> vertices;
    std::vector<GFace*> faces;
};

// Each wound as a face filling it would be
std::vector<Loop> open_loops(const std::vector<OpenEdge>& list)
{
    // a face across open edge a->b runs b->a
    std::unordered_map<GVertex*, std::vector<const OpenEdge*>> from;
    for (const OpenEdge& e : list) {
        from[e.b].push_back(&e);
    }
    std::unordered_set<const OpenEdge*> used;
    std::vector<Loop> loops;
    for (const OpenEdge& start : list) {
        if (used.count(&start)) continue;
        Loop loop;
        const OpenEdge* e = &start;
        bool closed = false;
        for (std::size_t guard = 0; guard <= list.size(); guard++) {
            used.insert(e);
            loop.vertices.push_back(e->b);
            loop.faces.push_back(e->face);
            const auto it = from.find(e->a);
            if (it == from.end() || it->second.size() != 1) {
                break;
            }
            e = it->second.front();
            if (e == &start) {
                closed = true;
                break;
            }
            if (used.count(e)) break;
        }
        if (closed && loop.vertices.size() >= 3) {
            loops.push_back(std::move(loop));
        }
    }
    return loops;
}

// Portal linking deletes a copy whose neighbours it can't place; the room gets the opposite of the copy across
int fill_portal_gaps(GSolid* solid, std::unordered_set<GRoom*>& touched)
{
    std::map<std::vector<GVertex*>, std::vector<GFace*>> portals;
    for (GFace* face : portal_faces(solid)) {
        if (face->which_room) {
            portals[sorted_vertices(*face)].push_back(face);
        }
    }
    int fills = 0;
    for (const auto& [room, list] : open_edges(solid)) {
        for (const Loop& loop : open_loops(list)) {
            std::vector<GVertex*> key = loop.vertices;
            std::sort(key.begin(), key.end());
            const auto it = portals.find(key);
            if (it == portals.end() || it->second.size() != 1) {
                continue;
            }
            GFace* source = it->second.front();
            GRoom* other = source->which_room;
            // the copy across winds against the loop, as the new copy must wind with it
            if (other == room || other->is_detail || dot(newell(*source), newell(positions(loop.vertices))) >= 0.0) {
                continue;
            }
            const std::vector<GFaceVertex*> ring = face_corner_list(*source);
            touched.insert(room);
            const GFaceAttributes attrs = attributes_of(*source);
            GFace* face = solid->create_face(&attrs);
            if (!face) continue;
            for (auto fv = ring.rbegin(); fv != ring.rend(); ++fv) {
                face->add_vertex((*fv)->vertex, (*fv)->u, (*fv)->v, (*fv)->lm_u, (*fv)->lm_v);
            }
            if (!face->compute_plane_and_bbox()) {
                remove_face(solid, face);
                continue;
            }
            room->add_face(face);
            solid->link_portal_face(face, room, other);
            fills++;
        }
    }
    return fills;
}

// A lost portal leaves a doorway, not a wall gap
bool in_doorway(const Loop& loop)
{
    for (std::size_t k = 0; k < loop.vertices.size(); k++) {
        if (on_portal(*loop.vertices[k], *loop.vertices[(k + 1) % loop.vertices.size()], g_portal_boxes)) {
            return true;
        }
    }
    return false;
}

int fill_wall_gaps(GSolid* solid, std::unordered_set<GRoom*>& touched)
{
    int fills = 0;
    for (const auto& [room, list] : open_edges(solid)) {
        for (const Loop& loop : open_loops(list)) {
            const bool regular =
                std::all_of(loop.faces.begin(), loop.faces.end(), [](const GFace* f) { return is_regular(*f); });
            if (loop.vertices.size() > max_face_corners || !regular) {
                continue;
            }
            const std::vector<Vec> points = positions(loop.vertices);
            const Vec n = newell(points);
            const double twice_area = length(n);
            if (!(twice_area > 0.0) || twice_area > 2.0 * max_fill_area) {
                continue;
            }
            GFace* wall = nullptr;
            for (GFace* f : loop.faces) {
                bool in_plane = dot(n, to_vec(f->plane.normal)) >= parallel_cosine * twice_area;
                for (GVertex* v : loop.vertices) {
                    in_plane = in_plane && std::abs(plane_distance(f->plane, v->pos)) <= repair_tolerance;
                }
                if (in_plane) {
                    wall = f;
                    break;
                }
            }
            const bool flat = wall != nullptr;
            const Vec wall_normal = flat ? to_vec(wall->plane.normal) : n * (1.0 / twice_area);
            // corners on a straight line count as convex
            bool convex = true;
            for (std::size_t k = 0; k < points.size(); k++) {
                const Vec& p0 = points[(k + points.size() - 1) % points.size()];
                const Vec& p1 = points[k];
                const Vec& p2 = points[(k + 1) % points.size()];
                convex = convex && dot(cross(p1 - p0, p2 - p1), wall_normal) >= -1e-9;
            }
            if (!flat || !convex || in_doorway(loop)) {
                xlog::info("[BuildRepair] gap of {} corners at ({:.3f} {:.3f} {:.3f}) left open", loop.vertices.size(),
                           points[0].x, points[0].y, points[0].z);
                continue;
            }

            const std::vector<GFaceVertex*> ring = face_corner_list(*wall);
            if (ring.size() < 3) continue;
            const Vec origin = to_vec(ring[0]->vertex->pos);
            const Vec e1 = to_vec(ring[1]->vertex->pos) - origin;
            const double e1_length = length(e1);
            if (!(e1_length > 0.0)) continue;
            const Vec axis_u = e1 * (1.0 / e1_length);
            const Vec axis_v = cross(wall_normal, axis_u);
            std::size_t third = 2;
            double best = 0.0;
            for (std::size_t k = 2; k < ring.size(); k++) {
                const double a = std::abs(dot(cross(e1, to_vec(ring[k]->vertex->pos) - origin), wall_normal));
                if (a > best) {
                    best = a;
                    third = k;
                }
            }
            auto st = [&](const Vec& p) { return std::pair{dot(p - origin, axis_u), dot(p - origin, axis_v)}; };
            const auto [s1, t1] = st(to_vec(ring[1]->vertex->pos));
            const auto [s2, t2] = st(to_vec(ring[third]->vertex->pos));
            const double det = s1 * t2 - s2 * t1;
            if (!(std::abs(det) > 1e-12)) continue;
            auto mapped = [&](double c0, double c1, double c2, const Vec& p) {
                const double ds = ((c1 - c0) * t2 - (c2 - c0) * t1) / det;
                const double dt = (s1 * (c2 - c0) - s2 * (c1 - c0)) / det;
                const auto [s, t] = st(p);
                return static_cast<float>(c0 + ds * s + dt * t);
            };

            touched.insert(room);
            GFaceAttributes attrs = attributes_of(*wall);
            attrs.portal_id = 0;
            attrs.surface_index = -1;
            GFace* face = solid->create_face(&attrs);
            if (!face) continue;
            for (std::size_t k = 0; k < loop.vertices.size(); k++) {
                face->add_vertex(loop.vertices[k], mapped(ring[0]->u, ring[1]->u, ring[third]->u, points[k]),
                                 mapped(ring[0]->v, ring[1]->v, ring[third]->v, points[k]), 0.0f, 0.0f);
            }
            if (!face->compute_plane_and_bbox()) {
                remove_face(solid, face);
                continue;
            }
            face->plane = wall->plane;
            room->add_face(face);
            xlog::info("[BuildRepair] filled a gap of {} corners at ({:.3f} {:.3f} {:.3f})", loop.vertices.size(),
                       points[0].x, points[0].y, points[0].z);
            fills++;
        }
    }
    return fills;
}

// ─── Out-of-level check ───

bool has_clearance(const FaceGrid& walls, const Vec& p)
{
    constexpr double clearance = 0.1;
    for (const Vec& axis : {Vec{1.0, 0.0, 0.0}, Vec{-1.0, 0.0, 0.0}, Vec{0.0, 1.0, 0.0}, Vec{0.0, -1.0, 0.0},
                            Vec{0.0, 0.0, 1.0}, Vec{0.0, 0.0, -1.0}}) {
        if (walls.crosses(p, p + axis * clearance, nullptr)) {
            return false;
        }
    }
    return true;
}

// Out-of-level marks are red, apart from CheckForHoles' blue edges and yellow pointers
bool is_marker(const HoleLine& line)
{
    return line.r == 255 && line.g == 0 && line.b == 0;
}

void add_marker(const Vector3& pos)
{
    auto line = [&](const Vector3& from, const Vector3& to) {
        auto* l = static_cast<HoleLine*>(editor_alloc(sizeof(HoleLine)));
        if (!l) return;
        *l = HoleLine{255, 0, 0, 0, from, to};
        hole_lines.push_back(l);
    };
    constexpr float arm = 0.25f;
    line({pos.x - arm, pos.y, pos.z}, {pos.x + arm, pos.y, pos.z});
    line({pos.x, pos.y - arm, pos.z}, {pos.x, pos.y + arm, pos.z});
    line({pos.x, pos.y, pos.z - arm}, {pos.x, pos.y, pos.z + arm});
    line(pos, {pos.x, pos.y + 4.0f, pos.z});
}

void find_out_of_level(GSolid* solid)
{
    report("Checking for out-of-level areas...");
    // an earlier check's marks; the hole check's lines stay
    for (int i = hole_lines.size - 1; i >= 0; i--) {
        if (hole_lines.data_ptr[i] && is_marker(*hole_lines.data_ptr[i])) {
            editor_free(hole_lines.data_ptr[i]);
            hole_lines.remove_at(i);
        }
    }
    std::vector<Vector3> outside;
    int tested = 0;
    int rooms_checked = 0;
    // closer in would find gaps too thin to stand in
    constexpr double depth = 0.3;
    const std::vector<GRoom*> rooms = normal_rooms(solid);
    for (std::size_t r = 0; r < rooms.size(); r++) {
        build_progress_steps(r, rooms.size());
        GRoom* room = rooms[r];
        if (room->is_sky) continue;
        rooms_checked++;
        std::vector<GFace*> faces;
        std::vector<GFace*> walls;
        for_each_room_face(room, [&](GFace& f) {
            if ((f.flags & (FACE_LIQUID | FACE_IS_DETAIL)) == 0) {
                faces.push_back(&f);
                if (is_regular(f)) {
                    walls.push_back(&f);
                }
            }
        });
        const FaceGrid grid{faces};
        const FaceGrid wall_grid{walls};
        for (GFace* face : faces) {
            const std::vector<GFaceVertex*> ring = face_corner_list(*face);
            if (ring.size() < 3) continue;
            Vec center;
            for (const GFaceVertex* fv : ring) {
                center = center + to_vec(fv->vertex->pos);
            }
            center = center * (1.0 / static_cast<double>(ring.size()));
            std::vector<Vec> starts{center};
            for (const GFaceVertex* fv : ring) {
                starts.push_back(center + (to_vec(fv->vertex->pos) - center) * 0.8);
            }
            const Vec normal = to_vec(face->plane.normal);
            for (const Vec& start : starts) {
                const Vec p = start + normal * depth;
                // past another face of the room is outside it, and a gap too thin to stand in doesn't matter
                if (grid.crosses(start, p, face) || !has_clearance(wall_grid, p)) {
                    continue;
                }
                tested++;
                const Vector3 pos{static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z)};
                if (!solid->room_at(&pos)) {
                    outside.push_back(pos);
                }
            }
        }
    }
    build_progress_complete();
    constexpr std::size_t max_markers = 2000;
    for (std::size_t i = 0; i < outside.size() && i < max_markers; i++) {
        add_marker(outside[i]);
    }
    editor_views_mark_repaint_all();
    for (std::size_t i = 0; i < outside.size() && i < static_cast<std::size_t>(max_logged_examples); i++) {
        xlog::info("[BuildRepair] out of level at ({:.3f} {:.3f} {:.3f})", outside[i].x, outside[i].y, outside[i].z);
    }
    report("Checked " + std::to_string(tested) + " points in " + std::to_string(rooms_checked) + " rooms");
    report("Found " + std::to_string(outside.size()) + " out-of-level points" + (outside.empty() ? "" : " (red)"));
    if (outside.size() > max_markers) {
        report("Marked the first " + std::to_string(max_markers));
    }
}

bool near_edge(const GFace& face, const Vector3& point)
{
    bool close = false;
    for_each_edge(face, [&](const GVertex* a, const GVertex* b) {
        const Vec ab = to_vec(b->pos) - to_vec(a->pos);
        const Vec ap = to_vec(point) - to_vec(a->pos);
        const double len2 = dot(ab, ab);
        const double t = len2 > 0.0 ? std::clamp(dot(ap, ab) / len2, 0.0, 1.0) : 0.0;
        close = close || length(ap - ab * t) <= red_epsilon;
    });
    return close;
}

// ─── Hooks ───

// After the room builder's flood fill and before it turns closed solids into detail rooms (0x00485dd4,
// "MOV [ESP+0x14],0", 8 bytes; 0x00485aa8 branches to the instruction before it). The solid is the builder's
// this, saved at [ESP+0x20].
CodeInjection after_flood_injection{
    0x00485dd4,
    [](auto& regs) {
        if (!build_repair_enabled()) return;
        auto* solid = *reinterpret_cast<GSolid**>(regs.esp + 0x20);
        // std::bad_alloc must not unwind into the room builder
        try {
            g_flood_portals.clear();
            for (GFace* face : portal_faces(solid)) {
                g_flood_portals.emplace_back(face, box_of(*face));
            }
        }
        catch (const std::bad_alloc&) {
            g_flood_portals.clear();
            g_stats.portals_unknown = true;
            log_out_of_memory("recording the portals; wall gaps will be left open");
        }
        try {
            g_stats.twins += fix_portal_twins(solid);
            g_stats.rooms_merged += merge_fragment_rooms(solid);
        }
        catch (const std::bad_alloc&) {
            log_out_of_memory("after the flood fill; its repairs were skipped");
        }
    },
};

void repair_holes(GSolid* solid)
{
    report("Repairing holes...");
    std::unordered_set<GRoom*> touched;
    // repairs allocate only between edits, so a bad_alloc still leaves the touched rooms to finish below
    try {
        g_stats.holes_after_rooms = count_holes(open_edges(solid));
        g_stats.planes += unify_portal_planes(solid, false);
        g_stats.faces_removed += remove_line_faces(solid, touched);
        for (int round = 0; round < max_repair_rounds; round++) {
            const int welds = weld_cracks(solid, touched);
            const int splits = split_tjunctions(solid, touched);
            g_stats.welds += welds;
            g_stats.splits += splits;
            if (welds == 0 && splits == 0) {
                break;
            }
        }
        g_stats.portal_fills += fill_portal_gaps(solid, touched);
        if (!g_stats.portals_unknown) {
            g_stats.wall_fills += fill_wall_gaps(solid, touched);
        }
    }
    catch (const std::bad_alloc&) {
        log_out_of_memory("repairing holes; the rest of the repair was skipped");
    }
    try {
        if (!touched.empty()) {
            // the splits and fills can take a face past the corner limit the t-joint pass just enforced
            solid->split_large_faces();
        }
        unify_portal_planes(solid, false);
    }
    catch (const std::bad_alloc&) {
        log_out_of_memory("finishing the hole repair");
    }
    for (GRoom* room : touched) {
        recompute_room_bbox(room);
        room->rebuild_bbox();
    }
    const std::pair<const char*, int> counts[] = {
        {"Holes after building rooms", g_stats.holes_after_rooms},
        {"Portal planes unified", g_stats.planes},
        {"Portal copies moved", g_stats.twins},
        {"Split rooms joined", g_stats.rooms_merged},
        {"Vertices welded", g_stats.welds},
        {"Edges split", g_stats.splits},
        {"Collapsed faces removed", g_stats.faces_removed},
        {"Portal gaps filled", g_stats.portal_fills},
        {"Wall gaps filled", g_stats.wall_fills},
        {"Unplaced portal faces removed", g_stats.portals_deleted},
    };
    for (const auto& [name, count] : counts) {
        report(std::string{name} + ": " + std::to_string(count));
    }
}

// Point-in-face (0x0048c9e0) has no tolerance, so where coplanar faces meet a float step apart neither may claim the
// sample point and a buried face is kept. During a build a coplanar candidate also claims points within 1e-4 of its
// edges. At 0x0048e474, before that call ("LEA EAX,[ESP+0x24]; MOV ECX,EBP", 6 bytes); 0x0048e4b1 records the hit.
CodeInjection coplanar_seam_injection{
    0x0048e474,
    [](auto& regs) {
        const bool coplanar = *reinterpret_cast<std::uint8_t*>(regs.esp + 0x58) != 0;
        if (!coplanar || !build_repair_enabled()) return;
        auto* level = CDedLevel::Get();
        if (!level || !level->build_running) return;
        auto& face = *reinterpret_cast<GFace*>(static_cast<std::uintptr_t>(regs.ebp));
        const auto& hit = *reinterpret_cast<const Vector3*>(regs.esp + 0x24);
        if (near_edge(face, hit) && !face.contains(hit)) {
            regs.eip = 0x0048e4b1;
        }
    },
};

// GeoBuild_Driver's call of CheckForHoles_report (thiscall, RET 4). A call-site hook, as Level > Check for holes
// (0x0043c8f0) must not repair.
int __fastcall check_for_holes_call(void* app, int edx, char draw_lines);
CallHook<decltype(check_for_holes_call)> check_for_holes_call_hook{0x0043a176, check_for_holes_call};

int __fastcall check_for_holes_call(void* app, int edx, char draw_lines)
{
    auto* level = CDedLevel::Get();
    GSolid* solid = level ? level->solid : nullptr;
    if (solid && build_repair_enabled()) {
        build_progress_phase(BuildPhase::repair);
        // std::bad_alloc must not unwind into the build driver
        try {
            repair_holes(solid);
        }
        catch (const std::bad_alloc&) {
            log_out_of_memory("repairing holes");
        }
    }
    g_portal_boxes.clear();
    build_progress_phase(BuildPhase::holes);
    const int holes = check_for_holes_call_hook.call_target(app, edx, draw_lines);
    if (solid && build_out_of_level_enabled()) {
        build_progress_phase(BuildPhase::out_of_level);
        // the check's lookups would count in the build's "Rays recast" line
        const int recasts = point_lookup_recasts;
        try {
            find_out_of_level(solid);
        }
        catch (const std::bad_alloc&) {
            log_out_of_memory("checking for out-of-level areas");
        }
        point_lookup_recasts = recasts;
    }
    build_progress_phase(BuildPhase::finish);
    return holes;
}

// GeoBuild_Driver's t-joint pass on the level solid (thiscall)
void __fastcall level_tjoints_call(GSolid* solid, int edx);
CallHook<decltype(level_tjoints_call)> level_tjoints_call_hook{0x0043a10f, level_tjoints_call};

void __fastcall level_tjoints_call(GSolid* solid, int edx)
{
    build_progress_phase(BuildPhase::tjoints);
    level_tjoints_call_hook.call_target(solid, edx);
}

} // namespace

bool build_repair_enabled()
{
    return g_editor_settings.repair_build_holes;
}

void build_repair_toggle()
{
    g_editor_settings.repair_build_holes = !g_editor_settings.repair_build_holes;
}

bool build_out_of_level_enabled()
{
    return g_editor_settings.rebuild_out_of_level;
}

void build_out_of_level_toggle()
{
    g_editor_settings.rebuild_out_of_level = !g_editor_settings.rebuild_out_of_level;
}

void build_repair_before_rooms(GSolid* solid)
{
    g_stats = {};
    g_flood_portals.clear();
    g_portal_boxes.clear();
    if (!build_repair_enabled()) return;
    // called from the room builder's hook, which std::bad_alloc must not unwind into
    try {
        g_stats.planes += unify_portal_planes(solid, true);
    }
    catch (const std::bad_alloc&) {
        log_out_of_memory("unifying portal planes; the portals keep their own planes");
    }
}

void build_repair_after_rooms(GSolid* solid)
{
    try {
        if (build_out_of_level_enabled()) {
            build_progress_expect(BuildPhase::out_of_level, normal_rooms(solid).size());
        }
    }
    catch (const std::bad_alloc&) {
    }
    if (g_flood_portals.empty()) return;
    try {
        // Portal linking (0x00486062) frees each portal face it can't place between exactly two rooms
        std::unordered_set<const GFace*> left;
        solid->for_each_face([&](GFace& face) {
            left.insert(&face);
            return false;
        });
        for (const auto& [face, box] : g_flood_portals) {
            g_portal_boxes.push_back(box);
            if (left.count(face)) continue;
            g_stats.portals_deleted++;
            if (g_stats.portals_deleted <= max_logged_examples) {
                xlog::info("[BuildRepair] portal face near ({:.3f} {:.3f} {:.3f}) removed by portal linking",
                           box.min.x, box.min.y, box.min.z);
            }
        }
    }
    catch (const std::bad_alloc&) {
        g_stats.portals_unknown = true;
        log_out_of_memory("checking portal linking; wall gaps will be left open");
    }
    // the deleted faces are freed: only where they were is kept
    g_flood_portals.clear();
}

void build_repair_find_out_of_level()
{
    auto* level = CDedLevel::Get();
    GSolid* solid = level ? level->solid : nullptr;
    if (!solid || !solid->face_list_head) {
        report("Find out-of-level areas: the level has no built geometry to check");
        return;
    }
    try {
        find_out_of_level(solid);
    }
    catch (const std::bad_alloc&) {
        log_out_of_memory("checking for out-of-level areas");
    }
}

void ApplyBuildRepairPatches()
{
    after_flood_injection.install();
    coplanar_seam_injection.install();
    check_for_holes_call_hook.install();
    level_tjoints_call_hook.install();
}
