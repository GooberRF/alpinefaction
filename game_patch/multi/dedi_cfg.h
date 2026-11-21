#pragma once

#include <vector>
#include <toml++/toml.hpp>
#include "server_internal.h"

// Rules of one scope of a config held in memory: its game type's defaults, `base_tbl` ([base]), then `level_tbl`
// (a [[levels]] entry) when given.
AlpineServerConfigRules build_config_scope_rules(const toml::table& base_tbl, const toml::table* level_tbl);

// Puts a config held in memory in force for a listen server. No file is read: `bot_profiles` stand for the
// profile files a bot_profiles key would name.
void install_listen_server_config(const toml::table& root, const std::vector<toml::table>& bot_profiles);
void clear_listen_server_config();
// A listen server runs a Host Game setup.
bool listen_server_config_active();
void listen_server_allow_visual_options();
