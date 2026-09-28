#include <array>
#include <vector>
#include <deque>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <set>
#include <unordered_set>
#include <unordered_map>
#include <limits>
#include <string>
#include <patch_common/FunHook.h>
#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/AsmWriter.h>
#include <xlog/xlog.h>
#include "../misc/alpine_options.h"
#include "../misc/alpine_settings.h"
#include "../graphics/weather.h"
#include "../graphics/d3d11/gr_d3d11_hooks.h"
#include "../multi/multi.h"
#include "../multi/server_internal.h"
#include "../main/main.h"
#include "../misc/misc.h"
#include "../rf/geometry.h"
#include "../rf/event.h"
#include "../rf/level.h"
#include "../rf/particle_emitter.h"
#include "../rf/bmpman.h"
#include "../rf/sound/sound.h"
#include "../rf/weapon.h"
#include "../rf/object.h"
#include "../rf/entity.h"
#include "../rf/player/player.h"
#include "../rf/player/camera.h"
#include "../os/console.h"
#include "../fflink/afstats_events.h"
#include "../fflink/fflink_session.h"
#include "destruction.h"
#include "level.h"
#include "alpine_terrain.h"
#include "alpine_terrain_decorations.h"
#include "../sound/sound_foley.h"

// Set by geomod_init hook; checked by boolean engine injections.
static bool g_rf2_style_boolean_active = false;

// RF2-style geomod limit — separate from the normal multiplayer geomod limit.
// -1 = unlimited (default), 0 = disabled, >0 = specific limit.
static int g_rf2_geo_limit = -1;
static int g_rf2_geo_count = 0;

// Tracks whether the boolean engine actually modified any detail room faces.
// Set in boolean State 5 when a detail room appears in the affected rooms list.
// Cleared at the start of each geomod (in geomod_init_hook).
// Used to suppress geomod effects when the crater didn't intersect any geometry.
static bool g_rf2_boolean_modified_detail = false;

// When true, suppresses geomod_create's visual effects via CallHooks so they
// can be deferred until carving is confirmed.
static bool g_rf2_suppress_geomod_create_effects = false;

// Rock debris saved params — single global, replayed directly in State 3.
// If two RF2 geomods overlap on the same frame, second overwrites first (acceptable).
struct DeferredRockDebris {
    bool pending = false;
    rf::Vector3 orientation;
    float scaled_radius;
    rf::Vector3 source_dir;
    int texture;
    int room;
};
static DeferredRockDebris g_rf2_deferred_debris;

// Geomod sound saved params — single global, played at earliest carving confirmation
// (boolean State 5). The stock code resolves weapon_type.geomod_foley_id into a
// random subsound handle before calling snd_play_3d; we save the resolved handle.
struct DeferredGeomodSound {
    bool pending = false;
    int handle = -1;
    rf::Vector3 pos;
};
static DeferredGeomodSound g_rf2_deferred_sound;

// Smoke emitter GeomodParams — two-stage FIFO:
// "pending" is populated during geomod_create. State 3 either moves front entry
// to "confirmed" (if carving happened) or discards it (near-miss).
// Per-frame hook only processes "confirmed" entries.
static std::deque<rf::GeomodParams> g_rf2_smoke_pending;
static std::deque<rf::GeomodParams> g_rf2_smoke_confirmed;

// Pointer(s) to entry_data of smoke-only emitter records.
static std::vector<void*> g_rf2_smoke_record_ptrs;

// Saved copy of the State 0 GDecalCreateInfo for g_decal_add.
// Only one state machine runs at a time, so a single global is safe.
static bool g_rf2_decal_deferred = false;
static rf::GDecalCreateInfo g_rf2_deferred_decal_info;

// Face snapshot taken before each boolean pass. Used by rf2_mark_unsupported_pieces
// to distinguish pre-existing disconnected components (e.g. interior faces of a hollow box)
// from components created by the crater split. Only components with at least one NEW face
// (not in this set) are eligible for extraction as debris.
//
// Tracked by raw GFace* (not face_id) intentionally: when the boolean splits a pre-existing
// face, the split sub-faces typically inherit the parent's face_id but are new GFace objects
// with new addresses. Pointer tracking correctly treats them as new (so crater-created
// disconnected pieces can fall). A face_id-based approach would treat the sub-faces as
// pre-existing and prevent valid debris from falling.
//
// Pool allocator can theoretically reuse a deleted face's address for a new face within the
// same boolean pass, causing the new face to be mis-identified as pre-existing. This would
// force a component to stay anchored instead of falling — a safe-direction error (geometry
// stays attached) that is rare in practice and not worth the complexity to guard against.
static std::unordered_set<rf::GFace*> g_rf2_pre_boolean_faces;

// Per-room targeting: when RF2-style is active, the boolean engine only processes
// faces from ONE specific detail room at a time. This prevents crashes and incorrect
// face classification that occur when the boolean engine tries to process faces from
// multiple disjoint detail rooms simultaneously.
// When a crater overlaps multiple rooms, we run multiple boolean passes (one per room).
static rf::GRoom* g_rf2_target_detail_room = nullptr;
static std::vector<rf::GRoom*> g_rf2_pending_detail_rooms;

// Support model (see rf2_snapshot_support / rf2_mark_unsupported_pieces).
// Faces of non-target geoable pieces that were supported before the current boolean pass.
static std::unordered_set<rf::GFace*> g_rf2_pre_supported_faces;
static bool g_rf2_target_supported_pre = false;
// Non-target geoable rooms that had pieces extracted this pass (render cache invalidation).
static std::vector<rf::GRoom*> g_rf2_cascaded_rooms;
// Rooms other than the target with faces split by boolean state 2 this pass (render cache invalidation).
static std::vector<rf::GRoom*> g_rf2_split_rooms;

// Find geoable detail rooms whose bboxes overlap the given position with padding
// scaled by level hardness. Base padding is 3 units at hardness 50 (baseline).
// Hardness 0 = 2x padding (softer rock, larger craters), hardness 100 = 0.5x padding.
// Formula: scale = 2^((50 - hardness) / 50), so 0→2.0, 50→1.0, 99→~0.507, 100→0.5.
static constexpr float geoable_bbox_base_padding = 3.0f;

// Per-material configuration table for non-glass breakable detail brushes.
// Indexed by ((int)DetailMaterial - 1). Glass (0) uses stock behavior and has no entry.
//                                      DT:  bash  bull  ap    expl  fire  enrg  elec  acid  scld  crsh  10    11
static constexpr BreakableMaterialConfig k_material_configs[] = {
    { // Rock
        .damage_factors     = {0.1f, 0.1f, 0.1f, 1.0f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 1.0f, 0.1f, 0.1f},
        .direct_hit_factor  = 0.1f,
        .debris             = {0.5f, 8, 3},
        .cap_texture        = "rock02.tga",
        .cap_texels_per_meter = 0.25f,
        .upward_velocity    = 3.0f,
        .horizontal_scatter = 2.0f,
        .explosion_push_speed = 8.0f,
        .break_foley_name   = "Geomod Debris Hit",
        .impact_iss_name    = nullptr,
        .impact_iss_ptr     = &g_rock_debris_iss,
        .explosion_name     = "geomod",
    },
    { // Wood
        .damage_factors     = {0.2f, 0.2f, 0.2f, 1.0f, 1.0f, 0.2f, 0.2f, 0.2f, 0.2f, 1.0f, 0.2f, 0.2f},
        .direct_hit_factor  = 0.15f,
        .debris             = {0.5f, 8, 3},
        .cap_texture        = "sld_wood_floor01.tga",
        .cap_texels_per_meter = 0.5f,
        .upward_velocity    = 4.0f,
        .horizontal_scatter = 2.5f,
        .explosion_push_speed = 10.0f,
        .break_foley_name   = "Solid Break",
        .impact_iss_name    = "wood break",
        .impact_iss_ptr     = nullptr,
        .explosion_name     = "geomod",
    },
    { // Metal
        .damage_factors     = {0.05f, 0.05f, 0.05f, 1.0f, 0.05f, 0.05f, 0.05f, 0.05f, 0.05f, 1.0f, 0.05f, 0.05f},
        .direct_hit_factor  = 0.1f,
        .debris             = {0.5f, 8, 3},
        .cap_texture        = "mtl_baserust04.tga",
        .cap_texels_per_meter = 0.5f,
        .upward_velocity    = 2.0f,
        .horizontal_scatter = 1.5f,
        .explosion_push_speed = 6.0f,
        .break_foley_name   = "metal break",
        .impact_iss_name    = "metal break",
        .impact_iss_ptr     = nullptr,
        .explosion_name     = "geomod",
    },
    { // Cement
        .damage_factors     = {0.1f, 0.1f, 0.1f, 1.0f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 1.0f, 0.1f, 0.1f},
        .direct_hit_factor  = 0.1f,
        .debris             = {0.5f, 8, 3},
        .cap_texture        = "cem_plainyel.tga",
        .cap_texels_per_meter = 0.5f,
        .upward_velocity    = 3.0f,
        .horizontal_scatter = 2.0f,
        .explosion_push_speed = 8.0f,
        .break_foley_name   = "Solid Break",
        .impact_iss_name    = "solid clatter",
        .impact_iss_ptr     = nullptr,
        .explosion_name     = "geomod",
    },
    { // Ice
        .damage_factors     = {0.15f, 0.15f, 0.15f, 1.0f, 0.5f, 0.15f, 0.15f, 0.15f, 0.15f, 1.0f, 0.15f, 0.15f},
        .direct_hit_factor  = 0.2f,
        .debris             = {0.5f, 8, 3},
        .cap_texture        = "ice_ice01.tga",
        .cap_texels_per_meter = 0.5f,
        .upward_velocity    = 3.5f,
        .horizontal_scatter = 2.5f,
        .explosion_push_speed = 9.0f,
        .break_foley_name   = "Icicle Break",
        .impact_iss_name    = nullptr,
        .impact_iss_ptr     = &g_ice_debris_iss,
        .explosion_name     = "geomod",
    },
};
static_assert(std::size(k_material_configs) == static_cast<int>(rf::DetailMaterial::Count) - 1);

// Per-material runtime state (indexed same as k_material_configs).
static BreakableMaterialState g_material_states[std::size(k_material_configs)];

const BreakableMaterialConfig* get_material_config(rf::DetailMaterial mat)
{
    int idx = static_cast<int>(mat) - 1;
    if (idx < 0 || idx >= static_cast<int>(std::size(k_material_configs)))
        return nullptr;
    return &k_material_configs[idx];
}

BreakableMaterialState* get_material_state(rf::DetailMaterial mat)
{
    int idx = static_cast<int>(mat) - 1;
    if (idx < 0 || idx >= static_cast<int>(std::size(g_material_states)))
        return nullptr;
    return &g_material_states[idx];
}

// Global flags: set in damage hooks before stock break code runs.
// Read by glass_sound/glass_shards entry hooks to suppress glass VFX for non-glass materials.
static rf::DetailMaterial g_breaking_material = rf::DetailMaterial::Glass;
static rf::GRoom* g_breaking_room = nullptr;
static bool g_breaking_from_explosion = false;

// Best-effort attribution for the current detail-brush break, for the stats stream.
// Set at the damage entry points -- exactly where g_breaking_from_explosion is -- and
// read once at the server-side break commit (glass_sound). A damage-driven break
// is synchronous within the damage call tree, so `capture_tick` (GetTickCount64, the same
// source s_last_vfx_tick uses) stamped at the damage entry equals the tick at the commit.
// The commit only trusts the attribution when the ticks match: a break that is NOT
// damage-driven this frame (level logic reaching glass_sound directly, or a stale capture
// from an earlier near-miss whose validate_destroy failed) reads a mismatched tick and is
// reported with null attribution -- wrong attribution is worse than null. Reset on level load.
struct BreakAttribution
{
    int killer_handle = -1;   // responsible entity handle; resolved to a player, null if not
    int weapon_type = -1;     // TC-mod weapon id, or -1 when not in scope (radius path)
    int damage_type = -1;     // stats registry value, or -1 (unknown)
    uint64_t capture_tick = 0; // GetTickCount64 at the damage entry; break commit must match
};
static BreakAttribution g_break_attr;

// The single predicate for both the capture sites and the commit site. Keeping them
// identical is what makes the capture side safe to skip: when this is false nothing is
// stamped, but nothing reads it either, and if it flips to true later the stale
// capture_tick can no longer match so the first break is simply reported anonymously.
// Gating the captures keeps the feature a strict no-op on a stats-disabled host and in
// single player, where apply_radius_damage runs on every explosion.
static bool afstats_break_attr_enabled()
{
    return rf::is_server && fflink::afstats_server_enabled();
}

// Per-room dedup: prevents multiple projectiles from creating effects on the same room
// in the same frame (e.g., pistol creating two weapon entities per shot).
static rf::GRoom* s_last_vfx_room = nullptr;
static uint64_t s_last_vfx_tick = 0;

// Captured damage_type from apply_radius_damage (FUN_00488dc0) for use in room damage scaling.
// apply_radius_damage has damage_type as param_5 but doesn't pass it to room_apply_radius_damage.
static int g_current_radius_damage_type = -1;

// Sentinel group_id value for face tagging (>= 0x80000000, can't match valid texture_mover ptrs)
static constexpr int k_debris_group_base = static_cast<int>(0x80000001);

// Executable code buffer for the radius damage trampoline (replaces naked asm function).
// Built dynamically with AsmWriter in apply_destruction_patches() to avoid compiler-specific
// inline assembly (__declspec(naked) + __asm{} is MSVC-only).
static CodeBuffer radius_damage_trampoline_code{64};

static float get_material_damage_factor(rf::DetailMaterial mat, int damage_type) {
    auto* cfg = get_material_config(mat);
    if (cfg && damage_type >= 0 && damage_type < rf::DT_COUNT)
        return cfg->damage_factors[damage_type];
    return 1.0f; // Glass or unknown = stock behavior
}

// Test if a point is inside a room's current geometry using ray casting.
// Works correctly for non-convex geometry (rooms modified by previous craters).
// Casts a ray from the test point and counts face polygon intersections; odd = inside.
// Uses the room's live face_list, which at State 5 reflects all previous craters
// but not the current one (split results aren't applied until State 6/7).
static bool is_point_inside_room_geometry(const rf::Vector3& pt, rf::GRoom* room)
{
    if (!room) return false;

    // AABB pre-filter
    constexpr float bbox_eps = 0.5f;
    if (pt.x < room->bbox_min.x - bbox_eps || pt.x > room->bbox_max.x + bbox_eps ||
        pt.y < room->bbox_min.y - bbox_eps || pt.y > room->bbox_max.y + bbox_eps ||
        pt.z < room->bbox_min.z - bbox_eps || pt.z > room->bbox_max.z + bbox_eps) {
        return false;
    }

    // Slightly tilted ray direction to avoid axis-aligned edge/vertex degeneracies
    constexpr float ray_dx = 0.00137f;
    constexpr float ray_dy = 0.00259f;
    constexpr float ray_dz = 1.0f;

    // Safety limits to prevent infinite loops from corrupted linked lists
    constexpr int max_face_count = 1 << 20;
    constexpr int face_margin = 1024;
    const int max_faces = std::clamp(room->face_list.size(), 0, max_face_count) + face_margin;
    constexpr int max_verts_per_face = 500;

    int crossing_count = 0;
    int face_count = 0;

    for (rf::GFace& face : room->face_list) {
        if (++face_count > max_faces) {
            xlog::warn("[RF2] is_point_inside_room_geometry: face iteration limit hit ({}) for room {} - possible list corruption",
                max_faces, room->room_index);
            break;
        }

        const rf::Plane& plane = face.plane;

        // Ray-plane intersection: t = -(dot(pt, normal) + offset) / dot(ray_dir, normal)
        float denom = ray_dx * plane.normal.x + ray_dy * plane.normal.y + ray_dz * plane.normal.z;
        if (denom > -1e-8f && denom < 1e-8f)
            continue; // ray nearly parallel to plane

        float numer = -(pt.x * plane.normal.x + pt.y * plane.normal.y + pt.z * plane.normal.z + plane.offset);
        float t = numer / denom;
        if (t < 0.0f)
            continue; // intersection behind ray origin

        // Compute intersection point on the plane
        float hit_x = pt.x + ray_dx * t;
        float hit_y = pt.y + ray_dy * t;
        float hit_z = pt.z + ray_dz * t;

        // Project onto 2D by dropping the axis with the largest normal component.
        // This gives the best-conditioned 2D polygon for the crossing test.
        float abs_nx = plane.normal.x >= 0.0f ? plane.normal.x : -plane.normal.x;
        float abs_ny = plane.normal.y >= 0.0f ? plane.normal.y : -plane.normal.y;
        float abs_nz = plane.normal.z >= 0.0f ? plane.normal.z : -plane.normal.z;

        bool drop_x = (abs_nx >= abs_ny && abs_nx >= abs_nz);
        bool drop_y = (!drop_x && abs_ny >= abs_nz);
        // drop_z implied when !drop_x && !drop_y

        float test_u, test_v;
        if (drop_x) {
            test_u = hit_y; test_v = hit_z;
        } else if (drop_y) {
            test_u = hit_x; test_v = hit_z;
        } else {
            test_u = hit_x; test_v = hit_y;
        }

        // Even-odd crossing number test against the face polygon
        rf::GFaceVertex* start = face.edge_loop;
        if (!start) continue;

        bool inside_polygon = false;
        int vert_count = 0;
        rf::GFaceVertex* fv = start;
        do {
            if (++vert_count > max_verts_per_face) {
                xlog::warn("[RF2] is_point_inside_room_geometry: vertex iteration limit hit ({}) - possible edge_loop corruption",
                    max_verts_per_face);
                break;
            }

            rf::GFaceVertex* fv_next = fv->next;
            if (!fv_next) break;
            if (!fv->vertex || !fv_next->vertex) break;

            const rf::Vector3& p0 = fv->vertex->pos;
            const rf::Vector3& p1 = fv_next->vertex->pos;

            float v0_u, v0_v, v1_u, v1_v;
            if (drop_x) {
                v0_u = p0.y; v0_v = p0.z;
                v1_u = p1.y; v1_v = p1.z;
            } else if (drop_y) {
                v0_u = p0.x; v0_v = p0.z;
                v1_u = p1.x; v1_v = p1.z;
            } else {
                v0_u = p0.x; v0_v = p0.y;
                v1_u = p1.x; v1_v = p1.y;
            }

            // Does a horizontal ray from (test_u, test_v) in +u direction cross this edge?
            if ((v0_v > test_v) != (v1_v > test_v)) {
                float edge_u = v0_u + (test_v - v0_v) / (v1_v - v0_v) * (v1_u - v0_u);
                if (test_u < edge_u) {
                    inside_polygon = !inside_polygon;
                }
            }

            fv = fv_next;
        } while (fv != start);

        if (inside_polygon) {
            crossing_count++;
        }
    }

    return (crossing_count & 1) != 0;
}

// Classification lookup table: DAT_005a38f4, indexed by (op_mode + face_type*8)*5 + classification.
// For geomod (op_mode=3), TYPE 1 (crater) entries at offset (3 + 1*8)*5 = 55 ints from base:
//   0x005a39D4: class 1 (INSIDE)       = 1 (mark for deletion)
//   0x005a39D8: class 2 (OUTSIDE)      = 2 (keep + flip)
//   0x005a39DC: class 3 (COPLANAR_SAME)= 1 (mark for deletion)
//   0x005a39E0: class 4 (COPLANAR_OPP) = 1 (mark for deletion)
//
// For RF2-style geomod, the stock boolean classifiers (room BSP trees, face adjacency)
// don't work correctly when only detail faces participate. Instead, we:
//   - State 1: Only detail brush TYPE 0 faces participate (boolean_skip_non_detail_faces_for_rf2)
//   - State 4: Skip all TYPE 1 faces (state4_skip_type1_for_rf2) to prevent the stock
//     classification table from acting on potentially-incorrect State 3 classifications
//   - State 5: Two hooks ensure ALL TYPE 1 faces are reclassified with our containment test:
//     a) state5_force_clear_type1_for_rf2: forces classification clearing for TYPE 1 faces
//        so they're always unclassified when the reclassification loop runs
//     b) state5_reclassify_type1_for_rf2: hooks BEFORE the cached_normal_room_list check
//        to apply convex hull containment test to ALL TYPE 1 faces, not just those where
//        the list is non-empty
//     Crater faces with centroids inside a detail brush → class 2 → keep (crater cap)
//     Crater faces with centroids outside all brushes → class 1 → delete (floating face)



static bool is_pos_in_any_geo_region(const rf::Vector3& pos)
{
    for (rf::GeoRegion* region : rf::level.regions) {
        if (rf::geo_region_test_point(pos, region))
            return true;
    }
    return false;
}

// FUN_004dc360 (boolean intersection detection, State 1) has two paths:
// - Fast path: traverses the solid's bbox tree to find faces within crater bounds.
//   Detail room faces are NOT in the main solid's bbox tree, so they're never found.
// - Slow path: iterates ALL registered faces and tests each for intersection.
//   This path is normally used only when DAT_01370f64 == 0 or for specific operations.
//
// For RF2-style geomod, we force the slow path so detail faces are found.
// At 0x004dc386: MOV EAX, [0x01370f64] — we override EAX to 0 so the JZ at
// 0x004dc39b takes the slow path branch.
CodeInjection state1_force_slow_path_for_rf2{
    0x004dc386,
    [](auto& regs) {
        if (g_rf2_style_boolean_active) {
            regs.eax = 0; // forces JZ at 004dc39b → slow path at 004dc41b
        } else {
            regs.eax = rf::g_boolean_fast_path_var;
        }
    },
};

// FUN_004dd450 (State 3 classification dispatcher) also branches on DAT_01370f64:
// - Fast path (FUN_004dd480): uses FUN_004e1430 to classify TYPE 1 faces against a
//   BSP tree built from ALL registered TYPE 0 faces, including non-detail. This gives
//   classification relative to the full level geometry, not just detail brushes.
// - Slow path (FUN_004dd5d0): uses FUN_004e05c0 (face-adjacency-based, only sees faces
//   from intersection candidate arrays which exclude non-detail) and falls back to
//   FUN_004e0d00 (BSP using detail rooms via boolean_state5_allow_detail_for_rf2).
//
// For RF2-style geomod, we force the slow path so TYPE 1 face classification is
// relative to detail brush geometry only.
// At 0x004dd450: MOV EAX, [0x01370f64] — we override EAX to 0 so TEST+JZ takes
// the slow path branch at 0x004dd46c.
CodeInjection state3_force_slow_path_for_rf2{
    0x004dd450,
    [](auto& regs) {
        if (g_rf2_style_boolean_active) {
            regs.eax = 0; // forces JZ at 004dd457 → slow path at 004dd46c
        } else {
            regs.eax = rf::g_boolean_fast_path_var;
        }
    },
};

// FUN_004dd780 (State 4) iterates all registered faces and applies actions 2
// (keep/flip) and 3 (duplicate for portals) based on State 3's classifications.
// State 5 (FUN_004dd8c0) clears and re-does classification independently, so
// State 4's work on TYPE 1 faces based on potentially-incorrect State 3
// classifications could create incorrect face duplicates. For RF2-style geomod,
// skip TYPE 1 faces in State 4 entirely — State 5 will handle them with our
// custom convex hull containment test.
//
// At 004dd7f4: CALL 0x004de9d0 returns face type in EAX.
// At 004dd7f9: we check if TYPE 1 → skip to next face at 004dd8a2.
CodeInjection state4_skip_type1_for_rf2{
    0x004dd7f9,
    [](auto& regs) {
        if (g_rf2_style_boolean_active && regs.eax == 1) {
            regs.eip = 0x004dd8a2; // skip to next face in loop
        }
    },
};

// FUN_004dd8c0 (State 5) calls FUN_004dba30(solid, op_mode) at 004dd8d7 to check
// whether reclassification is needed. If it returns false, JZ at 004dd8e1 skips to
// 004dd99e, bypassing the ENTIRE clearing and reclassification loops. For RF2-style
// geomod, FUN_004dba30 returns false because our filtered face set doesn't trigger
// its criteria. We force it to return true so reclassification proceeds.
//
// Disassembly:
//   004dd8d5: PUSH EAX              ; op_mode
//   004dd8d6: PUSH EDI              ; solid
//   004dd8d7: CALL 0x004dba30       ; <<< HOOKED (5 bytes)
//   004dd8dc: ADD ESP, 0x8
//   004dd8df: TEST AL, AL
//   004dd8e1: JZ 0x004dd99e         ; skip reclassification if false
CodeInjection state5_force_reclassify_for_rf2{
    0x004dd8d7,
    [](auto& regs) {
        if (g_rf2_style_boolean_active) {
            regs.eax = 1;            // force non-zero → reclassification proceeds
            regs.eip = 0x004dd8dc;   // skip CALL, go to ADD ESP cleanup
        }
    },
};

// FUN_004dd8c0 (State 5) has a first loop (004dd921-004dd944) that conditionally
// clears face classifications. FUN_004dea00 is called per face and returns whether
// the classification should be cleared. For some TYPE 1 faces, FUN_004dea00 returns
// false, leaving their (incorrect) State 3 classification intact. The subsequent
// reclassification loop then skips them because they're already classified.
//
// At 004dd926: CALL 0x004dea00 — the classification-clearing check.
// Registers: EBX = &face->attributes (also in ECX for the call).
// For RF2-style geomod, we force-clear TYPE 1 faces' classifications so they're
// always unclassified when the reclassification loop runs.
CodeInjection state5_force_clear_type1_for_rf2{
    0x004dd926,
    [](auto& regs) {
        if (!g_rf2_style_boolean_active) return;

        void* face_attrs = regs.ebx;
        if (!face_attrs) return; // safety: null face attributes

        // Check if this is a TYPE 1 face
        int type = AddrCaller{0x004de9d0}.this_call<int>(face_attrs);
        if (type == 1) {
            // Force classification to 0 (unclassified)
            AddrCaller{0x004de9e0}.this_call(face_attrs, 0);
            regs.eip = 0x004dd938; // skip to next face in clearing loop
        }
    },
};

// FUN_004dd8c0 (State 5) reclassification loop (004dd946-004dd99c) reclassifies
// unclassified faces. For TYPE 1 faces, it checks cached_normal_room_list size
// at [solid + 0x90]:
//   - If non-empty: calls FUN_004e0d00 (BSP room classifier)
//   - If empty: falls through to FUN_004e0980 (TYPE 0 classifier) — WRONG for TYPE 1
//
// We hook at 004dd96d (start of the TYPE 1 path, after JNZ at 004dd96b confirms
// the face is TYPE 1) to intercept ALL TYPE 1 faces BEFORE the cached_normal_room_list
// check. This ensures our convex hull containment test handles every TYPE 1 face.
//
// Registers: ESI = GFace*, EBX = &face->attributes, EDI = solid (param_1).
// We compute the face centroid, test it against saved detail room planes, and
// set classification directly (class 2 = inside/keep, class 1 = outside/delete).
CodeInjection state5_reclassify_type1_for_rf2{
    0x004dd96d,
    [](auto& regs) {
        if (!g_rf2_style_boolean_active) return;

        rf::GFace* face = regs.esi;
        void* face_attrs = regs.ebx;

        if (!face || !face_attrs) {
            regs.eip = 0x004dd990;
            return;
        }

        // Compute face centroid from edge loop vertices
        constexpr int max_centroid_verts = 500;
        rf::Vector3 centroid{0.0f, 0.0f, 0.0f};
        int vertex_count = 0;
        rf::GFaceVertex* fv = face->edge_loop;
        if (fv) {
            do {
                if (!fv->vertex) break;
                centroid.x += fv->vertex->pos.x;
                centroid.y += fv->vertex->pos.y;
                centroid.z += fv->vertex->pos.z;
                vertex_count++;
                if (vertex_count > max_centroid_verts) break;
                fv = fv->next;
            } while (fv && fv != face->edge_loop);
        }

        if (vertex_count > 0) {
            float inv = 1.0f / static_cast<float>(vertex_count);
            centroid.x *= inv;
            centroid.y *= inv;
            centroid.z *= inv;
        }

        // Classify against the target detail room's current geometry (ray casting).
        // Uses live face_list which correctly reflects previous craters (non-convex).
        int classification = is_point_inside_room_geometry(centroid, g_rf2_target_detail_room) ? 2 : 1;

        // Set classification on face attributes via FUN_004de9e0
        AddrCaller{0x004de9e0}.this_call(face_attrs, classification);
        regs.eip = 0x004dd990; // skip stock classifiers, go to loop continuation
    },
};

// In the slow path of FUN_004dc360, after a TYPE 0 face passes the intersection
// test (its bbox overlaps the crater bounds), it's about to be classified and
// potentially added to the output array for pairwise intersection testing.
//
// For RF2-style geomod, we skip non-detail TYPE 0 faces: set their side to 2
// (outside/keep) so they're never carved. Only detail faces proceed normally.
//
// Injection at 0x004dc4aa: first instruction of the TYPE 0 path after the
// intersection test succeeds. At this point EDI = face, ESI = &face->attributes.
// We call FUN_004de9e0(2) to set side=2, then skip to next face (0x004dc521).
CodeInjection boolean_skip_non_detail_faces_for_rf2{
    0x004dc4aa,
    [](auto& regs) {
        if (!g_rf2_style_boolean_active) return;

        rf::GFace* face = regs.edi;
        // Skip ALL faces when no target room (geomod suppression outside detail brushes).
        // Otherwise, only allow faces from the target detail room; skip everything else.
        if (!g_rf2_target_detail_room ||
            !face->which_room || !face->which_room->is_detail ||
            !face->which_room->is_geoable ||
            face->which_room != g_rf2_target_detail_room) {
            void* face_attrs = regs.esi;
            AddrCaller{0x004de9e0}.this_call(face_attrs, 2);
            regs.eip = 0x004dc521; // skip to next face in loop
        }
    },
};

// FUN_004e0d00 (called from boolean States 3 & 5 for TYPE 1 / crater faces) uses
// room BSP trees to reclassify unclassified TYPE 1 faces as INSIDE or OUTSIDE the
// level solid. It explicitly filters out detail rooms via FUN_00494a50 at 004e0e24,
// which reads the first byte of each room (is_detail). For RF2-style geomod, we
// INVERT this filter: only process detail rooms, skip non-detail rooms. This ensures
// the BSP reclassification determines whether crater faces are inside detail brush
// volumes (keep as crater cap) or outside (delete as floating face).
//
// Disassembly context:
//   004e0e20: MOV ESI, [EAX]       ; ESI = room pointer from array
//   004e0e22: MOV ECX, ESI
//   004e0e24: CALL 0x00494a50      ; is_detail(room) - returns byte [ECX]
//   004e0e29: TEST AL, AL
//   004e0e2b: JNZ 0x004e0e3e      ; if detail → skip (stock behavior)
//   004e0e2d: ... (include room)   ; non-detail rooms proceed
//   004e0e3e: ... (loop increment)
CodeInjection boolean_state5_allow_detail_for_rf2{
    0x004e0e24,
    [](auto& regs) {
        if (!g_rf2_style_boolean_active) return;

        // ESI = room pointer (GRoom*); first byte is is_detail
        auto* room = static_cast<rf::GRoom*>(regs.esi);
        bool is_target = room->is_detail && room->is_geoable &&
            (!g_rf2_target_detail_room || room == g_rf2_target_detail_room);
        if (is_target) {
            regs.eip = 0x004e0e2d; // allow target detail room
        } else {
            regs.eip = 0x004e0e3e; // skip non-target room
        }
    },
};

// During State 0 registration (FUN_004dbdf0), the boolean engine sets bit 3
// (0x08) on face attribute flags for detail room faces. This bit is later
// checked by FUN_004ce480 (flags & 0x0C == 0) to EXCLUDE detail faces from
// intersection candidate arrays, preventing them from being carved.
//
// For RF2-style geomod, we need detail faces to participate in the boolean
// pipeline like normal faces. The face attributes may ALREADY have bit 3 set
// from the original level geometry data, so simply skipping the bit-3-setting
// code is insufficient — we must explicitly CLEAR bit 3.
//
// At 0x004dbeff: CALL 0x004909b0 (5-byte instruction, safe for SubHook).
// This is the "room IS detail" branch. ESI = &face->attributes (GFaceAttributes*).
// The flags byte is at [ESI+0]. We clear bit 3 and skip to the next face.
//
// Disassembly context:
//   004dbef4: CALL 0x00494a50        ; is_detail(room)
//   004dbef9: TEST AL, AL
//   004dbefd: JZ 0x004dbf0e          ; if NOT detail, go to non-detail path
//   ; detail path:
//   004dbeff: CALL 0x004909b0        ; <<< OUR INJECTION
//   004dbf08: MOV EAX, [ESI]         ; get flags dword
//   004dbf0a: OR AL, 0x8             ; set bit 3
//   004dbf0c: JMP 0x004dbf1b
//   ; non-detail path:
//   004dbf0e: CALL 0x004909b0
//   004dbf17: MOV EAX, [ESI]
//   004dbf19: AND AL, 0xf7           ; clear bit 3
//   004dbf1b: MOV [ESI], EAX         ; store flags
//   004dbf1d: ... (next face)
CodeInjection boolean_clear_detail_bit3_for_rf2{
    0x004dbeff,
    [](auto& regs) {
        if (g_rf2_style_boolean_active) {
            // Only clear bit 3 for geoable detail rooms so their faces pass ce480.
            // Non-geoable detail rooms keep bit 3 set (stock behavior: excluded from boolean).
            auto* face = static_cast<rf::GFace*>(regs.edi);
            if (face->which_room && face->which_room->is_geoable) {
                auto* flags = reinterpret_cast<uint8_t*>(static_cast<void*>(regs.esi));
                flags[0] &= ~0x08;
            }
            regs.eip = 0x004dbf1d; // skip to next face
        }
    },
};

ConsoleCommand2 dbg_num_geomods_cmd{
    "dbg_numgeos",
    []() {
        if (!(rf::level.flags & rf::LEVEL_LOADED)) {
            rf::console::print("No level loaded!");
            return;
        }

        if (rf::is_multi && !rf::is_server) {
            rf::console::print("In multiplayer, this command can only be run by the server.");
            return;
        }

        int max_geos = rf::is_multi ? rf::netgame.geomod_limit : 128;

        rf::console::print("{} craters in the current level out of a maximum of {}", rf::g_num_geomods_this_level, max_geos);
        if (AlpineLevelProperties::instance().rf2_style_geomod) {
            std::string rf2_limit_str = (g_rf2_geo_limit < 0) ? "unlimited" : std::to_string(g_rf2_geo_limit);
            rf::console::print("  RF2-style: {} (limit: {})", g_rf2_geo_count, rf2_limit_str);
        }
    },
    "Count the number of geomod craters in the current level",
};


// Check if two planes are coplanar (same or opposite orientation, same plane).
// Handles both same-direction and opposite-direction normals since detail brush
// faces touching a wall will have opposite normals to the wall face.
static bool planes_are_coplanar(const rf::Plane& a, const rf::Plane& b)
{
    constexpr float normal_eps = 0.02f;  // ~1 degree tolerance
    constexpr float offset_eps = 0.15f;  // distance tolerance between planes

    float dot = a.normal.x * b.normal.x + a.normal.y * b.normal.y + a.normal.z * b.normal.z;

    // Same direction normals
    if (dot > 1.0f - normal_eps) {
        return std::abs(a.offset - b.offset) < offset_eps;
    }
    // Opposite direction normals (detail face touching wall face)
    if (dot < -1.0f + normal_eps) {
        return std::abs(a.offset + b.offset) < offset_eps;
    }
    return false;
}

// Check if two faces' bounding boxes overlap (with padding for floating point tolerance).
static bool face_bboxes_overlap(const rf::GFace& a, const rf::GFace& b)
{
    constexpr float pad = 0.1f;
    return a.bounding_box_max.x + pad >= b.bounding_box_min.x &&
           a.bounding_box_min.x - pad <= b.bounding_box_max.x &&
           a.bounding_box_max.y + pad >= b.bounding_box_min.y &&
           a.bounding_box_min.y - pad <= b.bounding_box_max.y &&
           a.bounding_box_max.z + pad >= b.bounding_box_min.z &&
           a.bounding_box_min.z - pad <= b.bounding_box_max.z;
}

// Crater face bitmap per terrain (alpine_terrain_get_all index), -1 = level geomod texture.
static std::vector<int> g_terrain_crater_bitmaps;

static void apply_terrain_geoable_flags(rf::GSolid* solid)
{
    const auto& terrains = alpine_terrain_get_all();
    for (rf::GRoom* room : solid->all_rooms) {
        const AlpineTerrainRoomRef* ref = alpine_terrain_find_room(room);
        if (!ref) continue;
        const AlpineTerrain& t = terrains[ref->terrain];
        const auto chunk = static_cast<std::size_t>(ref->chunk);
        if ((t.header.flags & alpine_terrain::flag_geoable) && chunk / 8 < t.geo_chunks.size() &&
            alpine_terrain::get_bit(t.geo_chunks.data(), chunk)) {
            room->is_geoable = true;
        }
    }
    g_terrain_crater_bitmaps.assign(terrains.size(), -1);
    for (std::size_t i = 0; i < terrains.size(); i++) {
        const AlpineTerrain& t = terrains[i];
        if (t.resolved && (t.header.flags & alpine_terrain::flag_geoable) && !t.crater_texture.empty()) {
            g_terrain_crater_bitmaps[i] = rf::bm::load(t.crater_texture.c_str(), -1, true);
        }
    }
}

// Apply geoable flags from AlpineLevelProperties UIDs to GRoom objects.
// Called from level_init_post_hook after both rooms and Alpine props are loaded.
void apply_geoable_flags()
{
    g_terrain_crater_bitmaps.clear();
    auto* solid = rf::level.geometry;
    if (!solid) return;

    // Clear is_geoable on all rooms first (GRoom padding is not zero-initialized)
    for (auto& room : solid->all_rooms) {
        room->is_geoable = false;
    }

    auto& props = AlpineLevelProperties::instance();
    xlog::debug("[Geoable] apply_geoable_flags: rf2_style_geomod={} geoable_room_uids={}", props.rf2_style_geomod, props.geoable_room_uids.size());
    if (!props.rf2_style_geomod) return;

    // Search solid->all_rooms directly (level_room_from_uid doesn't cover detail rooms)
    for (int32_t uid : props.geoable_room_uids) {
        bool found = false;
        for (auto& room : solid->all_rooms) {
            if (room->uid == uid && room->is_detail) {
                room->is_geoable = true;
                found = true;
                xlog::debug("[Geoable] applied is_geoable to room uid={} index={} bbox=({:.1f},{:.1f},{:.1f})-({:.1f},{:.1f},{:.1f})",
                    uid, room->room_index,
                    room->bbox_min.x, room->bbox_min.y, room->bbox_min.z,
                    room->bbox_max.x, room->bbox_max.y, room->bbox_max.z);
                break;
            }
        }
        if (!found) {
            xlog::debug("[Geoable] room uid={} not found in solid->all_rooms", uid);
        }
    }

    apply_terrain_geoable_flags(solid);
}

// Reset all breakable material global state. Called on level load to prevent stale pointers
// from the previous level/save from causing crashes.
void reset_breakable_material_state()
{
    g_breaking_material = rf::DetailMaterial::Glass;
    g_breaking_room = nullptr;
    g_breaking_from_explosion = false;
    g_break_attr = BreakAttribution{};
    s_last_vfx_room = nullptr;
    s_last_vfx_tick = 0;
    g_current_radius_damage_type = -1;

    // Reset per-material runtime state
    for (auto& s : g_material_states) {
        s.cap_bm_loaded = false;
        s.cap_bm = -1;
    }
    sound_foley_level_cleanup(g_material_states, std::size(g_material_states));
}

// Apply breakable material types from AlpineLevelProperties to GRoom objects.
// Called from level_init_post_hook after rooms and Alpine props are loaded.
void apply_breakable_materials()
{
    // Reset stale global state from previous level/save to prevent dangling pointer dereferences
    reset_breakable_material_state();

    auto* solid = rf::level.geometry;
    if (!solid) {
        xlog::trace("[Material] apply_breakable_materials: no solid geometry");
        return;
    }

    // Default all rooms to Glass with no flags (padding is not zero-initialized)
    int total_rooms = 0;
    int detail_rooms = 0;
    for (auto& room : solid->all_rooms) {
        room->material_type = rf::DetailMaterial::Glass;
        room->no_debris = false;
        total_rooms++;
        if (room->is_detail) detail_rooms++;
    }
    xlog::trace("[Material] apply_breakable_materials: total_rooms={} detail_rooms={}", total_rooms, detail_rooms);

    auto& props = AlpineLevelProperties::instance();
    xlog::trace("[Material] apply_breakable_materials: breakable_room_uids.size={} breakable_materials.size={}",
        props.breakable_room_uids.size(), props.breakable_materials.size());
    if (props.breakable_room_uids.empty()) {
        xlog::trace("[Material] apply_breakable_materials: no breakable entries, all rooms default to Glass");
        return;
    }

    for (std::size_t i = 0; i < props.breakable_room_uids.size(); i++) {
        int32_t uid = props.breakable_room_uids[i];
        if (uid == 0) {
            continue; // the editor found no compiled room for this brush
        }
        uint8_t raw = (i < props.breakable_materials.size()) ? props.breakable_materials[i] : 0;
        if (raw == 0) {
            // Mapping-only row: every room was defaulted to Glass with no flags above, so this
            // can only ever be a no-op — or, if its room UID came from the editor's position
            // fallback and landed on a neighbour, reset a real material. Skip it.
            continue;
        }
        uint8_t mat = raw & 0x7F;
        bool no_debris = (raw & 0x80) != 0;
        if (mat >= static_cast<uint8_t>(rf::DetailMaterial::Count)) {
            xlog::warn("[Material] room uid={} has out-of-range material {}, treating as Glass", uid, mat);
            mat = 0;
            no_debris = false;
        }
        bool found = false;
        for (auto& room : solid->all_rooms) {
            if (room->uid == uid && room->is_detail) {
                room->material_type = static_cast<rf::DetailMaterial>(mat);
                room->no_debris = no_debris;
                xlog::trace("[Material] applied material {} no_debris={} to room uid={} index={} life={:.1f}",
                    mat, no_debris, uid, room->room_index, room->life);
                found = true;
                break;
            }
        }
        if (!found) {
            xlog::debug("[Material] FAILED to find room uid={} for material {} (not found among detail rooms)", uid, mat);
        }
    }
}

// Destruction registry for the When_Destroyed catalyst event.
struct DestructibleBrushState
{
    std::vector<int> brush_uids;
    bool destroyed = false;
};

static std::unordered_map<int, DestructibleBrushState> g_brush_destruction; // room uid -> state
static std::unordered_map<int, int> g_brush_uid_to_room_uid;
// Every breakable brush UID the level declares, mapped or not. A row whose room mapping failed
// still names a brush, and a brush is never an activation target.
static std::unordered_set<int> g_breakable_brush_uids;

static DestructibleBrushState* brush_state_from_brush_uid(int brush_uid)
{
    auto uit = g_brush_uid_to_room_uid.find(brush_uid);
    if (uit == g_brush_uid_to_room_uid.end()) return nullptr;

    auto it = g_brush_destruction.find(uit->second);
    return it == g_brush_destruction.end() ? nullptr : &it->second;
}

bool brush_is_tracked(int brush_uid)
{
    return brush_state_from_brush_uid(brush_uid) != nullptr;
}

bool brush_uid_is_breakable(int brush_uid)
{
    return g_breakable_brush_uids.count(brush_uid) != 0;
}

bool brush_is_destroyed(int brush_uid)
{
    const DestructibleBrushState* state = brush_state_from_brush_uid(brush_uid);
    return state && state->destroyed;
}

static void fire_when_destroyed_for_room(int room_uid)
{
    auto it = g_brush_destruction.find(room_uid);
    if (it == g_brush_destruction.end()) return;

    // Copy: event activation is synchronous and a chained break can rehash the registry.
    const std::vector<int> brush_uids = it->second.brush_uids;
    for (int brush_uid : brush_uids) {
        rf::activate_all_events_of_type(rf::EventType::When_Destroyed, brush_uid, -1, true);
    }
}

// A destructible brush that breaks counts as fully destroyed. Guarded so a room can only
// commit once even if the break path were ever re-entered. `fire` is false on paths that
// only observe an already-destroyed room (save restore), where firing would replay links.
static void note_brush_break(int room_uid, bool fire = true)
{
    auto it = g_brush_destruction.find(room_uid);
    if (it == g_brush_destruction.end()) return;
    if (it->second.destroyed) return;

    it->second.destroyed = true;
    if (fire) {
        fire_when_destroyed_for_room(room_uid);
    }
}

// Build the brush uid -> room uid bridge. Called from level_init_post after
// apply_breakable_materials, which is what fills the pair lists this reads.
void destruction_level_init_post()
{
    g_brush_destruction.clear();
    g_brush_uid_to_room_uid.clear();
    g_breakable_brush_uids.clear();

    auto& props = AlpineLevelProperties::instance();
    g_breakable_brush_uids.insert(props.breakable_brush_uids.begin(), props.breakable_brush_uids.end());

    auto* solid = rf::level.geometry;
    if (!solid) return;

    std::unordered_map<int, std::vector<int>> room_to_brushes;
    auto add_pairs = [&](const std::vector<int32_t>& brush_uids, const std::vector<int32_t>& room_uids) {
        const std::size_t n = std::min(brush_uids.size(), room_uids.size());
        for (std::size_t i = 0; i < n; i++) {
            if (room_uids[i] == 0) continue; // editor found no matching compiled room

            auto [it, inserted] = g_brush_uid_to_room_uid.emplace(brush_uids[i], room_uids[i]);
            if (!inserted) {
                if (it->second != room_uids[i]) {
                    xlog::warn("[Destroyed] brush uid={} maps to room {} and room {}, keeping {}",
                        brush_uids[i], it->second, room_uids[i], it->second);
                }
                continue; // already recorded under the room it first claimed
            }
            room_to_brushes[room_uids[i]].push_back(brush_uids[i]);
        }
    };
    // Breakable entries only: When_Destroyed tracks destructible detail brushes (life != -1).
    // Geoable brushes belong to the RF2 geomod feature and are not tracked here.
    add_pairs(props.breakable_brush_uids, props.breakable_room_uids);

    if (room_to_brushes.empty()) return;

    for (auto& room : solid->all_rooms) {
        if (!room || !room->is_detail) continue;

        auto bit = room_to_brushes.find(room->uid);
        if (bit == room_to_brushes.end()) continue;

        DestructibleBrushState state;
        state.brush_uids = bit->second;
        g_brush_destruction.emplace(room->uid, state);
    }

    xlog::debug("[Destroyed] tracking {} destructible brushes", g_brush_destruction.size());
}

CodeInjection validate_destroy_vertex_count_fix{
    0x00490BBF,
    [](auto& regs) {
        auto* face = reinterpret_cast<rf::GFace*>(static_cast<uintptr_t>(regs.ecx));
        int count = face->vertex_count();

        // stock game allows breaking of quads only
        // v303+ levels allow breaking of faces with >= 3 verts
        bool pass = rfl_version_minimum(304) ? (count >= 3) : (count == 4);
        regs.eip = pass ? 0x00490BCD : 0x00490BC9;
    },
};

// Capture damage_type at entry of apply_radius_damage (0x00488dc0). Also stashes the
// radius blow's killer_handle and damage_type for the stats stream: the room
// break happens deeper in this call (room_apply_radius_damage -> glass_kill) with neither
// in scope. Stack: SUB ESP,0x18 has run but the four register pushes at 0x00488ded have
// not, so the cdecl args killer_handle (arg 4) and damage_type (arg 5) sit at esp+0x28 and
// esp+0x2C. The radius path carries no weapon type, so that stays null.
CodeInjection capture_damage_type_injection{
    0x00488ded,
    [](auto& regs) {
        g_current_radius_damage_type = *reinterpret_cast<int*>(regs.esp + 0x2C);
        if (afstats_break_attr_enabled()) {
            g_break_attr.killer_handle = *reinterpret_cast<int*>(regs.esp + 0x28);
            g_break_attr.weapon_type = -1;
            g_break_attr.damage_type = g_current_radius_damage_type;
            g_break_attr.capture_tick = GetTickCount64();
        }
    },
};

// Reset damage_type when apply_radius_damage returns (0x00488ffc, after last call to
// room_apply_radius_damage). Prevents stale values from causing subsequent direct-hit
// breaks to incorrectly set g_breaking_from_explosion = true.
CodeInjection reset_damage_type_injection{
    0x00488ffc,
    [](auto& regs) {
        g_current_radius_damage_type = -1;
    },
};

// Immediately invalidate room's rendering state after face extraction.
// Must be called BEFORE the next render frame to prevent stale geo_cache access.
static void invalidate_room_rendering(rf::GRoom* room)
{
    if (!room) return;

    // Invalidate parent room's render cache (same logic as process_destroy_cleanup_injection)
    auto* parent = room->room_to_render_with;
    if (parent) {
        bool valid = false;
        auto* solid = rf::level.geometry;
        if (solid) {
            for (auto& r : solid->all_rooms) {
                if (r == parent) { valid = true; break; }
            }
        }
        if (valid && parent->geo_cache) {
            parent->geo_cache->state = 2; // triggers cache rebuild
        }
    }

    // Prevent rendering of this room
    room->room_to_render_with = nullptr;
    room->geo_cache = nullptr;
    room->face_list.clear();
}

// Compute bbox_min/bbox_max and bounding sphere on a GSolid from its vertex positions.
// FUN_004d1330 fills a separate GBBox output buffer but does NOT update the solid's own
// bbox fields. The boolean engine reads bbox_min/bbox_max, so we must set them ourselves.
static void compute_solid_bounds(rf::GSolid* s)
{
    // Iterate face vertices rather than s->vertices VArray, because extracted
    // solids (from extract_faces_by_group) may have an empty VArray while
    // their faces still reference valid vertices from the parent solid.
    rf::Vector3 vmin{1e18f, 1e18f, 1e18f};
    rf::Vector3 vmax{-1e18f, -1e18f, -1e18f};
    int vert_count = 0;
    int face_count = 0;
    for (auto& face : s->face_list) {
        face_count++;
        auto* fv = face.edge_loop;
        if (!fv) continue;
        auto* start = fv;
        do {
            auto& p = fv->vertex->pos;
            if (p.x < vmin.x) vmin.x = p.x;
            if (p.y < vmin.y) vmin.y = p.y;
            if (p.z < vmin.z) vmin.z = p.z;
            if (p.x > vmax.x) vmax.x = p.x;
            if (p.y > vmax.y) vmax.y = p.y;
            if (p.z > vmax.z) vmax.z = p.z;
            vert_count++;
            fv = fv->next;
        } while (fv != start);
    }
    s->bbox_min = vmin;
    s->bbox_max = vmax;
    s->bounding_sphere_center.x = (vmin.x + vmax.x) * 0.5f;
    s->bounding_sphere_center.y = (vmin.y + vmax.y) * 0.5f;
    s->bounding_sphere_center.z = (vmin.z + vmax.z) * 0.5f;
    float dx = vmax.x - vmin.x, dy = vmax.y - vmin.y, dz = vmax.z - vmin.z;
    s->bounding_sphere_radius = std::sqrt(dx * dx + dy * dy + dz * dz) * 0.5f;
}

// Split faces in a solid along a cutting plane.
// Faces entirely on one side get their group_id set directly.
// Faces straddling the plane are clipped: two sub-faces are created (one per side)
// with interpolated texture coordinates at the intersection points, and the original
// face is removed. This ensures no face extends past the cut boundary, eliminating
// overlap between cap faces and existing geometry.
//
// Returns the count of faces assigned to each side.
static std::pair<int, int> split_faces_by_plane(
    rf::GSolid* solid,
    const rf::Vector3& plane_normal,
    float plane_offset,
    int pos_group_id,
    int neg_group_id)
{
    constexpr float eps = 0.001f;
    int pos_count = 0, neg_count = 0;

    // Cache intersection vertices by edge so adjacent faces sharing an edge reuse
    // the same GVertex at their shared crossing point. Without this, boundary loop
    // detection can't chain edges across faces (different vertex objects at same position).
    using EdgeKey = std::pair<rf::GVertex*, rf::GVertex*>;
    auto make_edge_key = [](rf::GVertex* a, rf::GVertex* b) -> EdgeKey {
        return (a < b) ? EdgeKey{a, b} : EdgeKey{b, a};
    };
    std::map<EdgeKey, rf::GVertex*> intersection_cache;

    // Snapshot face list (we modify the list during iteration)
    std::vector<rf::GFace*> faces;
    for (auto& face : solid->face_list) {
        faces.push_back(&face);
    }

    for (auto* face : faces) {
        // Classify each vertex relative to the cutting plane
        struct VertInfo {
            rf::GFaceVertex* fv;
            float dist;
        };
        std::vector<VertInfo> verts;
        auto* start = face->edge_loop;
        if (!start) continue;
        auto* fv = start;
        do {
            float d = plane_normal.dot_prod(fv->vertex->pos) + plane_offset;
            verts.push_back({fv, d});
            fv = fv->next;
        } while (fv != start);

        bool has_pos = false, has_neg = false;
        for (auto& v : verts) {
            if (v.dist > eps) has_pos = true;
            if (v.dist < -eps) has_neg = true;
        }

        if (!has_neg) {
            // Entirely positive (or on-plane)
            face->attributes.group_id = pos_group_id;
            pos_count++;
        }
        else if (!has_pos) {
            // Entirely negative
            face->attributes.group_id = neg_group_id;
            neg_count++;
        }
        else {
            // Face straddles the plane — clip into two sub-faces
            int n = static_cast<int>(verts.size());

            struct SubVert {
                rf::GVertex* vertex;
                float u, v, lu, lv;
            };
            std::vector<SubVert> pos_verts, neg_verts;

            for (int i = 0; i < n; i++) {
                int j = (i + 1) % n;
                auto& vi = verts[i];
                auto& vj = verts[j];

                SubVert sv{vi.fv->vertex, vi.fv->texture_u, vi.fv->texture_v,
                           vi.fv->lightmap_u, vi.fv->lightmap_v};

                // Vertex goes to the side it's on (on-plane vertices go to both)
                if (vi.dist >= -eps) pos_verts.push_back(sv);
                if (vi.dist <= eps) neg_verts.push_back(sv);

                // If edge crosses the plane, get or create intersection vertex.
                // Adjacent faces sharing this edge reuse the same GVertex (position),
                // but each face gets its own interpolated UVs via GFaceVertex.
                bool crosses = (vi.dist > eps && vj.dist < -eps) ||
                               (vi.dist < -eps && vj.dist > eps);
                if (crosses) {
                    float t = vi.dist / (vi.dist - vj.dist);

                    // Reuse intersection vertex if this edge was already split by an adjacent face
                    auto key = make_edge_key(vi.fv->vertex, vj.fv->vertex);
                    rf::GVertex* shared_vert;
                    auto cache_it = intersection_cache.find(key);
                    if (cache_it != intersection_cache.end()) {
                        shared_vert = cache_it->second;
                    }
                    else {
                        rf::Vector3 ipos{
                            vi.fv->vertex->pos.x + t * (vj.fv->vertex->pos.x - vi.fv->vertex->pos.x),
                            vi.fv->vertex->pos.y + t * (vj.fv->vertex->pos.y - vi.fv->vertex->pos.y),
                            vi.fv->vertex->pos.z + t * (vj.fv->vertex->pos.z - vi.fv->vertex->pos.z)
                        };
                        shared_vert = AddrCaller{0x004cfc80}.this_call<rf::GVertex*>(solid, &ipos);
                        intersection_cache[key] = shared_vert;
                    }

                    // UVs are per-face, so always interpolate fresh
                    float iu = vi.fv->texture_u + t * (vj.fv->texture_u - vi.fv->texture_u);
                    float iv = vi.fv->texture_v + t * (vj.fv->texture_v - vi.fv->texture_v);
                    float ilu = vi.fv->lightmap_u + t * (vj.fv->lightmap_u - vi.fv->lightmap_u);
                    float ilv = vi.fv->lightmap_v + t * (vj.fv->lightmap_v - vi.fv->lightmap_v);

                    SubVert isv{shared_vert, iu, iv, ilu, ilv};
                    pos_verts.push_back(isv);
                    neg_verts.push_back(isv);
                }
            }

            // Create sub-face from vertex list, preserving original face plane and attributes
            auto create_sub_face = [&](const std::vector<SubVert>& sub, int group_id) -> bool {
                if (sub.size() < 3) return false;
                rf::GFaceAttributes attrs = face->attributes;
                attrs.group_id = group_id;
                auto* nf = AddrCaller{0x004cfab0}.this_call<rf::GFace*>(solid, &attrs);
                if (!nf) return false;
                nf->plane = face->plane;

                rf::Vector3 bmin = sub[0].vertex->pos, bmax = sub[0].vertex->pos;
                for (auto& sv : sub) {
                    AddrCaller{0x004e0140}.this_call<rf::GFaceVertex*>(
                        nf, sv.vertex, sv.u, sv.v, sv.lu, sv.lv);
                    auto& p = sv.vertex->pos;
                    if (p.x < bmin.x) bmin.x = p.x;
                    if (p.y < bmin.y) bmin.y = p.y;
                    if (p.z < bmin.z) bmin.z = p.z;
                    if (p.x > bmax.x) bmax.x = p.x;
                    if (p.y > bmax.y) bmax.y = p.y;
                    if (p.z > bmax.z) bmax.z = p.z;
                }
                nf->bounding_box_min = bmin;
                nf->bounding_box_max = bmax;
                return true;
            };

            bool made_pos = create_sub_face(pos_verts, pos_group_id);
            bool made_neg = create_sub_face(neg_verts, neg_group_id);
            if (made_pos) pos_count++;
            if (made_neg) neg_count++;

            // Remove original straddling face from the solid
            // FUN_004ce2a0: __thiscall VList::remove(GFace*) — unlinks from face_list
            // FUN_004dfc70: __cdecl — frees face vertices, decals, face to pool
            //               (handles null which_room/which_bbox safely)
            AddrCaller{0x004ce2a0}.this_call<void>(&solid->face_list, face);
            AddrCaller{0x004dfc70}.c_call<void>(face);
        }
    }

    return {pos_count, neg_count};
}

// Ear clipping triangulation for a simple polygon defined by 2D-projected points.
// Returns triangle index triplets into the original vertex array.
// Works correctly for convex, concave, and complex polygon shapes.
static std::vector<std::array<int, 3>> ear_clip_triangulate(
    const std::vector<float>& px, const std::vector<float>& py, int n)
{
    std::vector<std::array<int, 3>> triangles;
    if (n < 3) return triangles;
    triangles.reserve(n - 2);

    // Working list of remaining vertex indices
    std::vector<int> poly(n);
    for (int i = 0; i < n; i++) poly[i] = i;

    // Compute signed area to determine winding
    float area = 0.0f;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        area += px[poly[i]] * py[poly[j]];
        area -= px[poly[j]] * py[poly[i]];
    }

    // Ensure consistent winding for ear clipping (CCW = positive area)
    if (area < 0.0f) {
        std::reverse(poly.begin(), poly.end());
        area = -area;
    }

    // Degenerate polygon check
    if (area < 1e-10f) {
        xlog::warn("[CapFace] Degenerate polygon: area={:.6f} n={}", area, n);
        return triangles;
    }

    // 2D cross product of vectors (b-a) and (c-a)
    auto cross2d = [&](int a, int b, int c) -> float {
        return (px[b] - px[a]) * (py[c] - py[a]) - (py[b] - py[a]) * (px[c] - px[a]);
    };

    // Strict point-in-triangle test (excludes edges to avoid false rejections)
    auto point_in_tri = [&](int p, int a, int b, int c) -> bool {
        float d1 = (px[p] - px[b]) * (py[a] - py[b]) - (px[a] - px[b]) * (py[p] - py[b]);
        float d2 = (px[p] - px[c]) * (py[b] - py[c]) - (px[b] - px[c]) * (py[p] - py[c]);
        float d3 = (px[p] - px[a]) * (py[c] - py[a]) - (px[c] - px[a]) * (py[p] - py[a]);
        constexpr float eps = 1e-6f;
        bool has_neg = (d1 < -eps) || (d2 < -eps) || (d3 < -eps);
        bool has_pos = (d1 > eps) || (d2 > eps) || (d3 > eps);
        return !(has_neg && has_pos);
    };

    int remaining = n;
    int max_iter = n * n;
    while (remaining > 3 && max_iter-- > 0) {
        bool found_ear = false;
        for (int i = 0; i < remaining; i++) {
            int prev_i = (i - 1 + remaining) % remaining;
            int next_i = (i + 1) % remaining;
            int a = poly[prev_i], b = poly[i], c = poly[next_i];

            float cross = cross2d(a, b, c);
            if (cross <= 1e-8f) continue;

            bool has_interior_point = false;
            for (int k = 0; k < remaining; k++) {
                if (k == prev_i || k == i || k == next_i) continue;
                if (point_in_tri(poly[k], a, b, c)) {
                    has_interior_point = true;
                    break;
                }
            }
            if (has_interior_point) continue;

            triangles.push_back({a, b, c});
            poly.erase(poly.begin() + i);
            remaining--;
            found_ear = true;
            break;
        }
        if (!found_ear) {
            xlog::warn("[CapFace] Ear clip stuck: remaining={} of {}", remaining, n);
            break;
        }
    }

    if (remaining == 3) {
        triangles.push_back({poly[0], poly[1], poly[2]});
    }

    return triangles;
}

// Find boundary loops of a solid — edges that appear in only one face (open boundary).
// In a closed manifold mesh, every half-edge (v0→v1) has a counterpart (v1→v0) in
// the adjacent face. After splitting, missing counterparts indicate the open boundary.
// Returns ordered vertex loops suitable for cap face creation.
static std::vector<std::vector<rf::GVertex*>> find_boundary_loops(rf::GSolid* solid)
{
    // Collect all half-edges as ordered (v0, v1) pairs
    using HalfEdge = std::pair<rf::GVertex*, rf::GVertex*>;
    std::set<HalfEdge> half_edges;

    for (auto& face : solid->face_list) {
        auto* start = face.edge_loop;
        if (!start) continue;
        auto* fv = start;
        do {
            half_edges.insert({fv->vertex, fv->next->vertex});
            fv = fv->next;
        } while (fv != start);
    }

    // Boundary half-edges: (v0, v1) where the reverse (v1, v0) is absent
    std::unordered_map<rf::GVertex*, rf::GVertex*> next_in_boundary;
    for (auto& he : half_edges) {
        if (half_edges.find({he.second, he.first}) == half_edges.end()) {
            next_in_boundary[he.first] = he.second;
        }
    }

    xlog::trace("[Boundary] {} total half-edges, {} boundary half-edges",
        half_edges.size(), next_in_boundary.size());

    // Chain boundary half-edges into closed loops
    std::unordered_set<rf::GVertex*> visited;
    std::vector<std::vector<rf::GVertex*>> loops;

    for (auto& [start_v, _] : next_in_boundary) {
        if (visited.count(start_v)) continue;

        std::vector<rf::GVertex*> loop;
        rf::GVertex* curr = start_v;
        while (true) {
            if (visited.count(curr)) break;
            visited.insert(curr);
            loop.push_back(curr);
            auto it = next_in_boundary.find(curr);
            if (it == next_in_boundary.end()) break;
            curr = it->second;
            if (curr == start_v) break; // closed loop
        }

        if (loop.size() >= 3) {
            loops.push_back(std::move(loop));
        }
    }

    return loops;
}

// Create triangulated cap faces from a boundary loop and add them to a solid.
// The loop is already ordered (from boundary half-edge chaining). We compute the
// best-fit plane normal via Newell method, orient it away from the piece interior,
// project to 2D, and triangulate via ear clipping.
//
// All geometry uses game's native pool allocators for safe cleanup.
static void add_cap_faces_from_loop(
    rf::GSolid* solid,
    const std::vector<rf::GVertex*>& loop,
    int bitmap_id,
    float texels_per_meter)
{
    int n = static_cast<int>(loop.size());
    if (n < 3) return;

    // Compute best-fit plane normal via Newell method
    rf::Vector3 normal{0.0f, 0.0f, 0.0f};
    for (int i = 0; i < n; i++) {
        auto& curr = loop[i]->pos;
        auto& next = loop[(i + 1) % n]->pos;
        normal.x += (curr.y - next.y) * (curr.z + next.z);
        normal.y += (curr.z - next.z) * (curr.x + next.x);
        normal.z += (curr.x - next.x) * (curr.y + next.y);
    }
    float len = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
    if (len < 1e-8f) return;
    normal.x /= len; normal.y /= len; normal.z /= len;

    // Compute loop centroid
    rf::Vector3 loop_center{0.0f, 0.0f, 0.0f};
    for (auto* v : loop) loop_center += v->pos;
    loop_center *= (1.0f / static_cast<float>(n));

    // Compute piece centroid from all face vertices to determine "inside"
    rf::Vector3 piece_center{0.0f, 0.0f, 0.0f};
    int vert_count = 0;
    for (auto& face : solid->face_list) {
        auto* start = face.edge_loop;
        if (!start) continue;
        auto* fv = start;
        do {
            piece_center += fv->vertex->pos;
            vert_count++;
            fv = fv->next;
        } while (fv != start);
    }
    if (vert_count > 0) piece_center *= (1.0f / static_cast<float>(vert_count));

    // Orient normal AWAY from piece interior (cap faces outward into the gap)
    rf::Vector3 to_center = piece_center - loop_center;
    if (normal.dot_prod(to_center) > 0.0f) {
        normal.x = -normal.x; normal.y = -normal.y; normal.z = -normal.z;
    }

    // Compute plane offset (average of -normal·pos for all loop vertices)
    float offset = 0.0f;
    for (auto* v : loop) offset -= normal.dot_prod(v->pos);
    offset /= static_cast<float>(n);

    // Build 2D coordinate frame on the cap plane
    rf::Vector3 right;
    if (std::abs(normal.y) < 0.9f)
        right = rf::Vector3{0.0f, 1.0f, 0.0f}.cross(normal);
    else
        right = rf::Vector3{1.0f, 0.0f, 0.0f}.cross(normal);
    right.normalize();
    rf::Vector3 forward = normal.cross(right);

    // Project loop vertices to 2D (loop is already ordered)
    std::vector<float> px(n), py(n);
    for (int i = 0; i < n; i++) {
        px[i] = loop[i]->pos.dot_prod(right);
        py[i] = loop[i]->pos.dot_prod(forward);
    }

    // Triangulate via ear clipping
    auto triangles = ear_clip_triangulate(px, py, n);

    xlog::trace("[CapLoop] n={} ear_clip produced {} triangles (expected {})",
        n, triangles.size(), n - 2);

    // Fallback: fan from vertex 0 (works for convex polygons)
    if (triangles.empty() && n >= 3) {
        xlog::warn("[CapLoop] Ear clip failed, falling back to fan");
        for (int i = 1; i < n - 1; i++) {
            triangles.push_back({0, i, i + 1});
        }
    }
    if (triangles.empty()) return;

    // Reuse existing boundary loop vertices — they're already in the solid's
    // vertex array (shared with the adjacent faces). This creates proper manifold
    // topology at the cap boundary.

    rf::GFaceAttributes attrs{};
    attrs.bitmap_id = bitmap_id;
    attrs.surface_index = -1;

    // UV mapping: project relative to loop centroid
    auto compute_uv = [&](rf::GVertex* v) -> std::pair<float, float> {
        rf::Vector3 d = v->pos - loop_center;
        return {d.dot_prod(right) * texels_per_meter,
                d.dot_prod(forward) * texels_per_meter};
    };

    for (auto& tri : triangles) {
        auto* face = AddrCaller{0x004cfab0}.this_call<rf::GFace*>(solid, &attrs);
        if (!face) continue;

        face->plane.normal = normal;
        face->plane.offset = offset;

        rf::Vector3 tri_positions[3];
        for (int vi = 0; vi < 3; vi++) {
            auto* v = loop[tri[vi]];
            auto [u, uv] = compute_uv(v);
            AddrCaller{0x004e0140}.this_call<rf::GFaceVertex*>(face, v, u, uv, 0.0f, 0.0f);
            tri_positions[vi] = v->pos;
        }

        // Set face bounding box (FUN_004e0140 does NOT set this)
        face->bounding_box_min = tri_positions[0];
        face->bounding_box_max = tri_positions[0];
        for (int vi = 1; vi < 3; vi++) {
            auto& p = tri_positions[vi];
            if (p.x < face->bounding_box_min.x) face->bounding_box_min.x = p.x;
            if (p.y < face->bounding_box_min.y) face->bounding_box_min.y = p.y;
            if (p.z < face->bounding_box_min.z) face->bounding_box_min.z = p.z;
            if (p.x > face->bounding_box_max.x) face->bounding_box_max.x = p.x;
            if (p.y > face->bounding_box_max.y) face->bounding_box_max.y = p.y;
            if (p.z > face->bounding_box_max.z) face->bounding_box_max.z = p.z;
        }
    }
}

// Replace particle effects with geometry debris for rock-material breakable brushes.
// Extracts the room's faces from g_level_solid, then repeatedly bisects the extracted
// solid using plane clipping. Faces straddling the cut plane are split into two sub-faces
// with interpolated texture coordinates — no face extends past the cut boundary.
// Boundary loops (open edges from the split) are detected topologically and capped with
// triangulated crater-texture faces. Each piece is spawned as a falling OT_DEBRIS object.
static void do_material_debris_shatter(rf::GRoom* room, const rf::Vector3& pos,
                                       const BreakableMaterialConfig& cfg, BreakableMaterialState& state)
{
    if (!room) return;
    auto* solid = rf::g_level_solid;
    if (!solid) return;

    int face_count = room->face_list.size();
    if (face_count <= 0) return;

    // Save room center (world-space position for debris spawn)
    rf::Vector3 room_center;
    room_center.x = (room->bbox_min.x + room->bbox_max.x) * 0.5f;
    room_center.y = (room->bbox_min.y + room->bbox_max.y) * 0.5f;
    room_center.z = (room->bbox_min.z + room->bbox_max.z) * 0.5f;

    xlog::trace("[MaterialShatter] room uid={} faces={} center=({:.2f},{:.2f},{:.2f})",
        room->uid, face_count, room_center.x, room_center.y, room_center.z);

    // Tag all room faces and extract from level solid
    for (auto& face : room->face_list) {
        face.attributes.group_id = k_debris_group_base;
    }

    // Invalidate room rendering before extraction frees faces from room's list
    invalidate_room_rendering(room);

    auto* extracted = solid->extract_faces_by_group(k_debris_group_base);
    if (!extracted || extracted->face_list.empty()) {
        if (extracted) AddrCaller{0x004136e0}.this_call(extracted, 1);
        rf::g_cache_clear();
        play_material_break_sound(cfg, state, pos);
        return;
    }

    AddrCaller{0x004d1330}.this_call(extracted, &rf::g_geomod_bbox_temp);
    compute_solid_bounds(extracted);

    {
        // Load cap texture before the cutting loop (needed for per-cut capping)
        if (!state.cap_bm_loaded) {
            state.cap_bm_loaded = true;
            state.cap_bm = rf::bm::load(cfg.cap_texture, -1, true);
            xlog::info("[MaterialShatter] loaded cap texture '{}' handle={}", cfg.cap_texture, state.cap_bm);
        }
        int crater_tex = (state.cap_bm >= 0) ? state.cap_bm : rf::g_geomod_texture_index;
        float tpm = cfg.cap_texels_per_meter;

        // Cap open boundary loops on a solid with crater-textured triangulated faces.
        // Each boundary loop (half-edges with no counterpart) is detected topologically
        // and closed via ear-clipping triangulation.
        auto cap_boundaries = [&](rf::GSolid* s) {
            auto loops = find_boundary_loops(s);
            for (auto& loop : loops) {
                xlog::trace("[MaterialShatter]   cap loop vertices={}", loop.size());
                add_cap_faces_from_loop(s, loop, crater_tex, tpm);
            }
        };

        // Work queue (FIFO): breadth-first bisection ensures all pieces at the same
        // subdivision level are split before going deeper, producing more uniform sizes.
        std::deque<rf::GSolid*> work_queue;
        std::vector<rf::GSolid*> final_pieces;
        work_queue.push_back(extracted);

        int total_cuts = 0;

        while (!work_queue.empty() && total_cuts < cfg.debris.max_subdivisions) {
            auto* chunk = work_queue.front();
            work_queue.pop_front();

            // Too small or too few faces to split further
            if (chunk->bounding_sphere_radius <= cfg.debris.min_bsphere_radius ||
                chunk->face_list.size() < cfg.debris.min_faces_to_split) {
                final_pieces.push_back(chunk);
                continue;
            }

            // Choose cutting plane along the chunk's longest bounding box axis.
            // This splits the chunk roughly in half by volume, producing more even
            // pieces than a random direction (which can shave off tiny slivers).
            rf::Vector3 extents;
            extents.x = chunk->bbox_max.x - chunk->bbox_min.x;
            extents.y = chunk->bbox_max.y - chunk->bbox_min.y;
            extents.z = chunk->bbox_max.z - chunk->bbox_min.z;

            rf::Vector3 plane_normal{0.0f, 0.0f, 0.0f};
            if (extents.x >= extents.y && extents.x >= extents.z)
                plane_normal.x = 1.0f;
            else if (extents.y >= extents.x && extents.y >= extents.z)
                plane_normal.y = 1.0f;
            else
                plane_normal.z = 1.0f;

            float plane_offset = -plane_normal.dot_prod(chunk->bounding_sphere_center);

            // Classify faces and clip those straddling the plane.
            // Straddling faces are split into two sub-faces at the plane boundary
            // with interpolated texture coords. Originals are removed.
            auto [pos_count, neg_count] = split_faces_by_plane(
                chunk, plane_normal, plane_offset,
                k_debris_group_base, k_debris_group_base + 1);

            // If all faces ended up on one side, can't split — finalize
            if (pos_count == 0 || neg_count == 0) {
                final_pieces.push_back(chunk);
                continue;
            }

            // Extract negative-side faces into a new solid
            auto* piece = chunk->extract_faces_by_group(k_debris_group_base + 1);
            total_cuts++;

            // Cap boundary loops on both halves IMMEDIATELY after this cut.
            // This ensures each boundary loop comes from exactly one (planar) cut.
            // Without this, later cuts can create vertices shared between boundaries
            // from different cut planes, producing non-planar loops that fail
            // ear-clipping triangulation (self-intersecting 2D projections).
            if (piece && !piece->face_list.empty()) {
                cap_boundaries(piece);
                compute_solid_bounds(piece);
                work_queue.push_back(piece);
            }
            else if (piece) {
                AddrCaller{0x004136e0}.this_call(piece, 1);
            }

            // Positive-side faces remain in chunk — cap and recompute bounds
            cap_boundaries(chunk);
            compute_solid_bounds(chunk);
            work_queue.push_back(chunk);
        }

        // Any remaining work queue items become final pieces
        for (auto* c : work_queue) {
            final_pieces.push_back(c);
        }

        xlog::trace("[MaterialShatter] {} cuts produced {} pieces", total_cuts, final_pieces.size());

        // Translate each piece's vertices from world space to local space (centered
        // on the piece's bounding sphere center). The D3D11 movable solid renderer
        // applies: world_pos = orient * vertex + obj->pos. For vertices to appear at
        // their correct world positions, they must be in local space so that
        // local_vertex + obj->pos = original_world_vertex.
        float upward_velocity = cfg.upward_velocity;
        float horizontal_scatter = cfg.horizontal_scatter;
        float explosion_push_speed = cfg.explosion_push_speed;

        bool from_explosion = g_breaking_from_explosion;
        g_breaking_from_explosion = false;

        auto rand_range = [](float lo, float hi) {
            return lo + (hi - lo) * (static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX));
        };

        for (auto* piece : final_pieces) {
            if (!piece || piece->face_list.empty()) continue;

            // FUN_004d1330 already translated all vertices to local space relative
            // to room_center. Each piece's bounding_sphere_center is now a small
            // offset from (0,0,0) indicating its position relative to the room origin.
            // World-space position = room_center + local_offset.
            rf::Vector3 local_offset = piece->bounding_sphere_center;
            rf::Vector3 world_center;
            world_center.x = room_center.x + local_offset.x;
            world_center.y = room_center.y + local_offset.y;
            world_center.z = room_center.z + local_offset.z;

            // Collect unique vertices from faces and translate to piece-local space
            std::unordered_set<rf::GVertex*> unique_verts;
            for (auto& face : piece->face_list) {
                auto* fv = face.edge_loop;
                if (!fv) continue;
                auto* start = fv;
                do {
                    unique_verts.insert(fv->vertex);
                    fv = fv->next;
                } while (fv != start);
            }

            for (auto* v : unique_verts) {
                v->pos.x -= local_offset.x;
                v->pos.y -= local_offset.y;
                v->pos.z -= local_offset.z;
            }

            piece->bbox_min.x -= local_offset.x;
            piece->bbox_min.y -= local_offset.y;
            piece->bbox_min.z -= local_offset.z;
            piece->bbox_max.x -= local_offset.x;
            piece->bbox_max.y -= local_offset.y;
            piece->bbox_max.z -= local_offset.z;
            piece->bounding_sphere_center = {0.0f, 0.0f, 0.0f};

            // Call FUN_004d1330 on the final piece-local geometry to set up
            // lightmap data and BSP tree info needed for rendering
            AddrCaller{0x004d1330}.this_call(piece, &rf::g_geomod_bbox_temp);

            rf::DebrisCreateStruct dcs{};
            dcs.pos = world_center;
            dcs.orient = rf::identity_matrix;

            if (from_explosion) {
                // Push outward from explosion center, per-piece direction
                rf::Vector3 push_dir{0.0f, 1.0f, 0.0f};
                rf::Vector3 d;
                d.x = dcs.pos.x - pos.x;
                d.y = dcs.pos.y - pos.y;
                d.z = dcs.pos.z - pos.z;
                float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
                if (len > 0.001f) {
                    push_dir.x = d.x / len;
                    push_dir.y = d.y / len;
                    push_dir.z = d.z / len;
                }
                dcs.vel.x = push_dir.x * explosion_push_speed + rand_range(-horizontal_scatter, horizontal_scatter);
                dcs.vel.y = push_dir.y * explosion_push_speed + upward_velocity;
                dcs.vel.z = push_dir.z * explosion_push_speed + rand_range(-horizontal_scatter, horizontal_scatter);
            }
            else {
                // Direct hit: upward + random horizontal scatter
                dcs.vel.x = rand_range(-horizontal_scatter, horizontal_scatter);
                dcs.vel.y = upward_velocity;
                dcs.vel.z = rand_range(-horizontal_scatter, horizontal_scatter);
            }

            dcs.spin = {0.0f, 0.0f, 0.0f};
            dcs.lifetime_ms = 10000;
            dcs.material = 1;
            dcs.explosion_index = -1;
            dcs.debris_flags = rf::DF_BOUNCE | rf::DF_VERTEX_TRANSFORM | rf::DF_OWNS_SOLID;
            dcs.obj_flags = rf::OF_NO_COLLIDE_REGISTER | rf::OF_WEAPON_ONLY_COLLIDE;
            dcs.room = nullptr;
            dcs.iss = resolve_material_iss(cfg);

            rf::geo_debris_obj_create(-1, piece, &dcs);
        }
    }

    rf::g_cache_clear();

    // Spawn particle explosion from explosion.tbl at break position
    if (cfg.explosion_name) {
        rf::Vector3 explosion_pos = room_center;
        rf::Vector3 up_dir{0.0f, 1.0f, 0.0f};
        rf::particle_explosion_create(cfg.explosion_name, &explosion_pos, &up_dir, 1.0f, room, 0);
    }

    play_material_break_sound(cfg, state, pos);
}

// Radius damage hook at 0x00492090 in room_apply_radius_damage (FUN_00491f50).
// At this point: ESI = GRoom*, ST(0) = distance fraction, [EBP+0x14] = damage, EDI = pos.
// Original: FMUL [EBP+0x14]; FSUBR [ESI+0x94]; FSTP [ESI+0x94]; JMP 0x4920A3 (17 bytes).
//
// SubHook/CodeInjection CANNOT hook here because SubHook fails on FPU opcodes (0xD8-0xDF).
// Instead, we use AsmWriter to overwrite the 17 bytes with a JMP to a dynamically-built
// trampoline (in radius_damage_trampoline_code) that:
//   1. Pops FPU ST(0) (distance fraction) to the stack, then passes it as a cdecl arg
//   2. Calls the C++ helper below to compute material-scaled damage and apply it
//   3. Jumps to stock life check at 0x4920A3
// The stock code after 0x4920A3 handles break for ALL materials: glass_kill → validate_destroy
// (queues room for destruction) → glass_sound (suppressed for non-glass) → glass_shards
// (suppressed for non-glass). The destroy queue processor fully tears down the room.

static void __cdecl room_damage_material_helper(rf::GRoom* room, float damage, float fraction)
{
    float scaled = fraction * damage;

    if (room->material_type != rf::DetailMaterial::Glass) {
        float factor = get_material_damage_factor(room->material_type, g_current_radius_damage_type);
        scaled *= factor;
    }

    room->life -= scaled;

    xlog::trace("[Material] RADIUS DMG: uid={} mat={} life={:.1f} scaled={:.1f}",
        room->uid, static_cast<int>(room->material_type), room->life, scaled);
}

// Direct hit damage hook at 0x004c4fe7 in FUN_004c4ec0 (projectile/weapon hit handler).
// At this point: EDI = GRoom*, ESI = entity, [ESP+0x14] = damage (float).
// Original instruction: TEST byte ptr [EDX+0x264], 0x40 (invincibility check, 7 bytes).
// Per-projectile deduplication is handled earlier in glass_decal_material_injection (0x004c4f85).
// This hook only scales damage and kills the projectile for non-glass materials.
// Stock code handles the break via FUN_004c4b20 → validate_destroy → glass_sound → glass_shards.
CodeInjection direct_hit_material_injection{
    0x004c4fe7,
    [](auto& regs) {
        auto* room = reinterpret_cast<rf::GRoom*>(static_cast<void*>(regs.edi));

        auto* mat_cfg = get_material_config(room->material_type);
        if (mat_cfg) {
            // Scale damage by material factor
            float* damage_ptr = reinterpret_cast<float*>(regs.esp + 0x14);
            float orig = *damage_ptr;
            *damage_ptr *= mat_cfg->direct_hit_factor;
            xlog::trace("[Material] DIRECT HIT: uid={} mat={} life={:.1f} dmg={:.1f}->{:.1f}",
                room->uid, static_cast<int>(room->material_type), room->life, orig, *damage_ptr);

            // Consume the projectile: zero its velocity so the substep loop removes it,
            // and set lifetime to -1 so it's destroyed. This matches how stock code handles
            // projectile hits on normal (non-detail) geometry.
            void* proj = static_cast<void*>(regs.esi);
            auto proj_addr = reinterpret_cast<uintptr_t>(proj);
            *reinterpret_cast<float*>(proj_addr + 0x34) = -1.0f;   // lifetime → destroy
            *reinterpret_cast<float*>(proj_addr + 0x1b0) = 0.0f;   // velocity → stop substep
        }
        // Fall through — trampoline executes original TEST instruction, stock code handles the rest
    },
};

// Hook inside glass_kill (FUN_00490b30) after validate_destroy succeeds, just before glass_sound
// is called. At this point EDI = param_1 = GFace* (head of room's face_list). We read
// face->which_room->material_type and set g_breaking_material so that the downstream
// glass_sound/glass_shards hooks use the correct material. This is essential for the network
// path: when a remote client receives a glass_kill packet, the damage trampoline never ran,
// so g_breaking_material would otherwise remain at its default (Glass).
CodeInjection glass_kill_material_injection{
    0x00490b5d,
    [](auto& regs) {
        auto* face = reinterpret_cast<rf::GFace*>(static_cast<void*>(regs.edi));
        if (face && face->which_room) {
            g_breaking_material = face->which_room->material_type;
            g_breaking_room = face->which_room;
        }
        else {
            // Leaving the previous break's room behind would aim this break's effects, stats and
            // When_Destroyed commit at the wrong brush. The material is read unguarded by the
            // downstream glass suppression hooks, so it goes back to the stock default too.
            g_breaking_room = nullptr;
            g_breaking_material = rf::DetailMaterial::Glass;
        }
        // Detect if this break is from an explosion:
        // - Local: g_current_radius_damage_type >= 0 (set by capture_damage_type_injection)
        // - Network: force_in_multi=1 (glass_kill only called from packet when explosion_flag=1)
        bool force_in_multi = *reinterpret_cast<char*>(regs.esp + 0x14) != 0;
        g_breaking_from_explosion = (g_current_radius_damage_type >= 0) || force_in_multi;
    },
};

// The glass_kill packet handler (0x004723C3) picks one of two commit functions for a break a
// client is replaying: glass_kill (explosion flag set), which the injection above covers, or
// FUN_00491ed0 (flag clear), which the local direct-hit setter at 0x004c4f85 never precedes on a
// client — so without this the break globals would still describe the previous break.
CodeInjection glass_kill_packet_material_injection{
    0x00491efe,
    [](auto& regs) {
        // This branch is only taken when the packet's explosion flag is clear. Glass rooms never
        // reach the consumer that resets this, so a stale true would push later debris outward.
        g_breaking_from_explosion = false;

        auto* face = reinterpret_cast<rf::GFace*>(static_cast<void*>(regs.edi));
        if (face && face->which_room) {
            g_breaking_material = face->which_room->material_type;
            g_breaking_room = face->which_room;
        }
        else {
            g_breaking_room = nullptr;
            g_breaking_material = rf::DetailMaterial::Glass;
        }
    },
};

// Hook at glass_sound entry (FUN_00490c00) to suppress glass break audio for non-glass materials
// and play material-appropriate break effects instead. This is called from both glass_kill
// (radius damage path) and FUN_00491ed0 (direct hit path).
// At entry: [ESP+4] = pos parameter (Vector3*). Skip to RET at 0x490c45 for non-glass.
//
// For non-glass materials: do_material_debris_shatter extracts faces from g_level_solid, freeing the
// original face objects. Stock glass_kill tries to send the glass_kill packet AFTER glass_sound
// returns, reading face->which_room->room_index — but the face is freed by then (dangling ptr).
// Fix: if we're the server in multiplayer, send the packet HERE before extraction.
// Stock glass_kill's send will be skipped (freed face->which_room reads as null).
CodeInjection glass_sound_entry_injection{
    0x00490c00,
    [](auto& regs) {
        auto* pos = *reinterpret_cast<rf::Vector3**>(regs.esp + 4);

        // Stats stream: one detail_brush_destroyed per committed break, for
        // EVERY material (glass included). glass_sound is the sole point both break paths
        // reach, and only after validate_destroy has succeeded, so it fires exactly once
        // per kill. Server-side only: a client applying a received glass_kill packet also
        // runs glass_sound, so gating on the deciding server avoids emitting the broadcast
        // twice. material is g_breaking_material; the room uid and best-effort attribution
        // come from the globals finalized above. Read before the non-glass shatter below,
        // which frees the room's faces. on_detail_brush_destroyed self-gates on a
        // stats-enabled server.
        if (afstats_break_attr_enabled() && g_breaking_room && pos) {
            // Trust the stashed attribution only when its damage entry landed on THIS tick:
            // a break not driven by damage this frame (level logic reaching glass_sound, or a
            // stale capture from an earlier near-miss) would otherwise inherit whoever last
            // damaged glass. Wrong attribution is worse than null, so a tick mismatch reports
            // the break as anonymous. material and room_uid are always fresh, so they stand.
            const bool fresh = g_break_attr.capture_tick == GetTickCount64();
            afstats::on_detail_brush_destroyed(
                static_cast<uint8_t>(g_breaking_material), g_breaking_room->uid,
                fresh ? g_break_attr.killer_handle : -1, fresh ? g_break_attr.weapon_type : -1,
                fresh ? g_break_attr.damage_type : -1, *pos);
        }

        // The broken brush can open a ceiling over a blocked-by-geometry weather region.
        if (g_breaking_room) {
            const rf::Vector3 room_center = (g_breaking_room->bbox_min + g_breaking_room->bbox_max) * 0.5f;
            const float dx = g_breaking_room->bbox_max.x - g_breaking_room->bbox_min.x;
            const float dz = g_breaking_room->bbox_max.z - g_breaking_room->bbox_min.z;
            weather_notify_geomod(room_center, 0.5f * std::sqrt(dx * dx + dz * dz));

            // When_Destroyed: glass_sound is the one point both break paths reach, and only
            // after validate_destroy succeeded, so a detail brush commits here exactly once
            // whatever its material. Event activation runs synchronously, so a chained break
            // inside it would leave these globals pointing at the other room; the shatter and
            // glass_kill packet below still need this room's values, so stash and restore them.
            const rf::DetailMaterial saved_material = g_breaking_material;
            rf::GRoom* const saved_room = g_breaking_room;
            note_brush_break(saved_room->uid);
            g_breaking_material = saved_material;
            g_breaking_room = saved_room;
        }

        auto* mat_cfg = get_material_config(g_breaking_material);
        if (mat_cfg && g_breaking_room) {
            auto* mat_state = get_material_state(g_breaking_material);

            // Send glass_kill packet before face extraction frees the original faces
            if (rf::is_multi && rf::is_server) {
                rf::Vector3 zero_vec{0.0f, 0.0f, 0.0f};
                rf::Vector3 break_pos = *pos;
                rf::send_glass_kill_packet(g_breaking_room->uid, &break_pos, &zero_vec, true);
            }
            if (g_breaking_room->no_debris) {
                // VFX/sounds only, no geometry debris
                play_material_break_sound(*mat_cfg, *mat_state, *pos);
            }
            else {
                // Geometry debris shatter: split brush into falling pieces
                do_material_debris_shatter(g_breaking_room, *pos, *mat_cfg, *mat_state);
            }
            regs.eip = 0x00490c45; // skip glass sound, jump to RET
        }
    },
};

// Hook in FUN_004c4ec0 to suppress glass-specific impact overlay for non-glass breakable brushes
// AND deduplicate per-projectile substep hits (effects + damage).
// At 0x004c4f85: MOV ECX,ESI / PUSH EAX / CALL 0x0048a230 applies glass crack visual overlay.
// ESI = projectile entity (Weapon*), EDI = GRoom*.
// This is the EARLIEST hook point in the detail room path where both ESI and EDI are set,
// so we perform deduplication here to prevent effects (decal, sound, vclip) from playing
// multiple times per physics substep. For duplicate hits, we jump directly to the "return 1"
// epilogue at 0x004c5023, skipping ALL effects and damage.
// Also sets g_breaking_material early so the FUN_004c54b0 entry hook (below) can use it —
// FUN_004c5820 is called right after this (at 0x004c4f9b) and creates a GDecal via FUN_004c54b0.
CodeInjection glass_decal_material_injection{
    0x004c4f85,
    [](auto& regs) {
        auto* room = reinterpret_cast<rf::GRoom*>(static_cast<void*>(regs.edi));
        // Set material globals EARLY (before FUN_004c5820 runs at 0x004c4f9b)
        g_breaking_material = room->material_type;
        g_breaking_room = room;

        // Explosive projectile direct hits (e.g. rocket) should push debris outward too.
        // Check if the projectile's weapon type has a damage radius > 0.
        auto* weapon = reinterpret_cast<rf::Weapon*>(static_cast<void*>(regs.esi));
        g_breaking_from_explosion = (weapon && weapon->info && weapon->info->damage_radius > 0.0f);

        // Stash direct-hit attribution for the stats stream: the shooter is the
        // projectile's parent entity, and the weapon and its damage type come off the
        // weapon info. Consumed at the server-side break commit (glass_sound).
        if (weapon && afstats_break_attr_enabled()) {
            g_break_attr.killer_handle = weapon->parent_handle;
            g_break_attr.weapon_type = weapon->info_index;
            g_break_attr.damage_type = weapon->info ? weapon->info->damage_type : -1;
            g_break_attr.capture_tick = GetTickCount64();
        }

        if (room->material_type != rf::DetailMaterial::Glass) {
            auto* proj = reinterpret_cast<uint8_t*>(static_cast<void*>(regs.esi));
            float lifetime = *reinterpret_cast<float*>(proj + 0x34);

            // Per-projectile substep dedup: direct_hit_material_injection sets lifetime
            // to -1.0f after the first hit. On subsequent substeps the projectile is still
            // in the substep loop but we detect it was already handled.
            if (lifetime < 0.0f) {
                regs.eip = 0x004c5023; // return 1 epilogue — skip everything
                return;
            }

            // Per-room dedup: some weapons create multiple projectile entities per shot
            // (e.g., pistol). Skip effects and damage for the second projectile hitting
            // the same room in the same frame. Kill it so it doesn't re-hit next substep.
            // Use a time window (50ms) instead of exact equality because GetTickCount64 has
            // ~15ms resolution and two projectiles may straddle a tick boundary.
            const uint64_t now = GetTickCount64();
            if (room == s_last_vfx_room && (now - s_last_vfx_tick) < 50) {
                *reinterpret_cast<float*>(proj + 0x34) = -1.0f;
                regs.eip = 0x004c5023;
                return;
            }
            s_last_vfx_room = room;
            s_last_vfx_tick = now;

            regs.eip = 0x004c4f8d; // skip glass crack overlay
        }
    },
};

// Hook at FUN_004c54b0 entry (weapon impact decal creation) to convert glass decals to scorch
// decals for non-glass breakable materials. FUN_004c5820 calls FUN_004c54b0 with is_glass=1
// for detail room hits. For non-glass materials, we change is_glass to 0 so a scorch (bullet hole)
// decal is created instead. At entry: [ESP+0x1C] = param7 (is_glass, char).
CodeInjection weapon_decal_glass_to_scorch_injection{
    0x004c54b0,
    [](auto& regs) {
        char* is_glass_ptr = reinterpret_cast<char*>(regs.esp + 0x1C);
        if (*is_glass_ptr == 1 && g_breaking_material != rf::DetailMaterial::Glass) {
            *is_glass_ptr = 0; // scorch decal instead of glass
        }
    },
};

// Hook at 0x004c590a inside FUN_004c5820 (weapon impact decal/VFX function) to skip the second
// "exit/backside" CALL FUN_004c54b0 for non-glass breakable materials. Stock code creates two
// decals for detail room hits (entry + exit glass cracks). For non-glass materials only the entry
// decal makes sense. Original instruction: CALL FUN_004c54b0 (5 bytes). Args are already pushed;
// skipping to 0x004c590f lets the ADD ESP,0x1c clean them up.
CodeInjection skip_exit_decal_for_non_glass{
    0x004c590a,
    [](auto& regs) {
        if (g_breaking_material != rf::DetailMaterial::Glass) {
            regs.eip = 0x004c590f; // skip CALL, fall through to stack cleanup
        }
    },
};

// Hook at 0x004c4fa3 in FUN_004c4ec0 (after FUN_004c5820 returns) to add impact VFX (sparks)
// for non-glass breakable detail brushes. Stock code skips impact vclips for detail room hits
// (the JNZ at 0x004c59a4 in FUN_004c5820 always skips them when cVar2=1 for detail rooms).
// At this point: ESI = entity (Weapon*), EDI = GRoom*.
// Original instruction: MOV ECX, [ESI+0x294] (6 bytes, loads weapon info ptr).
// This address is also the JZ target from 0x004c4f94 (effects-disabled skip), so we check
// the effects flag at [0x0064ecbb] to avoid creating VFX when effects are disabled.
CodeInjection detail_room_impact_vfx_injection{
    0x004c4fa3,
    [](auto& regs) {
        if (g_breaking_material != rf::DetailMaterial::Glass) {
            // Don't create VFX if effects are disabled (multiplayer/demo mode flag)
            if (*reinterpret_cast<uint8_t*>(0x0064ecbb) != 1) {
                auto* entity = reinterpret_cast<uint8_t*>(static_cast<void*>(regs.esi));

                // Check weapon is not underwater (matches FUN_004c5820 condition)
                if (!rf::weapon_is_underwater(entity)) {
                    int weapon_type = *reinterpret_cast<int*>(entity + 0x298);
                    int entity_base = *reinterpret_cast<int*>(entity); // equiv. to FUN_0040a490(entity)
                    rf::Vector3* hit_point = reinterpret_cast<rf::Vector3*>(entity + 0x1b4);
                    rf::Vector3* hit_normal = reinterpret_cast<rf::Vector3*>(entity + 0x1c0);
                    int parent_handle = *reinterpret_cast<int*>(entity + 0x30);

                    rf::weapon_create_impact_vclip(weapon_type, entity_base, nullptr, hit_point, hit_normal, parent_handle);
                }
            }
        }
    },
};

// Hook at glass_shards entry (FUN_00490c50) to suppress glass particle effects for non-glass
// materials. Called from glass_kill after validate_destroy has queued the room for destruction.
// First instruction: SUB ESP,0x90 (6 bytes). For non-glass: skip to RET at 0x490f53.
// NOTE: We do NOT clear face_list or geo_cache here — process_destroy needs face_list intact
// to iterate faces and properly remove them from collision/level structures via FUN_004cfb20.
// Visual cleanup (face_list clear + geo_cache null) is done by process_destroy_cleanup_injection.
CodeInjection glass_shards_entry_injection{
    0x00490c50,
    [](auto& regs) {
        if (g_breaking_material != rf::DetailMaterial::Glass) {
            regs.eip = 0x00490f53; // skip glass shards, jump to RET
        }
    },
};

// Hook inside process_destroy (FUN_004921f0) at 0x4922F2, AFTER all face processing
// (FUN_004cfb20 called for each face, removing from collision) and decal cleanup.
// At this point: EBX = GRoom* (room being destroyed).
// Original instruction: MOV EAX, [0x0076d034] (5 bytes, loads free room counter).
//
// Stock process_destroy does NOT invalidate the parent room's render cache or remove
// the detail room from parent->detail_rooms. For stock glass (alpha), this isn't an issue
// because alpha faces are in separate render batches and the alpha pass has its own handling.
// For opaque detail rooms (new for Alpine), the parent room's RoomRenderCache includes
// detail room faces baked in (via GRenderCacheBuilder::add_room recursion). We must:
//
// 1. Invalidate parent room's render cache: set GCache::state to 2.
//    This triggers a cache rebuild on the next render frame. The rebuild calls add_room,
//    which iterates detail_rooms — the destroyed room has empty face_list, so no faces
//    are added. Works for both D3D11 (RoomRenderCache) and D3D9 (stock GCache).
//
// 2. Clear destroyed room's rendering state: null room_to_render_with (prevents
//    render_detail from running), null geo_cache, clear face_list.
CodeInjection process_destroy_cleanup_injection{
    0x004922F2,
    [](auto& regs) {
        auto* room = reinterpret_cast<rf::GRoom*>(static_cast<void*>(regs.ebx));
        if (room) {
            // Keep the When_Destroyed registry consistent when a savegame restore tears
            // down rooms that were already broken. The break path sets this at glass_sound,
            // so this is a no-op there; it never fires links, only records the state.
            note_brush_break(room->uid, false);

            // Invalidate parent room's render cache so the destroyed detail room's faces
            // are excluded on the next cache rebuild. room_to_render_with may be stale during
            // save/load (the save restore at FUN_004b47a0 destroys killed rooms AFTER
            // level_init_post completes, so the pointer can reference freed memory from the
            // previous level). We validate it against the live room list before dereferencing.
            auto* parent = room->room_to_render_with;
            if (parent) {
                bool valid = false;
                auto* solid = rf::level.geometry;
                if (solid) {
                    for (auto& r : solid->all_rooms) {
                        if (r == parent) { valid = true; break; }
                    }
                }
                if (valid && parent->geo_cache) {
                    parent->geo_cache->state = 2;
                }
            }

            // Prevent direct rendering of this detail room
            room->room_to_render_with = nullptr;
            room->geo_cache = nullptr;
            room->face_list.clear();
        }
    },
};

// Hook inside pregame_glass handler (FUN_004767b0) at 0x004768ad.
// This handler runs when a client joins a multiplayer server and receives the state of
// previously-destroyed glass/breakable brushes. The stock handler removes faces from
// collision (via FUN_004cfb20 → FUN_004dfc70) but does NOT call process_destroy.
// FUN_004dfc70 already clears each face from the room's face_list, so face_list will
// be empty after this handler runs. However, for opaque detail rooms, the parent room's
// baked RoomRenderCache still contains the destroyed room's geometry. We must invalidate
// the parent's cache so it rebuilds without the destroyed room's faces.
//
// At this address (inside the "broken pane" branch, just before FUN_0040cb40 clears
// the processing queue): EBP = &glass_pane_array[index], so [EBP] = GRoom*.
CodeInjection pregame_glass_render_cleanup_injection{
    0x004768ad,
    [](auto& regs) {
        auto* room = *static_cast<rf::GRoom**>(static_cast<void*>(regs.ebp));
        if (room && room->is_detail) {
            // The D3D11 renderer's page_in_solid pre-builds parent room caches (RoomRenderCache)
            // which bake detail room faces into the parent via add_room. This runs BEFORE the
            // pregame handler, so the parent cache contains stale geometry from this destroyed room.
            // We can't use room_to_render_with to find the parent (it may be uninitialized at
            // pregame time). Instead, search all non-detail rooms' detail_rooms lists. A detail
            // room can appear in multiple parent rooms, so check all of them.
            auto* solid = rf::level.geometry;
            if (solid) {
                for (auto& r : solid->all_rooms) {
                    if (!r || r->is_detail || !r->geo_cache) continue;
                    for (auto* dr : r->detail_rooms) {
                        if (dr == room) {
                            r->geo_cache->state = 2; // mark dirty — rebuilt on next render
                            break;
                        }
                    }
                }
            }
            room->room_to_render_with = nullptr;
            room->geo_cache = nullptr;
        }
    },
};

// Support model. A geoable piece stays up if it touches, crosses or is buried in
// world geometry, a non-geoable detail brush or a terrain chunk, or touches another geoable
// piece that does.
// All face normals point out of solid volume (world faces into air, brush faces outward).
struct RF2SupportNode {
    rf::GRoom* room;
    std::vector<rf::GFace*> faces;
    rf::Vector3 bbox_min;
    rf::Vector3 bbox_max;
};

struct RF2SupportState {
    std::vector<bool> in_cluster; // connected to the target room through geoable contact
    std::vector<bool> supported;
};

static bool rf2_is_support_face(const rf::GFace& face)
{
    return !face.attributes.is_liquid() && face.attributes.portal_id <= 0;
}

static bool rf2_bboxes_overlap(const rf::Vector3& a_min, const rf::Vector3& a_max,
    const rf::Vector3& b_min, const rf::Vector3& b_max, float pad)
{
    return a_max.x + pad >= b_min.x && a_min.x - pad <= b_max.x &&
           a_max.y + pad >= b_min.y && a_min.y - pad <= b_max.y &&
           a_max.z + pad >= b_min.z && a_min.z - pad <= b_max.z;
}

// Point must lie on the face plane. Crossing-number test projected along the dominant axis.
static bool rf2_point_in_face(const rf::GFace& face, const rf::Vector3& p)
{
    float ax = std::abs(face.plane.normal.x);
    float ay = std::abs(face.plane.normal.y);
    float az = std::abs(face.plane.normal.z);
    auto project = [&](const rf::Vector3& v, float& u, float& w) {
        if (ax >= ay && ax >= az) { u = v.y; w = v.z; }
        else if (ay >= az) { u = v.x; w = v.z; }
        else { u = v.x; w = v.y; }
    };

    rf::GFaceVertex* fv = face.edge_loop;
    if (!fv) return false;
    float pu, pw;
    project(p, pu, pw);
    bool inside = false;
    do {
        rf::GFaceVertex* next = fv->next;
        if (!next || !fv->vertex || !next->vertex) return false;
        float u1, w1, u2, w2;
        project(fv->vertex->pos, u1, w1);
        project(next->vertex->pos, u2, w2);
        if ((w1 > pw) != (w2 > pw)) {
            float cross_u = u1 + (pw - w1) * (u2 - u1) / (w2 - w1);
            if (pu < cross_u) inside = !inside;
        }
        fv = next;
    } while (fv != face.edge_loop);
    return inside;
}

// True if any edge of `a` passes through or ends on the polygon of `b`.
static bool rf2_edges_cross_face(const rf::GFace& a, const rf::GFace& b)
{
    constexpr float eps = 0.01f;
    rf::GFaceVertex* fv = a.edge_loop;
    if (!fv) return false;
    do {
        rf::GFaceVertex* next = fv->next;
        if (!next || !fv->vertex || !next->vertex) return false;
        const rf::Vector3& p0 = fv->vertex->pos;
        const rf::Vector3& p1 = next->vertex->pos;
        float d0 = b.plane.distance_to_point(p0);
        float d1 = b.plane.distance_to_point(p1);
        bool in_plane = std::abs(d0) <= eps && std::abs(d1) <= eps;
        bool same_side = (d0 > eps && d1 > eps) || (d0 < -eps && d1 < -eps);
        if (!in_plane && !same_side) {
            float denom = d0 - d1;
            float t = std::abs(denom) > 1e-6f ? std::clamp(d0 / denom, 0.0f, 1.0f) : 0.0f;
            rf::Vector3 hit{p0.x + (p1.x - p0.x) * t, p0.y + (p1.y - p0.y) * t, p0.z + (p1.z - p0.z) * t};
            if (rf2_point_in_face(b, hit)) return true;
        }
        fv = next;
    } while (fv != a.edge_loop);
    return false;
}

static bool rf2_faces_touch(const rf::GFace& a, const rf::GFace& b)
{
    if (!face_bboxes_overlap(a, b)) return false;
    if (planes_are_coplanar(a.plane, b.plane)) return true;
    return rf2_edges_cross_face(a, b) || rf2_edges_cross_face(b, a);
}

struct RF2RayHit {
    float dist = std::numeric_limits<float>::max();
    bool inside = false;
    bool hit = false;
};

// Ray cast straight down from p. A nearest hit on a downward-facing face means p is inside
// the volume that face bounds.
static void rf2_ray_down_test(const rf::GFace& face, const rf::Vector3& p, RF2RayHit& best)
{
    if (p.x < face.bounding_box_min.x || p.x > face.bounding_box_max.x ||
        p.z < face.bounding_box_min.z || p.z > face.bounding_box_max.z ||
        face.bounding_box_min.y > p.y)
        return;
    float ny = face.plane.normal.y;
    if (std::abs(ny) < 1e-4f) return;
    float dist = face.plane.distance_to_point(p) / ny;
    if (dist <= 0.001f || dist >= best.dist) return;
    if (!rf2_point_in_face(face, rf::Vector3{p.x, p.y - dist, p.z})) return;
    best.dist = dist;
    best.inside = ny < 0.0f;
    best.hit = true;
}

static bool rf2_point_inside_faces(const std::vector<rf::GFace*>& faces, const rf::Vector3& p)
{
    RF2RayHit best;
    for (rf::GFace* face : faces) {
        rf2_ray_down_test(*face, p, best);
    }
    return best.hit && best.inside;
}

static bool rf2_point_inside_room(rf::GRoom* room, const rf::Vector3& p)
{
    RF2RayHit best;
    for (rf::GFace& face : room->face_list) {
        if (rf2_is_support_face(face)) rf2_ray_down_test(face, p, best);
    }
    return best.hit && best.inside;
}

static bool rf2_point_in_world_solid(rf::GSolid* solid, const rf::Vector3& p)
{
    constexpr float pad = 0.1f;
    RF2RayHit best;
    for (auto& room : solid->all_rooms) {
        if (room->is_detail) continue;
        if (p.x < room->bbox_min.x - pad || p.x > room->bbox_max.x + pad ||
            p.z < room->bbox_min.z - pad || p.z > room->bbox_max.z + pad ||
            room->bbox_min.y - pad > p.y)
            continue;
        for (rf::GFace& face : room->face_list) {
            if (rf2_is_support_face(face)) rf2_ray_down_test(face, p, best);
        }
    }
    // The world is sealed, so nothing below p means p is outside every room.
    return !best.hit || best.inside;
}

static rf::Vector3 rf2_face_center(const rf::GFace& face)
{
    rf::Vector3 sum{0.0f, 0.0f, 0.0f};
    int count = 0;
    rf::GFaceVertex* fv = face.edge_loop;
    if (!fv) return sum;
    do {
        if (fv->vertex) {
            sum.x += fv->vertex->pos.x;
            sum.y += fv->vertex->pos.y;
            sum.z += fv->vertex->pos.z;
            count++;
        }
        fv = fv->next;
    } while (fv && fv != face.edge_loop);
    if (count > 0) {
        sum.x /= count;
        sum.y /= count;
        sum.z /= count;
    }
    return sum;
}

// Without any surface contact, one piece is either fully inside the other or apart,
// so a single sample point settles containment.
static bool rf2_nodes_touch(const RF2SupportNode& a, const RF2SupportNode& b)
{
    if (!rf2_bboxes_overlap(a.bbox_min, a.bbox_max, b.bbox_min, b.bbox_max, 0.1f)) return false;
    for (rf::GFace* fa : a.faces) {
        for (rf::GFace* fb : b.faces) {
            if (rf2_faces_touch(*fa, *fb)) return true;
        }
    }
    return rf2_point_inside_faces(b.faces, rf2_face_center(*a.faces[0])) ||
           rf2_point_inside_faces(a.faces, rf2_face_center(*b.faces[0]));
}

static bool rf2_node_has_static_support(const RF2SupportNode& node, rf::GSolid* solid)
{
    constexpr float pad = 0.1f;
    rf::Vector3 sample = rf2_face_center(*node.faces[0]);
    for (auto& room : solid->all_rooms) {
        // Terrain chunks are always anchored: never support-model pieces, always static support.
        if (room->is_detail && room->is_geoable && !alpine_terrain_is_chunk_room(room)) continue;
        if (!rf2_bboxes_overlap(node.bbox_min, node.bbox_max, room->bbox_min, room->bbox_max, pad)) continue;
        for (rf::GFace& face : room->face_list) {
            if (!rf2_is_support_face(face)) continue;
            if (!rf2_bboxes_overlap(node.bbox_min, node.bbox_max, face.bounding_box_min, face.bounding_box_max, pad)) continue;
            for (rf::GFace* node_face : node.faces) {
                if (rf2_faces_touch(*node_face, face)) return true;
            }
        }
        if (room->is_detail && rf2_point_inside_room(room, sample)) return true;
    }
    return rf2_point_in_world_solid(solid, sample);
}

static void rf2_collect_room_nodes(rf::GRoom* room, std::vector<RF2SupportNode>& nodes)
{
    std::unordered_set<rf::GFace*> room_faces;
    for (rf::GFace& face : room->face_list) {
        if (rf2_is_support_face(face)) room_faces.insert(&face);
    }

    std::unordered_set<rf::GFace*> visited;
    for (rf::GFace& face : room->face_list) {
        if (!room_faces.count(&face) || !visited.insert(&face).second) continue;

        RF2SupportNode node{room, {&face}, face.bounding_box_min, face.bounding_box_max};
        for (size_t i = 0; i < node.faces.size(); i++) {
            rf::GFace* current = node.faces[i];
            node.bbox_min.x = std::min(node.bbox_min.x, current->bounding_box_min.x);
            node.bbox_min.y = std::min(node.bbox_min.y, current->bounding_box_min.y);
            node.bbox_min.z = std::min(node.bbox_min.z, current->bounding_box_min.z);
            node.bbox_max.x = std::max(node.bbox_max.x, current->bounding_box_max.x);
            node.bbox_max.y = std::max(node.bbox_max.y, current->bounding_box_max.y);
            node.bbox_max.z = std::max(node.bbox_max.z, current->bounding_box_max.z);

            rf::GFaceVertex* fv = current->edge_loop;
            if (!fv) continue;
            do {
                if (fv->vertex) {
                    for (int j = 0; j < fv->vertex->adjacent_faces.size(); j++) {
                        rf::GFace* adj = fv->vertex->adjacent_faces[j];
                        if (adj && room_faces.count(adj) && visited.insert(adj).second) {
                            node.faces.push_back(adj);
                        }
                    }
                }
                fv = fv->next;
            } while (fv && fv != current->edge_loop);
        }
        nodes.push_back(std::move(node));
    }
}

static std::vector<RF2SupportNode> rf2_collect_geoable_nodes(rf::GSolid* solid)
{
    std::vector<RF2SupportNode> nodes;
    for (auto& room : solid->all_rooms) {
        if (room->is_detail && room->is_geoable && !alpine_terrain_is_chunk_room(room)) {
            rf2_collect_room_nodes(room, nodes);
        }
    }
    return nodes;
}

// Support can only change for nodes connected to `target`, so everything else is skipped.
static RF2SupportState rf2_compute_support(const std::vector<RF2SupportNode>& nodes, rf::GRoom* target, rf::GSolid* solid)
{
    size_t n = nodes.size();
    std::vector<std::vector<size_t>> adjacency(n);
    for (size_t i = 0; i < n; i++) {
        for (size_t j = i + 1; j < n; j++) {
            if (rf2_nodes_touch(nodes[i], nodes[j])) {
                adjacency[i].push_back(j);
                adjacency[j].push_back(i);
            }
        }
    }

    RF2SupportState state{std::vector<bool>(n, false), std::vector<bool>(n, false)};
    std::vector<size_t> queue;
    for (size_t i = 0; i < n; i++) {
        if (nodes[i].room == target) {
            state.in_cluster[i] = true;
            queue.push_back(i);
        }
    }
    for (size_t q = 0; q < queue.size(); q++) {
        for (size_t nb : adjacency[queue[q]]) {
            if (!state.in_cluster[nb]) {
                state.in_cluster[nb] = true;
                queue.push_back(nb);
            }
        }
    }

    queue.clear();
    for (size_t i = 0; i < n; i++) {
        if (state.in_cluster[i] && rf2_node_has_static_support(nodes[i], solid)) {
            state.supported[i] = true;
            queue.push_back(i);
        }
    }
    for (size_t q = 0; q < queue.size(); q++) {
        for (size_t nb : adjacency[queue[q]]) {
            if (!state.supported[nb]) {
                state.supported[nb] = true;
                queue.push_back(nb);
            }
        }
    }
    return state;
}

// Taken before each boolean pass; compared afterwards to find pieces that lost support.
static void rf2_snapshot_support(rf::GRoom* target)
{
    g_rf2_pre_supported_faces.clear();
    g_rf2_target_supported_pre = false;
    rf::GSolid* solid = rf::level.geometry;
    if (!target || !solid || alpine_terrain_is_chunk_room(target)) return;

    auto nodes = rf2_collect_geoable_nodes(solid);
    auto state = rf2_compute_support(nodes, target, solid);
    for (size_t i = 0; i < nodes.size(); i++) {
        if (!state.supported[i]) continue;
        if (nodes[i].room == target) {
            g_rf2_target_supported_pre = true;
        }
        else {
            g_rf2_pre_supported_faces.insert(nodes[i].faces.begin(), nodes[i].faces.end());
        }
    }
}

// Gives each falling piece (target room or cascaded from another geoable room) an
// extraction index 0..n-1 in its faces' group_id and returns n.
static int rf2_mark_unsupported_pieces(rf::GRoom* target, rf::GSolid* solid)
{
    for (rf::GFace* face = solid->face_list.first(); face; face = solid->face_list.next(face)) {
        face->attributes.group_id = -1;
    }
    if (!target || alpine_terrain_is_chunk_room(target)) return 0;

    auto nodes = rf2_collect_geoable_nodes(solid);
    auto state = rf2_compute_support(nodes, target, solid);

    int target_pieces = 0;
    for (auto& node : nodes) {
        if (node.room == target) target_pieces++;
    }

    int extract_idx = 0;
    std::unordered_set<rf::GRoom*> rooms_with_kept_piece;
    for (size_t i = 0; i < nodes.size(); i++) {
        auto& node = nodes[i];
        bool extract = false;
        if (state.in_cluster[i] && !state.supported[i]) {
            if (node.room == target) {
                // Pieces made only of pre-existing faces (e.g. hollow brush interiors) stay.
                // A brush that was never supported keeps the legacy rule: it only sheds
                // pieces once the crater splits it.
                bool has_new_face = g_rf2_pre_boolean_faces.empty() ||
                    std::any_of(node.faces.begin(), node.faces.end(),
                        [](rf::GFace* f) { return !g_rf2_pre_boolean_faces.count(f); });
                extract = has_new_face && (g_rf2_target_supported_pre || target_pieces > 1);
            }
            else {
                extract = g_rf2_pre_supported_faces.count(node.faces[0]) != 0;
            }
        }

        if (!extract) {
            rooms_with_kept_piece.insert(node.room);
            continue;
        }
        for (rf::GFace* face : node.faces) {
            face->attributes.group_id = extract_idx;
        }
        extract_idx++;
        if (node.room != target &&
            std::find(g_rf2_cascaded_rooms.begin(), g_rf2_cascaded_rooms.end(), node.room) == g_rf2_cascaded_rooms.end()) {
            g_rf2_cascaded_rooms.push_back(node.room);
        }
    }

    // A pending room that fell completely has nothing left to carve.
    std::erase_if(g_rf2_pending_detail_rooms, [&](rf::GRoom* room) {
        return !rooms_with_kept_piece.count(room) &&
            std::find(g_rf2_cascaded_rooms.begin(), g_rf2_cascaded_rooms.end(), room) != g_rf2_cascaded_rooms.end();
    });

    xlog::debug("[RF2] support: {} geoable pieces, {} to extract ({} cascaded rooms, target supported pre={})",
        nodes.size(), extract_idx, g_rf2_cascaded_rooms.size(), g_rf2_target_supported_pre);
    return extract_idx;
}

static float get_hardness_scaled_padding()
{
    int hardness = std::clamp(rf::level.default_rock_hardness, 0, 100);
    float scale = std::pow(2.0f, (50.0f - hardness) / 50.0f);
    return geoable_bbox_base_padding * scale;
}

static bool sphere_overlaps_aabb(const rf::Vector3& center, float radius, const rf::Vector3& min,
                                 const rf::Vector3& max)
{
    float dx = std::max({min.x - center.x, 0.0f, center.x - max.x});
    float dy = std::max({min.y - center.y, 0.0f, center.y - max.y});
    float dz = std::max({min.z - center.z, 0.0f, center.z - max.z});
    return dx * dx + dy * dy + dz * dz <= radius * radius;
}

// Radius about the explosion pos bounding the crater solid as boolean State 0 (FUN_004dbdf0) places it:
// each vertex is rotated, scaled (only when scale > 1e-4) and offset by pos, then pushed along
// field_4C/field_58 by at most |push| * |v - pos| / (scale * bounding_sphere_radius). 0 = unknown.
static float get_crater_extent_radius(rf::GSolid* crater, float scale, const rf::Vector3& push_a,
    const rf::Vector3& push_b)
{
    constexpr float epsilon = 0.01f;
    if (!crater) return 0.0f;
    float max_len_sq = 0.0f;
    for (const rf::GVertex* vertex : crater->vertices) {
        max_len_sq = std::max(max_len_sq, vertex->pos.len_sq());
    }
    float radius = std::sqrt(max_len_sq) * (scale > 1e-4f ? scale : 1.0f);
    float push_base = scale * crater->bounding_sphere_radius;
    if (push_base > 0.0f) {
        for (const rf::Vector3* push : {&push_a, &push_b}) {
            float push_len = push->len();
            if (std::isfinite(push_len)) {
                radius *= 1.0f + push_len / push_base;
            }
        }
    }
    return std::isfinite(radius) ? radius + epsilon : 0.0f;
}

static float get_crater_extent_radius(const rf::GeomodParams& params)
{
    return get_crater_extent_radius(rf::geomod_get_crater_solid(params.shape_index), params.scale,
        params.field_4C, params.field_58);
}

// The boolean's face broad phase (FUN_004dc670) against the crater sphere, plus a face-plane distance cull.
static bool crater_overlaps_room_faces(const rf::Vector3& pos, float crater_radius, rf::GRoom* room)
{
    if (!sphere_overlaps_aabb(pos, crater_radius, room->bbox_min, room->bbox_max)) return false;
    for (rf::GFace& face : room->face_list) {
        if (sphere_overlaps_aabb(pos, crater_radius, face.bounding_box_min, face.bounding_box_max)
            && std::abs(face.plane.distance_to_point(pos)) <= crater_radius) {
            return true;
        }
    }
    return false;
}

// Rooms whose padded bbox contains pos come first, then rooms only reached by the crater extent
// (crater_radius, 0 = off). Each group is sorted by distance from pos to bbox center (closest first).
// A terrain chunk's bbox spans its whole height, so with a known extent it is reached by the extent only.
static std::vector<rf::GRoom*> find_overlapping_detail_rooms(const rf::Vector3& pos, float crater_radius)
{
    float padding = get_hardness_scaled_padding();
    std::vector<rf::GRoom*> result;
    std::vector<rf::GRoom*> extent_only;
    auto* solid = rf::level.geometry;
    if (!solid) return result;

    int geoable_count = 0;
    for (auto& room : solid->all_rooms) {
        if (!room->is_detail || !room->is_geoable) continue;
        geoable_count++;

        if (crater_radius > 0.0f && alpine_terrain_is_chunk_room(room)) {
            if (crater_overlaps_room_faces(pos, crater_radius, room)) {
                extent_only.push_back(room);
            }
            continue;
        }

        // Check if position is within room bbox + hardness-scaled padding
        bool in_x = pos.x >= room->bbox_min.x - padding && pos.x <= room->bbox_max.x + padding;
        bool in_y = pos.y >= room->bbox_min.y - padding && pos.y <= room->bbox_max.y + padding;
        bool in_z = pos.z >= room->bbox_min.z - padding && pos.z <= room->bbox_max.z + padding;

        if (in_x && in_y && in_z) {
            result.push_back(room);
        }
        else if (crater_radius > 0.0f && crater_overlaps_room_faces(pos, crater_radius, room)) {
            extent_only.push_back(room);
        }
    }

    if (geoable_count == 0) {
        xlog::debug("[RF2] find_rooms: no geoable detail rooms in level");
    }

    auto closest_first = [&pos](rf::GRoom* a, rf::GRoom* b) {
        auto center_a = rf::Vector3{
            (a->bbox_min.x + a->bbox_max.x) * 0.5f,
            (a->bbox_min.y + a->bbox_max.y) * 0.5f,
            (a->bbox_min.z + a->bbox_max.z) * 0.5f};
        auto center_b = rf::Vector3{
            (b->bbox_min.x + b->bbox_max.x) * 0.5f,
            (b->bbox_min.y + b->bbox_max.y) * 0.5f,
            (b->bbox_min.z + b->bbox_max.z) * 0.5f};
        float dx_a = pos.x - center_a.x, dy_a = pos.y - center_a.y, dz_a = pos.z - center_a.z;
        float dx_b = pos.x - center_b.x, dy_b = pos.y - center_b.y, dz_b = pos.z - center_b.z;
        float dist_sq_a = dx_a * dx_a + dy_a * dy_a + dz_a * dz_a;
        float dist_sq_b = dx_b * dx_b + dy_b * dy_b + dz_b * dz_b;
        return dist_sq_a < dist_sq_b;
    };
    std::sort(result.begin(), result.end(), closest_first);
    std::sort(extent_only.begin(), extent_only.end(), closest_first);
    result.insert(result.end(), extent_only.begin(), extent_only.end());

    return result;
}

// Replay geomod camera shake for nearby players.
static void replay_geomod_camera_shake()
{
    if (rf::is_dedicated_server || rf::is_multi) return;

    rf::Player* player = rf::local_player;
    if (!player || !player->cam) return;

    rf::Entity* entity = rf::entity_from_handle(player->entity_handle);
    if (!entity) return;

    float dx = entity->pos.x - rf::g_geomod_pos.x;
    float dy = entity->pos.y - rf::g_geomod_pos.y;
    float dz = entity->pos.z - rf::g_geomod_pos.z;
    float dist_sq = dx * dx + dy * dy + dz * dz;

    if (dist_sq < rf::g_geomod_shake_threshold_sq) {
        rf::camera_shake(player->cam, 0.003f, 0.75f);
    }
}

// Replay geomod entity proximity effects (pushes nearby entities).
static void replay_geomod_entity_proximity()
{
    rf::geomod_push_nearby_entities(&rf::g_geomod_pos);
}

// Process confirmed smoke emitter replay from a clean call context.
// Called from geomod_per_frame_hook (outside the state machine) because FUN_00437230
// creates emitter records that interact with FUN_00437180's linked list lifecycle.
static void process_pending_geomod_effects()
{
    // Smoke emitters: replay FUN_00437230 with saved crater params.
    // Must run from per-frame hook (not State 3) because FUN_00437230 creates
    // emitter records that interact with FUN_00437180's linked list lifecycle.
    while (!g_rf2_smoke_confirmed.empty()) {
        rf::geomod_queue_add(&g_rf2_smoke_confirmed.front());
        g_rf2_smoke_record_ptrs.push_back(&rf::g_geomod_pending_list.prev->parameters);
        g_rf2_smoke_confirmed.pop_front();
    }
}

// CallHooks inside geomod_create (FUN_00467020).
// Rock debris: saves params to a global for replay in State 3.
// Sound: suppresses (replayed from globals directly in State 3).
CallHook<int(rf::Vector3*, float, rf::Vector3*, int, int)> geomod_rock_debris_defer_hook{
    0x004673d8,
    [](rf::Vector3* orientation, float scaled_radius, rf::Vector3* source_dir, int texture, int room_ptr) -> int {
        if (g_rf2_suppress_geomod_create_effects) {
            g_rf2_deferred_debris.pending = true;
            g_rf2_deferred_debris.orientation = *orientation;
            g_rf2_deferred_debris.scaled_radius = scaled_radius;
            g_rf2_deferred_debris.source_dir = *source_dir;
            g_rf2_deferred_debris.texture = texture;
            g_rf2_deferred_debris.room = room_ptr;
            return 0;
        }
        return geomod_rock_debris_defer_hook.call_target(orientation, scaled_radius, source_dir, texture, room_ptr);
    },
};

CallHook<int(int, const rf::Vector3&, float, const rf::Vector3&, int)> geomod_sound_defer_hook{
    0x00467494,
    [](int handle, const rf::Vector3& pos, float vol_scale, const rf::Vector3& unused, int group) -> int {
        if (g_rf2_suppress_geomod_create_effects) {
            g_rf2_deferred_sound.pending = true;
            g_rf2_deferred_sound.handle = handle;
            g_rf2_deferred_sound.pos = pos;
            return -1;
        }
        return geomod_sound_defer_hook.call_target(handle, pos, vol_scale, unused, group);
    },
};

// CallHooks inside the real-time boolean packet receive handler (FUN_00476590).
// When the server sends a boolean packet, the client processes it here — bypassing
// geomod_create entirely (stock geomod_create returns early for multiplayer clients).
// The packet handler creates rock debris and smoke emitters IMMEDIATELY, before the
// boolean state machine confirms whether geometry was actually carved. For RF2-style
// geomods this causes spurious effects when the crater is near but doesn't intersect
// a geoable brush. These hooks mirror the suppression logic from geomod_create_hook.

CallHook<int(rf::Vector3*, float, rf::Vector3*, int, int)> packet_rock_debris_defer_hook{
    0x00476693,
    [](rf::Vector3* pos, float scaled_radius, rf::Vector3* source_dir, int texture, int room_ptr) -> int {
        bool rf2_enabled = AlpineLevelProperties::instance().rf2_style_geomod;
        if (rf2_enabled && pos) {
            bool in_geo_region = is_pos_in_any_geo_region(*pos);
            if (!in_geo_region) {
                // pos is &params.pos of the handler's stack GeomodParams (the same params geomod_init gets).
                const auto& params = *reinterpret_cast<const rf::GeomodParams*>(
                    reinterpret_cast<const char*>(pos) - offsetof(rf::GeomodParams, pos));
                auto overlapping = find_overlapping_detail_rooms(*pos, get_crater_extent_radius(params));
                if (!overlapping.empty()) {
                    // RF2 geomod near geoable brush — defer debris until carving confirmed
                    g_rf2_suppress_geomod_create_effects = true;
                    g_rf2_deferred_debris.pending = true;
                    g_rf2_deferred_debris.orientation = *pos;
                    g_rf2_deferred_debris.scaled_radius = scaled_radius;
                    g_rf2_deferred_debris.source_dir = *source_dir;
                    g_rf2_deferred_debris.texture = texture;
                    g_rf2_deferred_debris.room = room_ptr;
                    return 0;
                }
            }
        }
        return packet_rock_debris_defer_hook.call_target(pos, scaled_radius, source_dir, texture, room_ptr);
    },
};

CallHook<void(rf::GeomodParams*)> packet_emitter_save_params_hook{
    0x0047669D,
    [](rf::GeomodParams* params) {
        int saved_default = rf::g_geomod_emitter_default_idx;
        int saved_driller = rf::g_geomod_emitter_driller_idx;

        if (g_rf2_suppress_geomod_create_effects) {
            // Suppress smoke particle creation by nulling emitter indices.
            // The emitter RECORD is still created (needed to trigger the state machine),
            // but with null particle handles — matching the geomod_create_hook path.
            rf::g_geomod_emitter_default_idx = -1;
            rf::g_geomod_emitter_driller_idx = -1;
            g_rf2_smoke_pending.push_back(*params);
        }

        packet_emitter_save_params_hook.call_target(params);

        rf::g_geomod_emitter_default_idx = saved_default;
        rf::g_geomod_emitter_driller_idx = saved_driller;
        g_rf2_suppress_geomod_create_effects = false;
    },
};

// Hook FUN_00437180 — the per-frame geomod processor. Process pending effects
// from a clean call context (outside the state machine) before the stock logic runs.
FunHook<void(float)> geomod_per_frame_hook{
    0x00437180,
    [](float dt) {
        process_pending_geomod_effects();
        geomod_per_frame_hook.call_target(dt);
    },
};

// Hook geomod_queue_add call at 0x0046727a inside geomod_create (FUN_00467020).
// Always passes through (the emitter record is needed to trigger the state machine),
// but saves the GeomodParams into the FIFO queue for smoke emitter replay.
CallHook<void(rf::GeomodParams*)> geomod_emitter_save_params_hook{
    0x0046727a,
    [](rf::GeomodParams* params) {
        if (g_rf2_suppress_geomod_create_effects) {
            g_rf2_smoke_pending.push_back(*params);
        }
        geomod_emitter_save_params_hook.call_target(params);
    },
};

// Hook level_mod (0x00467020) — the master "create geomod" function called by the
// explosion system. Creates visual effects (emitters, rock debris, sound) AND queues
// the boolean request. Returning false prevents all geomod visuals and processing.
FunHook<bool(float, int, rf::GRoom*, rf::Vector3*, rf::Vector3*, int, int)> geomod_create_hook{
    0x00467020,
    [](float radius, int parent_handle, rf::GRoom* src_room, rf::Vector3* pos, rf::Vector3* hit_normal, int shape_index, int flags) -> bool {
        bool rf2_enabled = AlpineLevelProperties::instance().rf2_style_geomod;
        bool is_rf2_geomod = false;

        if (rf2_enabled && pos) {
            bool in_geo_region = is_pos_in_any_geo_region(*pos);
            if (!in_geo_region) {
                is_rf2_geomod = true;

                // Check RF2 geo limit
                if (g_rf2_geo_limit == 0) {
                    return false; // RF2 geomods disabled
                }
                if (g_rf2_geo_limit > 0 && g_rf2_geo_count >= g_rf2_geo_limit) {
                    return false; // RF2 limit reached
                }

                // Effects gate: check if explosion is near any geoable detail room
                // using bbox + padding. Reliable for all geometry shapes including
                // concave brushes and touching detail brushes.
                // Crater scale as geomod_create derives it: 1.0 with the unit-scale flag, otherwise radius / bsphere
                // times the FUN_0045cff0 hardness factor (level default outside Geo Regions).
                float crater_radius = 0.0f;
                rf::GSolid* crater = rf::geomod_get_crater_solid(shape_index);
                if (crater && crater->bounding_sphere_radius > 0.0f) {
                    float scale = 1.0f;
                    if (!(flags & rf::geomod_create_flag_unit_scale)) {
                        float hardness_factor = std::clamp(1.0f - rf::level.default_rock_hardness * 0.01f, 0.0f, 1.0f);
                        scale = radius / crater->bounding_sphere_radius * hardness_factor;
                    }
                    crater_radius = get_crater_extent_radius(crater, scale, rf::Vector3{}, rf::Vector3{});
                }
                auto overlapping = find_overlapping_detail_rooms(*pos, crater_radius);
                if (overlapping.empty()) {
                    return false; // skip entire geomod (no effects, no boolean, no state machine)
                }
            }
        }

        // For RF2-style geomods: bypass both the soft multiplayer limit and the hard
        // 128 crater record limit by temporarily zeroing g_num_geomods_this_level and
        // raising multi_geo_limit. The stock function then writes the crater's records at
        // slot 0, which is restored afterwards so a stock crater there is kept; RF2 craters
        // keep no record.
        //
        // For normal geomods in RF2-enabled levels: compensate the soft limit for RF2
        // entries in the persistent counter (DAT_0063715c) so RF2 geomods don't consume
        // normal limit quota. If rf2_geo_count is 10 and geo_limit is 64, the effective
        // limit becomes 74 — so 64 normal + 10 RF2 = 74 total triggers the block.
        auto& stock_limit = rf::multi_geo_limit;
        int saved_limit = stock_limit;
        int saved_crater_count = rf::g_num_geomods_this_level;
        const auto saved_crater_record = std::to_array(rf::g_geomod_crater_records[0]);
        const auto saved_crater_pushes = std::to_array(rf::g_geomod_crater_pushes[0]);

        if (is_rf2_geomod) {
            if (stock_limit > 0) stock_limit = INT_MAX;
            rf::g_num_geomods_this_level = 0;
        }
        else if (rf2_enabled && stock_limit > 0 && g_rf2_geo_count > 0) {
            stock_limit += g_rf2_geo_count;
        }

        // Suppress immediate visual effects for RF2-style geomods.
        // All effects are recreated from globals in State 3 if carving is confirmed.
        // - Smoke emitters: suppressed by temporarily setting emitter indices to -1.
        //   FUN_00437230 still creates the emitter RECORD (needed to trigger the state
        //   machine via FUN_00437180), but with null particle handles. The crater params
        //   are saved to a FIFO queue by geomod_emitter_save_params_hook for replay.
        // - Rock debris: saved via CallHook, replayed directly in State 3.
        // - Sound: saved via CallHook, replayed at earliest carving confirmation (boolean State 5).
        int saved_default_emitter = rf::g_geomod_emitter_default_idx;
        int saved_driller_emitter = rf::g_geomod_emitter_driller_idx;

        if (is_rf2_geomod) {
            if (!rf::is_multi || rf::is_server) {
                g_rf2_deferred_debris.pending = false;
            }
            g_rf2_suppress_geomod_create_effects = true;
            rf::g_geomod_emitter_default_idx = -1;
            rf::g_geomod_emitter_driller_idx = -1;
        }

        auto result = geomod_create_hook.call_target(radius, parent_handle, src_room, pos, hit_normal, shape_index, flags);
        stock_limit = saved_limit;
        if (is_rf2_geomod) {
            std::ranges::copy(saved_crater_record, rf::g_geomod_crater_records[0]);
            std::ranges::copy(saved_crater_pushes, rf::g_geomod_crater_pushes[0]);
        }
        g_rf2_suppress_geomod_create_effects = false;
        rf::g_geomod_emitter_default_idx = saved_default_emitter;
        rf::g_geomod_emitter_driller_idx = saved_driller_emitter;

        // Stats stream: one geomod event per confirmed carve, on both the
        // classic and rf2-brush paths. geomod_create is the sole caller of the queue and
        // returns early for MP clients, so this is a server-side-only choke point with no
        // double-emit against the geomod broadcast. Attribution is best-effort: the source
        // handle is a weapon whose parent is the shooter entity and whose info_index is the
        // weapon type; other callers (level logic, etc.) fall back to null.
        if (result && pos) {
            rf::Player* shooter = nullptr;
            int weapon_type = -1;
            if (rf::Object* src = rf::obj_from_handle(parent_handle)) {
                if (src->type == rf::OT_WEAPON) {
                    weapon_type = static_cast<rf::Weapon*>(src)->info_index;
                    shooter = rf::player_from_entity_handle(src->parent_handle);
                }
                else {
                    shooter = rf::player_from_entity_handle(parent_handle);
                }
            }
            afstats::on_geomod(*pos, radius, is_rf2_geomod, shooter, weapon_type);
        }

        if (is_rf2_geomod) {
            rf::g_num_geomods_this_level = saved_crater_count;
            if (result) {
                g_rf2_geo_count++;
            }
            else if (!rf::is_multi || rf::is_server) {
                if (!g_rf2_smoke_pending.empty()) {
                    g_rf2_smoke_pending.pop_back();
                }
                g_rf2_deferred_debris.pending = false;
            }
            // Return false to the caller so normal explosion effects (decal, camera shake)
            // are NOT suppressed. The geomod still runs internally (the emitter record
            // created by geomod_queue_add triggers the state machine via FUN_00437180).
            // For successful carves, geomod-specific effects are recreated in State 3.
            return false;
        }

        return result;
    },
};

// Check if separated solid chunks should become physics objects.
// In SP: controlled by client setting. In MP: controlled by server setting.
static bool should_enable_geo_chunk_physics()
{
    // Single-player: honor client setting.
    if (!rf::is_multi && g_alpine_game_config.geo_chunk_physics) {
        return true;
    }

    // Multiplayer server (including dedicated): honor active server rules.
    if ((rf::is_dedicated_server || rf::is_server) && g_alpine_server_config_active_rules.geo_chunk_physics) {
        return true;
    }

    // Multiplayer clients: honor server setting when available.
    // When server info is missing (e.g. vanilla/older servers), default to enabled
    if (rf::is_multi && !rf::is_dedicated_server && !rf::is_server) {
        auto server_info = get_af_server_info();
        if (!server_info.has_value()) {
            return true;
        }
        if (server_info->geo_chunk_physics) {
            return true;
        }
    }

    return false;
}

// Hook geomod_init (FUN_00466b00) to activate RF2-style boolean targeting.
// By this point, geomod_create_hook has already verified geoable rooms exist,
// so overlapping should always be non-empty when RF2-style is active.
// Both server and client deterministically derive this from position + level properties,
// so the pregame boolean packet (which omits the flags field) works correctly.
FunHook<void(rf::GeomodParams*)> geomod_init_hook{
    0x00466B00,
    [](rf::GeomodParams* params) {
        // Skip levelmod_do_blast_begin for smoke-only emitter records replayed by our effects queue.
        {
            auto it = std::find(g_rf2_smoke_record_ptrs.begin(),
                                g_rf2_smoke_record_ptrs.end(), static_cast<void*>(params));
            if (it != g_rf2_smoke_record_ptrs.end()) {
                g_rf2_smoke_record_ptrs.erase(it);
                return; // skip — FUN_004370d0 will destroy the record + emitters
            }
        }

        geomod_init_hook.call_target(params);

        // The carve can open or close a roof over a blocked-by-geometry weather region.
        if (rf::g_geomod_crater_solid) {
            weather_notify_geomod(params->pos, params->scale * rf::g_geomod_crater_solid->bounding_sphere_radius);
        }

        // Clear the modification flag at the start of each geomod.
        g_rf2_boolean_modified_detail = false;

        // Override separated solids physics flag based on the geo chunk physics option.
        // When disabled, isolated chunks disappear instead of falling as physics objects.
        // This applies to both stock and RF2-style geomods.
        if (!should_enable_geo_chunk_physics()) {
            rf::g_geomod_separate_solids = false;
        }

        bool rf2_enabled = AlpineLevelProperties::instance().rf2_style_geomod;
        bool in_geo_region = rf2_enabled && is_pos_in_any_geo_region(rf::g_geomod_pos);
        g_rf2_style_boolean_active = rf2_enabled && !in_geo_region;

        if (g_rf2_style_boolean_active) {
            // Enable stock separated solids for RF2-style geomods when the option allows it.
            // Stock code disables it for locally-created geomods (bit 0 of flags).
            // We override to enable it so the engine's chunk detection and physics work.
            if (should_enable_geo_chunk_physics()) {
                rf::g_geomod_separate_solids = true;
            }

            // Find detail rooms overlapping the crater and select the first target.
            const float crater_radius = get_crater_extent_radius(*params);
            auto overlapping = find_overlapping_detail_rooms(rf::g_geomod_pos, crater_radius);
            alpine_terrain_decorations_notify_crater(rf::g_geomod_pos, crater_radius, overlapping);
            g_rf2_pending_detail_rooms.clear();
            g_rf2_cascaded_rooms.clear();
            g_rf2_split_rooms.clear();
            if (!overlapping.empty()) {
                g_rf2_target_detail_room = overlapping[0];
                for (size_t i = 1; i < overlapping.size(); i++) {
                    g_rf2_pending_detail_rooms.push_back(overlapping[i]);
                }
                xlog::debug("[RF2] geomod_init: target_room uid={} index={}, {} pending",
                    g_rf2_target_detail_room->uid, g_rf2_target_detail_room->room_index,
                    g_rf2_pending_detail_rooms.size());
            } else {
                g_rf2_target_detail_room = nullptr;
                xlog::debug("[RF2] geomod_init: no overlapping geoable rooms (unexpected)");
            }
        }
    },
};

// Clear corrupted detail_rooms on all detail rooms in the level solid.
// The boolean engine's inner state 6 (result collection) adds entries to detail rooms'
// detail_rooms VArray, creating room reference cycles. Detail rooms should NEVER have
// sub-detail-rooms. Corrupted entries cause infinite loops in stock engine code paths
// (portal traversal, visibility, collision, cache rebuild) that iterate detail_rooms.
static void clear_corrupted_detail_rooms()
{
    rf::GSolid* solid = rf::level.geometry;
    if (!solid)
        return;

    for (int i = 0; i < solid->all_rooms.size(); i++) {
        rf::GRoom* room = solid->all_rooms[i];
        if (room && room->is_detail && room->detail_rooms.size() > 0) {
            room->detail_rooms.clear();
        }
    }
}

// Boolean state 5 gives each new crater face a GSurface (0x004DE21A), and state 7 every face that still
// has none (0x004DE50C; 0x005A3A58 is cleared only on dedicated servers): a noise lightmap relit from
// stock lights only. Terrain faces are lit from the terrain's chart and keep surface -1.
// GFace::surface_index is a short: a surface past 0x7FFF would wrap negative, and state 7 would make the
// face another one on every boolean, so faces keep surface -1 once the solid is full.
CallHook<void __fastcall(rf::GSolid*, int, rf::GFace*, float)> boolean_face_create_surface_hook{
    {0x004DE21A, 0x004DE50C},
    [](rf::GSolid* solid, int edx, rf::GFace* face, float ppm) FASTCALL_LAMBDA {
        if (solid->surfaces.size() > 0x7FFF) {
            return;
        }
        // A crater face no target face was cut for has no room yet.
        const rf::GRoom* room = face->which_room;
        if (!room && g_rf2_style_boolean_active) {
            room = g_rf2_target_detail_room;
        }
        if (!alpine_terrain_is_chunk_room(room)) {
            boolean_face_create_surface_hook.call_target(solid, edx, face, ppm);
        }
    },
};

// Boolean state 5 ends by linking every overlapping detail room into each affected room's detail list,
// comparing the bboxes of every face pair. RF2 carves affect only their target, a detail room, whose
// detail list clear_corrupted_detail_rooms empties after this state anyway; against a terrain chunk's
// neighbours the pass is hundreds of thousands of tests.
CodeInjection boolean_state5_skip_detail_relink_for_terrain{
    0x004DE2B9,
    [](auto& regs) {
        if (!g_rf2_style_boolean_active) {
            return;
        }
        auto& affected = rf::g_boolean_affected_rooms;
        const int count = rf::g_boolean_num_affected_rooms;
        if (count <= 0 || count > static_cast<int>(std::size(affected))) {
            return;
        }
        for (int i = 0; i < count; i++) {
            if (!affected[i] || !affected[i]->is_detail || !alpine_terrain_is_chunk_room(affected[i])) {
                return;
            }
        }
        regs.eip = 0x004DE454;
    },
};

// Boolean state 2 splits every listed face of more than 16 vertices, in whatever room it is.
CallHook<void(rf::GSolid*, rf::GFace*, rf::GFace**, rf::GFace**)> boolean_split_big_face_hook{
    0x004DBBEB,
    [](rf::GSolid* solid, rf::GFace* face, rf::GFace** a, rf::GFace** b) {
        rf::GRoom* room = face->which_room;
        if (g_rf2_style_boolean_active && solid == rf::g_level_solid && room && room != g_rf2_target_detail_room &&
            std::find(g_rf2_split_rooms.begin(), g_rf2_split_rooms.end(), room) == g_rf2_split_rooms.end()) {
            try {
                g_rf2_split_rooms.push_back(room);
            }
            catch (...) {
            }
        }
        boolean_split_big_face_hook.call_target(solid, face, a, b);
    },
};

// Fast boolean iteration for terrain targets: inner states 0, 1 and 5 see only the faces they can change,
// and the level face list is what stock leaves at every frame boundary.
namespace
{
    struct Entry
    {
        rf::GFace* face;
        bool in_tprime;
        bool survived;
    };

    // Faces chained through next[FACE_LIST_SOLID] in link order.
    struct FaceChain
    {
        rf::GFace* head = nullptr;
        rf::GFace* tail = nullptr;

        void link(rf::GFace* f)
        {
            (tail ? tail->next[rf::FACE_LIST_SOLID] : head) = f;
            tail = f;
        }

        // Links all of `other`, keeping its internal next[FACE_LIST_SOLID] links.
        void splice(const FaceChain& other)
        {
            if (other.head) {
                link(other.head);
                tail = other.tail;
            }
        }

        // Points the last face (or head, when empty) at `rest` and returns the head.
        rf::GFace* end_with(rf::GFace* rest)
        {
            (tail ? tail->next[rf::FACE_LIST_SOLID] : head) = rest;
            return head;
        }
    };

    struct Pass
    {
        bool active = false;
        rf::GSolid* solid = nullptr;
        std::vector<Entry> faces;
        // T' face -> index in faces (-1: adjacent to the target but not in the list)
        std::unordered_map<rf::GFace*, int> tprime;
        int num_tprime = 0;
        int last_tprime = -1;
        int num_clones = 0;
        int num_survivors = 0;
    };
    Pass g_pass;

    // What 0x004DBBB0 acts on: more than 16 vertices (0x004E03E0), or a pending decal fixup.
    bool needs_sweep(const rf::GFace* face)
    {
        constexpr uint32_t stale_decal = rf::FACE_HAS_LEVEL_DECAL | rf::FACE_LEVEL_DECAL_UVS_STALE;
        if ((face->attributes.flags & stale_decal) == stale_decal) {
            return true;
        }
        return face->vertex_count() > 16;
    }

    // Inner state 0 on a face that isn't the target's, with boolean_clear_detail_bit3_for_rf2.
    uint32_t registered_flags(uint32_t flags, const rf::GRoom* room)
    {
        flags &= ~(rf::FACE_BOOLEAN_INTERSECTED | rf::FACE_BOOLEAN_TYPE_1 | rf::FACE_BOOLEAN_SIDE);
        if (room && (!room->is_detail || room->is_geoable)) {
            flags &= ~rf::FACE_IS_DETAIL;
        }
        return flags;
    }

    void reset()
    {
        g_pass.active = false;
        g_pass.solid = nullptr;
        g_pass.faces.clear();
        g_pass.tprime.clear();
    }

    bool applicable(rf::GRoom* target)
    {
        return alpine_terrain_is_chunk_room(target) && !target->contains_liquid &&
            rf::g_level_solid && rf::g_boolean_solid == rf::g_level_solid &&
            rf::g_boolean_op == rf::GBooleanOperation::BOP_DIFFERENCE && !target->face_list.empty();
    }

    // Before inner state 0: remember the order, give every other face what state 0 would, and hand the
    // boolean T' (the target, faces sharing a vertex with it, and faces state 2's sweep would change).
    // False leaves the pass to the stock path.
    bool begin(rf::GRoom* target)
    {
        Pass& p = g_pass;
        rf::GSolid* solid = rf::g_level_solid;
        auto& list = solid->face_list;
        const int n = list.size();
        try {
            p.faces.reserve(n);
            for (rf::GFace& face : target->face_list) {
                if (face.which_room != target) {
                    return false;
                }
                p.tprime.emplace(&face, -1);
                int guard = 0;
                for (rf::GFaceVertex* fv = face.edge_loop; fv && guard++ < 1000;) {
                    for (rf::GFace* adj : fv->vertex->adjacent_faces) {
                        p.tprime.emplace(adj, -1);
                    }
                    fv = fv->next;
                    if (fv == face.edge_loop) {
                        break;
                    }
                }
            }
            // A bail-out from here on is harmless: stock state 0 gives other faces these same flags.
            int by_room = 0;
            for (rf::GFace* face = list.first(); face; face = face->next[rf::FACE_LIST_SOLID]) {
                const int index = static_cast<int>(p.faces.size());
                if (index >= n) {
                    return false;
                }
                auto it = p.tprime.find(face);
                bool in = it != p.tprime.end();
                if (face->which_room == target) {
                    by_room++;
                }
                else if (!in && needs_sweep(face)) {
                    it = p.tprime.emplace(face, -1).first;
                    in = true;
                }
                if (in) {
                    it->second = index;
                    p.num_tprime++;
                    p.last_tprime = index;
                }
                else {
                    face->attributes.flags = registered_flags(face->attributes.flags, face->which_room);
                }
                p.faces.push_back({face, in, false});
            }
            if (static_cast<int>(p.faces.size()) != n || by_room != target->face_list.size() || p.num_tprime == 0) {
                return false;
            }
        }
        catch (...) {
            return false;
        }

        FaceChain chain;
        for (const Entry& e : p.faces) {
            if (e.in_tprime) {
                chain.link(e.face);
            }
        }
        list.assign(chain.end_with(nullptr), p.num_tprime);
        p.solid = solid;
        p.active = true;
        return true;
    }

    // After inner state 0: the full list again, with the crater clones state 0 appended after T'.
    void after_register()
    {
        Pass& p = g_pass;
        auto& list = p.solid->face_list;
        rf::GFace* first_clone = p.faces[p.last_tprime].face->next[rf::FACE_LIST_SOLID];
        int num_clones = 0;
        for (rf::GFace* f = first_clone; f; f = f->next[rf::FACE_LIST_SOLID]) {
            num_clones++;
        }
        if (num_clones != list.size() - p.num_tprime) {
            xlog::warn("[RF2] fast geomod: {} crater clones but the list count says {}", num_clones,
                list.size() - p.num_tprime);
        }
        const int n = static_cast<int>(p.faces.size());
        for (int i = 0; i + 1 < n; i++) {
            p.faces[i].face->next[rf::FACE_LIST_SOLID] = p.faces[i + 1].face;
        }
        p.faces[n - 1].face->next[rf::FACE_LIST_SOLID] = first_clone;
        list.assign(p.faces[0].face, n + num_clones);
        p.num_clones = num_clones;
        rf::g_boolean_num_registered_faces = n;
    }

    // Before inner state 1: T' and the clones only. False puts the rest of the pass on the stock path.
    bool before_classify()
    {
        Pass& p = g_pass;
        auto& list = p.solid->face_list;
        const int n = static_cast<int>(p.faces.size());
        if (list.size() != n + p.num_clones) {
            return false;
        }
        // Stock state 1 gives these side 2 (boolean_skip_non_detail_faces_for_rf2) on either path.
        rf::GFace* f = list.first();
        for (int i = 0; i < n; i++, f = f->next[rf::FACE_LIST_SOLID]) {
            if (f != p.faces[i].face) {
                return false;
            }
            if (!p.faces[i].in_tprime) {
                f->attributes.flags =
                    (f->attributes.flags & ~rf::FACE_BOOLEAN_SIDE) | (2u << rf::face_boolean_side_shift);
            }
        }
        rf::GFace* first_clone = f;
        int num_clones = 0;
        for (; f && num_clones <= p.num_clones; f = f->next[rf::FACE_LIST_SOLID]) {
            num_clones++;
        }
        if (num_clones != p.num_clones) {
            return false;
        }

        FaceChain chain;
        for (const Entry& e : p.faces) {
            if (e.in_tprime) {
                chain.link(e.face);
            }
        }
        list.assign(chain.end_with(first_clone), p.num_tprime + p.num_clones);
        return true;
    }

    void before_collect()
    {
        g_pass.num_survivors = rf::g_boolean_outside_list.size();
    }

    // After inner state 5: the list is state 1's outside faces (first num_survivors), then the kept and
    // new faces. Stock's outside list also held every other face, in the original order.
    void after_collect()
    {
        Pass& p = g_pass;
        auto& list = p.solid->face_list;
        const int s = p.num_survivors;
        const int m = list.size();
        const int num_other = static_cast<int>(p.faces.size()) - p.num_tprime;
        auto survivor_index = [&](rf::GFace* f) {
            auto it = p.tprime.find(f);
            return it == p.tprime.end() ? -1 : it->second;
        };

        bool consistent = s >= 0 && s <= m;
        rf::GFace* node = list.first();
        for (int k = 0; consistent && k < s; k++) {
            if (!node) {
                consistent = false;
                break;
            }
            const int index = survivor_index(node);
            if (index >= 0) {
                p.faces[index].survived = true;
            }
            node = node->next[rf::FACE_LIST_SOLID];
        }
        rf::GFace* rest = node;
        int num_rest = 0;
        for (rf::GFace* f = rest; consistent && f && num_rest <= m; f = f->next[rf::FACE_LIST_SOLID]) {
            num_rest++;
        }
        consistent = consistent && s + num_rest == m;

        FaceChain chain;
        if (!consistent) {
            // Never lose a face: the other faces in their order, then the list as it is.
            xlog::warn("[RF2] fast geomod: unexpected list after state 5 (count {}, outside {})", m, s);
            rf::GFace* old_head = list.first();
            for (const Entry& e : p.faces) {
                if (!e.in_tprime) {
                    chain.link(e.face);
                }
            }
            list.assign(chain.end_with(old_head), m + num_other);
            return;
        }

        FaceChain extra;
        node = list.first();
        for (int k = 0; k < s; k++) {
            rf::GFace* next = node->next[rf::FACE_LIST_SOLID];
            if (survivor_index(node) < 0) {
                extra.link(node);
            }
            node = next;
        }
        for (const Entry& e : p.faces) {
            if (!e.in_tprime || e.survived) {
                chain.link(e.face);
            }
        }
        chain.splice(extra);
        list.assign(chain.end_with(rest), m + num_other);
    }
}

// Runs before the boolean state `inner_state`; false when the fast path is not in use for it.
static bool geomod_fast_before_state(int inner_state)
{
    if (inner_state == 0) {
        reset();
        if (!g_rf2_style_boolean_active) {
            return false;
        }
        if (g_rf2_target_detail_room && applicable(g_rf2_target_detail_room)) {
            g_pass.num_tprime = g_pass.num_clones = 0;
            g_pass.last_tprime = -1;
            if (!begin(g_rf2_target_detail_room)) {
                reset();
            }
        }
        return g_pass.active;
    }
    if (!g_pass.active) {
        return false;
    }
    if (inner_state == 1 && !before_classify()) {
        reset();
        return false;
    }
    if (inner_state == 5) {
        before_collect();
    }
    return true;
}

static void geomod_fast_after_state(int inner_state)
{
    if (inner_state == 0) {
        after_register();
    }
    else if (inner_state == 5) {
        after_collect();
        reset();
    }
}

// Hook FUN_004dbc50 (boolean_iterate) to clear corrupted detail_rooms after every call.
// States: 0=face_register, 1=intersection_detect, 2=face_split_setup,
//         3=classify_dispatch, 4=classify_action, 5=reclassify_and_collect,
//         6=cleanup, 7=finalize
//
// CRITICAL: Inner state 5 (FUN_004dd8c0) modifies room data structures and corrupts
// detail_rooms on detail rooms. The boolean engine processes ONE inner state per call,
// with a frame render between calls. If we only clear detail_rooms when the boolean is
// done (after state 7), states 6 and 7 render with corrupted detail_rooms — the stock
// renderer iterates detail_rooms without an is_detail guard, causing freezes/crashes.
// The severity grows with accumulated geomods (more result faces → more corruption).
// Fix: clear detail_rooms after EVERY boolean_iterate call so the renderer never sees
// corrupted state. States 6 (cleanup) and 7 (finalize) don't re-corrupt detail_rooms.
FunHook<int()> boolean_iterate_hook{
    0x004dbc50,
    []() -> int {
        if (g_rf2_style_boolean_active && rf::g_boolean_inner_state == 0) {
            // Snapshot target room's faces before the boolean modifies them.
            // Used by rf2_mark_unsupported_pieces to avoid extracting
            // pre-existing disconnected components (e.g. interior faces of hollow boxes
            // where interior and exterior faces don't share edges).
            g_rf2_pre_boolean_faces.clear();
            if (g_rf2_target_detail_room) {
                g_rf2_pre_boolean_faces.reserve(g_rf2_target_detail_room->face_list.size());
                for (rf::GFace& face : g_rf2_target_detail_room->face_list) {
                    g_rf2_pre_boolean_faces.insert(&face);
                }
            }
            rf2_snapshot_support(g_rf2_target_detail_room);
        }

        // A terrain's crater texture replaces the level geomod texture on the crater faces
        // State 5 textures (FUN_004f8740) while that terrain's chunk is the target.
        int crater_bitmap = -1;
        if (g_rf2_style_boolean_active) {
            const AlpineTerrainRoomRef* ref = alpine_terrain_find_room(g_rf2_target_detail_room);
            if (ref && static_cast<std::size_t>(ref->terrain) < g_terrain_crater_bitmaps.size()) {
                crater_bitmap = g_terrain_crater_bitmaps[ref->terrain];
            }
        }
        auto& crater_face_bitmaps = rf::g_boolean_crater_face_bitmaps;
        int saved_crater_face_bitmaps[3];
        if (crater_bitmap >= 0) {
            std::copy(std::begin(crater_face_bitmaps), std::end(crater_face_bitmaps), saved_crater_face_bitmaps);
            std::fill(std::begin(crater_face_bitmaps), std::end(crater_face_bitmaps), crater_bitmap);
        }

        const int inner_state = rf::g_boolean_inner_state;
        const bool fast = geomod_fast_before_state(inner_state);
        int result = boolean_iterate_hook.call_target();
        if (fast) {
            geomod_fast_after_state(inner_state);
        }

        if (crater_bitmap >= 0) {
            std::copy(std::begin(saved_crater_face_bitmaps), std::end(saved_crater_face_bitmaps), crater_face_bitmaps);
        }

        if (g_rf2_style_boolean_active) {
            // Clear corrupted detail_rooms after EVERY inner state, not just when done.
            // This ensures the renderer never sees corrupted detail_rooms between frames.
            clear_corrupted_detail_rooms();
        }
        return result;
    },
};


// Invalidate render caches after RF2-style boolean modifies detail room faces.
// D3D9: Call stock g_render_cache_clear — parent room caches include detail room
//       faces via recursive geo_cache_prepare_room, so clearing forces a rebuild.
// D3D11: Free the carved detail room's cache and invalidate only the normal rooms
//        that embed it (state = 2 triggers rebuild on next render).
//        Rebuilding every room is a whole-level rebuild per crater and re-bakes the
//        accumulated scrolled UVs. We CANNOT call the full clear_cache() because
//        destroying and recreating all RoomRenderCache objects causes a freeze.
static void invalidate_detail_room_render_cache_d3d11(rf::GSolid* solid, rf::GRoom* target)
{
    gr::d3d11::release_detail_room_render_cache(target);

    // Normal rooms embed the detail room's faces in their own cache
    // (GRenderCacheBuilder::add_room recurses into detail_rooms), and the stock
    // boolean never marks them because their own faces weren't modified.
    for (int i = 0; i < solid->all_rooms.size(); i++) {
        rf::GRoom* room = solid->all_rooms[i];
        if (!room || room->is_detail || !room->geo_cache)
            continue;
        for (rf::GRoom* detail_room : room->detail_rooms) {
            if (detail_room == target) {
                // A separate terrain chunk is not part of its parent's cache.
                if (!alpine_terrain_is_separate_chunk(room, target)) {
                    room->geo_cache->state = 2;
                }
                break;
            }
        }
    }
}

static void invalidate_rf2_render_caches()
{
    rf::GSolid* solid = rf::level.geometry;
    if (!solid)
        return;

    // Safety net: clear any remaining corrupted detail_rooms. The primary clearing
    // happens in boolean_iterate_hook when the boolean completes, but this catches
    // any edge cases (e.g., multi-room redirect between boolean passes).
    clear_corrupted_detail_rooms();

    std::vector<rf::GRoom*> cascaded_rooms = std::move(g_rf2_cascaded_rooms);
    g_rf2_cascaded_rooms.clear();
    std::vector<rf::GRoom*> split_rooms = std::move(g_rf2_split_rooms);
    g_rf2_split_rooms.clear();

    if (!is_d3d11()) {
        rf::g_cache_clear();
        return;
    }

    rf::GRoom* target = g_rf2_target_detail_room;
    if (target) {
        invalidate_detail_room_render_cache_d3d11(solid, target);
    }
    for (rf::GRoom* room : cascaded_rooms) {
        if (room != target) {
            invalidate_detail_room_render_cache_d3d11(solid, room);
        }
    }
    for (rf::GRoom* room : split_rooms) {
        if (std::find(cascaded_rooms.begin(), cascaded_rooms.end(), room) != cascaded_rooms.end()) {
            continue;
        }
        if (room->is_detail) {
            invalidate_detail_room_render_cache_d3d11(solid, room);
        }
        else if (room->geo_cache) {
            room->geo_cache->state = 2;
        }
    }
}

// Hook g_decal_add call at 0x00466d2f in State 0 of the geomod state machine.
// Creates the blastmark02 geomod decal before the boolean runs. For RF2-style,
// save the GDecalCreateInfo and skip; replay in State 3 if carving is confirmed.
CallHook<rf::GDecal*(rf::GDecalCreateInfo*)> state0_decal_defer_hook{
    0x00466d2f,
    [](rf::GDecalCreateInfo* dci) -> rf::GDecal* {
        if (g_rf2_style_boolean_active) {
            g_rf2_deferred_decal_info = *dci;
            g_rf2_decal_deferred = true;
            return nullptr; // skip decal creation
        }
        return state0_decal_defer_hook.call_target(dci);
    },
};

// Hook at the START of outer State 2 (0x00466dcd) in the geomod state machine.
// State 2 runs after the boolean completes (State 1). It detects disconnected face
// groups (separated solids) and extracts smaller chunks as physics debris.
//
// For RF2-style: the stock FUN_004d0990 can't be used because it does BFS across the
// entire level solid's face graph (through vertex adjacency), so the detail room's
// faces appear connected to normal world geometry as one giant component.
// Instead, rf2_mark_unsupported_pieces splits geoable rooms into pieces with a room-scoped
// BFS and extracts the pieces that are (or just became) unsupported.
//
// Disassembly at injection point:
//   00466dcd: MOV ECX, dword ptr [0x006460e8]  ; ECX = level solid (6 bytes)
//   00466dd3: CALL 0x004d0990                   ; count disconnected components (5 bytes)
//   00466dd8: MOV ESI, EAX                      ; ESI = count (2 bytes)
//   00466dda: LEA ECX, [ESP + 0x10]             ; (continues...)
CodeInjection state2_rf2_separated_solids_injection{
    0x00466dcd,
    [](auto& regs) {
        if (!g_rf2_style_boolean_active)
            return; // let stock code run normally

        int count = rf2_mark_unsupported_pieces(g_rf2_target_detail_room, rf::g_level_solid);

        // Set EAX and ESI to the extraction count
        regs.eax = count;
        regs.esi = count;

        // Skip the stock MOV ECX + CALL + MOV ESI,EAX (jump to LEA ECX, [ESP+0x10])
        regs.eip = 0x00466dda;
    },
};

// Hook at the START of outer State 3 (0x00466f4e) in the geomod state machine.
// State 3 is entered after State 2 (result collection + stock cache clearing).
//
// For RF2-style geomod with multiple overlapping detail rooms, this hook
// implements multi-room looping: after the boolean for one room completes
// and results are collected, it checks for pending rooms. If any remain,
// it re-initializes the boolean engine for the next room and redirects
// back to State 1 (boolean_iterate), skipping State 3's debris/decals
// (which should only run once, after the final room).
//
// Flow for multi-room:
//   Room 1: State 0→1→2→3 (our hook intercepts, redirects to State 1)
//   Room 2: State 1→2→3 (our hook intercepts again if more rooms, or proceeds)
//   Room N: State 1→2→3 (no more rooms, State 3 proceeds normally)
CodeInjection geomod_state3_clear_detail_caches_injection{
    0x00466f4e,
    [](auto& regs) {
        if (!g_rf2_style_boolean_active)
            return;

        // Invalidate render caches for the just-completed room's boolean pass
        invalidate_rf2_render_caches();

        // Check for pending rooms
        if (!g_rf2_pending_detail_rooms.empty()) {
            g_rf2_target_detail_room = g_rf2_pending_detail_rooms.front();
            g_rf2_pending_detail_rooms.erase(g_rf2_pending_detail_rooms.begin());

            xlog::debug("[RF2] multi-room: next room uid={} index={}",
                g_rf2_target_detail_room->uid, g_rf2_target_detail_room->room_index);

            // Re-call boolean_setup (FUN_004de530) with the same parameters.
            // The level solid, crater solid, position, scale etc. are all globals
            // that haven't changed since the original State 0 call.
            // boolean_setup stores params, checks resource availability, and
            // resets the inner boolean state (DAT_005a3a34) to 0.
            using BooleanSetupFn = void(__cdecl*)(
                uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
            auto boolean_setup = reinterpret_cast<BooleanSetupFn>(0x004de530);
            boolean_setup(
                reinterpret_cast<uint32_t>(rf::g_level_solid),          // level solid
                reinterpret_cast<uint32_t>(rf::g_geomod_crater_solid),  // crater solid
                3u,                                                      // op_mode = geomod
                0u,                                                      // param4
                reinterpret_cast<uint32_t>(&rf::g_geomod_pos),               // &position
                0x00647ca8u,                                             // &orientation
                static_cast<uint32_t>(rf::g_geomod_texture_index),      // texture index
                2u,                                                      // param8
                std::bit_cast<uint32_t>(rf::g_geomod_scale),              // scale
                0x00647ca0u,                                             // &crater bbox
                0u,                                                      // param11
                0x006485b0u,                                             // &param12
                0x006485c0u                                              // &param13
            );

            // Verify boolean_setup succeeded — it sets inner state to 0 when resources
            // are available. If it silently failed (resources exhausted), inner state
            // stays at -1 and we'd cycle 1→2→3→1→... endlessly. Abort remaining rooms.
            if (rf::g_boolean_inner_state != 0) {
                xlog::warn("[RF2] boolean_setup failed for room {} (resource exhaustion, inner_state={}), "
                    "skipping {} remaining rooms",
                    g_rf2_target_detail_room->room_index, rf::g_boolean_inner_state,
                    g_rf2_pending_detail_rooms.size());
                g_rf2_pending_detail_rooms.clear();
                // Let State 3 proceed normally (debris/decals for the rooms we did process)
                return;
            }

            // Set outer state back to 1 (boolean_iterate) so the state machine
            // runs the boolean for the next room on subsequent frames
            rf::g_geomod_outer_state = 1;

            // Skip rest of State 3 (no debris/decals for intermediate rooms)
            regs.eip = 0x00466fb5;
            return;
        }

        // No more pending rooms — check if geometry was actually carved.
        // Skip ALL State 3 effects (debris, decals, impact effects, foley, camera shake)
        // when either no geoable room was targeted or the boolean didn't modify any faces.
        if (g_rf2_target_detail_room == nullptr || !g_rf2_boolean_modified_detail) {
            rf::g_geomod_outer_state = -1; // mark geomod as done
            if (!g_rf2_smoke_pending.empty()) g_rf2_smoke_pending.pop_front();
            g_rf2_deferred_debris.pending = false;
            g_rf2_deferred_sound.pending = false;
            g_rf2_decal_deferred = false;
            regs.eip = 0x00466fb5;   // jump past all State 3 effects
            return;
        }

        // Boolean carved geometry — replay suppressed effects.

        // Direct replay in State 3 (safe — these were working here before):
        replay_geomod_camera_shake();
        replay_geomod_entity_proximity();

        if (g_rf2_decal_deferred) {
            rf::g_decal_add(&g_rf2_deferred_decal_info);
            g_rf2_decal_deferred = false;
        }

        if (g_rf2_deferred_debris.pending) {
            rf::geomod_create_rock_debris(
                &g_rf2_deferred_debris.orientation,
                g_rf2_deferred_debris.scaled_radius,
                &g_rf2_deferred_debris.source_dir,
                g_rf2_deferred_debris.texture,
                g_rf2_deferred_debris.room);
            g_rf2_deferred_debris.pending = false;
        }

        // Geomod sound: already played in boolean State 5 (earliest carving confirmation).
        // Clear the flag in case it wasn't consumed (e.g., dedicated server).
        g_rf2_deferred_sound.pending = false;
        // Move smoke params from pending to confirmed — per-frame hook will process
        if (!g_rf2_smoke_pending.empty()) {
            g_rf2_smoke_confirmed.push_back(g_rf2_smoke_pending.front());
            g_rf2_smoke_pending.pop_front();
        }

        // State 3 proceeds normally — stock effects fire for the carved room(s)
    },
};

// In FUN_004dd8c0 (boolean State 5, result collection), the stock code writes
// state=1 to geo_cache + 0x20 for rooms whose faces were modified by the boolean.
// This is safe for normal rooms (D3D11 uses RoomRenderCache with padding_[0x20]
// before the state_ field). But for detail rooms, D3D11 uses GRenderCache which
// has NO such padding — offset 0x20 falls inside the SolidBatches data (the
// liquid_batches_ vector), corrupting it and causing a freeze/crash on render.
//
// Disassembly context (room cache state write loop):
//   004dda75: MOV ECX, 0xc9f4dc         ; ECX = &affected_rooms_array
//   004dda7a: MOV ESI, 0x1              ; ESI = 1 (state value)
//   004dda7f: MOV EAX, [ECX]            ; EAX = room pointer        <<< INJECTION
//   004dda81: MOV EAX, [EAX + 0x4]      ; EAX = room->geo_cache
//   004dda84: TEST EAX, EAX
//   004dda86: JZ 0x004dda8b             ; skip if null
//   004dda88: MOV [EAX + 0x20], ESI     ; *(geo_cache + 0x20) = 1   <<< CORRUPTION
//   004dda8b: ADD ECX, 0x4              ; next room
//
// We inject at 0x004dda7f to replicate the two MOV instructions but return
// EAX=0 (pretending geo_cache is null) for detail rooms in RF2-style mode,
// causing the stock JZ to skip the write.
CodeInjection boolean_state5_protect_detail_cache_for_rf2{
    0x004dda7f,
    [](auto& regs) {
        // Replicate original: EAX = [ECX] then EAX = [EAX+4]
        rf::GRoom** room_ptr = regs.ecx;
        rf::GRoom* room = *room_ptr;
        rf::GCache* cache = room->geo_cache;

        if (g_rf2_style_boolean_active && room->is_detail) {
            // A detail room appeared in the boolean's affected rooms list, confirming
            // that the crater actually intersected and modified detail brush geometry.
            if (!g_rf2_boolean_modified_detail) {
                xlog::debug("[RF2] boolean State 5: detail room modified (uid={}, index={})",
                    room->uid, room->room_index);
                // Play geomod sound at the earliest confirmation of carving
                // (during boolean inner State 5, several frames before outer State 3).
                if (g_rf2_deferred_sound.pending) {
                    rf::snd_play_3d(g_rf2_deferred_sound.handle, g_rf2_deferred_sound.pos,
                        1.0f, rf::Vector3{0.0f, 0.0f, 0.0f}, 0);
                    g_rf2_deferred_sound.pending = false;
                }
            }
            g_rf2_boolean_modified_detail = true;
            // Return null so the stock code skips the write to geo_cache + 0x20
            regs.eax = static_cast<int32_t>(0);
        } else {
            regs.eax = cache;
        }
        // Skip trampoline re-execution of the overwritten MOVs — our EAX is already set.
        // Resume at TEST EAX, EAX (0x004dda84), past the two replicated instructions.
        regs.eip = 0x004dda84u;
    },
};

// RF2-style geomod: conditionally suppress crater decal effects.
// FUN_00492400 is called from geomod state machine State 3 to apply crater marks
// at the explosion point. It has only one caller (the geomod state machine).
// Suppress when the boolean didn't actually modify any detail room geometry.
FunHook<void(rf::Vector3*, float)> geomod_crater_decals_hook{
    0x00492400,
    [](rf::Vector3* pos, float radius) {
        if (g_rf2_style_boolean_active && !g_rf2_boolean_modified_detail) {
            return;
        }
        geomod_crater_decals_hook.call_target(pos, radius);
    },
};

// RF2-style geomod: conditionally suppress impact effects.
// FUN_00490900 is called from geomod state machine State 3 (at 0x00466f89) to update
// decal states near the explosion point. Suppress when no geometry was actually carved.
//
// Uses CallHook instead of FunHook because
// SubHook cannot decode the x87 FPU instruction (opcode 0xd8) at 0x490904, making it
// unable to create a trampoline for FUN_00490900. FUN_00490900 has only one caller.
CallHook<void(rf::Vector3*, float)> geomod_impact_effects_hook{
    0x00466f89,
    [](rf::Vector3* pos, float radius) {
        if (g_rf2_style_boolean_active && !g_rf2_boolean_modified_detail) {
            return; // suppress impact effects when no geometry was carved
        }
        geomod_impact_effects_hook.call_target(pos, radius);
    },
};

void destruction_level_cleanup()
{
    g_rf2_style_boolean_active = false;
    g_rf2_target_detail_room = nullptr;
    g_rf2_pending_detail_rooms.clear();
    g_rf2_geo_count = 0;
    g_rf2_pre_boolean_faces.clear();
    g_rf2_pre_supported_faces.clear();
    g_rf2_target_supported_pre = false;
    g_rf2_cascaded_rooms.clear();
    g_rf2_split_rooms.clear();
    reset();
    g_terrain_crater_bitmaps.clear();
    g_rf2_boolean_modified_detail = false;
    g_rf2_suppress_geomod_create_effects = false;
    g_rf2_deferred_debris.pending = false;
    g_rf2_deferred_sound.pending = false;
    g_rf2_smoke_pending.clear();
    g_rf2_smoke_confirmed.clear();
    g_rf2_smoke_record_ptrs.clear();
    g_rf2_decal_deferred = false;
    g_brush_destruction.clear();
    g_brush_uid_to_room_uid.clear();
    g_breakable_brush_uids.clear();
    auto& props = AlpineLevelProperties::instance();
    props.geoable_room_uids.clear();
    props.geoable_brush_uids.clear();
    props.breakable_room_uids.clear();
    props.breakable_brush_uids.clear();
    props.breakable_materials.clear();
}

void g_solid_set_rf2_geo_limit(int limit)
{
    g_rf2_geo_limit = limit;
}

void destruction_do_patch()
{
    // RF2-style geomod: geoable detail brush only mode
    geomod_per_frame_hook.install();
    geomod_emitter_save_params_hook.install();
    geomod_rock_debris_defer_hook.install();
    geomod_sound_defer_hook.install();
    packet_rock_debris_defer_hook.install();
    packet_emitter_save_params_hook.install();
    geomod_create_hook.install();
    geomod_init_hook.install();
    boolean_iterate_hook.install();
    boolean_clear_detail_bit3_for_rf2.install();
    state1_force_slow_path_for_rf2.install();
    state3_force_slow_path_for_rf2.install();
    state4_skip_type1_for_rf2.install();
    state5_force_reclassify_for_rf2.install();
    state5_force_clear_type1_for_rf2.install();
    state5_reclassify_type1_for_rf2.install();
    boolean_skip_non_detail_faces_for_rf2.install();
    boolean_state5_allow_detail_for_rf2.install();
    boolean_state5_protect_detail_cache_for_rf2.install();
    boolean_face_create_surface_hook.install();
    boolean_state5_skip_detail_relink_for_terrain.install();
    boolean_split_big_face_hook.install();
    state0_decal_defer_hook.install();
    state2_rf2_separated_solids_injection.install();
    geomod_state3_clear_detail_caches_injection.install();
    geomod_crater_decals_hook.install();
    geomod_impact_effects_hook.install();

    // Breakable detail brush material system
    // Fix validate_destroy to accept triangulated faces (>= 3 vertices) for Alpine levels
    validate_destroy_vertex_count_fix.install();

    capture_damage_type_injection.install();
    reset_damage_type_injection.install();
    // AsmWriter patch: overwrite 17 bytes of FPU damage code (0x492090-0x4920A0) with JMP
    // to our trampoline. SubHook/CodeInjection can't handle FPU opcodes at this address.
    // The trampoline is built dynamically with AsmWriter into executable memory (CodeBuffer)
    // to avoid MSVC-only __declspec(naked) + __asm{} syntax.
    {
        using namespace asm_regs;
        AsmWriter{radius_damage_trampoline_code}
            .sub(esp, 4)                        // temp space for fraction
            .fstp<float>(*(esp + 0))             // pop ST(0) → [esp]
            .mov(eax, *(esp + 0))                // eax = fraction (float bits)
            .add(esp, 4)                         // free temp space
            .push(eax)                           // arg3: fraction
            .mov(eax, *(ebp + 0x14))             // eax = damage (float bits)
            .push(eax)                           // arg2: damage
            .push(esi)                           // arg1: room (GRoom*)
            .call(&room_damage_material_helper)
            .add(esp, 12)                        // clean 3 cdecl args
            .jmp_long(0x004920A3);               // stock life check
    }
    AsmWriter{0x00492090, 0x004920A1}.jmp(radius_damage_trampoline_code.get());
    direct_hit_material_injection.install();
    glass_kill_material_injection.install();
    glass_kill_packet_material_injection.install();
    glass_sound_entry_injection.install();
    glass_decal_material_injection.install();
    weapon_decal_glass_to_scorch_injection.install();
    skip_exit_decal_for_non_glass.install();
    detail_room_impact_vfx_injection.install();
    glass_shards_entry_injection.install();
    process_destroy_cleanup_injection.install();
    pregame_glass_render_cleanup_injection.install();

    // Commands
    dbg_num_geomods_cmd.register_cmd();
}
