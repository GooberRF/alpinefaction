#pragma once

#include <string>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <common/config/CfgVar.h>

struct GameConfig
{
    // Path
    CfgVar<std::string> game_executable_path{""};

    // Display
    CfgVar<unsigned> res_width{1920, [](auto val) { return std::max(val, 128u); }};
    CfgVar<unsigned> res_height{1080, [](auto val) { return std::max(val, 96u); }};
    CfgVar<unsigned> res_bpp = 32;
    CfgVar<unsigned> res_backbuffer_format = 22U; // D3DFMT_X8R8G8B8
    CfgVar<unsigned> selected_video_card = 0;
    enum WndMode
    {
        FULLSCREEN,
        WINDOWED,
        STRETCHED,
    };

    CfgVar<WndMode> wnd_mode = FULLSCREEN;
    CfgVar<bool> vsync = false;
    CfgVar<unsigned> geometry_cache_size{32, [](auto val) { return std::clamp(val, 2u, 32u); }};

    enum class Renderer
    {
        // separate values for d3d8/d3d9?
        d3d8 = 0,
        d3d9 = 1,
        d3d11 = 2,
    };
    CfgVar<Renderer> renderer = Renderer::d3d11;

    // Graphics
    CfgVar<bool> anisotropic_filtering = true;
    CfgVar<uint32_t> msaa_level = 0;

    CfgVar<bool> high_scanner_res = true;
    CfgVar<bool> true_color_textures = true;

    // Audio
    CfgVar<bool> eax_sound = true;

    // Multiplayer
    static constexpr unsigned default_update_rate = 200000; // T1/LAN in stock launcher
    CfgVar<unsigned> update_rate = default_update_rate;

    CfgVar<unsigned> force_port{0, [](auto val) { return std::min<unsigned>(val, std::numeric_limits<uint16_t>::max()); }};

    // Interface
    CfgVar<int> language = -1;

    // Misc
    CfgVar<bool> fast_start = true;
    CfgVar<bool> allow_overwrite_game_files = false;
    CfgVar<bool> keep_launcher_open = true;
    CfgVar<bool> reduced_speed_in_background = false;

    // Internal
    CfgVar<std::string> alpine_faction_version{""};
    CfgVar<std::string> fflink_token{""};
    CfgVar<std::string> fflink_username{""};
    CfgVar<bool> suppress_first_launch_window = false;
    CfgVar<bool> suppress_ff_link_prompt = false;

    bool load();
    void save();
    bool detect_game_path();

private:
    template<typename T>
    bool visit_vars(T&& visitor, bool is_save);
};
