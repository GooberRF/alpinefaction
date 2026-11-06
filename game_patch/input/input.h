#pragma once

#include "../rf/player/control_config.h"

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
void mouse_apply_patch();
void key_apply_patch();
void control_input_filter_apply_patch();
