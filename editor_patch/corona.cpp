#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <algorithm>
#include <vector>
#include <cmath>
#include <xlog/xlog.h>
#include "corona.h"
#include "level.h"
#include "resources.h"
#include "vtypes.h"
#include "alpine_obj.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

// ─── Globals ─────────────────────────────────────────────────────────────────

static int g_corona_icon_handle = -1;
static std::vector<DedCorona*> g_corona_clipboard;

// ─── Sprite Loading ──────────────────────────────────────────────────────────

static void corona_load_icon()
{
    if (g_corona_icon_handle < 0) {
        g_corona_icon_handle = bm_load("Icon_AFCorona.tga", -1, 1);
        if (g_corona_icon_handle >= 0) {
            xlog::info("[Corona] Loaded corona icon: handle={}", g_corona_icon_handle);
        }
        else {
            xlog::warn("[Corona] Failed to load corona icon");
        }
    }
}

// ─── Cleanup ─────────────────────────────────────────────────────────────────

void DestroyDedCorona(DedCorona* corona)
{
    if (!corona) return;
    corona->field_4.free();
    corona->script_name.free();
    corona->class_name.free();
    delete corona;
}

// ─── Serialization ──────────────────────────────────────────────────────────

void corona_serialize_chunk(CDedLevel& level, rf::File& file)
{
    auto& coronas = level.GetAlpineLevelProperties().corona_objects;
    if (coronas.empty()) return;

    auto start_pos = level.BeginRflSection(file, alpine_corona_chunk_id);

    uint32_t count = static_cast<uint32_t>(coronas.size());
    file.write<uint32_t>(count);

    for (auto* corona : coronas) {
        file.write<int32_t>(corona->uid);
        file.write<float>(corona->pos.x);
        file.write<float>(corona->pos.y);
        file.write<float>(corona->pos.z);
        file.write<float>(corona->orient.rvec.x);
        file.write<float>(corona->orient.rvec.y);
        file.write<float>(corona->orient.rvec.z);
        file.write<float>(corona->orient.uvec.x);
        file.write<float>(corona->orient.uvec.y);
        file.write<float>(corona->orient.uvec.z);
        file.write<float>(corona->orient.fvec.x);
        file.write<float>(corona->orient.fvec.y);
        file.write<float>(corona->orient.fvec.z);
        write_rfl_string(file, corona->script_name);
        // color
        file.write<uint8_t>(corona->color_r);
        file.write<uint8_t>(corona->color_g);
        file.write<uint8_t>(corona->color_b);
        file.write<uint8_t>(corona->color_a);
        // corona bitmap
        write_rfl_string(file, corona->corona_bitmap);
        // float properties
        file.write<float>(corona->cone_angle);
        file.write<float>(corona->intensity);
        file.write<float>(corona->radius_distance);
        file.write<float>(corona->radius_scale);
        file.write<float>(corona->diminish_distance);
        // volumetric bitmap
        write_rfl_string(file, corona->volumetric_bitmap);
        if (!corona->volumetric_bitmap.empty()) {
            file.write<float>(corona->volumetric_height);
            file.write<float>(corona->volumetric_length);
        }
    }

    level.EndRflSection(file, start_pos);
}

void corona_deserialize_chunk(CDedLevel& level, rf::File& file, std::size_t chunk_len)
{
    auto& coronas = level.GetAlpineLevelProperties().corona_objects;
    std::size_t remaining = chunk_len;

    rf::File::ChunkGuard chunk_guard{file, remaining};

    auto read_bytes = [&](void* dst, std::size_t n) -> bool {
        if (remaining < n) return false;
        int got = file.read(dst, n);
        if (got != static_cast<int>(n)) {
            if (got > 0) remaining -= got;
            return false;
        }
        remaining -= n;
        return true;
    };

    uint32_t count = 0;
    if (!read_bytes(&count, sizeof(count))) return;
    if (count > 10000) count = 10000;

    for (uint32_t i = 0; i < count; i++) {
        auto* corona = new DedCorona();
        memset(static_cast<DedObject*>(corona), 0, sizeof(DedObject));
        corona->vtbl = reinterpret_cast<void*>(ded_object_vtbl_addr);
        corona->type = DedObjectType::DED_CORONA;

        if (!read_bytes(&corona->uid, sizeof(corona->uid))) { DestroyDedCorona(corona); return; }
        if (!read_bytes(&corona->pos.x, sizeof(float))) { DestroyDedCorona(corona); return; }
        if (!read_bytes(&corona->pos.y, sizeof(float))) { DestroyDedCorona(corona); return; }
        if (!read_bytes(&corona->pos.z, sizeof(float))) { DestroyDedCorona(corona); return; }
        if (!read_bytes(&corona->orient.rvec.x, sizeof(float))) { DestroyDedCorona(corona); return; }
        if (!read_bytes(&corona->orient.rvec.y, sizeof(float))) { DestroyDedCorona(corona); return; }
        if (!read_bytes(&corona->orient.rvec.z, sizeof(float))) { DestroyDedCorona(corona); return; }
        if (!read_bytes(&corona->orient.uvec.x, sizeof(float))) { DestroyDedCorona(corona); return; }
        if (!read_bytes(&corona->orient.uvec.y, sizeof(float))) { DestroyDedCorona(corona); return; }
        if (!read_bytes(&corona->orient.uvec.z, sizeof(float))) { DestroyDedCorona(corona); return; }
        if (!read_bytes(&corona->orient.fvec.x, sizeof(float))) { DestroyDedCorona(corona); return; }
        if (!read_bytes(&corona->orient.fvec.y, sizeof(float))) { DestroyDedCorona(corona); return; }
        if (!read_bytes(&corona->orient.fvec.z, sizeof(float))) { DestroyDedCorona(corona); return; }

        std::string sname = read_rfl_string(file, remaining);
        corona->script_name.assign_0(sname.c_str());

        // color
        if (!read_bytes(&corona->color_r, sizeof(uint8_t))) { DestroyDedCorona(corona); return; }
        if (!read_bytes(&corona->color_g, sizeof(uint8_t))) { DestroyDedCorona(corona); return; }
        if (!read_bytes(&corona->color_b, sizeof(uint8_t))) { DestroyDedCorona(corona); return; }
        if (!read_bytes(&corona->color_a, sizeof(uint8_t))) { DestroyDedCorona(corona); return; }

        // corona bitmap
        corona->corona_bitmap = read_rfl_string(file, remaining);

        // float properties
        if (!read_bytes(&corona->cone_angle, sizeof(float))) { DestroyDedCorona(corona); return; }
        if (!read_bytes(&corona->intensity, sizeof(float))) { DestroyDedCorona(corona); return; }
        if (!read_bytes(&corona->radius_distance, sizeof(float))) { DestroyDedCorona(corona); return; }
        if (!read_bytes(&corona->radius_scale, sizeof(float))) { DestroyDedCorona(corona); return; }
        if (!read_bytes(&corona->diminish_distance, sizeof(float))) { DestroyDedCorona(corona); return; }

        // volumetric bitmap
        corona->volumetric_bitmap = read_rfl_string(file, remaining);
        if (!corona->volumetric_bitmap.empty()) {
            if (!read_bytes(&corona->volumetric_height, sizeof(float))) { DestroyDedCorona(corona); return; }
            if (!read_bytes(&corona->volumetric_length, sizeof(float))) { DestroyDedCorona(corona); return; }
        }

        coronas.push_back(corona);
        level.master_objects.add(static_cast<DedObject*>(corona));
    }

    xlog::info("[Corona] Loaded {} corona objects", coronas.size());
}

// ─── Properties Dialog ──────────────────────────────────────────────────────

static std::vector<DedCorona*> g_selected_coronas;

static void corona_set_float_field(HWND hdlg, int idc, float value)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%.4g", value);
    SetDlgItemTextA(hdlg, idc, buf);
}

static float corona_get_float_field(HWND hdlg, int idc)
{
    char buf[32] = {};
    GetDlgItemTextA(hdlg, idc, buf, sizeof(buf));
    return static_cast<float>(atof(buf));
}

static INT_PTR CALLBACK CoronaDialogProc(HWND hdlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        if (g_selected_coronas.empty()) {
            EndDialog(hdlg, IDCANCEL);
            return TRUE;
        }
        auto* corona = g_selected_coronas[0];

        SetDlgItemTextA(hdlg, IDC_CORONA_SCRIPT_NAME, corona->script_name.c_str());
        SetDlgItemTextA(hdlg, IDC_CORONA_BITMAP, corona->corona_bitmap.c_str());
        SetDlgItemTextA(hdlg, IDC_CORONA_VOLUMETRIC_BITMAP, corona->volumetric_bitmap.c_str());

        corona_set_float_field(hdlg, IDC_CORONA_CONE_ANGLE, corona->cone_angle);
        corona_set_float_field(hdlg, IDC_CORONA_INTENSITY, corona->intensity);
        corona_set_float_field(hdlg, IDC_CORONA_RADIUS_DISTANCE, corona->radius_distance);
        corona_set_float_field(hdlg, IDC_CORONA_RADIUS_SCALE, corona->radius_scale);
        corona_set_float_field(hdlg, IDC_CORONA_DIMINISH_DISTANCE, corona->diminish_distance);
        corona_set_float_field(hdlg, IDC_CORONA_VOLUMETRIC_HEIGHT, corona->volumetric_height);
        corona_set_float_field(hdlg, IDC_CORONA_VOLUMETRIC_LENGTH, corona->volumetric_length);

        // Color fields (no alpha — unused by the engine)
        SetDlgItemInt(hdlg, IDC_CORONA_COLOR_R, corona->color_r, FALSE);
        SetDlgItemInt(hdlg, IDC_CORONA_COLOR_G, corona->color_g, FALSE);
        SetDlgItemInt(hdlg, IDC_CORONA_COLOR_B, corona->color_b, FALSE);

        CheckDlgButton(hdlg, IDC_CORONA_SHOW_IN_EDITOR, corona->show_in_editor ? BST_CHECKED : BST_UNCHECKED);

        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_CORONA_COLOR_R:
        case IDC_CORONA_COLOR_G:
        case IDC_CORONA_COLOR_B:
            // Refresh color preview when RGB fields change
            if (HIWORD(wp) == EN_CHANGE) {
                InvalidateRect(GetDlgItem(hdlg, IDC_CORONA_COLOR_PREVIEW), nullptr, TRUE);
            }
            break;
        case IDC_CORONA_COLOR_CHANGE: {
            CHOOSECOLORA cc = {};
            static COLORREF custom_colors[16] = {};
            cc.lStructSize = sizeof(cc);
            cc.hwndOwner = hdlg;
            cc.rgbResult = RGB(
                GetDlgItemInt(hdlg, IDC_CORONA_COLOR_R, nullptr, FALSE),
                GetDlgItemInt(hdlg, IDC_CORONA_COLOR_G, nullptr, FALSE),
                GetDlgItemInt(hdlg, IDC_CORONA_COLOR_B, nullptr, FALSE));
            cc.lpCustColors = custom_colors;
            cc.Flags = CC_RGBINIT | CC_FULLOPEN;
            if (ChooseColorA(&cc)) {
                SetDlgItemInt(hdlg, IDC_CORONA_COLOR_R, GetRValue(cc.rgbResult), FALSE);
                SetDlgItemInt(hdlg, IDC_CORONA_COLOR_G, GetGValue(cc.rgbResult), FALSE);
                SetDlgItemInt(hdlg, IDC_CORONA_COLOR_B, GetBValue(cc.rgbResult), FALSE);
            }
            return TRUE;
        }
        case IDOK: {
            char buf[256] = {};
            GetDlgItemTextA(hdlg, IDC_CORONA_SCRIPT_NAME, buf, sizeof(buf));
            char bmp_buf[256] = {};
            GetDlgItemTextA(hdlg, IDC_CORONA_BITMAP, bmp_buf, sizeof(bmp_buf));
            char vol_buf[256] = {};
            GetDlgItemTextA(hdlg, IDC_CORONA_VOLUMETRIC_BITMAP, vol_buf, sizeof(vol_buf));

            float cone_angle = corona_get_float_field(hdlg, IDC_CORONA_CONE_ANGLE);
            float intensity = corona_get_float_field(hdlg, IDC_CORONA_INTENSITY);
            float radius_distance = corona_get_float_field(hdlg, IDC_CORONA_RADIUS_DISTANCE);
            float radius_scale = corona_get_float_field(hdlg, IDC_CORONA_RADIUS_SCALE);
            float diminish_distance = corona_get_float_field(hdlg, IDC_CORONA_DIMINISH_DISTANCE);
            float vol_height = corona_get_float_field(hdlg, IDC_CORONA_VOLUMETRIC_HEIGHT);
            float vol_length = corona_get_float_field(hdlg, IDC_CORONA_VOLUMETRIC_LENGTH);

            uint8_t r = static_cast<uint8_t>(std::min(GetDlgItemInt(hdlg, IDC_CORONA_COLOR_R, nullptr, FALSE), 255u));
            uint8_t g = static_cast<uint8_t>(std::min(GetDlgItemInt(hdlg, IDC_CORONA_COLOR_G, nullptr, FALSE), 255u));
            uint8_t b = static_cast<uint8_t>(std::min(GetDlgItemInt(hdlg, IDC_CORONA_COLOR_B, nullptr, FALSE), 255u));

            for (auto* c : g_selected_coronas) {
                c->script_name.assign_0(buf);
                c->corona_bitmap = bmp_buf;
                c->volumetric_bitmap = vol_buf;
                c->cone_angle = cone_angle;
                c->intensity = intensity;
                c->radius_distance = radius_distance;
                c->radius_scale = radius_scale;
                c->diminish_distance = diminish_distance;
                c->volumetric_height = vol_height;
                c->volumetric_length = vol_length;
                c->color_r = r;
                c->color_g = g;
                c->color_b = b;
                c->color_a = 255;
                c->show_in_editor = (IsDlgButtonChecked(hdlg, IDC_CORONA_SHOW_IN_EDITOR) == BST_CHECKED);
            }
            EndDialog(hdlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(hdlg, IDCANCEL);
            return TRUE;
        }
        break;
    case WM_DRAWITEM: {
        // Draw color preview swatch (SS_OWNERDRAW static control)
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (dis && dis->CtlID == IDC_CORONA_COLOR_PREVIEW) {
            uint8_t r = static_cast<uint8_t>(std::min(GetDlgItemInt(hdlg, IDC_CORONA_COLOR_R, nullptr, FALSE), 255u));
            uint8_t g = static_cast<uint8_t>(std::min(GetDlgItemInt(hdlg, IDC_CORONA_COLOR_G, nullptr, FALSE), 255u));
            uint8_t b = static_cast<uint8_t>(std::min(GetDlgItemInt(hdlg, IDC_CORONA_COLOR_B, nullptr, FALSE), 255u));
            HBRUSH brush = CreateSolidBrush(RGB(r, g, b));
            FillRect(dis->hDC, &dis->rcItem, brush);
            DeleteObject(brush);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

void ShowCoronaPropertiesDialog(CDedLevel* level)
{
    auto& sel = level->selection;
    g_selected_coronas.clear();
    for (int i = 0; i < sel.get_size(); i++) {
        DedObject* obj = sel[i];
        if (obj && obj->type == DedObjectType::DED_CORONA) {
            g_selected_coronas.push_back(static_cast<DedCorona*>(obj));
        }
    }

    if (!g_selected_coronas.empty()) {
        DialogBoxParam(
            reinterpret_cast<HINSTANCE>(&__ImageBase),
            MAKEINTRESOURCE(IDD_ALPINE_CORONA_PROPERTIES),
            GetActiveWindow(),
            CoronaDialogProc,
            0
        );
    }

    g_selected_coronas.clear();
}

// ─── Object Lifecycle ───────────────────────────────────────────────────────

void PlaceNewCoronaObject()
{
    auto* level = CDedLevel::Get();
    if (!level) return;

    auto* corona = new DedCorona();
    memset(static_cast<DedObject*>(corona), 0, sizeof(DedObject));
    corona->vtbl = reinterpret_cast<void*>(ded_object_vtbl_addr);
    corona->type = DedObjectType::DED_CORONA;

    corona->script_name.assign_0("Corona");

    auto* viewport = get_active_viewport();
    if (viewport && viewport->view_data) {
        corona->pos = viewport->view_data->camera_pos;
    }

    corona->orient.rvec = {1.0f, 0.0f, 0.0f};
    corona->orient.uvec = {0.0f, 1.0f, 0.0f};
    corona->orient.fvec = {0.0f, 0.0f, 1.0f};

    corona->uid = generate_uid();

    level->GetAlpineLevelProperties().corona_objects.push_back(corona);
    level->master_objects.add(static_cast<DedObject*>(corona));

    level->clear_selection();
    level->add_to_selection(static_cast<DedObject*>(corona));
    level->update_console_display();
}

DedCorona* CloneCoronaObject(DedCorona* source, bool add_to_level)
{
    if (!source) return nullptr;

    auto* corona = new DedCorona();
    memset(static_cast<DedObject*>(corona), 0, sizeof(DedObject));
    corona->vtbl = reinterpret_cast<void*>(ded_object_vtbl_addr);
    corona->type = DedObjectType::DED_CORONA;

    corona->pos = source->pos;
    corona->orient = source->orient;
    corona->script_name.assign_0(source->script_name.c_str());

    // Copy corona-specific properties
    corona->color_r = source->color_r;
    corona->color_g = source->color_g;
    corona->color_b = source->color_b;
    corona->color_a = source->color_a;
    corona->corona_bitmap = source->corona_bitmap;
    corona->cone_angle = source->cone_angle;
    corona->intensity = source->intensity;
    corona->radius_distance = source->radius_distance;
    corona->radius_scale = source->radius_scale;
    corona->diminish_distance = source->diminish_distance;
    corona->volumetric_bitmap = source->volumetric_bitmap;
    corona->volumetric_height = source->volumetric_height;
    corona->volumetric_length = source->volumetric_length;
    corona->show_in_editor = source->show_in_editor;

    corona->uid = generate_uid();

    if (add_to_level) {
        auto* level = CDedLevel::Get();
        if (level) {
            level->GetAlpineLevelProperties().corona_objects.push_back(corona);
            level->master_objects.add(static_cast<DedObject*>(corona));
        }
    }

    return corona;
}

void DeleteCoronaObject(DedCorona* corona)
{
    if (!corona) return;
    auto* level = CDedLevel::Get();
    if (!level) return;

    auto& coronas = level->GetAlpineLevelProperties().corona_objects;
    auto it = std::find(coronas.begin(), coronas.end(), corona);
    if (it != coronas.end()) {
        coronas.erase(it);
    }
    level->master_objects.remove_by_value(static_cast<DedObject*>(corona));
    DestroyDedCorona(corona);
}

// ─── Rendering ──────────────────────────────────────────────────────────────

void corona_render(CDedLevel* level)
{
    auto& coronas = level->GetAlpineLevelProperties().corona_objects;
    if (coronas.empty()) return;

    corona_load_icon();

    float cam_param = gr_cam_param;

    for (auto* corona : coronas) {
        if (corona->hidden_in_editor) continue;

        bool selected = is_object_selected(level, corona);

        if (corona->show_in_editor) {
            // Show corona bitmap with additive blending (no icon)
            if (!corona->corona_bitmap.empty()) {
                int bm_handle = bm_load(corona->corona_bitmap.c_str(), -1, 1);
                if (bm_handle >= 0) {
                    set_draw_color(corona->color_r, corona->color_g, corona->color_b, 255);
                    gr_set_bitmap(bm_handle, -1);
                    render_additive_billboard(&corona->pos, corona->radius_scale * 0.5f, cam_param);
                }
            }

            // Show volumetric bitmap as axial billboard along forward vector
            if (!corona->volumetric_bitmap.empty() && corona->volumetric_length > 0.0f) {
                int vol_handle = bm_load(corona->volumetric_bitmap.c_str(), -1, 1);
                if (vol_handle >= 0) {
                    set_draw_color(corona->color_r, corona->color_g, corona->color_b, 128);
                    gr_set_bitmap(vol_handle, -1);
                    render_additive_axial_quad(
                        corona->pos, corona->orient,
                        corona->volumetric_length, corona->volumetric_height,
                        cam_param);
                }
            }
        }
        else {
            // Show icon sprite (no corona bitmap)
            if (selected) {
                set_draw_color(0xff, 0x00, 0x00, 0xff);
            }
            else {
                set_draw_color(corona->color_r, corona->color_g, corona->color_b, 0xff);
            }

            if (g_corona_icon_handle >= 0) {
                gr_set_bitmap(g_corona_icon_handle, -1);
            }

            gr_render_billboard(&corona->pos, 0, 0.25f, cam_param);
        }

        // Always draw direction arrow (cyan) along forward vector with volumetric length
        if (corona->volumetric_length > 0.0f) {
            float len = corona->volumetric_length;
            draw_3d_arrow(
                corona->pos.x, corona->pos.y, corona->pos.z,
                corona->pos.x + corona->orient.fvec.x * len,
                corona->pos.y + corona->orient.fvec.y * len,
                corona->pos.z + corona->orient.fvec.z * len,
                0, 255, 255
            );
        }

        // Red bounding sphere when selected
        if (selected) {
            draw_wireframe_sphere(corona->pos.x, corona->pos.y, corona->pos.z,
                                  0.75f, 255, 0, 0);
        }
    }
}

void corona_pick(CDedLevel* level, int param1, int param2)
{
    auto& coronas = level->GetAlpineLevelProperties().corona_objects;
    for (auto* corona : coronas) {
        if (corona->hidden_in_editor) continue;
        bool hit = level->hit_test_point(param1, param2, &corona->pos);
        if (hit) {
            level->select_object(static_cast<DedObject*>(corona));
        }
    }
}

DedCorona* corona_click_pick(CDedLevel* level, float click_x, float click_y)
{
    auto& coronas = level->GetAlpineLevelProperties().corona_objects;
    float best_dist_sq = 1e30f;
    DedCorona* best_corona = nullptr;

    for (auto* corona : coronas) {
        if (corona->hidden_in_editor) continue;

        float center_pos[3] = {corona->pos.x, corona->pos.y, corona->pos.z};
        float screen_cx = 0.0f, screen_cy = 0.0f;
        if (!project_to_screen_2d(center_pos, &screen_cx, &screen_cy))
            continue;

        constexpr float screen_radius_sq = 400.0f; // 20px

        float dx = screen_cx - click_x;
        float dy = screen_cy - click_y;
        float dist_sq = dx * dx + dy * dy;
        if (dist_sq <= screen_radius_sq && dist_sq < best_dist_sq) {
            best_dist_sq = dist_sq;
            best_corona = corona;
        }
    }

    return best_corona;
}

void corona_tree_populate(EditorTreeCtrl* tree, int master_groups, CDedLevel* level)
{
    auto& coronas = level->GetAlpineLevelProperties().corona_objects;

    char buf[64];
    snprintf(buf, sizeof(buf), "Coronas (%d)", static_cast<int>(coronas.size()));
    int parent = tree->insert_item(buf, master_groups, 0xffff0002);

    for (auto* corona : coronas) {
        const char* name = corona->script_name.c_str();
        if (!name || name[0] == '\0') {
            name = "(unnamed corona)";
        }
        int child = tree->insert_item(name, parent, 0xffff0002);
        tree->set_item_data(child, corona->uid);
    }
}

void corona_tree_add_object_type(EditorTreeCtrl* tree)
{
    tree->insert_item("Corona", 0xffff0000, 0xffff0002);
}

bool corona_copy_object(DedObject* source)
{
    if (!source || source->type != DedObjectType::DED_CORONA) return false;
    auto* staged = CloneCoronaObject(static_cast<DedCorona*>(source), false);
    if (staged) {
        g_corona_clipboard.push_back(staged);
        return true;
    }
    return false;
}

void corona_paste_objects(CDedLevel* level)
{
    for (auto* staged : g_corona_clipboard) {
        auto* clone = CloneCoronaObject(staged, true);
        if (clone) {
            level->add_to_selection(static_cast<DedObject*>(clone));
        }
    }
}

void corona_clear_clipboard()
{
    for (auto* corona : g_corona_clipboard) {
        DestroyDedCorona(corona);
    }
    g_corona_clipboard.clear();
}

void corona_handle_delete_or_cut(DedObject* obj)
{
    if (!obj || obj->type != DedObjectType::DED_CORONA) return;
    auto* level = CDedLevel::Get();
    if (!level) return;

    auto& corona_objects = level->GetAlpineLevelProperties().corona_objects;
    auto it = std::find(corona_objects.begin(), corona_objects.end(), static_cast<DedCorona*>(obj));
    if (it != corona_objects.end()) {
        corona_objects.erase(it);
    }
}

void corona_handle_delete_selection(CDedLevel* level)
{
    auto& sel = level->selection;
    for (int i = sel.size - 1; i >= 0; i--) {
        DedObject* obj = sel.data_ptr[i];
        if (obj && obj->type == DedObjectType::DED_CORONA) {
            for (int j = i; j < sel.size - 1; j++) {
                sel.data_ptr[j] = sel.data_ptr[j + 1];
            }
            sel.size--;
            DeleteCoronaObject(static_cast<DedCorona*>(obj));
        }
    }
}

void corona_ensure_uid(int& uid)
{
    auto* level = CDedLevel::Get();
    if (!level) return;
    for (auto* c : level->GetAlpineLevelProperties().corona_objects) {
        if (c->uid >= uid) uid = c->uid + 1;
    }
}
