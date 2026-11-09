#pragma once

// The Reference screen the Extras and in-game multiplayer menus open, run inside the opening menu's state.

void reference_menu_open();
// From the open until its exit animation is done.
bool reference_menu_is_active();
void reference_menu_do_frame(bool paused);
// Drops the preview's mesh and render target, and closes the screen without animating.
void reference_menu_release();
// For a menu that opens the screen in its place: advances the menu's `slide`, opens the screen once the menu has
// slid out with `pending` set, and hands it the frame while it is active. True when the screen had the frame.
bool reference_menu_host_frame(bool& pending, float& slide, bool closing, bool paused);
