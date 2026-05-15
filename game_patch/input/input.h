#pragma once

#include "../rf/player/control_config.h"

// Sentinel scan code injected into Input Rebind UI, allowing additional input bindings.
static constexpr int CTRL_REBIND_SENTINEL = 0x58; // KEY_F12

// Custom scan codes for extra mouse buttons (Mouse 4 and above), stored in scan_codes[0].
static constexpr int CTRL_EXTRA_MOUSE_SCAN_BASE  = 0x75;
static constexpr int CTRL_EXTRA_MOUSE_SCAN_COUNT = 5;

rf::ControlConfigAction get_af_control(rf::AlpineControlConfigAction alpine_control);
rf::String get_action_bind_name(int action);
void mouse_apply_patch();
int  mouse_take_pending_rebind();
void key_apply_patch();
int  key_take_pending_extra_rebind();
