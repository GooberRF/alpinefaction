#pragma once

#include "../rf/player/control_config.h"

// Sentinel scan code injected into Input Rebind UI, allowing additional input bindings.
static constexpr int CTRL_REBIND_SENTINEL = 0x58; // KEY_F12

// Custom scan codes for extra mouse buttons (Mouse 4 and above). Placed in the extended
// range (0x80 | scan) at slots no physical keyboard emits, so they cannot collide with
// real key presses.
static constexpr int CTRL_EXTRA_MOUSE_SCAN_BASE = 0xF5;
static constexpr int CTRL_EXTRA_MOUSE_SCAN_COUNT = 5;

rf::ControlConfigAction get_af_control(rf::AlpineControlConfigAction alpine_control);
rf::String get_action_bind_name(int action);
void mouse_apply_patch();
void camera_start_reset_to_horizon();
void key_apply_patch();
void gamepad_apply_patch();
void gamepad_do_frame();
void sdl_input_poll();
