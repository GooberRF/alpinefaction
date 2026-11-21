#pragma once

#include "../rf/gameseq.h"

// A level simulated live behind the menus outside a game, seen through the camera shots in af_main_menu.toml
// (alpinefaction.vpp, or a mod's own copy).

void menu_scene_apply_patch();
void menu_scene_load_config();
// The loaded level, or the level being loaded, is the scene's
bool menu_scene_active();
// The states the scene stays loaded under
bool menu_scene_state(rf::GameState state);
// For the GS_MAIN_MENU init, before the stock one: the load stops the menu music, which that init restarts.
void menu_scene_maybe_start();
void menu_scene_stop();
void menu_scene_on_level_closed();
void menu_scene_do_frame(rf::GameState state);
// Draws the scene, or the game the menus were opened over, in place of the Mars backdrop; false when there is
// nothing to draw
bool menu_scene_draw_background();
void menu_scene_on_setting_changed();
