#include <patch_common/FunHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/AsmWriter.h>
#include <patch_common/MemUtils.h>
#include <xlog/xlog.h>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
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
static void compute_geoable_room_uids(CDedLevel& level, AlpineLevelProperties& props)
{
    props.geoable_room_uids.clear();
    props.geoable_room_uids.resize(props.geoable_brush_uids.size(), 0);

    if (!level.solid) {
        xlog::warn("[Geoable Save] compute_room_uids: no compiled solid");
        return;
    }

    auto& all_rooms = level.solid->all_rooms;
    xlog::info("[Geoable Save] compute_room_uids: {} geoable brushes, {} compiled rooms",
                props.geoable_brush_uids.size(), all_rooms.get_size());

    // The editor's build/compile process creates final rooms as copies that lack UIDs (uid=-1).
    // Room UIDs are normally assigned in the GRoom constructor via a global decrementing counter
    // at 0x0057c954 (starts at 0x7FFFFFFF), but the final rooms in all_rooms are clones that
    // don't go through the constructor. Assign UIDs here so solid_write persists them to the .rfl
    // and our geoable mapping can reference them.
    int& uid_counter = *reinterpret_cast<int*>(0x0057c954);
    int uids_assigned = 0;
    for (int j = 0; j < all_rooms.get_size(); j++) {
        GRoom* room = all_rooms.data_ptr[j];
        if (room && room->uid == -1) {
            room->uid = uid_counter;
            uid_counter--;
            uids_assigned++;
        }
    }
    if (uids_assigned > 0) {
        xlog::info("[Geoable Save] assigned UIDs to {} rooms that had uid=-1", uids_assigned);
    }

    // Log all detail rooms for cross-reference
    int detail_count = 0;
    for (int j = 0; j < all_rooms.get_size(); j++) {
        GRoom* room = all_rooms.data_ptr[j];
        if (room && room->is_detail) {
            detail_count++;
            xlog::info("[Geoable Save]   detail room: uid={} index={} bbox=[({:.2f},{:.2f},{:.2f})-({:.2f},{:.2f},{:.2f})]",
                room->uid, j,
                room->bbox_min.x, room->bbox_min.y, room->bbox_min.z,
                room->bbox_max.x, room->bbox_max.y, room->bbox_max.z);
        }
    }
    xlog::info("[Geoable Save] total detail rooms: {}", detail_count);

    for (std::size_t i = 0; i < props.geoable_brush_uids.size(); i++) {
        int32_t brush_uid = props.geoable_brush_uids[i];

        // Find brush in linked list and get its position
        BrushNode* node = level.brush_list;
        Vector3 brush_pos;
        bool found_brush = false;
        if (node) {
            do {
                if (node->uid == brush_uid) {
                    brush_pos = node->pos;
                    found_brush = true;
                    break;
                }
                node = node->next;
            } while (node && node != level.brush_list);
        }

        if (!found_brush) {
            xlog::warn("[Geoable Save] brush uid={} NOT FOUND in brush list — will have room_uid=0", brush_uid);
            continue;
        }

        // Find detail room whose bbox center is closest to the brush position.
        // Multiple rooms may contain the brush position (e.g., a pillar room and a
        // rail room it touches). We pick the one whose bbox center is nearest the
        // brush center — this correctly maps pillar brushes to pillar rooms rather
        // than to adjacent rail/beam rooms that happen to overlap.
        constexpr float tolerance = 2.0f;
        bool matched = false;
        GRoom* best_room = nullptr;
        float best_dist_sq = FLT_MAX;
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
            props.geoable_room_uids[i] = best_room->uid;
            xlog::info("[Geoable Save] brush uid={} pos=({:.2f},{:.2f},{:.2f}) -> room uid={} (dist={:.2f}) "
                "bbox=[({:.2f},{:.2f},{:.2f})-({:.2f},{:.2f},{:.2f})]",
                brush_uid, brush_pos.x, brush_pos.y, brush_pos.z, best_room->uid, std::sqrt(best_dist_sq),
                best_room->bbox_min.x, best_room->bbox_min.y, best_room->bbox_min.z,
                best_room->bbox_max.x, best_room->bbox_max.y, best_room->bbox_max.z);
            matched = true;
        }

        if (!matched) {
            // Log near-misses: detail rooms that were closest but didn't match
            xlog::warn("[Geoable Save] brush uid={} at ({:.2f},{:.2f},{:.2f}) NO ROOM MATCH! "
                "Listing nearby detail rooms:",
                brush_uid, brush_pos.x, brush_pos.y, brush_pos.z);
            for (int j = 0; j < all_rooms.get_size(); j++) {
                GRoom* room = all_rooms.data_ptr[j];
                if (!room || !room->is_detail) continue;
                float dx = 0, dy = 0, dz = 0;
                if (brush_pos.x < room->bbox_min.x - tolerance)
                    dx = room->bbox_min.x - tolerance - brush_pos.x;
                else if (brush_pos.x > room->bbox_max.x + tolerance)
                    dx = brush_pos.x - room->bbox_max.x - tolerance;
                if (brush_pos.y < room->bbox_min.y - tolerance)
                    dy = room->bbox_min.y - tolerance - brush_pos.y;
                else if (brush_pos.y > room->bbox_max.y + tolerance)
                    dy = brush_pos.y - room->bbox_max.y - tolerance;
                if (brush_pos.z < room->bbox_min.z - tolerance)
                    dz = room->bbox_min.z - tolerance - brush_pos.z;
                else if (brush_pos.z > room->bbox_max.z + tolerance)
                    dz = brush_pos.z - room->bbox_max.z - tolerance;
                float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (dist < 20.0f) { // only log rooms within 20 units
                    xlog::warn("[Geoable Save]   near-miss room uid={} dist={:.2f} "
                        "bbox=[({:.2f},{:.2f},{:.2f})-({:.2f},{:.2f},{:.2f})] outside_by=({:.2f},{:.2f},{:.2f})",
                        room->uid, dist,
                        room->bbox_min.x, room->bbox_min.y, room->bbox_min.z,
                        room->bbox_max.x, room->bbox_max.y, room->bbox_max.z,
                        dx, dy, dz);
                }
            }
        }
    }

    // Summary
    int matched_count = 0;
    for (size_t i = 0; i < props.geoable_room_uids.size(); i++) {
        if (props.geoable_room_uids[i] != 0) matched_count++;
    }
    xlog::info("[Geoable Save] SUMMARY: {} geoable brushes, {} matched to rooms, {} UNMATCHED",
        props.geoable_brush_uids.size(), matched_count,
        props.geoable_brush_uids.size() - matched_count);
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
