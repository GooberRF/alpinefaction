#pragma once

#include "../rf/player/control_config.h"

// Custom scan codes for extra mouse buttons (Mouse 4 and above). Placed in the extended
// range (0x80 | scan) at slots no physical keyboard emits, so they cannot collide with
// real key presses.
static constexpr int CTRL_EXTRA_MOUSE_SCAN_BASE = 0xF5;
static constexpr int CTRL_EXTRA_MOUSE_SCAN_COUNT = 5;

rf::ControlConfigAction get_af_control(rf::AlpineControlConfigAction alpine_control);
rf::String get_action_bind_name(int action);
void mouse_apply_patch();
void key_apply_patch();
