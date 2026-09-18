#pragma once

#include <SDL3/SDL.h>
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
void key_apply_patch();
void gamepad_apply_patch();
void gamepad_do_frame();
void sdl_input_poll();
void control_input_filter_apply_patch();

// Glyph Detection
void input_active_gamepad(SDL_JoystickID which = 0);
void input_active_keyboard_mouse();
bool input_last_gamepad_active();
SDL_JoystickID input_get_last_active_gamepad_id();
void input_reset_last_active_gamepad_id();
