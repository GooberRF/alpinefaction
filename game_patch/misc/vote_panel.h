#pragma once

// Modal "CALL VOTE" panel.

// Closes the overlay if it is up (restores mouse mode, arms the attack swallow).
void vote_panel_close();
// Bindable control: opens/closes the overlay during a multiplayer game.
void vote_panel_toggle_gameplay();
// True while the overlay is up; drives HUD/chat/input suppression.
bool vote_panel_is_gameplay_overlay_active();
// Drop the whole form (leaving a server).
void vote_panel_reset();

void vote_panel_apply_patch();
