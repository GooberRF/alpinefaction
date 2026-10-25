#include <windows.h>
#include <commctrl.h>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <format>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <common/utils/os-utils.h>
#include <common/utils/string-utils.h>
#include <common/version/version.h>
#include <patch_common/FunHook.h>
#include <xlog/xlog.h>
#include "alpine_color_picker.h"
#include "alpine_obj.h"
#include "command_line.h"
#include "editor_settings.h"
#include "level.h"
#include "mfc_types.h"
#include "resources.h"
#include "textures.h"
#include "vtypes.h"

EditorSettings g_editor_settings;

namespace
{

constexpr const char* alpine_tab_name = "Alpine";

enum class SettingKind
{
    int_flag,
    int_value,
    long_value,
    int_spinner,
    float_spinner,
    texture,
    color,
    flag,
    colorref,
    renderer,
};

struct Setting
{
    const char* section;
    std::string key;
    // empty for a setting red.cfg never had
    std::string red_cfg_key;
    SettingKind kind;
    void* target;
};

using SettingValues = std::unordered_map<std::string, std::string>;

std::vector<std::string> g_orphaned_settings;
// The ini is there but could not be read, so saving would replace it with defaults
bool g_settings_unreadable = false;
EditorSettings g_settings_before_dialog;

std::vector<Setting> settings_of(PreferencesDialog& dlg)
{
    GeneralPrefsPage& general = *dlg.general;
    TextureManager& textures = *dlg.texture_manager;
    EditorColorPrefs& visual = *dlg.visual;
    EditorSettings& alpine = g_editor_settings;
    std::vector<Setting> settings{
        {"General", "AutosaveMinutes", "Autosave Frequency (in minutes)", SettingKind::int_spinner,
         general.autosave_minutes},
        {"General", "DefaultNavPointHeight", "Default Nav Point Height", SettingKind::float_spinner,
         general.nav_point_height},
        {"General", "PromptForSave", "Prompt for Save", SettingKind::int_flag, &general.prompt_for_save},
        {"General", "GridEnabled", "Grid Enabled", SettingKind::int_flag, &general.grid_enabled},
        {"Textures", "DefaultCeilingTexture", "Ceiling Texture", SettingKind::texture,
         textures.default_ceiling_texture},
        {"Textures", "DefaultWallTexture", "Wall Texture", SettingKind::texture, textures.default_wall_texture},
        {"Textures", "DefaultFloorTexture", "Floor Texture", SettingKind::texture, textures.default_floor_texture},
        {"Textures", "PixelsPerMeter", "Pixels Per Meter", SettingKind::int_spinner, textures.pixels_per_meter},
        {"Visual", "FarClipDistance", "Far Clip Distance", SettingKind::float_spinner, visual.far_clip},
    };

    constexpr const char* color_names[] = {
        "Background", "Grid", "Cookie Cutter", "Brush", "Locked Brush", "Detail Brush", "Portal Brush", "Mover Brush",
        "Link", "Keyframe Link", "Node", "Bounding Box", "Trigger", "Geo Region", "Climb Region", "Push Region",
    };
    static_assert(std::size(color_names) == static_cast<std::size_t>(EditorColorPrefIndex::count));
    for (std::size_t i = 0; i < std::size(color_names); ++i) {
        const std::string name = std::string{color_names[i]} + " Color";
        settings.push_back({"Visual", string_replace(name, " ", ""), name, SettingKind::color, &visual.colors[i]});
    }

    // red.cfg names the sides "<name> Left" and so on
    auto add_rect = [&](const char* section, std::string_view name, bool in_red_cfg, RECT& rect) {
        const std::pair<const char*, LONG*> sides[] = {
            {"Left", &rect.left}, {"Top", &rect.top}, {"Right", &rect.right}, {"Bottom", &rect.bottom},
        };
        for (const auto& [side, value] : sides) {
            const std::string red_cfg_key = std::format("{} {}", name, side);
            settings.push_back({section, string_replace(red_cfg_key, " ", ""), in_red_cfg ? red_cfg_key : "",
                                SettingKind::long_value, value});
        }
    };
    add_rect("Dialogs", "Cutscene Dialog", true, dlg.cutscene_dialog_rect);
    add_rect("Dialogs", "Brush Clip Dialog", true, dlg.brush_clip_dialog_rect);

    settings.insert(settings.end(), {
        {"Alpine", "Renderer", "", SettingKind::renderer, &alpine.renderer},
        {"Alpine", "TextureHoverOutline", "", SettingKind::flag, &alpine.texture_hover_outline},
        {"Alpine", "TextureHoverOutlineColor", "", SettingKind::colorref, &alpine.texture_hover_outline_color},
        {"Alpine", "BackgroundThrottle", "", SettingKind::flag, &alpine.background_throttle},
        {"Alpine", "ForceLOD0", "", SettingKind::flag, &alpine.force_lod0},
        {"Alpine", "PreviewLevelFog", "", SettingKind::flag, &alpine.preview_level_fog},
        {"Alpine", "ShowInvisibleFaces", "", SettingKind::flag, &alpine.show_invisible_faces},
        {"Alpine", "ShowBackFaces", "", SettingKind::flag, &alpine.show_back_faces},
        {"Alpine", "FullbrightMeshes", "", SettingKind::flag, &alpine.fullbright_meshes},
        {"Alpine", "ShowDecals", "", SettingKind::flag, &alpine.show_decals},
        {"Alpine", "ShowWireframe", "", SettingKind::flag, &alpine.show_wireframe},
        {"Alpine", "WireframeOverlay", "", SettingKind::flag, &alpine.wireframe_overlay},
        {"Alpine", "RepairBuildHoles", "", SettingKind::flag, &alpine.repair_build_holes},
        {"Alpine", "RebuildWithHoleLines", "", SettingKind::flag, &build_draws_hole_lines},
        {"Alpine", "RebuildWithOutOfLevelAreas", "", SettingKind::flag, &alpine.rebuild_out_of_level},
        {"TextureBrowser", "TextureBrowserDisplaySize", "", SettingKind::int_value,
         &alpine.texture_browser_display_size},
        {"TextureBrowser", "TextureBrowserMaximized", "", SettingKind::flag, &alpine.texture_browser_maximized},
    });
    add_rect("TextureBrowser", "Texture Browser", false, alpine.texture_browser_rect);
    return settings;
}

template<typename T>
std::optional<T> parse_number(std::string_view text)
{
    T value{};
    const char* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return value;
}

std::optional<EditorRendererKind> parse_renderer(std::string_view text)
{
    if (string_iequals(text, "D3D11")) {
        return EditorRendererKind::d3d11;
    }
    if (string_iequals(text, "D3D9")) {
        return EditorRendererKind::d3d9;
    }
    return std::nullopt;
}

std::optional<COLORREF> parse_color(std::string_view text)
{
    COLORREF color = 0;
    if (!alpine_color_parse_hex(std::string{text}.c_str(), color)) {
        return std::nullopt;
    }
    return color;
}

bool load_setting(const Setting& setting, std::string_view text)
{
    switch (setting.kind) {
    case SettingKind::int_flag:
    case SettingKind::flag: {
        const auto value = parse_number<int>(text);
        if (!value) return false;
        if (setting.kind == SettingKind::flag) {
            *static_cast<bool*>(setting.target) = *value != 0;
        }
        else {
            *static_cast<int*>(setting.target) = *value != 0;
        }
        return true;
    }
    case SettingKind::int_value: {
        const auto value = parse_number<int>(text);
        if (!value) return false;
        *static_cast<int*>(setting.target) = *value;
        return true;
    }
    case SettingKind::long_value: {
        const auto value = parse_number<LONG>(text);
        if (!value) return false;
        *static_cast<LONG*>(setting.target) = *value;
        return true;
    }
    case SettingKind::int_spinner: {
        const auto value = parse_number<int>(text);
        if (!value) return false;
        static_cast<DedIntSpinner*>(setting.target)->set(*value);
        return true;
    }
    case SettingKind::float_spinner: {
        const auto value = parse_number<float>(text);
        if (!value || !std::isfinite(*value)) return false;
        static_cast<DedFloatSpinner*>(setting.target)->set(*value);
        return true;
    }
    case SettingKind::texture: {
        const int bm_handle = alpine_dlg_resolve_bitmap(std::string{text}.c_str());
        if (bm_handle < 0) return false;
        static_cast<BitmapPreviewDialog*>(setting.target)->bm_handle = bm_handle;
        return true;
    }
    case SettingKind::color: {
        const auto color = parse_color(text);
        if (!color) return false;
        static_cast<EditorColorPref*>(setting.target)->color =
            Color{GetRValue(*color), GetGValue(*color), GetBValue(*color)};
        return true;
    }
    case SettingKind::colorref: {
        const auto color = parse_color(text);
        if (!color) return false;
        *static_cast<COLORREF*>(setting.target) = *color;
        return true;
    }
    case SettingKind::renderer: {
        const auto renderer = parse_renderer(text);
        if (!renderer) return false;
        *static_cast<EditorRendererKind*>(setting.target) = *renderer;
        return true;
    }
    }
    return false;
}

std::string format_setting(const Setting& setting)
{
    switch (setting.kind) {
    case SettingKind::int_flag:
    case SettingKind::int_value:
        return std::to_string(*static_cast<int*>(setting.target));
    case SettingKind::flag:
        return *static_cast<bool*>(setting.target) ? "1" : "0";
    case SettingKind::long_value:
        return std::to_string(*static_cast<LONG*>(setting.target));
    case SettingKind::int_spinner:
        return std::to_string(static_cast<DedIntSpinner*>(setting.target)->value);
    case SettingKind::float_spinner:
        return std::format("{}", static_cast<DedFloatSpinner*>(setting.target)->value);
    case SettingKind::texture: {
        const char* filename = bm_get_filename(static_cast<BitmapPreviewDialog*>(setting.target)->bm_handle);
        return filename ? filename : "";
    }
    case SettingKind::color: {
        const Color& color = static_cast<EditorColorPref*>(setting.target)->color;
        return alpine_color_format_hex(RGB(color.r, color.g, color.b));
    }
    case SettingKind::colorref:
        return alpine_color_format_hex(*static_cast<COLORREF*>(setting.target));
    case SettingKind::renderer:
        return *static_cast<EditorRendererKind*>(setting.target) == EditorRendererKind::d3d9 ? "D3D9" : "D3D11";
    }
    return {};
}

std::string settings_path()
{
    return get_module_dir(nullptr) + "alpine_editor.ini";
}

std::optional<SettingValues> read_ini(const std::string& path)
{
    std::ifstream file{path};
    if (!file.is_open()) {
        return std::nullopt;
    }
    SettingValues values;
    std::string line;
    while (std::getline(file, line)) {
        const std::string_view text = trim(line);
        if (text.empty() || text[0] == ';' || text[0] == '#' || text[0] == '[') continue;
        const auto eq = text.find('=');
        if (eq == std::string_view::npos) continue;
        values[std::string{trim(text.substr(0, eq))}] = trim(text.substr(eq + 1));
    }
    return values;
}

// "$Key:\tvalue" lines; strings are quoted and colors written as "{r, g, b}".
std::optional<SettingValues> read_red_cfg(const std::string& path)
{
    std::ifstream file{path};
    if (!file.is_open()) {
        return std::nullopt;
    }
    SettingValues values;
    std::string line;
    while (std::getline(file, line)) {
        const std::string_view text = trim(line);
        const auto colon = text.find(':');
        if (!text.starts_with('$') || colon == std::string_view::npos) continue;
        std::string value{trim(text.substr(colon + 1))};
        int r = 0, g = 0, b = 0;
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        else if (std::sscanf(value.c_str(), "{%d , %d , %d}", &r, &g, &b) == 3) {
            if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) continue;
            value = alpine_color_format_hex(RGB(r, g, b));
        }
        values[std::string{trim(text.substr(1, colon - 1))}] = std::move(value);
    }
    return values;
}

void apply_settings_side_effects()
{
    editor_apply_force_lod0();
    editor_views_mark_repaint_all();
}

void load_settings(PreferencesDialog& dlg)
{
    // The texture categories always come from the stock list: red.cfg's could only be edited by hand.
    dlg.set_defaults();
    // stock starts it off, which leaves the holes a build finds unmarked
    build_draws_hole_lines = true;
    dlg.cutscene_dialog_rect = {-1, -1, -1, -1};
    dlg.brush_clip_dialog_rect = {-1, -1, -1, -1};

    const std::string path = settings_path();
    const std::string red_cfg_path = get_module_dir(nullptr) + "red\\red.cfg";
    std::optional<SettingValues> values = read_ini(path);
    const bool from_ini = values.has_value();
    if (!from_ini && GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        g_settings_unreadable = true;
        xlog::warn("[Settings] {} could not be read, using defaults", path);
        return;
    }
    if (!from_ini) {
        values = read_red_cfg(red_cfg_path);
    }
    if (values) {
        for (const Setting& setting : settings_of(dlg)) {
            const std::string& key = from_ini ? setting.key : setting.red_cfg_key;
            const auto it = key.empty() ? values->end() : values->find(key);
            if (it == values->end()) continue;
            if (!load_setting(setting, it->second)) {
                xlog::warn("[Settings] Ignoring {}={}", key, it->second);
            }
            values->erase(it);
        }
        if (from_ini) {
            for (const auto& [key, value] : *values) {
                if (!key.starts_with("AFES")) {
                    g_orphaned_settings.push_back(key + "=" + value);
                }
            }
        }
        xlog::info("[Settings] Loaded {}", from_ini ? path : red_cfg_path);
    }
    else {
        xlog::info("[Settings] Neither {} nor {} found, using defaults", path, red_cfg_path);
    }
    gr_set_far_clip(dlg.visual->far_clip->value);
}

void save_settings(PreferencesDialog& dlg)
{
    const std::string path = settings_path();
    if (g_settings_unreadable) {
        throw std::runtime_error(path + " could not be read when the editor started, so it was left unchanged.");
    }
    // Stock saved the level's copies, which the dialogs update when they close.
    if (const CDedLevel* level = CDedLevel::Get()) {
        if (level->cutscene_dialog_rect.right >= 0) {
            dlg.cutscene_dialog_rect = level->cutscene_dialog_rect;
        }
        if (level->brush_clip_dialog_rect.right >= 0) {
            dlg.brush_clip_dialog_rect = level->brush_clip_dialog_rect;
        }
    }
    const std::time_t now = std::time(nullptr);
    std::ostringstream out;
    out << "; Alpine Faction Editor Settings File\n\n";
    out << "; This file is automatically generated by the Alpine Faction level editor.\n";
    out << "; Unless you really know what you are doing, manually editing it is NOT recommended.\n";
    out << "; Any edits made while the editor is running will be discarded.\n\n";
    out << "\n[Metadata]\n";
    out << "; DO NOT edit this section.\n";
    out << "AFESTimestamp=" << std::put_time(std::localtime(&now), "%Y-%m-%d %H:%M:%S") << "\n";
    out << "AFESClientVersion=" << PRODUCT_NAME_VERSION << " (" << VERSION_CODE << ")\n";
    out << "AFESFileVersion=" << AFES_VERSION << "\n";
    if (!g_orphaned_settings.empty()) {
        out << "\n[OrphanedSettings]\n";
        out << "; Items in this section were unrecognized by your Alpine Faction client.\n";
        out << "; They could be malformed or may require a newer version of Alpine Faction.\n";
        for (const std::string& line : g_orphaned_settings) {
            out << line << "\n";
        }
    }
    std::string_view section;
    for (const Setting& setting : settings_of(dlg)) {
        if (section != setting.section) {
            section = setting.section;
            out << "\n[" << section << "]\n";
        }
        out << setting.key << "=" << format_setting(setting) << "\n";
    }

    write_file_replacing(path, string_replace(out.str(), "\n", "\r\n"));
    xlog::info("[Settings] Saved {}", path);
}

void update_hover_color_controls(HWND hdlg)
{
    const COLORREF color = g_editor_settings.texture_hover_outline_color;
    alpine_dlg_set_color_controls(hdlg, IDC_PREFS_HOVER_COLOR_SWATCH, IDC_PREFS_HOVER_COLOR_VALUE, GetRValue(color),
                                  GetGValue(color), GetBValue(color));
    const bool enabled = g_editor_settings.texture_hover_outline;
    for (int id : {IDC_PREFS_HOVER_COLOR_SWATCH, IDC_PREFS_HOVER_COLOR_CHANGE, IDC_PREFS_HOVER_COLOR_VALUE}) {
        EnableWindow(GetDlgItem(hdlg, id), enabled);
    }
}

void set_check(HWND hdlg, int id, bool checked)
{
    CheckDlgButton(hdlg, id, checked ? BST_CHECKED : BST_UNCHECKED);
}

bool is_checked(HWND hdlg, int id)
{
    return IsDlgButtonChecked(hdlg, id) == BST_CHECKED;
}

// Edits apply at once, as on the stock tabs; Cancel restores what the dialog opened with.
INT_PTR CALLBACK alpine_tab_proc(HWND hdlg, UINT msg, WPARAM wp, LPARAM)
{
    if (msg == WM_INITDIALOG) {
        const HWND renderer = GetDlgItem(hdlg, IDC_PREFS_RENDERER);
        SendMessageA(renderer, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Direct3D 11"));
        SendMessageA(renderer, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Direct3D 9 (deprecated)"));
        SendMessageA(renderer, CB_SETCURSEL, g_editor_settings.renderer == EditorRendererKind::d3d9 ? 1 : 0, 0);
        set_check(hdlg, IDC_PREFS_HOVER_OUTLINE, g_editor_settings.texture_hover_outline);
        set_check(hdlg, IDC_PREFS_BACKGROUND_THROTTLE, g_editor_settings.background_throttle);
        set_check(hdlg, IDC_PREFS_FORCE_LOD0, g_editor_settings.force_lod0);
        update_hover_color_controls(hdlg);
        return TRUE;
    }
    if (msg == WM_COMMAND && LOWORD(wp) == IDC_PREFS_RENDERER && HIWORD(wp) == CBN_SELCHANGE) {
        const LRESULT sel = SendMessageA(GetDlgItem(hdlg, IDC_PREFS_RENDERER), CB_GETCURSEL, 0, 0);
        g_editor_settings.renderer = sel == 1 ? EditorRendererKind::d3d9 : EditorRendererKind::d3d11;
        return TRUE;
    }
    if (msg != WM_COMMAND || HIWORD(wp) != BN_CLICKED) {
        return FALSE;
    }
    switch (LOWORD(wp)) {
    case IDC_PREFS_HOVER_OUTLINE:
        g_editor_settings.texture_hover_outline = is_checked(hdlg, IDC_PREFS_HOVER_OUTLINE);
        update_hover_color_controls(hdlg);
        break;
    case IDC_PREFS_HOVER_COLOR_CHANGE: {
        COLORREF& color = g_editor_settings.texture_hover_outline_color;
        uint8_t r = GetRValue(color), g = GetGValue(color), b = GetBValue(color);
        if (!alpine_dlg_pick_color(hdlg, IDC_PREFS_HOVER_COLOR_SWATCH, IDC_PREFS_HOVER_COLOR_VALUE, r, g, b)) {
            return TRUE;
        }
        color = RGB(r, g, b);
        break;
    }
    case IDC_PREFS_BACKGROUND_THROTTLE:
        g_editor_settings.background_throttle = is_checked(hdlg, IDC_PREFS_BACKGROUND_THROTTLE);
        break;
    case IDC_PREFS_FORCE_LOD0:
        g_editor_settings.force_lod0 = is_checked(hdlg, IDC_PREFS_FORCE_LOD0);
        break;
    default:
        return FALSE;
    }
    apply_settings_side_effects();
    return TRUE;
}

} // namespace

EditorRendererKind editor_session_renderer()
{
    static const EditorRendererKind renderer = [] {
        EditorRendererKind result = EditorRendererKind::d3d11;
        if (const auto values = read_ini(settings_path())) {
            if (const auto it = values->find("Renderer"); it != values->end()) {
                result = parse_renderer(it->second).value_or(result);
            }
        }
        if (editor_command_line_has(L"-d3d9")) {
            result = EditorRendererKind::d3d9;
        }
        else if (editor_command_line_has(L"-d3d11")) {
            result = EditorRendererKind::d3d11;
        }
        xlog::info("[Settings] Renderer: {}", result == EditorRendererKind::d3d9 ? "Direct3D 9" : "Direct3D 11");
        return result;
    }();
    return renderer;
}

// Stock loads red.cfg here, or sets the defaults when it is missing.
void __fastcall preferences_load_new(PreferencesDialog* self, int edx);
FunHook<decltype(preferences_load_new)> preferences_load_hook{0x0046AC30, preferences_load_new};

void __fastcall preferences_load_new(PreferencesDialog* self, int)
{
    try {
        load_settings(*self);
    }
    catch (const std::exception& e) {
        xlog::error("[Settings] Loading failed: {}", e.what());
    }
    apply_settings_side_effects();
    texture_categories_loaded(self->texture_manager);
}

// Called by the Apply button and by CMainFrame::OnClose, before the frame goes away.
void __fastcall preferences_save_new(PreferencesDialog* self, int edx);
FunHook<decltype(preferences_save_new)> preferences_save_hook{0x0046AD30, preferences_save_new};

void __fastcall preferences_save_new(PreferencesDialog* self, int)
{
    try {
        save_settings(*self);
    }
    catch (const std::exception& e) {
        editor_report_blocking("Settings", "Preferences Not Saved", e.what());
    }
}

// The end of OnInitDialog, where each tab remembers its values for Cancel.
void __fastcall preferences_remember_values_new(PreferencesDialog* self, int edx);
FunHook<decltype(preferences_remember_values_new)> preferences_remember_values_hook{
    0x0046AC00, preferences_remember_values_new};

void __fastcall preferences_remember_values_new(PreferencesDialog* self, int edx)
{
    preferences_remember_values_hook.call_target(self, edx);
    g_settings_before_dialog = g_editor_settings;

    TVINSERTSTRUCTA insert{};
    insert.hParent = TVI_ROOT;
    insert.hInsertAfter = TVI_LAST;
    insert.item.mask = TVIF_TEXT;
    insert.item.pszText = const_cast<char*>(alpine_tab_name);
    SendMessageA(self->tree._d.m_hWnd, TVM_INSERTITEMA, 0, reinterpret_cast<LPARAM>(&insert));
}

void __fastcall preferences_cancel_new(PreferencesDialog* self, int edx);
FunHook<decltype(preferences_cancel_new)> preferences_cancel_hook{0x0046A940, preferences_cancel_new};

void __fastcall preferences_cancel_new(PreferencesDialog* self, int edx)
{
    g_editor_settings = g_settings_before_dialog;
    apply_settings_side_effects();
    preferences_cancel_hook.call_target(self, edx);
}

// Shows the tab named by the clicked tree item. The stock switch destroys current_page and opens the next tab in its
// place, so the Alpine tab only hides current_page and covers it.
void __fastcall preferences_show_tab_new(PreferencesDialog* self, int edx, const VString* name);
FunHook<decltype(preferences_show_tab_new)> preferences_show_tab_hook{0x0046AB30, preferences_show_tab_new};

void __fastcall preferences_show_tab_new(PreferencesDialog* self, int edx, const VString* name)
{
    const HWND dlg = GetParent(self->tree._d.m_hWnd);
    const HWND alpine_tab = GetDlgItem(dlg, IDD_ALPINE_PREFERENCES_PAGE);
    if (std::strcmp(name->c_str(), alpine_tab_name) != 0) {
        if (alpine_tab) {
            DestroyWindow(alpine_tab);
        }
        preferences_show_tab_hook.call_target(self, edx, name);
        return;
    }
    const HWND current = self->current_page ? self->current_page->_d.m_hWnd : nullptr;
    if (alpine_tab || !IsWindow(current)) return;

    RECT rect;
    GetWindowRect(current, &rect);
    MapWindowPoints(nullptr, dlg, reinterpret_cast<POINT*>(&rect), 2);
    const HWND tab = CreateDialogParamA(reinterpret_cast<HINSTANCE>(&__ImageBase),
                                        MAKEINTRESOURCEA(IDD_ALPINE_PREFERENCES_PAGE), dlg, alpine_tab_proc, 0);
    if (!tab) return;
    SetWindowLongPtrA(tab, GWLP_ID, IDD_ALPINE_PREFERENCES_PAGE);
    ShowWindow(current, SW_HIDE);
    SetWindowPos(tab, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOZORDER | SWP_SHOWWINDOW);
}

void ApplyEditorSettingsPatches()
{
    preferences_load_hook.install();
    preferences_save_hook.install();
    preferences_remember_values_hook.install();
    preferences_cancel_hook.install();
    preferences_show_tab_hook.install();
}
