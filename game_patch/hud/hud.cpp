#include <cassert>
#include <patch_common/FunHook.h>
#include <patch_common/CallHook.h>
#include <patch_common/AsmWriter.h>
#include <xlog/xlog.h>
#include "hud.h"
#include "multi_scoreboard.h"
#include "hud_internal.h"
#include "../misc/alpine_settings.h"
#include "../main/main.h"
#include "../os/console.h"
#include "../rf/gr/gr_font.h"
#include "../rf/hud.h"
#include "../rf/multi.h"
#include "../rf/file/file.h"
#include "../rf/entity.h"
#include "../rf/player/player.h"
#include "../rf/weapon.h"
#include "../rf/gameseq.h"
#include "multi_spectate.h"

int g_target_player_name_font = -1;

int hud_transform_value(int val, int old_max, int new_max)
{
    if (val < old_max / 3) {
        return val;
    }
    if (val < old_max * 2 / 3) {
        return val - old_max / 2 + new_max / 2;
    }
    return val - old_max + new_max;
}

int hud_scale_value(int val, int max, float scale)
{
    if (val < max / 3) {
        return static_cast<int>(std::round(val * scale));
    }
    if (val < max * 2 / 3) {
        return static_cast<int>(std::round(max / 2.0f + (val - max / 2.0f) * scale));
    }
    return static_cast<int>(std::round(max + (val - max) * scale));
}

rf::HudPoint hud_scale_coords(rf::HudPoint pt, float scale)
{
    return {
        hud_scale_value(pt.x, rf::gr::screen_width(), scale),
        hud_scale_value(pt.y, rf::gr::screen_height(), scale),
    };
}

FunHook<void()> hud_render_for_multi_hook{
    0x0046ECB0,
    []() {
        if (!rf::hud_disabled) {
            hud_render_for_multi_hook.call_target();
        }
    },
};

ConsoleCommand2 hud_cmd{
    "cl_hud",
    [](std::optional<bool> hud_opt) {

        // toggle if no parameter passed
        bool hud_visible = hud_opt.value_or(rf::hud_disabled);
        rf::hud_disabled = !hud_visible;
    },
    "Show and hide HUD",
};

void hud_setup_positions(int width)
{
    int height = rf::gr::screen_height();
    rf::HudPoint* pos_data = nullptr;

    xlog::trace("hud_setup_positionsHook({})", width);

    switch (width) {
    case 640:
        if (height == 480)
            pos_data = rf::hud_coords_640;
        break;
    case 800:
        if (height == 600)
            pos_data = rf::hud_coords_800;
        break;
    case 1024:
        if (height == 768)
            pos_data = rf::hud_coords_1024;
        break;
    case 1280:
        if (height == 1024)
            pos_data = rf::hud_coords_1280;
        break;
    }
    if (pos_data) {
        std::copy(pos_data, pos_data + rf::num_hud_coords, rf::hud_coords);
    }
    else {
        // We have to scale positions from other resolution here
        for (int i = 0; i < rf::num_hud_coords; ++i) {
            rf::HudPoint& src_pt = rf::hud_coords_1024[i];
            rf::HudPoint& dst_pt = rf::hud_coords[i];

            if (src_pt.x <= 1024 / 3)
                dst_pt.x = src_pt.x;
            else if (src_pt.x > 1024 / 3 && src_pt.x < 1024 * 2 / 3)
                dst_pt.x = src_pt.x + (width - 1024) / 2;
            else
                dst_pt.x = src_pt.x + width - 1024;

            if (src_pt.y <= 768 / 3)
                dst_pt.y = src_pt.y;
            else if (src_pt.y > 768 / 3 && src_pt.y < 768 * 2 / 3)
                dst_pt.y = src_pt.y + (height - 768) / 2;
            else // hud_coords_1024[i].y >= 768*2/3
                dst_pt.y = src_pt.y + height - 768;
        }
    }

    // Apply HUD offsets after setting up base positions
    hud_apply_offsets();
}

void hud_apply_offsets()
{
    // Apply health HUD offset - move as a group maintaining relative positions
    if (g_alpine_game_config.health_hud_offset.x != -1 || g_alpine_game_config.health_hud_offset.y != -1) {
        // Health elements: health icon, health text, armor icon, armor text
        rf::HudItem health_elements[] = {
            rf::hud_health,
            rf::hud_health_value_ul_corner,
            rf::hud_envirosuit,
            rf::hud_envirosuit_value_ul_corner
        };
        
        // Use health icon as the reference point for the group
        rf::HudPoint reference_original = rf::hud_coords[rf::hud_health];
        rf::HudPoint reference_new = {
            g_alpine_game_config.health_hud_offset.x != -1 ? g_alpine_game_config.health_hud_offset.x : reference_original.x,
            g_alpine_game_config.health_hud_offset.y != -1 ? g_alpine_game_config.health_hud_offset.y : reference_original.y
        };
        
        // Calculate offset from original position
        int offset_x = reference_new.x - reference_original.x;
        int offset_y = reference_new.y - reference_original.y;
        
        // Apply offset to all health elements
        for (auto element : health_elements) {
            if (g_alpine_game_config.health_hud_offset.x != -1) {
                rf::hud_coords[element].x += offset_x;
            }
            if (g_alpine_game_config.health_hud_offset.y != -1) {
                rf::hud_coords[element].y += offset_y;
            }
        }
    }

    // Apply ammo HUD offset - move as a group maintaining relative positions
    if (g_alpine_game_config.ammo_hud_offset.x != -1 || g_alpine_game_config.ammo_hud_offset.y != -1) {
        // Ammo elements: bar, signal, icon, text positions
        rf::HudItem ammo_elements[] = {
            rf::hud_ammo_bar,
            rf::hud_ammo_signal,
            rf::hud_ammo_icon,
            rf::hud_ammo_in_clip_text_ul_region_coord,
            rf::hud_ammo_in_inv_text_ul_region_coord,
            rf::hud_ammo_bar_position_no_clip,
            rf::hud_ammo_signal_position_no_clip,
            rf::hud_ammo_icon_position_no_clip,
            rf::hud_ammo_in_inv_ul_region_coord_no_clip,
            rf::hud_ammo_in_clip_ul_coord
        };
        
        // Use ammo bar as the reference point for the group
        rf::HudPoint reference_original = rf::hud_coords[rf::hud_ammo_bar];
        rf::HudPoint reference_new = {
            g_alpine_game_config.ammo_hud_offset.x != -1 ? g_alpine_game_config.ammo_hud_offset.x : reference_original.x,
            g_alpine_game_config.ammo_hud_offset.y != -1 ? g_alpine_game_config.ammo_hud_offset.y : reference_original.y
        };
        
        // Calculate offset from original position
        int offset_x = reference_new.x - reference_original.x;
        int offset_y = reference_new.y - reference_original.y;
        
        // Apply offset to all ammo elements
        for (auto element : ammo_elements) {
            if (g_alpine_game_config.ammo_hud_offset.x != -1) {
                rf::hud_coords[element].x += offset_x;
            }
            if (g_alpine_game_config.ammo_hud_offset.y != -1) {
                rf::hud_coords[element].y += offset_y;
            }
        }
    }

    // Timer offset is handled in multi_hud.cpp for multiplayer
    // The hud_countdown_timer coordinate is for singleplayer, not multiplayer
}

FunHook hud_setup_positions_hook{0x004377C0, hud_setup_positions};

void set_big_countdown_counter(bool is_big)
{
    float scale = is_big ? 2.0f : 1.0f;
    rf::hud_coords[rf::hud_countdown_timer] = hud_scale_coords(rf::hud_coords[rf::hud_countdown_timer], scale);
}

static bool is_screen_resolution_too_low_for_big_hud()
{
    return rf::gr::screen_width() < 1024 || rf::gr::screen_height() < 768;
}

void set_big_hud(bool is_big)
{
    hud_status_set_big(is_big);
    multi_hud_chat_set_big(is_big);
    hud_personas_set_big(is_big);
    weapon_select_set_big(is_big);
    multi_scoreboard_set_big(is_big);
    multi_hud_set_big(is_big);
    rf::hud_text_font_num = hud_get_messages_font();
    g_target_player_name_font = hud_get_default_font();

    hud_setup_positions(rf::gr::screen_width());
    hud_weapons_set_big(is_big);
    set_big_countdown_counter(is_big);

    // TODO: Message Log - Note: it remembers text height in save files so method of recalculation is needed
    //write_mem<i8>(0x004553DB + 1, is_big ? 127 : 70);
}

ConsoleCommand2 bighud_cmd{
    "bighud",
    []() {
        if (!g_alpine_game_config.big_hud && is_screen_resolution_too_low_for_big_hud()) {
            rf::console::print("Screen resolution is too low for big HUD!");
            return;
        }
        g_alpine_game_config.big_hud = !g_alpine_game_config.big_hud;
        set_big_hud(g_alpine_game_config.big_hud);
        rf::console::print("Big HUD is {}", g_alpine_game_config.big_hud ? "enabled" : "disabled");
    },
    "Toggle big HUD",
    "bighud",
};

ConsoleCommand2 ui_realarmor_cmd{
    "ui_realarmor",
    []() {
        g_alpine_game_config.real_armor_values = !g_alpine_game_config.real_armor_values;
        rf::console::print("Real armor value display is {}", g_alpine_game_config.real_armor_values ? "enabled" : "disabled");
    },
    "Toggle whether armor is displayed on HUD using real values (1:1 armor to effective health) or classic (2:1 - default).",
    "ui_realarmor",
};

ConsoleCommand2 ui_hudscale_cmd{
    "ui_hudscale",
    [](std::string element, std::optional<float> scale_opt) {
        if (element == "health") {
            if (scale_opt) {
                g_alpine_game_config.set_health_hud_scale(scale_opt.value());
                hud_status_update_scale();
                // Save settings to make them persistent
                extern void alpine_player_settings_save(rf::Player* player);
                alpine_player_settings_save(rf::local_player);
            }
            rf::console::print("Health HUD scale: {:.2f}", g_alpine_game_config.health_hud_scale);
        }
        else if (element == "ammo") {
            if (scale_opt) {
                g_alpine_game_config.set_ammo_hud_scale(scale_opt.value());
                hud_weapons_update_scale();
                // Save settings to make them persistent
                extern void alpine_player_settings_save(rf::Player* player);
                alpine_player_settings_save(rf::local_player);
            }
            rf::console::print("Ammo HUD scale: {:.2f}", g_alpine_game_config.ammo_hud_scale);
        }
        else {
            rf::console::print("Invalid element '{}'. Valid elements: health, ammo", element);
            rf::console::print("Usage: ui_hudscale <element> <multiplier>");
        }
    },
    "Scale HUD elements. Valid elements: health (health & armor icons), ammo (ammo bar and icons)",
    "ui_hudscale <element> <multiplier>",
};

ConsoleCommand2 ui_hudoffset_cmd{
    "ui_hudoffset",
    [](std::string element, std::optional<int> x_opt, std::optional<int> y_opt) {
        auto apply_offset = [&](AlpineGameSettings::HudOffset& offset, const std::string& name) {
            if (x_opt && y_opt) {
                offset.x = x_opt.value();
                offset.y = y_opt.value();
                // Reapply HUD positions
                hud_setup_positions(rf::gr::screen_width());
                // Update scaling for ALL elements to prevent misalignment
                // This fixes the bug where modifying one element would break ammo alignment
                hud_status_update_scale();  // Always update health scaling
                hud_weapons_update_scale(); // Always update ammo scaling
                // Save settings to make them persistent
                extern void alpine_player_settings_save(rf::Player* player);
                alpine_player_settings_save(rf::local_player);
            }
            
            if (offset.x == -1 && offset.y == -1) {
                rf::console::print("{} HUD offset: default position", name);
            } else {
                rf::console::print("{} HUD offset: X={}, Y={}", name, 
                    offset.x == -1 ? "default" : std::to_string(offset.x),
                    offset.y == -1 ? "default" : std::to_string(offset.y));
            }
        };
        
        if (element == "health") {
            apply_offset(g_alpine_game_config.health_hud_offset, "Health");
        }
        else if (element == "ammo") {
            apply_offset(g_alpine_game_config.ammo_hud_offset, "Ammo");
        }
        else if (element == "timer") {
            apply_offset(g_alpine_game_config.timer_hud_offset, "Timer");
        }
        else if (element == "fps") {
            apply_offset(g_alpine_game_config.fps_hud_offset, "FPS");
        }
        else if (element == "ping") {
            apply_offset(g_alpine_game_config.ping_hud_offset, "Ping");
        }
        else {
            rf::console::print("Invalid element '{}'. Valid elements: health, ammo, timer, fps, ping", element);
            rf::console::print("Usage: ui_hudoffset <element> <X> <Y>");
            rf::console::print("Use -1 for X or Y to keep default positioning for that axis");
        }
    },
    "Set HUD element positions. Valid elements: health, ammo, timer, fps, ping",
    "ui_hudoffset <element> <X> <Y>",
};

ConsoleCommand2 ui_fontsize_cmd{
    "ui_fontsize",
    [](std::string element, std::optional<int> size_opt) {
        if (element == "chat") {
            if (size_opt) {
                g_alpine_game_config.chat_font_size = std::clamp(size_opt.value(), 8, 72);
                // Save settings to make them persistent
                extern void alpine_player_settings_save(rf::Player* player);
                alpine_player_settings_save(rf::local_player);
            }
            rf::console::print("Chat font size: {}", g_alpine_game_config.chat_font_size);
        }

        else if (element == "scoreboard") {
            if (size_opt) {
                g_alpine_game_config.scoreboard_font_size = std::clamp(size_opt.value(), 8, 72);
                // Save settings to make them persistent
                extern void alpine_player_settings_save(rf::Player* player);
                alpine_player_settings_save(rf::local_player);
            }
            rf::console::print("Scoreboard font size: {}", g_alpine_game_config.scoreboard_font_size);
        }
        else if (element == "health") {
            if (size_opt) {
                g_alpine_game_config.health_font_size = std::clamp(size_opt.value(), 8, 72);
                // Save settings to make them persistent
                extern void alpine_player_settings_save(rf::Player* player);
                alpine_player_settings_save(rf::local_player);
            }
            rf::console::print("Health font size: {}", g_alpine_game_config.health_font_size);
        }
        else if (element == "ammo") {
            if (size_opt) {
                g_alpine_game_config.ammo_font_size = std::clamp(size_opt.value(), 8, 72);
                // Update the ammo font with new size
                extern void hud_weapons_update_ammo_font();
                hud_weapons_update_ammo_font();
                // Save settings to make them persistent
                extern void alpine_player_settings_save(rf::Player* player);
                alpine_player_settings_save(rf::local_player);
            }
            rf::console::print("Ammo font size: {}", g_alpine_game_config.ammo_font_size);
        }
        else if (element == "timer") {
            if (size_opt) {
                g_alpine_game_config.timer_font_size = std::clamp(size_opt.value(), 8, 72);
                // Save settings to make them persistent
                extern void alpine_player_settings_save(rf::Player* player);
                alpine_player_settings_save(rf::local_player);
            }
            rf::console::print("Timer font size: {}", g_alpine_game_config.timer_font_size);
        }
        else if (element == "fps") {
            if (size_opt) {
                g_alpine_game_config.fps_font_size = std::clamp(size_opt.value(), 8, 72);
                // Save settings to make them persistent
                extern void alpine_player_settings_save(rf::Player* player);
                alpine_player_settings_save(rf::local_player);
            }
            rf::console::print("FPS font size: {}", g_alpine_game_config.fps_font_size);
        }
        else if (element == "ping") {
            if (size_opt) {
                g_alpine_game_config.ping_font_size = std::clamp(size_opt.value(), 8, 72);
                // Save settings to make them persistent
                extern void alpine_player_settings_save(rf::Player* player);
                alpine_player_settings_save(rf::local_player);
            }
            rf::console::print("Ping font size: {}", g_alpine_game_config.ping_font_size);
        }
        else if (element == "messages") {
            if (size_opt) {
                g_alpine_game_config.hud_messages_font_size = std::clamp(size_opt.value(), 8, 72);
                // Save settings to make them persistent
                extern void alpine_player_settings_save(rf::Player* player);
                alpine_player_settings_save(rf::local_player);
            }
            rf::console::print("HUD messages font size: {}", g_alpine_game_config.hud_messages_font_size);
        }
        else {
            rf::console::print("Invalid element '{}'. Valid elements: chat, scoreboard, health, ammo, timer, fps, ping, messages", element);
            rf::console::print("Usage: ui_fontsize <element> <size>");
            rf::console::print("Font size range: 8-72 points");
        }
    },
    "Set font sizes for HUD elements. Valid elements: chat, scoreboard, health, ammo, timer, fps, ping, messages",
    "ui_fontsize <element> <size>",
};

#ifndef NDEBUG
ConsoleCommand2 hud_coords_cmd{
    "d_hud_coords",
    [](int idx, std::optional<int> x, std::optional<int> y) {
        if (x && y) {
            rf::hud_coords[idx].x = x.value();
            rf::hud_coords[idx].y = y.value();
        }
        rf::console::print("HUD coords[{}]: <{}, {}>", idx, rf::hud_coords[idx].x, rf::hud_coords[idx].y);
    },
};
#endif

struct ScaledBitmapInfo {
    int bmh = -1;
    float scale = 2.0f;
};

const ScaledBitmapInfo& hud_get_scaled_bitmap_info(int bmh)
{
    static std::unordered_map<int, ScaledBitmapInfo> scaled_bm_cache;
    // Use bitmap with "_1" suffix instead of "_0" if it exists
    auto it = scaled_bm_cache.find(bmh);
    if (it == scaled_bm_cache.end()) {
        std::string filename = rf::bm::get_filename(bmh);
        auto ext_pos = filename.rfind('.');
        if (ext_pos != std::string::npos) {
            if (ext_pos >= 2 && filename.compare(ext_pos - 2, 2, "_0") == 0) {
                // ends with "_0" - replace '0' by '1'
                filename[ext_pos - 1] = '1';
            }
            else {
                // does not end with "_0" - append "_1"
                filename.insert(ext_pos, "_1");
                assert(filename.size() < 32);
            }
        }

        xlog::trace("loading high res bm {}", filename);
        ScaledBitmapInfo scaled_bm_info;
        rf::File file;
        if (rf::File{}.find(filename.c_str())) {
            scaled_bm_info.bmh = rf::bm::load(filename.c_str(), -1, false);
        }
        xlog::trace("loaded high res bm {}: {}", filename, scaled_bm_info.bmh);
        if (scaled_bm_info.bmh != -1) {
            rf::gr::tcache_add_ref(scaled_bm_info.bmh);
            int bm_w, bm_h;
            rf::bm::get_dimensions(bmh, &bm_w, &bm_h);
            int scaled_bm_w, scaled_bm_h;
            rf::bm::get_dimensions(scaled_bm_info.bmh, &scaled_bm_w, &scaled_bm_h);
            scaled_bm_info.scale = static_cast<float>(scaled_bm_w) / static_cast<float>(bm_w);

        }

        it = scaled_bm_cache.insert({bmh, scaled_bm_info}).first;
    }
    return it->second;
}

void hud_preload_scaled_bitmap(int bmh)
{
    hud_get_scaled_bitmap_info(bmh);
}

void hud_scaled_bitmap(int bmh, int x, int y, float scale, rf::gr::Mode mode)
{
    if (scale > 1.0f) {
        const auto& scaled_bm_info = hud_get_scaled_bitmap_info(bmh);
        if (scaled_bm_info.bmh != -1) {
            bmh = scaled_bm_info.bmh;
            scale /= scaled_bm_info.scale;
        }
    }

    if (scale == 1.0f) {
        rf::gr::bitmap(bmh, x, y, mode);
    }
    else {
        // Get bitmap size and scale it
        int bm_w, bm_h;
        rf::bm::get_dimensions(bmh, &bm_w, &bm_h);
        int dst_w = static_cast<int>(std::round(bm_w * scale));
        int dst_h = static_cast<int>(std::round(bm_h * scale));
        rf::gr::bitmap_scaled(bmh, x, y, dst_w, dst_h, 0, 0, bm_w, bm_h, false, false, mode);
    }
}

void hud_rect_border(int x, int y, int w, int h, int border, rf::gr::Mode state)
{
    // top
    rf::gr::rect(x, y, w, border, state);
    // bottom
    rf::gr::rect(x, y + h - border, w, border, state);
    // left
    rf::gr::rect(x, y + border, border, h - 2 * border, state);
    // right
    rf::gr::rect(x + w - border, y + border, border, h - 2 * border, state);
}

std::string hud_fit_string(std::string_view str, int max_w, int* str_w_out, int font_id)
{
    std::string result{str};
    int str_w, str_h;
    bool has_ellipsis = false;
    rf::gr::get_string_size(&str_w, &str_h, result.c_str(), -1, font_id);
    while (str_w > max_w) {
        result = result.substr(0, result.size() - (has_ellipsis ? 4 : 1)) + "...";
        has_ellipsis = true;
        rf::gr::get_string_size(&str_w, &str_h, result.c_str(), -1, font_id);
    }
    if (str_w_out) {
        *str_w_out = str_w;
    }
    return result;
}

const char* hud_get_small_font_name(bool big)
{
    if (big) {
        return "regularfont.ttf:14";
    }
    return "rfpc-small.vf";
}

const char* hud_get_default_font_name(bool big)
{
    // Always use TTF fonts by default, but check for custom .vf files for mod compatibility
    static std::string font_name;
    
    if (big) {
        // For big HUD, use larger console font size
        int font_size = static_cast<int>(g_alpine_game_config.console_font_size * 1.2f);
        font_name = std::format("regularfont.ttf:{}", font_size);
    } else {
        // For normal HUD, use regular console font size
        font_name = std::format("regularfont.ttf:{}", g_alpine_game_config.console_font_size);
    }
    
    return font_name.c_str();
}

const char* hud_get_bold_font_name(bool big)
{
    if (big) {
        return "boldfont.ttf:26";
    }
    return "rfpc-large.vf";
}

int hud_get_small_font()
{
    if (g_alpine_game_config.big_hud) {
        static int font = -2;
        if (font == -2) {
            font = rf::gr::load_font(hud_get_small_font_name(true));
        }
        return font;
    }
    static int font = -2;
    if (font == -2) {
        font = rf::gr::load_font(hud_get_small_font_name(false));
    }
    return font;
}

int hud_get_default_font()
{
    if (g_alpine_game_config.big_hud) {
        static int font = -2;
        if (font == -2) {
            font = rf::gr::load_font(hud_get_default_font_name(true));
        }
        return font;
    }
    static int font = -2;
    if (font == -2) {
        font = rf::gr::load_font(hud_get_default_font_name(false));
    }
    return font;
}

// Specific font functions for different HUD elements
int hud_get_chat_font()
{
    static int font = -2;
    static int last_size = -1;
    if (font == -2 || last_size != g_alpine_game_config.chat_font_size) {
        std::string font_name = std::format("regularfont.ttf:{}", g_alpine_game_config.chat_font_size);
        font = rf::gr::load_font(font_name.c_str());
        last_size = g_alpine_game_config.chat_font_size;
    }
    return font;
}

int hud_get_health_font()
{
    static int font = -2;
    static int last_size = -1;
    if (font == -2 || last_size != g_alpine_game_config.health_font_size) {
        std::string font_name = std::format("regularfont.ttf:{}", g_alpine_game_config.health_font_size);
        font = rf::gr::load_font(font_name.c_str());
        last_size = g_alpine_game_config.health_font_size;
    }
    return font;
}

int hud_get_ammo_font()
{
    static int font = -2;
    static int last_size = -1;
    if (font == -2 || last_size != g_alpine_game_config.ammo_font_size) {
        std::string font_name = std::format("boldfont.ttf:{}", g_alpine_game_config.ammo_font_size);
        font = rf::gr::load_font(font_name.c_str());
        last_size = g_alpine_game_config.ammo_font_size;
    }
    return font;
}

int hud_get_timer_font()
{
    static int font = -2;
    static int last_size = -1;
    if (font == -2 || last_size != g_alpine_game_config.timer_font_size) {
        std::string font_name = std::format("regularfont.ttf:{}", g_alpine_game_config.timer_font_size);
        font = rf::gr::load_font(font_name.c_str());
        last_size = g_alpine_game_config.timer_font_size;
    }
    return font;
}

int hud_get_fps_font()
{
    static int font = -2;
    static int last_size = -1;
    if (font == -2 || last_size != g_alpine_game_config.fps_font_size) {
        std::string font_name = std::format("regularfont.ttf:{}", g_alpine_game_config.fps_font_size);
        font = rf::gr::load_font(font_name.c_str());
        last_size = g_alpine_game_config.fps_font_size;
    }
    return font;
}

int hud_get_ping_font()
{
    static int font = -2;
    static int last_size = -1;
    if (font == -2 || last_size != g_alpine_game_config.ping_font_size) {
        std::string font_name = std::format("regularfont.ttf:{}", g_alpine_game_config.ping_font_size);
        font = rf::gr::load_font(font_name.c_str());
        last_size = g_alpine_game_config.ping_font_size;
    }
    return font;
}

int hud_get_messages_font()
{
    static int font = -2;
    static int last_size = -1;
    if (font == -2 || last_size != g_alpine_game_config.hud_messages_font_size) {
        std::string font_name = std::format("regularfont.ttf:{}", g_alpine_game_config.hud_messages_font_size);
        font = rf::gr::load_font(font_name.c_str());
        last_size = g_alpine_game_config.hud_messages_font_size;
    }
    return font;
}

int hud_get_large_font()
{
    if (g_alpine_game_config.big_hud) {
        static int font = -2;
        if (font == -2) {
            font = rf::gr::load_font(hud_get_bold_font_name(true));
            if (font == -1) {
                xlog::error("Failed to load boldfont!");
            }
        }
        return font;
    }
    static int font = -2;
    if (font == -2) {
        font = rf::gr::load_font(hud_get_bold_font_name(false));
    }
    return font;
}

FunHook<void()> hud_init_hook{
    0x00437AB0,
    []() {
        hud_init_hook.call_target();
        // Init big HUD
        if (!rf::is_dedicated_server) {
            if (!g_alpine_game_config.big_hud || !is_screen_resolution_too_low_for_big_hud()) {
                set_big_hud(g_alpine_game_config.big_hud);
            }
            // Initialize custom ammo font
            extern void hud_weapons_update_ammo_font();
            hud_weapons_update_ammo_font();
        }
    },
};

CallHook hud_msg_render_gr_get_font_height_hook{
    0x004382DB,
    []([[ maybe_unused ]] int font_no) {
        // Fix wrong font number being used causing line spacing to be invalid
        return rf::gr::get_font_height(rf::hud_text_font_num);
    },
};

void hud_render_00437BC0()
{
    if (!rf::is_multi || !rf::local_player) {
        return;
    }

    // Render spectate mode UI under scoreboard
    multi_spectate_render();

    auto& cc = rf::local_player->settings.controls;
    bool scoreboard_control_pressed = rf::control_config_check_pressed(&cc, rf::CC_ACTION_MP_STATS, nullptr);
    bool is_player_dead = rf::player_is_dead(rf::local_player) || rf::player_is_dying(rf::local_player);
    bool limbo = rf::gameseq_get_state() == rf::GS_MULTI_LIMBO;
    bool show_scoreboard = scoreboard_control_pressed || (!multi_spectate_is_spectating() && is_player_dead) || limbo;

    scoreboard_maybe_render(show_scoreboard);
}

void hud_apply_patches()
{
    // Fix HUD on not supported resolutions
    hud_setup_positions_hook.install();

    // Command for hidding the HUD
    hud_render_for_multi_hook.install();
    hud_cmd.register_cmd();

    // Add some init code
    hud_init_hook.install();

    // Other commands
    bighud_cmd.register_cmd();
    ui_realarmor_cmd.register_cmd();
    ui_hudscale_cmd.register_cmd();
    ui_hudoffset_cmd.register_cmd();
    ui_fontsize_cmd.register_cmd();
#ifndef NDEBUG
    hud_coords_cmd.register_cmd();
#endif

    // Big HUD support
    hud_msg_render_gr_get_font_height_hook.install();

    write_mem_ptr(0x004780D2 + 1, &g_target_player_name_font);
    write_mem_ptr(0x004780FC + 2, &g_target_player_name_font);

    // Change scoreboard rendering logic
    AsmWriter(0x00437BC0).call(hud_render_00437BC0).jmp(0x00437C24);
    AsmWriter(0x00437D40).jmp(0x00437D5C);

    // Patches from other files
    hud_status_apply_patches();
    hud_weapons_apply_patches();
    hud_personas_apply_patches();
    weapon_select_apply_patches();
    multi_hud_chat_apply_patches();
    multi_hud_apply_patches();
    message_log_apply_patch();
    hud_world_apply_patch();
}

int hud_get_scoreboard_font()
{
    static int font = -2;
    static int last_size = -1;
    if (font == -2 || last_size != g_alpine_game_config.scoreboard_font_size) {
        std::string font_name = std::format("regularfont.ttf:{}", g_alpine_game_config.scoreboard_font_size);
        font = rf::gr::load_font(font_name.c_str());
        last_size = g_alpine_game_config.scoreboard_font_size;
    }
    return font;
}

int hud_get_console_font()
{
    static int font = -2;
    if (font == -2) {
        // Use legacy .vf font for console to maintain original console appearance
        font = rf::gr::load_font("rfpc-medium.vf");
    }
    return font;
}
