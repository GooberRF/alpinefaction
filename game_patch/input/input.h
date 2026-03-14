#pragma once

#include "../rf/player/control_config.h"

rf::ControlConfigAction get_af_control(rf::AlpineControlConfigAction alpine_control);
rf::String get_action_bind_name(int action);
void mouse_apply_patch();
void camera_start_reset_to_horizon();
void key_apply_patch();
void gamepad_apply_patch();
void gamepad_do_frame();
