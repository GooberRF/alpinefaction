#pragma once

#include "../../rf/player/control_config.h"

// In-gameplay "demo controls" popup for demo playback: scrubber with
// preview-on-drag (the seek fires on mouse release - seeking is expensive),
// -10s/+10s jump buttons and play/pause. Toggled with the player's USE key,
// which is swallowed for the whole playback session. Mutually exclusive with
// the other gameplay overlays (vote panel, remote server config).

bool demo_controls_ui_is_open();
// Closes the popup if open (restores mouse mode, arms the attack swallow).
void demo_controls_ui_close();
// Playback key bindings: USE toggles the popup, Vote Yes/No rewind/skip 10s,
// Reload toggles pause. Returns true when the action was consumed (press or
// release during demo playback). Called from player_execute_action_hook.
bool demo_controls_ui_execute_action(rf::ControlConfigAction action, bool was_pressed);
// Hit-test/input pass; called every frame from vote_panel's gameseq_process hook,
// before the state runs its frame.
void demo_controls_ui_input();
// Draw pass; called from vote_panel's gameseq_state_do_frame hook (outermost
// dispatch only).
void demo_controls_ui_render();
