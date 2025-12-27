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
#include "../rf/ui.h"
#include "../rf/sound/sound.h"
#include "../rf/input.h"
#include "../rf/player/player.h"
#include "../rf/misc.h"
#include "../rf/os/os.h"

#define DEBUG_UI_LAYOUT 0
#define SHARP_UI_TEXT 1

// options menu elements
static rf::ui::Gadget* new_gadgets[6]; // Allocate space for 6 options buttons
static rf::ui::Button alpine_options_btn;

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
static rf::ui::Checkbox ao_meshstatic_cbox;
static rf::ui::Label ao_meshstatic_label;
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
    rf::ui::popup_message("Enter new scanner sensitivity modifier value:", "", ao_scannersens_cbox_on_click_callback, 1);
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
    rf::ui::popup_message("Enter new scope sensitivity modifier value:", "", ao_scopesens_cbox_on_click_callback, 1);
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

void ao_meshstatic_cbox_on_click(int x, int y) {
    g_alpine_game_config.mesh_static_lighting = !g_alpine_game_config.mesh_static_lighting;
    ao_meshstatic_cbox.checked = g_alpine_game_config.mesh_static_lighting;
    recalc_mesh_static_lighting();
    ao_play_button_snd(g_alpine_game_config.mesh_static_lighting);
}

void ao_enemybullets_cbox_on_click(int x, int y) {
    g_alpine_game_config.show_enemy_bullets = !g_alpine_game_config.show_enemy_bullets;
    ao_enemybullets_cbox.checked = g_alpine_game_config.show_enemy_bullets;
    apply_show_enemy_bullets();
    ao_play_button_snd(g_alpine_game_config.show_enemy_bullets);
}

void alpine_options_panel_handle_key(rf::Key* key){
    // todo: more key support (tab, etc.)
    // close panel on escape
    if (*key == rf::Key::KEY_ESC) {
        rf::ui::options_close_current_panel();
        rf::snd_play(43, 0, 0.0f, 1.0f);
        return;
    }
}

void alpine_options_panel_handle_mouse(int x, int y) {
    int hovered_index = -1;
    //xlog::warn("handling mouse {}, {}", x, y);

    // Check which gadget is being hovered over
    for (size_t i = 0; i < alpine_options_panel_settings.size(); ++i) {
        auto* gadget = alpine_options_panel_settings[i];
        if (gadget && gadget->enabled) {
            int abs_x = static_cast<int>(gadget->get_absolute_x() * rf::ui::scale_x);
            int abs_y = static_cast<int>(gadget->get_absolute_y() * rf::ui::scale_y);
            int abs_w = static_cast<int>(gadget->w * rf::ui::scale_x);
            int abs_h = static_cast<int>(gadget->h * rf::ui::scale_y);

            //xlog::warn("Checking gadget {} at ({}, {}) size ({}, {})", i, abs_x, abs_y, abs_w, abs_h);

            if (x >= abs_x && x <= abs_x + abs_w &&
                y >= abs_y && y <= abs_y + abs_h) {
                hovered_index = static_cast<int>(i);
                break;
            }
        }
    }
    //xlog::warn("hovered {}", hovered_index);
    if (hovered_index >= 0) {
        auto* gadget = alpine_options_panel_settings[hovered_index];

        if (gadget) {
            if (rf::mouse_was_button_pressed(0)) { // Left mouse button pressed
                //xlog::warn("Clicked on gadget index {}", hovered_index);

                // Call on_click if assigned
                if (gadget->on_click) {
                    gadget->on_click(x, y);
                }
            }
            else if (rf::mouse_button_is_down(0) && gadget->on_mouse_btn_down) {
                // Handle mouse button being held down
                gadget->on_mouse_btn_down(x, y);
            }
        }
    }

    // Update all gadgets
    for (auto* gadget : alpine_options_panel_settings) {
        if (gadget) {
            gadget->highlighted = false;
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
    alpine_options_panel_checkbox_init(
        &ao_meshstatic_cbox, &ao_meshstatic_label, &alpine_options_panel0, ao_meshstatic_cbox_on_click, g_alpine_game_config.mesh_static_lighting, 280, 144, "Mesh static light");
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

    // render dynamic elements across all panels
    for (auto* ui_element : alpine_options_panel_settings) {
        if (ui_element) {
            auto checkbox = static_cast<rf::ui::Checkbox*>(ui_element);
            if (checkbox) {
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
    snprintf(ao_retscale_butlabel_text, sizeof(ao_retscale_butlabel_text), "%6.2f", g_alpine_game_config.reticle_scale);
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

    // render button labels
    for (auto* ui_label : alpine_options_panel_labels) {
        if (ui_label) {
            ui_label->render();
        }
    }
}

static void options_alpine_on_click() {
    constexpr int alpine_options_panel_id = 4;

    if (rf::ui::options_current_panel == alpine_options_panel_id) {
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

// handle alpine options panel rendering
CodeInjection options_render_alpine_panel_patch{
    0x0044F80B,
    []() {
        int index = rf::ui::options_current_panel;
        //xlog::warn("render index {}", index);

        // render alpine options panel
        if (index == 4) {
            alpine_options_panel_do_frame(static_cast<int>(rf::ui::options_animated_offset));
        }
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
