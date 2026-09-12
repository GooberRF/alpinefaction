#pragma once

// Headless lightmap bake: "-bake <input.rfl> -bakeout <output.rfl>"
bool headless_bake_active();
const char* headless_bake_input_path();
void ApplyHeadlessBakePatches();
