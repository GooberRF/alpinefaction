#pragma once

void side_scroller_do_patch();
void side_scroller_on_level_load();
bool is_side_scroller_mode();

// Called by hud_weapons to get reticle offset for side-scroller mode
bool side_scroller_get_reticle_offset(int& offset_x, int& offset_y);
