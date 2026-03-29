#pragma once

#include <string_view>

namespace rf
{
    struct Player;
}

enum class BotChatType
{
    status = 0,
    taunt = 1,
    team_info = 2,
    response = 3,
    misc = 4,
};

void bot_chat_manager_reset();
void bot_chat_manager_on_limbo_enter(const rf::Player& local_player);
void bot_chat_manager_on_remote_chat_message(const rf::Player& sender, std::string_view message);
void bot_chat_manager_update_frame(const rf::Player& local_player);
