#pragma once

#include <string_view>

void waypoints_utils_level_init();
void waypoints_utils_level_reset();
void waypoints_utils_on_level_unloaded();
void waypoints_utils_do_frame();
void waypoints_utils_render_overlay();
void waypoints_utils_render_debug();
void waypoints_utils_log(std::string_view message);
bool waypoints_utils_should_block_mouse_look();
bool waypoints_utils_link_editor_text_input_active();
void waypoints_utils_capture_numeric_key(int key_code, int count);
int waypoints_utils_consume_numeric_key(int key_code);
