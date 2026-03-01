#include <array>
#include <string>
#include <patch_common/FunHook.h>
#include "../rf/hud.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../rf/multi.h"
#include "../rf/os/string.h"
#include "../rf/os/timestamp.h"
#include "../misc/alpine_settings.h"
#include "hud_internal.h"
#include "hud.h"

static constexpr int killfeed_max_messages = 8;
static constexpr int killfeed_display_ms = 5000;
static constexpr int killfeed_fade_ms = 750;
static constexpr int killfeed_max_segments = 5;
// Vertical gap between the bottom of the armor HUD element and the first killfeed line
static constexpr int killfeed_top_margin = 50;
// Approximate height of the health/armor bitmap at 1x scale (health + armor stacked)
static constexpr int hud_status_bitmap_h = 100;

struct KillfeedColor
{
    int r, g, b;
};

static constexpr KillfeedColor killfeed_color_red    = {227, 48, 47};
static constexpr KillfeedColor killfeed_color_blue   = {117, 117, 254};
static constexpr KillfeedColor killfeed_color_white  = {255, 255, 255};
static constexpr KillfeedColor killfeed_color_green  = {52, 255, 57};

struct KillfeedSegment
{
    std::string text;
    KillfeedColor color = killfeed_color_green;
};

struct KillfeedMessage
{
    KillfeedSegment segments[killfeed_max_segments];
    int segment_count = 0;
    rf::Timestamp timestamp;
    bool active = false;
};

static std::array<KillfeedMessage, killfeed_max_messages> g_killfeed_messages;
static int g_killfeed_head = 0; // next slot to write

// Flag to suppress the multi_chat_print hook for kill messages (already handled by kill.cpp)
static bool g_killfeed_suppress_hook = false;

static KillfeedColor color_for_team(int team)
{
    // team 0 = red, team 1 = blue
    if (team == 0) return killfeed_color_red;
    if (team == 1) return killfeed_color_blue;
    return killfeed_color_green;
}

static KillfeedColor color_for_color_id(int color_id)
{
    switch (color_id) {
        case 0: return killfeed_color_red;    // red_white
        case 1: return killfeed_color_blue;   // blue_white
        case 2: return killfeed_color_red;    // red_red
        case 3: return killfeed_color_blue;   // blue_blue
        case 4: return killfeed_color_white;  // white_white (local player)
        case 6: return {255, 215, 0};         // gold_white
        default: return killfeed_color_green; // default green
    }
}

static KillfeedMessage& alloc_message()
{
    auto& msg = g_killfeed_messages[g_killfeed_head];
    msg.segment_count = 0;
    msg.active = true;
    msg.timestamp.set(killfeed_display_ms + killfeed_fade_ms);
    g_killfeed_head = (g_killfeed_head + 1) % killfeed_max_messages;
    return msg;
}

static void add_segment(KillfeedMessage& msg, const char* text, KillfeedColor color)
{
    if (msg.segment_count >= killfeed_max_segments) return;
    auto& seg = msg.segments[msg.segment_count++];
    seg.text = text;
    seg.color = color;
}

void killfeed_add_message(const char* text, int color_id)
{
    auto& msg = alloc_message();
    add_segment(msg, text, color_for_color_id(color_id));
}

void killfeed_add_kill(const char* killed_name, int killed_team,
                       const char* killer_name, int killer_team,
                       const char* verb, bool is_local_kill, bool is_team_mode)
{
    auto& msg = alloc_message();

    if (is_local_kill) {
        add_segment(msg, verb, killfeed_color_white);
    }
    else if (!killer_name) {
        KillfeedColor killed_color = is_team_mode ? color_for_team(killed_team) : killfeed_color_green;
        add_segment(msg, killed_name, killed_color);
        add_segment(msg, verb, killfeed_color_green);
    }
    else {
        KillfeedColor killed_color = is_team_mode ? color_for_team(killed_team) : killfeed_color_green;
        KillfeedColor killer_color = is_team_mode ? color_for_team(killer_team) : killfeed_color_green;
        add_segment(msg, killed_name, killed_color);
        add_segment(msg, verb, killfeed_color_green);
        add_segment(msg, killer_name, killer_color);
    }
}

void killfeed_set_suppress_hook(bool suppress)
{
    g_killfeed_suppress_hook = suppress;
}

void killfeed_clear()
{
    for (auto& msg : g_killfeed_messages) {
        msg.active = false;
        msg.segment_count = 0;
        msg.timestamp.invalidate();
    }
    g_killfeed_head = 0;
}

// Hook on multi_chat_print to intercept system/event messages (flag captures, joins, etc.)
FunHook<void(rf::String::Pod, rf::ChatMsgColor, rf::String::Pod)> multi_chat_print_hook{
    0x004785A0,
    [](rf::String::Pod text_pod, rf::ChatMsgColor color, rf::String::Pod prefix_pod) {
        if (!g_alpine_game_config.killfeed_enabled || g_killfeed_suppress_hook) {
            multi_chat_print_hook.call_target(text_pod, color, prefix_pod);
            return;
        }

        // Messages with a non-empty prefix are player chat or server messages - keep in chat
        rf::String prefix{prefix_pod};
        if (!prefix.empty()) {
            multi_chat_print_hook.call_target(text_pod, color, prefix_pod);
            return;
        }

        // System/event message (flag events, join/leave, etc.) - route to killfeed
        rf::String text{text_pod};
        killfeed_add_message(text.c_str(), static_cast<int>(color));
    },
};

void multi_hud_killfeed_apply_patches()
{
    multi_chat_print_hook.install();
}

void multi_hud_render_killfeed()
{
    if (!g_alpine_game_config.killfeed_enabled) {
        return;
    }

    const int font_id = hud_get_default_font();
    const int font_h = rf::gr::get_font_height(font_id);
    const int line_spacing = font_h + 2;

    // Position below the armor (envirosuit) HUD indicator
    const bool big = g_alpine_game_config.big_hud;
    const float scale = big ? 1.875f : 1.0f;
    auto enviro_pt = hud_scale_coords(rf::hud_coords[rf::hud_envirosuit], scale);
    int top_y = enviro_pt.y + static_cast<int>(hud_status_bitmap_h * scale) + killfeed_top_margin;
    const int base_x = enviro_pt.x;

    // Collect active messages in order (oldest to newest)
    int count = 0;
    int indices[killfeed_max_messages];
    for (int i = 0; i < killfeed_max_messages; ++i) {
        int idx = (g_killfeed_head + i) % killfeed_max_messages;
        auto& msg = g_killfeed_messages[idx];
        if (!msg.active) {
            continue;
        }
        if (!msg.timestamp.valid() || msg.timestamp.elapsed()) {
            msg.active = false;
            continue;
        }
        indices[count++] = idx;
    }

    if (count == 0) {
        return;
    }

    // Render top-to-bottom: newest at top, older ones below
    for (int i = 0; i < count; ++i) {
        int draw_idx = count - 1 - i;
        auto& msg = g_killfeed_messages[indices[draw_idx]];

        int y = top_y + i * line_spacing;

        int alpha = 255;
        int time_left = msg.timestamp.time_until();
        if (time_left < killfeed_fade_ms) {
            alpha = static_cast<int>(255.0f * time_left / killfeed_fade_ms);
        }
        if (alpha <= 0) {
            continue;
        }

        // Draw each segment with its own color
        int cur_x = base_x;
        for (int s = 0; s < msg.segment_count; ++s) {
            auto& seg = msg.segments[s];
            if (seg.text.empty()) continue;

            // Shadow
            rf::gr::set_color(0, 0, 0, alpha);
            rf::gr::string(cur_x + 1, y + 1, seg.text.c_str(), font_id);

            // Colored text
            rf::gr::set_color(seg.color.r, seg.color.g, seg.color.b, alpha);
            rf::gr::string(cur_x, y, seg.text.c_str(), font_id);

            auto [tw, th] = rf::gr::get_string_size(seg.text, font_id);
            cur_x += tw;
        }
    }
}
