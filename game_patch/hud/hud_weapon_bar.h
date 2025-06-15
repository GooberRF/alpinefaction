#pragma once

// Function declarations for weapon bar functionality
void render_weapon_bar();
void hud_weapon_bar_apply_patches();
void hud_weapon_bar_update_config();
void hud_weapon_bar_set_big(bool is_big);

// External C functions for integration
extern "C" {
    void render_weapon_bar_external();
    bool is_weapon_bar_enabled();
} 