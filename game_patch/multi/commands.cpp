#include "../os/console.h"
#include "server_internal.h"
#include "../rf/multi.h"
#include "../rf/gameseq.h"
#include "../rf/level.h"
#include "../rf/player/player.h"
#include "../rf/crt.h"
#include "../rf/os/string.h"
#include "server.h"
#include "multi.h"
#include <common/utils/string-utils.h>
#include <patch_common/AsmWriter.h>
#include <patch_common/CallHook.h>
#include <algorithm>
#include <cstdint>
#include <vector>
#include <format>
#include <optional>

std::vector<int> g_players_to_kick;

void extend_round_time(int minutes)
{
    rf::level.time -= minutes * 60.0f;
}

void restart_current_level()
{
    std::optional<ManualRulesOverride> manual_rules_override;
    if (g_manual_rules_override)
        manual_rules_override = *g_manual_rules_override;

    multi_change_level_alpine(rf::level.filename.c_str());

    if (manual_rules_override)
        set_manual_rules_override(std::move(*manual_rules_override));
}

// Restart, but running the level's CONFIGURED rules instead of whatever the
// session's votes put in front of them.
void restart_current_level_configured()
{
    clear_manual_rules_override();

    const std::string filename = rf::level.filename.c_str();

    if (g_dedicated_launched_from_ads) {
        const auto& levels = g_alpine_server_config.levels;
        const int idx = rf::netgame.current_level_index;
        const AlpineServerConfigRules* configured = &g_alpine_server_config.base_rules;
        if (idx >= 0 && idx < static_cast<int>(levels.size())
            && string_iequals(levels[idx].level_filename, filename)) {
            configured = &levels[idx].rule_overrides;
        }
        else {
            for (const auto& entry : levels) {
                if (string_iequals(entry.level_filename, filename)) {
                    configured = &entry.rule_overrides;
                    break;
                }
            }
        }
        // Explicit so a game type the session voted in cannot survive as the
        // still-queued upcoming type.
        set_upcoming_game_type(configured->game_type, UpcomingGameTypeSelection::ExplicitRequest);
    }

    multi_change_level_alpine(filename.c_str());
}

void load_next_level()
{
    clear_manual_rules_override();
    multi_change_level_alpine(nullptr);
}

void load_prev_level()
{
    clear_manual_rules_override();
    rf::netgame.current_level_index--;
    if (rf::netgame.current_level_index < 0) {
        rf::netgame.current_level_index = rf::netgame.levels.size() - 1;
    }
    if (g_prev_level.empty()) {
        // this is the first level running - use previous level from rotation
        multi_change_level_alpine(rf::netgame.levels[rf::netgame.current_level_index].c_str());
    }
    else {
        multi_change_level_alpine(g_prev_level.c_str());
    }
}

void load_rand_level()
{
    clear_manual_rules_override();
    multi_change_level_alpine(get_rand_level_filename());
}

bool validate_is_server()
{
    if (!rf::is_server) {
        rf::console::output("Command can be only executed on server", nullptr);
        return false;
    }
    return true;
}

bool validate_not_limbo()
{
    if (rf::gameseq_get_state() != rf::GS_GAMEPLAY) {
        rf::console::output("Command can not be used between rounds", nullptr);
        return false;
    }
    return true;
}

ConsoleCommand2 map_ext_cmd{
    "map_ext",
    [](std::optional<int> minutes_opt) {
        if (validate_is_server() && validate_not_limbo()) {
            int minutes = minutes_opt.value_or(5);
            extend_round_time(minutes);
            std::string msg = std::format("Round extended by {} minutes", minutes);
            rf::multi_chat_say(msg.c_str(), false);
        }
    },
    "Extend round time",
    "map_ext [minutes]",
};

ConsoleCommand2 map_rest_cmd{
    "map_rest",
    []() {
        if (validate_is_server() && validate_not_limbo()) {
            rf::multi_chat_say("Restarting current level", false);
            restart_current_level();
        }
    },
    "Restart current level",
};

ConsoleCommand2 map_next_cmd{
    "map_next",
    []() {
        if (validate_is_server() && validate_not_limbo()) {
            rf::multi_chat_say("Loading next level", false);
            load_next_level();
        }
    },
    "Load next level",
};

ConsoleCommand2 map_rand_cmd{
    "map_rand",
    []() {
        if (validate_is_server() && validate_not_limbo()) {
            rf::multi_chat_say("Loading random level from rotation", false);
            load_rand_level();
        }
    },
    "Load random level from rotation",
};

ConsoleCommand2 map_prev_cmd{
    "map_prev",
    []() {
        if (validate_is_server() && validate_not_limbo()) {
            rf::multi_chat_say("Loading previous level", false);
            load_prev_level();
        }
    },
    "Load previous level",
};

void kick_player_delayed(const rf::Player* const player) {
    if (!player || !player->net_data) {
        return;
    }
    const int player_id = player->net_data->player_id;
    if (std::find(g_players_to_kick.begin(), g_players_to_kick.end(), player_id) != g_players_to_kick.end()) {
        return;
    }
    rf::console::print("{}{}", player->name, rf::strings::was_kicked);
    g_players_to_kick.push_back(player_id);
}

CallHook<void(const rf::Player*)> multi_kick_player_hook{0x0047B9BD, kick_player_delayed};

void process_delayed_kicks()
{
    // Process kicks outside of packet processing loop to avoid crash when a player is suddenly destroyed.
    // The engine's receive loop caches player_list->next before dispatching each packet, so destroying
    // a player from inside a packet handler makes the loop walk a freed rf::Player on the next iteration.
    while (!g_players_to_kick.empty()) {
        std::vector<int> batch;
        batch.swap(g_players_to_kick);
        for (int player_id : batch) {
            rf::Player* player = rf::multi_find_player_by_id(static_cast<uint8_t>(player_id));
            if (player) {
                rf::multi_kick_player(player);
            }
        }
    }
}

void ban_cmd_handler_hook()
{
    if (rf::is_multi && rf::is_server) {
        if (rf::console::run) {
            rf::console::get_arg(rf::console::ARG_STR, true);
            rf::Player* player = find_best_matching_player(rf::console::str_arg);

            if (player) {
                if (player != rf::local_player) {
                    rf::console::printf(rf::strings::banning_player, player->name.c_str());
                    rf::multi_ban_ip(player->net_data->addr);
                    kick_player_delayed(player);
                }
                else
                    rf::console::print("You cannot ban yourself!");
            }
        }

        if (rf::console::help) {
            rf::console::output(rf::strings::usage, nullptr);
            rf::console::print("     ban <{}>", rf::strings::player_name);
        }
    }
}

void kick_cmd_handler_hook()
{
    if (rf::is_multi && rf::is_server) {
        if (rf::console::run) {
            rf::console::get_arg(rf::console::ARG_STR, true);
            rf::Player* player = find_best_matching_player(rf::console::str_arg);

            if (player) {
                if (player != rf::local_player) {
                    rf::console::printf(rf::strings::kicking_player, player->name.c_str());
                    kick_player_delayed(player);
                }
                else
                    rf::console::print("You cannot kick yourself!");
            }
        }

        if (rf::console::help) {
            rf::console::output(rf::strings::usage, nullptr);
            rf::console::print("     kick <{}>", rf::strings::player_name);
        }
    }
}

ConsoleCommand2 unban_last_cmd{
    "unban_last",
    []() {
        if (rf::is_multi && rf::is_server) {
            auto opt = multi_ban_unban_last();
            if (opt) {
                rf::console::print("{} has been unbanned!", opt.value());
            }
        }
    },
    "Unbans last banned player",
};

void init_server_commands()
{
    map_ext_cmd.register_cmd();
    map_rest_cmd.register_cmd();
    map_next_cmd.register_cmd();
    map_rand_cmd.register_cmd();
    map_prev_cmd.register_cmd();

    AsmWriter(0x0047B6F0).jmp(ban_cmd_handler_hook);
    AsmWriter(0x0047B580).jmp(kick_cmd_handler_hook);

    unban_last_cmd.register_cmd();

    multi_kick_player_hook.install();
}
