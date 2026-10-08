#include <windows.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <new>
#include <queue>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <common/lightmap/alpine_lightmap_reader.h>

#include "overflow_charts.h"
#include "alpine_lightmaps.h"
#include "bake_progress.h"
#include "level.h"
#include "mfc_types.h"
#include "terrain_build.h"
#include "work_pool.h"

namespace alm = alpine_lightmap;

namespace
{

void ov_log(const std::string& line)
{
    editor_report(EditorReportLevel::info, "AlpineLightmaps", line, false);
}

void ov_warn(const std::string& line)
{
    editor_report(EditorReportLevel::warn, "AlpineLightmaps", line, false);
}

// A face joins a chart within 30 degrees of the chart's seed while the chart stays within one tile at the
// starting density and, once past 8 x 8 texels, its faces fill at least 40% of its box.
constexpr float grow_cos = 0.8660254f;
constexpr double grow_fill_min = 0.4;
constexpr double grow_fill_free_texels = 8.0;
// The budget lowers a chart's density down to this before it drops charts.
constexpr float floor_density = 0.25f;
// Seams are blended across edges whose faces meet within 45 degrees, as surface edges are.
constexpr float seam_cos = 0.70710678f;
constexpr float seam_own = 9.0f / 16.0f;
constexpr float seam_other = 7.0f / 16.0f;
constexpr std::uint32_t page_blocks = alm::page_size / 4;
constexpr std::size_t light_batch = 4096;
// Grid texels of the tiles prepared ahead on the work pool at a time.
constexpr std::uint64_t prepare_run_texels = 1u << 18;
// Texels this close to a chart's faces are lit even outside its stored blocks, so the 3x3 filter sees light.
constexpr double shade_reach = 2.0;

struct V3
{
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

V3 operator+(V3 a, V3 b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

V3 operator-(V3 a, V3 b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

V3 operator*(V3 a, float s)
{
    return {a.x * s, a.y * s, a.z * s};
}

float dot(V3 a, V3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

V3 cross(V3 a, V3 b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

V3 normalized(V3 a)
{
    const float l = std::sqrt(dot(a, a));
    return l > 0.0f ? a * (1.0f / l) : V3{};
}

struct D2
{
    double x, y;
};

struct Face
{
    GFace* face;
    const GRoom* room;
    std::uint32_t ordinal;
    std::uint32_t first_vert;
    std::uint32_t num_verts;
    std::uint32_t smoothing;
    V3 normal;
    float area;
};

struct Chart
{
    std::uint32_t first_member = 0;
    std::uint32_t num_members = 0;
    V3 n, au, av, origin;
    float ext_u = 0.0f, ext_v = 0.0f, area = 0.0f;
    float density = 0.0f;
    std::uint16_t w = 0, h = 0;
    alm::ChartGeometry geom{};
    std::uint32_t first_mask = 0;
    std::uint32_t blocks = 0;
    std::uint32_t tile_base = 0; // relative to the first overflow tile
    double mean_sum[3] = {0.0, 0.0, 0.0};
    std::uint32_t mean_count = 0;
    double stored_sum[3] = {0.0, 0.0, 0.0}; // over every texel it owns, for faces too small to cover a centre
    std::uint32_t stored_count = 0;
    std::uint8_t mean[3] = {0, 0, 0};
};

// The 4x4 blocks of a tile's stored rect that hold texels its chart samples: bit bx of row by.
struct TileMask
{
    std::uint32_t first_row = 0;
    std::uint8_t bw = 0, bh = 0;
    std::uint16_t blocks = 0;
};

struct SharedEdge
{
    std::uint32_t a, b;   // faces
    std::uint32_t v0, v1; // the edge's vertices, in verts
};

struct State
{
    std::uint32_t src_num_faces = 0;
    std::uint32_t first_page = 0;
    std::vector<Face> faces; // by ordinal
    std::vector<V3> verts;
    std::vector<const GVertex*> vert_ids;
    std::vector<V3> corner_normals;
    std::vector<std::uint32_t> nbr_offsets, nbrs;
    std::vector<SharedEdge> shared_edges;
    std::vector<std::uint32_t> members; // chart-major, ascending ordinal within a chart
    std::vector<Chart> charts;
    std::vector<TileMask> masks;
    std::vector<std::uint64_t> mask_rows;
    OverflowLayout layout;
    std::uint64_t fingerprint = 0;
    bool unusable = false;
};

State g_ov;
std::unordered_map<const GFace*, std::array<std::uint8_t, 3>> g_preview;

// ─── faces ────────────────────────────────────────────────────────────────────

V3 vertex_pos(const GVertex* v)
{
    return {v->pos.x, v->pos.y, v->pos.z};
}

std::uintptr_t key(const void* p)
{
    return reinterpret_cast<std::uintptr_t>(p);
}

// Candidates in ordinal order: faces RED's surface pass would give a surface but left at -1.
void collect(const GSolid* solid, const std::vector<std::int32_t>& terrain_uids)
{
    struct VertFace
    {
        const GVertex* v;
        std::uint32_t smoothing;
        V3 n;
    };
    std::vector<VertFace> vf;
    std::vector<const GVertex*> ids;
    std::uint32_t ordinal = 0;
    for (GFace* f = solid->face_list_head; f; f = f->next_solid, ordinal++) {
        if (!alm::overflow_face_loop(f, ids) || ids.size() < 3) {
            continue;
        }
        double nx = 0.0, ny = 0.0, nz = 0.0;
        for (std::size_t k = 0; k < ids.size(); k++) {
            const Vector3& a = ids[k]->pos;
            const Vector3& b = ids[(k + 1) % ids.size()]->pos;
            nx += (static_cast<double>(a.y) - b.y) * (static_cast<double>(a.z) + b.z);
            ny += (static_cast<double>(a.z) - b.z) * (static_cast<double>(a.x) + b.x);
            nz += (static_cast<double>(a.x) - b.x) * (static_cast<double>(a.y) + b.y);
        }
        const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (!(len > 1e-12)) {
            continue;
        }
        V3 n{static_cast<float>(nx / len), static_cast<float>(ny / len), static_cast<float>(nz / len)};
        const V3 plane{f->plane.normal.x, f->plane.normal.y, f->plane.normal.z};
        if (dot(n, plane) < 0.0f) {
            n = n * -1.0f;
        }
        const auto smoothing = static_cast<std::uint32_t>(f->smoothing_groups);
        if (smoothing) {
            for (const GVertex* v : ids) {
                vf.push_back({v, smoothing, n});
            }
        }
        if (f->surface_index != -1 || !face_gets_stock_surface(f, terrain_uids)) {
            continue;
        }
        Face c{f,
               f->which_room,
               ordinal,
               static_cast<std::uint32_t>(g_ov.verts.size()),
               static_cast<std::uint32_t>(ids.size()),
               smoothing,
               n,
               static_cast<float>(0.5 * len)};
        for (const GVertex* v : ids) {
            g_ov.verts.push_back(vertex_pos(v));
            g_ov.vert_ids.push_back(v);
        }
        g_ov.faces.push_back(c);
    }
    g_ov.src_num_faces = ordinal;

    // A smoothed corner averages the plane normals of the faces sharing the vertex and a smoothing group,
    // each weighted by its agreement with the face, as the surface bake weights them.
    // stable, so each corner sums its faces in face order whatever the vertex addresses
    std::stable_sort(vf.begin(), vf.end(), [](const VertFace& a, const VertFace& b) { return key(a.v) < key(b.v); });
    g_ov.corner_normals.resize(g_ov.verts.size());
    for (const Face& c : g_ov.faces) {
        for (std::uint32_t k = 0; k < c.num_verts; k++) {
            V3 out = c.normal;
            if (c.smoothing) {
                const GVertex* v = g_ov.vert_ids[c.first_vert + k];
                auto it = std::lower_bound(vf.begin(), vf.end(), v,
                                           [](const VertFace& e, const GVertex* x) { return key(e.v) < key(x); });
                V3 sum{};
                for (; it != vf.end() && it->v == v; ++it) {
                    if (it->smoothing & c.smoothing) {
                        sum = sum + it->n * std::max(dot(c.normal, it->n), 0.0f);
                    }
                }
                const V3 s = normalized(sum);
                if (dot(s, s) > 0.0f) {
                    out = s;
                }
            }
            g_ov.corner_normals[c.first_vert + k] = out;
        }
    }
}

// Faces sharing an edge (the same two vertices) in the same room.
void build_adjacency()
{
    struct Edge
    {
        std::uint32_t room, a, b;
        std::uint32_t face, v0, v1;
    };
    // rooms and vertices numbered in face order, so the seams (blended in this order) match on every run
    std::unordered_map<const void*, std::uint32_t> ids;
    const auto id_of = [&ids](const void* p) {
        return ids.try_emplace(p, static_cast<std::uint32_t>(ids.size())).first->second;
    };
    std::vector<Edge> edges;
    edges.reserve(g_ov.verts.size());
    for (std::uint32_t i = 0; i < g_ov.faces.size(); i++) {
        const Face& c = g_ov.faces[i];
        const std::uint32_t room = id_of(c.room);
        for (std::uint32_t k = 0; k < c.num_verts; k++) {
            std::uint32_t v0 = c.first_vert + k;
            std::uint32_t v1 = c.first_vert + (k + 1) % c.num_verts;
            std::uint32_t a = id_of(g_ov.vert_ids[v0]);
            std::uint32_t b = id_of(g_ov.vert_ids[v1]);
            if (a > b) {
                std::swap(a, b);
                std::swap(v0, v1);
            }
            edges.push_back({room, a, b, i, v0, v1});
        }
    }
    ids = {};
    std::sort(edges.begin(), edges.end(), [](const Edge& x, const Edge& y) {
        if (x.room != y.room) return x.room < y.room;
        if (x.a != y.a) return x.a < y.a;
        if (x.b != y.b) return x.b < y.b;
        return x.face < y.face;
    });
    std::vector<std::pair<std::uint32_t, std::uint32_t>> pairs;
    for (std::size_t s = 0; s < edges.size();) {
        std::size_t e = s + 1;
        while (e < edges.size() && edges[e].room == edges[s].room && edges[e].a == edges[s].a
               && edges[e].b == edges[s].b) {
            e++;
        }
        // an edge shared by more than a few faces is not a surface seam
        if (e - s <= 4) {
            for (std::size_t i = s; i < e; i++) {
                for (std::size_t j = i + 1; j < e; j++) {
                    if (edges[i].face == edges[j].face) {
                        continue;
                    }
                    pairs.push_back({edges[i].face, edges[j].face});
                    pairs.push_back({edges[j].face, edges[i].face});
                    g_ov.shared_edges.push_back({edges[i].face, edges[j].face, edges[i].v0, edges[i].v1});
                }
            }
        }
        s = e;
    }
    edges.clear();
    edges.shrink_to_fit();
    std::sort(pairs.begin(), pairs.end());
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
    g_ov.nbr_offsets.assign(g_ov.faces.size() + 1, 0);
    for (const auto& p : pairs) {
        g_ov.nbr_offsets[p.first + 1]++;
    }
    for (std::size_t i = 0; i < g_ov.faces.size(); i++) {
        g_ov.nbr_offsets[i + 1] += g_ov.nbr_offsets[i];
    }
    g_ov.nbrs.resize(pairs.size());
    for (std::size_t i = 0; i < pairs.size(); i++) {
        g_ov.nbrs[i] = pairs[i].second;
    }
}

// ─── grouping and projection ──────────────────────────────────────────────────

void frame_of(V3 n, V3& u, V3& v)
{
    const V3 a = std::fabs(n.x) < 0.9f ? V3{1.0f, 0.0f, 0.0f} : V3{0.0f, 1.0f, 0.0f};
    u = normalized(cross(n, a));
    v = cross(n, u);
}

void project(const Face& c, V3 u, V3 v, std::vector<D2>& out)
{
    out.resize(c.num_verts);
    for (std::uint32_t k = 0; k < c.num_verts; k++) {
        const V3 p = g_ov.verts[c.first_vert + k];
        out[k] = {static_cast<double>(dot(p, u)), static_cast<double>(dot(p, v))};
    }
}

// The x interval where line y crosses convex polygon p; half open in y, so two polygons sharing an edge
// split the points on it.
bool polygon_span(const std::vector<D2>& p, double y, double& x0, double& x1)
{
    bool any = false;
    for (std::size_t k = 0; k < p.size(); k++) {
        const D2 a = p[k];
        const D2 b = p[(k + 1) % p.size()];
        if ((a.y <= y) == (b.y <= y)) {
            continue;
        }
        const double x = a.x + (y - a.y) * (b.x - a.x) / (b.y - a.y);
        if (!any) {
            x0 = x1 = x;
            any = true;
        }
        else {
            x0 = std::min(x0, x);
            x1 = std::max(x1, x);
        }
    }
    return any;
}

// Cell centres of the chart's faces on a 1 / d0 grid around the seed, to refuse a face that folds over them.
class CellGrid
{
public:
    void begin(double ox, double oy, double cell, std::uint32_t stamp)
    {
        ox_ = ox;
        oy_ = oy;
        cell_ = cell;
        stamp_ = stamp;
        if (stamps_.empty()) {
            stamps_.assign(static_cast<std::size_t>(dim) * dim, std::numeric_limits<std::uint32_t>::max());
        }
    }

    // With mark false, whether the polygon's cell centres meet the chart's; with mark true, adds them.
    bool visit(const std::vector<D2>& p, bool mark)
    {
        double ylo = p[0].y, yhi = p[0].y;
        for (const D2& q : p) {
            ylo = std::min(ylo, q.y);
            yhi = std::max(yhi, q.y);
        }
        const long iy0 = std::max(0L, static_cast<long>(std::ceil((ylo - oy_) / cell_ - 0.5)));
        const long iy1 = std::min(static_cast<long>(dim) - 1, static_cast<long>(std::floor((yhi - oy_) / cell_ - 0.5)));
        for (long iy = iy0; iy <= iy1; iy++) {
            double x0 = 0.0, x1 = 0.0;
            if (!polygon_span(p, oy_ + (iy + 0.5) * cell_, x0, x1)) {
                continue;
            }
            const long ix0 = std::max(0L, static_cast<long>(std::ceil((x0 - ox_) / cell_ - 0.5)));
            const long ix1 =
                std::min(static_cast<long>(dim) - 1, static_cast<long>(std::ceil((x1 - ox_) / cell_ - 0.5)) - 1);
            for (long ix = ix0; ix <= ix1; ix++) {
                std::uint32_t& s = stamps_[static_cast<std::size_t>(iy) * dim + ix];
                if (mark) {
                    s = stamp_;
                }
                else if (s == stamp_) {
                    return true;
                }
            }
        }
        return false;
    }

    // The grid spans the seed's box plus one cap either side, which bounds any chart that grows from it.
    static constexpr std::uint32_t dim = 3 * alm::tile_step + 4;

private:
    double ox_ = 0.0, oy_ = 0.0, cell_ = 1.0;
    std::uint32_t stamp_ = 0;
    std::vector<std::uint32_t> stamps_;
};

std::vector<D2> convex_hull(std::vector<D2> pts)
{
    std::sort(pts.begin(), pts.end(), [](const D2& a, const D2& b) { return a.x != b.x ? a.x < b.x : a.y < b.y; });
    pts.erase(std::unique(pts.begin(), pts.end(), [](const D2& a, const D2& b) { return a.x == b.x && a.y == b.y; }),
              pts.end());
    if (pts.size() < 3) {
        return pts;
    }
    std::vector<D2> h(2 * pts.size());
    std::size_t k = 0;
    const auto turn = [](const D2& o, const D2& a, const D2& b) {
        return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
    };
    for (std::size_t i = 0; i < pts.size(); i++) {
        while (k >= 2 && turn(h[k - 2], h[k - 1], pts[i]) <= 0.0) {
            k--;
        }
        h[k++] = pts[i];
    }
    for (std::size_t i = pts.size() - 1, t = k + 1; i > 0; i--) {
        while (k >= t && turn(h[k - 2], h[k - 1], pts[i - 1]) <= 0.0) {
            k--;
        }
        h[k++] = pts[i - 1];
    }
    h.resize(k - 1);
    return h;
}

// Plane of the seed's normal, axes along the smallest rectangle around the faces, origin at its min corner.
void finalize_projection(Chart& c, V3 u0, V3 v0)
{
    std::vector<D2> pts;
    std::vector<D2> poly;
    for (std::uint32_t m = 0; m < c.num_members; m++) {
        project(g_ov.faces[g_ov.members[c.first_member + m]], u0, v0, poly);
        pts.insert(pts.end(), poly.begin(), poly.end());
    }
    const std::vector<D2> hull = convex_hull(pts);
    double best_cs = 1.0, best_sn = 0.0, best_area = std::numeric_limits<double>::max();
    for (std::size_t i = 0; hull.size() >= 3 && i < hull.size(); i++) {
        const D2 a = hull[i];
        const D2 b = hull[(i + 1) % hull.size()];
        const double len = std::hypot(b.x - a.x, b.y - a.y);
        if (!(len > 0.0)) {
            continue;
        }
        const double cs = (b.x - a.x) / len, sn = (b.y - a.y) / len;
        double u_lo = 1e300, u_hi = -1e300, v_lo = 1e300, v_hi = -1e300;
        for (const D2& p : hull) {
            const double u = p.x * cs + p.y * sn, v = -p.x * sn + p.y * cs;
            u_lo = std::min(u_lo, u);
            u_hi = std::max(u_hi, u);
            v_lo = std::min(v_lo, v);
            v_hi = std::max(v_hi, v);
        }
        const double area = (u_hi - u_lo) * (v_hi - v_lo);
        if (area < best_area) {
            best_area = area;
            best_cs = cs;
            best_sn = sn;
        }
    }
    c.au = normalized(u0 * static_cast<float>(best_cs) + v0 * static_cast<float>(best_sn));
    c.av = normalized(cross(c.n, c.au));
    double u_lo = 1e300, u_hi = -1e300, v_lo = 1e300, v_hi = -1e300;
    for (std::uint32_t m = 0; m < c.num_members; m++) {
        const Face& f = g_ov.faces[g_ov.members[c.first_member + m]];
        for (std::uint32_t k = 0; k < f.num_verts; k++) {
            const V3 p = g_ov.verts[f.first_vert + k];
            const double u = dot(p, c.au), v = dot(p, c.av);
            u_lo = std::min(u_lo, u);
            u_hi = std::max(u_hi, u);
            v_lo = std::min(v_lo, v);
            v_hi = std::max(v_hi, v);
        }
    }
    const Face& seed = g_ov.faces[g_ov.members[c.first_member]];
    const float along = dot(g_ov.verts[seed.first_vert], c.n);
    c.origin = c.au * static_cast<float>(u_lo) + c.av * static_cast<float>(v_lo) + c.n * along;
    c.ext_u = static_cast<float>(u_hi - u_lo);
    c.ext_v = static_cast<float>(v_hi - v_lo);
}

void grow_charts(float d0)
{
    const std::size_t nf = g_ov.faces.size();
    const double cap = alm::tile_step / static_cast<double>(d0);
    const double fill_free = grow_fill_free_texels / static_cast<double>(d0);
    constexpr std::uint32_t none = std::numeric_limits<std::uint32_t>::max();
    std::vector<std::uint32_t> assigned(nf, none);
    std::vector<std::uint32_t> tried(nf, none);
    std::priority_queue<std::uint32_t, std::vector<std::uint32_t>, std::greater<std::uint32_t>> heap;
    CellGrid cells;
    std::vector<D2> poly;
    std::vector<D2> seed_poly;
    for (std::uint32_t seed = 0; seed < nf; seed++) {
        if (assigned[seed] != none) {
            continue;
        }
        const auto ci = static_cast<std::uint32_t>(g_ov.charts.size());
        Chart c;
        c.first_member = static_cast<std::uint32_t>(g_ov.members.size());
        c.n = g_ov.faces[seed].normal;
        V3 u0, v0;
        frame_of(c.n, u0, v0);
        project(g_ov.faces[seed], u0, v0, seed_poly);
        D2 lo = seed_poly[0], hi = seed_poly[0];
        for (const D2& p : seed_poly) {
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y)};
        }
        double parea = g_ov.faces[seed].area;
        g_ov.members.push_back(seed);
        assigned[seed] = ci;
        const auto push_nbrs = [&](std::uint32_t f) {
            for (std::uint32_t k = g_ov.nbr_offsets[f]; k < g_ov.nbr_offsets[f + 1]; k++) {
                if (assigned[g_ov.nbrs[k]] == none) {
                    heap.push(g_ov.nbrs[k]);
                }
            }
        };
        // a seed already past the cap takes no neighbour
        const bool can_grow = hi.x - lo.x <= cap && hi.y - lo.y <= cap;
        bool cells_ready = false;
        if (can_grow) {
            push_nbrs(seed);
        }
        while (!heap.empty()) {
            const std::uint32_t o = heap.top();
            heap.pop();
            if (assigned[o] != none || tried[o] == ci) {
                continue;
            }
            tried[o] = ci;
            const Face& fo = g_ov.faces[o];
            const float agree = dot(fo.normal, c.n);
            if (agree < grow_cos) {
                continue;
            }
            project(fo, u0, v0, poly);
            D2 nlo = lo, nhi = hi;
            for (const D2& p : poly) {
                nlo = {std::min(nlo.x, p.x), std::min(nlo.y, p.y)};
                nhi = {std::max(nhi.x, p.x), std::max(nhi.y, p.y)};
            }
            const double bw = nhi.x - nlo.x, bh = nhi.y - nlo.y;
            if (bw > cap || bh > cap) {
                continue;
            }
            const double npa = parea + static_cast<double>(fo.area) * agree;
            if ((bw > fill_free || bh > fill_free) && npa < grow_fill_min * bw * bh) {
                continue;
            }
            if (!cells_ready) {
                cells.begin(lo.x - cap, lo.y - cap, 1.0 / d0, ci);
                cells.visit(seed_poly, true);
                cells_ready = true;
            }
            if (cells.visit(poly, false)) {
                continue;
            }
            cells.visit(poly, true);
            lo = nlo;
            hi = nhi;
            parea = npa;
            g_ov.members.push_back(o);
            assigned[o] = ci;
            push_nbrs(o);
        }
        c.num_members = static_cast<std::uint32_t>(g_ov.members.size()) - c.first_member;
        std::sort(g_ov.members.begin() + c.first_member, g_ov.members.end());
        for (std::uint32_t m = 0; m < c.num_members; m++) {
            c.area += g_ov.faces[g_ov.members[c.first_member + m]].area;
        }
        finalize_projection(c, u0, v0);
        g_ov.charts.push_back(c);
    }
}

// ─── densities, masks and packing ─────────────────────────────────────────────

alm::OverflowChart wire_of(const Chart& c)
{
    alm::OverflowChart w{};
    w.num_faces = c.num_members;
    w.w = c.w;
    w.h = c.h;
    w.origin[0] = c.origin.x;
    w.origin[1] = c.origin.y;
    w.origin[2] = c.origin.z;
    w.axis_u[0] = c.au.x;
    w.axis_u[1] = c.au.y;
    w.axis_u[2] = c.au.z;
    w.axis_v[0] = c.av.x;
    w.axis_v[1] = c.av.y;
    w.axis_v[2] = c.av.z;
    w.texel_size = 1.0f / c.density;
    w.mean_rgb[0] = c.mean[0];
    w.mean_rgb[1] = c.mean[1];
    w.mean_rgb[2] = c.mean[2];
    return w;
}

std::uint16_t chart_dim(float extent, float density)
{
    const double t = std::ceil(static_cast<double>(extent) * density - 1e-4);
    return static_cast<std::uint16_t>(std::clamp(t, 1.0, 65535.0));
}

// False when no density down to the floor keeps it within the reader's chart size.
bool set_density(Chart& c, float density)
{
    while (density >= floor_density) {
        const std::uint16_t w = chart_dim(c.ext_u, density);
        const std::uint16_t h = chart_dim(c.ext_v, density);
        if (w <= alm::max_overflow_chart_dim && h <= alm::max_overflow_chart_dim) {
            c.density = density;
            c.w = w;
            c.h = h;
            c.geom = alm::overflow_chart_geometry(w, h);
            return !c.geom.empty();
        }
        density *= 0.5f;
    }
    return false;
}

// Stored blocks the chart's tile rects cover at `density`.
std::uint64_t rect_blocks(const Chart& c, float density)
{
    const alm::ChartGeometry g =
        alm::overflow_chart_geometry(chart_dim(c.ext_u, density), chart_dim(c.ext_v, density));
    std::uint64_t n = 0;
    for (std::uint32_t ty = 0; ty < g.ny; ty++) {
        for (std::uint32_t tx = 0; tx < g.nx; tx++) {
            const alm::TileDims td = alm::tile_dims(g, tx, ty);
            n += static_cast<std::uint64_t>(td.w_t / 4) * (td.h_t / 4);
        }
    }
    return n;
}

void chart_poly(const Chart& c, const Face& f, std::vector<D2>& out)
{
    const alm::OverflowChart w = wire_of(c);
    out.resize(f.num_verts);
    for (std::uint32_t k = 0; k < f.num_verts; k++) {
        const V3 p = g_ov.verts[f.first_vert + k];
        const float pos[3] = {p.x, p.y, p.z};
        const alm::ChartTexel t = alm::overflow_chart_coord(w, pos);
        out[k] = {t.u, t.v};
    }
}

// The x range of convex polygon p within the band ylo <= y <= yhi; false when they do not meet.
bool band_span(const std::vector<D2>& p, double ylo, double yhi, double& x0, double& x1)
{
    bool any = false;
    const auto take = [&](double x) {
        if (!any) {
            x0 = x1 = x;
            any = true;
        }
        else {
            x0 = std::min(x0, x);
            x1 = std::max(x1, x);
        }
    };
    for (std::size_t k = 0; k < p.size(); k++) {
        const D2 a = p[k];
        const D2 b = p[(k + 1) % p.size()];
        if (a.y >= ylo && a.y <= yhi) {
            take(a.x);
        }
        for (const double y : {ylo, yhi}) {
            if ((a.y < y && b.y > y) || (a.y > y && b.y < y)) {
                take(a.x + (y - a.y) * (b.x - a.x) / (b.y - a.y));
            }
        }
    }
    return any;
}

bool mask_bit(const TileMask& m, std::uint32_t bx, std::uint32_t by)
{
    return bx < m.bw && by < m.bh && ((g_ov.mask_rows[m.first_row + by] >> bx) & 1u) != 0;
}

// Every tile's blocks at its chart's density. A texel is needed when a bilinear tap at a point of a face
// reads it: its square widened by half a texel on each side meets the face.
std::uint64_t build_masks()
{
    g_ov.masks.clear();
    g_ov.mask_rows.clear();
    std::uint64_t total = 0;
    std::uint32_t tile_base = 0;
    std::vector<D2> poly;
    for (Chart& c : g_ov.charts) {
        c.first_mask = static_cast<std::uint32_t>(g_ov.masks.size());
        c.tile_base = tile_base;
        tile_base += c.geom.tile_count();
        for (std::uint32_t ty = 0; ty < c.geom.ny; ty++) {
            for (std::uint32_t tx = 0; tx < c.geom.nx; tx++) {
                const alm::TileDims td = alm::tile_dims(c.geom, tx, ty);
                TileMask m;
                m.first_row = static_cast<std::uint32_t>(g_ov.mask_rows.size());
                m.bw = static_cast<std::uint8_t>(td.w_t / 4);
                m.bh = static_cast<std::uint8_t>(td.h_t / 4);
                g_ov.mask_rows.resize(g_ov.mask_rows.size() + m.bh, 0);
                g_ov.masks.push_back(m);
            }
        }
        const std::int64_t lo_u = -static_cast<std::int64_t>(c.geom.pad_u);
        const std::int64_t hi_u = static_cast<std::int64_t>(c.w) + c.geom.pad_u - 1;
        const std::int64_t lo_v = -static_cast<std::int64_t>(c.geom.pad_v);
        const std::int64_t hi_v = static_cast<std::int64_t>(c.h) + c.geom.pad_v - 1;
        for (std::uint32_t m = 0; m < c.num_members; m++) {
            chart_poly(c, g_ov.faces[g_ov.members[c.first_member + m]], poly);
            double ylo = poly[0].y, yhi = poly[0].y;
            for (const D2& p : poly) {
                ylo = std::min(ylo, p.y);
                yhi = std::max(yhi, p.y);
            }
            const std::int64_t j0 = std::max(lo_v, static_cast<std::int64_t>(std::ceil(ylo - 1.5)));
            const std::int64_t j1 = std::min(hi_v, static_cast<std::int64_t>(std::floor(yhi + 0.5)));
            for (std::int64_t j = j0; j <= j1; j++) {
                double x0 = 0.0, x1 = 0.0;
                if (!band_span(poly, static_cast<double>(j) - 0.5, static_cast<double>(j) + 1.5, x0, x1)) {
                    continue;
                }
                const std::int64_t i0 = std::max(lo_u, static_cast<std::int64_t>(std::ceil(x0 - 1.5)));
                const std::int64_t i1 = std::min(hi_u, static_cast<std::int64_t>(std::floor(x1 + 0.5)));
                if (i0 > i1) {
                    continue;
                }
                for (std::uint32_t ty = 0; ty < c.geom.ny; ty++) {
                    for (std::uint32_t tx = 0; tx < c.geom.nx; tx++) {
                        const alm::TileDims td = alm::tile_dims(c.geom, tx, ty);
                        const alm::ChartCoord o = alm::tile_origin_chart(c.geom, tx, ty);
                        const std::int64_t sy = j - o.v;
                        if (sy < 0 || sy >= td.h_t) {
                            continue;
                        }
                        const std::int64_t s0 = std::max<std::int64_t>(i0 - o.u, 0);
                        const std::int64_t s1 = std::min<std::int64_t>(i1 - o.u, static_cast<std::int64_t>(td.w_t) - 1);
                        if (s0 > s1) {
                            continue;
                        }
                        const TileMask& tm = g_ov.masks[c.first_mask + ty * c.geom.nx + tx];
                        std::uint64_t& row = g_ov.mask_rows[tm.first_row + static_cast<std::size_t>(sy / 4)];
                        for (std::int64_t b = s0 / 4; b <= s1 / 4; b++) {
                            row |= std::uint64_t{1} << b;
                        }
                    }
                }
            }
        }
        c.blocks = 0;
        for (std::uint32_t t = 0; t < c.geom.tile_count(); t++) {
            TileMask& tm = g_ov.masks[c.first_mask + t];
            std::uint32_t n = 0;
            for (std::uint32_t r = 0; r < tm.bh; r++) {
                n += static_cast<std::uint32_t>(std::popcount(g_ov.mask_rows[tm.first_row + r]));
            }
            tm.blocks = static_cast<std::uint16_t>(n);
            c.blocks += n;
        }
        total += c.blocks;
    }
    return total;
}

// First fit of every tile's blocks, largest first, into pages of 64 x 64 block rows; false when they need
// more than `budget` pages.
bool pack(std::uint32_t budget)
{
    struct Page
    {
        std::array<std::uint64_t, page_blocks> rows{};
        std::uint32_t free = page_blocks * page_blocks;
        std::uint32_t failed = std::numeric_limits<std::uint32_t>::max(); // smallest count that failed here
        std::uint32_t first_open_row = 0;
    };
    struct Entry
    {
        std::uint32_t blocks;
        std::uint32_t chart;
        std::uint32_t tile;
    };
    std::vector<Entry> order;
    for (std::uint32_t ci = 0; ci < g_ov.charts.size(); ci++) {
        const Chart& c = g_ov.charts[ci];
        for (std::uint32_t t = 0; t < c.geom.tile_count(); t++) {
            order.push_back({g_ov.masks[c.first_mask + t].blocks, ci, t});
        }
    }
    std::stable_sort(order.begin(), order.end(), [](const Entry& a, const Entry& b) { return a.blocks > b.blocks; });

    std::vector<Page> pages;
    std::vector<alm::Tile> tiles(order.size(), alm::Tile{static_cast<std::uint16_t>(g_ov.first_page), 0, 0});
    const auto try_place = [](Page& pg, const TileMask& m, std::uint16_t& ox, std::uint16_t& oy) {
        const std::uint64_t* rows = g_ov.mask_rows.data() + m.first_row;
        for (std::uint32_t by = pg.first_open_row; by + m.bh <= page_blocks; by++) {
            if (pg.rows[by] == ~std::uint64_t{0} && rows[0] != 0) {
                continue;
            }
            for (std::uint32_t bx = 0; bx + m.bw <= page_blocks; bx++) {
                bool fits = true;
                for (std::uint32_t r = 0; r < m.bh && fits; r++) {
                    fits = (pg.rows[by + r] & (rows[r] << bx)) == 0;
                }
                if (!fits) {
                    continue;
                }
                for (std::uint32_t r = 0; r < m.bh; r++) {
                    pg.rows[by + r] |= rows[r] << bx;
                }
                pg.free -= m.blocks;
                while (pg.first_open_row < page_blocks && pg.rows[pg.first_open_row] == ~std::uint64_t{0}) {
                    pg.first_open_row++;
                }
                ox = static_cast<std::uint16_t>(bx * 4);
                oy = static_cast<std::uint16_t>(by * 4);
                return true;
            }
        }
        return false;
    };
    for (const Entry& e : order) {
        const Chart& c = g_ov.charts[e.chart];
        const TileMask& m = g_ov.masks[c.first_mask + e.tile];
        alm::Tile& out = tiles[c.tile_base + e.tile];
        if (m.blocks == 0) {
            continue; // never sampled; its rect only has to lie on a page
        }
        bool placed = false;
        for (std::size_t p = 0; p < pages.size() && !placed; p++) {
            Page& pg = pages[p];
            if (pg.free < m.blocks || m.blocks >= pg.failed) {
                continue;
            }
            if (try_place(pg, m, out.x, out.y)) {
                out.page = static_cast<std::uint16_t>(g_ov.first_page + p);
                placed = true;
            }
            else {
                pg.failed = m.blocks;
            }
        }
        if (!placed) {
            if (pages.size() >= budget) {
                return false;
            }
            pages.emplace_back();
            if (!try_place(pages.back(), m, out.x, out.y)) {
                return false;
            }
            out.page = static_cast<std::uint16_t>(g_ov.first_page + pages.size() - 1);
        }
    }
    g_ov.layout.tiles = std::move(tiles);
    g_ov.layout.num_pages = static_cast<std::uint32_t>(std::max<std::size_t>(pages.size(), 1));
    return true;
}

// Lowers densities, smallest charts first, until the blocks fit `budget` pages, then packs. Charts that still do
// not fit at the floor density are dropped, smallest first; their faces stay unlit.
bool fit_and_pack(std::uint32_t budget, bool& lowered, std::uint32_t& dropped_charts, std::uint32_t& dropped_faces)
{
    lowered = false;
    dropped_charts = 0;
    dropped_faces = 0;
    constexpr double page_capacity = static_cast<double>(page_blocks) * page_blocks;
    double efficiency = 0.8;
    std::uint32_t round = 0;
    std::uint64_t total = build_masks();
    for (int iter = 0; iter < 400; iter++) {
        const double target = budget * page_capacity * efficiency;
        if (static_cast<double>(total) <= target) {
            if (pack(budget)) {
                return true;
            }
            efficiency *= 0.92;
            continue;
        }
        const std::uint32_t limit = round >= 14 ? std::numeric_limits<std::uint32_t>::max() : (4u << round);
        std::vector<std::uint32_t> order;
        for (std::uint32_t ci = 0; ci < g_ov.charts.size(); ci++) {
            const Chart& c = g_ov.charts[ci];
            if (std::max(c.w, c.h) <= limit && c.density * 0.5f >= floor_density
                && rect_blocks(c, c.density * 0.5f) < rect_blocks(c, c.density)) {
                order.push_back(ci);
            }
        }
        std::stable_sort(order.begin(), order.end(), [](std::uint32_t a, std::uint32_t b) {
            return std::max(g_ov.charts[a].w, g_ov.charts[a].h) < std::max(g_ov.charts[b].w, g_ov.charts[b].h);
        });
        double projected = static_cast<double>(total);
        std::size_t halved = 0;
        for (; halved < order.size() && projected > target; halved++) {
            Chart& c = g_ov.charts[order[halved]];
            const double before = static_cast<double>(rect_blocks(c, c.density));
            const double after = static_cast<double>(rect_blocks(c, c.density * 0.5f));
            projected -= c.blocks * (1.0 - after / before);
            set_density(c, c.density * 0.5f);
            lowered = true;
        }
        if (halved == 0) {
            if (limit != std::numeric_limits<std::uint32_t>::max()) {
                round++;
                continue;
            }
            // nothing can shrink: drop the smallest charts until the rest fit
            std::vector<std::uint32_t> by_area(g_ov.charts.size());
            for (std::uint32_t i = 0; i < by_area.size(); i++) {
                by_area[i] = i;
            }
            std::stable_sort(by_area.begin(), by_area.end(), [](std::uint32_t a, std::uint32_t b) {
                return g_ov.charts[a].area < g_ov.charts[b].area;
            });
            std::vector<std::uint8_t> drop(g_ov.charts.size(), 0);
            double left = static_cast<double>(total);
            for (std::uint32_t i : by_area) {
                if (left <= target * 0.95) break;
                drop[i] = 1;
                left -= g_ov.charts[i].blocks;
                dropped_charts++;
                dropped_faces += g_ov.charts[i].num_members;
            }
            std::vector<Chart> kept;
            for (std::uint32_t i = 0; i < g_ov.charts.size(); i++) {
                if (!drop[i]) {
                    kept.push_back(g_ov.charts[i]);
                }
            }
            g_ov.charts = std::move(kept);
            if (g_ov.charts.empty()) {
                return false;
            }
        }
        else if (halved == order.size()) {
            round++;
        }
        total = build_masks();
    }
    return false;
}

// ─── shading ──────────────────────────────────────────────────────────────────

float ray_lift(float texel_size)
{
    return 0.02f + 0.05f * texel_size;
}

// The closest point of convex polygon p to q, its squared distance (0 inside).
double closest_on_polygon(const std::vector<D2>& p, D2 q, D2& out)
{
    double area = 0.0;
    for (std::size_t k = 0; k < p.size(); k++) {
        const D2 a = p[k], b = p[(k + 1) % p.size()];
        area += a.x * b.y - b.x * a.y;
    }
    const double orient = area >= 0.0 ? 1.0 : -1.0;
    bool inside = true;
    double best = std::numeric_limits<double>::max();
    for (std::size_t k = 0; k < p.size(); k++) {
        const D2 a = p[k], b = p[(k + 1) % p.size()];
        const double cr = ((b.x - a.x) * (q.y - a.y) - (b.y - a.y) * (q.x - a.x)) * orient;
        if (cr < 0.0) {
            inside = false;
        }
        const double ex = b.x - a.x, ey = b.y - a.y;
        const double len2 = ex * ex + ey * ey;
        double t = len2 > 0.0 ? ((q.x - a.x) * ex + (q.y - a.y) * ey) / len2 : 0.0;
        t = std::clamp(t, 0.0, 1.0);
        const D2 c{a.x + ex * t, a.y + ey * t};
        const double d2 = (q.x - c.x) * (q.x - c.x) + (q.y - c.y) * (q.y - c.y);
        if (d2 < best) {
            best = d2;
            out = c;
        }
    }
    if (inside) {
        out = q;
        return 0.0;
    }
    return best;
}

// Barycentric blend of a face's corner normals at chart point q, over the fan triangle holding it.
V3 smoothed_normal(const Face& f, const std::vector<D2>& p, D2 q)
{
    if (!f.smoothing) {
        return f.normal;
    }
    std::uint32_t best_k = 1;
    double best_w[3] = {1.0, 0.0, 0.0};
    double best_err = std::numeric_limits<double>::max();
    for (std::uint32_t k = 1; k + 1 < f.num_verts; k++) {
        const D2 a = p[0], b = p[k], c = p[k + 1];
        const double den = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
        if (std::fabs(den) < 1e-18) {
            continue;
        }
        const double w0 = ((b.y - c.y) * (q.x - c.x) + (c.x - b.x) * (q.y - c.y)) / den;
        const double w1 = ((c.y - a.y) * (q.x - c.x) + (a.x - c.x) * (q.y - c.y)) / den;
        const double w2 = 1.0 - w0 - w1;
        const double err = std::max({0.0, -w0, -w1, -w2});
        if (err < best_err) {
            best_err = err;
            best_k = k;
            best_w[0] = std::max(w0, 0.0);
            best_w[1] = std::max(w1, 0.0);
            best_w[2] = std::max(w2, 0.0);
        }
        if (err == 0.0) {
            break;
        }
    }
    const double sum = best_w[0] + best_w[1] + best_w[2];
    if (!(sum > 0.0)) {
        return f.normal;
    }
    const V3 n0 = g_ov.corner_normals[f.first_vert];
    const V3 n1 = g_ov.corner_normals[f.first_vert + best_k];
    const V3 n2 = g_ov.corner_normals[f.first_vert + best_k + 1];
    const V3 n = normalized(n0 * static_cast<float>(best_w[0] / sum) + n1 * static_cast<float>(best_w[1] / sum) +
                            n2 * static_cast<float>(best_w[2] / sum));
    return dot(n, f.normal) > 0.0f ? n : f.normal;
}

using Pages = std::vector<std::vector<std::uint8_t>>;

std::uint8_t* page_texel(Pages& pages, std::uint32_t page, std::uint32_t x, std::uint32_t y)
{
    if (page >= pages.size() || x >= alm::page_size || y >= alm::page_size || pages[page].empty()) {
        return nullptr;
    }
    return pages[page].data() + (static_cast<std::size_t>(y) * alm::page_size + x) * 3;
}

// One tile's shading: its stored rect plus a ring for the 3x3 filter, lit where it stores blocks or lies near a face.
struct Job
{
    std::uint32_t chart = 0;
    std::uint32_t tile = 0;
    std::int64_t gx0 = 0, gy0 = 0; // chart coordinate of grid texel (0, 0)
    std::uint32_t gw = 0, gh = 0;
    std::vector<std::int32_t> point_of; // per grid texel, its index among the job's points, -1 for unlit
    std::vector<std::uint8_t> covered;  // per grid texel: its centre lies on a face
    std::vector<LightmapPoint> points;
    std::vector<float> r, g, b; // per point
};

void prepare_job(Job& job, const std::vector<std::vector<D2>>& polys)
{
    const Chart& c = g_ov.charts[job.chart];
    const std::uint32_t tx = job.tile % c.geom.nx, ty = job.tile / c.geom.nx;
    const alm::TileDims td = alm::tile_dims(c.geom, tx, ty);
    const alm::ChartCoord o = alm::tile_origin_chart(c.geom, tx, ty);
    const TileMask& m = g_ov.masks[c.first_mask + job.tile];
    job.gx0 = o.u - 1;
    job.gy0 = o.v - 1;
    job.gw = td.w_t + 2;
    job.gh = td.h_t + 2;
    const std::size_t n = static_cast<std::size_t>(job.gw) * job.gh;
    job.point_of.assign(n, -1);
    job.covered.assign(n, 0);
    job.points.clear();
    if (m.blocks == 0) {
        return;
    }
    const alm::OverflowChart w = wire_of(c);
    const float ts = w.texel_size;
    const V3 origin = c.origin;
    for (std::uint32_t gy = 0; gy < job.gh; gy++) {
        for (std::uint32_t gx = 0; gx < job.gw; gx++) {
            const std::int64_t cu = job.gx0 + gx, cv = job.gy0 + gy;
            const std::int64_t sx = cu - o.u, sy = cv - o.v;
            const bool stored = sx >= 0 && sy >= 0 && sx < td.w_t && sy < td.h_t
                             && mask_bit(m, static_cast<std::uint32_t>(sx / 4), static_cast<std::uint32_t>(sy / 4));
            const D2 q{static_cast<double>(cu) + 0.5, static_cast<double>(cv) + 0.5};
            double best = std::numeric_limits<double>::max();
            D2 best_pt{};
            std::uint32_t best_face = 0;
            for (std::uint32_t k = 0; k < polys.size(); k++) {
                D2 pt;
                const double d2 = closest_on_polygon(polys[k], q, pt);
                if (d2 < best) {
                    best = d2;
                    best_pt = pt;
                    best_face = k;
                }
                if (d2 == 0.0) {
                    break;
                }
            }
            if (!stored && best > shade_reach * shade_reach) {
                continue;
            }
            const Face& f = g_ov.faces[g_ov.members[c.first_member + best_face]];
            const V3 on_plane =
                origin + c.au * static_cast<float>(best_pt.x * ts) + c.av * static_cast<float>(best_pt.y * ts);
            const float denom = dot(f.normal, c.n);
            const float s = denom > 1e-4f ? dot(f.normal, g_ov.verts[f.first_vert] - on_plane) / denom : 0.0f;
            const V3 pos = on_plane + c.n * s;
            const V3 nrm = smoothed_normal(f, polys[best_face], best_pt);
            LightmapPoint lp{};
            lp.pos[0] = pos.x;
            lp.pos[1] = pos.y;
            lp.pos[2] = pos.z;
            lp.normal[0] = nrm.x;
            lp.normal[1] = nrm.y;
            lp.normal[2] = nrm.z;
            const std::size_t gi = static_cast<std::size_t>(gy) * job.gw + gx;
            job.point_of[gi] = static_cast<std::int32_t>(job.points.size());
            job.covered[gi] = best == 0.0 ? 1 : 0;
            job.points.push_back(lp);
        }
    }
    job.r.assign(job.points.size(), 0.0f);
    job.g.assign(job.points.size(), 0.0f);
    job.b.assign(job.points.size(), 0.0f);
}

// Unlit grid texels take their nearest lit texel's light, then the grid is filtered and its stored blocks written.
void finish_job(Job& job, Pages& pages)
{
    Chart& c = g_ov.charts[job.chart];
    if (job.points.empty()) {
        return;
    }
    const std::size_t n = static_cast<std::size_t>(job.gw) * job.gh;
    std::vector<float> r(n), g(n), b(n);
    std::vector<std::int32_t> src(n, -1);
    std::vector<std::uint32_t> queue;
    queue.reserve(n);
    for (std::size_t i = 0; i < n; i++) {
        if (job.point_of[i] >= 0) {
            src[i] = job.point_of[i];
            queue.push_back(static_cast<std::uint32_t>(i));
        }
    }
    for (std::size_t head = 0; head < queue.size(); head++) {
        const std::uint32_t i = queue[head];
        const std::uint32_t x = i % job.gw, y = i / job.gw;
        const std::uint32_t nb[4] = {x > 0 ? i - 1 : i, x + 1 < job.gw ? i + 1 : i, y > 0 ? i - job.gw : i,
                                     y + 1 < job.gh ? i + job.gw : i};
        for (std::uint32_t k : nb) {
            if (src[k] < 0) {
                src[k] = src[i];
                queue.push_back(k);
            }
        }
    }
    for (std::size_t i = 0; i < n; i++) {
        const std::int32_t s = src[i] < 0 ? 0 : src[i];
        r[i] = job.r[s];
        g[i] = job.g[s];
        b[i] = job.b[s];
    }
    std::vector<std::uint8_t> bytes(n * 3);
    lightmap_encode_float_texels(r.data(), g.data(), b.data(), static_cast<int>(job.gw), static_cast<int>(job.gh),
                                 bytes.data());

    const std::uint32_t tx = job.tile % c.geom.nx, ty = job.tile / c.geom.nx;
    const alm::TileDims td = alm::tile_dims(c.geom, tx, ty);
    const alm::ChartCoord o = alm::tile_origin_chart(c.geom, tx, ty);
    const TileMask& m = g_ov.masks[c.first_mask + job.tile];
    const alm::Tile& t = g_ov.layout.tiles[c.tile_base + job.tile];
    for (std::uint32_t sy = 0; sy < td.h_t; sy++) {
        for (std::uint32_t sx = 0; sx < td.w_t; sx++) {
            if (!mask_bit(m, sx / 4, sy / 4)) {
                continue;
            }
            std::uint8_t* dst = page_texel(pages, t.page, t.x + sx, t.y + sy);
            if (!dst) {
                continue;
            }
            const std::size_t gi = static_cast<std::size_t>(sy + 1) * job.gw + (sx + 1);
            std::memcpy(dst, &bytes[gi * 3], 3);
            // the chart's mean over the texels its faces cover, each counted by the tile that owns it
            const std::int64_t cu = o.u + sx, cv = o.v + sy;
            if (cu >= 0 && cv >= 0 && cu < c.w && cv < c.h
                && alm::chart_tile_slot(c.geom, 0, cu, cv).index == job.tile) {
                for (int ch = 0; ch < 3; ch++) {
                    c.stored_sum[ch] += bytes[gi * 3 + ch];
                    if (job.covered[gi]) {
                        c.mean_sum[ch] += bytes[gi * 3 + ch];
                    }
                }
                c.stored_count++;
                c.mean_count += job.covered[gi];
            }
        }
    }
}

// Morton order of the charts' centres over the solid's box, so consecutive light batches stay local.
std::vector<std::uint32_t> chart_order()
{
    V3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    std::vector<V3> centre(g_ov.charts.size());
    for (std::size_t i = 0; i < g_ov.charts.size(); i++) {
        const Chart& c = g_ov.charts[i];
        centre[i] = c.origin + c.au * (c.ext_u * 0.5f) + c.av * (c.ext_v * 0.5f);
        lo = {std::min(lo.x, centre[i].x), std::min(lo.y, centre[i].y), std::min(lo.z, centre[i].z)};
        hi = {std::max(hi.x, centre[i].x), std::max(hi.y, centre[i].y), std::max(hi.z, centre[i].z)};
    }
    const auto spread = [](std::uint32_t v) {
        std::uint64_t x = v & 0x3ff;
        x = (x | (x << 16)) & 0x030000ff;
        x = (x | (x << 8)) & 0x0300f00f;
        x = (x | (x << 4)) & 0x030c30c3;
        x = (x | (x << 2)) & 0x09249249;
        return x;
    };
    const auto quant = [](float v, float a, float b) {
        return b > a ? static_cast<std::uint32_t>(std::clamp((v - a) / (b - a), 0.0f, 1.0f) * 1023.0f) : 0u;
    };
    std::vector<std::pair<std::uint64_t, std::uint32_t>> keys(g_ov.charts.size());
    for (std::uint32_t i = 0; i < keys.size(); i++) {
        keys[i] = {spread(quant(centre[i].x, lo.x, hi.x)) | (spread(quant(centre[i].y, lo.y, hi.y)) << 1) |
                       (spread(quant(centre[i].z, lo.z, hi.z)) << 2),
                   i};
    }
    std::sort(keys.begin(), keys.end());
    std::vector<std::uint32_t> out(keys.size());
    for (std::size_t i = 0; i < keys.size(); i++) {
        out[i] = keys[i].second;
    }
    return out;
}

bool light_points(Job& job, std::size_t first, std::size_t count, float lift)
{
    return lightmap_light_terrain_points(job.points.data() + first, static_cast<int>(count), lift,
                                         job.r.data() + first, job.g.data() + first, job.b.data() + first);
}

bool shade_all(Pages& pages)
{
    const std::vector<std::uint32_t> order = chart_order();
    std::vector<Job> pending;
    std::size_t pending_points = 0;
    std::vector<LightmapPoint> batch;
    std::vector<float> br, bg, bb;
    // small tiles are lit together, a batch per call, each at the lift of its own chart's texels
    const auto flush = [&]() -> bool {
        if (pending.empty()) {
            return true;
        }
        std::size_t i = 0;
        while (i < pending.size()) {
            const float lift = ray_lift(1.0f / g_ov.charts[pending[i].chart].density);
            batch.clear();
            std::size_t j = i;
            while (j < pending.size() && ray_lift(1.0f / g_ov.charts[pending[j].chart].density) == lift) {
                batch.insert(batch.end(), pending[j].points.begin(), pending[j].points.end());
                j++;
            }
            br.assign(batch.size(), 0.0f);
            bg.assign(batch.size(), 0.0f);
            bb.assign(batch.size(), 0.0f);
            if (!batch.empty() && !lightmap_light_terrain_points(batch.data(), static_cast<int>(batch.size()), lift,
                                                                 br.data(), bg.data(), bb.data())) {
                return false;
            }
            std::size_t at = 0;
            for (std::size_t k = i; k < j; k++) {
                Job& jb = pending[k];
                std::copy_n(br.begin() + at, jb.points.size(), jb.r.begin());
                std::copy_n(bg.begin() + at, jb.points.size(), jb.g.begin());
                std::copy_n(bb.begin() + at, jb.points.size(), jb.b.begin());
                at += jb.points.size();
                finish_job(jb, pages);
            }
            i = j;
        }
        pending.clear();
        pending_points = 0;
        return true;
    };
    // a run of tiles is prepared on the pool, then lit in chart order as if prepared one by one
    std::vector<Job> ready;
    std::size_t chart_at = 0;
    std::uint32_t tile_at = 0;
    while (chart_at < order.size()) {
        ready.clear();
        std::uint64_t texels = 0;
        while (chart_at < order.size() && (ready.empty() || texels < prepare_run_texels)) {
            const Chart& c = g_ov.charts[order[chart_at]];
            if (tile_at == c.geom.tile_count()) {
                chart_at++;
                tile_at = 0;
                continue;
            }
            const alm::TileDims td = alm::tile_dims(c.geom, tile_at % c.geom.nx, tile_at / c.geom.nx);
            texels += static_cast<std::uint64_t>(td.w_t + 2) * (td.h_t + 2);
            Job& job = ready.emplace_back();
            job.chart = order[chart_at];
            job.tile = tile_at++;
        }
        work_pool_run(static_cast<int>(ready.size()), [&](int i) {
            Job& job = ready[i];
            const Chart& c = g_ov.charts[job.chart];
            std::vector<std::vector<D2>> polys(c.num_members);
            for (std::uint32_t m = 0; m < c.num_members; m++) {
                chart_poly(c, g_ov.faces[g_ov.members[c.first_member + m]], polys[m]);
            }
            prepare_job(job, polys);
        });
        for (Job& job : ready) {
            bake_progress_step();
            if (bake_progress_cancelled()) {
                return false;
            }
            if (job.points.empty()) {
                continue;
            }
            const Chart& c = g_ov.charts[job.chart];
            if (job.points.size() > light_batch) {
                if (!flush()) {
                    return false;
                }
                const float lift = ray_lift(1.0f / c.density);
                for (std::size_t first = 0; first < job.points.size(); first += light_batch) {
                    if (!light_points(job, first, std::min(light_batch, job.points.size() - first), lift)) {
                        return false;
                    }
                }
                finish_job(job, pages);
                continue;
            }
            if (pending_points + job.points.size() > light_batch && !flush()) {
                return false;
            }
            pending_points += job.points.size();
            pending.push_back(std::move(job));
        }
    }
    if (!flush()) {
        return false;
    }
    for (Chart& c : g_ov.charts) {
        for (int ch = 0; ch < 3; ch++) {
            const double v = c.mean_count ? c.mean_sum[ch] / c.mean_count
                                          : (c.stored_count ? c.stored_sum[ch] / c.stored_count : 0.0);
            c.mean[ch] = static_cast<std::uint8_t>(std::clamp(std::lround(v), 0L, 255L));
        }
    }
    return true;
}

// ─── seams and gutters ────────────────────────────────────────────────────────

std::uint8_t* chart_texel(Pages& pages, const Chart& c, std::int64_t cu, std::int64_t cv)
{
    const alm::TileSlot s = alm::chart_tile_slot(c.geom, 0, cu, cv);
    if (!s.valid || s.index >= c.geom.tile_count()) {
        return nullptr;
    }
    const TileMask& m = g_ov.masks[c.first_mask + s.index];
    if (!mask_bit(m, static_cast<std::uint32_t>(s.sx / 4), static_cast<std::uint32_t>(s.sy / 4))) {
        return nullptr;
    }
    const alm::Tile& t = g_ov.layout.tiles[c.tile_base + s.index];
    return page_texel(pages, t.page, t.x + static_cast<std::uint32_t>(s.sx), t.y + static_cast<std::uint32_t>(s.sy));
}

void chart_sample(Pages& pages, const Chart& c, double u, double v, float* out)
{
    const double fx = u - 0.5, fy = v - 0.5;
    const auto x0 = static_cast<std::int64_t>(std::floor(fx));
    const auto y0 = static_cast<std::int64_t>(std::floor(fy));
    const float tx = static_cast<float>(fx - static_cast<double>(x0));
    const float ty = static_cast<float>(fy - static_cast<double>(y0));
    out[0] = out[1] = out[2] = 0.0f;
    float weight = 0.0f;
    for (int dy = 0; dy < 2; dy++) {
        const std::int64_t yy = std::clamp<std::int64_t>(y0 + dy, -1, c.h);
        const float wy = dy ? ty : 1.0f - ty;
        for (int dx = 0; dx < 2; dx++) {
            const std::int64_t xx = std::clamp<std::int64_t>(x0 + dx, -1, c.w);
            const float wgt = wy * (dx ? tx : 1.0f - tx);
            const std::uint8_t* px = chart_texel(pages, c, xx, yy);
            if (!px) {
                continue;
            }
            out[0] += px[0] * wgt;
            out[1] += px[1] * wgt;
            out[2] += px[2] * wgt;
            weight += wgt;
        }
    }
    if (weight > 0.0f && weight < 1.0f) {
        out[0] /= weight;
        out[1] /= weight;
        out[2] /= weight;
    }
}

struct Accum
{
    float sum[3] = {0.0f, 0.0f, 0.0f};
    int count = 0;
};

void apply_accum(Pages& pages, const Chart& c, std::unordered_map<std::uint64_t, Accum>& acc)
{
    for (const auto& [index, a] : acc) {
        const auto cu = static_cast<std::int64_t>(index % (static_cast<std::uint64_t>(c.w) + 2)) - 1;
        const auto cv = static_cast<std::int64_t>(index / (static_cast<std::uint64_t>(c.w) + 2)) - 1;
        std::uint8_t* dst = chart_texel(pages, c, cu, cv);
        if (!dst || a.count == 0) {
            continue;
        }
        const float inv = 1.0f / static_cast<float>(a.count);
        for (int ch = 0; ch < 3; ch++) {
            const int val = static_cast<int>(dst[ch] * seam_own + a.sum[ch] * inv * seam_other + 0.5f);
            dst[ch] = static_cast<std::uint8_t>(std::clamp(val, 0, 255));
        }
    }
    acc.clear();
}

// Each edge between faces in two charts pulls both charts' texels along it toward each other, as surface edges
// are blended.
std::uint32_t blend_seams(Pages& pages)
{
    std::vector<std::uint32_t> chart_of(g_ov.faces.size(), std::numeric_limits<std::uint32_t>::max());
    for (std::uint32_t ci = 0; ci < g_ov.charts.size(); ci++) {
        const Chart& c = g_ov.charts[ci];
        for (std::uint32_t m = 0; m < c.num_members; m++) {
            chart_of[g_ov.members[c.first_member + m]] = ci;
        }
    }
    std::unordered_map<std::uint64_t, Accum> acc_a, acc_b;
    std::uint32_t blended = 0;
    for (const SharedEdge& e : g_ov.shared_edges) {
        const std::uint32_t ca = chart_of[e.a], cb = chart_of[e.b];
        if (ca == cb || ca >= g_ov.charts.size() || cb >= g_ov.charts.size()
            || dot(g_ov.faces[e.a].normal, g_ov.faces[e.b].normal) < seam_cos) {
            continue;
        }
        const Chart& a = g_ov.charts[ca];
        const Chart& b = g_ov.charts[cb];
        const alm::OverflowChart wa = wire_of(a), wb = wire_of(b);
        const V3 p0 = g_ov.verts[e.v0], p1 = g_ov.verts[e.v1];
        const float q0[3] = {p0.x, p0.y, p0.z}, q1[3] = {p1.x, p1.y, p1.z};
        const alm::ChartTexel a0 = alm::overflow_chart_coord(wa, q0), a1 = alm::overflow_chart_coord(wa, q1);
        const alm::ChartTexel b0 = alm::overflow_chart_coord(wb, q0), b1 = alm::overflow_chart_coord(wb, q1);
        const float ext = std::max({std::fabs(a1.u - a0.u), std::fabs(a1.v - a0.v), std::fabs(b1.u - b0.u),
                                    std::fabs(b1.v - b0.v)});
        if (!(ext >= 0.0f) || ext > 1e6f) {
            continue;
        }
        const int steps = std::clamp(static_cast<int>(ext * 8.0f) + 1, 16, 4096);
        const auto index = [](const Chart& c, float u, float v) {
            const std::int64_t cu = std::clamp<std::int64_t>(static_cast<std::int64_t>(std::floor(u)), -1, c.w);
            const std::int64_t cv = std::clamp<std::int64_t>(static_cast<std::int64_t>(std::floor(v)), -1, c.h);
            return static_cast<std::uint64_t>(cv + 1) * (static_cast<std::uint64_t>(c.w) + 2) +
                   static_cast<std::uint64_t>(cu + 1);
        };
        for (int k = 0; k <= steps; k++) {
            const float t = static_cast<float>(k) / static_cast<float>(steps);
            const float au = a0.u + (a1.u - a0.u) * t, av = a0.v + (a1.v - a0.v) * t;
            const float bu = b0.u + (b1.u - b0.u) * t, bv = b0.v + (b1.v - b0.v) * t;
            float sa[3], sb[3];
            chart_sample(pages, a, au, av, sa);
            chart_sample(pages, b, bu, bv, sb);
            Accum& x = acc_a[index(a, au, av)];
            Accum& y = acc_b[index(b, bu, bv)];
            for (int ch = 0; ch < 3; ch++) {
                x.sum[ch] += sb[ch];
                y.sum[ch] += sa[ch];
            }
            x.count++;
            y.count++;
        }
        apply_accum(pages, a, acc_a);
        apply_accum(pages, b, acc_b);
        blended++;
    }
    return blended;
}

void mark_shared(std::vector<std::uint8_t>& shared, std::uint32_t page, std::uint32_t x, std::uint32_t y)
{
    const std::uint64_t i = alm::bc7_block_index(page, x, y);
    if (i < shared.size()) {
        shared[static_cast<std::size_t>(i)] = 1;
    }
}

// Texels several tiles of a chart store become copies of the owning tile's value.
std::uint64_t reconcile(Pages& pages, std::vector<std::uint8_t>& shared)
{
    std::uint64_t copied = 0;
    for (const Chart& c : g_ov.charts) {
        if (c.geom.tile_count() <= 1) {
            continue;
        }
        for (std::uint32_t t = 0; t < c.geom.tile_count(); t++) {
            const std::uint32_t tx = t % c.geom.nx, ty = t / c.geom.nx;
            const alm::TileDims td = alm::tile_dims(c.geom, tx, ty);
            const alm::ChartCoord o = alm::tile_origin_chart(c.geom, tx, ty);
            const TileMask& m = g_ov.masks[c.first_mask + t];
            const alm::Tile& tile = g_ov.layout.tiles[c.tile_base + t];
            for (std::uint32_t sy = 0; sy < td.h_t; sy++) {
                for (std::uint32_t sx = 0; sx < td.w_t; sx++) {
                    if (!mask_bit(m, sx / 4, sy / 4)) {
                        continue;
                    }
                    const alm::TileSlot owner = alm::chart_tile_slot(c.geom, 0, o.u + sx, o.v + sy);
                    if (!owner.valid || owner.index == t || owner.index >= c.geom.tile_count()) {
                        continue;
                    }
                    const std::uint8_t* src = chart_texel(pages, c, o.u + sx, o.v + sy);
                    std::uint8_t* dst = page_texel(pages, tile.page, tile.x + sx, tile.y + sy);
                    if (!src || !dst) {
                        continue;
                    }
                    std::memcpy(dst, src, 3);
                    const alm::Tile& ot = g_ov.layout.tiles[c.tile_base + owner.index];
                    mark_shared(shared, tile.page, tile.x + sx, tile.y + sy);
                    mark_shared(shared, ot.page, ot.x + static_cast<std::uint32_t>(owner.sx),
                                ot.y + static_cast<std::uint32_t>(owner.sy));
                    copied++;
                }
            }
        }
    }
    return copied;
}

// Ordinal -> face of the solid as solid_write emits it.
std::vector<GFace*> faces_by_ordinal(const GSolid* solid)
{
    std::vector<GFace*> out;
    for (GFace* f = solid ? solid->face_list_head : nullptr; f; f = f->next_solid) {
        out.push_back(f);
    }
    return out;
}

// overflow_fingerprint of `ordinals` over the solid's faces as they are now; false when an ordinal has no face or
// repeats, as the game refuses.
bool live_fingerprint(const std::vector<GFace*>& by_ordinal, std::uint32_t src_num_faces,
                      const std::vector<std::uint32_t>& ordinals, std::uint64_t& out)
{
    std::vector<std::uint8_t> seen(by_ordinal.size(), 0);
    for (const std::uint32_t o : ordinals) {
        if (o >= seen.size() || seen[o]) {
            return false;
        }
        seen[o] = 1;
    }
    std::vector<const GVertex*> scratch;
    bool ok = true;
    out = alm::overflow_fingerprint(src_num_faces, ordinals.data(), static_cast<std::uint32_t>(ordinals.size()),
                                    [&](std::uint32_t o) {
                                        std::uint64_t h = 0;
                                        ok = ok && o < by_ordinal.size() && by_ordinal[o]
                                          && alm::overflow_face_loop_hash(by_ordinal[o], scratch, h);
                                        return h;
                                    });
    return ok;
}

// The section as the reader takes it, the editor having no surface fingerprint to check.
alm::ReadResult read_table(const std::uint8_t* body, std::size_t len)
{
    return alm::read_section(body, len, nullptr, 0, 0, false);
}

} // namespace

// ─── overflow_charts.h interface ──────────────────────────────────────────────

void overflow_bake_reset()
{
    g_ov = State{};
}

const OverflowLayout& overflow_layout()
{
    return g_ov.layout;
}

bool overflow_has_table()
{
    return !g_ov.layout.tiles.empty() && !g_ov.charts.empty();
}

std::uint32_t overflow_table_tiles()
{
    return overflow_has_table() ? static_cast<std::uint32_t>(g_ov.layout.tiles.size()) : 0u;
}

std::uint64_t overflow_table_bytes()
{
    std::uint64_t faces = 0;
    for (const Chart& c : g_ov.charts) {
        faces += c.num_members;
    }
    return alm::overflow_table_bytes(g_ov.charts.size(), faces);
}

bool overflow_bake_begin(const GSolid* solid, const std::vector<std::int32_t>& terrain_room_uids, float density,
                         std::uint32_t first_page, std::uint32_t max_pages)
{
    overflow_bake_reset();
    if (!solid || !(density > 0.0f)) {
        return false;
    }
    const DWORD t0 = GetTickCount();
    g_ov.first_page = first_page;
    try {
        collect(solid, terrain_room_uids);
        if (g_ov.faces.empty()) {
            overflow_bake_reset();
            return false;
        }
        build_adjacency();
        grow_charts(density);
        // grouping was their only reader; the seams keep their own edge list
        g_ov.nbr_offsets = {};
        g_ov.nbrs = {};
        g_ov.vert_ids = {};
        for (Chart& c : g_ov.charts) {
            if (!set_density(c, density)) {
                c.num_members = 0;
            }
        }
        std::erase_if(g_ov.charts, [](const Chart& c) { return c.num_members == 0; });

        // the reader's caps on the table
        std::uint64_t faces = 0;
        std::size_t keep = 0;
        for (; keep < g_ov.charts.size() && keep < alm::max_overflow_charts; keep++) {
            if (faces + g_ov.charts[keep].num_members > alm::max_overflow_faces) {
                break;
            }
            faces += g_ov.charts[keep].num_members;
        }
        if (keep < g_ov.charts.size()) {
            ov_warn("the overflow faces need more charts than a section can hold; " +
                    std::to_string(g_ov.charts.size() - keep) + " charts stay unlit");
            g_ov.charts.resize(keep);
        }

        // Pages the address space can still back at the late check's 256 KiB each, past its fixed 96 MB and
        // 64 MB block and a margin for the shading scratch.
        std::uint64_t largest = 0, free_total = 0;
        editor_address_space_free(largest, free_total);
        constexpr std::uint64_t reserve = (96ull + 64ull + 128ull) << 20;
        constexpr std::uint64_t per_page = static_cast<std::uint64_t>(alm::page_size) * alm::page_size * 4;
        const std::uint64_t backed = free_total > reserve ? (free_total - reserve) / per_page : 0;
        const std::uint64_t affordable = backed > first_page ? backed - first_page : 0;
        const std::uint32_t budget = static_cast<std::uint32_t>(std::min<std::uint64_t>(max_pages, affordable));
        bool lowered = false;
        std::uint32_t dropped_charts = 0, dropped_faces = 0;
        const bool fitted = budget > 0 && fit_and_pack(budget, lowered, dropped_charts, dropped_faces);
        // only worth saying when the memory cap may have cost density or charts
        if (budget < max_pages && (!fitted || lowered || dropped_charts)) {
            ov_warn("RED's free memory allows " + std::to_string(budget) + " overflow pages of the "
                    + std::to_string(max_pages) + " a section could hold; restarting RED before Calculate Lighting "
                    "raises that");
        }
        if (!fitted) {
            ov_warn(std::to_string(g_ov.faces.size()) + " faces past RED's 32767 lightmap surfaces do not fit the "
                    "page budget and stay unlit");
            overflow_bake_reset();
            return false;
        }
        if (dropped_faces) {
            ov_warn(std::to_string(dropped_faces) + " faces in " + std::to_string(dropped_charts) +
                    " small overflow charts did not fit the page budget and stay unlit; lower the level's lightmap "
                    "density to fit them");
        }
        // the fingerprint of the faces as they are now, which the save checks them against
        std::vector<std::uint32_t> ordinals;
        std::vector<GFace*> by_ordinal(g_ov.src_num_faces, nullptr);
        for (const Chart& c : g_ov.charts) {
            for (std::uint32_t m = 0; m < c.num_members; m++) {
                const Face& f = g_ov.faces[g_ov.members[c.first_member + m]];
                ordinals.push_back(f.ordinal);
                by_ordinal[f.ordinal] = f.face;
            }
        }
        if (!live_fingerprint(by_ordinal, g_ov.src_num_faces, ordinals, g_ov.fingerprint)) {
            ov_warn("the overflow faces could not be fingerprinted and stay unlit");
            overflow_bake_reset();
            return false;
        }

        std::uint32_t at_base = 0;
        float d_min = density;
        for (const Chart& c : g_ov.charts) {
            at_base += c.density == density ? 1u : 0u;
            d_min = std::min(d_min, c.density);
        }
        char densities[96];
        if (d_min == density) {
            std::snprintf(densities, sizeof(densities), "all at density %g", density);
        }
        else {
            std::snprintf(densities, sizeof(densities), "%u at density %g, the rest down to %g", at_base, density,
                          d_min);
        }
        char buf[320];
        std::snprintf(buf, sizeof(buf), "overflow: %u faces in %u charts (%s), %u tiles, %u pages, laid out in %.1fs",
                      static_cast<unsigned>(faces - dropped_faces), static_cast<unsigned>(g_ov.charts.size()),
                      densities, static_cast<unsigned>(g_ov.layout.tiles.size()), g_ov.layout.num_pages,
                      (GetTickCount() - t0) / 1000.0);
        ov_log(buf);
        return true;
    }
    catch (const std::bad_alloc&) {
        overflow_bake_reset();
        ov_warn("out of memory laying out the overflow charts; the faces past RED's 32767 lightmap surfaces stay "
                "unlit");
        return false;
    }
}

bool overflow_shade(std::vector<std::vector<std::uint8_t>>& pages, std::vector<std::uint8_t>& shared_blocks)
{
    if (!overflow_has_table()) {
        return true;
    }
    const DWORD t0 = GetTickCount();
    try {
        lightmap_prepare_terrain_bake();
        if (!shade_all(pages)) {
            if (!bake_progress_cancelled()) {
                ov_warn("the overflow charts could not be lit");
            }
            return false;
        }
        const DWORD t1 = GetTickCount();
        const std::uint32_t seams = blend_seams(pages);
        const std::uint64_t copied = reconcile(pages, shared_blocks);
        char buf[200];
        std::snprintf(buf, sizeof(buf), "overflow: lit in %.1fs, %u seams blended, %llu gutter texels copied",
                      (t1 - t0) / 1000.0, seams, static_cast<unsigned long long>(copied));
        ov_log(buf);
        return true;
    }
    catch (const std::bad_alloc&) {
        ov_warn("out of memory lighting the overflow charts");
        return false;
    }
}

void overflow_mark_unusable()
{
    if (overflow_has_table()) {
        g_ov.fingerprint = ~g_ov.fingerprint;
        g_ov.unusable = true;
        lighting_calc_report_refusal("The Alpine overflow lightmaps could not be lit; the faces past RED's 32767 "
                                     "lightmap surfaces render fullbright.");
    }
}

void overflow_append_table(std::vector<std::uint8_t>& body)
{
    if (!overflow_has_table()) {
        return;
    }
    std::uint32_t faces = 0;
    for (const Chart& c : g_ov.charts) {
        faces += c.num_members;
    }
    const alm::OverflowTableHeader head{static_cast<std::uint32_t>(g_ov.charts.size()), faces, g_ov.src_num_faces, 0,
                                        g_ov.fingerprint};
    const auto append = [&body](const void* p, std::size_t n) {
        const auto* b = static_cast<const std::uint8_t*>(p);
        body.insert(body.end(), b, b + n);
    };
    append(&head, sizeof(head));
    for (const Chart& c : g_ov.charts) {
        const alm::OverflowChart w = wire_of(c);
        append(&w, sizeof(w));
    }
    for (const Chart& c : g_ov.charts) {
        for (std::uint32_t m = 0; m < c.num_members; m++) {
            const std::uint32_t ordinal = g_ov.faces[g_ov.members[c.first_member + m]].ordinal;
            append(&ordinal, sizeof(ordinal));
        }
    }
}

OverflowStamp overflow_save_stamp(const std::uint8_t* body, std::size_t len, const GSolid* solid, std::size_t& offset,
                                  std::uint64_t& value)
{
    const alm::ReadResult r = read_table(body, len);
    if (!r.overflow_body_off) {
        return OverflowStamp::none;
    }
    const alm::OverflowTableHeader& head = r.overflow_head;
    offset = static_cast<std::size_t>(r.overflow_body_off) + offsetof(alm::OverflowTableHeader, face_fingerprint);
    const std::vector<GFace*> by_ordinal = faces_by_ordinal(solid);
    std::uint64_t live = 0;
    if (by_ordinal.size() != head.src_num_faces
        || !live_fingerprint(by_ordinal, head.src_num_faces, r.overflow_faces, live)) {
        // the reader refuses the table on its face count alone
        value = head.face_fingerprint;
        return OverflowStamp::stale;
    }
    // a table already stamped stale for these faces, or baked unusable, stays as it is
    if (live == head.face_fingerprint || ~live == head.face_fingerprint) {
        value = head.face_fingerprint;
        return OverflowStamp::match;
    }
    value = ~live;
    return OverflowStamp::stale;
}

void overflow_preview_clear()
{
    g_preview.clear();
}

void overflow_preview_from_bake()
{
    g_preview.clear();
    for (const Chart& c : g_ov.charts) {
        if (g_ov.unusable) {
            break;
        }
        for (std::uint32_t m = 0; m < c.num_members; m++) {
            g_preview[g_ov.faces[g_ov.members[c.first_member + m]].face] = {c.mean[0], c.mean[1], c.mean[2]};
        }
    }
    // Calculate Lighting flushed the room caches before the bake
    if (!g_preview.empty()) {
        overflow_preview_changed(true);
    }
}

void overflow_preview_from_section(const std::uint8_t* body, std::size_t len, const GSolid* solid)
{
    g_preview.clear();
    if (!solid) {
        return;
    }
    const alm::ReadResult r = read_table(body, len);
    const alm::OverflowTableHeader& head = r.overflow_head;
    const std::vector<GFace*> by_ordinal = faces_by_ordinal(solid);
    std::uint64_t live = 0;
    if (!r.overflow_body_off || by_ordinal.size() != head.src_num_faces
        || !live_fingerprint(by_ordinal, head.src_num_faces, r.overflow_faces, live)
        || live != head.face_fingerprint) {
        return;
    }
    for (std::size_t i = 0; i < r.overflow.size(); i++) {
        if (!r.overflow_ok[i]) {
            continue;
        }
        const alm::OverflowChart& c = r.overflow[i];
        for (std::uint32_t k = 0; k < c.num_faces; k++) {
            g_preview[by_ordinal[r.overflow_faces[r.overflow_first_face[i] + k]]] = {c.mean_rgb[0], c.mean_rgb[1],
                                                                                    c.mean_rgb[2]};
        }
    }
    overflow_preview_changed(false);
}

const std::unordered_map<const GFace*, std::array<std::uint8_t, 3>>& overflow_preview_faces()
{
    return g_preview;
}
