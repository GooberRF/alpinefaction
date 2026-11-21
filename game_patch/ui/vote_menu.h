#pragma once

// The Call Vote menu: the server's vote tabs down the column, each with its form beside it. It runs inside the in-game
// multiplayer menu, or over the game when its control opens it.

// In the in-game multiplayer menu's place.
void vote_menu_open();
// From the open until its exit animation is done.
bool vote_menu_is_active();
void vote_menu_do_frame(bool paused);
// Closes the menu the in-game multiplayer menu runs without animating.
void vote_menu_release();

// Over the game. vote_panel opens and closes it, and feeds it the frame.
void vote_menu_open_over_game();
void vote_menu_close_over_game();
bool vote_menu_is_over_game();
// The frame's hit-test pass, before the game's; false when the menu asked to close.
bool vote_menu_over_game_input();
// The frame's draw pass, over the game's.
void vote_menu_over_game_render();
// Esc during gameplay.
void vote_menu_over_game_escape();
// A text box or a dialog over the game takes every key, which vote_menu_key then gets.
bool vote_menu_takes_keys();
void vote_menu_key(int key);

// The form is about to be dropped: lets go of anything editing it.
void vote_menu_drop_edits();
