#include <patch_common/FunHook.h>
#include <patch_common/CallHook.h>
#include <patch_common/AsmWriter.h>
#include <xlog/xlog.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>
#include "hud.h"
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
#include "../rf/os/string.h"

// Default weapon bar position (used when HudOffset is -1, -1)
constexpr int DEFAULT_WEAPON_BAR_X = 10;
constexpr int DEFAULT_WEAPON_BAR_Y = 50;

// Colors for different weapon states
struct WeaponBarColors {
    rf::Color equipped = {255, 0, 0, 255};         // Red for equipped weapon
    rf::Color has_ammo = {0, 255, 0, 255};         // Green for weapons with ammo
    rf::Color no_ammo = {64, 64, 64, 255};         // Dark gray for weapons without ammo
};

static WeaponBarColors g_weapon_bar_colors;

// Get weapon bar font (uses standardized font system)
int hud_get_weapon_bar_font()
{
    static int font = -2;
    static int last_size = -1;
    if (font == -2 || last_size != g_alpine_game_config.weapon_bar_font_size) {
        char font_name[64];
        std::snprintf(font_name, sizeof(font_name), "regularfont.ttf:%d", g_alpine_game_config.weapon_bar_font_size);
        font = rf::gr::load_font(font_name);
        last_size = g_alpine_game_config.weapon_bar_font_size;
    }
    return font;
}

// Get weapon bar bold font for equipped weapon
int hud_get_weapon_bar_bold_font()
{
    static int font = -2;
    static int last_size = -1;
    if (font == -2 || last_size != g_alpine_game_config.weapon_bar_font_size) {
        char font_name[64];
        std::snprintf(font_name, sizeof(font_name), "boldfont.ttf:%d", g_alpine_game_config.weapon_bar_font_size);
        font = rf::gr::load_font(font_name);
        last_size = g_alpine_game_config.weapon_bar_font_size;
    }
    return font;
}

// Check if player has weapon
bool player_has_weapon(rf::Player* player, int weapon_type)
{
    if (!player) return false;
    rf::AiInfo* ai_info = rf::player_get_ai(player);
    return ai_info && rf::ai_has_weapon(ai_info, weapon_type);
}

// Get weapon ammo count
int get_weapon_ammo_count(rf::Player* player, int weapon_type)
{
    if (!player) return 0;
    return rf::player_get_weapon_total_ammo(player, weapon_type);
}

// Get current equipped weapon
int get_current_weapon_type(rf::Player* player)
{
    if (!player) return -1;
    rf::Entity* entity = rf::entity_from_handle(player->entity_handle);
    if (!entity) return -1;
    return entity->ai.current_primary_weapon;
}

// Calculate weapon bar position using HudOffset system
void get_weapon_bar_position(int& x, int& y)
{
    if (g_alpine_game_config.weapon_bar_hud_offset.x == -1) {
        x = DEFAULT_WEAPON_BAR_X;
    } else {
        x = g_alpine_game_config.weapon_bar_hud_offset.x;
    }
    
    if (g_alpine_game_config.weapon_bar_hud_offset.y == -1) {
        y = DEFAULT_WEAPON_BAR_Y;
    } else {
        y = g_alpine_game_config.weapon_bar_hud_offset.y;
    }
    
    // Apply Big HUD scaling
    if (g_alpine_game_config.big_hud) {
        x = static_cast<int>(x * 1.875f);
        y = static_cast<int>(y * 1.875f);
    }
}

// Render weapon bar
void render_weapon_bar()
{
    if (!g_alpine_game_config.weapon_bar_enabled || !rf::local_player || rf::hud_disabled) {
        return;
    }

    // Don't show weapon bar in certain game states
    if (rf::gameseq_get_state() != rf::GS_GAMEPLAY && rf::gameseq_get_state() != rf::GS_MULTI_LIMBO) {
        return;
    }

    int regular_font = hud_get_weapon_bar_font();
    int bold_font = hud_get_weapon_bar_bold_font();
    if (regular_font == -1) return;

    int current_weapon = get_current_weapon_type(rf::local_player);
    
    // Get position using standardized offset system
    int x, y;
    get_weapon_bar_position(x, y);
    
    // Calculate line height based on font size and Big HUD
    int line_height = g_alpine_game_config.weapon_bar_font_size + 4;
    if (g_alpine_game_config.big_hud) {
        line_height = static_cast<int>(line_height * 1.875f);
    }

    // Collect weapons to display
    std::vector<int> weapons_to_show;
    
    // Use weapon preferences order but only show weapons player actually has
    for (auto& pref_id : rf::local_player->weapon_prefs) {
        if (pref_id >= 0 && pref_id < rf::num_weapon_types && 
            player_has_weapon(rf::local_player, pref_id)) {
            weapons_to_show.push_back(pref_id);
        }
    }

    // If no weapons from prefs, fall back to checking all weapon types
    if (weapons_to_show.empty()) {
        for (int i = 0; i < rf::num_weapon_types; ++i) {
            if (player_has_weapon(rf::local_player, i)) {
                weapons_to_show.push_back(i);
            }
        }
    }

    if (weapons_to_show.empty()) {
        return; // No weapons to show
    }

    // Draw each weapon as text only
    for (size_t i = 0; i < weapons_to_show.size(); ++i) {
        int weapon_id = weapons_to_show[i];
        int ammo_count = get_weapon_ammo_count(rf::local_player, weapon_id);
        bool has_ammo = ammo_count > 0;
        bool is_equipped = (current_weapon == weapon_id);
        
        // Get weapon name (no truncation - display full name)
        std::string weapon_name = rf::weapon_types[weapon_id].display_name.c_str();
        
        // Determine text color based on weapon state
        rf::Color text_color;
        if (is_equipped) {
            text_color = g_weapon_bar_colors.equipped;
        } else if (has_ammo) {
            text_color = g_weapon_bar_colors.has_ammo;
        } else {
            text_color = g_weapon_bar_colors.no_ammo;
        }
        
        // Choose font based on whether weapon is equipped
        int font_to_use = is_equipped ? bold_font : regular_font;
        if (font_to_use == -1) font_to_use = regular_font; // Fallback if bold font fails
        
        // Draw weapon name
        int text_y = y + i * line_height;
        rf::gr::set_color(text_color);
        rf::gr::string(x, text_y, weapon_name.c_str(), font_to_use);
    }
}

// Console command for weapon bar toggle
ConsoleCommand2 weaponbar_cmd{
    "ui_weaponbar",
    []() {
        g_alpine_game_config.weapon_bar_enabled = !g_alpine_game_config.weapon_bar_enabled;
        // Save settings to make them persistent
        extern void alpine_player_settings_save(rf::Player* player);
        alpine_player_settings_save(rf::local_player);
        rf::console::print("Weapon bar: {}", g_alpine_game_config.weapon_bar_enabled ? "enabled" : "disabled");
    },
    "Toggle weapon bar display",
};

void render_weapon_bar_external()
{
    render_weapon_bar();
}

bool is_weapon_bar_enabled()
{
    return g_alpine_game_config.weapon_bar_enabled;
}

void hud_weapon_bar_apply_patches()
{
    weaponbar_cmd.register_cmd();
}
