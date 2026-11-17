#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <optional>
#include <vector>
#include <xlog/xlog.h>
#include <common/alpine_waypoint_helper.h>
#include <common/rfl_chunk_reader.h>
#include "waypoint_helper.h"
#include "alpine_obj.h"
#include "alpine_spinner.h"
#include "level.h"
#include "resources.h"
#include "vtypes.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace awh = alpine_waypoint_helper;

namespace
{

constexpr float marker_half_size = 0.25f;
constexpr float landing_arrow_max_length = 2.0f;
constexpr float radius_spin_step = 0.5f;

std::vector<DedWaypointHelper*> g_waypoint_helper_clipboard;
std::vector<DedWaypointHelper*> g_selected_helpers;

// While the properties dialog is open, the selected helpers draw with the radius being typed.
struct RadiusPreview
{
    bool active = false;
    std::optional<float> radius;
};
RadiusPreview g_radius_preview;

const char* type_label(uint8_t type)
{
    switch (static_cast<awh::Type>(type)) {
    case awh::Type::air:    return "Air";
    case awh::Type::water:  return "Water";
    case awh::Type::ground: return "Ground";
    default:                return "Unknown";
    }
}

struct Rgb
{
    int r, g, b;
};

Rgb type_color(uint8_t type)
{
    switch (static_cast<awh::Type>(type)) {
    case awh::Type::air:    return {0x00, 0xdc, 0xff};
    case awh::Type::water:  return {0x5a, 0x5a, 0xff};
    case awh::Type::ground: return {0x3c, 0xdc, 0x3c};
    default:                return {0xa0, 0xa0, 0xa0};
    }
}

DedWaypointHelper* new_waypoint_helper()
{
    auto* helper = new DedWaypointHelper();
    memset(static_cast<DedObject*>(helper), 0, sizeof(DedObject));
    helper->vtbl = reinterpret_cast<void*>(ded_object_vtbl_addr);
    helper->type = DedObjectType::DED_WAYPOINT_HELPER;
    helper->orient = identity_orient;
    // Not saved (the record has no name), but group lists and the Select Objects dialog show objects by it.
    helper->script_name.assign_0("Waypoint Helper");
    return helper;
}

void add_to_level(CDedLevel& level, DedWaypointHelper* helper)
{
    level.GetAlpineLevelProperties().waypoint_helper_objects.push_back(helper);
    level.master_objects.add(static_cast<DedObject*>(helper));
}

// ─── Properties Dialog ──────────────────────────────────────────────────────

void update_type_controls(HWND hdlg)
{
    const LRESULT type = alpine_dlg_combo_data(hdlg, IDC_WAYPOINT_HELPER_TYPE, -1);
    // A mixed selection may hold either kind, so both stay editable.
    const BOOL ground = type == -1 || type == static_cast<LRESULT>(awh::Type::ground);
    const BOOL landing = type == -1 || type != static_cast<LRESULT>(awh::Type::ground);
    EnableWindow(GetDlgItem(hdlg, IDC_WAYPOINT_HELPER_SIZE), ground);
    EnableWindow(GetDlgItem(hdlg, IDC_WAYPOINT_HELPER_SIZE_LABEL), ground);
    EnableWindow(GetDlgItem(hdlg, IDC_WAYPOINT_HELPER_LANDING), landing);
}

// Text that is not a number leaves the radius as it is, as a blank field does.
std::optional<float> radius_field_value(HWND hdlg)
{
    char radius_text[32] = {};
    GetDlgItemTextA(hdlg, IDC_WAYPOINT_HELPER_RADIUS, radius_text, sizeof(radius_text));
    char* radius_end = nullptr;
    std::strtof(radius_text, &radius_end);
    if (radius_end == radius_text) {
        return std::nullopt;
    }
    return awh::sanitize_radius(
        alpine_dlg_get_float_field_exact(hdlg, IDC_WAYPOINT_HELPER_RADIUS, g_selected_helpers[0]->radius));
}

INT_PTR CALLBACK WaypointHelperDialogProc(HWND hdlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        if (g_selected_helpers.empty()) {
            EndDialog(hdlg, IDCANCEL);
            return TRUE;
        }
        const DedWaypointHelper* first = g_selected_helpers[0];
        // A field the selection disagrees on is shown blank (indeterminate for the checkbox), and OK leaves it as it
        // is on every helper unless the user sets it.
        const auto uniform = [first](auto field) {
            return std::all_of(g_selected_helpers.begin(), g_selected_helpers.end(),
                [&](const DedWaypointHelper* h) { return h->*field == first->*field; });
        };

        alpine_dlg_combo_add(hdlg, IDC_WAYPOINT_HELPER_TYPE, "Air", static_cast<LPARAM>(awh::Type::air));
        alpine_dlg_combo_add(hdlg, IDC_WAYPOINT_HELPER_TYPE, "Water", static_cast<LPARAM>(awh::Type::water));
        alpine_dlg_combo_add(hdlg, IDC_WAYPOINT_HELPER_TYPE, "Ground", static_cast<LPARAM>(awh::Type::ground));
        if (uniform(&DedWaypointHelper::helper_type)) {
            alpine_dlg_combo_select(hdlg, IDC_WAYPOINT_HELPER_TYPE, first->helper_type);
        }

        alpine_dlg_combo_add(hdlg, IDC_WAYPOINT_HELPER_SIZE, "Jeep", static_cast<LPARAM>(awh::SizeClass::jeep));
        alpine_dlg_combo_add(hdlg, IDC_WAYPOINT_HELPER_SIZE, "APC", static_cast<LPARAM>(awh::SizeClass::apc));
        alpine_dlg_combo_add(hdlg, IDC_WAYPOINT_HELPER_SIZE, "Driller (any vehicle)",
                             static_cast<LPARAM>(awh::SizeClass::driller));
        if (uniform(&DedWaypointHelper::size_class)) {
            alpine_dlg_combo_select(hdlg, IDC_WAYPOINT_HELPER_SIZE, first->size_class);
        }
        update_type_controls(hdlg);

        if (uniform(&DedWaypointHelper::radius)) {
            alpine_dlg_set_float_field_exact(hdlg, IDC_WAYPOINT_HELPER_RADIUS, first->radius);
        }
        alpine_spinner_init(hdlg, IDC_WAYPOINT_HELPER_RADIUS, IDC_WAYPOINT_HELPER_RADIUS_SPIN, radius_spin_step,
                            awh::min_radius, awh::max_radius, 2);

        const bool landing = (first->flags & awh::flag_landing) != 0;
        const bool landing_uniform = std::all_of(g_selected_helpers.begin(), g_selected_helpers.end(),
            [&](const DedWaypointHelper* h) { return ((h->flags & awh::flag_landing) != 0) == landing; });
        CheckDlgButton(hdlg, IDC_WAYPOINT_HELPER_LANDING,
                       !landing_uniform ? BST_INDETERMINATE : landing ? BST_CHECKED : BST_UNCHECKED);
        g_radius_preview.radius = radius_field_value(hdlg);
        g_radius_preview.active = true;
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_WAYPOINT_HELPER_LANDING:
            // Indeterminate is only a starting state: clicking cycles between the two real values.
            if (IsDlgButtonChecked(hdlg, IDC_WAYPOINT_HELPER_LANDING) == BST_INDETERMINATE) {
                CheckDlgButton(hdlg, IDC_WAYPOINT_HELPER_LANDING, BST_UNCHECKED);
            }
            return TRUE;
        case IDC_WAYPOINT_HELPER_TYPE:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                update_type_controls(hdlg);
                return TRUE;
            }
            break;
        case IDC_WAYPOINT_HELPER_RADIUS:
            if (HIWORD(wp) == EN_CHANGE && g_radius_preview.active) {
                g_radius_preview.radius = radius_field_value(hdlg);
                redraw_all_viewports();
                return TRUE;
            }
            break;
        case IDOK: {
            const LRESULT type = alpine_dlg_combo_data(hdlg, IDC_WAYPOINT_HELPER_TYPE, -1);
            const LRESULT size = alpine_dlg_combo_data(hdlg, IDC_WAYPOINT_HELPER_SIZE, -1);
            const std::optional<float> radius = radius_field_value(hdlg);
            const UINT landing = IsDlgButtonChecked(hdlg, IDC_WAYPOINT_HELPER_LANDING);

            for (auto* h : g_selected_helpers) {
                if (type >= 0) {
                    h->helper_type = static_cast<uint8_t>(type);
                }
                if (size >= 0) {
                    h->size_class = static_cast<uint8_t>(size);
                }
                if (radius) {
                    h->radius = *radius;
                }
                if (landing != BST_INDETERMINATE) {
                    h->flags = static_cast<uint8_t>(landing == BST_CHECKED ? (h->flags | awh::flag_landing)
                                                                           : (h->flags & ~awh::flag_landing));
                }
            }
            EndDialog(hdlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(hdlg, IDCANCEL);
            return TRUE;
        }
        break;
    case WM_NOTIFY:
        if (alpine_spinner_handle_notify(hdlg, lp)) return TRUE;
        break;
    }
    return FALSE;
}

} // namespace

void ShowWaypointHelperPropertiesDialog(CDedLevel* level)
{
    auto& sel = level->selection;
    g_selected_helpers.clear();
    for (int i = 0; i < sel.get_size(); i++) {
        DedObject* obj = sel[i];
        if (obj && obj->type == DedObjectType::DED_WAYPOINT_HELPER) {
            g_selected_helpers.push_back(static_cast<DedWaypointHelper*>(obj));
        }
    }
    if (!g_selected_helpers.empty()) {
        DialogBoxParam(reinterpret_cast<HINSTANCE>(&__ImageBase),
            MAKEINTRESOURCE(IDD_ALPINE_WAYPOINT_HELPER_PROPERTIES), GetActiveWindow(), WaypointHelperDialogProc, 0);
        g_radius_preview = {};
    }
    g_selected_helpers.clear();
}

// ─── Cleanup ─────────────────────────────────────────────────────────────────

void DestroyDedWaypointHelper(DedWaypointHelper* helper)
{
    if (!helper) return;
    // Grown by the stock VArray code (0x00416360), which allocates with the editor's operator new.
    if (helper->links.data_ptr) {
        editor_free(helper->links.data_ptr);
        helper->links = {};
    }
    helper->class_mesh_filename.free();
    helper->script_name.free();
    helper->class_name.free();
    delete helper;
}

// ─── Serialization ──────────────────────────────────────────────────────────

// Waypoint Helper chunk (id 0x0AFBAE0D, RFL 306+): u32 count, then alpine_waypoint_helper::write_record each.
// alpine_waypoint_helper_load_chunk in game_patch/object/alpine_waypoint_helper.cpp must stay in step.
void waypoint_helper_serialize_chunk(CDedLevel& level, rf::File& file)
{
    auto& helpers = level.GetAlpineLevelProperties().waypoint_helper_objects;
    if (helpers.empty()) return;

    auto start_pos = level.BeginRflSection(file, alpine_waypoint_helper_chunk_id);

    const auto count = static_cast<uint32_t>(std::min<std::size_t>(helpers.size(), awh::max_helpers));
    if (helpers.size() > count) {
        editor_report(EditorReportLevel::warn, "WaypointHelper",
                      std::format("Only the first {} of {} waypoint helpers were saved.", count, helpers.size()),
                      true);
    }
    file.write<uint32_t>(count);

    int links_dropped = 0;
    for (uint32_t i = 0; i < count; i++) {
        const DedWaypointHelper& helper = *helpers[i];
        awh::Record rec;
        rec.uid = helper.uid;
        rec.pos_x = helper.pos.x;
        rec.pos_y = helper.pos.y;
        rec.pos_z = helper.pos.z;
        rec.type = helper.helper_type;
        rec.flags = helper.flags;
        rec.size_class = helper.size_class;
        rec.radius = awh::sanitize_radius(helper.radius);
        const int link_count = std::max(helper.links.size, 0);
        rec.links.assign(helper.links.data_ptr, helper.links.data_ptr + link_count);
        if (rec.links.size() > awh::max_links) {
            links_dropped += static_cast<int>(rec.links.size() - awh::max_links);
        }
        awh::write_record(file, rec);
    }
    if (links_dropped > 0) {
        editor_report(EditorReportLevel::warn, "WaypointHelper",
                      std::format("{} waypoint helper link(s) past the {} per helper limit were not saved.",
                                  links_dropped, awh::max_links),
                      true);
    }

    level.EndRflSection(file, start_pos);
}

void waypoint_helper_deserialize_chunk(CDedLevel& level, rf::File& file, std::size_t chunk_len)
{
    auto& helpers = level.GetAlpineLevelProperties().waypoint_helper_objects;
    std::size_t remaining = chunk_len;

    rf::File::ChunkGuard chunk_guard{file, remaining};
    RflChunkReader<rf::File> reader{file, remaining};

    uint32_t count = 0;
    if (!reader.read(count)) return;
    // The cap is level-wide, as the game applies it across every chunk.
    const auto present = static_cast<uint32_t>(std::min<std::size_t>(helpers.size(), awh::max_helpers));
    if (count > awh::max_helpers - present) {
        editor_report(EditorReportLevel::warn, "WaypointHelper",
                      std::format("Only {} of {} waypoint helpers were loaded: a level holds at most {}.",
                                  awh::max_helpers - present, count, awh::max_helpers),
                      true);
        count = awh::max_helpers - present;
    }

    uint32_t loaded = 0;
    for (uint32_t i = 0; i < count; i++) {
        awh::Record rec;
        if (!awh::read_record(reader, rec)) {
            break;
        }
        auto* helper = new_waypoint_helper();
        helper->uid = rec.uid;
        helper->pos = awh::pos_is_finite(rec) ? Vector3{rec.pos_x, rec.pos_y, rec.pos_z} : Vector3{};
        // Unknown type, size and flag values are kept as they are, so a newer level round-trips.
        helper->helper_type = rec.type;
        helper->flags = rec.flags;
        helper->size_class = rec.size_class;
        helper->radius = awh::sanitize_radius(rec.radius);
        for (const int32_t link : rec.links) {
            helper->links.push_back(link);
        }
        add_to_level(level, helper);
        loaded++;
    }

    xlog::info("[WaypointHelper] Loaded {} waypoint helper object(s)", loaded);
}

// ─── Object Lifecycle ───────────────────────────────────────────────────────

void PlaceNewWaypointHelperObject()
{
    auto* level = CDedLevel::Get();
    if (!level) return;

    auto* helper = new_waypoint_helper();
    if (auto* viewport = get_active_viewport(); viewport && viewport->view_data) {
        helper->pos = viewport->view_data->camera_pos;
    }
    helper->uid = generate_uid();
    add_to_level(*level, helper);

    level->clear_selection();
    level->add_to_selection(static_cast<DedObject*>(helper));
    level->update_console_display();
}

// Links are not copied: a paste rebuilds the ones between copied objects (fix_paste_links).
DedWaypointHelper* CloneWaypointHelperObject(DedWaypointHelper* source, bool add_to_level_now)
{
    if (!source) return nullptr;

    auto* helper = new_waypoint_helper();
    helper->pos = source->pos;
    helper->orient = source->orient;
    helper->helper_type = source->helper_type;
    helper->flags = source->flags;
    helper->size_class = source->size_class;
    helper->radius = source->radius;
    helper->uid = generate_uid();

    if (add_to_level_now) {
        if (auto* level = CDedLevel::Get()) {
            add_to_level(*level, helper);
        }
    }
    return helper;
}

bool waypoint_helper_link_allowed(const DedObject* src, const DedObject* dst)
{
    return src->type == DedObjectType::DED_WAYPOINT_HELPER && dst->type == DedObjectType::DED_WAYPOINT_HELPER
        && src != dst;
}

// ─── Rendering ──────────────────────────────────────────────────────────────

void waypoint_helper_render(CDedLevel* level)
{
    for (auto* helper : level->GetAlpineLevelProperties().waypoint_helper_objects) {
        if (helper->hidden_in_editor) continue;

        const bool selected = is_object_selected(level, helper);
        const Rgb c = selected ? Rgb{0xff, 0x00, 0x00} : type_color(helper->helper_type);
        const Vector3& p = helper->pos;
        float radius = helper->radius;
        if (g_radius_preview.active && g_radius_preview.radius
            && std::find(g_selected_helpers.begin(), g_selected_helpers.end(), helper) != g_selected_helpers.end()) {
            radius = *g_radius_preview.radius;
        }

        draw_wireframe_sphere(p.x, p.y, p.z, radius, c.r, c.g, c.b);
        draw_3d_line(p.x - marker_half_size, p.y, p.z, p.x + marker_half_size, p.y, p.z, c.r, c.g, c.b);
        draw_3d_line(p.x, p.y - marker_half_size, p.z, p.x, p.y + marker_half_size, p.z, c.r, c.g, c.b);
        draw_3d_line(p.x, p.y, p.z - marker_half_size, p.x, p.y, p.z + marker_half_size, c.r, c.g, c.b);

        if (helper->flags & awh::flag_landing) {
            const float len = std::min(radius, landing_arrow_max_length);
            draw_3d_arrow(p.x, p.y, p.z, p.x, p.y - len, p.z, 255, 255, 0);
        }
    }
}

void waypoint_helper_pick(CDedLevel* level, int param1, int param2)
{
    for (auto* helper : level->GetAlpineLevelProperties().waypoint_helper_objects) {
        if (helper->hidden_in_editor) continue;
        if (level->hit_test_point(param1, param2, &helper->pos)) {
            level->select_object(static_cast<DedObject*>(helper));
        }
    }
}

DedWaypointHelper* waypoint_helper_click_pick(CDedLevel* level, float click_x, float click_y)
{
    return alpine_click_pick_point(level->GetAlpineLevelProperties().waypoint_helper_objects, click_x, click_y,
                                   alpine_click_pick_radius_sq);
}

void waypoint_helper_tree_populate(EditorTreeCtrl* tree, int master_groups, CDedLevel* level)
{
    auto& helpers = level->GetAlpineLevelProperties().waypoint_helper_objects;

    char buf[64];
    std::snprintf(buf, sizeof(buf), "Waypoint Helpers (%d)", static_cast<int>(helpers.size()));
    int parent = tree->insert_item(buf, master_groups, 0xffff0002);

    for (auto* helper : helpers) {
        std::snprintf(buf, sizeof(buf), "%s helper (%d)", type_label(helper->helper_type), helper->uid);
        int child = tree->insert_item(buf, parent, 0xffff0002);
        tree->set_item_data(child, helper->uid);
    }
}

void waypoint_helper_tree_add_object_type(EditorTreeCtrl* tree)
{
    tree->insert_item("Waypoint Helper", 0xffff0000, 0xffff0002);
}

// ─── Clipboard / Delete ─────────────────────────────────────────────────────

bool waypoint_helper_copy_object(DedObject* source)
{
    if (!source || source->type != DedObjectType::DED_WAYPOINT_HELPER) return false;
    auto* staged = CloneWaypointHelperObject(static_cast<DedWaypointHelper*>(source), false);
    if (!staged) return false;
    g_waypoint_helper_clipboard.push_back(staged);
    return true;
}

void waypoint_helper_paste_objects(CDedLevel* level)
{
    for (auto* staged : g_waypoint_helper_clipboard) {
        if (auto* clone = CloneWaypointHelperObject(staged, true)) {
            level->add_to_selection(static_cast<DedObject*>(clone));
        }
    }
}

void waypoint_helper_swap_clipboard(std::vector<DedWaypointHelper*>& other)
{
    g_waypoint_helper_clipboard.swap(other);
}

void waypoint_helper_clear_clipboard()
{
    for (auto* helper : g_waypoint_helper_clipboard) {
        DestroyDedWaypointHelper(helper);
    }
    g_waypoint_helper_clipboard.clear();
}

void waypoint_helper_handle_delete_or_cut(DedObject* obj)
{
    if (!obj || obj->type != DedObjectType::DED_WAYPOINT_HELPER) return;
    auto* level = CDedLevel::Get();
    if (!level) return;

    auto& helpers = level->GetAlpineLevelProperties().waypoint_helper_objects;
    auto it = std::find(helpers.begin(), helpers.end(), static_cast<DedWaypointHelper*>(obj));
    if (it != helpers.end()) {
        helpers.erase(it);
    }
}

void waypoint_helper_ensure_uid(int& uid)
{
    auto* level = CDedLevel::Get();
    if (!level) return;
    alpine_ensure_uid(level->GetAlpineLevelProperties().waypoint_helper_objects, uid);
}
