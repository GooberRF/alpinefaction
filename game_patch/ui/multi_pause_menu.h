#pragma once

// The in-game multiplayer menu: stock's main menu state while a multiplayer game runs beneath it.

// After stock's main menu init: takes the state when it is the in-game multiplayer menu.
void multi_pause_menu_on_main_menu_init();
// From that init until the state closes.
bool multi_pause_menu_is_active();
void multi_pause_menu_do_frame(bool paused);
bool multi_pause_menu_is_closed();
// For a state closed without asking multi_pause_menu_is_closed, as a forced change does.
void multi_pause_menu_release();
