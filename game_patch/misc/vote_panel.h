#pragma once

// The Call Vote menu over the game: opening, closing and feeding it the frame.

// A client on a server, which is where votes are called.
bool vote_panel_available();
// Closes the overlay if it is up (restores mouse mode, arms the attack swallow).
void vote_panel_close();
// Bindable control: opens/closes the overlay during a multiplayer game.
void vote_panel_toggle_gameplay();
// True while the overlay is up; drives HUD/chat/input suppression.
bool vote_panel_is_gameplay_overlay_active();
// True while a text box or a dialog of the overlay takes the keys. Anything that reads
// key state directly (rather than through the control config, which the overlay's veto
// already covers) has to consult this the same way it consults console/chat visibility.
bool vote_panel_is_capturing_text();
// Drop the whole form (leaving a server).
void vote_panel_reset();

// Cursor/mouse-look switch shared by gameplay overlays (vote panel, demo controls
// popup, scoreboard cursor, console cursor): true frees the cursor and disables
// mouse-look, false restores the player's saved mouse-look and re-grabs the cursor
// when still in gameplay. Safe to call every frame while an overlay is open.
void gameplay_overlay_apply_mouse(bool active);
// Any gameplay overlay currently has a free cursor.
bool gameplay_overlay_cursor_active();
// For an overlay that has just cleared its own open state: the last one to close restores the mouse.
void gameplay_overlay_release_mouse();
bool gameplay_overlay_mouse_is_overridden();

void vote_panel_apply_patch();
