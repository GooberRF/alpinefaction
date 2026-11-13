#include <windows.h>
#include <commctrl.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <numbers>
#include <optional>
#include <queue>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>
#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/FunHook.h>
#include <xlog/xlog.h>
#include "alpine_obj.h"
#include "alpine_spinner.h"
#include "dialog_tooltips.h"
#include "editor_settings.h"
#include "graphics/editor_gr.h"
#include "level.h"
#include "mfc_types.h"
#include "resources.h"
#include "texture_mode.h"
#include "textures.h"
#include "uv_map.h"
#include "vtypes.h"

namespace
{

// ─── Room caches ────────────────────────────────────────────────────────────

// FUN_00490c10 rewrites the UVs of the level geometry faces built from a brush face; Texture mode's UV edits (pan,
// rotate, scale, flip, paste, Fit, UV Unwrap) go through it. Only Render Everything draws faces as they are, so the
// room caches the other modes draw from are dropped to pick the new UVs up.
void __fastcall update_faces_from_brush_face_hooked(GSolid* level_solid, int edx, GFace* brush_face,
                                                    const Vector3* pos, const Matrix3* orient);
FunHook<decltype(update_faces_from_brush_face_hooked)> update_faces_from_brush_face_hook{
    0x00490C10, update_faces_from_brush_face_hooked};
void __fastcall update_faces_from_brush_face_hooked(GSolid* level_solid, int edx, GFace* brush_face,
                                                    const Vector3* pos, const Matrix3* orient)
{
    update_faces_from_brush_face_hook.call_target(level_solid, edx, brush_face, pos, orient);
    geo_cache_flush_all();
}

// Box, Planar and Cylinder Map (FUN_00438fe0, FUN_00439070, FUN_004392c0, from Apply Map and Texture mode's keys)
// write the level geometry's UVs themselves
FunHook<void __fastcall(CDedLevel*)> box_map_hook{
    0x00438FE0,
    [](CDedLevel* level) FASTCALL_LAMBDA {
        box_map_hook.call_target(level);
        geo_cache_flush_all();
    },
};
FunHook<void __fastcall(CDedLevel*)> planar_map_hook{
    0x00439070,
    [](CDedLevel* level) FASTCALL_LAMBDA {
        planar_map_hook.call_target(level);
        geo_cache_flush_all();
    },
};
FunHook<void __fastcall(CDedLevel*)> cylinder_map_hook{
    0x004392C0,
    [](CDedLevel* level) FASTCALL_LAMBDA {
        cylinder_map_hook.call_target(level);
        geo_cache_flush_all();
    },
};

// ─── Fit ────────────────────────────────────────────────────────────────────

// The corners with a vertex whose next corner has one too
template<typename Fn>
void for_each_corner(GFace* face, Fn&& fn)
{
    for_each_face_corner(*face, [&](GFaceVertex& fv) {
        if (fv.vertex && fv.next && fv.next->vertex) {
            fn(fv);
        }
    });
}

struct SelectedFace
{
    BrushNode* brush;
    GFace* face;
};

// A brush face's corner placed by its brush; a level geometry face's is already in the world
Vector3 corner_world(const SelectedFace& f, const GFaceVertex& fv)
{
    return f.brush ? f.brush->to_world(fv.vertex->pos) : fv.vertex->pos;
}

std::vector<Vector3> face_corners_world(const SelectedFace& f)
{
    std::vector<Vector3> corners;
    for_each_corner(f.face, [&](GFaceVertex& fv) { corners.push_back(corner_world(f, fv)); });
    return corners;
}

// Panel options, kept for the session
float g_fit_tiles_u = 1.0f;
float g_fit_tiles_v = 1.0f;
int g_fit_quarter_turns = 0;
bool g_edit_each_face = false;

// In brush list order
std::vector<SelectedFace> selected_faces(CDedLevel& level)
{
    std::vector<SelectedFace> faces;
    level.for_each_brush([&](BrushNode& brush) {
        auto* geometry = static_cast<GSolid*>(brush.geometry);
        if (!geometry) {
            return;
        }
        for (int i = 0; i < geometry->face_selection.size; ++i) {
            if (GFace* face = geometry->face_selection.data_ptr[i]) {
                faces.push_back({&brush, face});
            }
        }
    });
    return faces;
}

// Runs edit on the selected faces as one group, or on each alone, as one undo step, then pushes their UVs to the level
// geometry.
template<typename Fn>
void edit_selected_faces(const char* none_selected, bool each_face, Fn&& edit)
{
    auto* level = CDedLevel::Get();
    if (!level || !level->solid) {
        return;
    }
    if (selected_faces(*level).empty()) {
        show_error_message(none_selected);
        return;
    }
    // The snapshot keeps each brush with selected faces in the undo entry and lists a clone in its place, so the
    // faces to edit are the clones'.
    level->create_undo_snapshot();
    const std::vector<SelectedFace> faces = selected_faces(*level);
    if (each_face) {
        for (const SelectedFace& f : faces) {
            edit(std::span<const SelectedFace>{&f, 1});
        }
    }
    else {
        edit(std::span<const SelectedFace>{faces});
    }
    for (const SelectedFace& f : faces) {
        level->solid->update_faces_from_brush_face(f.face, &f.brush->pos, &f.brush->orient);
    }
    redraw_all_viewports();
}

// Width over height of the face's texture, 1 when unknown
float texture_aspect(const GFace& face)
{
    if (face.bitmap_id < 0) return 1.0f;
    int width = 0, height = 0;
    bm_get_dimensions(face.bitmap_id, &width, &height);
    return width > 0 && height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
}

// A world point where Box Map (FUN_00499820) puts it on a face facing n: across and down, in meters, on the world plane
// n faces most
std::pair<float, float> box_map_point(const Vector3& n, const Vector3& p)
{
    const float ax = std::abs(n.x), ay = std::abs(n.y), az = std::abs(n.z);
    if (az >= ax && az >= ay) {
        return {n.z > 0.0f ? -p.x : p.x, -p.y};
    }
    if (ax > ay) {
        return {n.x > 0.0f ? p.z : -p.z, -p.y};
    }
    return {n.y > 0.0f ? -p.x : p.x, p.z};
}

// Laid out as Box Map lays out a face facing the faces' summed (area weighted) normal, the texture turned clockwise
// by quarter_turns, and their bounds mapped onto tiles_u x tiles_v tiles. Keep Aspect keeps the first face's texels
// square and centers the shorter axis.
void fit_faces(std::span<const SelectedFace> faces, bool keep_aspect, float tiles_u, float tiles_v, int quarter_turns)
{
    Vector3 sum{0.0f, 0.0f, 0.0f};
    Vector3 first_normal{0.0f, 0.0f, 1.0f};
    bool have_first = false;
    for (const auto& f : faces) {
        // twice the face's area along its normal
        const Vector3 n = newell_normal(face_corners_world(f));
        if (!have_first && dot(n, n) > 1e-20f) {
            first_normal = n;
            have_first = true;
        }
        sum = sum + n;
    }
    const Vector3 n = dot(sum, sum) > 1e-12f ? sum : first_normal;
    // the texture turned clockwise is the face turned the other way under it
    auto texture_point = [&](const Vector3& p) -> std::pair<float, float> {
        const auto [x, y] = box_map_point(n, p);
        switch (quarter_turns & 3) {
        case 1: return {y, -x};
        case 2: return {-x, -y};
        case 3: return {-y, x};
        default: return {x, y};
        }
    };

    float min_u = INFINITY, max_u = -INFINITY, min_v = INFINITY, max_v = -INFINITY;
    for (const auto& f : faces) {
        for_each_corner(f.face, [&](GFaceVertex& fv) {
            const auto [pu, pv] = texture_point(corner_world(f, fv));
            min_u = std::min(min_u, pu);
            max_u = std::max(max_u, pu);
            min_v = std::min(min_v, pv);
            max_v = std::max(max_v, pv);
        });
    }
    if (!std::isfinite(min_u) || !std::isfinite(min_v)) {
        return;
    }

    const float du = max_u - min_u;
    const float dv = max_v - min_v;
    float su = du > 1e-6f ? tiles_u / du : 0.0f;
    float sv = dv > 1e-6f ? tiles_v / dv : 0.0f;
    if (keep_aspect) {
        // sv = su * aspect gives as many texels per meter along V as along U
        const float aspect = texture_aspect(*faces.front().face);
        float s = du > 1e-6f ? tiles_u / du : INFINITY;
        if (dv > 1e-6f) {
            s = std::min(s, tiles_v / (dv * aspect));
        }
        su = std::isfinite(s) ? s : 0.0f;
        sv = su * aspect;
    }
    const float off_u = (tiles_u - du * su) * 0.5f;
    const float off_v = (tiles_v - dv * sv) * 0.5f;

    for (const auto& f : faces) {
        for_each_corner(f.face, [&](GFaceVertex& fv) {
            const auto [pu, pv] = texture_point(corner_world(f, fv));
            fv.u = su > 0.0f ? (pu - min_u) * su + off_u : tiles_u * 0.5f;
            fv.v = sv > 0.0f ? (pv - min_v) * sv + off_v : tiles_v * 0.5f;
        });
    }
}

enum class Justify
{
    left,
    right,
    top,
    bottom,
    center,
};

// Moves the texture, without scaling or turning it, so a tile edge (or a tile's center) meets that edge (or the
// center) of the faces' bounds in texture space.
void justify_faces(std::span<const SelectedFace> faces, Justify justify)
{
    float min_u = INFINITY, max_u = -INFINITY, min_v = INFINITY, max_v = -INFINITY;
    for (const auto& f : faces) {
        for_each_corner(f.face, [&](GFaceVertex& fv) {
            min_u = std::min(min_u, fv.u);
            max_u = std::max(max_u, fv.u);
            min_v = std::min(min_v, fv.v);
            max_v = std::max(max_v, fv.v);
        });
    }
    if (!std::isfinite(min_u) || !std::isfinite(min_v)) {
        return;
    }
    float shift_u = 0.0f, shift_v = 0.0f;
    switch (justify) {
    case Justify::left: shift_u = std::floor(min_u) - min_u; break;
    case Justify::right: shift_u = std::ceil(max_u) - max_u; break;
    case Justify::top: shift_v = std::floor(min_v) - min_v; break;
    case Justify::bottom: shift_v = std::ceil(max_v) - max_v; break;
    case Justify::center: {
        const float cu = (min_u + max_u) * 0.5f;
        const float cv = (min_v + max_v) * 0.5f;
        shift_u = std::floor(cu) + 0.5f - cu;
        shift_v = std::floor(cv) + 0.5f - cv;
        break;
    }
    }
    for (const auto& f : faces) {
        for_each_corner(f.face, [&](GFaceVertex& fv) {
            fv.u += shift_u;
            fv.v += shift_v;
        });
    }
}

// ─── Wrap ───────────────────────────────────────────────────────────────────

Vector3 face_centroid(const SelectedFace& f)
{
    return centroid(face_corners_world(f));
}

// Unit normal of the face in world space, zero for a degenerate one
Vector3 face_normal(const SelectedFace& f)
{
    return normalized(newell_normal(face_corners_world(f)));
}

// The brush face texture mode's click last hit, Wrap's reference. RED keeps no record of it; a Build renumbers face
// ids, so the centroid is checked again before use.
struct WrapReference
{
    int brush_uid = -1;
    int face_id = -1;
    Vector3 centroid{0.0f, 0.0f, 0.0f};
};
WrapReference g_wrap_reference;

std::optional<SelectedFace> find_brush_face(CDedLevel& level, int face_id, int brush_uid)
{
    std::optional<SelectedFace> found;
    if (face_id < 0) {
        return found;
    }
    level.for_each_brush([&](BrushNode& brush) {
        auto* geometry = static_cast<GSolid*>(brush.geometry);
        if (!geometry || (brush_uid >= 0 && brush.uid != brush_uid)) {
            return false;
        }
        return geometry->for_each_face([&](GFace& face) {
            if (face.face_id == face_id) {
                found = SelectedFace{&brush, &face};
                return true;
            }
            return false;
        });
    });
    return found;
}

// Texture mode's click logs "Hit Face %d" for the face it hit, the brush face or a compiled one with the same id
CallHook<void __cdecl(void* log, const char* fmt, int face_id)> texture_mode_hit_face_log_hook{
    {0x0042C33B, 0x0042C425},
    [](void* log, const char* fmt, int face_id) {
        g_wrap_reference = {};
        if (auto* level = CDedLevel::Get()) {
            if (const auto hit = find_brush_face(*level, face_id, -1)) {
                g_wrap_reference = {hit->brush->uid, face_id, face_centroid(*hit)};
            }
        }
        texture_mode_hit_face_log_hook.call_target(log, fmt, face_id);
    },
};

std::optional<SelectedFace> wrap_reference_face(CDedLevel& level)
{
    const auto face = find_brush_face(level, g_wrap_reference.face_id, g_wrap_reference.brush_uid);
    if (!face || length(face_centroid(*face) - g_wrap_reference.centroid) > 0.001f) {
        return std::nullopt;
    }
    return face;
}

std::optional<UvMap> face_uv_map(const SelectedFace& f)
{
    std::vector<UvCorner> corners;
    for_each_corner(f.face, [&](GFaceVertex& fv) { corners.push_back({corner_world(f, fv), fv.u, fv.v}); });
    return uv_map_through(corners);
}

struct WrapFace
{
    SelectedFace face;
    std::vector<Vector3> corners;
    Vector3 normal;
    std::optional<UvMap> map;
};

WrapFace make_wrap_face(const SelectedFace& f)
{
    WrapFace w{f, face_corners_world(f), {}, std::nullopt};
    w.normal = normalized(newell_normal(w.corners));
    return w;
}

bool on_boundary(const Vector3& p, const std::vector<Vector3>& polygon, float eps)
{
    for (size_t i = 0; i < polygon.size(); ++i) {
        const Vector3& a = polygon[i];
        const Vector3 ab = polygon[(i + 1) % polygon.size()] - a;
        const float len_sq = dot(ab, ab);
        const float t = len_sq > 0.0f ? std::clamp(dot(p - a, ab) / len_sq, 0.0f, 1.0f) : 0.0f;
        if (length(p - (a + ab * t)) < eps) {
            return true;
        }
    }
    return false;
}

// The edge `to` is folded about: the two farthest apart of the corners of either face lying on the other's boundary,
// so faces meeting along part of an edge (a T-junction between brushes) count too
std::optional<std::pair<Vector3, Vector3>> shared_edge(const WrapFace& from, const WrapFace& to)
{
    constexpr float eps = 0.001f;
    std::vector<Vector3> touching;
    for (const Vector3& p : to.corners) {
        if (on_boundary(p, from.corners, eps)) {
            touching.push_back(p);
        }
    }
    for (const Vector3& p : from.corners) {
        if (on_boundary(p, to.corners, eps)) {
            touching.push_back(p);
        }
    }
    std::optional<std::pair<Vector3, Vector3>> edge;
    // long enough that points eps off the line can't turn it
    float longest = 10.0f * eps;
    for (size_t i = 0; i < touching.size(); ++i) {
        for (size_t j = i + 1; j < touching.size(); ++j) {
            const float len = length(touching[j] - touching[i]);
            if (len > longest) {
                longest = len;
                edge = std::pair{touching[i], touching[j]};
            }
        }
    }
    return edge;
}

// The map turned about a line through pivot until its plane's front (n_from) faces n_to, so it reads unmirrored on
// that front; facing back, half a turn about flip_axis (in the plane). Parallel and facing alike, it is kept.
UvMap turn_uv_map(const UvMap& m, const Vector3& n_from, const Vector3& n_to, const Vector3& pivot,
                  const Vector3& flip_axis)
{
    float c = dot(n_from, n_to);
    float s = length(cross(n_from, n_to));
    if (c > 0.9999f) {
        return m;
    }
    const bool back = c < -0.9999f;
    const Vector3 k = normalized(back ? flip_axis : cross(n_from, n_to));
    if (length(k) == 0.0f) {
        return m;
    }
    if (back) {
        c = -1.0f;
        s = 0.0f;
    }
    // Rodrigues; a gradient in the plane turns with it
    auto turn = [&](const Vector3& v) { return v * c + cross(k, v) * s + k * (dot(k, v) * (1.0f - c)); };
    return UvMap{pivot, turn(m.grad_u), turn(m.grad_v), m.u(pivot), m.v(pivot)};
}

// `to` unfolded about the edge onto `from`'s plane: the texture runs on across the edge at the same density, turned by
// the faces' fronts so a brush face reaching past the edge doesn't mirror it
UvMap fold_uv_map(const WrapFace& from, const WrapFace& to, const Vector3& a, const Vector3& b)
{
    return turn_uv_map(*from.map, from.normal, to.normal, a, b - a);
}

// Spreads the reference's mapping over the selected faces connected to it through shared edges, or with `apply`
// false only counts them
size_t wrap_faces(std::span<const SelectedFace> faces, const SelectedFace& reference, bool apply)
{
    std::vector<WrapFace> wraps;
    wraps.push_back(make_wrap_face(reference));
    wraps[0].map = face_uv_map(reference);
    if (!wraps[0].map) {
        return 0;
    }
    for (const SelectedFace& f : faces) {
        if (f.face != reference.face) {
            wraps.push_back(make_wrap_face(f));
        }
    }
    // Across the longest shared edges first (in mm, nearest the reference among equals): where the faces can't all
    // unfold flat (a box, a room's corner) the seams that can't line up fall on the shortest edges, away from the
    // reference
    struct Crossing
    {
        long edge_mm;
        size_t depth;
        size_t from;
        size_t to;
        Vector3 a;
        Vector3 b;

        bool operator<(const Crossing& other) const
        {
            return edge_mm < other.edge_mm || (edge_mm == other.edge_mm && depth > other.depth);
        }
    };
    std::vector<size_t> depth(wraps.size(), 0);
    std::priority_queue<Crossing> crossings;
    auto cross_from = [&](size_t from) {
        for (size_t i = 1; i < wraps.size(); ++i) {
            if (wraps[i].map || length(wraps[i].normal) == 0.0f) {
                continue;
            }
            if (const auto edge = shared_edge(wraps[from], wraps[i])) {
                const long edge_mm = std::lround(length(edge->second - edge->first) * 1000.0f);
                crossings.push({edge_mm, depth[from] + 1, from, i, edge->first, edge->second});
            }
        }
    };
    size_t reached = 0;
    cross_from(0);
    while (!crossings.empty()) {
        const Crossing c = crossings.top();
        crossings.pop();
        if (wraps[c.to].map) {
            continue;
        }
        wraps[c.to].map = fold_uv_map(wraps[c.from], wraps[c.to], c.a, c.b);
        depth[c.to] = c.depth;
        ++reached;
        cross_from(c.to);
    }
    if (!apply) {
        return reached;
    }
    for (size_t i = 1; i < wraps.size(); ++i) {
        if (!wraps[i].map) {
            continue;
        }
        const UvMap& m = *wraps[i].map;
        for_each_corner(wraps[i].face.face, [&](GFaceVertex& fv) {
            const Vector3 p = corner_world(wraps[i].face, fv);
            fv.u = m.u(p);
            fv.v = m.v(p);
        });
    }
    return reached;
}

void wrap_selected_faces()
{
    auto* level = CDedLevel::Get();
    const auto reference = level ? wrap_reference_face(*level) : std::nullopt;
    if (!reference) {
        show_error_message("Select the faces to wrap onto, then Ctrl+click the face to wrap from.");
        return;
    }
    const std::vector<SelectedFace> faces = selected_faces(*level);
    if (faces.empty()) {
        show_error_message("You must select a face to wrap onto.");
        return;
    }
    if (!wrap_faces(faces, *reference, false)) {
        show_error_message("None of the selected faces shares an edge with the face to wrap from.");
        return;
    }
    edit_selected_faces("You must select a face to wrap onto.", false, [&](std::span<const SelectedFace> clones) {
        // the snapshot may have replaced the reference's brush with a clone
        if (const auto clone = wrap_reference_face(*level)) {
            wrap_faces(clones, *clone, true);
        }
    });
}

// ─── Copy and Paste ─────────────────────────────────────────────────────────

// Paste applies the copied face's texture with its UVs; kept for the session
bool g_paste_texture = true;

// What Copy took besides stock's corner UVs (CDedLevel::texture_clipboard_u/v): the face's texture, and its mapping in
// the world for Paste to lay onto faces with another number of corners
struct TextureClipboard
{
    int bitmap_id = -1;
    std::optional<UvMap> map;
    Vector3 normal{0.0f, 0.0f, 0.0f};
};
TextureClipboard g_texture_clipboard;

// FUN_004132a0: Texture mode's Copy, of the one selected brush face's corner UVs
void __fastcall texture_copy_hooked(CDedLevel* level, int edx);
FunHook<decltype(texture_copy_hooked)> texture_copy_hook{0x004132A0, texture_copy_hooked};
void __fastcall texture_copy_hooked(CDedLevel* level, int edx)
{
    texture_copy_hook.call_target(level, edx);
    g_texture_clipboard = {};
    const std::vector<SelectedFace> faces = selected_faces(*level);
    if (level->texture_clipboard_u.size > 0 && faces.size() == 1) {
        const SelectedFace& f = faces.front();
        g_texture_clipboard = {f.face->bitmap_id, face_uv_map(f), face_normal(f)};
    }
}

// The map turned onto the plane of a face with normal n_to through p_to, about the line the planes meet in: across
// an edge the faces share this is Wrap's fold. A face parallel to it facing back turns about the texture's U, half
// way between the planes.
UvMap align_uv_map(const UvMap& m, const Vector3& n_from, const Vector3& n_to, const Vector3& p_to)
{
    const float c = dot(n_from, n_to);
    Vector3 pivot = m.origin + n_from * (dot(n_from, p_to - m.origin) * 0.5f);
    // near parallel, the planes' line is far off and its UVs lose precision; half way between them is near enough
    if (std::abs(c) <= 0.999f) {
        // origin + a n_from + b n_to on both planes, the point of the line nearest the origin
        const float b = dot(n_to, p_to - m.origin) / (1.0f - c * c);
        pivot = m.origin + n_from * (-c * b) + n_to * b;
    }
    return turn_uv_map(m, n_from, n_to, pivot, m.grad_u);
}

// FUN_00413470: Texture mode's Paste. Stock lays the copied UVs corner by corner onto faces with as many corners,
// stops at a message box for each other face, and has no undo step. This is one undo step, lays the copied face's
// mapping onto faces with another number of corners, and with Paste texture checked applies its texture too.
void __fastcall texture_paste_hooked(CDedLevel* level, int edx);
FunHook<decltype(texture_paste_hooked)> texture_paste_hook{0x00413470, texture_paste_hooked};
void __fastcall texture_paste_hooked(CDedLevel* level, [[maybe_unused]] int edx)
{
    const int count = level->texture_clipboard_u.size;
    if (count <= 0 || level->texture_clipboard_v.size != count) {
        show_error_message("Select one face and copy it first.");
        return;
    }
    const TextureClipboard& clip = g_texture_clipboard;
    const std::vector<SelectedFace> selection = selected_faces(*level);
    if (!selection.empty() && std::none_of(selection.begin(), selection.end(), [&](const SelectedFace& f) {
            return face_corner_count(*f.face) == count || (clip.map && length(face_normal(f)) > 0.0f);
        })) {
        show_error_message("None of the selected faces can take the copied texture: they or the copied face have no "
                           "area.");
        return;
    }
    const int bitmap_id = g_paste_texture ? clip.bitmap_id : -1;
    int skipped = 0;
    edit_selected_faces("You must select a face to paste onto.", false, [&](std::span<const SelectedFace> faces) {
        std::vector<int> retextured;
        for (const SelectedFace& f : faces) {
            bool pasted = false;
            if (face_corner_count(*f.face) == count) {
                if (GFaceVertex* first = texture_clipboard_first_corner(f.brush, f.face)) {
                    GFaceVertex* fv = first;
                    for (int i = 0; i < count && fv; ++i, fv = fv->next) {
                        fv->u = level->texture_clipboard_u.data_ptr[i];
                        fv->v = level->texture_clipboard_v.data_ptr[i];
                    }
                    pasted = true;
                }
            }
            else if (clip.map) {
                const Vector3 n = face_normal(f);
                const std::vector<Vector3> corners = face_corners_world(f);
                if (length(n) > 0.0f && !corners.empty()) {
                    const UvMap m = align_uv_map(*clip.map, clip.normal, n, corners.front());
                    for_each_corner(f.face, [&](GFaceVertex& fv) {
                        const Vector3 p = corner_world(f, fv);
                        fv.u = m.u(p);
                        fv.v = m.v(p);
                    });
                    pasted = true;
                }
            }
            if (!pasted) {
                ++skipped;
                continue;
            }
            if (bitmap_id >= 0) {
                f.face->bitmap_id = bitmap_id;
                retextured.push_back(f.face->face_id);
            }
        }
        if (retextured.empty()) {
            return;
        }
        // the level geometry faces built from them, as Apply sets them
        std::sort(retextured.begin(), retextured.end());
        level->solid->for_each_face([&](GFace& face) {
            if (face.face_id >= 0 && std::binary_search(retextured.begin(), retextured.end(), face.face_id)) {
                face.bitmap_id = bitmap_id;
            }
            return false;
        });
    });
    if (skipped) {
        show_error_message(std::format("{} of the selected faces could not take the copied texture: they or the "
                                       "copied face have no area.",
                                       skipped)
                               .c_str());
    }
}

// FUN_004028d0's DoModal: on OK Face Properties sets the selected faces' flags, smoothing groups and scrolling, on
// brushes and the level geometry alike, with no undo step. The snapshot lists clones of the brushes with selected
// faces for it to edit; undoing it puts the level geometry back through texture_undo_entry_applied.
CallHook<int __fastcall(void* dialog, int edx)> face_properties_do_modal_hook{
    0x00402C2B,
    [](void* dialog, int edx) FASTCALL_LAMBDA {
        const int result = face_properties_do_modal_hook.call_target(dialog, edx);
        if (result == IDOK) {
            if (CDedLevel* level = CDedLevel::Get()) {
                level->create_undo_snapshot();
            }
        }
        return result;
    },
};

// ─── Cylinder and Sphere Map ────────────────────────────────────────────────

// Times Cylinder and Sphere Map take the texture around, 0 for stock's texture scale on the Resize Map radius; kept for
// the session
float g_map_wraps = 0.0f;

// Cylinder or Sphere Map's UVs for the selected faces of a solid at obj_pos/obj_orient. As stock Cylinder Map
// (FUN_00499b60): U from the angle about the gizmo's uvec (0 along its rvec) and V from the height along it, in texels
// at the pixels per meter on the radius. With wraps the texture goes around that many times, its texels square;
// Sphere takes V from the latitude.
void map_round_faces(GSolid& solid, const Vector3& obj_pos, const Matrix3& obj_orient, const Vector3& map_pos,
                     const Matrix3& map_orient, float radius, bool sphere, float wraps)
{
    struct Corner
    {
        GFaceVertex* fv;
        Vector3 local; // along rvec, uvec, fvec from the gizmo
        float angle;
    };
    std::vector<Corner> corners;
    for (int i = 0; i < solid.face_selection.size; ++i) {
        GFace* face = solid.face_selection.data_ptr[i];
        if (!face || face->bitmap_id < 0) {
            continue;
        }
        corners.clear();
        for_each_face_corner(*face, [&](GFaceVertex& fv) {
            if (!fv.vertex) return;
            const Vector3 d = obj_orient * fv.vertex->pos + obj_pos - map_pos;
            corners.push_back({&fv, {dot(d, map_orient.rvec), dot(d, map_orient.uvec), dot(d, map_orient.fvec)},
                               0.0f});
        });
        // As stock: a face across the seam takes the angles on its +fvec side a turn further
        bool ahead = false, behind = false;
        for (const Corner& c : corners) {
            if (c.local.x > 0.0f) {
                (c.local.z < 0.0f ? behind : ahead) = true;
            }
        }
        auto on_axis = [](const Corner& c) { return std::abs(c.local.x) < 1e-5f && std::abs(c.local.z) < 1e-5f; };
        float angle_sum = 0.0f;
        int angle_count = 0;
        for (Corner& c : corners) {
            c.angle = std::atan2(c.local.z, c.local.x);
            if (c.angle < 0.0f) {
                c.angle += two_pi;
            }
            if (ahead && behind && c.local.z >= 0.0f) {
                c.angle += two_pi;
            }
            if (!on_axis(c)) {
                angle_sum += c.angle;
                ++angle_count;
            }
        }
        int width = 16, height_px = 16;
        bm_get_dimensions(face->bitmap_id, &width, &height_px);
        const float w = width > 0 ? static_cast<float>(width) : 16.0f;
        const float h = height_px > 0 ? static_cast<float>(height_px) : 16.0f;
        for (const Corner& c : corners) {
            // a pole has no angle of its own
            const float angle = sphere && on_axis(c) && angle_count ? angle_sum / angle_count : c.angle;
            // meters from the gizmo's middle along the surface
            const float along =
                sphere ? std::atan2(c.local.y, std::hypot(c.local.x, c.local.z)) * radius : c.local.y;
            if (wraps > 0.0f) {
                c.fv->u = angle * wraps / two_pi;
                c.fv->v = along * wraps * w / (two_pi * radius * h);
            }
            else {
                c.fv->u = angle * radius * texture_map_pixels_per_meter / w;
                c.fv->v = along * texture_map_pixels_per_meter / h;
            }
        }
    }
}

// FUN_00499b60: stock Cylinder Map's UVs for a solid, laid out by map_round_faces with Wraps set
void __fastcall texture_map_cylinder_faces_hooked(GSolid* solid, int edx, const Vector3* obj_pos,
                                                  const Matrix3* obj_orient, const Vector3* map_pos,
                                                  const Matrix3* map_orient, float radius, float height);
FunHook<decltype(texture_map_cylinder_faces_hooked)> texture_map_cylinder_faces_hook{
    0x00499B60, texture_map_cylinder_faces_hooked};
void __fastcall texture_map_cylinder_faces_hooked(GSolid* solid, int edx, const Vector3* obj_pos,
                                                  const Matrix3* obj_orient, const Vector3* map_pos,
                                                  const Matrix3* map_orient, float radius, float height)
{
    if (!(g_map_wraps > 0.0f) || !(radius > 0.0f)) {
        texture_map_cylinder_faces_hook.call_target(solid, edx, obj_pos, obj_orient, map_pos, map_orient, radius,
                                                    height);
        return;
    }
    map_round_faces(*solid, *obj_pos, *obj_orient, *map_pos, *map_orient, radius, false, g_map_wraps);
}

// Sphere Map, as Cylinder Map (FUN_004392c0) runs: with a level geometry face selected, one undo step over every
// brush's selected faces, then the level geometry's
void sphere_map(CDedLevel& level, const TextureModePanel& panel)
{
    if (!level.solid || level.solid->face_selection.size <= 0 || !panel.resize_map || !panel.resize_map->radius) {
        return;
    }
    const float radius = panel.resize_map->radius->value;
    if (!(radius > 0.0f)) {
        return;
    }
    level.create_undo_snapshot();
    level.for_each_brush([&](BrushNode& brush) {
        if (auto* geometry = static_cast<GSolid*>(brush.geometry)) {
            map_round_faces(*geometry, brush.pos, brush.orient, level.texture_map_pos, level.texture_map_orient,
                            radius, true, g_map_wraps);
        }
    });
    map_round_faces(*level.solid, Vector3{}, identity_orient, level.texture_map_pos, level.texture_map_orient, radius,
                    true, g_map_wraps);
    geo_cache_flush_all();
}

// FUN_00445680: Apply Map by the map combo's pick, Sphere among them
void __fastcall texture_apply_map_hooked(TextureModePanel* panel, int edx);
FunHook<decltype(texture_apply_map_hooked)> texture_apply_map_hook{0x00445680, texture_apply_map_hooked};
void __fastcall texture_apply_map_hooked(TextureModePanel* panel, int edx)
{
    if (panel->map_type != TEXTURE_MAP_SPHERE) {
        texture_apply_map_hook.call_target(panel, edx);
        return;
    }
    if (CDedLevel* level = CDedLevel::Get()) {
        sphere_map(*level, *panel);
    }
    redraw_all_viewports();
}

// FUN_0041e4c0 draws Cylinder Map's gizmo in Texture mode (EAX the panel, EBP the level); Sphere's is a circle about
// each of its axes. No trampoline: every path sets EIP past the replaced cmp/jne.
CodeInjection texture_map_gizmo_draw_injection{
    0x0041EC46,
    [](auto& regs) {
        const auto* panel = reinterpret_cast<const TextureModePanel*>(static_cast<uintptr_t>(regs.eax));
        if (panel->map_type == TEXTURE_MAP_CYLINDER) {
            regs.eip = 0x0041EC53;
            return;
        }
        regs.eip = 0x0041ECFD;
        const auto* level = reinterpret_cast<const CDedLevel*>(static_cast<uintptr_t>(regs.ebp));
        if (panel->map_type != TEXTURE_MAP_SPHERE || !panel->resize_map || !panel->resize_map->radius) {
            return;
        }
        set_draw_color(0, 255, 255, 255);
        const Matrix3& o = level->texture_map_orient;
        const float radius = panel->resize_map->radius->value;
        for (const Matrix3& ring : {o, Matrix3{o.uvec, o.fvec, o.rvec}, Matrix3{o.fvec, o.rvec, o.uvec}}) {
            draw_cylinder_gizmo(&level->texture_map_pos, &ring, radius, 0.0f, editor_line_mode());
        }
    },
    false,
};

// Unit eigenvectors of a symmetric 3x3 matrix by Jacobi rotations, ordered by eigenvalue, smallest first
std::array<std::pair<double, Vector3>, 3> symmetric_eigen(std::array<std::array<double, 3>, 3> a)
{
    std::array<std::array<double, 3>, 3> v{{{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}}};
    for (int sweep = 0; sweep < 32; ++sweep) {
        const double off = a[0][1] * a[0][1] + a[0][2] * a[0][2] + a[1][2] * a[1][2];
        if (off < 1e-24) break;
        for (int p = 0; p < 2; ++p) {
            for (int q = p + 1; q < 3; ++q) {
                if (std::abs(a[p][q]) < 1e-30) continue;
                const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
                const double t = (theta >= 0.0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0);
                const double s = t * c;
                for (int k = 0; k < 3; ++k) {
                    const double akp = a[k][p], akq = a[k][q];
                    a[k][p] = c * akp - s * akq;
                    a[k][q] = s * akp + c * akq;
                }
                for (int k = 0; k < 3; ++k) {
                    const double apk = a[p][k], aqk = a[q][k];
                    a[p][k] = c * apk - s * aqk;
                    a[q][k] = s * apk + c * aqk;
                }
                for (int k = 0; k < 3; ++k) {
                    const double vkp = v[k][p], vkq = v[k][q];
                    v[k][p] = c * vkp - s * vkq;
                    v[k][q] = s * vkp + c * vkq;
                }
            }
        }
    }
    std::array<std::pair<double, Vector3>, 3> out;
    for (int i = 0; i < 3; ++i) {
        out[i] = {a[i][i], normalized(Vector3{static_cast<float>(v[0][i]), static_cast<float>(v[1][i]),
                                               static_cast<float>(v[2][i])})};
    }
    std::sort(out.begin(), out.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    return out;
}

// The point nearest, by weighted least squares, the lines given as point, unit direction and weight; none when the
// lines are near parallel
std::optional<Vector3> nearest_to_lines(std::span<const std::tuple<Vector3, Vector3, float>> lines)
{
    // sum of w (I - n n^T) x = sum of w (I - n n^T) c
    std::array<std::array<double, 3>, 3> m{};
    std::array<double, 3> rhs{};
    for (const auto& [c, n, w] : lines) {
        const double nv[3] = {n.x, n.y, n.z};
        const double cv[3] = {c.x, c.y, c.z};
        const double nc = nv[0] * cv[0] + nv[1] * cv[1] + nv[2] * cv[2];
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                m[i][j] += w * ((i == j ? 1.0 : 0.0) - nv[i] * nv[j]);
            }
            rhs[i] += w * (cv[i] - nv[i] * nc);
        }
    }
    // solved in the eigenbasis
    const auto eigen = symmetric_eigen(m);
    if (!(eigen[0].first > eigen[2].first * 1e-3)) return std::nullopt;
    Vector3 x{0.0f, 0.0f, 0.0f};
    for (const auto& [value, vec] : eigen) {
        x = x + vec * static_cast<float>((rhs[0] * vec.x + rhs[1] * vec.y + rhs[2] * vec.z) / value);
    }
    return x;
}

// Snap Map's fit for Cylinder and Sphere: the gizmo where the selected faces' normals meet, its radius their mean
// distance from there, and a cylinder's axis the one the normals turn about, its seam away from the faces
void fit_map_gizmo(CDedLevel& level, TextureModePanel& panel, bool sphere)
{
    struct FitFace
    {
        std::vector<Vector3> corners;
        Vector3 centroid;
        Vector3 normal;
        float area;
    };
    std::vector<FitFace> faces;
    for (const SelectedFace& f : selected_faces(level)) {
        FitFace face{face_corners_world(f), face_centroid(f), {}, 0.0f};
        const Vector3 n = newell_normal(face.corners);
        face.area = length(n) * 0.5f;
        face.normal = normalized(n);
        if (face.area > 1e-6f) {
            faces.push_back(std::move(face));
        }
    }
    if (faces.size() < 2) {
        return;
    }
    Matrix3& orient = level.texture_map_orient;
    Vector3 map_axis = orient.uvec;
    if (!sphere) {
        // Sides' normals are square to the axis, the direction they have least of; caps face along it. Each main
        // direction of the normals is tried with the faces within 60 degrees of it left out as caps, each costing its
        // area times the sine squared of its angle to it; the try whose kept faces' least direction plus that cost is
        // smallest gives the axis.
        double area_sum = 0.0;
        for (const FitFace& f : faces) {
            area_sum += f.area;
        }
        auto normals_spread = [&](const Vector3* caps_along, double* caps_cost) {
            std::array<std::array<double, 3>, 3> m{};
            for (const FitFace& f : faces) {
                const double along = caps_along ? dot(f.normal, *caps_along) : 0.0;
                if (std::abs(along) > 0.5) {
                    *caps_cost += f.area * (1.0 - along * along);
                    continue;
                }
                const double nv[3] = {f.normal.x, f.normal.y, f.normal.z};
                for (int i = 0; i < 3; ++i) {
                    for (int j = 0; j < 3; ++j) {
                        m[i][j] += f.area * nv[i] * nv[j];
                    }
                }
            }
            return symmetric_eigen(m);
        };
        std::optional<Vector3> best;
        double best_cost = INFINITY;
        for (const auto& direction : normals_spread(nullptr, nullptr)) {
            double caps_cost = 0.0;
            const auto eigen = normals_spread(&direction.second, &caps_cost);
            const double total = eigen[0].first + eigen[1].first + eigen[2].first;
            // coplanar sides leave no axis
            if (!(eigen[1].first > total * 1e-3) || !(eigen[1].first > eigen[0].first * 4.0)) {
                continue;
            }
            const double cost = (eigen[0].first + caps_cost) / area_sum;
            if (cost < best_cost) {
                best_cost = cost;
                best = eigen[0].second;
            }
        }
        if (!best) {
            return;
        }
        map_axis = dot(*best, orient.uvec) < 0.0f ? *best * -1.0f : *best;
    }
    // a cylinder's normals with their part along the axis dropped, its end caps left out
    std::vector<std::tuple<Vector3, Vector3, float>> lines;
    for (const FitFace& f : faces) {
        const Vector3 n = sphere ? f.normal : f.normal - map_axis * dot(f.normal, map_axis);
        if (length(n) > 0.5f) {
            lines.emplace_back(f.centroid, normalized(n), f.area);
        }
    }
    const auto meet = nearest_to_lines(lines);
    if (!meet) {
        return;
    }
    const Vector3 center = *meet;
    float distance_sum = 0.0f, low = INFINITY, high = -INFINITY, area_sum = 0.0f;
    int corner_count = 0;
    Vector3 outward{0.0f, 0.0f, 0.0f};
    for (const FitFace& f : faces) {
        for (const Vector3& p : f.corners) {
            const Vector3 d = p - center;
            const float h = dot(d, map_axis);
            distance_sum += sphere ? length(d) : length(d - map_axis * h);
            low = std::min(low, h);
            high = std::max(high, h);
            ++corner_count;
        }
        const Vector3 d = f.centroid - center;
        outward = outward + (d - map_axis * dot(d, map_axis)) * f.area;
        area_sum += f.area;
    }
    const float radius = distance_sum / static_cast<float>(corner_count);
    // a cylinder's gizmo in the middle of the faces along its axis
    level.texture_map_pos = sphere ? center : center + map_axis * ((low + high) * 0.5f);
    if (!sphere) {
        // U starts at +rvec: across from the faces, so a part of a cylinder has its seam in the gap; a whole one keeps
        // the turn it had
        Vector3 r = length(outward) > 1e-3f * area_sum * radius ? outward * -1.0f : orient.rvec;
        r = normalized(r - map_axis * dot(r, map_axis));
        if (length(r) == 0.0f) {
            r = any_perpendicular(map_axis);
        }
        orient = Matrix3{r, map_axis, cross(r, map_axis)};
    }
    if (ResizeMapDialog* dialog = panel.resize_map) {
        if (dialog->radius) {
            dialog->radius->set(radius);
        }
        if (dialog->height && !sphere && high > low) {
            dialog->height->set(high - low);
        }
    }
}

// FUN_0042a4f0: Snap Map moves the gizmo to the middle of the selected faces' bounding box, which Cylinder and Sphere
// then fit to them
void __fastcall texture_snap_map_hooked(CDedLevel* level, int edx);
FunHook<decltype(texture_snap_map_hooked)> texture_snap_map_hook{0x0042A4F0, texture_snap_map_hooked};
void __fastcall texture_snap_map_hooked(CDedLevel* level, int edx)
{
    texture_snap_map_hook.call_target(level, edx);
    if (TextureModePanel* panel = texture_mode_panel();
        panel && (panel->map_type == TEXTURE_MAP_CYLINDER || panel->map_type == TEXTURE_MAP_SPHERE)) {
        fit_map_gizmo(*level, *panel, panel->map_type == TEXTURE_MAP_SPHERE);
    }
}

// The gizmo's axis (uvec) along world X, Y or Z
void set_map_axis(int index)
{
    CDedLevel* level = CDedLevel::Get();
    if (!level) return;
    static const Matrix3 orients[] = {
        {{0.0f, -1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
        identity_orient,
        {{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, -1.0f, 0.0f}},
    };
    level->texture_map_orient = orients[index];
    editor_views_mark_repaint_all();
}

// ─── Undo ───────────────────────────────────────────────────────────────────

// The flags Face Properties sets, which the level geometry faces copy
constexpr int face_properties_flags =
    FACE_SHOW_SKY | FACE_MIRRORED | FACE_SCROLL_TEXTURE | FACE_FULL_BRIGHT | FACE_LIGHTMAP_RESOLUTION;

struct FacePair
{
    GFace* live;
    GFace* old;
    std::vector<GFaceVertex*> live_corners;
    std::vector<GFaceVertex*> old_corners;
};

// The faces of two copies of a brush the same shape in the same place, which only their textures can tell apart;
// none for any other two
std::optional<std::vector<FacePair>> texture_only_pairs(const BrushNode& live, const BrushNode& old)
{
    auto* live_solid = static_cast<GSolid*>(live.geometry);
    auto* old_solid = static_cast<GSolid*>(old.geometry);
    const Matrix3& a = live.orient;
    const Matrix3& b = old.orient;
    if (!live_solid || !old_solid || !(live.pos == old.pos) || !(a.rvec == b.rvec) || !(a.uvec == b.uvec) ||
        !(a.fvec == b.fvec) || live_solid->face_list_count != old_solid->face_list_count) {
        return std::nullopt;
    }
    std::vector<FacePair> pairs;
    GFace* old_face = old_solid->face_list_head;
    const bool differ = live_solid->for_each_face([&](GFace& face) {
        if (!old_face || face.face_id != old_face->face_id) return true;
        FacePair pair{&face, old_face, face_corner_list(face), face_corner_list(*old_face)};
        old_face = old_face->next_solid;
        if (pair.live_corners.size() != pair.old_corners.size()) return true;
        for (size_t i = 0; i < pair.live_corners.size(); ++i) {
            const GVertex* p = pair.live_corners[i]->vertex;
            const GVertex* q = pair.old_corners[i]->vertex;
            if (!p || !q || !(p->pos == q->pos)) return true;
        }
        pairs.push_back(std::move(pair));
        return false;
    });
    if (differ || old_face) {
        return std::nullopt;
    }
    return pairs;
}

// The points for_each_point visits lie on the brush face: on its plane, within its bounds
template<typename ForEachPoint>
bool points_lie_on(const SelectedFace& f, ForEachPoint&& for_each_point)
{
    const std::vector<Vector3> corners = face_corners_world(f);
    const Vector3 n = normalized(newell_normal(corners));
    if (corners.size() < 3 || length(n) == 0.0f) {
        return false;
    }
    constexpr float eps = 0.01f;
    Vector3 low = corners.front(), high = corners.front();
    for (const Vector3& p : corners) {
        low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
        high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
    }
    const float d = dot(n, corners.front());
    bool on_face = true;
    for_each_point([&](const Vector3& p) {
        on_face = on_face && std::abs(dot(n, p) - d) <= eps && p.x >= low.x - eps && p.y >= low.y - eps &&
                  p.z >= low.z - eps && p.x <= high.x + eps && p.y <= high.y + eps && p.z <= high.z + eps;
    });
    return on_face;
}

// The level geometry faces given (those with the brush face's id) lie on the brush face, so none was built from
// another face since, as a Build numbering faces again could have
bool built_from(const SelectedFace& f, std::span<GFace* const> built)
{
    return points_lie_on(f, [&](auto&& visit) {
        for (const GFace* face : built) {
            for_each_face_corner(*face, [&](GFaceVertex& fv) {
                if (fv.vertex) {
                    visit(fv.vertex->pos);
                }
            });
        }
    });
}

// ─── Hover readout ──────────────────────────────────────────────────────────

constexpr DWORD hover_refresh_ms = 150;

struct TextureHover
{
    POINT cursor{};
    DWORD refreshed = 0;
    int face_id = -1;
    // the moving group brush the face was hit on, else -1 for the level geometry
    int mover_uid = -1;
    // pane 0's text before the readout took it, while it is shown, and the readout last shown
    std::optional<std::string> saved_status;
    std::string readout;
};
TextureHover g_texture_hover;

struct FacePick
{
    GFace* face = nullptr;
    BrushNode* mover = nullptr;
};

// Texture mode's click pick (0x0042C020) without its selection change: moving group brushes are hit as brushes, the
// rest on the level geometry, and a face of a hidden brush is no hit. The view's gr must be set up.
FacePick texture_mode_pick(CDedLevel& level, float x, float y)
{
    const EditorRay ray = editor_screen_ray(x, y);
    SolidRayQuery query;
    query.p0 = ray.o;
    query.dir = ray.d * 50000.0f;
    query.radius = 0.0f;
    query.flags = 0;
    SolidRayResult result;
    FacePick mover_hit;
    float mover_t = 0.0f;
    level.for_each_brush([&](BrushNode& brush) {
        auto* geometry = static_cast<GSolid*>(brush.geometry);
        if (!geometry || !brush.pickable() || !level.brush_in_moving_group(&brush)) {
            return;
        }
        query.pos = brush.pos;
        query.orient = brush.orient;
        if (geometry->collide(query, result) && (!mover_hit.face || result.t < mover_t)) {
            mover_hit = {result.face, &brush};
            mover_t = result.t;
        }
    });
    query.pos = {0.0f, 0.0f, 0.0f};
    query.orient = identity_orient;
    query.flags = SRQ_LEVEL_SOLID;
    const bool level_hit = level.solid->collide(query, result);
    if (mover_hit.face && (!level_hit || mover_t < result.t)) {
        return mover_hit;
    }
    if (!level_hit || !result.face) {
        return {};
    }
    const int face_id = result.face->face_id;
    const bool hidden = level.for_each_brush([&](BrushNode& brush) {
        auto* geometry = static_cast<GSolid*>(brush.geometry);
        return geometry && brush.state == BRUSH_STATE_HIDDEN &&
               geometry->for_each_face([&](GFace& face) { return face.face_id == face_id; });
    });
    return hidden ? FacePick{} : FacePick{result.face, nullptr};
}

// "Face 62: wall01.tga (256 x 128), 128 px/m", with U and V densities apart when they differ
std::string face_readout(const char* prefix, const SelectedFace& f)
{
    std::string text = std::format("{} {}", prefix, f.face->face_id);
    if (f.face->bitmap_id < 0) {
        return text;
    }
    int width = 0, height = 0;
    bm_get_dimensions(f.face->bitmap_id, &width, &height);
    if (width <= 0 || height <= 0) {
        return text;
    }
    text += std::format(": {} ({} x {})", bm_get_filename(f.face->bitmap_id), width, height);
    if (const auto map = face_uv_map(f)) {
        const float ppm_u = static_cast<float>(width) * length(map->grad_u);
        const float ppm_v = static_cast<float>(height) * length(map->grad_v);
        if (std::abs(ppm_u - ppm_v) <= 0.01f * std::max(ppm_u, ppm_v)) {
            text += std::format(", {:.4g} px/m", ppm_u);
        }
        else {
            text += std::format(", U {:.4g} / V {:.4g} px/m", ppm_u, ppm_v);
        }
    }
    return text;
}

void show_readout(const std::string& text)
{
    const HWND bar = editor_status_bar();
    if (!bar) return;
    char shown[512] = {};
    GetWindowTextA(bar, shown, sizeof(shown));
    // RED's own text, also when it set some while the readout was up
    if (!g_texture_hover.saved_status || g_texture_hover.readout != shown) {
        g_texture_hover.saved_status = shown;
    }
    if (text != shown) {
        SetWindowTextA(bar, text.c_str());
    }
    g_texture_hover.readout = text;
}

void hide_readout()
{
    if (!g_texture_hover.saved_status) {
        return;
    }
    if (const HWND bar = editor_status_bar()) {
        char shown[512] = {};
        GetWindowTextA(bar, shown, sizeof(shown));
        if (g_texture_hover.readout == shown) {
            SetWindowTextA(bar, g_texture_hover.saved_status->c_str());
        }
    }
    g_texture_hover.saved_status.reset();
}

void set_hovered_face(int face_id, int mover_uid)
{
    if (face_id == g_texture_hover.face_id && mover_uid == g_texture_hover.mover_uid) return;
    g_texture_hover.face_id = face_id;
    g_texture_hover.mover_uid = mover_uid;
    editor_views_mark_repaint_all();
}

constexpr uint8_t outline_occluded_alpha = 70;

void draw_face_outline(const SelectedFace& f, uint8_t alpha, uint32_t mode)
{
    const COLORREF color = g_editor_settings.texture_hover_outline_color;
    set_draw_color(GetRValue(color), GetGValue(color), GetBValue(color), alpha);
    for_each_corner(f.face, [&](GFaceVertex& fv) {
        const Vector3 a = corner_world(f, fv);
        const Vector3 b = corner_world(f, *fv.next);
        gr_line_3d(&a, &b, mode);
    });
    editor_gr_flush();
}

constexpr DialogTooltip texture_panel_tips[] = {
    {IDC_TEXTURE_WRAP, "Continue the last clicked face's texture across shared edges onto the selected faces: select "
                       "the faces, Ctrl+click the face to wrap from, then Wrap"},
    {IDC_TEXTURE_FIT, "Map the selected faces onto the tiles below, stretching to fill them"},
    {IDC_TEXTURE_FIT_ASPECT, "Map the selected faces onto the tiles below, keeping the texture's proportions"},
    {IDC_TEXTURE_FIT_ROTATION, "Turn the texture clockwise when fitting"},
    {IDC_TEXTURE_EACH_FACE, "Fit and Justify each selected face on its own instead of as one group"},
    {IDC_TEXTURE_JUSTIFY_LEFT, "Justify left: move the texture so a tile edge meets the faces' left edge"},
    {IDC_TEXTURE_JUSTIFY_RIGHT, "Justify right: move the texture so a tile edge meets the faces' right edge"},
    {IDC_TEXTURE_JUSTIFY_TOP, "Justify top: move the texture so a tile edge meets the faces' top edge"},
    {IDC_TEXTURE_JUSTIFY_BOTTOM, "Justify bottom: move the texture so a tile edge meets the faces' bottom edge"},
    {IDC_TEXTURE_JUSTIFY_CENTER, "Justify center: move the texture so a tile's center meets the faces' center"},
    {IDC_TEXTURE_SNAP_MAP, "Move the map gizmo onto the selected faces; for Cylinder and Sphere also fit its axis "
                           "and radius to them"},
    {IDC_TEXTURE_MAP_AXIS_X, "Point the map gizmo's axis along X"},
    {IDC_TEXTURE_MAP_AXIS_Y, "Point the map gizmo's axis along Y"},
    {IDC_TEXTURE_MAP_AXIS_Z, "Point the map gizmo's axis along Z"},
    {IDC_TEXTURE_MAP_WRAPS, "Cylinder and Sphere: times the texture goes around, with square texels. 0 keeps the "
                            "texture's own scale at the Resize Map radius"},
    {IDC_TEXTURE_PASTE_TEXTURE, "Paste (Ctrl+V) also applies the copied face's texture"},
};

LRESULT CALLBACK texture_panel_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR subclass_id,
                                    DWORD_PTR)
{
    if (msg == WM_COMMAND && HIWORD(wparam) == BN_CLICKED) {
        const int id = LOWORD(wparam);
        if (id == IDC_TEXTURE_EACH_FACE) {
            g_edit_each_face = IsDlgButtonChecked(hwnd, IDC_TEXTURE_EACH_FACE) == BST_CHECKED;
            return 0;
        }
        if (id == IDC_TEXTURE_FIT || id == IDC_TEXTURE_FIT_ASPECT) {
            // A field left blank, zero or unreadable fits one tile
            auto tiles = [hwnd](int idc) {
                const float v = alpine_dlg_get_float_field(hwnd, idc);
                return std::isfinite(v) && v > 0.0f ? v : 1.0f;
            };
            g_fit_tiles_u = tiles(IDC_TEXTURE_FIT_U);
            g_fit_tiles_v = tiles(IDC_TEXTURE_FIT_V);
            const bool keep_aspect = id == IDC_TEXTURE_FIT_ASPECT;
            edit_selected_faces("You must select a face to fit.", g_edit_each_face,
                                [&](std::span<const SelectedFace> faces) {
                                    fit_faces(faces, keep_aspect, g_fit_tiles_u, g_fit_tiles_v, g_fit_quarter_turns);
                                });
            return 0;
        }
        if (id >= IDC_TEXTURE_JUSTIFY_LEFT && id <= IDC_TEXTURE_JUSTIFY_CENTER) {
            const auto justify = static_cast<Justify>(id - IDC_TEXTURE_JUSTIFY_LEFT);
            edit_selected_faces("You must select a face to justify.", g_edit_each_face,
                                [&](std::span<const SelectedFace> faces) { justify_faces(faces, justify); });
            return 0;
        }
        if (id == IDC_TEXTURE_WRAP) {
            wrap_selected_faces();
            return 0;
        }
        if (id >= IDC_TEXTURE_MAP_AXIS_X && id <= IDC_TEXTURE_MAP_AXIS_Z) {
            set_map_axis(id - IDC_TEXTURE_MAP_AXIS_X);
            return 0;
        }
        if (id == IDC_TEXTURE_PASTE_TEXTURE) {
            g_paste_texture = IsDlgButtonChecked(hwnd, IDC_TEXTURE_PASTE_TEXTURE) == BST_CHECKED;
            return 0;
        }
    }
    if (msg == WM_COMMAND && HIWORD(wparam) == CBN_SELCHANGE && LOWORD(wparam) == IDC_TEXTURE_FIT_ROTATION) {
        const LRESULT sel = SendDlgItemMessageA(hwnd, IDC_TEXTURE_FIT_ROTATION, CB_GETCURSEL, 0, 0);
        g_fit_quarter_turns = sel >= 0 && sel < 4 ? static_cast<int>(sel) : 0;
    }
    if (msg == WM_COMMAND && HIWORD(wparam) == EN_CHANGE && LOWORD(wparam) == IDC_TEXTURE_MAP_WRAPS) {
        // whole turns; blank, negative or unreadable is stock's scale
        const float wraps = alpine_dlg_get_float_field(hwnd, IDC_TEXTURE_MAP_WRAPS);
        g_map_wraps = std::isfinite(wraps) && wraps > 0.0f ? std::min(std::round(wraps), 1000.0f) : 0.0f;
    }
    if (msg == WM_NOTIFY && alpine_spinner_handle_notify(hwnd, lparam)) {
        return TRUE;
    }
    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, texture_panel_proc, subclass_id);
    }
    return DefSubclassProc(hwnd, msg, wparam, lparam);
}

// ─── UV Unwrap rotation ─────────────────────────────────────────────────────

// The window's options, kept for the session
bool g_uv_rotate_keeps_aspect = true;
bool g_uv_face_colors = false;

// FUN_00472ff0 applies a move, rotate or scale step to the selected vertices: `delta` is the mouse movement, or the
// arrow key's direction in steps of the chosen increment. Stock rotates in UV space, which shears a texture that is
// not square; this rotates in texels instead.
void __fastcall uv_unwrap_transform_step_hooked(CWnd* dialog, int edx, const float* delta, char by_mouse);
FunHook<decltype(uv_unwrap_transform_step_hooked)> uv_unwrap_transform_step_hook{0x00472FF0,
                                                                                 uv_unwrap_transform_step_hooked};
void __fastcall uv_unwrap_transform_step_hooked(CWnd* dialog, int edx, const float* delta, char by_mouse)
{
    const float texel_u = uv_unwrap_texel_u;
    const float texel_v = uv_unwrap_texel_v;
    const bool texels_valid = std::isfinite(texel_u) && std::isfinite(texel_v) && texel_u > 0.0f && texel_v > 0.0f;
    if (uv_unwrap_drag_mode != UV_UNWRAP_DRAG_ROTATE || !g_uv_rotate_keeps_aspect || !texels_valid) {
        uv_unwrap_transform_step_hook.call_target(dialog, edx, delta, by_mouse);
        return;
    }
    const float degrees = by_mouse ? delta[0] : uv_unwrap_rotate_steps[uv_unwrap_rotate_step_index] * delta[0];
    const float angle = -degrees * (std::numbers::pi_v<float> / 180.0f);
    const float s = std::sin(angle);
    const float c = std::cos(angle);
    // u' = c*du + s*dv and v' = c*dv - s*du as stock, with du and dv in texels
    const float v_to_u = texel_u / texel_v;
    const float u_to_v = texel_v / texel_u;
    for (int i = 0; i < uv_unwrap_vertices.size; ++i) {
        const UvUnwrapVertex* vert = uv_unwrap_vertices.data_ptr[i];
        if (!vert || !vert->selected || !vert->face_vertex) {
            continue;
        }
        GFaceVertex& fv = *vert->face_vertex;
        const float du = fv.u - uv_unwrap_pivot_u;
        const float dv = fv.v - uv_unwrap_pivot_v;
        fv.u = c * du + s * dv * v_to_u + uv_unwrap_pivot_u;
        fv.v = c * dv - s * du * u_to_v + uv_unwrap_pivot_v;
    }
    PostMessageA(WndToHandle(dialog), WM_PAINT, 0, 0);
}

// ─── UV Unwrap tools ────────────────────────────────────────────────────────

UvUnwrapDialog* uv_unwrap_dialog()
{
    CDedLevel* level = CDedLevel::Get();
    return level ? static_cast<UvUnwrapDialog*>(level->dialog_panels[uv_unwrap_dialog_panel_index]) : nullptr;
}

// Not mid move, rotate or scale, and in Texture UVs: a selection change during a transform would break the undo
// step it records by selection order
bool uv_unwrap_idle()
{
    const int mode = uv_unwrap_drag_mode;
    return (mode == UV_UNWRAP_DRAG_SELECT || mode == UV_UNWRAP_DRAG_SELECT_ADD ||
            mode == UV_UNWRAP_DRAG_SELECT_REMOVE) &&
           !uv_unwrap_transforming && uv_unwrap_decal_mode == 0;
}

void uv_unwrap_select_all()
{
    for (int i = 0; i < uv_unwrap_vertices.size; ++i) {
        UvUnwrapVertex* vert = uv_unwrap_vertices.data_ptr[i];
        if (vert && !vert->selected) {
            uv_unwrap_select(vert);
        }
    }
}

// As stock's Flip ends (the level geometry updated, the step recorded, a redraw), and the view framed on the tile
// the corners were moved into
void uv_unwrap_finish_edit(UvUnwrapDialog* dialog, HWND hwnd)
{
    uv_unwrap_commit(dialog);
    uv_unwrap_record_undo(dialog);
    uv_unwrap_view_center_u = 0.5f;
    uv_unwrap_view_center_v = 0.5f;
    uv_unwrap_view_extent = 1.8f;
    uv_unwrap_layout(dialog);
    PostMessageA(hwnd, WM_PAINT, 0, 0);
}

// The selected corners (all of them when none is) scaled alike and centered to fill the tile
void uv_unwrap_fit(UvUnwrapDialog* dialog, HWND hwnd)
{
    const auto& verts = uv_unwrap_vertices;
    const bool any_selected = std::any_of(verts.data_ptr, verts.data_ptr + verts.size,
                                          [](const UvUnwrapVertex* v) { return v && v->selected; });
    if (!any_selected) {
        uv_unwrap_select_all();
    }
    float min_u = INFINITY, max_u = -INFINITY, min_v = INFINITY, max_v = -INFINITY;
    for (int i = 0; i < uv_unwrap_vertices.size; ++i) {
        const UvUnwrapVertex* vert = uv_unwrap_vertices.data_ptr[i];
        if (vert && vert->selected && vert->face_vertex) {
            min_u = std::min(min_u, vert->face_vertex->u);
            max_u = std::max(max_u, vert->face_vertex->u);
            min_v = std::min(min_v, vert->face_vertex->v);
            max_v = std::max(max_v, vert->face_vertex->v);
        }
    }
    const float du = max_u - min_u;
    const float dv = max_v - min_v;
    // UV scaled alike keeps texels square
    const float k = std::min(du > 1e-6f ? 1.0f / du : INFINITY, dv > 1e-6f ? 1.0f / dv : INFINITY);
    if (!std::isfinite(k)) {
        PostMessageA(hwnd, WM_PAINT, 0, 0);
        return;
    }
    uv_unwrap_push_undo(dialog);
    for (int i = 0; i < uv_unwrap_vertices.size; ++i) {
        const UvUnwrapVertex* vert = uv_unwrap_vertices.data_ptr[i];
        if (vert && vert->selected && vert->face_vertex) {
            GFaceVertex& fv = *vert->face_vertex;
            fv.u = (fv.u - min_u) * k + (1.0f - du * k) * 0.5f;
            fv.v = (fv.v - min_v) * k + (1.0f - dv * k) * 0.5f;
        }
    }
    uv_unwrap_finish_edit(dialog, hwnd);
}

// Every face projected along its own normal's main axis, as seen from the front, and the pieces packed into the tile
// in shelves without overlapping, at one texel density.
void uv_unwrap_auto(UvUnwrapDialog* dialog, HWND hwnd)
{
    struct Island
    {
        int first;
        int count;
        std::vector<std::pair<float, float>> points; // world meters, x right and y down
        float width = 0.0f, height = 0.0f;
        float x = 0.0f, y = 0.0f;                     // packed position
    };
    std::vector<Island> islands;
    for (int i = 0; i < uv_unwrap_vertices.size;) {
        const UvUnwrapVertex* first = uv_unwrap_vertices.data_ptr[i];
        int count = 1;
        while (i + count < uv_unwrap_vertices.size && first && uv_unwrap_vertices.data_ptr[i + count] &&
               uv_unwrap_vertices.data_ptr[i + count]->face == first->face) {
            ++count;
        }
        const bool whole = std::all_of(uv_unwrap_vertices.data_ptr + i, uv_unwrap_vertices.data_ptr + i + count,
                                       [](const UvUnwrapVertex* v) {
                                           return v && v->face_vertex && v->face_vertex->vertex;
                                       });
        if (whole && first->brush && first->face) {
            const SelectedFace f{first->brush, first->face};
            const Vector3 n = face_normal(f);
            Island island{i, count, {}};
            for (int c = 0; c < count; ++c) {
                const GFaceVertex& fv = *uv_unwrap_vertices.data_ptr[i + c]->face_vertex;
                island.points.push_back(box_map_point(n, corner_world(f, fv)));
            }
            float min_x = INFINITY, min_y = INFINITY, max_x = -INFINITY, max_y = -INFINITY;
            for (const auto& [x, y] : island.points) {
                min_x = std::min(min_x, x);
                min_y = std::min(min_y, y);
                max_x = std::max(max_x, x);
                max_y = std::max(max_y, y);
            }
            for (auto& [x, y] : island.points) {
                x -= min_x;
                y -= min_y;
            }
            island.width = max_x - min_x;
            island.height = max_y - min_y;
            islands.push_back(std::move(island));
        }
        i += count;
    }
    const float texels_u = 1.0f / uv_unwrap_texel_u;
    const float texels_v = 1.0f / uv_unwrap_texel_v;
    if (islands.empty() || !std::isfinite(texels_u) || !std::isfinite(texels_v) || texels_u <= 0.0f ||
        texels_v <= 0.0f) {
        return;
    }

    // Shelves tallest first, a shelf as wide as gives the texture's proportions; packed twice so the 2 texel gaps
    // are in the density they end up at
    std::vector<size_t> order(islands.size());
    for (size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return islands[a].height > islands[b].height; });
    auto pack = [&](float gap, float& packed_w, float& packed_h) {
        float area = 0.0f, widest = 0.0f;
        for (const Island& island : islands) {
            area += (island.width + gap) * (island.height + gap);
            widest = std::max(widest, island.width + gap);
        }
        const float shelf_width = std::max(widest, std::sqrt(area * texels_u / texels_v));
        float x = 0.0f, y = 0.0f, shelf_height = 0.0f;
        packed_w = packed_h = 0.0f;
        for (size_t index : order) {
            Island& island = islands[index];
            if (x > 0.0f && x + island.width > shelf_width) {
                x = 0.0f;
                y += shelf_height + gap;
                shelf_height = 0.0f;
            }
            island.x = x;
            island.y = y;
            x += island.width + gap;
            shelf_height = std::max(shelf_height, island.height);
            packed_w = std::max(packed_w, island.x + island.width);
            packed_h = std::max(packed_h, y + shelf_height);
        }
    };
    float packed_w = 0.0f, packed_h = 0.0f;
    pack(0.0f, packed_w, packed_h);
    // texels per meter that fit the packing in the tile
    auto density = [&] {
        return std::min(packed_w > 1e-6f ? texels_u / packed_w : INFINITY,
                        packed_h > 1e-6f ? texels_v / packed_h : INFINITY);
    };
    float s = density();
    if (!std::isfinite(s)) {
        return;
    }
    pack(2.0f / s, packed_w, packed_h);
    s = density();
    if (!std::isfinite(s)) {
        return;
    }

    uv_unwrap_select_all();
    uv_unwrap_push_undo(dialog);
    const float off_u = (1.0f - packed_w * s / texels_u) * 0.5f;
    const float off_v = (1.0f - packed_h * s / texels_v) * 0.5f;
    for (const Island& island : islands) {
        for (int c = 0; c < island.count; ++c) {
            GFaceVertex* fv = uv_unwrap_vertices.data_ptr[island.first + c]->face_vertex;
            const auto& [x, y] = island.points[c];
            fv->u = (island.x + x) * s / texels_u + off_u;
            fv->v = (island.y + y) * s / texels_v + off_v;
        }
    }
    uv_unwrap_finish_edit(dialog, hwnd);
}

// FUN_00475370 draws the corners of one selection state: a square marker (red when selected) and the edge to the
// next corner, both in the line color. With Color each face checked, each face of several gets its own color.
CallHook<void __cdecl(int selected_pass)> uv_unwrap_draw_corners_hook{
    {0x004750E6, 0x004750ED},
    [](int selected_pass) {
        if (!g_uv_face_colors) {
            uv_unwrap_draw_corners_hook.call_target(selected_pass);
            return;
        }
        static constexpr uint8_t face_colors[][3] = {
            {0, 255, 255}, {255, 255, 0}, {0, 255, 0}, {255, 128, 0},
            {255, 0, 255}, {128, 160, 255}, {255, 255, 255}, {160, 255, 160},
        };
        const UvUnwrapDialog* dialog = uv_unwrap_dialog();
        if (!dialog) return;
        const auto& verts = uv_unwrap_vertices;
        const UvUnwrapVertex* first = verts.size > 0 ? verts.data_ptr[0] : nullptr;
        const bool several = first && std::any_of(verts.data_ptr, verts.data_ptr + verts.size,
                                                  [&](const UvUnwrapVertex* v) { return v && v->face != first->face; });
        constexpr float marker = 2.0f;
        const uint32_t mode = gr_line_2d_mode;
        auto screen = [](const GFaceVertex& fv) {
            return std::pair{(fv.u - uv_unwrap_view_u0) / uv_unwrap_view_du,
                             (fv.v - uv_unwrap_view_v0) / uv_unwrap_view_dv};
        };
        int face_index = -1;
        const GFace* last_face = nullptr;
        for (int i = 0; i < verts.size; ++i) {
            const UvUnwrapVertex* vert = verts.data_ptr[i];
            if (!vert || !vert->face_vertex) continue;
            if (vert->face != last_face) {
                last_face = vert->face;
                ++face_index;
            }
            if (vert->selected != (selected_pass != 0)) continue;
            const uint8_t* color = several ? face_colors[face_index % std::size(face_colors)] : dialog->line_color;
            const auto [x, y] = screen(*vert->face_vertex);
            if (vert->selected) {
                set_draw_color(255, 0, 0, 255);
            }
            else {
                set_draw_color(color[0], color[1], color[2], 255);
            }
            gr_line_2d(x - marker, y - marker, x + marker, y - marker, mode);
            gr_line_2d(x + marker, y - marker, x + marker, y + marker, mode);
            gr_line_2d(x + marker, y + marker, x - marker, y + marker, mode);
            gr_line_2d(x - marker, y + marker, x - marker, y - marker, mode);
            if (const GFaceVertex* next = vert->face_vertex->next) {
                set_draw_color(color[0], color[1], color[2], 255);
                const auto [nx, ny] = screen(*next);
                gr_line_2d(x, y, nx, ny, mode);
            }
        }
    },
};

// A button released after a press outside the view (which leaves the drag flag clear) selected with a stale anchor
CodeInjection uv_unwrap_lbutton_up_stale_select_fix{
    0x004742F6,
    [](auto& regs) {
        if (!uv_unwrap_dragging) {
            regs.eip = 0x00474310;
        }
    },
};
CodeInjection uv_unwrap_rbutton_up_stale_select_fix{
    0x00474416,
    [](auto& regs) {
        if (!uv_unwrap_dragging) {
            regs.eip = 0x00474430;
        }
    },
};

LRESULT CALLBACK uv_unwrap_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR subclass_id, DWORD_PTR)
{
    if (msg == WM_COMMAND && LOWORD(wparam) == IDC_UV_ROTATE_KEEP_ASPECT && HIWORD(wparam) == BN_CLICKED) {
        g_uv_rotate_keeps_aspect = IsDlgButtonChecked(hwnd, IDC_UV_ROTATE_KEEP_ASPECT) == BST_CHECKED;
    }
    if (msg == WM_COMMAND && LOWORD(wparam) == IDC_UV_FACE_COLORS && HIWORD(wparam) == BN_CLICKED) {
        g_uv_face_colors = IsDlgButtonChecked(hwnd, IDC_UV_FACE_COLORS) == BST_CHECKED;
        PostMessageA(hwnd, WM_PAINT, 0, 0);
    }
    if (msg == WM_COMMAND && HIWORD(wparam) == BN_CLICKED &&
        (LOWORD(wparam) == IDC_UV_SELECT_ALL || LOWORD(wparam) == IDC_UV_FIT || LOWORD(wparam) == IDC_UV_AUTO_UNWRAP)) {
        UvUnwrapDialog* dialog = uv_unwrap_dialog();
        if (dialog && uv_unwrap_idle()) {
            if (LOWORD(wparam) == IDC_UV_SELECT_ALL) {
                uv_unwrap_select_all();
                PostMessageA(hwnd, WM_PAINT, 0, 0);
            }
            else if (LOWORD(wparam) == IDC_UV_FIT) {
                uv_unwrap_fit(dialog, hwnd);
            }
            else {
                uv_unwrap_auto(dialog, hwnd);
            }
        }
        return 0;
    }
    // Stock has no Ctrl+A; its key bookkeeping runs first
    if (msg == WM_KEYDOWN && wparam == 'A' && GetKeyState(VK_CONTROL) < 0) {
        const LRESULT result = DefSubclassProc(hwnd, msg, wparam, lparam);
        if (uv_unwrap_dialog() && uv_unwrap_idle()) {
            uv_unwrap_select_all();
            PostMessageA(hwnd, WM_PAINT, 0, 0);
        }
        return result;
    }
    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, uv_unwrap_proc, subclass_id);
    }
    return DefSubclassProc(hwnd, msg, wparam, lparam);
}

HHOOK g_uv_unwrap_msg_hook = nullptr;

LRESULT CALLBACK uv_unwrap_msg_hook_proc(int code, WPARAM wparam, LPARAM lparam)
{
    if (code == HC_ACTION) {
        const auto* msg = reinterpret_cast<const CWPRETSTRUCT*>(lparam);
        if (msg->message == WM_INITDIALOG && GetDlgItem(msg->hwnd, IDC_UV_ROTATE_KEEP_ASPECT)) {
            CheckDlgButton(msg->hwnd, IDC_UV_ROTATE_KEEP_ASPECT,
                           g_uv_rotate_keeps_aspect ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(msg->hwnd, IDC_UV_FACE_COLORS, g_uv_face_colors ? BST_CHECKED : BST_UNCHECKED);
            SetWindowSubclass(msg->hwnd, uv_unwrap_proc, 1, 0);
            HHOOK hook = g_uv_unwrap_msg_hook;
            g_uv_unwrap_msg_hook = nullptr;
            UnhookWindowsHookEx(hook);
            return CallNextHookEx(nullptr, code, wparam, lparam);
        }
    }
    return CallNextHookEx(g_uv_unwrap_msg_hook, code, wparam, lparam);
}

void hover_tick()
{
    CDedLevel* level = CDedLevel::Get();
    if (!level || level->edit_mode != DedEditMode::Texture || !level->solid) {
        set_hovered_face(-1, -1);
        hide_readout();
        return;
    }
    POINT cursor{};
    if (!GetCursorPos(&cursor)) return;
    const DWORD now = GetTickCount();
    if (cursor.x == g_texture_hover.cursor.x && cursor.y == g_texture_hover.cursor.y &&
        now - g_texture_hover.refreshed < hover_refresh_ms) {
        return;
    }
    g_texture_hover.cursor = cursor;
    g_texture_hover.refreshed = now;

    FacePick pick;
    auto* view = static_cast<EditorViewport*>(editor_view_under_cursor(cursor));
    const HWND main = GetMainFrameHandle();
    POINT client = cursor;
    if (view && main && IsWindowEnabled(main) && GetForegroundWindow() == main && !view->lbutton_held &&
        !view->rbutton_held && !view->mbutton_held && !editor_transform_key_held() && !level->transform_in_progress &&
        ScreenToClient(editor_view_hwnd(view), &client)) {
        view->setup_gr(0);
        pick = texture_mode_pick(*level, static_cast<float>(client.x), static_cast<float>(client.y));
    }
    if (pick.face) {
        set_hovered_face(pick.face->face_id, pick.mover ? pick.mover->uid : -1);
        show_readout(face_readout("Face", {pick.mover, pick.face}));
        return;
    }
    set_hovered_face(-1, -1);
    const std::vector<SelectedFace> selected = selected_faces(*level);
    if (selected.empty()) {
        hide_readout();
        return;
    }
    show_readout(face_readout("Selected face", selected.front()));
}

} // namespace

void texture_mode_idle()
{
    try {
        hover_tick();
    }
    catch (const std::exception& e) {
        xlog::error("[Texture] hover: {}", e.what());
    }
}

void texture_mode_render_view()
{
    CDedLevel* level = CDedLevel::Get();
    const auto* view = static_cast<EditorViewport*>(editor_view_at(painting_view_index));
    const int face_id = g_texture_hover.face_id;
    if (!g_editor_settings.texture_hover_outline || !level || !view || level->edit_mode != DedEditMode::Texture
        || !level->solid || face_id < 0) {
        return;
    }
    editor_gr_flush();
    // In the 3D view the parts behind other geometry show dimmed
    auto draw = [&](const SelectedFace& f) {
        if (view->view_type == editor_view_type_perspective) {
            draw_face_outline(f, outline_occluded_alpha, mode_overlay);
            draw_face_outline(f, 255, mode_vertex_alpha);
        }
        else {
            draw_face_outline(f, 255, mode_overlay);
        }
    };
    if (g_texture_hover.mover_uid >= 0) {
        if (const auto face = find_brush_face(*level, face_id, g_texture_hover.mover_uid)) {
            draw(*face);
        }
        return;
    }
    level->solid->for_each_face([&](GFace& face) {
        if (face.face_id == face_id) {
            draw({nullptr, &face});
        }
        return false;
    });
}

void texture_mode_panel_created(HWND hwnd, TextureMapType map_type)
{
    alpine_dlg_set_float_field(hwnd, IDC_TEXTURE_FIT_U, g_fit_tiles_u);
    alpine_dlg_set_float_field(hwnd, IDC_TEXTURE_FIT_V, g_fit_tiles_v);
    alpine_spinner_init(hwnd, IDC_TEXTURE_FIT_U, IDC_TEXTURE_FIT_U_SPIN, 1.0f, 0.01f, 1000.0f, 2);
    alpine_spinner_init(hwnd, IDC_TEXTURE_FIT_V, IDC_TEXTURE_FIT_V_SPIN, 1.0f, 0.01f, 1000.0f, 2);
    alpine_dlg_set_float_field(hwnd, IDC_TEXTURE_MAP_WRAPS, g_map_wraps);
    alpine_spinner_init_int(hwnd, IDC_TEXTURE_MAP_WRAPS, IDC_TEXTURE_MAP_WRAPS_SPIN, 1, 0, 1000);
    for (const char* turn : {"0\xB0", "90\xB0", "180\xB0", "270\xB0"}) {
        SendDlgItemMessageA(hwnd, IDC_TEXTURE_FIT_ROTATION, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(turn));
    }
    SendDlgItemMessageA(hwnd, IDC_TEXTURE_FIT_ROTATION, CB_SETCURSEL, g_fit_quarter_turns, 0);
    CheckDlgButton(hwnd, IDC_TEXTURE_EACH_FACE, g_edit_each_face ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hwnd, IDC_TEXTURE_PASTE_TEXTURE, g_paste_texture ? BST_CHECKED : BST_UNCHECKED);
    // after the stock three, which OnInitDialog adds before it selects the panel's map type
    SendDlgItemMessageA(hwnd, IDC_TEXTURE_MAP_TYPE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Sphere"));
    if (map_type == TEXTURE_MAP_SPHERE) {
        SendDlgItemMessageA(hwnd, IDC_TEXTURE_MAP_TYPE, CB_SETCURSEL, TEXTURE_MAP_SPHERE, 0);
    }
    alpine_dlg_add_tooltips(hwnd, texture_panel_tips);
    SetWindowSubclass(hwnd, texture_panel_proc, 1, 0);
}

void uv_unwrap_dialog_opening()
{
    if (!g_uv_unwrap_msg_hook) {
        g_uv_unwrap_msg_hook = SetWindowsHookExA(WH_CALLWNDPROCRET, uv_unwrap_msg_hook_proc, nullptr,
                                                 GetCurrentThreadId());
    }
}

bool level_faces_lie_on_brush_face(BrushNode& brush, GFace& face, std::span<GFace* const> built)
{
    return built_from({&brush, &face}, built);
}

bool level_faces_lie_on_brush_face(BrushNode& brush, GFace& face, std::span<const Vector3> built_corners)
{
    return points_lie_on({&brush, &face}, [&](auto&& visit) {
        for (const Vector3& p : built_corners) {
            visit(p);
        }
    });
}

void texture_mode_level_reset()
{
    // a handle of the level it was copied in; the mapping, like stock's UVs, carries over
    g_texture_clipboard.bitmap_id = -1;
    g_wrap_reference = {};
    g_texture_hover.face_id = -1;
    g_texture_hover.mover_uid = -1;
}

void texture_undo_entry_applied(CDedLevel& level, const UndoEntry& entry)
{
    try {
        if (entry.type != undo_modify_brushes || !level.solid || entry.brushes.size != entry.brushes_aux.size) {
            return;
        }
        // the level geometry faces by face id, gathered at the first brush face that needs them
        std::optional<std::unordered_map<int, std::vector<GFace*>>> built;
        bool changed = false;
        for (int i = 0; i < entry.brushes.size; ++i) {
            BrushNode* live = entry.brushes.data_ptr[i];
            BrushNode* old = entry.brushes_aux.data_ptr[i];
            const auto pairs = live && old ? texture_only_pairs(*live, *old) : std::nullopt;
            if (!pairs) {
                continue;
            }
            auto* live_solid = static_cast<GSolid*>(live->geometry);
            auto* old_solid = static_cast<GSolid*>(old->geometry);
            for (const FacePair& pair : *pairs) {
                const GFace& f = *pair.live;
                const GFace& g = *pair.old;
                if (f.face_id < 0) {
                    continue;
                }
                bool uvs = false;
                for (size_t c = 0; c < pair.live_corners.size(); ++c) {
                    uvs = uvs || pair.live_corners[c]->u != pair.old_corners[c]->u ||
                          pair.live_corners[c]->v != pair.old_corners[c]->v;
                }
                const bool attributes = f.bitmap_id != g.bitmap_id || ((f.flags ^ g.flags) & face_properties_flags) ||
                                        f.smoothing_groups != g.smoothing_groups;
                const GTextureMover* mover = live_solid->find_texture_mover(f.face_id);
                const GTextureMover* old_mover = old_solid->find_texture_mover(f.face_id);
                const bool scrolling = !mover != !old_mover ||
                                       (mover && (mover->u_pan_speed != old_mover->u_pan_speed ||
                                                  mover->v_pan_speed != old_mover->v_pan_speed));
                if (!uvs && !attributes && !scrolling) {
                    continue;
                }
                if (!built) {
                    built = level.solid->faces_by_id();
                }
                const auto it = built->find(f.face_id);
                if (it == built->end() || !built_from({live, pair.live}, it->second)) {
                    continue;
                }
                if (uvs) {
                    level.solid->update_faces_from_brush_face(pair.live, &live->pos, &live->orient);
                }
                if (attributes) {
                    for (GFace* face : it->second) {
                        face->bitmap_id = f.bitmap_id;
                        face->flags = (face->flags & ~face_properties_flags) | (f.flags & face_properties_flags);
                        face->smoothing_groups = f.smoothing_groups;
                    }
                }
                if (scrolling) {
                    GTextureMover* built_mover = level.solid->find_texture_mover(f.face_id);
                    if (mover && built_mover) {
                        built_mover->u_pan_speed = mover->u_pan_speed;
                        built_mover->v_pan_speed = mover->v_pan_speed;
                    }
                    else if (mover) {
                        level.solid->add_texture_mover(f.face_id, mover->u_pan_speed, mover->v_pan_speed);
                    }
                    else if (built_mover) {
                        level.solid->remove_texture_mover(built_mover);
                    }
                }
                changed = true;
            }
        }
        if (changed) {
            geo_cache_flush_all();
            redraw_all_viewports();
        }
    }
    catch (const std::exception& e) {
        xlog::error("[Texture] undo: {}", e.what());
    }
}

void ApplyTextureModePatches()
{
    update_faces_from_brush_face_hook.install();
    box_map_hook.install();
    planar_map_hook.install();
    cylinder_map_hook.install();
    texture_mode_hit_face_log_hook.install();
    texture_copy_hook.install();
    texture_paste_hook.install();
    face_properties_do_modal_hook.install();
    texture_map_cylinder_faces_hook.install();
    texture_apply_map_hook.install();
    texture_map_gizmo_draw_injection.install();
    texture_snap_map_hook.install();
    uv_unwrap_transform_step_hook.install();
    uv_unwrap_draw_corners_hook.install();
    uv_unwrap_lbutton_up_stale_select_fix.install();
    uv_unwrap_rbutton_up_stale_select_fix.install();
}
