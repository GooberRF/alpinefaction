#include <windows.h>
#include <algorithm>
#include <bit>
#include <memory>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <limits>
#include <string>
#include <utility>
#include <vector>
#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/FunHook.h>
#include <patch_common/MemUtils.h>
#include <patch_common/ShortTypes.h>
#include <xlog/xlog.h>
#include <common/scope_guard.h>
#include <common/bitmap/formats.h>
#include <common/lightmap/alpine_lightmap.h>
#include <common/alpine_dir_light.h>
#include "dir_light.h"
#include "level.h"
#include "lightmap_mesh_occluders.h"
#include "lightmap_alpha_masks.h"
#include "alpine_lightmaps.h"
#include "bake_progress.h"
#include "headless_bake.h"
#include "overflow_charts.h"
#include "terrain_build.h"
#include "textures.h"
#include "work_pool.h"

// High resolution lightmaps
static constexpr int lm_stock_page_size = static_cast<int>(alpine_lightmap::stock_page_size(false));
static constexpr int lm_highres_page_size = static_cast<int>(alpine_lightmap::stock_page_size(true));
static constexpr int lm_stock_fragment_max = 64;
static constexpr int lm_highres_fragment_max = 254;
static constexpr int lm_max_fragment_texels = lm_highres_fragment_max * lm_highres_fragment_max;

// Max lights that can be processed per face (shadow mask buffer limit).
// Faces with more lights than this get the pink fill safety fallback.
static constexpr int max_shadow_masks = 1024;
static void* shadow_mask_ptrs[max_shadow_masks];
static std::unique_ptr<uint8_t[]> shadow_mask_pool;
static std::size_t shadow_mask_pool_bytes = 0;

static void shadow_mask_release()
{
    shadow_mask_pool.reset();
    shadow_mask_pool_bytes = 0;
}

// Lays out one mask of `texels` + 1 bytes per light of the surface about to be shaded. The masks
// only live for that surface, so a pool too small for it is freed before the bigger one is made.
static bool shadow_mask_reserve(int texels, int lights)
{
    const std::size_t entry = static_cast<std::size_t>(texels) + 1;
    const std::size_t need = entry * static_cast<std::size_t>(lights);
    if (need > shadow_mask_pool_bytes) {
        std::size_t bytes = std::max(need, shadow_mask_pool_bytes + shadow_mask_pool_bytes / 2);
        shadow_mask_release();
        shadow_mask_pool.reset(new (std::nothrow) uint8_t[bytes]);
        if (!shadow_mask_pool && bytes > need) {
            bytes = need;
            shadow_mask_pool.reset(new (std::nothrow) uint8_t[bytes]);
        }
        if (!shadow_mask_pool) {
            xlog::error("Lightmap: cannot allocate {} shadow masks of {} texels", lights, texels);
            return false;
        }
        shadow_mask_pool_bytes = bytes;
    }
    for (int i = 0; i < lights; i++) {
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

// Independent of Legacy lighting: a legacy level can still be baked at high resolution.
static bool highres_lightmaps_active()
{
    auto* level = CDedLevel::Get();
    return level && level->GetAlpineLevelProperties().highres_lightmaps;
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

static bool alpha_tested_occlusion_active()
{
    auto* level = CDedLevel::Get();
    return level && level->GetAlpineLevelProperties().alpha_tested_occlusion_active();
}

// Which compiled faces belong to a brush flagged "No shadow cast".
// CSG carries a source brush face's id (GFace +0x38) onto every compiled fragment it produces.
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

        // One shadow mask per light of this surface
        const auto* surface = reinterpret_cast<const GSurface*>(static_cast<uintptr_t>(regs.esi));
        int width = surface->width;
        int height = surface->height;
        if (light_count >= max_shadow_masks) {
            xlog::warn("Lightmap: {} lights affect the surface at 0x{:x}, exceeding the {} shadow "
                       "mask limit! Falling back to pink fill",
                       light_count, static_cast<uintptr_t>(regs.esi), max_shadow_masks);
            regs.eip = 0x004AC9E4; // pink fill safety fallback
        }
        else if (width <= 0 || height <= 0 || width > lm_highres_page_size ||
                 height > lm_highres_page_size ||
                 !shadow_mask_reserve(width * std::max(width, height), light_count)) {
            xlog::error("Lightmap: cannot cover a {}x{} surface at 0x{:x} with shadow masks! "
                        "Falling back to pink fill",
                        width, height, static_cast<uintptr_t>(regs.esi));
            regs.eip = 0x004AC9E4; // pink fill safety fallback
        }
        else {
            regs.eip = 0x004AC611; // normal lightmap processing
        }
    },
    // no trampoline: cannot be relocated; every path above sets eip
    false,
};

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

// The page FUN_004a3c80 synthesised for a level that ships no stock lightmaps, while it lives.
static void* g_synth_page = nullptr;

// FUN_004a65b0, the lightmap page destructor, releases the page's bitmap but never its D3D texture.
static void __fastcall lightmap_page_free_new(void* page);
static FunHook<void __fastcall(void*)> lightmap_page_free_hook{0x004a65b0, lightmap_page_free_new};

static void __fastcall lightmap_page_free_new(void* page)
{
    if (page == g_synth_page) {
        g_synth_page = nullptr;
    }
    const int bm = static_cast<GLightmap*>(page)->bm_handle;
    if (GrTextureSlot* slot = gr_texture_slot_of(bm); slot && slot->bm_handle == bm) {
        gr_texture_free(slot);
    }
    lightmap_page_free_hook.call_target(page);
}

// FUN_004a3c80 gives a level that ships no stock lightmaps one synthesised 128x128 page for every
// surface to point at, but the surfaces' rects and uv_scale/uv_add were packed for pages of the
// level's stock page size. Only the alpine lightmap section records that size, and it is read after
// the geometry, so the page starts at the size the High-res lightmaps property implies and
// lightmap_synthesized_page_resize corrects it once the section is read.
static bool g_stock_layout_synthesized = false;

bool lightmap_stock_layout_synthesized()
{
    return g_stock_layout_synthesized;
}

static void* __fastcall lightmap_synth_page_new(void* self, int edx, int w, int h, char alpha,
                                                void* pixels);
static CallHook<void* __fastcall(void*, int, int, int, char, void*)> lightmap_synth_page_hook{
    0x004a3d04, lightmap_synth_page_new};

static void* __fastcall lightmap_synth_page_new(void* self, int edx, int w, int h, char alpha,
                                                void* pixels)
{
    if (highres_lightmaps_active()) {
        w = lm_highres_page_size;
        h = lm_highres_page_size;
    }
    void* page = lightmap_synth_page_hook.call_target(self, edx, w, h, alpha, pixels);
    // the loader clamps every surface's -1 index to this page, so the rects packed across many
    // pages now overlap on one until the next repack
    g_stock_layout_synthesized = true;
    g_synth_page = page;
    auto* level = CDedLevel::Get();
    auto* buf = self ? static_cast<GLightmap*>(self)->pixels : nullptr;
    // a d3d11-only level reads as unlit in the RED viewport rather than black
    if (buf && level && level->GetAlpineLevelProperties().stock_lightmaps_omitted) {
        std::memset(buf, 0xff, static_cast<std::size_t>(w) * h * 3);
    }
    return page;
}

void lightmap_synthesized_page_resize(int edge)
{
    auto* page = static_cast<GLightmap*>(g_synth_page);
    if (!g_stock_layout_synthesized || !page || edge <= 0 || edge > lm_highres_page_size) {
        return;
    }
    if (page->w == edge && page->h == edge) {
        return;
    }
    const std::size_t bytes = static_cast<std::size_t>(edge) * edge * 3;
    auto* pixels = static_cast<std::uint8_t*>(editor_alloc(bytes));
    if (!pixels) {
        return;
    }
    // Rebuilt in place as FUN_004a6510 builds it: the surfaces and the page list hold its address and index.
    lightmap_page_free_new(page);
    page->alpha = nullptr;
    page->w = edge;
    page->h = edge;
    page->pixels = pixels;
    page->bm_handle = bm_create(BM_FORMAT_1555_ARGB, edge, edge);
    g_synth_page = page;
    auto* level = CDedLevel::Get();
    const bool omitted = level && level->GetAlpineLevelProperties().stock_lightmaps_omitted;
    std::memset(pixels, omitted ? 0xff : 0, bytes);
    editor_report(EditorReportLevel::info, "Lightmap",
                  "the synthesised lightmap page is " + std::to_string(edge) + "x" + std::to_string(edge) +
                      ", the stock page size the alpine lightmap section records",
                  false);
}

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
        if (!bake_fixes_active()) {
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
    // no trampoline: cannot be relocated; every path above sets eip
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
// Run at the entry of the batch lightmap calculator (FUN_004aabf0) for the solid it bakes, this
// iterates the room linker list, applying ambient settings to the corresponding rooms.
static void lightmap_collect_room_ambient(uintptr_t gsolid)
{
    s_ambient_room_count = 0;
    if (!bake_fixes_active()) return;
    if (!gsolid) return;

    auto* level = CDedLevel::Get();
    if (!level) return;

    // Room linkers are the room effect objects
    const auto& room_linkers = level->room_effects;
    if (room_linkers.size <= 0 || !room_linkers.data_ptr) return;

    // The BSP spatial lookup and GRoom bounding boxes don't reliably match surface
    // room_index values (BSP can return room_index=183 when surfaces use 0-6).
    // Instead, build combined bounding boxes from surfaces per room_index. This matches
    // the exact room assignment the lightmap code uses.
    const auto* solid = reinterpret_cast<const GSolid*>(gsolid);
    int room_count = solid->all_rooms.size;
    GRoom* const* room_elements = solid->all_rooms.data_ptr;
    if (room_count <= 0 || !room_elements) return;

    int surface_count = solid->surfaces.size;
    const GSurface* const* surface_elements = solid->surfaces.data_ptr;
    if (surface_count <= 0 || !surface_elements) return;

    // Build combined bbox per room_index from surfaces
    constexpr int max_tracked_rooms = 512;
    struct RoomBBox { float mn[3]; float mx[3]; bool valid; };
    auto* room_bboxes = new (std::nothrow) RoomBBox[max_tracked_rooms]();
    if (!room_bboxes) return;

    for (int s = 0; s < surface_count; s++) {
        const GSurface* surf = surface_elements[s];
        if (!surf) continue;
        int ridx = surf->room_index;
        if (ridx < 0 || ridx >= max_tracked_rooms) continue;
        auto* smn = surf->bbox_mn;
        auto* smx = surf->bbox_mx;
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
            GRoom* room = room_elements[best_ridx];
            if (room) {
                room->ambient_light_defined = true;
                room->ambient_light = std::bit_cast<Color>(linker->ambient_color);
            }
        }
    }
    // Collect per-texel ambient data from rooms with custom ambient, using the
    // surface-combined bboxes (which reliably match surface room_index assignments).
    // Must be done BEFORE deleting room_bboxes.
    s_ambient_room_count = 0;
    for (int ridx = 0; ridx < room_count && ridx < max_tracked_rooms
             && s_ambient_room_count < kMaxAmbientRooms; ridx++) {
        const GRoom* room = room_elements[ridx];
        if (!room) continue;
        if (room->ambient_light_defined != 1) continue;
        if (!room_bboxes[ridx].valid) continue; // no surfaces for this room
        auto& entry = s_ambient_rooms[s_ambient_room_count];
        for (int c = 0; c < 3; c++) {
            entry.bbox_min[c] = room_bboxes[ridx].mn[c];
            entry.bbox_max[c] = room_bboxes[ridx].mx[c];
        }
        constexpr float inv255 = 1.0f / 255.0f;
        entry.r = static_cast<float>(room->ambient_light.r) * inv255;
        entry.g = static_cast<float>(room->ambient_light.g) * inv255;
        entry.b = static_cast<float>(room->ambient_light.b) * inv255;
        s_ambient_room_count++;
    }

    delete[] room_bboxes;
}

// FUN_004aabf0 lights the level solid first, then each mover's.
static void lightmap_progress_batch(uintptr_t solid)
{
    auto* level = CDedLevel::Get();
    if (level && reinterpret_cast<GSolid*>(solid) == level->solid) {
        bake_progress_phase(BakePhase::surfaces, solid_surfaces(level->solid).size());
    }
    else {
        bake_progress_phase(BakePhase::movers);
    }
}

CodeInjection lightmap_apply_room_ambient_injection{
    0x004aabf0, // entry of FUN_004aabf0 (batch lightmap calculator)
    [](auto& regs) {
        // ECX at FUN_004aabf0 entry is the GSolid used for lightmap calculation.
        lightmap_collect_room_ambient(regs.ecx);
        lightmap_progress_batch(regs.ecx);
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
        const float shadowed = red_const_half;

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

// Texel (col, row) of a surface's fragment at (xstart, ystart) in a page `stride` texels wide, as the
// engine addresses it. A tile view's page pointer is biased so this lands in its buffer
// (alpine_lightmaps.cpp shade_tile), which relies on 32 bit wraparound, so it is unsigned arithmetic.
static uint8_t* lm_page_texel(uint8_t* buf, int stride, int xstart, int ystart, int col, int row)
{
    static_assert(sizeof(uintptr_t) == sizeof(uint32_t));
    const uint32_t y = static_cast<uint32_t>(ystart) + static_cast<uint32_t>(row);
    const uint32_t x = static_cast<uint32_t>(xstart) + static_cast<uint32_t>(col);
    const uint32_t off = (y * static_cast<uint32_t>(stride) + x) * 3u;
    return reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(buf) + off);
}

// Per-texel ambient fill for the no-lights path in FUN_004ac470.
CodeInjection lightmap_per_texel_ambient_nolights_injection{
    0x004ac563, // FLD [ESP+0x78] — start of ambient byte conversion
    [](auto& regs) {
        // NOTE: no trampoline.
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
        const bool per_texel = bake_fixes_active() && s_ambient_room_count > 0 &&
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
                uint8_t* texel = lm_page_texel(buf, stride, xstart, ystart, col, row);
                texel[0] = static_cast<uint8_t>(static_cast<int>(ar * scale));
                texel[1] = static_cast<uint8_t>(static_cast<int>(ag * scale));
                texel[2] = static_cast<uint8_t>(static_cast<int>(ab * scale));
            }
        }
    },
    false, // no trampoline: the injection fully replaces the fill
};

// Ray traced shadow masks
static constexpr float lm_ray_lift = 0.02f;
static constexpr float lm_ray_eps = 0.01f;
static constexpr float lm_ray_band = 0.05f;
static constexpr float lm_oneside_eps = 1.0e-4f;

// Synthetic OccTri flag, outside the 16 bit face flags word: the face's texture carries an alpha
// channel, which is what the stock occluder filter rejects it for (FUN_004bcc60 at 0x004aed62).
static constexpr unsigned lm_occ_alpha_texture = 0x80000000u;
// A ray that keeps less of its light than this is fully shadowed.
static constexpr float lm_alpha_blocked = 1.0f / 512.0f;
// The alpha is averaged over this many receiving texels: one alone leaves moire where a grate's holes
// land a few texels apart.
static constexpr float lm_alpha_filter_width = 2.0f;
// Grazing hits are filtered as if at this cosine, so their footprint stays finite.
static constexpr float lm_alpha_min_cosine = 0.25f;
// Parallel hits on one texture this close along their normal are one layer: both faces of a thin grate brush
// or back to back mesh cards, whose holes line up. So are hits at one point, either side of a shared edge.
static constexpr float lm_alpha_layer_gap = 0.25f;
static constexpr float lm_alpha_layer_parallel = 0.9f;
static constexpr float lm_alpha_layer_coincident = 1.0e-3f;
// Distinct layers a ray keeps apart before folding the rest straight into its transmittance.
static constexpr int lm_alpha_max_layers = 8;

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

// False, leaving `v` as it is, for a near-zero or NaN length.
inline bool vnormalize(Vec3f& v)
{
    const float len = std::sqrt(vdot(v, v));
    if (!(len >= 1e-6f)) {
        return false;
    }
    v = {v.x / len, v.y / len, v.z / len};
    return true;
}

struct OccTri {
    Vec3f v0, e1, e2;
    Vec3f normal;
    Vec3f centroid;
    Vec3f bmin, bmax;
    int surf_id;
    int mesh_uid; // owning alpine mesh object, -1 for a brush face
    unsigned flags;
    int alpha = -1; // OccluderTree::alpha_tris_ index of a triangle that blocks by its texture's alpha
};

// The texture mapping of an alpha-tested triangle: uv = uv0 + u * duv1 + v * duv2 at barycentrics (u, v).
struct OccAlphaTri {
    const LightmapAlphaMask* mask;
    float u0, v0, du1, dv1, du2, dv2;
    float density; // texels of the mask's finest level per world unit
    bool in_rect;  // the triangle maps inside one texture repeat, so it samples only its own texture area
    LightmapAlphaMask::Rect rect;
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
    float footprint = 0.0f; // world width of the receiving texel, which alpha-tested hits are filtered over
    float converge = 0.0f;  // 1 / distance to a point light the footprint narrows onto, 0 for parallel rays
};

class OccluderTree
{
public:
    bool build(uintptr_t solid, const NoShadowCastFilter& no_shadow_cast, bool local_space);
    bool empty() const { return tris_.empty(); }
    void clear()
    {
        tris_.clear();
        alpha_tris_.clear();
        order_.clear();
        nodes_.clear();
        skipped_faces_ = 0;
    }
    int skipped_faces() const { return skipped_faces_; }
    // The fraction of the ray's light that reaches tmax: 0 at an opaque hit, the product of what the
    // alpha-tested triangles it crosses let through otherwise.
    float transmittance(const OccQuery& q) const;

private:
    int build_range(int begin, int end, int depth);
    bool add_alpha(OccTri& t, const LightmapAlphaMask* mask, const float* uv0, const float* uv1, const float* uv2);

    int skipped_faces_ = 0;
    std::vector<OccTri> tris_;
    std::vector<OccAlphaTri> alpha_tris_;
    std::vector<int> order_;
    std::vector<OccNode> nodes_;
};

// A fan from vertex 0 only reproduces a convex polygon.
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

// False, leaving `t` as it is, for non-finite texture coordinates.
bool OccluderTree::add_alpha(OccTri& t, const LightmapAlphaMask* mask, const float* uv0, const float* uv1,
                             const float* uv2)
{
    for (const float* uv : {uv0, uv1, uv2}) {
        if (!std::isfinite(uv[0]) || !std::isfinite(uv[1])) {
            return false;
        }
    }
    OccAlphaTri a{mask, uv0[0], uv0[1], uv1[0] - uv0[0], uv1[1] - uv0[1], uv2[0] - uv0[0], uv2[1] - uv0[1], 0.0f,
                  false, {}};
    // an atlas part or a card: filtering must not reach the texture around it
    const float lo_u = std::min({uv0[0], uv1[0], uv2[0]});
    const float lo_v = std::min({uv0[1], uv1[1], uv2[1]});
    const float tile_u = std::floor(lo_u + 1e-4f);
    const float tile_v = std::floor(lo_v + 1e-4f);
    const float hi_u = std::max({uv0[0], uv1[0], uv2[0]}) - tile_u;
    const float hi_v = std::max({uv0[1], uv1[1], uv2[1]}) - tile_v;
    if (hi_u <= 1.0f + 1e-4f && hi_v <= 1.0f + 1e-4f) {
        a.in_rect = true;
        a.u0 -= tile_u;
        a.v0 -= tile_v;
        a.rect = {std::max(lo_u - tile_u, 0.0f), std::max(lo_v - tile_v, 0.0f), std::min(hi_u, 1.0f),
                  std::min(hi_v, 1.0f)};
    }
    const Vec3f n = vcross(t.e1, t.e2);
    const float world = std::sqrt(vdot(n, n));
    const auto& finest = mask->levels.front();
    const float texels = std::abs(a.du1 * a.dv2 - a.du2 * a.dv1) * static_cast<float>(finest.w) *
                         static_cast<float>(finest.h);
    const float density = std::sqrt(texels / world);
    a.density = std::isfinite(density) ? density : 0.0f;
    t.alpha = static_cast<int>(alpha_tris_.size());
    alpha_tris_.push_back(a);
    return true;
}

bool OccluderTree::build(uintptr_t solid, const NoShadowCastFilter& no_shadow_cast, bool local_space)
{
    tris_.clear();
    alpha_tris_.clear();
    order_.clear();
    nodes_.clear();
    skipped_faces_ = 0;
    if (!solid) {
        return false;
    }
    const bool alpha_tested = alpha_tested_occlusion_active();
    int alpha_face_tris = 0;
    constexpr int max_faces = 1 << 21;
    int guard = 0;
    for (uintptr_t face = *reinterpret_cast<uintptr_t*>(solid + 0x70); face && guard < max_faces;
         face = *reinterpret_cast<uintptr_t*>(face + 0x54), guard++) {
        // the engine's face flags are the 16 bit RFL word, so the synthetic bits below own
        // everything above it; masking keeps a stray high bit out of the alpha texture test
        unsigned flags = *reinterpret_cast<unsigned*>(face + 0x28) & 0xffffu;
        const int bitmap = *reinterpret_cast<int*>(face + 0x30);
        // A see-through face is drawn alpha blended, so alpha-tested occlusion lets it block by its texels'
        // alpha. Glass (no FACE_HAS_HOLES) keeps letting light through.
        const LightmapAlphaMask* alpha_mask = nullptr;
        if (flags & FACE_SEE_THRU) {
            alpha_mask = alpha_tested && (flags & FACE_HAS_HOLES) ? lightmap_alpha_mask(bitmap) : nullptr;
            if (!alpha_mask) {
                continue;
            }
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
        // liquid and invisible faces answer to their own level property, so the stock rejection
        // never gets to overrule it - a water texture is alpha capable practically by definition
        if (!alpha_mask && !(flags & 0x2004u) && bitmap != -1 && bm_has_alpha(bitmap) != 0) {
            flags |= lm_occ_alpha_texture;
        }
        const int surf_id = *reinterpret_cast<std::int16_t*>(face + 0x36);
        // the ear clipper winds every triangle the same way in its projection plane, which flips
        // half of them; the face's own plane normal is what "front" has to mean
        const auto* face_normal = reinterpret_cast<const float*>(face);
        Vec3f verts[occ_max_face_verts];
        float uvs[occ_max_face_verts][2];
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
            uvs[n][0] = reinterpret_cast<const GFaceVertex*>(node)->u;
            uvs[n][1] = reinterpret_cast<const GFaceVertex*>(node)->v;
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
            if (alpha_mask) {
                if (!add_alpha(t, alpha_mask, uvs[fan[i * 3]], uvs[fan[i * 3 + 1]], uvs[fan[i * 3 + 2]])) {
                    continue;
                }
                alpha_face_tris++;
            }
            tris_.push_back(t);
        }
    }
    int alpha_mesh_tris = 0;
    // Alpine mesh objects and terrain decorations live in world space, so they only belong to the
    // static solid's tree; a mover's tree is brush local and answers only the rays cast onto that mover.
    if (!local_space) {
        std::vector<MeshOccluderTri> mesh_tris;
        lightmap_collect_mesh_occluders(mesh_tris);
        lightmap_collect_decoration_occluders(mesh_tris);
        for (const MeshOccluderTri& m : mesh_tris) {
            // additive glows and beams add light, they never stop it
            if (alpha_tested && m.additive) {
                continue;
            }
            OccTri t{};
            if (!occ_make_tri({m.v0.x, m.v0.y, m.v0.z}, {m.v1.x, m.v1.y, m.v1.z}, {m.v2.x, m.v2.y, m.v2.z},
                              t)) {
                continue;
            }
            // no surface owns a mesh triangle and none of the face flag classes apply to it,
            // so it is a plain two-sided occluder that only answers to the alpha properties
            t.surf_id = -1;
            t.mesh_uid = m.uid;
            t.flags = m.alpha ? lm_occ_alpha_texture : 0u;
            // the texture drawn decides, an override included; one whose alpha cannot be sampled keeps
            // the whole-triangle rule of "Alpha-textured faces block light"
            if (alpha_tested && m.bitmap != -1) {
                const LightmapAlphaMask* mask = lightmap_alpha_mask(m.bitmap);
                if (mask && add_alpha(t, mask, m.uv[0], m.uv[1], m.uv[2])) {
                    t.flags = 0u;
                    alpha_mesh_tris++;
                }
                else {
                    t.flags = bm_has_alpha(m.bitmap) != 0 ? lm_occ_alpha_texture : 0u;
                }
            }
            tris_.push_back(t);
        }
    }
    if (alpha_face_tris + alpha_mesh_tris > 0) {
        xlog::info("[AlphaOcclusion] solid {:#x}: {} see-through face triangles and {} mesh triangles block "
                   "light by their texture's alpha",
                   solid, alpha_face_tris, alpha_mesh_tris);
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

float OccluderTree::transmittance(const OccQuery& qy) const
{
    if (nodes_.empty()) {
        return 1.0f;
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
    struct Layer {
        const LightmapAlphaMask* mask;
        Vec3f normal;
        float dist;
        float cover;
    };
    Layer layers[lm_alpha_max_layers];
    int layer_count = 0;
    float folded = 1.0f;
    auto passed = [&] {
        float p = folded;
        for (int k = 0; k < layer_count; k++) {
            p *= 1.0f - layers[k].cover;
        }
        return p;
    };
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
                // distance of it, so geometry a few hundredths above the surface still occludes;
                // both sides, or rays toward a light behind the face pass the far side of a slab
                if (std::abs(lm_ray_lift + dist * qy.nd) < lm_ray_band &&
                    std::abs(vdot(t.normal, qy.surf_normal)) > 0.999f) {
                    continue;
                }
                if (t.alpha < 0 || static_cast<std::size_t>(t.alpha) >= alpha_tris_.size()) {
                    return 0.0f;
                }
                // The alpha is averaged over the receiving texel's footprint where the ray meets the
                // triangle, laid onto its plane, so detail finer than a lightmap texel shades evenly.
                const OccAlphaTri& a = alpha_tris_[t.alpha];
                const float facing = std::abs(vdot(d, t.normal));
                const float width = qy.footprint * std::max(0.0f, 1.0f - dist * qy.converge);
                float lod = 0.0f;
                if (a.density > 0.0f && width > 0.0f) {
                    const float texels =
                        lm_alpha_filter_width * width / std::max(facing, lm_alpha_min_cosine) * a.density;
                    lod = texels > 1.0f ? std::log2(texels) : 0.0f;
                }
                const float cover =
                    a.mask->coverage(a.u0 + a.du1 * u + a.du2 * v, a.v0 + a.dv1 * u + a.dv2 * v, lod,
                                     a.in_rect ? &a.rect : nullptr);
                // a layer that lines up with one already crossed casts its holes onto the same spots
                bool merged = false;
                for (int k = 0; k < layer_count && !merged; k++) {
                    Layer& l = layers[k];
                    const float gap = std::abs(dist - l.dist);
                    if (l.mask == a.mask &&
                        (gap <= lm_alpha_layer_coincident ||
                         (gap * facing <= lm_alpha_layer_gap &&
                          std::abs(vdot(t.normal, l.normal)) > lm_alpha_layer_parallel))) {
                        l.cover = std::max(l.cover, cover);
                        merged = true;
                    }
                }
                if (!merged) {
                    if (layer_count < lm_alpha_max_layers) {
                        layers[layer_count++] = {a.mask, t.normal, dist, cover};
                    }
                    else {
                        folded *= 1.0f - cover;
                    }
                }
                if (passed() < lm_alpha_blocked) {
                    return 0.0f;
                }
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
    return passed();
}

// Soft directional light sampling: the axis plus two rings of four, all fixed - bakes must be reproducible.
constexpr int dir_light_cone_samples = 9;

// The frame the engine shades the current solid in: world, or a mover's own space while its
// transform is pushed.
struct ShadeFrame
{
    bool local = false;
    Vector3 pos{};
    Matrix3 orient{};

    static ShadeFrame current()
    {
        ShadeFrame f;
        if (gr_transform_stack_depth != 0) {
            f.local = true;
            f.pos = gr_transform_pos;
            f.orient = gr_transform_orient;
        }
        return f;
    }

    alpine_dir_light::Vec3 world_dir(float x, float y, float z) const
    {
        if (!local) {
            return {x, y, z};
        }
        const Vector3 d = orient * Vector3{x, y, z};
        return {d.x, d.y, d.z};
    }

    alpine_dir_light::Vec3 world_point(float x, float y, float z) const
    {
        if (!local) {
            return {x, y, z};
        }
        const Vector3 p = orient * Vector3{x, y, z} + pos;
        return {p.x, p.y, p.z};
    }
};

// How one light's shadow rays are cast from a receiving point.
struct LightRays
{
    int type = 0;
    const float* vec = nullptr;
    const float* vec_end = nullptr;
    float radius = 0.0f;
    unsigned skip_flags = 0;
    unsigned oneside_flags = 0;
    Vec3f cone[dir_light_cone_samples];
    int cone_count = 0;
    // set when only occluders inside this volume may shadow the light
    const alpine_dir_light::Volume* clip_volume = nullptr;
    ShadeFrame frame;
};

// The Alpine directional lights of one bake: the level sun first when it is enabled, then every
// Directional Light object that starts on.
struct BakeDirLight
{
    int handle = -1;
    float spread = 0.0f;
    bool liquid_occludes = true;
    bool sky_passes = true;
    bool outside_casts = true;
    alpine_dir_light::Volume volume{};
    float bound_radius = 0.0f;
    bool bounded() const
    {
        return volume.shape != alpine_dir_light::Shape::none;
    }
};

// Fixed storage, so building the table cannot throw out of BakeScope's constructor.
constexpr int max_bake_dir_lights = static_cast<int>(alpine_dir_light::max_lights) + 1;
BakeDirLight g_dir_lights[max_bake_dir_lights];
int g_dir_light_count = 0;
// Per scene light handle: its g_dir_lights index + 1, 0 for any other light.
int g_dir_light_slot[max_scene_lights];
bool g_dir_lights_bounded = false;

const BakeDirLight* bake_dir_light(const void* light)
{
    if (g_dir_light_count == 0 || !light) {
        return nullptr;
    }
    const auto off = reinterpret_cast<uintptr_t>(light) - reinterpret_cast<uintptr_t>(light_pool);
    if (off >= sizeof(light_pool) || off % light_entry_size != 0) {
        return nullptr;
    }
    const int slot = g_dir_light_slot[off / light_entry_size];
    return slot > 0 ? &g_dir_lights[slot - 1] : nullptr;
}

constexpr float deg_to_rad = 3.14159265358979f / 180.0f;

} // namespace

// Set for exactly as long as one of the two Calculate Lighting commands is running.
static bool g_bake_active = false;
// The shadow mode that command hands FUN_004ac470: 1 casts shadows, 0 does not.
static int g_bake_mode = 0;

// One cache per GSolid. Does not outlive a bake.
struct SolidCache {
    OccluderTree tree;
    bool tree_built = false;
    std::unordered_map<int, std::vector<uintptr_t>> faces_by_surface;
    // the whole face list was indexed
    bool complete = false;
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
        GFace* face = reinterpret_cast<GSolid*>(solid)->face_list_head;
        for (; face && guard < (1 << 21); face = face->next_solid, guard++) {
            if (face->surface_index >= 0) {
                cache->faces_by_surface[face->surface_index].push_back(reinterpret_cast<uintptr_t>(face));
            }
        }
        cache->complete = !face;
        return g_solid_cache.emplace(solid, std::move(cache)).first->second.get();
    }
    catch (...) {
        xlog::error("Lightmap: out of memory indexing a solid's faces, falling back to the stock "
                    "bake for it");
        // remembered, so the per-surface callers do not retry it
        try {
            g_solid_cache.emplace(solid, nullptr);
        }
        catch (...) {
        }
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
    lightmap_alpha_masks_release();
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

static void dir_light_cone_directions(const Vec3f& axis, float spread_deg, Vec3f* out, int& count)
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

static bool light_rays_setup(uintptr_t light, LightRays& lr)
{
    const auto* l = reinterpret_cast<const GrLight*>(light);
    lr.type = l->type;
    lr.frame = ShadeFrame::current();
    // while a mover transform is pushed the engine keeps the light in the solid's own space
    const bool local = lr.frame.local;
    lr.vec = local ? &l->local_vec.x : &l->vec.x;
    lr.vec_end = local ? &l->local_vec2.x : &l->vec2.x;
    lr.radius = l->rad_2;
    const BakeDirLight* dl = bake_dir_light(reinterpret_cast<const void*>(light));
    const bool one_sided = invisible_faces_occlude_active();
    unsigned skip_flags = FACE_LIQUID; // unless a directional light asks for it
    if (dl) {
        skip_flags = dl->sky_passes ? FACE_SHOW_SKY : 0u;
        if (!dl->liquid_occludes) {
            skip_flags |= FACE_LIQUID;
        }
    }
    if (!one_sided) {
        skip_flags |= 0x2000u;
    }
    if (!alpha_faces_occlude_active()) {
        skip_flags |= lm_occ_alpha_texture;
    }
    lr.skip_flags = skip_flags;
    lr.oneside_flags = one_sided ? FACE_INVISIBLE : 0u;
    lr.cone_count = 0;
    if (lr.type == LT_DIRECTIONAL) {
        Vec3f axis{lr.vec[0], lr.vec[1], lr.vec[2]};
        const float len = std::sqrt(vdot(axis, axis));
        if (!(len >= 1e-6f)) {
            return false;
        }
        axis = {axis.x / len, axis.y / len, axis.z / len};
        dir_light_cone_directions(axis, dl ? dl->spread : 0.0f, lr.cone, lr.cone_count);
        if (dl && dl->bounded() && !dl->outside_casts) {
            lr.clip_volume = &dl->volume;
        }
    }
    return true;
}

// The light's mask byte at one receiving point: 255 fully lit, 0 fully shadowed. The rays start
// `lift` off the point along the receiver's normal `ns`; `footprint` is the receiving texel's width.
static std::uint8_t light_rays_visibility(const OccluderTree& tree, const LightRays& lr, const Vec3f& pos,
                                          const Vec3f& ns, float lift, int skip_surf, float footprint)
{
    const Vec3f origin{pos.x + ns.x * lift, pos.y + ns.y * lift, pos.z + ns.z * lift};
    float lit = 0.0f;
    int taken = 0;
    OccQuery q{};
    q.origin = origin;
    q.surf_normal = ns;
    q.tmin = lm_ray_eps;
    q.skip_surf = skip_surf;
    q.skip_flags = lr.skip_flags;
    q.oneside_flags = lr.oneside_flags;
    q.footprint = std::isfinite(footprint) && footprint > 0.0f ? footprint : 0.0f;
    if (lr.type == LT_DIRECTIONAL) {
        q.tmax = 1.0e6f;
        const alpine_dir_light::Vec3 world_origin =
            lr.clip_volume ? lr.frame.world_point(origin.x, origin.y, origin.z) : alpine_dir_light::Vec3{};
        for (int k = 0; k < lr.cone_count; k++) {
            q.dir = lr.cone[k];
            q.nd = vdot(ns, q.dir);
            if (lr.clip_volume) {
                const float exit = alpine_dir_light::volume_ray_exit_distance(
                    *lr.clip_volume, world_origin, lr.frame.world_dir(q.dir.x, q.dir.y, q.dir.z));
                q.tmax = std::min(1.0e6f, exit);
            }
            taken++;
            lit += tree.transmittance(q);
        }
    }
    else {
        const int samples = lr.type == LT_TUBE ? 2 : 1;
        for (int k = 0; k < samples; k++) {
            const float* target = k == 0 ? lr.vec : lr.vec_end;
            Vec3f d{target[0] - origin.x, target[1] - origin.y, target[2] - origin.z};
            const float len = std::sqrt(vdot(d, d));
            if (!(len >= 1e-4f)) {
                taken++;
                lit += 1.0f;
                continue;
            }
            if (lr.radius > 0.0f && len > lr.radius) {
                continue; // outside the light's range, the mask value is never read
            }
            q.dir = {d.x / len, d.y / len, d.z / len};
            q.nd = vdot(ns, q.dir);
            q.tmax = len - lm_ray_eps;
            q.converge = 1.0f / len;
            taken++;
            lit += tree.transmittance(q);
        }
    }
    if (taken == 0) {
        return 0xffu;
    }
    // exact for whole rays, so a bake without alpha-tested occluders rounds as it always has
    const int lit255 = static_cast<int>(std::lround(lit * 255.0f));
    return static_cast<std::uint8_t>((lit255 + taken / 2) / taken);
}

// A texel's world space edges along its column and row.
static void texel_edges(const SurfaceUVParams& p, Vec3f& ec, Vec3f& er)
{
    float x0, y0, z0, x1, y1, z1, x2, y2, z2;
    texel_to_world(p, 0, 0, x0, y0, z0);
    texel_to_world(p, 1, 0, x1, y1, z1);
    texel_to_world(p, 0, 1, x2, y2, z2);
    ec = {x1 - x0, y1 - y0, z1 - z0};
    er = {x2 - x0, y2 - y0, z2 - z0};
}

// Farthest the accumulator may weigh a texel from its texel_to_world point (a diagonal, or the smooth vertex snap).
static float texel_weight_point_reach(const SurfaceUVParams& p)
{
    Vec3f ec, er;
    texel_edges(p, ec, er);
    const float edges2 = vdot(ec, ec) + vdot(er, er);
    const float diag = std::sqrt(edges2 + 2.0f * std::abs(vdot(ec, er)));
    const float tu = p.inv_lm_w;
    const float tv = p.inv_lm_h;
    const float snap = 0.5f * std::sqrt(tu * tu + tv * tv) / std::min(tu, tv) * std::sqrt(edges2);
    return std::max(diag, snap);
}

static bool lightmap_raycast_mask(uintptr_t solid, uintptr_t surface, uintptr_t light,
                                  std::uint8_t* mask)
{
    if (!surface || !light || !mask) {
        return false;
    }
    const auto* surf = reinterpret_cast<const GSurface*>(surface);
    const int width = surf->width;
    const int height = surf->height;
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

    LightRays lr;
    if (!light_rays_setup(light, lr)) {
        return false;
    }
    const int skip_surf = surf->index;
    const Vec3f ns{p.nx, p.ny, p.nz};
    Vec3f ec, er;
    texel_edges(p, ec, er);
    const float footprint = std::sqrt(std::sqrt(vdot(ec, ec)) * std::sqrt(vdot(er, er)));

    // Texels the accumulator will weight 0 anyway need no rays. Its weight point is within
    // texel_weight_point_reach of the texel centre, and the inside distance is 1-Lipschitz.
    const BakeDirLight* dl = bake_dir_light(reinterpret_cast<const void*>(light));
    const alpine_dir_light::Volume* cull = dl && dl->bounded() ? &dl->volume : nullptr;
    const float cull_margin = cull ? texel_weight_point_reach(p) + lm_ray_lift : 0.0f;

    auto shade_rows = [&](int row_begin, int row_end) {
        for (int row = row_begin; row < row_end; row++) {
            for (int col = 0; col < width; col++) {
                float wx, wy, wz;
                texel_to_world(p, col, row, wx, wy, wz);
                if (cull
                    && alpine_dir_light::volume_inside_distance(*cull, lr.frame.world_point(wx, wy, wz))
                           < -cull_margin) {
                    mask[row * width + col] = 0;
                    continue;
                }
                mask[row * width + col] =
                    light_rays_visibility(*tree, lr, {wx, wy, wz}, ns, lm_ray_lift, skip_surf, footprint);
            }
        }
    };

    if (width * height < 256) {
        shade_rows(0, height);
    }
    else {
        work_pool_run(height, [&](int row) { shade_rows(row, row + 1); });
    }
    return true;
}

// Alpine directional lights
static constexpr float dir_light_origin_distance = 5000.0f;
static constexpr int dir_light_spread_samples = 4;

// Every light is a temporary type 1 scene light; the x4 offsets the fixed 0.25 gain the engine applies to them.
static bool dir_light_add(Vector3 to_light, float intensity, std::uint8_t r, std::uint8_t g, std::uint8_t b,
                          bool cast_shadows, BakeDirLight entry)
{
    constexpr float inv255 = 1.0f / 255.0f;
    const int handle = light_create_directional(&to_light, intensity * 4.0f, r * inv255, g * inv255, b * inv255, 0,
                                                cast_shadows ? 1 : 0, 0);
    if (handle < 0 || handle >= max_scene_lights) {
        return false;
    }
    entry.handle = handle;
    entry.bound_radius = alpine_dir_light::volume_bounding_radius(entry.volume);
    g_dir_lights[g_dir_light_count++] = entry;
    g_dir_light_slot[handle] = g_dir_light_count;
    g_dir_lights_bounded = g_dir_lights_bounded || entry.bounded();
    return true;
}

static void dir_lights_destroy()
{
    for (int i = 0; i < g_dir_light_count; i++) {
        light_free(g_dir_lights[i].handle, 0);
        g_dir_light_slot[g_dir_lights[i].handle] = 0;
    }
    g_dir_light_count = 0;
    g_dir_lights_bounded = false;
}

static void dir_lights_create()
{
    dir_lights_destroy();
    auto* level = CDedLevel::Get();
    if (!level) {
        return;
    }
    auto& props = level->GetAlpineLevelProperties();

    if (props.enable_sun) {
        BakeDirLight sun;
        sun.spread = props.sun_spread_angle;
        sun.liquid_occludes = props.sun_liquid_occludes;
        if (!dir_light_add(props.sun_to_light_dir(), props.sun_intensity, props.sun_color_r, props.sun_color_g,
                           props.sun_color_b, props.sun_cast_baked_shadows, sun)) {
            xlog::error("Sunlight: failed to allocate a scene light for the lightmap bake");
        }
    }

    for (const DedDirectionalLight* obj : props.directional_light_objects) {
        if (!obj) {
            continue;
        }
        alpine_dir_light::Record rec = directional_light_record(*obj);
        alpine_dir_light::sanitize_record(rec);
        if (!rec.initially_on) {
            continue;
        }
        if (g_dir_light_count == max_bake_dir_lights) {
            xlog::warn("Lightmap: only {} directional lights are baked, the rest are left out",
                       alpine_dir_light::max_lights);
            break;
        }
        BakeDirLight dl;
        dl.spread = rec.spread;
        dl.liquid_occludes = rec.liquid_occludes != 0;
        dl.sky_passes = rec.sky_passes != 0;
        dl.outside_casts = rec.outside_casts != 0;
        dl.volume = alpine_dir_light::make_volume(rec);
        if (!dir_light_add({-rec.fvec.x, -rec.fvec.y, -rec.fvec.z}, rec.intensity, rec.color_r, rec.color_g,
                           rec.color_b, rec.cast_baked_shadows != 0, dl)) {
            xlog::error("Lightmap: failed to allocate a scene light for directional light {}", rec.uid);
        }
    }
}

// Brackets one Calculate Lighting command: the scene light, the caches and the reporting all
// belong to it and none of them may survive it, including when the bake below unwinds.
class BakeScope
{
public:
    BakeScope()
    {
        dir_lights_create();
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
            dir_lights_destroy();
        }
        catch (...) {
        }
        shadow_mask_release();
    }
    BakeScope(const BakeScope&) = delete;
    BakeScope& operator=(const BakeScope&) = delete;
};

void lighting_calc_report_refusal(const char* msg)
{
    editor_report_blocking("Lightmap", "Calculate Lighting", msg);
    headless_bake_mark_refused();
}

// Whether the address space holds what a bake of `pages` alpine pages allocates: the occluder tree is its
// biggest single allocation, and each page takes an RGB8 buffer and its BC7 blocks. `headroom` is added
// to both for what runs before the bake allocates. A shortfall is reported as a refusal.
static bool lighting_calc_fits(std::uint32_t pages, std::uint64_t headroom, const char* advice)
{
    constexpr std::uint64_t largest = 64ull << 20;
    constexpr std::uint64_t fixed = 96ull << 20;
    constexpr std::uint64_t per_page = alpine_lightmap::page_size * alpine_lightmap::page_size * 4ull;
    const std::string shortfall =
        editor_address_space_shortfall(largest + headroom, fixed + pages * per_page + headroom, advice);
    if (shortfall.empty()) {
        return true;
    }
    lighting_calc_report_refusal(shortfall.c_str());
    return false;
}

// Covers the address space RED's surface pass takes between the two checks (measured at 26-51 MB).
static constexpr std::uint64_t lighting_surface_pass_headroom = 64ull << 20;

// Before Calculate Lighting frees the level's lightmaps: refused unless a bake of the most pages fits
// after the surface pass. Only surface charts (which movers follow) and terrain charts take pages.
bool lighting_calc_memory_admits()
{
    auto* level = CDedLevel::Get();
    const auto* props = level ? &level->GetAlpineLevelProperties() : nullptr;
    const bool alpine_pages = props && (props->surface_charts_enabled() || !props->terrain_objects.empty());
    return lighting_calc_fits(alpine_pages ? alpine_lightmap::stage_page_budget : 0, lighting_surface_pass_headroom,
                              "Save the level and restart RED.");
}

// Once the charts are laid out, which has already dropped the loaded alpine section; the surface pass,
// when it ran, also rebuilt every stock page blank.
static bool lighting_calc_memory_ok(std::uint32_t pages, bool surface_pass_ran)
{
    return lighting_calc_fits(
        pages, 0,
        surface_pass_ran
            ? "Calculate Lighting has already cleared the level's lighting: restart RED and run Calculate Lighting "
              "again. Saving now saves the level without lighting."
            : "Calculate Lighting has already cleared the level's Alpine lightmaps, terrain lighting included: "
              "restart RED and run Calculate Lighting again. Saving now saves the level without its Alpine "
              "lightmaps.");
}

namespace
{

// The alpine half of a bake: one that is left without finishing is dropped, not encoded.
class AlpineBakeScope
{
public:
    explicit AlpineBakeScope(bool surface_pass_ran)
    {
        try {
            if (!lighting_calc_memory_ok(alpine_lm_bake_begin(), surface_pass_ran)) {
                admitted_ = false;
                return;
            }
            alpine_lm_bake_allocate();
        }
        catch (...) {
            alpine_lm_bake_abort();
            editor_report(EditorReportLevel::error, "Lightmap",
                          "out of memory preparing the alpine lightmap charts, the alpine lightmap section is not "
                          "baked",
                          true);
        }
    }
    // False when the address space cannot hold the bake, which must then not run.
    bool admitted() const { return admitted_; }
    // Nothing may unwind into the engine's lighting frames; a failed encode is dropped like an abort.
    void finish()
    {
        try {
            alpine_lm_bake_end();
            finished_ = true;
        }
        catch (...) {
            try {
                editor_report(EditorReportLevel::error, "Lightmap",
                              "the alpine lightmap section could not be encoded and is dropped", true);
            }
            catch (...) {
            }
        }
    }
    ~AlpineBakeScope()
    {
        if (!finished_) {
            try {
                alpine_lm_bake_abort();
            }
            catch (...) {
            }
        }
    }
    AlpineBakeScope(const AlpineBakeScope&) = delete;
    AlpineBakeScope& operator=(const AlpineBakeScope&) = delete;

private:
    bool finished_ = false;
    bool admitted_ = true;
};

} // namespace

// The blend and ring copy passes after FUN_004ac470 address every flagged surface's rect in its
// page with no bound, so a layout that does not fit its pages must not be baked at all.
static bool lighting_calc_refused()
{
    // the surface-pass hook already reported it
    if (alpine_lm_take_lighting_refused()) {
        return true;
    }
    auto* level = CDedLevel::Get();
    if (!level || !level->solid) {
        return false;
    }
    const auto surfaces = solid_surfaces(level->solid);
    const auto& props = level->GetAlpineLevelProperties();
    const bool stock_may_be_written = !(props.d3d11_only_lightmaps && props.surface_charts_enabled());
    if (g_stock_layout_synthesized && stock_may_be_written && !surfaces.empty()) {
        lighting_calc_report_refusal(
            "Calculate Lighting was not run: this level was loaded without stock lightmaps, so its "
            "surfaces share one page and would bake over each other. Run Build Geometry first.");
        return true;
    }
    for (int i = 0; i < static_cast<int>(surfaces.size()); i++) {
        const GSurface* s = surfaces[i];
        const GLightmap* lm = s ? s->lightmap : nullptr;
        if (!lm) {
            continue;
        }
        const int page_w = lm->w;
        const int page_h = lm->h;
        const int x = s->xstart;
        const int y = s->ystart;
        const int w = s->width;
        const int h = s->height;
        if (x >= 0 && y >= 0 && w >= 0 && h >= 0 && x <= page_w - w && y <= page_h - h) {
            continue;
        }
        char msg[320];
        std::snprintf(msg, sizeof(msg),
                      "Calculate Lighting was not run: surface %d has a %dx%d lightmap at %d,%d that "
                      "does not fit its %dx%d page. Run Build Geometry first.",
                      i, w, h, x, y, page_w, page_h);
        lighting_calc_report_refusal(msg);
        return true;
    }
    return false;
}

static void __fastcall lighting_calc_shadows_new(void* self);
static FunHook<void __fastcall(void*)> lighting_calc_shadows_hook{0x00448f20, lighting_calc_shadows_new};
static void __fastcall lighting_calc_no_shadows_new(void* self);
static FunHook<void __fastcall(void*)> lighting_calc_no_shadows_hook{0x004492d0, lighting_calc_no_shadows_new};

// The phases a bake of the level goes through, as far as the level's settings tell before the layout.
static unsigned lighting_calc_phases(std::uint64_t mover_surfaces)
{
    unsigned phases = bake_phase_bit(BakePhase::layout) | bake_phase_bit(BakePhase::surfaces) |
                      bake_phase_bit(BakePhase::blend) | bake_phase_bit(BakePhase::smoothing);
    if (mover_surfaces) {
        phases |= bake_phase_bit(BakePhase::movers);
    }
    auto* level = CDedLevel::Get();
    if (!level) {
        return phases;
    }
    const auto& props = level->GetAlpineLevelProperties();
    if (!props.terrain_objects.empty()) {
        phases |= bake_phase_bit(BakePhase::terrain) | bake_phase_bit(BakePhase::encode);
    }
    if (props.surface_charts_enabled()) {
        phases |= bake_phase_bit(BakePhase::encode);
        if (props.d3d11_only_lightmaps) {
            phases |= bake_phase_bit(BakePhase::overflow);
        }
    }
    return phases;
}

// After the layout: the phases it left nothing for are marked, the rest get their expected steps.
static void lighting_calc_expect(std::uint64_t mover_surfaces)
{
    auto* level = CDedLevel::Get();
    const std::uint64_t surfaces = level && level->solid ? solid_surfaces(level->solid).size() : 0;
    bake_progress_expect(BakePhase::surfaces, surfaces);
    bake_progress_expect(BakePhase::blend, surfaces);
    bake_progress_expect(BakePhase::smoothing, surfaces);
    bake_progress_expect(BakePhase::movers, mover_surfaces);
    const AlpineBakePlan plan = alpine_lm_bake_plan();
    if (plan.terrains) {
        bake_progress_expect(BakePhase::terrain, plan.terrains);
    }
    else {
        bake_progress_skip(BakePhase::terrain);
    }
    if (plan.active && overflow_has_table()) {
        bake_progress_expect(BakePhase::overflow, overflow_table_tiles());
    }
    else {
        bake_progress_skip(BakePhase::overflow);
    }
    if (plan.active) {
        bake_progress_expect(BakePhase::encode, plan.encode_steps);
    }
    else {
        bake_progress_skip(BakePhase::encode);
    }
}

// A cancelled bake leaves the level as its surface pass does: blank lightmaps and no alpine section. The stock pass
// runs without the hook's checks: they guard a bake that no longer runs, and a refusal would only leave the
// cancelled bake's partial lighting behind.
static void lighting_calc_discard(void* self)
{
    lighting_surfaces_stock(self);
    editor_report(EditorReportLevel::info, "Lightmap",
                  "Calculate Lighting was cancelled, the level has no baked lighting", true);
}

static void lighting_calc_bake(void* self, bool shadows, bool surface_pass_ran)
{
    if (bake_progress_active() || lighting_calc_refused()) {
        return;
    }
    bool cancelled = false;
    {
        const std::uint64_t mover_surfaces = mover_bake_surfaces();
        BakeProgressScope progress{lighting_calc_phases(mover_surfaces)};
        BakeScope bake;
        g_bake_mode = shadows ? 1 : 0;
        bake_progress_phase(BakePhase::layout, 0);
        AlpineBakeScope af_bake{surface_pass_ran};
        if (!af_bake.admitted()) {
            return;
        }
        lighting_calc_expect(mover_surfaces);
        if (shadows) {
            lighting_calc_shadows_hook.call_target(self);
        }
        else {
            lighting_calc_no_shadows_hook.call_target(self);
        }
        if (!bake_progress_cancelled()) {
            af_bake.finish();
        }
        cancelled = bake_progress_cancelled();
    }
    if (cancelled) {
        lighting_calc_discard(self);
    }
}

// Entered directly (the Calculate Lighting menu items and Shift+L), the bake runs without the surface
// pass, whose hook otherwise makes this check.
static void lighting_calc_bake_alone(void* self, bool shadows)
{
    if (!lighting_calc_memory_admits()) {
        return;
    }
    // no surface pass ran first, so any refusal it noted is stale
    alpine_lm_note_lighting_refused(false);
    lighting_calc_bake(self, shadows, false);
}

static void __fastcall lighting_calc_shadows_new(void* self)
{
    lighting_calc_bake_alone(self, true);
}

static void __fastcall lighting_calc_no_shadows_new(void* self)
{
    lighting_calc_bake_alone(self, false);
}

// Calculate Maps and Light (FUN_00449680) and its no-shadows twin (FUN_00449660) bake right after
// the surface pass.
static CallHook<void __fastcall(void*)> lighting_calc_shadows_after_surfaces_hook{
    0x0044968a,
    [](void* self) FASTCALL_LAMBDA { lighting_calc_bake(self, true, true); },
};
static CallHook<void __fastcall(void*)> lighting_calc_no_shadows_after_surfaces_hook{
    0x0044966a,
    [](void* self) FASTCALL_LAMBDA { lighting_calc_bake(self, false, true); },
};

static void lightmap_blend_reset();

// Build Geometry frees the GSolid the caches are keyed by, and a level load frees the whole level;
// the bake bracket already means nothing survives to see either, but the ambient snapshot is only
// rebuilt by a batch pass and would otherwise describe the previous level.
void lightmap_reset_level_state()
{
    s_ambient_room_count = 0;
    g_stock_layout_synthesized = false;
    g_synth_page = nullptr;
    alpine_lm_note_lighting_refused(false);
    g_no_shadow_cast_dropped.clear();
    g_occluder_tree_built = false;
    lightmap_release_occluders();
    lightmap_blend_reset();
}

// A bounded light whose volume cannot reach the gather box (world space, a mover's too) adds nothing to it.
static bool dir_light_misses_box(const BakeDirLight& dl, const Vector3* lo, const Vector3* hi)
{
    if (!dl.bounded() || !lo || !hi) {
        return false;
    }
    auto gap = [](float v, float low, float high) { return v < low ? low - v : (v > high ? v - high : 0.0f); };
    const auto& c = dl.volume.center;
    const float dx = gap(c.x, lo->x, hi->x);
    const float dy = gap(c.y, lo->y, hi->y);
    const float dz = gap(c.z, lo->z, hi->z);
    const float d2 = dx * dx + dy * dy + dz * dz;
    return d2 > dl.bound_radius * dl.bound_radius;
}

// Stock face light gathering adds type 1 lights unconditionally, once per room, so a surface reached
// through more than one room list would accumulate a directional light several times. Runs in place of
// "INC EAX; MOV [0x007432ec],EAX" that commits the list entry; EBP and EDI hold the gather box.
CodeInjection dir_light_face_light_dedup_injection{
    0x004889a2,
    [](auto& regs) {
        int index = regs.eax;
        bool duplicate = false;
        const BakeDirLight* dl =
            index >= 0 && index < max_scene_lights ? bake_dir_light(face_light_list[index]) : nullptr;
        if (dl) {
            duplicate = dir_light_misses_box(*dl, reinterpret_cast<const Vector3*>(static_cast<uintptr_t>(regs.ebp)),
                                             reinterpret_cast<const Vector3*>(static_cast<uintptr_t>(regs.edi)));
            for (int i = 0; i < index && !duplicate; i++) {
                duplicate = face_light_list[i] == face_light_list[index];
            }
        }
        // 0x00488810 stores into face_light_list without bounding the index; the light pool it
        // walks cannot exceed max_scene_lights entries, so the clamp is only a backstop
        int count = duplicate ? index : index + 1;
        count = std::clamp(count, 0, max_scene_lights);
        face_light_count = count;
        regs.eax = count;
        regs.eip = 0x004889a8;
    },
    false, // no trampoline: the injection fully replaces the 6 byte block
};

// Skip show sky faces for the directional lights that pass through them.
CodeInjection dir_light_sky_occluder_skip_injection{
    0x004aed36,
    [](auto& regs) {
        const auto* face = reinterpret_cast<const GFace*>(static_cast<uintptr_t>(regs.esi));
        const auto& args = *reinterpret_cast<const ShadowProjectorArgs*>(static_cast<uintptr_t>(regs.ebp));
        const auto flags = static_cast<uint32_t>(face->flags);
        regs.eax = static_cast<uintptr_t>(flags);
        const BakeDirLight* dl = bake_dir_light(args.light);
        constexpr uint32_t never_occludes = FACE_LIQUID | FACE_SEE_THRU | FACE_INVISIBLE;
        if ((flags & never_occludes) != 0 || (dl && dl->sky_passes && (flags & FACE_SHOW_SKY) != 0)) {
            regs.eip = 0x004af2e5; // continue with the next face
        }
        else {
            regs.eip = 0x004aed4a;
        }
    },
    false, // no trampoline: the injection fully replaces the 5 byte block
};

static void __cdecl shadow_mask_new(uintptr_t solid, uintptr_t surface, uintptr_t light, char debug,
                                    uint8_t* mask);
static FunHook<void __cdecl(uintptr_t, uintptr_t, uintptr_t, char, uint8_t*)> shadow_mask_hook{
    0x004ae360, shadow_mask_new};

// The stock projector is replaced by the per texel trace above for every light of a fixed pipeline
// bake and for the directional lights in a legacy one.
static void __cdecl shadow_mask_new(uintptr_t solid, uintptr_t surface, uintptr_t light, char debug,
                                    uint8_t* mask)
{
    const BakeDirLight* dl = bake_dir_light(reinterpret_cast<const void*>(light));
    // outside a bake this is RED's viewport relight of a single surface, which gets the stock
    // projector: the tracer's caches are only meaningful for as long as the command that built them
    if (g_bake_active && (bake_fixes_active() || dl) &&
        lightmap_raycast_mask(solid, surface, light, mask)) {
        return;
    }
    if (!dl) {
        shadow_mask_hook.call_target(solid, surface, light, debug, mask);
        return;
    }
    static bool warned = false;
    if (!warned) {
        warned = true;
        xlog::warn("Lightmap: a directional light's ray traced shadow mask could not be built for at least "
                   "one surface, falling back to the stock projector for it");
    }

    // while a mover transform is pushed the engine reads the solid-local copy
    auto& light_ref = *reinterpret_cast<GrLight*>(light);
    float* vec = gr_transform_stack_depth != 0 ? &light_ref.local_vec.x : &light_ref.vec.x;
    Vec3f to_light{vec[0], vec[1], vec[2]};
    if (!vnormalize(to_light)) {
        shadow_mask_hook.call_target(solid, surface, light, debug, mask);
        return;
    }

    const auto* bbox_min = reinterpret_cast<const float*>(surface + 0x34);
    const auto* bbox_max = reinterpret_cast<const float*>(surface + 0x40);
    const Vector3 center{(bbox_min[0] + bbox_max[0]) * 0.5f, (bbox_min[1] + bbox_max[1]) * 0.5f,
                         (bbox_min[2] + bbox_max[2]) * 0.5f};
    const Vector3 axis{center.x + to_light.x * dir_light_origin_distance,
                       center.y + to_light.y * dir_light_origin_distance,
                       center.z + to_light.z * dir_light_origin_distance};

    const float saved_vec[3] = {vec[0], vec[1], vec[2]};
    float& rad_2 = light_ref.rad_2;
    const float saved_rad_2 = rad_2;
    rad_2 = 1.0e9f;

    const int width = *reinterpret_cast<int*>(surface + 0x18);
    const int height = *reinterpret_cast<int*>(surface + 0x1c);
    const int texels = width * height;

    Vec3f offsets[dir_light_spread_samples] = {};
    int sample_count = 1;
    if (dl->spread > 0.0f && texels > 0 && texels <= lm_max_fragment_texels) {
        Vec3f up = std::abs(to_light.y) < 0.9f ? Vec3f{0.0f, 1.0f, 0.0f} : Vec3f{1.0f, 0.0f, 0.0f};
        Vec3f u = vcross(up, to_light);
        if (vnormalize(u)) {
            Vec3f v = vcross(to_light, u);
            float radius = std::tan(dl->spread * deg_to_rad) * dir_light_origin_distance;
            for (int k = 1; k < dir_light_spread_samples; k++) {
                float angle = (90.0f + 120.0f * static_cast<float>(k - 1)) * deg_to_rad;
                float cs = std::cos(angle) * radius;
                float sn = std::sin(angle) * radius;
                offsets[k] = {u.x * cs + v.x * sn, u.y * cs + v.y * sn, u.z * cs + v.z * sn};
            }
            sample_count = dir_light_spread_samples;
        }
    }

    if (sample_count == 1) {
        vec[0] = axis.x;
        vec[1] = axis.y;
        vec[2] = axis.z;
        shadow_mask_hook.call_target(solid, surface, light, debug, mask);
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
            shadow_mask_hook.call_target(solid, surface, light, debug, sample_mask);
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

// Stock's area test reads the unclipped face with the clipped count: a clip that gains vertices read stack garbage.
CodeInjection shadow_clip_area_count_injection{
    0x004af283,
    [](auto& regs) {
        const auto& frame = *reinterpret_cast<const ShadowProjectorFrame*>(static_cast<uintptr_t>(regs.esp));
        const int face_count = frame.faces.data_ptr[regs.ebx - 1]->count;
        if (face_count >= 3 && regs.esi > face_count) {
            regs.esi = face_count;
        }
    },
};

static void __cdecl light_accum_at_texel_new(float* r, float* g, float* b, const Vector3* pos, const Vector3* normal,
                                             void* masks, int texel_index, const void* smooth);
static FunHook<decltype(light_accum_at_texel_new)> light_accum_at_texel_hook{0x004894C0, light_accum_at_texel_new};

// FUN_004ad160's smooth path extrapolates a lumel from its row's two edge crossings, which float rounding can
// move off the texel when they nearly coincide. The texel centre keeps the volume weight within the reach
// lightmap_raycast_mask culls by.
static Vector3 smooth_lumel_weight_point(const GSurface& surface, const Vector3& pos, int texel_index)
{
    SurfaceUVParams p;
    if (surface.width <= 0 || texel_index < 0
        || !init_surface_uv_params(reinterpret_cast<uintptr_t>(&surface), p)) {
        return pos;
    }
    Vector3 centre;
    texel_to_world(p, texel_index % surface.width, texel_index / surface.width, centre.x, centre.y, centre.z);
    const Vec3f d{pos.x - centre.x, pos.y - centre.y, pos.z - centre.z};
    const float reach = texel_weight_point_reach(p);
    return vdot(d, d) > reach * reach ? centre : pos;
}

// Every bake path reaches this accumulator. The volume weight scales the light's colour for the one call,
// not its mask byte, which would quantise feathered edges to 1/255 of a bright light's output. Stock still
// shades at pos; only the volume weight moves, for a smooth lumel_surface.
static void light_accum_volume_weighted(float* r, float* g, float* b, const Vector3* pos, const Vector3* normal,
                                        void* masks, int texel_index, const void* smooth,
                                        const GSurface* lumel_surface)
{
    const int count =
        g_bake_active && g_dir_lights_bounded && pos ? std::clamp(face_light_count, 0, max_scene_lights) : 0;
    int first = count;
    for (int i = 0; i < count; i++) {
        const BakeDirLight* dl = bake_dir_light(face_light_list[i]);
        if (dl && dl->bounded()) {
            first = i;
            break;
        }
    }
    if (first == count) {
        light_accum_at_texel_hook.call_target(r, g, b, pos, normal, masks, texel_index, smooth);
        return;
    }

    // Main thread only: the worker pools never reach the accumulator.
    struct Scaled
    {
        GrLight* light;
        float r, g, b;
    };
    static std::uint8_t bytes[max_scene_lights];
    static const std::uint8_t* views[max_scene_lights];
    static Scaled scaled[max_scene_lights];
    int num_scaled = 0;
    const auto* in = static_cast<const std::uint8_t* const*>(masks);
    const Vector3 weight_point = lumel_surface ? smooth_lumel_weight_point(*lumel_surface, *pos, texel_index) : *pos;
    const alpine_dir_light::Vec3 world =
        ShadeFrame::current().world_point(weight_point.x, weight_point.y, weight_point.z);
    for (int i = 0; i < count; i++) {
        std::uint8_t m = in ? in[i][texel_index] : 0xffu;
        const BakeDirLight* dl = i < first ? nullptr : bake_dir_light(face_light_list[i]);
        if (dl && dl->bounded() && m != 0) {
            const float w = alpine_dir_light::volume_weight(dl->volume, world);
            if (!(w > 0.0f)) {
                m = 0;
            }
            else if (w < 1.0f) {
                auto* light = reinterpret_cast<GrLight*>(face_light_list[i]);
                scaled[num_scaled++] = {light, light->r, light->g, light->b};
                light->r *= w;
                light->g *= w;
                light->b *= w;
            }
        }
        bytes[i] = m;
        views[i] = &bytes[i];
    }
    light_accum_at_texel_hook.call_target(r, g, b, pos, normal, views, 0, smooth);
    for (int i = num_scaled - 1; i >= 0; i--) {
        scaled[i].light->r = scaled[i].r;
        scaled[i].light->g = scaled[i].g;
        scaled[i].light->b = scaled[i].b;
    }
}

static void __cdecl light_accum_at_texel_new(float* r, float* g, float* b, const Vector3* pos, const Vector3* normal,
                                             void* masks, int texel_index, const void* smooth)
{
    light_accum_volume_weighted(r, g, b, pos, normal, masks, texel_index, smooth, nullptr);
}

// When a row's two edge crossings coincide, FUN_004ad160 divides by 1.0 instead of their distance, which shades
// the whole row at the crossing. Loading NaN there instead marks those lumels for light_accum_smooth_lumel.
static const float lightmap_smooth_degenerate_row_marker = std::numeric_limits<float>::quiet_NaN();

// FUN_004ad160's smooth path call, which passes &surface->smooth.
static void __cdecl light_accum_smooth_lumel(float* r, float* g, float* b, const Vector3* pos, const Vector3* normal,
                                             void* masks, int texel_index, const void* smooth);
static CallHook<decltype(light_accum_smooth_lumel)> light_accum_smooth_lumel_hook{0x004adb30, light_accum_smooth_lumel};

static void __cdecl light_accum_smooth_lumel(float* r, float* g, float* b, const Vector3* pos, const Vector3* normal,
                                             void* masks, int texel_index, const void* smooth)
{
    const auto* surface = reinterpret_cast<const GSurface*>(static_cast<const std::uint8_t*>(smooth)
                                                            - offsetof(GSurface, smooth));
    if (std::isnan(pos->x)) {
        const auto& locals = *reinterpret_cast<const SmoothLumelLocals*>(normal);
        Vector3 texel = locals.crossing_pos;
        SurfaceUVParams p;
        if (surface->width > 0 && init_surface_uv_params(reinterpret_cast<uintptr_t>(surface), p)) {
            texel_to_world(p, texel_index % surface->width, texel_index / surface->width, texel.x, texel.y, texel.z);
        }
        light_accum_volume_weighted(r, g, b, &texel, &locals.crossing_normal, masks, texel_index, smooth, surface);
        return;
    }
    light_accum_volume_weighted(r, g, b, pos, normal, masks, texel_index, smooth, surface);
}

// Lightmap bake accuracy fixes

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
static void lm_encode_filtered(const float* acc_r, const float* acc_g, const float* acc_b, int col,
                               int row, int width, int height, std::uint8_t* out)
{
    const int x_min = std::max(col - 1, 0);
    const int x_max = std::min(col + 1, width - 1);
    const int y_min = std::max(row - 1, 0);
    const int y_max = std::min(row + 1, height - 1);

    double sum_r = 0.0, sum_g = 0.0, sum_b = 0.0;
    for (int y = y_min; y <= y_max; y++) {
        const std::size_t base = static_cast<std::size_t>(y) * width;
        for (int x = x_min; x <= x_max; x++) {
            sum_r += acc_r[base + x];
            sum_g += acc_g[base + x];
            sum_b += acc_b[base + x];
        }
    }
    const double scale = static_cast<double>(lm_read_const(0x0055c7fc)) /
                         ((x_max - x_min + 1) * (y_max - y_min + 1));
    lm_encode_bytes(sum_r * scale, sum_g * scale, sum_b * scale, out);
}

void lightmap_encode_float_texels(const float* r, const float* g, const float* b, int width, int height,
                                  std::uint8_t* out)
{
    for (int row = 0; row < height; row++) {
        for (int col = 0; col < width; col++) {
            lm_encode_filtered(r, g, b, col, row, width, height,
                               out + (static_cast<std::size_t>(row) * width + col) * 3);
        }
    }
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
            for (int col = 0; col < width; col++) {
                std::uint8_t* out = lm_page_texel(buf, stride, xstart, ystart, col, row);
                if (filter) {
                    lm_encode_filtered(lm_accum_r, lm_accum_g, lm_accum_b, col, row, width, height, out);
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
// dir_light_sky_occluder_skip_injection, which sits earlier in the same filter chain.
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

// High resolution lightmaps
static float g_lm_fragment_max_f = static_cast<float>(lm_stock_fragment_max);

CodeInjection lightmap_highres_setup_injection{
    0x004a9d49,
    [](auto& regs) {
        const uintptr_t esp = static_cast<uintptr_t>(regs.esp);
        regs.ebp = *reinterpret_cast<std::uint32_t*>(esp + 0x94);
        g_stock_layout_synthesized = false;
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

// Blend-pass pair cull: FUN_004aae80 skips the face pairs no vertex match can join.
namespace
{

// A longer (or cyclic) edge loop leaves the pass to the stock scan.
constexpr int face_loop_max = 1 << 16;

// A GFace's edge loop: fn(vertex, next) per GFaceVertex, next wrapping to the first. False when fn
// stops or the loop passes face_loop_max.
template<typename F>
bool for_each_face_vertex(uintptr_t face, F&& fn)
{
    const auto head = reinterpret_cast<uintptr_t>(reinterpret_cast<const GFace*>(face)->edge_loop);
    int steps = 0;
    for (uintptr_t v = head; v;) {
        const auto next = reinterpret_cast<uintptr_t>(reinterpret_cast<const GFaceVertex*>(v)->next);
        if (++steps > face_loop_max || !fn(v, next && next != head ? next : head)) {
            return false;
        }
        if (next == head) {
            break;
        }
        v = next;
    }
    return true;
}

constexpr double blend_cull_cell = 0.01;
// FUN_004aae80's entry array stride.
constexpr uintptr_t blend_entry_stride = sizeof(LightmapBlendEntry);

class BlendPairCull
{
public:
    // False leaves the whole pass to the stock scan: a non-finite vertex matches anything there.
    bool build(uintptr_t entries, int count)
    {
        try {
            count_ = count;
            for (int e = 0; e < count; e++) {
                const auto& faces = reinterpret_cast<const LightmapBlendEntry*>(entries)[e].faces;
                const int n = faces.size;
                GFace* const* data = faces.data_ptr;
                for (int i = 0; i < n; i++) {
                    const auto face = reinterpret_cast<uintptr_t>(data[i]);
                    face_entry_.emplace(face, e);
                    if (!for_each_vertex(face, [&](const float* p) {
                            if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) {
                                return false;
                            }
                            auto& list = cells_[cell_of(p)];
                            if (list.empty() || list.back() != face) {
                                list.push_back(face);
                            }
                            return true;
                        })) {
                        return false;
                    }
                }
            }
            return true;
        }
        catch (...) {
            return false;
        }
    }

    // The first entry at or after `e` that can hold a candidate, `last` when none can; `e` itself when the pass
    // is left to stock for this face.
    int next_candidate_entry(uintptr_t fa, int e, int last)
    {
        if (!select(fa)) {
            return e;
        }
        const auto end = candidate_entries_.begin() + std::min(last, count_);
        const auto it = std::find(candidate_entries_.begin() + std::min(e, count_), end, char{1});
        return it == end ? last : static_cast<int>(it - candidate_entries_.begin());
    }

    bool face_is_candidate(uintptr_t fa, uintptr_t fb)
    {
        if (!select(fa)) {
            return true;
        }
        return std::find(candidate_faces_.begin(), candidate_faces_.end(), fb) != candidate_faces_.end();
    }

private:
    struct Cell
    {
        std::int64_t x, y, z;
        bool operator==(const Cell&) const = default;
    };
    struct CellHash
    {
        std::size_t operator()(const Cell& c) const
        {
            std::uint64_t h = static_cast<std::uint64_t>(c.x) * 0x9E3779B97F4A7C15ull;
            h ^= static_cast<std::uint64_t>(c.y) * 0xC2B2AE3D27D4EB4Full + (h << 6) + (h >> 2);
            h ^= static_cast<std::uint64_t>(c.z) * 0x165667B19E3779F9ull + (h << 6) + (h >> 2);
            return static_cast<std::size_t>(h ^ (h >> 32));
        }
    };

    static Cell cell_of(const float* p)
    {
        return {static_cast<std::int64_t>(std::floor(static_cast<double>(p[0]) / blend_cull_cell)),
                static_cast<std::int64_t>(std::floor(static_cast<double>(p[1]) / blend_cull_cell)),
                static_cast<std::int64_t>(std::floor(static_cast<double>(p[2]) / blend_cull_cell))};
    }

    // Each GFaceVertex's GVertex position; false stops.
    template<typename F>
    static bool for_each_vertex(uintptr_t face, F&& fn)
    {
        return for_each_face_vertex(
            face, [&](uintptr_t v, uintptr_t) { return fn(&reinterpret_cast<const GFaceVertex*>(v)->vertex->pos.x); });
    }

    bool select(uintptr_t fa)
    {
        if (fa == selected_) {
            return selected_ok_;
        }
        selected_ = fa;
        selected_ok_ = false;
        try {
            candidate_faces_.clear();
            candidate_entries_.assign(static_cast<std::size_t>(count_), 0);
            const bool complete = for_each_vertex(fa, [&](const float* p) {
                const Cell c = cell_of(p);
                for (std::int64_t dz = -1; dz <= 1; dz++) {
                    for (std::int64_t dy = -1; dy <= 1; dy++) {
                        for (std::int64_t dx = -1; dx <= 1; dx++) {
                            const auto it = cells_.find({c.x + dx, c.y + dy, c.z + dz});
                            if (it == cells_.end()) {
                                continue;
                            }
                            for (const uintptr_t face : it->second) {
                                if (std::find(candidate_faces_.begin(), candidate_faces_.end(), face) ==
                                    candidate_faces_.end()) {
                                    candidate_faces_.push_back(face);
                                    const auto [first, last] = face_entry_.equal_range(face);
                                    for (auto e = first; e != last; ++e) {
                                        candidate_entries_[static_cast<std::size_t>(e->second)] = 1;
                                    }
                                }
                            }
                        }
                    }
                }
                return true;
            });
            selected_ok_ = complete;
        }
        catch (...) {
        }
        return selected_ok_;
    }

    int count_ = 0;
    std::unordered_map<Cell, std::vector<uintptr_t>, CellHash> cells_;
    std::unordered_multimap<uintptr_t, int> face_entry_;
    uintptr_t selected_ = 0;
    bool selected_ok_ = false;
    std::vector<uintptr_t> candidate_faces_;
    std::vector<char> candidate_entries_;
};

// Scoped by lightmap_blend_pass_new to one FUN_004aae80 call; null when the pass is left to stock.
BlendPairCull* g_blend_cull = nullptr;

// The current A face: [ESP+0x10] is A's face VArray, [ESP+0x2c] the face index.
uintptr_t blend_current_face(uintptr_t esp)
{
    const auto* faces = *reinterpret_cast<const VArray<GFace*>* const*>(esp + 0x10);
    const int index = *reinterpret_cast<int*>(esp + 0x2c);
    return reinterpret_cast<uintptr_t>(faces->data_ptr[index]);
}

} // namespace

// Replaces "MOV EAX,[EBX-4]; MOV ECX,[EDI]" (5 bytes) at the head of the B-entry loop, which is also its
// back-edge target. EDI and [ESP+0x1c] are the entry, [ESP+0x24] the entries left including it, [ESP+0xc8] the
// array. A run of entries without candidates is stepped over at once, as 0x004ab07c (where the stock room check
// sends them) would one at a time; when none are left the loop exits to 0x004ab092 as its JNZ would.
CodeInjection lightmap_blend_entry_cull_injection{
    0x004aaf05,
    [](auto& regs) {
        const uintptr_t esp = static_cast<uintptr_t>(regs.esp);
        uintptr_t entry = static_cast<uintptr_t>(regs.edi);
        if (g_blend_cull) {
            const uintptr_t base = *reinterpret_cast<uintptr_t*>(esp + 0xc8);
            int& left = *reinterpret_cast<int*>(esp + 0x24);
            const int at = static_cast<int>((entry - base) / blend_entry_stride);
            const int next = g_blend_cull->next_candidate_entry(blend_current_face(esp), at, at + left);
            if (next != at) {
                left -= next - at;
                entry = base + static_cast<uintptr_t>(next) * blend_entry_stride;
                regs.edi = entry;
                *reinterpret_cast<uintptr_t*>(esp + 0x1c) = entry;
                if (left == 0) {
                    regs.eax = 0;
                    regs.eip = 0x004ab092;
                    return;
                }
            }
        }
        regs.eax = *reinterpret_cast<uintptr_t*>(static_cast<uintptr_t>(regs.ebx) - 4);
        regs.ecx = *reinterpret_cast<uintptr_t*>(entry);
        regs.eip = 0x004aaf0a;
    },
    false, // no trampoline: the injection replaces the two loads
};

// Replaces "MOV EAX,[EAX]; XOR EBX,EBX; MOV [ESP+0x40],EAX" (8 bytes) after B's face fetch;
// 0x004ab05d moves to the next face.
CodeInjection lightmap_blend_face_cull_injection{
    0x004aaf78,
    [](auto& regs) {
        const uintptr_t esp = static_cast<uintptr_t>(regs.esp);
        const uintptr_t face = *reinterpret_cast<uintptr_t*>(static_cast<uintptr_t>(regs.eax));
        regs.eax = face;
        regs.ebx = 0;
        *reinterpret_cast<uintptr_t*>(esp + 0x40) = face;
        regs.eip = 0x004aaf80;
        if (g_blend_cull && !g_blend_cull->face_is_candidate(blend_current_face(esp), face)) {
            regs.eip = 0x004ab05d;
        }
    },
    false, // no trampoline: the injection replaces the three instructions
};

static void __cdecl lightmap_blend_surfaces_new(void** a, void** b, void* p3, void* p4, void* p5);
static FunHook<void __cdecl(void**, void**, void*, void*, void*)> lightmap_blend_surfaces_hook{
    0x004ab0d0, lightmap_blend_surfaces_new};

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

static constexpr float lm_blend_own = 9.0f / 16.0f;
static constexpr float lm_blend_other = 7.0f / 16.0f;
static constexpr float lm_blend_samples_per_texel = 128.0f;
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
// before the edge, so the snapshot has to be the page as it stands.
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
        try {
            const auto* p_0 = *reinterpret_cast<const float**>(p3);
            const auto* p_1 = *reinterpret_cast<const float**>(p5);
            if (!lightmap_blend_edge_is_new(surf_a, surf_b, p_0, p_1)) {
                return;
            }
            if (lightmap_blend_edge(reinterpret_cast<uintptr_t>(*a),
                                    reinterpret_cast<uintptr_t>(*b), p_0, p_1)) {
                alpine_lm_blend_edge(static_cast<const GSurface*>(*a), static_cast<const GSurface*>(*b), p_0, p_1);
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
    if (bake_progress_cancelled()) {
        return;
    }
    // the level's blend and second pass are phases of their own; a mover's count as the movers phase
    const bool level_pass = bake_progress_current() == BakePhase::surfaces;
    if (level_pass) {
        std::uint64_t faces = 0;
        for (int e = 0; e < count; e++) {
            faces += reinterpret_cast<const LightmapBlendEntry*>(entries)[e].faces.size;
        }
        bake_progress_phase(BakePhase::blend, faces);
    }
    g_blend_edges.clear();
    blend_sides_clear();
    {
        BlendPairCull cull;
        ScopeGuard reset{[] { g_blend_cull = nullptr; }};
        g_blend_cull = cull.build(reinterpret_cast<uintptr_t>(entries), count) ? &cull : nullptr;
        lightmap_blend_pass_hook.call_target(entries, count);
    }
    g_blend_edges.clear();
    blend_sides_clear();
    if (level_pass) {
        bake_progress_phase(BakePhase::smoothing, static_cast<std::uint64_t>(std::max(count, 0)));
    }
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

static constexpr int lm_max_face_verts = 4096;
static uintptr_t g_face_vert_nodes[lm_max_face_verts];

CodeInjection lightmap_blend_face_verts_injection{
    0x004aaecb,
    [](auto& regs) {
        if (bake_progress_current() == BakePhase::blend) {
            bake_progress_step();
        }
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

// Alpine lightmaps: terrain texels
// The light model FUN_004ac470 gives a surface texel of the static solid, applied to arbitrary points:
// the ambient seed (the level ambient, or the custom ambient room containing the point, halved by
// red_const_half), the lights room_setup_bbox gathers for the points' bounding box from the global
// list, each shadow casting light (GrLight::shadow_condition != 0) masked by the ray tracer when the
// command casts shadows, and light_accum_at_texel with the point's own normal and a smooth receiver,
// clamped at zero.

bool lightmap_light_terrain_points(const LightmapPoint* points, int count, float lift, float texel_size,
                                   float* out_r, float* out_g, float* out_b)
{
    auto* level = CDedLevel::Get();
    if (!g_bake_active || !level || !level->solid || !points || count <= 0) {
        return false;
    }
    const auto solid = reinterpret_cast<uintptr_t>(level->solid);

    Vector3 lo{1e30f, 1e30f, 1e30f};
    Vector3 hi{-1e30f, -1e30f, -1e30f};
    for (int i = 0; i < count; i++) {
        const float* p = points[i].pos;
        lo = {std::min(lo.x, p[0]), std::min(lo.y, p[1]), std::min(lo.z, p[2])};
        hi = {std::max(hi.x, p[0]), std::max(hi.y, p[1]), std::max(hi.z, p[2])};
    }
    const float pad = std::max(lift, lm_ray_lift);
    lo = {lo.x - pad, lo.y - pad, lo.z - pad};
    hi = {hi.x + pad, hi.y + pad, hi.z + pad};

    float base[3] = {0.0f, 0.0f, 0.0f};
    light_get_ambient(&base[0], &base[1], &base[2]);
    const float half = red_const_half;
    const bool per_room = bake_fixes_active() && s_ambient_room_count > 0;
    for (int i = 0; i < count; i++) {
        float a[3] = {base[0], base[1], base[2]};
        if (per_room) {
            get_ambient_at(points[i].pos[0], points[i].pos[1], points[i].pos[2], base[0], base[1], base[2],
                           a[0], a[1], a[2]);
        }
        out_r[i] = a[0] * half;
        out_g[i] = a[1] * half;
        out_b[i] = a[2] * half;
    }

    const int lights = std::clamp(room_setup_bbox(nullptr, &lo, &hi, 0, 1), 0, max_scene_lights);
    ScopeGuard light_list_scope{[] { room_cleanup(); }};
    room_lights_to_local();
    if (lights == 0) {
        return true;
    }

    const OccluderTree* tree = g_bake_mode != 0 ? lightmap_occluder_tree(solid) : nullptr;
    std::vector<std::uint8_t> lit_mask(static_cast<std::size_t>(count), 0xffu);
    std::vector<LightRays> rays;
    std::vector<int> ray_light;
    std::vector<const alpine_dir_light::Volume*> ray_cull;
    std::vector<void*> masks(static_cast<std::size_t>(lights), lit_mask.data());
    for (int li = 0; li < lights; li++) {
        const auto light = reinterpret_cast<uintptr_t>(face_light_list[li]);
        LightRays lr;
        if (tree && light && reinterpret_cast<const GrLight*>(light)->shadow_condition != 0 &&
            light_rays_setup(light, lr)) {
            rays.push_back(lr);
            ray_light.push_back(li);
            const BakeDirLight* dl = bake_dir_light(reinterpret_cast<const void*>(light));
            ray_cull.push_back(dl && dl->bounded() ? &dl->volume : nullptr);
        }
    }
    // the accumulator weighs each point at its own position: outside the volume it is 0 exactly
    const ShadeFrame frame = ShadeFrame::current();
    std::vector<std::uint8_t> ray_masks(rays.size() * static_cast<std::size_t>(count));
    for (std::size_t k = 0; k < rays.size(); k++) {
        masks[ray_light[k]] = ray_masks.data() + k * count;
    }

    // Every (light, point) pair is independent and each writes its own byte, so any split of the
    // work gives the same masks.
    const std::size_t work = ray_masks.size();
    auto shade = [&](std::size_t begin, std::size_t end) {
        for (std::size_t w = begin; w < end; w++) {
            const std::size_t k = w / count;
            const LightmapPoint& pt = points[w % count];
            if (ray_cull[k]
                && alpine_dir_light::volume_inside_distance(
                       *ray_cull[k], frame.world_point(pt.pos[0], pt.pos[1], pt.pos[2]))
                       < -lm_ray_lift) {
                ray_masks[w] = 0;
                continue;
            }
            ray_masks[w] = light_rays_visibility(*tree, rays[k], {pt.pos[0], pt.pos[1], pt.pos[2]},
                                                 {pt.normal[0], pt.normal[1], pt.normal[2]}, lift,
                                                 std::numeric_limits<int>::min(), texel_size);
        }
    };
    constexpr std::size_t items_per_task = 256;
    work_pool_run(static_cast<int>((work + items_per_task - 1) / items_per_task), [&](int task) {
        const std::size_t begin = static_cast<std::size_t>(task) * items_per_task;
        shade(begin, std::min(work, begin + items_per_task));
    });

    static const std::uint8_t smooth_receiver = 1;
    for (int i = 0; i < count; i++) {
        const Vector3 pos{points[i].pos[0], points[i].pos[1], points[i].pos[2]};
        const Vector3 normal{points[i].normal[0], points[i].normal[1], points[i].normal[2]};
        light_accum_at_texel(&out_r[i], &out_g[i], &out_b[i], &pos, &normal, masks.data(), i, &smooth_receiver);
        out_r[i] = std::max(out_r[i], 0.0f);
        out_g[i] = std::max(out_g[i], 0.0f);
        out_b[i] = std::max(out_b[i], 0.0f);
    }
    return true;
}

// The terrain bake runs after the movers, whose passes leave their own rooms in the ambient table.
void lightmap_prepare_terrain_bake()
{
    auto* level = CDedLevel::Get();
    lightmap_collect_room_ambient(level ? reinterpret_cast<uintptr_t>(level->solid) : 0);
}

// Alpine lightmaps: per-surface driver
// FUN_004ac470 shades one surface into its lightmap page. After the stock pass has produced the
// stock fragment, the alpine writer re-runs this same function once per chart tile against a tile
// view of the surface, then box-averages the result back into the stock fragment's interior.
static void __fastcall lightmap_shade_surface_new(GSurface* surface, int edx, void* solid, int mode);
static FunHook<void __fastcall(GSurface*, int, void*, int)> lightmap_shade_surface_hook{
    0x004ac470, lightmap_shade_surface_new};

static void __fastcall lightmap_shade_surface_new(GSurface* surface, int edx, void* solid, int mode)
{
    // a cancelled bake is discarded, so nothing more is shaded
    if (bake_progress_cancelled()) {
        return;
    }
    if (alpine_lm_tile_pass_active()) {
        lightmap_shade_surface_hook.call_target(surface, edx, solid, mode);
        return;
    }
    // Every write this function makes is addressed as (ystart + row) * page_w + xstart + col, with
    // no bound of its own. The bake is refused up front when a rect does not fit its page; this
    // is the backstop.
    const GLightmap* lm = surface->lightmap;
    if (lm) {
        const int page_w = lm->w;
        const int page_h = lm->h;
        const int x = surface->xstart;
        const int y = surface->ystart;
        const int w = surface->width;
        const int h = surface->height;
        if (x < 0 || y < 0 || w < 0 || h < 0 || x + w > page_w || y + h > page_h) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                xlog::warn("Lightmap: a {}x{} surface at {},{} does not fit its {}x{} page, "
                           "skipping it - the level needs Build Geometry",
                           w, h, x, y, page_w, page_h);
            }
            surface->flags = 0;
            return;
        }
    }
    // both halves of the engine's own gate at 0x004ac48c: shade only for SURFACE_SHADE or
    // SURFACE_SHADE_RUNTIME, and never when fullbright is set, or the tile pass would produce a black
    // chart and downsample it back
    const std::uint8_t state = surface->flags;
    const std::uint8_t fullbright = surface->fullbright;
    lightmap_shade_surface_hook.call_target(surface, edx, solid, mode);
    if ((state & (SURFACE_SHADE | SURFACE_SHADE_RUNTIME)) && !fullbright) {
        alpine_lm_shade_surface(static_cast<GSolid*>(solid), surface, mode);
    }
    // FUN_004aabf0's second pass (state 8) re-shades a mover's smoothed surfaces, which its count already holds
    if (state != 8 || bake_progress_current() != BakePhase::movers) {
        bake_progress_step();
    }
}

// Replaces "TEST byte ptr [ESI+8],1; JZ 0x004accda" (10 bytes) after the lightmap pass. Both blocks
// behind it write the surface's live preview texture through the lightmap's bitmap handle, sized
// for the real page; a tile view has neither, so SURFACE_DYNAMIC_LIGHTS and SURFACE_UPLOAD are
// cleared for a tile pass and both blocks fall away. Branches into this filter land on the address
// itself, never inside it.
CodeInjection lightmap_preview_upload_injection{
    0x004aca49,
    [](auto& regs) {
        auto* state = &reinterpret_cast<GSurface*>(static_cast<uintptr_t>(regs.esi))->flags;
        if (alpine_lm_tile_pass_active()) {
            *state &= static_cast<std::uint8_t>(~(SURFACE_DYNAMIC_LIGHTS | SURFACE_UPLOAD));
        }
        regs.eip = (*state & SURFACE_DYNAMIC_LIGHTS) ? 0x004aca53 : 0x004accda;
    },
    false, // no trampoline: the injection fully replaces the test and its branch
};

// Cross-room surface merging
// A portal brush splitting a face puts the fragments in different rooms, and the stock surface
// group flood fill (FUN_004aa610) treats the room pointer as a hard boundary, so the fragments get
// independent lightmaps and a visible seam. The six sites below are always-installed injections
// that merge across the boundary on the fixed pipeline and reproduce the stock branch exactly
// whenever bake_fixes_active() is false.

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
        regs.eip = (same_room || bake_fixes_active()) ? 0x004aa809 : 0x004aa835;
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
            bake_fixes_active() ? 1u : 0u;
        regs.ebx = 0;
        regs.eip = 0x004a9da3;
    },
    false, // no trampoline: the injection fully replaces the 6 byte block
};

// Merged surfaces span rooms, so the per-room light and face lists no longer describe them. These
// sites each branch on surface->room_index == -1 to pick the global list instead; the branch is
// forced when merging is on.

// FUN_004ac470 shadow-pass gather: "MOV ECX,[ESI+0x68]; XOR EAX,EAX" (5 bytes).
CodeInjection lightmap_global_lights_shadow_injection{
    0x004ac4a9,
    [](auto& regs) {
        regs.ecx = *reinterpret_cast<int*>(static_cast<uintptr_t>(regs.esi) + 0x68);
        regs.eax = 0;
        regs.eip = bake_fixes_active() ? 0x004ac4c1 : 0x004ac4ae;
    },
    false, // no trampoline: the injection fully replaces the 5 byte block
};

// FUN_004ac470 vertex-lighting gather: "MOV EAX,[ESI+0x68]; XOR EBP,EBP" (5 bytes).
CodeInjection lightmap_global_lights_vertex_injection{
    0x004aca53,
    [](auto& regs) {
        regs.eax = *reinterpret_cast<int*>(static_cast<uintptr_t>(regs.esi) + 0x68);
        regs.ebp = 0;
        regs.eip = bake_fixes_active() ? 0x004aca6f : 0x004aca58;
    },
    false, // no trampoline: the injection fully replaces the 5 byte block
};

// FUN_004ae360 occluder face list: "MOV dword ptr [ESP+0x8f8],0" (a single 11 byte store of the
// SEH state index; EAX is already loaded by the preceding instruction).
CodeInjection lightmap_global_faces_shadow_injection{
    0x004ae6fc,
    [](auto& regs) {
        *reinterpret_cast<std::uint32_t*>(static_cast<uintptr_t>(regs.esp) + 0x8f8) = 0;
        regs.eip = bake_fixes_active() ? 0x004ae803 : 0x004ae707;
    },
    false, // no trampoline: the injection fully replaces the 11 byte store
};

// FUN_004ad160 lumel face list: "MOV dword ptr [ESP+0x28c],EBX" (a single 7 byte store, EBX = 0;
// EAX = surface->room_index). The global list is the solid's faces carrying this surface's index in
// list order, which the bake's face index already holds, so it fills the stack VArray at [ESP+0x5c]
// from that and skips the walk (0x004ad262-0x004ad297), leaving EDI and EBP as the walk does.
CodeInjection lightmap_global_faces_lumel_injection{
    0x004ad20f,
    [](auto& regs) {
        const uintptr_t esp = static_cast<uintptr_t>(regs.esp);
        *reinterpret_cast<std::uint32_t*>(esp + 0x28c) = static_cast<std::uint32_t>(static_cast<int>(regs.ebx));
        if (!bake_fixes_active() && static_cast<int>(regs.eax) != -1) {
            regs.eip = 0x004ad216;
            return;
        }
        auto* solid = *reinterpret_cast<GSolid**>(esp + 0x294);
        const int surf_id = reinterpret_cast<const GSurface*>(static_cast<uintptr_t>(regs.esi))->index;
        const SolidCache* cache = surf_id >= 0 ? lightmap_solid_cache(reinterpret_cast<uintptr_t>(solid)) : nullptr;
        if (!cache || !cache->complete) {
            regs.eip = 0x004ad262;
            return;
        }
        if (auto faces = cache->faces_by_surface.find(surf_id); faces != cache->faces_by_surface.end()) {
            auto* list = reinterpret_cast<VArray<GFace*>*>(esp + 0x5c);
            for (uintptr_t face : faces->second) {
                list->push_back(reinterpret_cast<GFace*>(face));
            }
        }
        regs.edi = 0;
        regs.ebp = reinterpret_cast<uintptr_t>(&solid->face_list_head);
        regs.eip = 0x004ad299;
    },
    false, // no trampoline: the injection fully replaces the 7 byte store
};

// Lumel edge-crossing cull: FUN_004ad160 tests only the edges near each lumel square.
namespace
{

constexpr float lumel_cull_pad = 1.0e-4f;

class LumelEdgeGrid
{
public:
    // The face list is the stack VArray at [ESP+0x5c] of FUN_004ad160; false leaves the stock scan
    // in charge for the rest of the call.
    bool ready(uintptr_t faces)
    {
        if (state_ == State::unbuilt) {
            bool built = false;
            try {
                built = build(faces);
            }
            catch (...) {
            }
            state_ = built ? State::ready : State::failed;
            if (!built) {
                edges_ = {};
                cell_start_ = {};
                cell_edges_ = {};
                stamp_ = {};
            }
        }
        return state_ == State::ready;
    }

    bool any_crossing(const float* p0, const float* p1, const float* p2, const float* p3)
    {
        const float qx0 = std::min({p0[0], p1[0], p2[0], p3[0]}) - lumel_cull_pad;
        const float qx1 = std::max({p0[0], p1[0], p2[0], p3[0]}) + lumel_cull_pad;
        const float qy0 = std::min({p0[1], p1[1], p2[1], p3[1]}) - lumel_cull_pad;
        const float qy1 = std::max({p0[1], p1[1], p2[1], p3[1]}) + lumel_cull_pad;
        const int cx0 = std::max(0, cell(qx0, x0_, sx_));
        const int cx1 = std::min(grid_ - 1, cell(qx1, x0_, sx_));
        const int cy0 = std::max(0, cell(qy0, y0_, sy_));
        const int cy1 = std::min(grid_ - 1, cell(qy1, y0_, sy_));
        if (cx0 > cx1 || cy0 > cy1) {
            return false;
        }
        if (++stamp_value_ == 0) {
            std::fill(stamp_.begin(), stamp_.end(), 0u);
            stamp_value_ = 1;
        }
        for (int cy = cy0; cy <= cy1; cy++) {
            for (int cx = cx0; cx <= cx1; cx++) {
                const std::size_t c = static_cast<std::size_t>(cy) * grid_ + cx;
                for (std::size_t k = cell_start_[c]; k < cell_start_[c + 1]; k++) {
                    const int i = cell_edges_[k];
                    if (stamp_[i] == stamp_value_) {
                        continue;
                    }
                    stamp_[i] = stamp_value_;
                    const Edge& e = edges_[i];
                    if (std::max(e.ax, e.bx) < qx0 || std::min(e.ax, e.bx) > qx1 || std::max(e.ay, e.by) < qy0 ||
                        std::min(e.ay, e.by) > qy1) {
                        continue;
                    }
                    if (hits(p0, p1, p2, p3, e)) {
                        return true;
                    }
                }
            }
        }
        return false;
    }

private:
    struct Edge
    {
        float ax, ay, bx, by;
    };
    enum class State { unbuilt, ready, failed };

    // the four segments FUN_004ad160 tests, with the same arguments
    static bool hits(const float* p0, const float* p1, const float* p2, const float* p3, const Edge& e)
    {
        return segments_intersect_2d(p0, p1, e.ax, e.ay, e.bx, e.by) ||
               segments_intersect_2d(p2, p3, e.ax, e.ay, e.bx, e.by) ||
               segments_intersect_2d(p0, p2, e.ax, e.ay, e.bx, e.by) ||
               segments_intersect_2d(p1, p3, e.ax, e.ay, e.bx, e.by);
    }

    static int cell(float v, float origin, float size)
    {
        const float c = std::floor((v - origin) / size);
        if (!(c > -1.0e6f)) {
            return -1000000;
        }
        return c < 1.0e6f ? static_cast<int>(c) : 1000000;
    }

    // Edges as FUN_004ad160 walks them, from every vertex to the next, in lightmap UVs; false when an
    // edge loop is too long to walk.
    bool build(uintptr_t faces)
    {
        const auto& list = *reinterpret_cast<const VArray<GFace*>*>(faces);
        const int count = list.size;
        GFace* const* data = list.data_ptr;
        for (int i = 0; i < count; i++) {
            const auto face = reinterpret_cast<uintptr_t>(data[i]);
            const bool walked = for_each_face_vertex(face, [&](uintptr_t v, uintptr_t w) {
                const auto* a = reinterpret_cast<const GFaceVertex*>(v);
                const auto* b = reinterpret_cast<const GFaceVertex*>(w);
                const Edge e{a->lm_u, a->lm_v, b->lm_u, b->lm_v};
                // FUN_004c9ef0 never reports an edge with a non-finite coordinate
                if (std::isfinite(e.ax) && std::isfinite(e.ay) && std::isfinite(e.bx) && std::isfinite(e.by)) {
                    edges_.push_back(e);
                }
                return true;
            });
            if (!walked) {
                return false;
            }
        }
        float x0 = std::numeric_limits<float>::max();
        float y0 = std::numeric_limits<float>::max();
        float x1 = std::numeric_limits<float>::lowest();
        float y1 = std::numeric_limits<float>::lowest();
        for (const Edge& e : edges_) {
            x0 = std::min({x0, e.ax, e.bx});
            y0 = std::min({y0, e.ay, e.by});
            x1 = std::max({x1, e.ax, e.bx});
            y1 = std::max({y1, e.ay, e.by});
        }
        const int n = static_cast<int>(edges_.size());
        grid_ = std::clamp(static_cast<int>(std::sqrt(static_cast<float>(n)) * 2.0f), 1, 128);
        x0_ = x0 - lumel_cull_pad;
        y0_ = y0 - lumel_cull_pad;
        sx_ = std::max((x1 - x0 + 2.0f * lumel_cull_pad) / static_cast<float>(grid_), 1.0e-12f);
        sy_ = std::max((y1 - y0 + 2.0f * lumel_cull_pad) / static_cast<float>(grid_), 1.0e-12f);
        const std::size_t cells = static_cast<std::size_t>(grid_) * grid_;
        std::vector<int> ranges(static_cast<std::size_t>(n) * 4);
        cell_start_.assign(cells + 1, 0);
        for (int i = 0; i < n; i++) {
            const Edge& e = edges_[i];
            int* r = &ranges[static_cast<std::size_t>(i) * 4];
            r[0] = std::clamp(cell(std::min(e.ax, e.bx) - lumel_cull_pad, x0_, sx_), 0, grid_ - 1);
            r[1] = std::clamp(cell(std::max(e.ax, e.bx) + lumel_cull_pad, x0_, sx_), 0, grid_ - 1);
            r[2] = std::clamp(cell(std::min(e.ay, e.by) - lumel_cull_pad, y0_, sy_), 0, grid_ - 1);
            r[3] = std::clamp(cell(std::max(e.ay, e.by) + lumel_cull_pad, y0_, sy_), 0, grid_ - 1);
            for (int cy = r[2]; cy <= r[3]; cy++) {
                for (int cx = r[0]; cx <= r[1]; cx++) {
                    cell_start_[static_cast<std::size_t>(cy) * grid_ + cx + 1]++;
                }
            }
        }
        for (std::size_t c = 0; c < cells; c++) {
            cell_start_[c + 1] += cell_start_[c];
        }
        cell_edges_.resize(cell_start_[cells]);
        std::vector<std::size_t> fill(cell_start_.begin(), cell_start_.end() - 1);
        for (int i = 0; i < n; i++) {
            const int* r = &ranges[static_cast<std::size_t>(i) * 4];
            for (int cy = r[2]; cy <= r[3]; cy++) {
                for (int cx = r[0]; cx <= r[1]; cx++) {
                    cell_edges_[fill[static_cast<std::size_t>(cy) * grid_ + cx]++] = i;
                }
            }
        }
        stamp_.assign(edges_.size(), 0u);
        return true;
    }

    State state_ = State::unbuilt;
    std::vector<Edge> edges_;
    std::vector<std::size_t> cell_start_;
    std::vector<int> cell_edges_;
    std::vector<unsigned> stamp_;
    unsigned stamp_value_ = 0;
    int grid_ = 1;
    float x0_ = 0.0f;
    float y0_ = 0.0f;
    float sx_ = 1.0f;
    float sy_ = 1.0f;
};

// Scoped by lightmap_lumel_pass_new to one FUN_004ad160 call.
LumelEdgeGrid* g_lumel_grid = nullptr;

} // namespace

static void __cdecl lightmap_lumel_pass_new(void* solid, void* surface, int flag, void* masks);
static FunHook<void __cdecl(void*, void*, int, void*)> lightmap_lumel_pass_hook{0x004ad160, lightmap_lumel_pass_new};

static void __cdecl lightmap_lumel_pass_new(void* solid, void* surface, int flag, void* masks)
{
    LumelEdgeGrid grid;
    ScopeGuard restore{[outer = std::exchange(g_lumel_grid, &grid)] { g_lumel_grid = outer; }};
    lightmap_lumel_pass_hook.call_target(solid, surface, flag, masks);
}

// Replaces "XOR EDI,EDI; LEA ECX,[ESP+0x5c]" (6 bytes) at the head of the per-texel crossing scan. What
// the scan leaves at 0x004ad4d6: EBP > 0 on a crossing, and its face index [ESP+0x3c] at the face count.
CodeInjection lightmap_lumel_cull_injection{
    0x004ad3b9,
    [](auto& regs) {
        const uintptr_t esp = static_cast<uintptr_t>(regs.esp);
        const uintptr_t faces = esp + 0x5c;
        LumelEdgeGrid* grid = g_lumel_grid;
        if (!grid || !grid->ready(faces)) {
            regs.edi = 0;
            regs.ecx = faces;
            regs.eip = 0x004ad3bf;
            return;
        }
        const auto* p0 = reinterpret_cast<const float*>(esp + 0xc8);
        const auto* p1 = reinterpret_cast<const float*>(esp + 0xd0);
        const auto* p2 = reinterpret_cast<const float*>(esp + 0xd8);
        const auto* p3 = reinterpret_cast<const float*>(esp + 0xc0);
        const bool hit = grid->any_crossing(p0, p1, p2, p3);
        *reinterpret_cast<int*>(esp + 0x3c) = reinterpret_cast<const VArray<GFace*>*>(faces)->size;
        regs.ebp = hit ? 1 : 0;
        regs.eip = 0x004ad4d6;
    },
    false, // no trampoline: the injection replaces the 6 bytes it covers
};

void ApplyLightmapPatches()
{
    // Replace light handle-to-pointer with bounds-checked version
    light_handle_to_pointer_injection.install();

    // Fix pink lightmaps when a face is affected by >= 64 lights: replace the limit check, and
    // allocate each surface's shadow masks on the heap
    lightmap_light_limit_injection.install();

    // Lightmap pages are saved whole, so their inter-fragment gaps must not be heap garbage
    lightmap_page_clear_injection.install();
    lightmap_synth_page_hook.install();

    // Release a freed lightmap page's D3D texture
    lightmap_page_free_hook.install();

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

    // Alpine directional lights (the sun and Directional Light objects): temporary type 1 lights in both
    // bake commands. Inert unless the level has any.
    lighting_calc_shadows_hook.install();
    lighting_calc_no_shadows_hook.install();
    lighting_calc_shadows_after_surfaces_hook.install();
    lighting_calc_no_shadows_after_surfaces_hook.install();
    shadow_mask_hook.install();
    dir_light_face_light_dedup_injection.install();
    dir_light_sky_occluder_skip_injection.install();
    light_accum_at_texel_hook.install();
    light_accum_smooth_lumel_hook.install();

    // Lightmap bake accuracy fixes, all inert when the level sets Legacy lighting
    lightmap_texel_convert_injection.install();
    lightmap_smooth_grey_01_injection.install();
    lightmap_smooth_grey_033_injection.install();
    lightmap_smoothing_normal_weight_injection.install();
    lightmap_border_duplicate_skip_injection.install();
    lightmap_alpha_texture_occluder_injection.install();

    // Stock shadow projector fix, Legacy lighting included
    shadow_clip_area_count_injection.install();

    // Smooth path rows whose edge crossings coincide, Legacy lighting included: the 1.0 operand of the FLD
    // at 0x004ad81e, which light_accum_smooth_lumel_hook resolves
    write_mem_ptr(0x004ad820, &lightmap_smooth_degenerate_row_marker);

    // Alpine lightmaps: the per-surface driver and the preview-upload gate it needs
    lightmap_shade_surface_hook.install();
    lightmap_preview_upload_injection.install();

    // Lumel edge-crossing cull and blend-pass pair cull
    lightmap_lumel_pass_hook.install();
    lightmap_lumel_cull_injection.install();
    lightmap_blend_entry_cull_injection.install();
    lightmap_blend_face_cull_injection.install();

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

    // Cross-room surface merging, inert when the level sets Legacy lighting
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
