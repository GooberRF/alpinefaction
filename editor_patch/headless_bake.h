#pragma once

// Headless lightmap bake: "-bake <input.rfl> -bakeout <output.rfl>"
// Steps: load the input, Build Geometry (driven to completion, so terrain chunks are compiled and
// lightmap fragments repacked), Calculate Lighting with shadows, save the output.
// Exit codes: 0 saved, 1 bad arguments or input not found, 2 load failed, 3 too many dialogs,
// 4 save failed, 5 Calculate Lighting refused the level (nothing saved), 6 Build Geometry did not
// complete (nothing saved).
bool headless_bake_active();
const char* headless_bake_input_path();
// Appends a line to the bake's own log; a no-op outside bake mode.
void headless_bake_note(const char* line);
// Calculate Lighting refused to bake; the run exits 5 instead of saving.
void headless_bake_mark_refused();
// RED's idle tick; true in bake mode, which owns it (the first ticks start the bake).
bool headless_bake_idle();
// A level load (not an autosave) returned `ok`.
void headless_bake_level_loaded(const char* path, bool ok);
void ApplyHeadlessBakePatches();
