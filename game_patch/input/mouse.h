#pragma once

// Custom scan codes for extra mouse buttons (Mouse 4 and above), stored in scan_codes[0].
// Range placed after CTRL_GAMEPAD_RIGHT_TRIGGER (0x74) to avoid conflicts.
static constexpr int CTRL_EXTRA_MOUSE_SCAN_BASE  = 0x75; // scan code for rf_btn 3 (Mouse 4)
static constexpr int CTRL_EXTRA_MOUSE_SCAN_COUNT = 5;    // covers Mouse 4-8 (rf indices 3-7)

void mouse_apply_patch();
void mouse_on_focus_changed(bool focused);
int  mouse_take_pending_rebind();
void mouse_sdl_poll();
void consume_raw_mouse_deltas(float& out_pitch, float& out_yaw, bool apply_scope_sens);
