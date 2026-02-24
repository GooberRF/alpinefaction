#include <vector>
#include <algorithm>
#include <cmath>
#include <patch_common/FunHook.h>
#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/AsmWriter.h>
#include <xlog/xlog.h>
#include "../misc/alpine_options.h"
#include "../misc/alpine_settings.h"
#include "../main/main.h"
#include "../misc/misc.h"
#include "../rf/geometry.h"
#include "../rf/level.h"
#include "../rf/event.h"
#include "../rf/mover.h"
#include "../rf/file/file.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../rf/os/frametime.h"
#include "../rf/player/camera.h"
#include "../rf/gameseq.h"
#include "../os/console.h"
#include "../bmpman/bmpman.h"
#include "../bmpman/fmt_conv_templates.h"
#include "level.h"

constexpr auto reference_fps = 30.0f;
constexpr auto reference_frametime = 1.0f / reference_fps;
static int g_max_decals = 512;
static float g_crater_autotexture_ppm = 32.0f;
static bool g_show_room_clip_wnd = false;

// Geomod entry flags (stored at entry offset 0x38 / param_1[0xe] in geomod_init)
enum GeomodFlags : uint32_t {
    GEOMOD_LOCAL_CREATED    = 0x01,  // bit 0: set for locally-created geomods
    GEOMOD_SKIP_CSG         = 0x02,  // bit 1: skip CSG boolean setup
    GEOMOD_FROM_SERVER      = 0x04,  // bit 2: geomod received from server
    GEOMOD_ORIENTED         = 0x08,  // bit 3: use directional orientation
    GEOMOD_ICE_TEXTURE      = 0x10,  // bit 4: use ice crater texture
    GEOMOD_RF2_STYLE        = 0x20,  // bit 5: RF2-style (detail brushes only)
};

// Set by geomod_init hook; checked by boolean engine injections.
static bool g_rf2_style_boolean_active = false;

// RF2-style geomod limit — separate from the normal multiplayer geomod limit.
// -1 = unlimited (default), 0 = disabled, >0 = specific limit.
static int g_rf2_geo_limit = -1;
static int g_rf2_geo_count = 0;

// Per-room targeting: when RF2-style is active, the boolean engine only processes
// faces from ONE specific detail room at a time. This prevents crashes and incorrect
// face classification that occur when the boolean engine tries to process faces from
// multiple disjoint detail rooms simultaneously.
// When a crater overlaps multiple rooms, we run multiple boolean passes (one per room).
static rf::GRoom* g_rf2_target_detail_room = nullptr;
static std::vector<rf::GRoom*> g_rf2_pending_detail_rooms;

// Saved original detail room face planes for convex hull containment test.
// Saved once on the first RF2-style geomod, before any boolean modifies the geometry.
// For a convex detail brush, all face normals point outward from the brush interior.
// A point is inside the brush if distance_to_point() <= epsilon for ALL face planes.
struct SavedDetailRoomPlanes {
    rf::GRoom* room;
    rf::Vector3 bbox_min;
    rf::Vector3 bbox_max;
    std::vector<rf::Plane> planes;
};
static std::vector<SavedDetailRoomPlanes> g_saved_detail_room_planes;
static bool g_detail_planes_initialized = false;

static void save_original_detail_room_planes()
{
    g_saved_detail_room_planes.clear();
    auto* solid = rf::level.geometry;
    if (!solid) return;

    for (auto& room : solid->all_rooms) {
        if (!room->is_detail || !room->is_geoable) continue;

        SavedDetailRoomPlanes saved;
        saved.room = room;
        saved.bbox_min = room->bbox_min;
        saved.bbox_max = room->bbox_max;

        for (rf::GFace& face : room->face_list) {
            saved.planes.push_back(face.plane);
        }

        if (!saved.planes.empty()) {
            g_saved_detail_room_planes.push_back(std::move(saved));
        }
    }
}

// Test if a point is inside a saved detail room's original convex volume.
// Uses half-space intersection: a point is inside if it's on the negative side
// (or within tolerance) of ALL face planes (outward-pointing normals convention).
// If target_room is set, only checks that room. Otherwise checks all saved rooms.
static bool is_point_in_saved_detail_room(const rf::Vector3& pt, rf::GRoom* target_room = nullptr)
{
    constexpr float epsilon = 0.1f;
    for (const auto& saved : g_saved_detail_room_planes) {
        if (target_room && saved.room != target_room) continue;

        // Quick AABB pre-filter
        if (pt.x < saved.bbox_min.x - epsilon || pt.x > saved.bbox_max.x + epsilon ||
            pt.y < saved.bbox_min.y - epsilon || pt.y > saved.bbox_max.y + epsilon ||
            pt.z < saved.bbox_min.z - epsilon || pt.z > saved.bbox_max.z + epsilon) {
            continue;
        }

        bool inside = true;
        for (const auto& plane : saved.planes) {
            if (plane.distance_to_point(pt) > epsilon) {
                inside = false;
                break;
            }
        }
        if (inside) return true;
    }
    return false;
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
    constexpr int max_faces = 5000;
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

// Geomod position global (set by FUN_00466b00)
static auto& geomod_pos = addr_as_ref<rf::Vector3>(0x006485a0);


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
            regs.eax = addr_as_ref<int>(0x01370f64);
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
            regs.eax = addr_as_ref<int>(0x01370f64);
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
            xlog::warn("[RF2] state5_reclass: null face or attrs, skipping");
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
                if (!fv->vertex) {
                    xlog::warn("[RF2] state5_reclass: null vertex in edge_loop");
                    break;
                }
                centroid.x += fv->vertex->pos.x;
                centroid.y += fv->vertex->pos.y;
                centroid.z += fv->vertex->pos.z;
                vertex_count++;
                if (vertex_count > max_centroid_verts) {
                    xlog::warn("[RF2] state5_reclass: vertex limit hit, possible edge_loop corruption");
                    break;
                }
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
std::optional<int> g_sky_room_uid_override;
std::optional<rf::Object*> g_sky_room_eye_anchor;
std::optional<float> g_sky_room_eye_offset_scale;
static rf::Vector3 g_adjusted_sky_room_eye_position;


FunHook<int(rf::GSolid*, rf::GRoom*)> geo_cache_prepare_room_hook{
    0x004F0C00,
    [](rf::GSolid* solid, rf::GRoom* room) {
        int ret = geo_cache_prepare_room_hook.call_target(solid, room);
        if (ret == 0 && room->geo_cache) {
            int num_verts = struct_field_ref<int>(room->geo_cache, 4);
            if (num_verts > 8000) {
                static int once = 0;
                if (!(once++))
                    xlog::warn("Not rendering room with {} vertices!", num_verts);
                room->geo_cache = nullptr;
                return -1;
            }
        }
        return ret;
    },
};

CodeInjection GSurface_calculate_lightmap_color_conv_patch{
    0x004F2F23,
    [](auto& regs) {
        // Always skip original code
        regs.eip = 0x004F3023;

        rf::GSurface* surface = regs.esi;
        rf::GLightmap& lightmap = *surface->lightmap;
        rf::gr::LockInfo lock;
        if (!rf::gr::lock(lightmap.bm_handle, 0, &lock, rf::gr::LOCK_WRITE_ONLY)) {
            return;
        }

        int offset_x = surface->xstart;
        int offset_y = surface->ystart;
        int src_width = lightmap.w;
        int dst_pixel_size = bm_bytes_per_pixel(lock.format);
        uint8_t* src_data = lightmap.buf + 3 * (offset_x + offset_y * src_width);
        uint8_t* dst_data = &lock.data[dst_pixel_size * offset_x + offset_y * lock.stride_in_bytes];
        int height = surface->height;
        int src_pitch = 3 * src_width;
        bool success = bm_convert_format(dst_data, lock.format, src_data, rf::bm::FORMAT_888_BGR,
            src_width, height, lock.stride_in_bytes, src_pitch);
        if (!success)
            xlog::error("bm_convert_format failed for geomod (fmt {})", static_cast<int>(lock.format));
        rf::gr::unlock(&lock);
    },
};

CodeInjection GSurface_alloc_lightmap_color_conv_patch{
    0x004E487B,
    [](auto& regs) {
        // Skip original code
        regs.eip = 0x004E4993;

        rf::GSurface* surface = regs.esi;
        rf::GLightmap& lightmap = *surface->lightmap;
        rf::gr::LockInfo lock;
        if (!rf::gr::lock(lightmap.bm_handle, 0, &lock, rf::gr::LOCK_WRITE_ONLY)) {
            return;
        }

        int offset_y = surface->ystart;
        int src_width = lightmap.w;
        int offset_x = surface->xstart;
        uint8_t* src_data_begin = lightmap.buf;
        int src_offset = 3 * (offset_x + src_width * surface->ystart); // src offset
        uint8_t* src_data = src_offset + src_data_begin;
        int height = surface->height;
        int dst_pixel_size = bm_bytes_per_pixel(lock.format);
        uint8_t* dst_row_ptr = &lock.data[dst_pixel_size * offset_x + offset_y * lock.stride_in_bytes];
        int src_pitch = 3 * src_width;
        bool success = bm_convert_format(dst_row_ptr, lock.format, src_data, rf::bm::FORMAT_888_BGR,
                                                 src_width, height, lock.stride_in_bytes, src_pitch);
        if (!success)
            xlog::error("ConvertBitmapFormat failed for geomod2 (fmt {})", static_cast<int>(lock.format));
        rf::gr::unlock(&lock);
    },
};

CodeInjection GSolid_get_ambient_color_from_lightmap_patch{
    0x004E5CE3,
    [](auto& regs) {
        auto stack_frame = regs.esp + 0x34;
        int x = regs.edi;
        int y = regs.ebx;
        auto& lm = *static_cast<rf::GLightmap*>(regs.esi);
        auto& color = *reinterpret_cast<rf::Color*>(stack_frame - 0x28);

        // Skip original code
        regs.eip = 0x004E5D57;

        // Optimization: instead of locking the lightmap texture get color data from lightmap pixels stored in RAM
        const uint8_t* src_ptr = lm.buf + (y * lm.w + x) * 3;
        color.set(src_ptr[0], src_ptr[1], src_ptr[2], 255);
    },
};

// perhaps this code should be in g_solid.cpp but we don't have access to PixelsReader/Writer there
void gr_copy_water_bitmap(rf::gr::LockInfo& src_lock, rf::gr::LockInfo& dst_lock)
{
    int src_pixel_size = bm_bytes_per_pixel(src_lock.format);
    try {
        call_with_format(src_lock.format, [=](auto s) {
            call_with_format(dst_lock.format, [=](auto d) {
                if constexpr (decltype(s)::value == rf::bm::FORMAT_8_PALETTED
                    || decltype(d)::value == rf::bm::FORMAT_8_PALETTED) {
                    assert(false);
                    return;
                }
                uint8_t* dst_row_ptr = dst_lock.data;
                for (int y = 0; y < dst_lock.h; ++y) {
                    auto& byte_1370f90 = addr_as_ref<uint8_t[256]>(0x1370F90);
                    auto& byte_1371b14 = addr_as_ref<uint8_t[256]>(0x1371B14);
                    auto& byte_1371090 = addr_as_ref<uint8_t[512]>(0x1371090);
                    int t1 = byte_1370f90[y];
                    int t2 = byte_1371b14[y];
                    uint8_t* off_arr = &byte_1371090[-t1];
                    PixelsWriter<decltype(d)::value> wrt{dst_row_ptr};
                    for (int x = 0; x < dst_lock.w; ++x) {
                        int src_x = t1;
                        int src_y = t2 + off_arr[t1];
                        int src_x_limited = src_x & (dst_lock.w - 1);
                        int src_y_limited = src_y & (dst_lock.h - 1);
                        const uint8_t* src_ptr = src_lock.data + src_x_limited * src_pixel_size + src_y_limited * src_lock.stride_in_bytes;
                        PixelsReader<decltype(s)::value> rdr{src_ptr};
                        wrt.write(rdr.read());
                        ++t1;
                    }
                    dst_row_ptr += dst_lock.stride_in_bytes;
                }
            });
        });
    }
    catch (const std::exception& e) {
        xlog::error("Pixel format conversion failed for liquid wave texture: {}", e.what());
    }
}

CodeInjection g_proctex_update_water_patch{
    0x004E68D1,
    [](auto& regs) {
        // Skip original code
        regs.eip = 0x004E6B68;

        auto& proctex = *static_cast<rf::GProceduralTexture*>(regs.esi);
        rf::gr::LockInfo base_bm_lock;
        if (!rf::gr::lock(proctex.base_bm_handle, 0, &base_bm_lock, rf::gr::LOCK_READ_ONLY)) {
            return;
        }
        bm_set_dynamic(proctex.user_bm_handle, true);
        rf::gr::LockInfo user_bm_lock;
        if (!rf::gr::lock(proctex.user_bm_handle, 0, &user_bm_lock, rf::gr::LOCK_WRITE_ONLY)) {
            rf::gr::unlock(&base_bm_lock);
            return;
        }

        gr_copy_water_bitmap(base_bm_lock, user_bm_lock);

        rf::gr::unlock(&base_bm_lock);
        rf::gr::unlock(&user_bm_lock);
    }
};

CodeInjection g_proctex_create_bm_create_injection{
    0x004E66A2,
    [](auto& regs) {
        int bm_handle = regs.eax;
        bm_set_dynamic(bm_handle, true);
    },
};

CodeInjection face_scroll_fix{
    0x004EE1D6,
    [](auto& regs) {
        rf::GSolid* solid = regs.ebp;
        auto& texture_movers = solid->texture_movers;
        for (auto& tm : texture_movers) {
            tm->update_solid(solid);
        }
    },
};

CodeInjection ingame_add_lightmap_to_face_fullbright_fix{
    0x004E5BBE,
    [](auto& regs) {
        // idea for future AlpineLevelProps bool:
        // Use level ambient light for surfaces without lighting (geo craters and uncalced)
        // would need to do the same operation as below
        rf::GSurface* surface = regs.esi;
        rf::GFace* face = regs.edi;

        if (!surface || !face)
            return;

        if (face->attributes.flags & rf::GFaceFlags::FACE_FULL_BRIGHT) {
            surface->fullbright = true; // should be set already anyway, but just in case

            xlog::debug("Skipping lightmap generation for fullbright face {} (surface {})",
                face->attributes.face_id,
                surface->index);

            regs.eip = 0x004E5C33;
        }
    },
};

CodeInjection g_proctex_update_water_speed_fix{
    0x004E68A0,
    [](auto& regs) {
        auto& pt = *static_cast<rf::GProceduralTexture*>(regs.esi);
        pt.slide_pos_xt += 12.8f * rf::frametime / reference_frametime;
        pt.slide_pos_yc += 4.27f * rf::frametime / reference_frametime;
        pt.slide_pos_yt += 3.878788f * rf::frametime / reference_frametime;
    },
};

CodeInjection g_face_does_point_lie_in_face_crashfix{
    0x004E1F93,
    [](auto& regs) {
        void* face_vertex = regs.esi;
        if (!face_vertex) {
            regs.bl = false;
            regs.eip = 0x004E206F;
        }
    },
};

CodeInjection level_load_lightmaps_color_conv_patch{
    0x004ED3E9,
    [](auto& regs) {
        // Always skip original code
        regs.eip = 0x004ED4FA;

        rf::GLightmap* lightmap = regs.ebx;

        rf::gr::LockInfo lock;
        if (!rf::gr::lock(lightmap->bm_handle, 0, &lock, rf::gr::LOCK_WRITE_ONLY))
            return;

        uint32_t floor_clamp = 0; // no floor
        uint32_t ceiling_clamp = 0xFFFFFFFF; // no ceiling
        bool should_clamp = false; // no clamping by default
        bool floor_clamp_defined = false;

        // Check if the level explicitly defines clamp floor
        if (g_alpine_level_info_config
            .is_option_loaded(
                rf::level.filename,
                AlpineLevelInfoID::LightmapClampFloor
            )
        ) {
            floor_clamp = get_level_info_value<uint32_t>(
                AlpineLevelInfoID::LightmapClampFloor
            );
            floor_clamp_defined = true;
            should_clamp = true;
        }

        // Check if the level explicitly defines clamp ceiling
        if (g_alpine_level_info_config
            .is_option_loaded(
                rf::level.filename,
                AlpineLevelInfoID::LightmapClampCeiling
            )
        ) {
            ceiling_clamp = get_level_info_value<uint32_t>(
                AlpineLevelInfoID::LightmapClampCeiling
            );
            should_clamp = true;
        }

        // If no per-level floor clamp, consider using legacy clamping for non-Alpine levels
        if (!floor_clamp_defined && rf::level.version < 300) {
            if ((g_alpine_game_config.always_clamp_official_lightmaps
                && rf::level.version < 200)
                || (!DashLevelProps::instance().lightmaps_full_depth
                && !g_alpine_game_config.full_range_lighting)) {
                should_clamp = true;
                constexpr int default_floor_clamp = 0x202020FF;
                floor_clamp = default_floor_clamp;
            }
        }

        // Clamp lightmaps only if:
        // - mapname_info.tbl says to (takes priority over all other config)
        // - Is an official Volition map AND "Always clamp official lightmaps" is turned on
        // - Is a version 200 map, AF "Full range lights" is turned off, AND DF "Lightmaps full depth" is turned off (or not set)

        // Apply clamping
        if (should_clamp) {
            xlog::debug("Applying lightmap clamping");

            const rf::gr::Color floor = rf::gr::Color::from_hex(floor_clamp);
            const rf::gr::Color ceiling = rf::gr::Color::from_hex(ceiling_clamp);

            if (ceiling.red < floor.red
                || ceiling.green < floor.green
                || ceiling.blue < floor.blue)
            {
                xlog::warn("Normalizing an invalid lightmap clamping range");
            }

            const auto [r_min, r_max] = std::minmax(floor.red, ceiling.red);
            const auto [g_min, g_max] = std::minmax(floor.green, ceiling.green);
            const auto [b_min, b_max] = std::minmax(floor.blue, ceiling.blue);

            for (int i = 0; i < lightmap->w * lightmap->h * 3; i += 3) {
                lightmap->buf[i + 0] = std::clamp(lightmap->buf[i + 0], r_min, r_max);
                lightmap->buf[i + 1] = std::clamp(lightmap->buf[i + 1], g_min, g_max);
                lightmap->buf[i + 2] = std::clamp(lightmap->buf[i + 2], b_min, b_max);
            }
        }

        bool success = bm_convert_format(lock.data, lock.format, lightmap->buf,
            rf::bm::FORMAT_888_BGR, lightmap->w, lightmap->h, lock.stride_in_bytes, 3 * lightmap->w, nullptr);
        if (!success)
            xlog::error("ConvertBitmapFormat failed for lightmap (dest format {})", static_cast<int>(lock.format));

        rf::gr::unlock(&lock);
    },
};

ConsoleCommand2 lighting_color_range_cmd{
    "r_fullrangelighting",
    []() {
        g_alpine_game_config.full_range_lighting = !g_alpine_game_config.full_range_lighting;        
        rf::console::printf("Full range lighting is: %s", g_alpine_game_config.full_range_lighting ? "enabled" : "disabled");
    },
    "Toggle full range lighting. Only affects new level loads.",
};

ConsoleCommand2 clamp_official_lightmaps_cmd{
    "r_clampofficiallightmaps",
    []() {
        g_alpine_game_config.always_clamp_official_lightmaps = !g_alpine_game_config.always_clamp_official_lightmaps;        
        rf::console::printf("Forced clamping of lightmaps in official levels is: %s", g_alpine_game_config.always_clamp_official_lightmaps ? "enabled" : "disabled");
    },
    "Toggle forced lightmap clamping for official Volition levels. Only affects new level loads. Only applicable if full range lighting is enabled.",
};

CodeInjection shadow_render_one_injection{
    0x004CB195,
    [](auto& regs) {
        void* svol = regs.eax;
        auto& bbox_min = struct_field_ref<rf::Vector3>(svol, 0xE4);
        auto& bbox_max = struct_field_ref<rf::Vector3>(svol, 0xF0);
        rf::MoverBrush *mb = regs.esi;
        if (!rf::bbox_intersect(bbox_min, bbox_max, mb->p_data.bbox_min, mb->p_data.bbox_max)) {
            regs.eip = 0x004CB1DA;
        }
    },
};

void* __fastcall decals_farray_ctor(void* that)
{
    return AddrCaller{0x004D3120}.this_call<void*>(that, 64);
}

CodeInjection g_decal_add_internal_cmp_global_weak_limit_injection{
    0x004D54AC,
    [](auto& regs) {
        // total decals soft limit, 96 by default
        if (regs.esi < g_max_decals * 3 / 4) {
            regs.eip = 0x004D54D6;
        }
        else {
            regs.eip = 0x004D54B1;
        }
    },
};

void decal_patch_limit(int max_decals)
{
    unsigned decal_farray_get_addresses[] = {
        0x00492298,
        0x004BBEC9,

        0x004D5515,
        0x004D5525,
        0x004D5545,
        0x004D5579,
        0x004D5589,
        0x004D55AD,
        0x004D55D9,
        0x004D55E9,
        0x004D5609,
        0x004D5635,
        0x004D5645,
        0x004D5666,
        0x004D569E,
        0x004D56AE,
        0x004D56CE,
        0x004D56FA,
        0x004D570A,
        0x004D572B,
    };
    unsigned decal_farray_add_addresses[] = {
        0x004D58AE,
        0x004D58BE,
        0x004D58D2,
    };
    unsigned decal_farray_add_unique_addresses[] = {
        0x004D67B1,
    };
    unsigned decal_farray_remove_matching_addresses[] = {
        0x004D6C68,
        0x004D6C93,
        0x004D6CB6,
        0x004D6CE1,
        0x004D6D10,
    };
    unsigned decal_farray_ctor_addresses[] = {
        0x004CCC51,
        0x004CF152,
    };
    unsigned decal_farray_dtor_addresses[] = {
        0x004CCE79,
        0x004CF452,
    };
    for (auto addr : decal_farray_get_addresses) {
        AsmWriter{addr}.call(0x0040A480);
    }
    for (auto addr : decal_farray_add_addresses) {
        AsmWriter{addr}.call(0x0045EC40);
    }
    for (auto addr : decal_farray_add_unique_addresses) {
        AsmWriter{addr}.call(0x0040A450);
    }
    for (auto addr : decal_farray_remove_matching_addresses) {
        AsmWriter{addr}.call(0x004BF550);
    }
    for (auto addr : decal_farray_ctor_addresses) {
        AsmWriter{addr}.call(decals_farray_ctor);
    }
    for (auto addr : decal_farray_dtor_addresses) {
        AsmWriter{addr}.call(0x0040EC50);
    }
    max_decals = std::min(max_decals, 512);
    constexpr int i8_max = std::numeric_limits<i8>::max();
    static rf::GDecal decal_slots[512]; // 128 by default
    write_mem_ptr(0x004D4F51 + 1, &decal_slots);
    write_mem_ptr(0x004D4F93 + 1, &decal_slots[std::size(decal_slots)]);
    // crossing soft limit causes fading out of old decals
    // crossing hard limit causes deletion of old decals
    write_mem<i32>(0x004D5456 + 2, max_decals); // total hard limit,  128 by default
    // Note: total soft level is handled in g_decal_add_internal_cmp_global_weak_limit_injection
    int max_decals_in_room = std::min<int>(max_decals / 3, i8_max);
    write_mem<i8>(0x004D55C4 + 2, max_decals_in_room); // room hard limit,   48 by default
    write_mem<i8>(0x004D5620 + 2, max_decals_in_room * 5 / 6); // room soft limit,   40 by default
    write_mem<i8>(0x004D5689 + 2, max_decals_in_room); // room hard limit,   48 by default
    write_mem<i8>(0x004D56E5 + 2, max_decals_in_room * 5 / 6); // room soft limit,   40 by default
    write_mem<i8>(0x004D5500 + 2, max_decals_in_room); // solid hard limit,  48 by default
    write_mem<i8>(0x004D555C + 2, max_decals_in_room * 5 / 6); // solid soft limit,  40 by default
    int max_weapon_decals = std::min<int>(max_decals / 8, i8_max);
    write_mem<i8>(0x004D5752 + 2, max_weapon_decals);  // weapon hard limit, 16 by default
    write_mem<i8>(0x004D579F + 2, max_weapon_decals);  // weapon hard limit, 16 by default
    write_mem<i8>(0x004D57AA + 2, max_weapon_decals * 7 / 8);  // weapon soft limit, 14 by default
    int max_geomod_decals = std::min<int>(max_decals / 4, i8_max);
    write_mem<i8>(0x004D584D + 2, max_geomod_decals);  // geomod hard limit, 32 by default
    write_mem<i8>(0x004D5852 + 2, max_geomod_decals * 7 / 8);  // geomod soft limit, 30 by default
}

ConsoleCommand2 max_decals_cmd{
    "max_decals",
    [](std::optional<int> max_decals) {
        if (max_decals) {
            g_max_decals = std::clamp(max_decals.value(), 128, 512);
            decal_patch_limit(g_max_decals);
        }
        rf::console::print("Max decals: {}", g_max_decals);
    },
};

static void render_rooms_clip_wnds()
{
    rf::GRoom** rooms;
    int num_rooms;
    rf::g_get_room_render_list(&rooms, &num_rooms);
    rf::gr::set_color(255, 255, 255, 255);
    for (int i = 0; i < num_rooms; ++i) {
        rf::GRoom* room = rooms[i];
        char buf[256];
        std::snprintf(buf, sizeof(buf), "room %d", room->room_index);
        rf::gr::string(room->clip_wnd.left, room->clip_wnd.top, buf);
        rf::gr::rect_border(room->clip_wnd.left, room->clip_wnd.top,
            room->clip_wnd.right - room->clip_wnd.left, room->clip_wnd.bot - room->clip_wnd.top);
    }
}

static ConsoleCommand2 dbg_room_clip_wnd_cmd{
    "dbg_room_clip_wnd",
    []() {
        g_show_room_clip_wnd = !g_show_room_clip_wnd;
        rf::console::print("Show room clip windows: {}", g_show_room_clip_wnd);
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

void g_solid_render_ui()
{
    if (g_show_room_clip_wnd && rf::gameseq_in_gameplay()) {
        render_rooms_clip_wnds();
    }
}

CodeInjection levelmod_do_blast_autotexture_ppm_patch{
    0x00466C00,
    [](auto& regs) {
        rf::GSolid* solid = regs.ecx;
        int bitmap_handle = regs.edi;
        int bitmap_w = 256; // default to dimensions of stock geomod bitmaps
        int bitmap_h = 256;
        rf::bm::get_dimensions(bitmap_handle, &bitmap_w, &bitmap_h); // get dimensions of geomod bitmap if different
        float ppm_default = 256.0f / g_crater_autotexture_ppm;       // ppm of stock geomod bitmaps

        // New bitmap is likely square anyway, but if not, use maximum between its dimensions to prevent
        // one dimension from displaying with higher pixel density than expected
        float ppm_new = std::max(bitmap_w, bitmap_h) / ppm_default; // apply same ppm to new bitmap as default
        solid->set_levelmod_blast_autotexture_ppm(ppm_new);
    },
};

void set_levelmod_autotexture_ppm() {
    if (g_alpine_level_info_config.is_option_loaded(rf::level.filename, AlpineLevelInfoID::CraterTexturePPM)) {
        g_crater_autotexture_ppm = get_level_info_value<float>(AlpineLevelInfoID::CraterTexturePPM);
    }
    else {
        g_crater_autotexture_ppm = 32.0f;
    }
}

// Apply geoable flags from AlpineLevelProperties UIDs to GRoom objects.
// Called from level_init_post_hook after both rooms and Alpine props are loaded.
void apply_geoable_flags()
{
    auto* solid = rf::level.geometry;
    if (!solid) return;

    // Clear is_geoable on all rooms first (GRoom padding is not zero-initialized)
    for (auto& room : solid->all_rooms) {
        room->is_geoable = false;
    }

    auto& props = AlpineLevelProperties::instance();
    xlog::debug("[Geoable] apply_geoable_flags: rf2_style_geomod={} geoable_room_uids.size={}", props.rf2_style_geomod, props.geoable_room_uids.size());
    if (!props.rf2_style_geomod) return;

    // Search solid->all_rooms directly (level_room_from_uid doesn't cover detail rooms)
    for (int32_t uid : props.geoable_room_uids) {
        bool found = false;
        for (auto& room : solid->all_rooms) {
            if (room->uid == uid && room->is_detail) {
                room->is_geoable = true;
                found = true;
                xlog::debug("[Geoable] applied is_geoable to room uid={} index={}", uid, room->room_index);
                break;
            }
        }
        if (!found) {
            xlog::warn("[Geoable] room uid={} not found in solid->all_rooms", uid);
        }
    }
}

// Find geoable detail rooms whose bboxes overlap the given position with padding
// scaled by level hardness. Base padding is 3 units at hardness 50 (baseline).
// Hardness 0 = 2x padding (softer rock, larger craters), hardness 100 = 0.5x padding.
// Formula: scale = 2^((50 - hardness) / 50), so 0→2.0, 50→1.0, 99→~0.507, 100→0.5.
static constexpr float geoable_bbox_base_padding = 3.0f;

static float get_hardness_scaled_padding()
{
    int hardness = std::clamp(rf::level.default_rock_hardness, 0, 100);
    float scale = std::pow(2.0f, (50.0f - hardness) / 50.0f);
    return geoable_bbox_base_padding * scale;
}

// Results are sorted by distance from pos to bbox center (closest first).
static std::vector<rf::GRoom*> find_overlapping_detail_rooms(const rf::Vector3& pos)
{
    float padding = get_hardness_scaled_padding();
    std::vector<rf::GRoom*> result;
    auto* solid = rf::level.geometry;
    if (!solid) return result;

    for (auto& room : solid->all_rooms) {
        if (!room->is_detail || !room->is_geoable) continue;
        // Check if position is within room bbox + hardness-scaled padding
        if (pos.x >= room->bbox_min.x - padding && pos.x <= room->bbox_max.x + padding &&
            pos.y >= room->bbox_min.y - padding && pos.y <= room->bbox_max.y + padding &&
            pos.z >= room->bbox_min.z - padding && pos.z <= room->bbox_max.z + padding) {
            result.push_back(room);
        }
    }

    // Sort by distance from pos to bbox center (closest first)
    std::sort(result.begin(), result.end(), [&pos](rf::GRoom* a, rf::GRoom* b) {
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
    });

    return result;
}

// Hook FUN_00467020 — the master "create geomod" function called by the explosion system.
// This is the earliest point where we can gate geomod: it creates visual effects (particle
// emitters for rock debris, geomod explosion vclip, geomod sound) AND queues the boolean
// request. Returning 0 from here prevents ALL geomod visuals and processing from starting.
// param_4 (4th parameter) is the explosion position (rf::Vector3*).
static int g_rf2_geomod_counter = 0;

FunHook<uint8_t(float, void*, int, rf::Vector3*, void*, unsigned int, unsigned int)> geomod_create_hook{
    0x00467020,
    [](float radius, void* param2, int param3, rf::Vector3* pos, void* param5, unsigned int crater_idx, unsigned int flags) -> uint8_t {
        bool rf2_enabled = AlpineLevelProperties::instance().rf2_style_geomod;
        bool is_rf2_geomod = false;

        if (rf2_enabled && pos) {
            bool in_geo_region = is_pos_in_any_geo_region(*pos);
            if (!in_geo_region) {
                is_rf2_geomod = true;

                // Check RF2 geo limit
                if (g_rf2_geo_limit == 0) {
                    return 0; // RF2 geomods disabled
                }
                if (g_rf2_geo_limit > 0 && g_rf2_geo_count >= g_rf2_geo_limit) {
                    return 0; // RF2 limit reached
                }

                // Effects gate: check if explosion is near any geoable detail room
                // using bbox + padding. Reliable for all geometry shapes including
                // concave brushes and touching detail brushes.
                auto overlapping = find_overlapping_detail_rooms(*pos);
                if (overlapping.empty()) {
                    g_rf2_geomod_counter++;
                    xlog::info("[RF2] geomod_create #{}: pos=({:.1f},{:.1f},{:.1f}) not near any geoable room -> SKIPPING",
                        g_rf2_geomod_counter, pos->x, pos->y, pos->z);
                    return 0; // skip entire geomod (no effects, no boolean, no state machine)
                }
            }
        }

        // For RF2-style geomods: bypass both the soft multiplayer limit and the hard
        // 128 crater record limit by temporarily zeroing g_num_geomods_this_level and
        // raising multi_geo_limit. The stock function writes a crater record at slot 0
        // (which gets overwritten by future geomods — RF2 geomods don't need persistent
        // crater records since the carved geometry IS the visual result).
        //
        // For normal geomods in RF2-enabled levels: compensate the soft limit for RF2
        // entries in the persistent counter (DAT_0063715c) so RF2 geomods don't consume
        // normal limit quota. If rf2_geo_count is 10 and geo_limit is 64, the effective
        // limit becomes 74 — so 64 normal + 10 RF2 = 74 total triggers the block.
        auto& stock_limit = rf::multi_geo_limit;
        int saved_limit = stock_limit;
        int saved_crater_count = rf::g_num_geomods_this_level;

        if (is_rf2_geomod) {
            if (stock_limit > 0) stock_limit = INT_MAX;
            rf::g_num_geomods_this_level = 0;
        }
        else if (rf2_enabled && stock_limit > 0 && g_rf2_geo_count > 0) {
            stock_limit += g_rf2_geo_count;
        }

        auto result = geomod_create_hook.call_target(radius, param2, param3, pos, param5, crater_idx, flags);
        stock_limit = saved_limit;

        if (is_rf2_geomod) {
            rf::g_num_geomods_this_level = saved_crater_count;
            if (result) g_rf2_geo_count++;
        }

        return result;
    },
};

// Hook geomod_init (FUN_00466b00) to activate RF2-style boolean targeting.
// By this point, geomod_create_hook has already verified geoable rooms exist,
// so overlapping should always be non-empty when RF2-style is active.
// Both server and client deterministically derive this from position + level properties,
// so the pregame boolean packet (which omits the flags field) works correctly.
FunHook<void(void*)> geomod_init_hook{
    0x00466B00,
    [](void* entry_data) {
        geomod_init_hook.call_target(entry_data);

        bool rf2_enabled = AlpineLevelProperties::instance().rf2_style_geomod;
        bool in_geo_region = rf2_enabled && is_pos_in_any_geo_region(geomod_pos);
        g_rf2_style_boolean_active = rf2_enabled && !in_geo_region;

        if (g_rf2_style_boolean_active) {
            g_rf2_geomod_counter++;

            // Save original detail room planes on first RF2-style geomod.
            if (!g_detail_planes_initialized) {
                save_original_detail_room_planes();
                g_detail_planes_initialized = true;
            }

            // Find detail rooms overlapping the crater and select the first target.
            auto overlapping = find_overlapping_detail_rooms(geomod_pos);
            g_rf2_pending_detail_rooms.clear();
            if (!overlapping.empty()) {
                g_rf2_target_detail_room = overlapping[0];
                for (size_t i = 1; i < overlapping.size(); i++) {
                    g_rf2_pending_detail_rooms.push_back(overlapping[i]);
                }
                xlog::info("[RF2] geomod_init #{}: pos=({:.1f},{:.1f},{:.1f}) target_room={} (index={}) pending={}",
                    g_rf2_geomod_counter, geomod_pos.x, geomod_pos.y, geomod_pos.z,
                    (void*)g_rf2_target_detail_room, g_rf2_target_detail_room->room_index,
                    g_rf2_pending_detail_rooms.size());
            } else {
                g_rf2_target_detail_room = nullptr;
                xlog::warn("[RF2] geomod_init #{}: no overlapping geoable rooms (unexpected — geomod_create should have filtered)",
                    g_rf2_geomod_counter);
            }
        }
    },
};

// Inner boolean state variable (DAT_005a3a34) — tracks states 0-7 in FUN_004dbc50.
static auto& boolean_inner_state = addr_as_ref<int>(0x005a3a34);

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
        int result = boolean_iterate_hook.call_target();
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
// D3D11: Invalidate surgically — null detail room caches (lazily recreated) and
//        mark normal room caches as invalid (state_ = 2 at offset 0x20 triggers
//        rebuild on next render). We CANNOT call the full clear_cache() because
//        destroying and recreating all RoomRenderCache objects causes a freeze.
static void invalidate_rf2_render_caches()
{
    rf::GSolid* solid = rf::level.geometry;
    if (!solid)
        return;

    // Safety net: clear any remaining corrupted detail_rooms. The primary clearing
    // happens in boolean_iterate_hook when the boolean completes, but this catches
    // any edge cases (e.g., multi-room redirect between boolean passes).
    clear_corrupted_detail_rooms();

    if (!is_d3d11()) {
        AddrCaller{0x004f0b90}.c_call();
        return;
    }

    for (int i = 0; i < solid->all_rooms.size(); i++) {
        rf::GRoom* room = solid->all_rooms[i];
        if (!room || !room->geo_cache)
            continue;
        if (room->is_detail) {
            room->geo_cache = nullptr;
        } else {
            auto* state = reinterpret_cast<int*>(
                reinterpret_cast<char*>(room->geo_cache) + 0x20);
            *state = 2;
        }
    }
}

// Outer geomod state variable (DAT_0059c9f4): States 0-3, then -1 = done.
static auto& geomod_outer_state = addr_as_ref<int>(0x0059c9f4);

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
                addr_as_ref<uint32_t>(0x006460e8),  // level solid
                addr_as_ref<uint32_t>(0x00646a20),  // crater solid
                3u,                                  // op_mode = geomod
                0u,                                  // param4
                0x006485a0u,                         // &position
                0x00647ca8u,                         // &orientation
                addr_as_ref<uint32_t>(0x00647c94),  // texture index
                2u,                                  // param8
                addr_as_ref<uint32_t>(0x00648598),  // scale (float bits as uint32)
                0x00647ca0u,                         // &crater bbox
                0u,                                  // param11
                0x006485b0u,                         // &param12
                0x006485c0u                          // &param13
            );

            // Verify boolean_setup succeeded — it sets inner state to 0 when resources
            // are available. If it silently failed (resources exhausted), inner state
            // stays at -1 and we'd cycle 1→2→3→1→... endlessly. Abort remaining rooms.
            if (boolean_inner_state != 0) {
                xlog::warn("[RF2] boolean_setup failed for room {} (resource exhaustion, inner_state={}), "
                    "skipping {} remaining rooms",
                    g_rf2_target_detail_room->room_index, boolean_inner_state,
                    g_rf2_pending_detail_rooms.size());
                g_rf2_pending_detail_rooms.clear();
                // Let State 3 proceed normally (debris/decals for the rooms we did process)
                return;
            }

            // Set outer state back to 1 (boolean_iterate) so the state machine
            // runs the boolean for the next room on subsequent frames
            geomod_outer_state = 1;

            // Skip rest of State 3 (no debris/decals for intermediate rooms)
            regs.eip = 0x00466fb5;
            return;
        }

        // No more pending rooms — check if any geoable room was actually targeted.
        // When g_rf2_target_detail_room is nullptr, no geoable detail rooms overlapped
        // the crater, so the boolean produced no visible geometry change. Skip ALL
        // State 3 effects (rock debris, decal updates, crater decals, foley sound).
        if (g_rf2_target_detail_room == nullptr) {
            xlog::info("[RF2] State 3: SUPPRESSING all effects (no geoable room targeted)");
            geomod_outer_state = -1; // mark geomod as done (0x00466fab equivalent)
            regs.eip = 0x00466fb5;   // jump to cleanup/exit
            return;
        }
        xlog::info("[RF2] State 3: ALLOWING effects (target_room={}, index={})",
            (void*)g_rf2_target_detail_room, g_rf2_target_detail_room->room_index);
        // State 3 proceeds normally — all effects fire for the successfully carved room(s)
    },
};

// verify proposed new sky room UID is a sky room and if so, set it as the override
void set_sky_room_uid_override(int room_uid, int anchor_uid, bool relative_position, float position_scale)
{
    // stock behaviour if no room specified
    if (room_uid <= 0) {
        g_sky_room_uid_override.reset();
        g_sky_room_eye_anchor.reset();
        g_sky_room_eye_offset_scale.reset();
        return;
    }

    // set new sky room if it's a valid room
    if (auto* possible_new_sky_room = rf::level_room_from_uid(room_uid)) {
        g_sky_room_uid_override = room_uid;
    }
    else {
        g_sky_room_uid_override.reset();
        g_sky_room_eye_anchor.reset();
        g_sky_room_eye_offset_scale.reset();
        return;
    }

    // no anchor specified
    if (anchor_uid <= 0) {
        g_sky_room_eye_anchor.reset();
        g_sky_room_eye_offset_scale.reset();
        return;
    }

    if (auto* possible_new_sky_eye_anchor = rf::obj_lookup_from_uid(anchor_uid)) {
        if (check_if_object_is_event_type(possible_new_sky_eye_anchor, rf::EventType::Anchor_Marker) ||
            check_if_object_is_event_type(possible_new_sky_eye_anchor, rf::EventType::Anchor_Marker_Orient)) {
            g_sky_room_eye_anchor = possible_new_sky_eye_anchor;

            g_sky_room_eye_offset_scale =
                (relative_position && position_scale > 0)
                ? std::make_optional(position_scale)
                : std::nullopt;
        }
    }
}

FunHook<rf::GRoom*(rf::GSolid*)> find_sky_room_hook{
    0x004D4B90,
    [](rf::GSolid* solid) {
        // Use original game behavior if no override is set
        if (!g_sky_room_uid_override) {
            return find_sky_room_hook.call_target(solid);
        }

        // Attempt to fetch the room directly by UID
        const int new_sky_room_uid = *g_sky_room_uid_override;
        if (auto* room = rf::level_room_from_uid(new_sky_room_uid)) {
            return room;
        }

        // Fallback to original game behavior if the room isn't found
        return find_sky_room_hook.call_target(solid);
    },
};

CodeInjection sky_room_eye_position_patch{
    0x004D3A0C,
    [](auto& regs) {
        // if we don't have an anchor, go to center of the room with original game logic
        if (!g_sky_room_eye_anchor) {
            return;
        }

        // anchor in world space
        rf::Object* eye_anchor = *g_sky_room_eye_anchor;
        rf::Vector3 anchor_pos = eye_anchor->pos;

        // translate position is on, calculate based on camera distance from 0,0,0 world space
        if (g_sky_room_eye_offset_scale && *g_sky_room_eye_offset_scale > 0) {

            rf::Player* local_player = rf::local_player;
            if (!local_player || !local_player->cam) {
                return;
            }

            // camera in world space
            rf::Camera* camera = local_player->cam;
            rf::Vector3 camera_pos = rf::camera_get_pos(camera);

            // translate camera position (relative to world origin) to eye position (relative to anchor)
            g_adjusted_sky_room_eye_position = {anchor_pos.x + (camera_pos.x * *g_sky_room_eye_offset_scale),
                                                anchor_pos.y + (camera_pos.y * *g_sky_room_eye_offset_scale),
                                                anchor_pos.z + (camera_pos.z * *g_sky_room_eye_offset_scale)};
        }
        else {
            // translate position is off, just use anchor position as the eye position
            g_adjusted_sky_room_eye_position = anchor_pos;
        }

        regs.eax = reinterpret_cast<int32_t>(&g_adjusted_sky_room_eye_position);
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
            // Return null so the stock code skips the write to geo_cache + 0x20
            regs.eax = static_cast<int32_t>(0);
        } else {
            regs.eax = cache;
        }
    },
};

// RF2-style geomod: conditionally suppress crater decal effects.
// FUN_00492400 is called from geomod state machine State 3 to apply crater marks
// at the explosion point. It has only one caller (the geomod state machine).
// When RF2-style is active and a geoable room was carved, allow crater decals
// (they mark nearby world surfaces around the detail brush crater).
// When no geoable room was targeted, suppress them (no visible geometry change occurred).
FunHook<void(rf::Vector3*, float)> geomod_crater_decals_hook{
    0x00492400,
    [](rf::Vector3* pos, float radius) {
        if (g_rf2_style_boolean_active && g_rf2_target_detail_room == nullptr) {
            return; // suppress crater decals when no geoable room was targeted
        }
        geomod_crater_decals_hook.call_target(pos, radius);
    },
};

// RF2-style geomod: conditionally suppress impact effects.
// FUN_00490900 is called from geomod state machine State 3 (at 0x00466f89) to update
// decal states near the explosion point. When RF2-style is active and a geoable room
// was carved, allow impact effects (they update decals on nearby world surfaces).
// When no geoable room was targeted, suppress them.
//
// Uses CallHook (hooks the CALL site) instead of FunHook (hooks function entry) because
// SubHook cannot decode the x87 FPU instruction (opcode 0xd8) at 0x490904, making it
// unable to create a trampoline for FUN_00490900. FUN_00490900 has only one caller.
CallHook<void(rf::Vector3*, float)> geomod_impact_effects_hook{
    0x00466f89,
    [](rf::Vector3* pos, float radius) {
        if (g_rf2_style_boolean_active && g_rf2_target_detail_room == nullptr) {
            return; // suppress impact effects when no geoable room was targeted
        }
        geomod_impact_effects_hook.call_target(pos, radius);
    },
};

// clean up sky room overrides when shutting down level
CodeInjection level_release_sky_room_shutdown_patch{
    0x0045CAF9,
    [](auto& regs) {
        set_sky_room_uid_override(-1, -1, false, -1);
        g_rf2_style_boolean_active = false;
        g_rf2_target_detail_room = nullptr;
        g_rf2_pending_detail_rooms.clear();
        g_detail_planes_initialized = false;
        g_saved_detail_room_planes.clear();
        g_rf2_geomod_counter = 0;
        g_rf2_geo_count = 0;
        AlpineLevelProperties::instance().geoable_room_uids.clear();
    },
};

void g_solid_set_rf2_geo_limit(int limit)
{
    g_rf2_geo_limit = limit;
}

int g_solid_get_rf2_geo_limit()
{
    return g_rf2_geo_limit;
}

void g_solid_do_patch()
{
    // allow Set_Skybox to set a specific sky room
    find_sky_room_hook.install();
    sky_room_eye_position_patch.install();
    level_release_sky_room_shutdown_patch.install();

    // Buffer overflows in solid_read
    // Note: Buffer size is 1024 but opcode allows only 1 byte size
    //       What is more important bm_load copies texture name to 32 bytes long buffers
    write_mem<i8>(0x004ED612 + 1, 32);
    write_mem<i8>(0x004ED66E + 1, 32);
    write_mem<i8>(0x004ED72E + 1, 32);
    write_mem<i8>(0x004EDB02 + 1, 32);

    // Fix crash in geometry rendering
    geo_cache_prepare_room_hook.install();

    // 32-bit color format - geomod
    GSurface_calculate_lightmap_color_conv_patch.install();
    GSurface_alloc_lightmap_color_conv_patch.install();
    // 32-bit color format - ambient color
    GSolid_get_ambient_color_from_lightmap_patch.install();
    // water
    AsmWriter(0x004E68B0, 0x004E68B6).nop();
    g_proctex_update_water_patch.install();

    // Fix face scroll in levels after version 0xB4
    face_scroll_fix.install();

    // Fix faces with "fullbright" flag having randomly generated lightmaps applied after geomod
    ingame_add_lightmap_to_face_fullbright_fix.install();

    // Fix water waves animation on high FPS
    AsmWriter(0x004E68A0, 0x004E68A9).nop();
    AsmWriter(0x004E68B6, 0x004E68D1).nop();
    g_proctex_update_water_speed_fix.install();

    // Set dynamic flag on proctex texture
    g_proctex_create_bm_create_injection.install();

    // Add a missing check if face has any vertex in GFace::does_point_lie_in_face
    g_face_does_point_lie_in_face_crashfix.install();

    // fix pixel format for lightmaps
    write_mem<u8>(0x004F5EB8 + 1, rf::bm::FORMAT_888_RGB);

    // lightmaps format conversion
    level_load_lightmaps_color_conv_patch.install();

    // When rendering shadows check mover's bounding box before processing its faces
    shadow_render_one_injection.install();

    // Change decals limit
    decal_patch_limit(512);
    AsmWriter{0x004D54AF}.nop(2); // fix subhook trampoline preparation error
    g_decal_add_internal_cmp_global_weak_limit_injection.install();

    // When rendering semi-transparent objects do not group objects that are behind a detail room.
    // Grouping breaks sorting in many cases because orientation and sizes of detail rooms and objects
    // are not taken into account.
    AsmWriter{0x004D4409}.jmp(0x004D44B1);
    AsmWriter{0x004D44C7}.nop(2);

    // Set PPM for geo crater texture based on its resolution instead of static value of 32.0
    levelmod_do_blast_autotexture_ppm_patch.install();

    // RF2-style geomod: detail brush only mode
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
    geomod_state3_clear_detail_caches_injection.install();
    geomod_crater_decals_hook.install();
    geomod_impact_effects_hook.install();

    // Commands
    max_decals_cmd.register_cmd();
    dbg_room_clip_wnd_cmd.register_cmd();
    dbg_num_geomods_cmd.register_cmd();
    lighting_color_range_cmd.register_cmd();
    clamp_official_lightmaps_cmd.register_cmd();
}
