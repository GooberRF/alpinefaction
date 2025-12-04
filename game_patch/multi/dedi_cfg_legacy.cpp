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
#include <windows.h>
#include <winsock2.h>
#include "server.h"
#include "server_internal.h"
#include "alpine_packets.h"
#include "multi.h"
#include "../os/console.h"
#include "../misc/player.h"
#include "../main/main.h"
#include "../misc/achievements.h"
#include "../rf/file/file.h"
#include "../rf/math/vector.h"
#include "../rf/math/matrix.h"
#include "../rf/player/player.h"
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

// server config parsing helper functions
void parse_boolean_option(rf::Parser& parser, const char* key, bool& option, const char* label = nullptr)
{
    if (parser.parse_optional(key)) {
        option = parser.parse_bool();
        if (label) {
            rf::console::print("{}: {}", label, option ? "true" : "false");
        }
    }
}

void parse_uint_option(rf::Parser& parser, const char* key, unsigned int& option, const char* label = nullptr)
{
    if (parser.parse_optional(key)) {
        option = parser.parse_uint();
        if (label) {
            rf::console::print("{}: {}", label, option);
        }
    }
}

void parse_int_option(rf::Parser& parser, const char* key, int& option, const char* label = nullptr)
{
    if (parser.parse_optional(key)) {
        option = parser.parse_int();
        if (label) {
            rf::console::print("{}: {}", label, option);
        }
    }
}

void parse_float_option(rf::Parser& parser, const char* key, float& option, const char* label = nullptr)
{
    if (parser.parse_optional(key)) {
        option = parser.parse_float();
        if (label) {
            rf::console::print("{}: {}", label, option);
        }
    }
}

void parse_vote_config(const char* vote_name, VoteConfig& config, rf::Parser& parser)
{
    std::string vote_option_name = std::format("{}:", vote_name);
    if (parser.parse_optional(vote_option_name.c_str())) {
        config.enabled = parser.parse_bool();
        rf::console::print("{}: {}", vote_name, config.enabled ? "true" : "false");

        if (parser.parse_optional("+Time Limit:")) {
            config.time_limit_seconds = parser.parse_uint();
            rf::console::print("{}: {}", "+Time Limit", config.time_limit_seconds);
        }
    }
}

void parse_spawn_protection(rf::Parser& parser)
{
    if (parser.parse_optional("$Spawn Protection Enabled:")) {
        g_alpine_server_config.base_rules.spawn_protection.enabled = parser.parse_bool();
        rf::console::print("Spawn Protection: {}",
                           g_alpine_server_config.base_rules.spawn_protection.enabled ? "true" : "false");

        parse_uint_option(parser, "+Duration:", g_alpine_server_config.base_rules.spawn_protection.duration, "+Duration");
        parse_boolean_option(parser, "+Use Powerup:", g_alpine_server_config.base_rules.spawn_protection.use_powerup,
                             "+Use Powerup");
    }
}

void parse_respawn_logic(rf::Parser& parser)
{
    if (parser.parse_optional("$Player Respawn Logic")) {
        rf::console::print("Parsing Player Respawn Logic...");

        parse_boolean_option(parser,
                             "+Respect Team Spawns:", g_alpine_server_config_active_rules.spawn_logic.respect_team_spawns,
                             "+Respect Team Spawns");
        parse_boolean_option(parser,
                             "+Prefer Avoid Players:", g_alpine_server_config_active_rules.spawn_logic.try_avoid_players,
                             "+Prefer Avoid Players");
        parse_boolean_option(parser,
                             "+Always Avoid Last:", g_alpine_server_config_active_rules.spawn_logic.always_avoid_last,
                             "+Always Avoid Last");
        parse_boolean_option(parser,
                             "+Always Use Furthest:", g_alpine_server_config_active_rules.spawn_logic.always_use_furthest,
                             "+Always Use Furthest");
        parse_boolean_option(parser,
                             "+Only Avoid Enemies:", g_alpine_server_config_active_rules.spawn_logic.only_avoid_enemies,
                             "+Only Avoid Enemies");
    }
}

void parse_damage_notifications(rf::Parser& parser)
{
    if (parser.parse_optional("$Damage Notifications:")) {
        g_alpine_server_config.damage_notification_config.enabled = parser.parse_bool();
        rf::console::print("Damage Notifications Enabled: {}",
                           g_alpine_server_config.damage_notification_config.enabled ? "true" : "false");

        parse_boolean_option(parser, "+Legacy Client Compatibility:",
                             g_alpine_server_config.damage_notification_config.support_legacy_clients,
                             "+Legacy Client Compatibility");
    }
}

void parse_critical_hits(rf::Parser& parser)
{
    if (parser.parse_optional("$Critical Hits:")) {
        g_alpine_server_config_active_rules.critical_hits.enabled = parser.parse_bool();
        rf::console::print("Critical Hits Enabled: {}",
                           g_alpine_server_config_active_rules.critical_hits.enabled ? "true" : "false");

        parse_uint_option(parser, "+Reward Duration:", g_alpine_server_config_active_rules.critical_hits.reward_duration,
                          "+Reward Duration");
        parse_float_option(parser, "+Base Chance Percent:", g_alpine_server_config_active_rules.critical_hits.base_chance,
                           "+Base Chance");
        parse_boolean_option(parser,
                             "+Use Dynamic Chance Bonus:", g_alpine_server_config_active_rules.critical_hits.dynamic_scale,
                             "+Dynamic Scaling");
        parse_float_option(parser, "+Dynamic Chance Damage Ceiling:",
                           g_alpine_server_config_active_rules.critical_hits.dynamic_damage_bonus_ceiling,
                           "+Dynamic Damage Ceiling");
    }
}

void parse_weapon_ammo_settings(rf::Parser& parser)
{
    if (parser.parse_optional("$Weapon Items Give Full Ammo:")) {
        g_alpine_server_config_active_rules.weapon_items_give_full_ammo = parser.parse_bool();
        rf::console::print("Weapon Items Give Full Ammo: {}",
                           g_alpine_server_config_active_rules.weapon_items_give_full_ammo ? "true" : "false");

        parse_boolean_option(parser, "+Infinite Magazines:", g_alpine_server_config_active_rules.weapon_infinite_magazines,
                             "+Infinite Magazines");
    }
}

void parse_default_player_weapon(rf::Parser& parser)
{
    if (parser.parse_optional("$Default Player Weapon:")) {
        rf::String default_weapon;
        parser.parse_string(&default_weapon);
        g_alpine_server_config_active_rules.default_player_weapon.set_weapon(default_weapon.c_str());
        rf::console::print("Default Player Weapon: {}", g_alpine_server_config_active_rules.default_player_weapon.weapon_name);
    }
}

void parse_force_character(rf::Parser& parser)
{
    if (parser.parse_optional("$Force Player Character:")) {
        rf::String character_name;
        parser.parse_string(&character_name);
        int character_num = rf::multi_find_character(character_name.c_str());
        if (character_num != -1) {
            g_alpine_server_config_active_rules.force_character.set_character(character_name.c_str());
            g_alpine_server_config_active_rules.force_character.enabled = true;
            rf::console::print("Forced Player Character: {} (ID {})", character_name, g_alpine_server_config_active_rules.force_character.character_index);
        }
        else {
            xlog::warn("Unknown character name in Force Player Character setting: {}", character_name);
        }
    }
}

void parse_kill_rewards(rf::Parser& parser)
{
    if (parser.parse_optional("$Kill Reward")) {
        rf::console::print("Parsing Kill Rewards...");
        parse_float_option(parser, "+Effective Health:", g_alpine_server_config_active_rules.kill_rewards.kill_reward_effective_health,
                           "Kill Reward: Effective Health");
        parse_float_option(parser, "+Health:", g_alpine_server_config_active_rules.kill_rewards.kill_reward_health, "Kill Reward: Health");
        parse_float_option(parser, "+Armor:", g_alpine_server_config_active_rules.kill_rewards.kill_reward_armor, "Kill Reward: Armor");
        parse_boolean_option(parser, "+Health Is Super:", g_alpine_server_config_active_rules.kill_rewards.kill_reward_health_super,
                             "Kill Reward: Health Is Super");
        parse_boolean_option(parser, "+Armor Is Super:", g_alpine_server_config_active_rules.kill_rewards.kill_reward_armor_super,
                             "Kill Reward: Armor Is Super");
    }
}

void parse_miscellaneous_options(rf::Parser& parser)
{
    parse_int_option(parser, "$Desired Player Count:", g_alpine_server_config_active_rules.ideal_player_count,
                     "Desired Player Count");
    parse_boolean_option(parser, "$Use SP Damage Calculation:", g_alpine_server_config.use_sp_damage_calculation,
                         "Use SP Damage Calculation");
    parse_int_option(parser, "$CTF Flag Return Time:", g_alpine_server_config_active_rules.ctf_flag_return_time_ms,
                     "CTF Flag Return Time");
    parse_boolean_option(parser, "$Dynamic Rotation:", g_alpine_server_config.dynamic_rotation, "Dynamic Rotation");
    parse_boolean_option(parser, "$Require Client Mod:", g_alpine_server_config.require_client_mod,
                         "Clients Require Mod");
    parse_float_option(parser, "$Player Damage Modifier:", g_alpine_server_config_active_rules.pvp_damage_modifier,
                       "Player Damage Modifier");
    parse_boolean_option(parser, "$UPnP Enabled:", g_alpine_server_config.upnp_enabled, "UPnP Enabled");
    parse_boolean_option(parser, "$Send Player Stats Message:", g_alpine_server_config.stats_message_enabled,
                         "Send Player Stats Message");
    parse_boolean_option(parser, "$Drop Amps On Death:", g_alpine_server_config_active_rules.drop_amps, "Drop Amps On Death");
    parse_boolean_option(parser, "$Flag Dropping:", g_alpine_server_config_active_rules.flag_dropping, "Flag Dropping");
    parse_boolean_option(parser, "$Flag Captures While Stolen:", g_alpine_server_config_active_rules.flag_captures_while_stolen,
                         "Flag Captures While Stolen");
    parse_boolean_option(parser, "$Saving Enabled:", g_alpine_server_config_active_rules.saving_enabled, "Saving Enabled");
    parse_boolean_option(parser, "$Allow Fullbright Meshes:", g_alpine_server_config.allow_fullbright_meshes,
                         "Allow Fullbright Meshes");
    parse_boolean_option(parser, "$Allow Lightmaps Only Mode:", g_alpine_server_config.allow_lightmaps_only,
                         "Allow Lightmaps Only Mode");
    parse_boolean_option(parser, "$Allow Disable Screenshake:", g_alpine_server_config.allow_disable_screenshake,
                         "Allow Disable Screenshake");
    parse_boolean_option(parser,
                         "$Allow Disable Muzzle Flash Lights:", g_alpine_server_config.allow_disable_muzzle_flash,
                         "Allow Disable Muzzle Flash Lights");
    parse_boolean_option(parser, "$Allow Client Unlimited FPS:", g_alpine_server_config.allow_unlimited_fps,
                         "Allow Client Unlimited FPS");
}

void parse_alpine_locking(rf::Parser& parser)
{
    parse_boolean_option(parser, "$Advertise Alpine Faction:", g_alpine_server_config.alpine_restricted_config.advertise_alpine,
                         "Advertise Alpine Faction");
    if (parser.parse_optional("$Clients Require Alpine Faction:")) {
        g_alpine_server_config.alpine_restricted_config.clients_require_alpine = parser.parse_bool();
        rf::console::print("Clients Require Alpine Faction: {}",
                           g_alpine_server_config.alpine_restricted_config.clients_require_alpine ? "true" : "false");

        parse_boolean_option(parser, "+Require Official Build:",
                             g_alpine_server_config.alpine_restricted_config.alpine_require_release_build,
                             "+Require Official Build");
        parse_boolean_option(parser, "+Reject Legacy Clients:",
                             g_alpine_server_config.alpine_restricted_config.reject_non_alpine_clients,
                             "+Reject Legacy Clients");
    }
}

void parse_item_respawn_time_override(rf::Parser& parser)
{
    while (parser.parse_optional("$Item Respawn Time Override:")) {
        rf::String item_name;
        parser.parse_string(&item_name);
        auto new_time = parser.parse_uint();
        g_alpine_server_config_active_rules.item_respawn_time_overrides[item_name.c_str()] = new_time;
        rf::console::print("Item Respawn Time Override: {} -> {}ms", item_name.c_str(), new_time);
    }
}

void parse_item_replacements(rf::Parser& parser)
{
    while (parser.parse_optional("$Item Replacement:")) {
        rf::String old_item, new_item;
        parser.parse_string(&old_item);
        parser.parse_string(&new_item);
        g_alpine_server_config_active_rules.item_replacements[old_item.c_str()] = new_item.c_str();
        rf::console::print("Item Replaced: {} -> {}", old_item.c_str(), new_item.c_str());
    }
}

void load_additional_server_config(rf::Parser& parser)
{
    // Vote config
    parse_vote_config("$Vote Kick", g_alpine_server_config.vote_kick, parser);
    parse_vote_config("$Vote Level", g_alpine_server_config.vote_level, parser);
    parse_vote_config("$Vote Extend", g_alpine_server_config.vote_extend, parser);
    parse_vote_config("$Vote Restart", g_alpine_server_config.vote_restart, parser);
    parse_vote_config("$Vote Next", g_alpine_server_config.vote_next, parser);
    parse_vote_config("$Vote Random", g_alpine_server_config.vote_rand, parser);
    parse_vote_config("$Vote Previous", g_alpine_server_config.vote_previous, parser);

    // Core config
    parse_spawn_protection(parser);
    parse_respawn_logic(parser);
    parse_damage_notifications(parser);
    parse_critical_hits(parser);
    parse_weapon_ammo_settings(parser);
    parse_default_player_weapon(parser);
    parse_force_character(parser);
    parse_kill_rewards(parser);

    // Misc config
    parse_miscellaneous_options(parser);
    parse_alpine_locking(parser);

    parse_boolean_option(parser, "$Use Gaussian Bullet Spread:", g_alpine_server_config.gaussian_spread,
                         "Use Gaussian Bullet Spread");
    if (parser.parse_optional("$Enforce Semi Auto Fire Rate Limit:")) {
        g_alpine_server_config.click_limiter_config.enabled = parser.parse_bool();
        rf::console::print("Enforce Semi Auto Fire Rate Limit: {}",
                           g_alpine_server_config.click_limiter_config.enabled ? "true" : "false");
        if (parser.parse_optional("+Cooldown:")) {
            int fire_wait = parser.parse_int();
            g_alpine_server_config.click_limiter_config.cooldown = {fire_wait};
            rf::console::print("+Cooldown: {}", g_alpine_server_config.click_limiter_config.cooldown);
        }
    }

    // Repeatable config
    parse_item_respawn_time_override(parser);
    parse_item_replacements(parser);
}
