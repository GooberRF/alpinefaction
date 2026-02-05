#include "../os/console.h"
#include "server_internal.h"
#include "../rf/multi.h"
#include "../rf/gameseq.h"
#include "../rf/level.h"
#include "../rf/player/player.h"
#include "../rf/crt.h"
#include "../rf/os/string.h"
#include "../rf/os/timer.h"
#include "server.h"
#include "multi.h"
#include <patch_common/AsmWriter.h>
#include <patch_common/CallHook.h>
#include <common/version/version.h>
#include <vector>
#include <format>
#include <optional>
#include <winsock2.h>

std::vector<int> g_players_to_kick;
static int g_next_bot_index = 1;
static uint16_t g_next_bot_port = 40000;

static rf::NetAddr make_bot_addr()
{
    rf::NetAddr addr{};
    addr.ip_addr = htonl(0x7F000001);
    addr.port = htons(g_next_bot_port++);
    return addr;
}

static bool is_name_in_use(std::string_view name)
{
    for (const rf::Player& player : SinglyLinkedList{rf::player_list}) {
        if (name == player.name.c_str()) {
            return true;
        }
    }

    return false;
}

static std::string make_default_bot_name()
{
    std::string name;
    do {
        name = std::format("Bot {}", g_next_bot_index++);
    } while (is_name_in_use(name));

    return name;
}

static rf::Player* add_server_bot(std::optional<std::string> name_opt)
{
    if (!rf::is_dedicated_server) {
        return nullptr;
    }

    if (rf::multi_num_players() >= rf::multi_max_players) {
        rf::console::output("Server is full", nullptr);
        return nullptr;
    }

    const std::string name = name_opt && !name_opt->empty()
        ? std::move(*name_opt)
        : make_default_bot_name();

    static auto& multi_on_new_player = addr_as_ref<void(
        const rf::NetAddr*,
        rf::String::Pod,
        int,
        int)>(0x0047AF30);

    rf::NetAddr addr = make_bot_addr();
    rf::String bot_name{name.c_str()};
    multi_on_new_player(&addr, bot_name, 5, 10000);

    rf::Player* player = rf::multi_find_player_by_addr(addr);
    if (!player) {
        rf::console::output("Failed to add bot", nullptr);
        return nullptr;
    }

    player->net_data->reliable_socket = -1;
    player->net_data->flags &= ~rf::NPF_WAITING_FOR_RELIABLE_SOCKET;
    player->net_data->join_time_ms = rf::timer_get(1000);

    player->is_bot = true;
    player->is_spawn_disabled = false;
    player->is_browser = false;
    player->is_human_player = false;
    player->version_info = ClientVersionInfoProfile{
        .software = ClientSoftware::AlpineFaction,
        .major = VERSION_MAJOR,
        .minor = VERSION_MINOR,
        .patch = VERSION_PATCH,
        .type = VERSION_TYPE,
        .max_rfl_ver = MAXIMUM_RFL_VERSION,
    };

    if (rf::gameseq_get_state() == rf::GS_GAMEPLAY) {
        rf::multi_spawn_player_server_side(player);
    }

    return player;
}

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
    rf::console::print("{}{}", player->name, rf::strings::was_kicked);
    g_players_to_kick.push_back(player->net_data->player_id);
}

CallHook<void(const rf::Player*)> multi_kick_player_hook{0x0047B9BD, kick_player_delayed};

void process_delayed_kicks()
{
    // Process kicks outside of packet processing loop to avoid crash when a player is suddenly destroyed (00479299)
    for (int player_id : g_players_to_kick) {
        rf::Player* player = rf::multi_find_player_by_id(player_id);
        if (player) {
            rf::multi_kick_player(player);
        }
    }
    g_players_to_kick.clear();
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

ConsoleCommand2 bot_add_cmd{
    "bot_add",
    [](std::optional<std::string> name) {
        if (rf::is_server) {
            if (rf::Player* player = add_server_bot(std::move(name))) {
                rf::console::print("Added bot '{}'", player->name);
            }
        }
    },
    "Add a bot",
    "bot_add [name]",
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
    bot_add_cmd.register_cmd();

    multi_kick_player_hook.install();
}
