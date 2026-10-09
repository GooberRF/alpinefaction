#include <windows.h>
#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstring>
#include <exception>
#include <format>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>
#include <patch_common/CallHook.h>
#include <common/alpine_mesh_scale.h>
#include <xlog/xlog.h>
#include "alpine_obj.h"
#include "dialog_tooltips.h"
#include "dir_light.h"
#include "gizmo.h"
#include "level.h"
#include "mfc_types.h"
#include "resources.h"
#include "terrain_paint.h"
#include "vtypes.h"

extern HMODULE g_module;

namespace
{

// ─── Constants ──────────────────────────────────────────────────────────────

// An arrow's length on screen; the rest of the handle layout is in fractions of it.
constexpr float arrow_px = 100.0f;
constexpr float shaft_start = 0.2f;
constexpr float cone_base = 0.8f;
constexpr float cone_radius = 0.06f;
constexpr float plane_near = 0.28f;
constexpr float plane_far = 0.46f;
constexpr float view_circle_radius = 0.1f;
// Rotate: the axis rings, and the view ring around them.
constexpr float ring_radius = 0.8f;
constexpr float view_ring_radius = 1.0f;
constexpr int ring_segments = 64;

constexpr float axis_pick_px = 8.0f;
constexpr float view_pick_px = 13.0f;
constexpr float ring_pick_px = 8.0f;
// An axis pointing at the camera, or a plane seen edge-on, shrinks below these on screen and is hidden.
constexpr float min_axis_px = 12.0f;
constexpr float min_plane_area_px = 40.0f;

constexpr float near_depth = 0.05f;
// A ray this close to parallel with an axis or plane gives an unstable solve; the drag falls back.
constexpr float min_axis_ray_sin_sq = 0.01f;
constexpr float min_plane_ray_cos = 0.02f;
// A solve this far from the pivot is a ray grazing the drag plane, not a move.
constexpr float max_drag_distance = 1.0e5f;
// A ring whose axis is this close to the screen plane is nearly a line on screen: its angle follows the cursor along
// the ring instead of around the pivot.
constexpr float edge_on_ring_cos = 0.2f;
// Closer to the pivot than this, the cursor's angle around it is noise.
constexpr float min_angle_radius_px = 4.0f;
// Trackball: dragging the rings' screen radius turns a quarter turn.
constexpr float trackball_rad_per_ring = 1.5707963f;
// Scale: the factor's range (never zero or flipped), and the uniform handle's drag per doubling.
constexpr float min_scale_factor = 0.01f;
constexpr float max_scale_factor = 100.0f;
constexpr float uniform_px_per_double = 100.0f;
// Scale: the box at the end of an axis, as a fraction of the arrow.
constexpr float scale_box_half = 0.04f;

constexpr DWORD hover_refresh_ms = 150;
constexpr uint8_t occluded_alpha = 70;
// Alpha blended; the overlay ignores the depth buffer, the depth tested pass reads it without writing.
constexpr uint32_t mode_overlay = gr_mode(0, 0, 0, 3, 0, 0);
constexpr uint32_t mode_depth_tested = mode_vertex_alpha;

constexpr std::array<float, 6> scale_steps{0.01f, 0.05f, 0.1f, 0.25f, 0.5f, 1.0f};
constexpr int default_scale_step = 2;

constexpr float two_pi = 6.2831853f;
constexpr float deg_per_rad = 57.2957795f;
// The status bar is ANSI: the code page's degree sign.
constexpr char degree_sign = '\xB0';
constexpr char times_sign = '\xD7';

constexpr const char* axis_names[3] = {"X", "Y", "Z"};

void report(const char* where, const std::exception& e)
{
    xlog::error("[Gizmo] {}: {}", where, e.what());
}

// ─── Vector math ────────────────────────────────────────────────────────────

bool is_finite(const Vector3& a)
{
    return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z);
}

const Vector3& frame_axis(const Matrix3& m, int axis)
{
    return axis == 0 ? m.rvec : axis == 1 ? m.uvec : m.fvec;
}

const Vector3& world_axis(int axis)
{
    return frame_axis(identity_orient, axis);
}

Vector3 any_perpendicular(const Vector3& a)
{
    return normalized(cross(a, std::abs(a.x) < 0.9f ? Vector3{1.0f, 0.0f, 0.0f} : Vector3{0.0f, 1.0f, 0.0f}));
}

// The rows made orthonormal: forward kept, up made orthogonal to it, right = up x forward with the sign of the old
// right (so a mirrored orient stays mirrored). False when they are degenerate.
bool orthonormalize(const Matrix3& m, Matrix3& out)
{
    const Vector3 f = normalized(m.fvec);
    const Vector3 u = normalized(m.uvec - f * dot(m.uvec, f));
    Vector3 r = cross(u, f);
    if (length(f) < 0.5f || length(u) < 0.5f) return false;
    if (dot(r, m.rvec) < 0.0f) {
        r = r * -1.0f;
    }
    out = {r, u, f};
    return true;
}

// Identity when degenerate.
Matrix3 orthonormal(const Matrix3& m)
{
    Matrix3 out;
    return orthonormalize(m, out) ? out : identity_orient;
}

// A turn by `angle` about the unit `axis` (Rodrigues); positive turns carry v towards axis x v.
struct Rotation
{
    Vector3 axis;
    float c = 1.0f;
    float s = 0.0f;

    Rotation() = default;
    Rotation(const Vector3& unit_axis, float angle) : axis(unit_axis), c(std::cos(angle)), s(std::sin(angle)) {}

    Vector3 apply(const Vector3& v) const
    {
        return v * c + cross(axis, v) * s + axis * (dot(axis, v) * (1.0f - c));
    }

    // Each row turned, then made orthonormal again, so float error never builds up into a skewed basis.
    Matrix3 apply(const Matrix3& m) const
    {
        const Matrix3 turned{apply(m.rvec), apply(m.uvec), apply(m.fvec)};
        Matrix3 out;
        return orthonormalize(turned, out) ? out : turned;
    }
};

// An accumulated turn; Rotation applies it.
struct Quat
{
    float w = 1.0f, x = 0.0f, y = 0.0f, z = 0.0f;

    Quat() = default;
    Quat(float w_, float x_, float y_, float z_) : w(w_), x(x_), y(y_), z(z_) {}
    Quat(const Vector3& unit_axis, float angle)
    {
        const float s = std::sin(angle * 0.5f);
        w = std::cos(angle * 0.5f);
        x = unit_axis.x * s;
        y = unit_axis.y * s;
        z = unit_axis.z * s;
    }

    // This turn after `o`.
    Quat operator*(const Quat& o) const
    {
        return {w * o.w - x * o.x - y * o.y - z * o.z, w * o.x + x * o.w + y * o.z - z * o.y,
                w * o.y - x * o.z + y * o.w + z * o.x, w * o.z + x * o.y - y * o.x + z * o.w};
    }

    void normalize()
    {
        const float len = std::sqrt(w * w + x * x + y * y + z * z);
        if (len > 1e-12f && std::isfinite(len)) {
            w /= len;
            x /= len;
            y /= len;
            z /= len;
        }
        else {
            *this = Quat{};
        }
    }

    // The same turn about a unit axis, `fallback` when there is none.
    void to_axis_angle(const Vector3& fallback, Vector3& axis, float& angle) const
    {
        const float s = std::sqrt(x * x + y * y + z * z);
        if (s < 1e-7f) {
            axis = fallback;
            angle = 0.0f;
            return;
        }
        axis = Vector3{x, y, z} * (1.0f / s);
        angle = 2.0f * std::atan2(s, w);
    }
};

// Into [-pi, pi].
float wrap_angle(float a)
{
    return std::remainder(a, two_pi);
}

struct Pt
{
    float x = 0.0f;
    float y = 0.0f;
};

float distance(Pt a, Pt b)
{
    return std::hypot(a.x - b.x, a.y - b.y);
}

float distance_to_segment(Pt p, Pt a, Pt b)
{
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len_sq = dx * dx + dy * dy;
    const float t = len_sq > 0.0f ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / len_sq, 0.0f, 1.0f) : 0.0f;
    return distance(p, {a.x + dx * t, a.y + dy * t});
}

float cross_2d(Pt o, Pt a, Pt b)
{
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

bool inside_quad(Pt p, const std::array<Pt, 4>& q)
{
    bool positive = false, negative = false;
    for (int i = 0; i < 4; i++) {
        const float c = cross_2d(q[i], q[(i + 1) % 4], p);
        positive = positive || c > 0.0f;
        negative = negative || c < 0.0f;
    }
    return !(positive && negative);
}

float quad_area(const std::array<Pt, 4>& q)
{
    float twice = 0.0f;
    for (int i = 0; i < 4; i++) {
        twice += q[i].x * q[(i + 1) % 4].y - q[(i + 1) % 4].x * q[i].y;
    }
    return std::abs(twice) * 0.5f;
}

// The cursor's angle around a screen point, growing clockwise on screen (y runs down).
float screen_angle(Pt centre, Pt p)
{
    return std::atan2(p.y - centre.y, p.x - centre.x);
}

// Along the axis through `p`, the point nearest the ray. Fails when they are near parallel or, in perspective, when
// that point is behind the camera.
bool closest_on_axis(const Vector3& p, const Vector3& axis, const EditorRay& ray, bool perspective, float& s)
{
    const Vector3 w = p - ray.o;
    const float b = dot(axis, ray.d);
    const float sin_sq = 1.0f - b * b;
    if (sin_sq < min_axis_ray_sin_sq) return false;
    s = (b * dot(ray.d, w) - dot(axis, w)) / sin_sq;
    const float t = dot(ray.d, w) + s * b;
    return std::isfinite(s) && (!perspective || t > 0.0f);
}

bool ray_plane(const EditorRay& ray, const Vector3& p, const Vector3& n, bool perspective, Vector3& hit)
{
    const float den = dot(ray.d, n);
    if (std::abs(den) < min_plane_ray_cos) return false;
    const float t = dot(p - ray.o, n) / den;
    if (!std::isfinite(t) || (perspective && t <= 0.0f)) return false;
    hit = ray.o + ray.d * t;
    return true;
}

// ─── Settings and the top bar ───────────────────────────────────────────────

enum class Mode : int
{
    select = 0,
    move = 1,
    rotate = 2,
    scale = 3,
};
constexpr int mode_count = 4;
constexpr std::array<UINT, mode_count> mode_button_ids{ID_GIZMO_SELECT, ID_GIZMO_MOVE, ID_GIZMO_ROTATE, ID_GIZMO_SCALE};

// Session only.
struct Settings
{
    Mode mode = Mode::select;
    bool snap = true;
    int scale_step = default_scale_step; // index into scale_steps
};
Settings g_settings;

struct TopBarState
{
    HWND hwnd = nullptr;
    WNDPROC base_proc = nullptr;
    bool synced = false;
    Settings shown;
};
TopBarState g_top_bar;

constexpr DialogTooltip top_bar_tips[] = {
    {ID_GIZMO_SELECT, "Select (1)"},
    {ID_GIZMO_MOVE, "Move (2)"},
    {ID_GIZMO_ROTATE, "Rotate (3)"},
    {ID_GIZMO_SCALE, "Scale (4)"},
    {ID_GIZMO_SNAP, "Snap (5): grid / angle / scale steps; Ctrl inverts while dragging"},
    {IDC_GIZMO_SCALE_STEP, "Scale snap step"},
};

HWND top_bar_hwnd()
{
    const HWND bar = g_main_frame ? g_main_frame->top_bar_hwnd() : nullptr;
    return bar && IsWindow(bar) ? bar : nullptr;
}

bool is_icon_button(UINT id)
{
    return id == ID_GIZMO_SNAP ||
           std::find(mode_button_ids.begin(), mode_button_ids.end(), id) != mode_button_ids.end();
}

bool icon_button_on(UINT id)
{
    if (id == ID_GIZMO_SNAP) return g_settings.snap;
    const auto it = std::find(mode_button_ids.begin(), mode_button_ids.end(), id);
    return it != mode_button_ids.end() && static_cast<int>(g_settings.mode) == it - mode_button_ids.begin();
}

using GlyphPoints = std::vector<std::array<float, 2>>;

// Icon glyphs on a 16 x 16 grid centred in the button.
struct Glyph
{
    HDC dc;
    float x0, y0, unit;
    COLORREF ink, paper;

    std::vector<POINT> to_px(const GlyphPoints& pts) const
    {
        std::vector<POINT> out;
        for (const auto& p : pts) {
            out.push_back({std::lround(x0 + p[0] * unit), std::lround(y0 + p[1] * unit)});
        }
        return out;
    }

    void stroke(const GlyphPoints& pts, float width, COLORREF color) const
    {
        LOGBRUSH lb{BS_SOLID, color, 0};
        const DWORD px = static_cast<DWORD>(std::max(1L, std::lround(width * unit)));
        constexpr DWORD style = PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_FLAT | PS_JOIN_ROUND;
        const HPEN pen = ExtCreatePen(style, px, &lb, 0, nullptr);
        const HGDIOBJ old = SelectObject(dc, pen);
        const std::vector<POINT> poly = to_px(pts);
        Polyline(dc, poly.data(), static_cast<int>(poly.size()));
        SelectObject(dc, old);
        DeleteObject(pen);
    }

    void stroke(const GlyphPoints& pts, float width) const
    {
        stroke(pts, width, ink);
    }

    void fill(const GlyphPoints& pts) const
    {
        const HPEN pen = CreatePen(PS_SOLID, 1, ink);
        const HBRUSH brush = CreateSolidBrush(ink);
        const HGDIOBJ old_pen = SelectObject(dc, pen);
        const HGDIOBJ old_brush = SelectObject(dc, brush);
        const std::vector<POINT> poly = to_px(pts);
        Polygon(dc, poly.data(), static_cast<int>(poly.size()));
        SelectObject(dc, old_brush);
        SelectObject(dc, old_pen);
        DeleteObject(brush);
        DeleteObject(pen);
    }
};

// From a0 to a1 degrees around (cx, cy), counter-clockwise as seen.
GlyphPoints glyph_arc(float cx, float cy, float r, float a0, float a1, int steps)
{
    GlyphPoints pts;
    for (int i = 0; i <= steps; i++) {
        const float a = (a0 + (a1 - a0) * static_cast<float>(i) / static_cast<float>(steps)) / deg_per_rad;
        pts.push_back({cx + r * std::cos(a), cy - r * std::sin(a)});
    }
    return pts;
}

void draw_glyph(const Glyph& g, UINT id)
{
    switch (id) {
        case ID_GIZMO_SELECT:
            g.fill({{4.0f, 1.0f}, {4.0f, 14.0f}, {7.0f, 11.0f}, {9.5f, 15.5f}, {11.5f, 14.5f}, {9.0f, 10.0f},
                    {13.0f, 10.0f}});
            break;
        case ID_GIZMO_MOVE:
            g.stroke({{8.0f, 3.0f}, {8.0f, 13.0f}}, 1.5f);
            g.stroke({{3.0f, 8.0f}, {13.0f, 8.0f}}, 1.5f);
            g.fill({{8.0f, 0.5f}, {5.0f, 4.0f}, {11.0f, 4.0f}});
            g.fill({{8.0f, 15.5f}, {5.0f, 12.0f}, {11.0f, 12.0f}});
            g.fill({{0.5f, 8.0f}, {4.0f, 5.0f}, {4.0f, 11.0f}});
            g.fill({{15.5f, 8.0f}, {12.0f, 5.0f}, {12.0f, 11.0f}});
            break;
        case ID_GIZMO_ROTATE: {
            // Open on the right, the arrowhead ending the arc there.
            constexpr float end_deg = 310.0f;
            g.stroke(glyph_arc(8.0f, 8.5f, 5.5f, 40.0f, end_deg, 24), 1.6f);
            const float a = end_deg / deg_per_rad;
            const float ex = 8.0f + 5.5f * std::cos(a), ey = 8.5f - 5.5f * std::sin(a);
            const float tx = -std::sin(a), ty = -std::cos(a); // onwards along the arc, on screen
            g.fill({{ex + tx * 3.5f, ey + ty * 3.5f},
                    {ex - ty * 2.6f, ey + tx * 2.6f},
                    {ex + ty * 2.6f, ey - tx * 2.6f}});
            break;
        }
        case ID_GIZMO_SCALE:
            g.stroke({{2.0f, 6.0f}, {10.0f, 6.0f}, {10.0f, 14.0f}, {2.0f, 14.0f}, {2.0f, 6.0f}}, 1.3f);
            g.stroke({{5.5f, 10.5f}, {12.0f, 4.0f}}, 1.5f);
            g.fill({{15.0f, 1.0f}, {9.0f, 2.0f}, {14.0f, 7.0f}});
            break;
        case ID_GIZMO_SNAP: {
            // A horseshoe magnet, its pole tips split off by a gap.
            GlyphPoints u{{3.5f, 1.5f}};
            for (const auto& p : glyph_arc(8.0f, 9.0f, 4.5f, 180.0f, 360.0f, 16)) {
                u.push_back(p);
            }
            u.push_back({12.5f, 1.5f});
            g.stroke(u, 3.0f);
            g.stroke({{0.5f, 4.5f}, {15.5f, 4.5f}}, 1.2f, g.paper);
            break;
        }
        default:
            break;
    }
}

// Pushed in and highlighted for the active mode and an enabled Snap, so the state reads at a glance.
void draw_icon_button(const DRAWITEMSTRUCT& dis)
{
    const bool on = icon_button_on(dis.CtlID);
    const bool pressed = (dis.itemState & ODS_SELECTED) != 0;
    RECT r = dis.rcItem;
    const int paper = on ? COLOR_HIGHLIGHT : COLOR_BTNFACE;
    FillRect(dis.hDC, &r, GetSysColorBrush(paper));
    DrawEdge(dis.hDC, &r, on || pressed ? EDGE_SUNKEN : EDGE_RAISED, BF_RECT);
    const int w = r.right - r.left, h = r.bottom - r.top;
    const int side = std::max(8, std::min(w, h) - 8);
    const int shift = pressed ? 1 : 0;
    const Glyph g{dis.hDC,
                  static_cast<float>(r.left + (w - side) / 2 + shift),
                  static_cast<float>(r.top + (h - side) / 2 + shift),
                  static_cast<float>(side) / 16.0f,
                  GetSysColor(on ? COLOR_HIGHLIGHTTEXT : COLOR_BTNTEXT),
                  GetSysColor(paper)};
    draw_glyph(g, dis.CtlID);
    if ((dis.itemState & ODS_FOCUS) && !(dis.itemState & ODS_NOFOCUSRECT)) {
        InflateRect(&r, -3, -3);
        DrawFocusRect(dis.hDC, &r);
    }
}

// The owner-drawn buttons send WM_DRAWITEM to the top bar, whose CControlBar::WindowProc would pass it to the frame.
LRESULT CALLBACK top_bar_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    const WNDPROC base = g_top_bar.base_proc;
    if (msg == WM_DRAWITEM) {
        const auto* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
        if (dis && dis->CtlType == ODT_BUTTON && is_icon_button(dis->CtlID)) {
            try {
                draw_icon_button(*dis);
            }
            catch (const std::exception& e) {
                report("draw button", e);
            }
            return TRUE;
        }
    }
    else if (msg == WM_NCDESTROY) {
        SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(base));
        g_top_bar = TopBarState{};
    }
    return CallWindowProcA(base, hwnd, msg, wp, lp);
}

void sync_top_bar()
{
    const HWND bar = top_bar_hwnd();
    if (!bar) return;
    if (bar != g_top_bar.hwnd) {
        g_top_bar = TopBarState{};
        g_top_bar.hwnd = bar;
        g_top_bar.base_proc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrA(bar, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(top_bar_proc)));
        alpine_dlg_add_tooltips(bar, top_bar_tips);
        SendDlgItemMessageA(bar, IDC_GIZMO_SCALE_STEP, CB_RESETCONTENT, 0, 0);
        for (float step : scale_steps) {
            const std::string label = std::format("{:g}", step);
            SendDlgItemMessageA(bar, IDC_GIZMO_SCALE_STEP, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
    }
    const Settings& shown = g_top_bar.shown;
    if (g_top_bar.synced && shown.mode == g_settings.mode && shown.snap == g_settings.snap &&
        shown.scale_step == g_settings.scale_step) {
        return;
    }
    for (UINT id : mode_button_ids) {
        InvalidateRect(GetDlgItem(bar, static_cast<int>(id)), nullptr, FALSE);
    }
    InvalidateRect(GetDlgItem(bar, ID_GIZMO_SNAP), nullptr, FALSE);
    SendDlgItemMessageA(bar, IDC_GIZMO_SCALE_STEP, CB_SETCURSEL, g_settings.scale_step, 0);
    g_top_bar.shown = g_settings;
    g_top_bar.synced = true;
}

bool scale_list_dropped()
{
    const HWND bar = top_bar_hwnd();
    return bar && SendDlgItemMessageA(bar, IDC_GIZMO_SCALE_STEP, CB_GETDROPPEDSTATE, 0, 0) != 0;
}

// ─── What the gizmo acts on ─────────────────────────────────────────────────

enum Cap : uint32_t
{
    cap_move = 0x1,
    cap_rotate = 0x2,
    cap_scale = 0x4,
};

uint32_t mode_cap(Mode mode)
{
    switch (mode) {
        case Mode::move:
            return cap_move;
        case Mode::rotate:
            return cap_rotate;
        case Mode::scale:
            return cap_scale;
        default:
            return 0;
    }
}

// Every type moves, turns and has its position spread by a scale, like stock R-drags turn any of them; how a scale
// changes an object's own size is size_kind's business.
uint32_t object_caps(const DedObject& obj)
{
    switch (obj.type) {
        // sun_arrow_sync locks its position above the player start each frame; turning it aims the sun (its forward
        // becomes the sun's yaw and pitch, roll is dropped).
        case DedObjectType::DED_SUN_ARROW:
            return cap_rotate;
        // Its orientation is forced back to identity each frame.
        case DedObjectType::DED_TERRAIN:
            return cap_move;
        default:
            return cap_move | cap_rotate | cap_scale;
    }
}

constexpr uint32_t brush_caps = cap_move | cap_rotate | cap_scale;

bool mode_has_objects(DedEditMode mode)
{
    return mode == DedEditMode::Object || mode == DedEditMode::Group;
}

bool mode_has_brushes(DedEditMode mode)
{
    return mode == DedEditMode::Brush || mode == DedEditMode::Group;
}

struct Targets
{
    std::vector<DedObject*> objects;
    std::vector<BrushNode*> brushes;

    bool empty() const
    {
        return objects.empty() && brushes.empty();
    }

    std::size_t count() const
    {
        return objects.size() + brushes.size();
    }
};

// The selected items of the edit mode that allow `cap`. Brush mode with no brush selected moves the brush cursor in
// stock; the gizmo leaves it alone. The sun arrow only turns on its own: in a mixed selection it is left out, of the
// pivot and the frame too.
Targets collect_targets(const CDedLevel& level, uint32_t cap)
{
    Targets t;
    bool sun_arrow = false;
    if (mode_has_objects(level.edit_mode)) {
        for (int i = 0; i < level.selection.size; i++) {
            DedObject* obj = level.selection.data_ptr[i];
            if (obj && (object_caps(*obj) & cap)) {
                sun_arrow = sun_arrow || obj->type == DedObjectType::DED_SUN_ARROW;
                t.objects.push_back(obj);
            }
        }
    }
    if (mode_has_brushes(level.edit_mode) && (brush_caps & cap)) {
        t.brushes = level.selected_brushes();
    }
    if (sun_arrow && t.count() > 1) {
        std::erase_if(t.objects, [](const DedObject* obj) { return obj->type == DedObjectType::DED_SUN_ARROW; });
    }
    return t;
}

// The size fields of a box or sphere volume object, as the viewport draws them: full box sizes along the object's
// own axes (width x, height y, depth z), or a radius.
struct Volume
{
    enum class Kind
    {
        none,
        sphere,
        box,
    };
    Kind kind = Kind::none;
    float* radius = nullptr;
    std::array<float*, 3> size{};
    bool world_axes = false; // a push region's axis-aligned box ignores its orient
    // A directional light's box is turned only by its yaw about world Y; its cylinder runs along its travel direction
    // with length size[1] and radius `radius`.
    bool own_axes_set = false;
    Matrix3 own_axes;
    bool cylinder = false;

    template<typename T>
    void set(T& obj, bool sphere, bool box)
    {
        kind = sphere ? Kind::sphere : box ? Kind::box : Kind::none;
        radius = &obj.radius;
        size = {&obj.width, &obj.height, &obj.depth};
    }
};

Volume volume_of(DedObject& obj)
{
    Volume v;
    switch (obj.type) {
        case DedObjectType::DED_TRIGGER: {
            auto& trigger = static_cast<DedTrigger&>(obj);
            v.set(trigger, trigger.is_box == 0, trigger.is_box != 0);
            break;
        }
        case DedObjectType::DED_GAS_REGION: {
            auto& gas = static_cast<DedGasRegion&>(obj);
            v.set(gas, gas.shape == GasRegionShape::sphere, gas.shape == GasRegionShape::box);
            break;
        }
        case DedObjectType::DED_GEO_REGION: {
            auto& geo = static_cast<DedGeoRegion&>(obj);
            v.set(geo, geo.shape == GeoRegionShape::sphere, geo.shape == GeoRegionShape::box);
            break;
        }
        case DedObjectType::DED_PUSH_REGION: {
            auto& push = static_cast<DedPushRegion&>(obj);
            v.set(push, push.shape == PushRegionShape::sphere, push.shape != PushRegionShape::sphere);
            v.world_axes = push.shape == PushRegionShape::axis_aligned_box;
            break;
        }
        case DedObjectType::DED_WEATHER_REGION: {
            auto& weather = static_cast<DedWeatherRegion&>(obj);
            v.set(weather, weather.shape == WeatherRegionShape::sphere, weather.shape == WeatherRegionShape::box);
            break;
        }
        case DedObjectType::DED_DIRECTIONAL_LIGHT: {
            auto& light = static_cast<DedDirectionalLight&>(obj);
            switch (light.shape) {
                case alpine_dir_light::Shape::box: {
                    v.kind = Volume::Kind::box;
                    v.size = {&light.extent_x, &light.extent_y, &light.extent_z};
                    alpine_dir_light::Vec3 right, forward;
                    alpine_dir_light::box_yaw_axes(light.box_yaw, right, forward);
                    v.own_axes_set = true;
                    v.own_axes = {{right.x, right.y, right.z}, {0.0f, 1.0f, 0.0f}, {forward.x, forward.y, forward.z}};
                    break;
                }
                case alpine_dir_light::Shape::sphere:
                    v.kind = Volume::Kind::sphere;
                    v.radius = &light.extent_x;
                    break;
                case alpine_dir_light::Shape::cylinder:
                    v.kind = Volume::Kind::box;
                    v.cylinder = true;
                    v.radius = &light.extent_x;
                    v.size[1] = &light.extent_y;
                    break;
                default:
                    break;
            }
            break;
        }
        case DedObjectType::DED_CLIMBING_REGION: {
            auto& climb = static_cast<DedClimbingRegion&>(obj);
            v.kind = Volume::Kind::box;
            v.size = {&climb.width, &climb.height, &climb.depth};
            break;
        }
        default:
            break;
    }
    return v;
}

// What a scale does to an object's own size.
enum class SizeKind
{
    none,         // only its position spreads about the pivot
    uniform_only, // Alpine Mesh draw scale, sphere radius: uniform drags only
    per_axis,     // box sizes along its own axes
};

SizeKind size_kind(DedObject& obj)
{
    if (obj.type == DedObjectType::DED_MESH) return SizeKind::uniform_only;
    switch (volume_of(obj).kind) {
        case Volume::Kind::sphere:
            return SizeKind::uniform_only;
        case Volume::Kind::box:
            return SizeKind::per_axis;
        default:
            return SizeKind::none;
    }
}

// The Alpine state a drag changes beside pos / orient, which the stock move record lacks.
struct Extras
{
    float radius = 0.0f;
    std::array<float, 3> size{};
    float draw_scale = 1.0f;
    float box_yaw = 0.0f;

    bool operator==(const Extras& o) const
    {
        return radius == o.radius && size == o.size && draw_scale == o.draw_scale && box_yaw == o.box_yaw;
    }
};

bool has_extras(DedObject& obj)
{
    const Volume v = volume_of(obj);
    return obj.type == DedObjectType::DED_MESH || obj.type == DedObjectType::DED_DIRECTIONAL_LIGHT || v.radius ||
           v.size[0];
}

Extras read_extras(DedObject& obj)
{
    Extras e;
    const Volume v = volume_of(obj);
    if (v.radius) {
        e.radius = *v.radius;
    }
    for (int i = 0; i < 3; i++) {
        if (v.size[i]) {
            e.size[i] = *v.size[i];
        }
    }
    if (obj.type == DedObjectType::DED_MESH) {
        e.draw_scale = static_cast<DedMesh&>(obj).draw_scale;
    }
    else if (obj.type == DedObjectType::DED_DIRECTIONAL_LIGHT) {
        e.box_yaw = static_cast<DedDirectionalLight&>(obj).box_yaw;
    }
    return e;
}

void write_extras(DedObject& obj, const Extras& e)
{
    const Volume v = volume_of(obj);
    if (v.radius) {
        *v.radius = e.radius;
    }
    for (int i = 0; i < 3; i++) {
        if (v.size[i]) {
            *v.size[i] = e.size[i];
        }
    }
    if (obj.type == DedObjectType::DED_MESH) {
        static_cast<DedMesh&>(obj).draw_scale = e.draw_scale;
    }
    else if (obj.type == DedObjectType::DED_DIRECTIONAL_LIGHT) {
        static_cast<DedDirectionalLight&>(obj).box_yaw = e.box_yaw;
    }
    else if (obj.type == DedObjectType::DED_GAS_REGION) {
        auto& gas = static_cast<DedGasRegion&>(obj);
        if (gas.region) {
            gas.sync_region_sizes();
        }
    }
}

// As stock vertex edits leave a GSolid, minus their planarity split and re-centre: a scale keeps faces planar, and
// re-centring would move pos under the undo record.
void refresh_solid(GSolid& solid)
{
    for (GFace* face = solid.face_list_head; face; face = face->next_solid) {
        face->compute_plane_and_bbox();
    }
    solid.compute_bbox_sphere();
}

// A GSolid's vertices with everything refresh_solid derives from them, so a cancel or undo puts it back bit for bit
// rather than deriving it again.
struct FaceShape
{
    Plane plane;
    Vector3 bbox_min;
    Vector3 bbox_max;
};

struct BrushShape
{
    std::vector<Vector3> verts;
    std::vector<FaceShape> faces;
    Vector3 bbox_min;
    Vector3 bbox_max;
    float sphere_radius = 0.0f;
    Vector3 sphere_center;
};

BrushShape read_brush_shape(const GSolid& solid)
{
    BrushShape shape;
    for (int i = 0; i < solid.vertices.size; i++) {
        const GVertex* v = solid.vertices.data_ptr[i];
        shape.verts.push_back(v ? v->pos : Vector3{});
    }
    for (const GFace* face = solid.face_list_head; face; face = face->next_solid) {
        shape.faces.push_back({face->plane, face->bounding_box_min, face->bounding_box_max});
    }
    shape.bbox_min = solid.bbox_min;
    shape.bbox_max = solid.bbox_max;
    shape.sphere_radius = solid.bounding_sphere_radius;
    shape.sphere_center = solid.bounding_sphere_center;
    return shape;
}

bool shape_fits(const GSolid& solid, const BrushShape& shape)
{
    int faces = 0;
    for (const GFace* face = solid.face_list_head; face; face = face->next_solid) {
        faces++;
    }
    return solid.vertices.size == static_cast<int>(shape.verts.size()) &&
           faces == static_cast<int>(shape.faces.size());
}

void write_brush_shape(GSolid& solid, const BrushShape& shape)
{
    if (!shape_fits(solid, shape)) return;
    for (int i = 0; i < solid.vertices.size; i++) {
        if (GVertex* v = solid.vertices.data_ptr[i]) {
            v->pos = shape.verts[i];
        }
    }
    std::size_t n = 0;
    for (GFace* face = solid.face_list_head; face; face = face->next_solid) {
        face->plane = shape.faces[n].plane;
        face->bounding_box_min = shape.faces[n].bbox_min;
        face->bounding_box_max = shape.faces[n].bbox_max;
        n++;
    }
    solid.bbox_min = shape.bbox_min;
    solid.bbox_max = shape.bbox_max;
    solid.bounding_sphere_radius = shape.sphere_radius;
    solid.bounding_sphere_center = shape.sphere_center;
}

// The centre of the targets' bounds over object positions and brush world vertices, as the stock transform pivot
// (0x004265E0). The one place the pivot is chosen.
bool selection_pivot(const Targets& t, Vector3& pivot)
{
    Vector3 lo{INFINITY, INFINITY, INFINITY};
    Vector3 hi{-INFINITY, -INFINITY, -INFINITY};
    auto grow = [&](const Vector3& p) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    };
    for (const DedObject* obj : t.objects) {
        grow(obj->pos);
    }
    for (const BrushNode* brush : t.brushes) {
        const auto* solid = static_cast<const GSolid*>(brush->geometry);
        if (!solid || solid->vertices.size <= 0) {
            grow(brush->pos);
            continue;
        }
        for (int i = 0; i < solid->vertices.size; i++) {
            if (const GVertex* v = solid->vertices.data_ptr[i]) {
                grow(brush->orient * v->pos + brush->pos);
            }
        }
    }
    if (!(lo.x <= hi.x && lo.y <= hi.y && lo.z <= hi.z)) return false;
    pivot = (lo + hi) * 0.5f;
    return is_finite(pivot);
}

// Global: world axes (stock Global M-drags follow the camera instead). Local: the first target's own axes.
Matrix3 gizmo_frame(const CDedLevel& level, const Targets& t)
{
    if (!level.coords_local) return identity_orient;
    if (!t.objects.empty()) return orthonormal(t.objects.front()->orient);
    if (!t.brushes.empty()) return orthonormal(t.brushes.front()->orient);
    return identity_orient;
}

// Where the handles go, and for Scale whether only the uniform one makes sense.
struct Placement
{
    Mode mode = Mode::move;
    Vector3 pivot;
    Matrix3 frame;
    bool uniform_only = false;
};

// Scale shows when something has a size to change or several positions can spread; it offers only the uniform
// handle when every target with a size can only grow uniformly (meshes, spheres).
bool scale_placement(const Targets& t, Placement& p)
{
    bool sized = !t.brushes.empty(), per_axis = !t.brushes.empty();
    for (DedObject* obj : t.objects) {
        const SizeKind kind = size_kind(*obj);
        sized = sized || kind != SizeKind::none;
        per_axis = per_axis || kind == SizeKind::per_axis;
    }
    p.uniform_only = sized && !per_axis;
    return sized || t.count() > 1;
}

bool gizmo_shown(const CDedLevel& level, Targets& targets, Placement& placement)
{
    const Mode mode = g_settings.mode;
    if (mode == Mode::select || terrain_paint_panel_open() || brush_clip_dialog) return false;
    if (!mode_has_objects(level.edit_mode) && !mode_has_brushes(level.edit_mode)) return false;
    targets = collect_targets(level, mode_cap(mode));
    placement.mode = mode;
    if (targets.empty() || (mode == Mode::scale && !scale_placement(targets, placement))) return false;
    if (!selection_pivot(targets, placement.pivot)) return false;
    placement.frame = gizmo_frame(level, targets);
    return true;
}

bool modifier_key_held()
{
    for (uint8_t key : {DIK_LCONTROL, DIK_RCONTROL, DIK_LSHIFT, DIK_RSHIFT, DIK_LMENU, DIK_RMENU}) {
        if (g_dinput_keys[key]) return true;
    }
    return false;
}

// Ctrl held turns snapping the other way for as long as it is held.
bool snapping()
{
    const bool ctrl = g_dinput_keys[DIK_LCONTROL] || g_dinput_keys[DIK_RCONTROL];
    return g_settings.snap != ctrl;
}

// ─── Projection and layout ──────────────────────────────────────────────────

// The projection of the view last set up, recovered from screen_to_ray so it carries RED's own field of view (and
// AF's Hor+ change to it): a perspective ray scaled to unit depth, or an ortho ray's origin, is linear in the pixel.
struct ViewProj
{
    bool perspective = false;
    Vector3 eye;  // perspective
    Vector3 fwd;  // unit
    Vector3 base; // pixel (0, 0): its unit-depth ray (perspective) or its ray origin (ortho)
    Vector3 px_x; // per pixel rightwards
    Vector3 px_y; // per pixel downwards
    float px_x_sq = 0.0f;
    float px_y_sq = 0.0f;

    float depth(const Vector3& p) const
    {
        return perspective ? dot(p - eye, fwd) : 1.0f;
    }

    bool project(const Vector3& p, Pt& out) const
    {
        Vector3 w;
        if (perspective) {
            const float t = depth(p);
            if (t < near_depth) return false;
            w = (p - eye) * (1.0f / t) - base;
        }
        else {
            w = p - base;
        }
        out = {dot(w, px_x) / px_x_sq, dot(w, px_y) / px_y_sq};
        return std::isfinite(out.x) && std::isfinite(out.y);
    }

    float units_per_px(const Vector3& p) const
    {
        const float s = std::sqrt(px_x_sq);
        return perspective ? std::max(depth(p), near_depth) * s : s;
    }

    // From `p` towards the camera.
    Vector3 to_viewer(const Vector3& p) const
    {
        return perspective ? normalized(eye - p) : fwd * -1.0f;
    }
};

bool current_view_proj(ViewProj& vp)
{
    constexpr float span = 100.0f;
    const EditorRay centre = editor_screen_ray(gr_half_width, gr_half_height);
    const EditorRay r0 = editor_screen_ray(0.0f, 0.0f);
    const EditorRay rx = editor_screen_ray(span, 0.0f);
    const EditorRay ry = editor_screen_ray(0.0f, span);
    vp.perspective = gr_perspective != 0;
    vp.fwd = normalized(centre.d);
    if (length(vp.fwd) < 0.5f) return false;
    if (vp.perspective) {
        Vector3 unit[3];
        const Vector3* dirs[3] = {&r0.d, &rx.d, &ry.d};
        for (int i = 0; i < 3; i++) {
            const float k = dot(*dirs[i], vp.fwd);
            if (k < 1e-4f) return false;
            unit[i] = *dirs[i] * (1.0f / k);
        }
        vp.eye = r0.o;
        vp.base = unit[0];
        vp.px_x = (unit[1] - unit[0]) * (1.0f / span);
        vp.px_y = (unit[2] - unit[0]) * (1.0f / span);
    }
    else {
        vp.base = r0.o;
        vp.px_x = (rx.o - r0.o) * (1.0f / span);
        vp.px_y = (ry.o - r0.o) * (1.0f / span);
    }
    vp.px_x_sq = dot(vp.px_x, vp.px_x);
    vp.px_y_sq = dot(vp.px_y, vp.px_y);
    return vp.px_x_sq > 1e-14f && vp.px_y_sq > 1e-14f && std::isfinite(vp.px_x_sq) && std::isfinite(vp.px_y_sq) &&
           is_finite(vp.base) && is_finite(vp.eye);
}

// The segment's projection, cut at the near depth in perspective.
bool project_segment(const ViewProj& vp, Vector3 a, Vector3 b, Pt& pa, Pt& pb)
{
    if (vp.perspective) {
        const float ta = vp.depth(a), tb = vp.depth(b);
        const float cut = near_depth * 2.0f;
        if (ta < cut && tb < cut) return false;
        if (ta < cut) {
            a = a + (b - a) * ((cut - ta) / (tb - ta));
        }
        else if (tb < cut) {
            b = b + (a - b) * ((cut - tb) / (ta - tb));
        }
    }
    return vp.project(a, pa) && vp.project(b, pb);
}

enum class HandleKind : uint8_t
{
    none,
    axis,      // move along frame axis `axis`
    plane,     // move in the plane whose normal is frame axis `axis`
    view,      // move in the view plane
    ring,      // rotate about frame axis `axis`
    view_ring, // rotate about the view direction
    trackball, // rotate freely
};

struct Handle
{
    HandleKind kind = HandleKind::none;
    int axis = 0;

    bool operator==(const Handle& o) const
    {
        const bool has_axis = kind == HandleKind::axis || kind == HandleKind::plane || kind == HandleKind::ring;
        return kind == o.kind && (!has_axis || axis == o.axis);
    }
};

struct Layout
{
    Mode mode = Mode::move;
    ViewProj vp;
    Vector3 pivot;
    Matrix3 frame;
    Pt pivot_px;
    float size = 0.0f; // an arrow's world length
    bool two_d = false;
    Vector3 to_viewer; // from the pivot
    Vector3 right;     // the screen's axes in the world, unit
    Vector3 down;
    // Move
    bool axis_shown[3] = {};
    bool plane_shown[3] = {}; // by normal axis
    bool view_shown = false;
    // Rotate
    bool ring_shown[3] = {};
    bool view_ring_shown = false;
    bool trackball_shown = false;
    float trackball_px = 0.0f; // the axis rings' radius on screen
    int view_ring_color = -1;  // 2D: the world axis it turns about, -1 white
    // During a view ring drag every view draws the ring about the drag's axis, not its own view direction.
    bool view_ring_axis_set = false;
    Vector3 view_ring_axis;
};

std::array<Vector3, 4> plane_corners(const Layout& l, int normal_axis)
{
    const Vector3 a = frame_axis(l.frame, (normal_axis + 1) % 3) * l.size;
    const Vector3 b = frame_axis(l.frame, (normal_axis + 2) % 3) * l.size;
    return {l.pivot + a * plane_near + b * plane_near, l.pivot + a * plane_far + b * plane_near,
            l.pivot + a * plane_far + b * plane_far, l.pivot + a * plane_near + b * plane_far};
}

bool project_quad(const ViewProj& vp, const std::array<Vector3, 4>& corners, std::array<Pt, 4>& out)
{
    for (int i = 0; i < 4; i++) {
        if (!vp.project(corners[i], out[i])) return false;
    }
    return true;
}

// The rotation axis of a ring handle: a frame axis, or the view direction through the pivot.
Vector3 ring_axis(const Layout& l, Handle h)
{
    if (h.kind == HandleKind::ring) return frame_axis(l.frame, h.axis);
    return l.view_ring_axis_set ? l.view_ring_axis : l.to_viewer * -1.0f;
}

// A ring's points, ring_segments + 1 with the first repeated last, and whether each segment faces the camera. The
// view ring lies in the screen plane and faces it all round.
struct RingPoints
{
    std::array<Vector3, ring_segments + 1> p;
    std::array<bool, ring_segments> front;
};

RingPoints ring_points(const Layout& l, Handle h)
{
    RingPoints out;
    const bool view_ring = h.kind == HandleKind::view_ring;
    const bool screen_plane = view_ring && !l.view_ring_axis_set;
    const float radius = (view_ring ? view_ring_radius : ring_radius) * l.size;
    const Vector3 a = ring_axis(l, h);
    const Vector3 u = screen_plane ? l.right : any_perpendicular(a);
    const Vector3 v = screen_plane ? l.down : cross(a, u);
    for (int n = 0; n <= ring_segments; n++) {
        const float ang = two_pi * static_cast<float>(n) / ring_segments;
        out.p[n] = l.pivot + (u * std::cos(ang) + v * std::sin(ang)) * radius;
    }
    for (int n = 0; n < ring_segments; n++) {
        const Vector3 mid = (out.p[n] + out.p[n + 1]) * 0.5f - l.pivot;
        out.front[n] = screen_plane || l.two_d || dot(mid, l.to_viewer) >= -0.02f * radius;
    }
    return out;
}

// For the view last set up. A 2D view shows only the in-plane axes, their plane (and for Scale the centre), or for
// Rotate the ring about the exact view normal.
bool make_layout(const EditorViewport& view, const Placement& placement, Layout& l)
{
    if (!current_view_proj(l.vp)) return false;
    const Mode mode = placement.mode;
    const Vector3& pivot = placement.pivot;
    const Matrix3& frame = placement.frame;
    l.mode = mode;
    l.pivot = pivot;
    l.frame = frame;
    l.two_d = view.view_type != editor_view_type_perspective;
    if (!l.vp.project(pivot, l.pivot_px)) return false;
    l.size = arrow_px * l.vp.units_per_px(pivot);
    l.to_viewer = l.vp.to_viewer(pivot);
    l.right = normalized(l.vp.px_x);
    l.down = normalized(l.vp.px_y);
    int depth_axis = -1;
    if (l.two_d) {
        float best = -1.0f;
        for (int i = 0; i < 3; i++) {
            const float along = std::abs(dot(frame_axis(frame, i), l.vp.fwd));
            if (along > best) {
                best = along;
                depth_axis = i;
            }
        }
    }
    if (mode == Mode::rotate) {
        for (int i = 0; i < 3; i++) {
            l.ring_shown[i] = !l.two_d;
            if (l.two_d && std::abs(dot(world_axis(i), l.vp.fwd)) > 0.9999f) {
                l.view_ring_color = i;
            }
        }
        l.view_ring_shown = true;
        l.trackball_shown = !l.two_d;
        Pt edge;
        if (l.vp.project(pivot + l.right * (ring_radius * l.size), edge)) {
            l.trackball_px = distance(edge, l.pivot_px);
        }
        return true;
    }
    const bool per_axis = mode != Mode::scale || !placement.uniform_only;
    for (int i = 0; i < 3; i++) {
        Pt a, b;
        l.axis_shown[i] = per_axis && i != depth_axis &&
                          project_segment(l.vp, pivot, pivot + frame_axis(frame, i) * l.size, a, b) &&
                          distance(a, b) >= min_axis_px;
    }
    for (int k = 0; k < 3; k++) {
        std::array<Pt, 4> quad;
        l.plane_shown[k] = l.axis_shown[(k + 1) % 3] && l.axis_shown[(k + 2) % 3] &&
                           project_quad(l.vp, plane_corners(l, k), quad) && quad_area(quad) >= min_plane_area_px;
    }
    l.view_shown = !l.two_d || mode == Mode::scale;
    return true;
}

// Screen distance from the cursor to a ring's facing segments.
float ring_distance(const Layout& l, Handle h, Pt cursor)
{
    const RingPoints ring = ring_points(l, h);
    float best = INFINITY;
    for (int n = 0; n < ring_segments; n++) {
        Pt a, b;
        if (ring.front[n] && project_segment(l.vp, ring.p[n], ring.p[n + 1], a, b)) {
            best = std::min(best, distance_to_segment(cursor, a, b));
        }
    }
    return best;
}

Handle hit_test_rotate(const Layout& l, Pt cursor)
{
    Handle best;
    float best_px = ring_pick_px;
    for (int i = 0; i < 3; i++) {
        const Handle h{HandleKind::ring, i};
        const float px = l.ring_shown[i] ? ring_distance(l, h, cursor) : INFINITY;
        if (px <= best_px) {
            best_px = px;
            best = h;
        }
    }
    if (l.view_ring_shown) {
        const Handle h{HandleKind::view_ring, 0};
        if (ring_distance(l, h, cursor) <= best_px) return h;
    }
    if (best.kind == HandleKind::none && l.trackball_shown &&
        distance(cursor, l.pivot_px) < l.trackball_px - ring_pick_px) {
        best = {HandleKind::trackball, 0};
    }
    return best;
}

Handle hit_test(const Layout& l, Pt cursor)
{
    if (l.mode == Mode::rotate) return hit_test_rotate(l, cursor);
    if (l.view_shown && distance(cursor, l.pivot_px) <= view_pick_px) return {HandleKind::view, 0};
    for (int k = 0; k < 3; k++) {
        std::array<Pt, 4> quad;
        if (l.plane_shown[k] && project_quad(l.vp, plane_corners(l, k), quad) && inside_quad(cursor, quad)) {
            return {HandleKind::plane, k};
        }
    }
    Handle best;
    float best_px = axis_pick_px;
    for (int i = 0; i < 3; i++) {
        Pt a, b;
        const Vector3 dir = frame_axis(l.frame, i) * l.size;
        if (!l.axis_shown[i] || !project_segment(l.vp, l.pivot + dir * shaft_start, l.pivot + dir, a, b)) continue;
        const float px = distance_to_segment(cursor, a, b);
        if (px <= best_px) {
            best_px = px;
            best = {HandleKind::axis, i};
        }
    }
    return best;
}

// ─── Drawing ────────────────────────────────────────────────────────────────

struct Rgb
{
    uint8_t r, g, b;
};
constexpr Rgb axis_colors[3] = {{235, 60, 70}, {125, 200, 40}, {50, 120, 245}};
constexpr Rgb hot_color{255, 225, 60};
constexpr Rgb view_color{225, 225, 225};

void line(const Vector3& a, const Vector3& b, uint32_t mode)
{
    gr_line_3d(&a, &b, mode);
}

// Lines are a pixel wide; `copies` more lines a pixel apart, across the view, thicken one.
void thick_line(const Layout& l, const Vector3& a, const Vector3& b, uint32_t mode, int copies)
{
    line(a, b, mode);
    const Vector3 view_dir = l.vp.perspective ? normalized(a - l.vp.eye) : l.vp.fwd;
    const Vector3 side = normalized(cross(b - a, view_dir)) * l.vp.units_per_px(a);
    for (int k = 1; k <= copies; k++) {
        const Vector3 off = side * static_cast<float>(k % 2 ? (k + 1) / 2 : -(k / 2));
        line(a + off, b + off, mode);
    }
}

void draw_screen_circle(const Layout& l, float radius, uint32_t mode)
{
    constexpr int segments = 48;
    Vector3 prev = l.pivot + l.right * radius;
    for (int n = 1; n <= segments; n++) {
        const float ang = two_pi * static_cast<float>(n) / segments;
        const Vector3 p = l.pivot + (l.right * std::cos(ang) + l.down * std::sin(ang)) * radius;
        line(prev, p, mode);
        prev = p;
    }
}

// `whole`: a dragged ring shows its back half too.
void draw_handle(const Layout& l, Handle h, bool hot, uint8_t alpha, uint32_t mode, bool whole)
{
    const bool white = h.kind == HandleKind::view || h.kind == HandleKind::view_ring || h.kind == HandleKind::trackball;
    const int color_axis = h.kind == HandleKind::view_ring ? l.view_ring_color : white ? -1 : h.axis;
    const Rgb c = hot ? hot_color : color_axis < 0 ? view_color : axis_colors[color_axis];
    set_draw_color(c.r, c.g, c.b, alpha);
    switch (h.kind) {
        case HandleKind::axis: {
            const Vector3 a = frame_axis(l.frame, h.axis);
            if (l.mode == Mode::scale) {
                // A box-ended line, the box lined up with the frame.
                const Vector3 tip = l.pivot + a * l.size;
                thick_line(l, l.pivot + a * (shaft_start * l.size), tip, mode, hot ? 2 : 1);
                const Vector3 b1 = frame_axis(l.frame, (h.axis + 1) % 3) * (scale_box_half * l.size);
                const Vector3 b2 = frame_axis(l.frame, (h.axis + 2) % 3) * (scale_box_half * l.size);
                const Vector3 ba = a * (scale_box_half * l.size);
                for (float sa : {-1.0f, 1.0f}) {
                    for (float s1 : {-1.0f, 1.0f}) {
                        line(tip + ba * sa + b1 * s1 - b2, tip + ba * sa + b1 * s1 + b2, mode);
                        line(tip + ba * sa - b1 + b2 * s1, tip + ba * sa + b1 + b2 * s1, mode);
                        line(tip - ba + b1 * sa + b2 * s1, tip + ba + b1 * sa + b2 * s1, mode);
                    }
                }
                break;
            }
            const Vector3 base = l.pivot + a * (cone_base * l.size);
            const Vector3 tip = l.pivot + a * l.size;
            thick_line(l, l.pivot + a * (shaft_start * l.size), base, mode, hot ? 2 : 1);
            const Vector3 s1 = any_perpendicular(a) * (cone_radius * l.size);
            const Vector3 s2 = normalized(cross(a, s1)) * (cone_radius * l.size);
            constexpr int cone_sides = 8;
            Vector3 prev = base + s1;
            for (int n = 1; n <= cone_sides; n++) {
                const float ang = two_pi * static_cast<float>(n) / cone_sides;
                const Vector3 ring = base + s1 * std::cos(ang) + s2 * std::sin(ang);
                line(prev, ring, mode);
                line(ring, tip, mode);
                prev = ring;
            }
            break;
        }
        case HandleKind::plane: {
            const std::array<Vector3, 4> q = plane_corners(l, h.axis);
            for (int i = 0; i < 4; i++) {
                thick_line(l, q[i], q[(i + 1) % 4], mode, hot ? 1 : 0);
            }
            if (hot) {
                for (float f : {0.25f, 0.5f, 0.75f}) {
                    line(q[0] + (q[1] - q[0]) * f, q[3] + (q[2] - q[3]) * f, mode);
                    line(q[0] + (q[3] - q[0]) * f, q[1] + (q[2] - q[1]) * f, mode);
                }
            }
            break;
        }
        case HandleKind::view:
            draw_screen_circle(l, view_circle_radius * l.size, mode);
            if (hot) {
                draw_screen_circle(l, view_circle_radius * 0.85f * l.size, mode);
            }
            break;
        case HandleKind::ring:
        case HandleKind::view_ring: {
            const RingPoints ring = ring_points(l, h);
            for (int n = 0; n < ring_segments; n++) {
                if (whole || ring.front[n]) {
                    thick_line(l, ring.p[n], ring.p[n + 1], mode, hot ? 2 : 1);
                }
            }
            break;
        }
        // Only drawn hot: the free-rotation area inside the rings.
        case HandleKind::trackball:
            draw_screen_circle(l, ring_radius * l.size, mode);
            draw_screen_circle(l, ring_radius * 0.97f * l.size, mode);
            break;
        default:
            break;
    }
}

// Every shown handle, or only `only` (a drag's, drawn whole even if the view no longer shows it).
void draw_handles(const Layout& l, Handle hot, const Handle* only, uint8_t alpha, uint32_t mode)
{
    if (only) {
        draw_handle(l, *only, true, alpha, mode, true);
        return;
    }
    auto draw = [&](Handle h) { draw_handle(l, h, hot == h, alpha, mode, false); };
    if (l.mode == Mode::rotate) {
        for (int i = 0; i < 3; i++) {
            if (l.ring_shown[i]) {
                draw({HandleKind::ring, i});
            }
        }
        if (l.view_ring_shown) {
            draw({HandleKind::view_ring, 0});
        }
        if (hot.kind == HandleKind::trackball) {
            draw(hot);
        }
        return;
    }
    for (int k = 0; k < 3; k++) {
        if (l.plane_shown[k]) {
            draw({HandleKind::plane, k});
        }
    }
    for (int i = 0; i < 3; i++) {
        if (l.axis_shown[i]) {
            draw({HandleKind::axis, i});
        }
    }
    if (l.view_shown) {
        draw({HandleKind::view, 0});
    }
}

// AF keeps D3D line lists open across draw calls (graphics.cpp), so lines queued in one depth mode would be drawn in
// the next unless the batch is flushed between them.
void flush_lines()
{
    if (gr_batch_open) {
        gr_flush_batch();
    }
}

// Over the 3D view, handles behind level geometry show dimmed: a dim pass ignoring depth, then a full one tested
// against it. The hovered or dragged handle is always drawn in full.
void draw_gizmo(const Layout& l, Handle hot, const Handle* only)
{
    flush_lines();
    if (l.two_d) {
        draw_handles(l, hot, only, 255, mode_overlay);
    }
    else {
        draw_handles(l, hot, only, occluded_alpha, mode_overlay);
        flush_lines();
        draw_handles(l, hot, only, 255, mode_depth_tested);
        flush_lines();
        if (only) {
            draw_handle(l, *only, true, 255, mode_overlay, true);
        }
        else if (hot.kind != HandleKind::none) {
            draw_handle(l, hot, true, 255, mode_overlay, false);
        }
    }
    flush_lines();
}

// ─── Drag ───────────────────────────────────────────────────────────────────

struct ObjectStart
{
    DedObject* obj;
    Vector3 pos;
    Matrix3 orient;
    bool has_extras;
    Extras extras;
};

struct BrushStart
{
    BrushNode* brush;
    Vector3 pos;
    Matrix3 orient;
    BrushShape shape; // Scale: its geometry at the press
};

// Copies a Shift drag made of the selection, to take out again if the drag comes to nothing.
struct Duplicate
{
    UndoEntry* paste_entry = nullptr; // the paste's create entry, holding every copy
    std::vector<DedObject*> original_selection;
    std::vector<BrushNode*> original_brushes;
};

struct Drag
{
    bool active = false;
    CDedLevel* level = nullptr;
    EditorViewport* view = nullptr;
    HWND hwnd = nullptr;
    Mode mode = Mode::move;
    bool perspective = false;
    bool local = false; // Coords: Local
    DedEditMode edit_mode = DedEditMode::Brush;
    Handle handle;
    Matrix3 frame;
    Vector3 pivot;        // at the press
    Vector3 snap_origin;  // the first target's position at the press, which a Global snap puts on the grid
    Vector3 plane_normal; // plane and view handles
    Vector3 view_fwd;     // the camera's at the press
    Vector3 grab;         // plane and view handles: where the press ray met the drag plane
    Vector3 grab_view;    // where the press ray met the view plane through the pivot
    float grab_s = 0.0f;  // axis handle: the press ray's nearest point, along the axis
    Pt grab_px;
    bool started = false; // the cursor left the drag dead zone around grab_px
    Pt axis_px_dir;       // axis handle: its unit direction on screen at the press, and pixels per unit
    float axis_px_per_unit = 0.0f;
    // Rings: the turn follows the cursor's angle around the pivot (sign: screen angle to turn), or for a ring seen
    // edge-on its travel along the ring's screen tangent at the grab point.
    Vector3 rot_axis;
    bool rot_along_tangent = false;
    float rot_sign = 1.0f;
    float last_screen_angle = 0.0f;
    float screen_turn = 0.0f;
    Pt tangent_px_dir;
    float tangent_px_per_rad = 0.0f;
    // Trackball: the camera's axes at the press, the rings' screen radius, the turn so far and the last cursor.
    Vector3 ball_right;
    Vector3 ball_up;
    Vector3 ball_toward;
    float ball_px = 0.0f;
    Quat ball_turn;
    Pt ball_last;
    // Scale: the cursor's distance from the pivot at the press (along an axis handle's screen direction), the
    // factor's range, and the targets' farthest point from the pivot.
    float scale_from = 1.0f;
    float factor_min = min_scale_factor;
    float factor_max = max_scale_factor;
    float reach = 0.0f;
    // Shift: the drag works on copies, made when the cursor first leaves the dead zone (so a still Shift click, or one
    // that ends in a cancel, touches no undo history); a cancel takes them out again.
    bool duplicate_pending = false;
    bool duplicated = false;
    Duplicate dup;
    std::vector<ObjectStart> objects;
    std::vector<BrushStart> brushes;
    // The whole selection at the press: a selection change mid-drag (a key, a menu) cancels it.
    std::vector<DedObject*> selection;
    std::vector<BrushNode*> selected_brushes;
    // A lone sun arrow: the sun's angles at the press.
    bool sun = false;
    SunArrowState sun_start;
    // The transform so far: a move, a turn about the pivot, or a scale about it along the frame axes.
    Vector3 delta;
    Vector3 turn_axis;
    float turn = 0.0f;
    std::array<float, 3> factors{1.0f, 1.0f, 1.0f};
    POINT last_cursor{LONG_MIN, LONG_MIN};
    bool last_snap = false;
    std::string saved_status;

    bool changed() const
    {
        if (!started) return false;
        switch (mode) {
            case Mode::rotate:
                return turn != 0.0f;
            case Mode::scale:
                return factors != std::array<float, 3>{1.0f, 1.0f, 1.0f};
            default:
                return !(delta == Vector3{});
        }
    }
};
Drag g_drag;

struct Hover
{
    EditorViewport* view = nullptr;
    Handle handle;
    POINT cursor{LONG_MIN, LONG_MIN};
    DWORD refreshed = 0;
};
Hover g_hover;

HWND status_bar_hwnd()
{
    const HWND bar = g_main_frame ? g_main_frame->status_bar_hwnd() : nullptr;
    return bar && IsWindow(bar) ? bar : nullptr;
}

bool selection_unchanged(const Drag& d)
{
    const CDedLevel& level = *d.level;
    if (level.edit_mode != d.edit_mode || level.selection.size != static_cast<int>(d.selection.size())) return false;
    for (int i = 0; i < level.selection.size; i++) {
        if (level.selection.data_ptr[i] != d.selection[i]) return false;
    }
    std::size_t n = 0;
    const bool differs = level.for_each_brush([&](const BrushNode& brush) {
        if (brush.state != BRUSH_STATE_SELECTED) return false;
        return n >= d.selected_brushes.size() || d.selected_brushes[n++] != &brush;
    });
    return !differs && n == d.selected_brushes.size();
}

bool object_alive(const CDedLevel& level, const DedObject* obj)
{
    for (const VArray<DedObject*>* list : {&level.selection, &level.master_objects}) {
        for (int i = 0; i < list->size; i++) {
            if (list->data_ptr[i] == obj) return true;
        }
    }
    return false;
}

bool brush_alive(const CDedLevel& level, const BrushNode* target)
{
    return level.for_each_brush([&](const BrushNode& brush) { return &brush == target; });
}

// The frame axes a move or scale handle works along: one, a plane's two, or all three for the view / uniform handle.
std::vector<int> handle_axes(const Drag& d)
{
    switch (d.handle.kind) {
        case HandleKind::axis:
            return {d.handle.axis};
        case HandleKind::plane:
            return {(d.handle.axis + 1) % 3, (d.handle.axis + 2) % 3};
        case HandleKind::view:
            return {0, 1, 2};
        default:
            return {};
    }
}

bool solve_move(const Drag& d, const EditorRay& ray, Pt cursor, Vector3& delta)
{
    if (d.handle.kind == HandleKind::axis) {
        const Vector3& a = frame_axis(d.frame, d.handle.axis);
        float s = 0.0f;
        if (!closest_on_axis(d.pivot, a, ray, d.perspective, s)) {
            if (d.axis_px_per_unit <= 0.0f) return false;
            const float px = (cursor.x - d.grab_px.x) * d.axis_px_dir.x + (cursor.y - d.grab_px.y) * d.axis_px_dir.y;
            s = d.grab_s + px / d.axis_px_per_unit;
        }
        delta = a * (s - d.grab_s);
    }
    else {
        Vector3 hit;
        if (ray_plane(ray, d.pivot, d.plane_normal, d.perspective, hit)) {
            delta = hit - d.grab;
        }
        else if (ray_plane(ray, d.pivot, d.view_fwd, d.perspective, hit)) {
            const Vector3 m = hit - d.grab_view;
            delta = m - d.plane_normal * dot(m, d.plane_normal);
        }
        else {
            return false;
        }
    }
    return is_finite(delta) && length(delta) <= max_drag_distance;
}

// Global: the first target's position lands on the world grid along each handle axis (all three for the view handle,
// which may shift it up to half a step in depth). Local: the move itself is whole grid steps along the local axes.
// Either way everything moves by the same delta.
Vector3 snap_delta(const Drag& d, const Vector3& delta, float grid)
{
    if (d.local) {
        Vector3 out = delta;
        for (int i : handle_axes(d)) {
            const Vector3& e = frame_axis(d.frame, i);
            const float c = dot(delta, e);
            out = out + e * (std::round(c / grid) * grid - c);
        }
        return out;
    }
    Vector3 p = d.snap_origin + delta;
    for (int i : handle_axes(d)) {
        const Vector3& e = frame_axis(d.frame, i);
        const float c = dot(p, e);
        p = p + e * (std::round(c / grid) * grid - c);
    }
    return p - d.snap_origin;
}

// The turn so far, from the press; `pivot_px` is where the pivot is on screen now.
bool solve_rotate(Drag& d, Pt pivot_px, Pt cursor, Vector3& axis, float& angle)
{
    if (d.handle.kind == HandleKind::trackball) {
        // Unbounded: each cursor step turns about the screen axis across it, onto the turn so far.
        const float dx = cursor.x - d.ball_last.x, dy = cursor.y - d.ball_last.y;
        d.ball_last = cursor;
        const float px = std::hypot(dx, dy);
        if (px > 0.0f && d.ball_px > 0.0f) {
            const Vector3 across = normalized(cross(d.ball_toward, d.ball_right * dx - d.ball_up * dy));
            d.ball_turn = Quat{across, px * trackball_rad_per_ring / d.ball_px} * d.ball_turn;
            d.ball_turn.normalize();
        }
        d.ball_turn.to_axis_angle(d.ball_toward, axis, angle);
        return std::isfinite(angle);
    }
    axis = d.rot_axis;
    if (d.rot_along_tangent) {
        if (d.tangent_px_per_rad <= 0.0f) return false;
        const float px = (cursor.x - d.grab_px.x) * d.tangent_px_dir.x + (cursor.y - d.grab_px.y) * d.tangent_px_dir.y;
        angle = px / d.tangent_px_per_rad;
        return std::isfinite(angle);
    }
    if (distance(cursor, pivot_px) >= min_angle_radius_px) {
        const float a = screen_angle(pivot_px, cursor);
        d.screen_turn += wrap_angle(a - d.last_screen_angle);
        d.last_screen_angle = a;
    }
    angle = d.rot_sign * d.screen_turn;
    return std::isfinite(angle);
}

// The scale factor so far: along an axis, the cursor's screen distance from the pivot over that at the press, and
// the same radially for a plane. The uniform (centre) handle is pressed on the pivot itself, so there a 100 px drag
// right or up doubles and left or down halves. Kept within [factor_min, factor_max], never flipped.
float solve_scale(const Drag& d, Pt pivot_px, Pt cursor)
{
    float f = 1.0f;
    switch (d.handle.kind) {
        case HandleKind::axis:
            f = ((cursor.x - pivot_px.x) * d.axis_px_dir.x + (cursor.y - pivot_px.y) * d.axis_px_dir.y) / d.scale_from;
            break;
        case HandleKind::plane:
            f = distance(cursor, pivot_px) / d.scale_from;
            break;
        case HandleKind::view: {
            const float doublings = ((cursor.x - d.grab_px.x) - (cursor.y - d.grab_px.y)) / uniform_px_per_double;
            f = std::exp2(std::clamp(doublings, -7.0f, 7.0f));
            break;
        }
        default:
            break;
    }
    return std::isfinite(f) ? std::clamp(f, d.factor_min, d.factor_max) : 1.0f;
}

// Growing snaps to 1 + k * step and shrinking to its mirror 1 / (1 + k * step), so a step up and a step down undo
// each other and no step reaches zero.
float snap_factor(float f, float step)
{
    if (f >= 1.0f) return 1.0f + std::round((f - 1.0f) / step) * step;
    return 1.0f / (1.0f + std::round((1.0f / f - 1.0f) / step) * step);
}

// The frame-axis scale of a vector.
Vector3 scale_vector(const Drag& d, const Vector3& v)
{
    Vector3 out;
    for (int i = 0; i < 3; i++) {
        const Vector3& e = frame_axis(d.frame, i);
        out = out + e * (dot(e, v) * d.factors[i]);
    }
    return out;
}

Vector3 scale_point(const Drag& d, const Vector3& p)
{
    return d.pivot + scale_vector(d, p - d.pivot);
}

// How much the scale stretches a unit direction.
float scale_along(const Drag& d, const Vector3& dir)
{
    return length(scale_vector(d, dir));
}

bool uniform_scale(const Drag& d)
{
    return d.factors[0] == d.factors[1] && d.factors[1] == d.factors[2];
}

// An object's sizes under the drag's scale: meshes and radii follow a uniform scale only; boxes, and a directional
// light's cylinder length, stretch by how much the frame-axis scale stretches their own axes.
Extras scaled_extras(const Drag& d, DedObject& obj, const Extras& start)
{
    Extras e = start;
    const bool uniform = uniform_scale(d);
    if (obj.type == DedObjectType::DED_MESH) {
        if (uniform) {
            e.draw_scale = alpine_mesh_scale::sanitize(start.draw_scale * d.factors[0]);
        }
        return e;
    }
    const Volume v = volume_of(obj);
    // Only the fields the current shape uses: a box's unused radius comes back unscaled if the shape changes.
    if (v.radius && uniform && (v.kind == Volume::Kind::sphere || v.cylinder)) {
        e.radius = start.radius * d.factors[0];
    }
    if (v.cylinder) {
        e.size[1] = start.size[1] * scale_along(d, normalized(obj.orient.fvec));
    }
    else if (v.kind == Volume::Kind::box) {
        const Matrix3& own = v.own_axes_set ? v.own_axes : v.world_axes ? identity_orient : obj.orient;
        for (int j = 0; j < 3; j++) {
            e.size[j] = start.size[j] * scale_along(d, normalized(frame_axis(own, j)));
        }
    }
    return e;
}

// A directional light's box turns with a pure world-Y yaw (a Global Y ring, or any turn about +-Y), as Mirror keeps it
// (geometry.cpp); other turns leave box_yaw alone.
Extras turned_extras(const Drag& d, DedObject& obj, const Extras& start)
{
    Extras e = start;
    if (obj.type != DedObjectType::DED_DIRECTIONAL_LIGHT) return e;
    const float up = dot(d.turn_axis, world_axis(1));
    if (std::abs(up) > 0.9999f) {
        const float yaw = (up > 0.0f ? d.turn : -d.turn) * deg_per_rad;
        e.box_yaw = alpine_dir_light::normalize_degrees(start.box_yaw + yaw);
    }
    return e;
}

// A brush's vertices under the scale, kept in its own unchanged frame around its new position; or its press shape
// back verbatim with `scaled` false.
void place_brush_vertices(const Drag& d, const BrushStart& b, bool scaled)
{
    auto* solid = static_cast<GSolid*>(b.brush->geometry);
    if (!solid || !shape_fits(*solid, b.shape)) return;
    if (!scaled) {
        write_brush_shape(*solid, b.shape);
        return;
    }
    const Matrix3& m = b.orient;
    for (int i = 0; i < solid->vertices.size; i++) {
        if (GVertex* v = solid->vertices.data_ptr[i]) {
            const Vector3 w = scale_point(d, m * b.shape.verts[i] + b.pos) - b.brush->pos;
            v->pos = {dot(w, m.rvec), dot(w, m.uvec), dot(w, m.fvec)};
        }
    }
    refresh_solid(*solid);
}

// Each target back to its press pose, or given the drag's transform.
void apply_pose(const Drag& d, bool transformed, bool notify)
{
    const Rotation rot{d.turn_axis, d.turn};
    auto pose_pos = [&](const Vector3& pos) {
        if (!transformed) return pos;
        switch (d.mode) {
            case Mode::rotate:
                return d.pivot + rot.apply(pos - d.pivot);
            case Mode::scale:
                return scale_point(d, pos);
            default:
                return pos + d.delta;
        }
    };
    for (const ObjectStart& o : d.objects) {
        o.obj->pos = pose_pos(o.pos);
        if (d.mode == Mode::rotate) {
            o.obj->orient = transformed ? rot.apply(o.orient) : o.orient;
        }
        if (o.has_extras && d.mode != Mode::move) {
            Extras e = o.extras;
            if (transformed) {
                e = d.mode == Mode::scale ? scaled_extras(d, *o.obj, o.extras) : turned_extras(d, *o.obj, o.extras);
            }
            write_extras(*o.obj, e);
        }
        if (notify && ded_object_updaters_safe(*o.obj)) {
            ded_object_pos_changed(o.obj);
            if (d.mode == Mode::rotate) {
                ded_object_orient_changed(o.obj);
            }
        }
    }
    for (const BrushStart& b : d.brushes) {
        b.brush->pos = pose_pos(b.pos);
        if (d.mode == Mode::rotate) {
            b.brush->orient = transformed ? rot.apply(b.orient) : b.orient;
        }
        if (d.mode == Mode::scale) {
            place_brush_vertices(d, b, transformed);
        }
    }
}

const char* handle_name(const Drag& d)
{
    switch (d.handle.kind) {
        case HandleKind::view_ring:
            for (int i = 0; i < 3; i++) {
                if (std::abs(dot(world_axis(i), d.rot_axis)) > 0.9999f) return axis_names[i];
            }
            return "View";
        case HandleKind::trackball:
            return "Free";
        default:
            return axis_names[d.handle.axis];
    }
}

void show_status(const Drag& d, bool snap, float step)
{
    const HWND bar = status_bar_hwnd();
    if (!bar) return;
    std::string text = d.duplicated ? "Duplicate + " : "";
    if (d.mode == Mode::rotate) {
        text += std::format("Rotate {} {:+.1f}", handle_name(d), d.turn * deg_per_rad) + degree_sign;
        if (snap) {
            text += std::format("  (snap {:g}", step * deg_per_rad) + degree_sign + ")";
        }
    }
    else if (d.mode == Mode::scale) {
        text += "Scale";
        if (d.handle.kind == HandleKind::view) {
            text += std::format(" {}{:.2f}", times_sign, d.factors[0]);
        }
        else {
            for (int i : handle_axes(d)) {
                text += std::format(" {} {}{:.2f}", axis_names[i], times_sign, d.factors[i]);
            }
        }
        if (snap) {
            text += std::format("  (snap {:g})", step);
        }
    }
    else {
        text += "Move";
        for (int i : handle_axes(d)) {
            text += std::format(" {} {:+.2f}", axis_names[i], dot(d.delta, frame_axis(d.frame, i)));
        }
        text += " m";
        if (snap) {
            text += std::format("  (snap {:g} m)", step);
        }
    }
    SetWindowTextA(bar, text.c_str());
}

// ─── Undo side records ──────────────────────────────────────────────────────

// What a drag changes beside pos / orient, which the stock move record lacks, goes in a side record found through an
// undo link right after the entry's move records. A record whose link no stack holds any more is dropped.
// The magic lies where a move record keeps an orient float, which a unit row never reaches (this is about 1.4e16).
constexpr uint32_t gizmo_undo_link_magic = 0x5A49474D;

struct ObjectChange
{
    DedObject* obj;
    Extras before;
    Extras after;
};

struct BrushChange
{
    BrushNode* brush;
    const GSolid* solid;
    BrushShape before;
    BrushShape after;
};

struct SideRecord
{
    uint32_t serial = 0;
    std::vector<ObjectChange> objects;
    std::vector<BrushChange> brushes;
    bool sun = false;
    SunArrowState sun_before;
    SunArrowState sun_after;

    bool empty() const
    {
        return objects.empty() && brushes.empty() && !sun;
    }
};
std::vector<SideRecord> g_side_records;
uint32_t g_next_side_serial = 1;

bool entry_may_carry_link(const UndoEntry* entry)
{
    return entry && (entry->type == undo_transform_objects || entry->type == undo_transform_brushes ||
                     entry->type == undo_transform_group);
}

int move_record_count(const UndoEntry& entry)
{
    return entry.objects.size + entry.brushes.size;
}

// The link follows the entry's move records, one per object and brush, and nothing follows it.
uint32_t link_serial(const UndoEntry* entry)
{
    if (!entry_may_carry_link(entry)) return 0;
    const int records = move_record_count(*entry);
    if (records < 0 || entry->raw_blocks.size != records + 1) return 0;
    return static_cast<uint32_t>(undo_link_value(*entry, records, gizmo_undo_link_magic));
}

void prune_side_records(const CDedLevel& level)
{
    std::vector<uint32_t> live;
    for (const VArray<UndoEntry*>* stack : {&level.undo_stack, &level.redo_stack}) {
        for (int i = 0; i < stack->size; i++) {
            if (const uint32_t serial = link_serial(stack->data_ptr[i])) {
                live.push_back(serial);
            }
        }
    }
    std::erase_if(g_side_records, [&](const SideRecord& r) {
        return std::find(live.begin(), live.end(), r.serial) == live.end();
    });
}

// Ties `record` to `entry`, which finish_transform has just filled.
void attach_side_record(CDedLevel& level, UndoEntry* entry, SideRecord&& record)
{
    if (!entry_may_carry_link(entry) || record.empty()) return;
    if (entry->raw_blocks.size != move_record_count(*entry)) {
        xlog::warn("[Gizmo] the undo step has {} blocks for {} records: its Alpine changes will not undo",
                   entry->raw_blocks.size, move_record_count(*entry));
        return;
    }
    prune_side_records(level);
    const uint32_t serial = g_next_side_serial;
    record.serial = serial;
    g_side_records.push_back(std::move(record));
    if (!undo_link_append(*entry, gizmo_undo_link_magic, serial)) {
        g_side_records.pop_back();
        return;
    }
    g_next_side_serial++;
}

// Only while the brush still owns that GSolid; write_brush_shape also checks the counts.
void write_brush_change(BrushNode& brush, const GSolid* solid, const BrushShape& shape)
{
    auto* live = static_cast<GSolid*>(brush.geometry);
    if (live && live == solid) {
        write_brush_shape(*live, shape);
    }
}

// What the drag changed beside pos / orient, before and after, for the side record.
SideRecord side_record_for(const Drag& d)
{
    SideRecord r;
    for (const ObjectStart& o : d.objects) {
        if (o.has_extras) {
            const Extras after = read_extras(*o.obj);
            if (!(after == o.extras)) {
                r.objects.push_back({o.obj, o.extras, after});
            }
        }
    }
    if (d.mode == Mode::scale) {
        for (const BrushStart& b : d.brushes) {
            const auto* solid = static_cast<const GSolid*>(b.brush->geometry);
            if (solid && shape_fits(*solid, b.shape)) {
                r.brushes.push_back({b.brush, solid, b.shape, read_brush_shape(*solid)});
            }
        }
    }
    return r;
}

// ─── Drag lifecycle ─────────────────────────────────────────────────────────

// Releases what the drag holds: capture, the status bar message.
void end_drag_ui(const Drag& d)
{
    if (const HWND bar = status_bar_hwnd()) {
        SetWindowTextA(bar, d.saved_status.c_str());
    }
    if (d.hwnd && GetCapture() == d.hwnd) {
        ReleaseCapture();
    }
    editor_views_mark_repaint_all();
}

Drag take_drag()
{
    Drag d = std::move(g_drag);
    g_drag = Drag{};
    return d;
}

// The press pose back, bit for bit, and the sun's angles with it so no float round trip shows.
void restore_start(const Drag& d)
{
    apply_pose(d, false, true);
    if (d.sun) {
        sun_arrow_restore(d.level, d.sun_start);
    }
}

// Takes a Shift drag's copies out again by undoing the paste's create entry, which holds them all, then freeing it
// with the redo stack, where it is then the only entry (0x0041C360 frees the copies). The originals are selected again.
void remove_duplicates(CDedLevel& level, const Duplicate& dup)
{
    if (dup.paste_entry && undo_stack_top(level.undo_stack) == dup.paste_entry) {
        level.undo();
        if (undo_stack_top(level.redo_stack) == dup.paste_entry) {
            level.clear_redo_stack();
        }
    }
    else if (dup.paste_entry) {
        xlog::warn("[Gizmo] the duplicated objects stay: their paste is no longer the last undo step");
    }
    level.clear_selection();
    for (DedObject* obj : dup.original_selection) {
        if (object_alive(level, obj)) {
            level.add_to_selection(obj);
        }
    }
    level.for_each_brush([&](BrushNode& brush) {
        const bool original =
            std::find(dup.original_brushes.begin(), dup.original_brushes.end(), &brush) != dup.original_brushes.end();
        if (original) {
            brush.state = BRUSH_STATE_SELECTED;
        }
        else if (brush.state == BRUSH_STATE_SELECTED) {
            brush.state = BRUSH_STATE_NORMAL;
        }
    });
    editor_views_mark_repaint_all();
}

// Puts back what the drag moved (with `check_alive`, only what is still in the level) and takes out a Shift drag's
// copies. Commands, Undo / Redo and loads cancel a drag first (main.cpp), so only a selection change can leave it
// holding something gone, and the level's lists say what is.
void cancel_drag(bool check_alive)
{
    Drag d = take_drag();
    end_drag_ui(d);
    if (!d.level) return;
    if (check_alive) {
        std::erase_if(d.objects, [&](const ObjectStart& o) { return !object_alive(*d.level, o.obj); });
        std::erase_if(d.brushes, [&](const BrushStart& b) { return !brush_alive(*d.level, b.brush); });
    }
    restore_start(d);
    if (d.duplicated) {
        remove_duplicates(*d.level, d.dup);
    }
}

// One stock undo step per drag: begin (0x00426A30) records the restored press pose, finish_transform the re-applied
// result, and a side record what the move record lacks. transform_in_progress stays clear during the drag, so RED's
// Ctrl / Shift finalize (0x0047BD57) leaves it alone.
void commit_drag()
{
    // It allocates, so it is built while a throw still cancels the drag.
    SideRecord record;
    if (g_drag.changed()) {
        record = side_record_for(g_drag);
        if (g_drag.sun) {
            record.sun = true;
            record.sun_before = g_drag.sun_start;
            record.sun_after = sun_arrow_state_toward(g_drag.level, g_drag.objects.front().obj->orient.fvec);
        }
    }
    const Drag d = take_drag();
    end_drag_ui(d);
    if (!d.changed()) {
        // A turn or scale that came back to nothing leaves rounding behind otherwise.
        if (d.started) {
            restore_start(d);
        }
        // A Shift click that never moved duplicates nothing.
        if (d.duplicated) {
            remove_duplicates(*d.level, d.dup);
        }
        return;
    }
    CDedLevel* level = d.level;
    restore_start(d);
    level->commit_pending_transform();
    const UndoEntry* top_before = undo_stack_top(level->undo_stack);
    level->begin_transform();
    apply_pose(d, true, true);
    if (record.sun) {
        sun_arrow_restore(level, record.sun_after);
    }
    // Finalize writes "after" into whatever entry is on top, so it runs only over the entry begin pushed.
    UndoEntry* entry = undo_stack_top(level->undo_stack);
    if (level->transform_in_progress && entry != top_before) {
        level->finish_transform();
        attach_side_record(*level, entry, std::move(record));
    }
    else {
        level->transform_in_progress = false;
        xlog::warn("[Gizmo] the transform was applied without an undo step");
    }
    // The brush finalize leaves this to a closed clip dialog (0x00427433).
    if (!d.brushes.empty()) {
        level->mark_geometry_dirty();
    }
    mark_level_modified();
}

// The handle under a view's client point, set up for that view.
bool handle_under(CDedLevel& level, EditorViewport* view, Pt cursor, Targets& targets, Placement& placement,
                  Layout& l, Handle& handle)
{
    if (!gizmo_shown(level, targets, placement)) return false;
    view->setup_gr(0);
    if (!make_layout(*view, placement, l)) return false;
    handle = hit_test(l, cursor);
    return handle.kind != HandleKind::none;
}

// The axis handle's direction on screen at the press, and its pixels per world unit; false when it has none.
bool measure_axis_on_screen(Drag& d, const Layout& l)
{
    const Vector3& a = frame_axis(d.frame, d.handle.axis);
    Pt p0, p1;
    if (project_segment(l.vp, d.pivot, d.pivot + a * l.size, p0, p1)) {
        const float len = distance(p0, p1);
        if (len > 0.0f) {
            d.axis_px_dir = {(p1.x - p0.x) / len, (p1.y - p0.y) / len};
            d.axis_px_per_unit = len / l.size;
        }
    }
    return d.axis_px_per_unit > 0.0f;
}

bool begin_move(Drag& d, const Layout& l, const EditorRay& ray)
{
    if (d.handle.kind == HandleKind::axis) {
        if (!measure_axis_on_screen(d, l)) return false;
        if (!closest_on_axis(d.pivot, frame_axis(d.frame, d.handle.axis), ray, d.perspective, d.grab_s)) {
            d.grab_s = 0.0f;
        }
    }
    else {
        d.plane_normal = d.handle.kind == HandleKind::plane ? frame_axis(d.frame, d.handle.axis) : d.view_fwd;
        if (!ray_plane(ray, d.pivot, d.plane_normal, d.perspective, d.grab)) {
            d.grab = d.grab_view;
        }
    }
    return true;
}

bool begin_scale(Drag& d, const Layout& l, Pt cursor)
{
    constexpr float min_from_px = 4.0f;
    if (d.handle.kind == HandleKind::axis) {
        if (!measure_axis_on_screen(d, l)) return false;
        const float s = (cursor.x - l.pivot_px.x) * d.axis_px_dir.x + (cursor.y - l.pivot_px.y) * d.axis_px_dir.y;
        d.scale_from = std::abs(s) >= min_from_px ? s : min_from_px;
    }
    else if (d.handle.kind == HandleKind::plane) {
        d.scale_from = std::max(distance(cursor, l.pivot_px), min_from_px);
    }
    return true;
}

// After the targets are in: the uniform factor stays where every mesh's draw scale can follow it, so positions and
// sizes agree; and how far a target reaches from the pivot, to drop a scale that would throw it out of range.
void measure_scale_targets(Drag& d)
{
    for (const ObjectStart& o : d.objects) {
        d.reach = std::max(d.reach, length(o.pos - d.pivot));
        if (d.handle.kind == HandleKind::view && o.obj->type == DedObjectType::DED_MESH && o.extras.draw_scale > 0.0f) {
            d.factor_min = std::max(d.factor_min, alpine_mesh_scale::min_scale / o.extras.draw_scale);
            d.factor_max = std::min(d.factor_max, alpine_mesh_scale::max_scale / o.extras.draw_scale);
        }
    }
    for (const BrushStart& b : d.brushes) {
        d.reach = std::max(d.reach, length(b.pos - d.pivot));
        for (const Vector3& v : b.shape.verts) {
            d.reach = std::max(d.reach, length(b.orient * v + b.pos - d.pivot));
        }
    }
    d.factor_max = std::max(d.factor_max, d.factor_min);
}

void begin_rotate(Drag& d, const Layout& l, Pt cursor)
{
    if (d.handle.kind == HandleKind::trackball) {
        d.ball_right = l.right;
        d.ball_up = l.down * -1.0f;
        d.ball_toward = l.to_viewer;
        d.ball_px = l.trackball_px;
        d.ball_last = cursor;
        d.turn_axis = d.ball_toward;
        return;
    }
    d.rot_axis = ring_axis(l, d.handle);
    d.turn_axis = d.rot_axis;
    d.last_screen_angle = screen_angle(l.pivot_px, cursor);
    // The ring point nearest the cursor, and the screen direction it travels in on a positive turn.
    const RingPoints ring = ring_points(l, d.handle);
    Vector3 grab = ring.p[0];
    float best = INFINITY;
    for (int n = 0; n < ring_segments; n++) {
        Pt p;
        if (ring.front[n] && l.vp.project(ring.p[n], p) && distance(p, cursor) < best) {
            best = distance(p, cursor);
            grab = ring.p[n];
        }
    }
    const Vector3 radial = grab - d.pivot;
    const Vector3 tangent = normalized(cross(d.rot_axis, radial));
    constexpr float step = 0.05f; // radians
    Pt p0, p1;
    const bool projects = l.vp.project(grab, p0) && l.vp.project(grab + tangent * (length(radial) * step), p1);
    const float len = projects ? distance(p0, p1) : 0.0f;
    if (len > 0.5f) {
        d.tangent_px_dir = {(p1.x - p0.x) / len, (p1.y - p0.y) / len};
        d.tangent_px_per_rad = len / step;
        const float turn = wrap_angle(screen_angle(l.pivot_px, p1) - screen_angle(l.pivot_px, p0));
        d.rot_sign = turn >= 0.0f ? 1.0f : -1.0f;
    }
    else {
        // Moving straight at the camera there: drag across the axis' line on screen, a ring radius per radian.
        Pt a0, a1;
        if (project_segment(l.vp, d.pivot, d.pivot + d.rot_axis * l.size, a0, a1) && distance(a0, a1) > 0.5f) {
            const float alen = distance(a0, a1);
            d.tangent_px_dir = {-(a1.y - a0.y) / alen, (a1.x - a0.x) / alen};
        }
        else {
            d.tangent_px_dir = {1.0f, 0.0f};
        }
        d.tangent_px_per_rad = ring_radius * arrow_px;
    }
    d.rot_along_tangent = std::abs(dot(d.rot_axis, l.to_viewer)) < edge_on_ring_cos || len <= 0.5f;
}

// The drag's targets with their press state, and the selection to notice a change of.
void capture_targets(Drag& d, const Targets& targets)
{
    CDedLevel& level = *d.level;
    d.objects.clear();
    d.brushes.clear();
    d.sun = false;
    for (DedObject* obj : targets.objects) {
        const bool extras = has_extras(*obj);
        d.objects.push_back({obj, obj->pos, obj->orient, extras, extras ? read_extras(*obj) : Extras{}});
        d.sun = d.sun || obj->type == DedObjectType::DED_SUN_ARROW;
    }
    for (BrushNode* brush : targets.brushes) {
        BrushStart start{brush, brush->pos, brush->orient, {}};
        const auto* solid = static_cast<const GSolid*>(brush->geometry);
        if (d.mode == Mode::scale && solid) {
            start.shape = read_brush_shape(*solid);
        }
        d.brushes.push_back(std::move(start));
    }
    if (d.sun) {
        d.sun_start = sun_arrow_state(&level);
    }
    if (d.mode == Mode::scale) {
        d.reach = 0.0f;
        d.factor_min = min_scale_factor;
        d.factor_max = max_scale_factor;
        measure_scale_targets(d);
    }
    d.snap_origin = d.objects.empty() ? d.brushes.front().pos : d.objects.front().pos;
    d.selection.assign(level.selection.data_ptr, level.selection.data_ptr + std::max(level.selection.size, 0));
    d.selected_brushes = level.selected_brushes();
}

bool begin_drag(CDedLevel& level, EditorViewport* view, Pt cursor)
{
    const HWND hwnd = editor_view_hwnd(view);
    Targets targets;
    Placement placement;
    Layout l;
    Handle handle;
    if (!hwnd || !handle_under(level, view, cursor, targets, placement, l, handle)) return false;

    Drag d;
    d.level = &level;
    d.view = view;
    d.hwnd = hwnd;
    d.mode = l.mode;
    d.perspective = l.vp.perspective;
    d.local = level.coords_local != 0;
    d.edit_mode = level.edit_mode;
    d.handle = handle;
    d.frame = l.frame;
    d.pivot = placement.pivot;
    d.view_fwd = l.vp.fwd;
    d.grab_px = cursor;
    const EditorRay ray = editor_screen_ray(cursor.x, cursor.y);
    if (!ray_plane(ray, d.pivot, d.view_fwd, d.perspective, d.grab_view)) {
        d.grab_view = d.pivot;
    }
    bool measured = true;
    switch (d.mode) {
        case Mode::rotate:
            begin_rotate(d, l, cursor);
            break;
        case Mode::scale:
            measured = begin_scale(d, l, cursor);
            break;
        default:
            measured = begin_move(d, l, ray);
            break;
    }
    if (!measured) return false;
    capture_targets(d, targets);
    d.last_cursor = {static_cast<LONG>(cursor.x), static_cast<LONG>(cursor.y)};
    d.last_snap = snapping();
    if (const HWND bar = status_bar_hwnd()) {
        char text[256] = {};
        GetWindowTextA(bar, text, sizeof(text));
        d.saved_status = text;
    }
    d.active = true;
    g_drag = std::move(d);
    g_hover = Hover{};
    SetCapture(hwnd);
    editor_views_mark_repaint_all();
    return true;
}

// Something Ctrl+C would copy: keyframes and the sun arrow never are.
bool copyable_selection(const Targets& targets)
{
    return !targets.brushes.empty() ||
           std::any_of(targets.objects.begin(), targets.objects.end(), [](const DedObject* obj) {
               return obj->type != DedObjectType::DED_KEYFRAME && obj->type != DedObjectType::DED_SUN_ARROW;
           });
}

// Shift on a handle drags copies of the selection, made once the drag really starts; with nothing copyable selected it
// is a plain drag. Shift anywhere else stays stock's camera.
bool begin_duplicate_drag(CDedLevel& level, EditorViewport* view, Pt cursor)
{
    if (!begin_drag(level, view, cursor)) return false;
    Targets targets;
    targets.objects.reserve(g_drag.objects.size());
    for (const ObjectStart& o : g_drag.objects) {
        targets.objects.push_back(o.obj);
    }
    for (const BrushStart& b : g_drag.brushes) {
        targets.brushes.push_back(b.brush);
    }
    g_drag.duplicate_pending = copyable_selection(targets);
    return true;
}

// Moves the drag onto copies of exactly its targets, landing on them; the press pivot, frame and grab stay even if a
// copy was refused. False when the drag ended instead. `releasing`: run from the button's own release.
bool make_duplicates(Drag& d, bool releasing)
{
    CDedLevel& level = *d.level;
    d.duplicate_pending = false;
    Duplicate dup;
    dup.original_selection = d.selection;
    dup.original_brushes = d.selected_brushes;
    // Ctrl+C copies the whole selection: an object the drag leaves alone (terrain in Rotate / Scale) must not be
    // copied. Brushes are all targets in the modes that copy them.
    level.clear_selection();
    for (const ObjectStart& o : d.objects) {
        level.add_to_selection(o.obj);
    }
    dup.paste_entry = alpine_duplicate_selection(&level);
    // A message box during the paste (a terrain over its limits) runs a message loop: a release it let through may
    // already have ended the drag, or it swallowed the release.
    if (!g_drag.active) {
        remove_duplicates(level, dup);
        return false;
    }
    d.dup = std::move(dup);
    d.duplicated = true;
    if (!releasing && GetKeyState(VK_LBUTTON) >= 0) {
        cancel_drag(false);
        return false;
    }
    const Targets copies = collect_targets(level, mode_cap(d.mode));
    if (copies.empty()) {
        cancel_drag(false);
        return false;
    }
    capture_targets(d, copies);
    return true;
}

void drag_update(bool force)
{
    Drag& d = g_drag;
    POINT cursor{};
    if (!GetCursorPos(&cursor) || !ScreenToClient(d.hwnd, &cursor)) return;
    const bool snap = snapping();
    if (!force && cursor.x == d.last_cursor.x && cursor.y == d.last_cursor.y && snap == d.last_snap) return;
    d.last_cursor = cursor;
    d.last_snap = snap;
    const Pt px{static_cast<float>(cursor.x), static_cast<float>(cursor.y)};
    // A click that wobbles a pixel or two changes nothing and records no undo step.
    if (!d.started) {
        if (std::abs(px.x - d.grab_px.x) <= GetSystemMetrics(SM_CXDRAG) &&
            std::abs(px.y - d.grab_px.y) <= GetSystemMetrics(SM_CYDRAG)) {
            return;
        }
        d.started = true;
        if (d.duplicate_pending && !make_duplicates(d, force)) return;
    }
    d.view->setup_gr(0);
    ViewProj vp;
    Pt pivot_px;
    if (d.mode != Mode::move && (!current_view_proj(vp) || !vp.project(d.pivot, pivot_px))) return;
    bool snapped = false;
    float step = 0.0f;
    if (d.mode == Mode::rotate) {
        Vector3 axis;
        float angle = 0.0f;
        if (!solve_rotate(d, pivot_px, px, axis, angle)) return;
        step = d.level->rotate_step;
        snapped = snap && d.handle.kind != HandleKind::trackball && step > 1e-6f && std::isfinite(step);
        if (snapped) {
            angle = std::round(angle / step) * step;
        }
        if (axis == d.turn_axis && angle == d.turn) return;
        d.turn_axis = axis;
        d.turn = angle;
    }
    else if (d.mode == Mode::scale) {
        float f = solve_scale(d, pivot_px, px);
        step = scale_steps[std::clamp(g_settings.scale_step, 0, static_cast<int>(scale_steps.size()) - 1)];
        snapped = snap;
        if (snapped) {
            f = std::clamp(snap_factor(f, step), d.factor_min, d.factor_max);
        }
        if (d.reach * f > max_drag_distance) return;
        std::array<float, 3> factors{1.0f, 1.0f, 1.0f};
        for (int i : handle_axes(d)) {
            factors[i] = f;
        }
        if (factors == d.factors) return;
        d.factors = factors;
    }
    else {
        Vector3 delta;
        if (!solve_move(d, editor_screen_ray(px.x, px.y), px, delta)) return;
        step = d.level->grid_size;
        snapped = snap && step > 1e-6f && std::isfinite(step);
        if (snapped) {
            delta = snap_delta(d, delta, step);
        }
        if (delta == d.delta) return;
        d.delta = delta;
    }
    apply_pose(d, true, true);
    show_status(d, snapped, step);
    editor_views_mark_repaint_all();
}

// Ends the drag when the selection or the main frame changed under it, or a stock transform began; true when it did.
bool settle_broken_drag()
{
    if (!selection_unchanged(g_drag) || g_drag.level->transform_in_progress) {
        cancel_drag(true);
        return true;
    }
    if (!IsWindowEnabled(GetMainFrameHandle())) {
        cancel_drag(false);
        return true;
    }
    return false;
}

void drag_tick()
{
    if (settle_broken_drag()) return;
    // A held transform key would start stock input on the selection (the keys themselves are dropped meanwhile, see
    // view_input_key_hook); losing capture (Alt+Tab, a window taking it) leaves the button pressed.
    if (g_dinput_keys[DIK_ESCAPE] || editor_transform_key_held() || GetCapture() != g_drag.hwnd) {
        cancel_drag(false);
        return;
    }
    drag_update(false);
}

// ─── Hover and hotkeys ──────────────────────────────────────────────────────

void set_hover(EditorViewport* view, Handle handle)
{
    if (view == g_hover.view && handle == g_hover.handle) return;
    editor_view_mark_repaint(g_hover.view);
    editor_view_mark_repaint(view);
    g_hover.view = view;
    g_hover.handle = handle;
}

bool view_latched(const EditorViewport& view)
{
    return view.lbutton_held || view.rbutton_held || view.mbutton_held;
}

void hover_tick()
{
    POINT cursor{};
    if (!GetCursorPos(&cursor)) return;
    const DWORD now = GetTickCount();
    if (cursor.x == g_hover.cursor.x && cursor.y == g_hover.cursor.y && now - g_hover.refreshed < hover_refresh_ms) {
        return;
    }
    g_hover.cursor = cursor;
    g_hover.refreshed = now;
    auto* view = static_cast<EditorViewport*>(editor_view_under_cursor(cursor));
    CDedLevel* level = CDedLevel::Get();
    const HWND main = GetMainFrameHandle();
    Handle handle;
    POINT client = cursor;
    if (view && level && main && IsWindowEnabled(main) && GetForegroundWindow() == main && !view_latched(*view) &&
        !editor_transform_key_held() && ScreenToClient(editor_view_hwnd(view), &client)) {
        Targets targets;
        Placement placement;
        Layout l;
        handle_under(*level, view, {static_cast<float>(client.x), static_cast<float>(client.y)}, targets, placement,
                     l, handle);
    }
    set_hover(handle.kind == HandleKind::none ? nullptr : view, handle);
}

// Every view redraws the gizmo, and the hover is found again.
void refresh_gizmo()
{
    set_hover(nullptr, {});
    g_hover.refreshed = 0;
    editor_views_mark_repaint_all();
}

void set_mode(Mode mode)
{
    if (g_drag.active || mode == g_settings.mode) return;
    g_settings.mode = mode;
    sync_top_bar();
    refresh_gizmo();
}

// Coords can change without a repaint (the G key, 0x00448990).
int g_shown_coords_local = -1;

void coords_tick()
{
    const CDedLevel* level = CDedLevel::Get();
    if (!level || level->coords_local == g_shown_coords_local) return;
    g_shown_coords_local = level->coords_local;
    refresh_gizmo();
}

void toggle_snap()
{
    g_settings.snap = !g_settings.snap;
    sync_top_bar();
}

bool focus_on_views()
{
    const HWND focus = GetFocus();
    if (focus && focus == GetMainFrameHandle()) return true;
    for (int i = 0; focus && i < editor_num_views; i++) {
        if (editor_view_hwnd(editor_view_at(i)) == focus) return true;
    }
    return false;
}

// Typed keys belong to edit controls (an editable combo's text box is one), rich edits and combo boxes (a drop-down
// list selects by typed character), so no hotkey takes them there. A tree or list box gets the hotkey instead, its
// type-ahead losing the digit (hotkey_msg_proc).
bool typing_in(HWND hwnd)
{
    char cls[32] = {};
    if (!hwnd || !GetClassNameA(hwnd, cls, sizeof(cls))) return false;
    return _stricmp(cls, "Edit") == 0 || _strnicmp(cls, "RichEdit", 8) == 0 || _stricmp(cls, "ComboBox") == 0 ||
           _stricmp(cls, "ComboLBox") == 0;
}

// RED is in front with no modal dialog or menu open, and the keyboard focus is in its main frame or a window it owns,
// and not in a text field or list (a menu's mnemonics, such as the recent files' 1, take the digits).
bool hotkeys_usable(HWND focus)
{
    const HWND main = GetMainFrameHandle();
    const HWND front = GetForegroundWindow();
    GUITHREADINFO gui{};
    gui.cbSize = sizeof(gui);
    if (GetGUIThreadInfo(GetCurrentThreadId(), &gui) &&
        (gui.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE))) {
        return false;
    }
    return main && IsWindowEnabled(main) && front && GetAncestor(front, GA_ROOTOWNER) == main && focus &&
           GetAncestor(focus, GA_ROOTOWNER) == main && !typing_in(focus);
}

void run_hotkey(int index)
{
    if (index < mode_count) {
        set_mode(static_cast<Mode>(index));
    }
    else {
        toggle_snap();
    }
}

// 1-4 pick the mode, 5 toggles Snap (unbound in stock and AF). A focused view gives them through DirectInput; elsewhere
// in the frame the window message runs them and is dropped, so no tree or list type-ahead selects on the digit.
constexpr int hotkey_count = 5;
bool g_hotkey_down[hotkey_count] = {};
bool g_hotkey_message_taken[hotkey_count] = {};
HHOOK g_hotkey_msg_hook = nullptr;

void hotkey_tick()
{
    const bool usable = focus_on_views() && hotkeys_usable(GetFocus()) && !modifier_key_held();
    for (int i = 0; i < hotkey_count; i++) {
        const bool down = g_dinput_keys[DIK_1 + i] != 0;
        const bool pressed = down && !g_hotkey_down[i];
        g_hotkey_down[i] = down;
        if (pressed && usable) {
            run_hotkey(i);
        }
    }
}

// A key message may run a hotkey with a control focused (a focused view reads DirectInput instead, hotkey_tick), or
// with no usable focus at all, as when a clicked button disabled itself: the active window then gets the key as
// WM_SYSKEYDOWN with Alt up (lParam bit 29 clear).
bool message_may_run_hotkey(const MSG& msg)
{
    if (GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_SHIFT) < 0 || GetKeyState(VK_MENU) < 0) return false;
    const HWND focus = GetFocus();
    if (msg.message == WM_SYSKEYDOWN) {
        const bool unfocused = !focus || !IsWindowEnabled(focus);
        return unfocused && !(msg.lParam & (1 << 29)) && hotkeys_usable(msg.hwnd);
    }
    return !focus_on_views() && hotkeys_usable(focus);
}

bool is_key_message(UINT message)
{
    return message == WM_KEYDOWN || message == WM_SYSKEYDOWN || message == WM_KEYUP || message == WM_SYSKEYUP;
}

// A key of the frame's Undo or Redo accelerator, with its modifiers.
bool undo_redo_accelerator(const MSG& msg)
{
    static const HACCEL table = LoadAcceleratorsA(g_module, MAKEINTRESOURCEA(IDR_MAIN_FRAME));
    std::array<ACCEL, 128> accels{};
    const int count = table ? CopyAcceleratorTableA(table, accels.data(), static_cast<int>(accels.size())) : 0;
    const BYTE mods = (GetKeyState(VK_CONTROL) < 0 ? FCONTROL : 0) | (GetKeyState(VK_SHIFT) < 0 ? FSHIFT : 0) |
                      (GetKeyState(VK_MENU) < 0 ? FALT : 0);
    for (int i = 0; i < count; i++) {
        const ACCEL& a = accels[i];
        if ((a.cmd == ID_EDIT_UNDO || a.cmd == ID_EDIT_REDO) && (a.fVirt & FVIRTKEY) && a.key == msg.wParam &&
            (a.fVirt & (FCONTROL | FSHIFT | FALT)) == mods) {
            return true;
        }
    }
    return false;
}

// The drag's mouse capture makes TranslateAccelerator drop menu accelerators without a WM_COMMAND, so Undo / Redo
// never reach the cancel in CMainFrame_OnCmdMsg. Their keys cancel the drag here instead; the key and its repeats
// are dropped, so a held key does not go on to undo once the capture is gone.
WPARAM g_cancel_key = 0;

bool take_drag_cancel_key(const MSG& msg)
{
    if (!is_key_message(msg.message)) return false;
    const bool down = msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN;
    // A fresh press clears it too: the release can go to another window if RED loses focus while the key is held.
    if (g_cancel_key && msg.wParam == g_cancel_key) {
        if (down && (msg.lParam & (1 << 30))) return true;
        g_cancel_key = 0;
    }
    if (!down || !g_drag.active || !undo_redo_accelerator(msg)) return false;
    g_cancel_key = msg.wParam;
    gizmo_cancel_drag();
    return true;
}

LRESULT CALLBACK hotkey_msg_proc(int code, WPARAM wp, LPARAM lp)
{
    auto* msg = reinterpret_cast<MSG*>(lp);
    if (code != HC_ACTION || wp != PM_REMOVE || !msg) {
        return CallNextHookEx(g_hotkey_msg_hook, code, wp, lp);
    }
    if (take_drag_cancel_key(*msg)) {
        msg->message = WM_NULL;
    }
    else if (msg->wParam >= '1' && msg->wParam < '1' + hotkey_count) {
        const int i = static_cast<int>(msg->wParam - '1');
        if (msg->message == WM_KEYUP || msg->message == WM_SYSKEYUP) {
            g_hotkey_message_taken[i] = false;
        }
        else if (msg->message == WM_KEYDOWN || msg->message == WM_SYSKEYDOWN) {
            const bool repeat = (msg->lParam & (1 << 30)) != 0;
            if (!repeat) {
                g_hotkey_message_taken[i] = message_may_run_hotkey(*msg);
                if (g_hotkey_message_taken[i]) {
                    try {
                        run_hotkey(i);
                    }
                    catch (const std::exception& e) {
                        xlog::error("[Gizmo] hotkey: {}", e.what());
                    }
                }
            }
            // Dropping the key down also drops the WM_CHAR TranslateMessage would make of it.
            if (g_hotkey_message_taken[i]) {
                msg->message = WM_NULL;
            }
        }
    }
    return CallNextHookEx(g_hotkey_msg_hook, code, wp, lp);
}

// ─── Rendering hook ─────────────────────────────────────────────────────────

void render_view()
{
    CDedLevel* level = CDedLevel::Get();
    auto* view = static_cast<EditorViewport*>(editor_view_at(painting_view_index));
    if (!level || !view) return;
    Placement placement;
    Handle hot;
    const Handle* only = nullptr;
    if (g_drag.active) {
        if (g_drag.level != level) return;
        placement.mode = g_drag.mode;
        placement.pivot = g_drag.mode == Mode::move ? g_drag.pivot + g_drag.delta : g_drag.pivot;
        placement.frame = g_drag.frame;
        only = &g_drag.handle;
    }
    else {
        Targets targets;
        if (!gizmo_shown(*level, targets, placement)) return;
        if (g_hover.view == view) {
            hot = g_hover.handle;
        }
    }
    Layout l;
    if (!make_layout(*view, placement, l)) return;
    if (g_drag.active && g_drag.handle.kind == HandleKind::view_ring) {
        l.view_ring_axis_set = true;
        l.view_ring_axis = g_drag.rot_axis;
    }
    draw_gizmo(l, hot, only);
}

// The view paint's gr_flip (FUN_0047ad30): last, over the level, icons and labels, in every view. esi no longer holds
// the view here, so painting_view_index (set by 0x0041E4C0, which every path to this call runs) names it.
CallHook<void()> view_paint_flip_hook{
    0x0047B9D0,
    []() {
        try {
            render_view();
        }
        catch (const std::exception& e) {
            report("render", e);
        }
        // What stock set just before the flip (0x0047B9B4).
        set_draw_color(255, 255, 255, 255);
        view_paint_flip_hook.call_target();
    },
};

// RED's viewport input (0x0047BA70) reads its next buffered key here. Mid-drag a key would start a stock transform or
// change what the drag holds (Delete, X, Space...), so it is dropped.
CallHook<int()> view_input_key_hook{
    0x0047BD5C,
    []() {
        const int key = view_input_key_hook.call_target();
        return g_drag.active ? 0 : key;
    },
};

} // namespace

void gizmo_idle()
{
    try {
        if (!g_hotkey_msg_hook) {
            g_hotkey_msg_hook = SetWindowsHookExA(WH_GETMESSAGE, hotkey_msg_proc, nullptr, GetCurrentThreadId());
        }
        sync_top_bar();
        hotkey_tick();
        coords_tick();
        if (g_drag.active) {
            drag_tick();
        }
        else {
            hover_tick();
        }
    }
    catch (const std::exception& e) {
        report("idle", e);
        if (g_drag.active) {
            cancel_drag(true);
        }
    }
}

bool gizmo_view_lbutton_down(EditorViewport* view, UINT flags, int x, int y)
{
    try {
        if (g_drag.active) {
            cancel_drag(true);
        }
        CDedLevel* level = CDedLevel::Get();
        const HWND main = GetMainFrameHandle();
        if (!level || !editor_is_view(view) || !main || !IsWindowEnabled(main)) return false;
        // Ctrl and Alt clicks stay multi-select and marquee-deselect; Shift off a handle stays the camera.
        if ((flags & (MK_CONTROL | MK_RBUTTON | MK_MBUTTON)) || GetKeyState(VK_MENU) < 0) return false;
        if (view_latched(*view) || editor_transform_key_held() || level->transform_in_progress ||
            level->transform_mode != 0) {
            return false;
        }
        const Pt cursor{static_cast<float>(x), static_cast<float>(y)};
        return (flags & MK_SHIFT) ? begin_duplicate_drag(*level, view, cursor) : begin_drag(*level, view, cursor);
    }
    catch (const std::exception& e) {
        report("press", e);
        if (g_drag.active) {
            cancel_drag(true);
        }
        return false;
    }
}

void gizmo_view_lbutton_up(EditorViewport* /*view*/)
{
    if (!g_drag.active) return;
    try {
        if (settle_broken_drag()) return;
        drag_update(true);
        if (g_drag.active) {
            commit_drag();
        }
    }
    catch (const std::exception& e) {
        report("release", e);
        if (g_drag.active) {
            cancel_drag(true);
        }
    }
}

bool gizmo_view_rbutton_down(EditorViewport* /*view*/)
{
    if (!g_drag.active) return false;
    try {
        cancel_drag(true);
    }
    catch (const std::exception& e) {
        report("right press", e);
    }
    return true;
}

bool gizmo_view_mbutton_down(EditorViewport* /*view*/)
{
    return g_drag.active;
}

bool gizmo_dragging()
{
    return g_drag.active;
}

bool gizmo_holds_view_focus()
{
    return g_drag.active || scale_list_dropped();
}

bool gizmo_cancel_drag()
{
    if (!g_drag.active) return true;
    try {
        cancel_drag(true);
        return true;
    }
    catch (const std::exception& e) {
        report("cancel", e);
        return false;
    }
}

void gizmo_button_clicked(UINT id)
{
    try {
        if (id == ID_GIZMO_SNAP) {
            toggle_snap();
        }
        else {
            const auto it = std::find(mode_button_ids.begin(), mode_button_ids.end(), id);
            if (it != mode_button_ids.end()) {
                set_mode(static_cast<Mode>(it - mode_button_ids.begin()));
            }
        }
    }
    catch (const std::exception& e) {
        report("button", e);
    }
    // The keyboard goes back to the views, where the hotkeys and RED's own keys work.
    if (EditorViewport* view = get_active_viewport()) {
        SetFocus(editor_view_hwnd(view));
    }
}

void gizmo_scale_step_changed()
{
    const HWND bar = top_bar_hwnd();
    const LRESULT sel = bar ? SendDlgItemMessageA(bar, IDC_GIZMO_SCALE_STEP, CB_GETCURSEL, 0, 0) : CB_ERR;
    if (sel >= 0 && sel < static_cast<LRESULT>(scale_steps.size())) {
        g_settings.scale_step = static_cast<int>(sel);
    }
    try {
        sync_top_bar();
    }
    catch (const std::exception& e) {
        report("scale step", e);
    }
}

void gizmo_undo_entry_applied(UndoEntry* entry, bool undone)
{
    try {
        CDedLevel* level = CDedLevel::Get();
        if (!level) return;
        prune_side_records(*level);
        const uint32_t serial = link_serial(entry);
        const auto record = std::find_if(g_side_records.begin(), g_side_records.end(),
                                         [&](const SideRecord& r) { return serial && r.serial == serial; });
        if (record == g_side_records.end()) return;
        for (const ObjectChange& c : record->objects) {
            write_extras(*c.obj, undone ? c.before : c.after);
        }
        for (const BrushChange& c : record->brushes) {
            write_brush_change(*c.brush, c.solid, undone ? c.before : c.after);
        }
        if (!record->brushes.empty()) {
            level->mark_geometry_dirty();
        }
        if (record->sun) {
            sun_arrow_restore(level, undone ? record->sun_before : record->sun_after);
        }
        editor_views_mark_repaint_all();
    }
    catch (const std::exception& e) {
        report("undo", e);
    }
}

void gizmo_level_loaded()
{
    g_side_records.clear();
}

void ApplyGizmoPatches()
{
    view_input_key_hook.install();
    view_paint_flip_hook.install();
}
