#pragma once

// Modal "CALL VOTE" panel. Openable two ways: the CALL VOTE button in the
// multiplayer menu (GS_MULTI_MENU), or a bindable Alpine control during
// gameplay. Only one instance exists; the open context decides which site
// renders it, so it is never drawn twice in a frame.

bool vote_panel_is_open();
void vote_panel_open();
void vote_panel_close();

// Drawn from the multi menu render injection, after the menu buttons and before
// the mouse cursor. No-op unless the panel was opened from the multi menu.
void vote_panel_render();

// Called from the multi menu mouse/key hooks while the panel owns input.
void vote_panel_handle_mouse();
void vote_panel_handle_key(int key);

// --- gameplay overlay context ---
// Bindable control: opens/closes the overlay during a multiplayer game.
void vote_panel_toggle_gameplay();
// True while the overlay is up in gameplay; drives HUD/chat/input suppression.
bool vote_panel_is_gameplay_overlay_active();
// Per-frame input pump, run before the gameplay state takes its own input.
void vote_panel_gameplay_input();
// Drawn from the after-scene render hook so it lands on top of the HUD.
void vote_panel_gameplay_render();

void vote_panel_apply_patch();
