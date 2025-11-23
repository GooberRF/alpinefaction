#include <patch_common/CallHook.h>
#include <patch_common/FunHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/ShortTypes.h>
#include <patch_common/AsmWriter.h>
#include <common/config/BuildConfig.h>
#include <common/version/version.h>
#include <common/rfproto.h>
#include <common/utils/list-utils.h>
#include <xlog/xlog.h>
#include <algorithm>
#include <limits>
#include <format>
#include <sstream>
#include <numeric>
#include <unordered_set>
#include <array>
#include <utility>
#include <windows.h>
#include <winsock2.h>
#include "server.h"
#include "server_internal.h"
#include "alpine_packets.h"
#include "multi.h"
#include "../os/console.h"
#include "../os/os.h"
#include "../hud/hud.h"
#include "../misc/player.h"
#include "../main/main.h"
#include "../misc/achievements.h"
#include "../misc/alpine_settings.h"
#include "../rf/file/file.h"
#include "../rf/math/vector.h"
#include "../rf/math/matrix.h"
#include "../rf/player/player.h"
#include "../rf/os/frametime.h"
#include "../rf/item.h"
#include "../rf/gameseq.h"
#include "../rf/misc.h"
#include "../rf/ai.h"
#include "../rf/multi.h"
#include "../rf/parse.h"
#include "../rf/weapon.h"
#include "../rf/entity.h"
#include "../rf/os/os.h"
#include "../rf/os/timer.h"
#include "../rf/level.h"
#include "../rf/collide.h"
#include "../purefaction/pf.h"

const char* g_rcon_cmd_whitelist[] = {
    "gt",
    "kick",
    "level",
    "sv_pass",
    "map",
    "ban",
    "ban_ip",
    "map_ext",
    "map_rest",
    "map_next",
    "map_rand",
    "map_prev",
    "sv_caplimit",
    "sv_fraglimit",
    "sv_gametype",
    "sv_geolimit",
    "sv_timelimit",
    "unban_last"
};

std::vector<rf::AlpineRespawnPoint> g_alpine_respawn_points;
std::vector<std::tuple<std::string, rf::Vector3, rf::Matrix3>> queued_item_spawn_points; // queued generated spawns
std::optional<rf::Vector3> likely_position_of_central_item; // guess at the center of the map for generated spawns
static const std::vector<std::string> possible_central_item_names = {
    "Multi Damage Amplifier",
    "Multi Invulnerability",
    "Multi Super Armor",
    "shoulder cannon",
    "Multi Super Health"
}; // prioritized list of common central items
int current_center_item_priority = possible_central_item_names.size();

AlpineServerConfig g_alpine_server_config;
AlpineServerConfigRules g_alpine_server_config_active_rules; // currently active rules which are applied
std::optional<ManualRulesOverride> g_manual_rules_override;
bool g_manually_loaded_level = false; // used to decide whether to use level-specific rules or base rules
AFGameInfoFlags g_game_info_server_flags;
std::string g_prev_level;
bool g_is_overtime = false;
rf::NetGameType upcoming_game_type;
UpcomingGameTypeSelection g_upcoming_game_type_selection = UpcomingGameTypeSelection::Rotation;
static std::optional<rf::NetGameType> g_previous_level_game_type;

const rf::NetGameType get_upcoming_game_type()
{
    return upcoming_game_type;
}

UpcomingGameTypeSelection get_upcoming_game_type_selection()
{
    return g_upcoming_game_type_selection;
}

void clear_explicit_upcoming_game_type_request()
{
    if (g_upcoming_game_type_selection == UpcomingGameTypeSelection::ExplicitRequest)
        g_upcoming_game_type_selection = UpcomingGameTypeSelection::Rotation;
}

bool set_upcoming_game_type(rf::NetGameType gt, UpcomingGameTypeSelection selection)
{
    upcoming_game_type = gt;
    g_upcoming_game_type_selection = selection;

    return upcoming_game_type != rf::netgame.type;
}

const bool was_level_loaded_manually()
{
    return g_manually_loaded_level;
}

void set_manually_loaded_level(bool is_true)
{
    g_manually_loaded_level = is_true;
    if (!g_manually_loaded_level)
        g_manual_rules_override.reset();
}

void set_manual_rules_override(ManualRulesOverride override_rules)
{
    g_manual_rules_override = std::move(override_rules);
    set_manually_loaded_level(true);
}

void clear_manual_rules_override()
{
    g_manual_rules_override.reset();
}

// Weapon stay exemption part 1: remove item when it is picked up and start respawn timer
CodeInjection weapon_stay_remove_instance_injection{
    0x0045982E,
    [](auto& regs){
        for (auto const& e : g_alpine_server_config_active_rules.weapon_stay_exemptions.exemptions) {
            if (e.exemption_enabled && regs.eax == e.index) {
                regs.eip = 0x00459865;
                return;
            }
        }
    }
};

CodeInjection weapon_stay_allow_pickup_injection{
    0x004596B4,
    [](auto& regs){
        for (auto const& e : g_alpine_server_config_active_rules.weapon_stay_exemptions.exemptions) {
            if (e.exemption_enabled && regs.eax == e.index) {
                regs.eip = 0x004596CD;
                return;
            }
        }
    }
};

FunHook<void ()> dedicated_server_load_config_hook{
    0x0046D900,
    []() {
        if (g_dedicated_launched_from_ads) {
            launch_alpine_dedicated_server();
            on_dedicated_server_launch_post();
        }
        else {
            dedicated_server_load_config_hook.call_target();
		}
    },
};

// handle setting dedicated_server flag when launched with -ads param 
CodeInjection rf_process_command_line_dedicated_server_patch{
    0x004B28A0,
    []() {
        if (get_ads_cmd_line_param().found()) {
            rf::is_dedicated_server = get_ads_cmd_line_param().found();
            std::string ads_filename = get_ads_cmd_line_param().get_arg();
            g_dedicated_launched_from_ads = true;
            g_ads_config_name = ads_filename;
            handle_min_param(); // check if -min switch was used
            handle_log_param(); // check if -log switch was used
        }
    },
};

void set_server_window_title() {
    std::string wnd_name;
    wnd_name.append(rf::netgame.name.c_str());
    wnd_name.append(" - " PRODUCT_NAME " Dedicated Server");
    SetWindowTextA(rf::main_wnd, wnd_name.c_str());
}

void on_dedicated_server_launch_post() {
    initialize_game_info_server_flags(); // build global flags var used in game_info packets
    set_server_window_title();
    set_dedicated_server_timer_frequency();

    // shuffle maplist on legacy server config (ADS shuffle is handled when rotation is built)
    if (!g_dedicated_launched_from_ads && g_alpine_server_config.dynamic_rotation) {
        shuffle_level_array();
    }
}

// should weapons drop on player death?
CodeInjection entity_drop_weapon_patch{
    0x0042B0D3,
    [](auto& regs) {
        if (g_alpine_server_config_active_rules.gungame.enabled ||
            !g_alpine_server_config_active_rules.drop_weapons) {
            regs.eip = 0x0042B2BC;
        }
    },
};

// should a reload subtract from reserve ammo?
CodeInjection entity_reload_current_primary_patch{
    0x00425506,
    [](auto& regs) {
        if (g_alpine_server_config_active_rules.gungame.enabled ||
            g_alpine_server_config_active_rules.weapon_infinite_magazines) {
            int current_reserve = regs.ecx;
            int used_ammo = regs.eax;
            regs.ecx = current_reserve + used_ammo; // negate the reload subtraction
        }
    },
};

// used only in legacy dedi serv mode
CodeInjection dedicated_server_load_config_patch{
    0x0046E103,
    [](auto& regs) {
        auto& parser = *reinterpret_cast<rf::Parser*>(regs.esp - 4 + 0x4C0 - 0x470);
        load_additional_server_config(parser);
    },
};

// used only in legacy dedi serv mode
CodeInjection dedicated_server_load_post_map_patch{
    0x0046E216,
    [](auto& regs) {
        on_dedicated_server_launch_post();
    },
};

int get_level_file_version(const std::string& file_name)
{
    auto level_file = std::make_unique<rf::File>();

    if (level_file->open(file_name.c_str(), rf::File::mode_read) != 0) {
        xlog::debug("Could not open {}", file_name);
        return -1; // error
    }

    // Seek directly to offset 4
    if (level_file->seek(4, rf::File::seek_set) != 0) {
        xlog::debug("Failed to seek in {}", file_name);
        level_file->close();
        return -1;
    }

    uint32_t version = level_file->read<uint32_t>(0, 0);

    level_file->close();
    return static_cast<int>(version);
}

void print_player_info(rf::Player* player, bool new_join) {
    if (player == rf::local_player) {
        rf::console::print("- {} (local player)",player->name);
        return;
    }

    // clients have only limited info
    if (!rf::is_server && !rf::is_dedicated_server) {
        rf::console::print("- {} | Ping: {}", player->name, player->net_data->ping);
        return;
    }

    auto& pdata = get_player_additional_data(player);

    std::string client_info;
    if (pdata.client_version == ClientVersion::alpine_faction) {
        client_info = std::format("Alpine Faction {}.{}-{}",
            pdata.client_version_major, pdata.client_version_minor, pdata.client_version_type == VERSION_TYPE_RELEASE ? "stable" : "dev");
    }
    else if (pdata.client_version == ClientVersion::dash_faction) {
        client_info = std::format("Dash Faction {}.{}{}",
            pdata.client_version_major, pdata.client_version_minor, pdata.client_version_type == VERSION_TYPE_BETA ? "-m" : "");
    }
    else if (pdata.client_version == ClientVersion::browser) {
        client_info = std::format("RF Server Browser {}.{}.{}",
            pdata.client_version_major, pdata.client_version_minor, pdata.client_version_patch);
    }
    else {
        client_info = std::format("Legacy Client");
    }

    in_addr addr;
    auto player_addr = player->net_data->addr;
    addr.S_un.S_addr = ntohl(player_addr.ip_addr);
    if (new_join) {
        rf::console::print("===| {}{} | IP: {}:{} | {} | Max RFL: {} |===",
            player->name, rf::strings::has_joined, inet_ntoa(addr), player->net_data->addr.port, client_info, pdata.max_rfl_version);
    }
    else {
        rf::console::print("- {} | IP: {}:{} | {} | Max RFL: {} | Ping: {} | HC: {}%",
            player->name, inet_ntoa(addr), player->net_data->addr.port, client_info, pdata.max_rfl_version, player->net_data->ping, pdata.damage_handicap);
    }
}

void print_all_player_info() {
    if (!rf::player_list) {
        rf::console::print("No players are currently connected!");
        return;
    }

    auto player_list = SinglyLinkedList{rf::player_list};

    rf::console::print("Connected players:");    

    for (auto& player : player_list) {
        print_player_info(&player, false);
    }
}

FunHook<void ()> dcf_info_hook{
    0x00486050,
    []() {
        int64_t total_sec = static_cast<int64_t>(rf::level.time);
        int days = int(total_sec / 86'400);
        int hours = int((total_sec / 3'600) % 24);
        int minutes = int((total_sec / 60) % 60);
        int seconds = int(total_sec % 60);

        rf::console::print("====================================================");
        if (rf::level.flags & rf::LEVEL_LOADED) {
            rf::console::print("{}: {} by {} ({})\n", rf::strings::level_name, rf::level.name, rf::level.author, rf::level.filename);

            auto time_line = std::format("{}:  {} {}, {}h {}m {}s\n", rf::strings::level_time, days, rf::strings::days, hours, minutes, seconds);
            rf::console::print("{}", time_line);

            rf::console::print("{}: {}\n", "Game type", get_game_type_string_long(rf::netgame.type));
        }
        else {
            rf::console::print("No level loaded\n");
        }

        std::string framerate_line = "";
        bool is_server = rf::is_multi && (rf::is_server || rf::is_dedicated_server);
        if (is_server) {
            framerate_line = std::format("Framerate: {:.3f} | FPS: {:.0f} ({} max) | NetFPS: {}\n",
                rf::frametime,
                rf::current_fps,
                rf::is_dedicated_server ? g_alpine_game_config.server_max_fps : g_alpine_game_config.max_fps,
                g_alpine_game_config.server_netfps
            );
        }
        else {
            if (rf::local_player)
                rf::console::print("Local player name: {}\n", rf::local_player->name);

            framerate_line = std::format("Framerate: {:.3f} | FPS: {:.0f} ({} max)\n",
                rf::frametime, rf::current_fps, g_alpine_game_config.max_fps);
        }            
        rf::console::print("{}", framerate_line);

        rf::console::print("====================================================\n");

        if (rf::is_multi) {
            rf::console::print("\n");
            print_all_player_info();
        }
    },
};

std::pair<std::string_view, std::string_view> strip_by_space(std::string_view str)
{
    auto space_pos = str.find(' ');
    if (space_pos == std::string_view::npos) {
        return {str, {}};
    }
    return {str.substr(0, space_pos), str.substr(space_pos + 1)};
}

void handle_next_map_command(rf::Player* player)
{
    int next_idx = (rf::netgame.current_level_index + 1) % rf::netgame.levels.size();
    rf::String next_level_filename = rf::netgame.levels[next_idx];
    int version = get_level_file_version(next_level_filename);
    auto msg = std::format("Next level: {} (version {})", next_level_filename, version);
    af_send_automated_chat_msg(msg, player);
}

void handle_has_map_command(rf::Player* player, std::string_view level_name)
{
    auto [is_valid, checked_level_name] = is_level_name_valid(level_name);

    auto availability = is_valid ? "available" : "NOT available";
    auto msg = std::format("Level {} is {} on the server.", checked_level_name, availability);

    af_send_automated_chat_msg(msg, player);
}

void handle_save_command(rf::Player* player, std::string_view save_name)
{
    auto& pdata = get_player_additional_data(player);
    rf::Entity* entity = rf::entity_from_handle(player->entity_handle);
    if (entity && g_alpine_server_config_active_rules.saving_enabled) {
        PlayerNetGameSaveData save_data;
        save_data.pos = entity->pos;
        save_data.orient = entity->orient;
        pdata.saves.insert_or_assign(std::string{save_name}, save_data);
        af_send_automated_chat_msg("Your position has been saved!", player);
    }
}

void handle_load_command(rf::Player* player, std::string_view save_name)
{
    auto& pdata = get_player_additional_data(player);
    rf::Entity* entity = rf::entity_from_handle(player->entity_handle);
    if (entity && g_alpine_server_config_active_rules.saving_enabled && !rf::entity_is_dying(entity)) {
        auto it = pdata.saves.find(std::string{save_name});
        if (it != pdata.saves.end()) {
            auto& save_data = it->second;
            entity->p_data.pos = save_data.pos;
            entity->p_data.next_pos = save_data.pos;
            entity->pos = save_data.pos;
            entity->orient = save_data.orient;
            if (entity->obj_interp) {
                entity->obj_interp->Clear();
            }
            rf::send_entity_create_packet_to_all(entity);
            pdata.last_teleport_timestamp.set(300);
            pdata.last_teleport_pos = save_data.pos;
            if (player && player->stats) {
                ++player->stats->caps; // repurpose caps to track load teleports in RUN scoreboard
            }
            af_send_automated_chat_msg("Your position has been restored!", player);
        }
        else {
            af_send_automated_chat_msg("You do not have any position saved!", player);
        }
    }
}

void handle_player_set_handicap(rf::Player* player, uint8_t amount)
{
    auto& pdata = get_player_additional_data(player);
    pdata.damage_handicap = amount;
    rf::console::print("At their request, {} has been given a {}% damage reduction handicap.", player->name, amount);
    auto msg = std::format("At your request, you have been given a {}% damage reduction handicap.", amount);
    af_send_automated_chat_msg(msg, player);
}

std::string get_ready_player_names(bool is_blue_team)
{
    const auto& team_players = is_blue_team ? g_match_info.ready_players_blue : g_match_info.ready_players_red;
    std::ostringstream oss;
    for (const auto& player : team_players) {
        if (oss.tellp() > 0) {
            oss << ", ";
        }
        oss << player->name;
    }
    return oss.str();
}

std::string get_unready_player_names()
{
    const auto& all_players = get_current_player_list(false);

    // Create sets
    std::unordered_set<rf::Player*> ready_players(g_match_info.ready_players_blue.begin(),
        g_match_info.ready_players_blue.end());

    ready_players.insert(g_match_info.ready_players_red.begin(), g_match_info.ready_players_red.end());

    std::ostringstream oss;
    for (const auto& player : all_players) {
        // Check if the player is not in the ready set
        if (ready_players.find(player) == ready_players.end()) {
            if (oss.tellp() > 0) {
                oss << ", ";
            }
            oss << player->name;
        }
    }
    return oss.str();
}


void handle_matchinfo_command(rf::Player* player)
{
    auto msg = std::format("\xA6 No match is queued.");

    if (g_match_info.pre_match_active) {
        if (!g_match_info.ready_players_red.empty() || !g_match_info.ready_players_blue.empty()) {
            msg = std::format("\xA6 These players are ready:\n"
                                   "RED TEAM: {}\n"
                                   "BLUE TEAM: {}\n",
                                   get_ready_player_names(0), get_ready_player_names(1));
        }
        else {
            msg = std::format("\xA6 No players are ready.");
        }        
    }

    af_send_automated_chat_msg(msg, player);
}

void handle_whosready_command(rf::Player* player)
{
    if (g_match_info.pre_match_active) {
        auto msg = std::format("\xA6 Not ready: {}\n", get_unready_player_names());
        af_broadcast_automated_chat_msg(msg);
    }
    else if (player) {
        auto msg = std::format("\xA6 No match is queued.");
        af_send_automated_chat_msg(msg, player);
    }
}

static void handle_drop_flag_request(rf::Player* player)
{
    if (rf::multi_get_game_type() != rf::NG_TYPE_CTF) {
        return; // can't drop flags unless in CTF
    }

    if (!g_alpine_server_config_active_rules.flag_dropping) {
        af_send_automated_chat_msg("\xA6 This server has disabled flag dropping.", player);
        return;
    }

    // drop flag if held
    if (rf::multi_ctf_get_red_flag_player() == player || rf::multi_ctf_get_blue_flag_player() == player) {
        rf::multi_ctf_drop_flag(player);
        rf::ctf_flag_cooldown_timestamp.set(750);
    }
}

CodeInjection process_obj_update_set_pos_injection{
    0x0047E563,
    [](auto& regs) {
        if (!rf::is_server) {
            return;
        }
        auto& entity = addr_as_ref<rf::Entity>(regs.edi);
        auto& pos = addr_as_ref<rf::Vector3>(regs.esp + 0x9C - 0x60);
        auto player = rf::player_from_entity_handle(entity.handle);
        auto& pdata = get_player_additional_data(player);
        if (pdata.last_teleport_timestamp.valid()) {
            float dist = (pos - pdata.last_teleport_pos).len();
            if (!pdata.last_teleport_timestamp.elapsed() && dist > 1.0f) {
                // Ignore obj_update packets for some time after restoring the position
                xlog::trace("ignoring obj_update after teleportation (distance {})", dist);
                regs.eip = 0x0047DFF6;
            }
            else {
                xlog::trace("not ignoring obj_update anymore after teleportation (distance {})", dist);
                pdata.last_teleport_timestamp.invalidate();
            }
        }
    },
};

static void send_private_message_with_stats(rf::Player* player)
{
    auto* stats = static_cast<PlayerStatsNew*>(player->stats);
    int accuracy = static_cast<int>(stats->calc_accuracy() * 100.0f);
    auto str = std::format(
        "PLAYER STATS\n"
        "Kills: {} - Deaths: {} - Max Streak: {}\n"
        "Accuracy: {}% ({:.0f}/{:.0f}) - Damage Given: {:.0f} - Damage Taken: {:.0f}",
        stats->num_kills, stats->num_deaths, stats->max_streak,
        accuracy, stats->num_shots_hit, stats->num_shots_fired,
        stats->damage_given, stats->damage_received);
    af_send_automated_chat_msg(str, player);
}

static void notify_for_upcoming_level_version_incompatible(rf::Player* player)
{
    std::string client_msg1 = "================== IMPORTANT ==================";
    std::string client_msg2 = "\xA6 Your client is NOT compatible with the next level!";
    std::string client_msg3 = "\xA6 To continue playing, upgrade at www.alpinefaction.com";
    af_send_automated_chat_msg(client_msg1, player);
    af_send_automated_chat_msg(client_msg2, player);
    af_send_automated_chat_msg(client_msg3, player);

    auto server_msg = std::format("{} cannot load the upcoming level. The maximum RFL version they can load is {}.",
                    player->name, get_player_additional_data(player).max_rfl_version);    
    rf::console::printf(server_msg.c_str());
}

static void notify_for_client_incompatible_with_switching_game_type(rf::Player* player)
{
    std::string client_msg1 = "================== IMPORTANT ==================";
    std::string client_msg2 = "\xA6 Your client does not support changing game type.";
    std::string client_msg3 = "\xA6 You will be kicked, but can rejoin after the level changes.";
    std::string client_msg4 = "\xA6 To avoid this in the future, upgrade at www.alpinefaction.com";
    af_send_automated_chat_msg(client_msg1, player);
    af_send_automated_chat_msg(client_msg2, player);
    af_send_automated_chat_msg(client_msg3, player);
    af_send_automated_chat_msg(client_msg4, player);

    auto server_msg = std::format("{} doesn't support changing game type. They can rejoin after the level changes.", player->name);
    rf::console::printf(server_msg.c_str());
}

CodeInjection multi_limbo_leave_pre_patch{
    0x0047C497,
    [](auto& regs) {
        if (rf::is_server) {
            const int ver = get_level_file_version(rf::level_filename_to_load);
            std::vector<rf::Player*> to_kick;

            auto plist = SinglyLinkedList{rf::player_list};
            for (auto& p : plist) {
                if (&p == rf::local_player)
                    continue;

                auto& pad = get_player_additional_data(&p);
                if (static_cast<int>(pad.max_rfl_version) < ver && pad.client_version != ClientVersion::browser) {
                    auto server_msg = std::format("{} was kicked because they cannot load the upcoming level.", p.name);
                    rf::console::printf(server_msg.c_str());

                    // queue for kick
                    to_kick.push_back(&p);
                }
                else if (get_upcoming_game_type() != rf::netgame.type &&
                    !(pad.client_version == ClientVersion::alpine_faction && pad.client_version_minor >= 2) &&
                    pad.client_version != ClientVersion::browser) {
                    auto server_msg = std::format("{} was kicked because their client does not support changing game type.", p.name);
                    rf::console::printf(server_msg.c_str());

                    // queue for kick
                    to_kick.push_back(&p);
                }
            }

            // kick anyone queued for kick
            for (auto* p : to_kick) {
                rf::multi_kick_player(p);
            }
        }
    },
};

void shuffle_level_array()
{
    // ads servers, shuffle the level+rules array (ssot) and then sync the netgame levels array
    if (g_dedicated_launched_from_ads) {
        auto& cfg = g_alpine_server_config;

        if (cfg.levels.size() <= 0)
            return;

        std::ranges::shuffle(cfg.levels, g_rng);
        rebuild_rotation_from_cfg(); // sync netgame levels array
    }
    else { // legacy servers shuffle netgame array directly
        std::ranges::shuffle(rf::netgame.levels, g_rng);
    }
    xlog::info("Shuffled level rotation");
}

static std::string normalize_level_name(std::string_view in)
{
    std::string s(in);
    // add .rfl if missing
    auto ends_with_ci = [](const std::string& str, const char* suf) {
        if (str.size() < 4)
            return false;
        auto a = str.substr(str.size() - 4);
        for (auto& c : a) c = (char)std::tolower((unsigned char)c);
        return a == suf;
    };
    if (!ends_with_ci(s, ".rfl"))
        s += ".rfl";
    return s;
}

static std::pair<bool, int> find_rotation_index_for_level(std::string_view level_name)
{
    if (!g_dedicated_launched_from_ads)
        return {false, -1};

    const auto wanted = normalize_level_name(level_name);
    const auto& cfg = g_alpine_server_config;

    for (int i = 0; i < (int)cfg.levels.size(); ++i) {
        if (string_equals_ignore_case(cfg.levels[i].level_filename, wanted)) {
            return {true, i};
        }
    }
    return {false, -1};
}

static void queue_level_switch_preferring_rotation(std::string_view level_name)
{
    const auto [found, idx] = find_rotation_index_for_level(level_name);
    if (found) {
        rf::netgame.current_level_index = idx;
        rf::level_filename_to_load = rf::netgame.levels[idx];
        set_manually_loaded_level(false);
    }
    else {
        // Not in rotation
        rf::level_filename_to_load = normalize_level_name(level_name).c_str();
        set_manually_loaded_level(true);
    }
}

std::optional<rf::NetGameType> resolve_gametype_from_name(std::string_view gametype_name)
{
    if (gametype_name.empty()) {
        return std::nullopt;
    }
    if (string_equals_ignore_case(gametype_name, "dm")) {
        return rf::NetGameType::NG_TYPE_DM;
    }
    if (string_equals_ignore_case(gametype_name, "tdm") || string_equals_ignore_case(gametype_name, "teamdm")) {
        return rf::NetGameType::NG_TYPE_TEAMDM;
    }
    if (string_equals_ignore_case(gametype_name, "ctf")) {
        return rf::NetGameType::NG_TYPE_CTF;
    }
    if (string_equals_ignore_case(gametype_name, "koth")) {
        return rf::NetGameType::NG_TYPE_KOTH;
    }
    if (string_equals_ignore_case(gametype_name, "dc")) {
        return rf::NetGameType::NG_TYPE_DC;
    }
    if (string_equals_ignore_case(gametype_name, "rev")) {
        return rf::NetGameType::NG_TYPE_REV;
    }
    if (string_equals_ignore_case(gametype_name, "run")) {
        return rf::NetGameType::NG_TYPE_RUN;
    }

    return std::nullopt;
}

bool is_gametype_name_valid(std::string_view gametype_name)
{
    return resolve_gametype_from_name(gametype_name).has_value();
}

bool multi_set_gametype_alpine(std::string_view gametype_name)
{
    auto resolved_type = resolve_gametype_from_name(gametype_name);
    if (!resolved_type) {
        return false;
    }

    set_upcoming_game_type(resolved_type.value(), UpcomingGameTypeSelection::ExplicitRequest);
    return true;
}

ConsoleCommand2 sv_game_type_cmd{
    "sv_gametype",
    [](std::optional<std::string> new_game_type, std::optional<std::string> level_name) {
        if (g_dedicated_launched_from_ads && rf::is_dedicated_server) {
            if (rf::gameseq_get_state() != rf::GameState::GS_GAMEPLAY) {
                rf::console::print("You cannot change the game type while between levels.\n");
                return;
            }

            if (new_game_type.has_value()) {
                auto resolved_type = resolve_gametype_from_name(new_game_type.value());
                if (!resolved_type) {
                    rf::console::print("Unknown game type '{}'.\n", new_game_type.value());
                    return;
                }

                std::string level_to_load;
                bool explicit_level = false;

                if (level_name.has_value()) {
                    auto [is_valid_level, normalized_level_name] = is_level_name_valid(level_name.value());
                    if (!is_valid_level) {
                        rf::console::print("Level '{}' is not available on the server!\n", level_name.value());
                        return;
                    }

                    level_to_load = std::move(normalized_level_name);
                    explicit_level = true;
                }
                else {
                    level_to_load = rf::level.filename.c_str();
                }

                set_upcoming_game_type(*resolved_type, UpcomingGameTypeSelection::ExplicitRequest);

                if (explicit_level) {
                    clear_manual_rules_override();

                    std::string display_level_name = level_to_load;
                    if (display_level_name.size() > 4) {
                        display_level_name.resize(display_level_name.size() - 4);
                    }

                    auto msg = std::format("\xA6 Loading {} on {}", display_level_name,
                                           get_game_type_string_long(*resolved_type));
                    rf::multi_chat_say(msg.c_str(), false);

                    multi_change_level_alpine(level_to_load.c_str());
                }
                else {
                    restart_current_level();
                }
            }
            else {
                rf::console::print("Current game type: {}\n", get_game_type_string_long(rf::netgame.type));
            }
        }
        else {
            rf::console::print("This command is only available for Alpine Faction dedicated servers launched with the -ads switch.\n");
        }
    },
    "Load a specific gametype. Loads level if specificed, otherwise restarts current level. Only available for ADS dedicated servers.",
    "sv_gametype <dm|tdm|ctf|koth|dc|rev> [level]",
};

DcCommandAlias gt_cmd{
    "gt",
    sv_game_type_cmd,
};

static std::string describe_alpine_restrict_verdict(const std::pair<AlpineRestrictVerdict, std::string>& verdict)
{
    switch (verdict.first) {
    case AlpineRestrictVerdict::ok:
        return "allowed";
    case AlpineRestrictVerdict::need_alpine:
        return verdict.second.empty() ? "needs Alpine Faction" : std::format("needs Alpine Faction ({})", verdict.second);
    case AlpineRestrictVerdict::need_release:
        return verdict.second.empty() ? "requires an official AF release" : std::format("requires release build ({})", verdict.second);
    case AlpineRestrictVerdict::need_update:
        return verdict.second.empty() ? "needs a newer AF build" : std::format("needs update ({})", verdict.second);
    }

    return "unknown status";
}

static void print_alpine_restrict_status_summary()
{
    const auto& cfg = g_alpine_server_config.alpine_restricted_config;
    const auto [auto_require_alpine, auto_min_minor, hard_reject] = server_features_require_alpine_client();
    const bool reject_non_alpine = cfg.reject_non_alpine_clients || hard_reject;
    const bool require_alpine = cfg.clients_require_alpine || auto_require_alpine;
    const bool enforce_release = cfg.alpine_require_release_build;
    const auto level_version = get_level_file_version(rf::level.filename.c_str());
    const auto game_type = rf::multi_get_game_type();

    rf::console::print("Alpine restriction summary:");
    rf::console::print("  Level: {} (RFL version {})", rf::level.filename.c_str(), level_version);
    rf::console::print("  Gametype: {} (ID {})", get_game_type_string_long(game_type), static_cast<int>(game_type));
    rf::console::print("  Server rules auto-require Alpine: {}{}", auto_require_alpine ? "yes" : "no",
        auto_require_alpine ? std::format(" (min version 1.{})", auto_min_minor) : "");
    rf::console::print("  Server config requires Alpine: {}", cfg.clients_require_alpine ? "yes" : "no");
    rf::console::print("  Non-Alpine clients rejected: {}", reject_non_alpine ? "yes" : "no");
    rf::console::print("  Require stable AF build: {}", enforce_release ? "yes" : "no");

    const uint32_t max_rfl = MAXIMUM_RFL_VERSION;
    const uint32_t alpine_max_rfl = MAXIMUM_RFL_VERSION;
    const uint32_t alpine_v1_max_rfl = 301u;
    const uint32_t legacy_max_rfl = 200u;

    const auto describe_client = [](std::string_view label, const ClientVersionInfoProfile& info) {
        const auto [verdict, verdict_string, hard_reject] = evaluate_alpine_restrict_status(info, true);
        return std::format("  {}: {}", label, describe_alpine_restrict_verdict(std::pair{verdict, verdict_string}));
    };

    rf::console::print("Common test cases:");
    rf::console::print("{}", describe_client("Alpine Faction 1.2", ClientVersionInfoProfile{ClientVersion::alpine_faction, VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH, VERSION_TYPE, max_rfl}));
    rf::console::print("{}", describe_client("Alpine Faction 1.2-dev", ClientVersionInfoProfile{ClientVersion::alpine_faction, VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH, VERSION_TYPE_DEV, max_rfl}));
    rf::console::print("{}", describe_client("Alpine Faction 1.1", ClientVersionInfoProfile{ClientVersion::alpine_faction, 1u, 1u, 0u, VERSION_TYPE_RELEASE, alpine_v1_max_rfl}));
    rf::console::print("{}", describe_client("Dash Faction 1.9", ClientVersionInfoProfile{ClientVersion::dash_faction, 1u, 9u, 0u, VERSION_TYPE_RELEASE, legacy_max_rfl}));
    rf::console::print("{}", describe_client("Pure Faction 3.0", ClientVersionInfoProfile{ClientVersion::pure_faction, 3u, 0u, 0u, VERSION_TYPE_RELEASE, legacy_max_rfl}));
    rf::console::print("{}", describe_client("Official RF 1.21", ClientVersionInfoProfile{ClientVersion::unknown, 1u, 2u, 1u, VERSION_TYPE_RELEASE, legacy_max_rfl}));
    rf::console::print("{}", describe_client("RFSB 5.1.4", ClientVersionInfoProfile{ClientVersion::browser, 5u, 1u, 4u, VERSION_TYPE_RELEASE, max_rfl}));
}

ConsoleCommand2 alpine_restrict_status_cmd{
    "sv_restrict_status",
    []() {
        if (rf::is_server) {
            print_alpine_restrict_status_summary();
        }
        else {
            rf::console::print("This command is only available for servers.\n");
        }
    },
    "Show the current Alpine restriction checks and expected join verdicts.",
    "sv_restrict_status",
};

void multi_change_level_alpine(const char* filename) {
    const bool have_name = (filename && filename[0] != '\0');

    if (have_name) {
        queue_level_switch_preferring_rotation(filename);
        rf::multi_change_level(rf::level_filename_to_load.c_str());
    }
    else {
        set_manually_loaded_level(false);
        rf::multi_change_level(nullptr);
    }
}

const char* get_rand_level_filename()
{
    const std::size_t num_levels = rf::netgame.levels.size();

    if (num_levels <= 1) {
        // nowhere else to go, we're staying here!
        return rf::level_filename_to_load.c_str();
    }

    std::uniform_int_distribution<std::size_t> dist_levels(0, num_levels - 1);
    std::size_t rand_level_index = dist_levels(g_rng);

    // avoid selecting current level filename (unless it appears more than once on map list)
    if (rf::netgame.levels[rand_level_index] == rf::level_filename_to_load) {
        rand_level_index = (rand_level_index + 1) % num_levels;
    }

    return rf::netgame.levels[rand_level_index].c_str();
}

bool handle_server_chat_command(std::string_view server_command, rf::Player* sender)
{
    auto [cmd_name, cmd_arg] = strip_by_space(server_command);

    if (cmd_name == "info") {
        af_send_automated_chat_msg(std::format("Server powered by Alpine Faction {} ({}), build date: {} {}", VERSION_STR, VERSION_CODE, __DATE__, __TIME__), sender);
    }
    else if (cmd_name == "vote") {
        auto [vote_name, vote_arg] = strip_by_space(cmd_arg);
        handle_vote_command(vote_name, vote_arg, sender);
    }
    else if (cmd_name == "nextmap" || cmd_name == "nextlevel") {
        handle_next_map_command(sender);
    }
    else if (cmd_name == "save") {
        handle_save_command(sender, cmd_arg);
    }
    else if (cmd_name == "load") {
        handle_load_command(sender, cmd_arg);
    }
    else if (cmd_name == "stats") {
        send_private_message_with_stats(sender);
    }
    else if (cmd_name == "hasmap" || cmd_name == "haslevel") {
        handle_has_map_command(sender, cmd_arg);
    }
    else if (cmd_name == "ready") {
        toggle_ready_status(sender);
    }
    else if (cmd_name == "matchinfo") {
        handle_matchinfo_command(sender);
    }
    else if (cmd_name == "whosready") {
        handle_whosready_command(sender);
    }
    else if (cmd_name == "dropflag") {
        handle_drop_flag_request(sender);
    }
    else {
        return false;
    }
    return true;
}

bool check_server_chat_command(const char* msg, rf::Player* sender)
{
    if (msg[0] == '/') {
        if (!handle_server_chat_command(msg + 1, sender))
            af_send_automated_chat_msg("Unrecognized server command!", sender);
        return true;
    }

    auto [cmd, rest] = strip_by_space(msg);
    if (cmd == "server")
        return handle_server_chat_command(rest, sender);
    if (cmd == "vote")
        return handle_server_chat_command(msg, sender);
    return false;
}

CodeInjection spawn_protection_duration_patch{
    0x0048089A,
    [](auto& regs) {
        if (g_alpine_server_config_active_rules.spawn_protection.enabled) {
            if (g_alpine_server_config_active_rules.spawn_protection.use_powerup) {
                rf::Player* pp = regs.esi;
                rf::multi_powerup_add(pp, 0, g_alpine_server_config_active_rules.spawn_protection.duration);
                return;
            }
        }
        *static_cast<int*>(regs.esp) = g_alpine_server_config_active_rules.spawn_protection.enabled
			? g_alpine_server_config_active_rules.spawn_protection.duration
			: 0;
    },
};

void send_sound_packet_throwaway(rf::Player* target, int sound_id)
{
    // Send sound packet
    RF_SoundPacket packet;
    packet.header.type = RF_GPT_SOUND;
    packet.header.size = sizeof(packet) - sizeof(packet.header);
    packet.sound_id = sound_id;
    // FIXME: it does not work on RF 1.21
    packet.pos.x = packet.pos.y = packet.pos.z = std::numeric_limits<float>::quiet_NaN();
    rf::multi_io_send(target, &packet, sizeof(packet));
}

void send_sound_packet(rf::Player* target, int& last_sent_time, int rate_limit, int sound_id)
{
    // Rate limiting - max `rate_limit` times per second
    int now = rf::timer_get(1000);
    if (now - last_sent_time < 1000 / rate_limit) {
        return;
    }
    last_sent_time = now;

    // Send sound packet
    RF_SoundPacket packet;
    packet.header.type = RF_GPT_SOUND;
    packet.header.size = sizeof(packet) - sizeof(packet.header);
    packet.sound_id = sound_id;
    // FIXME: it does not work on RF 1.21
    packet.pos.x = packet.pos.y = packet.pos.z = std::numeric_limits<float>::quiet_NaN();
    rf::multi_io_send(target, &packet, sizeof(packet));
}

void send_hit_sound_packet(rf::Player* target)
{
    auto& pdata = get_player_additional_data(target);
    send_sound_packet(target, pdata.last_hitsound_sent_ms, 10, 29); // fallback for legacy clients
}

// todo optimization: move this to a new flag on af_damage_notify packet
void send_critical_hit_packet(rf::Player* target)
{
    auto& pdata = get_player_additional_data(target);
    send_sound_packet(target, pdata.last_critsound_sent_ms, 10, 35); // rate limit 10/sec, sound id 35
}

FunHook<float(rf::Entity*, float, int, int, int)> entity_damage_hook{
    0x0041A350,
    [](rf::Entity* damaged_ep, float damage, int killer_handle, int damage_type, int killer_uid) {
        rf::Player* damaged_player = rf::player_from_entity_handle(damaged_ep->handle);
        rf::Player* killer_player = rf::player_from_entity_handle(killer_handle);
        bool is_pvp_damage = damaged_player && killer_player && damaged_player != killer_player;
        if (rf::is_server && is_pvp_damage) {
            damage *= g_alpine_server_config_active_rules.pvp_damage_modifier;
            if (damage == 0.0f) {
                return 0.0f;
            }

            // handle handicap
            auto& pdata = get_player_additional_data(killer_player);
            if (pdata.damage_handicap > 0) {
                float reduction = 1.0f - (pdata.damage_handicap / 100.0f);
                damage *= reduction;
                //xlog::debug("Applying handicap {}% ({}x multiplier) to damage, new damage: {}", pdata.damage_handicap, reduction, damage);
            }

            // Check if this is a crit
            if (g_alpine_server_config_active_rules.critical_hits.enabled) {
                float base_chance = g_alpine_server_config_active_rules.critical_hits.base_chance;
                float bonus_chance = 0.0f;

                // calculate bonus chance
                if (g_alpine_server_config_active_rules.critical_hits.dynamic_scale && killer_player->stats) {
                    auto* killer_stats = static_cast<PlayerStatsNew*>(killer_player->stats);

                    bonus_chance = 0.1f * std::min(killer_stats->damage_given_current_life /
                        g_alpine_server_config_active_rules.critical_hits.dynamic_damage_bonus_ceiling, 1.0f);
                }

                float critical_hit_chance = base_chance + bonus_chance;
                //xlog::debug("Critical hit chance: {:.2f}", critical_hit_chance);

                std::uniform_real_distribution<float> dist_crit(0.0f, 1.0f);
                float random_value = dist_crit(g_rng);
                if (random_value < critical_hit_chance) {

                    // Apply amp modifier to the critical hit
                    damage *= (!rf::multi_powerup_has_player(killer_player, 1)) ? rf::g_multi_damage_modifier : 1.0f;

                    // On a crit, add amp for a duration
                    int amp_time_to_add = rf::multi_powerup_get_time_until(killer_player, 1) + g_alpine_server_config_active_rules.critical_hits.reward_duration;

                    rf::multi_powerup_add(killer_player, 1, amp_time_to_add);

                    // Notify with sound
                    send_critical_hit_packet(killer_player);
                }
            }
        }

        // should entity gib?
        if (damaged_ep->life < -5.0f &&
            damage_type == 3 &&                         // explosive
            damaged_ep->material == 3 &&                // flesh
            rf::game_get_gore_level() >= 2 &&
            !(damaged_ep->entity_flags & 0x2000000) &&  // custom_corpse (used by snakes and sea creature)
            !(damaged_ep->entity_flags & 0x1) &&        // dying
            !(damaged_ep->entity_flags & 0x1000) &&     // in_water
            !(damaged_ep->entity_flags & 0x2000))       // eye_under_water
        {
            damaged_ep->entity_flags |= 0x80;
        }

        float real_damage = entity_damage_hook.call_target(damaged_ep, damage, killer_handle, damage_type, killer_uid);

        // damaged_ep may be invalid at this point. If so, assume dead to avoid a rare crash from checking life
        bool is_dead = damaged_ep ? damaged_ep->life <= 0.0f : true;

        if (rf::is_server && is_pvp_damage && real_damage > 0.0f) {

            auto* killer_player_stats = static_cast<PlayerStatsNew*>(killer_player->stats);
            killer_player_stats->add_damage_given(real_damage);

            auto* damaged_player_stats = static_cast<PlayerStatsNew*>(damaged_player->stats);
            damaged_player_stats->add_damage_received(real_damage);

            if (g_alpine_server_config.damage_notification_config.enabled && damaged_player && killer_player) {
                if (!(!damaged_ep || rf::entity_is_dying(damaged_ep) || rf::player_is_dead(damaged_player))) {

                    // use new packet for clients that can process it (Alpine 1.1+)
                    if (is_player_minimum_af_client_version(killer_player, 1, 1)) {
                        //xlog::warn("sending damage notify to {}, is dead? {}", killer_player->name, is_dead);
                        af_send_damage_notify_packet(
                            damaged_player->net_data->player_id,
                            real_damage,
                            is_dead,
                            killer_player);
                    }
                    else if (g_alpine_server_config.damage_notification_config.support_legacy_clients) {
                        //xlog::warn("sending legacy notify to {}", killer_player->name);
                        send_hit_sound_packet(killer_player); // fallback for old clients
                    }
                }
            }
        }
        
        if (is_achievement_system_initialized() &&
            !rf::is_multi &&
            damaged_ep &&
            damaged_ep->life <= 0.0f) {
            achievement_player_killed_entity(damaged_ep, damage_type, damaged_ep->killer_handle);
        }

        return real_damage;
    },
};

CallHook<int(const char*)> item_lookup_type_hook{
    0x00465102,
    [](const char* cls_name) {
        if (rf::is_dedicated_server) {
            // support item replacement mapping
            auto it = g_alpine_server_config_active_rules.item_replacements.find(cls_name);
            if (it != g_alpine_server_config_active_rules.item_replacements.end())
                cls_name = it->second.c_str();
        }
        return item_lookup_type_hook.call_target(cls_name);
    },
};

// legacy client compatible
CallHook<int(const char*)> find_default_weapon_for_entity_hook{
    0x004A43DA,
    [](const char* weapon_name) {
        if (rf::is_dedicated_server && !g_alpine_server_config_active_rules.gungame.enabled) {
            weapon_name = g_alpine_server_config_active_rules.default_player_weapon.weapon_name.data();
        }
        return find_default_weapon_for_entity_hook.call_target(weapon_name);
    },
};

// legacy client compatible
CallHook<void(rf::Player*, int, int)> give_default_weapon_ammo_hook{
    0x004A4414,
    [](rf::Player* player, int weapon_type, int ammo) {
        // if not using loadouts, this adjusts spawn weapon reserve ammo to match our clip config
        if (rf::is_server && !g_alpine_server_config_active_rules.spawn_loadout.loadouts_active) {
            ammo = rf::weapon_types[g_alpine_server_config_active_rules.default_player_weapon.index].clip_size_multi *
                   g_alpine_server_config_active_rules.default_player_weapon.num_clips;
        }

        give_default_weapon_ammo_hook.call_target(player, weapon_type, ammo);
    },
};

FunHook<bool (const char*, int)> multi_is_level_matching_game_type_hook{
    0x00445050,
    [](const char *filename, int ng_type) {
        if (ng_type == rf::NetGameType::NG_TYPE_CTF) {
            return string_starts_with_ignore_case(filename, "ctf") || string_starts_with_ignore_case(filename, "pctf");
        }
        else if (ng_type == rf::NetGameType::NG_TYPE_KOTH) {
            return string_starts_with_ignore_case(filename, "koth");
        }
        else if (ng_type == rf::NetGameType::NG_TYPE_DC) {
            return string_starts_with_ignore_case(filename, "dc");
        }
        else if (ng_type == rf::NetGameType::NG_TYPE_REV) {
            return string_starts_with_ignore_case(filename, "rev");
        }
        else if (ng_type == rf::NetGameType::NG_TYPE_RUN) {
            return string_contains_ignore_case(filename, "run");
        }
        return string_starts_with_ignore_case(filename, "dm") || string_starts_with_ignore_case(filename, "pdm");
    },
};

// handle spawn loadouts (not legacy client compatible)
CodeInjection player_create_entity_default_weapon_injection {
    0x004A43F6,
    [](auto& regs) {
        if (rf::is_server &&
            g_alpine_server_config_active_rules.spawn_loadout.loadouts_active &&
            !g_alpine_server_config_active_rules.gungame.enabled // no loadouts when gungame is on
            ) {
            rf::Player* player = regs.ebp;

            for (auto const& e : g_alpine_server_config_active_rules.spawn_loadout.red_weapons) {
                rf::player_add_weapon(player, e.index, e.reserve_ammo);
                rf::player_set_default_primary(player, e.index);
            }
            regs.eip = 0x004A4481;
        }
    },
};

CallHook<void(char*)> get_mod_name_require_client_mod_hook{
    {
        0x0047B1E0, // send_game_info_packet
        0x004B32A3, // init_anticheat_checksums
    },
    [](char* mod_name) {
        if (!rf::is_dedicated_server || g_alpine_server_config.require_client_mod) {
            get_mod_name_require_client_mod_hook.call_target(mod_name);
        }
        else {
            mod_name[0] = '\0';
        }
    },
};

CodeInjection send_ping_time_wrap_fix{
    0x0047CCE8,
    [](auto& regs) {
        auto& io_stats = addr_as_ref<rf::MultiIoStats>(regs.esi);
        auto player = addr_as_ref<rf::Player*>(regs.esp + 0xC + 0x4);
        if (!io_stats.send_ping_packet_timestamp.valid() || io_stats.send_ping_packet_timestamp.elapsed()) {
            xlog::trace("sending ping");
            io_stats.send_ping_packet_timestamp.set(3000);
            rf::multi_ping_player(player);
            io_stats.last_ping_time = rf::timer_get(1000);

            // check if player is idle
            player_idle_check(player);
        }
        regs.eip = 0x0047CD64;
    },
};

CodeInjection multi_on_new_player_injection{
    0x0047B013,
    [](auto& regs) {
        rf::Player* player = regs.esi;
        if (player) {
            update_player_active_status(player); // active pulse on join
        }

        // ADS version is in handled in process_join_req_packet_hook in network.cpp
        if (!g_dedicated_launched_from_ads) {
            rf::Player* player = regs.esi;
            in_addr addr;
            addr.S_un.S_addr = ntohl(player->net_data->addr.ip_addr);
            rf::console::print("{}{} ({})", player->name,  rf::strings::has_joined, inet_ntoa(addr));
        }
        regs.eip = 0x0047B051;
    },
};

static bool check_player_ac_status([[maybe_unused]] rf::Player* player)
{
#ifdef HAS_PF
    if (g_additional_server_config.anticheat_level > 0) {
        bool verified = pf_is_player_verified(player);
        if (!verified) {
            af_send_automated_chat_msg(
                "Sorry! Your spawn request was rejected because verification of your client software failed. "
                "Please use the latest officially released version of Alpine Faction.",
                player);
            return false;
        }

        int ac_level = pf_get_player_ac_level(player);
        if (ac_level < g_additional_server_config.anticheat_level) {
            auto msg = std::format(
                "Sorry! Your spawn request was rejected because your client did not pass anti-cheat verification (your level {}, required {}). "
                "Please make sure you do not have any mods installed and that your client software is up to date.",
                ac_level, g_additional_server_config.anticheat_level
            );
            af_send_automated_chat_msg(msg, player);
            return false;
        }
    }
#endif // HAS_PF
    return true;
}

std::vector<rf::Player*> get_current_player_list(bool include_browsers)
{
    std::vector<rf::Player*> player_list;

    SinglyLinkedList<rf::Player> linked_player_list{rf::player_list};

    for (auto& player : linked_player_list) {
        auto additional_data = get_player_additional_data(&player);

        if (include_browsers || !(additional_data.client_version == ClientVersion::browser || ends_with(player.name, " (Bot)"))) {
            player_list.push_back(&player);
        }
    }

    return player_list;
}

static bool should_balance_teams(rf::NetGameType current_game_type)
{
    const bool previous_was_team_type = g_previous_level_game_type.has_value() && multi_game_type_is_team_type(g_previous_level_game_type.value());
    const bool current_is_team_type = multi_game_type_is_team_type(current_game_type);
    const bool balance_teams_flag = (rf::netgame.flags & rf::NetGameFlags::NG_FLAG_BALANCE_TEAMS) != 0;

    const bool transitioned = g_previous_level_game_type.has_value() && !previous_was_team_type && current_is_team_type;

    g_previous_level_game_type = current_game_type;

    return transitioned || (current_is_team_type && balance_teams_flag);
}

bool is_player_ready(rf::Player* player)
{
    return g_match_info.pre_match_active &&
        (g_match_info.ready_players_red.contains(player) || g_match_info.ready_players_blue.contains(player));
}

bool is_player_in_match(rf::Player* player)
{
    return g_match_info.match_active && g_match_info.active_match_players.contains(player);
}

void update_pre_match_powerups(rf::Player* player)
{
    rf::multi_powerup_remove_all_for_player(player);

    if (g_match_info.pre_match_active) {
        rf::multi_powerup_add(player, 0, 3600000);

        if (g_match_info.ready_players_red.contains(player) || g_match_info.ready_players_blue.contains(player)) {
            rf::multi_powerup_add(player, 1, 3600000);
        }
    }
}

void start_match()
{
    auto msg = std::format(
        "\n>>>>>>>>>>>>>>>> {}v{} MATCH STARTING NOW <<<<<<<<<<<<<<<<\n"
        "RED TEAM: {}\n"
        "BLUE TEAM: {}\n",
        g_match_info.team_size, g_match_info.team_size,
        get_ready_player_names(0), get_ready_player_names(1));

    af_broadcast_automated_chat_msg(msg);

    g_match_info.active_match_players.clear();

    g_match_info.active_match_players.insert(g_match_info.ready_players_red.begin(),
                                             g_match_info.ready_players_red.end());
    g_match_info.ready_players_red.clear();

    g_match_info.active_match_players.insert(g_match_info.ready_players_blue.begin(),
                                             g_match_info.ready_players_blue.end());
    g_match_info.ready_players_blue.clear();

    restart_current_level();

    // restore time limit when starting match
    rf::multi_time_limit = g_match_info.time_limit_on_pre_match_start.value_or(10.0f);
}

void cancel_match()
{
    rf::console::print("Canceling match");
    if (g_match_info.match_active) {
        load_next_level(); // end the round if active match is canceled
    }
    else {
        // restore the level timer and limit if pre-match is canceled        
        rf::level.time = 0.0f;
        rf::multi_time_limit = g_match_info.time_limit_on_pre_match_start.value_or(10.0f);        
    }

    g_match_info.reset();

    for (rf::Player* player : get_current_player_list(false)) {
        update_pre_match_powerups(player);
    }
}

void start_pre_match()
{
    if (g_match_info.pre_match_queued) {
        g_match_info.pre_match_active = g_match_info.pre_match_queued;
        g_match_info.pre_match_queued = false;
        g_match_info.pre_match_start_time = std::time(nullptr);
        g_match_info.last_ready_reminder_time = g_match_info.pre_match_start_time; // don't remind immediately

        // store time limit for later, remove level timer during pre-match
        g_match_info.time_limit_on_pre_match_start = rf::multi_time_limit;
        rf::multi_time_limit = 0.0f;

        for (rf::Player* player : get_current_player_list(false)) {
            if (!player) continue;

            std::string msg = std::format(
                "\n>>>>>>>>>>>>>>>>> {}v{} MATCH QUEUED <<<<<<<<<<<<<<<<<\n"
                "Waiting for players. Ready up or use \"/vote nomatch\" to call a vote to cancel the match.",
                g_match_info.team_size, g_match_info.team_size);
        
            af_send_automated_chat_msg(msg, player);
        }



        for (rf::Player* player : get_current_player_list(false)) {
            update_pre_match_powerups(player);
        }
    }
}

void add_ready_player(rf::Player* player)
{
    auto& team_ready_list = (player->team == 0) ? g_match_info.ready_players_red : g_match_info.ready_players_blue;
    const std::string_view team_name = (player->team == 0) ? "RED" : "BLUE";

    if (team_ready_list.contains(player)) {
        af_send_automated_chat_msg("\xA6 You are already ready.", player);
        return;
    }

    const auto match_team_size = static_cast<size_t>(g_match_info.team_size);

    if (team_ready_list.size() >= match_team_size) {
        af_send_automated_chat_msg("\xA6 Your team is full.", player);
        return;
    }

    team_ready_list.insert(player);
    update_pre_match_powerups(player);

    auto ready_msg = std::format("\xA6 {} ({}) is ready!", player->name.c_str(), team_name);
    af_broadcast_automated_chat_msg(ready_msg);

    const auto ready_red = g_match_info.ready_players_red.size();
    const auto ready_blue = g_match_info.ready_players_blue.size();

    if (ready_red >= match_team_size && ready_blue >= match_team_size) {
        af_broadcast_automated_chat_msg("\xA6 All players are ready. Match starting!");
        g_match_info.everyone_ready = true;
        start_match(); // Start the match
    }
    else {
        auto waiting_msg = std::format("\xA6 Still waiting for players - RED: {}, BLUE: {}.", match_team_size - ready_red, match_team_size - ready_blue);
        af_broadcast_automated_chat_msg(waiting_msg);
    }
}

void remove_ready_player_silent(rf::Player* player)
{
    g_match_info.ready_players_red.erase(player);
    g_match_info.ready_players_blue.erase(player);
}

void remove_ready_player(rf::Player* player)
{
    bool was_in_red = g_match_info.ready_players_red.erase(player) > 0;
    bool was_in_blue = g_match_info.ready_players_blue.erase(player) > 0;

    if (!was_in_red && !was_in_blue) {
        af_send_automated_chat_msg("You were not marked as ready.", player);
        return;
    }

    update_pre_match_powerups(player);

    auto msg_source = std::format("\xA6 You are no longer ready! Still waiting for players - RED: {}, BLUE: {}.",
        g_match_info.team_size - g_match_info.ready_players_red.size(),
        g_match_info.team_size - g_match_info.ready_players_blue.size());

    auto msg_others = std::format("\xA6 {} is no longer ready! Still waiting for players - RED: {}, BLUE: {}.",
        player->name.c_str(),
        g_match_info.team_size - g_match_info.ready_players_red.size(),
        g_match_info.team_size - g_match_info.ready_players_blue.size());

    // send the message to the player who unreadied
    af_send_automated_chat_msg(msg_source, player);

    // send the message to other players
    for (rf::Player* proc_player : get_current_player_list(false)) {
        if (!proc_player || proc_player == player) {
            continue; // skip the player who started the vote
        }
        af_send_automated_chat_msg(msg_others, proc_player);
    }
}

void toggle_ready_status(rf::Player* player)
{
    if (!g_match_info.pre_match_active) {
        af_send_automated_chat_msg("\xA6 No match is queued. Use \"/vote match\" to queue a match.", player);
        return;
    }

    if (get_player_additional_data(player).client_version != ClientVersion::alpine_faction) {
        af_send_automated_chat_msg("\xA6 Only Alpine Faction clients can ready for matches. Learn more: alpinefaction.com", player);
        return;
    }

    // Toggle based on current ready status
    if (get_ready_status(player)) {
        remove_ready_player(player);
    }
    else {
        add_ready_player(player);
    }
}

void set_ready_status(rf::Player* player, bool is_ready)
{
    if (get_player_additional_data(player).client_version != ClientVersion::alpine_faction) {
        af_send_automated_chat_msg("\xA6 Only Alpine Faction clients can ready for matches. Learn more: alpinefaction.com", player);
        return;
    }

    if (g_match_info.pre_match_active) {
        if (is_ready) {
            add_ready_player(player);
        }
        else {
            remove_ready_player(player);
        }
    }
    else {
        af_send_automated_chat_msg("\xA6 No match is queued. Use \"/vote match\" to queue a match.", player);
    }
}

bool get_ready_status(const rf::Player* player)
{
    const auto& blue_team = g_match_info.ready_players_blue;
    const auto& red_team = g_match_info.ready_players_red;

    // Check both teams
    auto is_ready_in_blue = std::find(blue_team.begin(), blue_team.end(), player) != blue_team.end();
    auto is_ready_in_red = std::find(red_team.begin(), red_team.end(), player) != red_team.end();

    return is_ready_in_blue || is_ready_in_red;
}


void match_do_frame()
{
    if (!g_alpine_server_config.vote_match.enabled) {
        return;
    }

    if (rf::multi_num_players() <= 0) {
        if (g_match_info.match_active || g_match_info.pre_match_active) {
            cancel_match();
        }
        return; // no reminders to an empty server
    }

    std::time_t current_time = std::time(nullptr);

    if (!g_match_info.match_active && !g_match_info.pre_match_active)
    {
        if (current_time >= g_match_info.last_match_reminder_time + 270) {
            g_match_info.last_match_reminder_time = current_time;

            af_broadcast_automated_chat_msg(
                "\xA6 No active match. Use \"/vote match <type> <map filename> [preset]\" to call a match vote.");
        }
    }
    else if (g_match_info.pre_match_active) {
        int reminder_interval = (current_time - g_match_info.pre_match_start_time) > 90 ? 15 : 30;

        if (current_time >= g_match_info.last_ready_reminder_time + reminder_interval) {
            g_match_info.last_ready_reminder_time = current_time;

            const auto ready_red = g_match_info.ready_players_red.size();
            const auto ready_blue = g_match_info.ready_players_blue.size();

            for (rf::Player* player : get_current_player_list(false)) {
                if (!is_player_ready(player)) {                    
                    auto msg = std::format(
                        "\xA6 You are NOT ready! {}v{} match queued, waiting for players - RED: {}, BLUE: {}.\n"
                        "Ready up or use \"/vote nomatch\" to call a vote to cancel the match.",
                        g_match_info.team_size, g_match_info.team_size,
                        g_match_info.team_size - ready_red, g_match_info.team_size - ready_blue);
                    af_send_automated_chat_msg(msg, player);
                }
            }
        }
    }
}

std::pair<bool, std::string> is_level_name_valid(std::string_view level_name_input)
{
    std::string level_name{level_name_input};

    // add ".rfl" if it's missing
    if (!string_ends_with_ignore_case(level_name, ".rfl")) {
        level_name += ".rfl";
    }

    bool is_valid = rf::get_file_checksum(level_name.c_str()) != 0;

    return {is_valid, level_name};
}

// Function to count total spawned players (including bots and humans)
int count_spawned_players()
{
    auto player_list = SinglyLinkedList{rf::player_list};

    return std::count_if(player_list.begin(), player_list.end(), [](rf::Player& player) {
        rf::Entity* entity = rf::entity_from_handle(player.entity_handle);
        return entity != nullptr;
    });
}

void update_player_active_status(rf::Player* player)
{
    if (rf::is_dedicated_server && g_alpine_server_config.inactivity_config.enabled) {
        auto& additional_data = get_player_additional_data(player);
        additional_data.idle_kick_timestamp.invalidate();
        additional_data.idle_check_timestamp.set(g_alpine_server_config.inactivity_config.allowed_inactive_ms);    
        //xlog::warn("player {} active now! timestamp {}", player->name, get_player_additional_data(player).last_activity_ms);
    }
}

bool is_player_idle(rf::Player* player)
{
    // Check if the player's idle timer has elapsed
    const auto& additional_data = get_player_additional_data(player);
    bool is_idle = additional_data.idle_check_timestamp.valid()
        ? additional_data.idle_check_timestamp.elapsed()
        : false;

    // Player is idle if timer has elapsed and they're not spawned
    return is_idle && rf::player_is_dead(player);
}

void player_idle_check(rf::Player* player)
{
    const auto& additional_data = get_player_additional_data(player);
    if (!g_alpine_server_config.inactivity_config.enabled) {
        return; // don't continue if inactivity monitoring is disabled
    }

    if (additional_data.idle_kick_timestamp.valid()) {
        if (additional_data.idle_kick_timestamp.elapsed()) {
            kick_player_delayed(player);
        }
        return; // don't continue if a kick is already pending
    }

    if (rf::gameseq_get_state() != rf::GS_GAMEPLAY) {
        return; // don't mark players as idle unless we're in gameplay
    }

    if (additional_data.client_version == ClientVersion::browser || ends_with(player->name, " (Bot)")) {
        return; // don't mark browsers or bots as idle
    }

    if (g_match_info.match_active || g_match_info.pre_match_active) {
        return; // don't mark players as idle during a match or pre-match
    }

    if (player->net_data->join_time_ms > (rf::timer_get_milliseconds() - g_alpine_server_config.inactivity_config.new_player_grace_ms)) {
        return; // don't mark new players as idle
    }

    if (is_player_idle(player)) {
        rf::console::print("{} is idle and will be kicked if they don't spawn within 10 seconds.", player->name);
        std::string msg = std::format("\xA6 {}", g_alpine_server_config.inactivity_config.kick_message);
        af_send_automated_chat_msg(msg, player);

        // set timer to kick them after 10 seconds
        if (!additional_data.idle_kick_timestamp.valid()) {
            get_player_additional_data(player).idle_kick_timestamp.set(g_alpine_server_config.inactivity_config.warning_duration_ms);
        }
    }
}

bool version_is_older(int aMaj, int aMin, int bMaj, int bMin)
{
    if (aMaj != bMaj)
        return aMaj < bMaj;
    if (aMin != bMin)
        return aMin < bMin;
    return false;
}

std::tuple<AlpineRestrictVerdict, std::string, bool> evaluate_alpine_restrict_status(
    const ClientVersionInfoProfile& info, const bool check_level_version)
{
    const auto& cfg = g_alpine_server_config.alpine_restricted_config;

    const auto [auto_require_alpine, min_minor_version, hard_reject] = server_features_require_alpine_client();
    const bool reject_non_alpine = cfg.reject_non_alpine_clients || hard_reject;
    const bool require_alpine = cfg.clients_require_alpine || auto_require_alpine;
    const bool enforce_release_build = cfg.alpine_require_release_build;

    if (require_alpine) {
        if (info.client_version != ClientVersion::alpine_faction) {
            switch (info.client_version) {
            case ClientVersion::dash_faction:
                return {AlpineRestrictVerdict::need_alpine,
                        std::format("unsupported client - Dash Faction {}.{}", info.version_major, info.version_minor),
                        reject_non_alpine};
            case ClientVersion::browser:
                return {AlpineRestrictVerdict::ok, {}, reject_non_alpine}; // server browsers are allowed
            case ClientVersion::pure_faction:
                return {AlpineRestrictVerdict::need_alpine, "unsupported client - Pure Faction", reject_non_alpine};
            default:
                return {AlpineRestrictVerdict::need_alpine, "unsupported client - Legacy Client", reject_non_alpine};
            }
        }

        if (enforce_release_build && info.version_type != VERSION_TYPE_RELEASE) {
            return {AlpineRestrictVerdict::need_release,
                    std::format("incompatible AF client {}.{}.{} non-stable", info.version_major, info.version_minor, info.version_patch),
                    reject_non_alpine};
        }

        if (auto_require_alpine) {
            int required_minor_version = std::min(VERSION_MINOR, min_minor_version);

            if (version_is_older(info.version_major, info.version_minor, VERSION_MAJOR, required_minor_version)) {
                return {AlpineRestrictVerdict::need_update,
                    std::format("updated AF client required, current is {}.{}.{}-{}",
                        info.version_major, info.version_minor, info.version_patch, info.version_type),
                    reject_non_alpine};
            }
        }
    }

    if (check_level_version) {
        int level_version = get_level_file_version(rf::level.filename.c_str());

        if (level_version > info.max_rfl_version && info.client_version != ClientVersion::browser) {
            std::string client_name = "";
            switch (info.client_version) {
                case ClientVersion::alpine_faction:
                    client_name = std::format("Alpine Faction {}.{}.{}-{}",
                        info.version_major, info.version_minor, info.version_patch, info.version_type);
                    break;
                case ClientVersion::dash_faction:
                    client_name = std::format("Dash Faction {}.{}{}",
                        info.version_major, info.version_minor, info.version_type == VERSION_TYPE_BETA ? "-m" : "");
                    break;
                case ClientVersion::pure_faction:
                    client_name = "Pure Faction";
                    break;
                default:
                    client_name = "Legacy Client";
                    break;
            }

            return {AlpineRestrictVerdict::need_update,
                std::format("{} max RFL ver {}, {} is {}", client_name, info.max_rfl_version, rf::level.filename.c_str(), level_version),
                reject_non_alpine};
        }
    }

    return {AlpineRestrictVerdict::ok, {}, reject_non_alpine};
}

static ClientVersionInfoProfile make_client_version_info(const rf::Player* player)
{
    const auto& pad = get_player_additional_data(player);

    return ClientVersionInfoProfile{
        pad.client_version,
        pad.client_version_major,
        pad.client_version_minor,
        pad.client_version_patch,
        pad.client_version_type,
        pad.max_rfl_version
    };
}

void enforce_alpine_hard_reject_for_all_players_on_current_level()
{
    std::vector<rf::Player*> to_kick;
    auto plist = SinglyLinkedList{rf::player_list};

    for (auto& p : plist) {
        if (&p == rf::local_player) {
            continue;
        }

        const auto info = make_client_version_info(&p);
        const auto [verdict, verdict_string, hard_reject] = evaluate_alpine_restrict_status(info, true);

        if (!hard_reject || verdict == AlpineRestrictVerdict::ok) {
            continue;
        }

        auto reason = describe_alpine_restrict_verdict(std::pair{verdict, verdict_string});
        rf::console::print("{} was kicked: {}", p.name, reason);

        // queue for kick
        to_kick.push_back(&p);
    }

    // kick anyone queued for kick
    for (auto* p : to_kick) {
        rf::multi_kick_player(p);
    }
}

AlpineRestrictVerdict check_player_alpine_restrict_status(rf::Player* player)
{
    const auto info = make_client_version_info(player);
    const auto [verdict, verdict_string, hard_reject] = evaluate_alpine_restrict_status(info, false);
    return verdict;
}

bool check_can_player_spawn(rf::Player* player)
{
    if (player == rf::local_player && rf::is_server)
        return true; // listen server host can always spawn

    const auto v = check_player_alpine_restrict_status(player);
    switch (v) {
    case AlpineRestrictVerdict::ok:
        return true;
    case AlpineRestrictVerdict::need_alpine:
        af_send_automated_chat_msg("\xA6 You must upgrade to Alpine Faction to play here. Learn more at alpinefaction.com", player);
        return false;
    case AlpineRestrictVerdict::need_release:
        af_send_automated_chat_msg("\xA6 This server requires an official Alpine Faction build. Get it at alpinefaction.com", player);
        return false;
    case AlpineRestrictVerdict::need_update:
        af_send_automated_chat_msg("\xA6 This server requires a newer version of Alpine Faction. Download the update at alpinefaction.com", player);
        return false;
    }
    return false;
}

FunHook<void(rf::Player*)> multi_spawn_player_server_side_hook{
    0x00480820,
    [](rf::Player* player) {
        update_player_active_status(player); // active pulse on spawn
        const auto& pad = get_player_additional_data(player);

        if (g_alpine_server_config_active_rules.force_character.enabled) {
            player->settings.multi_character = g_alpine_server_config_active_rules.force_character.character_index;
        }
        if (!check_can_player_spawn(player)) {
            return;
        }
        if (!check_player_ac_status(player)) {
            return;
        }
        if (g_match_info.match_active && !is_player_in_match(player)) {
            af_send_automated_chat_msg("\xA6 You cannot spawn because a match is in progress. Please feel free to spectate.", player);
            return;
        }
        if (g_alpine_server_config_active_rules.ideal_player_count < 32 &&
            ends_with(player->name, " (Bot)") &&
            count_spawned_players() >= g_alpine_server_config_active_rules.ideal_player_count) {
            std::string msg = std::format("\xA6 You're a bot and you can't spawn right now.");

            af_send_automated_chat_msg(msg, player);
            return;
        }

        // if a respawn timer has been set by the server, enforce it
        if (pad.respawn_timer.valid() && !pad.respawn_timer.elapsed()) {
            float spawn_delay_left = std::max(static_cast<float>(pad.respawn_timer.time_until()) / 1000.0f, 0.001f); // at least 1ms
            std::string msg = std::format("\xA6 Respawn delay: {} seconds left until you can respawn.", spawn_delay_left);
            af_send_automated_chat_msg(msg, player);
            return;
        }

        multi_spawn_player_server_side_hook.call_target(player);

        if (player) {
            if (auto* ep = rf::entity_from_handle(player->entity_handle)) {
                if (g_alpine_server_config_active_rules.spawn_life.enabled) {
                    ep->life = g_alpine_server_config_active_rules.spawn_life.value;
                }
                if (g_alpine_server_config_active_rules.spawn_armour.enabled) {
                    ep->armor = g_alpine_server_config_active_rules.spawn_armour.value;
                }
                if (g_alpine_server_config_active_rules.gungame.enabled) {
                    gungame_on_player_spawn(player);
                }
            }

            // inform newly spawned players of their loadout
            if (rf::is_server &&
                (g_alpine_server_config_active_rules.spawn_loadout.loadouts_active &&
                !g_alpine_server_config_active_rules.gungame.enabled) // no loadouts when gungame is on
                ) {

                // Add each weapon in the loadout to the player on the server
                for (auto const& e : g_alpine_server_config_active_rules.spawn_loadout.red_weapons) {
                    rf::player_add_weapon(player, e.index, e.reserve_ammo);

                    // if remote charge, we also need to add the detonator
                    if (e.index == rf::remote_charge_weapon_type) {
                        if (auto ep = rf::entity_from_handle(player->entity_handle))
                            rf::ai_add_weapon(&ep->ai, rf::remote_charge_det_weapon_type, 0);
                    }
                }
                
                af_send_just_spawned_loadout(player, g_alpine_server_config_active_rules.spawn_loadout.red_weapons);
            }
        }

        if (g_match_info.pre_match_active) {
            update_pre_match_powerups(player);
        }
    },
};

static float get_weapon_shot_stats_delta(rf::Weapon* wp)
{
    int num_projectiles = wp->info->num_projectiles;
    rf::Entity* parent_ep = rf::entity_from_handle(wp->parent_handle);
    if (parent_ep && parent_ep->entity_flags2 & 0x1000) { // EF2_SHOTGUN_DOUBLE_BULLET_UNK
        num_projectiles *= 2;
    }
    return 1.0f / num_projectiles;
}

static void maybe_increment_weapon_hits_stat(int hit_obj_handle, rf::Weapon *wp)
{
    rf::Entity* attacker_ep = rf::entity_from_handle(wp->parent_handle);
    if (!attacker_ep) {
        return;
    }

    rf::Entity* hit_ep = rf::entity_from_handle(hit_obj_handle);
    if (!hit_ep) {
        return;
    }

    rf::Player* attacker_pp = rf::player_from_entity_handle(attacker_ep->handle);
    rf::Player* hit_pp = rf::player_from_entity_handle(hit_ep->handle);
    if (!attacker_pp || !hit_pp) {
        return;
    }

    if (!multi_is_team_game_type() || attacker_pp->team != hit_pp->team) {
        auto* stats = static_cast<PlayerStatsNew*>(attacker_pp->stats);
        stats->add_shots_hit(get_weapon_shot_stats_delta(wp));
        xlog::trace("hit a_ep {} wp {} h_ep {}", attacker_ep, wp, hit_ep);
    }
}

FunHook<int(rf::LevelCollisionOut*, rf::Weapon*)> multi_lag_comp_handle_hit_hook{
    0x0046F380,
    [](rf::LevelCollisionOut *col_info, rf::Weapon *wp) {
        if (rf::is_server) {
            maybe_increment_weapon_hits_stat(col_info->obj_handle, wp);
        }
        return multi_lag_comp_handle_hit_hook.call_target(col_info, wp);
    },
};

FunHook<void(rf::Entity*, rf::Weapon*)> multi_lag_comp_weapon_fire_hook{
    0x0046F7E0,
    [](rf::Entity *ep, rf::Weapon *wp) {
        multi_lag_comp_weapon_fire_hook.call_target(ep, wp);
        rf::Player* pp = rf::player_from_entity_handle(ep->handle);
        if (pp && pp->stats) {
            auto* stats = static_cast<PlayerStatsNew*>(pp->stats);
            stats->add_shots_fired(get_weapon_shot_stats_delta(wp));
            xlog::trace("fired a_ep {} wp {}", ep, wp);
        }
    },
};

void server_reliable_socket_ready(rf::Player* player)
{
    auto& data = get_player_additional_data(player);

    // welcome players, restricting to only welcoming alpine clients if configured
    if (g_alpine_server_config_active_rules.welcome_message.enabled) {
        if (!g_alpine_server_config.alpine_restricted_config.only_welcome_alpine || data.client_version == ClientVersion::alpine_faction) {
            auto msg = string_replace(g_alpine_server_config_active_rules.welcome_message.welcome_message, "$PLAYER", player->name.c_str());
            af_send_automated_chat_msg(msg, player);
        }
    }

    // alert alpine clients to the queued match on join
    if (g_match_info.pre_match_active && data.client_version == ClientVersion::alpine_faction) {    
        auto msg = std::format("\xA6 Match is queued and waiting for players: {}v{}! Use \"/ready\" to ready up.",
            g_match_info.team_size, g_match_info.team_size);

        af_send_automated_chat_msg(msg, player);
    }

    int pm = data.client_version_major;
    int pn = data.client_version_minor;

    // advertise AF to non-alpine clients if configured
    if (g_alpine_server_config.alpine_restricted_config.advertise_alpine) {
        if (data.client_version != ClientVersion::alpine_faction) {
            auto msg = std::format(
                "\xA6 Have you heard of Alpine Faction? It's a new patch with lots of new and modern features! This server encourages you to upgrade for the best player experience. Learn more at alpinefaction.com");
            af_send_automated_chat_msg(msg, player);
        }
        else if (VERSION_TYPE == VERSION_TYPE_RELEASE && (pm < VERSION_MAJOR || (pm == VERSION_MAJOR && pn < VERSION_MINOR))) {
            auto msg = std::format("\xA6 A new version of Alpine Faction is available! Learn more at alpinefaction.com");
            af_send_automated_chat_msg(msg, player);
        }
    }
}

CodeInjection multi_level_init_injection{
    0x0046E450,
    [](auto& regs) {
        if (rf::is_server) {
            initialize_game_info_server_flags();
            af_send_server_info_packet_to_all();
            if (g_alpine_server_config.dynamic_rotation &&
                rf::netgame.current_level_index == rf::netgame.levels.size() - 1 && rf::netgame.levels.size() > 1) {
                // if this is the last level in the list and dynamic rotation is on, shuffle
                shuffle_level_array();
            }
            enforce_alpine_hard_reject_for_all_players_on_current_level();
        }
    },
};

CodeInjection multi_balance_teams_injection{
    0x0048215D,
    [](auto& regs) {
        const rf::NetGameType current_game_type = rf::multi_get_game_type();
        if (should_balance_teams(current_game_type)) {
            regs.eip = 0x00482170; // balance teams
        }
        else {
            regs.eip = 0x004823ED; // end function, no balancing
        }
    },
};

bool round_is_tied(rf::NetGameType game_type)
{
    if (rf::multi_num_players() <= 1) {
        return false;
    }

    switch (game_type) {
    case rf::NG_TYPE_DM: {
        const auto current_players = get_current_player_list(false);

        if (current_players.empty())
            return false;

        const auto [highest_score, players_with_highest_score] = [&]() {
            int highest = (*current_players.begin())->stats->score;
            int count = 0;

            for (const auto* player : current_players) {
                if (player->stats->score > highest) {
                    highest = player->stats->score;
                    count = 1;
                }
                else if (player->stats->score == highest) {
                    ++count;
                }
            }
            return std::make_pair(highest, count);
        }();

        return players_with_highest_score >= 2;
    }
    case rf::NG_TYPE_CTF: {
        int red_score = rf::multi_ctf_get_red_team_score();
        int blue_score = rf::multi_ctf_get_blue_team_score();
        //xlog::warn("red: {}, blue: {}", red_score, blue_score);

        if (red_score == blue_score) {
            return true;
        }

        if (g_alpine_server_config.overtime.consider_tie_if_flag_stolen) {
            bool red_flag_stolen = !rf::multi_ctf_is_red_flag_in_base();
            bool blue_flag_stolen = !rf::multi_ctf_is_blue_flag_in_base();

            // not currently tied, but if the team with the flag right now caps it, they will be
            return (red_flag_stolen && blue_score == red_score - 1) || (blue_flag_stolen && red_score == blue_score - 1);
        }
        else {
            return false;
        }        
    }
    case rf::NG_TYPE_TEAMDM: {
        return rf::multi_tdm_get_red_team_score() == rf::multi_tdm_get_blue_team_score();
    }
    case rf::NG_TYPE_DC:
    case rf::NG_TYPE_KOTH: {
        return multi_koth_get_red_team_score() == multi_koth_get_blue_team_score();
    }
    default: // other modes (e.g. REV and RUN) can't be tied
        return false;
    }
}

bool rev_all_points_permalocked()
{
    if (!gt_is_rev() || g_koth_info.hills.empty())
        return false;

    for (const auto& h : g_koth_info.hills) {
        if (h.lock_status != HillLockStatus::HLS_Permalocked)
            return false;
    }
    return true;
}

FunHook<void()> multi_check_for_round_end_hook{
    0x0046E7C0,
    []() {
        if (g_match_info.pre_match_active) {
            return; // round can't end during pre-match
        }

        bool time_up = (rf::multi_time_limit > 0.0f && rf::level.time >= rf::multi_time_limit);
        bool round_over = time_up;
        const auto game_type = rf::multi_get_game_type();

        if (g_is_overtime) {
            round_over = (time_up || !round_is_tied(game_type));
        }
        else {
            switch (game_type) {
            case rf::NG_TYPE_DM: {
                auto current_players = get_current_player_list(true);

                if (current_players.empty())
                    break;

                for (rf::Player* player : current_players) {
                    if (player->stats->score >= rf::multi_kill_limit) {
                        round_over = true;
                        break;
                    }
                }
                break;
            }
            case rf::NG_TYPE_CTF: {
                if (rf::multi_ctf_get_red_team_score() >= rf::multi_cap_limit ||
                    rf::multi_ctf_get_blue_team_score() >= rf::multi_cap_limit) {
                    round_over = true;
                }
                break;
            }
            case rf::NG_TYPE_TEAMDM: {
                if (rf::multi_tdm_get_red_team_score() >= rf::multi_kill_limit ||
                    rf::multi_tdm_get_blue_team_score() >= rf::multi_kill_limit) {
                    round_over = true;
                }
                break;
            }
            case rf::NG_TYPE_DC: {
                if (multi_koth_get_red_team_score() >= g_alpine_server_config_active_rules.dc_score_limit ||
                    multi_koth_get_blue_team_score() >= g_alpine_server_config_active_rules.dc_score_limit) {
                    round_over = true;
                }
                break;
            }
            case rf::NG_TYPE_KOTH: {
                if (multi_koth_get_red_team_score() >= g_alpine_server_config_active_rules.koth_score_limit ||
                    multi_koth_get_blue_team_score() >= g_alpine_server_config_active_rules.koth_score_limit) {
                    round_over = true;
                }
                break;
            }
            case rf::NG_TYPE_REV: {
                if (rev_all_points_permalocked()) {
                    round_over = true;
                }
                break;
            }
            default: // other modes (e.g. RUN) have no round end condition except timer
                break;
            }
        }

        if (round_over && rf::gameseq_get_state() != rf::GS_MULTI_LIMBO) {
            //xlog::warn("round time up {}, overtime? {}, already? {}, tied? {}", time_up, g_additional_server_config.overtime.enabled, g_is_overtime, round_is_tied(game_type));

            if (time_up && g_alpine_server_config.overtime.enabled && !g_is_overtime && g_match_info.match_active && round_is_tied(game_type)) {
                g_is_overtime = true;
                extend_round_time(g_alpine_server_config.overtime.additional_time);

                std::string msg = std::format("\xA6 OVERTIME! Game will end when the tie is broken");
                msg += g_alpine_server_config.overtime.additional_time > 0
                           ? std::format(", or in {} minutes!", g_alpine_server_config.overtime.additional_time)
                           : "!";
                af_broadcast_automated_chat_msg(msg);
            }
            else {
                set_manually_loaded_level(false);
                rf::multi_change_level(nullptr);
            }
        }
    }
};

rf::AlpineRespawnPoint* get_alpine_respawn_point_by_uid(int uid)
{
    auto it = std::ranges::find(g_alpine_respawn_points, uid, &rf::AlpineRespawnPoint::uid);
    return (it != g_alpine_respawn_points.end()) ? std::addressof(*it) : nullptr;
}

void set_alpine_respawn_point_enabled(rf::AlpineRespawnPoint* point, bool enabled)
{
    if (point) {
        point->enabled = enabled;
    }
}

void set_alpine_respawn_point_teams(rf::AlpineRespawnPoint* point, bool red, bool blue)
{
    if (point) {
        point->red_team = red;
        point->blue_team = blue;
    }
}

void multi_create_alpine_respawn_point(int uid, const char* name, rf::Vector3 pos, rf::Matrix3 orient, bool red, bool blue, bool enabled = true) {
    constexpr size_t max_respawn_points = 2048;

    if (g_alpine_respawn_points.size() >= max_respawn_points) {
        return; // reached max spawn points
    }

    g_alpine_respawn_points.emplace_back(rf::AlpineRespawnPoint{uid, enabled, rf::String(name), pos, orient, red, blue});
    //xlog::warn("New spawn point added! Name: {}, UID: {}, RedTeam: {}, BlueTeam: {}", name, uid, red, blue);
}

// clear spawn point array and reset last spawn index at level start
FunHook<void()> multi_respawn_level_init_hook {
    0x00470180,
    []() {
        g_alpine_respawn_points.clear();    
        
        auto player_list = get_current_player_list(false);
        std::for_each(player_list.begin(), player_list.end(),
            [](rf::Player* player) { get_player_additional_data(player).last_spawn_point_index.reset(); });        

        multi_respawn_level_init_hook.call_target();
    }
};

// more flexible replacement for get_nearest_other_player_dist_sq in stock game
float get_nearest_other_player(const rf::Player* player, const rf::Vector3* spawn_pos, bool only_enemies = false)
{
    float min_dist_sq = std::numeric_limits<float>::max();
    const bool is_team_game = multi_is_team_game_type();
    const int player_team = player->team;

    auto player_list = get_current_player_list(false);

    for (const auto* other_player : player_list) {
        if (other_player == player) {
            continue;
        }

        if (only_enemies && is_team_game && other_player->team == player_team) {
            continue;
        }

        auto* other_entity = rf::entity_from_handle(other_player->entity_handle);
        if (!other_entity) {
            continue;
        }

        const float dist_sq = rf::vec_dist_squared(spawn_pos, &other_entity->pos);
        //xlog::debug("Distance to {}: {}", other_player->name, std::sqrt(dist_sq));

        if (dist_sq < min_dist_sq) {
            min_dist_sq = dist_sq;
        }
    }

    return min_dist_sq;
}

FunHook<void(rf::Vector3*, rf::Matrix3*, rf::Player*)> multi_respawn_get_next_point_hook{
    0x00470300, [](rf::Vector3* pos, rf::Matrix3* orient, rf::Player* player) {
        //if (!rf::is_server)
        //    return; // needs more investigation

        // Level has no respawn points
        if (g_alpine_respawn_points.empty()) {
            *pos = rf::level.player_start_pos;
            *orient = rf::level.player_start_orient;
            xlog::debug("No Multiplayer Respawn Points found. Spawning {} at the Player Start.", player->name);
            return;
        }

        // Use full RNG if player is invalid (strictly for safety, this should never happen)
        if (!player) {
            std::uniform_int_distribution<int> dist(0, g_alpine_respawn_points.size() - 1);
            int index = dist(g_rng);
            *pos = g_alpine_respawn_points[index].position;
            *orient = g_alpine_respawn_points[index].orientation;
            xlog::debug("A respawn point was requested for an invalid player.");
            return;
        }

        auto& pdata = get_player_additional_data(player);
        const int team = player->team;
        const int last_index = pdata.last_spawn_point_index.value_or(-1);
        const bool is_team_game = multi_is_team_game_type();
        const auto& config = g_alpine_server_config_active_rules.spawn_logic;

        //xlog::debug("Spawn point requested! Player: {}, Team: {}, Last Spawn Index: {}", player->name, team, last_index);

        // Step 1: Build a list of eligible spawn points for this request
        std::vector<rf::AlpineRespawnPoint*> eligible_points;
        for (auto& point : g_alpine_respawn_points) {
            if (!point.enabled) {
                continue; // Skip disabled spawn points
            }
            if (config.respect_team_spawns && is_team_game) {
                if ((team == 0 && !point.red_team) || (team == 1 && !point.blue_team)) {
                    continue; // Only use correct team spawn points in team games
                }
            }
            if (config.always_avoid_last && last_index == (&point - &g_alpine_respawn_points[0])) {
                continue; // If avoid_last is on, remove this player's last spawn point
            }

            // Calculate this point's distance from the nearest other player
            point.dist_other_player = get_nearest_other_player(player, &point.position, config.only_avoid_enemies);

            eligible_points.push_back(&point);
        }

        // If no valid spawn points remain, use full RNG
        if (eligible_points.empty()) {
            std::uniform_int_distribution<int> dist(0, g_alpine_respawn_points.size() - 1);
            int index = dist(g_rng);
            *pos = g_alpine_respawn_points[index].position;
            *orient = g_alpine_respawn_points[index].orientation;
            xlog::debug("No eligible respawn points were found. Spawning {} at random respawn point {}.", player->name, index);
            return;
        }

        // Step 2: If needed, sort the list based on distance from the nearest other player
        if (config.try_avoid_players || config.always_use_furthest) {
            std::sort(eligible_points.begin(), eligible_points.end(),
                      [](const rf::AlpineRespawnPoint* a, const rf::AlpineRespawnPoint* b) {
                          return a->dist_other_player > b->dist_other_player;
                      });
        }

        // Step 3: Select a spawn point
        int selected_index = 0;
        if (config.always_use_furthest && eligible_points[0]->dist_other_player < std::numeric_limits<float>::max()) {
            selected_index = 0; // Always pick the furthest point
            if (config.always_avoid_last &&
                last_index == std::distance(g_alpine_respawn_points.data(), eligible_points[0]) &&
                eligible_points.size() > 1) {
                selected_index = 1; // Pick second furthest if the last spawn was the furthest
            }
        }
        else if (config.try_avoid_players) {
            // Weighted RNG to favor further spawns
            std::uniform_real_distribution<double> real_dist(0.0, 1.0);
            selected_index = static_cast<int>((1 - std::sqrt(real_dist(g_rng))) * eligible_points.size());
        }
        else {
            // Full RNG if we don't care about player distance
            std::uniform_int_distribution<int> dist(0, eligible_points.size() - 1);
            selected_index = dist(g_rng);
        }

        // Convert selected_index (from eligible_points) to g_new_multi_respawn_points index
        int global_index = std::distance(g_alpine_respawn_points.data(), eligible_points[selected_index]);

        // Return position and orientation of the selected spawn point
        *pos = eligible_points[selected_index]->position;
        *orient = eligible_points[selected_index]->orientation;
        pdata.last_spawn_point_index = global_index; // Record this spawn for future avoid_last checks

        // Log final selection
        //xlog::debug("Selected a spawn point for {}: Eligible Spawns Index {} (Global Index {})", player->name, selected_index, global_index);
    }
};

std::vector<rf::AlpineRespawnPoint> get_alpine_respawn_points() {
    return g_alpine_respawn_points;
}

bool are_flags_initialized()
{
    return rf::ctf_red_flag_item != nullptr && rf::ctf_blue_flag_item != nullptr;
}

// returns 1 if closer to red, 0 if closer to blue, nullopt if no flags or flags are the same position
std::optional<int> is_closer_to_red_flag(const rf::Vector3* pos)
{
    if (!are_flags_initialized()) {
        return std::nullopt;
    }

    rf::Vector3 red_flag_pos, blue_flag_pos;
    rf::multi_ctf_get_red_flag_pos(&red_flag_pos);
    rf::multi_ctf_get_blue_flag_pos(&blue_flag_pos);

    if (red_flag_pos.x == blue_flag_pos.x &&
        red_flag_pos.y == blue_flag_pos.y &&
        red_flag_pos.z == blue_flag_pos.z) {
        return std::nullopt;
    }

    float dist_to_red_sq =  std::pow(pos->x - red_flag_pos.x, 2) +
                            std::pow(pos->y - red_flag_pos.y, 2) +
                            std::pow(pos->z - red_flag_pos.z, 2);

    float dist_to_blue_sq = std::pow(pos->x - blue_flag_pos.x, 2) +
                            std::pow(pos->y - blue_flag_pos.y, 2) +
                            std::pow(pos->z - blue_flag_pos.z, 2);

    return dist_to_red_sq < dist_to_blue_sq ? 1 : 0;
}

void create_spawn_point_from_item(const std::string& name, const rf::Vector3* pos, rf::Matrix3* orient)
{
    bool red_spawn = true;
    bool blue_spawn = true;

    if (multi_is_team_game_type()) {
        if (auto is_closer_to_red = is_closer_to_red_flag(pos); is_closer_to_red.has_value()) {
            red_spawn = *is_closer_to_red == 1;
            blue_spawn = !red_spawn;
        }
    }

    multi_create_alpine_respawn_point(-1, name.c_str(), *pos, *orient, red_spawn, blue_spawn);
}

int get_item_priority(const std::string& item_name)
{
    auto it = std::find(possible_central_item_names.begin(), possible_central_item_names.end(), item_name);
    return it != possible_central_item_names.end() ?
        std::distance(possible_central_item_names.begin(), it) : possible_central_item_names.size();
}

void adjust_yaw_to_face_center(rf::Matrix3& orient, const rf::Vector3& pos, const rf::Vector3& center)
{
    rf::Vector3 direction = center - pos;
    direction.normalize();
    orient.fvec = direction;
    orient.uvec = rf::Vector3{0.0f, 1.0f, 0.0f};
    orient.rvec = orient.uvec.cross(orient.fvec);
    orient.rvec.normalize();
    orient.uvec = orient.fvec.cross(orient.rvec);
    orient.uvec.normalize();
}

void process_queued_spawn_points_from_items()
{
    const auto& logic = g_alpine_server_config_active_rules.spawn_logic;

    if (!rf::is_dedicated_server || !logic.dynamic_respawns || logic.dynamic_respawn_items.empty()) {
        return;
    }

    auto map_center = likely_position_of_central_item;

    for (auto& [name, pos, orient] : queued_item_spawn_points) {
        rf::Matrix3 adjusted_orient = orient;

        if (map_center) {
            adjust_yaw_to_face_center(adjusted_orient, pos, *map_center);
        }

        create_spawn_point_from_item(name, &pos, &adjusted_orient);
    }

    //reset item generated spawn vars
    queued_item_spawn_points.clear();
    likely_position_of_central_item.reset();
    current_center_item_priority = possible_central_item_names.size();
}

CallHook<rf::Item*(int, const char*, int, int, const rf::Vector3*, rf::Matrix3*, int, bool, bool)> item_create_hook{
    0x00465175,
    [](int type, const char* name, int count, int parent_handle, const rf::Vector3* pos, rf::Matrix3* orient,
       int respawn_time, bool permanent, bool from_packet) {

        // when creating it, check if a spawn time override is configured for this item
        if (auto it = g_alpine_server_config_active_rules.item_respawn_time_overrides.find(name);
            it != g_alpine_server_config_active_rules.item_respawn_time_overrides.end()) {
            respawn_time = it->second;
        }

        const auto& logic = g_alpine_server_config_active_rules.spawn_logic;
        if (rf::is_dedicated_server && logic.dynamic_respawns) {
            // look up this item in the vector
            auto itcfg = std::find_if(logic.dynamic_respawn_items.begin(), logic.dynamic_respawn_items.end(),
                                      [&](auto const& cfg) { return cfg.item_name == name; });

            if (itcfg != logic.dynamic_respawn_items.end()) {
                int threshold = itcfg->min_respawn_points;
                // queue if no threshold or we're under it
                if (threshold == 0 || threshold > static_cast<int>(g_alpine_respawn_points.size())) {
                    queued_item_spawn_points.emplace_back(std::string(name), *pos, *orient);
                }
            }

            // attempt to isolate the center of the map based on items likely located there
            int item_priority = get_item_priority(name);
            if (item_priority < static_cast<int>(possible_central_item_names.size())) {
                if (!likely_position_of_central_item || item_priority < current_center_item_priority) {
                    likely_position_of_central_item = *pos;
                    current_center_item_priority = item_priority;
                }
            }
        }

        return item_create_hook.call_target(
            type, name, count, parent_handle, pos, orient, respawn_time, permanent, from_packet);
    }
};

void server_add_player_weapon(rf::Player* player, int weapon_type, bool full_ammo)
{
    rf::WeaponInfo& winfo = rf::weapon_types[weapon_type];
    int ammo_count = winfo.clip_size;
    if (full_ammo) {
        if (g_alpine_server_config_active_rules.gungame.enabled && !rf::weapon_uses_clip(weapon_type)) {
            // hackfix: in gungame, set max ammo to 999 for non-clip weapons
            winfo.max_ammo = 9999;
        }        
        ammo_count = winfo.max_ammo + winfo.clip_size;
    }
    rf::Entity* ep = rf::entity_from_handle(player->entity_handle);
    rf::ai_add_weapon(&ep->ai, weapon_type, ammo_count);
     if (!rf::weapon_uses_clip(weapon_type)) {
        if (!rf::player_is_dead(player)) {
            rf::Entity* entity = rf::entity_from_handle(player->entity_handle);
            RF_ReloadPacket packet;
            packet.header.type = RF_GPT_RELOAD;
            packet.header.size = sizeof(packet) - sizeof(packet.header);
            packet.entity_handle = entity->handle;
            packet.weapon = weapon_type;
            packet.ammo = entity->ai.clip_ammo[weapon_type]; // could be zeroed
            packet.clip_ammo = entity->ai.ammo[winfo.ammo_type];
            rf::multi_io_send_reliable(player, reinterpret_cast<uint8_t*>(&packet), sizeof(packet), 0);
        }
    }
}

void entity_drop_powerup(rf::Entity* ep, int powerup_type, int count)
{
    if (!ep || (ep->p_data.flags & 0x400000) != 0) {
        return;
    }

    rf::Vector3 drop_position = ep->pos;
    rf::Matrix3 drop_orient = ep->orient;

    rf::Item* dropped_item = nullptr;

    switch (powerup_type) {
    case 1:
        dropped_item = rf::item_create(33, "Multi Damage Amplifier", count, -1, &drop_position, &drop_orient, -1, 0, 0);
        break;
    case 0:
        dropped_item = rf::item_create(32, "Multi Invulnerability", count, -1, &drop_position, &drop_orient, -1, 0, 0);
        break;
    default:
        return;
    }

    if (dropped_item) {
        dropped_item->item_flags |= 8u;
        rf::send_item_create_packet(dropped_item, 0);
    }
}

// should amps drop on player death?
CodeInjection entity_maybe_die_patch{
    0x00420600,
    [](auto& regs) {
        if (rf::is_multi && (rf::is_server || rf::is_dedicated_server)) {
            rf::Entity* ep = regs.esi;

            if (ep) {
                rf::Player* player = rf::player_from_entity_handle(ep->handle);

                if (g_alpine_server_config_active_rules.spawn_delay.enabled) {
                    rf::Player* player = rf::player_from_entity_handle(ep->handle);
                    auto& pad = get_player_additional_data(player);
                    pad.respawn_timer.set(g_alpine_server_config_active_rules.spawn_delay.base_value);
                    bool respawn_allowed = true; // nothing currently disables respawns
                    bool force_respawn = (rf::multi_server_flags & rf::NetGameFlags::NG_FLAG_FORCE_RESPAWN) != 0;

                    if (rf::is_server) {
                        set_local_spawn_delay(respawn_allowed, force_respawn, static_cast<uint16_t>(pad.respawn_timer.time_until()));
                    }
                    af_send_just_died_info_packet(player, respawn_allowed, force_respawn, static_cast<uint16_t>(pad.respawn_timer.time_until()));
                }

                if (g_alpine_server_config_active_rules.drop_amps) {
                    if (rf::multi_powerup_has_player(player, 1)) {
                        int amp_count = 0;
                        int time_left = rf::multi_powerup_get_time_until(player, 1);
                        amp_count = time_left >= 1000 ? time_left : 0;

                        if (amp_count >= 1000) { // only drop if at least 1 second left
                            entity_drop_powerup(ep, 1, amp_count / 1000); // item_touch_multi_amp multiplies by 1000
                        }
                    }

                    if (rf::multi_powerup_has_player(player, 0)) {
                        int invuln_count = 0;
                        int time_left = rf::multi_powerup_get_time_until(player, 0);
                        invuln_count = time_left >= 1000 ? time_left : 0;

                        if (invuln_count >= 1000) { // only drop if at least 1 second left
                            entity_drop_powerup(ep, 0, invuln_count / 1000); // item_touch_multi_amp multiplies by 1000
                        }
                    }
                }
            }
        }
    },
};

// ensure bag isn't purged for being the oldest dropped item (not currently used)
/* CodeInjection item_get_oldest_dynamic_patch{
    0x00458858,
    [](auto& regs) {
        if (g_additional_server_config.bagman.enabled) {

            rf::Item* item = regs.esi;

            if (item) {
                //xlog::warn("checked item {}, UID {}", item->name, item->uid);
                if (item->name == "Multi Damage Amplifier") {
                    //xlog::warn("found bag item, ensuring it persists");
                    regs.eip = 0x0045887E;
                }
            }
        }
    },
};*/

CallHook<rf::Entity*(int, const char*, int, rf::Vector3*, rf::Matrix3*, int, int)> entity_create_no_collide_hook {
    0x004A41D3,
    [](int type, const char* name, int parent_handle, rf::Vector3* pos, rf::Matrix3* orient, int create_flags, int mp_character) {

        if (get_df_server_info().has_value() && get_df_server_info()->no_player_collide) {
            create_flags |= 0x4;
        }

        return entity_create_no_collide_hook.call_target(type, name, parent_handle, pos, orient, create_flags, mp_character);
    }
};

CodeInjection allow_red_cap_when_stolen_patch{
    0x00473C0A,
    [](auto& regs) {
        if (g_alpine_server_config_active_rules.flag_captures_while_stolen) {
            regs.eip = 0x00473C2C;
        }
    },
};

CodeInjection allow_blue_cap_when_stolen_patch{
    0x00473CB9,
    [](auto& regs) {
        if (g_alpine_server_config_active_rules.flag_captures_while_stolen) {
            regs.eip = 0x00473CD3;
        }
    },
};

CodeInjection dropped_blue_flag_return_time_patch{
    0x00473B88,
    [](auto& regs) {
        rf::multi_ctf_flag_blue_stolen_timestamp.set(g_alpine_server_config_active_rules.ctf_flag_return_time_ms);
        regs.eip = 0x00473B97; // skip stock timestamp set call
    },
};

CodeInjection dropped_red_flag_return_time_patch{
    0x00473B28,
    [](auto& regs) {
        rf::multi_ctf_flag_red_stolen_timestamp.set(g_alpine_server_config_active_rules.ctf_flag_return_time_ms);
        regs.eip = 0x00473B37; // skip stock timestamp set call
    },
};

CodeInjection sp_damage_calculation_patch{
    0x0041A373,
    [](auto& regs) {
        if (g_alpine_server_config.use_sp_damage_calculation) {
            regs.eip = 0x0041A3C1;
        }
    },
};

void server_init()
{
    // Update the message when a dedicated server launches with some wrong options
    static char new_config_error_message[] =
        "Oh no! Either you haven't specified any maps, or you have an error in your dedicated server configuration.\n"
        "If this issue persists, join the community Discord at https://discord.gg/factionfiles for some assistance.\n";
    AsmWriter{0x0046E230}.push(reinterpret_cast<int32_t>(new_config_error_message));

    // Handle no player collide server option
    entity_create_no_collide_hook.install();

    // Handle dropping amps on death
    entity_maybe_die_patch.install();
    //item_get_oldest_dynamic_patch.install(); // bagman, not currently used

    // Allow players to capture CTF flag even if their own flag is stolen
    allow_red_cap_when_stolen_patch.install();
    allow_blue_cap_when_stolen_patch.install();

    // Set CTF flag return time
    dropped_blue_flag_return_time_patch.install();
    dropped_red_flag_return_time_patch.install();

    // SP-style damage calculations (ie. armour doesnt fully protect health)
    sp_damage_calculation_patch.install();

    // new "info" command for ADS servers
    dcf_info_hook.install();

    // Override rcon command whitelist
    write_mem_ptr(0x0046C794 + 1, g_rcon_cmd_whitelist);
    write_mem_ptr(0x0046C7D1 + 2, g_rcon_cmd_whitelist + std::size(g_rcon_cmd_whitelist));

    // Additional server config
    dedicated_server_load_config_hook.install(); // asd loading
    rf_process_command_line_dedicated_server_patch.install(); // set dedi server bool when launching via ads
    dedicated_server_load_config_patch.install();
    dedicated_server_load_post_map_patch.install();

    // handle weapon dropping and infinite magazines
    entity_drop_weapon_patch.install();
    entity_reload_current_primary_patch.install();

    // handle weapon stay exemptions
    weapon_stay_remove_instance_injection.install();
    weapon_stay_allow_pickup_injection.install();

    // Apply customized spawn protection duration
    spawn_protection_duration_patch.install();

    // Critical hits and hit sounds
    entity_damage_hook.install();

    // Item replacements
    item_lookup_type_hook.install();

    // Kick players when server is about to switch to a map they cannot load,
    // so they don't spend time autodownloading it only to be told they can't play
    multi_limbo_leave_pre_patch.install();

    // Default player weapon class and ammo override
    find_default_weapon_for_entity_hook.install();
    give_default_weapon_ammo_hook.install();

    init_server_commands();

    // Remove level prefix restriction (dm/ctf) for 'level' command and dedicated_server.txt
    AsmWriter(0x004350FE).nop(2);
    AsmWriter(0x0046E179).nop(2);

    // In Multi -> Create game fix level filtering so 'pdm' and 'pctf' is supported
    multi_is_level_matching_game_type_hook.install();

    // Support loadouts in ads
    player_create_entity_default_weapon_injection.install();

    // Allow disabling mod name announcement
    get_mod_name_require_client_mod_hook.install();

    // Fix items not being respawned after time in ms wraps around (~25 days)
    AsmWriter(0x004599DB).nop(2);

    // Fix sending ping packets after time in ms wraps around (~25 days)
    send_ping_time_wrap_fix.install();

    // Ignore obj_update position for some time after teleportation
    process_obj_update_set_pos_injection.install();

    // Customized dedicated server console message when player joins
    multi_on_new_player_injection.install();
    AsmWriter(0x0047B061, 0x0047B064).add(asm_regs::esp, 0x14);

    // respawn point selection logic
    multi_respawn_level_init_hook.install();
    multi_respawn_get_next_point_hook.install();
    item_create_hook.install(); // also used for respawn time overrides

    // Support forcing player character
    multi_spawn_player_server_side_hook.install();

    // Hook lag compensation functions to calculate accuracy only for weapons with bullets
    // Note: weapons with bullets (projectiles) are created twice server-side so hooking weapon_create would
    // be problematic (PF went this way and its accuracy stat is broken)
    multi_lag_comp_handle_hit_hook.install();
    multi_lag_comp_weapon_fire_hook.install();

    // Set lower bound of server max players clamp range to 1 (instead of 2)
    write_mem<i8>(0x0046DD4F + 1, 1);

    // Shuffle rotation when the last map in the list is loaded
    multi_level_init_injection.install();

    // Balance teams when server transitions from a non-team mode to a team mode
    multi_balance_teams_injection.install();
    AsmWriter(0x00482157).jmp(0x0048215D); // ignore balance teams netgame flag in stock code

    // Check if round is finished or if overtime should begin
    multi_check_for_round_end_hook.install();

    // initialize -ads and -min switches
    get_ads_cmd_line_param();
    get_min_cmd_line_param();
    get_log_cmd_line_param();

    // console commands
    sv_game_type_cmd.register_cmd();
    alpine_restrict_status_cmd.register_cmd();
    gt_cmd.register_cmd();
}

void server_do_frame()
{
    server_vote_do_frame();
    match_do_frame();
    process_delayed_kicks();
}

void server_on_limbo_state_enter()
{
    g_is_overtime = false;
    g_prev_level = rf::level.filename.c_str();
    server_vote_on_limbo_state_enter();

    auto player_list = SinglyLinkedList{rf::player_list};

    const int ver = get_level_file_version(rf::level_filename_to_load);

    apply_game_type_for_current_level();
    koth_force_broadcast_all_hill_states();
    af_send_server_info_packet_to_all();

    // Clear save data for all players
    for (auto& player : player_list) {
        update_player_active_status(&player);

        auto& pdata = get_player_additional_data(&player);
        pdata.saves.clear();
        pdata.last_teleport_timestamp.invalidate();
        if (g_alpine_server_config.stats_message_enabled) {
            send_private_message_with_stats(&player);
        }

        if (&player != rf::local_player) {
            if (ver > pdata.max_rfl_version && pdata.client_version != ClientVersion::browser) {
                notify_for_upcoming_level_version_incompatible(&player);
            }
            else if (get_upcoming_game_type() != rf::netgame.type &&
                !(pdata.client_version == ClientVersion::alpine_faction && pdata.client_version_minor >= 2) &&
                pdata.client_version != ClientVersion::browser) {
                // only notify them if they CAN load the map, otherwise they can't play anyway so no point
                notify_for_client_incompatible_with_switching_game_type(&player);
            }
        }
    }

    if (get_upcoming_game_type() != rf::netgame.type) {
        std::string gt_swap_notif = std::format("\xA6 Game type will switch to {} for the next level.",
            get_game_type_string(get_upcoming_game_type()));
        af_broadcast_automated_chat_msg(gt_swap_notif);
    }
}

bool server_is_saving_enabled()
{
    return g_alpine_server_config_active_rules.saving_enabled;
}

bool server_allow_fullbright_meshes()
{
    return g_alpine_server_config.allow_fullbright_meshes;
}

bool server_allow_lightmaps_only()
{
    return g_alpine_server_config.allow_lightmaps_only;
}

bool server_allow_disable_screenshake()
{
    return g_alpine_server_config.allow_disable_screenshake;
}

bool server_no_player_collide()
{
    return g_alpine_server_config_active_rules.no_player_collide;
}

bool server_location_pinging()
{
    return g_alpine_server_config_active_rules.location_pinging;
}

bool server_delayed_spawns()
{
    return g_alpine_server_config_active_rules.spawn_delay.enabled;
}

bool server_allow_disable_muzzle_flash()
{
    return g_alpine_server_config.allow_disable_muzzle_flash;
}

bool server_apply_click_limiter()
{
    return g_alpine_server_config.click_limiter_config.enabled;
}

bool server_allow_unlimited_fps()
{
    return g_alpine_server_config.allow_unlimited_fps;
}

bool server_gaussian_spread()
{
    return g_alpine_server_config.gaussian_spread;
}

std::tuple<bool, int, bool> server_features_require_alpine_client()
{
    bool requires_alpine = false; // alpine required to spawn
    int min_minor_version = 0;    // minimum alpine minor version required
    bool hard_reject = false;     // reject non-alpine clients outright, also decides if they will be kicked on map change

    if (g_alpine_server_config.vote_match.enabled) {
        requires_alpine = true;
        min_minor_version = std::max(min_minor_version, 1);
    }

    if (g_alpine_server_config_active_rules.spawn_loadout.loadouts_active ||
        g_alpine_server_config_active_rules.no_player_collide ||
        g_alpine_server_config_active_rules.location_pinging) {
        requires_alpine = true;
        min_minor_version = std::max(min_minor_version, 2);
    }

    if (static_cast<int>(g_alpine_server_config_active_rules.game_type) >= 3) {
        requires_alpine = true;
        hard_reject = true;
        min_minor_version = std::max(min_minor_version, 2);
    }

    return {requires_alpine, min_minor_version, hard_reject};
}

bool server_weapon_items_give_full_ammo()
{
    return g_alpine_server_config_active_rules.weapon_items_give_full_ammo;
}

const AlpineServerConfig& server_get_alpine_config()
{
    return g_alpine_server_config;
}

bool server_is_modded()
{
    return !g_alpine_server_config.require_client_mod && rf::mod_param.found();
}

bool server_is_match_mode_enabled()
{
    return g_alpine_server_config.vote_match.enabled;
}

bool server_is_alpine_only_enabled()
{
    const auto [auto_require_alpine, min_ver, hard_reject] = server_features_require_alpine_client();
    return g_alpine_server_config.alpine_restricted_config.clients_require_alpine || auto_require_alpine;
}

bool server_rejects_legacy_clients()
{
    const auto [auto_require_alpine, min_ver, hard_reject] = server_features_require_alpine_client();
    return g_alpine_server_config.alpine_restricted_config.reject_non_alpine_clients || hard_reject;
}

bool server_enforces_click_limiter()
{
    return g_alpine_server_config.click_limiter_config.enabled;
}

bool server_enforces_no_player_collide()
{
    return g_alpine_server_config_active_rules.no_player_collide;
}

bool server_has_damage_notifications()
{
    return g_alpine_server_config.damage_notification_config.enabled;
}

const AFGameInfoFlags& server_get_game_info_flags()
{
    initialize_game_info_server_flags();
    return g_game_info_server_flags;
}

void initialize_game_info_server_flags()
{
    g_game_info_server_flags.modded_server = server_is_modded();
    g_game_info_server_flags.alpine_only = server_is_alpine_only_enabled();
    g_game_info_server_flags.reject_legacy_clients = server_rejects_legacy_clients();
    g_game_info_server_flags.click_limiter = server_enforces_click_limiter();
    g_game_info_server_flags.no_player_collide = server_enforces_no_player_collide();
    g_game_info_server_flags.match_mode = server_is_match_mode_enabled();
    g_game_info_server_flags.saving_enabled = server_is_saving_enabled();
    g_game_info_server_flags.gaussian_spread = server_gaussian_spread();
    g_game_info_server_flags.damage_notifications = server_has_damage_notifications();
}
