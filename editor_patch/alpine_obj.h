#pragma once

#include <cmath>
#include <cstdint>
#include "vtypes.h"
#include "level.h"

// Shared Alpine object infrastructure — hooks that dispatch to all Alpine object types.
// Type-specific logic lives in mesh.cpp / note.cpp / corona.cpp; this file wires them together.
void ApplyAlpineObjectPatches();

// Replacement dialogs for stock Select Objects / Hide Objects (Tools menu)
void alpine_select_objects(CDedLevel* level);
void alpine_hide_objects(CDedLevel* level);

// ─── Shared selection helper ────────────────────────────────────────────────

inline bool is_object_selected(CDedLevel* level, DedObject* obj)
{
    auto& sel = level->selection;
    for (int i = 0; i < sel.size; i++) {
        if (sel.data_ptr[i] == obj) return true;
    }
    return false;
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
    constexpr unsigned int RS_ALPHABLENDENABLE = 27;
    constexpr unsigned int RS_SRCBLEND = 19;
    constexpr unsigned int RS_DESTBLEND = 20;
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
