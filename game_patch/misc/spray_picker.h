#pragma once

#include "../rf/input.h"

void spray_picker_open();
bool spray_picker_is_open();
void spray_picker_render();
void spray_picker_handle_mouse(int x, int y);
void spray_picker_handle_key(rf::Key* key);
