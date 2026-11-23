#pragma once

#include <string>
#include <vector>
#include "../rf/gameseq.h"

// A level simulated live behind the menus outside a game, seen through the camera shots in af_main_menu.toml
// (alpinefaction.vpp, or a mod's own copy), or with the MP level background, a multiplayer level of the player's
// choice seen through its cutscene cameras.

void menu_scene_apply_patch();
void menu_scene_load_config();
// The loaded level, or the level being loaded, is the scene's
bool menu_scene_active();
// The states the scene stays loaded under
bool menu_scene_state(rf::GameState state);
// For the GS_MAIN_MENU init, before the stock one: the load stops the menu music, which that init restarts. True
// when it tried to load the level.
bool menu_scene_maybe_start();
void menu_scene_stop();
void menu_scene_on_level_closed();
void menu_scene_do_frame(rf::GameState state);
// Draws the scene, or the game the menus were opened over, in place of the Mars backdrop; false when there is
// nothing to draw
bool menu_scene_draw_background();
// Applied from the next menu_scene_do_frame, under any menu.
void menu_scene_on_setting_changed();
void menu_scene_on_shot_setting_changed();
// The scene runs the MP level background's level, whose items spin as in multiplayer.
bool menu_scene_runs_mp_level();
// The MP level background's choice of levels: those Host Game lists, stock levels first.
std::vector<std::string> menu_scene_mp_levels();
bool menu_scene_mp_level_background();
