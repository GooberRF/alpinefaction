#pragma once

#include <string_view>

// Returns -1 on failure; only files from alpinefaction.vpp are loaded.
// A handle left unrequested for a while is released, so re-query it every frame.
int svg_bitmap(std::string_view name, int w, int h);
