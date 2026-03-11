#pragma once

void gamepad_apply_patch();
void gamepad_do_frame();
void gamepad_get_camera(float& pitch_delta, float& yaw_delta);
bool gamepad_is_motionsensors_supported();
