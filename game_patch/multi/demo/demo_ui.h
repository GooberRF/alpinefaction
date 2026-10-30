#pragma once

#include "../../rf/player/control_config.h"

// Demo playback's controls panel, which also carries the spectate nameplate as its
// bottom row (multi_spectate skips its own plaque and mode label meanwhile).
// Expanded, it adds a scrubber with preview-on-drag (the seek fires on mouse
// release - seeking is expensive), -10s/+10s jump, play/pause and set-marker buttons
// labelled with their shortcuts, a playback speed button (click faster, right-click slower), and clickable previous/next arrows; while following a
// player in first person the scrubber carries clickable markers at their kills, powerup
// pickups, objective events (flags, bags, point captures) and awards (hover names the
// award). The viewer's own custom marker shows in every camera and seeks exactly to its
// moment. Compact, it is the nameplate under a progress line, or with spectate_minimalbar
// just the stock-sized nameplate.
// Expanded by default (except in free look and third-person orbit, which need the
// mouse); the player's USE key switches between the two and is swallowed for the
// whole playback session. Clicks on the expanded panel are swallowed; clicks
// elsewhere still cycle the spectated player. Mutually exclusive with the other
// gameplay overlays (vote panel, remote server config).

// True while expanded (interactive, free cursor).
bool demo_controls_ui_is_open();
// True once the viewer has set a custom marker this playback session.
bool demo_controls_ui_has_custom_marker();
// True when the panel draws the spectate nameplate this frame (demo playback in
// gameplay, expanded or with the HUD shown).
bool demo_controls_ui_draws_nameplate();
// Compacts the panel if expanded (restores mouse mode, arms the attack swallow).
void demo_controls_ui_close();
// Playback key bindings: USE expands/compacts the panel, Vote Yes/No rewind/skip 10s,
// Reload toggles pause, Quick Save sets the custom marker at the current position, Quick
// Load jumps to it. Returns true when the action was consumed (press or
// release during demo playback). Called from player_execute_action_hook.
bool demo_controls_ui_execute_action(rf::ControlConfigAction action, bool was_pressed);
// Hit-test/input pass; called every frame from vote_panel's gameseq_process hook,
// before the state runs its frame.
void demo_controls_ui_input();
// Draw pass; called from vote_panel's gameseq_state_do_frame hook (outermost
// dispatch only).
void demo_controls_ui_render();
