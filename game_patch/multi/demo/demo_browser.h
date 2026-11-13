#pragma once

class UiContext;
struct MenuPanel;

// Demo browser for the Extras menu: a scrollable list of <rf_root>\demos\*.afd files, shown as
// the Alpine Extras menu's Demos panel, or as an overlay over the stock Extras menu.

void demo_browser_apply_patch();
// Draws the legacy overlay, and closes the browser once the Extras menu is left; called from
// after_frame_render_hook (main.cpp).
void demo_browser_render();
bool demo_browser_is_open();
// Opens (or refreshes) the browser. Only meaningful while the Extras menu
// state is active - the renderer force-closes it in any other state. Also used by the
// demo-exit navigation in misc.cpp to land back on the demo list after playback.
void demo_browser_open();
// Up one folder, or closed at the root.
void demo_browser_back();
// The Demos panel's contents inside the frame the Extras menu drew, for either pass.
void demo_browser_build_panel(UiContext& ctx, const MenuPanel& panel, int canvas_y);
// Runs the row or PLAY press the hit-test pass took, once the pass is over.
void demo_browser_commit_panel();
