#pragma once

// Multiplayer client: two speed lines that trail from the ankles of a hasted player moving faster than the multiplayer
// run speed, fading out behind them.

void haste_trails_clear();
// From rf_do_frame: samples each hasted player's position once a frame, as a level may be drawn in several views.
void haste_trails_do_frame();
void haste_trails_render();
