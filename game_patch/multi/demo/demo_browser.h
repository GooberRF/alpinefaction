#pragma once

// Demo browser overlay for the Extras menu: adds a "DEMOS" button to the stock Extras
// submenu that opens a scrollable list of <rf_root>\demos\*.afd files; clicking an
// entry starts playback. Installed from demo_do_patch().

void demo_browser_apply_patch();
// Overlay renderer; called from after_frame_render_hook (main.cpp).
void demo_browser_render();
bool demo_browser_is_open();
// Opens (or refreshes) the browser overlay. Only meaningful while the Extras menu
// state is active - the renderer force-closes it in any other state. Also used by the
// demo-exit navigation in misc.cpp to land back on the demo list after playback.
void demo_browser_open();
