#pragma once

#include "../rf/player/control_config.h"

// Sentinel scan code injected during rebind to make RF process the binding entry.
// On the falling edge of waiting_for_key, the sentinel is replaced with the real scan code.
static constexpr int CTRL_REBIND_SENTINEL = 0x58; // KEY_F12

// Custom scan codes for extra mouse buttons (Mouse 4 and above), stored in scan_codes[0].
// Range placed after CTRL_GAMEPAD_RIGHT_TRIGGER (0x74) to avoid conflicts.
static constexpr int CTRL_EXTRA_MOUSE_SCAN_BASE  = 0x75; // scan code for rf_btn 3 (Mouse 4)
static constexpr int CTRL_EXTRA_MOUSE_SCAN_COUNT = 5;    // covers Mouse 4-8 (rf indices 3-7)

rf::ControlConfigAction get_af_control(rf::AlpineControlConfigAction alpine_control);
rf::String get_action_bind_name(int action);
void mouse_apply_patch();
void mouse_init_sdl_window();
int  mouse_take_pending_rebind();
void mouse_sdl_poll();
void keyboard_sdl_poll();
void sdl_input_poll();
void key_apply_patch();
void set_input_mode(int mode);
