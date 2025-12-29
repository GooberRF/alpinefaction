#pragma once

#include <cstddef>
#include <string>
#include "../rf/multi.h"

extern bool g_character_meshes_are_fullbright;
void misc_init();
void set_jump_to_multi_server_list(bool jump);
void start_join_multi_game_sequence(const rf::NetAddr& addr, const std::string& password);
void start_levelm_load_sequence(std::string filename);
bool multi_join_game(const rf::NetAddr& addr, const std::string& password);
void ui_get_string_size(int* w, int* h, const char* s, int s_len, int font_num);
void g_solid_render_ui();
bool tc_mod_is_loaded();
bool af_rfl_version(int version);
bool rfl_version_minimum(int check_version);
void evaluate_restrict_disable_ss();
void evaluate_restrict_disable_muzzle_flash();
void initialize_achievement_manager();
void set_levelmod_autotexture_ppm();
void clear_explicit_upcoming_game_type_request();
bool file_loaded_from_alpinefaction_vpp(const char* filename);
bool weapon_reticle_is_customized(int weapon_id, bool bighud);
bool rocket_locked_reticle_is_customized(bool bighud);
