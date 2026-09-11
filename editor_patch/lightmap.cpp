#include <windows.h>
#include <algorithm>
#include <memory>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <map>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <patch_common/CodeInjection.h>
#include <patch_common/FunHook.h>
#include <patch_common/MemUtils.h>
#include <patch_common/ShortTypes.h>
#include <xlog/xlog.h>
#include "level.h"
#include "lightmap_mesh_occluders.h"


// High resolution lightmaps: 256x256 atlas pages, 254 texel fragments and 4x base density.
// The fragment cap is 254 rather than the 255 the u8 surface rect allows because FUN_004a5f60
// reserves w + 2*border columns in a page and the u8 x/y fields cap a page at 256 texels.
static constexpr int lm_stock_page_size = 128;
static constexpr int lm_highres_page_size = 256;
static constexpr int lm_stock_fragment_max = 64;
static constexpr int lm_highres_fragment_max = 254;
static constexpr int lm_max_fragment_texels = lm_highres_fragment_max * lm_highres_fragment_max;

// Max lights that can be processed per face (shadow mask buffer limit).
// Faces with more lights than this get the pink fill safety fallback.
static constexpr int max_shadow_masks = 1024;
static void* shadow_mask_ptrs[max_shadow_masks];
static std::unique_ptr<uint8_t[]> shadow_mask_pool;
static int shadow_mask_texels = 0;

// The scanline rasteriser FUN_004abed0 clamps a span's right edge to w inclusive, so it can
// store one byte past w*h; every entry carries a spare byte for that.
static bool shadow_mask_reserve(int texels)
{
    if (texels <= shadow_mask_texels) {
        return true;
    }
    const std::size_t entry = static_cast<std::size_t>(texels) + 1;
    std::unique_ptr<uint8_t[]> pool{new (std::nothrow) uint8_t[entry * max_shadow_masks]};
    if (!pool) {
        xlog::error("Lightmap: cannot grow the shadow mask pool to {} texels per light", texels);
        return false;
    }
    shadow_mask_pool = std::move(pool);
    shadow_mask_texels = texels;
    for (int i = 0; i < max_shadow_masks; i++) {
        shadow_mask_ptrs[i] = shadow_mask_pool.get() + static_cast<std::size_t>(i) * entry;
    }
    return true;
}

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

// Every bake fix below is gated on the level's "Legacy lighting" property; when it is set each
// patched path falls through to byte-identical stock behaviour.
static bool bake_fixes_active()
{
    auto* level = CDedLevel::Get();
    return level && !level->GetAlpineLevelProperties().legacy_lighting;
}

// -smoothlights forces the cross-room surface merging on even for legacy levels.
static bool g_force_smooth_lights = false;

static bool smooth_lights_active()
{
    return g_force_smooth_lights || bake_fixes_active();
}

// Independent of Legacy lighting: a legacy level can still be baked at high resolution.
static bool highres_lightmaps_active()
{
    auto* level = CDedLevel::Get();
    return level && level->GetAlpineLevelProperties().highres_lightmaps;
}

static bool sun_liquid_occludes_active()
{
    auto* level = CDedLevel::Get();
    return !level || level->GetAlpineLevelProperties().sun_liquid_occludes;
}

static bool invisible_faces_occlude_active()
{
    auto* level = CDedLevel::Get();
    return level && level->GetAlpineLevelProperties().invisible_faces_occlude;
}

static bool alpha_faces_occlude_active()
{
    auto* level = CDedLevel::Get();
    return level && level->GetAlpineLevelProperties().alpha_faces_occlude;
}

// Which compiled faces belong to a brush flagged "No shadow cast".
//
// CSG carries a source brush face's id (GFace +0x38) onto every compiled fragment it produces,
// which is what the geoable brush -> room mapping already relies on (level.cpp
// find_room_by_face_ids). The id alone is NOT enough: RED's brush clone copies a source brush's
// GFace records verbatim, so a pasted brush shares its face ids with the original - on
// dmabruptdecayrc2 crate uid 11143 carries ids 6888-6893, which also sit on a wall thirty units
// away. Each candidate must therefore also land inside the flagged brush's own bounds. Those are
// taken as pos +/- the geometry's local radius, which needs no assumption about how the brush's
// orientation matrix composes. A brush's own geometry is in brush local space and so is a mover
// solid built from it (measured: a mover's compiled planes are the brush's local planes
// unchanged), while the static solid is in world space.
struct NoShadowCastBrush {
    Vector3 pos;
    float radius;
    int uid;
};

class NoShadowCastFilter
{
public:
    void build();
    bool empty() const { return owners_.empty(); }

    // uid of the flagged brush this compiled face belongs to, or -1
    int owner(uintptr_t face, bool local_space) const
    {
        const auto it = owners_.find(*reinterpret_cast<int*>(face + 0x38));
        if (it == owners_.end()) {
            return -1;
        }
        const auto* lo = reinterpret_cast<const float*>(face + 0x10);
        const auto* hi = reinterpret_cast<const float*>(face + 0x1c);
        for (const auto& b : it->second) {
            const float c[3] = {local_space ? 0.0f : b.pos.x, local_space ? 0.0f : b.pos.y,
                                local_space ? 0.0f : b.pos.z};
            const float r = b.radius + 0.05f;
            bool inside = true;
            for (int a = 0; a < 3 && inside; a++) {
                inside = lo[a] <= c[a] + r && hi[a] >= c[a] - r;
            }
            if (inside) {
                return b.uid;
            }
        }
        return -1;
    }

private:
    std::unordered_map<int, std::vector<NoShadowCastBrush>> owners_;
};

// Per-brush tally of the current bake, so a flag that resolved nothing can be reported instead of
// silently doing nothing. Reset by the two Calculate Lighting hooks.
static std::map<int, int> g_no_shadow_cast_dropped;
static bool g_occluder_tree_built = false;

void NoShadowCastFilter::build()
{
    owners_.clear();
    auto* level = CDedLevel::Get();
    if (!level) {
        return;
    }
    const auto& uids = level->GetAlpineLevelProperties().no_shadow_cast_brush_uids;
    if (uids.empty()) {
        return;
    }
    const std::unordered_set<int32_t> flagged{uids.begin(), uids.end()};
    BrushNode* head = level->brush_list;
    if (!head) {
        return;
    }
    BrushNode* node = head;
    do {
        auto* geom = flagged.count(node->uid) ? static_cast<GSolid*>(node->geometry) : nullptr;
        if (geom) {
            // furthest local corner, so the bound survives any brush orientation
            float extent[3] = {0.0f, 0.0f, 0.0f};
            for (GFace* face = geom->face_list_head; face; face = face->next_solid) {
                const float* box = &face->bounding_box_min.x;
                for (int a = 0; a < 3; a++) {
                    extent[a] = std::max({extent[a], std::abs(box[a]), std::abs(box[a + 3])});
                }
            }
            const float radius = std::sqrt(extent[0] * extent[0] + extent[1] * extent[1] +
                                           extent[2] * extent[2]);
            for (GFace* face = geom->face_list_head; face; face = face->next_solid) {
                if (face->face_id >= 0) {
                    owners_[face->face_id].push_back({node->pos, radius, node->uid});
                }
            }
        }
        node = node->next;
    } while (node && node != head);
}

static float lm_read_const(uintptr_t addr)
{
    return *reinterpret_cast<const float*>(addr);
}

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

        // Grow the shadow mask pool to whatever this surface needs; a fragment that cannot be
        // covered (bad dimensions or out of memory) still takes the stock pink fill fallback.
        // The reservation is width * max(width, height), not width * height: the per-texel
        // rasteriser FUN_004ae050 bounds BOTH of its loops with the width at 0x004ae334 and
        // 0x004ae348, so it stores width^2 bytes. Stock's 4096 byte entries hid that because a
        // stock fragment is at most 64 wide.
        int width = *reinterpret_cast<int*>(regs.esi + 0x18);
        int height = *reinterpret_cast<int*>(regs.esi + 0x1c);
        if (width <= 0 || height <= 0 || width > lm_highres_page_size ||
            height > lm_highres_page_size ||
            !shadow_mask_reserve(width * std::max(width, height))) {
            xlog::error("Lightmap: cannot cover a {}x{} surface at 0x{:x} with shadow masks! "
                        "Falling back to pink fill",
                        width, height, static_cast<uintptr_t>(regs.esi));
            regs.eip = 0x004AC9E4; // pink fill safety fallback
        }
        else if (light_count >= max_shadow_masks) {
            regs.eip = 0x004AC9E4; // pink fill safety fallback
        }
        else {
            regs.eip = 0x004AC611; // normal lightmap processing
        }
    },
    // no trampoline: the 5 byte window is "CMP EDI,0x40" plus the first two bytes of the JGE
    // behind it, which cannot be relocated; every path above sets eip
    false,
};

// FUN_004a6510 takes the pixel buffer of a new lightmap page straight from FUN_0052ee74 and never
// initialises it. Nothing ever writes the gap FUN_004a5f60 reserves between fragments - the fills
// in FUN_004ac470 and the ring copy FUN_004abad0 both stay inside a surface's own rect - so those
// texels reach the saved RFL as whatever the heap block last held, which is what made two bakes of
// one level differ. Replaces "MOV ECX,[ESI+8]; MOV EDX,[ESI+4]" (the JMP at 0x004a653b lands on the
// injection itself); ESP+0x14 is the caller supplied buffer, which is borrowed, not allocated.
CodeInjection lightmap_page_clear_injection{
    0x004a6540,
    [](auto& regs) {
        const uintptr_t page = regs.esi;
        const int w = *reinterpret_cast<int*>(page + 4);
        const int h = *reinterpret_cast<int*>(page + 8);
        auto* pixels = *reinterpret_cast<std::uint8_t**>(page + 0xc);
        const auto supplied =
            *reinterpret_cast<uintptr_t*>(static_cast<uintptr_t>(regs.esp) + 0x14);
        if (!supplied && pixels && w > 0 && h > 0) {
            std::memset(pixels, 0, static_cast<std::size_t>(w) * h * 3);
        }
        regs.ecx = h;
        regs.edx = w;
        regs.eip = 0x004a6546;
    },
    false, // no trampoline: the injection fully replaces the two loads
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
        if (!smooth_lights_active()) {
            regs.eip = 0x004ab07c;
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
    // no trampoline: the 5 byte window is "CMP EDX,ESI" plus the first three bytes of the JNZ
    // behind it, which cannot be relocated; every path above sets eip
    false,
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
        s_ambient_room_count = 0;
        if (!smooth_lights_active()) return;
        uintptr_t gsolid = regs.ecx;
        if (!gsolid) return;

        auto* level = CDedLevel::Get();
        if (!level) return;

        // Room linkers are the room effect objects
        const auto& room_linkers = level->room_effects;
        if (room_linkers.size <= 0 || !room_linkers.data_ptr) return;

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

        for (int i = 0; i < room_linkers.size; i++) {
            auto* linker = static_cast<DedRoomEffect*>(room_linkers.data_ptr[i]);
            if (!linker) continue;

            if (linker->effect_type != 3) continue; // only ambient linkers

            const Vector3& pos = linker->pos;

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
                if (pos.x >= bb.mn[0] && pos.x <= bb.mx[0] &&
                    pos.y >= bb.mn[1] && pos.y <= bb.mx[1] &&
                    pos.z >= bb.mn[2] && pos.z <= bb.mx[2]) {
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
                    *reinterpret_cast<uint32_t*>(room + 0x46) = linker->ambient_color;
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
// Returns the tightest-fitting custom-ambient room, or the surface ambient stock already resolved.
static void get_ambient_at(float wx, float wy, float wz,
                           float fallback_r, float fallback_g, float fallback_b,
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
        r = fallback_r;
        g = fallback_g;
        b = fallback_b;
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

        // stock already resolved this surface's ambient into these frame slots
        const uintptr_t esp = static_cast<uintptr_t>(regs.esp);
        const float base_r = *reinterpret_cast<float*>(esp + 0x18);
        const float base_g = *reinterpret_cast<float*>(esp + 0x1c);
        const float base_b = *reinterpret_cast<float*>(esp + 0x20);
        const float shadowed = lm_read_const(0x00554720); // 0.5

        auto* buf_r = reinterpret_cast<float*>(0x0138a620);
        auto* buf_g = reinterpret_cast<float*>(0x0140ac20);
        auto* buf_b = reinterpret_cast<float*>(0x0134a620);

        int idx = 0;
        for (int row = 0; row < height; row++) {
            for (int col = 0; col < width; col++) {
                float wx, wy, wz;
                texel_to_world(p, col, row, wx, wy, wz);
                float ar, ag, ab;
                get_ambient_at(wx, wy, wz, base_r, base_g, base_b, ar, ag, ab);
                buf_r[idx] = ar * shadowed;
                buf_g[idx] = ag * shadowed;
                buf_b[idx] = ab * shadowed;
                idx++;
            }
        }

        regs.eip = 0x004ac742; // skip original uniform fill
    },
};

// Per-texel ambient fill for the no-lights path in FUN_004ac470.
// Replaces the uniform byte fill at 0x004ac563-0x004ac603. Stock resolves one ambient colour for
// the whole surface (room ambient if the room defines one, otherwise the global ambient) into the
// three frame slots read below, converts it with __ftol(a * 128.0) and stores the low byte with no
// clamp; that is reproduced exactly whenever the per-texel path is not taken.
CodeInjection lightmap_per_texel_ambient_nolights_injection{
    0x004ac563, // FLD [ESP+0x78] — start of ambient byte conversion
    [](auto& regs) {
        // NOTE: no trampoline (SubHook can't decode FPU opcode 0xD8 at 0x4ac567).
        // Every code path MUST set regs.eip before returning.
        regs.eip = 0x004aca40;

        const uintptr_t esp = static_cast<uintptr_t>(regs.esp);
        const float base_r = *reinterpret_cast<float*>(esp + 0x78);
        const float base_g = *reinterpret_cast<float*>(esp + 0x14);
        const float base_b = *reinterpret_cast<float*>(esp + 0x18);

        const uintptr_t surface = regs.esi;
        const int width = *reinterpret_cast<int*>(surface + 0x18);
        const int height = *reinterpret_cast<int*>(surface + 0x1c);
        if (width <= 0 || height <= 0) {
            return;
        }
        const uintptr_t lm = *reinterpret_cast<uintptr_t*>(surface + 0xC);
        if (!lm) {
            return;
        }
        auto* buf = reinterpret_cast<uint8_t*>(*reinterpret_cast<uintptr_t*>(lm + 0xC));
        if (!buf) {
            return;
        }
        const int stride = *reinterpret_cast<int*>(lm + 4);
        const int xstart = *reinterpret_cast<int*>(surface + 0x10);
        const int ystart = *reinterpret_cast<int*>(surface + 0x14);

        SurfaceUVParams p;
        const bool per_texel = smooth_lights_active() && s_ambient_room_count > 0 &&
                               init_surface_uv_params(surface, p);
        const float scale = lm_read_const(0x0055c870); // 128.0

        for (int row = 0; row < height; row++) {
            for (int col = 0; col < width; col++) {
                float ar = base_r, ag = base_g, ab = base_b;
                if (per_texel) {
                    float wx, wy, wz;
                    texel_to_world(p, col, row, wx, wy, wz);
                    get_ambient_at(wx, wy, wz, base_r, base_g, base_b, ar, ag, ab);
                }
                const int off = ((ystart + row) * stride + (xstart + col)) * 3;
                buf[off]     = static_cast<uint8_t>(static_cast<int>(ar * scale));
                buf[off + 1] = static_cast<uint8_t>(static_cast<int>(ag * scale));
                buf[off + 2] = static_cast<uint8_t>(static_cast<int>(ab * scale));
            }
        }
    },
    false, // no trampoline: the injection fully replaces the fill
};

// ============================================================
// Ray traced shadow masks
// ============================================================
// Stock builds a shadow mask by projecting each occluder polygon from the light position onto the
// receiving fragment's plane and rasterising it (FUN_004ae360). The projection is clipped against a
// six plane frustum whose four side planes run through the light position and the fragment corners;
// with the sun's virtual origin 5000 units away those planes carry roughly 0.1 world units of
// cancellation error, which is a large fraction of a small fragment's width, so whole occluders get
// clipped away and their shadow is never rasterised at all. Measured on the sun2 rebake the leak
// rate tracks the fragment size exactly: 44.8% of the in-face texels on fragments under one world
// unit across, 0.04% on fragments 16 units and larger.
//
// The projector is bypassed for every light of a fixed pipeline bake, and for the sun in a legacy
// one: the mask is traced per texel against a BVH over the solid's eligible faces. Legacy is only
// promised to be byte-identical to stock for STOCK content, and the sun is not stock, so it takes
// the deterministic tracer there too rather than masquerading as a point light 5000 units away.
//
// Per-ray eligibility, as skip_flags/oneside_flags in OccQuery (build() drops portals, has-alpha
// 0x40 faces and the faces of "No shadow cast" brushes outright, and a surface never shadows
// itself):
//   show sky 0x1      occludes every light but the sun, which is what enters through it
//   liquid 0x4        sun only, per "Water blocks sunlight"; never occludes another light
//   invisible 0x2000  one-sided, per "Invisible faces block light"; else transparent
//   alpha textured    per "Alpha-textured faces block light"; off by default, which is what stock
//                     does (FUN_004bcc60) - but never for the two classes above, whose own
//                     property decides them
// Deliberate deviation under legacy: stock walks the receiving surface's own room face list when
// the surface has a room, the tracer always has the whole solid - it can only add occluders.

// Origin lift and ray trims: the sample point is pushed 0.02 off the receiving plane and hits
// closer than 0.01 are ignored. A hit still inside a 0.05 band of the receiving plane counts as
// the surface's own geometry only when it is parallel to it - as a blanket minimum ray distance
// the band also swallowed thin geometry standing on the surface.
static constexpr float lm_ray_lift = 0.02f;
static constexpr float lm_ray_eps = 0.01f;
static constexpr float lm_ray_band = 0.05f;
static constexpr float lm_oneside_eps = 1.0e-4f;

// Synthetic OccTri flag, outside the 16 bit face flags word: the face's texture carries an alpha
// channel, which is what the stock occluder filter rejects it for (FUN_004bcc60 at 0x004aed62).
static constexpr unsigned lm_occ_alpha_texture = 0x80000000u;

namespace {

struct Vec3f {
    float x, y, z;
};

inline Vec3f vsub(const Vec3f& a, const Vec3f& b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

inline Vec3f vcross(const Vec3f& a, const Vec3f& b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline float vdot(const Vec3f& a, const Vec3f& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

struct OccTri {
    Vec3f v0, e1, e2;
    Vec3f normal;
    Vec3f centroid;
    Vec3f bmin, bmax;
    int surf_id;
    int mesh_uid; // owning alpine mesh object, -1 for a brush face
    unsigned flags;
};

// A node build_range never got to finish (bad_alloc deeper in the recursion) has to read as an
// internal node with no children, not as a leaf over triangle 0 and not as a child index of 0.
struct OccNode {
    Vec3f bmin{}, bmax{};
    int start = 0, count = 0; // count > 0 marks a leaf
    int left = -1, right = -1; // build_range lays the left subtree out between them, so both are stored
};

constexpr int occ_leaf_size = 8;
constexpr int occ_max_depth = 48;
constexpr int occ_max_face_verts = 128;

struct OccQuery {
    Vec3f origin;
    Vec3f dir;
    Vec3f surf_normal;
    float nd;   // dot(surf_normal, dir)
    float tmin;
    float tmax;
    int skip_surf;
    unsigned skip_flags;    // face flags that make a triangle transparent to this light
    unsigned oneside_flags; // face flags that make it block only rays hitting its front
};

class OccluderTree
{
public:
    bool build(uintptr_t solid, const NoShadowCastFilter& no_shadow_cast, bool local_space);
    bool empty() const { return tris_.empty(); }
    void clear()
    {
        tris_.clear();
        order_.clear();
        nodes_.clear();
        skipped_faces_ = 0;
    }
    int skipped_faces() const { return skipped_faces_; }
    bool occluded(const OccQuery& q) const;

private:
    int build_range(int begin, int end, int depth);

    int skipped_faces_ = 0;
    std::vector<OccTri> tris_;
    std::vector<int> order_;
    std::vector<OccNode> nodes_;
};

// A fan from vertex 0 only reproduces a convex polygon. CSG leaves plenty of non-convex faces
// behind (205 eligible occluders on sun2.rfl), and a fan over one of those both invents shadow
// casters outside the face and drops parts of it, so the loop is ear clipped in its own plane.
int triangulate_face(const Vec3f* v, int n, int* out)
{
    if (n < 3 || n > occ_max_face_verts) {
        return 0;
    }
    Vec3f nrm{0.0f, 0.0f, 0.0f};
    for (int i = 0; i < n; i++) {
        const Vec3f& a = v[i];
        const Vec3f& b = v[(i + 1) % n];
        nrm.x += (a.y - b.y) * (a.z + b.z);
        nrm.y += (a.z - b.z) * (a.x + b.x);
        nrm.z += (a.x - b.x) * (a.y + b.y);
    }
    const float ax = std::abs(nrm.x);
    const float ay = std::abs(nrm.y);
    const float az = std::abs(nrm.z);
    const int drop = (ax >= ay && ax >= az) ? 0 : (ay >= az ? 1 : 2);

    float px[occ_max_face_verts], py[occ_max_face_verts];
    int idx[occ_max_face_verts];
    for (int i = 0; i < n; i++) {
        const float* c = &v[i].x;
        px[i] = c[drop == 0 ? 1 : 0];
        py[i] = c[drop == 2 ? 1 : 2];
        idx[i] = i;
    }
    float area = 0.0f;
    for (int i = 0; i < n; i++) {
        const int j = (i + 1) % n;
        area += px[i] * py[j] - px[j] * py[i];
    }
    if (area < 0.0f) {
        for (int i = 0; i < n / 2; i++) {
            std::swap(idx[i], idx[n - 1 - i]);
        }
    }

    auto side = [&](int a, int b, float x, float y) {
        return (px[b] - px[a]) * (y - py[a]) - (py[b] - py[a]) * (x - px[a]);
    };

    int count = 0;
    int m = n;
    while (m > 3) {
        bool clipped = false;
        for (int i = 0; i < m; i++) {
            const int i0 = idx[(i + m - 1) % m];
            const int i1 = idx[i];
            const int i2 = idx[(i + 1) % m];
            if (side(i0, i1, px[i2], py[i2]) <= 0.0f) {
                continue; // reflex or degenerate corner
            }
            bool ok = true;
            for (int k = 0; k < m && ok; k++) {
                const int p = idx[k];
                if (p == i0 || p == i1 || p == i2) {
                    continue;
                }
                ok = !(side(i0, i1, px[p], py[p]) > 0.0f && side(i1, i2, px[p], py[p]) > 0.0f &&
                       side(i2, i0, px[p], py[p]) > 0.0f);
            }
            if (!ok) {
                continue;
            }
            out[count * 3] = i0;
            out[count * 3 + 1] = i1;
            out[count * 3 + 2] = i2;
            count++;
            for (int k = i; k < m - 1; k++) {
                idx[k] = idx[k + 1];
            }
            m--;
            clipped = true;
            break;
        }
        if (!clipped) {
            // self intersecting or fully degenerate loop, take the fan and lose nothing that the
            // shipped behaviour did not already lose
            count = 0;
            for (int i = 2; i < n; i++) {
                out[count * 3] = 0;
                out[count * 3 + 1] = i - 1;
                out[count * 3 + 2] = i;
                count++;
            }
            return count;
        }
    }
    out[count * 3] = idx[0];
    out[count * 3 + 1] = idx[1];
    out[count * 3 + 2] = idx[2];
    return count + 1;
}

// Fills in everything the traversal derives from a triangle's corners; false for a degenerate one.
// A non-finite corner is rejected outright: it would otherwise reach the BVH with a NaN bound, and
// every comparison the split and the slab test make against a NaN is false, so it would neither
// sort nor cull. The length test is written so that a NaN fails it too.
bool occ_make_tri(const Vec3f& a, const Vec3f& b, const Vec3f& c, OccTri& t)
{
    for (const Vec3f* v : {&a, &b, &c}) {
        if (!std::isfinite(v->x) || !std::isfinite(v->y) || !std::isfinite(v->z)) {
            return false;
        }
    }
    t.v0 = a;
    t.e1 = vsub(b, a);
    t.e2 = vsub(c, a);
    t.normal = vcross(t.e1, t.e2);
    const float len = std::sqrt(vdot(t.normal, t.normal));
    if (!(len >= 1e-12f) || !std::isfinite(len)) {
        return false;
    }
    t.normal = {t.normal.x / len, t.normal.y / len, t.normal.z / len};
    t.bmin = {std::min({a.x, b.x, c.x}), std::min({a.y, b.y, c.y}), std::min({a.z, b.z, c.z})};
    t.bmax = {std::max({a.x, b.x, c.x}), std::max({a.y, b.y, c.y}), std::max({a.z, b.z, c.z})};
    t.centroid = {(t.bmin.x + t.bmax.x) * 0.5f, (t.bmin.y + t.bmax.y) * 0.5f,
                  (t.bmin.z + t.bmax.z) * 0.5f};
    return true;
}

bool OccluderTree::build(uintptr_t solid, const NoShadowCastFilter& no_shadow_cast, bool local_space)
{
    tris_.clear();
    order_.clear();
    nodes_.clear();
    skipped_faces_ = 0;
    if (!solid) {
        return false;
    }
    constexpr int max_faces = 1 << 21;
    int guard = 0;
    for (uintptr_t face = *reinterpret_cast<uintptr_t*>(solid + 0x70); face && guard < max_faces;
         face = *reinterpret_cast<uintptr_t*>(face + 0x54), guard++) {
        // the engine's face flags are the 16 bit RFL word, so the synthetic bits below own
        // everything above it; masking keeps a stray high bit out of the alpha texture test
        unsigned flags = *reinterpret_cast<unsigned*>(face + 0x28) & 0xffffu;
        if (flags & 0x40u) {
            continue;
        }
        if (*reinterpret_cast<std::int16_t*>(face + 0x34) > 0) {
            continue;
        }
        // the flag covers every ray this tree answers, so the triangles never need to exist
        if (!no_shadow_cast.empty()) {
            const int nsc_uid = no_shadow_cast.owner(face, local_space);
            if (nsc_uid >= 0) {
                skipped_faces_++;
                g_no_shadow_cast_dropped[nsc_uid]++;
                continue;
            }
        }
        const int bitmap = *reinterpret_cast<int*>(face + 0x30);
        // liquid and invisible faces answer to their own level property, so the stock rejection
        // never gets to overrule it - a water texture is alpha capable practically by definition
        if (!(flags & 0x2004u) && bitmap != -1 && bm_has_alpha(bitmap) != 0) {
            flags |= lm_occ_alpha_texture;
        }
        const int surf_id = *reinterpret_cast<std::int16_t*>(face + 0x36);
        // the ear clipper winds every triangle the same way in its projection plane, which flips
        // half of them; the face's own plane normal is what "front" has to mean
        const auto* face_normal = reinterpret_cast<const float*>(face);
        Vec3f verts[occ_max_face_verts];
        int n = 0;
        bool truncated = false;
        const uintptr_t head = *reinterpret_cast<uintptr_t*>(face + 0x40);
        for (uintptr_t node = head; node;) {
            if (n == occ_max_face_verts) {
                truncated = true;
                break;
            }
            auto* pos = *reinterpret_cast<const float**>(node);
            if (!pos) {
                break;
            }
            verts[n++] = {pos[0], pos[1], pos[2]};
            node = *reinterpret_cast<uintptr_t*>(node + 0x14);
            if (node == head) {
                break;
            }
        }
        if (truncated) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                xlog::warn("Lightmap: a face has more than {} vertices, only the first {} of them "
                           "cast a baked shadow",
                           occ_max_face_verts, occ_max_face_verts);
            }
        }
        int fan[(occ_max_face_verts - 2) * 3];
        const int tri_count = triangulate_face(verts, n, fan);
        for (int i = 0; i < tri_count; i++) {
            OccTri t{};
            if (!occ_make_tri(verts[fan[i * 3]], verts[fan[i * 3 + 1]], verts[fan[i * 3 + 2]], t)) {
                continue;
            }
            if (t.normal.x * face_normal[0] + t.normal.y * face_normal[1] +
                    t.normal.z * face_normal[2] <
                0.0f) {
                t.normal = {-t.normal.x, -t.normal.y, -t.normal.z};
            }
            t.surf_id = surf_id;
            t.mesh_uid = -1;
            t.flags = flags;
            tris_.push_back(t);
        }
    }
    // Alpine mesh objects live in world space, so they only belong to the static solid's tree; a
    // mover's tree is brush local and answers only the rays cast onto that mover.
    if (!local_space) {
        std::vector<MeshOccluderTri> mesh_tris;
        if (lightmap_collect_mesh_occluders(mesh_tris)) {
            for (const MeshOccluderTri& m : mesh_tris) {
                OccTri t{};
                if (!occ_make_tri({m.v0.x, m.v0.y, m.v0.z}, {m.v1.x, m.v1.y, m.v1.z},
                                  {m.v2.x, m.v2.y, m.v2.z}, t)) {
                    continue;
                }
                // no surface owns a mesh triangle and none of the face flag classes apply to it,
                // so it is a plain two-sided occluder that only answers to the alpha property
                t.surf_id = -1;
                t.mesh_uid = m.uid;
                t.flags = m.alpha ? lm_occ_alpha_texture : 0u;
                tris_.push_back(t);
            }
        }
    }
    if (tris_.empty()) {
        return true;
    }
    order_.resize(tris_.size());
    for (std::size_t i = 0; i < order_.size(); i++) {
        order_[i] = static_cast<int>(i);
    }
    nodes_.reserve(tris_.size() * 2);
    build_range(0, static_cast<int>(order_.size()), 0);
    return true;
}

int OccluderTree::build_range(int begin, int end, int depth)
{
    const int self = static_cast<int>(nodes_.size());
    nodes_.emplace_back();
    Vec3f bmin{1e30f, 1e30f, 1e30f};
    Vec3f bmax{-1e30f, -1e30f, -1e30f};
    Vec3f cmin = bmin;
    Vec3f cmax = bmax;
    for (int i = begin; i < end; i++) {
        const OccTri& t = tris_[order_[i]];
        bmin = {std::min(bmin.x, t.bmin.x), std::min(bmin.y, t.bmin.y), std::min(bmin.z, t.bmin.z)};
        bmax = {std::max(bmax.x, t.bmax.x), std::max(bmax.y, t.bmax.y), std::max(bmax.z, t.bmax.z)};
        cmin = {std::min(cmin.x, t.centroid.x), std::min(cmin.y, t.centroid.y),
                std::min(cmin.z, t.centroid.z)};
        cmax = {std::max(cmax.x, t.centroid.x), std::max(cmax.y, t.centroid.y),
                std::max(cmax.z, t.centroid.z)};
    }
    nodes_[self].bmin = bmin;
    nodes_[self].bmax = bmax;
    const int count = end - begin;
    if (count <= occ_leaf_size || depth >= occ_max_depth) {
        nodes_[self].start = begin;
        nodes_[self].count = count;
        nodes_[self].left = -1;
        nodes_[self].right = -1;
        return self;
    }
    const float ex = cmax.x - cmin.x, ey = cmax.y - cmin.y, ez = cmax.z - cmin.z;
    const int axis = (ex >= ey && ex >= ez) ? 0 : (ey >= ez ? 1 : 2);
    const int mid = begin + count / 2;
    std::nth_element(order_.begin() + begin, order_.begin() + mid, order_.begin() + end,
                     [&](int a, int b) {
                         const float ca = (&tris_[a].centroid.x)[axis];
                         const float cb = (&tris_[b].centroid.x)[axis];
                         // occ_make_tri rejects non-finite corners, so a NaN centroid cannot get
                         // here; ordering them last anyway keeps this a strict weak ordering
                         // whatever reaches it, which nth_element needs to stay in bounds
                         const bool na = std::isnan(ca);
                         const bool nb = std::isnan(cb);
                         if (na || nb) {
                             return na == nb ? a < b : nb;
                         }
                         return ca != cb ? ca < cb : a < b;
                     });
    nodes_[self].start = 0;
    nodes_[self].count = 0;
    // the right child lands after the whole left subtree, never at left + 1
    const int left = build_range(begin, mid, depth + 1);
    const int right = build_range(mid, end, depth + 1);
    nodes_[self].left = left;
    nodes_[self].right = right;
    return self;
}

bool OccluderTree::occluded(const OccQuery& qy) const
{
    if (nodes_.empty()) {
        return false;
    }
    // axis aligned rays are the common case here, and a zero component would turn the slab test
    // into 0 * inf = NaN on any node whose face lies exactly on the ray origin
    auto safe_inv = [](float c) {
        if (c > -1e-8f && c < 1e-8f) {
            return c < 0.0f ? -1.0e8f : 1.0e8f;
        }
        return 1.0f / c;
    };
    const Vec3f& o = qy.origin;
    const Vec3f& d = qy.dir;
    const float inv[3] = {safe_inv(d.x), safe_inv(d.y), safe_inv(d.z)};
    const float org[3] = {o.x, o.y, o.z};
    int stack[occ_max_depth * 2 + 8];
    constexpr int stack_size = static_cast<int>(sizeof(stack) / sizeof(stack[0]));
    int sp = 0;
    stack[sp++] = 0;
    while (sp > 0) {
        const int node_index = stack[--sp];
        if (node_index < 0 || static_cast<std::size_t>(node_index) >= nodes_.size()) {
            continue;
        }
        const OccNode& nd = nodes_[node_index];
        float t0 = qy.tmin, t1 = qy.tmax;
        const float* lo = &nd.bmin.x;
        const float* hi = &nd.bmax.x;
        for (int a = 0; a < 3; a++) {
            float na = (lo[a] - org[a]) * inv[a];
            float fa = (hi[a] - org[a]) * inv[a];
            if (na > fa) {
                std::swap(na, fa);
            }
            t0 = na > t0 ? na : t0;
            t1 = fa < t1 ? fa : t1;
        }
        if (!(t0 <= t1)) {
            continue;
        }
        if (nd.count > 0) {
            for (int i = nd.start; i < nd.start + nd.count; i++) {
                const OccTri& t = tris_[order_[i]];
                if (t.surf_id == qy.skip_surf) {
                    continue;
                }
                if ((t.flags & qy.skip_flags) != 0) {
                    continue;
                }
                // A one-sided face lets light through its back and stops it at its front. The
                // rule is stated on the light, but these rays run the other way, receiver to
                // light, so it has to be restated on the ray: light travels along L = -d, it
                // arrives on the front when it opposes the normal, dot(L, n) < 0, and that is
                // dot(d, n) > 0. Grazing hits (|dot| <= eps) pass, the answer is arbitrary there.
                if ((t.flags & qy.oneside_flags) != 0 && vdot(d, t.normal) < lm_oneside_eps) {
                    continue;
                }
                const Vec3f p = vcross(d, t.e2);
                const float det = vdot(t.e1, p);
                if (det > -1e-9f && det < 1e-9f) {
                    continue;
                }
                const float inv_det = 1.0f / det;
                const Vec3f tv = vsub(o, t.v0);
                const float u = vdot(tv, p) * inv_det;
                if (u < 0.0f || u > 1.0f) {
                    continue;
                }
                const Vec3f q = vcross(tv, t.e1);
                const float v = vdot(d, q) * inv_det;
                if (v < 0.0f || u + v > 1.0f) {
                    continue;
                }
                const float dist = vdot(t.e2, q) * inv_det;
                if (dist <= qy.tmin || dist >= qy.tmax) {
                    continue;
                }
                // reject the receiving surface's own plane rather than everything within a ray
                // distance of it, so geometry a few hundredths above the surface still occludes
                if (lm_ray_lift + dist * qy.nd < lm_ray_band &&
                    std::abs(vdot(t.normal, qy.surf_normal)) > 0.999f) {
                    continue;
                }
                return true;
            }
        }
        else {
            // a build that ran out of memory leaves -1 children behind; without this the walk
            // would push them and come back around to node 0 for ever
            if (nd.left >= 0 && sp < stack_size) {
                stack[sp++] = nd.left;
            }
            if (nd.right >= 0 && sp < stack_size) {
                stack[sp++] = nd.right;
            }
        }
    }
    return false;
}

} // namespace

static int g_sun_light_handle = -1;
static void* g_sun_light_ptr = nullptr;
static float g_sun_spread_angle = 0.0f;

// Set for exactly as long as one of the two Calculate Lighting commands is running.
//
// RED also relights a single surface from the viewport (FUN_004e85a0 -> FUN_0049a700 ->
// FUN_004ac470(surface, 1)), and that path reaches FUN_004ae360 - and so the ray caster - with no
// command around it at all. Nothing there ever ends, so a GSolid keyed cache built from it is
// never dropped and outlives the geometry Build Geometry frees under it, and every repaint pays
// for the BVH. The caches are therefore built only inside a bake, which puts live relight back on
// the stock projector exactly as a Legacy lighting level is, and bounds every cache by the
// command that made it.
static bool g_bake_active = false;

// One cache per GSolid; the bake walks the static solid and every mover solid in turn and each
// keeps its own coordinate space, so occluders are never shared between them. Both halves are
// built in a single pass over the solid's face list, which is the only reason the per-surface
// lookups below are not quadratic in the level size. Dropped by the two Calculate Lighting hooks
// so nothing outlives a bake.
struct SolidCache {
    OccluderTree tree;
    bool tree_built = false;
    std::unordered_map<int, std::vector<uintptr_t>> faces_by_surface;
};

static std::unordered_map<uintptr_t, std::unique_ptr<SolidCache>> g_solid_cache;

static SolidCache* lightmap_solid_cache(uintptr_t solid)
{
    if (!solid || !g_bake_active) {
        return nullptr;
    }
    auto it = g_solid_cache.find(solid);
    if (it != g_solid_cache.end()) {
        return it->second.get();
    }
    try {
        auto cache = std::make_unique<SolidCache>();
        int guard = 0;
        for (uintptr_t face = *reinterpret_cast<uintptr_t*>(solid + 0x70); face && guard < (1 << 21);
             face = *reinterpret_cast<uintptr_t*>(face + 0x54), guard++) {
            const int surf_id = *reinterpret_cast<std::int16_t*>(face + 0x36);
            if (surf_id >= 0) {
                cache->faces_by_surface[surf_id].push_back(face);
            }
        }
        return g_solid_cache.emplace(solid, std::move(cache)).first->second.get();
    }
    catch (...) {
        xlog::error("Lightmap: out of memory indexing a solid's faces, falling back to the stock "
                    "bake for it");
        return nullptr;
    }
}

// Built on first use so the no-shadow Calculate Lighting command never pays for it.
static const OccluderTree* lightmap_occluder_tree(uintptr_t solid)
{
    SolidCache* cache = lightmap_solid_cache(solid);
    if (!cache) {
        return nullptr;
    }
    if (!cache->tree_built) {
        try {
            NoShadowCastFilter no_shadow_cast;
            no_shadow_cast.build();
            auto* level = CDedLevel::Get();
            const bool local_space = !level || solid != reinterpret_cast<uintptr_t>(level->solid);
            cache->tree.build(solid, no_shadow_cast, local_space);
            g_occluder_tree_built = true;
            xlog::debug("[NoShadowCast] solid {:#x}: {} occluder faces dropped", solid,
                        cache->tree.skipped_faces());
        }
        catch (...) {
            // build() leaves the triangle list populated and the node array half written, which
            // reads as a usable tree; drop it so empty() reports it and the stock projector, which
            // the message promises, is what actually runs
            cache->tree.clear();
            xlog::error("Lightmap: out of memory building the occluder tree, falling back to the "
                        "stock shadow projector");
        }
        cache->tree_built = true;
    }
    return cache->tree.empty() ? nullptr : &cache->tree;
}

static void lightmap_release_occluders()
{
    g_solid_cache.clear();
    lightmap_mesh_occluders_release();
}

// Face ids only reach the compiled solid through Build Geometry, so a brush flagged after the last
// build resolves nothing and the flag would otherwise be a silent no-op.
static void no_shadow_cast_report()
{
    auto* level = CDedLevel::Get();
    if (!level || !g_occluder_tree_built) {
        return;
    }
    const auto& uids = level->GetAlpineLevelProperties().no_shadow_cast_brush_uids;
    if (uids.empty()) {
        return;
    }
    std::unordered_set<int32_t> live;
    if (BrushNode* head = level->brush_list) {
        BrushNode* node = head;
        do {
            live.insert(node->uid);
            node = node->next;
        } while (node && node != head);
    }
    int total = 0;
    for (const auto& e : g_no_shadow_cast_dropped) {
        total += e.second;
        xlog::debug("[NoShadowCast] brush {}: {} occluder faces dropped", e.first, e.second);
    }
    xlog::info("[NoShadowCast] {} of {} flagged brushes resolved, {} occluder faces dropped",
               g_no_shadow_cast_dropped.size(), uids.size(), total);
    for (int32_t uid : uids) {
        if (g_no_shadow_cast_dropped.count(uid)) {
            continue;
        }
        if (live.count(uid)) {
            xlog::warn("[NoShadowCast] brush {} is flagged but matched no compiled geometry - run "
                       "Build Geometry, the flag did nothing for it", uid);
        }
        else {
            xlog::warn("[NoShadowCast] flagged brush {} no longer exists", uid);
        }
    }
}

// Soft sun sampling: the axis plus two rings of four, all fixed - bakes must be reproducible.
static constexpr int sun_cone_samples = 9;

static void sun_cone_directions(const Vec3f& axis, float spread_deg, Vec3f* out, int& count)
{
    out[0] = axis;
    count = 1;
    if (spread_deg <= 0.0f) {
        return;
    }
    Vec3f up = std::abs(axis.y) < 0.9f ? Vec3f{0.0f, 1.0f, 0.0f} : Vec3f{1.0f, 0.0f, 0.0f};
    Vec3f u = vcross(up, axis);
    float len = std::sqrt(vdot(u, u));
    if (!(len >= 1e-6f)) {
        return;
    }
    u = {u.x / len, u.y / len, u.z / len};
    const Vec3f v = vcross(axis, u);
    const float tan_max = std::tan(spread_deg * 3.14159265358979f / 180.0f);
    for (int ring = 0; ring < 2; ring++) {
        const float r = tan_max * (ring == 0 ? 0.55f : 1.0f);
        const float phase = ring == 0 ? 45.0f : 0.0f;
        for (int k = 0; k < 4; k++) {
            const float a = (phase + 90.0f * static_cast<float>(k)) * 3.14159265358979f / 180.0f;
            const float cs = std::cos(a) * r;
            const float sn = std::sin(a) * r;
            Vec3f d{axis.x + u.x * cs + v.x * sn, axis.y + u.y * cs + v.y * sn,
                    axis.z + u.z * cs + v.z * sn};
            const float l = std::sqrt(vdot(d, d));
            out[count++] = {d.x / l, d.y / l, d.z / l};
        }
    }
}

static bool lightmap_raycast_mask(uintptr_t solid, uintptr_t surface, uintptr_t light,
                                  std::uint8_t* mask)
{
    if (!surface || !light || !mask) {
        return false;
    }
    const int width = *reinterpret_cast<int*>(surface + 0x18);
    const int height = *reinterpret_cast<int*>(surface + 0x1c);
    if (width <= 0 || height <= 0 || width > lm_highres_page_size ||
        height > lm_highres_page_size) {
        return false;
    }
    SurfaceUVParams p;
    if (!init_surface_uv_params(surface, p)) {
        return false;
    }
    // an empty occluder set would mean the face walk found nothing, so leave stock in charge
    const OccluderTree* tree = lightmap_occluder_tree(solid);
    if (!tree) {
        return false;
    }

    const int light_type = *reinterpret_cast<int*>(light + 8);
    // while a mover transform is pushed the engine keeps the light in the solid's own space
    const int local = *reinterpret_cast<int*>(0x0158f414) != 0 ? 0x50 : 0;
    const auto* vec = reinterpret_cast<const float*>(light + 0x0c + local);
    const auto* vec_end = reinterpret_cast<const float*>(light + 0x18 + local);
    const float radius = *reinterpret_cast<const float*>(light + 0x3c);
    const int skip_surf = *reinterpret_cast<int*>(surface);
    const bool is_sun = g_sun_light_ptr && reinterpret_cast<void*>(light) == g_sun_light_ptr;
    const bool one_sided = invisible_faces_occlude_active();
    unsigned skip_flags = 0x4u; // liquid, unless this is the sun and the level asks for it
    if (is_sun) {
        skip_flags = 0x1u; // sky is where the sun enters
        if (!sun_liquid_occludes_active()) {
            skip_flags |= 0x4u;
        }
    }
    if (!one_sided) {
        skip_flags |= 0x2000u;
    }
    if (!alpha_faces_occlude_active()) {
        skip_flags |= lm_occ_alpha_texture;
    }
    const unsigned oneside_flags = one_sided ? 0x2000u : 0u;

    const Vec3f ns{p.nx, p.ny, p.nz};
    Vec3f cone[sun_cone_samples];
    int cone_count = 0;
    if (light_type == 1) {
        Vec3f axis{vec[0], vec[1], vec[2]};
        const float len = std::sqrt(vdot(axis, axis));
        if (!(len >= 1e-6f)) {
            return false;
        }
        axis = {axis.x / len, axis.y / len, axis.z / len};
        sun_cone_directions(axis, is_sun ? g_sun_spread_angle : 0.0f, cone, cone_count);
    }

    auto shade_rows = [&](int row_begin, int row_end) {
        for (int row = row_begin; row < row_end; row++) {
            for (int col = 0; col < width; col++) {
                float wx, wy, wz;
                texel_to_world(p, col, row, wx, wy, wz);
                const Vec3f origin{wx + ns.x * lm_ray_lift, wy + ns.y * lm_ray_lift,
                                   wz + ns.z * lm_ray_lift};
                int lit = 0;
                int taken = 0;
                OccQuery q{};
                q.origin = origin;
                q.surf_normal = ns;
                q.tmin = lm_ray_eps;
                q.skip_surf = skip_surf;
                q.skip_flags = skip_flags;
                q.oneside_flags = oneside_flags;
                if (light_type == 1) {
                    q.tmax = 1.0e6f;
                    for (int k = 0; k < cone_count; k++) {
                        q.dir = cone[k];
                        q.nd = vdot(ns, q.dir);
                        taken++;
                        if (!tree->occluded(q)) {
                            lit++;
                        }
                    }
                }
                else {
                    const int samples = light_type == 4 ? 2 : 1;
                    for (int k = 0; k < samples; k++) {
                        const float* target = k == 0 ? vec : vec_end;
                        Vec3f d{target[0] - origin.x, target[1] - origin.y, target[2] - origin.z};
                        const float len = std::sqrt(vdot(d, d));
                        if (!(len >= 1e-4f)) {
                            taken++;
                            lit++;
                            continue;
                        }
                        if (radius > 0.0f && len > radius) {
                            continue; // outside the light's range, the mask value is never read
                        }
                        q.dir = {d.x / len, d.y / len, d.z / len};
                        q.nd = vdot(ns, q.dir);
                        q.tmax = len - lm_ray_eps;
                        taken++;
                        if (!tree->occluded(q)) {
                            lit++;
                        }
                    }
                }
                const int index = row * width + col;
                mask[index] = taken == 0
                                  ? 0xffu
                                  : static_cast<std::uint8_t>((lit * 255 + taken / 2) / taken);
            }
        }
    };

    unsigned workers = std::thread::hardware_concurrency();
    if (workers < 1) {
        workers = 1;
    }
    workers = std::min<unsigned>(workers, static_cast<unsigned>(height));
    // Texels are independent and the workers write disjoint row ranges, so the result is the same
    // whatever the split is; any worker that cannot be started just leaves its rows to this thread.
    const int chunk = (workers > 1 && height >= 8)
                          ? (height + static_cast<int>(workers) - 1) / static_cast<int>(workers)
                          : height;
    int covered = std::min(height, chunk);
    std::vector<std::thread> pool;
    try {
        pool.reserve(workers > 1 ? workers - 1 : 0);
        while (covered < height && pool.size() + 1 < workers) {
            const int a = covered;
            const int b = std::min(height, a + chunk);
            pool.emplace_back([&shade_rows, a, b] { shade_rows(a, b); });
            covered = b;
        }
    }
    catch (...) {
    }
    shade_rows(0, std::min(height, chunk));
    for (auto& t : pool) {
        t.join();
    }
    if (covered < height) {
        shade_rows(covered, height);
    }
    return true;
}

// ============================================================
// Alpine directional sunlight - lightmap bake contribution
// ============================================================
// A type 1 (directional) gr light is created for the duration of the two
// "Calculate Lighting" commands and destroyed again when they return, so the
// sun never outlives the bake and never shows up in the live editor lighting.
//
// Stock per-lumel accumulation (FUN_004894c0 case 1) is
//   f = 0.25 (DAT_0057c974) * dot(light->vec, texel_normal)
// with no attenuation, so intensity is pre-multiplied by 4 to compensate.

static constexpr float sun_deg_to_rad = 3.14159265358979f / 180.0f;

// Distance of the virtual projection origin used to turn the directional light
// into something FUN_004ae360 (which projects occluders from a point) can bake.
static constexpr float sun_origin_distance = 5000.0f;

// Soft shadow sampling: on-axis origin plus a 3 point ring at 90/210/330 degrees
// on the disc of radius tan(spread) * sun_origin_distance perpendicular to the sun.
// Fixed offsets, no RNG - bakes must be reproducible.
static constexpr int sun_spread_samples = 4;

static void sun_light_create()
{
    auto* level = CDedLevel::Get();
    if (!level) {
        return;
    }
    auto& props = level->GetAlpineLevelProperties();
    if (!props.enable_sun) {
        return;
    }

    Vector3 dir = props.sun_to_light_dir();
    constexpr float inv255 = 1.0f / 255.0f;
    int handle = light_create_directional(
        &dir, props.sun_intensity * 4.0f, props.sun_color_r * inv255, props.sun_color_g * inv255,
        props.sun_color_b * inv255, 0, props.sun_cast_baked_shadows ? 1 : 0, 0);
    if (handle < 0 || handle >= max_scene_lights) {
        xlog::error("Sunlight: failed to allocate a scene light for the lightmap bake");
        return;
    }

    g_sun_light_handle = handle;
    g_sun_light_ptr = light_pool + handle * light_entry_size;
    g_sun_spread_angle = props.sun_spread_angle;
}

static void sun_light_destroy()
{
    if (g_sun_light_handle < 0) {
        return;
    }
    light_free(g_sun_light_handle, 0);
    g_sun_light_handle = -1;
    g_sun_light_ptr = nullptr;
    g_sun_spread_angle = 0.0f;
}

// Brackets one Calculate Lighting command: the scene light, the caches and the reporting all
// belong to it and none of them may survive it, including when the bake below unwinds.
class BakeScope
{
public:
    BakeScope()
    {
        sun_light_create();
        lightmap_release_occluders();
        g_no_shadow_cast_dropped.clear();
        g_occluder_tree_built = false;
        g_bake_active = true;
    }
    ~BakeScope()
    {
        g_bake_active = false;
        // a throw here during an unwind would end the process, and the caches below still have to go
        try {
            no_shadow_cast_report();
            if (g_occluder_tree_built) {
                lightmap_mesh_occluder_report();
            }
        }
        catch (...) {
        }
        try {
            lightmap_release_occluders();
            sun_light_destroy();
        }
        catch (...) {
        }
    }
    BakeScope(const BakeScope&) = delete;
    BakeScope& operator=(const BakeScope&) = delete;
};

static void __fastcall lighting_calc_shadows_new(void* self);
static FunHook<void __fastcall(void*)> lighting_calc_shadows_hook{0x00448f20, lighting_calc_shadows_new};

static void __fastcall lighting_calc_shadows_new(void* self)
{
    BakeScope bake;
    lighting_calc_shadows_hook.call_target(self);
}

static void __fastcall lighting_calc_no_shadows_new(void* self);
static FunHook<void __fastcall(void*)> lighting_calc_no_shadows_hook{0x004492d0, lighting_calc_no_shadows_new};

static void __fastcall lighting_calc_no_shadows_new(void* self)
{
    BakeScope bake;
    lighting_calc_no_shadows_hook.call_target(self);
}

static void lightmap_blend_reset();

// Build Geometry frees the GSolid the caches are keyed by, and a level load frees the whole level;
// the bake bracket already means nothing survives to see either, but the ambient snapshot is only
// rebuilt by a batch pass and would otherwise describe the previous level.
void lightmap_reset_level_state()
{
    s_ambient_room_count = 0;
    g_no_shadow_cast_dropped.clear();
    g_occluder_tree_built = false;
    lightmap_release_occluders();
    lightmap_blend_reset();
}

// Stock face light gathering adds type 1 lights unconditionally, once per room, so a
// surface reached through more than one room list would accumulate the sun several times.
// Runs in place of "INC EAX; MOV [0x007432ec],EAX" that commits the list entry.
CodeInjection sun_face_light_dedup_injection{
    0x004889a2,
    [](auto& regs) {
        int index = regs.eax;
        bool duplicate = false;
        if (g_sun_light_ptr && index > 0 && index < max_scene_lights &&
            face_light_list[index] == g_sun_light_ptr) {
            for (int i = 0; i < index; i++) {
                if (face_light_list[i] == g_sun_light_ptr) {
                    duplicate = true;
                    break;
                }
            }
        }
        // 0x00488810 stores into face_light_list without bounding the index; the light pool it
        // walks cannot exceed max_scene_lights entries, so the clamp is only a backstop
        int count = duplicate ? index : index + 1;
        count = std::clamp(count, 0, max_scene_lights);
        *reinterpret_cast<int*>(0x007432ec) = count;
        regs.eax = count;
        regs.eip = 0x004889a8;
    },
    false, // no trampoline: the injection fully replaces the 6 byte block
};

// RF levels are sealed shells whose openings are faces flagged show sky (0x1). The stock
// occluder filter inside FUN_004ae360 only rejects liquid/see-thru (0x44), invisible (0x2000)
// and portals (+0x34), so the virtual sun origin outside the shell projects every sky face as
// an occluder and shadows practically the whole level. Sky faces are where the sun enters, so
// they are skipped while our sun light's mask is built; other lights keep stock behaviour.
// Replaces "MOV EAX,[ESI+0x28]; TEST AL,0x44" (exactly 5 bytes) - ESI is the candidate face and
// [EBP+0x10] is the Light* parameter, both live for the whole occluder loop.
CodeInjection sun_sky_occluder_skip_injection{
    0x004aed36,
    [](auto& regs) {
        const uintptr_t face = regs.esi;
        const uint32_t flags = *reinterpret_cast<uint32_t*>(face + 0x28);
        regs.eax = static_cast<uintptr_t>(flags);
        const uintptr_t light = *reinterpret_cast<uintptr_t*>(static_cast<uintptr_t>(regs.ebp) + 0x10);
        const bool is_sun = g_sun_light_ptr && reinterpret_cast<void*>(light) == g_sun_light_ptr;
        if ((flags & 0x2044) != 0 || (is_sun && (flags & 0x1) != 0)) {
            regs.eip = 0x004af2e5; // continue with the next face
        }
        else {
            regs.eip = 0x004aed4a;
        }
    },
    false, // no trampoline: the injection fully replaces the 5 byte block
};

static Vector3 sun_cross(const Vector3& a, const Vector3& b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

static bool sun_normalize(Vector3& v)
{
    float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (!(len >= 1e-6f)) {
        return false;
    }
    v.x /= len;
    v.y /= len;
    v.z /= len;
    return true;
}

static void __cdecl sun_shadow_mask_new(uintptr_t solid, uintptr_t surface, uintptr_t light,
                                        char debug, uint8_t* mask);
static FunHook<void __cdecl(uintptr_t, uintptr_t, uintptr_t, char, uint8_t*)> sun_shadow_mask_hook{
    0x004ae360, sun_shadow_mask_new};

// The stock projector is replaced by the per texel trace above for every light of a fixed pipeline
// bake and for the sun in a legacy one. What is left below is the sun's masquerade, now reachable
// only when the tracer cannot run at all (unusable fragment dimensions or no occluders indexed):
// FUN_004ae360 projects occluders from light->vec treated as a world position, and rejects
// faces outside the light bbox (vec +/- rad_2). For the sun, vec is a direction and rad_2 is 0,
// so both are temporarily replaced with a virtual origin far along the to-sun vector and an
// effectively infinite radius. That drives the stock projector well outside its design envelope -
// at 5000 units its float32 frustum planes drop whole occluders, and two bakes of one level do not
// agree - so the warning below is the signal that a bake fell back onto it.
static void __cdecl sun_shadow_mask_new(uintptr_t solid, uintptr_t surface, uintptr_t light,
                                        char debug, uint8_t* mask)
{
    const bool is_sun = g_sun_light_ptr && reinterpret_cast<void*>(light) == g_sun_light_ptr;
    // outside a bake this is RED's viewport relight of a single surface, which gets the stock
    // projector: the tracer's caches are only meaningful for as long as the command that built them
    if (g_bake_active && (bake_fixes_active() || is_sun) &&
        lightmap_raycast_mask(solid, surface, light, mask)) {
        return;
    }
    if (!is_sun) {
        sun_shadow_mask_hook.call_target(solid, surface, light, debug, mask);
        return;
    }
    static bool warned = false;
    if (!warned) {
        warned = true;
        xlog::warn("Lightmap: the sun's ray traced shadow mask could not be built for at least one "
                   "surface, falling back to the stock projector for it");
    }

    // while a mover transform is pushed (DAT_0158f414) the engine reads the solid-local copy
    const int vec_off = *reinterpret_cast<int*>(0x0158f414) != 0 ? 0x5c : 0x0c;
    auto* vec = reinterpret_cast<float*>(light + vec_off);
    Vector3 to_sun{vec[0], vec[1], vec[2]};
    if (!sun_normalize(to_sun)) {
        sun_shadow_mask_hook.call_target(solid, surface, light, debug, mask);
        return;
    }

    const auto* bbox_min = reinterpret_cast<const float*>(surface + 0x34);
    const auto* bbox_max = reinterpret_cast<const float*>(surface + 0x40);
    const Vector3 center{(bbox_min[0] + bbox_max[0]) * 0.5f, (bbox_min[1] + bbox_max[1]) * 0.5f,
                         (bbox_min[2] + bbox_max[2]) * 0.5f};
    const Vector3 axis{center.x + to_sun.x * sun_origin_distance,
                       center.y + to_sun.y * sun_origin_distance,
                       center.z + to_sun.z * sun_origin_distance};

    const float saved_vec[3] = {vec[0], vec[1], vec[2]};
    auto& rad_2 = *reinterpret_cast<float*>(light + 0x3c);
    const float saved_rad_2 = rad_2;
    rad_2 = 1.0e9f;

    const int width = *reinterpret_cast<int*>(surface + 0x18);
    const int height = *reinterpret_cast<int*>(surface + 0x1c);
    const int texels = width * height;

    Vector3 offsets[sun_spread_samples] = {};
    int sample_count = 1;
    if (g_sun_spread_angle > 0.0f && texels > 0 && texels <= lm_max_fragment_texels) {
        Vector3 up = std::abs(to_sun.y) < 0.9f ? Vector3{0.0f, 1.0f, 0.0f} : Vector3{1.0f, 0.0f, 0.0f};
        Vector3 u = sun_cross(up, to_sun);
        if (sun_normalize(u)) {
            Vector3 v = sun_cross(to_sun, u);
            float radius = std::tan(g_sun_spread_angle * sun_deg_to_rad) * sun_origin_distance;
            for (int k = 1; k < sun_spread_samples; k++) {
                float angle = (90.0f + 120.0f * static_cast<float>(k - 1)) * sun_deg_to_rad;
                float cs = std::cos(angle) * radius;
                float sn = std::sin(angle) * radius;
                offsets[k] = {u.x * cs + v.x * sn, u.y * cs + v.y * sn, u.z * cs + v.z * sn};
            }
            sample_count = sun_spread_samples;
        }
    }

    if (sample_count == 1) {
        vec[0] = axis.x;
        vec[1] = axis.y;
        vec[2] = axis.z;
        sun_shadow_mask_hook.call_target(solid, surface, light, debug, mask);
    }
    else {
        // Each sample gets its own full-strength mask (the stock rasteriser subtracts a fixed
        // weight per shadow polygon and wraps around, so partial weights cannot be stacked);
        // the samples are averaged into the caller's mask instead.
        // one spare byte: FUN_004abed0 can store one past w*h
        static uint8_t sample_mask[lm_max_fragment_texels + 1];
        static uint16_t accum[lm_max_fragment_texels];
        std::memset(accum, 0, texels * sizeof(uint16_t));
        for (int k = 0; k < sample_count; k++) {
            vec[0] = axis.x + offsets[k].x;
            vec[1] = axis.y + offsets[k].y;
            vec[2] = axis.z + offsets[k].z;
            std::memset(sample_mask, 0xff, texels);
            sun_shadow_mask_hook.call_target(solid, surface, light, debug, sample_mask);
            for (int i = 0; i < texels; i++) {
                accum[i] = static_cast<uint16_t>(accum[i] + sample_mask[i]);
            }
        }
        for (int i = 0; i < texels; i++) {
            mask[i] = static_cast<uint8_t>(accum[i] / sample_count);
        }
    }

    vec[0] = saved_vec[0];
    vec[1] = saved_vec[1];
    vec[2] = saved_vec[2];
    rad_2 = saved_rad_2;
}

// ============================================================
// Lightmap bake accuracy fixes
// ============================================================

// Per-texel float accumulation buffers shared by the whole lightmap pipeline.
static constexpr int lm_accum_texels = 65536;
static auto* const lm_accum_r = reinterpret_cast<float*>(0x0138a620);
static auto* const lm_accum_g = reinterpret_cast<float*>(0x0140ac20);
static auto* const lm_accum_b = reinterpret_cast<float*>(0x0134a620);

// Stock converts the accumulated floats with __ftol, which truncates and so loses up to a
// full LSB on every texel. Everything after the conversion (negative clamp, hue preserving
// rescale when the brightest channel exceeds 255) is reproduced exactly.
static void lm_encode_bytes(double r, double g, double b, std::uint8_t* out)
{
    // keep the float->int conversion inside the range where the stock integer rescale below
    // cannot overflow; stock overflows into garbage past this point anyway
    constexpr double convert_limit = 8000000.0;
    r = std::clamp(r, -convert_limit, convert_limit);
    g = std::clamp(g, -convert_limit, convert_limit);
    b = std::clamp(b, -convert_limit, convert_limit);

    int ir = static_cast<int>(r + 0.5);
    int ig = static_cast<int>(g + 0.5);
    int ib = static_cast<int>(b + 0.5);
    if (ir < 0) ir = 0;
    if (ig < 0) ig = 0;
    if (ib < 0) ib = 0;

    int max_channel = ir;
    if (ig > max_channel) max_channel = ig;
    if (ib > max_channel) max_channel = ib;
    if (max_channel > 255) {
        ir = ir * 255 / max_channel;
        ig = ig * 255 / max_channel;
        ib = ib * 255 / max_channel;
    }

    out[0] = static_cast<std::uint8_t>(ir);
    out[1] = static_cast<std::uint8_t>(ig);
    out[2] = static_cast<std::uint8_t>(ib);
}

// Stock (FUN_004ac470 at 0x004ac8a8-0x004aca3e) only hands the filtered conversion FUN_004aced0
// to texels at least two in from every edge of a fragment that is at least 9x9, and the plain
// FUN_004ace10 to everything else, which leaves a processing seam two texels in from every
// fragment edge and no filtering at all on small fragments. The whole conversion block is taken
// over here so that every texel gets the same 3x3 box, renormalised where the kernel is clipped.
static void lm_encode_filtered(int col, int row, int width, int height, std::uint8_t* out)
{
    const int x_min = std::max(col - 1, 0);
    const int x_max = std::min(col + 1, width - 1);
    const int y_min = std::max(row - 1, 0);
    const int y_max = std::min(row + 1, height - 1);

    double sum_r = 0.0, sum_g = 0.0, sum_b = 0.0;
    for (int y = y_min; y <= y_max; y++) {
        const int base = y * width;
        for (int x = x_min; x <= x_max; x++) {
            sum_r += lm_accum_r[base + x];
            sum_g += lm_accum_g[base + x];
            sum_b += lm_accum_b[base + x];
        }
    }
    const double scale = static_cast<double>(lm_read_const(0x0055c7fc)) /
                         ((x_max - x_min + 1) * (y_max - y_min + 1));
    lm_encode_bytes(sum_r * scale, sum_g * scale, sum_b * scale, out);
}

CodeInjection lightmap_texel_convert_injection{
    0x004ac8a8,
    [](auto& regs) {
        const uintptr_t surface = regs.esi;
        const uintptr_t lm = regs.ecx;
        if (!bake_fixes_active()) {
            regs.eax = *reinterpret_cast<int*>(lm + 4) * *reinterpret_cast<int*>(surface + 0x14);
            regs.eip = 0x004ac8af;
            return;
        }

        regs.eip = 0x004aca40;
        const int width = *reinterpret_cast<int*>(surface + 0x18);
        const int height = *reinterpret_cast<int*>(surface + 0x1c);
        auto* buf = reinterpret_cast<std::uint8_t*>(*reinterpret_cast<uintptr_t*>(lm + 0xc));
        // the accumulators this reads are 65536 floats, the same bound every other consumer of a
        // fragment's dimensions checks
        if (width <= 0 || height <= 0 || width > lm_highres_page_size ||
            height > lm_highres_page_size || width * height > lm_accum_texels || !buf) {
            return;
        }
        const int stride = *reinterpret_cast<int*>(lm + 4);
        const int xstart = *reinterpret_cast<int*>(surface + 0x10);
        const int ystart = *reinterpret_cast<int*>(surface + 0x14);
        const bool filter = width >= 3 && height >= 3;
        const double scale = lm_read_const(0x0055c7fc); // 255.0

        for (int row = 0; row < height; row++) {
            std::uint8_t* out = buf + ((ystart + row) * stride + xstart) * 3;
            for (int col = 0; col < width; col++, out += 3) {
                if (filter) {
                    lm_encode_filtered(col, row, width, height, out);
                }
                else {
                    const int index = row * width + col;
                    lm_encode_bytes(static_cast<double>(lm_accum_r[index]) * scale,
                                    static_cast<double>(lm_accum_g[index]) * scale,
                                    static_cast<double>(lm_accum_b[index]) * scale, out);
                }
            }
        }
    },
    false, // no trampoline: the injection fully replaces the 7 byte block
};

// The smooth lumel path in FUN_004ad160 gives up on any texel whose lumel found no face edge
// crossings - typically the padding rows/columns around a fragment - and stores a flat grey
// (0.1 at 0x004adb79, 0.33 after 10 failed subdivisions at 0x004ad777). Those greys ignore the
// room ambient already seeded in the buffers and bleed into neighbours through the box filter,
// which is what shows up as dark fragment edges and speckles. Both sites instead run the flat
// lumel calculation for the same texel, exactly as FUN_004ad160 does for unsmoothed surfaces.
static void lightmap_smooth_grey_fallback(BaseCodeInjection::Regs& regs, std::uint32_t legacy_bits)
{
    const uintptr_t esp = static_cast<uintptr_t>(regs.esp);
    auto* p_r = *reinterpret_cast<float**>(esp + 0x38);
    auto* p_g = *reinterpret_cast<float**>(esp + 0x28);
    auto* p_b = *reinterpret_cast<float**>(esp + 0x1c);
    const uintptr_t surface = *reinterpret_cast<uintptr_t*>(esp + 0x298);
    void* masks = *reinterpret_cast<void**>(esp + 0x2a0);
    const int texel_index = *reinterpret_cast<int*>(esp + 0x44);
    const int col = *reinterpret_cast<int*>(esp + 0x74);
    const int row = *reinterpret_cast<int*>(esp + 0x34);

    SurfaceUVParams p;
    if (!bake_fixes_active() || !init_surface_uv_params(surface, p)) {
        *reinterpret_cast<std::uint32_t*>(p_r) = legacy_bits;
        *reinterpret_cast<std::uint32_t*>(p_g) = legacy_bits;
        *reinterpret_cast<std::uint32_t*>(p_b) = legacy_bits;
    }
    else {
        Vector3 pos{};
        texel_to_world(p, col, row, pos.x, pos.y, pos.z);
        light_accum_at_texel(p_r, p_g, p_b, &pos, reinterpret_cast<const Vector3*>(surface + 0x6c),
                             masks, texel_index, reinterpret_cast<const void*>(surface + 9));
        if (*p_r < 0.0f) *p_r = 0.0f;
        if (*p_g < 0.0f) *p_g = 0.0f;
        if (*p_b < 0.0f) *p_b = 0.0f;
    }

    // the shared tail at 0x004adb9d advances ESI/EDI/EBX as the R/G/B cursors
    regs.esi = p_r;
    regs.edi = p_g;
    regs.ebx = p_b;
    regs.eip = 0x004adb9d;
}

CodeInjection lightmap_smooth_grey_01_injection{
    0x004adb79,
    [](auto& regs) { lightmap_smooth_grey_fallback(regs, 0x3dcccccd); },
    false, // no trampoline: the injection fully replaces the 12 byte block
};

CodeInjection lightmap_smooth_grey_033_injection{
    0x004ad777,
    [](auto& regs) { lightmap_smooth_grey_fallback(regs, 0x3ea8f5c3); },
    false, // no trampoline: the injection fully replaces the 12 byte block
};

// FUN_004aded0 builds each smoothing group vertex normal as the unweighted mean of the plane
// normals of every face sharing that vertex, filtered by a hard dot > 0 test, so a face meeting
// the current one near 90 degrees either lands in the average at full weight or drops out of it
// entirely. Weighting each contribution by max(dot, 0) keeps the same cutoff but drives
// near-perpendicular neighbours smoothly to zero. Replaces "PUSH EDI; LEA ECX,[ESP+0x24]"
// (exactly 5 bytes) ahead of the call to Vector3::operator+=; EBX is the current face, EDI the
// neighbour, and the accumulator lives at ESP+0x20.
CodeInjection lightmap_smoothing_normal_weight_injection{
    0x004adfb1,
    [](auto& regs) {
        auto* accum = reinterpret_cast<float*>(static_cast<uintptr_t>(regs.esp) + 0x20);
        const auto* other_normal = reinterpret_cast<const float*>(static_cast<uintptr_t>(regs.edi));
        float weight = 1.0f;
        if (bake_fixes_active()) {
            const auto* face_normal =
                reinterpret_cast<const float*>(static_cast<uintptr_t>(regs.ebx));
            weight = face_normal[0] * other_normal[0] + face_normal[1] * other_normal[1] +
                     face_normal[2] * other_normal[2];
            if (weight < 0.0f) {
                weight = 0.0f;
            }
        }
        accum[0] += other_normal[0] * weight;
        accum[1] += other_normal[1] * weight;
        accum[2] += other_normal[2] * weight;
        regs.eip = 0x004adfbb;
    },
    false, // no trampoline: the injection fully replaces the 5 byte block
};

// Texels whose lumel never lands on one of the surface's own faces - the one texel padding ring
// and any part of the fragment rectangle the faces do not cover - carry no meaningful lighting of
// their own. Shading them at their true world position puts them past a terminator or on the wrong
// side of a curvature, and in game bilinear filtering pulls those values into the visible face edge
// as a dark rim. This builds the in-face texel map of the fragment (from the lightmap UVs of the
// faces bound to the surface, exactly the polygons FUN_004ae360 clips against) plus, for every
// out-of-face texel, the nearest in-face texel.
struct SurfaceFaceMap {
    int w = 0, h = 0;
    std::vector<std::uint8_t> inface;
    std::vector<int> nearest;
};

static bool lightmap_build_face_map(uintptr_t solid, uintptr_t surface, SurfaceFaceMap& m)
{
    const int w = *reinterpret_cast<int*>(surface + 0x18);
    const int h = *reinterpret_cast<int*>(surface + 0x1c);
    const uintptr_t lm = *reinterpret_cast<uintptr_t*>(surface + 0xc);
    if (!solid || !lm || w <= 0 || h <= 0 || w > lm_highres_page_size ||
        h > lm_highres_page_size) {
        return false;
    }
    const float lm_w = static_cast<float>(*reinterpret_cast<int*>(lm + 4));
    const float lm_h = static_cast<float>(*reinterpret_cast<int*>(lm + 8));
    const float xstart = static_cast<float>(*reinterpret_cast<int*>(surface + 0x10));
    const float ystart = static_cast<float>(*reinterpret_cast<int*>(surface + 0x14));
    const int surf_id = *reinterpret_cast<int*>(surface);

    m.w = w;
    m.h = h;
    m.inface.assign(static_cast<std::size_t>(w) * h, 0);

    SolidCache* cache = lightmap_solid_cache(solid);
    if (!cache) {
        return false;
    }
    auto faces = cache->faces_by_surface.find(surf_id);
    if (faces == cache->faces_by_surface.end()) {
        return false;
    }

    constexpr int max_face_verts = 128;
    float px[max_face_verts], py[max_face_verts];
    for (uintptr_t face : faces->second) {
        int n = 0;
        const uintptr_t head = *reinterpret_cast<uintptr_t*>(face + 0x40);
        for (uintptr_t node = head; node && n < max_face_verts;) {
            px[n] = lm_w * *reinterpret_cast<const float*>(node + 0x0c) - xstart;
            py[n] = lm_h * *reinterpret_cast<const float*>(node + 0x10) - ystart;
            n++;
            node = *reinterpret_cast<uintptr_t*>(node + 0x14);
            if (node == head) {
                break;
            }
        }
        if (n < 3) {
            continue;
        }
        float x_lo = px[0], x_hi = px[0], y_lo = py[0], y_hi = py[0];
        for (int i = 0; i < n; i++) {
            x_lo = std::min(x_lo, px[i]);
            x_hi = std::max(x_hi, px[i]);
            y_lo = std::min(y_lo, py[i]);
            y_hi = std::max(y_hi, py[i]);
        }
        const int c0 = std::max(0, static_cast<int>(std::floor(x_lo - 0.5f)));
        const int c1 = std::min(w - 1, static_cast<int>(std::ceil(x_hi)));
        const int r0 = std::max(0, static_cast<int>(std::floor(y_lo - 0.5f)));
        const int r1 = std::min(h - 1, static_cast<int>(std::ceil(y_hi)));
        for (int row = r0; row <= r1; row++) {
            const float ty = static_cast<float>(row) + 0.5f;
            for (int col = c0; col <= c1; col++) {
                if (m.inface[static_cast<std::size_t>(row) * w + col]) {
                    continue;
                }
                const float tx = static_cast<float>(col) + 0.5f;
                // even-odd crossings, not one half plane per edge: a face left non-convex by CSG
                // has a reflex wedge the half plane test drops out of the fragment entirely
                bool inside = false;
                for (int i = 0; i < n; i++) {
                    const int j = (i + 1) % n;
                    if ((py[i] > ty) == (py[j] > ty)) {
                        continue;
                    }
                    const float x_at = px[i] + (ty - py[i]) * (px[j] - px[i]) / (py[j] - py[i]);
                    if (tx < x_at) {
                        inside = !inside;
                    }
                }
                if (inside) {
                    m.inface[static_cast<std::size_t>(row) * w + col] = 1;
                }
            }
        }
    }

    const std::size_t total = static_cast<std::size_t>(w) * h;
    m.nearest.assign(total, -1);
    std::vector<int> queue;
    queue.reserve(total);
    for (std::size_t i = 0; i < total; i++) {
        if (m.inface[i]) {
            m.nearest[i] = static_cast<int>(i);
            queue.push_back(static_cast<int>(i));
        }
    }
    if (queue.empty() || queue.size() == total) {
        return false;
    }
    for (std::size_t head = 0; head < queue.size(); head++) {
        const int i = queue[head];
        const int cx = i % w;
        const int cy = i / w;
        for (int dy = -1; dy <= 1; dy++) {
            const int ny = cy + dy;
            if (ny < 0 || ny >= h) {
                continue;
            }
            for (int dx = -1; dx <= 1; dx++) {
                const int nx = cx + dx;
                if (nx < 0 || nx >= w || (dx == 0 && dy == 0)) {
                    continue;
                }
                const int j = ny * w + nx;
                if (m.nearest[j] < 0) {
                    m.nearest[j] = m.nearest[i];
                    queue.push_back(j);
                }
            }
        }
    }
    return true;
}

// FUN_004ad160 shades every texel of a smoothed fragment, including the one texel padding ring,
// and then throws the ring away again at 0x004adc2d-0x004add1a by copying the neighbouring row and
// column over it in the float buffers. That only ever reaches ring 0 and only from one direction,
// so it is replaced by a gutter fill: every out-of-face texel takes the finished value of the
// nearest in-face texel, which makes a fragment's outermost stored values continuations of real
// in-face lighting and leaves bilinear filtering nothing dark to pull in. The byte level ring copy
// in FUN_004aabf0 (FUN_004abad0) is deliberately left alone: it runs after FUN_004ab0d0, which
// writes the cross-surface blend strictly inside [1,w-2]x[1,h-2], so it is the only thing that
// carries the blend into the padding. Replaces "MOV EAX,[ESI+0x18]; XOR ECX,ECX" (exactly 5 bytes);
// 0x004ad2b7 is the shared tail that releases the face list and returns, ESP is balanced here and
// [ESP+0x294] is the GSolid parameter.
CodeInjection lightmap_border_duplicate_skip_injection{
    0x004adc37,
    [](auto& regs) {
        regs.eax = *reinterpret_cast<int*>(static_cast<uintptr_t>(regs.esi) + 0x18);
        regs.ecx = 0;
        // the gutter fill needs the solid's face index, which only a bake builds; viewport relight
        // keeps the stock ring copy rather than leaving the ring unwritten
        if (!bake_fixes_active() || !g_bake_active) {
            regs.eip = 0x004adc3c;
            return;
        }
        regs.eip = 0x004ad2b7;

        const uintptr_t surface = regs.esi;
        const uintptr_t solid = *reinterpret_cast<uintptr_t*>(static_cast<uintptr_t>(regs.esp) + 0x294);
        try {
            SurfaceFaceMap m;
            if (!lightmap_build_face_map(solid, surface, m)) {
                return;
            }
            const std::size_t total = m.inface.size();
            for (std::size_t i = 0; i < total; i++) {
                if (m.inface[i]) {
                    continue;
                }
                const int j = m.nearest[i];
                if (j < 0) {
                    continue;
                }
                lm_accum_r[i] = lm_accum_r[j];
                lm_accum_g[i] = lm_accum_g[j];
                lm_accum_b[i] = lm_accum_b[j];
            }
        }
        catch (...) {
        }
    },
    false, // no trampoline: the injection fully replaces the 5 byte block
};

// The occluder filter inside FUN_004ae360 drops every face whose texture (face+0x30 bitmap handle)
// has an alpha channel: FUN_004bcc60 reports pixel format 4, 5 or 7 from the bitmap record. RED
// only sets the face's own has-alpha flag 0x40 on detail brushes (FlagFaceTextureTraits 0x0041d3c0
// gates it on the detail bit), so this second, ungated test is what silently stops a structural
// wall textured with anything alpha-capable from casting a shadow. The test is skipped only when
// the level opts in; every other filter, including the +0x36 owner test just above, is untouched.
// Replaces "MOV EAX,[ESI+0x30]; CMP EAX,-1" (exactly 6 bytes) and composes with
// sun_sky_occluder_skip_injection, which sits earlier in the same filter chain.
CodeInjection lightmap_alpha_texture_occluder_injection{
    0x004aed59,
    [](auto& regs) {
        const int bitmap = *reinterpret_cast<int*>(static_cast<uintptr_t>(regs.esi) + 0x30);
        regs.eax = bitmap;
        // 0x004aed72 is the JZ target, 0x004aed61 the PUSH EAX feeding FUN_004bcc60; resuming on
        // the JZ itself would read flags the skipped CMP never set
        regs.eip = (alpha_faces_occlude_active() || bitmap == -1) ? 0x004aed72 : 0x004aed61;
    },
    false, // no trampoline: the injection fully replaces the 6 byte block
};

// ============================================================
// High resolution lightmaps
// ============================================================
// FUN_004a9d30 sizes a fragment as round(extent * pixels_per_meter), clamps each axis to 64 and
// then asks FUN_004a5f60 for room in a 128x128 page. All three numbers are widened together: the
// incoming pixels_per_meter argument x4, the clamp to 254 and the page dimension global to 256.
// Everything downstream of that already reads the page's own w/h and the surface's own w/h, and
// both the RFL container (i32 page w/h, u8 surface rect) and both loaders (RED FUN_004a2c30 /
// FUN_004a3c80, RF FUN_004ed1c0 / FUN_004ee210) take the dimensions from the file.
// Fragment sizes are decided when geometry is built, so toggling this needs a Build Geometry.

// The clamp rescale reads its 64.0f through FDIVR at 0x004aa06f and 0x004aa08d; both operands are
// re-pointed here so the pooled 0x0055c888 constant stays untouched.
static float g_lm_fragment_max_f = static_cast<float>(lm_stock_fragment_max);

// Replaces "MOV EBP,[ESP+0x94]" (exactly 7 bytes) just after the SEH prologue, where ESP+0x9c is
// the pixels_per_meter argument. x4 is exact in binary floating point, so the only difference
// from stock is the exponent.
CodeInjection lightmap_highres_setup_injection{
    0x004a9d49,
    [](auto& regs) {
        const uintptr_t esp = static_cast<uintptr_t>(regs.esp);
        regs.ebp = *reinterpret_cast<std::uint32_t*>(esp + 0x94);
        if (highres_lightmaps_active()) {
            *reinterpret_cast<float*>(esp + 0x9c) *= 4.0f;
            *reinterpret_cast<int*>(0x0144ac24) = lm_highres_page_size;
        }
        else {
            *reinterpret_cast<int*>(0x0144ac24) = lm_stock_page_size;
        }
        regs.eip = 0x004a9d50;
    },
    false, // no trampoline: the injection fully replaces the 7 byte load
};

// Replaces "CMP EAX,ECX; MOV [ESP+0x30],EAX" (6 bytes) ahead of the width clamp. EAX is the
// fragment width, ECX carries the cap into both the width and the height clamp, and the two
// stock x87 rescale blocks are left to do the arithmetic so the off case stays byte-identical.
CodeInjection lightmap_fragment_clamp_injection{
    0x004aa060,
    [](auto& regs) {
        const int cap = highres_lightmaps_active() ? lm_highres_fragment_max : lm_stock_fragment_max;
        g_lm_fragment_max_f = static_cast<float>(cap);
        regs.ecx = cap;
        const int width = static_cast<int>(regs.eax);
        *reinterpret_cast<int*>(static_cast<uintptr_t>(regs.esp) + 0x30) = width;
        // 0x004aa068 is the rescale, 0x004aa07b the height clamp the stock JLE skips to
        regs.eip = (width <= cap) ? 0x004aa07b : 0x004aa068;
    },
    false, // no trampoline: the injection fully replaces the 6 byte block
};

// FUN_004ab0d0 alloca()s 4*(wA*hA) + 4*(wB*hB) bytes (0x004ab112/0x004ab122/0x004ab131/0x004ab140)
// on RED's 1 MB thread stack. Stock fragments cap that at 32 KB; 254x254 pairs need 504 KB, which
// fits in practice but is not something to take on trust, so the seam blend is skipped rather
// than overflowing the stack. Stock sizes never reach the limit, so the off case is unaffected.
static std::size_t lightmap_stack_headroom()
{
    MEMORY_BASIC_INFORMATION mbi{};
    volatile char probe = 0;
    auto sp = reinterpret_cast<uintptr_t>(const_cast<const char*>(&probe));
    if (VirtualQuery(reinterpret_cast<void*>(sp), &mbi, sizeof(mbi)) != sizeof(mbi)) {
        return 0;
    }
    return sp - reinterpret_cast<uintptr_t>(mbi.AllocationBase);
}

static void __cdecl lightmap_blend_surfaces_new(void** a, void** b, void* p3, void* p4, void* p5);
static FunHook<void __cdecl(void**, void**, void*, void*, void*)> lightmap_blend_surfaces_hook{
    0x004ab0d0, lightmap_blend_surfaces_new};

// FUN_004ab0d0 mixes a fixed 7/16 of the neighbouring fragment into the ring 1 texels along a
// shared edge and applies no test at all to the angle between the two surfaces, so where the
// junction is a real crease between a sunlit face and one turned away from the sun it stamps a
// one texel band of foreign light along the polygon boundary - a hard line drawn on geometry
// whose own lighting is flat. Measured on the four sun2 bakes that is 240 to 349 bands per level,
// median contrast 44 bytes and up to 121, and three quarters of them sit on junctions of 45
// degrees or more. Only junctions the geometry means to look continuous are blended; a crease
// keeps the lighting break it is entitled to.
static constexpr float lm_blend_min_cos = 0.70710678f; // 45 degrees

static bool lightmap_surfaces_are_smooth_neighbours(const void* surf_a, const void* surf_b)
{
    const auto* na = reinterpret_cast<const float*>(static_cast<const char*>(surf_a) + 0x6c);
    const auto* nb = reinterpret_cast<const float*>(static_cast<const char*>(surf_b) + 0x6c);
    return na[0] * nb[0] + na[1] * nb[1] + na[2] * nb[2] >= lm_blend_min_cos;
}

// FUN_004aae80 blends the first shared edge it finds for a surface pair and then records the pair
// in both surfaces' done lists (0x004ab04b / 0x004ab058), so a boundary built out of more than one
// segment - an L shaped junction, or one the CSG split in two - keeps a hard step on every segment
// but the first. On sun2_nowater that is the 5.9 unit ground boundary beside brush 10: the pair's
// short segment carries a 1 byte step and its long one a 127 byte step. The pair scan is skipped by
// the injection below and the dedup redone here per shared edge, so each segment blends once.
static std::map<std::pair<int, int>, std::vector<Vector3>> g_blend_edges;

static bool lightmap_blend_edge_is_new(const int* surf_a, const int* surf_b, const float* p0,
                                       const float* p1)
{
    const int ia = surf_a[0];
    const int ib = surf_b[0];
    const Vector3 mid{(p0[0] + p1[0]) * 0.5f, (p0[1] + p1[1]) * 0.5f, (p0[2] + p1[2]) * 0.5f};
    auto& seen = g_blend_edges[{std::min(ia, ib), std::max(ia, ib)}];
    for (const Vector3& v : seen) {
        const float dx = v.x - mid.x;
        const float dy = v.y - mid.y;
        const float dz = v.z - mid.z;
        // FUN_004aae80 matches the shared vertices themselves to 0.001 (0x00554844)
        if (dx * dx + dy * dy + dz * dz < 4.0e-4f) {
            return false;
        }
    }
    seen.push_back(mid);
    return true;
}

// Stock addresses a texel as ftol(lm_dim * uv + 0.5) - start (0x004ab450 / 0x004ab480), which
// rounds at texel BOUNDARIES instead of flooring into the texel that contains the sample, so the
// run of texels it stamps sits half a texel off the border texels it means to reach. Measured on
// the sun2_nowater bake that misses 32% of the border texels along the gate-eligible edges and
// stamps interior texels in their place; on a diagonal boundary the surviving stamps are an
// intermittent dotted band of half-blended texels rather than a line. The walk is redone below:
// the same shared edge, floored addressing, every texel the edge crosses, and the neighbour's
// value averaged over the part of the edge inside each texel instead of whichever sample arrived
// first. The 9:7 mix and the ring 1 clamp are stock's (0x004ab5c2 / 0x004ab64e, 0x004ab4d7), and
// stock already writes both sides, so the reconciliation stays symmetric.
static constexpr float lm_blend_own = 9.0f / 16.0f;
static constexpr float lm_blend_other = 7.0f / 16.0f;
static constexpr float lm_blend_samples_per_texel = 128.0f;
// The walk is 128 samples per texel of the longer projection of the edge; the floor only binds for
// an edge a small fraction of a texel long, where it is what keeps a sub-texel crossing from
// resting on one or two samples.
static constexpr int lm_blend_min_samples = 8;

struct BlendSide {
    std::uint8_t* page = nullptr;
    int stride = 0;
    int xstart = 0, ystart = 0, w = 0, h = 0;
    float lm_w = 0.0f, lm_h = 0.0f;
    float scale_x = 0.0f, scale_y = 0.0f, add_x = 0.0f, add_y = 0.0f;
    int dropped = 0, u_coeff = 0;
    std::vector<std::uint8_t> snap;
};

// The blended value of each texel is mixed from the neighbour and from what this surface held
// before the edge, so the snapshot has to be the page as it stands. A surface shares a boundary
// with as many neighbours as the geometry gives it, and re-copying its whole fragment for each of
// them was the pass's dominant cost, so the snapshots are kept for the length of one pass instead.
// blend_side_apply writes every byte it changes into the snapshot as well, which keeps a cached
// snapshot equal to the page at all times - an entry can therefore be dropped and rebuilt at any
// point without changing a single output byte, which is what makes the budget below safe.
struct BlendAccum {
    std::vector<float> sum;
    std::vector<int> count;
    std::vector<int> touched;
};

static std::unordered_map<uintptr_t, BlendSide> g_blend_sides;
static std::size_t g_blend_sides_bytes = 0;
static BlendAccum g_blend_accum[2];
static constexpr std::size_t lm_blend_cache_budget = 64u << 20;

static bool blend_side_init(BlendSide& s, uintptr_t surface)
{
    const uintptr_t lm = *reinterpret_cast<uintptr_t*>(surface + 0xc);
    if (!lm) {
        return false;
    }
    s.page = *reinterpret_cast<std::uint8_t**>(lm + 0xc);
    s.stride = *reinterpret_cast<int*>(lm + 4);
    const int page_h = *reinterpret_cast<int*>(lm + 8);
    s.w = *reinterpret_cast<int*>(surface + 0x18);
    s.h = *reinterpret_cast<int*>(surface + 0x1c);
    s.scale_x = *reinterpret_cast<float*>(surface + 0x4c);
    s.scale_y = *reinterpret_cast<float*>(surface + 0x50);
    if (!s.page || s.stride <= 0 || page_h <= 0 || s.w <= 0 || s.h <= 0 || s.scale_x == 0.0f ||
        s.scale_y == 0.0f) {
        return false;
    }
    s.xstart = *reinterpret_cast<int*>(surface + 0x10);
    s.ystart = *reinterpret_cast<int*>(surface + 0x14);
    s.lm_w = static_cast<float>(s.stride);
    s.lm_h = static_cast<float>(page_h);
    s.add_x = *reinterpret_cast<float*>(surface + 0x54);
    s.add_y = *reinterpret_cast<float*>(surface + 0x58);
    s.dropped = *reinterpret_cast<int*>(surface + 0x5c);
    s.u_coeff = *reinterpret_cast<int*>(surface + 0x60);

    const std::size_t texels = static_cast<std::size_t>(s.w) * s.h;
    s.snap.resize(texels * 3);
    for (int row = 0; row < s.h; row++) {
        std::memcpy(&s.snap[static_cast<std::size_t>(row) * s.w * 3],
                    s.page + (static_cast<std::size_t>(s.ystart + row) * s.stride + s.xstart) * 3,
                    static_cast<std::size_t>(s.w) * 3);
    }
    return true;
}

static BlendSide* blend_side_get(uintptr_t surface)
{
    auto it = g_blend_sides.find(surface);
    if (it != g_blend_sides.end()) {
        return &it->second;
    }
    BlendSide side;
    if (!blend_side_init(side, surface)) {
        return nullptr;
    }
    BlendSide& entry = g_blend_sides.emplace(surface, std::move(side)).first->second;
    g_blend_sides_bytes += entry.snap.size();
    return &entry;
}

// Anything that writes a surface's texels outside blend_side_apply - the stock blend below - makes
// that surface's snapshot stale.
static void blend_side_drop(uintptr_t surface)
{
    auto it = g_blend_sides.find(surface);
    if (it != g_blend_sides.end()) {
        g_blend_sides_bytes -= std::min(g_blend_sides_bytes, it->second.snap.size());
        g_blend_sides.erase(it);
    }
}

static void blend_sides_clear()
{
    g_blend_sides.clear();
    g_blend_sides_bytes = 0;
}

static void blend_accum_prepare(BlendAccum& a, const BlendSide& s)
{
    const std::size_t texels = static_cast<std::size_t>(s.w) * s.h;
    if (a.count.size() < texels) {
        a.sum.assign(texels * 3, 0.0f);
        a.count.assign(texels, 0);
    }
}

// An edge that threw part way through leaves accumulated neighbour values behind; they belong to
// nothing and must not reach the next edge's average.
static void blend_accum_discard(BlendAccum& a)
{
    for (int index : a.touched) {
        if (index < 0 || static_cast<std::size_t>(index) >= a.count.size()) {
            continue;
        }
        a.count[index] = 0;
        float* acc = &a.sum[static_cast<std::size_t>(index) * 3];
        acc[0] = acc[1] = acc[2] = 0.0f;
    }
    a.touched.clear();
}

static void lightmap_blend_reset()
{
    g_blend_edges.clear();
    blend_sides_clear();
    blend_accum_discard(g_blend_accum[0]);
    blend_accum_discard(g_blend_accum[1]);
}

// The inverse of texel_to_world: page pixel coordinates of a world position, exact for any point
// on the surface's plane because only the two kept axes take part.
static void blend_world_to_page(const BlendSide& s, const float* p, float& x, float& y)
{
    float u, v;
    switch (s.dropped) {
    case 0:
        if (s.u_coeff == 1) { u = p[1]; v = p[2]; } else { u = p[2]; v = p[1]; }
        break;
    case 1:
        if (s.u_coeff == 0) { u = p[0]; v = p[2]; } else { u = p[2]; v = p[0]; }
        break;
    default:
        if (s.u_coeff == 0) { u = p[0]; v = p[1]; } else { u = p[1]; v = p[0]; }
        break;
    }
    x = (u * s.scale_x + s.add_x) * s.lm_w;
    y = (v * s.scale_y + s.add_y) * s.lm_h;
}

static int blend_side_texel(const BlendSide& s, float x, float y)
{
    int col = static_cast<int>(std::floor(x)) - s.xstart;
    int row = static_cast<int>(std::floor(y)) - s.ystart;
    col = std::min(std::max(col, 1), s.w - 2);
    row = std::min(std::max(row, 1), s.h - 2);
    if (col < 0 || row < 0) {
        return -1;
    }
    return row * s.w + col;
}

// The two grids meet at an arbitrary angle and density, so nearest sampling of the neighbour would
// quantise its value into steps along the edge and put those steps back into the blended line;
// bilinear on the neighbour's snapshot varies continuously along it. Sampling stays inside the
// neighbour's own fragment rect, whose out of face texels the gutter fill already made
// continuations of real in face lighting.
static void blend_side_sample(const BlendSide& s, float x, float y, float* out)
{
    const float fx = x - static_cast<float>(s.xstart) - 0.5f;
    const float fy = y - static_cast<float>(s.ystart) - 0.5f;
    const int x0 = static_cast<int>(std::floor(fx));
    const int y0 = static_cast<int>(std::floor(fy));
    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);
    out[0] = out[1] = out[2] = 0.0f;
    for (int dy = 0; dy < 2; dy++) {
        const int yy = std::min(std::max(y0 + dy, 0), s.h - 1);
        const float wy = dy ? ty : 1.0f - ty;
        for (int dx = 0; dx < 2; dx++) {
            const int xx = std::min(std::max(x0 + dx, 0), s.w - 1);
            const float wgt = wy * (dx ? tx : 1.0f - tx);
            const std::uint8_t* px = &s.snap[(static_cast<std::size_t>(yy) * s.w + xx) * 3];
            out[0] += px[0] * wgt;
            out[1] += px[1] * wgt;
            out[2] += px[2] * wgt;
        }
    }
}

static void blend_side_add(BlendAccum& a, int index, const float* other)
{
    if (a.count[index] == 0) {
        a.touched.push_back(index);
    }
    a.count[index]++;
    float* dst = &a.sum[static_cast<std::size_t>(index) * 3];
    dst[0] += other[0];
    dst[1] += other[1];
    dst[2] += other[2];
}

static void blend_side_apply(BlendSide& s, BlendAccum& a)
{
    for (int index : a.touched) {
        const float inv = 1.0f / static_cast<float>(a.count[index]);
        std::uint8_t* dst =
            s.page + (static_cast<std::size_t>(s.ystart + index / s.w) * s.stride + s.xstart +
                      index % s.w) * 3;
        float* acc = &a.sum[static_cast<std::size_t>(index) * 3];
        std::uint8_t* own = &s.snap[static_cast<std::size_t>(index) * 3];
        for (int c = 0; c < 3; c++) {
            const int v = static_cast<int>(static_cast<float>(own[c]) * lm_blend_own +
                                           acc[c] * inv * lm_blend_other + 0.5f);
            const auto out = static_cast<std::uint8_t>(std::min(std::max(v, 0), 255));
            dst[c] = out;
            own[c] = out;
            acc[c] = 0.0f;
        }
        a.count[index] = 0;
    }
    a.touched.clear();
}

static bool lightmap_blend_edge(uintptr_t surf_a, uintptr_t surf_b, const float* p0, const float* p1)
{
    if (surf_a == surf_b) {
        return true; // stock pre-filters this, and there is nothing for the stock blend to do either
    }
    // the blend pass is serial (FUN_004aabf0 -> FUN_004aae80 -> here on the bake thread), so the
    // scratch buffers are kept across calls rather than reallocated per edge
    if (g_blend_sides_bytes > lm_blend_cache_budget) {
        blend_sides_clear();
    }
    BlendSide* pa = blend_side_get(surf_a);
    BlendSide* pb = blend_side_get(surf_b);
    if (!pa || !pb) {
        return false;
    }
    BlendSide& a = *pa;
    BlendSide& b = *pb;
    BlendAccum& acc_a = g_blend_accum[0];
    BlendAccum& acc_b = g_blend_accum[1];
    blend_accum_prepare(acc_a, a);
    blend_accum_prepare(acc_b, b);
    float ax0, ay0, ax1, ay1, bx0, by0, bx1, by1;
    blend_world_to_page(a, p0, ax0, ay0);
    blend_world_to_page(a, p1, ax1, ay1);
    blend_world_to_page(b, p0, bx0, by0);
    blend_world_to_page(b, p1, bx1, by1);
    const float ext = std::max(std::max(std::abs(ax1 - ax0), std::abs(ay1 - ay0)),
                               std::max(std::abs(bx1 - bx0), std::abs(by1 - by0)));
    if (!(ext >= 0.0f) || ext > 1.0e6f) {
        return false;
    }
    int steps = static_cast<int>(ext * lm_blend_samples_per_texel) + 1;
    steps = std::min(std::max(steps, lm_blend_min_samples), 1 << 16);
    for (int k = 0; k <= steps; k++) {
        const float t = static_cast<float>(k) / static_cast<float>(steps);
        const float ax = ax0 + (ax1 - ax0) * t;
        const float ay = ay0 + (ay1 - ay0) * t;
        const float bx = bx0 + (bx1 - bx0) * t;
        const float by = by0 + (by1 - by0) * t;
        const int ia = blend_side_texel(a, ax, ay);
        const int ib = blend_side_texel(b, bx, by);
        if (ia < 0 || ib < 0) {
            continue;
        }
        float sa[3], sb[3];
        blend_side_sample(a, ax, ay, sa);
        blend_side_sample(b, bx, by, sb);
        blend_side_add(acc_a, ia, sb);
        blend_side_add(acc_b, ib, sa);
    }
    blend_side_apply(a, acc_a);
    blend_side_apply(b, acc_b);
    return true;
}

static void __cdecl lightmap_blend_surfaces_new(void** a, void** b, void* p3, void* p4, void* p5)
{
    const auto* surf_a = reinterpret_cast<const int*>(*a);
    const auto* surf_b = reinterpret_cast<const int*>(*b);
    if (bake_fixes_active()) {
        if (!lightmap_surfaces_are_smooth_neighbours(surf_a, surf_b)) {
            return;
        }
        // arguments 3 and 5 carry the receiving surface's two edge vertex nodes; FUN_004ab0d0 reads
        // the world position behind each of them (0x004ab249 / 0x004ab257) and nothing else
        try {
            const auto* p_0 = *reinterpret_cast<const float**>(p3);
            const auto* p_1 = *reinterpret_cast<const float**>(p5);
            if (!lightmap_blend_edge_is_new(surf_a, surf_b, p_0, p_1)) {
                return;
            }
            if (lightmap_blend_edge(reinterpret_cast<uintptr_t>(*a),
                                    reinterpret_cast<uintptr_t>(*b), p_0, p_1)) {
                return;
            }
        }
        catch (...) {
            blend_accum_discard(g_blend_accum[0]);
            blend_accum_discard(g_blend_accum[1]);
        }
    }
    // the stock blend writes both surfaces' texels behind the snapshots' back
    blend_side_drop(reinterpret_cast<uintptr_t>(*a));
    blend_side_drop(reinterpret_cast<uintptr_t>(*b));
    const std::size_t need =
        4u * static_cast<std::size_t>(surf_a[6]) * static_cast<std::size_t>(surf_a[7]) +
        4u * static_cast<std::size_t>(surf_b[6]) * static_cast<std::size_t>(surf_b[7]);
    if (need + 0x10000u > lightmap_stack_headroom()) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            xlog::warn("Lightmap: skipping the cross-surface blend for a {}x{} / {}x{} pair, it "
                       "needs {} bytes of stack",
                       surf_a[6], surf_a[7], surf_b[6], surf_b[7], need);
        }
        return;
    }
    lightmap_blend_surfaces_hook.call_target(a, b, p3, p4, p5);
}

// The entry list FUN_004aabf0 hands over is rebuilt per solid, so the per-edge dedup above must not
// outlive one pass over it.
static void __cdecl lightmap_blend_pass_new(void* entries, int count);
static FunHook<void __cdecl(void*, int)> lightmap_blend_pass_hook{0x004aae80, lightmap_blend_pass_new};

static void __cdecl lightmap_blend_pass_new(void* entries, int count)
{
    g_blend_edges.clear();
    blend_sides_clear();
    lightmap_blend_pass_hook.call_target(entries, count);
    g_blend_edges.clear();
    blend_sides_clear();
}

// Skips the per-pair done list scan at 0x004aaf29-0x004aaf56 so every shared edge of a pair reaches
// FUN_004ab0d0. Replaces "LEA EBP,[EBX+0xc]; XOR ESI,ESI" (exactly 5 bytes): EBX is the current
// entry + 4, [ESP+0x38] is where the stock pre-loop parks the done list for the two push sites at
// 0x004ab046, and 0x004aaf58 is the first instruction past the scan.
CodeInjection lightmap_blend_pair_dedup_injection{
    0x004aaf24,
    [](auto& regs) {
        const uintptr_t done_list = static_cast<uintptr_t>(regs.ebx) + 0xc;
        regs.ebp = done_list;
        *reinterpret_cast<std::uint32_t*>(static_cast<uintptr_t>(regs.esp) + 0x38) =
            static_cast<std::uint32_t>(done_list);
        regs.esi = 0;
        regs.eip = bake_fixes_active() ? 0x004aaf58 : 0x004aaf29;
    },
    false, // no trampoline: the injection fully replaces the 5 byte block
};

// FUN_004aae80 gathers the vertex loop of the face it is matching into [ESP+0x44], whose frame only
// reaches the return address at [ESP+0xc4] - 32 pointers - while the walk at 0x004aaeda has no bound
// at all. A 35 vertex face therefore overwrites the return address and both incoming arguments with
// vertex node pointers, after which the entry list pointer and its count are garbage and the pair
// loop walks off into unrelated memory (dmabruptdecayrc2.rfl, read of 0x68 at 0x004aaf0a). Stock
// only ever reached the pass with surfaces that carry smoothing groups, so the fixed pipeline's
// force_should_smooth is what makes it common, but the overflow is stock's and is not gated here.
// The loop is relocated to a buffer large enough for any face: the write at 0x004aaecb, the base at
// 0x004aaf9b and the indexed read at 0x004aafe1 are the only three sites that touch it.
static constexpr int lm_max_face_verts = 4096;
static uintptr_t g_face_vert_nodes[lm_max_face_verts];

// Replaces "MOV EAX,[EAX]; MOV ECX,[EAX+0x40]" (exactly 5 bytes) and the whole store loop behind it.
// A face with no vertex list takes stock's JZ, which leaves the count slot holding the previous
// face's value; that is reproduced by returning without writing it.
CodeInjection lightmap_blend_face_verts_injection{
    0x004aaecb,
    [](auto& regs) {
        regs.eip = 0x004aaeef;
        const uintptr_t face = *reinterpret_cast<uintptr_t*>(static_cast<uintptr_t>(regs.eax));
        const uintptr_t head = face ? *reinterpret_cast<uintptr_t*>(face + 0x40) : 0;
        regs.eax = head;
        regs.ecx = head;
        if (!head) {
            return;
        }
        int count = 0;
        uintptr_t node = head;
        while (count < lm_max_face_verts) {
            g_face_vert_nodes[count++] = node;
            node = *reinterpret_cast<uintptr_t*>(node + 0x14);
            if (!node || node == head) {
                break;
            }
        }
        if (node && node != head) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                xlog::warn("Lightmap: a face has more than {} vertices, the cross-surface blend will "
                           "miss some of its edges",
                           lm_max_face_verts);
            }
        }
        *reinterpret_cast<int*>(static_cast<uintptr_t>(regs.esp) + 0x14) = count;
        regs.esi = count;
    },
    false, // no trampoline: the injection fully replaces the gather
};

// Replaces "CMP [ESP+0x14],EBX; JLE 0x004aaffd" (the jmp lands inside the JLE) plus the
// "LEA EBP,[ESP+0x44]" behind it; 0x004aafa5 is the first instruction of the loop body.
CodeInjection lightmap_blend_face_verts_base_injection{
    0x004aaf9b,
    [](auto& regs) {
        const int count = *reinterpret_cast<int*>(static_cast<uintptr_t>(regs.esp) + 0x14);
        if (count <= static_cast<int>(regs.ebx)) {
            regs.eip = 0x004aaffd;
            return;
        }
        regs.ebp = reinterpret_cast<uintptr_t>(g_face_vert_nodes);
        regs.eip = 0x004aafa5;
    },
    false, // no trampoline: the injection fully replaces the 6 byte test and the base load
};

// Replaces "MOV EBX,[ESP+EBX*4+0x44]; MOV [ESP+0x20],ESI" (8 bytes); 0x004aafe9 is the TEST whose
// flags the following JNZ consumes.
CodeInjection lightmap_blend_face_vert_index_injection{
    0x004aafe1,
    [](auto& regs) {
        const int index = static_cast<int>(regs.ebx);
        regs.ebx = (index >= 0 && index < lm_max_face_verts) ? g_face_vert_nodes[index] : 0;
        *reinterpret_cast<std::uint32_t*>(static_cast<uintptr_t>(regs.esp) + 0x20) =
            static_cast<std::uint32_t>(regs.esi);
        regs.eip = 0x004aafe9;
    },
    false, // no trampoline: the injection fully replaces the 8 byte block
};

// ============================================================
// Cross-room surface merging (the former -smoothlights patches)
// ============================================================
// A portal brush splitting a face puts the fragments in different rooms, and the stock surface
// group flood fill (FUN_004aa610) treats the room pointer as a hard boundary, so the fragments get
// independent lightmaps and a visible seam. The six sites below used to be static byte patches
// installed only for -smoothlights; each is now an always-installed injection that reproduces the
// stock branch exactly whenever smooth_lights_active() is false.

// FUN_004aa610 candidate filter: stock rejects a coplanar neighbour whose face+0x44 room pointer
// differs from the seed face's. Replaces "MOV EAX,[ESP+0x30]; MOV ECX,[ESP+0x14]" (8 bytes).
CodeInjection lightmap_group_cross_room_injection{
    0x004aa7f9,
    [](auto& regs) {
        const uintptr_t esp = static_cast<uintptr_t>(regs.esp);
        const uintptr_t seed_base = *reinterpret_cast<uintptr_t*>(esp + 0x30);
        const uintptr_t seed_index = *reinterpret_cast<uintptr_t*>(esp + 0x14);
        const uintptr_t candidate_room =
            *reinterpret_cast<uintptr_t*>(static_cast<uintptr_t>(regs.edi) + 0x44);
        regs.eax = seed_base;
        regs.ecx = seed_index;
        regs.edx = candidate_room;
        const bool same_room = candidate_room == *reinterpret_cast<uintptr_t*>(seed_base + seed_index);
        regs.eip = (same_room || smooth_lights_active()) ? 0x004aa809 : 0x004aa835;
    },
    false, // no trampoline: the injection fully replaces the 8 byte block
};

// FUN_004a9d30 seeds should_smooth from "any face in the group has smoothing groups"; merged
// groups need it on unconditionally so neighbouring fragments blend. Replaces
// "MOV byte ptr [ESI+9],0; XOR EBX,EBX" (6 bytes, the jmp lands inside the second one).
CodeInjection lightmap_force_should_smooth_injection{
    0x004a9d9d,
    [](auto& regs) {
        *reinterpret_cast<std::uint8_t*>(static_cast<uintptr_t>(regs.esi) + 9) =
            smooth_lights_active() ? 1u : 0u;
        regs.ebx = 0;
        regs.eip = 0x004a9da3;
    },
    false, // no trampoline: the injection fully replaces the 6 byte block
};

// Merged surfaces span rooms, so the per-room light and face lists no longer describe them. These
// three sites each branch on surface->room_index == -1 to pick the global list instead; the branch
// is forced when merging is on. Every handler reproduces the skipped stock instructions exactly.

// FUN_004ac470 shadow-pass gather: "MOV ECX,[ESI+0x68]; XOR EAX,EAX" (5 bytes).
CodeInjection lightmap_global_lights_shadow_injection{
    0x004ac4a9,
    [](auto& regs) {
        regs.ecx = *reinterpret_cast<int*>(static_cast<uintptr_t>(regs.esi) + 0x68);
        regs.eax = 0;
        regs.eip = smooth_lights_active() ? 0x004ac4c1 : 0x004ac4ae;
    },
    false, // no trampoline: the injection fully replaces the 5 byte block
};

// FUN_004ac470 vertex-lighting gather: "MOV EAX,[ESI+0x68]; XOR EBP,EBP" (5 bytes).
CodeInjection lightmap_global_lights_vertex_injection{
    0x004aca53,
    [](auto& regs) {
        regs.eax = *reinterpret_cast<int*>(static_cast<uintptr_t>(regs.esi) + 0x68);
        regs.ebp = 0;
        regs.eip = smooth_lights_active() ? 0x004aca6f : 0x004aca58;
    },
    false, // no trampoline: the injection fully replaces the 5 byte block
};

// FUN_004ae360 occluder face list: "MOV dword ptr [ESP+0x8f8],0" (a single 11 byte store of the
// SEH state index; EAX is already loaded by the preceding instruction).
CodeInjection lightmap_global_faces_shadow_injection{
    0x004ae6fc,
    [](auto& regs) {
        *reinterpret_cast<std::uint32_t*>(static_cast<uintptr_t>(regs.esp) + 0x8f8) = 0;
        regs.eip = smooth_lights_active() ? 0x004ae803 : 0x004ae707;
    },
    false, // no trampoline: the injection fully replaces the 11 byte store
};

// FUN_004ad160 lumel face list: "MOV dword ptr [ESP+0x28c],EBX" (a single 7 byte store, EBX = 0).
CodeInjection lightmap_global_faces_lumel_injection{
    0x004ad20f,
    [](auto& regs) {
        *reinterpret_cast<std::uint32_t*>(static_cast<uintptr_t>(regs.esp) + 0x28c) =
            static_cast<std::uint32_t>(static_cast<int>(regs.ebx));
        regs.eip = smooth_lights_active() ? 0x004ad262 : 0x004ad216;
    },
    false, // no trampoline: the injection fully replaces the 7 byte store
};

void ApplyLightmapPatches()
{
    // Fix pink lightmaps when a face is affected by >= 64 lights
    // Allocate shadow mask buffers on the heap; grown on demand for high resolution levels
    shadow_mask_reserve(0x1000);

    // Replace light handle-to-pointer with bounds-checked version
    // Prevents corruption on handle=-1, which occurs when trying to add/edit lights after max_scene_lights
    // Original: ptr = pool_base + handle * 0x10C (no validation)
    light_handle_to_pointer_injection.install();

    // Replace the >= 64 limit check with new limit
    lightmap_light_limit_injection.install();

    // Lightmap pages are saved whole, so their inter-fragment gaps must not be heap garbage
    lightmap_page_clear_injection.install();

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

    // Alpine directional sunlight: contribute a temporary type 1 light to both bake commands.
    // Inert unless the level has enable_sun set.
    lighting_calc_shadows_hook.install();
    lighting_calc_no_shadows_hook.install();
    sun_shadow_mask_hook.install();
    sun_face_light_dedup_injection.install();
    sun_sky_occluder_skip_injection.install();

    // Lightmap bake accuracy fixes, all inert when the level sets Legacy lighting
    lightmap_texel_convert_injection.install();
    lightmap_smooth_grey_01_injection.install();
    lightmap_smooth_grey_033_injection.install();
    lightmap_smoothing_normal_weight_injection.install();
    lightmap_border_duplicate_skip_injection.install();
    lightmap_alpha_texture_occluder_injection.install();

    // High resolution lightmaps, inert unless the level sets it
    lightmap_highres_setup_injection.install();
    lightmap_fragment_clamp_injection.install();
    lightmap_blend_surfaces_hook.install();
    lightmap_blend_pass_hook.install();
    lightmap_blend_pair_dedup_injection.install();
    lightmap_blend_face_verts_injection.install();
    lightmap_blend_face_verts_base_injection.install();
    lightmap_blend_face_vert_index_injection.install();
    write_mem_ptr(0x004aa06f + 2, &g_lm_fragment_max_f);
    write_mem_ptr(0x004aa08d + 2, &g_lm_fragment_max_f);

    // Cross-room surface merging. Default on, off for Legacy lighting levels; -smoothlights forces
    // it on for those too. GetCommandLineA() is used instead of argv/argc because this runs during
    // DLL init, before the CRT has populated argv/argc at their fixed addresses.
    g_force_smooth_lights = std::strstr(GetCommandLineA(), "-smoothlights") != nullptr;

    lightmap_group_cross_room_injection.install();
    lightmap_force_should_smooth_injection.install();
    lightmap_global_lights_shadow_injection.install();
    lightmap_global_lights_vertex_injection.install();
    lightmap_global_faces_shadow_injection.install();
    lightmap_global_faces_lumel_injection.install();
    lightmap_cross_room_blend_injection.install();
    lightmap_per_texel_ambient_fill_injection.install();
    lightmap_per_texel_ambient_nolights_injection.install();

    // Room linker ambient properties are only written to GRoom objects by the room properties
    // dialog, so a "Build Geometry" leaves fresh rooms with ambient_light_defined = 0.
    lightmap_apply_room_ambient_injection.install();
}
