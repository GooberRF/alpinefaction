#pragma once

void mouse_apply_patch();
void consume_raw_mouse_deltas(float& out_pitch, float& out_yaw, bool apply_scope_sens);
void consume_vehicle_orbit_mouse_deltas(float& out_pitch, float& out_yaw);
void mouse_handle_xbutton_wm(int rf_btn, bool down);
void apply_mouse_scale();
void apply_direct_input();
