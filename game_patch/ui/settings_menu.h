#pragma once

void settings_menu_enter();
void settings_menu_do_frame(bool paused);
void settings_menu_push_toast(const char* text);
bool settings_menu_is_closed();
// A queued menu jump closes the state without polling settings_menu_is_closed().
void settings_menu_abandon();
