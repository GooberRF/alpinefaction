#pragma once

#include "../rf/gr/gr.h"

// Coordinates are clip-relative screen pixels, like rf::gr::bitmap.

void ui_rounded_rect(float x, float y, float w, float h, float radius, rf::Color color);

// The border is drawn inside the rect's footprint.
void ui_rounded_rect_border(float x, float y, float w, float h, float radius, float thickness, rf::Color color);

void ui_aa_line(float x0, float y0, float x1, float y1, float width, rf::Color color);
