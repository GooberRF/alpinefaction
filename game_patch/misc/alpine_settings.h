#pragma once

#include <algorithm>

extern bool g_loaded_alpine_settings_file;

struct AlpineGameSettings
{
    // fov
    static constexpr float min_fov = 75.0f;
    static constexpr float max_fov = 160.0f;
    float horz_fov = 90.0f;
    void set_horz_fov(float fov)
    {
        if (fov == 0.0f) {
            horz_fov = 0.0f; // Allow 0.0f for auto scaling
        }
        else {
            horz_fov = std::clamp(fov, min_fov, max_fov);
        }
    }

    // fpgun fov scale
    static constexpr float min_fpgun_fov_scale = 0.1f;
    static constexpr float max_fpgun_fov_scale = 1.5f;
    float fpgun_fov_scale = 1.0f;
    void set_fpgun_fov_scale(float scale)
    {
        fpgun_fov_scale = std::clamp(scale, min_fpgun_fov_scale, max_fpgun_fov_scale);
    }

    // scope and scanner sens modifiers
    static constexpr float min_sens_mod = 0.01f;
    static constexpr float max_sens_mod = 10.0f;
    float scope_sensitivity_modifier = 0.25f;    
    void set_scope_sens_mod(float mod)
    {
        scope_sensitivity_modifier = std::clamp(mod, min_sens_mod, max_sens_mod);
    }
    float scanner_sensitivity_modifier = 0.25f;
    void set_scanner_sens_mod(float mod)
    {
        scanner_sensitivity_modifier = std::clamp(mod, min_sens_mod, max_sens_mod);
    }

    bool scope_static_sensitivity = false;
    bool swap_ar_controls = false;
    bool swap_gn_controls = false;
    bool swap_sg_controls = false;
    bool mouse_linear_pitch = false;
    bool big_hud = false;
    int skip_cutscene_bind_alias = -1;
    bool try_disable_weapon_shake = false;
    bool try_fullbright_characters = false;
    bool try_disable_textures = false;
    bool try_disable_muzzle_flash_lights = false;
};

extern AlpineGameSettings g_alpine_game_config;
void set_big_hud(bool is_big);
void update_scope_sensitivity();
void update_scanner_sensitivity();
