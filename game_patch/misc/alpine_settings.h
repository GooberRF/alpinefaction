#pragma once

#include <algorithm>
#include "../rf/os/timestamp.h"
#include "../hud/hud.h"
#include "../hud/remote_server_cfg_ui.h"

extern bool g_loaded_alpine_settings_file;

struct AlpineGameSettings
{
    // fov
    static constexpr float min_fov = 75.0f;
    static constexpr float max_fov = 160.0f;
    float horz_fov = 0.0f;
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

    float level_sound_volume = 1.0f;
    void set_level_sound_volume(float scale)
    {
        level_sound_volume = std::clamp(scale, 0.0f, 1.0f);
    }

    // lod settings
    bool multi_no_character_lod = true;

    float entity_sim_distance = 100.0f;
    void set_entity_sim_distance(float dist)
    {
        entity_sim_distance = std::clamp(dist, 1.0f, 100000.0f);
    }

    float lod_dist_scale = 10.0f;
    void set_lod_dist_scale(float scale)
    {
        lod_dist_scale = std::clamp(scale, 0.1f, 1000.0f);
    }

    int monitor_resolution_scale = 2;
    void set_monitor_resolution_scale(int scale)
    {
        monitor_resolution_scale = std::clamp(scale, 1, 8);
    }

    bool scope_static_sensitivity = false;
    bool swap_ar_controls = false;
    bool swap_gn_controls = false;
    bool swap_sg_controls = false;
    bool mouse_linear_pitch = true;
    bool big_hud = false;
    int skip_cutscene_bind_alias = -1;
    bool try_disable_weapon_shake = false;
    bool try_fullbright_characters = false;
    bool try_disable_textures = false;
    bool try_disable_muzzle_flash_lights = false;
    bool world_hud_ctf_icons = true;
    bool world_hud_alt_damage_indicators = false;
    bool world_hud_flag_overdraw = true;
    bool world_hud_hill_overdraw = true;
    bool world_hud_damage_numbers = true;
    bool world_hud_spectate_player_labels = false;
    bool world_hud_team_player_labels = false;
    bool show_location_pings = true;
    bool play_hit_sounds = true;

    static constexpr int min_hit_sound_interval_ms = 0;
    static constexpr int max_hit_sound_interval_ms = 1000;
    int hit_sound_min_interval_ms = 20;
    void set_hit_sound_min_interval_ms(int interval_ms)
    {
        hit_sound_min_interval_ms = std::clamp(interval_ms, min_hit_sound_interval_ms, max_hit_sound_interval_ms);
    }

    bool play_taunt_sounds = true;
    bool play_global_rad_msg_sounds = true;
    bool play_team_rad_msg_sounds = true;
    bool unlimited_semi_auto = false;
    bool gaussian_spread = false;
    bool show_run_timer = true;
    bool multi_ricochet = false;
    bool damage_screen_flash = true;
    bool explosion_weapon_flash_lights = true;
    bool explosion_env_flash_lights = true;
    bool burning_entity_lights = true;
    bool death_bars = true;
    bool mesh_static_lighting = true;
    bool show_glares = true;
    bool show_enemy_bullets = true;
    bool fps_counter = true;
    static constexpr int min_fps_counter_average_ms = 0;
    static constexpr int max_fps_counter_average_ms = 60000;
    int fps_counter_average_ms = 100;
    void set_fps_counter_average_ms(int window_ms)
    {
        fps_counter_average_ms = std::clamp(window_ms, min_fps_counter_average_ms, max_fps_counter_average_ms);
    }
    bool speed_display = false;
    bool ping_display = true;
    bool spectate_mode_minimal_ui = false;
    bool save_console_history = false; // checked before config loaded, must be false here
    bool screen_shake_force_off = false;
    bool display_target_player_names = true;
    bool verbose_time_left_display = true;
    bool nearest_texture_filtering = false;
    bool direct_input = true;
    bool scoreboard_anim = true;
    bool scoreboard_split_simple = true;
    bool scoreboard_split_spectators = true;
    bool scoreboard_split_bots = false;
    bool scoreboard_split_browsers = true;
    bool scoreboard_split_idle = false;
    bool autosave = true;
    bool af_branding = true;
    int seasonal_effect = 1; // 0=none, 1=auto, 2=always_snow
    bool player_join_beep = false;
    bool full_range_lighting = true;
    bool always_clamp_official_lightmaps = false;
    bool static_bomb_code = false;
    bool entity_pain_sounds = true;
    bool footsteps = true;
    static constexpr int min_gib_chunk_count = 7;
    static constexpr int max_gib_chunk_count = 100;
    int gib_chunk_count = 14;
    void set_gib_chunk_count(int count)
    {
        gib_chunk_count = std::clamp(count, min_gib_chunk_count, max_gib_chunk_count);
    }
    static constexpr float min_gib_velocity_scale = 3.0f;
    static constexpr float max_gib_velocity_scale = 100.0f;
    float gib_velocity_scale = 15.0f;
    void set_gib_velocity_scale(float scale)
    {
        gib_velocity_scale = std::clamp(scale, min_gib_velocity_scale, max_gib_velocity_scale);
    }
    static constexpr int min_gib_lifetime_ms = 1000;
    static constexpr int max_gib_lifetime_ms = 15000;
    int gib_lifetime_ms = 7000;
    void set_gib_lifetime_ms(int lifetime_ms)
    {
        gib_lifetime_ms = std::clamp(lifetime_ms, min_gib_lifetime_ms, max_gib_lifetime_ms);
    }
    bool real_armor_values = false;
    bool always_show_spectators = false;
    RemoteServerCfgPopup::DisplayMode remote_server_cfg_display_mode =
        RemoteServerCfgPopup::DISPLAY_MODE_ALIGN_RIGHT_HIGHLIGHT_BOX;
    bool simple_server_chat_msgs = true;
    bool quick_exit = false;
    uint32_t bot_shared_secret = 0;
    int suppress_autoswitch_alias = -1;
    bool always_autoswitch_empty = true;
    bool apply_exposure_damage = true;

    // hud color overrides
    std::optional<uint32_t> sniper_scope_color_override{};
    std::optional<uint32_t> precision_scope_color_override{};
    std::optional<uint32_t> rail_scope_color_override{};
    std::optional<uint32_t> ar_ammo_digit_color_override{};
    std::optional<uint32_t> damage_notify_color_override{};
    std::optional<uint32_t> location_ping_color_override{};
    std::optional<uint32_t> multi_timer_color_override{};
    std::optional<uint32_t> teammate_label_color_override{};
    std::optional<uint32_t> reticle_color_override{};
    std::optional<uint32_t> reticle_locked_color_override{};
    bool colorize_custom_reticles = false;

    // hud scale overrides
    std::optional<float> reticle_scale{};
    float get_reticle_scale() const
    {
        return reticle_scale.value_or(1.0f);
    }
    void set_reticle_scale(float scale)
    {
        reticle_scale = std::clamp(scale, 0.0f, 100.0f);
    }
    void clear_reticle_scale()
    {
        reticle_scale.reset();
    }

    std::optional<float> world_hud_damage_text_scale{};
    float get_world_hud_damage_text_scale() const
    {
        return world_hud_damage_text_scale.value_or(1.0f);
    }
    void set_world_hud_damage_text_scale(float scale)
    {
        world_hud_damage_text_scale = std::clamp(scale, 0.5f, 3.0f);
    }
    void clear_world_hud_damage_text_scale()
    {
        world_hud_damage_text_scale.reset();
    }

    std::optional<float> world_hud_label_text_scale{};
    float get_world_hud_label_text_scale() const
    {
        return world_hud_label_text_scale.value_or(1.0f);
    }
    void set_world_hud_label_text_scale(float scale)
    {
        world_hud_label_text_scale = std::clamp(scale, 0.5f, 3.0f);
    }
    void clear_world_hud_label_text_scale()
    {
        world_hud_label_text_scale.reset();
    }

    std::optional<float> world_hud_ping_label_text_scale{};
    float get_world_hud_ping_label_text_scale() const
    {
        return world_hud_ping_label_text_scale.value_or(1.0f);
    }
    void set_world_hud_ping_label_text_scale(float scale)
    {
        world_hud_ping_label_text_scale = std::clamp(scale, 0.5f, 3.0f);
    }
    void clear_world_hud_ping_label_text_scale()
    {
        world_hud_ping_label_text_scale.reset();
    }

    int picmip = 1; // d3d11 only
    void set_picmip(int value)
    {
        picmip = std::clamp(value, 1, 256);
    }

    int colorblind_mode = 0;    // 0=off,1=protanopia,2=deuteranopia,3=tritanopia (d3d11 only)
    void set_colorblind_mode(int value)
    {
        colorblind_mode = std::clamp(value, 0, 3);
    }

    bool precache_rooms = true; // d3d11 only

    int suppress_autoswitch_fire_wait = 0;
    void set_suppress_autoswitch_fire_wait(int value)
    {
        suppress_autoswitch_fire_wait = std::clamp(value, 0, 10000);
    }

    std::string multiplayer_tracker = "rfgt.factionfiles.com";
    static constexpr size_t max_tracker_hostname_length = 200;
    void set_multiplayer_tracker(const std::string& tracker_hostname)
    {
        if (!tracker_hostname.empty() && tracker_hostname.length() <= max_tracker_hostname_length)
            multiplayer_tracker = tracker_hostname;
        else
            multiplayer_tracker = "rfgt.factionfiles.com";
    }

    // max_fps default is 120
    static constexpr unsigned min_fps_limit = 1u;
    static constexpr unsigned max_fps_limit = 100000u;
    static constexpr unsigned max_fps_limit_mp = 240u;
    unsigned max_fps = 240u;
    void set_max_fps(unsigned fps_value)
    {
        max_fps = std::clamp(fps_value, min_fps_limit, max_fps_limit);
    }

    unsigned server_max_fps = 60u;
    void set_server_max_fps(unsigned fps_value)
    {
        server_max_fps = std::clamp(fps_value, min_fps_limit, max_fps_limit);
    }

    // server netfps default is 1/0.085 ~= 12
    static constexpr unsigned min_server_netfps = 12u;
    static constexpr unsigned max_server_netfps = 300u;
    unsigned server_netfps = 30u;
    void set_server_netfps(unsigned netfps_value)
    {
        server_netfps = std::clamp(netfps_value, min_server_netfps, max_server_netfps);
    }

    int desired_handicap = 0;
    void set_desired_handicap(int value)
    {
        desired_handicap = std::clamp(value, 0, 99);
    }

    float control_point_outline_height_scale = 5.0f;
    void set_control_point_outline_height_scale(float scale)
    {
        control_point_outline_height_scale = std::clamp(scale, 0.0f, 1000.0f);
    }

    int control_point_outline_segments = 32;
    void set_control_point_outline_segments(int segments)
    {
        control_point_outline_segments = std::clamp(segments, 3, 256);
    }

    int control_point_column_segments = 8;
    void set_control_point_column_segments(int segments)
    {
        control_point_column_segments = std::clamp(segments, 3, 256);
    }

    float control_point_column_height_scale = 1.0f;
    void set_control_point_column_height_scale(float scale)
    {
        control_point_column_height_scale = std::clamp(scale, 0.0f, 1000.0f);
    }

    bool rendering_enabled = true;
    bool sound_enabled = true;
    bool background_mouse = false;
};

struct FpsCounterState
{
    int last_window_ms = -1;
    float display_fps = 0.0f;
    int accumulated_frames = 0;
    float accumulated_time = 0.0f;
    rf::TimestampRealtime window_timer;
};

extern AlpineGameSettings g_alpine_game_config;
std::optional<uint32_t> parse_hex_color_string(const std::string& value);
std::string format_hex_color_string(uint32_t color);
std::tuple<int, int, int, int> extract_color_components(uint32_t color);
std::tuple<float, float, float, float> extract_normalized_color_components(uint32_t color);
void initialize_alpine_core_config();
void alpine_core_config_save();
void set_big_hud(bool is_big);
void update_scope_sensitivity();
void update_scanner_sensitivity();
void recalc_mesh_static_lighting();
void apply_show_enemy_bullets();
void apply_console_history_setting();
void build_time_left_string_format();
void gr_update_texture_filtering();
void set_play_sound_events_volume_scale();
void apply_entity_sim_distance();
void gr_d3d_update_vsync();
bool is_d3d11();
