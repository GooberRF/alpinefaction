#pragma once

void mouse_apply_patch();
void consume_raw_mouse_deltas(float& out_pitch, float& out_yaw, bool apply_scope_sens);
