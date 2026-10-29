#pragma once

void debug_apply_patches();
void debug_init();
void debug_cleanup();
void debug_render();
void debug_render_ui();
void debug_do_frame_pre();
void debug_do_frame_post();
// For long work on the main thread that is still making progress, so the watchdog does not report it as hung.
void debug_unresponsive_do_update();
