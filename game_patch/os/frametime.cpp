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
#include "../rf/entity.h"
#include "../rf/os/frametime.h"
#include "../multi/multi.h"
#include "../main/main.h"
#include "../misc/alpine_settings.h"
#include "../hud/hud.h"
#include <xlog/xlog.h>

static float g_frametime_history[1024];
static int g_frametime_history_index = 0;
static bool g_show_frametime_graph = false;
static FpsCounterState g_fps_counter_state;

static int frametime_hud_counter_base_y()
{
    int y = 10;
    if (rf::gameseq_in_gameplay()) {
        y = g_alpine_game_config.big_hud ? 110 : 60;
        if (hud_weapons_is_double_ammo()) {
            y += g_alpine_game_config.big_hud ? 80 : 40;
        }
    }
    return y;
}

static int frametime_hud_counter_line_gap()
{
    return g_alpine_game_config.big_hud ? 25 : 15;
}

static void frametime_render_speed_meter()
{
    if (!g_alpine_game_config.speed_display || rf::hud_disabled || !rf::gameseq_in_gameplay()) {
        return;
    }

    rf::Player* const player = rf::local_player;
    if (!player) {
        return;
    }

    rf::Entity* const entity = rf::entity_from_handle(player->entity_handle);
    if (!entity) {
        return;
    }

    const rf::Vector3& velocity = entity->p_data.vel;
    float horizontal_speed = std::sqrt(velocity.x * velocity.x + velocity.z * velocity.z);
    float speed_mps = std::clamp(horizontal_speed, 0.0f, 9999.9f);
    std::string text = std::format("{:.2f}", speed_mps);

    int y = frametime_hud_counter_base_y();
    if (g_alpine_game_config.fps_counter) {
        y += frametime_hud_counter_line_gap();
    }

    rf::gr::set_color(0, 255, 0, 255);
    const int value_anchor = rf::gr::screen_width() - 20;
    const int label_offset = g_alpine_game_config.big_hud ? 125 : 65;
    int font_id = hud_get_default_font();
    const std::string_view speed_label = "Speed:";
    rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, value_anchor - label_offset, y, speed_label.data(), font_id);
    rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, value_anchor, y, text.c_str(), font_id);
}

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
        const int fps_window_ms = g_alpine_game_config.fps_counter_average_ms;
        std::string text;

        if (fps_window_ms <= 0) {
            //text = std::format("FPS: {:.1f}", rf::current_fps);
            g_fps_counter_state.display_fps = rf::current_fps;
            g_fps_counter_state.accumulated_frames = 0;
            g_fps_counter_state.accumulated_time = 0.0f;
            g_fps_counter_state.last_window_ms = fps_window_ms;
            g_fps_counter_state.window_timer.invalidate();
        }
        else {
            if (g_fps_counter_state.last_window_ms != fps_window_ms) {
                g_fps_counter_state.display_fps = 0.0f;
                g_fps_counter_state.accumulated_frames = 0;
                g_fps_counter_state.accumulated_time = 0.0f;
                g_fps_counter_state.last_window_ms = fps_window_ms;
                g_fps_counter_state.window_timer.invalidate();
            }

            if (g_fps_counter_state.display_fps == 0.0f) {
                g_fps_counter_state.display_fps = rf::current_fps;
            }
            g_fps_counter_state.accumulated_frames++;
            g_fps_counter_state.accumulated_time += rf::frametime;

            if (!g_fps_counter_state.window_timer.valid()) {
                g_fps_counter_state.window_timer.set(fps_window_ms);
            }

            if (g_fps_counter_state.window_timer.elapsed()) {
                float averaged_fps = rf::current_fps;
                if (g_fps_counter_state.accumulated_time > 0.0f) {
                    averaged_fps = static_cast<float>(g_fps_counter_state.accumulated_frames) /
                        g_fps_counter_state.accumulated_time;
                }

                g_fps_counter_state.display_fps = averaged_fps;
                g_fps_counter_state.accumulated_frames = 0;
                g_fps_counter_state.accumulated_time = 0.0f;
                g_fps_counter_state.window_timer.set(fps_window_ms);
            }
        }

        float clamped_fps = std::clamp(g_fps_counter_state.display_fps, 0.0f, 99999.9f);
        text = std::format("{:7.1f}", clamped_fps);

        rf::gr::set_color(0, 255, 0, 255);
        const int value_anchor = rf::gr::screen_width() - 20;
        const int label_offset = g_alpine_game_config.big_hud ? 125 : 65;
        int y = frametime_hud_counter_base_y();

        int font_id = hud_get_default_font();
        const std::string_view fps_label = "FPS:";
        rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, value_anchor - label_offset, y, fps_label.data(), font_id);
        rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, value_anchor, y, text.c_str(), font_id);
    }

    if (g_alpine_game_config.ping_display && !rf::hud_disabled && rf::is_multi && !rf::is_server) {
        int clamped_ping = std::clamp(rf::local_player->net_data->ping, 0, 9999);
        auto text = std::format("{}", clamped_ping);
        rf::gr::set_color(0, 255, 0, 255);
        const int value_anchor = rf::gr::screen_width() - 20;
        const int label_offset = g_alpine_game_config.big_hud ? 125 : 65;
        const int gap = 6;
        int y = frametime_hud_counter_base_y();
        int offset_lines = 0;
        if (g_alpine_game_config.fps_counter) {
            offset_lines++;
        }
        offset_lines++;
        y += frametime_hud_counter_line_gap() * offset_lines;

        int font_id = hud_get_default_font();
        const std::string_view ping_label = "Ping:";
        rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, value_anchor - label_offset, y, ping_label.data(), font_id);
        rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, value_anchor, y, text.c_str(), font_id);
    }
}

void frametime_render_ui()
{
    frametime_render_fps_counter();
    frametime_render_speed_meter();
    frametime_render_graph();
}

ConsoleCommand2 fps_counter_cmd{
    "ui_show_fps",
    []() {
        g_alpine_game_config.fps_counter = !g_alpine_game_config.fps_counter;
        rf::console::print("FPS counter display is {}", g_alpine_game_config.fps_counter ? "enabled" : "disabled");
    },
    "Toggle FPS counter",
    "ui_show_fps",
};

ConsoleCommand2 fps_counter_average_cmd{
    "ui_fpsavg",
    [](std::optional<int> window_ms) {
        if (window_ms) {
            g_alpine_game_config.set_fps_counter_average_ms(window_ms.value());
        }

        rf::console::print(
            "FPS counter averaging window is {} ms (0 = draw every frame)", g_alpine_game_config.fps_counter_average_ms);
    },
    "Set the FPS counter averaging window in milliseconds (0 to draw every frame)",
    "ui_fpsavg [milliseconds]",
};

ConsoleCommand2 ping_display_cmd{
    "ui_show_ping",
    []() {
        g_alpine_game_config.ping_display = !g_alpine_game_config.ping_display;
        rf::console::print("Ping counter display is {}", g_alpine_game_config.ping_display ? "enabled" : "disabled");
    },
    "Toggle ping counter",
    "ui_show_ping",
};

ConsoleCommand2 speed_display_cmd{
    "ui_show_speed",
    []() {
        g_alpine_game_config.speed_display = !g_alpine_game_config.speed_display;
        rf::console::print("Speed display is {}", g_alpine_game_config.speed_display ? "enabled" : "disabled");
    },
    "Toggle speed meter display",
    "ui_show_speed",
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
        const auto& server_info_opt = get_af_server_info();
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
    fps_counter_average_cmd.register_cmd();
    ping_display_cmd.register_cmd();
    speed_display_cmd.register_cmd();
}
