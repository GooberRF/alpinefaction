#include <array>
#include <string>
#include <patch_common/FunHook.h>
#include <patch_common/CallHook.h>
#include "../rf/hud.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../rf/multi.h"
#include "../rf/os/string.h"
#include "../rf/os/timestamp.h"
#include "../misc/alpine_settings.h"
#include "hud_internal.h"
#include "hud.h"

static constexpr int KILLFEED_MAX_MESSAGES = 8;
static constexpr int KILLFEED_DISPLAY_MS = 5000;
static constexpr int KILLFEED_FADE_MS = 750;
static constexpr int KILLFEED_MAX_SEGMENTS = 5;
// Vertical gap between the bottom of the armor HUD element and the first killfeed line
static constexpr int KILLFEED_TOP_MARGIN = 50;
// Approximate height of the health/armor bitmap at 1x scale (health + armor stacked)
static constexpr int HUD_STATUS_BITMAP_H = 100;

struct KillfeedColor
{
    int r, g, b;
};

static constexpr KillfeedColor KILLFEED_COLOR_RED    = {227, 48, 47};
static constexpr KillfeedColor KILLFEED_COLOR_BLUE   = {117, 117, 254};
static constexpr KillfeedColor KILLFEED_COLOR_WHITE  = {255, 255, 255};
static constexpr KillfeedColor KILLFEED_COLOR_GREEN  = {52, 255, 57};

struct KillfeedSegment
{
    std::string text;
    KillfeedColor color = KILLFEED_COLOR_GREEN;
};

struct KillfeedMessage
{
    KillfeedSegment segments[KILLFEED_MAX_SEGMENTS];
    int segment_count = 0;
    rf::Timestamp timestamp;
    bool active = false;
};

static std::array<KillfeedMessage, KILLFEED_MAX_MESSAGES> g_killfeed_messages;
static int g_killfeed_head = 0; // next slot to write

// When set, the next multi_chat_print call will be routed to the killfeed instead of the chat
static bool g_killfeed_route_next = false;

static KillfeedColor color_for_color_id(rf::ChatMsgColor color_id)
{
    switch (color_id) {
        case rf::ChatMsgColor::red_white:    return KILLFEED_COLOR_RED;
        case rf::ChatMsgColor::blue_white:   return KILLFEED_COLOR_BLUE;
        case rf::ChatMsgColor::red_red:      return KILLFEED_COLOR_RED;
        case rf::ChatMsgColor::blue_blue:    return KILLFEED_COLOR_BLUE;
        case rf::ChatMsgColor::white_white:  return KILLFEED_COLOR_WHITE;
        case rf::ChatMsgColor::gold_white:   return {255, 215, 0};
        default: return KILLFEED_COLOR_GREEN;
    }
}

static KillfeedColor color_for_team(int team)
{
    // team 0 = red, team 1 = blue
    if (team == 0) return KILLFEED_COLOR_RED;
    if (team == 1) return KILLFEED_COLOR_BLUE;
    return KILLFEED_COLOR_GREEN;
}

static KillfeedMessage& alloc_message()
{
    auto& msg = g_killfeed_messages[g_killfeed_head];
    msg.segment_count = 0;
    msg.active = true;
    msg.timestamp.set(KILLFEED_DISPLAY_MS + KILLFEED_FADE_MS);
    g_killfeed_head = (g_killfeed_head + 1) % KILLFEED_MAX_MESSAGES;
    return msg;
}

static void add_segment(KillfeedMessage& msg, const char* text, KillfeedColor color)
{
    if (msg.segment_count >= KILLFEED_MAX_SEGMENTS) return;
    auto& seg = msg.segments[msg.segment_count++];
    seg.text = text;
    seg.color = color;
}

static void killfeed_add_message(const char* text, rf::ChatMsgColor color_id)
{
    auto& msg = alloc_message();
    add_segment(msg, text, color_for_color_id(color_id));
}

void killfeed_route_next_message()
{
    g_killfeed_route_next = true;
}

void killfeed_add_kill(const char* killed_name, int killed_team,
                       const char* killer_name, int killer_team,
                       const char* verb, bool is_local_kill, bool is_team_mode)
{
    auto& msg = alloc_message();

    if (is_local_kill) {
        add_segment(msg, verb, KILLFEED_COLOR_WHITE);
    }
    else if (!killer_name) {
        KillfeedColor killed_color = is_team_mode ? color_for_team(killed_team) : KILLFEED_COLOR_GREEN;
        add_segment(msg, killed_name, killed_color);
        add_segment(msg, verb, KILLFEED_COLOR_GREEN);
    }
    else {
        KillfeedColor killed_color = is_team_mode ? color_for_team(killed_team) : KILLFEED_COLOR_GREEN;
        KillfeedColor killer_color = is_team_mode ? color_for_team(killer_team) : KILLFEED_COLOR_GREEN;
        add_segment(msg, killed_name, killed_color);
        add_segment(msg, verb, KILLFEED_COLOR_GREEN);
        add_segment(msg, killer_name, killer_color);
    }
}

void killfeed_clear()
{
    for (auto& msg : g_killfeed_messages) {
        msg.active = false;
        msg.segment_count = 0;
        msg.timestamp.invalidate();
    }
    g_killfeed_head = 0;
    g_killfeed_route_next = false;
}

// Hook on multi_chat_print to intercept game event messages that have opted in via killfeed_route_next_message
FunHook<void(rf::String::Pod, rf::ChatMsgColor, rf::String::Pod)> multi_chat_print_hook{
    0x004785A0,
    [](rf::String::Pod text_pod, rf::ChatMsgColor color, rf::String::Pod prefix_pod) {
        bool should_route = g_killfeed_route_next;
        g_killfeed_route_next = false;

        if (should_route && g_alpine_game_config.killfeed_enabled) {
            rf::String text{text_pod};
            killfeed_add_message(text.c_str(), color);
            return;
        }

        multi_chat_print_hook.call_target(text_pod, color, prefix_pod);
    },
};

// Helper for CTF CallHooks: set the routing flag then call multi_chat_print
static void ctf_chat_print_with_killfeed_routing(
    CallHook<void(rf::String::Pod, rf::ChatMsgColor, rf::String::Pod)>& hook,
    rf::String::Pod text, rf::ChatMsgColor color, rf::String::Pod prefix)
{
    killfeed_route_next_message();
    hook.call_target(text, color, prefix);
}

// Hook multi_chat_print calls inside stock CTF flag packet handlers
CallHook<void(rf::String::Pod, rf::ChatMsgColor, rf::String::Pod)> ctf_flag_pickup_chat_hook1{
    0x004741A4,
    [](rf::String::Pod text, rf::ChatMsgColor color, rf::String::Pod prefix) {
        ctf_chat_print_with_killfeed_routing(ctf_flag_pickup_chat_hook1, text, color, prefix);
    },
};

CallHook<void(rf::String::Pod, rf::ChatMsgColor, rf::String::Pod)> ctf_flag_pickup_chat_hook2{
    0x00474246,
    [](rf::String::Pod text, rf::ChatMsgColor color, rf::String::Pod prefix) {
        ctf_chat_print_with_killfeed_routing(ctf_flag_pickup_chat_hook2, text, color, prefix);
    },
};

CallHook<void(rf::String::Pod, rf::ChatMsgColor, rf::String::Pod)> ctf_flag_capture_chat_hook{
    0x00473E5E,
    [](rf::String::Pod text, rf::ChatMsgColor color, rf::String::Pod prefix) {
        ctf_chat_print_with_killfeed_routing(ctf_flag_capture_chat_hook, text, color, prefix);
    },
};

CallHook<void(rf::String::Pod, rf::ChatMsgColor, rf::String::Pod)> ctf_flag_return_chat_hook{
    0x004734C4,
    [](rf::String::Pod text, rf::ChatMsgColor color, rf::String::Pod prefix) {
        ctf_chat_print_with_killfeed_routing(ctf_flag_return_chat_hook, text, color, prefix);
    },
};

void multi_hud_killfeed_apply_patches()
{
    multi_chat_print_hook.install();
    ctf_flag_pickup_chat_hook1.install();
    ctf_flag_pickup_chat_hook2.install();
    ctf_flag_capture_chat_hook.install();
    ctf_flag_return_chat_hook.install();
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
    int top_y = enviro_pt.y + static_cast<int>(HUD_STATUS_BITMAP_H * scale) + KILLFEED_TOP_MARGIN;
    const int base_x = enviro_pt.x;

    // Collect active messages in order (oldest to newest)
    int count = 0;
    int indices[KILLFEED_MAX_MESSAGES];
    for (int i = 0; i < KILLFEED_MAX_MESSAGES; ++i) {
        int idx = (g_killfeed_head + i) % KILLFEED_MAX_MESSAGES;
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
        if (time_left < KILLFEED_FADE_MS) {
            alpha = static_cast<int>(255.0f * time_left / KILLFEED_FADE_MS);
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
