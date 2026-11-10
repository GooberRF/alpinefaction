#pragma once

#include <SDL3/SDL.h>
#include <string>
#include "../rf/player/control_config.h"

// Sentinel scan code injected into Input Rebind UI, allowing additional input bindings.
static constexpr int CTRL_REBIND_SENTINEL = 0x58; // KEY_F12

// Custom scan codes for extra mouse buttons (Mouse 4 and above). Placed in the extended
// range (0x80 | scan) at slots no physical keyboard emits, so they cannot collide with
// real key presses.
static constexpr int CTRL_EXTRA_MOUSE_SCAN_BASE = 0xF5;
static constexpr int CTRL_EXTRA_MOUSE_SCAN_COUNT = 5;

inline bool ctrl_is_extra_mouse_scan(int scan)
{
    return scan >= CTRL_EXTRA_MOUSE_SCAN_BASE && scan < CTRL_EXTRA_MOUSE_SCAN_BASE + CTRL_EXTRA_MOUSE_SCAN_COUNT;
}

rf::ControlConfigAction get_af_control(rf::AlpineControlConfigAction alpine_control);
// Index of the first Alpine action row in ControlConfig::bindings, or -1 before control init.
int alpine_control_start_index();
rf::String get_action_bind_name(int action);
// The bind name, or `fallback` for an unbound action. Allocates on the engine heap: not per frame.
std::string get_action_bind_name_or(int action, const char* fallback);
void mouse_apply_patch();
void key_apply_patch();
void gamepad_apply_patch();
void gamepad_do_frame();
void sdl_input_poll();
void control_input_filter_apply_patch();

enum class InputDevice {
    KeyboardMouse,
    Gamepad,
};

void input_record_activity(InputDevice device, SDL_JoystickID gamepad_id = 0);
InputDevice input_active_device();
InputDevice input_prompt_device();
SDL_JoystickID input_recent_gamepad_id();
void input_clear_recent_gamepad();
