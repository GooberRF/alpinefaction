#pragma once

#include "../rf/os/timestamp.h"
#include "../rf/math/vector.h"
#include "../rf/gr/gr.h"
#include "../os/os.h"

struct WorldHUDAssets
{
    int flag_red_d;
    int flag_blue_d;
    int flag_red_a;
    int flag_blue_a;
    int flag_red_s;
    int flag_blue_s;
    int mp_respawn;
    int koth_neutral;
    int koth_neutral_atk;
    int koth_neutral_def;
    int koth_atk_red;
    int koth_atk_blue;
    int koth_def_red;
    int koth_def_blue;
    int koth_red;
    int koth_blue;
    int koth_neutral_c;
    int koth_red_c;
    int koth_blue_c;
    int koth_neutral_l;
    int koth_red_l;
    int koth_blue_l;
    int koth_fill_red;
    int koth_fill_blue;
    int koth_ring_fade;
};

struct KothHudTuning
{
    float fill_vs_ring_scale = 0.975f;
    float icon_base_scale = 1.0f;
};

struct WorldHUDView
{
    rf::Vector3 pos; // includes possible push due to fog level
    float dist_factor; // clamped to >= 1 / ref
};

struct WorldHUDRender
{
    static constexpr float base_scale = 1.0f;
    static constexpr float reference_distance = 17.5f;
    static constexpr float min_scale = 0.5f;
    static constexpr float max_scale = 2.0f;
    static constexpr float ctf_flag_offset = 1.75f;
    static constexpr float fog_dist_multi = 0.85f;
    static constexpr float fog_dist_min = 5.0f;
    static constexpr float fog_dist_max = 100.0f;
    static constexpr float koth_hill_offset = 0.0f;
};

enum class WorldHUDRenderMode : int
{
    no_overdraw,
    no_overdraw_glow,
    overdraw,
    overdraw_colorized
};

struct EphemeralWorldHUDSprite
{
    int bitmap = -1;
    rf::Vector3 pos;
    std::string label = "";
    int player_id = -1;
    WorldHUDRenderMode render_mode = WorldHUDRenderMode::overdraw;
    rf::Timestamp timestamp;
    int duration = 10000;
    bool float_away = false;
    float wind_phase_offset = 0.0f;
    rf::Color color{255, 255, 255, 255};
};

struct EphemeralWorldHUDString
{
    rf::Vector3 pos;
    uint8_t player_id;
    uint16_t damage;
    WorldHUDRenderMode render_mode = WorldHUDRenderMode::overdraw;
    HighResTimer timestamp;
    bool float_away = false;
    float wind_phase_offset = 0.0f;
    rf::Color color = {255, 255, 255, 255};
};

struct NameLabelTex
{
    int bm = -1; // handle
    int w_px = 0;
    int h_px = 0;
    std::string text;
    int font = 0;
};

void hud_world_do_frame();
void load_world_hud_assets();
void clear_koth_name_textures();
void populate_world_hud_sprite_events();
void add_location_ping_world_hud_sprite(rf::Vector3 pos, std::string player_name, int player_id);
void add_damage_notify_world_hud_string(rf::Vector3 pos, uint8_t damaged_player_id, uint16_t damage, bool died);
void do_render_world_hud_sprite(rf::Vector3 pos, float base_scale, int bitmap_handle, WorldHUDRenderMode render_mode,
                                bool stay_inside_fog, bool distance_scaling, bool only_draw_during_gameplay);
