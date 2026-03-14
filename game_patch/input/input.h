#pragma once

#include "../rf/player/control_config.h"

rf::ControlConfigAction get_af_control(rf::AlpineControlConfigAction alpine_control);
rf::String get_action_bind_name(int action);
void mouse_apply_patch();
void mouse_init_sdl_window();
void mouse_sdl_poll();
void key_apply_patch();
