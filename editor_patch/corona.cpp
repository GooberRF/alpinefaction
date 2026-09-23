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
#include "alpine_color_picker.h"
#include "alpine_spinner.h"
#include "corona.h"
#include "level.h"
#include "resources.h"
#include "vtypes.h"
#include "alpine_obj.h"
#include "tbl.h"

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
        if (rfl_name_over_long(corona->corona_bitmap)) corona->corona_bitmap.clear();

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
        if (rfl_name_over_long(corona->volumetric_bitmap)) corona->volumetric_bitmap.clear();

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

// Authoring ranges. They bound what an arrow click or a drag can reach, and since the commit path
// holds itself to the same numbers, they bound what a typed value can store as well.
constexpr float corona_max_cone_angle = 360.0f;
constexpr float corona_max_intensity = 1000.0f;
constexpr float corona_max_radius_distance = 10000.0f;
constexpr float corona_max_radius_scale = 1000.0f;
constexpr float corona_max_diminish_distance = 100000.0f;
constexpr float corona_max_volumetric = 1000.0f;

// ─── Live viewport preview ──────────────────────────────────────────────────
// Everything corona_render actually draws with, staged while the dialog is open so the viewport
// tracks the fields. Nothing here reaches a corona object: only IDOK writes.

struct CoronaPreview
{
    bool active = false;
    bool show_in_editor = false;
    uint8_t r = 255, g = 255, b = 255;
    float radius_scale = 1.0f;
    float volumetric_height = 0.0f;
    float volumetric_length = 0.0f;
    int bitmap = -1;
    int volumetric_bitmap = -1;
};
static CoronaPreview g_corona_preview;

// One clamp for both paths: a half typed field must not reach the renderer as a NaN or a negative
// size, and IDOK stores exactly what the preview drew.
static float corona_clamp_field(HWND hdlg, int idc, float min_v, float max_v)
{
    const float v = corona_get_float_field(hdlg, idc);
    if (!std::isfinite(v)) return 0.0f;
    return std::clamp(v, min_v, max_v);
}

// The commit path adds one thing to that clamp: when it actually moves the typed value, the object
// stores something the field on screen does not show, and nothing else in the dialog says so.
static float corona_commit_field(HWND hdlg, int idc, float min_v, float max_v, const char* label)
{
    const float raw = corona_get_float_field(hdlg, idc);
    const float value = corona_clamp_field(hdlg, idc, min_v, max_v);
    if (!(raw == value)) { // a NaN fails this too, which is the point
        xlog::warn("[AlpineCorona] {} {} is outside {}..{}, storing {}", label, raw, min_v, max_v,
                   value);
    }
    return value;
}

static void corona_capture_preview(HWND hdlg)
{
    g_corona_preview.show_in_editor =
        IsDlgButtonChecked(hdlg, IDC_CORONA_SHOW_IN_EDITOR) == BST_CHECKED;
    g_corona_preview.r = static_cast<uint8_t>(
        std::min(GetDlgItemInt(hdlg, IDC_CORONA_COLOR_R, nullptr, FALSE), 255u));
    g_corona_preview.g = static_cast<uint8_t>(
        std::min(GetDlgItemInt(hdlg, IDC_CORONA_COLOR_G, nullptr, FALSE), 255u));
    g_corona_preview.b = static_cast<uint8_t>(
        std::min(GetDlgItemInt(hdlg, IDC_CORONA_COLOR_B, nullptr, FALSE), 255u));
    g_corona_preview.radius_scale =
        corona_clamp_field(hdlg, IDC_CORONA_RADIUS_SCALE, 0.0f, corona_max_radius_scale);
    g_corona_preview.volumetric_height =
        corona_clamp_field(hdlg, IDC_CORONA_VOLUMETRIC_HEIGHT, 0.0f, corona_max_volumetric);
    g_corona_preview.volumetric_length =
        corona_clamp_field(hdlg, IDC_CORONA_VOLUMETRIC_LENGTH, 0.0f, corona_max_volumetric);
}

// Resolved rather than loaded by name, so a half typed name never leaves a placeholder in bm_load's
// cache; an unresolved name draws nothing, as it would in game.
static void corona_capture_preview_bitmaps(HWND hdlg)
{
    char buf[256] = {};
    GetDlgItemTextA(hdlg, IDC_CORONA_BITMAP, buf, sizeof(buf));
    g_corona_preview.bitmap = alpine_dlg_resolve_bitmap(buf);
    GetDlgItemTextA(hdlg, IDC_CORONA_VOLUMETRIC_BITMAP, buf, sizeof(buf));
    g_corona_preview.volumetric_bitmap = alpine_dlg_resolve_bitmap(buf);
}

static void corona_refresh_preview(HWND hdlg)
{
    if (!g_corona_preview.active) return;
    corona_capture_preview(hdlg);
    redraw_all_viewports();
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

        alpine_spinner_init(hdlg, IDC_CORONA_CONE_ANGLE, IDC_CORONA_CONE_ANGLE_SPIN,
                            1.0f, 0.0f, corona_max_cone_angle, 1);
        alpine_spinner_init(hdlg, IDC_CORONA_INTENSITY, IDC_CORONA_INTENSITY_SPIN,
                            0.05f, 0.0f, corona_max_intensity, 2);
        alpine_spinner_init(hdlg, IDC_CORONA_RADIUS_DISTANCE, IDC_CORONA_RADIUS_DISTANCE_SPIN,
                            0.1f, 0.0f, corona_max_radius_distance, 2);
        alpine_spinner_init(hdlg, IDC_CORONA_RADIUS_SCALE, IDC_CORONA_RADIUS_SCALE_SPIN,
                            0.1f, 0.0f, corona_max_radius_scale, 2);
        // Signed, and stock authoring values sit within a tenth of zero, so it steps finer.
        alpine_spinner_init(hdlg, IDC_CORONA_DIMINISH_DISTANCE, IDC_CORONA_DIMINISH_DISTANCE_SPIN,
                            0.05f, -corona_max_diminish_distance, corona_max_diminish_distance, 2);
        alpine_spinner_init(hdlg, IDC_CORONA_VOLUMETRIC_HEIGHT, IDC_CORONA_VOLUMETRIC_HEIGHT_SPIN,
                            0.1f, 0.0f, corona_max_volumetric, 2);
        alpine_spinner_init(hdlg, IDC_CORONA_VOLUMETRIC_LENGTH, IDC_CORONA_VOLUMETRIC_LENGTH_SPIN,
                            0.1f, 0.0f, corona_max_volumetric, 2);

        corona_capture_preview(hdlg);
        corona_capture_preview_bitmaps(hdlg);
        g_corona_preview.active = true;

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
                corona_refresh_preview(hdlg);
            }
            break;
        case IDC_CORONA_RADIUS_SCALE:
        case IDC_CORONA_VOLUMETRIC_HEIGHT:
        case IDC_CORONA_VOLUMETRIC_LENGTH:
            if (HIWORD(wp) == EN_CHANGE) {
                corona_refresh_preview(hdlg);
            }
            break;
        case IDC_CORONA_SHOW_IN_EDITOR:
            if (HIWORD(wp) == BN_CLICKED) {
                corona_refresh_preview(hdlg);
            }
            break;
        case IDC_CORONA_BITMAP:
        case IDC_CORONA_VOLUMETRIC_BITMAP:
            if (HIWORD(wp) == EN_CHANGE && g_corona_preview.active) {
                corona_capture_preview_bitmaps(hdlg);
                corona_refresh_preview(hdlg);
            }
            break;
        case IDC_CORONA_COLOR_CHANGE: {
            COLORREF color = RGB(
                std::min(GetDlgItemInt(hdlg, IDC_CORONA_COLOR_R, nullptr, FALSE), 255u),
                std::min(GetDlgItemInt(hdlg, IDC_CORONA_COLOR_G, nullptr, FALSE), 255u),
                std::min(GetDlgItemInt(hdlg, IDC_CORONA_COLOR_B, nullptr, FALSE), 255u));
            if (alpine_pick_color(hdlg, color, alpine_shared_custom_colors())) {
                SetDlgItemInt(hdlg, IDC_CORONA_COLOR_R, GetRValue(color), FALSE);
                SetDlgItemInt(hdlg, IDC_CORONA_COLOR_G, GetGValue(color), FALSE);
                SetDlgItemInt(hdlg, IDC_CORONA_COLOR_B, GetBValue(color), FALSE);
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

            // Committed through the same ranges the spinners and the live preview already hold
            // themselves to, so a typed extreme no longer reaches the object where an arrow click
            // could not. Diminish distance is the one signed field.
            float cone_angle = corona_commit_field(hdlg, IDC_CORONA_CONE_ANGLE, 0.0f,
                                                   corona_max_cone_angle, "cone angle");
            float intensity = corona_commit_field(hdlg, IDC_CORONA_INTENSITY, 0.0f,
                                                  corona_max_intensity, "intensity");
            float radius_distance =
                corona_commit_field(hdlg, IDC_CORONA_RADIUS_DISTANCE, 0.0f,
                                    corona_max_radius_distance, "radius distance");
            float radius_scale = corona_commit_field(hdlg, IDC_CORONA_RADIUS_SCALE, 0.0f,
                                                     corona_max_radius_scale, "radius scale");
            float diminish_distance =
                corona_commit_field(hdlg, IDC_CORONA_DIMINISH_DISTANCE,
                                    -corona_max_diminish_distance, corona_max_diminish_distance,
                                    "diminish distance");
            float vol_height = corona_commit_field(hdlg, IDC_CORONA_VOLUMETRIC_HEIGHT, 0.0f,
                                                   corona_max_volumetric, "volumetric height");
            float vol_length = corona_commit_field(hdlg, IDC_CORONA_VOLUMETRIC_LENGTH, 0.0f,
                                                   corona_max_volumetric, "volumetric length");

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
    case WM_NOTIFY:
        if (alpine_spinner_handle_notify(hdlg, lp)) return TRUE;
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
        // Objects are only written by IDOK, so a cancelled dialog has nothing to restore.
        g_corona_preview.active = false;
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
    alpine_remove_from_groups(level, static_cast<DedObject*>(corona));
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

    // Selection membership, so a multi-select previews on every corona the dialog will write to.
    const bool preview_all = g_corona_preview.active && !g_selected_coronas.empty();

    for (auto* corona : coronas) {
        if (corona->hidden_in_editor) continue;

        bool selected = is_object_selected(level, corona);

        const bool preview = preview_all &&
            std::find(g_selected_coronas.begin(), g_selected_coronas.end(), corona) !=
                g_selected_coronas.end();

        const uint8_t color_r = preview ? g_corona_preview.r : corona->color_r;
        const uint8_t color_g = preview ? g_corona_preview.g : corona->color_g;
        const uint8_t color_b = preview ? g_corona_preview.b : corona->color_b;
        const float radius_scale = preview ? g_corona_preview.radius_scale : corona->radius_scale;
        const float vol_height = preview ? g_corona_preview.volumetric_height
                                         : corona->volumetric_height;
        const float vol_length = preview ? g_corona_preview.volumetric_length
                                         : corona->volumetric_length;
        const bool show_in_editor = preview ? g_corona_preview.show_in_editor
                                            : corona->show_in_editor;

        if (show_in_editor) {
            // Show corona bitmap with additive blending (no icon)
            const int bm_handle = preview ? g_corona_preview.bitmap
                : corona->corona_bitmap.empty() ? -1
                : bm_load(corona->corona_bitmap.c_str(), -1, 1);
            if (bm_handle >= 0) {
                set_draw_color(color_r, color_g, color_b, 255);
                gr_set_bitmap(bm_handle, -1);
                render_additive_billboard(&corona->pos, radius_scale * 0.5f, cam_param);
            }

            // Show volumetric bitmap as axial billboard along forward vector
            if (vol_length > 0.0f) {
                const int vol_handle = preview ? g_corona_preview.volumetric_bitmap
                    : corona->volumetric_bitmap.empty() ? -1
                    : bm_load(corona->volumetric_bitmap.c_str(), -1, 1);
                if (vol_handle >= 0) {
                    set_draw_color(color_r, color_g, color_b, 128);
                    gr_set_bitmap(vol_handle, -1);
                    render_additive_axial_quad(
                        corona->pos, corona->orient,
                        vol_length, vol_height,
                        cam_param);
                }
            }
        }
        else {
            // Show icon sprite (no corona bitmap). While the dialog is open the staged tint wins
            // over the selection red, which is the only way a color edit is visible at all.
            if (selected && !preview) {
                set_draw_color(0xff, 0x00, 0x00, 0xff);
            }
            else {
                set_draw_color(color_r, color_g, color_b, 0xff);
            }

            if (g_corona_icon_handle >= 0) {
                gr_set_bitmap(g_corona_icon_handle, -1);
            }

            gr_render_billboard(&corona->pos, 0, 0.25f, cam_param);
        }

        // Always draw direction arrow (cyan) along forward vector (min 1m, or volumetric length)
        {
            float len = std::max(vol_length, 1.0f);
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

// ─── Corona creation from mesh tag points ───────────────────────────────────

static DedCorona* create_corona_from_glare(const GlareClassInfo* gi,
                                           const Vector3& world_pos,
                                           const Matrix3& world_orient)
{
    auto* corona = new DedCorona();
    memset(static_cast<DedObject*>(corona), 0, sizeof(DedObject));
    corona->vtbl = reinterpret_cast<void*>(ded_object_vtbl_addr);
    corona->type = DedObjectType::DED_CORONA;
    corona->pos = world_pos;
    corona->orient = world_orient;
    corona->script_name.assign_0("Corona");
    corona->uid = generate_uid();

    corona->color_r = gi->color_r;
    corona->color_g = gi->color_g;
    corona->color_b = gi->color_b;
    corona->color_a = 255;
    corona->corona_bitmap = gi->corona_bitmap;
    corona->cone_angle = gi->cone_angle;
    corona->intensity = gi->intensity;
    corona->radius_distance = gi->radius_distance;
    corona->radius_scale = gi->radius_scale;
    corona->diminish_distance = gi->diminish_distance;
    corona->volumetric_bitmap = gi->volumetric_bitmap;
    corona->volumetric_height = gi->volumetric_height;
    corona->volumetric_length = gi->volumetric_length;

    return corona;
}

std::vector<DedCorona*> corona_create_from_mesh_tags(
    CDedLevel* level, EditorVMesh* vmesh,
    const Vector3& obj_pos, const Matrix3& obj_orient,
    const std::vector<std::string>& glare_names)
{
    std::vector<DedCorona*> result;
    if (!level || !vmesh || glare_names.empty()) return result;

    auto& props = level->GetAlpineLevelProperties();

    // Iterate corona_1, corona_2, ... tag points in the mesh
    for (int i = 0; ; i++) {
        char tag_name[32];
        snprintf(tag_name, sizeof(tag_name), "corona_%d", i + 1);
        int tag_idx = vmesh_find_tag_by_name(vmesh, tag_name);
        if (tag_idx < 0) break;

        // Pick the glare for this tag: per-tag if available, otherwise first entry
        const auto& gname = (i < static_cast<int>(glare_names.size()))
            ? glare_names[i] : glare_names[0];
        auto* gi = glare_tbl_find(gname.c_str());
        if (!gi || gi->corona_bitmap.empty()) continue;

        // Get tag transform and compose with object world transform
        Vector3 tag_pos;
        Matrix3 tag_orient;
        vmesh_get_tag_local_transform(vmesh, &tag_pos, &tag_orient, tag_idx);

        Vector3 world_pos = obj_pos + obj_orient * tag_pos;
        Matrix3 world_orient = obj_orient * tag_orient;

        auto* corona = create_corona_from_glare(gi, world_pos, world_orient);
        props.corona_objects.push_back(corona);
        level->master_objects.add(static_cast<DedObject*>(corona));
        result.push_back(corona);
    }

    return result;
}
