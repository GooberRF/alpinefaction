#include <patch_common/CallHook.h>
#include <patch_common/FunHook.h>
#include <patch_common/AsmWriter.h>
#include <common/version/version.h>
#include <format>
#include "console.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../rf/multi.h"
#include "../rf/gameseq.h"
#include "../rf/hud.h"
#include "../rf/os/frametime.h"
#include "../multi/multi.h"
#include "../main/main.h"
#include "../misc/alpine_settings.h"
#include "../hud/hud.h"
#include <xlog/xlog.h>

static float g_frametime_history[1024];
static int g_frametime_history_index = 0;
static bool g_show_frametime_graph = false;

static void frametime_render_graph()
{
    if (g_show_frametime_graph) {
        g_frametime_history[g_frametime_history_index] = rf::frametime;
        g_frametime_history_index = (g_frametime_history_index + 1) % std::size(g_frametime_history);
        float max_frametime = 0.0f;
        for (auto frametime : g_frametime_history) {
            max_frametime = std::max(max_frametime, frametime);
        }

        rf::gr::set_color(255, 255, 255, 128);
        int scr_w = rf::gr::screen_width();
        int scr_h = rf::gr::screen_height();
        for (unsigned i = 0; i < std::size(g_frametime_history); ++i) {
            int slot_index = (g_frametime_history_index + 1 + i) % std::size(g_frametime_history);
            int x = scr_w - i - 1;
            int h = static_cast<int>(g_frametime_history[slot_index] / max_frametime * 100.0f);
            rf::gr::rect(x, scr_h - h, 1, h);
        }
    }
}

static void frametime_render_fps_counter()
{
    if (g_alpine_game_config.fps_counter && !rf::hud_disabled) {
        auto text = std::format("FPS: {:.1f}", rf::current_fps);
        rf::gr::set_color(0, 255, 0, 255);
        
        // Use offset if specified, otherwise use default positioning
        int x, y;
        if (g_alpine_game_config.fps_hud_offset.x != -1) {
            x = g_alpine_game_config.fps_hud_offset.x;
        } else {
            x = rf::gr::screen_width() - (g_alpine_game_config.big_hud ? 165 : 90);
        }
        
        if (g_alpine_game_config.fps_hud_offset.y != -1) {
            y = g_alpine_game_config.fps_hud_offset.y;
        } else {
            y = 10;
            if (rf::gameseq_in_gameplay()) {
                y = g_alpine_game_config.big_hud ? 110 : 60;
                if (hud_weapons_is_double_ammo()) {
                    y += g_alpine_game_config.big_hud ? 80 : 40;
                }
            }
        }

        int font_id = hud_get_fps_font();
        rf::gr::string(x, y, text.c_str(), font_id);
    }

    if (g_alpine_game_config.ping_display && !rf::hud_disabled && rf::is_multi && !rf::is_server) {
        auto text = std::format("Ping: {}", rf::local_player->net_data->ping);
        rf::gr::set_color(0, 255, 0, 255);
        
        // Use offset if specified, otherwise use default positioning
        int x, y;
        if (g_alpine_game_config.ping_hud_offset.x != -1) {
            x = g_alpine_game_config.ping_hud_offset.x;
        } else {
            x = rf::gr::screen_width() - (g_alpine_game_config.big_hud ? 165 : 90);
        }
        
        if (g_alpine_game_config.ping_hud_offset.y != -1) {
            y = g_alpine_game_config.ping_hud_offset.y;
        } else {
            y = g_alpine_game_config.big_hud ? 35 : 25;
            if (rf::gameseq_in_gameplay()) {
                y = g_alpine_game_config.big_hud ? 135 : 75;
                if (hud_weapons_is_double_ammo()) {
                    y += g_alpine_game_config.big_hud ? 105 : 55;
                }
            }
        }

        int font_id = hud_get_ping_font();
        rf::gr::string(x, y, text.c_str(), font_id);
    }
}

void frametime_render_ui()
{
    frametime_render_fps_counter();
    frametime_render_graph();
}

ConsoleCommand2 fps_counter_cmd{
    "cl_showfps",
    []() {
        g_alpine_game_config.fps_counter = !g_alpine_game_config.fps_counter;
        rf::console::print("FPS counter display is {}", g_alpine_game_config.fps_counter ? "enabled" : "disabled");
    },
    "Toggle FPS counter",
    "cl_showfps",
};

ConsoleCommand2 ping_display_cmd{
    "cl_showping",
    []() {
        g_alpine_game_config.ping_display = !g_alpine_game_config.ping_display;
        rf::console::print("Ping counter display is {}", g_alpine_game_config.ping_display ? "enabled" : "disabled");
    },
    "Toggle ping counter",
    "cl_showping",
};

CallHook<void(int)> frametime_calculate_sleep_hook{
    0x005095B4,
    [](int ms) {
        --ms;
        if (ms > 0) {
            frametime_calculate_sleep_hook.call_target(ms);
        }
    },
};

float get_maximum_fps()
{
    return 1.0f / rf::frametime_min;
}

void apply_maximum_fps()
{
    unsigned max_fps;

    if (rf::is_dedicated_server) {
        max_fps = g_alpine_game_config.server_max_fps;
    }
    else if (rf::is_multi) {
        const auto& server_info_opt = get_df_server_info();
        if (server_info_opt && server_info_opt->unlimited_fps) {
            max_fps = g_alpine_game_config.max_fps;
        }
        else {
            max_fps = std::clamp(g_alpine_game_config.max_fps, g_alpine_game_config.min_fps_limit, g_alpine_game_config.max_fps_limit_mp);
        }
    }
    else {
        max_fps = g_alpine_game_config.max_fps;
    }

    rf::frametime_min = 1.0f / static_cast<float>(max_fps);
    //xlog::warn("applying max fps {}", max_fps);
}

FunHook<void()> frametime_reset_hook{
    0x00509490,
    []() {
        frametime_reset_hook.call_target();

        // Set initial FPS limit
        apply_maximum_fps();
    },
};

ConsoleCommand2 max_fps_cmd{
    "maxfps",
    [](std::optional<int> limit_opt) {
        if (limit_opt) {
            if (rf::is_dedicated_server) {
                g_alpine_game_config.set_server_max_fps(limit_opt.value());
            }
            else {
                g_alpine_game_config.set_max_fps(limit_opt.value());
            }
            apply_maximum_fps();
        }
        else
            rf::console::print("Maximal FPS: {:.1f}", get_maximum_fps());
    },
    "Sets maximal FPS",
    "maxfps <limit>",
};

ConsoleCommand2 frametime_graph_cmd{
    "dbg_framegraph",
    []() {
        g_show_frametime_graph = !g_show_frametime_graph;
    },
};

void frametime_apply_patch()
{
    // Fix incorrect frame time calculation
    AsmWriter(0x00509595).nop(2);
    write_mem<u8>(0x00509532, asm_opcodes::jmp_rel_short);
    frametime_calculate_sleep_hook.install();

    // Set initial FPS limit
    frametime_reset_hook.install();

    // Commands
    max_fps_cmd.register_cmd();
    frametime_graph_cmd.register_cmd();
    fps_counter_cmd.register_cmd();
    ping_display_cmd.register_cmd();
}
