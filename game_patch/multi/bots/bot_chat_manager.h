#pragma once

#include "../../rf/os/timestamp.h"
#include <string_view>

namespace rf
{
    struct Player;
}

struct BotContext;

enum class BotChatType
{
    status = 0,
    taunt = 1,
    team_info = 2,
    response = 3,
    misc = 4,
};

struct BotChatRuntimeState
{
    bool sit_out_for_missing_waypoints = false;
    bool missing_waypoint_status_sent = false;
    bool join_hello_evaluated = false;
    bool pending_red_faction_response = false;
    int pending_taunts = 0;
    int observed_local_kill_count = -1;
    BotChatType last_chat_type = BotChatType::status;
    rf::Timestamp chat_cooldown_timer{};
    rf::Timestamp taunt_cooldown_timer{};
    rf::Timestamp red_faction_response_delay_timer{};
    rf::Timestamp red_faction_response_cooldown_timer{};
};

void bot_chat_manager_reset(BotContext& ctx);
void bot_chat_manager_on_limbo_enter(BotContext& ctx, const rf::Player& local_player);
void bot_chat_manager_on_remote_chat_message(BotContext& ctx, const rf::Player& sender, std::string_view message);
void bot_chat_manager_update_frame(BotContext& ctx, const rf::Player& local_player);
