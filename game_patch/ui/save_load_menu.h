#pragma once

#include "../rf/gameseq.h"

// The Save Game and Load Game screens: stock's list of saved games in a panel beside a column of SAVE or LOAD, and
// BACK. The panel's footer holds SAVE or LOAD again, beside the name box on Save, and DELETE.

void save_load_menu_enter(rf::GameState state);
void save_load_menu_do_frame(bool paused);
bool save_load_menu_is_closed();
