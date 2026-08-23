#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <algorithm>
#include <vector>
#include <xlog/xlog.h>
#include "weather_region.h"
#include "level.h"
#include "resources.h"
#include "vtypes.h"
#include "alpine_obj.h"
#include "textures.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

// ─── Globals ─────────────────────────────────────────────────────────────────

static std::vector<DedWeatherRegion*> g_weather_region_clipboard;
static int g_weather_region_icon_handle = -1;

static void weather_region_load_icon()
{
    if (g_weather_region_icon_handle < 0) {
        g_weather_region_icon_handle = bm_load("Icon_AFWeather.tga", -1, 1);
    }
}

// ─── Cleanup ─────────────────────────────────────────────────────────────────

void DestroyDedWeatherRegion(DedWeatherRegion* weather_region)
{
    if (!weather_region) return;
    weather_region->field_4.free();
    weather_region->script_name.free();
    weather_region->class_name.free();
    delete weather_region;
}

// ─── Serialization ──────────────────────────────────────────────────────────

void weather_region_serialize_chunk(CDedLevel& level, rf::File& file)
{
    auto& regions = level.GetAlpineLevelProperties().weather_region_objects;
    if (regions.empty()) return;

    auto start_pos = level.BeginRflSection(file, alpine_weather_region_chunk_id);

    uint32_t count = static_cast<uint32_t>(regions.size());
    file.write<uint32_t>(count);

    for (auto* region : regions) {
        file.write<int32_t>(region->uid);
        file.write<float>(region->pos.x);
        file.write<float>(region->pos.y);
        file.write<float>(region->pos.z);
        file.write<float>(region->orient.rvec.x);
        file.write<float>(region->orient.rvec.y);
        file.write<float>(region->orient.rvec.z);
        file.write<float>(region->orient.uvec.x);
        file.write<float>(region->orient.uvec.y);
        file.write<float>(region->orient.uvec.z);
        file.write<float>(region->orient.fvec.x);
        file.write<float>(region->orient.fvec.y);
        file.write<float>(region->orient.fvec.z);
        write_rfl_string(file, region->script_name);
        // shape and type
        file.write<int32_t>(static_cast<int32_t>(region->shape));
        file.write<int32_t>(static_cast<int32_t>(region->weather_type));
        // dimensions — both shapes always written so the record stays fixed size
        file.write<float>(region->width);
        file.write<float>(region->height);
        file.write<float>(region->depth);
        file.write<float>(region->radius);
        // shared tuning
        file.write<float>(region->density_scale);
        // rain tuning
        file.write<uint8_t>(region->rain_color_r);
        file.write<uint8_t>(region->rain_color_g);
        file.write<uint8_t>(region->rain_color_b);
        file.write<uint8_t>(region->rain_color_a);
        file.write<float>(region->rain_fall_speed);
        file.write<float>(region->rain_wind_x);
        file.write<float>(region->rain_wind_z);
        file.write<float>(region->rain_streak_seconds);
        // snow tuning
        file.write<uint8_t>(region->snow_color_r);
        file.write<uint8_t>(region->snow_color_g);
        file.write<uint8_t>(region->snow_color_b);
        file.write<uint8_t>(region->snow_color_a);
        file.write<float>(region->snow_fall_speed);
        file.write<float>(region->snow_sway_amplitude);
        file.write<float>(region->snow_sway_speed);
        file.write<float>(region->snow_sprite_radius);
        write_rfl_string(file, region->snow_bitmap);
        // editor-only display flag, then the initial runtime state
        file.write<uint8_t>(region->always_show_range ? 1 : 0);
        file.write<uint8_t>(region->initially_enabled ? 1 : 0);
        // per-region overrides, 0 meaning auto
        file.write<float>(region->active_distance);
        file.write<float>(region->visible_distance);
    }

    level.EndRflSection(file, start_pos);
}

void weather_region_deserialize_chunk(CDedLevel& level, rf::File& file, std::size_t chunk_len)
{
    auto& regions = level.GetAlpineLevelProperties().weather_region_objects;
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
        auto* region = new DedWeatherRegion();
        memset(static_cast<DedObject*>(region), 0, sizeof(DedObject));
        region->vtbl = reinterpret_cast<void*>(ded_object_vtbl_addr);
        region->type = DedObjectType::DED_WEATHER_REGION;

        if (!read_bytes(&region->uid, sizeof(region->uid))) { DestroyDedWeatherRegion(region); return; }
        if (!read_bytes(&region->pos.x, sizeof(float))) { DestroyDedWeatherRegion(region); return; }
        if (!read_bytes(&region->pos.y, sizeof(float))) { DestroyDedWeatherRegion(region); return; }
        if (!read_bytes(&region->pos.z, sizeof(float))) { DestroyDedWeatherRegion(region); return; }
        if (!read_bytes(&region->orient.rvec.x, sizeof(float))) { DestroyDedWeatherRegion(region); return; }
        if (!read_bytes(&region->orient.rvec.y, sizeof(float))) { DestroyDedWeatherRegion(region); return; }
        if (!read_bytes(&region->orient.rvec.z, sizeof(float))) { DestroyDedWeatherRegion(region); return; }
        if (!read_bytes(&region->orient.uvec.x, sizeof(float))) { DestroyDedWeatherRegion(region); return; }
        if (!read_bytes(&region->orient.uvec.y, sizeof(float))) { DestroyDedWeatherRegion(region); return; }
        if (!read_bytes(&region->orient.uvec.z, sizeof(float))) { DestroyDedWeatherRegion(region); return; }
        if (!read_bytes(&region->orient.fvec.x, sizeof(float))) { DestroyDedWeatherRegion(region); return; }
        if (!read_bytes(&region->orient.fvec.y, sizeof(float))) { DestroyDedWeatherRegion(region); return; }
        if (!read_bytes(&region->orient.fvec.z, sizeof(float))) { DestroyDedWeatherRegion(region); return; }

        std::string sname = read_rfl_string(file, remaining);
        region->script_name.assign_0(sname.c_str());

        int32_t shape = 0;
        int32_t weather_type = 0;
        if (!read_bytes(&shape, sizeof(shape))) { DestroyDedWeatherRegion(region); return; }
        if (!read_bytes(&weather_type, sizeof(weather_type))) { DestroyDedWeatherRegion(region); return; }
        region->shape = static_cast<WeatherRegionShape>(shape);
        region->weather_type = static_cast<WeatherRegionType>(weather_type);

        if (!read_bytes(&region->width, sizeof(float))) { DestroyDedWeatherRegion(region); return; }
        if (!read_bytes(&region->height, sizeof(float))) { DestroyDedWeatherRegion(region); return; }
        if (!read_bytes(&region->depth, sizeof(float))) { DestroyDedWeatherRegion(region); return; }
        if (!read_bytes(&region->radius, sizeof(float))) { DestroyDedWeatherRegion(region); return; }

        if (!read_bytes(&region->density_scale, sizeof(float))) { DestroyDedWeatherRegion(region); return; }

        if (!read_bytes(&region->rain_color_r, sizeof(uint8_t))) { DestroyDedWeatherRegion(region); return; }
        if (!read_bytes(&region->rain_color_g, sizeof(uint8_t))) { DestroyDedWeatherRegion(region); return; }
        if (!read_bytes(&region->rain_color_b, sizeof(uint8_t))) { DestroyDedWeatherRegion(region); return; }
        if (!read_bytes(&region->rain_color_a, sizeof(uint8_t))) { DestroyDedWeatherRegion(region); return; }
        if (!read_bytes(&region->rain_fall_speed, sizeof(float))) { DestroyDedWeatherRegion(region); return; }
        if (!read_bytes(&region->rain_wind_x, sizeof(float))) { DestroyDedWeatherRegion(region); return; }
        if (!read_bytes(&region->rain_wind_z, sizeof(float))) { DestroyDedWeatherRegion(region); return; }
        if (!read_bytes(&region->rain_streak_seconds, sizeof(float))) { DestroyDedWeatherRegion(region); return; }

        if (!read_bytes(&region->snow_color_r, sizeof(uint8_t))) { DestroyDedWeatherRegion(region); return; }
        if (!read_bytes(&region->snow_color_g, sizeof(uint8_t))) { DestroyDedWeatherRegion(region); return; }
        if (!read_bytes(&region->snow_color_b, sizeof(uint8_t))) { DestroyDedWeatherRegion(region); return; }
        if (!read_bytes(&region->snow_color_a, sizeof(uint8_t))) { DestroyDedWeatherRegion(region); return; }
        if (!read_bytes(&region->snow_fall_speed, sizeof(float))) { DestroyDedWeatherRegion(region); return; }
        if (!read_bytes(&region->snow_sway_amplitude, sizeof(float))) { DestroyDedWeatherRegion(region); return; }
        if (!read_bytes(&region->snow_sway_speed, sizeof(float))) { DestroyDedWeatherRegion(region); return; }
        if (!read_bytes(&region->snow_sprite_radius, sizeof(float))) { DestroyDedWeatherRegion(region); return; }

        std::string snow_bitmap = read_rfl_string(file, remaining);
        if (!snow_bitmap.empty()) {
            region->snow_bitmap = std::move(snow_bitmap);
        }

        uint8_t always_show_range = 0;
        if (!read_bytes(&always_show_range, sizeof(uint8_t))) { DestroyDedWeatherRegion(region); return; }
        region->always_show_range = always_show_range != 0;
        uint8_t initially_enabled = 1;
        if (!read_bytes(&initially_enabled, sizeof(uint8_t))) { DestroyDedWeatherRegion(region); return; }
        region->initially_enabled = initially_enabled != 0;

        if (!read_bytes(&region->active_distance, sizeof(float))) { DestroyDedWeatherRegion(region); return; }
        if (!read_bytes(&region->visible_distance, sizeof(float))) { DestroyDedWeatherRegion(region); return; }

        regions.push_back(region);
        level.master_objects.add(static_cast<DedObject*>(region));
    }

    xlog::info("[WeatherRegion] Loaded {} weather region objects", regions.size());
}

// ─── Properties Dialog ──────────────────────────────────────────────────────

static std::vector<DedWeatherRegion*> g_selected_weather_regions;

// Bitmap currently shown in the snow bitmap preview, tracked so an edit-box keystroke only
// touches the bitmap manager when the name actually changed.
static std::string g_snow_bitmap_preview_name;
static int g_snow_bitmap_preview_handle = -1;

// Names that aren't on disk or in a vpp stay at -1 rather than going through bm_load, which
// would manufacture (and permanently cache) a placeholder entry for every half-typed name.
// A -1 handle takes the same empty-preview path the stock panel uses when nothing is selected.
static int weather_region_resolve_bitmap(const char* name)
{
    if (!name || name[0] == '\0') return -1;
    if (strlen(name) > rfl_name_max_len) return -1;
    const char* ext = strrchr(name, '.');
    if (ext && strlen(ext) > rfl_ext_max_len) return -1;
    rf::File file;
    if (!file.open(name)) return -1;
    file.close(); // rf::File has no destructor, so the probe leaks the OS handle otherwise
    return bm_load(name, -1, 1);
}

// Mirrors CBitmapPreviewDialog::OnPaint (0x0044C1B0): the editor renderer draws into the
// control's own window, letterboxed so the texture keeps its aspect ratio.
static void weather_region_draw_bitmap_preview(HWND ctrl, const RECT& rc, int bm_handle)
{
    int w = std::min<int>(rc.right - rc.left, gr_get_max_width());
    int h = std::min<int>(rc.bottom - rc.top, gr_get_max_height());
    if (w <= 0 || h <= 0) return;

    gr_set_viewport_wnd(ctrl);

    if (bm_handle < 0) {
        gr_set_clip(0, 0, w, h);
        gr_clear();
        gr_flip();
        return;
    }

    for (int pass = 0; pass < 2; pass++) {
        gr_set_clip(0, 0, w, h);
        gr_clear();

        int src_w = 0, src_h = 0, num_pixels = 0, mip_levels = 0;
        bm_get_mipmap_info(bm_handle, &src_w, &src_h, &num_pixels, &mip_levels);
        if (src_w <= 0 || src_h <= 0) break;

        int dst_x = 0, dst_y = 0, dst_w = w, dst_h = h;
        if (src_h > src_w) {
            dst_w = static_cast<int>(std::lround(static_cast<float>(h) / src_h * src_w));
            dst_x = static_cast<int>(std::lround((w - dst_w) * 0.5f));
        }
        else if (src_w > src_h) {
            dst_h = static_cast<int>(std::lround(static_cast<float>(w) / src_w * src_h));
            dst_y = static_cast<int>(std::lround((h - dst_h) * 0.5f));
        }

        gr_bitmap_scaled(bm_handle, dst_x, dst_y, dst_w, dst_h, 0, 0, src_w, src_h,
                         0.0f, 0.0f, gr_bitmap_preview_mode);
    }

    gr_flip();
}

static void weather_region_update_bitmap_preview(HWND hdlg, bool force)
{
    char buf[256] = {};
    GetDlgItemTextA(hdlg, IDC_WEATHER_SNOW_BITMAP, buf, sizeof(buf));
    if (!force && g_snow_bitmap_preview_name == buf) return;

    g_snow_bitmap_preview_name = buf;
    g_snow_bitmap_preview_handle = weather_region_resolve_bitmap(buf);
    InvalidateRect(GetDlgItem(hdlg, IDC_WEATHER_SNOW_BITMAP_PREVIEW), nullptr, TRUE);
}

static void weather_region_set_float_field(HWND hdlg, int idc, float value)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%.4g", value);
    SetDlgItemTextA(hdlg, idc, buf);
}

static float weather_region_get_float_field(HWND hdlg, int idc)
{
    char buf[32] = {};
    GetDlgItemTextA(hdlg, idc, buf, sizeof(buf));
    return static_cast<float>(atof(buf));
}

static uint8_t weather_region_get_color_field(HWND hdlg, int idc)
{
    return static_cast<uint8_t>(std::min(GetDlgItemInt(hdlg, idc, nullptr, FALSE), 255u));
}

// Stock dimension fields (RED's float spinner, dialog 209) step by 0.02 per arrow click, clamp to
// 0..999 and render with two decimals - matched here so the weather region feels the same.
constexpr float weather_dim_step = 0.02f;
constexpr float weather_dim_min = 0.0f;
constexpr float weather_dim_max = 999.0f;

static void weather_region_set_dim_field(HWND hdlg, int idc, float value)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%0.2f", value);
    SetDlgItemTextA(hdlg, idc, buf);
}

static int weather_region_spin_field(int idc_spin)
{
    switch (idc_spin) {
    case IDC_WEATHER_WIDTH_SPIN: return IDC_WEATHER_WIDTH;
    case IDC_WEATHER_DEPTH_SPIN: return IDC_WEATHER_DEPTH;
    case IDC_WEATHER_HEIGHT_SPIN: return IDC_WEATHER_HEIGHT;
    case IDC_WEATHER_RADIUS_SPIN: return IDC_WEATHER_RADIUS;
    default: return 0;
    }
}

// The spinner never moves itself (UDN_DELTAPOS is refused), so its own range only has to be wide
// enough that both arrows always report a delta. Accel matches stock: 1 unit, 10 while held.
static void weather_region_init_spinner(HWND hdlg, int idc_edit, int idc_spin)
{
    HWND spin = GetDlgItem(hdlg, idc_spin);
    if (!spin) return;
    SendMessage(spin, UDM_SETRANGE32, static_cast<WPARAM>(-0x10000), static_cast<LPARAM>(0x10000));
    SendMessage(spin, UDM_SETPOS32, 0, 0);
    UDACCEL accel[2] = {{0, 1}, {1, 10}};
    SendMessage(spin, UDM_SETACCEL, 2, reinterpret_cast<LPARAM>(accel));
    SendMessage(spin, UDM_SETBUDDY, reinterpret_cast<WPARAM>(GetDlgItem(hdlg, idc_edit)), 0);
}

// Only the fields the selected shape uses stay enabled.
static void weather_region_update_shape_fields(HWND hdlg)
{
    bool is_box = SendDlgItemMessage(hdlg, IDC_WEATHER_SHAPE, CB_GETCURSEL, 0, 0) ==
        static_cast<int>(WeatherRegionShape::box);
    EnableWindow(GetDlgItem(hdlg, IDC_WEATHER_BOX_GROUP), is_box);
    EnableWindow(GetDlgItem(hdlg, IDC_WEATHER_WIDTH), is_box);
    EnableWindow(GetDlgItem(hdlg, IDC_WEATHER_WIDTH_SPIN), is_box);
    EnableWindow(GetDlgItem(hdlg, IDC_WEATHER_HEIGHT), is_box);
    EnableWindow(GetDlgItem(hdlg, IDC_WEATHER_HEIGHT_SPIN), is_box);
    EnableWindow(GetDlgItem(hdlg, IDC_WEATHER_DEPTH), is_box);
    EnableWindow(GetDlgItem(hdlg, IDC_WEATHER_DEPTH_SPIN), is_box);
    EnableWindow(GetDlgItem(hdlg, IDC_WEATHER_SPHERE_GROUP), !is_box);
    EnableWindow(GetDlgItem(hdlg, IDC_WEATHER_RADIUS), !is_box);
    EnableWindow(GetDlgItem(hdlg, IDC_WEATHER_RADIUS_SPIN), !is_box);
}

// Both tuning groups are on screen at once, so everything but the selected type greys out - same
// treatment the Box/Sphere size groups already get from the shape dropdown.
static void weather_region_enable_group(HWND hdlg, bool enable, const int* idcs, int count)
{
    for (int i = 0; i < count; i++) {
        EnableWindow(GetDlgItem(hdlg, idcs[i]), enable);
    }
}

static void weather_region_update_type_fields(HWND hdlg)
{
    static const int rain_idcs[] = {
        IDC_WEATHER_RAIN_GROUP, IDC_WEATHER_RAIN_COLOR_R, IDC_WEATHER_RAIN_COLOR_G,
        IDC_WEATHER_RAIN_COLOR_B, IDC_WEATHER_RAIN_COLOR_A, IDC_WEATHER_RAIN_COLOR_PREVIEW,
        IDC_WEATHER_RAIN_COLOR_CHANGE, IDC_WEATHER_RAIN_FALL_SPEED, IDC_WEATHER_RAIN_WIND_X,
        IDC_WEATHER_RAIN_WIND_Z, IDC_WEATHER_RAIN_STREAK_SECONDS,
    };
    static const int snow_idcs[] = {
        IDC_WEATHER_SNOW_GROUP, IDC_WEATHER_SNOW_COLOR_R, IDC_WEATHER_SNOW_COLOR_G,
        IDC_WEATHER_SNOW_COLOR_B, IDC_WEATHER_SNOW_COLOR_A, IDC_WEATHER_SNOW_COLOR_PREVIEW,
        IDC_WEATHER_SNOW_COLOR_CHANGE, IDC_WEATHER_SNOW_FALL_SPEED, IDC_WEATHER_SNOW_SWAY_AMPLITUDE,
        IDC_WEATHER_SNOW_SWAY_SPEED, IDC_WEATHER_SNOW_SPRITE_RADIUS, IDC_WEATHER_SNOW_BITMAP,
        IDC_WEATHER_SNOW_BITMAP_BROWSE, IDC_WEATHER_SNOW_BITMAP_PREVIEW,
    };

    auto type = static_cast<WeatherRegionType>(
        SendDlgItemMessage(hdlg, IDC_WEATHER_TYPE, CB_GETCURSEL, 0, 0));

    weather_region_enable_group(hdlg, type == WeatherRegionType::rain, rain_idcs,
        static_cast<int>(std::size(rain_idcs)));
    weather_region_enable_group(hdlg, type == WeatherRegionType::snow, snow_idcs,
        static_cast<int>(std::size(snow_idcs)));
}

static void weather_region_pick_color(HWND hdlg, int idc_r, int idc_g, int idc_b)
{
    CHOOSECOLORA cc = {};
    static COLORREF custom_colors[16] = {};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = hdlg;
    cc.rgbResult = RGB(
        GetDlgItemInt(hdlg, idc_r, nullptr, FALSE),
        GetDlgItemInt(hdlg, idc_g, nullptr, FALSE),
        GetDlgItemInt(hdlg, idc_b, nullptr, FALSE));
    cc.lpCustColors = custom_colors;
    cc.Flags = CC_RGBINIT | CC_FULLOPEN;
    if (ChooseColorA(&cc)) {
        SetDlgItemInt(hdlg, idc_r, GetRValue(cc.rgbResult), FALSE);
        SetDlgItemInt(hdlg, idc_g, GetGValue(cc.rgbResult), FALSE);
        SetDlgItemInt(hdlg, idc_b, GetBValue(cc.rgbResult), FALSE);
    }
}

static INT_PTR CALLBACK WeatherRegionDialogProc(HWND hdlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        if (g_selected_weather_regions.empty()) {
            EndDialog(hdlg, IDCANCEL);
            return TRUE;
        }
        auto* region = g_selected_weather_regions[0];

        SetDlgItemTextA(hdlg, IDC_WEATHER_SCRIPT_NAME, region->script_name.c_str());

        SendDlgItemMessage(hdlg, IDC_WEATHER_TYPE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Rain"));
        SendDlgItemMessage(hdlg, IDC_WEATHER_TYPE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Snow"));
        SendDlgItemMessage(hdlg, IDC_WEATHER_TYPE, CB_SETCURSEL,
            static_cast<WPARAM>(region->weather_type), 0);

        SendDlgItemMessage(hdlg, IDC_WEATHER_SHAPE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Box"));
        SendDlgItemMessage(hdlg, IDC_WEATHER_SHAPE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Sphere"));
        SendDlgItemMessage(hdlg, IDC_WEATHER_SHAPE, CB_SETCURSEL,
            static_cast<WPARAM>(region->shape), 0);

        weather_region_set_dim_field(hdlg, IDC_WEATHER_WIDTH, region->width);
        weather_region_set_dim_field(hdlg, IDC_WEATHER_HEIGHT, region->height);
        weather_region_set_dim_field(hdlg, IDC_WEATHER_DEPTH, region->depth);
        weather_region_set_dim_field(hdlg, IDC_WEATHER_RADIUS, region->radius);
        weather_region_init_spinner(hdlg, IDC_WEATHER_WIDTH, IDC_WEATHER_WIDTH_SPIN);
        weather_region_init_spinner(hdlg, IDC_WEATHER_HEIGHT, IDC_WEATHER_HEIGHT_SPIN);
        weather_region_init_spinner(hdlg, IDC_WEATHER_DEPTH, IDC_WEATHER_DEPTH_SPIN);
        weather_region_init_spinner(hdlg, IDC_WEATHER_RADIUS, IDC_WEATHER_RADIUS_SPIN);
        weather_region_set_float_field(hdlg, IDC_WEATHER_DENSITY_SCALE, region->density_scale);
        weather_region_set_float_field(hdlg, IDC_WEATHER_ACTIVE_DISTANCE, region->active_distance);
        weather_region_set_float_field(hdlg, IDC_WEATHER_VISIBLE_DISTANCE, region->visible_distance);

        CheckDlgButton(hdlg, IDC_WEATHER_ALWAYS_SHOW_RANGE,
            region->always_show_range ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hdlg, IDC_WEATHER_INITIALLY_ENABLED,
            region->initially_enabled ? BST_CHECKED : BST_UNCHECKED);

        SetDlgItemInt(hdlg, IDC_WEATHER_RAIN_COLOR_R, region->rain_color_r, FALSE);
        SetDlgItemInt(hdlg, IDC_WEATHER_RAIN_COLOR_G, region->rain_color_g, FALSE);
        SetDlgItemInt(hdlg, IDC_WEATHER_RAIN_COLOR_B, region->rain_color_b, FALSE);
        SetDlgItemInt(hdlg, IDC_WEATHER_RAIN_COLOR_A, region->rain_color_a, FALSE);
        weather_region_set_float_field(hdlg, IDC_WEATHER_RAIN_FALL_SPEED, region->rain_fall_speed);
        weather_region_set_float_field(hdlg, IDC_WEATHER_RAIN_WIND_X, region->rain_wind_x);
        weather_region_set_float_field(hdlg, IDC_WEATHER_RAIN_WIND_Z, region->rain_wind_z);
        weather_region_set_float_field(hdlg, IDC_WEATHER_RAIN_STREAK_SECONDS, region->rain_streak_seconds);

        SetDlgItemInt(hdlg, IDC_WEATHER_SNOW_COLOR_R, region->snow_color_r, FALSE);
        SetDlgItemInt(hdlg, IDC_WEATHER_SNOW_COLOR_G, region->snow_color_g, FALSE);
        SetDlgItemInt(hdlg, IDC_WEATHER_SNOW_COLOR_B, region->snow_color_b, FALSE);
        SetDlgItemInt(hdlg, IDC_WEATHER_SNOW_COLOR_A, region->snow_color_a, FALSE);
        weather_region_set_float_field(hdlg, IDC_WEATHER_SNOW_FALL_SPEED, region->snow_fall_speed);
        weather_region_set_float_field(hdlg, IDC_WEATHER_SNOW_SWAY_AMPLITUDE, region->snow_sway_amplitude);
        weather_region_set_float_field(hdlg, IDC_WEATHER_SNOW_SWAY_SPEED, region->snow_sway_speed);
        weather_region_set_float_field(hdlg, IDC_WEATHER_SNOW_SPRITE_RADIUS, region->snow_sprite_radius);
        SetDlgItemTextA(hdlg, IDC_WEATHER_SNOW_BITMAP, region->snow_bitmap.c_str());

        weather_region_update_shape_fields(hdlg);
        weather_region_update_type_fields(hdlg);
        weather_region_update_bitmap_preview(hdlg, true);

        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_WEATHER_SHAPE:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                weather_region_update_shape_fields(hdlg);
            }
            break;
        case IDC_WEATHER_TYPE:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                weather_region_update_type_fields(hdlg);
            }
            break;
        case IDC_WEATHER_RAIN_COLOR_R:
        case IDC_WEATHER_RAIN_COLOR_G:
        case IDC_WEATHER_RAIN_COLOR_B:
            if (HIWORD(wp) == EN_CHANGE) {
                InvalidateRect(GetDlgItem(hdlg, IDC_WEATHER_RAIN_COLOR_PREVIEW), nullptr, TRUE);
            }
            break;
        case IDC_WEATHER_SNOW_COLOR_R:
        case IDC_WEATHER_SNOW_COLOR_G:
        case IDC_WEATHER_SNOW_COLOR_B:
            if (HIWORD(wp) == EN_CHANGE) {
                InvalidateRect(GetDlgItem(hdlg, IDC_WEATHER_SNOW_COLOR_PREVIEW), nullptr, TRUE);
            }
            break;
        case IDC_WEATHER_RAIN_COLOR_CHANGE:
            weather_region_pick_color(hdlg, IDC_WEATHER_RAIN_COLOR_R, IDC_WEATHER_RAIN_COLOR_G,
                IDC_WEATHER_RAIN_COLOR_B);
            return TRUE;
        case IDC_WEATHER_SNOW_COLOR_CHANGE:
            weather_region_pick_color(hdlg, IDC_WEATHER_SNOW_COLOR_R, IDC_WEATHER_SNOW_COLOR_G,
                IDC_WEATHER_SNOW_COLOR_B);
            return TRUE;
        case IDC_WEATHER_SNOW_BITMAP:
            if (HIWORD(wp) == EN_CHANGE) {
                weather_region_update_bitmap_preview(hdlg, false);
            }
            break;
        case IDC_WEATHER_SNOW_BITMAP_BROWSE: {
            // The browser runs its own modal loop off the main frame, which leaves this dialog
            // clickable; disable it so OK/Cancel can't run underneath it.
            EnableWindow(hdlg, FALSE);
            int picked = texture_browser_pick("Effects", g_snow_bitmap_preview_handle);
            EnableWindow(hdlg, TRUE);
            SetActiveWindow(hdlg);
            if (picked >= 0) {
                const char* name = bm_get_filename(picked);
                SetDlgItemTextA(hdlg, IDC_WEATHER_SNOW_BITMAP, name ? name : "");
                weather_region_update_bitmap_preview(hdlg, true);
            }
            return TRUE;
        }
        case IDOK: {
            char buf[256] = {};
            GetDlgItemTextA(hdlg, IDC_WEATHER_SCRIPT_NAME, buf, sizeof(buf));
            char bmp_buf[256] = {};
            GetDlgItemTextA(hdlg, IDC_WEATHER_SNOW_BITMAP, bmp_buf, sizeof(bmp_buf));

            int type_sel = static_cast<int>(SendDlgItemMessage(hdlg, IDC_WEATHER_TYPE, CB_GETCURSEL, 0, 0));
            int shape_sel = static_cast<int>(SendDlgItemMessage(hdlg, IDC_WEATHER_SHAPE, CB_GETCURSEL, 0, 0));
            if (type_sel < 0) type_sel = 0;
            if (shape_sel < 0) shape_sel = 0;

            float width = weather_region_get_float_field(hdlg, IDC_WEATHER_WIDTH);
            float height = weather_region_get_float_field(hdlg, IDC_WEATHER_HEIGHT);
            float depth = weather_region_get_float_field(hdlg, IDC_WEATHER_DEPTH);
            float radius = weather_region_get_float_field(hdlg, IDC_WEATHER_RADIUS);
            float density_scale = weather_region_get_float_field(hdlg, IDC_WEATHER_DENSITY_SCALE);
            float active_distance = weather_region_get_float_field(hdlg, IDC_WEATHER_ACTIVE_DISTANCE);
            float visible_distance = weather_region_get_float_field(hdlg, IDC_WEATHER_VISIBLE_DISTANCE);
            bool always_show_range = IsDlgButtonChecked(hdlg, IDC_WEATHER_ALWAYS_SHOW_RANGE) == BST_CHECKED;
            bool initially_enabled = IsDlgButtonChecked(hdlg, IDC_WEATHER_INITIALLY_ENABLED) == BST_CHECKED;

            uint8_t rain_r = weather_region_get_color_field(hdlg, IDC_WEATHER_RAIN_COLOR_R);
            uint8_t rain_g = weather_region_get_color_field(hdlg, IDC_WEATHER_RAIN_COLOR_G);
            uint8_t rain_b = weather_region_get_color_field(hdlg, IDC_WEATHER_RAIN_COLOR_B);
            uint8_t rain_a = weather_region_get_color_field(hdlg, IDC_WEATHER_RAIN_COLOR_A);
            float rain_fall_speed = weather_region_get_float_field(hdlg, IDC_WEATHER_RAIN_FALL_SPEED);
            float rain_wind_x = weather_region_get_float_field(hdlg, IDC_WEATHER_RAIN_WIND_X);
            float rain_wind_z = weather_region_get_float_field(hdlg, IDC_WEATHER_RAIN_WIND_Z);
            float rain_streak_seconds = weather_region_get_float_field(hdlg, IDC_WEATHER_RAIN_STREAK_SECONDS);

            uint8_t snow_r = weather_region_get_color_field(hdlg, IDC_WEATHER_SNOW_COLOR_R);
            uint8_t snow_g = weather_region_get_color_field(hdlg, IDC_WEATHER_SNOW_COLOR_G);
            uint8_t snow_b = weather_region_get_color_field(hdlg, IDC_WEATHER_SNOW_COLOR_B);
            uint8_t snow_a = weather_region_get_color_field(hdlg, IDC_WEATHER_SNOW_COLOR_A);
            float snow_fall_speed = weather_region_get_float_field(hdlg, IDC_WEATHER_SNOW_FALL_SPEED);
            float snow_sway_amplitude = weather_region_get_float_field(hdlg, IDC_WEATHER_SNOW_SWAY_AMPLITUDE);
            float snow_sway_speed = weather_region_get_float_field(hdlg, IDC_WEATHER_SNOW_SWAY_SPEED);
            float snow_sprite_radius = weather_region_get_float_field(hdlg, IDC_WEATHER_SNOW_SPRITE_RADIUS);

            // Every other field bulk-applies, but the script name is the region's identity - writing
            // one name over a whole selection would collapse them all into the same object.
            const bool single = g_selected_weather_regions.size() == 1;

            for (auto* w : g_selected_weather_regions) {
                if (single) {
                    w->script_name.assign_0(buf);
                }
                w->weather_type = static_cast<WeatherRegionType>(type_sel);
                w->shape = static_cast<WeatherRegionShape>(shape_sel);
                w->width = width;
                w->height = height;
                w->depth = depth;
                w->radius = radius;
                w->density_scale = density_scale;
                w->active_distance = active_distance;
                w->visible_distance = visible_distance;
                w->always_show_range = always_show_range;
                w->initially_enabled = initially_enabled;
                w->rain_color_r = rain_r;
                w->rain_color_g = rain_g;
                w->rain_color_b = rain_b;
                w->rain_color_a = rain_a;
                w->rain_fall_speed = rain_fall_speed;
                w->rain_wind_x = rain_wind_x;
                w->rain_wind_z = rain_wind_z;
                w->rain_streak_seconds = rain_streak_seconds;
                w->snow_color_r = snow_r;
                w->snow_color_g = snow_g;
                w->snow_color_b = snow_b;
                w->snow_color_a = snow_a;
                w->snow_fall_speed = snow_fall_speed;
                w->snow_sway_amplitude = snow_sway_amplitude;
                w->snow_sway_speed = snow_sway_speed;
                w->snow_sprite_radius = snow_sprite_radius;
                w->snow_bitmap = bmp_buf;
            }
            EndDialog(hdlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(hdlg, IDCANCEL);
            return TRUE;
        }
        break;
    case WM_NOTIFY: {
        auto* nm = reinterpret_cast<NMHDR*>(lp);
        if (!nm || nm->code != UDN_DELTAPOS) break;
        int idc_edit = weather_region_spin_field(static_cast<int>(nm->idFrom));
        if (!idc_edit) break;
        float value = weather_region_get_float_field(hdlg, idc_edit) +
            reinterpret_cast<NMUPDOWN*>(lp)->iDelta * weather_dim_step;
        weather_region_set_dim_field(hdlg, idc_edit, std::clamp(value, weather_dim_min, weather_dim_max));
        SetWindowLongPtr(hdlg, DWLP_MSGRESULT, 1); // refuse the spinner's own position change
        return TRUE;
    }
    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (!dis) break;
        if (dis->CtlID == IDC_WEATHER_SNOW_BITMAP_PREVIEW) {
            weather_region_draw_bitmap_preview(dis->hwndItem, dis->rcItem, g_snow_bitmap_preview_handle);
            return TRUE;
        }
        int idc_r = 0, idc_g = 0, idc_b = 0;
        if (dis->CtlID == IDC_WEATHER_RAIN_COLOR_PREVIEW) {
            idc_r = IDC_WEATHER_RAIN_COLOR_R;
            idc_g = IDC_WEATHER_RAIN_COLOR_G;
            idc_b = IDC_WEATHER_RAIN_COLOR_B;
        }
        else if (dis->CtlID == IDC_WEATHER_SNOW_COLOR_PREVIEW) {
            idc_r = IDC_WEATHER_SNOW_COLOR_R;
            idc_g = IDC_WEATHER_SNOW_COLOR_G;
            idc_b = IDC_WEATHER_SNOW_COLOR_B;
        }
        else {
            break;
        }
        HBRUSH brush = CreateSolidBrush(RGB(
            weather_region_get_color_field(hdlg, idc_r),
            weather_region_get_color_field(hdlg, idc_g),
            weather_region_get_color_field(hdlg, idc_b)));
        FillRect(dis->hDC, &dis->rcItem, brush);
        DeleteObject(brush);
        return TRUE;
    }
    }
    return FALSE;
}

void ShowWeatherRegionPropertiesDialog(CDedLevel* level)
{
    auto& sel = level->selection;
    g_selected_weather_regions.clear();
    for (int i = 0; i < sel.get_size(); i++) {
        DedObject* obj = sel[i];
        if (obj && obj->type == DedObjectType::DED_WEATHER_REGION) {
            g_selected_weather_regions.push_back(static_cast<DedWeatherRegion*>(obj));
        }
    }

    if (!g_selected_weather_regions.empty()) {
        DialogBoxParam(
            reinterpret_cast<HINSTANCE>(&__ImageBase),
            MAKEINTRESOURCE(IDD_ALPINE_WEATHER_REGION_PROPERTIES),
            GetActiveWindow(),
            WeatherRegionDialogProc,
            0
        );
    }

    g_selected_weather_regions.clear();
}

// ─── Object Lifecycle ───────────────────────────────────────────────────────

void PlaceNewWeatherRegionObject()
{
    auto* level = CDedLevel::Get();
    if (!level) return;

    auto* region = new DedWeatherRegion();
    memset(static_cast<DedObject*>(region), 0, sizeof(DedObject));
    region->vtbl = reinterpret_cast<void*>(ded_object_vtbl_addr);
    region->type = DedObjectType::DED_WEATHER_REGION;

    region->script_name.assign_0("Weather Region");

    // Match stock placement (FUN_004431c0): take both position and orientation from the active viewport
    auto* viewport = get_active_viewport();
    if (viewport && viewport->view_data) {
        region->pos = viewport->view_data->camera_pos;
        region->orient = viewport->view_data->camera_orient;
    }
    else {
        region->orient.rvec = {1.0f, 0.0f, 0.0f};
        region->orient.uvec = {0.0f, 1.0f, 0.0f};
        region->orient.fvec = {0.0f, 0.0f, 1.0f};
    }

    region->uid = generate_uid();

    level->GetAlpineLevelProperties().weather_region_objects.push_back(region);
    level->master_objects.add(static_cast<DedObject*>(region));

    level->clear_selection();
    level->add_to_selection(static_cast<DedObject*>(region));
    level->update_console_display();
}

DedWeatherRegion* CloneWeatherRegionObject(DedWeatherRegion* source, bool add_to_level)
{
    if (!source) return nullptr;

    auto* region = new DedWeatherRegion();
    memset(static_cast<DedObject*>(region), 0, sizeof(DedObject));
    region->vtbl = reinterpret_cast<void*>(ded_object_vtbl_addr);
    region->type = DedObjectType::DED_WEATHER_REGION;

    region->pos = source->pos;
    region->orient = source->orient;
    region->script_name.assign_0(source->script_name.c_str());

    region->shape = source->shape;
    region->weather_type = source->weather_type;
    region->width = source->width;
    region->height = source->height;
    region->depth = source->depth;
    region->radius = source->radius;
    region->density_scale = source->density_scale;
    region->active_distance = source->active_distance;
    region->visible_distance = source->visible_distance;
    region->always_show_range = source->always_show_range;
    region->initially_enabled = source->initially_enabled;
    region->rain_color_r = source->rain_color_r;
    region->rain_color_g = source->rain_color_g;
    region->rain_color_b = source->rain_color_b;
    region->rain_color_a = source->rain_color_a;
    region->rain_fall_speed = source->rain_fall_speed;
    region->rain_wind_x = source->rain_wind_x;
    region->rain_wind_z = source->rain_wind_z;
    region->rain_streak_seconds = source->rain_streak_seconds;
    region->snow_color_r = source->snow_color_r;
    region->snow_color_g = source->snow_color_g;
    region->snow_color_b = source->snow_color_b;
    region->snow_color_a = source->snow_color_a;
    region->snow_fall_speed = source->snow_fall_speed;
    region->snow_sway_amplitude = source->snow_sway_amplitude;
    region->snow_sway_speed = source->snow_sway_speed;
    region->snow_sprite_radius = source->snow_sprite_radius;
    region->snow_bitmap = source->snow_bitmap;

    region->uid = generate_uid();

    if (add_to_level) {
        auto* level = CDedLevel::Get();
        if (level) {
            level->GetAlpineLevelProperties().weather_region_objects.push_back(region);
            level->master_objects.add(static_cast<DedObject*>(region));
        }
    }

    return region;
}

void DeleteWeatherRegionObject(DedWeatherRegion* weather_region)
{
    if (!weather_region) return;
    auto* level = CDedLevel::Get();
    if (!level) return;

    auto& regions = level->GetAlpineLevelProperties().weather_region_objects;
    auto it = std::find(regions.begin(), regions.end(), weather_region);
    if (it != regions.end()) {
        regions.erase(it);
    }
    level->master_objects.remove_by_value(static_cast<DedObject*>(weather_region));
    DestroyDedWeatherRegion(weather_region);
}

// ─── Rendering ──────────────────────────────────────────────────────────────

void weather_region_render(CDedLevel* level)
{
    auto& regions = level->GetAlpineLevelProperties().weather_region_objects;
    if (regions.empty()) return;

    weather_region_load_icon();

    const uint32_t mode = editor_line_mode();
    const float cam_param = gr_cam_param;

    for (auto* region : regions) {
        if (region->hidden_in_editor) continue;

        const bool selected = is_object_selected(level, region);

        int r = 0xff, g = 0x00, b = 0x00; // selected always wins
        if (!selected) {
            switch (region->weather_type) {
                case WeatherRegionType::rain:
                    r = 0x40; g = 0xa0; b = 0xff; // blue
                    break;
                case WeatherRegionType::snow:
                    r = 0xc8; g = 0xe6; b = 0xff; // pale blue-white
                    break;
            }
        }

        if (selected || region->always_show_range) {
            set_draw_color(r, g, b, 0xff);

            switch (region->shape) {
                case WeatherRegionShape::box: {
                    Vector3 dims{region->width, region->height, region->depth};
                    draw_wireframe_box_3d(&region->pos, &region->orient, &dims, mode);
                    break;
                }
                case WeatherRegionShape::sphere:
                    // Rotation-invariant, so the stored orientation is ignored.
                    draw_wireframe_sphere_3d(&region->pos, region->radius, mode);
                    break;
            }
        }

        // The icon marks the region regardless of the range setting, like every other object type.
        set_draw_color(r, g, b, 0xff);
        if (g_weather_region_icon_handle >= 0) {
            gr_set_bitmap(g_weather_region_icon_handle, -1);
        }
        gr_render_billboard(&region->pos, 0, 0.25f, cam_param);
    }
}

void weather_region_pick(CDedLevel* level, int param1, int param2)
{
    auto& regions = level->GetAlpineLevelProperties().weather_region_objects;
    for (auto* region : regions) {
        if (region->hidden_in_editor) continue;
        bool hit = level->hit_test_point(param1, param2, &region->pos);
        if (hit) {
            level->select_object(static_cast<DedObject*>(region));
        }
    }
}

DedWeatherRegion* weather_region_click_pick(CDedLevel* level, float click_x, float click_y)
{
    auto& regions = level->GetAlpineLevelProperties().weather_region_objects;
    float best_dist_sq = 1e30f;
    DedWeatherRegion* best_region = nullptr;

    for (auto* region : regions) {
        if (region->hidden_in_editor) continue;

        float center_pos[3] = {region->pos.x, region->pos.y, region->pos.z};
        float screen_cx = 0.0f, screen_cy = 0.0f;
        if (!project_to_screen_2d(center_pos, &screen_cx, &screen_cy))
            continue;

        constexpr float screen_radius_sq = 400.0f; // 20px

        float dx = screen_cx - click_x;
        float dy = screen_cy - click_y;
        float dist_sq = dx * dx + dy * dy;
        if (dist_sq <= screen_radius_sq && dist_sq < best_dist_sq) {
            best_dist_sq = dist_sq;
            best_region = region;
        }
    }

    return best_region;
}

void weather_region_tree_populate(EditorTreeCtrl* tree, int master_groups, CDedLevel* level)
{
    auto& regions = level->GetAlpineLevelProperties().weather_region_objects;

    char buf[64];
    snprintf(buf, sizeof(buf), "Weather Regions (%d)", static_cast<int>(regions.size()));
    int parent = tree->insert_item(buf, master_groups, 0xffff0002);

    for (auto* region : regions) {
        const char* name = region->script_name.c_str();
        if (!name || name[0] == '\0') {
            name = "(unnamed weather region)";
        }
        int child = tree->insert_item(name, parent, 0xffff0002);
        tree->set_item_data(child, region->uid);
    }
}

void weather_region_tree_add_object_type(EditorTreeCtrl* tree)
{
    tree->insert_item("Weather Region", 0xffff0000, 0xffff0002);
}

bool weather_region_copy_object(DedObject* source)
{
    if (!source || source->type != DedObjectType::DED_WEATHER_REGION) return false;
    auto* staged = CloneWeatherRegionObject(static_cast<DedWeatherRegion*>(source), false);
    if (staged) {
        g_weather_region_clipboard.push_back(staged);
        return true;
    }
    return false;
}

void weather_region_paste_objects(CDedLevel* level)
{
    for (auto* staged : g_weather_region_clipboard) {
        auto* clone = CloneWeatherRegionObject(staged, true);
        if (clone) {
            level->add_to_selection(static_cast<DedObject*>(clone));
        }
    }
}

void weather_region_clear_clipboard()
{
    for (auto* region : g_weather_region_clipboard) {
        DestroyDedWeatherRegion(region);
    }
    g_weather_region_clipboard.clear();
}

void weather_region_handle_delete_or_cut(DedObject* obj)
{
    if (!obj || obj->type != DedObjectType::DED_WEATHER_REGION) return;
    auto* level = CDedLevel::Get();
    if (!level) return;

    auto& regions = level->GetAlpineLevelProperties().weather_region_objects;
    auto it = std::find(regions.begin(), regions.end(), static_cast<DedWeatherRegion*>(obj));
    if (it != regions.end()) {
        regions.erase(it);
    }
}

void weather_region_handle_delete_selection(CDedLevel* level)
{
    auto& sel = level->selection;
    for (int i = sel.size - 1; i >= 0; i--) {
        DedObject* obj = sel.data_ptr[i];
        if (obj && obj->type == DedObjectType::DED_WEATHER_REGION) {
            for (int j = i; j < sel.size - 1; j++) {
                sel.data_ptr[j] = sel.data_ptr[j + 1];
            }
            sel.size--;
            DeleteWeatherRegionObject(static_cast<DedWeatherRegion*>(obj));
        }
    }
}

void weather_region_ensure_uid(int& uid)
{
    auto* level = CDedLevel::Get();
    if (!level) return;
    for (auto* w : level->GetAlpineLevelProperties().weather_region_objects) {
        if (w->uid >= uid) uid = w->uid + 1;
    }
}
