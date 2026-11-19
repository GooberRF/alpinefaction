#include <windows.h>
#include <commdlg.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <vector>
#include <xlog/xlog.h>
#include <common/alpine_ambience_region.h>
#include <common/rfl_chunk_reader.h>
#include <common/utils/string-utils.h>
#include "ambience_region.h"
#include "alpine_obj.h"
#include "alpine_spinner.h"
#include "level.h"
#include "resources.h"
#include "sound_browser.h"
#include "vtypes.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace aar = alpine_ambience_region;

namespace
{

// The size spinners step and clamp as RED's own size fields do.
constexpr float dim_step = 0.02f;
constexpr float dim_min = 0.0f;
constexpr float dim_max = 999.0f;
constexpr float icon_size = 1.0f;

std::vector<DedAmbienceRegion*> g_ambience_region_clipboard;
std::vector<DedAmbienceRegion*> g_selected_regions;
int g_icon_handle = -1;

// While the properties dialog is open, the selected regions draw with the shape and size being edited.
struct DimsPreview
{
    bool active = false;
    std::optional<uint8_t> shape;
    std::optional<float> width, height, depth, radius;
};
DimsPreview g_dims_preview;

bool is_sphere(uint8_t shape)
{
    return shape == static_cast<uint8_t>(aar::Shape::sphere);
}

DedAmbienceRegion* new_ambience_region()
{
    auto* region = new DedAmbienceRegion();
    memset(static_cast<DedObject*>(region), 0, sizeof(DedObject));
    region->vtbl = reinterpret_cast<void*>(ded_object_vtbl_addr);
    region->type = DedObjectType::DED_AMBIENCE_REGION;
    region->orient = identity_orient;
    return region;
}

void add_to_level(CDedLevel& level, DedAmbienceRegion* region)
{
    level.GetAlpineLevelProperties().ambience_region_objects.push_back(region);
    level.master_objects.add(static_cast<DedObject*>(region));
}

// ─── Properties Dialog ──────────────────────────────────────────────────────

// Text that is not a finite number leaves the stored value as it is, as a blank field does.
std::optional<float> field_value(HWND hdlg, int idc, float shown)
{
    char text[32] = {};
    GetDlgItemTextA(hdlg, idc, text, sizeof(text));
    char* end = nullptr;
    std::strtof(text, &end);
    if (end == text) {
        return std::nullopt;
    }
    const float value = alpine_dlg_get_float_field_exact(hdlg, idc, shown);
    if (!std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

// Empty when the combo has no selection (a newer level's shape), which each region then keeps.
std::optional<uint8_t> selected_shape(HWND hdlg)
{
    const LRESULT shape = alpine_dlg_combo_data(hdlg, IDC_AMBIENCE_SHAPE, -1);
    return shape < 0 ? std::nullopt : std::optional<uint8_t>{static_cast<uint8_t>(shape)};
}

// The preview shows what OK would store.
void capture_dims_preview(HWND hdlg)
{
    const DedAmbienceRegion* first = g_selected_regions[0];
    auto dim = [hdlg](int idc, float shown) -> std::optional<float> {
        const std::optional<float> value = field_value(hdlg, idc, shown);
        return value ? std::optional<float>{aar::sanitize_dim(*value, shown)} : std::nullopt;
    };
    g_dims_preview.shape = selected_shape(hdlg);
    g_dims_preview.width = dim(IDC_AMBIENCE_WIDTH, first->width);
    g_dims_preview.height = dim(IDC_AMBIENCE_HEIGHT, first->height);
    g_dims_preview.depth = dim(IDC_AMBIENCE_DEPTH, first->depth);
    g_dims_preview.radius = dim(IDC_AMBIENCE_RADIUS, first->radius);
}

void update_shape_fields(HWND hdlg)
{
    const bool sphere = is_sphere(selected_shape(hdlg).value_or(static_cast<uint8_t>(aar::Shape::box)));
    for (int idc : {IDC_AMBIENCE_BOX_GROUP, IDC_AMBIENCE_WIDTH, IDC_AMBIENCE_WIDTH_SPIN, IDC_AMBIENCE_HEIGHT,
                    IDC_AMBIENCE_HEIGHT_SPIN, IDC_AMBIENCE_DEPTH, IDC_AMBIENCE_DEPTH_SPIN}) {
        EnableWindow(GetDlgItem(hdlg, idc), !sphere);
    }
    for (int idc : {IDC_AMBIENCE_SPHERE_GROUP, IDC_AMBIENCE_RADIUS, IDC_AMBIENCE_RADIUS_SPIN}) {
        EnableWindow(GetDlgItem(hdlg, idc), sphere);
    }
}

void browse_sound(HWND hdlg)
{
    char file[MAX_PATH] = {};
    char title[MAX_PATH] = {};
    GetDlgItemTextA(hdlg, IDC_AMBIENCE_SOUND, file, sizeof(file));
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hdlg;
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof(file);
    ofn.lpstrFileTitle = title;
    ofn.nMaxFileTitle = sizeof(title);
    if (alpine_sound_browser_answer(&ofn)) {
        SetDlgItemTextA(hdlg, IDC_AMBIENCE_SOUND, file);
    }
}

INT_PTR CALLBACK AmbienceRegionDialogProc(HWND hdlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        if (g_selected_regions.empty()) {
            EndDialog(hdlg, IDCANCEL);
            return TRUE;
        }
        const DedAmbienceRegion* region = g_selected_regions[0];

        SetDlgItemTextA(hdlg, IDC_AMBIENCE_SCRIPT_NAME, region->script_name.c_str());
        SetDlgItemTextA(hdlg, IDC_AMBIENCE_SOUND, region->sound.c_str());
        SendDlgItemMessageA(hdlg, IDC_AMBIENCE_SOUND, EM_LIMITTEXT, aar::max_sound_name, 0);
        alpine_dlg_set_float_field_exact(hdlg, IDC_AMBIENCE_VOLUME, region->volume);
        alpine_dlg_set_float_field_exact(hdlg, IDC_AMBIENCE_FADE_DISTANCE, region->fade_distance);
        alpine_dlg_set_float_field_exact(hdlg, IDC_AMBIENCE_FADE_TIME, region->fade_time);

        alpine_dlg_combo_add(hdlg, IDC_AMBIENCE_SHAPE, "Box", static_cast<LPARAM>(aar::Shape::box));
        alpine_dlg_combo_add(hdlg, IDC_AMBIENCE_SHAPE, "Sphere", static_cast<LPARAM>(aar::Shape::sphere));
        alpine_dlg_combo_select(hdlg, IDC_AMBIENCE_SHAPE, region->shape);

        alpine_dlg_set_float_field_exact(hdlg, IDC_AMBIENCE_WIDTH, region->width);
        alpine_dlg_set_float_field_exact(hdlg, IDC_AMBIENCE_HEIGHT, region->height);
        alpine_dlg_set_float_field_exact(hdlg, IDC_AMBIENCE_DEPTH, region->depth);
        alpine_dlg_set_float_field_exact(hdlg, IDC_AMBIENCE_RADIUS, region->radius);
        alpine_spinner_init(hdlg, IDC_AMBIENCE_WIDTH, IDC_AMBIENCE_WIDTH_SPIN, dim_step, dim_min, dim_max, 2);
        alpine_spinner_init(hdlg, IDC_AMBIENCE_HEIGHT, IDC_AMBIENCE_HEIGHT_SPIN, dim_step, dim_min, dim_max, 2);
        alpine_spinner_init(hdlg, IDC_AMBIENCE_DEPTH, IDC_AMBIENCE_DEPTH_SPIN, dim_step, dim_min, dim_max, 2);
        alpine_spinner_init(hdlg, IDC_AMBIENCE_RADIUS, IDC_AMBIENCE_RADIUS_SPIN, dim_step, dim_min, dim_max, 2);

        CheckDlgButton(hdlg, IDC_AMBIENCE_ALWAYS_SHOW_RANGE, region->always_show_range ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hdlg, IDC_AMBIENCE_INITIALLY_ENABLED, region->initially_enabled ? BST_CHECKED : BST_UNCHECKED);

        update_shape_fields(hdlg);
        capture_dims_preview(hdlg);
        g_dims_preview.active = true;
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_AMBIENCE_SHAPE:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                update_shape_fields(hdlg);
                capture_dims_preview(hdlg);
                redraw_all_viewports();
            }
            break;
        case IDC_AMBIENCE_WIDTH:
        case IDC_AMBIENCE_HEIGHT:
        case IDC_AMBIENCE_DEPTH:
        case IDC_AMBIENCE_RADIUS:
            if (HIWORD(wp) == EN_CHANGE && g_dims_preview.active) {
                capture_dims_preview(hdlg);
                redraw_all_viewports();
            }
            break;
        case IDC_AMBIENCE_SOUND_BROWSE:
            browse_sound(hdlg);
            return TRUE;
        case IDOK: {
            char name[256] = {};
            GetDlgItemTextA(hdlg, IDC_AMBIENCE_SOUND, name, sizeof(name));
            const std::string sound{trim(name)};
            // The game skips a region with any other name.
            if (!aar::sound_name_valid(sound)) {
                MessageBoxA(hdlg,
                            std::format("The sound must be a file name of 1 to {} characters, without a folder.",
                                        aar::max_sound_name).c_str(),
                            "Ambience Region", MB_OK | MB_ICONWARNING);
                return TRUE;
            }

            char script_name[256] = {};
            GetDlgItemTextA(hdlg, IDC_AMBIENCE_SCRIPT_NAME, script_name, sizeof(script_name));
            const DedAmbienceRegion* first = g_selected_regions[0];
            const auto width = field_value(hdlg, IDC_AMBIENCE_WIDTH, first->width);
            const auto height = field_value(hdlg, IDC_AMBIENCE_HEIGHT, first->height);
            const auto depth = field_value(hdlg, IDC_AMBIENCE_DEPTH, first->depth);
            const auto radius = field_value(hdlg, IDC_AMBIENCE_RADIUS, first->radius);
            const auto volume = field_value(hdlg, IDC_AMBIENCE_VOLUME, first->volume);
            const auto fade_distance = field_value(hdlg, IDC_AMBIENCE_FADE_DISTANCE, first->fade_distance);
            const auto fade_time = field_value(hdlg, IDC_AMBIENCE_FADE_TIME, first->fade_time);
            const bool always_show_range = IsDlgButtonChecked(hdlg, IDC_AMBIENCE_ALWAYS_SHOW_RANGE) == BST_CHECKED;
            const bool initially_enabled = IsDlgButtonChecked(hdlg, IDC_AMBIENCE_INITIALLY_ENABLED) == BST_CHECKED;

            // Every other field bulk-applies, but the script name is the region's identity.
            const bool single = g_selected_regions.size() == 1;
            for (auto* r : g_selected_regions) {
                if (single) {
                    r->script_name.assign_0(script_name);
                }
                r->sound = sound;
                r->shape = selected_shape(hdlg).value_or(r->shape);
                r->width = aar::sanitize_dim(width.value_or(r->width), r->width);
                r->height = aar::sanitize_dim(height.value_or(r->height), r->height);
                r->depth = aar::sanitize_dim(depth.value_or(r->depth), r->depth);
                r->radius = aar::sanitize_dim(radius.value_or(r->radius), r->radius);
                r->volume = aar::sanitize_volume(volume.value_or(r->volume));
                r->fade_distance = aar::sanitize_fade_distance(fade_distance.value_or(r->fade_distance));
                r->fade_time = aar::sanitize_fade_time(fade_time.value_or(r->fade_time));
                r->always_show_range = always_show_range;
                r->initially_enabled = initially_enabled;
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

void ShowAmbienceRegionPropertiesDialog(CDedLevel* level)
{
    auto& sel = level->selection;
    g_selected_regions.clear();
    for (int i = 0; i < sel.get_size(); i++) {
        DedObject* obj = sel[i];
        if (obj && obj->type == DedObjectType::DED_AMBIENCE_REGION) {
            g_selected_regions.push_back(static_cast<DedAmbienceRegion*>(obj));
        }
    }
    if (!g_selected_regions.empty()) {
        DialogBoxParam(reinterpret_cast<HINSTANCE>(&__ImageBase),
            MAKEINTRESOURCE(IDD_ALPINE_AMBIENCE_REGION_PROPERTIES), GetActiveWindow(), AmbienceRegionDialogProc, 0);
        g_dims_preview = {};
    }
    g_selected_regions.clear();
}

// ─── Cleanup ─────────────────────────────────────────────────────────────────

void DestroyDedAmbienceRegion(DedAmbienceRegion* region)
{
    if (!region) return;
    region->class_mesh_filename.free();
    region->script_name.free();
    region->class_name.free();
    delete region;
}

// ─── Serialization ──────────────────────────────────────────────────────────

// Ambience Region chunk (id 0x0AFBAE10, RFL 306+): u32 count, then alpine_ambience_region::write_record each.
void ambience_region_serialize_chunk(CDedLevel& level, rf::File& file)
{
    auto& regions = level.GetAlpineLevelProperties().ambience_region_objects;
    if (regions.empty()) return;

    auto start_pos = level.BeginRflSection(file, alpine_ambience_region_chunk_id);

    const auto count = static_cast<uint32_t>(std::min<std::size_t>(regions.size(), aar::max_regions));
    if (regions.size() > count) {
        editor_report(EditorReportLevel::warn, "AmbienceRegion",
                      std::format("Only the first {} of {} ambience regions were saved.", count, regions.size()),
                      true);
    }
    file.write<uint32_t>(count);

    for (uint32_t i = 0; i < count; i++) {
        const DedAmbienceRegion& region = *regions[i];
        aar::Record rec;
        rec.uid = region.uid;
        rec.pos[0] = region.pos.x;
        rec.pos[1] = region.pos.y;
        rec.pos[2] = region.pos.z;
        const Vector3* axes[] = {&region.orient.rvec, &region.orient.uvec, &region.orient.fvec};
        for (int a = 0; a < 3; a++) {
            rec.orient[a * 3] = axes[a]->x;
            rec.orient[a * 3 + 1] = axes[a]->y;
            rec.orient[a * 3 + 2] = axes[a]->z;
        }
        rec.script_name = region.script_name.c_str();
        rec.sound = region.sound;
        rec.shape = region.shape;
        rec.width = region.width;
        rec.height = region.height;
        rec.depth = region.depth;
        rec.radius = region.radius;
        rec.volume = region.volume;
        rec.fade_distance = region.fade_distance;
        rec.fade_time = region.fade_time;
        rec.flags = (region.initially_enabled ? aar::flag_initially_enabled : 0)
                    | (region.always_show_range ? aar::flag_always_show_range : 0);
        aar::write_record(file, rec);
    }

    level.EndRflSection(file, start_pos);
}

void ambience_region_deserialize_chunk(CDedLevel& level, rf::File& file, std::size_t chunk_len)
{
    std::size_t remaining = chunk_len;

    rf::File::ChunkGuard chunk_guard{file, remaining};
    RflChunkReader<rf::File> reader{file, remaining};

    uint32_t count = 0;
    if (!reader.read(count)) return;
    count = std::min(count, aar::max_regions);

    uint32_t loaded = 0;
    for (uint32_t i = 0; i < count; i++) {
        aar::Record rec;
        if (!aar::read_record(reader, rec)) {
            break;
        }
        auto* region = new_ambience_region();
        region->uid = rec.uid;
        if (aar::all_finite(rec.pos)) {
            region->pos = {rec.pos[0], rec.pos[1], rec.pos[2]};
        }
        if (aar::all_finite(rec.orient)) {
            region->orient.rvec = {rec.orient[0], rec.orient[1], rec.orient[2]};
            region->orient.uvec = {rec.orient[3], rec.orient[4], rec.orient[5]};
            region->orient.fvec = {rec.orient[6], rec.orient[7], rec.orient[8]};
        }
        region->script_name.assign_0(rec.script_name.c_str());
        region->sound = rec.sound;
        // An unknown shape is kept as it is, so a newer level round-trips.
        region->shape = rec.shape;
        region->width = aar::sanitize_dim(rec.width, aar::default_size);
        region->height = aar::sanitize_dim(rec.height, aar::default_size);
        region->depth = aar::sanitize_dim(rec.depth, aar::default_size);
        region->radius = aar::sanitize_dim(rec.radius, aar::default_radius);
        region->volume = aar::sanitize_volume(rec.volume);
        region->fade_distance = aar::sanitize_fade_distance(rec.fade_distance);
        region->fade_time = aar::sanitize_fade_time(rec.fade_time);
        region->initially_enabled = (rec.flags & aar::flag_initially_enabled) != 0;
        region->always_show_range = (rec.flags & aar::flag_always_show_range) != 0;
        add_to_level(level, region);
        loaded++;
    }

    xlog::info("[AmbienceRegion] Loaded {} ambience region object(s)", loaded);
}

// ─── Object Lifecycle ───────────────────────────────────────────────────────

void PlaceNewAmbienceRegionObject()
{
    auto* level = CDedLevel::Get();
    if (!level) return;

    auto* region = new_ambience_region();
    region->script_name.assign_0("Ambience Region");
    editor_new_object_pose(region->pos, region->orient);
    region->uid = generate_uid();
    add_to_level(*level, region);

    level->clear_selection();
    level->add_to_selection(static_cast<DedObject*>(region));
    level->update_console_display();
}

DedAmbienceRegion* CloneAmbienceRegionObject(DedAmbienceRegion* source, bool add_to_level_now)
{
    if (!source) return nullptr;

    auto* region = new_ambience_region();
    region->pos = source->pos;
    region->orient = source->orient;
    region->script_name.assign_0(source->script_name.c_str());
    region->shape = source->shape;
    region->width = source->width;
    region->height = source->height;
    region->depth = source->depth;
    region->radius = source->radius;
    region->sound = source->sound;
    region->volume = source->volume;
    region->fade_distance = source->fade_distance;
    region->fade_time = source->fade_time;
    region->initially_enabled = source->initially_enabled;
    region->always_show_range = source->always_show_range;
    region->uid = generate_uid();

    if (add_to_level_now) {
        if (auto* level = CDedLevel::Get()) {
            add_to_level(*level, region);
        }
    }
    return region;
}

// ─── Rendering ──────────────────────────────────────────────────────────────

void ambience_region_render(CDedLevel* level)
{
    auto& regions = level->GetAlpineLevelProperties().ambience_region_objects;
    if (regions.empty()) return;

    if (g_icon_handle < 0) {
        g_icon_handle = bm_load("Icon_AFAmbience.tga", -1, 1);
    }

    const uint32_t mode = editor_line_mode();
    for (auto* region : regions) {
        if (region->hidden_in_editor) continue;

        const bool selected = is_object_selected(level, region);
        const int r = 0xff;
        const int g = selected ? 0x00 : 0xb4;
        const int b = selected ? 0x00 : 0x3c;

        if (selected || region->always_show_range) {
            set_draw_color(r, g, b, 0xff);
            uint8_t shape = region->shape;
            Vector3 dims{region->width, region->height, region->depth};
            float radius = region->radius;
            if (g_dims_preview.active
                && std::find(g_selected_regions.begin(), g_selected_regions.end(), region) != g_selected_regions.end()) {
                shape = g_dims_preview.shape.value_or(shape);
                dims.x = g_dims_preview.width.value_or(dims.x);
                dims.y = g_dims_preview.height.value_or(dims.y);
                dims.z = g_dims_preview.depth.value_or(dims.z);
                radius = g_dims_preview.radius.value_or(radius);
            }
            if (is_sphere(shape)) {
                draw_wireframe_sphere_3d(&region->pos, radius, mode);
            }
            else if (shape == static_cast<uint8_t>(aar::Shape::box)) {
                draw_wireframe_box_3d(&region->pos, &region->orient, &dims, mode);
            }
        }

        set_draw_color(r, g, b, 0xff);
        if (g_icon_handle >= 0) {
            gr_set_bitmap(g_icon_handle, -1);
        }
        gr_render_billboard(&region->pos, 0, icon_size, rf::gr::bitmap_3d_mode_no_z);
    }
}

void ambience_region_pick(CDedLevel* level, int param1, int param2)
{
    for (auto* region : level->GetAlpineLevelProperties().ambience_region_objects) {
        if (region->hidden_in_editor) continue;
        if (level->hit_test_point(param1, param2, &region->pos)) {
            level->select_object(static_cast<DedObject*>(region));
        }
    }
}

DedAmbienceRegion* ambience_region_click_pick(CDedLevel* level, float click_x, float click_y)
{
    return alpine_click_pick_point(level->GetAlpineLevelProperties().ambience_region_objects, click_x, click_y,
                                   alpine_click_pick_radius_sq);
}

void ambience_region_tree_populate(EditorTreeCtrl* tree, int master_groups, CDedLevel* level)
{
    auto& regions = level->GetAlpineLevelProperties().ambience_region_objects;

    char buf[64];
    std::snprintf(buf, sizeof(buf), "Ambience Regions (%d)", static_cast<int>(regions.size()));
    int parent = tree->insert_item(buf, master_groups, 0xffff0002);

    for (auto* region : regions) {
        const char* name = region->script_name.c_str();
        if (!name || name[0] == '\0') {
            name = "(unnamed ambience region)";
        }
        int child = tree->insert_item(name, parent, 0xffff0002);
        tree->set_item_data(child, region->uid);
    }
}

void ambience_region_tree_add_object_type(EditorTreeCtrl* tree)
{
    tree->insert_item("Ambience Region", 0xffff0000, 0xffff0002);
}

// ─── Clipboard / Delete ─────────────────────────────────────────────────────

bool ambience_region_copy_object(DedObject* source)
{
    if (!source || source->type != DedObjectType::DED_AMBIENCE_REGION) return false;
    auto* staged = CloneAmbienceRegionObject(static_cast<DedAmbienceRegion*>(source), false);
    if (!staged) return false;
    g_ambience_region_clipboard.push_back(staged);
    return true;
}

void ambience_region_paste_objects(CDedLevel* level)
{
    for (auto* staged : g_ambience_region_clipboard) {
        if (auto* clone = CloneAmbienceRegionObject(staged, true)) {
            level->add_to_selection(static_cast<DedObject*>(clone));
        }
    }
}

void ambience_region_swap_clipboard(std::vector<DedAmbienceRegion*>& other)
{
    g_ambience_region_clipboard.swap(other);
}

void ambience_region_clear_clipboard()
{
    for (auto* region : g_ambience_region_clipboard) {
        DestroyDedAmbienceRegion(region);
    }
    g_ambience_region_clipboard.clear();
}

void ambience_region_handle_delete_or_cut(DedObject* obj)
{
    if (!obj || obj->type != DedObjectType::DED_AMBIENCE_REGION) return;
    auto* level = CDedLevel::Get();
    if (!level) return;

    auto& regions = level->GetAlpineLevelProperties().ambience_region_objects;
    auto it = std::find(regions.begin(), regions.end(), static_cast<DedAmbienceRegion*>(obj));
    if (it != regions.end()) {
        regions.erase(it);
    }
}

void ambience_region_ensure_uid(int& uid)
{
    auto* level = CDedLevel::Get();
    if (!level) return;
    alpine_ensure_uid(level->GetAlpineLevelProperties().ambience_region_objects, uid);
}
