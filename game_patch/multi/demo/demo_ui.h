#pragma once

#include "../../rf/player/control_config.h"

// Demo playback's controls panel. It carries the spectate nameplate as its bottom row, so
// multi_spectate skips its own plaque and mode label meanwhile. A scrub seeks on mouse release
// because seeking is expensive.

// Expanded (interactive, free cursor)
bool demo_controls_ui_is_open();
bool demo_controls_ui_has_custom_marker();
bool demo_controls_ui_draws_nameplate();
// Top of the panel this frame, or the clip height when it is not drawn
int demo_controls_ui_top_y();
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
