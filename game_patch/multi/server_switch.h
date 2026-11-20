#pragma once

#include <optional>
#include <string>
#include "../rf/multi.h"

// Joining a server from the menus, the in-game menu or the console, leaving the game in progress if there is one.

// Stock's refusal (993 / 994) when the server's mod is not the one running; nullopt when they match.
std::optional<std::string> server_join_mod_mismatch(const std::string& server_mod);
// A game is left: a listen server host always confirms, everyone else unless the setting is off.
bool server_join_needs_confirmation();
struct ServerJoinConfirmation
{
    std::string title;
    std::string text;
};
ServerJoinConfirmation server_join_confirmation(const std::string& server_name);
// Joins at once from the server list's state, else leaves the game (if any) and gets there through the menus.
void server_join(const rf::NetAddr& addr, const std::string& password);

void server_switch_apply_patch();
