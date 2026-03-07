#include <patch_common/FunHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/AsmWriter.h>
#include <patch_common/MemUtils.h>
#include <xlog/xlog.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>
#include "level.h"
#include "vtypes.h"
#include "mfc_types.h"
#include "resources.h"

// Forward declarations
int get_level_rfl_version();
void set_initial_level_rfl_version();

// add AlpineLevelProperties chunk after stock game chunks when creating a new level
CodeInjection CDedLevel_construct_patch{
    0x004181B8,
    [](auto& regs) {
        set_initial_level_rfl_version();
        std::byte* level = regs.esi;
        new (&level[stock_cdedlevel_size]) AlpineLevelProperties();
    },
};

// load default AlpineLevelProperties values
CodeInjection CDedLevel_LoadLevel_patch1{
    0x0042F136,
    []() {
        CDedLevel::Get()->GetAlpineLevelProperties().LoadDefaults();
    },
};

// load AlpineLevelProperties chunk from rfl file
CodeInjection CDedLevel_LoadLevel_patch2{
    0x0042F2D4,
    [](auto& regs) {
        auto& file = *static_cast<rf::File*>(regs.esi);

        // Alpine level properties chunk was introduced in rfl v302, no point looking for it before that
        if (file.check_version(302)) {
            auto& level = *static_cast<CDedLevel*>(regs.ebp);
            int chunk_id = regs.edi;
            std::size_t chunk_size = regs.ebx;
            if (chunk_id == alpine_props_chunk_id) {
                auto& alpine_level_props = level.GetAlpineLevelProperties();
                alpine_level_props.Deserialize(file, chunk_size);
                regs.eip = 0x0043090C;
            }
        }
    },
};

// At save time, match geoable brush UIDs to compiled room UIDs via position.
// For each geoable brush, find the detail room whose bbox contains the brush position.
// Find the compiled room that contains a brush's faces by matching face_ids.
// This is the primary method for mapping brushes to rooms — it directly traces
// brush geometry faces through the compiled solid to find which room they ended up in.
// Returns nullptr if no matching face is found (e.g., brush geometry is null).
static GRoom* find_room_by_face_ids(const CDedLevel& level, int32_t brush_uid)
{
    // Find the brush node
    BrushNode* brush_node = nullptr;
    BrushNode* node = level.brush_list;
    if (node) {
        do {
            if (node->uid == brush_uid) {
                brush_node = node;
                break;
            }
            node = node->next;
        } while (node && node != level.brush_list);
    }
    if (!brush_node) return nullptr;

    // Collect face_ids from the brush's source geometry
    auto* brush_geom = static_cast<GSolid*>(brush_node->geometry);
    if (!brush_geom) return nullptr;

    std::unordered_set<int> brush_face_ids;
    for (GFace* face = brush_geom->face_list_head; face; face = face->next_solid) {
        if (face->face_id >= 0) {
            brush_face_ids.insert(face->face_id);
        }
    }
    if (brush_face_ids.empty()) return nullptr;

    // Search the compiled solid's faces for any matching face_id
    for (GFace* face = level.solid->face_list_head; face; face = face->next_solid) {
        if (face->which_room && face->which_room->is_detail &&
            brush_face_ids.count(face->face_id)) {
            return face->which_room;
        }
    }
    return nullptr;
}

// Fallback: find the detail room whose bbox center is closest to the brush position.
// Used when face_id matching fails (e.g., face_ids weren't assigned).
static GRoom* find_room_by_position(const CDedLevel& level, int32_t brush_uid)
{
    BrushNode* node = level.brush_list;
    Vector3 brush_pos;
    bool found = false;
    if (node) {
        do {
            if (node->uid == brush_uid) {
                brush_pos = node->pos;
                found = true;
                break;
            }
            node = node->next;
        } while (node && node != level.brush_list);
    }
    if (!found) return nullptr;

    constexpr float tolerance = 2.0f;
    GRoom* best_room = nullptr;
    float best_dist_sq = FLT_MAX;
    auto& all_rooms = level.solid->all_rooms;
    for (int j = 0; j < all_rooms.get_size(); j++) {
        GRoom* room = all_rooms.data_ptr[j];
        if (!room || !room->is_detail) continue;
        if (brush_pos.x >= room->bbox_min.x - tolerance && brush_pos.x <= room->bbox_max.x + tolerance &&
            brush_pos.y >= room->bbox_min.y - tolerance && brush_pos.y <= room->bbox_max.y + tolerance &&
            brush_pos.z >= room->bbox_min.z - tolerance && brush_pos.z <= room->bbox_max.z + tolerance) {
            float cx = (room->bbox_min.x + room->bbox_max.x) * 0.5f;
            float cy = (room->bbox_min.y + room->bbox_max.y) * 0.5f;
            float cz = (room->bbox_min.z + room->bbox_max.z) * 0.5f;
            float dx = brush_pos.x - cx, dy = brush_pos.y - cy, dz = brush_pos.z - cz;
            float dist_sq = dx * dx + dy * dy + dz * dz;
            if (dist_sq < best_dist_sq) {
                best_dist_sq = dist_sq;
                best_room = room;
            }
        }
    }

    if (best_room) {
        xlog::debug("[Geoable] brush uid={} matched by position fallback to room uid={}",
            brush_uid, best_room->uid);
    }
    else {
        xlog::debug("[Geoable] brush uid={} at ({:.2f},{:.2f},{:.2f}) has no matching detail room",
            brush_uid, brush_pos.x, brush_pos.y, brush_pos.z);
    }
    return best_room;
}

static void compute_geoable_room_uids(CDedLevel& level, AlpineLevelProperties& props)
{
    // Prune stale UIDs: remove any geoable brush UIDs that no longer exist in the
    // brush list (e.g., the brush was deleted). This must happen before room matching.
    {
        std::unordered_set<int32_t> live_uids;
        BrushNode* node = level.brush_list;
        if (node) {
            do {
                live_uids.insert(node->uid);
                node = node->next;
            } while (node && node != level.brush_list);
        }
        auto& uids = props.geoable_brush_uids;
        uids.erase(std::remove_if(uids.begin(), uids.end(),
            [&live_uids](int32_t uid) { return live_uids.find(uid) == live_uids.end(); }),
            uids.end());
    }

    props.geoable_room_uids.clear();
    props.geoable_room_uids.resize(props.geoable_brush_uids.size(), 0);

    if (!level.solid) {
        xlog::debug("[Geoable] compute_room_uids: no compiled solid");
        return;
    }

    auto& all_rooms = level.solid->all_rooms;

    // The editor's build/compile process creates final rooms as copies that lack UIDs (uid=-1).
    // Final rooms in all_rooms are clones that skip the GRoom constructor and therefore never get
    // a UID. Assign UIDs here so solid_write persists them to the .rfl and our geoable mapping
    // can reference them.
    for (int j = 0; j < all_rooms.get_size(); j++) {
        GRoom* room = all_rooms.data_ptr[j];
        if (room && room->uid == -1) {
            room->uid = g_groom_uid_counter--;
        }
    }

    for (std::size_t i = 0; i < props.geoable_brush_uids.size(); i++) {
        int32_t brush_uid = props.geoable_brush_uids[i];

        // Primary method: trace brush face_ids through compiled solid to find the exact room.
        // This is reliable because the room isolation code ensures each geoable brush's faces
        // end up in their own dedicated room, and face_ids are preserved during compilation.
        GRoom* room = find_room_by_face_ids(level, brush_uid);
        if (room) {
            props.geoable_room_uids[i] = room->uid;
            xlog::debug("[Geoable] brush uid={} matched by face_id to room uid={} index={}",
                brush_uid, room->uid, room->room_index);
            continue;
        }

        // Fallback: position-based matching (for edge cases where face_ids aren't available)
        room = find_room_by_position(level, brush_uid);
        if (room) {
            props.geoable_room_uids[i] = room->uid;
        }
    }

}

// At save time, match breakable brush UIDs to compiled room UIDs.
// Uses face_id matching (primary) with position-based fallback.
static void compute_breakable_room_uids(CDedLevel& level, AlpineLevelProperties& props)
{
    // Prune stale UIDs
    {
        std::unordered_set<int32_t> live_uids;
        BrushNode* node = level.brush_list;
        if (node) {
            do {
                live_uids.insert(node->uid);
                node = node->next;
            } while (node && node != level.brush_list);
        }
        for (std::size_t i = 0; i < props.breakable_brush_uids.size(); ) {
            if (live_uids.find(props.breakable_brush_uids[i]) == live_uids.end()) {
                props.breakable_brush_uids.erase(props.breakable_brush_uids.begin() + i);
                props.breakable_room_uids.erase(props.breakable_room_uids.begin() + i);
                props.breakable_materials.erase(props.breakable_materials.begin() + i);
            } else {
                i++;
            }
        }
    }

    // Clear room UIDs — will be recomputed
    for (auto& uid : props.breakable_room_uids) uid = 0;

    if (!level.solid) {
        xlog::debug("[Breakable] compute_room_uids: no compiled solid");
        return;
    }

    for (std::size_t i = 0; i < props.breakable_brush_uids.size(); i++) {
        int32_t brush_uid = props.breakable_brush_uids[i];

        // Primary method: trace brush face_ids through compiled solid to find the exact room.
        GRoom* room = find_room_by_face_ids(level, brush_uid);
        if (room) {
            props.breakable_room_uids[i] = room->uid;
            xlog::debug("[Breakable] brush uid={} matched by face_id to room uid={} index={}",
                brush_uid, room->uid, room->room_index);
            continue;
        }

        // Fallback: position-based matching
        room = find_room_by_position(level, brush_uid);
        if (room) {
            props.breakable_room_uids[i] = room->uid;
        }
        else {
            xlog::debug("[Breakable] brush uid={} has no matching detail room", brush_uid);
        }
    }
}

// ─── Geoable room isolation ───────────────────────────────────────────────────
// During geometry building, the room builder (FUN_00485990) flood-fills adjacent
// coplanar faces into the same GRoom. This merges geoable and non-geoable detail
// brushes into one room, breaking in-game geomod. Similarly, breakable detail brushes
// must each be in their own room for destruction to work correctly. The hooks below
// ensure that each geoable/breakable brush always gets its own self-contained room:
//   1. adjacency_test_hook prevents merging via the geometric adjacency path
//   2. isolate_marked_rooms post-processes to split any remaining mixed rooms
//      (the flood-fill also merges via edge-based adjacency which bypasses the
//       adjacency test, so post-processing is the primary fix)

// Map from face_id (GFaceAttributes+0x10) to brush UID for brushes that need isolation
// (geoable and breakable detail brushes). Populated before room building, cleared afterwards.
static std::unordered_map<int, int> g_isolated_face_map;

static void populate_isolated_face_map()
{
    g_isolated_face_map.clear();

    auto* level = CDedLevel::Get();
    if (!level) return;
    auto& props = level->GetAlpineLevelProperties();
    if (props.geoable_brush_uids.empty() && props.breakable_brush_uids.empty()) return;

    std::unordered_set<int32_t> isolated_set;
    isolated_set.insert(props.geoable_brush_uids.begin(), props.geoable_brush_uids.end());
    isolated_set.insert(props.breakable_brush_uids.begin(), props.breakable_brush_uids.end());

    BrushNode* head = level->brush_list;
    if (!head) return;
    BrushNode* node = head;
    do {
        if (node->is_detail && isolated_set.count(node->uid)) {
            auto* geom = static_cast<GSolid*>(node->geometry);
            if (geom) {
                for (GFace* face = geom->face_list_head; face; face = face->next_solid) {
                    g_isolated_face_map[face->face_id] = node->uid;
                }
            }
        }
        node = node->next;
    } while (node && node != head);
}

// Split any rooms that contain faces from multiple isolated brushes (geoable or
// breakable) or mixed isolated/non-isolated faces.  Each isolated brush gets its
// own room so in-game geomod and destruction only affect the intended brush.
//
// Called from inside FUN_00485990 (room builder) via CodeInjection at 0x00485e88,
// which is after the detail-marking loop (loop 1) but before the parent-room
// association loop (loop 2).  This ensures:
//   - new rooms are properly associated with parent non-detail rooms (loop 2)
//   - spatial data is rebuilt for all rooms including new ones (final loop)
// Recompute a room's bbox_min/bbox_max from its current face list.
// GRoom::add_face only expands the bbox and removing faces does not shrink it,
// so after splitting faces out we must recompute from scratch.
static void recompute_room_bbox(GRoom* room)
{
    // Walk the room's face list (VList<GFace, FACE_LIST_ROOM> at +0x28)
    // head is at +0x28, faces linked via next_room (+0x5C)
    GFace* head = *reinterpret_cast<GFace**>(reinterpret_cast<char*>(room) + 0x28);
    if (!head) return;

    Vector3 vmin = head->bounding_box_min;
    Vector3 vmax = head->bounding_box_max;

    for (GFace* f = head->next_room; f; f = f->next_room) {
        if (f->bounding_box_min.x < vmin.x) vmin.x = f->bounding_box_min.x;
        if (f->bounding_box_min.y < vmin.y) vmin.y = f->bounding_box_min.y;
        if (f->bounding_box_min.z < vmin.z) vmin.z = f->bounding_box_min.z;
        if (f->bounding_box_max.x > vmax.x) vmax.x = f->bounding_box_max.x;
        if (f->bounding_box_max.y > vmax.y) vmax.y = f->bounding_box_max.y;
        if (f->bounding_box_max.z > vmax.z) vmax.z = f->bounding_box_max.z;
    }

    room->bbox_min = vmin;
    room->bbox_max = vmax;
}

static void isolate_marked_rooms(GSolid* solid)
{
    // Group faces by room, then by isolated brush UID (-1 = not isolated)
    struct FaceGroup {
        std::unordered_map<int, std::vector<GFace*>> by_brush;
    };
    std::unordered_map<GRoom*, FaceGroup> room_groups;

    for (GFace* face = solid->face_list_head; face; face = face->next_solid) {
        GRoom* room = face->which_room;
        if (!room) continue;
        auto it = g_isolated_face_map.find(face->face_id);
        int uid = (it != g_isolated_face_map.end()) ? it->second : -1;
        room_groups[room].by_brush[uid].push_back(face);
    }

    int rooms_created = 0;
    std::vector<GRoom*> modified_rooms; // original rooms that had faces removed

    for (auto& [room, fg] : room_groups) {
        int isolated_count = 0;
        bool has_unmarked = fg.by_brush.count(-1) > 0;
        for (auto& [uid, faces] : fg.by_brush) {
            if (uid != -1) isolated_count++;
        }

        if (isolated_count == 0) continue;                 // no isolated faces
        if (!has_unmarked && isolated_count <= 1) continue; // single isolated brush, no mixing

        // Room has mixed content — split each isolated brush into its own room
        bool first_isolated = true;
        for (auto& [uid, faces] : fg.by_brush) {
            if (uid == -1) continue;

            // If room has only isolated faces (no unmarked), keep the first
            // isolated group in the original room to avoid creating an empty room
            if (!has_unmarked && first_isolated) {
                first_isolated = false;
                continue;
            }
            first_isolated = false;

            // Use first face's bbox to initialize the new room
            GFace* seed = faces[0];

            GRoom* new_room = GRoom::alloc();
            if (!new_room) continue;
            new_room->init(solid, &seed->bounding_box_min, &seed->bounding_box_max);

            for (GFace* f : faces) {
                new_room->add_face(f);
            }

            new_room->set_detail(solid, 1);

            rooms_created++;

            xlog::debug("[RoomIsolation] isolated brush uid={}: {} faces -> new room",
                       uid, faces.size());
        }

        // Original room had faces removed — needs bbox recomputation
        modified_rooms.push_back(room);
    }

    // Recompute bbox for original rooms that lost faces. add_face only expands
    // the bbox when adding faces; it does not shrink when faces are removed.
    for (GRoom* room : modified_rooms) {
        recompute_room_bbox(room);
    }

    if (rooms_created > 0) {
        xlog::debug("[RoomIsolation] created {} isolated rooms", rooms_created);
    }
}

// CodeInjection inside FUN_00485990 at 0x00485e88: after the detail-marking
// loop (loop 1), before the parent-room association loop (loop 2).
// At this address EBP = solid (GSolid* this, set at 0x004859aa).
// The subsequent loops naturally handle parent association, portal creation,
// and spatial data rebuilding for any new rooms we create here.
CodeInjection isolate_rooms_injection{
    0x00485e88,
    [](auto& regs) {
        if (g_isolated_face_map.empty()) return;
        auto* solid = reinterpret_cast<GSolid*>(static_cast<std::byte*>(regs.ebp));
        isolate_marked_rooms(solid);
    },
};

// Hook FUN_00485990 (room builder, thiscall on GSolid*) to populate/clear
// the face_id → brush UID map around the room builder execution.
void __fastcall build_rooms_hooked(GSolid* solid, void* edx_unused);
FunHook<decltype(build_rooms_hooked)> build_rooms_hook{
    0x00485990,
    build_rooms_hooked,
};
void __fastcall build_rooms_hooked(GSolid* solid, void* edx_unused)
{
    populate_isolated_face_map();
    build_rooms_hook.call_target(solid, edx_unused);
    g_isolated_face_map.clear();
}

// Hook FUN_004861d0 (face adjacency test, cdecl) as secondary defense.
// The primary fix is post-processing in isolate_marked_rooms, but this hook
// also prevents merging via the geometric adjacency path during flood-fill.
bool __cdecl adjacency_test_hooked(GFace* face1, GFace* face2);
FunHook<decltype(adjacency_test_hooked)> adjacency_test_hook{
    0x004861d0,
    adjacency_test_hooked,
};

bool __cdecl adjacency_test_hooked(GFace* face1, GFace* face2)
{
    bool result = adjacency_test_hook.call_target(face1, face2);
    if (!result || g_isolated_face_map.empty()) return result;

    auto it1 = g_isolated_face_map.find(face1->face_id);
    auto it2 = g_isolated_face_map.find(face2->face_id);

    bool iso1 = (it1 != g_isolated_face_map.end());
    bool iso2 = (it2 != g_isolated_face_map.end());

    if (!iso1 && !iso2) return true;   // both unmarked: allow
    if (iso1 != iso2) return false;    // mixed: block
    if (it1->second != it2->second) return false; // different isolated brushes: block
    return true;                        // same isolated brush: allow
}

// save AlpineLevelProperties when saving rfl file
CodeInjection CDedLevel_SaveLevel_patch{
    0x00430CBD,
    [](auto& regs) {
        auto& level = *static_cast<CDedLevel*>(regs.edi);
        auto& file = *static_cast<rf::File*>(regs.esi);

        // Compute room UIDs from brush UIDs before serializing
        auto& alpine_level_props = level.GetAlpineLevelProperties();
        compute_geoable_room_uids(level, alpine_level_props);
        compute_breakable_room_uids(level, alpine_level_props);

        auto start_pos = level.BeginRflSection(file, alpine_props_chunk_id);
        alpine_level_props.Serialize(file);
        level.EndRflSection(file, start_pos);
    },
};

// load AlpineLevelProperties settings when opening level properties dialog
CodeInjection CLevelDialog_OnInitDialog_patch{
    0x004676C0,
    [](auto& regs) {
        HWND hdlg = WndToHandle(regs.esi);
        int level_version = get_level_rfl_version();
        std::string version = std::to_string(level_version);
        SetDlgItemTextA(hdlg, IDC_LEVEL_VERSION, version.c_str());

        auto& alpine_level_props = CDedLevel::Get()->GetAlpineLevelProperties();
        CheckDlgButton(hdlg, IDC_LEGACY_CYCLIC_TIMERS, alpine_level_props.legacy_cyclic_timers ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hdlg, IDC_LEGACY_MOVERS, alpine_level_props.legacy_movers ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hdlg, IDC_STARTS_WITH_HEADLAMP, alpine_level_props.starts_with_headlamp ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hdlg, IDC_OVERRIDE_MESH_AMBIENT_LIGHT_MODIFIER, alpine_level_props.override_static_mesh_ambient_light_modifier ? BST_CHECKED : BST_UNCHECKED);
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.3f", alpine_level_props.static_mesh_ambient_light_modifier);
        SetDlgItemTextA(hdlg, IDC_MESH_AMBIENT_LIGHT_MODIFIER, buffer);
        CheckDlgButton(hdlg, IDC_RF2_STYLE_GEOMOD, alpine_level_props.rf2_style_geomod ? BST_CHECKED : BST_UNCHECKED);
    },
};

// save AlpineLevelProperties settings when closing level properties dialog
CodeInjection CLevelDialog_OnOK_patch{
    0x00468470,
    [](auto& regs) {
        HWND hdlg = WndToHandle(regs.ecx);
        auto& alpine_level_props = CDedLevel::Get()->GetAlpineLevelProperties();
        alpine_level_props.legacy_cyclic_timers = IsDlgButtonChecked(hdlg, IDC_LEGACY_CYCLIC_TIMERS) == BST_CHECKED;
        alpine_level_props.legacy_movers = IsDlgButtonChecked(hdlg, IDC_LEGACY_MOVERS) == BST_CHECKED;
        alpine_level_props.starts_with_headlamp = IsDlgButtonChecked(hdlg, IDC_STARTS_WITH_HEADLAMP) == BST_CHECKED;
        alpine_level_props.override_static_mesh_ambient_light_modifier = IsDlgButtonChecked(hdlg, IDC_OVERRIDE_MESH_AMBIENT_LIGHT_MODIFIER) == BST_CHECKED;
        char buffer[64] = {};
        GetDlgItemTextA(hdlg, IDC_MESH_AMBIENT_LIGHT_MODIFIER, buffer, static_cast<int>(sizeof(buffer)));
        char* end = nullptr;
        float modifier = std::strtof(buffer, &end);
        if (end != buffer && std::isfinite(modifier)) {
            if (modifier < 0.0f) {
                modifier = 0.0f;
            }
            alpine_level_props.static_mesh_ambient_light_modifier = modifier;
        }
        alpine_level_props.rf2_style_geomod = IsDlgButtonChecked(hdlg, IDC_RF2_STYLE_GEOMOD) == BST_CHECKED;
    },
};

static bool is_link_allowed(const DedObject* src, const DedObject* dst)
{
    const auto t0 = src->type;
    const auto t1 = dst->type;

    return
        t0 == DedObjectType::DED_TRIGGER ||
        t0 == DedObjectType::DED_EVENT ||
        (t0 == DedObjectType::DED_NAV_POINT && t1 == DedObjectType::DED_EVENT) ||
        (t0 == DedObjectType::DED_CLUTTER && t1 == DedObjectType::DED_LIGHT);
}

void DedLevel_DoLinkImpl(CDedLevel* level, bool reverse_link_direction)
{
    auto& sel = level->selection;
    const int count = sel.get_size();

    if (count < 2) {
        g_main_frame->DedMessageBox(
            "You must select at least 2 objects to create a link.",
            "Error",
            0
        );
        return;
    }

    DedObject* primary = sel[0];
    if (!primary) {
        g_main_frame->DedMessageBox(
            "You must select at least 2 objects to create a link.",
            "Error",
            0
        );
        return;
    }

    int num_success = 0;
    std::vector<int> attempted_uids;

    for (int i = 1; i < count; ++i) {
        DedObject* src = reverse_link_direction ? sel[i] : primary;
        DedObject* dst = reverse_link_direction ? primary : sel[i];
        if (!src || !dst) {
            continue;
        }

        if (!is_link_allowed(src, dst)) {
            xlog::warn(
                "DoLink: disallowed type combination src_type={} dst_type={}",
                static_cast<int>(src->type),
                static_cast<int>(dst->type)
            );
            continue;
        }

        attempted_uids.push_back(reverse_link_direction ? src->uid : dst->uid);

        int old_size = src->links.get_size();
        int idx = src->links.add_if_not_exists_int(dst->uid);

        if (idx < 0) {
            xlog::warn("DoLink: Failed to add link src_uid={} dst_uid={}", src->uid, dst->uid);
        }
        else if (idx >= old_size) {
            ++num_success;
            xlog::debug("DoLink: Added new link src_uid={} -> dst_uid={}", src->uid, dst->uid);
        }
        else {
            xlog::debug("DoLink: Link already existed src_uid={} -> dst_uid={}", src->uid, dst->uid);
        }
    }

    if (num_success == 0) {
        std::string uid_list;
        for (size_t i = 0; i < attempted_uids.size(); ++i) {
            if (i > 0) {
                uid_list += ", ";
            }
            uid_list += std::to_string(attempted_uids[i]);
        }

        std::string msg;
        if (!attempted_uids.empty()) {
            if (reverse_link_direction) {
                msg = "All links to selected destination UID " +
                        std::to_string(primary->uid) +
                        " from valid source UID(s) " +
                        uid_list +
                        " already exist.";
            } else {
                msg = "All links from selected source UID " +
                        std::to_string(primary->uid) +
                        " to valid destination UID(s) " +
                        uid_list +
                        " already exist.";
            }
        } else {
            if (reverse_link_direction) {
                msg = "No valid link combinations were found for selected destination UID " +
                    std::to_string(primary->uid) +
                    ".";
            } else {
                msg = "No valid link combinations were found for selected source UID " +
                    std::to_string(primary->uid) +
                    ".";
            }
        }

        g_main_frame->DedMessageBox(msg.c_str(), "Error", 0);
        return;
    }
}

void __fastcall CDedLevel_DoLink_new(CDedLevel* this_);
FunHook<decltype(CDedLevel_DoLink_new)> CDedLevel_DoLink_hook{
    0x00415850,
    CDedLevel_DoLink_new,
};
void __fastcall CDedLevel_DoLink_new(CDedLevel* this_)
{
    DedLevel_DoLinkImpl(this_, false);
}

void DedLevel_DoBackLink()
{
    DedLevel_DoLinkImpl(CDedLevel::Get(), true);
}

void ApplyLevelPatches()
{
    // include space for default AlpineLevelProperties chunk in newly created rfls
    write_mem<std::uint32_t>(0x0041C906 + 1, 0x668 + sizeof(AlpineLevelProperties));

    // handle AlpineLevelProperties chunk
    CDedLevel_construct_patch.install();
    CDedLevel_LoadLevel_patch1.install();
    CDedLevel_LoadLevel_patch2.install();
    CDedLevel_SaveLevel_patch.install();
    CLevelDialog_OnInitDialog_patch.install();
    CLevelDialog_OnOK_patch.install();

    // Prevent geoable/breakable detail brushes from merging rooms with other brushes
    build_rooms_hook.install();
    adjacency_test_hook.install();
    isolate_rooms_injection.install();

    // Ensure the face_id assignment phase (Phase 1 of FUN_004399b0) always runs.
    // Phase 1 assigns unique sequential face_ids to brush geometry faces, which our
    // adjacency test hook uses to identify which brush each compiled face belongs to.
    // The stock code skips Phase 1 on the first build after editor launch (flag at
    // 0x005774a0 starts at 0). Setting it to 1 ensures face_ids are always assigned.
    write_mem<uint8_t>(0x005774a0, 1);

    // Avoid clamping lightmaps when loading rfl files
    AsmWriter{0x004A5D6A}.jmp(0x004A5D6E);

    // Default level fog color to flat black
    constexpr std::uint8_t default_fog = 0;
    write_mem<std::uint8_t>(0x0041CB07 + 1, default_fog);
    write_mem<std::uint8_t>(0x0041CB09 + 1, default_fog);
    write_mem<std::uint8_t>(0x0041CB0B + 1, default_fog);

    // Allow creating multiple links in a single operation
    CDedLevel_DoLink_hook.install();
}
