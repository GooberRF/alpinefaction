#include <windows.h>
#include <memory>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <patch_common/CodeInjection.h>
#include <patch_common/MemUtils.h>
#include <patch_common/ShortTypes.h>
#include <xlog/xlog.h>


// Max lights that can be processed per face (shadow mask buffer limit).
// Faces with more lights than this get the pink fill safety fallback.
static constexpr int max_shadow_masks = 1024;
static void* shadow_mask_ptrs[max_shadow_masks];
static std::unique_ptr<uint8_t[]> shadow_mask_pool;

// Scene light object pool: replaces the stock 1100-entry pool at 0x006FB248
// Each entry is sizeof(rf::gr::Light) = 0x10C bytes (see game_patch/rf/gr/gr_light.h)
// todo: shared header between editor and game for this and other common structs
static constexpr int max_scene_lights = 8192;

// Per-face light list: replaces the stock ~1100-entry array at 0x006F9FF8
// Must be at least max_scene_lights since all scene lights could affect a single face
// 0x00488810 writes to this array with no bounds check
static void* face_light_list[max_scene_lights];
static constexpr int light_entry_size = 0x10C;
alignas(16) static uint8_t light_pool[max_scene_lights * light_entry_size];

// Dummy light entry for out-of-bounds handle access.
// reads return all zeros (type=LT_NONE). Prevents memory corruption when the
// stock code calls handle_to_ptr with handle=-1 (allocation failure).
alignas(16) static uint8_t dummy_light[light_entry_size] = {};

CodeInjection light_handle_to_pointer_injection{
    0x00487a00,
    [](auto& regs) {
        int handle = *reinterpret_cast<int*>(regs.esp + 4);
        if (handle >= 0 && handle < max_scene_lights) {
            regs.eax = reinterpret_cast<uintptr_t>(light_pool) +
                        static_cast<unsigned>(handle) * light_entry_size;
        }
        else {
            regs.eax = reinterpret_cast<uintptr_t>(dummy_light);
        }
        regs.eip = 0x00487a15; // jump to RET
    },
};

CodeInjection lightmap_light_limit_injection{
    0x004AC608,
    [](auto& regs) {
        int light_count = regs.edi;

        // Also check that the surface texel count fits in the shadow mask buffer (0x1000 bytes).
        // With -smoothlights, surface groups can merge across rooms and potentially exceed the
        // 64x64 texel limit, which would overflow the shadow mask buffer and corrupt heap memory.
        int width = *reinterpret_cast<int*>(regs.esi + 0x18);
        int height = *reinterpret_cast<int*>(regs.esi + 0x1c);
        if (width * height > 0x1000) {
            xlog::error("Lightmap: surface texel count {}x{}={} exceeds shadow mask buffer size 4096! "
                        "Falling back to pink fill for surface at 0x{:x}",
                        width, height, width * height, static_cast<uintptr_t>(regs.esi));
            regs.eip = 0x004AC9E4; // pink fill safety fallback
        }
        else if (light_count >= max_shadow_masks) {
            regs.eip = 0x004AC9E4; // pink fill safety fallback
        }
        else {
            regs.eip = 0x004AC611; // normal lightmap processing
        }
    },
};

// Fix lightmap seam at portal-split face boundaries.
// When a portal brush splits a face (e.g., a floor), the fragments end up in different
// rooms. FUN_004aae80 (cross-surface lightmap blending) skips blending when room_index
// differs (JNZ at 0x004aaf12), causing a visible seam. This patch allows blending across
// room boundaries when the two surfaces are coplanar (same geometric plane), which is the
// case for fragments of the same original face split by a portal.
CodeInjection lightmap_cross_room_blend_injection{
    0x004aaf10,
    [](auto& regs) {
        // EAX = surface A ptr, ECX = surface B ptr
        // EDX = B->room_index, ESI = A->room_index
        int room_a = static_cast<int>(regs.esi);
        int room_b = static_cast<int>(regs.edx);
        if (room_a == room_b) {
            regs.eip = 0x004aaf18;
            return;
        }
        // Different rooms - only allow blending if surfaces are coplanar
        auto* surf_a = reinterpret_cast<const char*>(static_cast<uintptr_t>(regs.eax));
        auto* surf_b = reinterpret_cast<const char*>(static_cast<uintptr_t>(regs.ecx));
        // Surface plane: normal at +0x6C (vec3), distance at +0x78 (float)
        auto* na = reinterpret_cast<const float*>(surf_a + 0x6C);
        auto* nb = reinterpret_cast<const float*>(surf_b + 0x6C);
        float da = *reinterpret_cast<const float*>(surf_a + 0x78);
        float db = *reinterpret_cast<const float*>(surf_b + 0x78);
        float dot = na[0] * nb[0] + na[1] * nb[1] + na[2] * nb[2];
        // Coplanar: normals aligned (or opposite) and plane distance matches
        if ((dot > 0.999f && std::abs(da - db) < 0.001f) ||
            (dot < -0.999f && std::abs(da + db) < 0.001f)) {
            regs.eip = 0x004aaf18; // allow blending
        }
        else {
            regs.eip = 0x004ab07c; // skip blending
        }
    },
};

// Per-texel room ambient data, populated at FUN_004aabf0 entry and used by the
// per-texel fill injections to vary ambient color across room boundaries.
struct PerTexelRoomAmbient {
    float bbox_min[3];
    float bbox_max[3];
    float r, g, b; // ambient color as float (byte * 1/255)
};

static constexpr int kMaxAmbientRooms = 64;
static PerTexelRoomAmbient s_ambient_rooms[kMaxAmbientRooms];
static int s_ambient_room_count = 0;

// Fix pre-existing editor bug: room linker ambient properties are not reapplied to
// GRoom objects after geometry rebuild. The room properties dialog (FUN_0040ab80) applies
// them, but a subsequent "Build Geometry" recreates rooms with ambient_light_defined=0.
// This injection runs at the entry of the batch lightmap calculator (FUN_004aabf0) and
// iterates the room linker list, applying ambient settings to the corresponding rooms.
CodeInjection lightmap_apply_room_ambient_injection{
    0x004aabf0, // entry of FUN_004aabf0 (batch lightmap calculator)
    [](auto& regs) {
        // ECX at FUN_004aabf0 entry is the GSolid used for lightmap calculation.
        uintptr_t gsolid = regs.ecx;
        if (!gsolid) return;

        // Room linker list comes from the editor state
        auto get_editor_state = reinterpret_cast<uintptr_t (*)()>(0x004835f0);
        uintptr_t editor_state = get_editor_state();
        if (!editor_state) return;

        // Room linker VArray at editor_state+0x3e8: {count: int, capacity: int, data: ptr}
        uintptr_t room_linker_list = editor_state + 0x3e8;
        int linker_count = *reinterpret_cast<int*>(room_linker_list);
        if (linker_count <= 0) return;
        uintptr_t linker_data = *reinterpret_cast<uintptr_t*>(room_linker_list + 8);
        if (!linker_data) return;

        // GSolid::all_rooms VArray at GSolid+0x90, surfaces VArray at GSolid+0xC0.
        // The BSP spatial lookup and GRoom bounding boxes don't reliably match surface
        // room_index values (BSP can return room_index=183 when surfaces use 0-6).
        // Instead, build combined bounding boxes from surfaces per room_index. This matches
        // the exact room assignment the lightmap code uses.
        int room_count = *reinterpret_cast<int*>(gsolid + 0x90);
        uintptr_t room_elements = *reinterpret_cast<uintptr_t*>(gsolid + 0x90 + 8);
        if (room_count <= 0 || !room_elements) return;

        int surface_count = *reinterpret_cast<int*>(gsolid + 0xC0);
        uintptr_t surface_elements = *reinterpret_cast<uintptr_t*>(gsolid + 0xC0 + 8);
        if (surface_count <= 0 || !surface_elements) return;

        // Build combined bbox per room_index from surfaces
        // GSurface layout: bbox_mn at +0x34 (Vec3), bbox_mx at +0x40 (Vec3), room_index at +0x68
        constexpr int max_tracked_rooms = 512;
        struct RoomBBox { float mn[3]; float mx[3]; bool valid; };
        auto* room_bboxes = new (std::nothrow) RoomBBox[max_tracked_rooms]();
        if (!room_bboxes) return;

        for (int s = 0; s < surface_count; s++) {
            uintptr_t surf = *reinterpret_cast<uintptr_t*>(surface_elements + s * 4);
            if (!surf) continue;
            int ridx = *reinterpret_cast<int*>(surf + 0x68);
            if (ridx < 0 || ridx >= max_tracked_rooms) continue;
            auto* smn = reinterpret_cast<const float*>(surf + 0x34);
            auto* smx = reinterpret_cast<const float*>(surf + 0x40);
            auto& bb = room_bboxes[ridx];
            if (!bb.valid) {
                for (int c = 0; c < 3; c++) { bb.mn[c] = smn[c]; bb.mx[c] = smx[c]; }
                bb.valid = true;
            } else {
                for (int c = 0; c < 3; c++) {
                    if (smn[c] < bb.mn[c]) bb.mn[c] = smn[c];
                    if (smx[c] > bb.mx[c]) bb.mx[c] = smx[c];
                }
            }
        }

        for (int i = 0; i < linker_count; i++) {
            uintptr_t brush = *reinterpret_cast<uintptr_t*>(linker_data + i * 4);
            if (!brush) continue;

            int type = *reinterpret_cast<int*>(brush + 0x94);
            if (type != 3) continue; // only ambient linkers

            auto* pos = reinterpret_cast<const float*>(brush + 0x14);

            // Find the smallest room bbox containing the linker position.
            // Multiple room bboxes may overlap (adjacent rooms share boundaries, and
            // cross-room merged surfaces extend bboxes). Using smallest-volume avoids
            // matching a large room (e.g., room 0) that happens to contain the linker.
            int best_ridx = -1;
            float best_volume = 1e30f;
            for (int ridx = 0; ridx < max_tracked_rooms; ridx++) {
                auto& bb = room_bboxes[ridx];
                if (!bb.valid) continue;
                if (ridx >= room_count) continue;
                if (pos[0] >= bb.mn[0] && pos[0] <= bb.mx[0] &&
                    pos[1] >= bb.mn[1] && pos[1] <= bb.mx[1] &&
                    pos[2] >= bb.mn[2] && pos[2] <= bb.mx[2]) {
                    float vol = (bb.mx[0] - bb.mn[0]) *
                                (bb.mx[1] - bb.mn[1]) *
                                (bb.mx[2] - bb.mn[2]);
                    if (vol < best_volume) {
                        best_volume = vol;
                        best_ridx = ridx;
                    }
                }
            }
            if (best_ridx >= 0) {
                uintptr_t room = *reinterpret_cast<uintptr_t*>(room_elements + best_ridx * 4);
                if (room) {
                    *reinterpret_cast<uint8_t*>(room + 0x45) = 1;
                    *reinterpret_cast<uint32_t*>(room + 0x46) =
                        *reinterpret_cast<uint32_t*>(brush + 0x98);
                }
            }
        }
        // Collect per-texel ambient data from rooms with custom ambient, using the
        // surface-combined bboxes (which reliably match surface room_index assignments).
        // Must be done BEFORE deleting room_bboxes.
        s_ambient_room_count = 0;
        for (int ridx = 0; ridx < room_count && ridx < max_tracked_rooms
                 && s_ambient_room_count < kMaxAmbientRooms; ridx++) {
            uintptr_t room = *reinterpret_cast<uintptr_t*>(room_elements + ridx * 4);
            if (!room) continue;
            if (*reinterpret_cast<uint8_t*>(room + 0x45) != 1) continue;
            if (!room_bboxes[ridx].valid) continue; // no surfaces for this room
            auto& entry = s_ambient_rooms[s_ambient_room_count];
            for (int c = 0; c < 3; c++) {
                entry.bbox_min[c] = room_bboxes[ridx].mn[c];
                entry.bbox_max[c] = room_bboxes[ridx].mx[c];
            }
            constexpr float inv255 = 1.0f / 255.0f;
            entry.r = static_cast<float>(*reinterpret_cast<uint8_t*>(room + 0x46)) * inv255;
            entry.g = static_cast<float>(*reinterpret_cast<uint8_t*>(room + 0x47)) * inv255;
            entry.b = static_cast<float>(*reinterpret_cast<uint8_t*>(room + 0x48)) * inv255;
            s_ambient_room_count++;
        }

        delete[] room_bboxes;
    },
};

// ============================================================
// Per-texel room ambient system
// ============================================================
// When surfaces span room boundaries (due to cross-room merge), the ambient
// color should vary per-texel based on which room the texel is in. The stock
// code applies ambient uniformly per-surface from the surface's room_index.
// This system pre-collects rooms with custom ambient and their bounding boxes
// (in lightmap_apply_room_ambient_injection above), then replaces the uniform
// fill with a per-texel fill that checks room containment for each texel's
// world position.

// Surface UV-to-world mapping parameters, precomputed per surface.
struct SurfaceUVParams {
    float inv_lm_w, inv_lm_h;
    float inv_scale_x, inv_scale_y;
    float uv_add_x, uv_add_y;
    int xstart, ystart;
    int dropped, u_coeff;
    float nx, ny, nz, d;
};

static bool init_surface_uv_params(uintptr_t surface, SurfaceUVParams& p)
{
    uintptr_t lm = *reinterpret_cast<uintptr_t*>(surface + 0xC);
    if (!lm) return false;
    int lm_w = *reinterpret_cast<int*>(lm + 4);
    int lm_h = *reinterpret_cast<int*>(lm + 8);
    if (lm_w <= 0 || lm_h <= 0) return false;
    float scale_x = *reinterpret_cast<float*>(surface + 0x4C);
    float scale_y = *reinterpret_cast<float*>(surface + 0x50);
    if (scale_x == 0.0f || scale_y == 0.0f) return false;
    p.inv_lm_w = 1.0f / static_cast<float>(lm_w);
    p.inv_lm_h = 1.0f / static_cast<float>(lm_h);
    p.inv_scale_x = 1.0f / scale_x;
    p.inv_scale_y = 1.0f / scale_y;
    p.uv_add_x = *reinterpret_cast<float*>(surface + 0x54);
    p.uv_add_y = *reinterpret_cast<float*>(surface + 0x58);
    p.xstart = *reinterpret_cast<int*>(surface + 0x10);
    p.ystart = *reinterpret_cast<int*>(surface + 0x14);
    p.dropped = *reinterpret_cast<int*>(surface + 0x5C);
    p.u_coeff = *reinterpret_cast<int*>(surface + 0x60);
    p.nx = *reinterpret_cast<float*>(surface + 0x6C);
    p.ny = *reinterpret_cast<float*>(surface + 0x70);
    p.nz = *reinterpret_cast<float*>(surface + 0x74);
    p.d = *reinterpret_cast<float*>(surface + 0x78);
    return true;
}

// Convert texel (col, row) to world position using the surface's UV-to-world mapping.
// Matches the dropped-axis projection in FUN_004a9b10/4a9b60/4a9bb0.
// Plane convention: nx*x + ny*y + nz*z + d = 0
static void texel_to_world(const SurfaceUVParams& p, int col, int row,
                           float& wx, float& wy, float& wz)
{
    float u = ((static_cast<float>(p.xstart + col) + 0.5f) * p.inv_lm_w - p.uv_add_x) * p.inv_scale_x;
    float v = ((static_cast<float>(p.ystart + row) + 0.5f) * p.inv_lm_h - p.uv_add_y) * p.inv_scale_y;
    switch (p.dropped) {
    case 0: // X dropped
        if (p.u_coeff == 1) { wy = u; wz = v; } else { wy = v; wz = u; }
        wx = -(p.ny * wy + p.nz * wz + p.d) / p.nx;
        break;
    case 1: // Y dropped
        if (p.u_coeff == 0) { wx = u; wz = v; } else { wx = v; wz = u; }
        wy = -(p.nx * wx + p.nz * wz + p.d) / p.ny;
        break;
    default: // Z dropped
        if (p.u_coeff == 0) { wx = u; wy = v; } else { wx = v; wy = u; }
        wz = -(p.nx * wx + p.ny * wy + p.d) / p.nz;
        break;
    }
}

// Find the ambient color at a world position by checking room bounding boxes.
// Uses smallest-volume match to handle overlapping bboxes correctly.
// Returns the tightest-fitting custom-ambient room, or global ambient.
static void get_ambient_at(float wx, float wy, float wz,
                           float& r, float& g, float& b)
{
    int best = -1;
    float best_volume = 1e30f;
    for (int i = 0; i < s_ambient_room_count; i++) {
        const auto& room = s_ambient_rooms[i];
        if (wx >= room.bbox_min[0] && wx <= room.bbox_max[0] &&
            wy >= room.bbox_min[1] && wy <= room.bbox_max[1] &&
            wz >= room.bbox_min[2] && wz <= room.bbox_max[2]) {
            float vol = (room.bbox_max[0] - room.bbox_min[0]) *
                        (room.bbox_max[1] - room.bbox_min[1]) *
                        (room.bbox_max[2] - room.bbox_min[2]);
            if (vol < best_volume) {
                best_volume = vol;
                best = i;
            }
        }
    }
    if (best >= 0) {
        r = s_ambient_rooms[best].r;
        g = s_ambient_rooms[best].g;
        b = s_ambient_rooms[best].b;
    }
    else {
        // No custom ambient room contains this point — use global ambient
        r = *reinterpret_cast<float*>(0x0057c968);
        g = *reinterpret_cast<float*>(0x0057c96c);
        b = *reinterpret_cast<float*>(0x0057c970);
    }
}

// Per-texel ambient fill for the has-lights path in FUN_004ac470.
// Replaces the uniform ambient fill at 0x004ac68b-0x004ac742 with a per-texel
// fill that computes each texel's world position and uses the containing room's
// ambient color. The three float buffers (R/G/B) are initialized per-texel
// before the shadow/light calculation modifies them.
CodeInjection lightmap_per_texel_ambient_fill_injection{
    0x004ac68b, // MOV EAX,[ESI+0x1c] — start of ambient fill section
    [](auto& regs) {
        if (s_ambient_room_count == 0) return; // no custom ambient rooms, use original fill (trampoline OK here)

        uintptr_t surface = regs.esi;
        int width = *reinterpret_cast<int*>(surface + 0x18);
        int height = *reinterpret_cast<int*>(surface + 0x1c);
        if (width <= 0 || height <= 0) return; // trampoline OK at this address

        SurfaceUVParams p;
        if (!init_surface_uv_params(surface, p)) return; // trampoline OK at this address

        auto* buf_r = reinterpret_cast<float*>(0x0138a620);
        auto* buf_g = reinterpret_cast<float*>(0x0140ac20);
        auto* buf_b = reinterpret_cast<float*>(0x0134a620);

        int idx = 0;
        for (int row = 0; row < height; row++) {
            for (int col = 0; col < width; col++) {
                float wx, wy, wz;
                texel_to_world(p, col, row, wx, wy, wz);
                float ar, ag, ab;
                get_ambient_at(wx, wy, wz, ar, ag, ab);
                buf_r[idx] = ar * 0.5f;
                buf_g[idx] = ag * 0.5f;
                buf_b[idx] = ab * 0.5f;
                idx++;
            }
        }

        regs.eip = 0x004ac742; // skip original uniform fill
    },
};

// Per-texel ambient fill for the no-lights path in FUN_004ac470.
// Replaces the uniform byte fill at 0x004ac563-0x004ac603 with a per-texel
// fill that writes ambient bytes directly to the lightmap buffer.
CodeInjection lightmap_per_texel_ambient_nolights_injection{
    0x004ac563, // FLD [ESP+0x78] — start of ambient byte conversion
    [](auto& regs) {
        // NOTE: trampoline is null here (SubHook can't decode FPU opcode 0xD8 at 0x4ac567).
        // Every code path MUST set regs.eip before returning.

        uintptr_t surface = regs.esi;
        int width = *reinterpret_cast<int*>(surface + 0x18);
        int height = *reinterpret_cast<int*>(surface + 0x1c);
        if (width <= 0 || height <= 0) {
            regs.eip = 0x004aca40;
            return;
        }

        SurfaceUVParams p;
        if (!init_surface_uv_params(surface, p)) {
            regs.eip = 0x004aca40;
            return;
        }

        if (s_ambient_room_count == 0) {
            // No custom ambient rooms — replicate original uniform byte fill inline.
            // Cannot fall through to original code because trampoline is null at this address.
            float ar = *reinterpret_cast<float*>(0x0057c968);
            float ag = *reinterpret_cast<float*>(0x0057c96c);
            float ab = *reinterpret_cast<float*>(0x0057c970);
            int br = static_cast<int>(ar * 128.0f);
            int bg = static_cast<int>(ag * 128.0f);
            int bb = static_cast<int>(ab * 128.0f);
            if (br > 255) br = 255;
            if (bg > 255) bg = 255;
            if (bb > 255) bb = 255;

            uintptr_t lm = *reinterpret_cast<uintptr_t*>(surface + 0xC);
            auto* buf = reinterpret_cast<uint8_t*>(*reinterpret_cast<uintptr_t*>(lm + 0xC));
            int stride = *reinterpret_cast<int*>(lm + 4);

            for (int row = 0; row < height; row++) {
                for (int col = 0; col < width; col++) {
                    int off = ((p.ystart + row) * stride + (p.xstart + col)) * 3;
                    buf[off]     = static_cast<uint8_t>(br);
                    buf[off + 1] = static_cast<uint8_t>(bg);
                    buf[off + 2] = static_cast<uint8_t>(bb);
                }
            }

            regs.eip = 0x004aca40;
            return;
        }

        uintptr_t lm = *reinterpret_cast<uintptr_t*>(surface + 0xC);
        auto* buf = reinterpret_cast<uint8_t*>(*reinterpret_cast<uintptr_t*>(lm + 0xC));
        int stride = *reinterpret_cast<int*>(lm + 4);

        for (int row = 0; row < height; row++) {
            for (int col = 0; col < width; col++) {
                float wx, wy, wz;
                texel_to_world(p, col, row, wx, wy, wz);
                float ar, ag, ab;
                get_ambient_at(wx, wy, wz, ar, ag, ab);
                // Match original conversion: float * 128.0f → byte (DAT_0055c870 = 128.0f)
                int br = static_cast<int>(ar * 128.0f);
                int bg = static_cast<int>(ag * 128.0f);
                int bb = static_cast<int>(ab * 128.0f);
                if (br > 255) br = 255;
                if (bg > 255) bg = 255;
                if (bb > 255) bb = 255;
                int off = ((p.ystart + row) * stride + (p.xstart + col)) * 3;
                buf[off]     = static_cast<uint8_t>(br);
                buf[off + 1] = static_cast<uint8_t>(bg);
                buf[off + 2] = static_cast<uint8_t>(bb);
            }
        }

        regs.eip = 0x004aca40; // skip original fill, set "calculated" flag
    },
};


// Guard against invalid dropped-axis value in the baked lightmap rendering path.
// FUN_004ac470 at 0x004acbc8 does: CALL dword ptr [EAX*4 + 0x57ce68]
// where EAX = surface->dropped (0=X, 1=Y, 2=Z). The function pointer table only
// has valid entries for indices 0-2. A corrupt dropped value would read a null or
// garbage pointer and CALL to address 0, crashing with an all-zero context dump.
// This injection runs right after MOV EAX,[ESI+0x5c] loads the dropped value.
CodeInjection lightmap_dropped_axis_guard{
    0x004acbbb, // LEA ECX,[ESP+0x44] — first instruction after MOV EAX,[ESI+0x5c]
    [](auto& regs) {
        auto dropped = static_cast<unsigned>(regs.eax);
        if (dropped > 2) {
            xlog::error("Lightmap crash prevented: invalid dropped axis {} for surface at 0x{:x} "
                        "(flags=0x{:x}, width={}, height={}, room_index={}, surface_index={}, lm=0x{:x})",
                        static_cast<int>(regs.eax),
                        static_cast<uintptr_t>(regs.esi),
                        static_cast<unsigned>(*reinterpret_cast<uint8_t*>(regs.esi + 0x8)),
                        *reinterpret_cast<int*>(regs.esi + 0x18),
                        *reinterpret_cast<int*>(regs.esi + 0x1c),
                        *reinterpret_cast<int*>(regs.esi + 0x68),
                        *reinterpret_cast<short*>(regs.esi + 0x00), // surface group id at start
                        *reinterpret_cast<uintptr_t*>(regs.esi + 0xC));
            regs.eax = 0; // clamp to valid index to prevent crash
        }
    },
};

void ApplyLightmapPatches()
{
    // Fix pink lightmaps when a face is affected by >= 64 lights
    // Allocate shadow mask buffers on the heap
    shadow_mask_pool = std::make_unique<uint8_t[]>(max_shadow_masks * 0x1000);
    for (int i = 0; i < max_shadow_masks; i++) {
        shadow_mask_ptrs[i] = &shadow_mask_pool[i * 0x1000];
    }

    // Replace light handle-to-pointer with bounds-checked version
    // Prevents corruption on handle=-1, which occurs when trying to add/edit lights after max_scene_lights
    // Original: ptr = pool_base + handle * 0x10C (no validation)
    light_handle_to_pointer_injection.install();

    // Guard against invalid dropped-axis index in baked lightmap rendering
    lightmap_dropped_axis_guard.install();

    // Replace the >= 64 limit check with new limit
    lightmap_light_limit_injection.install();

    // Redirect mask buffer array references from old 64-entry array (0x0057CE78) to new array
    write_mem_ptr(0x004AC7A0 + 4, shadow_mask_ptrs);
    write_mem_ptr(0x004AC888 + 1, shadow_mask_ptrs);

    // Expand per-face light list from 1100 entries (0x006F9FF8)
    write_mem_ptr(0x004887C9, face_light_list);
    write_mem_ptr(0x0048899E, face_light_list);
    write_mem_ptr(0x00488B6F, face_light_list);
    write_mem_ptr(0x00488C52, face_light_list);
    write_mem_ptr(0x00488C59, face_light_list);
    write_mem_ptr(0x00488CB7, face_light_list);
    write_mem_ptr(0x0048911D, face_light_list);
    write_mem_ptr(0x0050364B, face_light_list);
    write_mem_ptr(0x00488C09, face_light_list);
    write_mem_ptr(0x00489519, face_light_list);
    write_mem_ptr(0x00505B26, face_light_list);
    write_mem_ptr(0x00489E59, face_light_list);
    write_mem_ptr(0x005025E8, face_light_list);
    write_mem_ptr(0x0050459C, face_light_list);

    // Expand scene light object pool from 1100 entries (0x006FB248)
    // Redirect pool base address references
    // 0x00487A11 omitted because it's inside 0x00487a00 and avoided by light_handle_to_pointer_injection
    write_mem_ptr(0x00486CA0, light_pool);
    write_mem_ptr(0x00487045, light_pool);
    write_mem_ptr(0x00487A85, light_pool);
    write_mem_ptr(0x00487ABA, light_pool);
    // Redirect pool base+4 (prev pointer field) references
    write_mem_ptr(0x00487A7F, light_pool + 4);
    write_mem_ptr(0x00487AB4, light_pool + 4);
    // Redirect pool base+8 (type/active field) reference
    write_mem_ptr(0x00487A24, light_pool + 8);
    // Redirect pool base+0xC (data field) reference
    write_mem_ptr(0x0048A464, light_pool + 0xC);
    // Update scan end limit
    auto scan_end = reinterpret_cast<uintptr_t>(light_pool + 8) + max_scene_lights * light_entry_size;
    write_mem<uint32_t>(0x00487A34, static_cast<uint32_t>(scan_end));
    // Update count limit
    write_mem<uint32_t>(0x00487A41, max_scene_lights);
    // Update zeroing loop count
    write_mem<uint32_t>(0x00487077, max_scene_lights * light_entry_size / 4);

    // Fix lightmap surface group ID overflow crash (face+0x36 is a signed short)
    // When >32767 lightmap surface groups exist (typically >~45000 faces), the signed
    // short wraps negative. MOVSX sign-extends it to a negative 32-bit index, causing
    // out-of-bounds array access and heap corruption.
    // Fix: patch all MOVSX reads of face+0x36 to MOVZX (0x0FBF -> 0x0FB7), raising the
    // limit from 32767 to 65535 groups. Also fix sentinel checks and group comparisons.

    // MOVSX -> MOVZX for face+0x36 reads from memory (14 sites)
    // Each MOVSX word ptr [reg+0x36] is encoded 0F BF; change second byte to B7 for MOVZX
    write_mem<u8>(0x004aa900, 0xB7); // FUN_004aa610: lightmap surface grouping
    write_mem<u8>(0x004aa929, 0xB7);
    write_mem<u8>(0x004aa946, 0xB7);
    write_mem<u8>(0x004aa99d, 0xB7);
    write_mem<u8>(0x004aab06, 0xB7);
    write_mem<u8>(0x004aad59, 0xB7); // FUN_004aabf0: batch lightmap calculator
    write_mem<u8>(0x004aad8a, 0xB7);
    write_mem<u8>(0x004ad241, 0xB7); // FUN_004ad160: shadow geometry
    write_mem<u8>(0x004ad27a, 0xB7);
    write_mem<u8>(0x004ae26c, 0xB7); // FUN_004ae050: shadow calculation
    write_mem<u8>(0x004ae73a, 0xB7); // FUN_004ae360: shadow calculation
    write_mem<u8>(0x004ae81f, 0xB7);
    write_mem<u8>(0x004aed4e, 0xB7);
    write_mem<u8>(0x004a45b1, 0xB7); // solid_write: level file export

    // MOVSX -> MOVZX for face+0x36 reads from register (5 sites)
    // These follow 16-bit MOV AX,[face+0x36] and sign-extend AX to 32-bit before use as index
    write_mem<u8>(0x0048778d, 0xB7); // FUN_004872e0: lightmap surface container lookup
    write_mem<u8>(0x00487864, 0xB7);
    write_mem<u8>(0x0049b955, 0xB7); // FUN_0049b550: geo-cache build
    write_mem<u8>(0x0049bea0, 0xB7);
    write_mem<u8>(0x0049bed3, 0xB7);

    // Fix sentinel checks: JLE -> JE (0x7E -> 0x74)
    // Original: CMP AX,0xFFFF; JLE skip — skips all negative values (wrong for groups > 32767)
    // Fixed: CMP AX,0xFFFF; JE skip — skips only the 0xFFFF "no group" sentinel
    write_mem<u8>(0x00487781, 0x74); // FUN_004872e0
    write_mem<u8>(0x00487858, 0x74);
    write_mem<u8>(0x0049b94b, 0x74); // FUN_0049b550

    // Note: CMP AX,BX; JLE at 0x0049be96 and 0x0049bec9 left as signed — BX can be
    // 0xFFFF ("no batch" sentinel), and unsigned JBE would cause all faces to be skipped.
    // Signed comparison is correct for group IDs 0-32767 and safe for the sentinel case.

    // -smoothlights: experimental lightmap seam fix at portal boundaries
    // Note: GetCommandLineA() is used instead of argv/argc because this runs during DLL
    // init, before the CRT has populated argv/argc at their fixed addresses.
    bool smooth_lights = std::strstr(GetCommandLineA(), "-smoothlights") != nullptr;

    if (smooth_lights) {
        // Fix lightmap seam at portal boundaries
        //
        // Root cause: FUN_004aa610 (surface group flood fill) uses face+0x44 (room pointer)
        // as a hard boundary. When a portal brush splits a face, the fragments end up in
        // different rooms and can never share a surface group. They get independent lightmaps
        // with independent shadow calculations, producing a visible discontinuity.
        //
        // Fix: Remove the room boundary from the flood fill's candidate filter. The existing
        // coplanarity test (normal angle > 0.999, plane distance match) at 0x004aa897 already
        // prevents non-coplanar faces from being merged. With this patch, coplanar portal-split
        // faces join the same surface group and share a single unified lightmap — the shadow
        // calculation covers the entire merged surface, eliminating the seam entirely.

        // Part 1: Allow surface group flood fill to cross room boundaries.
        // NOP the JNZ at 0x004aa807 that skips candidates with a different face+0x44 room ptr.
        // Original: CMP EDX,[EAX+ECX]; JNZ +0x2C (skip to next candidate)
        // Patched: CMP EDX,[EAX+ECX]; NOP NOP (fall through to dedup check)
        write_mem<u8>(0x004aa807, 0x90);
        write_mem<u8>(0x004aa808, 0x90);

        // Part 1b: Force global light/face lists at read sites.
        // surface+0x68 (room_index) is kept intact so ambient color lookup still uses per-room
        // ambient. Instead, patch the 4 sites that branch on room_index==-1 for light gathering
        // and face iteration to always take the global path (JZ → JMP).
        //
        // FUN_004ac470 light gathering: two JZ short (74 → EB)
        //   0x004ac4b1: JZ +0E → JMP +0E (skip per-room light list, use global)
        //   0x004aca5b: JZ +12 → JMP +12 (second light-gathering call, same pattern)
        write_mem<u8>(0x004ac4b1, 0xEB);
        write_mem<u8>(0x004aca5b, 0xEB);
        // FUN_004ae360 face iteration: JZ near (0F 84 → NOP + JMP near: 90 E9)
        //   0x004ae70a: if room_index == -1, use global face list (face+0x54 linked list)
        //   Original: 0F 84 F3 00 00 00 (JZ +0xF3)
        //   Patched:  90 E9 F3 00 00 00 (NOP; JMP +0xF3) — same target, unconditional
        write_mem<u8>(0x004ae70a, 0x90);
        write_mem<u8>(0x004ae70b, 0xE9);
        // FUN_004ad160 face iteration: JZ short (74 → EB)
        //   0x004ad219: if room_index == -1, use global face list (GSolid+0x70)
        write_mem<u8>(0x004ad219, 0xEB);

        // Part 2: Enable cross-surface lightmap blending on all surfaces.
        // Even with unified surface groups, edge blending between DIFFERENT groups still needs
        // should_smooth=true. Force it for all surfaces (was gated on smoothing_groups != 0).
        // Side effect: minimum lightmap size becomes 8x8 instead of 4x4.
        write_mem<u8>(0x004a9da0, 0x01); // MOV byte ptr [ESI+9], 0 -> MOV byte ptr [ESI+9], 1

        // Part 3: Allow cross-surface blending across room boundaries for coplanar surfaces.
        // Fallback for cases where faces couldn't be merged into the same group (e.g., the
        // merged lightmap would exceed the 64x64 texel maximum).
        lightmap_cross_room_blend_injection.install();
        // Part 4b: Per-texel ambient fill for cross-room surfaces.
        // When a surface spans room boundaries, vary ambient per-texel instead of
        // using a single room's ambient for the entire surface.
        lightmap_per_texel_ambient_fill_injection.install();
        lightmap_per_texel_ambient_nolights_injection.install();

        // Part 4: Fix per-room ambient not applied during lightmap calculation.
        // Pre-existing bug: room linker ambient properties are only written to GRoom objects
        // by the room properties dialog. After geometry rebuild, fresh rooms lose ambient.
        lightmap_apply_room_ambient_injection.install();
    }
}
