#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <format>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>
#include <common/rfl_chunk_reader.h>
#include <xlog/xlog.h>
#include "dir_light.h"
#include "alpine_obj.h"
#include "alpine_spinner.h"
#include "level.h"
#include "resources.h"
#include "vtypes.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace adl = alpine_dir_light;

namespace
{

std::vector<DedDirectionalLight*> g_directional_light_clipboard;
int g_directional_light_icon_handle = -1;

void directional_light_load_icon()
{
    if (g_directional_light_icon_handle < 0) {
        g_directional_light_icon_handle = bm_load("Icon_AFDirLight.tga", -1, 1);
    }
}

adl::Vec3 to_adl(const Vector3& v)
{
    return {v.x, v.y, v.z};
}

Vector3 from_adl(const adl::Vec3& v)
{
    return {v.x, v.y, v.z};
}

DedDirectionalLight* new_directional_light()
{
    auto* light = new DedDirectionalLight();
    memset(static_cast<DedObject*>(light), 0, sizeof(DedObject));
    light->vtbl = reinterpret_cast<void*>(ded_object_vtbl_addr);
    light->type = DedObjectType::DED_DIRECTIONAL_LIGHT;
    return light;
}

// `rec` must already be sanitized.
void directional_light_apply_record(DedDirectionalLight& light, const adl::Record& rec)
{
    light.uid = rec.uid;
    light.pos = from_adl(rec.pos);
    light.orient.rvec = from_adl(rec.rvec);
    light.orient.uvec = from_adl(rec.uvec);
    light.orient.fvec = from_adl(rec.fvec);
    light.color_r = rec.color_r;
    light.color_g = rec.color_g;
    light.color_b = rec.color_b;
    light.intensity = rec.intensity;
    light.initially_on = rec.initially_on != 0;
    light.shape = static_cast<adl::Shape>(rec.shape);
    light.extent_x = rec.extent_x;
    light.extent_y = rec.extent_y;
    light.extent_z = rec.extent_z;
    light.box_yaw = rec.box_yaw;
    light.feather = rec.feather;
    light.spread = rec.spread;
    light.cast_baked_shadows = rec.cast_baked_shadows != 0;
    light.liquid_occludes = rec.liquid_occludes != 0;
    light.sky_passes = rec.sky_passes != 0;
    light.outside_casts = rec.outside_casts != 0;
    light.affects_meshes = rec.affects_meshes != 0;
    light.mesh_mode = static_cast<adl::MeshMode>(rec.mesh_mode);
    light.always_show_range = rec.always_show_range != 0;
}

void directional_light_sanitize(DedDirectionalLight& light)
{
    adl::Record rec = directional_light_record(light);
    adl::sanitize_record(rec);
    directional_light_apply_record(light, rec);
}

// ─── Properties Dialog ──────────────────────────────────────────────────────

std::vector<DedDirectionalLight*> g_selected_directional_lights;

uint8_t g_dlight_color_r = 255;
uint8_t g_dlight_color_g = 255;
uint8_t g_dlight_color_b = 255;

// The dialog's unsaved shape, extents and colour, drawn on its lights while it is open.
struct DirLightPreview
{
    bool active = false;
    adl::Shape shape = adl::Shape::none;
    std::optional<float> extent_x, extent_y, extent_z, box_yaw;
};
DirLightPreview g_dlight_preview;

adl::Shape directional_light_dlg_shape(HWND hdlg)
{
    const auto data = alpine_dlg_combo_data(hdlg, IDC_DLIGHT_SHAPE, static_cast<LRESULT>(adl::Shape::none));
    return data >= 0 && data <= adl::shape_max ? static_cast<adl::Shape>(data) : adl::Shape::none;
}

void directional_light_enable_controls(HWND hdlg, bool enable, std::initializer_list<int> idcs)
{
    for (int id : idcs) {
        EnableWindow(GetDlgItem(hdlg, id), enable);
    }
}

// Only the extents the selected shape reads stay enabled, relabelled for what they mean to it.
void directional_light_update_shape_fields(HWND hdlg)
{
    const adl::Shape shape = directional_light_dlg_shape(hdlg);
    const bool bounded = shape != adl::Shape::none;
    const bool radial = shape == adl::Shape::sphere || shape == adl::Shape::cylinder;

    SetDlgItemTextA(hdlg, IDC_DLIGHT_EXTENT_X_LABEL, radial ? "Radius:" : "Width:");
    SetDlgItemTextA(hdlg, IDC_DLIGHT_EXTENT_Y_LABEL, shape == adl::Shape::cylinder ? "Length:" : "Height:");

    directional_light_enable_controls(hdlg, bounded,
        {IDC_DLIGHT_FEATHER_LABEL, IDC_DLIGHT_FEATHER, IDC_DLIGHT_FEATHER_SPIN, IDC_DLIGHT_EXTENT_X_LABEL,
         IDC_DLIGHT_EXTENT_X, IDC_DLIGHT_EXTENT_X_SPIN, IDC_DLIGHT_OUTSIDE_CASTS});
    directional_light_enable_controls(hdlg, shape == adl::Shape::box || shape == adl::Shape::cylinder,
        {IDC_DLIGHT_EXTENT_Y_LABEL, IDC_DLIGHT_EXTENT_Y, IDC_DLIGHT_EXTENT_Y_SPIN});
    directional_light_enable_controls(hdlg, shape == adl::Shape::box,
        {IDC_DLIGHT_EXTENT_Z_LABEL, IDC_DLIGHT_EXTENT_Z, IDC_DLIGHT_EXTENT_Z_SPIN, IDC_DLIGHT_BOX_YAW_LABEL,
         IDC_DLIGHT_BOX_YAW, IDC_DLIGHT_BOX_YAW_SPIN});
}

void directional_light_update_mesh_fields(HWND hdlg)
{
    EnableWindow(GetDlgItem(hdlg, IDC_DLIGHT_MESH_MODE_SCALE),
                 IsDlgButtonChecked(hdlg, IDC_DLIGHT_AFFECTS_MESHES) == BST_CHECKED);
}

void directional_light_update_color_controls(HWND hdlg)
{
    alpine_dlg_set_color_controls(hdlg, IDC_DLIGHT_COLOR_SWATCH, IDC_DLIGHT_COLOR_VALUE, g_dlight_color_r,
                                  g_dlight_color_g, g_dlight_color_b);
}

void directional_light_capture_preview(HWND hdlg)
{
    g_dlight_preview.shape = directional_light_dlg_shape(hdlg);
    g_dlight_preview.extent_x = alpine_dlg_get_optional_float_field(hdlg, IDC_DLIGHT_EXTENT_X);
    g_dlight_preview.extent_y = alpine_dlg_get_optional_float_field(hdlg, IDC_DLIGHT_EXTENT_Y);
    g_dlight_preview.extent_z = alpine_dlg_get_optional_float_field(hdlg, IDC_DLIGHT_EXTENT_Z);
    g_dlight_preview.box_yaw = alpine_dlg_get_optional_float_field(hdlg, IDC_DLIGHT_BOX_YAW);
}

void directional_light_refresh_preview(HWND hdlg)
{
    if (!g_dlight_preview.active) return;
    directional_light_capture_preview(hdlg);
    redraw_all_viewports();
}

INT_PTR CALLBACK DirectionalLightDialogProc(HWND hdlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        if (g_selected_directional_lights.empty()) {
            EndDialog(hdlg, IDCANCEL);
            return TRUE;
        }
        const DedDirectionalLight* light = g_selected_directional_lights[0];

        SetDlgItemTextA(hdlg, IDC_DLIGHT_SCRIPT_NAME, light->script_name.c_str());
        // The script name is each light's identity, so a multi-select cannot edit it.
        if (g_selected_directional_lights.size() > 1) {
            EnableWindow(GetDlgItem(hdlg, IDC_DLIGHT_SCRIPT_NAME), FALSE);
        }
        CheckDlgButton(hdlg, IDC_DLIGHT_INITIALLY_ON, light->initially_on ? BST_CHECKED : BST_UNCHECKED);

        g_dlight_color_r = light->color_r;
        g_dlight_color_g = light->color_g;
        g_dlight_color_b = light->color_b;
        directional_light_update_color_controls(hdlg);

        alpine_dlg_set_float_field_exact(hdlg, IDC_DLIGHT_INTENSITY, light->intensity);
        alpine_dlg_set_float_field_exact(hdlg, IDC_DLIGHT_SPREAD, light->spread);
        CheckDlgButton(hdlg, IDC_DLIGHT_CAST_SHADOWS, light->cast_baked_shadows ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hdlg, IDC_DLIGHT_LIQUID_OCCLUDES, light->liquid_occludes ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hdlg, IDC_DLIGHT_SKY_PASSES, light->sky_passes ? BST_CHECKED : BST_UNCHECKED);

        alpine_dlg_combo_add(hdlg, IDC_DLIGHT_SHAPE, "None", static_cast<LPARAM>(adl::Shape::none));
        alpine_dlg_combo_add(hdlg, IDC_DLIGHT_SHAPE, "Box", static_cast<LPARAM>(adl::Shape::box));
        alpine_dlg_combo_add(hdlg, IDC_DLIGHT_SHAPE, "Sphere", static_cast<LPARAM>(adl::Shape::sphere));
        alpine_dlg_combo_add(hdlg, IDC_DLIGHT_SHAPE, "Cylinder", static_cast<LPARAM>(adl::Shape::cylinder));
        if (!alpine_dlg_combo_select(hdlg, IDC_DLIGHT_SHAPE, static_cast<LPARAM>(light->shape))) {
            alpine_dlg_combo_select(hdlg, IDC_DLIGHT_SHAPE, static_cast<LPARAM>(adl::Shape::none));
        }
        alpine_dlg_set_float_field_exact(hdlg, IDC_DLIGHT_FEATHER, light->feather);
        alpine_dlg_set_float_field_exact(hdlg, IDC_DLIGHT_EXTENT_X, light->extent_x);
        alpine_dlg_set_float_field_exact(hdlg, IDC_DLIGHT_EXTENT_Y, light->extent_y);
        alpine_dlg_set_float_field_exact(hdlg, IDC_DLIGHT_EXTENT_Z, light->extent_z);
        alpine_dlg_set_float_field_exact(hdlg, IDC_DLIGHT_BOX_YAW, light->box_yaw);
        CheckDlgButton(hdlg, IDC_DLIGHT_OUTSIDE_CASTS, light->outside_casts ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hdlg, IDC_DLIGHT_ALWAYS_SHOW_RANGE, light->always_show_range ? BST_CHECKED : BST_UNCHECKED);

        CheckDlgButton(hdlg, IDC_DLIGHT_AFFECTS_MESHES, light->affects_meshes ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hdlg, IDC_DLIGHT_MESH_MODE_SCALE,
                       light->mesh_mode == adl::MeshMode::lightmap_scaled ? BST_CHECKED : BST_UNCHECKED);

        alpine_spinner_init(hdlg, IDC_DLIGHT_INTENSITY, IDC_DLIGHT_INTENSITY_SPIN, 0.05f, 0.0f,
                            adl::max_intensity, 2);
        alpine_spinner_init(hdlg, IDC_DLIGHT_SPREAD, IDC_DLIGHT_SPREAD_SPIN, 0.5f, 0.0f, adl::max_spread, 1);
        alpine_spinner_init(hdlg, IDC_DLIGHT_FEATHER, IDC_DLIGHT_FEATHER_SPIN, 0.1f, 0.0f, adl::max_feather, 2);
        alpine_spinner_init(hdlg, IDC_DLIGHT_EXTENT_X, IDC_DLIGHT_EXTENT_X_SPIN, 0.1f, 0.0f, adl::max_extent, 2);
        alpine_spinner_init(hdlg, IDC_DLIGHT_EXTENT_Y, IDC_DLIGHT_EXTENT_Y_SPIN, 0.1f, 0.0f, adl::max_extent, 2);
        alpine_spinner_init(hdlg, IDC_DLIGHT_EXTENT_Z, IDC_DLIGHT_EXTENT_Z_SPIN, 0.1f, 0.0f, adl::max_extent, 2);
        alpine_spinner_init(hdlg, IDC_DLIGHT_BOX_YAW, IDC_DLIGHT_BOX_YAW_SPIN, 5.0f, 0.0f, 360.0f, 1);

        directional_light_update_shape_fields(hdlg);
        directional_light_update_mesh_fields(hdlg);

        directional_light_capture_preview(hdlg);
        g_dlight_preview.active = true;
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_DLIGHT_SHAPE:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                directional_light_update_shape_fields(hdlg);
                directional_light_refresh_preview(hdlg);
            }
            break;
        case IDC_DLIGHT_EXTENT_X:
        case IDC_DLIGHT_EXTENT_Y:
        case IDC_DLIGHT_EXTENT_Z:
        case IDC_DLIGHT_BOX_YAW:
            if (HIWORD(wp) == EN_CHANGE) {
                directional_light_refresh_preview(hdlg);
            }
            break;
        case IDC_DLIGHT_AFFECTS_MESHES:
            if (HIWORD(wp) == BN_CLICKED) {
                directional_light_update_mesh_fields(hdlg);
            }
            break;
        case IDC_DLIGHT_COLOR_VALUE:
            if (HIWORD(wp) == EN_KILLFOCUS) {
                alpine_dlg_parse_color_text(hdlg, IDC_DLIGHT_COLOR_VALUE, g_dlight_color_r, g_dlight_color_g,
                                            g_dlight_color_b);
                directional_light_update_color_controls(hdlg);
                redraw_all_viewports();
            }
            break;
        case IDC_DLIGHT_COLOR_CHANGE:
            alpine_dlg_pick_color(hdlg, IDC_DLIGHT_COLOR_SWATCH, IDC_DLIGHT_COLOR_VALUE, g_dlight_color_r,
                                  g_dlight_color_g, g_dlight_color_b);
            redraw_all_viewports();
            return TRUE;
        case IDOK: {
            const bool single = g_selected_directional_lights.size() == 1;
            const DedDirectionalLight* first = g_selected_directional_lights[0];

            char name_buf[256] = {};
            GetDlgItemTextA(hdlg, IDC_DLIGHT_SCRIPT_NAME, name_buf, sizeof(name_buf));

            // The text field is authoritative for RGB: it may not have lost focus yet.
            alpine_dlg_parse_color_text(hdlg, IDC_DLIGHT_COLOR_VALUE, g_dlight_color_r, g_dlight_color_g,
                                        g_dlight_color_b);

            const bool initially_on = IsDlgButtonChecked(hdlg, IDC_DLIGHT_INITIALLY_ON) == BST_CHECKED;
            const float intensity = alpine_dlg_get_float_field_exact(hdlg, IDC_DLIGHT_INTENSITY, first->intensity);
            const float spread = alpine_dlg_get_float_field_exact(hdlg, IDC_DLIGHT_SPREAD, first->spread);
            const bool cast_baked_shadows = IsDlgButtonChecked(hdlg, IDC_DLIGHT_CAST_SHADOWS) == BST_CHECKED;
            const bool liquid_occludes = IsDlgButtonChecked(hdlg, IDC_DLIGHT_LIQUID_OCCLUDES) == BST_CHECKED;
            const bool sky_passes = IsDlgButtonChecked(hdlg, IDC_DLIGHT_SKY_PASSES) == BST_CHECKED;
            const adl::Shape shape = directional_light_dlg_shape(hdlg);
            const float feather = alpine_dlg_get_float_field_exact(hdlg, IDC_DLIGHT_FEATHER, first->feather);
            const float extent_x = alpine_dlg_get_float_field_exact(hdlg, IDC_DLIGHT_EXTENT_X, first->extent_x);
            const float extent_y = alpine_dlg_get_float_field_exact(hdlg, IDC_DLIGHT_EXTENT_Y, first->extent_y);
            const float extent_z = alpine_dlg_get_float_field_exact(hdlg, IDC_DLIGHT_EXTENT_Z, first->extent_z);
            const float box_yaw = alpine_dlg_get_float_field_exact(hdlg, IDC_DLIGHT_BOX_YAW, first->box_yaw);
            const bool outside_casts = IsDlgButtonChecked(hdlg, IDC_DLIGHT_OUTSIDE_CASTS) == BST_CHECKED;
            const bool always_show_range = IsDlgButtonChecked(hdlg, IDC_DLIGHT_ALWAYS_SHOW_RANGE) == BST_CHECKED;
            const bool affects_meshes = IsDlgButtonChecked(hdlg, IDC_DLIGHT_AFFECTS_MESHES) == BST_CHECKED;
            const auto mesh_mode = IsDlgButtonChecked(hdlg, IDC_DLIGHT_MESH_MODE_SCALE) == BST_CHECKED
                                       ? adl::MeshMode::lightmap_scaled
                                       : adl::MeshMode::everywhere;

            for (auto* light : g_selected_directional_lights) {
                if (single) {
                    light->script_name.assign_0(name_buf);
                }
                light->initially_on = initially_on;
                light->color_r = g_dlight_color_r;
                light->color_g = g_dlight_color_g;
                light->color_b = g_dlight_color_b;
                light->intensity = intensity;
                light->spread = spread;
                light->cast_baked_shadows = cast_baked_shadows;
                light->liquid_occludes = liquid_occludes;
                light->sky_passes = sky_passes;
                light->shape = shape;
                light->feather = feather;
                light->extent_x = extent_x;
                light->extent_y = extent_y;
                light->extent_z = extent_z;
                light->box_yaw = box_yaw;
                light->outside_casts = outside_casts;
                light->always_show_range = always_show_range;
                light->affects_meshes = affects_meshes;
                light->mesh_mode = mesh_mode;
                directional_light_sanitize(*light);
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

// ─── Rendering ──────────────────────────────────────────────────────────────

constexpr float directional_light_icon_size = 0.25f;
constexpr float directional_light_arrow_length = 2.0f;

void draw_arrow_along(const Vector3& p, const Vector3& f, float length, int r, int g, int b)
{
    draw_3d_arrow(p.x, p.y, p.z, p.x + f.x * length, p.y + f.y * length, p.z + f.z * length, r, g, b);
}

void directional_light_draw_volume(const DedDirectionalLight& light, bool previewing, uint32_t mode)
{
    adl::Shape shape = light.shape;
    float extent_x = light.extent_x, extent_y = light.extent_y, extent_z = light.extent_z;
    float box_yaw = light.box_yaw;
    if (previewing) {
        shape = g_dlight_preview.shape;
        extent_x = adl::clamp_finite(g_dlight_preview.extent_x.value_or(extent_x), 0.0f, adl::max_extent, extent_x);
        extent_y = adl::clamp_finite(g_dlight_preview.extent_y.value_or(extent_y), 0.0f, adl::max_extent, extent_y);
        extent_z = adl::clamp_finite(g_dlight_preview.extent_z.value_or(extent_z), 0.0f, adl::max_extent, extent_z);
        box_yaw = adl::normalize_degrees(g_dlight_preview.box_yaw.value_or(box_yaw));
    }

    switch (shape) {
    case adl::Shape::box: {
        adl::Vec3 right, forward;
        adl::box_yaw_axes(box_yaw, right, forward);
        const Matrix3 orient{from_adl(right), {0.0f, 1.0f, 0.0f}, from_adl(forward)};
        const Vector3 dims{extent_x, extent_y, extent_z};
        draw_wireframe_box_3d(&light.pos, &orient, &dims, mode);
        break;
    }
    case adl::Shape::sphere:
        draw_wireframe_sphere_3d(&light.pos, extent_x, mode);
        break;
    case adl::Shape::cylinder:
        draw_wireframe_cylinder_3d(light.pos, light.orient, extent_x, extent_y, mode);
        break;
    case adl::Shape::none:
        break;
    }
}

// ─── Sun Arrow ──────────────────────────────────────────────────────────────

// Never freed: undo records of a rotation keep a raw pointer to it.
DedSunArrow* g_sun_arrow = nullptr;
bool g_sun_arrow_active = false;
// fvec as last derived from the level properties; any other value is the mapper's rotation (or an
// undo of one).
Vector3 g_sun_arrow_synced_fvec;

constexpr float sun_arrow_height = 5.0f;
constexpr float sun_arrow_length = 3.0f;
constexpr float sun_arrow_icon_size = 0.35f;
constexpr int sun_arrow_r = 0xff, sun_arrow_g = 0xc8, sun_arrow_b = 0x20;

DedSunArrow* sun_arrow_object()
{
    if (!g_sun_arrow) {
        g_sun_arrow = new DedSunArrow();
        memset(static_cast<DedObject*>(g_sun_arrow), 0, sizeof(DedObject));
        g_sun_arrow->vtbl = reinterpret_cast<void*>(ded_object_vtbl_addr);
        g_sun_arrow->type = DedObjectType::DED_SUN_ARROW;
        g_sun_arrow->uid = -1;
        g_sun_arrow->script_name.assign_0("Sun");
        g_sun_arrow->orient = identity_orient;
    }
    return g_sun_arrow;
}

// Roll-free: right stays horizontal, taken from the yaw alone so it is defined at the zenith too.
Matrix3 sun_arrow_orient(float sun_yaw, float sun_pitch)
{
    const alpine_lighting::Direction to_sun = alpine_lighting::sun_to_light_dir(sun_yaw, sun_pitch);
    const Vector3 f{-to_sun.x, -to_sun.y, -to_sun.z};
    const alpine_lighting::Direction rd = alpine_lighting::sun_to_light_dir(sun_yaw - 90.0f, 0.0f);
    const Vector3 r{rd.x, rd.y, rd.z};
    return {r, from_adl(adl::cross(to_adl(f), to_adl(r))), f};
}

// Writes the arrow's travel direction back into sun_yaw / sun_pitch. False when that changes nothing:
// the angles already match, or the direction cannot be one (non-finite or zero) and the caller simply
// overwrites it.
bool sun_arrow_write_props(AlpineLevelProperties& props, const Vector3& fvec)
{
    float yaw = 0.0f, pitch = 0.0f;
    if (!alpine_light_dir_to_sun_angles(fvec, yaw, pitch) || (pitch == props.sun_pitch && yaw == props.sun_yaw)) {
        return false;
    }
    props.sun_pitch = pitch;
    props.sun_yaw = yaw;
    return true;
}

} // namespace

// ─── Cleanup ─────────────────────────────────────────────────────────────────

void DestroyDedDirectionalLight(DedDirectionalLight* light)
{
    if (!light) return;
    light->class_mesh_filename.free();
    light->script_name.free();
    light->class_name.free();
    delete light;
}

// ─── Record ──────────────────────────────────────────────────────────────────

adl::Record directional_light_record(const DedDirectionalLight& light)
{
    adl::Record rec;
    rec.uid = light.uid;
    rec.pos = to_adl(light.pos);
    rec.rvec = to_adl(light.orient.rvec);
    rec.uvec = to_adl(light.orient.uvec);
    rec.fvec = to_adl(light.orient.fvec);
    rec.color_r = light.color_r;
    rec.color_g = light.color_g;
    rec.color_b = light.color_b;
    rec.intensity = light.intensity;
    rec.initially_on = light.initially_on ? 1 : 0;
    rec.shape = static_cast<uint8_t>(light.shape);
    rec.extent_x = light.extent_x;
    rec.extent_y = light.extent_y;
    rec.extent_z = light.extent_z;
    rec.box_yaw = light.box_yaw;
    rec.feather = light.feather;
    rec.spread = light.spread;
    rec.cast_baked_shadows = light.cast_baked_shadows ? 1 : 0;
    rec.liquid_occludes = light.liquid_occludes ? 1 : 0;
    rec.sky_passes = light.sky_passes ? 1 : 0;
    rec.outside_casts = light.outside_casts ? 1 : 0;
    rec.affects_meshes = light.affects_meshes ? 1 : 0;
    rec.mesh_mode = static_cast<uint8_t>(light.mesh_mode);
    rec.always_show_range = light.always_show_range ? 1 : 0;
    return rec;
}

// ─── Serialization ──────────────────────────────────────────────────────────

void directional_light_serialize_chunk(CDedLevel& level, rf::File& file)
{
    auto& lights = level.GetAlpineLevelProperties().directional_light_objects;
    if (lights.empty()) return;

    auto start_pos = level.BeginRflSection(file, alpine_directional_light_chunk_id);

    const auto count = static_cast<uint32_t>(std::min<std::size_t>(lights.size(), adl::max_lights));
    if (lights.size() > count) {
        editor_report(EditorReportLevel::warn, "DirLight",
                      std::format("Only the first {} of {} directional lights were saved.", count, lights.size()),
                      true);
    }
    file.write<uint32_t>(count);

    for (uint32_t i = 0; i < count; i++) {
        const DedDirectionalLight& light = *lights[i];
        const adl::Record rec = directional_light_record(light);
        file.write<int32_t>(rec.uid);
        for (const adl::Vec3* v : {&rec.pos, &rec.rvec, &rec.uvec, &rec.fvec}) {
            file.write<float>(v->x);
            file.write<float>(v->y);
            file.write<float>(v->z);
        }
        write_rfl_string(file, light.script_name);
        file.write<uint8_t>(rec.color_r);
        file.write<uint8_t>(rec.color_g);
        file.write<uint8_t>(rec.color_b);
        file.write<uint8_t>(rec.color_a);
        file.write<float>(rec.intensity);
        file.write<uint8_t>(rec.initially_on);
        file.write<uint8_t>(rec.shape);
        file.write<float>(rec.extent_x);
        file.write<float>(rec.extent_y);
        file.write<float>(rec.extent_z);
        file.write<float>(rec.box_yaw);
        file.write<float>(rec.feather);
        file.write<float>(rec.spread);
        file.write<uint8_t>(rec.cast_baked_shadows);
        file.write<uint8_t>(rec.liquid_occludes);
        file.write<uint8_t>(rec.sky_passes);
        file.write<uint8_t>(rec.outside_casts);
        file.write<uint8_t>(rec.affects_meshes);
        file.write<uint8_t>(rec.mesh_mode);
        file.write<uint8_t>(rec.always_show_range);
    }

    level.EndRflSection(file, start_pos);
}

void directional_light_deserialize_chunk(CDedLevel& level, rf::File& file, std::size_t chunk_len,
                                         [[maybe_unused]] int content_version)
{
    auto& lights = level.GetAlpineLevelProperties().directional_light_objects;
    std::size_t remaining = chunk_len;

    rf::File::ChunkGuard chunk_guard{file, remaining};
    RflChunkReader<rf::File> reader{file, remaining};

    uint32_t count = 0;
    if (!reader.read(count)) return;
    count = std::min(count, adl::max_lights);

    int corrected = 0;
    uint32_t loaded = 0;
    for (uint32_t i = 0; i < count; i++) {
        adl::Record rec;
        std::string sname;
        if (!adl::read_record(reader, rec, sname)) {
            break;
        }
        if (adl::sanitize_record(rec)) {
            corrected++;
        }

        auto* light = new_directional_light();
        directional_light_apply_record(*light, rec);
        light->script_name.assign_0(sname.c_str());

        lights.push_back(light);
        level.master_objects.add(static_cast<DedObject*>(light));
        loaded++;
    }

    if (corrected) {
        xlog::warn("[DirLight] {} directional light(s) had out of range properties corrected", corrected);
    }
    xlog::info("[DirLight] Loaded {} directional light object(s)", loaded);
}

// ─── Properties Dialog ──────────────────────────────────────────────────────

void ShowDirectionalLightPropertiesDialog(CDedLevel* level)
{
    auto& sel = level->selection;
    g_selected_directional_lights.clear();
    for (int i = 0; i < sel.get_size(); i++) {
        DedObject* obj = sel[i];
        if (obj && obj->type == DedObjectType::DED_DIRECTIONAL_LIGHT) {
            g_selected_directional_lights.push_back(static_cast<DedDirectionalLight*>(obj));
        }
    }

    if (!g_selected_directional_lights.empty()) {
        DialogBoxParam(reinterpret_cast<HINSTANCE>(&__ImageBase),
                       MAKEINTRESOURCE(IDD_ALPINE_DIR_LIGHT_PROPERTIES), GetActiveWindow(),
                       DirectionalLightDialogProc, 0);
        g_dlight_preview.active = false;
    }

    g_selected_directional_lights.clear();
}

// ─── Object Lifecycle ───────────────────────────────────────────────────────

void PlaceNewDirectionalLightObject()
{
    auto* level = CDedLevel::Get();
    if (!level) return;

    auto* light = new_directional_light();
    light->script_name.assign_0("Directional Light");

    auto* viewport = get_active_viewport();
    if (viewport && viewport->view_data) {
        light->pos = viewport->view_data->camera_pos;
        light->orient = viewport->view_data->camera_orient;
    }
    directional_light_sanitize(*light);

    light->uid = generate_uid();

    level->GetAlpineLevelProperties().directional_light_objects.push_back(light);
    level->master_objects.add(static_cast<DedObject*>(light));

    level->clear_selection();
    level->add_to_selection(static_cast<DedObject*>(light));
    level->update_console_display();
}

DedDirectionalLight* CloneDirectionalLightObject(DedDirectionalLight* source, bool add_to_level)
{
    if (!source) return nullptr;

    auto* light = new_directional_light();
    adl::Record rec = directional_light_record(*source);
    adl::sanitize_record(rec);
    directional_light_apply_record(*light, rec);
    light->script_name.assign_0(source->script_name.c_str());

    light->uid = generate_uid();

    if (add_to_level) {
        auto* level = CDedLevel::Get();
        if (level) {
            level->GetAlpineLevelProperties().directional_light_objects.push_back(light);
            level->master_objects.add(static_cast<DedObject*>(light));
        }
    }

    return light;
}

// ─── Rendering ──────────────────────────────────────────────────────────────

void directional_light_render(CDedLevel* level)
{
    auto& lights = level->GetAlpineLevelProperties().directional_light_objects;
    if (lights.empty()) return;

    directional_light_load_icon();

    const uint32_t mode = editor_line_mode();
    const float cam_param = gr_cam_param;
    const auto& previewed = g_selected_directional_lights;

    for (auto* light : lights) {
        if (light->hidden_in_editor) continue;

        const bool selected = is_object_selected(level, light);
        int r = 0xff, g = 0x00, b = 0x00;
        if (!selected) {
            if (light->initially_on) {
                g = 0xe0;
                b = 0x60;
            }
            else {
                r = g = b = 0x90;
            }
        }

        const bool previewing = g_dlight_preview.active
                                && std::find(previewed.begin(), previewed.end(), light) != previewed.end();

        draw_arrow_along(light->pos, light->orient.fvec, directional_light_arrow_length, r, g, b);

        if (selected || light->always_show_range) {
            set_draw_color(r, g, b, 0xff);
            directional_light_draw_volume(*light, previewing, mode);
        }

        if (selected && !previewing) {
            set_draw_color(0xff, 0x00, 0x00, 0xff);
        }
        else {
            int cr = previewing ? g_dlight_color_r : light->color_r;
            int cg = previewing ? g_dlight_color_g : light->color_g;
            int cb = previewing ? g_dlight_color_b : light->color_b;
            if (!light->initially_on) {
                cr /= 2;
                cg /= 2;
                cb /= 2;
            }
            set_draw_color(cr, cg, cb, 0xff);
        }
        if (g_directional_light_icon_handle >= 0) {
            gr_set_bitmap(g_directional_light_icon_handle, -1);
        }
        gr_render_billboard(&light->pos, 0, directional_light_icon_size, cam_param);
    }
}

void directional_light_pick(CDedLevel* level, int param1, int param2)
{
    for (auto* light : level->GetAlpineLevelProperties().directional_light_objects) {
        if (light->hidden_in_editor) continue;
        if (level->hit_test_point(param1, param2, &light->pos)) {
            level->select_object(static_cast<DedObject*>(light));
        }
    }
}

DedDirectionalLight* directional_light_click_pick(CDedLevel* level, float click_x, float click_y)
{
    return alpine_click_pick_point(level->GetAlpineLevelProperties().directional_light_objects, click_x, click_y,
                                   alpine_click_pick_radius_sq);
}

void directional_light_tree_populate(EditorTreeCtrl* tree, int master_groups, CDedLevel* level)
{
    auto& lights = level->GetAlpineLevelProperties().directional_light_objects;

    char buf[64];
    snprintf(buf, sizeof(buf), "Directional Lights (%d)", static_cast<int>(lights.size()));
    int parent = tree->insert_item(buf, master_groups, 0xffff0002);

    for (auto* light : lights) {
        const char* name = light->script_name.c_str();
        if (!name || name[0] == '\0') {
            name = "(unnamed directional light)";
        }
        int child = tree->insert_item(name, parent, 0xffff0002);
        tree->set_item_data(child, light->uid);
    }
}

void directional_light_tree_add_object_type(EditorTreeCtrl* tree)
{
    tree->insert_item("Directional Light", 0xffff0000, 0xffff0002);
}

bool directional_light_copy_object(DedObject* source)
{
    if (!source || source->type != DedObjectType::DED_DIRECTIONAL_LIGHT) return false;
    auto* staged = CloneDirectionalLightObject(static_cast<DedDirectionalLight*>(source), false);
    if (staged) {
        g_directional_light_clipboard.push_back(staged);
        return true;
    }
    return false;
}

void directional_light_paste_objects(CDedLevel* level)
{
    for (auto* staged : g_directional_light_clipboard) {
        auto* clone = CloneDirectionalLightObject(staged, true);
        if (clone) {
            level->add_to_selection(static_cast<DedObject*>(clone));
        }
    }
}

void directional_light_swap_clipboard(std::vector<DedDirectionalLight*>& other)
{
    g_directional_light_clipboard.swap(other);
}

void directional_light_clear_clipboard()
{
    for (auto* light : g_directional_light_clipboard) {
        DestroyDedDirectionalLight(light);
    }
    g_directional_light_clipboard.clear();
}

void directional_light_handle_delete_or_cut(DedObject* obj)
{
    if (!obj || obj->type != DedObjectType::DED_DIRECTIONAL_LIGHT) return;
    auto* level = CDedLevel::Get();
    if (!level) return;

    auto& lights = level->GetAlpineLevelProperties().directional_light_objects;
    auto it = std::find(lights.begin(), lights.end(), static_cast<DedDirectionalLight*>(obj));
    if (it != lights.end()) {
        lights.erase(it);
    }
}

void directional_light_ensure_uid(int& uid)
{
    auto* level = CDedLevel::Get();
    if (!level) return;
    alpine_ensure_uid(level->GetAlpineLevelProperties().directional_light_objects, uid);
}

// ─── Sun Arrow ──────────────────────────────────────────────────────────────

void sun_arrow_remove_from_selection(CDedLevel* level)
{
    if (!level || !g_sun_arrow) return;
    level->selection.remove_by_value(static_cast<DedObject*>(g_sun_arrow));
}

void sun_arrow_detach(CDedLevel* level)
{
    sun_arrow_remove_from_selection(level);
    g_sun_arrow_active = false;
}

void sun_arrow_sync(CDedLevel* level)
{
    if (!level) return;
    auto& props = level->GetAlpineLevelProperties();
    if (!props.enable_sun) {
        if (g_sun_arrow_active) {
            sun_arrow_detach(level);
        }
        return;
    }

    DedSunArrow* arrow = sun_arrow_object();
    if (g_sun_arrow_active) {
        const Vector3& f = arrow->orient.fvec;
        const Vector3& s = g_sun_arrow_synced_fvec;
        if ((f.x != s.x || f.y != s.y || f.z != s.z) && sun_arrow_write_props(props, f)) {
            mark_level_modified();
        }
    }
    g_sun_arrow_active = true;

    arrow->orient = sun_arrow_orient(props.sun_yaw, props.sun_pitch);
    g_sun_arrow_synced_fvec = arrow->orient.fvec;

    Vector3 base = level->player_start_pos;
    if (!adl::is_finite(to_adl(base))) {
        base = {0.0f, 0.0f, 0.0f};
    }
    arrow->pos = {base.x, base.y + sun_arrow_height, base.z};
    arrow->hidden_in_editor = false;
}

void sun_arrow_render(CDedLevel* level)
{
    if (!g_sun_arrow_active || !g_sun_arrow) return;

    directional_light_load_icon();

    const bool selected = is_object_selected(level, g_sun_arrow);
    const int r = sun_arrow_r;
    const int g = selected ? 0x00 : sun_arrow_g;
    const int b = selected ? 0x00 : sun_arrow_b;

    draw_arrow_along(g_sun_arrow->pos, g_sun_arrow->orient.fvec, sun_arrow_length, r, g, b);

    if (selected) {
        set_draw_color(0xff, 0x00, 0x00, 0xff);
    }
    else {
        const auto& props = level->GetAlpineLevelProperties();
        set_draw_color(props.sun_color_r, props.sun_color_g, props.sun_color_b, 0xff);
    }
    if (g_directional_light_icon_handle >= 0) {
        gr_set_bitmap(g_directional_light_icon_handle, -1);
    }
    gr_render_billboard(&g_sun_arrow->pos, 0, sun_arrow_icon_size, gr_cam_param);
}

SunArrowState sun_arrow_state(CDedLevel* level)
{
    SunArrowState state;
    if (level) {
        const auto& props = level->GetAlpineLevelProperties();
        state.sun_yaw = props.sun_yaw;
        state.sun_pitch = props.sun_pitch;
    }
    state.synced_fvec = g_sun_arrow_synced_fvec;
    return state;
}

SunArrowState sun_arrow_state_toward(CDedLevel* level, const Vector3& fvec)
{
    SunArrowState state = sun_arrow_state(level);
    float yaw = 0.0f, pitch = 0.0f;
    if (alpine_light_dir_to_sun_angles(fvec, yaw, pitch)) {
        state.sun_yaw = yaw;
        state.sun_pitch = pitch;
    }
    state.synced_fvec = sun_arrow_orient(state.sun_yaw, state.sun_pitch).fvec;
    return state;
}

void sun_arrow_restore(CDedLevel* level, const SunArrowState& state)
{
    if (!level) return;
    auto& props = level->GetAlpineLevelProperties();
    props.sun_yaw = state.sun_yaw;
    props.sun_pitch = state.sun_pitch;
    if (g_sun_arrow) {
        g_sun_arrow->orient = sun_arrow_orient(state.sun_yaw, state.sun_pitch);
    }
    g_sun_arrow_synced_fvec = state.synced_fvec;
}

DedSunArrow* sun_arrow_click_pick(float click_x, float click_y)
{
    if (!g_sun_arrow_active || !g_sun_arrow) return nullptr;
    return alpine_click_pick_point(&g_sun_arrow, &g_sun_arrow + 1, click_x, click_y, alpine_click_pick_radius_sq);
}
