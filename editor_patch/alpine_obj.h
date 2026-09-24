#pragma once

#include <windows.h>
#include <commctrl.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "alpine_color_picker.h"
#include "vtypes.h"
#include "level.h"

// Shared Alpine object infrastructure — hooks that dispatch to all Alpine object types.
// Type-specific logic lives in mesh.cpp / note.cpp / corona.cpp; this file wires them together.
void ApplyAlpineObjectPatches();

// Replacement dialogs for stock Select Objects / Hide Objects (Tools menu)
void alpine_select_objects(CDedLevel* level);
void alpine_hide_objects(CDedLevel* level);

// Singular display name for an object type, as the object lists show it.
const char* get_type_display_name(DedObjectType type);

inline void alpine_remove_from_groups(CDedLevel* level, DedObject* obj)
{
    auto& mg = level->moving_groups;
    for (int i = 0; i < mg.size; i++) {
        auto* group = mg[i];
        if (!group) continue;
        for (int j = group->objects.size - 1; j >= 0; j--) {
            if (group->objects[j] == obj) {
                group->objects.remove_at(j);
            }
        }
    }
}

// ─── Shared selection helper ────────────────────────────────────────────────

inline bool is_object_selected(CDedLevel* level, DedObject* obj)
{
    auto& sel = level->selection;
    for (int i = 0; i < sel.size; i++) {
        if (sel.data_ptr[i] == obj) return true;
    }
    return false;
}

// ─── Shared Alpine object-type machinery (Tier 2) ───────────────────────────
// Per docs/PLAN_alpine_shared_machinery.md. Only the rope emitter uses these so far; the six
// older types still carry their own copies and are migrated separately.

// Keeps the stock UID generator ahead of a type's objects, which it cannot see.
template<typename T>
inline void alpine_ensure_uid(const std::vector<T*>& objects, int& uid)
{
    for (auto* obj : objects) {
        if (obj->uid >= uid) uid = obj->uid + 1;
    }
}

// Deletes every selected object of one type, compacting the selection in place. Reverse
// iteration is required: `destroy` frees the object, and each removal shifts the tail down.
template<typename T, typename DestroyFn>
inline void alpine_compact_selection(CDedLevel* level, DedObjectType type, DestroyFn destroy)
{
    auto& sel = level->selection;
    for (int i = sel.size - 1; i >= 0; i--) {
        DedObject* obj = sel.data_ptr[i];
        if (obj && obj->type == type) {
            for (int j = i; j < sel.size - 1; j++) {
                sel.data_ptr[j] = sel.data_ptr[j + 1];
            }
            sel.size--;
            destroy(static_cast<T*>(obj));
        }
    }
}

// Closest object whose placed position projects within radius_sq of the click, in pixels.
template<typename T>
inline T* alpine_click_pick_point(const std::vector<T*>& objects, float click_x, float click_y,
                                  float radius_sq)
{
    float best_dist_sq = 1e30f;
    T* best = nullptr;

    for (auto* obj : objects) {
        if (obj->hidden_in_editor) continue;

        float center_pos[3] = {obj->pos.x, obj->pos.y, obj->pos.z};
        float screen_cx = 0.0f, screen_cy = 0.0f;
        if (!project_to_screen_2d(center_pos, &screen_cx, &screen_cy))
            continue;

        float dx = screen_cx - click_x;
        float dy = screen_cy - click_y;
        float dist_sq = dx * dx + dy * dy;
        if (dist_sq <= radius_sq && dist_sq < best_dist_sq) {
            best_dist_sq = dist_sq;
            best = obj;
        }
    }

    return best;
}

// Screen radius the point-picked Alpine object types share (20 px).
constexpr float alpine_click_pick_radius_sq = 400.0f;

// ─── Shared properties-dialog helpers (Tier 2) ──────────────────────────────
// The older types still carry their own copies; they migrate with the rest of Tier 2.

inline void alpine_dlg_set_float_field(HWND hdlg, int idc, float value)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.4g", value);
    SetDlgItemTextA(hdlg, idc, buf);
}

inline float alpine_dlg_get_float_field(HWND hdlg, int idc)
{
    char buf[32] = {};
    GetDlgItemTextA(hdlg, idc, buf, sizeof(buf));
    return static_cast<float>(std::atof(buf));
}

// Text rather than GetDlgItemInt, so a half typed or empty field reads as 0 instead of leaving the
// caller to interpret a FALSE translated flag.
inline int alpine_dlg_get_int_field(HWND hdlg, int idc)
{
    char buf[32] = {};
    GetDlgItemTextA(hdlg, idc, buf, sizeof(buf));
    return std::atoi(buf);
}

// Names that aren't on disk or in a vpp stay at -1 rather than going through bm_load, which would
// manufacture (and permanently cache) a placeholder entry for every half-typed name. A -1 handle
// takes the same empty-preview path the stock panel uses when nothing is selected.
inline int alpine_dlg_resolve_bitmap(const char* name)
{
    if (!name || name[0] == '\0') return -1;
    if (std::strlen(name) > rfl_name_max_len) return -1;
    const char* ext = std::strrchr(name, '.');
    if (ext && std::strlen(ext) > rfl_ext_max_len) return -1;
    // open (0x004CF9A0) locates the file without opening a stream, so no close belongs here:
    // close (0x004CFF60) would index the open file table at slot -1 (the constructor's value).
    rf::File file;
    if (!file.open(name)) return -1;
    return bm_load(name, -1, 1);
}

// Mirrors CBitmapPreviewDialog::OnPaint (0x0044C1B0): the editor renderer draws into the control's
// own window, letterboxed so the texture keeps its aspect ratio.
inline void alpine_dlg_draw_bitmap_preview(HWND ctrl, const RECT& rc, int bm_handle)
{
    int w = std::min<int>(rc.right - rc.left, gr_get_max_width());
    int h = std::min<int>(rc.bottom - rc.top, gr_get_max_height());
    if (w <= 0 || h <= 0) return;

    gr_set_viewport_wnd(ctrl);

    if (bm_handle < 0) {
        gr_set_clip(0, 0, w, h);
        gr_clear();
        gr_flip();
        return;
    }

    for (int pass = 0; pass < 2; pass++) {
        gr_set_clip(0, 0, w, h);
        gr_clear();

        int src_w = 0, src_h = 0, num_pixels = 0, mip_levels = 0;
        bm_get_mipmap_info(bm_handle, &src_w, &src_h, &num_pixels, &mip_levels);
        if (src_w <= 0 || src_h <= 0) break;

        int dst_x = 0, dst_y = 0, dst_w = w, dst_h = h;
        if (src_h > src_w) {
            dst_w = static_cast<int>(std::lround(static_cast<float>(h) / src_h * src_w));
            dst_x = static_cast<int>(std::lround((w - dst_w) * 0.5f));
        }
        else if (src_w > src_h) {
            dst_h = static_cast<int>(std::lround(static_cast<float>(w) / src_w * src_h));
            dst_y = static_cast<int>(std::lround((h - dst_h) * 0.5f));
        }

        gr_bitmap_scaled(bm_handle, dst_x, dst_y, dst_w, dst_h, 0, 0, src_w, src_h,
                         0.0f, 0.0f, gr_bitmap_preview_mode);
    }

    gr_flip();
}

// ─── Shared color controls ──────────────────────────────────────────────────
// The Level Properties sun color idiom: a swatch tinted to the current color, a "<r, g, b>" text
// field, and a button that opens the shared picker. One derivation, so every site behaves the same.

inline void alpine_dlg_set_color_controls(HWND hdlg, int swatch_idc, int value_idc, uint8_t r,
                                          uint8_t g, uint8_t b)
{
    // a null HWND would send InvalidateRect at every window on the desktop
    if (HWND swatch = GetDlgItem(hdlg, swatch_idc)) {
        SendMessageA(swatch, LVM_SETBKCOLOR, 0, static_cast<LPARAM>(RGB(r, g, b)));
        InvalidateRect(swatch, nullptr, TRUE);
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "<%d, %d, %d>", r, g, b);
    SetDlgItemTextA(hdlg, value_idc, buf);
}

// Leaves r/g/b untouched unless all three components parse.
inline bool alpine_dlg_parse_color_text(HWND hdlg, int value_idc, uint8_t& r, uint8_t& g, uint8_t& b)
{
    char buf[64] = {};
    GetDlgItemTextA(hdlg, value_idc, buf, sizeof(buf));
    long values[3] = {};
    const char* p = buf;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '<') ++p;
    for (int i = 0; i < 3; i++) {
        while (*p == ' ' || *p == '\t' || (i > 0 && *p == ',')) ++p;
        char* end = nullptr;
        long value = std::strtol(p, &end, 10);
        if (end == p || value < 0 || value > 255) return false;
        values[i] = value;
        p = end;
    }
    r = static_cast<uint8_t>(values[0]);
    g = static_cast<uint8_t>(values[1]);
    b = static_cast<uint8_t>(values[2]);
    return true;
}

inline bool alpine_dlg_pick_color(HWND hdlg, int swatch_idc, int value_idc, uint8_t& r, uint8_t& g,
                                  uint8_t& b)
{
    COLORREF color = RGB(r, g, b);
    if (!alpine_pick_color(hdlg, color, alpine_shared_custom_colors())) {
        return false;
    }
    r = GetRValue(color);
    g = GetGValue(color);
    b = GetBValue(color);
    alpine_dlg_set_color_controls(hdlg, swatch_idc, value_idc, r, g, b);
    return true;
}

// ─── Shared drawing helpers for Alpine object types ─────────────────────────

inline void draw_3d_line(float x1, float y1, float z1, float x2, float y2, float z2, int r, int g, int b)
{
    uint8_t screen1[48] = {};
    uint8_t screen2[48] = {};
    float p1[3] = {x1, y1, z1};
    float p2[3] = {x2, y2, z2};

    project_to_screen(screen1, p1);
    project_to_screen(screen2, p2);

    float sx1 = *reinterpret_cast<float*>(screen1);
    float sy1 = *reinterpret_cast<float*>(screen1 + 4);
    float sx2 = *reinterpret_cast<float*>(screen2);
    float sy2 = *reinterpret_cast<float*>(screen2 + 4);
    if (sx1 == sx2 && sy1 == sy2) return;

    set_draw_color(r, g, b, 0xff);
    draw_line_2d(screen1, screen2, *reinterpret_cast<uint32_t*>(0x0147d260));
}

inline void draw_wireframe_sphere(float cx, float cy, float cz, float radius, int r, int g, int b)
{
    constexpr int segments = 24;
    constexpr float pi2 = 6.2831853f;
    for (int i = 0; i < segments; i++) {
        float a0 = pi2 * i / segments;
        float a1 = pi2 * (i + 1) / segments;
        float c0 = std::cos(a0) * radius, s0 = std::sin(a0) * radius;
        float c1 = std::cos(a1) * radius, s1 = std::sin(a1) * radius;
        draw_3d_line(cx + c0, cy + s0, cz, cx + c1, cy + s1, cz, r, g, b);
        draw_3d_line(cx + c0, cy, cz + s0, cx + c1, cy, cz + s1, r, g, b);
        draw_3d_line(cx, cy + c0, cz + s0, cx, cy + c1, cz + s1, r, g, b);
    }
}

// ─── Additive blending helpers ──────────────────────────────────────────────

namespace d3d8 {
    constexpr unsigned int RS_ZWRITEENABLE = 14;
    constexpr unsigned int RS_CULLMODE = 22;
    constexpr unsigned int RS_ALPHABLENDENABLE = 27;
    constexpr unsigned int RS_SRCBLEND = 19;
    constexpr unsigned int RS_DESTBLEND = 20;
    constexpr unsigned int CULL_NONE = 1;
    constexpr unsigned int BLEND_ONE = 2;
    constexpr unsigned int BLEND_SRCALPHA = 5;
    constexpr unsigned int BLEND_INVSRCALPHA = 6;

    using SetRenderStateFn = long(__stdcall*)(void*, unsigned int, unsigned int);

    inline SetRenderStateFn get_set_render_state()
    {
        void* device = d3d_device_ptr;
        if (!device) return nullptr;
        auto** vtable = *reinterpret_cast<void***>(device);
        return reinterpret_cast<SetRenderStateFn>(vtable[50]); // IDirect3DDevice8::SetRenderState
    }
}

// Flush the current batch with additive blend states, then restart with normal states
inline void flush_additive()
{
    void* device = d3d_device_ptr;
    auto setRS = d3d8::get_set_render_state();
    if (!device || !setRS) return;

    setRS(device, d3d8::RS_ALPHABLENDENABLE, 1);
    setRS(device, d3d8::RS_SRCBLEND, d3d8::BLEND_SRCALPHA);
    setRS(device, d3d8::RS_DESTBLEND, d3d8::BLEND_ONE);
    setRS(device, d3d8::RS_ZWRITEENABLE, 0);

    gr_flush_batch();
    gr_begin_batch(4, 3);

    setRS(device, d3d8::RS_ALPHABLENDENABLE, 0);
    setRS(device, d3d8::RS_SRCBLEND, d3d8::BLEND_SRCALPHA);
    setRS(device, d3d8::RS_DESTBLEND, d3d8::BLEND_INVSRCALPHA);
    setRS(device, d3d8::RS_ZWRITEENABLE, 1);
}

// Render a billboard with additive blending
inline void render_additive_billboard(void* pos, float scale, float cam_param)
{
    gr_render_billboard(pos, 0, scale, cam_param);
    flush_additive();
}

// Transform a world-space point to view-space and fill a GrVertex
inline void world_to_view(GrVertex& v, const Vector3& w)
{
    float dx = w.x - ed_cam_pos[0];
    float dy = w.y - ed_cam_pos[1];
    float dz = w.z - ed_cam_pos[2];
    v.vx = ed_view_mat[0] * dx + ed_view_mat[1] * dy + ed_view_mat[2] * dz;
    v.vy = ed_view_mat[3] * dx + ed_view_mat[4] * dy + ed_view_mat[5] * dz;
    v.vz = ed_view_mat[6] * dx + ed_view_mat[7] * dy + ed_view_mat[8] * dz;
}

// Render a textured quad stretched between two 3D points (axial billboard).
// The quad is locked along the given forward axis and rotates around it to face the camera.
inline void render_additive_axial_quad(
    const Vector3& pos, const Matrix3& orient,
    float length, float height, float cam_param)
{
    Vector3 cam_pos = {ed_cam_pos[0], ed_cam_pos[1], ed_cam_pos[2]};

    const Vector3& fvec = orient.fvec;
    Vector3 end = {
        pos.x + fvec.x * length,
        pos.y + fvec.y * length,
        pos.z + fvec.z * length
    };

    Vector3 center = {
        (pos.x + end.x) * 0.5f,
        (pos.y + end.y) * 0.5f,
        (pos.z + end.z) * 0.5f
    };

    // Side vector = cross(fvec, to_camera), perpendicular facing camera
    Vector3 to_cam = {cam_pos.x - center.x, cam_pos.y - center.y, cam_pos.z - center.z};
    Vector3 side = {
        fvec.y * to_cam.z - fvec.z * to_cam.y,
        fvec.z * to_cam.x - fvec.x * to_cam.z,
        fvec.x * to_cam.y - fvec.y * to_cam.x
    };

    float side_len = std::sqrt(side.x * side.x + side.y * side.y + side.z * side.z);
    if (side_len < 0.0001f) return; // degenerate: looking straight along beam axis

    float half_h = (height * 0.5f) / side_len;
    side.x *= half_h;
    side.y *= half_h;
    side.z *= half_h;

    Vector3 corners[4] = {
        {pos.x - side.x, pos.y - side.y, pos.z - side.z},
        {end.x - side.x, end.y - side.y, end.z - side.z},
        {end.x + side.x, end.y + side.y, end.z + side.z},
        {pos.x + side.x, pos.y + side.y, pos.z + side.z},
    };

    static const float uvs[4][2] = {{0,1}, {0,0}, {1,0}, {1,1}};

    GrVertex verts[4] = {};
    uint8_t all_clip = 0xff;
    for (int i = 0; i < 4; i++) {
        world_to_view(verts[i], corners[i]);
        gr_compute_clip_flags(&verts[i]);
        all_clip &= verts[i].clip_flags;
        verts[i].proj_flags = 0;
        verts[i].u = uvs[i][0];
        verts[i].v = uvs[i][1];
        verts[i].r = 255; verts[i].g = 255; verts[i].b = 255; verts[i].a = 255;
    }

    if (all_clip != 0) return;

    void* ptrs[4] = {&verts[0], &verts[1], &verts[2], &verts[3]};

    gr_set_mode(0x10);
    gr_poly_render(4, ptrs, 1, cam_param, 0, 0.0f);
    flush_additive();
}
