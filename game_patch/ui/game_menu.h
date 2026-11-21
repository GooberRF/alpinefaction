#pragma once

// Stock's main menu state: the title menu, or the pause menu over a single player or multiplayer game.

// After stock's main menu init. `passing_through`: a menu jump leaves the state at once.
void game_menu_on_main_menu_init(bool passing_through);
// From that init until the state closes.
bool game_menu_is_active();
void game_menu_do_frame(bool paused);
bool game_menu_is_closed();
// For a state closed without asking game_menu_is_closed, as a forced change does.
void game_menu_release();
