#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace rf
{
    struct Player;
    struct Entity;
}

struct ServerBot;

// The roster: the server bots that should exist. Loading the server config resizes its configured bots.
bool server_bots_configured();
void server_bots_reset_roster(int count);
// profile_index / team -1 = automatic. False when the roster is full.
bool server_bots_add_to_roster(int profile_index, int team);
void server_bots_remove_from_roster(const rf::Player* player);
void server_bots_clear_roster();
std::string server_bots_build_list_output();

// Join Bots mutator: matches the roster's Join Bots entries to the active rules. A match's bots don't spawn
// until it starts and leave once it is over.
void server_bots_sync_join_bots();
void server_bots_end_join_bots();
int server_bots_num_join_bots();
// Slots a queued, forming or live match's Join Bots hold before they have joined, so joining players can't take them.
int server_bots_num_reserved_slots();
bool server_bot_is_join_bot(const rf::Player* player);
bool server_bots_join_bot_may_spawn(rf::Player* player);
std::vector<rf::Player*> server_bots_join_bot_players();

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
