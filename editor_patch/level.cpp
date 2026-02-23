#include <patch_common/FunHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/AsmWriter.h>
#include <patch_common/MemUtils.h>
#include <xlog/xlog.h>
#include <cmath>
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
// The compiled solid at CDedLevel + 0x4c contains GRoom objects with bboxes.
// The brush linked list at CDedLevel + 0x118 contains brush nodes with positions at +0x08.
// For each geoable brush, find the detail room whose bbox contains the brush position.
static void compute_geoable_room_uids(CDedLevel& level, AlpineLevelProperties& props)
{
    props.geoable_room_uids.clear();
    props.geoable_room_uids.resize(props.geoable_brush_uids.size(), 0);

    auto* level_ptr = reinterpret_cast<std::byte*>(&level);

    // Access compiled solid at CDedLevel + 0x4c
    auto* solid_ptr = *reinterpret_cast<std::byte**>(level_ptr + 0x4c);
    if (!solid_ptr) {
        xlog::warn("[Geoable] compute_room_uids: no compiled solid");
        return;
    }

    // GSolid::all_rooms VArray at solid + 0x90: {int num, int capacity, GRoom** elements}
    int rooms_count = *reinterpret_cast<int*>(solid_ptr + 0x90);
    auto** rooms_data = *reinterpret_cast<std::byte***>(solid_ptr + 0x90 + 8);

    xlog::debug("[Geoable] compute_room_uids: geoable_brush_uids={} rooms_count={} rooms_data={}", props.geoable_brush_uids.size(), rooms_count, (void*)rooms_data);

    // Brush linked list head at CDedLevel + 0x118
    auto* brush_head = *reinterpret_cast<std::byte**>(level_ptr + 0x118);

    for (std::size_t i = 0; i < props.geoable_brush_uids.size(); i++) {
        int32_t brush_uid = props.geoable_brush_uids[i];

        // Find brush in linked list and get its position (+0x08 = Vec3)
        auto* node = brush_head;
        float bx = 0, by = 0, bz = 0;
        bool found_brush = false;
        if (node) {
            do {
                int uid = *reinterpret_cast<int*>(node + 0x04);
                if (uid == brush_uid) {
                    bx = *reinterpret_cast<float*>(node + 0x08);
                    by = *reinterpret_cast<float*>(node + 0x0C);
                    bz = *reinterpret_cast<float*>(node + 0x10);
                    found_brush = true;
                    break;
                }
                node = *reinterpret_cast<std::byte**>(node + 0x4c);
            } while (node && node != brush_head);
        }

        if (!found_brush) {
            xlog::warn("[Geoable] compute_room_uids: brush uid={} not found in list", brush_uid);
            continue;
        }

        // Find detail room whose bbox contains the brush position
        constexpr float tolerance = 2.0f;
        for (int j = 0; j < rooms_count; j++) {
            auto* room = rooms_data[j];
            if (!room) continue;
            // GRoom: +0x00=is_detail(bool), +0x08=bbox_min(Vec3), +0x14=bbox_max(Vec3), +0x24=uid(int)
            bool is_detail = *reinterpret_cast<bool*>(room + 0x00);
            if (!is_detail) continue;
            float rx0 = *reinterpret_cast<float*>(room + 0x08);
            float ry0 = *reinterpret_cast<float*>(room + 0x0C);
            float rz0 = *reinterpret_cast<float*>(room + 0x10);
            float rx1 = *reinterpret_cast<float*>(room + 0x14);
            float ry1 = *reinterpret_cast<float*>(room + 0x18);
            float rz1 = *reinterpret_cast<float*>(room + 0x1C);
            if (bx >= rx0 - tolerance && bx <= rx1 + tolerance &&
                by >= ry0 - tolerance && by <= ry1 + tolerance &&
                bz >= rz0 - tolerance && bz <= rz1 + tolerance) {
                int room_uid = *reinterpret_cast<int*>(room + 0x24);
                props.geoable_room_uids[i] = room_uid;
                xlog::debug("[Geoable] matched brush uid={} pos=({:.1f},{:.1f},{:.1f}) -> room uid={}", brush_uid, bx, by, bz, room_uid);
                break;
            }
        }

        if (props.geoable_room_uids[i] == 0) {
            xlog::warn("[Geoable] compute_room_uids: no room match for brush uid={} at ({:.1f},{:.1f},{:.1f})", brush_uid, bx, by, bz);
        }
    }
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
