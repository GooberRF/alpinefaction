#pragma once

namespace rf
{
    enum GameState : int;
}

// The Multi menu (GS_MULTI_MENU) and its Join Game browser (GS_MULTI_SERVER_LIST): one column over both states, the
// browser its panel while the second is on top.

void multi_menu_enter(rf::GameState old_state);
void multi_menu_enter_server_list();
// Either state's frame.
void multi_menu_do_frame(bool paused);
bool multi_menu_is_closed();
// Whether the column last closed as a jump passed through it to a menu below.
bool multi_menu_passed_by_jump();
// Before stock's close of either state.
void multi_menu_on_close(rf::GameState state, rf::GameState new_state);
