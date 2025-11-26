#include <patch_common/FunHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/AsmWriter.h>
#include <patch_common/MemUtils.h>
#include <xlog/xlog.h>
#include "level.h"
#include "vtypes.h"
#include "mfc_types.h"
#include "resources.h"

// add AlpineLevelProperties chunk after stock game chunks when creating a new level
CodeInjection CDedLevel_construct_patch{
    0x004181B8,
    [](auto& regs) {
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

// save AlpineLevelProperties when saving rfl file
CodeInjection CDedLevel_SaveLevel_patch{
    0x00430CBD,
    [](auto& regs) {
        auto& level = *static_cast<CDedLevel*>(regs.edi);
        auto& file = *static_cast<rf::File*>(regs.esi);
        auto start_pos = level.BeginRflSection(file, alpine_props_chunk_id);
        auto& alpine_level_props = level.GetAlpineLevelProperties();
        alpine_level_props.Serialize(file);
        level.EndRflSection(file, start_pos);
    },
};

// load AlpineLevelProperties settings when opening level properties dialog
CodeInjection CLevelDialog_OnInitDialog_patch{
    0x004676C0,
    [](auto& regs) {
        HWND hdlg = WndToHandle(regs.esi);
        auto& alpine_level_props = CDedLevel::Get()->GetAlpineLevelProperties();
        CheckDlgButton(hdlg, IDC_LEGACY_CYCLIC_TIMERS, alpine_level_props.legacy_cyclic_timers ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hdlg, IDC_LEGACY_MOVERS, alpine_level_props.legacy_movers ? BST_CHECKED : BST_UNCHECKED);
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

void __fastcall CDedLevel_DoLink_new(CDedLevel* this_);
FunHook<decltype(CDedLevel_DoLink_new)> CDedLevel_DoLink_hook{
    0x00415850,
    CDedLevel_DoLink_new,
};
void __fastcall CDedLevel_DoLink_new(CDedLevel* this_)
{
    auto& sel = this_->selection;
    const int count = sel.get_size();

    if (count < 2) {
        g_main_frame->DedMessageBox(
            "You must select at least 2 objects to create a link.",
            "Error",
            0
        );
        return;
    }

    DedObject* src = sel[0];
    if (!src) {
        g_main_frame->DedMessageBox(
            "You must select at least 2 objects to create a link.",
            "Error",
            0
        );
        return;
    }

    int num_success = 0;
    std::vector<int> attempted_dst_uids; // valid destination UIDs

    for (int i = 1; i < count; ++i) {
        DedObject* dst = sel[i];
        if (!dst) {
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

        attempted_dst_uids.push_back(dst->uid);

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
        std::string dst_list;
        for (size_t i = 0; i < attempted_dst_uids.size(); ++i) {
            if (i > 0) {
                dst_list += ", ";
            }
            dst_list += std::to_string(attempted_dst_uids[i]);
        }

        std::string msg;
        if (!attempted_dst_uids.empty()) {
            msg = "All links from selected source UID " +
                  std::to_string(src->uid) +
                  " to valid destination UID(s) " +
                  dst_list +
                  " already exist.";
        } else {
            msg = "No valid link combinations were found for selected source UID " +
                std::to_string(src->uid) +
                ".";
        }

        g_main_frame->DedMessageBox(msg.c_str(), "Error", 0);
        return;
    }
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

    // Default level ambient light and fog color to flat black
    constexpr std::uint8_t default_ambient_light = 0;
    constexpr std::uint8_t default_fog = 0;
    write_mem<std::uint8_t>(0x0041CABD + 1, default_ambient_light);
    write_mem<std::uint8_t>(0x0041CABF + 1, default_ambient_light);
    write_mem<std::uint8_t>(0x0041CAC1 + 1, default_ambient_light);
    write_mem<std::uint8_t>(0x0041CAD3 + 1, default_ambient_light);
    write_mem<std::uint8_t>(0x0041CAD5 + 1, default_ambient_light);
    write_mem<std::uint8_t>(0x0041CAD7 + 1, default_ambient_light);
    write_mem<std::uint8_t>(0x0041CB07 + 1, default_fog);
    write_mem<std::uint8_t>(0x0041CB09 + 1, default_fog);
    write_mem<std::uint8_t>(0x0041CB0B + 1, default_fog);

    // Allow creating multiple links in a single operation
    CDedLevel_DoLink_hook.install();
}
