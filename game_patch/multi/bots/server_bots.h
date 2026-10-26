#pragma once

#include <string>
#include <string_view>

namespace rf
{
    struct Player;
    struct Entity;
}

struct ServerBot;

// The roster: the server bots that should exist. Loading the server config resizes it.
bool server_bots_configured();
void server_bots_reset_roster(int count);
// profile_index / team -1 = automatic. False when the roster is full.
bool server_bots_add_to_roster(int profile_index, int team);
void server_bots_remove_from_roster(const rf::Player* player);
void server_bots_clear_roster();
std::string server_bots_build_list_output();

ServerBot* server_bot_from_player(const rf::Player* player);
ServerBot* server_bot_from_entity(const rf::Entity* entity);
// A server bot's reload also lasts as long as it would on a player's client.
bool server_bot_is_reloading(const rf::Player& pp, rf::Entity& ep);
void server_bot_reload(rf::Player& pp, int weapon_type);
// Server bots that can still leave to make room for a joining human.
int server_bots_num_evictable();
void server_bot_say(rf::Player& sender, std::string_view message, bool is_team_msg);
void server_bots_on_chat_message(const rf::Player& sender, std::string_view message, bool is_team_msg);
void server_bots_on_limbo_enter();
void server_bots_on_player_destroyed(const rf::Player* player);
void server_bots_on_level_init();
void server_bots_on_multi_stop();
void server_bots_do_frame();
void server_bots_apply_patches();
