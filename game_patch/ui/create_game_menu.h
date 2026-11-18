#pragma once

// Create Game (GS_MULTI_CREATE_GAME) under the new menus: a column of its own, OPTIONS, MAPS, BOTS and MUTATORS as its
// panels, and START hosting the setup they edit.

void create_game_menu_enter();
void create_game_menu_do_frame(bool paused);
bool create_game_menu_is_closed();
// In place of stock's close of the state.
void create_game_menu_on_close(bool immediately);
