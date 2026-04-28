#include <cstring>
#include <xlog/xlog.h>
#include <patch_common/FunHook.h>
#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/AsmWriter.h>
#include <format>
#include <algorithm>
#include "alpine_settings.h"
#include "misc.h"
#include "../main/main.h"
#include "../graphics/gr.h"
#include "../os/os.h"
#include "../multi/multi.h"
#include "../rf/ui.h"
#include "../rf/sound/sound.h"
#include "../rf/input.h"
#include "../rf/player/player.h"
#include "../rf/misc.h"
#include "../rf/os/os.h"
#include "../object/object.h"
#include "../graphics/d3d11/gr_d3d11_mesh.h"
#include "../rf/level.h"
#include "../input/gyro.h"
#include "../input/gamepad.h"
#include "../input/input.h"

#define DEBUG_UI_LAYOUT 0
#define SHARP_UI_TEXT 1

// options menu elements
static rf::ui::Gadget* new_gadgets[6]; // Allocate space for 6 options buttons
static rf::ui::Button alpine_options_btn;

// Controller bindings overlay (shown when the CONTROLLER tab is active on options panel 3)
static bool g_ctrl_bind_view       = false; // KEYBOARD vs CONTROLLER tab toggle
static bool g_ctrl_codes_installed = false; // true while gamepad scan codes are in cc.bindings

static int16_t g_saved_scan_codes[128]   = {}; // keyboard scan_codes[0] per binding slot
static int16_t g_saved_sc1[128]          = {}; // keyboard scan_codes[1] per binding slot
static int16_t g_saved_mouse_btn_ids[128] = {}; // keyboard mouse_btn_id per binding slot

// CONTROLLER mode checkbox (integrated into the controls panel)
static rf::ui::Checkbox g_ctrl_mode_cbox;
static bool g_ctrl_mode_btns_initialized = false;
static bool g_alpine_options_hud_dirty = false;

// alpine options panel elements
static rf::ui::Panel alpine_options_panel; // parent to all subpanels
static rf::ui::Panel alpine_options_panel0;
static rf::ui::Panel alpine_options_panel1;
static rf::ui::Panel alpine_options_panel2;
static rf::ui::Panel alpine_options_panel3;
static int alpine_options_panel_current_tab = 0;
std::vector<rf::ui::Gadget*> alpine_options_panel_settings;
std::vector<rf::ui::Label*> alpine_options_panel_labels;
std::vector<rf::ui::Label*> alpine_options_panel_tab_labels;

// scroll state and content-area bounds for the scrollable settings panels
static int  alpine_options_scroll_offsets[4]  = {0, 0, 0, 0};
static bool alpine_options_panel_scrollable[4] = {false, false, true, false};
static constexpr int AO_CONTENT_TOP    = 44;
static constexpr int AO_CONTENT_BOTTOM = 344;
static constexpr int AO_SCROLL_STEP    = 30;

// scrollbar geometry cached each frame for hit-testing
static int  g_sb_x = 0, g_sb_y = 0, g_sb_pw = 0, g_sb_ph = 0;
static int  g_sb_thumb_y = 0, g_sb_thumb_h = 0;
static bool g_sb_visible  = false;
static bool g_sb_dragging = false;
static int  g_sb_drag_origin_y = 0, g_sb_drag_origin_scroll = 0;

// Column layout packer: assigns Y positions to gadget rows in order,
// advancing by AO_SCROLL_STEP for each visible row.  Construct with the
// starting Y, then call add_checkbox / add_inputbox for each row.
struct AoColumn {
    int y;

    void add_checkbox(rf::ui::Checkbox& cbox, rf::ui::Label& lbl, bool visible = true)
    {
        if (!visible) return;
        cbox.y = y; lbl.y = y + 6;
        y += AO_SCROLL_STEP;
    }

    void add_inputbox(rf::ui::Checkbox& cbox, rf::ui::Label& lbl, rf::ui::Label& butlbl, bool visible = true)
    {
        if (!visible) return;
        cbox.y = y; lbl.y = y + 6; butlbl.y = y + 6;
        y += AO_SCROLL_STEP;
    }
};

// alpine options tabs
static rf::ui::Checkbox ao_tab_0_cbox;
static rf::ui::Label ao_tab_0_label;
static rf::ui::Checkbox ao_tab_1_cbox;
static rf::ui::Label ao_tab_1_label;
static rf::ui::Checkbox ao_tab_2_cbox;
static rf::ui::Label ao_tab_2_label;
static rf::ui::Checkbox ao_tab_3_cbox;
static rf::ui::Label ao_tab_3_label;

// alpine options inputboxes and labels
static rf::ui::Checkbox ao_retscale_cbox;
static rf::ui::Label ao_retscale_label;
static rf::ui::Label ao_retscale_butlabel;
static char ao_retscale_butlabel_text[9];
static rf::ui::Checkbox ao_fov_cbox;
static rf::ui::Label ao_fov_label;
static rf::ui::Label ao_fov_butlabel;
static char ao_fov_butlabel_text[9];
static rf::ui::Checkbox ao_fpfov_cbox;
static rf::ui::Label ao_fpfov_label;
static rf::ui::Label ao_fpfov_butlabel;
static char ao_fpfov_butlabel_text[9];
static rf::ui::Checkbox ao_ms_cbox;
static rf::ui::Label ao_ms_label;
static rf::ui::Label ao_ms_butlabel;
static char ao_ms_butlabel_text[9];
static rf::ui::Checkbox ao_scannersens_cbox;
static rf::ui::Label ao_scannersens_label;
static rf::ui::Label ao_scannersens_butlabel;
static char ao_scannersens_butlabel_text[9];
static rf::ui::Checkbox ao_scopesens_cbox;
static rf::ui::Label ao_scopesens_label;
static rf::ui::Label ao_scopesens_butlabel;
static char ao_scopesens_butlabel_text[9];
static rf::ui::Checkbox ao_maxfps_cbox;

static rf::ui::Checkbox ao_gamepad_icon_override_cbox;
static rf::ui::Label ao_gamepad_icon_override_label;
static rf::ui::Label ao_gamepad_icon_override_butlabel;
static char ao_gamepad_icon_override_butlabel_text[20];

static rf::ui::Checkbox ao_input_prompt_mode_cbox;
static rf::ui::Label ao_input_prompt_mode_label;
static rf::ui::Label ao_input_prompt_mode_butlabel;
static char ao_input_prompt_mode_butlabel_text[16];

static rf::ui::Label ao_maxfps_label;
static rf::ui::Label ao_maxfps_butlabel;
static char ao_maxfps_butlabel_text[9];
static rf::ui::Checkbox ao_loddist_cbox;
static rf::ui::Label ao_loddist_label;
static rf::ui::Label ao_loddist_butlabel;
static char ao_loddist_butlabel_text[9];
static rf::ui::Checkbox ao_simdist_cbox;
static rf::ui::Label ao_simdist_label;
static rf::ui::Label ao_simdist_butlabel;
static char ao_simdist_butlabel_text[9];

// alpine options checkboxes and labels
static rf::ui::Checkbox ao_mpcharlod_cbox;
static rf::ui::Label ao_mpcharlod_label;
static rf::ui::Checkbox ao_dinput_cbox;
static rf::ui::Label ao_dinput_label;
static rf::ui::Checkbox ao_linearpitch_cbox;
static rf::ui::Label ao_linearpitch_label;
static rf::ui::Checkbox ao_mousecamerascale_cbox;
static char ao_mousecamerascale_butlabel_text[9];
static rf::ui::Checkbox ao_bighud_cbox;
static rf::ui::Label ao_bighud_label;
static rf::ui::Checkbox ao_ctfwh_cbox;
static rf::ui::Label ao_ctfwh_label;
static rf::ui::Checkbox ao_flag_overdrawwh_cbox;
static rf::ui::Label ao_flag_overdrawwh_label;
static rf::ui::Checkbox ao_hill_overdrawwh_cbox;
static rf::ui::Label ao_hill_overdrawwh_label;
static rf::ui::Checkbox ao_sbanim_cbox;
static rf::ui::Label ao_sbanim_label;
static rf::ui::Checkbox ao_teamlabels_cbox;
static rf::ui::Label ao_teamlabels_label;
static rf::ui::Checkbox ao_minimaltimer_cbox;
static rf::ui::Label ao_minimaltimer_label;
static rf::ui::Checkbox ao_targetnames_cbox;
static rf::ui::Label ao_targetnames_label;
static rf::ui::Checkbox ao_always_show_spectators_cbox{};
static rf::ui::Label ao_always_show_spectators_label{};
static rf::ui::Checkbox ao_staticscope_cbox;
static rf::ui::Label ao_staticscope_label;
static rf::ui::Checkbox ao_hitsounds_cbox;
static rf::ui::Label ao_hitsounds_label;
static rf::ui::Checkbox ao_taunts_cbox;
static rf::ui::Label ao_taunts_label;
static rf::ui::Checkbox ao_teamrad_cbox;
static rf::ui::Label ao_teamrad_label;
static rf::ui::Checkbox ao_globalrad_cbox;
static rf::ui::Label ao_globalrad_label;
static rf::ui::Checkbox ao_clicklimit_cbox;
static rf::ui::Label ao_clicklimit_label;
static rf::ui::Checkbox ao_gaussian_cbox;
static rf::ui::Label ao_gaussian_label;
static rf::ui::Checkbox ao_geochunk_cbox;
static rf::ui::Label ao_geochunk_label;
static rf::ui::Checkbox ao_autosave_cbox;
static rf::ui::Label ao_autosave_label;
static rf::ui::Checkbox ao_damagenum_cbox;
static rf::ui::Label ao_damagenum_label;
static rf::ui::Checkbox ao_showfps_cbox;
static rf::ui::Label ao_showfps_label;
static rf::ui::Checkbox ao_showping_cbox;
static rf::ui::Label ao_showping_label;
static rf::ui::Checkbox ao_locpings_cbox;
static rf::ui::Label ao_locpings_label;
static rf::ui::Checkbox ao_redflash_cbox;
static rf::ui::Label ao_redflash_label;
static rf::ui::Checkbox ao_deathbars_cbox;
static rf::ui::Label ao_deathbars_label;
static rf::ui::Checkbox ao_swapar_cbox;
static rf::ui::Label ao_swapar_label;
static rf::ui::Checkbox ao_swapgn_cbox;
static rf::ui::Label ao_swapgn_label;
static rf::ui::Checkbox ao_swapsg_cbox;
static rf::ui::Label ao_swapsg_label;
static rf::ui::Checkbox ao_weapshake_cbox;
static rf::ui::Label ao_weapshake_label;
static rf::ui::Checkbox ao_firelights_cbox;
static rf::ui::Label ao_firelights_label;
static rf::ui::Checkbox ao_glares_cbox;
static rf::ui::Label ao_glares_label;
static rf::ui::Checkbox ao_nearest_cbox;
static rf::ui::Label ao_nearest_label;
static rf::ui::Checkbox ao_camshake_cbox;
static rf::ui::Label ao_camshake_label;
static rf::ui::Checkbox ao_ricochet_cbox;
static rf::ui::Label ao_ricochet_label;
static rf::ui::Checkbox ao_fullbrightchar_cbox;
static rf::ui::Label ao_fullbrightchar_label;
static rf::ui::Checkbox ao_notex_cbox;
static rf::ui::Label ao_notex_label;
static rf::ui::Checkbox ao_meshlight_cbox;
static rf::ui::Label ao_meshlight_label;
static rf::ui::Label ao_meshlight_butlabel;
static char ao_meshlight_butlabel_text[9];
static rf::ui::Checkbox ao_enemybullets_cbox;
static rf::ui::Label ao_enemybullets_label;
static rf::ui::Checkbox ao_togglecrouch_cbox;
static rf::ui::Label ao_togglecrouch_label;
static rf::ui::Checkbox ao_joinbeep_cbox;
static rf::ui::Label ao_joinbeep_label;
static rf::ui::Checkbox ao_vsync_cbox;
static rf::ui::Label ao_vsync_label;
static rf::ui::Checkbox ao_unclamplights_cbox;
static rf::ui::Label ao_unclamplights_label;
static rf::ui::Checkbox ao_bombrng_cbox;
static rf::ui::Label ao_bombrng_label;
static rf::ui::Checkbox ao_exposuredamage_cbox;
static rf::ui::Label ao_exposuredamage_label;
static rf::ui::Checkbox ao_painsounds_cbox;
static rf::ui::Label ao_painsounds_label;

// gamepad settings
static rf::ui::Checkbox ao_gyro_enabled_cbox;
static rf::ui::Label ao_gyro_enabled_label;
static rf::ui::Checkbox ao_joy_sensitivity_cbox;
static rf::ui::Label ao_joy_sensitivity_label;
static rf::ui::Label ao_joy_sensitivity_butlabel;
static char ao_joy_sensitivity_butlabel_text[9];
static char ao_joy_sensitivity_label_text[17];
static rf::ui::Checkbox ao_move_deadzone_cbox;
static rf::ui::Label ao_move_deadzone_label;
static rf::ui::Label ao_move_deadzone_butlabel;
static char ao_move_deadzone_butlabel_text[9];
static rf::ui::Checkbox ao_look_deadzone_cbox;
static rf::ui::Label ao_look_deadzone_label;
static rf::ui::Label ao_look_deadzone_butlabel;
static char ao_look_deadzone_butlabel_text[9];
static rf::ui::Checkbox ao_gyro_sensitivity_cbox;
static rf::ui::Label ao_gyro_sensitivity_label;
static rf::ui::Label ao_gyro_sensitivity_butlabel;
static char ao_gyro_sensitivity_butlabel_text[9];
static rf::ui::Checkbox ao_gyro_autocalibration_cbox;
static rf::ui::Label ao_gyro_autocalibration_label;
static rf::ui::Label ao_gyro_autocalibration_butlabel;
static char ao_gyro_autocalibration_butlabel_text[16];
static rf::ui::Checkbox ao_gyro_modifier_mode_cbox;
static rf::ui::Label ao_gyro_modifier_mode_label;
static rf::ui::Label ao_gyro_modifier_mode_butlabel;
static char ao_gyro_modifier_mode_butlabel_text[12];
static rf::ui::Checkbox ao_gyro_space_cbox;
static rf::ui::Label ao_gyro_space_label;
static rf::ui::Label ao_gyro_space_butlabel;
static char ao_gyro_space_butlabel_text[12];
static rf::ui::Checkbox ao_gyro_invert_y_cbox;
static rf::ui::Label ao_gyro_invert_y_label;
static rf::ui::Checkbox ao_gyro_vehicle_cbox;
static rf::ui::Label ao_gyro_vehicle_label;
static rf::ui::Checkbox ao_gyro_tightening_cbox;
static rf::ui::Label ao_gyro_tightening_label;
static rf::ui::Label ao_gyro_tightening_butlabel;
static char ao_gyro_tightening_butlabel_text[9];
static rf::ui::Checkbox ao_gyro_smoothing_cbox;
static rf::ui::Label ao_gyro_smoothing_label;
static rf::ui::Label ao_gyro_smoothing_butlabel;
static char ao_gyro_smoothing_butlabel_text[9];
static rf::ui::Checkbox ao_gyro_vh_mixer_cbox;
static rf::ui::Label ao_gyro_vh_mixer_label;
static rf::ui::Label ao_gyro_vh_mixer_butlabel;
static char ao_gyro_vh_mixer_butlabel_text[9];
static rf::ui::Checkbox ao_gyro_scannersens_cbox;
static rf::ui::Label ao_gyro_scannersens_label;
static rf::ui::Label ao_gyro_scannersens_butlabel;
static char ao_gyro_scannersens_butlabel_text[9];
static rf::ui::Checkbox ao_gyro_scopesens_cbox;
static rf::ui::Label ao_gyro_scopesens_label;
static rf::ui::Label ao_gyro_scopesens_butlabel;
static char ao_gyro_scopesens_butlabel_text[9];
static rf::ui::Checkbox ao_rumble_intensity_cbox;
static rf::ui::Label ao_rumble_intensity_label;
static rf::ui::Label ao_rumble_intensity_butlabel;
static char ao_rumble_intensity_butlabel_text[9];
static rf::ui::Checkbox ao_rumble_trigger_cbox;
static rf::ui::Label ao_rumble_trigger_label;
static rf::ui::Label ao_rumble_trigger_butlabel;
static char ao_rumble_trigger_butlabel_text[9];
static rf::ui::Checkbox ao_rumble_filter_cbox;
static rf::ui::Label ao_rumble_filter_label;
static rf::ui::Label ao_rumble_filter_butlabel;
static char ao_rumble_filter_butlabel_text[9];
static rf::ui::Checkbox ao_rumble_weapon_cbox;
static rf::ui::Label ao_rumble_weapon_label;
static rf::ui::Checkbox ao_rumble_env_cbox;
static rf::ui::Label ao_rumble_env_label;
static rf::ui::Checkbox ao_rumble_primary_cbox;
static rf::ui::Label ao_rumble_primary_label;
static rf::ui::Checkbox ao_joy_camera_cbox;
static rf::ui::Label ao_joy_camera_label;
static rf::ui::Label ao_joy_camera_butlabel;
static char ao_joy_camera_butlabel_text[12];
static rf::ui::Checkbox ao_joy_invert_y_cbox;
static rf::ui::Label ao_joy_invert_y_label;
static rf::ui::Checkbox ao_swap_sticks_cbox;
static rf::ui::Label ao_swap_sticks_label;
static rf::ui::Checkbox ao_flickstick_sweep_cbox;
static rf::ui::Label ao_flickstick_sweep_label;
static rf::ui::Label ao_flickstick_sweep_butlabel;
static char ao_flickstick_sweep_butlabel_text[9];
static rf::ui::Checkbox ao_flickstick_deadzone_cbox;
static rf::ui::Label ao_flickstick_deadzone_label;
static rf::ui::Label ao_flickstick_deadzone_butlabel;
static char ao_flickstick_deadzone_butlabel_text[9];
static rf::ui::Checkbox ao_flickstick_release_dz_cbox;
static rf::ui::Label ao_flickstick_release_dz_label;
static rf::ui::Label ao_flickstick_release_dz_butlabel;
static char ao_flickstick_release_dz_butlabel_text[9];
static rf::ui::Checkbox ao_flickstick_smoothing_cbox;
static rf::ui::Label ao_flickstick_smoothing_label;
static rf::ui::Label ao_flickstick_smoothing_butlabel;
static char ao_flickstick_smoothing_butlabel_text[9];
static rf::ui::Checkbox ao_joy_scannersens_cbox;
static rf::ui::Label ao_joy_scannersens_label;
static rf::ui::Label ao_joy_scannersens_butlabel;
static char ao_joy_scannersens_butlabel_text[9];
static rf::ui::Checkbox ao_joy_scopesens_cbox;
static rf::ui::Label ao_joy_scopesens_label;
static rf::ui::Label ao_joy_scopesens_butlabel;
static char ao_joy_scopesens_butlabel_text[9];

// levelsounds audio options slider
std::vector<rf::ui::Gadget*> alpine_audio_panel_settings;
std::vector<rf::ui::Gadget*> alpine_audio_panel_settings_buttons;
static rf::ui::Slider levelsound_opt_slider;
static rf::ui::Button levelsound_opt_button;
static rf::ui::Label levelsound_opt_label;

// fflink info strings
static rf::ui::Label ao_fflink_label1;
static rf::ui::Label ao_fflink_label2;
static rf::ui::Label ao_fflink_label3;

static inline void debug_ui_layout([[ maybe_unused ]] rf::ui::Gadget& gadget)
{
#if DEBUG_UI_LAYOUT
    int x = gadget.get_absolute_x() * rf::ui::scale_x;
    int y = gadget.get_absolute_y() * rf::ui::scale_y;
    int w = gadget.w * rf::ui::scale_x;
    int h = gadget.h * rf::ui::scale_y;
    rf::gr::set_color((x ^ y) & 255, 0, 0, 64);
    rf::gr::rect(x, y, w, h);
#endif
}

void __fastcall UiButton_create(rf::ui::Button& this_, int, const char *normal_bm, const char *selected_bm, int x, int y, int id, const char *text, int font)
{
    this_.key = id;
    this_.x = x;
    this_.y = y;
    if (*normal_bm) {
        this_.bg_bitmap = rf::bm::load(normal_bm, -1, false);
        rf::gr::tcache_add_ref(this_.bg_bitmap);
        rf::bm::get_dimensions(this_.bg_bitmap, &this_.w, &this_.h);
    }
    if (*selected_bm) {
        this_.selected_bitmap = rf::bm::load(selected_bm, -1, false);
        rf::gr::tcache_add_ref(this_.selected_bitmap);
        if (this_.bg_bitmap < 0) {
            rf::bm::get_dimensions(this_.selected_bitmap, &this_.w, &this_.h);
        }
    }
    this_.text = strdup(text);
    this_.font = font;
}
FunHook UiButton_create_hook{0x004574D0, UiButton_create};

void __fastcall UiButton_set_text(rf::ui::Button& this_, int, const char *text, int font)
{
    delete[] this_.text;
    this_.text = strdup(text);
    this_.font = font;
}
FunHook UiButton_set_text_hook{0x00457710, UiButton_set_text};

void __fastcall UiButton_render(rf::ui::Button& this_)
{
    int x = static_cast<int>(this_.get_absolute_x() * rf::ui::scale_x);
    int y = static_cast<int>(this_.get_absolute_y() * rf::ui::scale_y);
    int w = static_cast<int>(this_.w * rf::ui::scale_x);
    int h = static_cast<int>(this_.h * rf::ui::scale_y);

    if (this_.bg_bitmap >= 0) {
        rf::gr::set_color(255, 255, 255, 255);
        rf::gr::bitmap_scaled(this_.bg_bitmap, x, y, w, h, 0, 0, this_.w, this_.h, false, false, rf::gr::bitmap_clamp_mode);
    }

    if (!this_.enabled) {
        rf::gr::set_color(96, 96, 96, 255);
    }
    else if (this_.highlighted) {
        rf::gr::set_color(240, 240, 240, 255);
    }
    else {
        rf::gr::set_color(192, 192, 192, 255);
    }

    if (this_.enabled && this_.highlighted && this_.selected_bitmap >= 0) {
        auto mode = addr_as_ref<rf::gr::Mode>(0x01775B0C);
        rf::gr::bitmap_scaled(this_.selected_bitmap, x, y, w, h, 0, 0, this_.w, this_.h, false, false, mode);
    }

    // Change clip region for text rendering
    int clip_x, clip_y, clip_w, clip_h;
    rf::gr::get_clip(&clip_x, &clip_y, &clip_w, &clip_h);
    rf::gr::set_clip(x, y, w, h);

    std::string_view text_sv{this_.text};
    int num_lines = 1 + std::count(text_sv.begin(), text_sv.end(), '\n');
    int text_h = rf::gr::get_font_height(this_.font) * num_lines;
    int text_y = (h - text_h) / 2;
    rf::gr::string(rf::gr::center_x, text_y, this_.text, this_.font);

    // Restore clip region
    rf::gr::set_clip(clip_x, clip_y, clip_w, clip_h);

    debug_ui_layout(this_);
}
FunHook UiButton_render_hook{0x004577A0, UiButton_render};

void __fastcall UiLabel_create(rf::ui::Label& this_, int, rf::ui::Gadget *parent, int x, int y, const char *text, int font)
{
    this_.parent = parent;
    this_.x = x;
    this_.y = y;
    const auto [text_w, text_h] = rf::gr::get_string_size(text, font);
    this_.w = static_cast<int>(text_w / rf::ui::scale_x);
    this_.h = static_cast<int>(text_h / rf::ui::scale_y);
    this_.text = strdup(text);
    this_.font = font;
    this_.align = rf::gr::ALIGN_LEFT;
    this_.clr.set(0, 0, 0, 255);
}
FunHook UiLabel_create_hook{0x00456B60, UiLabel_create};

void __fastcall UiLabel_create2(rf::ui::Label& this_, int, rf::ui::Gadget *parent, int x, int y, int w, int h, const char *text, int font)
{
    this_.parent = parent;
    this_.x = x;
    this_.y = y;
    this_.w = w;
    this_.h = h;
    if (*text == ' ') {
        while (*text == ' ') {
            ++text;
        }
        this_.align = rf::gr::ALIGN_CENTER;
    }
    else {
        this_.align = rf::gr::ALIGN_LEFT;
    }
    this_.text = strdup(text);
    this_.font = font;
    this_.clr.set(0, 0, 0, 255);
}
FunHook UiLabel_create2_hook{0x00456C20, UiLabel_create2};

void __fastcall UiLabel_set_text(rf::ui::Label& this_, int, const char *text, int font)
{
    delete[] this_.text;
    this_.text = strdup(text);
    this_.font = font;
}
FunHook UiLabel_set_text_hook{0x00456DC0, UiLabel_set_text};

void __fastcall UiLabel_render(rf::ui::Label& this_) {
    if (this_.text) {
        if (!this_.enabled) {
            rf::gr::set_color(48, 48, 48, 128);
        } else if (this_.highlighted) {
            rf::gr::set_color(240, 240, 240, 255);
        } else {
            rf::gr::set_color(this_.clr);
        }
        int x = static_cast<int>(this_.get_absolute_x() * rf::ui::scale_x);
        int y = static_cast<int>(this_.get_absolute_y() * rf::ui::scale_y);
        const auto [text_w, text_h] = rf::gr::get_string_size(this_.text, this_.font);
        if (this_.align == rf::gr::ALIGN_CENTER) {
            x += static_cast<int>(this_.w * rf::ui::scale_x / 2);
        } else if (this_.align == rf::gr::ALIGN_RIGHT) {
            x += static_cast<int>(this_.w * rf::ui::scale_x);
        } else {
            x += static_cast<int>(1 * rf::ui::scale_x);
        }
        rf::gr::string_aligned(this_.align, x, y, this_.text, this_.font);
    }

    debug_ui_layout(this_);
}
FunHook UiLabel_render_hook{0x00456ED0, UiLabel_render};

void __fastcall UiInputBox_create(rf::ui::InputBox& this_, int, rf::ui::Gadget *parent, int x, int y, const char *text, int font, int w)
{
    this_.parent = parent;
    this_.x = x;
    this_.y = y;
    this_.w = w;
    this_.h = static_cast<int>(rf::gr::get_font_height(font) / rf::ui::scale_y);
    this_.max_text_width = static_cast<int>(w * rf::ui::scale_x);
    this_.font = font;
    std::strncpy(this_.text, text, std::size(this_.text));
    this_.text[std::size(this_.text) - 1] = '\0';
}
FunHook UiInputBox_create_hook{0x00456FE0, UiInputBox_create};

void __fastcall UiInputBox_render(rf::ui::InputBox& this_, void*)
{
    if (this_.enabled && this_.highlighted) {
        rf::gr::set_color(240, 240, 240, 255);
    }
    else {
        rf::gr::set_color(192, 192, 192, 255);
    }

    int x = static_cast<int>((this_.get_absolute_x() + 1) * rf::ui::scale_x);
    int y = static_cast<int>(this_.get_absolute_y() * rf::ui::scale_y);
    int clip_x, clip_y, clip_w, clip_h;
    rf::gr::get_clip(&clip_x, &clip_y, &clip_w, &clip_h);
    rf::gr::set_clip(x, y, this_.max_text_width, static_cast<int>(this_.h * rf::ui::scale_y + 5)); // for some reason input fields are too thin
    int text_offset_x = static_cast<int>(1 * rf::ui::scale_x);
    rf::gr::string(text_offset_x, 0, this_.text, this_.font);

    if (this_.enabled && this_.highlighted) {
        rf::ui::update_input_box_cursor();
        if (rf::ui::input_box_cursor_visible) {
            const auto [text_w, text_h] = rf::gr::get_string_size(this_.text, this_.font);
            rf::gr::string(text_offset_x + text_w, 0, "_", this_.font);
        }
    }
    rf::gr::set_clip(clip_x, clip_y, clip_w, clip_h);

    debug_ui_layout(this_);
}
FunHook UiInputBox_render_hook{0x004570E0, UiInputBox_render};

void __fastcall UiCycler_add_item(rf::ui::Cycler& this_, int, const char *text, int font)
{
    if (this_.num_items < rf::ui::Cycler::max_items) {
        this_.items_text[this_.num_items] = strdup(text);
        this_.items_font[this_.num_items] = font;
        ++this_.num_items;
    }
}
FunHook UiCycler_add_item_hook{0x00458080, UiCycler_add_item};

void __fastcall UiCycler_render(rf::ui::Cycler& this_)
{
    if (this_.enabled && this_.highlighted) {
        rf::gr::set_color(255, 255, 255, 255);
    }
    else if (this_.enabled) {
        rf::gr::set_color(192, 192, 192, 255);
    }
    else {
        rf::gr::set_color(96, 96, 96, 255);
    }

    int x = static_cast<int>(this_.get_absolute_x() * rf::ui::scale_x);
    int y = static_cast<int>(this_.get_absolute_y() * rf::ui::scale_y);

    const char* text = this_.items_text[this_.current_item];
    int font = this_.items_font[this_.current_item];
    int font_h = rf::gr::get_font_height(font);
    int text_x = x + static_cast<int>(this_.w * rf::ui::scale_x / 2);
    int text_y = y + static_cast<int>((this_.h * rf::ui::scale_y - font_h) / 2);
    rf::gr::string_aligned(rf::gr::ALIGN_CENTER, text_x, text_y, text, font);

    debug_ui_layout(this_);
}
FunHook UiCycler_render_hook{0x00457F40, UiCycler_render};

CallHook<void(int*, int*, const char*, int, int, char, int)> popup_set_text_gr_split_str_hook{
    0x00455A7D,
    [](int *n_chars, int *start_indices, const char *str, int max_pixel_w, int max_lines, char ignore_char, int font) {
        max_pixel_w = static_cast<int>(max_pixel_w * rf::ui::scale_x);
        popup_set_text_gr_split_str_hook.call_target(n_chars, start_indices, str, max_pixel_w, max_lines, ignore_char, font);
    },
};

static bool is_any_font_modded()
{
    auto rfpc_large_checksum = rf::get_file_checksum("rfpc-large.vf");
    auto rfpc_medium_checksum = rf::get_file_checksum("rfpc-medium.vf");
    auto rfpc_small_checksum = rf::get_file_checksum("rfpc-small.vf");
    // Note: rfpc-large differs between Steam and CD game distributions
    bool rfpc_large_modded = rfpc_large_checksum != 0x5E7DC24Au && rfpc_large_checksum != 0xEB80AD63u;
    bool rfpc_medium_modded = rfpc_medium_checksum != 0x19E7184Cu;
    bool rfpc_small_modded = rfpc_small_checksum != 0xAABA52E6u;
    bool any_font_modded = rfpc_large_modded || rfpc_medium_modded || rfpc_small_modded;
    if (any_font_modded) {
        xlog::info("Detected modded fonts: rfpc-large {} ({:08X}) rfpc-medium {} ({:08X}) rfpc-small {} ({:08X})",
            rfpc_large_modded, rfpc_large_checksum,
            rfpc_medium_modded, rfpc_medium_checksum,
            rfpc_small_modded, rfpc_small_checksum
        );
    }
    return any_font_modded;
}

FunHook<void()> menu_init_hook{
    0x00442BB0,
    []() {
        if (is_headless_mode() || headless_requested_from_raw_cmdline()) {
            return;
        }
        menu_init_hook.call_target();
#if SHARP_UI_TEXT
        xlog::info("UI scale: {:.4f} {:.4f}", rf::ui::scale_x, rf::ui::scale_y);
        if (rf::ui::scale_y > 1.0f && !is_any_font_modded()) {
            int large_font_size = std::min(128, static_cast<int>(std::round(rf::ui::scale_y * 14.5f))); // 32
            int medium_font_size = std::min(128, static_cast<int>(std::round(rf::ui::scale_y * 9.0f))); // 20
            int small_font_size = std::min(128, static_cast<int>(std::round(rf::ui::scale_y * 7.5f))); // 16
            xlog::info("UI font sizes: {} {} {}", large_font_size, medium_font_size, small_font_size);

            rf::ui::large_font = rf::gr::load_font(std::format("boldfont.ttf:{}", large_font_size).c_str());
            rf::ui::medium_font_0 = rf::gr::load_font(std::format("regularfont.ttf:{}", medium_font_size).c_str());
            rf::ui::medium_font_1 = rf::ui::medium_font_0;
            rf::ui::small_font = rf::gr::load_font(std::format("regularfont.ttf:{}", small_font_size).c_str());
        }
#endif
    },
};

auto UiInputBox_add_char = reinterpret_cast<bool (__thiscall*)(void *this_, char c)>(0x00457260);

extern FunHook<bool __fastcall(void*, int, rf::Key)> UiInputBox_process_key_hook;
bool __fastcall UiInputBox_process_key_new(void *this_, int edx, rf::Key key)
{
    if (key == (rf::KEY_V | rf::KEY_CTRLED)) {
        char buf[256];
        rf::os_get_clipboard_text(buf, static_cast<int>(std::size(buf) - 1));
        for (int i = 0; buf[i]; ++i) {
            UiInputBox_add_char(this_, buf[i]);
        }
        return true;
    }
    return UiInputBox_process_key_hook.call_target(this_, edx, key);
}
FunHook<bool __fastcall(void *this_, int edx, rf::Key key)> UiInputBox_process_key_hook{
    0x00457300,
    UiInputBox_process_key_new,
};

void ao_play_button_snd(bool on) {
    if (on) {
        rf::snd_play(45, 0, 0.0f, 1.0f);
    }
    else {
        rf::snd_play(44, 0, 0.0f, 1.0f);
    }
}

void ao_play_tab_snd() {
    rf::snd_play(41, 0, 0.0f, 1.0f);
}

void ao_tab_button_on_click_0(int x, int y) {
    alpine_options_panel_current_tab = 0;
    ao_play_tab_snd();
}

void ao_tab_button_on_click_1(int x, int y) {
    alpine_options_panel_current_tab = 1;
    ao_play_tab_snd();
}

void ao_tab_button_on_click_2(int x, int y) {
    alpine_options_panel_current_tab = 2;
    ao_play_tab_snd();
}

void ao_tab_button_on_click_3(int x, int y) {
    alpine_options_panel_current_tab = 3;
    ao_play_tab_snd();
}

void ao_bighud_cbox_on_click(int x, int y) {
    g_alpine_game_config.big_hud = !g_alpine_game_config.big_hud;
    ao_bighud_cbox.checked = g_alpine_game_config.big_hud;
    set_big_hud(g_alpine_game_config.big_hud);
    ao_play_button_snd(g_alpine_game_config.big_hud);
}

void ao_dinput_cbox_on_click(int x, int y)
{
    g_alpine_game_config.direct_input = !g_alpine_game_config.direct_input;
    ao_dinput_cbox.checked = g_alpine_game_config.direct_input;
    ao_play_button_snd(g_alpine_game_config.direct_input);
}

void ao_linearpitch_cbox_on_click(int x, int y) {
    g_alpine_game_config.mouse_linear_pitch = !g_alpine_game_config.mouse_linear_pitch;
    ao_linearpitch_cbox.checked = g_alpine_game_config.mouse_linear_pitch;
    ao_play_button_snd(g_alpine_game_config.mouse_linear_pitch);
}

// fov
void ao_fov_cbox_on_click_callback() {
    char str_buffer[7] = "";
    rf::ui::popup_get_input(str_buffer, sizeof(str_buffer));
    std::string str = str_buffer;
    try {
        float new_fov = std::stof(str);
        g_alpine_game_config.set_horz_fov(new_fov);
    }
    catch (const std::exception& e) {
        xlog::info("Invalid FOV input: '{}', reason: {}", str, e.what());
    }
}
void ao_fov_cbox_on_click(int x, int y) {
    rf::ui::popup_message("Enter new FOV value (0 for automatic scaling):", "", ao_fov_cbox_on_click_callback, 1);
}

// fpgun fov
void ao_fpfov_cbox_on_click_callback() {
    char str_buffer[7] = "";
    rf::ui::popup_get_input(str_buffer, sizeof(str_buffer));
    std::string str = str_buffer;
    try {
        float new_fpfov = std::stof(str);
        g_alpine_game_config.set_fpgun_fov_scale(new_fpfov);
    }
    catch (const std::exception& e) {
        xlog::info("Invalid FPGun FOV input: '{}', reason: {}", str, e.what());
    }
}
void ao_fpfov_cbox_on_click(int x, int y) {
    rf::ui::popup_message("Enter new FPGun FOV modifier value:", "", ao_fpfov_cbox_on_click_callback, 1);
}

// ms
void ao_ms_cbox_on_click_callback() {
    char str_buffer[7] = "";
    rf::ui::popup_get_input(str_buffer, sizeof(str_buffer));
    std::string str = str_buffer;
    try {
        float new_ms = std::stof(str);
        rf::local_player->settings.controls.mouse_sensitivity = new_ms;
    }
    catch (const std::exception& e) {
        xlog::info("Invalid sensitivity input: '{}', reason: {}", str, e.what());
    }
}
void ao_ms_cbox_on_click(int x, int y) {
    rf::ui::popup_message("Enter new mouse sensitivity value:", "", ao_ms_cbox_on_click_callback, 1);
}

// scanner ms
void ao_scannersens_cbox_on_click_callback() {
    char str_buffer[7] = "";
    rf::ui::popup_get_input(str_buffer, sizeof(str_buffer));
    std::string str = str_buffer;
    try {
        float new_scale = std::stof(str);
        g_alpine_game_config.set_scanner_sens_mod(new_scale);
        update_scanner_sensitivity();
    }
    catch (const std::exception& e) {
        xlog::info("Invalid modifier input: '{}', reason: {}", str, e.what());
    }
}
void ao_scannersens_cbox_on_click(int x, int y) {
    rf::ui::popup_message("Enter new mouse scanner sensitivity modifier value:", "", ao_scannersens_cbox_on_click_callback, 1);
}

// scope ms
void ao_scopesens_cbox_on_click_callback() {
    char str_buffer[7] = "";
    rf::ui::popup_get_input(str_buffer, sizeof(str_buffer));
    std::string str = str_buffer;
    try {
        float new_scale = std::stof(str);
        g_alpine_game_config.set_scope_sens_mod(new_scale);
        update_scope_sensitivity();
    }
    catch (const std::exception& e) {
        xlog::info("Invalid modifier input: '{}', reason: {}", str, e.what());
    }
}
void ao_scopesens_cbox_on_click(int x, int y) {
    rf::ui::popup_message("Enter new mouse scope sensitivity modifier value:", "", ao_scopesens_cbox_on_click_callback, 1);
}

// joy scanner modifier
void ao_joy_scannersens_cbox_on_click_callback() {
    char str_buffer[7] = "";
    rf::ui::popup_get_input(str_buffer, sizeof(str_buffer));
    std::string str = str_buffer;
    try {
        float new_scale = std::stof(str);
        g_alpine_game_config.set_gamepad_scanner_sens_mod(new_scale);
    }
    catch (const std::exception& e) {
        xlog::info("Invalid modifier input: '{}', reason: {}", str, e.what());
    }
}
void ao_joy_scannersens_cbox_on_click(int x, int y) {
    rf::ui::popup_message("Enter new joy scanner sensitivity modifier value:", "", ao_joy_scannersens_cbox_on_click_callback, 1);
}

// joy scope modifier
void ao_joy_scopesens_cbox_on_click_callback() {
    char str_buffer[7] = "";
    rf::ui::popup_get_input(str_buffer, sizeof(str_buffer));
    std::string str = str_buffer;
    try {
        float new_scale = std::stof(str);
        g_alpine_game_config.set_gamepad_scope_sens_mod(new_scale);
    }
    catch (const std::exception& e) {
        xlog::info("Invalid modifier input: '{}', reason: {}", str, e.what());
    }
}
void ao_joy_scopesens_cbox_on_click(int x, int y) {
    rf::ui::popup_message("Enter new joy scope sensitivity modifier value:", "", ao_joy_scopesens_cbox_on_click_callback, 1);
}

// gyro scanner modifier
void ao_gyro_scannersens_cbox_on_click_callback() {
    char str_buffer[7] = "";
    rf::ui::popup_get_input(str_buffer, sizeof(str_buffer));
    std::string str = str_buffer;
    try {
        float new_scale = std::stof(str);
        g_alpine_game_config.set_gamepad_scanner_gyro_sens_mod(new_scale);
    }
    catch (const std::exception& e) {
        xlog::info("Invalid modifier input: '{}', reason: {}", str, e.what());
    }
}
void ao_gyro_scannersens_cbox_on_click(int x, int y) {
    rf::ui::popup_message("Enter new gyro scanner sensitivity modifier value:", "", ao_gyro_scannersens_cbox_on_click_callback, 1);
}

// gyro scope modifier
void ao_gyro_scopesens_cbox_on_click_callback() {
    char str_buffer[7] = "";
    rf::ui::popup_get_input(str_buffer, sizeof(str_buffer));
    std::string str = str_buffer;
    try {
        float new_scale = std::stof(str);
        g_alpine_game_config.set_gamepad_scope_gyro_sens_mod(new_scale);
    }
    catch (const std::exception& e) {
        xlog::info("Invalid modifier input: '{}', reason: {}", str, e.what());
    }
}
void ao_gyro_scopesens_cbox_on_click(int x, int y) {
    rf::ui::popup_message("Enter new gyro scope sensitivity modifier value:", "", ao_gyro_scopesens_cbox_on_click_callback, 1);
}

// reticle scale
void ao_retscale_cbox_on_click_callback() {
    char str_buffer[7] = "";
    rf::ui::popup_get_input(str_buffer, sizeof(str_buffer));
    std::string str = str_buffer;
    try {
        float new_scale = std::stof(str);
        g_alpine_game_config.set_reticle_scale(new_scale);
    }
    catch (const std::exception& e) {
        xlog::info("Invalid reticle scale input: '{}', reason: {}", str, e.what());
    }
}
void ao_retscale_cbox_on_click(int x, int y) {
    rf::ui::popup_message("Enter new reticle scale value:", "", ao_retscale_cbox_on_click_callback, 1);
}

// max fps
void ao_maxfps_cbox_on_click_callback()
{
    char str_buffer[7] = "";
    rf::ui::popup_get_input(str_buffer, sizeof(str_buffer));
    std::string str = str_buffer;
    try {
        unsigned new_fps = std::stoi(str);
        g_alpine_game_config.set_max_fps(new_fps);
        apply_maximum_fps();
    }
    catch (const std::exception& e) {
        xlog::info("Invalid max FPS input: '{}', reason: {}", str, e.what());
    }
}
void ao_maxfps_cbox_on_click(int x, int y)
{
    rf::ui::popup_message("Enter new maximum FPS value:", "", ao_maxfps_cbox_on_click_callback, 1);
}

// lod dist scale
void ao_loddist_cbox_on_click_callback()
{
    char str_buffer[7] = "";
    rf::ui::popup_get_input(str_buffer, sizeof(str_buffer));
    std::string str = str_buffer;
    try {
        float new_dist = std::stof(str);
        g_alpine_game_config.set_lod_dist_scale(new_dist);
    }
    catch (const std::exception& e) {
        xlog::info("Invalid LOD distance scale input: '{}', reason: {}", str, e.what());
    }
}
void ao_loddist_cbox_on_click(int x, int y)
{
    rf::ui::popup_message("Enter new LOD distance scale value:", "", ao_loddist_cbox_on_click_callback, 1);
}

// simulation distance
void ao_simdist_cbox_on_click_callback()
{
    char str_buffer[7] = "";
    rf::ui::popup_get_input(str_buffer, sizeof(str_buffer));
    std::string str = str_buffer;
    try {
        float new_dist = std::stof(str);
        g_alpine_game_config.set_entity_sim_distance(new_dist);
        apply_entity_sim_distance();
    }
    catch (const std::exception& e) {
        xlog::info("Invalid simulation distance input: '{}', reason: {}", str, e.what());
    }
}
void ao_simdist_cbox_on_click(int x, int y)
{
    rf::ui::popup_message("Enter new simulation distance value:", "", ao_simdist_cbox_on_click_callback, 1);
}

void ao_mpcharlod_cbox_on_click(int x, int y) {
    g_alpine_game_config.multi_no_character_lod = !g_alpine_game_config.multi_no_character_lod;
    ao_mpcharlod_cbox.checked = !g_alpine_game_config.multi_no_character_lod;
    ao_play_button_snd(!g_alpine_game_config.multi_no_character_lod);
}

void ao_damagenum_cbox_on_click(int x, int y) {
    g_alpine_game_config.world_hud_damage_numbers = !g_alpine_game_config.world_hud_damage_numbers;
    ao_damagenum_cbox.checked = g_alpine_game_config.world_hud_damage_numbers;
    ao_play_button_snd(g_alpine_game_config.world_hud_damage_numbers);
}

void ao_hitsounds_cbox_on_click(int x, int y) {
    g_alpine_game_config.play_hit_sounds = !g_alpine_game_config.play_hit_sounds;
    ao_hitsounds_cbox.checked = g_alpine_game_config.play_hit_sounds;
    ao_play_button_snd(g_alpine_game_config.play_hit_sounds);
}

void ao_taunts_cbox_on_click(int x, int y) {
    g_alpine_game_config.play_taunt_sounds = !g_alpine_game_config.play_taunt_sounds;
    ao_taunts_cbox.checked = g_alpine_game_config.play_taunt_sounds;
    ao_play_button_snd(g_alpine_game_config.play_taunt_sounds);
}

void ao_teamrad_cbox_on_click(int x, int y) {
    g_alpine_game_config.play_team_rad_msg_sounds = !g_alpine_game_config.play_team_rad_msg_sounds;
    ao_teamrad_cbox.checked = g_alpine_game_config.play_team_rad_msg_sounds;
    ao_play_button_snd(g_alpine_game_config.play_team_rad_msg_sounds);
}

void ao_bombrng_cbox_on_click(int x, int y) {
    g_alpine_game_config.static_bomb_code = !g_alpine_game_config.static_bomb_code;
    ao_bombrng_cbox.checked = !g_alpine_game_config.static_bomb_code;
    ao_play_button_snd(!g_alpine_game_config.static_bomb_code);
}

void ao_exposuredamage_cbox_on_click(int x, int y) {
    g_alpine_game_config.apply_exposure_damage = !g_alpine_game_config.apply_exposure_damage;
    ao_exposuredamage_cbox.checked = g_alpine_game_config.apply_exposure_damage;
    ao_play_button_snd(g_alpine_game_config.apply_exposure_damage);
}

void ao_painsounds_cbox_on_click(int x, int y) {
    g_alpine_game_config.entity_pain_sounds = !g_alpine_game_config.entity_pain_sounds;
    ao_painsounds_cbox.checked = g_alpine_game_config.entity_pain_sounds;
    ao_play_button_snd(g_alpine_game_config.entity_pain_sounds);
}

void ao_togglecrouch_cbox_on_click(int x, int y) {
    rf::local_player->settings.toggle_crouch = !rf::local_player->settings.toggle_crouch;
    ao_togglecrouch_cbox.checked = rf::local_player->settings.toggle_crouch;
    ao_play_button_snd(rf::local_player->settings.toggle_crouch);
}

// gamepad settings
void ao_gyro_enabled_cbox_on_click(int x, int y) {
    if (!gamepad_is_motionsensors_supported()) return;
    g_alpine_game_config.gamepad_gyro_enabled = !g_alpine_game_config.gamepad_gyro_enabled;
    ao_gyro_enabled_cbox.checked = g_alpine_game_config.gamepad_gyro_enabled;
    ao_play_button_snd(g_alpine_game_config.gamepad_gyro_enabled);
    gyro_update_calibration_mode();
}

void ao_joy_sensitivity_cbox_on_click_callback() {
    char str_buffer[7] = "";
    rf::ui::popup_get_input(str_buffer, sizeof(str_buffer));
    std::string str = str_buffer;
    try {
        float val = std::stof(str);
        g_alpine_game_config.gamepad_joy_sensitivity = std::max(0.0f, val);
    }
    catch (const std::exception& e) {
        xlog::info("Invalid sensitivity input: '{}', reason: {}", str, e.what());
    }
}
void ao_joy_sensitivity_cbox_on_click(int x, int y) {
    rf::ui::popup_message("Enter new joy stick sensitivity value:", "", ao_joy_sensitivity_cbox_on_click_callback, 1);
}

void ao_move_deadzone_cbox_on_click_callback() {
    char str_buffer[7] = "";
    rf::ui::popup_get_input(str_buffer, sizeof(str_buffer));
    std::string str = str_buffer;
    try {
        float val = std::stof(str);
        g_alpine_game_config.gamepad_move_deadzone = std::clamp(val, 0.0f, 0.9f);
    }
    catch (const std::exception& e) {
        xlog::info("Invalid deadzone input: '{}', reason: {}", str, e.what());
    }
}
void ao_move_deadzone_cbox_on_click(int x, int y) {
    rf::ui::popup_message("Enter new joy move deadzone value (0.0 - 0.9):", "", ao_move_deadzone_cbox_on_click_callback, 1);
}

void ao_look_deadzone_cbox_on_click_callback() {
    char str_buffer[7] = "";
    rf::ui::popup_get_input(str_buffer, sizeof(str_buffer));
    std::string str = str_buffer;
    try {
        float val = std::stof(str);
        g_alpine_game_config.gamepad_look_deadzone = std::clamp(val, 0.0f, 0.9f);
    }
    catch (const std::exception& e) {
        xlog::info("Invalid deadzone input: '{}', reason: {}", str, e.what());
    }
}
void ao_look_deadzone_cbox_on_click(int x, int y) {
    rf::ui::popup_message("Enter new joy look deadzone value (0.0 - 0.9):", "", ao_look_deadzone_cbox_on_click_callback, 1);
}

void ao_gyro_sensitivity_cbox_on_click_callback() {
    char str_buffer[7] = "";
    rf::ui::popup_get_input(str_buffer, sizeof(str_buffer));
    std::string str = str_buffer;
    try {
        float val = std::stof(str);
        g_alpine_game_config.gamepad_gyro_sensitivity = std::clamp(val, 0.0f, 30.0f);
    }
    catch (const std::exception& e) {
        xlog::info("Invalid sensitivity input: '{}', reason: {}", str, e.what());
    }
}
void ao_gyro_sensitivity_cbox_on_click(int x, int y) {
    rf::ui::popup_message("Enter new gyro sensitivity value (0.0-30.00):", "", ao_gyro_sensitivity_cbox_on_click_callback, 1);
}

void ao_gyro_autocalibration_cbox_on_click([[maybe_unused]] int x, [[maybe_unused]] int y) {
    int mode = static_cast<int>(g_alpine_game_config.gamepad_gyro_autocalibration_mode);
    gyro_set_autocalibration_mode((mode + 1) % 3);
    ao_play_button_snd(true);
}

void ao_gyro_modifier_mode_cbox_on_click([[maybe_unused]] int x, [[maybe_unused]] int y) {
    g_alpine_game_config.gamepad_gyro_modifier_mode = (g_alpine_game_config.gamepad_gyro_modifier_mode + 1) % 4;
    ao_play_button_snd(true);
}

void ao_gyro_space_cbox_on_click([[maybe_unused]] int x, [[maybe_unused]] int y) {
    g_alpine_game_config.gamepad_gyro_space = (g_alpine_game_config.gamepad_gyro_space + 1) % 5;
    ao_play_button_snd(true);
}

void ao_gamepad_icon_override_cbox_on_click([[maybe_unused]] int x, [[maybe_unused]] int y) {
    g_alpine_game_config.gamepad_icon_override = (g_alpine_game_config.gamepad_icon_override + 1) % 11;
    ao_play_button_snd(true);
    g_alpine_options_hud_dirty = true;
}

void ao_input_prompt_mode_cbox_on_click([[maybe_unused]] int x, [[maybe_unused]] int y) {
    g_alpine_game_config.input_prompt_override = (g_alpine_game_config.input_prompt_override + 1) % 3;
    ao_play_button_snd(true);
    g_alpine_options_hud_dirty = true;
}

void ao_gyro_invert_y_cbox_on_click(int x, int y) {
    g_alpine_game_config.gamepad_gyro_invert_y = !g_alpine_game_config.gamepad_gyro_invert_y;
    ao_gyro_invert_y_cbox.checked = g_alpine_game_config.gamepad_gyro_invert_y;
    ao_play_button_snd(g_alpine_game_config.gamepad_gyro_invert_y);
}

void ao_gyro_vehicle_cbox_on_click(int x, int y) {
    g_alpine_game_config.gamepad_gyro_vehicle_camera = !g_alpine_game_config.gamepad_gyro_vehicle_camera;
    ao_gyro_vehicle_cbox.checked = g_alpine_game_config.gamepad_gyro_vehicle_camera;
    ao_play_button_snd(g_alpine_game_config.gamepad_gyro_vehicle_camera);
}

void ao_rumble_intensity_cbox_on_click_callback() {
    char str_buffer[7] = "";
    rf::ui::popup_get_input(str_buffer, sizeof(str_buffer));
    std::string str = str_buffer;
    try {
        float val = std::clamp(std::stof(str), 0.0f, 1.0f);
        g_alpine_game_config.gamepad_rumble_intensity = val;
    }
    catch (const std::exception& e) {
        xlog::info("Invalid intensity input: '{}', reason: {}", str, e.what());
    }
}
void ao_rumble_intensity_cbox_on_click(int x, int y) {
    rf::ui::popup_message("Enter new rumble intensity value (0.0-1.0):", "", ao_rumble_intensity_cbox_on_click_callback, 1);
}

void ao_rumble_trigger_cbox_on_click_callback() {
    char str_buffer[7] = "";
    rf::ui::popup_get_input(str_buffer, sizeof(str_buffer));
    std::string str = str_buffer;
    try {
        float val = std::clamp(std::stof(str), 0.0f, 1.0f);
        g_alpine_game_config.gamepad_trigger_rumble_intensity = val;
    }
    catch (const std::exception& e) {
        xlog::info("Invalid intensity input: '{}', reason: {}", str, e.what());
    }
}
void ao_rumble_trigger_cbox_on_click(int x, int y) {
    rf::ui::popup_message("Enter new trigger rumble intensity value (0.0-1.0):", "", ao_rumble_trigger_cbox_on_click_callback, 1);
}

void ao_rumble_filter_cbox_on_click([[maybe_unused]] int x, [[maybe_unused]] int y) {
    g_alpine_game_config.gamepad_rumble_vibration_filter = (g_alpine_game_config.gamepad_rumble_vibration_filter + 1) % 3;
    ao_play_button_snd(true);
}

void ao_rumble_weapon_cbox_on_click(int x, int y) {
    g_alpine_game_config.gamepad_weapon_rumble_enabled = !g_alpine_game_config.gamepad_weapon_rumble_enabled;
    ao_rumble_weapon_cbox.checked = g_alpine_game_config.gamepad_weapon_rumble_enabled;
    ao_play_button_snd(g_alpine_game_config.gamepad_weapon_rumble_enabled);
}

void ao_rumble_env_cbox_on_click(int x, int y) {
    g_alpine_game_config.gamepad_environmental_rumble_enabled = !g_alpine_game_config.gamepad_environmental_rumble_enabled;
    ao_rumble_env_cbox.checked = g_alpine_game_config.gamepad_environmental_rumble_enabled;
    ao_play_button_snd(g_alpine_game_config.gamepad_environmental_rumble_enabled);
}

void ao_rumble_primary_cbox_on_click(int x, int y) {
    g_alpine_game_config.gamepad_rumble_when_primary = !g_alpine_game_config.gamepad_rumble_when_primary;
    ao_rumble_primary_cbox.checked = g_alpine_game_config.gamepad_rumble_when_primary;
    ao_play_button_snd(g_alpine_game_config.gamepad_rumble_when_primary);
}

void ao_gyro_tightening_cbox_on_click_callback() {
    char str_buffer[7] = "";
    rf::ui::popup_get_input(str_buffer, sizeof(str_buffer));
    std::string str = str_buffer;
    try {
        float val = std::stof(str);
        g_alpine_game_config.gamepad_gyro_tightening = std::clamp(val, 0.0f, 100.0f);
    }
    catch (const std::exception& e) {
        xlog::info("Invalid tightening input: '{}', reason: {}", str, e.what());
    }
}
void ao_gyro_tightening_cbox_on_click(int x, int y) {
    rf::ui::popup_message("Enter new gyro tightening value (0.0-100.0):", "", ao_gyro_tightening_cbox_on_click_callback, 1);
}

void ao_gyro_smoothing_cbox_on_click_callback() {
    char str_buffer[7] = "";
    rf::ui::popup_get_input(str_buffer, sizeof(str_buffer));
    std::string str = str_buffer;
    try {
        float val = std::stof(str);
        g_alpine_game_config.gamepad_gyro_smoothing = std::clamp(val, 0.0f, 100.0f);
    }
    catch (const std::exception& e) {
        xlog::info("Invalid smoothing input: '{}', reason: {}", str, e.what());
    }
}
void ao_gyro_smoothing_cbox_on_click(int x, int y) {
    rf::ui::popup_message("Enter new gyro smoothing value (0.0-100.0):", "", ao_gyro_smoothing_cbox_on_click_callback, 1);
}

void ao_gyro_vh_mixer_cbox_on_click_callback() {
    char str_buffer[7] = "";
    rf::ui::popup_get_input(str_buffer, sizeof(str_buffer));
    std::string str = str_buffer;
    try {
        int val = std::stoi(str);
        g_alpine_game_config.gamepad_gyro_vh_mixer = std::clamp(val, -100, 100);
    }
    catch (const std::exception& e) {
        xlog::info("Invalid mixer input: '{}', reason: {}", str, e.what());
    }
}
void ao_gyro_vh_mixer_cbox_on_click(int x, int y) {
    rf::ui::popup_message("Enter Gyro V/H Mixer (Vertical: -100 to -1 , Horizontal: 1 to 100):", "", ao_gyro_vh_mixer_cbox_on_click_callback, 1);
}

void ao_joy_camera_cbox_on_click([[maybe_unused]] int x, [[maybe_unused]] int y) {
    g_alpine_game_config.gamepad_joy_camera = !g_alpine_game_config.gamepad_joy_camera;
    ao_play_button_snd(true);
}

void ao_flickstick_sweep_cbox_on_click_callback() {
    char str_buffer[7] = "";
    rf::ui::popup_get_input(str_buffer, sizeof(str_buffer));
    std::string str = str_buffer;
    try {
        float val = std::stof(str);
        g_alpine_game_config.gamepad_flickstick_sweep = std::clamp(val, 0.01f, 10.0f);
    }
    catch (const std::exception& e) {
        xlog::info("Invalid sweep input: '{}', reason: {}", str, e.what());
    }
}
void ao_flickstick_sweep_cbox_on_click(int x, int y) {
    rf::ui::popup_message("Enter new flick stick sweep value (0.01-10.0):", "", ao_flickstick_sweep_cbox_on_click_callback, 1);
}

void ao_flickstick_deadzone_cbox_on_click_callback() {
    char str_buffer[7] = "";
    rf::ui::popup_get_input(str_buffer, sizeof(str_buffer));
    std::string str = str_buffer;
    try {
        float val = std::stof(str);
        g_alpine_game_config.gamepad_flickstick_deadzone = std::clamp(val, 0.0f, 0.9f);
    }
    catch (const std::exception& e) {
        xlog::info("Invalid deadzone input: '{}', reason: {}", str, e.what());
    }
}
void ao_flickstick_deadzone_cbox_on_click(int x, int y) {
    rf::ui::popup_message("Enter new flick stick deadzone value (0.0-0.9):", "", ao_flickstick_deadzone_cbox_on_click_callback, 1);
}

void ao_flickstick_release_dz_cbox_on_click_callback() {
    char str_buffer[7] = "";
    rf::ui::popup_get_input(str_buffer, sizeof(str_buffer));
    std::string str = str_buffer;
    try {
        float val = std::stof(str);
        g_alpine_game_config.gamepad_flickstick_release_deadzone = std::clamp(val, 0.0f, 0.9f);
    }
    catch (const std::exception& e) {
        xlog::info("Invalid release deadzone input: '{}', reason: {}", str, e.what());
    }
}
void ao_flickstick_release_dz_cbox_on_click(int x, int y) {
    rf::ui::popup_message("Enter new flick stick release deadzone value (0.0-0.9):", "", ao_flickstick_release_dz_cbox_on_click_callback, 1);
}

void ao_flickstick_smoothing_cbox_on_click_callback() {
    char str_buffer[7] = "";
    rf::ui::popup_get_input(str_buffer, sizeof(str_buffer));
    std::string str = str_buffer;
    try {
        float val = std::stof(str);
        g_alpine_game_config.gamepad_flickstick_smoothing = std::clamp(val, 0.0f, 1.0f);
    }
    catch (const std::exception& e) {
        xlog::info("Invalid smoothing input: '{}', reason: {}", str, e.what());
    }
}
void ao_flickstick_smoothing_cbox_on_click(int x, int y) {
    rf::ui::popup_message("Enter new flick stick smoothing value (0.0-1.0):", "", ao_flickstick_smoothing_cbox_on_click_callback, 1);
}

void ao_joy_invert_y_cbox_on_click(int x, int y) {
    g_alpine_game_config.gamepad_joy_invert_y = !g_alpine_game_config.gamepad_joy_invert_y;
    ao_joy_invert_y_cbox.checked = g_alpine_game_config.gamepad_joy_invert_y;
    ao_play_button_snd(g_alpine_game_config.gamepad_joy_invert_y);
}

void ao_swap_sticks_cbox_on_click(int x, int y) {
    g_alpine_game_config.gamepad_swap_sticks = !g_alpine_game_config.gamepad_swap_sticks;
    ao_swap_sticks_cbox.checked = g_alpine_game_config.gamepad_swap_sticks;
    ao_play_button_snd(g_alpine_game_config.gamepad_swap_sticks);
}

void ao_joinbeep_cbox_on_click(int x, int y) {
    g_alpine_game_config.player_join_beep = !g_alpine_game_config.player_join_beep;
    ao_joinbeep_cbox.checked = g_alpine_game_config.player_join_beep;
    ao_play_button_snd(g_alpine_game_config.player_join_beep);
}

void ao_vsync_cbox_on_click(int x, int y) {
    g_alpine_system_config.vsync = !g_alpine_system_config.vsync;
    g_alpine_system_config.save();
    ao_vsync_cbox.checked = g_alpine_system_config.vsync;
    ao_play_button_snd(g_alpine_system_config.vsync);
    gr_d3d_update_vsync();
}

void ao_unclamplights_cbox_on_click(int x, int y) {
    g_alpine_game_config.full_range_lighting = !g_alpine_game_config.full_range_lighting;
    ao_unclamplights_cbox.checked = g_alpine_game_config.full_range_lighting;
    ao_play_button_snd(g_alpine_game_config.full_range_lighting);
}

void ao_globalrad_cbox_on_click(int x, int y) {
    g_alpine_game_config.play_global_rad_msg_sounds = !g_alpine_game_config.play_global_rad_msg_sounds;
    ao_globalrad_cbox.checked = g_alpine_game_config.play_global_rad_msg_sounds;
    ao_play_button_snd(g_alpine_game_config.play_global_rad_msg_sounds);
}

void ao_clicklimit_cbox_on_click(int x, int y) {
    g_alpine_game_config.unlimited_semi_auto = !g_alpine_game_config.unlimited_semi_auto;
    ao_clicklimit_cbox.checked = !g_alpine_game_config.unlimited_semi_auto;
    ao_play_button_snd(!g_alpine_game_config.unlimited_semi_auto);
}

void ao_gaussian_cbox_on_click(int x, int y) {
    g_alpine_game_config.gaussian_spread = !g_alpine_game_config.gaussian_spread;
    ao_gaussian_cbox.checked = g_alpine_game_config.gaussian_spread;
    ao_play_button_snd(g_alpine_game_config.gaussian_spread);
}

void ao_geochunk_cbox_on_click(int x, int y) {
    g_alpine_game_config.geo_chunk_physics = !g_alpine_game_config.geo_chunk_physics;
    ao_geochunk_cbox.checked = g_alpine_game_config.geo_chunk_physics;
    ao_play_button_snd(g_alpine_game_config.geo_chunk_physics);
}

void ao_autosave_cbox_on_click(int x, int y) {
    g_alpine_game_config.autosave = !g_alpine_game_config.autosave;
    ao_autosave_cbox.checked = g_alpine_game_config.autosave;
    ao_play_button_snd(g_alpine_game_config.autosave);
}

void ao_showfps_cbox_on_click(int x, int y) {
    g_alpine_game_config.fps_counter = !g_alpine_game_config.fps_counter;
    ao_showfps_cbox.checked = g_alpine_game_config.fps_counter;
    ao_play_button_snd(g_alpine_game_config.fps_counter);
}

void ao_showping_cbox_on_click(int x, int y) {
    g_alpine_game_config.ping_display = !g_alpine_game_config.ping_display;
    ao_showping_cbox.checked = g_alpine_game_config.ping_display;
    ao_play_button_snd(g_alpine_game_config.ping_display);
}

void ao_locpings_cbox_on_click(int x, int y) {
    g_alpine_game_config.show_location_pings = !g_alpine_game_config.show_location_pings;
    ao_locpings_cbox.checked = g_alpine_game_config.show_location_pings;
    ao_play_button_snd(g_alpine_game_config.show_location_pings);
}

void ao_redflash_cbox_on_click(int x, int y) {
    g_alpine_game_config.damage_screen_flash = !g_alpine_game_config.damage_screen_flash;
    ao_redflash_cbox.checked = g_alpine_game_config.damage_screen_flash;
    ao_play_button_snd(g_alpine_game_config.damage_screen_flash);
}

void ao_deathbars_cbox_on_click(int x, int y) {
    g_alpine_game_config.death_bars = !g_alpine_game_config.death_bars;
    ao_deathbars_cbox.checked = g_alpine_game_config.death_bars;
    ao_play_button_snd(g_alpine_game_config.death_bars);
}

void ao_ctfwh_cbox_on_click(int x, int y) {
    g_alpine_game_config.world_hud_ctf_icons = !g_alpine_game_config.world_hud_ctf_icons;
    ao_ctfwh_cbox.checked = g_alpine_game_config.world_hud_ctf_icons;
    ao_play_button_snd(g_alpine_game_config.world_hud_ctf_icons);
}

void ao_flag_overdrawwh_cbox_on_click(int x, int y) {
    g_alpine_game_config.world_hud_flag_overdraw = !g_alpine_game_config.world_hud_flag_overdraw;
    ao_flag_overdrawwh_cbox.checked = g_alpine_game_config.world_hud_flag_overdraw;
    ao_play_button_snd(g_alpine_game_config.world_hud_flag_overdraw);
}

void ao_hill_overdrawwh_cbox_on_click(int x, int y) {
    g_alpine_game_config.world_hud_hill_overdraw = !g_alpine_game_config.world_hud_hill_overdraw;
    ao_hill_overdrawwh_cbox.checked = g_alpine_game_config.world_hud_hill_overdraw;
    ao_play_button_snd(g_alpine_game_config.world_hud_hill_overdraw);
}

void ao_sbanim_cbox_on_click(int x, int y) {
    g_alpine_game_config.scoreboard_anim = !g_alpine_game_config.scoreboard_anim;
    ao_sbanim_cbox.checked = g_alpine_game_config.scoreboard_anim;
    ao_play_button_snd(g_alpine_game_config.scoreboard_anim);
}

void ao_teamlabels_cbox_on_click(int x, int y) {
    g_alpine_game_config.world_hud_team_player_labels = !g_alpine_game_config.world_hud_team_player_labels;
    ao_teamlabels_cbox.checked = g_alpine_game_config.world_hud_team_player_labels;
    ao_play_button_snd(g_alpine_game_config.world_hud_team_player_labels);
}

void ao_minimaltimer_cbox_on_click(int x, int y) {
    g_alpine_game_config.verbose_time_left_display = !g_alpine_game_config.verbose_time_left_display;
    ao_minimaltimer_cbox.checked = !g_alpine_game_config.verbose_time_left_display;
    build_time_left_string_format();
    ao_play_button_snd(!g_alpine_game_config.verbose_time_left_display);
}

void ao_targetnames_cbox_on_click(int x, int y) {
    g_alpine_game_config.display_target_player_names = !g_alpine_game_config.display_target_player_names;
    ao_targetnames_cbox.checked = g_alpine_game_config.display_target_player_names;
    ao_play_button_snd(g_alpine_game_config.display_target_player_names);
}

void ao_always_show_spectators_cbox_on_click(const int x, const int y) {
    g_alpine_game_config.always_show_spectators = !g_alpine_game_config.always_show_spectators;
    ao_always_show_spectators_cbox.checked = g_alpine_game_config.always_show_spectators;
    ao_play_button_snd(g_alpine_game_config.always_show_spectators);
}

void ao_staticscope_cbox_on_click(int x, int y) {
    g_alpine_game_config.scope_static_sensitivity = !g_alpine_game_config.scope_static_sensitivity;
    ao_staticscope_cbox.checked = g_alpine_game_config.scope_static_sensitivity;
    ao_play_button_snd(g_alpine_game_config.scope_static_sensitivity);
}

void ao_swapar_cbox_on_click(int x, int y) {
    g_alpine_game_config.swap_ar_controls = !g_alpine_game_config.swap_ar_controls;
    ao_swapar_cbox.checked = g_alpine_game_config.swap_ar_controls;
    ao_play_button_snd(g_alpine_game_config.swap_ar_controls);
}

void ao_swapgn_cbox_on_click(int x, int y) {
    g_alpine_game_config.swap_gn_controls = !g_alpine_game_config.swap_gn_controls;
    ao_swapgn_cbox.checked = g_alpine_game_config.swap_gn_controls;
    ao_play_button_snd(g_alpine_game_config.swap_gn_controls);
}

void ao_swapsg_cbox_on_click(int x, int y) {
    g_alpine_game_config.swap_sg_controls = !g_alpine_game_config.swap_sg_controls;
    ao_swapsg_cbox.checked = g_alpine_game_config.swap_sg_controls;
    ao_play_button_snd(g_alpine_game_config.swap_sg_controls);
}

void ao_camshake_cbox_on_click(int x, int y) {
    g_alpine_game_config.screen_shake_force_off = !g_alpine_game_config.screen_shake_force_off;
    ao_camshake_cbox.checked = !g_alpine_game_config.screen_shake_force_off;
    ao_play_button_snd(!g_alpine_game_config.screen_shake_force_off);
}

void ao_ricochet_cbox_on_click(int x, int y) {
    g_alpine_game_config.multi_ricochet = !g_alpine_game_config.multi_ricochet;
    ao_ricochet_cbox.checked = g_alpine_game_config.multi_ricochet;
    ao_play_button_snd(g_alpine_game_config.multi_ricochet);
}

void ao_firelights_cbox_on_click(int x, int y) {
    g_alpine_game_config.try_disable_muzzle_flash_lights = !g_alpine_game_config.try_disable_muzzle_flash_lights;
    ao_firelights_cbox.checked = !g_alpine_game_config.try_disable_muzzle_flash_lights;
    ao_play_button_snd(!g_alpine_game_config.try_disable_muzzle_flash_lights);
}

void ao_glares_cbox_on_click(int x, int y) {
    g_alpine_game_config.show_glares = !g_alpine_game_config.show_glares;
    ao_glares_cbox.checked = g_alpine_game_config.show_glares;
    ao_play_button_snd(g_alpine_game_config.show_glares);
}

void ao_nearest_cbox_on_click(int x, int y) {
    g_alpine_game_config.nearest_texture_filtering = !g_alpine_game_config.nearest_texture_filtering;
    ao_nearest_cbox.checked = g_alpine_game_config.nearest_texture_filtering;
    gr_update_texture_filtering();
    ao_play_button_snd(g_alpine_game_config.nearest_texture_filtering);
}

void ao_weapshake_cbox_on_click(int x, int y) {
    g_alpine_game_config.try_disable_weapon_shake = !g_alpine_game_config.try_disable_weapon_shake;
    ao_weapshake_cbox.checked = !g_alpine_game_config.try_disable_weapon_shake;
    evaluate_restrict_disable_ss();
    ao_play_button_snd(!g_alpine_game_config.try_disable_weapon_shake);
}

void ao_fullbrightchar_cbox_on_click(int x, int y) {
    g_alpine_game_config.try_fullbright_characters = !g_alpine_game_config.try_fullbright_characters;
    ao_fullbrightchar_cbox.checked = g_alpine_game_config.try_fullbright_characters;
    evaluate_fullbright_meshes();
    ao_play_button_snd(g_alpine_game_config.try_fullbright_characters);
}

void ao_notex_cbox_on_click(int x, int y) {
    g_alpine_game_config.try_disable_textures = !g_alpine_game_config.try_disable_textures;
    ao_notex_cbox.checked = g_alpine_game_config.try_disable_textures;
    evaluate_lightmaps_only();
    ao_play_button_snd(g_alpine_game_config.try_disable_textures);
}

static constexpr const char* meshlight_mode_names[] = {"Ambient", "Vertex", "Pixel"};

void ao_meshlight_cbox_on_click(int x, int y) {
    g_alpine_game_config.mesh_lighting_mode = (g_alpine_game_config.mesh_lighting_mode + 1) % 3;
    recalc_mesh_static_lighting();
    if (is_d3d11()) {
        df::gr::d3d11::evaluate_mesh_lighting(rf::level.filename);
    }
    snprintf(ao_meshlight_butlabel_text, sizeof(ao_meshlight_butlabel_text), "%s",
        meshlight_mode_names[g_alpine_game_config.mesh_lighting_mode]);
    ao_play_button_snd(g_alpine_game_config.mesh_lighting_mode > 0);
}

void ao_enemybullets_cbox_on_click(int x, int y) {
    g_alpine_game_config.show_enemy_bullets = !g_alpine_game_config.show_enemy_bullets;
    ao_enemybullets_cbox.checked = g_alpine_game_config.show_enemy_bullets;
    apply_show_enemy_bullets();
    ao_play_button_snd(g_alpine_game_config.show_enemy_bullets);
}

static rf::ui::Panel* ao_get_active_subpanel()
{
    switch (alpine_options_panel_current_tab) {
    case 0: return &alpine_options_panel0;
    case 1: return &alpine_options_panel1;
    case 2: return &alpine_options_panel2;
    case 3: return &alpine_options_panel3;
    default: return nullptr;
    }
}

static int ao_compute_max_scroll(rf::ui::Panel* subpanel)
{
    if (!subpanel) return 0;
    int max_bottom = 0;
    for (auto* g : alpine_options_panel_settings) {
        if (g && g->enabled && g->parent == subpanel)
            max_bottom = std::max(max_bottom, g->y + g->h);
    }
    for (auto* l : alpine_options_panel_labels) {
        if (l && l->enabled && l->parent == subpanel)
            max_bottom = std::max(max_bottom, l->y + l->h);
    }
    int raw_max = std::max(0, max_bottom - AO_CONTENT_BOTTOM);
    return (raw_max / AO_SCROLL_STEP) * AO_SCROLL_STEP;
}

void alpine_options_panel_handle_key(rf::Key* key){
    if (*key == rf::Key::KEY_ESC) {
        rf::ui::options_close_current_panel();
        rf::snd_play(43, 0, 0.0f, 1.0f);
        return;
    }
}

void alpine_options_panel_handle_mouse(int x, int y) {
    for (auto* gadget : alpine_options_panel_settings)
        if (gadget) gadget->highlighted = false;

    if (g_sb_dragging) {
        if (!rf::mouse_button_is_down(0)) {
            g_sb_dragging = false;
        } else {
            if (g_sb_visible && g_sb_ph > g_sb_thumb_h) {
                const int tab = alpine_options_panel_current_tab;
                int max_scroll = ao_compute_max_scroll(ao_get_active_subpanel());
                int drag_delta = y - g_sb_drag_origin_y;
                int scroll_range_px = g_sb_ph - g_sb_thumb_h;
                int raw = g_sb_drag_origin_scroll + drag_delta * max_scroll / scroll_range_px;
                int snapped = (raw / AO_SCROLL_STEP) * AO_SCROLL_STEP;
                alpine_options_scroll_offsets[tab] = std::clamp(snapped, 0, max_scroll);
            }
            return;
        }
    }

    if (g_sb_visible && rf::mouse_was_button_pressed(0)) {
        const bool in_track = (x >= g_sb_x && x < g_sb_x + g_sb_pw
                            && y >= g_sb_y && y < g_sb_y + g_sb_ph);
        if (in_track) {
            const bool in_thumb = (y >= g_sb_thumb_y && y < g_sb_thumb_y + g_sb_thumb_h);
            if (in_thumb) {
                g_sb_dragging           = true;
                g_sb_drag_origin_y      = y;
                g_sb_drag_origin_scroll = alpine_options_scroll_offsets[alpine_options_panel_current_tab];
            } else {
                const int tab = alpine_options_panel_current_tab;
                int max_scroll = ao_compute_max_scroll(ao_get_active_subpanel());
                int step = (y < g_sb_thumb_y) ? -(AO_SCROLL_STEP * 5) : (AO_SCROLL_STEP * 5);
                alpine_options_scroll_offsets[tab] = std::clamp(
                    alpine_options_scroll_offsets[tab] + step, 0, max_scroll);
            }
            return;
        }
    }

    int dx = 0, dy = 0, dz = 0;
    rf::mouse_get_delta(dx, dy, dz);
    if (dz == 0)
        dz = gamepad_consume_menu_scroll();
    if (dz != 0 && alpine_options_panel_scrollable[alpine_options_panel_current_tab]) {
        const int tab = alpine_options_panel_current_tab;
        int step = (dz > 0) ? -AO_SCROLL_STEP : AO_SCROLL_STEP;
        int max_scroll = ao_compute_max_scroll(ao_get_active_subpanel());
        alpine_options_scroll_offsets[tab] = std::clamp(alpine_options_scroll_offsets[tab] + step, 0, max_scroll);
    }

    rf::ui::Panel* active_subpanel = ao_get_active_subpanel();
    int current_scroll = alpine_options_scroll_offsets[alpine_options_panel_current_tab];

    int hovered_index = -1;

    for (size_t i = 0; i < alpine_options_panel_settings.size(); ++i) {
        auto* gadget = alpine_options_panel_settings[i];
        if (gadget && gadget->enabled) {
            int scroll_adj = (active_subpanel && gadget->parent == active_subpanel) ? current_scroll : 0;
            int abs_x = static_cast<int>(gadget->get_absolute_x() * rf::ui::scale_x);
            int abs_y = static_cast<int>((gadget->get_absolute_y() - scroll_adj) * rf::ui::scale_y);
            int abs_w = static_cast<int>(gadget->w * rf::ui::scale_x);
            int abs_h = static_cast<int>(gadget->h * rf::ui::scale_y);

            if (x >= abs_x && x <= abs_x + abs_w &&
                y >= abs_y && y <= abs_y + abs_h) {
                hovered_index = static_cast<int>(i);
                break;
            }
        }
    }
    if (hovered_index >= 0) {
        auto* gadget = alpine_options_panel_settings[hovered_index];
        if (rf::mouse_was_button_pressed(0)) {
            if (gadget->on_click)
                gadget->on_click(x, y);
        } else if (rf::mouse_button_is_down(0) && gadget->on_mouse_btn_down) {
            gadget->on_mouse_btn_down(x, y);
        }
    }

    if (hovered_index >= 0) {
        alpine_options_panel_settings[hovered_index]->highlighted = true;
    }
}

void alpine_options_panel_checkbox_init(rf::ui::Checkbox* checkbox, rf::ui::Label* label, rf::ui::Panel* parent_panel,
    void (*on_click)(int, int), bool checked, int x, int y, std::string label_text) {
    checkbox->create("checkbox.tga", "checkbox_selected.tga", "checkbox_checked.tga", x, y, 45, "", 0);
    checkbox->parent = parent_panel;
    checkbox->checked = checked;
    checkbox->on_click = on_click;
    checkbox->enabled = true;
    alpine_options_panel_settings.push_back(checkbox);

    label->create(parent_panel, x + 87, y + 6, label_text.c_str(), rf::ui::medium_font_0);
    label->enabled = true;
    alpine_options_panel_labels.push_back(label);
}

void alpine_options_panel_inputbox_init(rf::ui::Checkbox* checkbox, rf::ui::Label* label, rf::ui::Label* but_label,
    rf::ui::Panel* parent_panel, void (*on_click)(int, int), int x, int y, std::string label_text) {
    checkbox->create("ao_smbut1.tga", "ao_smbut1_hover.tga", "ao_tab.tga", x, y, 45, "106.26", 0);
    checkbox->parent = parent_panel;
    checkbox->checked = false;
    checkbox->on_click = on_click;
    checkbox->enabled = true;
    alpine_options_panel_settings.push_back(checkbox);

    label->create(parent_panel, x + 87, y + 6, label_text.c_str(), rf::ui::medium_font_0);
    label->enabled = true;
    alpine_options_panel_labels.push_back(label);

    but_label->create(parent_panel, x + 32, y + 6, "", rf::ui::medium_font_0);
    but_label->clr = {255, 255, 255, 255};
    but_label->enabled = true;
    alpine_options_panel_labels.push_back(but_label);
}

void alpine_options_panel_tab_init(rf::ui::Checkbox* tab_button, rf::ui::Label* tab_label,
    void (*on_click)(int, int), bool checked, int x, int y, int text_offset, std::string tab_label_text) {
    tab_button->create("ao_tab.tga", "ao_tab_hover.tga", "ao_tab.tga", x, y, 0, "", 0);
    tab_button->parent = &alpine_options_panel;
    tab_button->checked = false;
    tab_button->on_click = on_click;
    tab_button->enabled = true;
    alpine_options_panel_settings.push_back(tab_button);

    tab_label->create(&alpine_options_panel, x + text_offset, y + 22, tab_label_text.c_str(), rf::ui::medium_font_0);
    tab_label->enabled = true;
    alpine_options_panel_tab_labels.push_back(tab_label);
}

void alpine_options_panel_init() {
    // reset per-tab scroll positions whenever the panel is (re-)opened
    std::fill(std::begin(alpine_options_scroll_offsets), std::end(alpine_options_scroll_offsets), 0);

    // panels
    alpine_options_panel.create("alpine_options_panelp.tga", rf::ui::options_panel_x, rf::ui::options_panel_y);
    alpine_options_panel0.create("alpine_options_panel0.tga", 0, 0);
    alpine_options_panel0.parent = &alpine_options_panel;
    alpine_options_panel1.create("alpine_options_panel1.tga", 0, 0);
    alpine_options_panel1.parent = &alpine_options_panel;
    alpine_options_panel2.create("alpine_options_panel2.tga", 0, 0);
    alpine_options_panel2.parent = &alpine_options_panel;
    alpine_options_panel3.create("alpine_options_panel3.tga", 0, 0);
    alpine_options_panel3.parent = &alpine_options_panel;

    // tabs
    alpine_options_panel_tab_init(
        &ao_tab_0_cbox, &ao_tab_0_label, ao_tab_button_on_click_0, alpine_options_panel_current_tab == 0, 107, 0, 27, "Visual");
    alpine_options_panel_tab_init(
        &ao_tab_1_cbox, &ao_tab_1_label, ao_tab_button_on_click_1, alpine_options_panel_current_tab == 1, 199, 0, 18, "Interface");
    alpine_options_panel_tab_init(
        &ao_tab_2_cbox, &ao_tab_2_label, ao_tab_button_on_click_2, alpine_options_panel_current_tab == 2, 291, 0, 29, "Input");
    alpine_options_panel_tab_init(
        &ao_tab_3_cbox, &ao_tab_3_label, ao_tab_button_on_click_3, alpine_options_panel_current_tab == 3, 383, 0, 30, "Misc");

    // panel 0
    alpine_options_panel_checkbox_init(
        &ao_enemybullets_cbox, &ao_enemybullets_label, &alpine_options_panel0, ao_enemybullets_cbox_on_click, g_alpine_game_config.show_enemy_bullets, 112, 54, "Enemy bullets");
    alpine_options_panel_checkbox_init(
        &ao_notex_cbox, &ao_notex_label, &alpine_options_panel0, ao_notex_cbox_on_click, g_alpine_game_config.try_disable_textures, 112, 84, "Lightmaps only");
    alpine_options_panel_checkbox_init(
        &ao_weapshake_cbox, &ao_weapshake_label, &alpine_options_panel0, ao_weapshake_cbox_on_click, !g_alpine_game_config.try_disable_weapon_shake, 112, 114, "Weapon shake");
    alpine_options_panel_inputbox_init(
        &ao_fov_cbox, &ao_fov_label, &ao_fov_butlabel, &alpine_options_panel0, ao_fov_cbox_on_click, 112, 144, "Horizontal FOV");
    alpine_options_panel_inputbox_init(
        &ao_fpfov_cbox, &ao_fpfov_label, &ao_fpfov_butlabel, &alpine_options_panel0, ao_fpfov_cbox_on_click, 112, 174, "Gun FOV mod");
    alpine_options_panel_inputbox_init(
        &ao_maxfps_cbox, &ao_maxfps_label, &ao_maxfps_butlabel, &alpine_options_panel0, ao_maxfps_cbox_on_click, 112, 204, "Max FPS");
    alpine_options_panel_inputbox_init(
        &ao_simdist_cbox, &ao_simdist_label, &ao_simdist_butlabel, &alpine_options_panel0, ao_simdist_cbox_on_click, 112, 234, "Simulation dist");
    alpine_options_panel_inputbox_init(
        &ao_loddist_cbox, &ao_loddist_label, &ao_loddist_butlabel, &alpine_options_panel0, ao_loddist_cbox_on_click, 112, 262, "LOD scale");
    alpine_options_panel_checkbox_init(
        &ao_unclamplights_cbox, &ao_unclamplights_label, &alpine_options_panel0, ao_unclamplights_cbox_on_click, g_alpine_game_config.full_range_lighting, 112, 292, "Full light range");

    alpine_options_panel_checkbox_init(
        &ao_camshake_cbox, &ao_camshake_label, &alpine_options_panel0, ao_camshake_cbox_on_click, !g_alpine_game_config.screen_shake_force_off, 280, 54, "View shake (SP)");
    alpine_options_panel_checkbox_init(
        &ao_ricochet_cbox, &ao_ricochet_label, &alpine_options_panel0, ao_ricochet_cbox_on_click, g_alpine_game_config.multi_ricochet, 280, 84, "Ricochet FX (MP)");
    alpine_options_panel_checkbox_init(
        &ao_fullbrightchar_cbox, &ao_fullbrightchar_label, &alpine_options_panel0, ao_fullbrightchar_cbox_on_click, g_alpine_game_config.try_fullbright_characters, 280, 114, "Fullbright models");
    alpine_options_panel_inputbox_init(
        &ao_meshlight_cbox, &ao_meshlight_label, &ao_meshlight_butlabel, &alpine_options_panel0, ao_meshlight_cbox_on_click, 280, 144, "Mesh lighting");
    alpine_options_panel_checkbox_init(
        &ao_nearest_cbox, &ao_nearest_label, &alpine_options_panel0, ao_nearest_cbox_on_click, g_alpine_game_config.nearest_texture_filtering, 280, 174, "Nearest filtering");
    alpine_options_panel_checkbox_init(
        &ao_glares_cbox, &ao_glares_label, &alpine_options_panel0, ao_glares_cbox_on_click, g_alpine_game_config.show_glares, 280, 204, "Light glares");
    alpine_options_panel_checkbox_init(
        &ao_firelights_cbox, &ao_firelights_label, &alpine_options_panel0, ao_firelights_cbox_on_click, !g_alpine_game_config.try_disable_muzzle_flash_lights, 280, 234, "Muzzle lights");
    alpine_options_panel_checkbox_init(
        &ao_mpcharlod_cbox, &ao_mpcharlod_label, &alpine_options_panel0, ao_mpcharlod_cbox_on_click, !g_alpine_game_config.multi_no_character_lod, 280, 262, "Entity LOD (MP)");
    alpine_options_panel_checkbox_init(
        &ao_vsync_cbox, &ao_vsync_label, &alpine_options_panel0, ao_vsync_cbox_on_click, g_alpine_system_config.vsync, 280, 292, "Vertical sync");

    // panel 1
    alpine_options_panel_checkbox_init(
        &ao_bighud_cbox, &ao_bighud_label, &alpine_options_panel1, ao_bighud_cbox_on_click, g_alpine_game_config.big_hud, 112, 54, "Big HUD");
    alpine_options_panel_checkbox_init(
        &ao_damagenum_cbox, &ao_damagenum_label, &alpine_options_panel1, ao_damagenum_cbox_on_click, g_alpine_game_config.world_hud_damage_numbers, 112, 84, "Hit numbers");
    alpine_options_panel_checkbox_init(
        &ao_showfps_cbox, &ao_showfps_label, &alpine_options_panel1, ao_showfps_cbox_on_click, g_alpine_game_config.fps_counter, 112, 114, "Show FPS");
    alpine_options_panel_checkbox_init(
        &ao_showping_cbox, &ao_showping_label, &alpine_options_panel1, ao_showping_cbox_on_click, g_alpine_game_config.ping_display, 112, 144, "Show ping");
    alpine_options_panel_checkbox_init(
        &ao_redflash_cbox, &ao_redflash_label, &alpine_options_panel1, ao_redflash_cbox_on_click, g_alpine_game_config.damage_screen_flash, 112, 174, "Damage flash");
    alpine_options_panel_checkbox_init(
        &ao_deathbars_cbox, &ao_deathbars_label, &alpine_options_panel1, ao_deathbars_cbox_on_click, g_alpine_game_config.death_bars, 112, 204, "Death bars");
    alpine_options_panel_checkbox_init(
        &ao_locpings_cbox, &ao_locpings_label, &alpine_options_panel1, ao_locpings_cbox_on_click, g_alpine_game_config.show_location_pings, 112, 234, "Location pings");
    alpine_options_panel_inputbox_init(
        &ao_retscale_cbox, &ao_retscale_label, &ao_retscale_butlabel, &alpine_options_panel1, ao_retscale_cbox_on_click, 112, 262, "Reticle scale");

    alpine_options_panel_checkbox_init(
        &ao_ctfwh_cbox, &ao_ctfwh_label, &alpine_options_panel1, ao_ctfwh_cbox_on_click, g_alpine_game_config.world_hud_ctf_icons, 280, 54, "CTF icons");
    alpine_options_panel_checkbox_init(
        &ao_flag_overdrawwh_cbox, &ao_flag_overdrawwh_label, &alpine_options_panel1, ao_flag_overdrawwh_cbox_on_click, g_alpine_game_config.world_hud_flag_overdraw, 280, 84, "Overdraw flags");
    alpine_options_panel_checkbox_init(
        &ao_hill_overdrawwh_cbox, &ao_hill_overdrawwh_label, &alpine_options_panel1, ao_hill_overdrawwh_cbox_on_click, g_alpine_game_config.world_hud_hill_overdraw, 280, 114, "Overdraw CPs");
    alpine_options_panel_checkbox_init(
        &ao_sbanim_cbox, &ao_sbanim_label, &alpine_options_panel1, ao_sbanim_cbox_on_click, g_alpine_game_config.scoreboard_anim, 280, 144, "Scoreboard anim");
    alpine_options_panel_checkbox_init(
        &ao_teamlabels_cbox, &ao_teamlabels_label, &alpine_options_panel1, ao_teamlabels_cbox_on_click, g_alpine_game_config.world_hud_team_player_labels, 280, 174, "Label teammates");
    alpine_options_panel_checkbox_init(
        &ao_minimaltimer_cbox, &ao_minimaltimer_label, &alpine_options_panel1, ao_minimaltimer_cbox_on_click, !g_alpine_game_config.verbose_time_left_display, 280, 204, "Minimal timer");
    alpine_options_panel_checkbox_init(
        &ao_targetnames_cbox, &ao_targetnames_label, &alpine_options_panel1, ao_targetnames_cbox_on_click, g_alpine_game_config.display_target_player_names, 280, 234, "Target names");
    alpine_options_panel_checkbox_init(
        &ao_always_show_spectators_cbox, &ao_always_show_spectators_label, &alpine_options_panel1, ao_always_show_spectators_cbox_on_click, g_alpine_game_config.always_show_spectators, 280, 264, "Show spectators");

    alpine_options_panel_inputbox_init(
        &ao_input_prompt_mode_cbox, &ao_input_prompt_mode_label, &ao_input_prompt_mode_butlabel,
        &alpine_options_panel1, ao_input_prompt_mode_cbox_on_click, 280, 294, "Input Glyph");
    ao_input_prompt_mode_butlabel.x -= 8;
    alpine_options_panel_inputbox_init(
        &ao_gamepad_icon_override_cbox, &ao_gamepad_icon_override_label, &ao_gamepad_icon_override_butlabel,
        &alpine_options_panel1, ao_gamepad_icon_override_cbox_on_click, 280, 324, "Gamepad Glyph");
    ao_gamepad_icon_override_butlabel.x -= 8;

    // panel 2
    alpine_options_panel_checkbox_init(
        &ao_dinput_cbox, &ao_dinput_label, &alpine_options_panel2, ao_dinput_cbox_on_click, g_alpine_game_config.direct_input, 112, 54, "DirectInput"); 
    alpine_options_panel_checkbox_init(
        &ao_linearpitch_cbox, &ao_linearpitch_label, &alpine_options_panel2, ao_linearpitch_cbox_on_click, g_alpine_game_config.mouse_linear_pitch, 112, 84, "Linear pitch");
    alpine_options_panel_checkbox_init(
        &ao_swapar_cbox, &ao_swapar_label, &alpine_options_panel2, ao_swapar_cbox_on_click, g_alpine_game_config.swap_ar_controls, 112, 114, "Swap AR");
    alpine_options_panel_checkbox_init(
        &ao_swapgn_cbox, &ao_swapgn_label, &alpine_options_panel2, ao_swapgn_cbox_on_click, g_alpine_game_config.swap_gn_controls, 112, 144, "Swap Grenade");
    alpine_options_panel_checkbox_init(
        &ao_swapsg_cbox, &ao_swapsg_label, &alpine_options_panel2, ao_swapsg_cbox_on_click, g_alpine_game_config.swap_sg_controls, 112, 174, "Swap Shotgun");

    alpine_options_panel_inputbox_init(
        &ao_ms_cbox, &ao_ms_label, &ao_ms_butlabel, &alpine_options_panel2, ao_ms_cbox_on_click, 280, 54, "Mouse sensitivity");
    alpine_options_panel_inputbox_init(
        &ao_scannersens_cbox, &ao_scannersens_label, &ao_scannersens_butlabel, &alpine_options_panel2, ao_scannersens_cbox_on_click, 280, 84, "Scanner modifier");
    alpine_options_panel_inputbox_init(
        &ao_scopesens_cbox, &ao_scopesens_label, &ao_scopesens_butlabel, &alpine_options_panel2, ao_scopesens_cbox_on_click, 280, 114, "Scope modifier");
    alpine_options_panel_checkbox_init(
        &ao_staticscope_cbox, &ao_staticscope_label, &alpine_options_panel2, ao_staticscope_cbox_on_click, g_alpine_game_config.scope_static_sensitivity, 280, 144, "Linear scope");
    alpine_options_panel_checkbox_init(
        &ao_togglecrouch_cbox, &ao_togglecrouch_label, &alpine_options_panel2, ao_togglecrouch_cbox_on_click, rf::local_player->settings.toggle_crouch, 280, 174, "Toggle crouch");
    alpine_options_panel_inputbox_init(
        &ao_joy_camera_cbox, &ao_joy_camera_label, &ao_joy_camera_butlabel, &alpine_options_panel2, ao_joy_camera_cbox_on_click, 112, 234, "Joy cam modes");
    alpine_options_panel_inputbox_init(
        &ao_joy_sensitivity_cbox, &ao_joy_sensitivity_label, &ao_joy_sensitivity_butlabel, &alpine_options_panel2, ao_joy_sensitivity_cbox_on_click, 112, 234, "Joy sensitivity");
    alpine_options_panel_inputbox_init(
        &ao_flickstick_sweep_cbox, &ao_flickstick_sweep_label, &ao_flickstick_sweep_butlabel, &alpine_options_panel2, ao_flickstick_sweep_cbox_on_click, 112, 234, "Flick sweep");
    alpine_options_panel_inputbox_init(
        &ao_flickstick_deadzone_cbox, &ao_flickstick_deadzone_label, &ao_flickstick_deadzone_butlabel, &alpine_options_panel2, ao_flickstick_deadzone_cbox_on_click, 112, 294, "Flick deadzone");
    alpine_options_panel_inputbox_init(
        &ao_flickstick_release_dz_cbox, &ao_flickstick_release_dz_label, &ao_flickstick_release_dz_butlabel, &alpine_options_panel2, ao_flickstick_release_dz_cbox_on_click, 112, 324, "Flick release dz");
    alpine_options_panel_inputbox_init(
        &ao_flickstick_smoothing_cbox, &ao_flickstick_smoothing_label, &ao_flickstick_smoothing_butlabel, &alpine_options_panel2, ao_flickstick_smoothing_cbox_on_click, 112, 354, "Flick smoothing");
    alpine_options_panel_inputbox_init(
        &ao_move_deadzone_cbox, &ao_move_deadzone_label, &ao_move_deadzone_butlabel, &alpine_options_panel2, ao_move_deadzone_cbox_on_click, 112, 204, "Joy move dz");
    alpine_options_panel_inputbox_init(
        &ao_look_deadzone_cbox, &ao_look_deadzone_label, &ao_look_deadzone_butlabel, &alpine_options_panel2, ao_look_deadzone_cbox_on_click, 112, 294, "Joy cam dz");
    alpine_options_panel_checkbox_init(
        &ao_joy_invert_y_cbox, &ao_joy_invert_y_label, &alpine_options_panel2, ao_joy_invert_y_cbox_on_click, g_alpine_game_config.gamepad_joy_invert_y, 112, 384, "Joy cam Y-Invert");
    alpine_options_panel_checkbox_init(
        &ao_swap_sticks_cbox, &ao_swap_sticks_label, &alpine_options_panel2, ao_swap_sticks_cbox_on_click, g_alpine_game_config.gamepad_swap_sticks, 112, 414, "Swap joysticks");
    alpine_options_panel_inputbox_init(
        &ao_joy_scannersens_cbox, &ao_joy_scannersens_label, &ao_joy_scannersens_butlabel, &alpine_options_panel2, ao_joy_scannersens_cbox_on_click, 112, 324, "Joy scanner mod");
    alpine_options_panel_inputbox_init(
        &ao_joy_scopesens_cbox, &ao_joy_scopesens_label, &ao_joy_scopesens_butlabel, &alpine_options_panel2, ao_joy_scopesens_cbox_on_click, 112, 354, "Joy scope mod");
    alpine_options_panel_checkbox_init(
        &ao_gyro_enabled_cbox, &ao_gyro_enabled_label, &alpine_options_panel2, ao_gyro_enabled_cbox_on_click, g_alpine_game_config.gamepad_gyro_enabled, 280, 204, "Gyro aiming");
    alpine_options_panel_inputbox_init(
        &ao_gyro_sensitivity_cbox, &ao_gyro_sensitivity_label, &ao_gyro_sensitivity_butlabel, &alpine_options_panel2, ao_gyro_sensitivity_cbox_on_click, 280, 234, "Gyro sensitivity");
    alpine_options_panel_checkbox_init(
        &ao_gyro_invert_y_cbox, &ao_gyro_invert_y_label,
        &alpine_options_panel2, ao_gyro_invert_y_cbox_on_click,
        g_alpine_game_config.gamepad_gyro_invert_y, 280, 264, "Gyro Y-Invert");
    alpine_options_panel_checkbox_init(
        &ao_gyro_vehicle_cbox, &ao_gyro_vehicle_label,
        &alpine_options_panel2, ao_gyro_vehicle_cbox_on_click,
        g_alpine_game_config.gamepad_gyro_vehicle_camera, 280, 294, "Gyro vehicle cam");
    alpine_options_panel_inputbox_init(
        &ao_gyro_space_cbox, &ao_gyro_space_label, &ao_gyro_space_butlabel,
        &alpine_options_panel2, ao_gyro_space_cbox_on_click, 280, 294, "Gyro space");
    alpine_options_panel_inputbox_init(
        &ao_gyro_autocalibration_cbox, &ao_gyro_autocalibration_label, &ao_gyro_autocalibration_butlabel,
        &alpine_options_panel2, ao_gyro_autocalibration_cbox_on_click, 280, 324, "Gyro auto-calib");
    alpine_options_panel_inputbox_init(
        &ao_gyro_modifier_mode_cbox, &ao_gyro_modifier_mode_label, &ao_gyro_modifier_mode_butlabel,
        &alpine_options_panel2, ao_gyro_modifier_mode_cbox_on_click, 280, 354, "Gyro modifier");
    alpine_options_panel_inputbox_init(
        &ao_gyro_tightening_cbox, &ao_gyro_tightening_label, &ao_gyro_tightening_butlabel,
        &alpine_options_panel2, ao_gyro_tightening_cbox_on_click, 280, 354, "Gyro tightening");
    alpine_options_panel_inputbox_init(
        &ao_gyro_smoothing_cbox, &ao_gyro_smoothing_label, &ao_gyro_smoothing_butlabel,
        &alpine_options_panel2, ao_gyro_smoothing_cbox_on_click, 280, 384, "Gyro smoothing");
    alpine_options_panel_inputbox_init(
        &ao_gyro_vh_mixer_cbox, &ao_gyro_vh_mixer_label, &ao_gyro_vh_mixer_butlabel,
        &alpine_options_panel2, ao_gyro_vh_mixer_cbox_on_click, 280, 414, "Gyro V/H mixer");
    alpine_options_panel_inputbox_init(
        &ao_gyro_scannersens_cbox, &ao_gyro_scannersens_label, &ao_gyro_scannersens_butlabel,
        &alpine_options_panel2, ao_gyro_scannersens_cbox_on_click, 280, 414, "Gyro scanner mod");
    alpine_options_panel_inputbox_init(
        &ao_gyro_scopesens_cbox, &ao_gyro_scopesens_label, &ao_gyro_scopesens_butlabel,
        &alpine_options_panel2, ao_gyro_scopesens_cbox_on_click, 280, 444, "Gyro scope mod");
    alpine_options_panel_inputbox_init(
        &ao_rumble_intensity_cbox, &ao_rumble_intensity_label, &ao_rumble_intensity_butlabel,
        &alpine_options_panel2, ao_rumble_intensity_cbox_on_click, 280, 474, "Rumble intensity");
    alpine_options_panel_inputbox_init(
        &ao_rumble_trigger_cbox, &ao_rumble_trigger_label, &ao_rumble_trigger_butlabel,
        &alpine_options_panel2, ao_rumble_trigger_cbox_on_click, 280, 504, "Trigger rumble");
    alpine_options_panel_inputbox_init(
        &ao_rumble_filter_cbox, &ao_rumble_filter_label, &ao_rumble_filter_butlabel,
        &alpine_options_panel2, ao_rumble_filter_cbox_on_click, 280, 534, "Vibration filter");
    alpine_options_panel_checkbox_init(
        &ao_rumble_weapon_cbox, &ao_rumble_weapon_label,
        &alpine_options_panel2, ao_rumble_weapon_cbox_on_click, g_alpine_game_config.gamepad_weapon_rumble_enabled, 280, 564, "Weapon rumble");
    alpine_options_panel_checkbox_init(
        &ao_rumble_env_cbox, &ao_rumble_env_label,
        &alpine_options_panel2, ao_rumble_env_cbox_on_click, g_alpine_game_config.gamepad_environmental_rumble_enabled, 280, 594, "Environ. rumble");
    alpine_options_panel_checkbox_init(
        &ao_rumble_primary_cbox, &ao_rumble_primary_label,
        &alpine_options_panel2, ao_rumble_primary_cbox_on_click, g_alpine_game_config.gamepad_rumble_when_primary, 280, 624, "Rumble priority");

    // panel 3
    alpine_options_panel_checkbox_init(
        &ao_hitsounds_cbox, &ao_hitsounds_label, &alpine_options_panel3, ao_hitsounds_cbox_on_click, g_alpine_game_config.play_hit_sounds, 112, 54, "Hit sounds");
    alpine_options_panel_checkbox_init(
        &ao_taunts_cbox, &ao_taunts_label, &alpine_options_panel3, ao_taunts_cbox_on_click, g_alpine_game_config.play_taunt_sounds, 112, 84, "Taunt sounds");
    alpine_options_panel_checkbox_init(
        &ao_autosave_cbox, &ao_autosave_label, &alpine_options_panel3, ao_autosave_cbox_on_click, g_alpine_game_config.autosave, 112, 114, "Autosave");
    alpine_options_panel_checkbox_init(
        &ao_joinbeep_cbox, &ao_joinbeep_label, &alpine_options_panel3, ao_joinbeep_cbox_on_click, g_alpine_game_config.player_join_beep, 112, 144, "Join beep");
    alpine_options_panel_checkbox_init(
        &ao_painsounds_cbox, &ao_painsounds_label, &alpine_options_panel3, ao_painsounds_cbox_on_click, g_alpine_game_config.entity_pain_sounds, 112, 174, "Pain sounds");
    alpine_options_panel_checkbox_init(
        &ao_geochunk_cbox, &ao_geochunk_label, &alpine_options_panel3, ao_geochunk_cbox_on_click, g_alpine_game_config.geo_chunk_physics, 112, 204, "Geo chunks");

    alpine_options_panel_checkbox_init(
        &ao_teamrad_cbox, &ao_teamrad_label, &alpine_options_panel3, ao_teamrad_cbox_on_click, g_alpine_game_config.play_team_rad_msg_sounds, 280, 54, "Team radio msgs");
    alpine_options_panel_checkbox_init(
        &ao_globalrad_cbox, &ao_globalrad_label, &alpine_options_panel3, ao_globalrad_cbox_on_click, g_alpine_game_config.play_global_rad_msg_sounds, 280, 84, "Global radio msgs");
    alpine_options_panel_checkbox_init(
        &ao_gaussian_cbox, &ao_gaussian_label, &alpine_options_panel3, ao_gaussian_cbox_on_click, g_alpine_game_config.gaussian_spread, 280, 114, "Gaussian spread");
    alpine_options_panel_checkbox_init(
        &ao_bombrng_cbox, &ao_bombrng_label, &alpine_options_panel3, ao_bombrng_cbox_on_click, !g_alpine_game_config.static_bomb_code, 280, 144, "Randomize bomb");
    alpine_options_panel_checkbox_init(
        &ao_exposuredamage_cbox, &ao_exposuredamage_label, &alpine_options_panel3, ao_exposuredamage_cbox_on_click, g_alpine_game_config.apply_exposure_damage, 280, 174, "Exposure damage");

    // fflink text (panel3)
    std::string fflink_username = g_game_config.fflink_username.value();
    std::string fflink_label_text1 = "";
    std::string fflink_label_text2 = "";
    std::string fflink_label_text3 = "";
    if (fflink_username.empty()) {
        fflink_label_text1 = "Alpine Faction is NOT linked to a FactionFiles account!";
        fflink_label_text2 = "Linking enables achievements and map ratings.";
        fflink_label_text3 = "Visit alpinefaction.com/link to link your account.";
    }
    else {
        fflink_label_text1 = "";
        fflink_label_text2 = "Alpine Faction is linked to FactionFiles as " + fflink_username;
        fflink_label_text3 = "";
    }

    ao_fflink_label1.create(&alpine_options_panel3, 125, 304, fflink_label_text1.c_str(), rf::ui::medium_font_0);
    ao_fflink_label1.enabled = true;
    alpine_options_panel_labels.push_back(&ao_fflink_label1);
    ao_fflink_label2.create(&alpine_options_panel3, 125, 319, fflink_label_text2.c_str(), rf::ui::medium_font_0);
    ao_fflink_label2.enabled = true;
    alpine_options_panel_labels.push_back(&ao_fflink_label2);
    ao_fflink_label3.create(&alpine_options_panel3, 125, 334, fflink_label_text3.c_str(), rf::ui::medium_font_0);
    ao_fflink_label3.enabled = true;
    alpine_options_panel_labels.push_back(&ao_fflink_label3);
}

void alpine_options_panel_do_frame(int x)
{
    // render parent panel
    alpine_options_panel.x = x;
    alpine_options_panel.render();

    // render selected panel
    switch (alpine_options_panel_current_tab) {
    case 1:
        alpine_options_panel1.x = 0;
        alpine_options_panel1.render();
        alpine_options_panel0.x = 10000;
        alpine_options_panel2.x = 10000;
        alpine_options_panel3.x = 10000;
        break;
    case 2:
        alpine_options_panel2.x = 0;
        alpine_options_panel2.render();
        alpine_options_panel0.x = 10000;
        alpine_options_panel1.x = 10000;
        alpine_options_panel3.x = 10000;
        break;
    case 3:
        alpine_options_panel3.x = 0;
        alpine_options_panel3.render();
        alpine_options_panel0.x = 10000;
        alpine_options_panel1.x = 10000;
        alpine_options_panel2.x = 10000;
        break;
    case 0:
    default:
        alpine_options_panel0.x = 0;
        alpine_options_panel0.render();
        alpine_options_panel1.x = 10000;
        alpine_options_panel2.x = 10000;
        alpine_options_panel3.x = 10000;
        break;
    }

    // prepare scroll state for this frame
    rf::ui::Panel* active_subpanel = ao_get_active_subpanel();
    int& current_scroll = alpine_options_scroll_offsets[alpine_options_panel_current_tab];

    // render all gadgets – tab buttons are unscrolled; content items get a Y offset
    for (auto* ui_element : alpine_options_panel_settings) {
        if (ui_element) {
            auto checkbox = static_cast<rf::ui::Checkbox*>(ui_element);
            if (!checkbox || !checkbox->enabled) continue;
            if (checkbox->parent == active_subpanel) {
                // skip items whose top edge is outside the content viewport
                int scrolled_y = checkbox->y - current_scroll;
                if (scrolled_y < AO_CONTENT_TOP || scrolled_y >= AO_CONTENT_BOTTOM)
                    continue;
                checkbox->y = scrolled_y;
                checkbox->render();
                checkbox->y = scrolled_y + current_scroll;
            } else {
                checkbox->render();
            }
        }
    }

    // render tab labels
    for (auto* ui_label : alpine_options_panel_tab_labels) {
        if (ui_label) {
            ui_label->render();
        }
    }

    // set dynamic strings for button labels
    // fov
    if (g_alpine_game_config.horz_fov == 0.0f) {
        snprintf(ao_fov_butlabel_text, sizeof(ao_fov_butlabel_text), " auto ");
    }
    else {
        snprintf(ao_fov_butlabel_text, sizeof(ao_fov_butlabel_text), "%6.2f", g_alpine_game_config.horz_fov);
    }
    ao_fov_butlabel.text = ao_fov_butlabel_text;

    // fpgun fov
    snprintf(ao_fpfov_butlabel_text, sizeof(ao_fpfov_butlabel_text), "%6.2f", g_alpine_game_config.fpgun_fov_scale);
    ao_fpfov_butlabel.text = ao_fpfov_butlabel_text;

    // ms
    snprintf(ao_ms_butlabel_text, sizeof(ao_ms_butlabel_text), "%6.4f", rf::local_player->settings.controls.mouse_sensitivity);
    ao_ms_butlabel.text = ao_ms_butlabel_text;

    // scanner ms
    snprintf(ao_scannersens_butlabel_text, sizeof(ao_scannersens_butlabel_text), "%6.4f", g_alpine_game_config.scanner_sensitivity_modifier);
    ao_scannersens_butlabel.text = ao_scannersens_butlabel_text;

    // scope ms
    snprintf(ao_scopesens_butlabel_text, sizeof(ao_scopesens_butlabel_text), "%6.4f", g_alpine_game_config.scope_sensitivity_modifier);
    ao_scopesens_butlabel.text = ao_scopesens_butlabel_text;

    // ret scale
    snprintf(ao_retscale_butlabel_text, sizeof(ao_retscale_butlabel_text), "%6.2f", g_alpine_game_config.get_reticle_scale());
    ao_retscale_butlabel.text = ao_retscale_butlabel_text;

    // max fps
    snprintf(ao_maxfps_butlabel_text, sizeof(ao_maxfps_butlabel_text), "%u", g_alpine_game_config.max_fps);
    ao_maxfps_butlabel.text = ao_maxfps_butlabel_text;

    // lod dist
    snprintf(ao_loddist_butlabel_text, sizeof(ao_loddist_butlabel_text), "%6.2f", g_alpine_game_config.lod_dist_scale);
    ao_loddist_butlabel.text = ao_loddist_butlabel_text;

    // simulation dist
    snprintf(ao_simdist_butlabel_text, sizeof(ao_simdist_butlabel_text), "%6.2f", g_alpine_game_config.entity_sim_distance);
    ao_simdist_butlabel.text = ao_simdist_butlabel_text;

    // mesh lighting
    snprintf(ao_meshlight_butlabel_text, sizeof(ao_meshlight_butlabel_text), "%s",
        meshlight_mode_names[std::clamp(g_alpine_game_config.mesh_lighting_mode, 0, 2)]);
    ao_meshlight_butlabel.text = ao_meshlight_butlabel_text;

    // gamepad settings
    snprintf(ao_joy_camera_butlabel_text, sizeof(ao_joy_camera_butlabel_text), "%s",
        g_alpine_game_config.gamepad_joy_camera ? "Flick Stick" : "Standard");
    ao_joy_camera_butlabel.text  = ao_joy_camera_butlabel_text;
    ao_joy_camera_butlabel.align = rf::gr::ALIGN_CENTER;

    snprintf(ao_flickstick_sweep_butlabel_text, sizeof(ao_flickstick_sweep_butlabel_text), "%6.4f", g_alpine_game_config.gamepad_flickstick_sweep);
    ao_flickstick_sweep_butlabel.text = ao_flickstick_sweep_butlabel_text;

    snprintf(ao_flickstick_deadzone_butlabel_text, sizeof(ao_flickstick_deadzone_butlabel_text), "%6.4f", g_alpine_game_config.gamepad_flickstick_deadzone);
    ao_flickstick_deadzone_butlabel.text = ao_flickstick_deadzone_butlabel_text;

    snprintf(ao_flickstick_release_dz_butlabel_text, sizeof(ao_flickstick_release_dz_butlabel_text), "%6.4f", g_alpine_game_config.gamepad_flickstick_release_deadzone);
    ao_flickstick_release_dz_butlabel.text = ao_flickstick_release_dz_butlabel_text;

    snprintf(ao_flickstick_smoothing_butlabel_text, sizeof(ao_flickstick_smoothing_butlabel_text), "%6.4f", g_alpine_game_config.gamepad_flickstick_smoothing);
    ao_flickstick_smoothing_butlabel.text = ao_flickstick_smoothing_butlabel_text;

    snprintf(ao_joy_sensitivity_butlabel_text, sizeof(ao_joy_sensitivity_butlabel_text), "%6.4f", g_alpine_game_config.gamepad_joy_sensitivity);
    ao_joy_sensitivity_butlabel.text = ao_joy_sensitivity_butlabel_text;

    snprintf(ao_move_deadzone_butlabel_text, sizeof(ao_move_deadzone_butlabel_text), "%6.4f", g_alpine_game_config.gamepad_move_deadzone);
    ao_move_deadzone_butlabel.text = ao_move_deadzone_butlabel_text;

    snprintf(ao_look_deadzone_butlabel_text, sizeof(ao_look_deadzone_butlabel_text), "%6.4f", g_alpine_game_config.gamepad_look_deadzone);
    ao_look_deadzone_butlabel.text = ao_look_deadzone_butlabel_text;

    snprintf(ao_gyro_sensitivity_butlabel_text, sizeof(ao_gyro_sensitivity_butlabel_text), "%6.4f", g_alpine_game_config.gamepad_gyro_sensitivity);
    ao_gyro_sensitivity_butlabel.text = ao_gyro_sensitivity_butlabel_text;

    snprintf(ao_gyro_tightening_butlabel_text, sizeof(ao_gyro_tightening_butlabel_text), "%6.4f", g_alpine_game_config.gamepad_gyro_tightening);
    ao_gyro_tightening_butlabel.text = ao_gyro_tightening_butlabel_text;

    snprintf(ao_gyro_smoothing_butlabel_text, sizeof(ao_gyro_smoothing_butlabel_text), "%6.4f", g_alpine_game_config.gamepad_gyro_smoothing);
    ao_gyro_smoothing_butlabel.text = ao_gyro_smoothing_butlabel_text;

    {
        int v = g_alpine_game_config.gamepad_gyro_vh_mixer;
        if (v == 0)
            snprintf(ao_gyro_vh_mixer_butlabel_text, sizeof(ao_gyro_vh_mixer_butlabel_text), "0%%");
        else if (v > 0)
            snprintf(ao_gyro_vh_mixer_butlabel_text, sizeof(ao_gyro_vh_mixer_butlabel_text), "%d%% H", v);
        else
            snprintf(ao_gyro_vh_mixer_butlabel_text, sizeof(ao_gyro_vh_mixer_butlabel_text), "%d%% V", v);
    }
    ao_gyro_vh_mixer_butlabel.text = ao_gyro_vh_mixer_butlabel_text;
    ao_gyro_vh_mixer_butlabel.align = rf::gr::ALIGN_CENTER;

    snprintf(ao_joy_scannersens_butlabel_text, sizeof(ao_joy_scannersens_butlabel_text), "%6.4f", g_alpine_game_config.gamepad_scanner_sensitivity_modifier);
    ao_joy_scannersens_butlabel.text = ao_joy_scannersens_butlabel_text;

    snprintf(ao_joy_scopesens_butlabel_text, sizeof(ao_joy_scopesens_butlabel_text), "%6.4f", g_alpine_game_config.gamepad_scope_sensitivity_modifier);
    ao_joy_scopesens_butlabel.text = ao_joy_scopesens_butlabel_text;

    snprintf(ao_gyro_scannersens_butlabel_text, sizeof(ao_gyro_scannersens_butlabel_text), "%6.4f", g_alpine_game_config.gamepad_scanner_gyro_sensitivity_modifier);
    ao_gyro_scannersens_butlabel.text = ao_gyro_scannersens_butlabel_text;

    snprintf(ao_gyro_scopesens_butlabel_text, sizeof(ao_gyro_scopesens_butlabel_text), "%6.4f", g_alpine_game_config.gamepad_scope_gyro_sensitivity_modifier);
    ao_gyro_scopesens_butlabel.text = ao_gyro_scopesens_butlabel_text;

    const char* mode_name = "(unknown)";
    switch (std::clamp(g_alpine_game_config.gamepad_gyro_autocalibration_mode, 0, 2)) {
    case 0:
        mode_name = "Off";
        break;
    case 1:
        mode_name = "Menu";
        break;
    case 2:
        mode_name = "Always";
        break;
    }

    snprintf(ao_gyro_autocalibration_butlabel_text, sizeof(ao_gyro_autocalibration_butlabel_text), "%s", mode_name);
    ao_gyro_autocalibration_butlabel.text  = ao_gyro_autocalibration_butlabel_text;
    ao_gyro_autocalibration_butlabel.align = rf::gr::ALIGN_CENTER;

    static const char* gyro_modifier_mode_names[] = {"Always", "Hold (Off)", "Hold (On)", "Toggle"};
    snprintf(ao_gyro_modifier_mode_butlabel_text, sizeof(ao_gyro_modifier_mode_butlabel_text), "%s",
        gyro_modifier_mode_names[std::clamp(g_alpine_game_config.gamepad_gyro_modifier_mode, 0, 3)]);
    ao_gyro_modifier_mode_butlabel.text  = ao_gyro_modifier_mode_butlabel_text;
    ao_gyro_modifier_mode_butlabel.align = rf::gr::ALIGN_CENTER;

    snprintf(ao_gyro_space_butlabel_text, sizeof(ao_gyro_space_butlabel_text), "%s", gyro_get_space_name(g_alpine_game_config.gamepad_gyro_space));
    ao_gyro_space_butlabel.text  = ao_gyro_space_butlabel_text;
    ao_gyro_space_butlabel.align = rf::gr::ALIGN_CENTER;

    snprintf(ao_rumble_intensity_butlabel_text, sizeof(ao_rumble_intensity_butlabel_text), "%6.4f", g_alpine_game_config.gamepad_rumble_intensity);
    ao_rumble_intensity_butlabel.text = ao_rumble_intensity_butlabel_text;

    snprintf(ao_rumble_trigger_butlabel_text, sizeof(ao_rumble_trigger_butlabel_text), "%6.4f", g_alpine_game_config.gamepad_trigger_rumble_intensity);
    ao_rumble_trigger_butlabel.text = ao_rumble_trigger_butlabel_text;

    static const char* rumble_filter_names[] = {"Off", "Auto", "On"};
    snprintf(ao_rumble_filter_butlabel_text, sizeof(ao_rumble_filter_butlabel_text), "%s",
        rumble_filter_names[std::clamp(g_alpine_game_config.gamepad_rumble_vibration_filter, 0, 2)]);
    ao_rumble_filter_butlabel.text = ao_rumble_filter_butlabel_text;
    ao_rumble_filter_butlabel.align = rf::gr::ALIGN_CENTER;

    static const char* gamepad_icon_names[] = {"auto", "generic", "xbox360", "xboxone", "ds3", "ds4", "dualsense", "ns switch", "gamecube", "sc1", "sd"};
    int gamepad_icon_index = std::clamp(g_alpine_game_config.gamepad_icon_override, 0, 10);
    if (gamepad_icon_index == 0) {
        snprintf(ao_gamepad_icon_override_butlabel_text, sizeof(ao_gamepad_icon_override_butlabel_text), " auto ");
    } else {
        snprintf(ao_gamepad_icon_override_butlabel_text, sizeof(ao_gamepad_icon_override_butlabel_text), "%s", gamepad_icon_names[gamepad_icon_index]);
    }
    ao_gamepad_icon_override_butlabel.text = ao_gamepad_icon_override_butlabel_text;
    ao_gamepad_icon_override_butlabel.align = rf::gr::ALIGN_CENTER;

    static const char* input_prompt_names[] = {"auto", "gamepad", "kb/mouse"};
    int input_prompt_index = std::clamp(g_alpine_game_config.input_prompt_override, 0, 2);
    if (input_prompt_index == 0) {
        snprintf(ao_input_prompt_mode_butlabel_text, sizeof(ao_input_prompt_mode_butlabel_text), " auto ");
    } else {
        snprintf(ao_input_prompt_mode_butlabel_text, sizeof(ao_input_prompt_mode_butlabel_text), "%s", input_prompt_names[input_prompt_index]);
    }
    ao_input_prompt_mode_butlabel.text = ao_input_prompt_mode_butlabel_text;
    ao_input_prompt_mode_butlabel.align = rf::gr::ALIGN_CENTER;

    ao_joy_camera_butlabel.x             = 112 + 50;
    ao_gyro_vh_mixer_butlabel.x          = 280 + 50;
    ao_gyro_space_butlabel.x             = 280 + 50;
    ao_gyro_autocalibration_butlabel.x   = 280 + 50;
    ao_gyro_modifier_mode_butlabel.x     = 280 + 50;
    ao_rumble_filter_butlabel.x          = 280 + 50;
    ao_gamepad_icon_override_butlabel.x  = 280 + 50;
    ao_input_prompt_mode_butlabel.x      = 280 + 50;

    // show/hide gyro ui if gamepad supports motion sensors and (for subcontrols) gyro aiming is enabled
    bool gyro_hw = gamepad_is_motionsensors_supported();
    bool gyro_enabled = gyro_hw && g_alpine_game_config.gamepad_gyro_enabled;

    ao_gyro_enabled_cbox.enabled         = gyro_hw;
    ao_gyro_enabled_label.enabled        = gyro_hw;

    ao_gyro_sensitivity_cbox.enabled     = gyro_enabled;
    ao_gyro_sensitivity_label.enabled    = gyro_enabled;
    ao_gyro_sensitivity_butlabel.enabled = gyro_enabled;

    ao_gyro_autocalibration_cbox.enabled   = gyro_hw;
    ao_gyro_autocalibration_label.enabled  = gyro_hw;
    ao_gyro_autocalibration_butlabel.enabled = gyro_hw;

    ao_gyro_modifier_mode_cbox.enabled     = gyro_enabled;
    ao_gyro_modifier_mode_label.enabled    = gyro_enabled;
    ao_gyro_modifier_mode_butlabel.enabled = gyro_enabled;

    ao_gyro_invert_y_cbox.enabled        = gyro_enabled;
    ao_gyro_invert_y_label.enabled       = gyro_enabled;
    ao_gyro_vehicle_cbox.enabled         = gyro_enabled;
    ao_gyro_vehicle_label.enabled        = gyro_enabled;
    ao_gyro_space_cbox.enabled           = gyro_enabled;
    ao_gyro_space_label.enabled          = gyro_enabled;
    ao_gyro_space_butlabel.enabled       = gyro_enabled;
    ao_gyro_tightening_cbox.enabled      = gyro_enabled;
    ao_gyro_tightening_label.enabled     = gyro_enabled;
    ao_gyro_tightening_butlabel.enabled  = gyro_enabled;
    ao_gyro_smoothing_cbox.enabled       = gyro_enabled;
    ao_gyro_smoothing_label.enabled      = gyro_enabled;
    ao_gyro_smoothing_butlabel.enabled   = gyro_enabled;
    ao_gyro_vh_mixer_cbox.enabled        = gyro_enabled;
    ao_gyro_vh_mixer_label.enabled       = gyro_enabled;
    ao_gyro_vh_mixer_butlabel.enabled    = gyro_enabled;
    ao_gyro_scannersens_cbox.enabled     = gyro_enabled;
    ao_gyro_scannersens_label.enabled    = gyro_enabled;
    ao_gyro_scannersens_butlabel.enabled = gyro_enabled;
    ao_gyro_scopesens_cbox.enabled       = gyro_enabled;
    ao_gyro_scopesens_label.enabled      = gyro_enabled;
    ao_gyro_scopesens_butlabel.enabled   = gyro_enabled;

    ao_rumble_intensity_cbox.enabled     = true;
    ao_rumble_intensity_label.enabled    = true;
    ao_rumble_intensity_butlabel.enabled = true;
    bool trigger_rumble_hw = gamepad_is_trigger_rumble_supported();
    ao_rumble_trigger_cbox.enabled       = trigger_rumble_hw;
    ao_rumble_trigger_label.enabled      = trigger_rumble_hw;
    ao_rumble_trigger_butlabel.enabled   = trigger_rumble_hw;
    ao_rumble_filter_cbox.enabled        = true;
    ao_rumble_filter_label.enabled       = true;
    ao_rumble_filter_butlabel.enabled    = true;
    ao_rumble_weapon_cbox.enabled        = true;
    ao_rumble_weapon_label.enabled       = true;
    ao_rumble_env_cbox.enabled           = true;
    ao_rumble_env_label.enabled          = true;
    ao_rumble_primary_cbox.enabled       = true;
    ao_rumble_primary_label.enabled      = true;

    ao_joy_scannersens_cbox.enabled      = true;
    ao_joy_scannersens_label.enabled     = true;
    ao_joy_scannersens_butlabel.enabled  = true;
    ao_joy_scopesens_cbox.enabled        = true;
    ao_joy_scopesens_label.enabled       = true;
    ao_joy_scopesens_butlabel.enabled    = true;

    // dynamic right-column layout: pack gyro and rumble items tightly based on what's active
    {
        AoColumn rc{204};
        rc.add_checkbox(ao_gyro_enabled_cbox, ao_gyro_enabled_label, gyro_hw);
        rc.add_checkbox(ao_gyro_vehicle_cbox, ao_gyro_vehicle_label, gyro_enabled);
        rc.add_inputbox(ao_gyro_sensitivity_cbox, ao_gyro_sensitivity_label, ao_gyro_sensitivity_butlabel, gyro_enabled);
        rc.add_inputbox(ao_gyro_scopesens_cbox, ao_gyro_scopesens_label, ao_gyro_scopesens_butlabel, gyro_enabled);
        rc.add_inputbox(ao_gyro_scannersens_cbox, ao_gyro_scannersens_label, ao_gyro_scannersens_butlabel, gyro_enabled);
        rc.add_inputbox(ao_gyro_modifier_mode_cbox, ao_gyro_modifier_mode_label, ao_gyro_modifier_mode_butlabel, gyro_enabled);
        rc.add_inputbox(ao_gyro_tightening_cbox, ao_gyro_tightening_label, ao_gyro_tightening_butlabel, gyro_enabled);
        rc.add_inputbox(ao_gyro_smoothing_cbox, ao_gyro_smoothing_label, ao_gyro_smoothing_butlabel, gyro_enabled);
        rc.add_inputbox(ao_gyro_vh_mixer_cbox, ao_gyro_vh_mixer_label, ao_gyro_vh_mixer_butlabel, gyro_enabled);
        rc.add_inputbox(ao_gyro_space_cbox, ao_gyro_space_label, ao_gyro_space_butlabel, gyro_enabled);
        rc.add_checkbox(ao_gyro_invert_y_cbox, ao_gyro_invert_y_label, gyro_enabled);
        rc.add_inputbox(ao_gyro_autocalibration_cbox, ao_gyro_autocalibration_label, ao_gyro_autocalibration_butlabel, gyro_hw);
        rc.add_inputbox(ao_rumble_intensity_cbox, ao_rumble_intensity_label, ao_rumble_intensity_butlabel);
        rc.add_inputbox(ao_rumble_trigger_cbox, ao_rumble_trigger_label, ao_rumble_trigger_butlabel, trigger_rumble_hw);
        rc.add_checkbox(ao_rumble_primary_cbox, ao_rumble_primary_label);
        rc.add_inputbox(ao_rumble_filter_cbox, ao_rumble_filter_label, ao_rumble_filter_butlabel);
        rc.add_checkbox(ao_rumble_weapon_cbox, ao_rumble_weapon_label);
        rc.add_checkbox(ao_rumble_env_cbox, ao_rumble_env_label);
    }

    ao_joy_camera_cbox.enabled      = true;
    ao_joy_camera_label.enabled     = true;
    ao_joy_camera_butlabel.enabled  = true;

     // toggle regular stick vs flick stick controls, then reflow left column Y positions to avoid dead space
    bool flick_stick = g_alpine_game_config.gamepad_joy_camera;
    // Joy sensitivity is always shown: as "Joy sensitivity" in joystick mode, "Joy sens (misc)" in flick-stick mode.
    snprintf(ao_joy_sensitivity_label_text, sizeof(ao_joy_sensitivity_label_text), "%s",
        flick_stick ? "Joy sens (misc)" : "Joy sensitivity");
    ao_joy_sensitivity_label.text          = ao_joy_sensitivity_label_text;
    ao_joy_sensitivity_cbox.enabled        = true;
    ao_joy_sensitivity_label.enabled       = true;
    ao_joy_sensitivity_butlabel.enabled    = true;
    ao_look_deadzone_cbox.enabled          = !flick_stick;
    ao_look_deadzone_label.enabled         = !flick_stick;
    ao_look_deadzone_butlabel.enabled      = !flick_stick;
    ao_flickstick_sweep_cbox.enabled       = flick_stick;
    ao_flickstick_sweep_label.enabled      = flick_stick;
    ao_flickstick_sweep_butlabel.enabled   = flick_stick;
    ao_flickstick_deadzone_cbox.enabled    = flick_stick;
    ao_flickstick_deadzone_label.enabled   = flick_stick;
    ao_flickstick_deadzone_butlabel.enabled = flick_stick;
    ao_flickstick_release_dz_cbox.enabled    = flick_stick;
    ao_flickstick_release_dz_label.enabled   = flick_stick;
    ao_flickstick_release_dz_butlabel.enabled = flick_stick;
    ao_flickstick_smoothing_cbox.enabled     = flick_stick;
    ao_flickstick_smoothing_label.enabled    = flick_stick;
    ao_flickstick_smoothing_butlabel.enabled = flick_stick;
    ao_joy_invert_y_cbox.enabled           = !flick_stick;
    ao_joy_invert_y_label.enabled          = !flick_stick;
    ao_swap_sticks_cbox.enabled            = true;
    ao_swap_sticks_label.enabled           = true;

    // dynamic left-column layout: pack items tightly based on active mode
    {
        AoColumn lc{264};
        lc.add_inputbox(ao_flickstick_sweep_cbox, ao_flickstick_sweep_label, ao_flickstick_sweep_butlabel, flick_stick);
        lc.add_inputbox(ao_joy_sensitivity_cbox, ao_joy_sensitivity_label, ao_joy_sensitivity_butlabel);
        lc.add_inputbox(ao_flickstick_deadzone_cbox, ao_flickstick_deadzone_label, ao_flickstick_deadzone_butlabel, flick_stick);
        lc.add_inputbox(ao_flickstick_release_dz_cbox, ao_flickstick_release_dz_label, ao_flickstick_release_dz_butlabel, flick_stick);
        lc.add_inputbox(ao_flickstick_smoothing_cbox, ao_flickstick_smoothing_label, ao_flickstick_smoothing_butlabel, flick_stick);
        lc.add_inputbox(ao_look_deadzone_cbox, ao_look_deadzone_label, ao_look_deadzone_butlabel, !flick_stick);
        lc.add_inputbox(ao_joy_scannersens_cbox, ao_joy_scannersens_label, ao_joy_scannersens_butlabel);
        lc.add_inputbox(ao_joy_scopesens_cbox, ao_joy_scopesens_label, ao_joy_scopesens_butlabel);
        lc.add_checkbox(ao_joy_invert_y_cbox, ao_joy_invert_y_label, !flick_stick);
        lc.add_checkbox(ao_swap_sticks_cbox, ao_swap_sticks_label);
    }

    ao_gamepad_icon_override_cbox.enabled      = true;
    ao_gamepad_icon_override_label.enabled     = true;
    ao_gamepad_icon_override_butlabel.enabled  = true;

    ao_input_prompt_mode_cbox.enabled      = true;
    ao_input_prompt_mode_label.enabled     = true;
    ao_input_prompt_mode_butlabel.enabled  = true;

    // clamp scroll after all dynamic layout passes have updated item positions
    const int max_scroll = ao_compute_max_scroll(active_subpanel);
    current_scroll = std::clamp(current_scroll, 0, max_scroll);

    for (auto* ui_label : alpine_options_panel_labels) {
        if (!ui_label || !ui_label->enabled) continue;
        if (active_subpanel && ui_label->parent == active_subpanel) {
            // clip against row base (label y is item_y+6) so labels never orphan at viewport edges
            if ((ui_label->y - 6 - current_scroll) < AO_CONTENT_TOP ||
                (ui_label->y - 6 - current_scroll) >= AO_CONTENT_BOTTOM)
                continue;
            int scrolled_y = ui_label->y - current_scroll;
            ui_label->y = scrolled_y;
            ui_label->render();
            ui_label->y = scrolled_y + current_scroll;
        } else {
            ui_label->render();
        }
    }

    g_sb_visible = false;
    if (alpine_options_panel_scrollable[alpine_options_panel_current_tab] && max_scroll > 0) {
        constexpr int   sb_width     = 6;
        constexpr int   sb_margin    = 39;
        constexpr int   AO_PANEL_W   = 512;
        constexpr int   sb_y_offset  = 10;
        constexpr float viewport_h_l = static_cast<float>(AO_CONTENT_BOTTOM - AO_CONTENT_TOP);
        const float total_h          = static_cast<float>(max_scroll) + viewport_h_l;

        const int sb_x  = static_cast<int>((x + AO_PANEL_W - sb_margin - sb_width) * rf::ui::scale_x);
        const int sb_y  = static_cast<int>((alpine_options_panel.y + AO_CONTENT_TOP + sb_y_offset) * rf::ui::scale_y);
        const int sb_pw = static_cast<int>(sb_width      * rf::ui::scale_x);
        const int sb_ph = static_cast<int>(viewport_h_l  * rf::ui::scale_y);

        rf::gr::set_color(40, 40, 40, 180);
        rf::gr::rect(sb_x, sb_y, sb_pw, sb_ph);

        const float thumb_h_f    = viewport_h_l * (viewport_h_l / total_h);
        const float scroll_ratio = static_cast<float>(current_scroll) / static_cast<float>(max_scroll);
        const int thumb_y = sb_y + static_cast<int>(scroll_ratio * (sb_ph - thumb_h_f * rf::ui::scale_y));
        const int thumb_h = std::max(static_cast<int>(4 * rf::ui::scale_y),
                                     static_cast<int>(thumb_h_f * rf::ui::scale_y));

        rf::gr::set_color(0, 200, 210, 255);
        rf::gr::rect(sb_x, thumb_y, sb_pw, thumb_h);

        g_sb_x = sb_x;       g_sb_y = sb_y;
        g_sb_pw = sb_pw;     g_sb_ph = sb_ph;
        g_sb_thumb_y = thumb_y; g_sb_thumb_h = thumb_h;
        g_sb_visible = true;
    }
}

static void options_alpine_on_click() {
    constexpr int alpine_options_panel_id = 4;

    if (rf::ui::options_current_panel == alpine_options_panel_id) {
        if (g_alpine_options_hud_dirty) {
            hud_refresh_action_tokens();
            g_alpine_options_hud_dirty = false;
        }
        rf::ui::options_close_current_panel();
        return;
    }

    rf::ui::options_menu_tab_move_anim_speed = -rf::ui::menu_move_anim_speed;
    rf::ui::options_incoming_panel = alpine_options_panel_id;
    rf::ui::options_set_panel_open(); // Transition to new panel
}

// build alpine options button
CodeInjection options_init_build_button_patch{
    0x0044F038,
    [](auto& regs) {
        //xlog::warn("Creating new button...");
        alpine_options_btn.init();
        alpine_options_btn.create("button_more.tga", "button_selected.tga", 0, 0, 99, "ADVANCED", rf::ui::medium_font_0);
        alpine_options_btn.key = 0x2E;
        alpine_options_btn.enabled = true;
    },
};

// build new gadgets array
CodeInjection options_init_build_button_array_patch{
    0x0044F051,
    [](auto& regs) {
        regs.ecx += 0x4; // realignment

        // fetch stock buttons, add to new array
        rf::ui::Gadget** old_gadgets = reinterpret_cast<rf::ui::Gadget**>(0x0063FB6C);
        for (int i = 0; i < 4; ++i) {
            new_gadgets[i] = old_gadgets[i];
        }

        new_gadgets[4] = &alpine_options_btn;   // add alpine options button
        new_gadgets[5] = old_gadgets[4];        // position back button after alpine options

        alpine_options_panel_init();
    },
};

// handle button click
CodeInjection handle_options_button_click_patch{
    0x0044F337,
    [](auto& regs) {
        int index = regs.eax;
        //xlog::warn("button index {} clicked", index);

        // 4 = alpine, 5 = back
        if (index == 4 || index == 5) {
            if (index == 4) {
                options_alpine_on_click();
                regs.eip = 0x0044F3D2;
            }
            if (index == 5) {
                regs.eip = 0x0044F3A8;
            }
        }
    },
};

// Mouse scale button toggle injected into the stock Controls panel (next to Mouse Y-Invert)
static constexpr int CTRL_CAMSCALE_X = 306;
static constexpr int CTRL_CAMSCALE_Y = 107;

static bool g_ctrl_camscale_initialized = false;

static constexpr const char* camscale_mode_names[] = {"Classic", "Raw", "Modern"};

static void ctrl_camscale_on_click(int, int)
{
    g_alpine_game_config.mouse_scale = (g_alpine_game_config.mouse_scale + 1) % 3;
    snprintf(ao_mousecamerascale_butlabel_text, sizeof(ao_mousecamerascale_butlabel_text), "%s",
        camscale_mode_names[g_alpine_game_config.mouse_scale]);
    ao_play_button_snd(g_alpine_game_config.mouse_scale != 0);
}

static void init_ctrl_camscale_btns()
{
    if (g_ctrl_camscale_initialized) return;
    ao_mousecamerascale_cbox.create("ao_smbut1.tga", "ao_smbut1_hover.tga", "ao_tab.tga",
        CTRL_CAMSCALE_X, CTRL_CAMSCALE_Y, 45, "106.26", 0);
    ao_mousecamerascale_cbox.checked = false;
    ao_mousecamerascale_cbox.on_click = ctrl_camscale_on_click;
    ao_mousecamerascale_cbox.enabled = true;
    snprintf(ao_mousecamerascale_butlabel_text, sizeof(ao_mousecamerascale_butlabel_text), "%s",
        camscale_mode_names[std::clamp(g_alpine_game_config.mouse_scale, 0, 2)]);
    g_ctrl_camscale_initialized = true;
}

static void render_ctrl_camscale_btns()
{
    init_ctrl_camscale_btns();
    snprintf(ao_mousecamerascale_butlabel_text, sizeof(ao_mousecamerascale_butlabel_text), "%s",
        camscale_mode_names[std::clamp(g_alpine_game_config.mouse_scale, 0, 2)]);
    ao_mousecamerascale_cbox.x = CTRL_CAMSCALE_X + static_cast<int>(rf::ui::options_animated_offset);
    ao_mousecamerascale_cbox.render();
    int val_x = static_cast<int>((ao_mousecamerascale_cbox.x + 50) * rf::ui::scale_x);
    int val_y = static_cast<int>((CTRL_CAMSCALE_Y + 6) * rf::ui::scale_y);
    rf::gr::set_color(255, 255, 255, 255);
    rf::gr::string_aligned(rf::gr::ALIGN_CENTER, val_x, val_y, ao_mousecamerascale_butlabel_text, rf::ui::medium_font_0);
    int name_x = static_cast<int>((ao_mousecamerascale_cbox.x + 87) * rf::ui::scale_x);
    rf::gr::set_color(0, 0, 0, 255);
    rf::gr::string(name_x, val_y, "Mouse scale", rf::ui::medium_font_0);
}

static void handle_ctrl_camscale_btns(int x, int y)
{
    if (!g_ctrl_camscale_initialized)
        return;

    // Use absolute position so hit-testing tracks parent panel offsets/animations.
    int bx = static_cast<int>(ao_mousecamerascale_cbox.get_absolute_x() * rf::ui::scale_x);
    int by = static_cast<int>(ao_mousecamerascale_cbox.get_absolute_y() * rf::ui::scale_y);
    int bw = static_cast<int>(ao_mousecamerascale_cbox.w * rf::ui::scale_x);
    int bh = static_cast<int>(ao_mousecamerascale_cbox.h * rf::ui::scale_y);

    bool inside = (x >= bx && x < bx + bw && y >= by && y < by + bh);

    // Keep hover state in sync with cursor position so the correct bitmap/state is rendered.
    ao_mousecamerascale_cbox.highlighted = inside;

    // Do not react to clicks while the controls panel is waiting for a key/mouse binding.
    if (!inside || rf::ui::options_controls_waiting_for_key || !rf::mouse_was_button_pressed(0))
        return;

    ctrl_camscale_on_click(x, y);
}

// Controller bindings tab strip (drawn on top of options panel 3 = Controls)
// Write the current gamepad binding for every action into scan_codes[0] using the
// CTRL_GAMEPAD_SCAN_BASE encoding, saving the original keyboard scan codes first.
static void restore_keyboard_fields()
{
    if (!rf::local_player || !g_ctrl_codes_installed) return;
    auto& cc = rf::local_player->settings.controls;
    int n = std::min(cc.num_bindings, static_cast<int>(std::size(g_saved_scan_codes)));
    for (int i = 0; i < n; ++i) {
        cc.bindings[i].scan_codes[0] = g_saved_scan_codes[i];
        cc.bindings[i].scan_codes[1] = g_saved_sc1[i];
        cc.bindings[i].mouse_btn_id  = g_saved_mouse_btn_ids[i];
    }
}

static void install_ctrl_gamepad_codes()
{
    if (!rf::local_player || g_ctrl_codes_installed) return;
    auto& cc = rf::local_player->settings.controls;
    int n = std::min(cc.num_bindings, static_cast<int>(std::size(g_saved_scan_codes)));
    for (int i = 0; i < n; ++i) {
        g_saved_scan_codes[i]    = cc.bindings[i].scan_codes[0];
        g_saved_sc1[i]           = cc.bindings[i].scan_codes[1];
        g_saved_mouse_btn_ids[i] = cc.bindings[i].mouse_btn_id;
        bool menu_only = gamepad_is_menu_only_action(i);
        int btn = -1, btn_alt = -1;
        gamepad_get_buttons_for_action(i, &btn, &btn_alt);
        int trig = gamepad_get_trigger_for_action(i);
        int16_t code = 0; // unbound
        if (btn >= 0)
            code = menu_only ? static_cast<int16_t>(CTRL_GAMEPAD_MENU_BASE + btn)
                             : static_cast<int16_t>(CTRL_GAMEPAD_SCAN_BASE + btn);
        else if (trig == 0) code = static_cast<int16_t>(CTRL_GAMEPAD_LEFT_TRIGGER);
        else if (trig == 1) code = static_cast<int16_t>(CTRL_GAMEPAD_RIGHT_TRIGGER);
        cc.bindings[i].scan_codes[0] = code;
        // Menu-only actions never have secondary bindings.
        cc.bindings[i].scan_codes[1] = (!menu_only && btn_alt >= 0)
            ? static_cast<int16_t>(CTRL_GAMEPAD_SCAN_BASE + btn_alt) : int16_t{0};
        cc.bindings[i].mouse_btn_id  = -1; // no mouse binding in gamepad view
    }
    g_ctrl_codes_installed = true;
}

// Rewrite scan_codes[0] and scan_codes[1] from the current g_button_map/g_trigger_action state.
// Called after a bind completes so the list immediately reflects the new assignment.
static void refresh_ctrl_gamepad_codes()
{
    if (!rf::local_player || !g_ctrl_codes_installed) return;
    auto& cc = rf::local_player->settings.controls;
    int n = std::min(cc.num_bindings, static_cast<int>(std::size(g_saved_scan_codes)));
    for (int i = 0; i < n; ++i) {
        bool menu_only = gamepad_is_menu_only_action(i);
        int btn = -1, btn_alt = -1;
        gamepad_get_buttons_for_action(i, &btn, &btn_alt);
        int trig = gamepad_get_trigger_for_action(i);
        int16_t code = 0;
        if (btn >= 0)
            code = menu_only ? static_cast<int16_t>(CTRL_GAMEPAD_MENU_BASE + btn)
                             : static_cast<int16_t>(CTRL_GAMEPAD_SCAN_BASE + btn);
        else if (trig == 0) code = static_cast<int16_t>(CTRL_GAMEPAD_LEFT_TRIGGER);
        else if (trig == 1) code = static_cast<int16_t>(CTRL_GAMEPAD_RIGHT_TRIGGER);
        cc.bindings[i].scan_codes[0] = code;
        cc.bindings[i].scan_codes[1] = (!menu_only && btn_alt >= 0)
            ? static_cast<int16_t>(CTRL_GAMEPAD_SCAN_BASE + btn_alt) : int16_t{0};
    }
}

// Harvest whatever RF wrote into scan_codes[0/1] back into the button maps,
// then restore the original keyboard/mouse binding fields.
static void uninstall_ctrl_gamepad_codes()
{
    if (!rf::local_player || !g_ctrl_codes_installed) return;
    gamepad_sync_bindings_from_scan_codes();
    restore_keyboard_fields();
    g_ctrl_codes_installed = false;
}

bool ui_ctrl_bindings_view_active()
{
    return g_ctrl_bind_view;
}

void ui_ctrl_bindings_view_reset()
{
    uninstall_ctrl_gamepad_codes();
    g_ctrl_bind_view = false;
}

// X/Y position for the CONTROLLER mode checkbox in UI 640x480 space.
// Sits to the right of the stock "Change Binding" button on the same row.
static constexpr int CTRL_CHK_X = 265;
static constexpr int CTRL_CHK_Y = 350;

static void ctrl_mode_cbox_on_click(int, int)
{
    if (g_ctrl_bind_view) {
        uninstall_ctrl_gamepad_codes();
        g_ctrl_bind_view = false;
    } else {
        g_ctrl_bind_view = true;
        install_ctrl_gamepad_codes();
    }
    g_ctrl_mode_cbox.checked = g_ctrl_bind_view;
    rf::snd_play(43, 0, 0.0f, 1.0f);
}

// Create the checkbox once (called lazily on first Controls panel render).
static void init_ctrl_mode_btns()
{
    if (g_ctrl_mode_btns_initialized) return;
    g_ctrl_mode_cbox.create("checkbox.tga", "checkbox_selected.tga", "checkbox_checked.tga",
        CTRL_CHK_X, CTRL_CHK_Y, 0, "", rf::ui::medium_font_0);
    g_ctrl_mode_cbox.enabled = true;
    g_ctrl_mode_cbox.on_click = ctrl_mode_cbox_on_click;
    g_ctrl_mode_btns_initialized = true;
}

// Render the checkbox and a label indicating what it controls.
static void render_ctrl_mode_btns()
{
    init_ctrl_mode_btns();
    g_ctrl_mode_cbox.checked = g_ctrl_bind_view;
    g_ctrl_mode_cbox.x = CTRL_CHK_X + static_cast<int>(rf::ui::options_animated_offset);
    g_ctrl_mode_cbox.render();
    int lx = static_cast<int>((g_ctrl_mode_cbox.x + g_ctrl_mode_cbox.w + 5) * rf::ui::scale_x);
    int cbox_screen_h = static_cast<int>(g_ctrl_mode_cbox.h * rf::ui::scale_y);
    int font_h = rf::gr::get_font_height(rf::ui::medium_font_0);
    int ly = static_cast<int>(CTRL_CHK_Y * rf::ui::scale_y) + (cbox_screen_h - font_h) / 2;
    rf::gr::set_color(0, 0, 0, 255);
    rf::gr::string(lx, ly, g_ctrl_bind_view ? "Switch to Keyboard" : "Switch to Gamepad", rf::ui::medium_font_0);
}

// Handle a click on the checkbox.
static void handle_ctrl_mode_btns(int x, int y)
{
    if (!g_ctrl_mode_btns_initialized)
        return;

    // Use absolute position so hit-testing tracks the panel animation offset.
    int bx = static_cast<int>(g_ctrl_mode_cbox.get_absolute_x() * rf::ui::scale_x);
    int by = static_cast<int>(g_ctrl_mode_cbox.get_absolute_y() * rf::ui::scale_y);
    int bw = static_cast<int>(g_ctrl_mode_cbox.w * rf::ui::scale_x);
    int bh = static_cast<int>(g_ctrl_mode_cbox.h * rf::ui::scale_y);

    bool inside = (x >= bx && x < bx + bw && y >= by && y < by + bh);

    // Keep hover state in sync with cursor position.
    g_ctrl_mode_cbox.highlighted = inside;

    // Do not react to clicks while the controls panel is waiting for a key/mouse binding.
    if (!inside || rf::ui::options_controls_waiting_for_key || !rf::mouse_was_button_pressed(0))
        return;

    ctrl_mode_cbox_on_click(x, y);
}

// handle alpine options panel rendering
CodeInjection options_render_alpine_panel_patch{
    0x0044F80B,
    []() {
        int index = rf::ui::options_current_panel;

        // Restore keyboard bindings if user has navigated away from the Controls panel
        if (index != 3 && g_ctrl_bind_view) {
            uninstall_ctrl_gamepad_codes();
            g_ctrl_bind_view = false;
        }

        // how mouse scale toggle when Controls panel is active
        if (index == 3 && !rf::ui::options_controls_waiting_for_key) {
            render_ctrl_camscale_btns();
        }

        // render alpine options panel
        if (index == 4) {
            alpine_options_panel_do_frame(static_cast<int>(rf::ui::options_animated_offset));
        }

        // Detect bind completion (falling edge of waiting_for_key).
        static bool s_was_waiting = false;
        bool now_waiting = (index == 3) && rf::ui::options_controls_waiting_for_key;

        if (s_was_waiting && !now_waiting && g_ctrl_bind_view) {
            if (gamepad_has_pending_rebind()) {
                gamepad_apply_rebind();
                gamepad_sync_bindings_from_scan_codes();
            }
            restore_keyboard_fields();
            refresh_ctrl_gamepad_codes();
            hud_mark_bindings_dirty();
        }
        s_was_waiting = now_waiting;

        if (index == 3 && g_ctrl_codes_installed && rf::local_player) {
            auto& cc = rf::local_player->settings.controls;
            int n = std::min(cc.num_bindings, static_cast<int>(std::size(g_saved_scan_codes)));
            bool defaults_hit = false;
            for (int i = 0; i < n && !defaults_hit; ++i) {
                int16_t sc = cc.bindings[i].scan_codes[0];
                bool is_gamepad = (sc >= static_cast<int16_t>(CTRL_GAMEPAD_SCAN_BASE)
                                && sc <= static_cast<int16_t>(CTRL_GAMEPAD_RIGHT_TRIGGER))
                               || (sc >= static_cast<int16_t>(CTRL_GAMEPAD_MENU_BASE)
                                && sc <  static_cast<int16_t>(CTRL_GAMEPAD_MENU_BASE + gamepad_get_button_count()));
                if (sc != 0 && sc != static_cast<int16_t>(CTRL_REBIND_SENTINEL) && !is_gamepad)
                    defaults_hit = true;
            }
            if (defaults_hit) {
                for (int i = 0; i < n; ++i)
                    g_saved_scan_codes[i] = cc.bindings[i].scan_codes[0];
                gamepad_reset_to_defaults();
                refresh_ctrl_gamepad_codes();
                hud_mark_bindings_dirty();
            }
        }

        if (index == 3)
            render_ctrl_mode_btns();
    },
};

// forward pressed keys to alpine options panel handler
CodeInjection options_handle_key_patch{
    0x0044F2D3,
    [](auto& regs) {
        rf::Key* key = reinterpret_cast<rf::Key*>(regs.esp + 0x4);
        int index = rf::ui::options_current_panel;

        if (index == 4) {
            alpine_options_panel_handle_key(key);
        }
    },
};

// forward mouse activity to alpine options panel handler
CodeInjection options_handle_mouse_patch{
    0x0044F609,
    [](auto& regs) {
        int x = *reinterpret_cast<int*>(regs.esp + 0x8);
        int y = *reinterpret_cast<int*>(regs.esp + 0x4);
        int index = rf::ui::options_current_panel;

        if (index == 4) {
            alpine_options_panel_handle_mouse(x, y);
        }
        if (index == 3) {
            handle_ctrl_camscale_btns(x, y);
            handle_ctrl_mode_btns(x, y);
        }
    },
};

// unhighlight buttons when not active
CodeInjection options_do_frame_unhighlight_buttons_patch{
    0x0044F1E1,
    [](auto& regs) {

        for (int i = 0; i < 6; ++i) {
            if (new_gadgets[i]) { // Avoid null pointer dereference
                regs.ecx = regs.esi;
                new_gadgets[i]->highlighted = false;
                regs.esi += 0x44;
            }
        }

        //regs.eip = 0x0044F1F8;

    },
};

// render alpine options button
CodeInjection options_render_alpine_options_button_patch{
    0x0044F879,
    [](auto& regs) {
        // Loop through all options buttons and render them dynamically
        for (int i = 0; i < 6; ++i) {
            if (new_gadgets[i]) { // ensure the gadget pointer is valid
                rf::ui::Button* button = static_cast<rf::ui::Button*>(new_gadgets[i]);
                if (button) { // ensure the button pointer is valid
                    button->x = static_cast<int>(rf::ui::g_fOptionsMenuOffset);
                    button->y = rf::ui::g_MenuMainButtonsY + (i * rf::ui::menu_button_offset_y);
                    button->render(); // Render the button
                }
            }
        }

        regs.eip = 0x0044F8A3; // skip stock rendering loop
    },
};

// highlight options buttons with mouse
CodeInjection options_do_frame_highlight_buttons_patch{
    0x0044F233,
    [](auto& regs) {
        int index = regs.ecx;
        int new_index = index / 16;
        //xlog::warn("adding4  index {}", new_index);
        new_gadgets[new_index]->highlighted = true;
        regs.eip = 0x0044F23F;
    },
};

void levelsound_opt_slider_on_click(int x, int y)
{
    levelsound_opt_slider.update_value(x, y);
    float vol_value = levelsound_opt_slider.get_value();
    g_alpine_game_config.set_level_sound_volume(vol_value);
    set_play_sound_events_volume_scale();
}

// Add level sounds slider to audio options panel
CodeInjection options_audio_init_patch{
    0x004544F6,
    [](auto& regs) {
        alpine_audio_panel_settings.push_back(&rf::ui::audio_sfx_slider);
        rf::ui::audio_sfx_slider.on_mouse_btn_down = rf::ui::audio_sfx_slider_on_click;

        alpine_audio_panel_settings.push_back(&rf::ui::audio_music_slider);
        rf::ui::audio_music_slider.on_mouse_btn_down = rf::ui::audio_music_slider_on_click;

        alpine_audio_panel_settings.push_back(&rf::ui::audio_message_slider);
        rf::ui::audio_message_slider.on_mouse_btn_down = rf::ui::audio_message_slider_on_click;

        alpine_audio_panel_settings_buttons.push_back(&rf::ui::audio_sfx_button);
        alpine_audio_panel_settings_buttons.push_back(&rf::ui::audio_music_button);
        alpine_audio_panel_settings_buttons.push_back(&rf::ui::audio_message_button);

        levelsound_opt_slider.create(&rf::ui::audio_options_panel, "slider_bar.tga", "slider_bar_on.tga", 141, 176, 118, 21, 0.0, 1.0);
        levelsound_opt_slider.on_click = levelsound_opt_slider_on_click;
        levelsound_opt_slider.on_mouse_btn_down = levelsound_opt_slider_on_click;
        float levelsound_value = g_alpine_game_config.level_sound_volume;
        levelsound_opt_slider.set_value(levelsound_value);
        levelsound_opt_slider.enabled = true;
        alpine_audio_panel_settings.push_back(&levelsound_opt_slider);

        levelsound_opt_button.create("indicator.tga", "indicator_selected.tga", 110, 172, -1, "", 0);
        levelsound_opt_button.parent = &rf::ui::audio_options_panel;
        levelsound_opt_button.enabled = true;
        alpine_audio_panel_settings_buttons.push_back(&levelsound_opt_button);

        levelsound_opt_label.create(&rf::ui::audio_options_panel, 285, 178, "Environment Sounds Multiplier", rf::ui::medium_font_1);
        levelsound_opt_label.enabled = true;
    },
};

CodeInjection options_audio_do_frame_patch{
    0x0045487B,
    [](auto& regs) {
        levelsound_opt_slider.render();
        levelsound_opt_button.render();
        levelsound_opt_label.render();
    },
};

FunHook<int(int, int)> audio_panel_handle_mouse_hook{
    0x004548B0,
    [](int x, int y) {
        static int last_hovered_index = -1;
        static int last_hover_sound_index = -1;
        int hovered_index = -1;
        //xlog::warn("handling mouse {}, {}", x, y);

        // Check which gadget is being hovered over
        for (size_t i = 0; i < alpine_audio_panel_settings.size(); ++i) {
            auto* gadget = alpine_audio_panel_settings[i];
            if (gadget && gadget->enabled) {
                int abs_x = static_cast<int>(gadget->get_absolute_x() * rf::ui::scale_x);
                int abs_y = static_cast<int>(gadget->get_absolute_y() * rf::ui::scale_y);
                int abs_w = static_cast<int>(gadget->w * rf::ui::scale_x);
                int abs_h = static_cast<int>(gadget->h * rf::ui::scale_y);

                //xlog::warn("Checking gadget {} at ({}, {}) size ({}, {})", i, abs_x, abs_y, abs_w, abs_h);

                if (x >= abs_x && x <= abs_x + abs_w && y >= abs_y && y <= abs_y + abs_h) {
                    hovered_index = static_cast<int>(i);

                    if (last_hover_sound_index != hovered_index) {
                        rf::snd_play(42, 0, 0.0f, 1.0f);
                        last_hover_sound_index = hovered_index;
                    }
                    break;
                }
            }
        }

        // Click
        if (hovered_index >= 0) {
            auto* gadget = alpine_audio_panel_settings[hovered_index];
            if (gadget && rf::mouse_was_button_pressed(0)) {
                if (gadget->on_click)
                    gadget->on_click(x, y);

                last_hovered_index = hovered_index; // remember active gadget for slider drag
                rf::snd_play(43, 0, 0.0f, 1.0f);
            }
        }

        // Drag or movement on last clicked gadget
        int dx = 0, dy = 0, dz = 0;
        rf::mouse_get_delta(dx, dy, dz);
        if ((dx || dy) && rf::mouse_button_is_down(0)) {
            if (last_hovered_index >= 0) {
                auto* gadget = alpine_audio_panel_settings[last_hovered_index];
                if (gadget && gadget->on_mouse_btn_down)
                    gadget->on_mouse_btn_down(x, y);
            }
        }

        // Reset last hovered index if mouse is released
        if (!rf::mouse_button_is_down(0)) {
            last_hovered_index = -1;
        }

        for (auto* gadget : alpine_audio_panel_settings) {
            if (gadget) {
                gadget->highlighted = false;
            }
        }

        for (auto* gadget : alpine_audio_panel_settings_buttons) {
            if (gadget) {
                gadget->highlighted = false;
            }
        }

        if (hovered_index >= 0) {
            alpine_audio_panel_settings[hovered_index]->highlighted = true;
            alpine_audio_panel_settings_buttons[hovered_index]->highlighted = true;
        }

        if (hovered_index == -1) {
            last_hover_sound_index = -1; // reset so next hover triggers sound again
        }

        //xlog::warn("over {}", hovered_index);
        return 0;
    },
};

void ui_apply_patch()
{
    // Alpine Faction options button and panel
    options_init_build_button_patch.install();
    options_init_build_button_array_patch.install();
    handle_options_button_click_patch.install();
    options_render_alpine_panel_patch.install();
    options_render_alpine_options_button_patch.install();
    options_do_frame_highlight_buttons_patch.install();
    options_do_frame_unhighlight_buttons_patch.install();
    options_handle_key_patch.install();
    options_handle_mouse_patch.install();
    AsmWriter{0x0044F550}.push(6); // num buttons in options menu
    AsmWriter{0x0044F552}.push(&new_gadgets); // support mouseover for alpine options button
    AsmWriter{0x0044F285}.push(5); // back button index, used when hitting esc in options menu

    // Audio options panel
    options_audio_init_patch.install();
    options_audio_do_frame_patch.install();
    audio_panel_handle_mouse_hook.install();

    // set mouse sens slider in controls options panel to max at 0.5 (stock game is 1.0)
    AsmWriter{0x004504AE}.push(0x3F000000);

    // Sharp UI text
#if SHARP_UI_TEXT
    UiButton_create_hook.install();
    UiButton_set_text_hook.install();
    UiButton_render_hook.install();
    UiLabel_create_hook.install();
    UiLabel_create2_hook.install();
    UiLabel_set_text_hook.install();
    UiLabel_render_hook.install();
    UiInputBox_create_hook.install();
    UiInputBox_render_hook.install();
    UiCycler_add_item_hook.install();
    UiCycler_render_hook.install();
    popup_set_text_gr_split_str_hook.install();
#endif

    // Init
    menu_init_hook.install();

    // Handle CTRL+V in input boxes
    UiInputBox_process_key_hook.install();
}

void ui_get_string_size(int* w, int* h, const char* s, int s_len, int font_num) {
    std::tie(*w, *h) = rf::gr::get_string_size(
        std::string_view{
            s,
            s_len == -1
                ? std::strlen(s)
                : static_cast<size_t>(s_len)
        },
        font_num
    );
#if SHARP_UI_TEXT
    *w = static_cast<int>(*w / rf::ui::scale_x);
    *h = static_cast<int>(*h / rf::ui::scale_y);
#endif
}
