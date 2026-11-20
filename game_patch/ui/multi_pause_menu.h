#pragma once

// The in-game multiplayer menu's MULTI column, handed off to in the pause column's place.

void multi_pause_menu_open();
// From the open until its exit animation is done.
bool multi_pause_menu_is_active();
void multi_pause_menu_do_frame(bool paused);
// Closes it without animating, for a state closed under it.
void multi_pause_menu_release();
