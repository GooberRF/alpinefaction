#pragma once

#include <string>
#include "../rf/player/control_config.h"
#include "mouse.h"

// Sentinel scan code injected into Input Rebind UI, allowing additional input bindings.
static constexpr int CTRL_REBIND_SENTINEL = 0x58; // KEY_F12

// Extra keyboard scan codes not in RF's original DInput table.
// Placed after CTRL_EXTRA_MOUSE_SCAN (0x75–0x79), before RF extended keys (0x9C+).
static constexpr int CTRL_EXTRA_KEY_SCAN_BASE  = 0x7A;
static constexpr int CTRL_EXTRA_KEY_SCAN_COUNT = 14;

enum ExtraKeyScanOffset : int {
    EXTRA_KEY_KP_DIVIDE = 0,  // SDL_SCANCODE_KP_DIVIDE
    EXTRA_KEY_NONUSBACKSLASH, // SDL_SCANCODE_NONUSBACKSLASH  (ISO key between LShift and Z)
    EXTRA_KEY_F13,            // SDL_SCANCODE_F13 … F24
    EXTRA_KEY_F14,
    EXTRA_KEY_F15,
    EXTRA_KEY_F16,
    EXTRA_KEY_F17,
    EXTRA_KEY_F18,
    EXTRA_KEY_F19,
    EXTRA_KEY_F20,
    EXTRA_KEY_F21,
    EXTRA_KEY_F22,
    EXTRA_KEY_F23,
    EXTRA_KEY_F24,
};

rf::ControlConfigAction get_af_control(rf::AlpineControlConfigAction alpine_control);
rf::String get_action_bind_name(int action);
void keyboard_sdl_poll();
int  key_take_pending_extra_rebind();
std::string keyboard_take_pending_text();
void key_apply_patch();
void set_input_mode(int mode);
void ui_refresh_input_mode_label();
