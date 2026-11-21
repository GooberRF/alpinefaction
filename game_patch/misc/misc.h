#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include "../rf/multi.h"
#include "destruction.h"

namespace rf {
    enum GameState : int;
}

extern bool g_character_meshes_are_fullbright;
void misc_init();
// The launcher's "Use deprecated legacy menus", as it was when the game started.
bool legacy_menus_enabled();
// The main menu's version label, its clicks, and the easter egg three of them start.
std::string main_menu_version_text();
void main_menu_version_clicked();
// The menus' seasonal effect (snow, and af_main_menu.toml's seasonal level) is on.
bool main_menu_seasonal_effect_active();
// The menu music choices; Default is game.tbl's track. New tracks append, as the setting stores the index.
inline constexpr const char* main_menu_music_labels[] = {
    "Default", "PS2", "ClockWerk", "Faction", "Ghost in the Machine", "Grinder", "MooGroov", "Neology",
    "Self Destruct Sequence", "Storm", "Tribes", "Tension", "Zatso", "Trilogy",
};
void main_menu_music_on_setting_changed();
// While set, the menu background draws nothing: a frame run beneath a menu that paints its own over it.
void main_menu_set_background_hidden(bool hidden);
void main_menu_render_egg();
void set_jump_to_multi_server_list(bool jump);
void set_jump_to_demo_browser(bool jump);
void start_join_multi_game_sequence(const rf::NetAddr& addr, const std::string& password);
void start_levelm_load_sequence(std::string filename, std::optional<rf::NetGameType> game_type = std::nullopt);
bool multi_join_game(const rf::NetAddr& addr, const std::string& password);
void ui_get_string_size(int* w, int* h, const char* s, int s_len, int font_num);
void g_solid_render_ui();
bool tc_mod_is_loaded();
bool af_rfl_version(int version);
bool rfl_version_minimum(int check_version);
// Like rfl_version_minimum, but also valid while the RFL is loading, before LEVEL_LOADED is set.
bool level_rfl_version_at_least(int check_version);
void evaluate_restrict_disable_ss();
void alpine_camera_clear_static_mode();
void evaluate_restrict_disable_muzzle_flash();
void evaluate_footsteps();
void initialize_achievement_manager();
void set_levelmod_autotexture_ppm();
void clear_explicit_upcoming_game_type_request();
bool file_loaded_from_alpinefaction_vpp(const char* filename);
bool file_loaded_from_tc_mod(const char* filename);
bool weapon_reticle_is_customized(int weapon_id, bool bighud);
// The weapon's 3rd person mesh, or "" for none or an unknown type.
const char* weapon_third_person_mesh(int weapon_type);
bool rocket_locked_reticle_is_customized(bool bighud);
bool gameseq_is_stacked(rf::GameState state);
bool gameseq_is_deep_stacked(rf::GameState state);
bool gameseq_is_visible();
int letterbox_clip_height();
void hud_refresh_action_tokens(); // Redraw currently active HUD token message after input icon/settings change
void hud_mark_bindings_dirty(); // Redraw currently active HUD token message after input icon/settings change
