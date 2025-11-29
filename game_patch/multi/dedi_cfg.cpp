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
#include <filesystem>
#include <limits>
#include <format>
#include <sstream>
#include <numeric>
#include <string_view>
#include <vector>
#include <windows.h>
#include <winsock2.h>
#include <toml++/toml.hpp>
#include "server.h"
#include "server_internal.h"
#include "alpine_packets.h"
#include "multi.h"
#include "../os/console.h"
#include "../misc/player.h"
#include "../misc/alpine_settings.h"
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

bool g_dedicated_launched_from_ads = false; // was the server launched from an ads file?
std::string g_ads_config_name = "";
bool g_ads_minimal_server_info = false;     // print only minimal server info when launching
bool g_ads_full_console_log = false;        // log full console output to file
int g_ads_loaded_version = ADS_VERSION;

rf::CmdLineParam& get_ads_cmd_line_param()
{
    static rf::CmdLineParam ads_param{"-ads", "", true};
    return ads_param;
}

// print only minimal server info when launching
rf::CmdLineParam& get_min_cmd_line_param()
{
    static rf::CmdLineParam min_param{"-min", "", false};
    return min_param;
}

// print full server console log to file
rf::CmdLineParam& get_log_cmd_line_param()
{
    static rf::CmdLineParam log_param{"-log", "", false};
    return log_param;
}

void handle_min_param()
{
    g_ads_minimal_server_info = get_min_cmd_line_param().found();
}

void handle_log_param()
{
    g_ads_full_console_log = get_log_cmd_line_param().found();
}

static OvertimeConfig parse_overtime_config(const toml::table& t)
{
    OvertimeConfig v;
    if (auto x = t["enabled"].value<bool>())
        v.enabled = *x;

    if (v.enabled) {
        if (auto x = t["time"].value<int>())
            v.set_additional_time(*x);
        if (auto x = t["tie_when_flag_stolen"].value<bool>())
            v.consider_tie_if_flag_stolen = *x;
        if (auto x = t["tie_when_hill_contested"].value<bool>())
            v.consider_tie_if_hill_contested = *x;
    }
    return v;
}

static DefaultPlayerWeaponConfig parse_default_player_weapon(const toml::table& t, DefaultPlayerWeaponConfig c)
{
    if (auto x = t["weapon_name"].value<std::string>()) {
        c.set_weapon(*x);
    }

    if (c.index >= 0) {
        if (auto v = t["clips"].value<int>())
            c.num_clips = *v;
    }

    return c;
}

static SpawnLifeConfig parse_spawn_life_config(const toml::table& t, SpawnLifeConfig c)
{
    if (auto x = t["enabled"].value<bool>())
        c.enabled = *x;

    if (c.enabled) {
        if (auto v = t["value"].value<float>())
            c.set_value(*v);
    }

    return c;
}

static SpawnDelayConfig parse_spawn_delay_config(const toml::table& t, SpawnDelayConfig c)
{
    if (auto x = t["enabled"].value<bool>())
        c.enabled = *x;

    if (c.enabled) {
        if (auto v = t["base_seconds"].value<float>())
            c.set_base_value(*v);
    }

    return c;
}


static ForceCharacterConfig parse_force_character_config(const toml::table& t, ForceCharacterConfig c)
{
    if (auto x = t["enabled"].value<bool>())
        c.enabled = *x;

    if (c.enabled) {
        if (auto v = t["character"].value<std::string>())
            c.set_character(*v);
    }

    return c;
}

static CriticalHitsConfig parse_critical_hits_config(const toml::table& t, CriticalHitsConfig c)
{
    if (auto x = t["enabled"].value<bool>())
        c.enabled = *x;

    if (c.enabled) {
        if (auto v = t["reward_duration"].value<int>())
            c.set_reward_duration(*v);
        if (auto v = t["base_chance"].value<float>())
            c.set_base_chance(*v);
        if (auto v = t["dynamic_scale"].value<float>())
            c.dynamic_scale = *v;
        if (auto v = t["dynamic_damage_bonus_ceiling"].value<float>())
            c.set_damage_bonus_ceiling(*v);
    }

    return c;
}

static GunGameConfig parse_gungame_config(const toml::table& t, GunGameConfig gg)
{
    if (auto v = t["enabled"].value<bool>())
        gg.enabled = *v;
    if (auto v = t["dynamic_progression"].value<bool>())
        gg.dynamic_progression = *v;
    if (auto v = t["rampage_rewards"].value<bool>())
        gg.rampage_rewards = *v;

    if (auto ft = t["final"].as_table()) {
        int kills = (*ft)["kills"].value_or<int>(-1);
        auto w = (*ft)["weapon"].value_or<std::string>("");
        if (kills >= 0 && !w.empty())
            gg.set_final_level(kills, w);
        else
            xlog::warn("GunGame: [final] requires kills>=0 and weapon");
    }

    if (auto arr = t["levels"].as_array()) {
        gg.levels.clear(); // levels array specified for this map. Otherwise, inherit base

        int prev_kills = 0;
        for (auto& node : *arr) {
            auto tbl = node.as_table();
            if (!tbl) {
                xlog::warn("GunGame: level is not a table; skipping");
                continue;
            }

            auto w = (*tbl)["weapon"].value_or<std::string>("");
            if (w.empty()) {
                xlog::warn("GunGame: level missing 'weapon'");
                continue;
            }

            if (gg.dynamic_progression) {
                int tier = (*tbl)["tier"].value_or<int>(-1);
                if (tier < 1) {
                    xlog::warn("GunGame: dynamic requires 'tier' >= 1");
                    continue;
                }
                gg.add_level_by_tier(tier, w);
            }
            else {
                int kills = (*tbl)["kills"].value_or<int>(prev_kills + 1);
                gg.add_level_by_kills(kills, w);
                prev_kills = kills;
            }
        }

        if (!gg.dynamic_progression)
            gg.normalize_manual(); // dedupe kill levels
    }

    return gg;
}

static WelcomeMessageConfig parse_welcome_message_config(const toml::table& t, WelcomeMessageConfig c)
{
    if (auto x = t["enabled"].value<bool>())
        c.enabled = *x;

    if (c.enabled) {
        if (auto v = t["text"].value<std::string>())
            c.set_welcome_message(*v);
    }

    return c;
}

static SpawnProtectionConfig parse_spawn_protection_config(const toml::table& t, SpawnProtectionConfig c)
{
    if (auto x = t["enabled"].value<bool>())
        c.enabled = *x;

    if (c.enabled) {
        if (auto v = t["duration"].value<float>())
            c.set_duration(*v);
        if (auto v = t["use_powerup"].value<bool>())
            c.use_powerup = *v;
    }

    return c;
}

static NewSpawnLogicConfig parse_spawn_logic_config(const toml::table& t, NewSpawnLogicConfig c)
{
    if (auto x = t["respect_team_spawns"].value<bool>())
        c.respect_team_spawns = *x;
    if (auto x = t["try_avoid_players"].value<bool>())
        c.try_avoid_players = *x;
    if (auto x = t["always_avoid_last"].value<bool>())
        c.always_avoid_last = *x;
    if (auto x = t["always_use_furthest"].value<bool>())
        c.always_use_furthest = *x;
    if (auto x = t["only_avoid_enemies"].value<bool>())
        c.only_avoid_enemies = *x;
    if (auto x = t["dynamic_respawns"].value<bool>())
        c.dynamic_respawns = *x;

    if (c.dynamic_respawns) {
        c.clear_dynamic_respawn_items();

        if (auto arr = t["dynamic_respawn_items"].as_array()) {
            for (auto& elem : *arr) {
                if (auto sub = elem.as_table()) {
                    auto name = (*sub)["item_name"].value_or<std::string>("");
                    int pts = (*sub)["min_respawn_points"].value_or<int>(8);

                    if (!c.add_dynamic_respawn_item(name, pts)) {
                        xlog::warn("Invalid dynamic_respawn_items entry: item_name='{}'", name);
                    }
                }
            }
        }
    }

    return c;
}

static KillRewardConfig parse_kill_reward_config(const toml::table& t, KillRewardConfig c)
{
    if (auto x = t["health"].value<float>())
        c.kill_reward_health = *x;
    if (auto x = t["armor"].value<float>())
        c.kill_reward_armor = *x;
    if (auto x = t["effective_health"].value<float>())
        c.kill_reward_effective_health = *x;
    if (auto x = t["health_is_super"].value<bool>())
        c.kill_reward_health_super = *x;
    if (auto x = t["armor_is_super"].value<bool>())
        c.kill_reward_armor_super = *x;

    return c;
}

void apply_defaults_for_game_type(rf::NetGameType game_type, AlpineServerConfigRules& rules)
{
    // all modes get baton
    int baton_ammo = rf::weapon_types[rf::riot_stick_weapon_type].clip_size_multi;
    rules.spawn_loadout.add("Riot Stick", baton_ammo, false, true);
    rules.set_pvp_damage_modifier(1.0f);
    rules.no_player_collide = false;
    rules.location_pinging = false;
    rules.saving_enabled = false;

    switch (game_type) {
        case rf::NetGameType::NG_TYPE_KOTH: {
            rules.spawn_delay.enabled = true;
            rules.spawn_delay.set_base_value(5.0f);
            rules.location_pinging = true;

            // secondary weapon
            rules.spawn_loadout.add("Remote Charge", 3, false, true);

            // primary weapon
            rules.spawn_loadout.remove("12mm handgun", false);
            rules.default_player_weapon.set_weapon("Machine Pistol");

            rules.spawn_loadout.loadouts_active = true;
            break;
        }

        case rf::NetGameType::NG_TYPE_DC: {
            rules.spawn_delay.enabled = true;
            rules.spawn_delay.set_base_value(2.5f);
            rules.location_pinging = true;

            // primary weapon
            rules.default_player_weapon.set_weapon("12mm handgun");

            rules.spawn_loadout.loadouts_active = false;
            break;
        }

        case rf::NetGameType::NG_TYPE_REV: {
            rules.spawn_delay.enabled = true;
            rules.spawn_delay.set_base_value(2.0f);
            rules.location_pinging = true;

            // secondary weapon
            rules.spawn_loadout.add("Remote Charge", 3, false, true);

            // primary weapon
            rules.spawn_loadout.remove("12mm handgun", false);
            rules.default_player_weapon.set_weapon("Machine Pistol");

            rules.spawn_loadout.loadouts_active = true;
            break;
        }

        case rf::NetGameType::NG_TYPE_ESC: {
            rules.spawn_delay.enabled = true;
            rules.spawn_delay.set_base_value(3.0f);
            rules.location_pinging = true;

            // secondary weapon
            rules.spawn_loadout.add("Remote Charge", 3, false, true);

            // primary weapon
            rules.spawn_loadout.remove("12mm handgun", false);
            rules.default_player_weapon.set_weapon("Machine Pistol");

            rules.spawn_loadout.loadouts_active = true;
            break;
        }

        case rf::NetGameType::NG_TYPE_RUN: {
            rules.set_pvp_damage_modifier(0.0f);
            rules.no_player_collide = true;
            rules.location_pinging = true;
            rules.saving_enabled = true;

            rules.spawn_delay.enabled = false;

            // primary weapon
            rules.default_player_weapon.set_weapon("12mm handgun");

            rules.spawn_loadout.loadouts_active = false;
            break;
        }

        default: {
            rules.spawn_delay.enabled = false;

            // primary weapon
            rules.default_player_weapon.set_weapon("12mm handgun");

            rules.spawn_loadout.loadouts_active = false;
            break;
        }
    }

    // handle default player weapon
    if (rules.default_player_weapon.index >= 0) {
        int default_ammo = rf::weapon_types[rules.default_player_weapon.index].clip_size_multi * rules.default_player_weapon.num_clips;
        rules.spawn_loadout.add(rules.default_player_weapon.weapon_name, default_ammo, false, true);
    }
}

// parse toml rules
// for base rules, load all speciifed. For not specified, defaults are in struct
// for level-specific rules, start with base rules and load anything specified beyond that
AlpineServerConfigRules parse_server_rules(const toml::table& t, const AlpineServerConfigRules& base_rules)
{
    AlpineServerConfigRules o = base_rules;

    rf::NetGameType resolved_game_type = o.game_type;
    if (auto v = t["game_type"].value<std::string>()) {
        auto gt_opt = resolve_gametype_from_name(*v);
        resolved_game_type = gt_opt.has_value() ? gt_opt.value() : rf::NetGameType::NG_TYPE_DM;
    }

    const bool game_type_changed = resolved_game_type != base_rules.game_type;

    o.game_type = resolved_game_type;

    if (game_type_changed)
        apply_defaults_for_game_type(o.game_type, o);

    if (auto v = t["time_limit"].value<float>())
        o.set_time_limit(*v);
    if (auto v = t["overtime"].as_table())
        o.overtime = parse_overtime_config(*v);

    if (auto v = t["individual_kill_limit"].value<int>())
        o.set_individual_kill_limit(*v);
    if (auto v = t["team_kill_limit"].value<int>())
        o.set_team_kill_limit(*v);
    if (auto v = t["cap_limit"].value<int>())
        o.set_cap_limit(*v);
    if (auto v = t["koth_score_limit"].value<int>())
        o.set_koth_score_limit(*v);
    if (auto v = t["dc_score_limit"].value<int>())
        o.set_dc_score_limit(*v);
    if (auto v = t["geo_limit"].value<int>())
        o.set_geo_limit(*v);

    if (auto v = t["team_damage"].value<bool>())
        o.team_damage   = *v;
    if (auto v = t["fall_damage"].value<bool>())
        o.fall_damage   = *v;
    if (auto v = t["weapons_stay"].value<bool>())
        o.weapons_stay  = *v;
    if (auto v = t["force_respawn"].value<bool>())
        o.force_respawn = *v;
    if (auto v = t["balance_teams"].value<bool>())
        o.balance_teams = *v;
    if (auto v = t["ideal_player_count"].value<int>())
        o.set_ideal_player_count(*v);
    if (auto v = t["saving_enabled"].value<bool>())
        o.saving_enabled = *v;
    if (auto v = t["flag_dropping"].value<bool>())
        o.flag_dropping = *v;
    if (auto v = t["flag_captures_while_stolen"].value<bool>())
        o.flag_captures_while_stolen = *v;
    if (auto v = t["flag_return_time"].value<int>())
        o.set_flag_return_time(*v);
    if (auto v = t["pvp_damage_modifier"].value<float>())
        o.set_pvp_damage_modifier(*v);
    if (auto v = t["drop_amps"].value<bool>())
        o.drop_amps = *v;
    if (auto v = t["no_player_collide"].value<bool>())
        o.no_player_collide = *v;
    if (auto v = t["location_pinging"].value<bool>())
        o.location_pinging = *v;
    if (auto v = t["weapon_pickups_give_full_ammo"].value<bool>())
        o.weapon_items_give_full_ammo = *v;
    if (auto v = t["infinite_reloads"].value<bool>())
        o.weapon_infinite_magazines = *v;
    if (auto v = t["drop_weapons"].value<bool>())
        o.drop_weapons = *v;

    if (auto sub = t["spawn_weapon"].as_table())
        o.default_player_weapon = parse_default_player_weapon(*sub, o.default_player_weapon);
    if (auto sub = t["spawn_life"].as_table())
        o.spawn_life  = parse_spawn_life_config(*sub, o.spawn_life);
    if (auto sub = t["spawn_armor"].as_table())
        o.spawn_armour = parse_spawn_life_config(*sub, o.spawn_armour);

    if (auto sub = t["spawn_delay"].as_table())
        o.spawn_delay = parse_spawn_delay_config(*sub, o.spawn_delay);

    if (auto arr = t["spawn_loadout"].as_array()) {
        for (auto& node : *arr) {
            if (auto tbl = node.as_table()) {
                if (auto nameOpt = (*tbl)["weapon_name"].value<std::string>()) {
                    int ammo = (*tbl)["ammo"].value<int>().value_or(0);
                    bool enabled = (*tbl)["include"].value<bool>().value_or(true); // default true if not specified
                    o.spawn_loadout.add(*nameOpt, ammo, false, enabled);
                    o.spawn_loadout.loadouts_active = true;
                }
            }
        }
    }

    if (auto sub = t["spawn_protection"].as_table())
        o.spawn_protection = parse_spawn_protection_config(*sub, o.spawn_protection);
    if (auto sub = t["spawn_selection"].as_table())
        o.spawn_logic = parse_spawn_logic_config(*sub, o.spawn_logic);
    if (auto sub = t["kill_rewards"].as_table())
        o.kill_rewards = parse_kill_reward_config(*sub, o.kill_rewards);
    if (auto sub = t["welcome_message"].as_table())
        o.welcome_message = parse_welcome_message_config(*sub, o.welcome_message);

    //o.weapon_stay_exemptions.exemptions.clear();
    o.weapon_stay_exemptions.add("shoulder_cannon", true); // stock default

    if (auto arr = t["weapon_stay_exemptions"].as_array()) {
        for (auto& node : *arr) {
            if (auto tbl = node.as_table()) {
                if (auto nameOpt = (*tbl)["weapon_name"].value<std::string>()) {
                    bool ex = (*tbl)["exempt"].value<bool>().value_or(true); // default true if not specified
                    o.weapon_stay_exemptions.add(*nameOpt, ex);
                }
            }
        }
    }

    if (auto arr = t["item_replacements"].as_array()) {
        for (auto& node : *arr) {
            if (auto tbl = node.as_table()) {
                auto from = (*tbl)["original"].value_or<std::string>("");
                auto to = (*tbl)["replacement"].value_or<std::string>("");
                if (!o.add_item_replacement(from, to))
                    xlog::warn("Invalid replacement {} -> {}", from, to);
            }
        }
    }

    if (auto arr = t["item_respawn_time_overrides"].as_array()) {
        for (auto& node : *arr) {
            if (auto tbl = node.as_table()) {
                auto name = (*tbl)["item_name"].value_or<std::string>("");
                auto ms = (*tbl)["respawn_ms"].value_or<int>(0);
                if (!o.set_item_respawn_time(name, ms))
                    xlog::warn("Invalid respawn override for '{}'", name);
            }
        }
    }

    if (auto sub = t["force_character"].as_table())
        o.force_character = parse_force_character_config(*sub, o.force_character);

    if (auto sub = t["critical_hits"].as_table())
        o.critical_hits = parse_critical_hits_config(*sub, o.critical_hits);

    if (auto sub = t["gungame"].as_table())
        o.gungame = parse_gungame_config(*sub, o.gungame);

    return o;
}

static VoteConfig parse_vote_config(const toml::table& t)
{
    VoteConfig v;
    if (auto x = t["enabled"].value<bool>())
        v.enabled = *x;

    if (v.enabled) {
        if (auto x = t["ignore_nonvoters"].value<bool>())
            v.ignore_nonvoters = *x;
        if (auto x = t["time"].value<float>())
            v.set_time_limit_seconds(*x);
    }
    return v;
}

static InactivityConfig parse_inactivity_config(const toml::table &t)
{
    InactivityConfig o;
    if (auto v = t["enabled"].value<bool>())
        o.enabled = *v;

    if (o.enabled) {
        if (auto v = t["new_player_grace"].value<float>())
            o.set_new_player_grace(*v);
        if (auto v = t["allowed_inactive"].value<float>())
            o.set_allowed_inactive(*v);
        if (auto v = t["warning_duration"].value<float>())
            o.set_warning_duration(*v);
        if (auto v = t["kick_message"].value<std::string>())
            o.kick_message = *v;
    }
    return o;
}

static ClickLimiterConfig parse_click_limiter_config(const toml::table& t)
{
    ClickLimiterConfig o;
    if (auto v = t["enabled"].value<bool>())
        o.enabled = *v;

    if (o.enabled) {
        if (auto v = t["cooldown"].value<int>())
            o.set_cooldown(*v);
    }
    return o;
}

static DamageNotificationConfig parse_damage_notification_config(const toml::table& t)
{
    DamageNotificationConfig o;
    if (auto v = t["enabled"].value<bool>())
        o.enabled = *v;

    if (o.enabled) {
        if (auto v = t["support_legacy"].value<bool>())
            o.support_legacy_clients = *v;
    }
    return o;
}

static AlpineRestrictConfig parse_alpine_restrict_config(const toml::table &t)
{
    AlpineRestrictConfig o;
    if (auto v = t["advertise_alpine"].value<bool>())
        o.advertise_alpine = *v;
    if (auto v = t["only_welcome_alpine"].value<bool>())
        o.only_welcome_alpine = *v;
    if (auto v = t["clients_require_alpine"].value<bool>())
        o.clients_require_alpine = *v;

    if (o.clients_require_alpine) {
        if (auto v = t["reject_non_alpine_clients"].value<bool>())
            o.reject_non_alpine_clients = *v;
        if (auto v = t["alpine_require_release_build"].value<bool>())
            o.alpine_require_release_build = *v;
    }
    return o;
}

namespace fs = std::filesystem;

static AlpineServerConfigRules apply_rules_presets_and_overrides(
    const toml::table& scope_tbl, const fs::path& base_dir, const AlpineServerConfigRules& starting_rules,
    std::string_view context, const std::map<std::string, std::filesystem::path>* preset_aliases = nullptr,
    std::vector<fs::path>* preset_stack = nullptr, std::vector<std::pair<std::filesystem::path, std::optional<std::string>>>* applied_presets = nullptr)
{
    std::vector<fs::path> local_stack;
    const bool is_root_call = !preset_stack;
    if (!preset_stack)
        preset_stack = &local_stack;

    if (is_root_call && applied_presets)
        applied_presets->clear();

    AlpineServerConfigRules rules = starting_rules;

    auto apply_preset = [&](std::string_view preset_path) {
        bool used_alias = false;
        fs::path resolved_path;

        if (preset_aliases) {
            auto alias_it = preset_aliases->find(std::string(preset_path));
            if (alias_it != preset_aliases->end()) {
                resolved_path = alias_it->second;
                used_alias = true;
            }
        }

        if (resolved_path.empty())
            resolved_path = base_dir / preset_path;

        try {
            resolved_path = fs::weakly_canonical(resolved_path);
        }
        catch (const fs::filesystem_error& err) {
            rf::console::print("  [WARN] failed to canonicalize rules preset '{}' in {}: {}\n", resolved_path.generic_string(), context, err.what()); resolved_path = fs::absolute(resolved_path);
        }

        if (std::find(preset_stack->begin(), preset_stack->end(), resolved_path) != preset_stack->end()) {
            rf::console::print("  [ERROR] rules preset cycle detected at '{}' in {}\n", resolved_path.generic_string(), context);
            return;
        }

        preset_stack->push_back(resolved_path);
        try {
            toml::table preset_root = toml::parse_file(resolved_path.generic_string());
            const toml::table* preset_rules = nullptr;

            if (auto tbl = preset_root["rules"].as_table())
                preset_rules = tbl;
            else
                preset_rules = &preset_root;

            std::string next_context = std::format("rules preset '{}'", resolved_path.generic_string());
            rules = apply_rules_presets_and_overrides(*preset_rules, resolved_path.parent_path(), rules, next_context, preset_aliases, preset_stack, applied_presets);
            if (applied_presets) {
                if (used_alias)
                    applied_presets->emplace_back(resolved_path, preset_path);
                else
                    applied_presets->emplace_back(resolved_path, std::nullopt);
            }
        }
        catch (const toml::parse_error& err) {
            rf::console::print("  [ERROR] failed to parse rules preset '{}' in {}: {}\n", resolved_path.generic_string(), context, err.description());
        }
        preset_stack->pop_back();
    };

    if (auto presets = scope_tbl["rules_presets"]) {
        if (auto arr = presets.as_array()) {
            for (auto& node : *arr) {
                if (auto preset = node.value<std::string>())
                    apply_preset(*preset);
                else
                    rf::console::print("  [WARN] rules_presets entries in {} must be strings.\n", context);
            }
        }
        else if (auto preset = presets.value<std::string>()) {
            apply_preset(*preset);
        }
        else {
            rf::console::print("  [WARN] 'rules_presets' in {} must be a string or array of strings.\n", context);
        }
    }

    // Allow presets to specify rule keys directly at the current scope.
    rules = parse_server_rules(scope_tbl, rules);

    if (auto rules_tbl = scope_tbl["rules"].as_table())
        rules = parse_server_rules(*rules_tbl, rules);

    return rules;
}

std::optional<ManualRulesOverride> load_rules_preset_alias(std::string_view preset_name)
{
    const auto it = g_alpine_server_config.rules_preset_aliases.find(std::string(preset_name));
    if (it == g_alpine_server_config.rules_preset_aliases.end())
        return std::nullopt;

    fs::path resolved_path = it->second;
    try {
        resolved_path = fs::weakly_canonical(resolved_path);
    }
    catch (const fs::filesystem_error& err) {
        rf::console::print("  [WARN] failed to canonicalize rules preset alias '{}' at '{}': {}\n", preset_name, resolved_path.generic_string(), err.what());
        resolved_path = fs::absolute(resolved_path);
    }

    std::vector<std::pair<std::filesystem::path, std::optional<std::string>>> applied_presets;

    try {
        toml::table preset_root = toml::parse_file(resolved_path.generic_string());
        const toml::table* preset_rules = nullptr;

        if (auto tbl = preset_root["rules"].as_table())
            preset_rules = tbl;
        else
            preset_rules = &preset_root;

        ManualRulesOverride result;
        result.rules = apply_rules_presets_and_overrides(
            *preset_rules, resolved_path.parent_path(), g_alpine_server_config.base_rules,
            std::format("rules preset alias '{}'", preset_name), &g_alpine_server_config.rules_preset_aliases,
            nullptr, &applied_presets);

        applied_presets.emplace_back(resolved_path, preset_name);
        result.applied_preset_paths = std::move(applied_presets);
        result.preset_alias = std::string(preset_name);
        return result;
    }
    catch (const toml::parse_error& err) {
        rf::console::print("  [ERROR] failed to parse rules preset alias '{}' at '{}': {}\n",
            preset_name, resolved_path.generic_string(), err.description());
        return std::nullopt;
    }
}

static void add_level_entry_from_table(AlpineServerConfig& cfg, const toml::table& lvl_tbl, const fs::path& base_dir)
{
    for (auto&& [k, v] : lvl_tbl) {
        const std::string key = std::string(k.str());
        if (key != "filename" && key != "rules" && key != "rules_presets") {
            xlog::warn("Unknown key '{}' inside a [[levels]] entry; did you intend to put it in [root]?", key);
        }
    }

    auto tmp_filename = lvl_tbl["filename"].value_or<std::string>("");
    rf::File f;
    if (!f.find(tmp_filename.c_str())) {
        rf::console::print("----> Level {} is not installed!\n\n", tmp_filename);
        return;
    }

    AlpineServerConfigLevelEntry entry;
    entry.level_filename = tmp_filename;

    std::string context = "level '" + (tmp_filename.empty() ? std::string("<unknown>") : tmp_filename) + "'";
    entry.rule_overrides = apply_rules_presets_and_overrides(
        lvl_tbl, base_dir, cfg.base_rules, context, &cfg.rules_preset_aliases, nullptr, &entry.applied_rules_preset_paths);

    cfg.levels.push_back(std::move(entry));
}

static void apply_rules_preset_aliases(AlpineServerConfig& cfg, const toml::table& tbl, const fs::path& base_dir)
{
    for (auto&& [alias_key, node] : tbl) {
        if (!node.is_value()) {
            xlog::warn("rules_preset_aliases entry '{}' must be a string", alias_key.str());
            continue;
        }

        auto alias_value = node.value<std::string>();
        if (!alias_value) {
            xlog::warn("rules_preset_aliases entry '{}' must be a string", alias_key.str());
            continue;
        }

        fs::path resolved_path = base_dir / *alias_value;
        try {
            resolved_path = fs::weakly_canonical(resolved_path);
        }
        catch (const fs::filesystem_error& err) {
            rf::console::print("  [WARN] failed to canonicalize rules preset alias '{}' -> '{}' : {}\n",
                std::string(alias_key), *alias_value, err.what());
            resolved_path = fs::absolute(resolved_path);
        }

        const std::string alias_name = static_cast<std::string>(alias_key.str());

        auto it = cfg.rules_preset_aliases.find(alias_name);
        if (it != cfg.rules_preset_aliases.end()) {
            if (it->second == resolved_path)
                continue;

            it->second = resolved_path;
            rf::console::print("  Updated rules preset alias '{}' -> {}\n", alias_name, resolved_path.generic_string());
        }
        else {
            cfg.rules_preset_aliases.emplace(alias_name, resolved_path);
            rf::console::print("  Registered rules preset alias '{}' -> {}\n", alias_name, resolved_path.generic_string());
        }
    }
}

// apply base config single keys
static void apply_known_key_in_order(AlpineServerConfig& cfg, const std::string& key, const toml::node& node)
{
    if (key == "ads_version") {
        if (auto v = node.value<int>())
            g_ads_loaded_version = *v;
    }
    else if (key == "server_name") {
        if (auto v = node.value<std::string>())
            cfg.server_name = *v;
    }
    else if (key == "max_players") {
        if (auto v = node.value<int>())
            cfg.set_max_players(*v);
    }
    else if (key == "password") {
        if (auto v = node.value<std::string>())
            cfg.set_password(*v);
    }
    else if (key == "rcon_password") {
        if (auto v = node.value<std::string>())
            cfg.set_rcon_password(*v);
    }
    else if (key == "upnp") {
        if (auto v = node.value<bool>())
            cfg.upnp_enabled = *v;
    }
    else if (key == "dynamic_rotation") {
        if (auto v = node.value<bool>())
            cfg.dynamic_rotation = *v;
    }
    else if (key == "require_client_mod") {
        if (auto v = node.value<bool>())
            cfg.require_client_mod = *v;
    }
    else if (key == "gaussian_spread") {
        if (auto v = node.value<bool>())
            cfg.gaussian_spread = *v;
    }
    else if (key == "send_stats_message") {
        if (auto v = node.value<bool>())
            cfg.stats_message_enabled = *v;
    }
    else if (key == "allow_fullbright_meshes") {
        if (auto v = node.value<bool>())
            cfg.allow_fullbright_meshes = *v;
    }
    else if (key == "allow_lightmap_mode") {
        if (auto v = node.value<bool>())
            cfg.allow_lightmaps_only = *v;
    }
    else if (key == "allow_disable_screenshake") {
        if (auto v = node.value<bool>())
            cfg.allow_disable_screenshake = *v;
    }
    else if (key == "allow_disable_muzzle_flash") {
        if (auto v = node.value<bool>())
            cfg.allow_disable_muzzle_flash = *v;
    }
    else if (key == "allow_unlimited_fps") {
        if (auto v = node.value<bool>())
            cfg.allow_unlimited_fps = *v;
    }
    else if (key == "use_sp_damage_calculation") {
        if (auto v = node.value<bool>())
            cfg.use_sp_damage_calculation = *v;
    }
}

// apply base config toml tables
static void apply_known_table_in_order(AlpineServerConfig& cfg, const std::string& key, const toml::table& tbl, const fs::path& base_dir)
{
    if (key == "inactivity")
        cfg.inactivity_config = parse_inactivity_config(tbl);
    else if (key == "damage_notifications")
        cfg.damage_notification_config = parse_damage_notification_config(tbl);
    else if (key == "alpine_restrict")
        cfg.alpine_restricted_config = parse_alpine_restrict_config(tbl);
    else if (key == "click_limiter")
        cfg.click_limiter_config = parse_click_limiter_config(tbl);
    else if (key == "vote_match")
        cfg.vote_match = parse_vote_config(tbl);
    else if (key == "vote_kick")
        cfg.vote_kick = parse_vote_config(tbl);
    else if (key == "vote_level")
        cfg.vote_level = parse_vote_config(tbl);
    else if (key == "vote_gametype")
        cfg.vote_gametype = parse_vote_config(tbl);
    else if (key == "vote_extend")
        cfg.vote_extend = parse_vote_config(tbl);
    else if (key == "vote_restart")
        cfg.vote_restart = parse_vote_config(tbl);
    else if (key == "vote_next")
        cfg.vote_next = parse_vote_config(tbl);
    else if (key == "vote_rand")
        cfg.vote_rand = parse_vote_config(tbl);
    else if (key == "vote_previous")
        cfg.vote_previous = parse_vote_config(tbl);
    else if (key == "rules_preset_aliases")
        apply_rules_preset_aliases(cfg, tbl, base_dir);
    else if (key == "base") {
        cfg.base_rules = apply_rules_presets_and_overrides(
            tbl, base_dir, cfg.base_rules, "base configuration", &cfg.rules_preset_aliases, nullptr, &cfg.base_rules_preset_paths);
    }
    else if (key == "levels") {
        if (auto arr = tbl.as_array()) {
            for (auto& elem : *arr) {
                if (!elem.is_table())
                    continue;
                auto& lvl_tbl = *elem.as_table();
                add_level_entry_from_table(cfg, lvl_tbl, base_dir);
            }
        }
    }
}

// apply known array toml nodes
static void apply_known_array_in_order(AlpineServerConfig& cfg, const std::string& key, const toml::array& arr, const fs::path& base_dir)
{
    if (key == "levels") {
        for (auto& elem : arr) {
            if (!elem.is_table())
                continue;

            auto& lvl_tbl = *elem.as_table();

            add_level_entry_from_table(cfg, lvl_tbl, base_dir);
        }
    }
}

// unified parser for config files
static void apply_config_table_in_order(
    AlpineServerConfig& cfg, const toml::table& tbl, const fs::path& base_dir, ParsePass pass)
{
    struct Entry
    {
        std::string key;
        const toml::node* node;
        int line = std::numeric_limits<int>::max();
        int col = std::numeric_limits<int>::max();
    };

    std::vector<Entry> entries;
    entries.reserve(tbl.size());

    for (auto&& [k, v] : tbl) {
        Entry e{std::string(k.str()), &v};

        if (auto src = v.source(); src.path) {
            e.line = static_cast<int>(src.begin.line);
            e.col = static_cast<int>(src.begin.column);
        }

        entries.push_back(std::move(e));
    }

    // custom sorting so entries are processed in file order when metadata is available
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.line != b.line)
            return a.line < b.line;
        return a.col < b.col;
    });

    for (const auto& e : entries) {
        const std::string& key = e.key;
        const toml::node& v = *e.node;

        // Single keys
        if (pass == ParsePass::Core) {
            apply_known_key_in_order(cfg, key, v);
        }

        // Arrays
        if (auto* arr = v.as_array()) {
            if (key == "levels") {
                if (pass == ParsePass::Levels)
                    apply_known_array_in_order(cfg, key, *arr, base_dir);
            }
            continue;
        }

        // Tables
        if (auto* sub_tbl = v.as_table()) {
            if (key == "levels") {
                if (pass == ParsePass::Levels) {
                    if (auto nested = sub_tbl->as_array())
                        apply_known_array_in_order(cfg, key, *nested, base_dir);
                }
                continue;
            }

            // allow root table workaround to allow root config after subsections in parent toml
            if (key == "root") {
                apply_config_table_in_order(cfg, *sub_tbl, base_dir, pass);
            }
            else {
                apply_known_table_in_order(cfg, key, *sub_tbl, base_dir);
            }

            continue;
        }
    }
}

void load_ads_server_config(std::string ads_config_name)
{
    rf::console::print("Loading and applying server configuration from {}...\n\n", ads_config_name);

    AlpineServerConfig cfg;     // start from defaults

    toml::table root;
    try {
        root = toml::parse_file(ads_config_name);
    }
    catch (const toml::parse_error& err) {
        rf::console::print("  [ERROR] failed to parse {}: {}\n", ads_config_name, err.description());
        return;
    }

    const fs::path root_path = fs::weakly_canonical(fs::path(ads_config_name));

    // config pass
    apply_config_table_in_order(cfg, root, root_path.parent_path(), ParsePass::Core);
    // level pass
    apply_config_table_in_order(cfg, root, root_path.parent_path(), ParsePass::Levels);

    rf::console::print("\n");

    g_alpine_server_config = std::move(cfg);
}

void print_gungame(std::string& output, const GunGameConfig& cur, const GunGameConfig& base_cfg, bool base = true)
{
    const auto iter = std::back_inserter(output);

    // helper functions
    auto gg_level_equal = [](const GunGameLevelEntry& a, const GunGameLevelEntry& b) {
        return a.kills == b.kills && a.tier == b.tier && a.weapon_index == b.weapon_index;
    };

    auto gg_level_key = [](const GunGameLevelEntry& e) {
        return std::tuple<int, int, int>{e.kills, e.tier, e.weapon_index};
    };

    auto gg_canon = [&](std::vector<GunGameLevelEntry> v, bool dynamic) {
        if (dynamic) {
            v.erase(std::remove_if(v.begin(), v.end(), [](auto& e) { return e.tier < 0 || e.weapon_index < 0; }),
                    v.end());
        }
        else {
            v.erase(std::remove_if(v.begin(), v.end(), [](auto& e) { return e.kills < 0 || e.weapon_index < 0; }),
                    v.end());
        }
        std::sort(v.begin(), v.end(), [&](auto const& a, auto const& b) { return gg_level_key(a) < gg_level_key(b); });

        std::vector<GunGameLevelEntry> out;
        for (auto const& e : v) {
            if (!out.empty() && gg_level_key(out.back()) == gg_level_key(e))
                out.back() = e;
            else
                out.push_back(e);
        }
        return out;
    };

    auto gg_final_equal = [&](const std::optional<GunGameLevelEntry>& a, const std::optional<GunGameLevelEntry>& b) {
        if (a.has_value() != b.has_value())
            return false;
        return !a || gg_level_equal(*a, *b);
    };
    // end helpers

    if (base || cur.enabled != base_cfg.enabled)
        std::format_to(iter, "  GunGame:                               {}\n", cur.enabled);

    if (!cur.enabled)
        return;

    if (base || cur.dynamic_progression != base_cfg.dynamic_progression)
        std::format_to(iter, "    Dynamic progression:                 {}\n", cur.dynamic_progression);
    if (base || cur.rampage_rewards != base_cfg.rampage_rewards)
        std::format_to(iter, "    Rampage rewards:                     {}\n", cur.rampage_rewards);

    bool cur_final = cur.final_level.has_value();
    bool base_final = base_cfg.final_level.has_value();
    if (base || cur_final != base_final)
        std::format_to(iter, "    Final level:                         {}\n", cur_final);

    if (cur.final_level) {
        bool print_details = base;
        if (!print_details && base_cfg.final_level)
            print_details = !gg_level_equal(*cur.final_level, *base_cfg.final_level);
        else if (!base_cfg.final_level)
            print_details = true;

        if (print_details) {
            std::format_to(iter, "      Kills:                             {}\n", cur.final_level->kills);
            std::format_to(iter, "      Weapon:                            {}\n", cur.final_level->weapon_name);
        }
    }

    const bool dyn = cur.dynamic_progression;
    auto cur_levels = gg_canon(cur.levels, dyn);
    auto base_levels = gg_canon(base_cfg.levels, dyn);

    bool levels_changed = base || cur_levels.size() != base_levels.size() ||
                          !std::equal(cur_levels.begin(), cur_levels.end(), base_levels.begin(), base_levels.end(),
                                      [&](auto const& a, auto const& b) { return gg_level_equal(a, b); });

    if (!levels_changed)
        return;

    if (dyn) {
        std::format_to(iter, "    Dynamic tiers:\n");
        for (auto const& e : cur_levels) std::format_to(iter, "      Tier {:<3} -> {}\n", e.tier, e.weapon_name);
    }
    else {
        std::format_to(iter, "    Levels (kills -> weapon):\n");
        for (auto const& e : cur_levels) std::format_to(iter, "      {:>4} -> {}\n", e.kills, e.weapon_name);
    }
}

void print_rules(std::string& output, const AlpineServerConfigRules& rules, bool base = true)
{
    const auto iter = std::back_inserter(output);
    const auto& b = g_alpine_server_config.base_rules;

    // game type
    if (base || rules.game_type != b.game_type)
        std::format_to(iter, "  Game type:                             {}\n", multi_game_type_name_short(rules.game_type));

    // time limit
    if (base || rules.time_limit != b.time_limit)
        std::format_to(iter, "  Time limit:                            {} min\n", rules.time_limit / 60.0f);

    const bool overtime_changed =
        rules.overtime.enabled != b.overtime.enabled ||
        (rules.overtime.enabled && (rules.overtime.additional_time != b.overtime.additional_time ||
          rules.overtime.consider_tie_if_flag_stolen != b.overtime.consider_tie_if_flag_stolen ||
          rules.overtime.consider_tie_if_hill_contested != b.overtime.consider_tie_if_hill_contested));

    if (base || overtime_changed) {
        std::format_to(iter, "  Overtime:                              {}\n", rules.overtime.enabled);
        if (rules.overtime.enabled) {
            std::format_to(iter, "    Additional time:                     {} min\n", rules.overtime.additional_time);
            std::format_to(iter, "    Tie when flag stolen (CTF):          {}\n", rules.overtime.consider_tie_if_flag_stolen);
            std::format_to(iter, "    Tie when hill contested (KOTH):      {}\n", rules.overtime.consider_tie_if_hill_contested);
        }
    }

    // score limits
    if (base || rules.individual_kill_limit != b.individual_kill_limit)
        std::format_to(iter, "  Player score limit (DM):               {}\n", rules.individual_kill_limit);
    if (base || rules.team_kill_limit != b.team_kill_limit)
        std::format_to(iter, "  Team score limit (TDM):                {}\n", rules.team_kill_limit);
    if (base || rules.cap_limit != b.cap_limit)
        std::format_to(iter, "  Flag capture limit (CTF):              {}\n", rules.cap_limit);
    if (base || rules.koth_score_limit != b.koth_score_limit)
        std::format_to(iter, "  Team score limit (KOTH):               {}\n", rules.koth_score_limit);
    if (base || rules.dc_score_limit != b.dc_score_limit)
        std::format_to(iter, "  Team score limit (DC):                 {}\n", rules.dc_score_limit);

    // common limits & flags
    if (base || rules.geo_limit != b.geo_limit)
        std::format_to(iter, "  Geomod crater limit:                   {}\n", rules.geo_limit);
    if (base || rules.team_damage != b.team_damage)
        std::format_to(iter, "  Team damage:                           {}\n", rules.team_damage);
    if (base || rules.fall_damage != b.fall_damage)
        std::format_to(iter, "  Fall damage:                           {}\n", rules.fall_damage);
    if (base || rules.weapons_stay != b.weapons_stay)
        std::format_to(iter, "  Weapon stay:                           {}\n", rules.weapons_stay);
    if (base || rules.force_respawn != b.force_respawn)
        std::format_to(iter, "  Force respawn:                         {}\n", rules.force_respawn);
    if (base || rules.balance_teams != b.balance_teams)
        std::format_to(iter, "  Balance teams:                         {}\n", rules.balance_teams);
    if (base || rules.ideal_player_count != b.ideal_player_count)
        std::format_to(iter, "  Ideal player count:                    {}\n", rules.ideal_player_count);
    if (base || rules.saving_enabled != b.saving_enabled)
        std::format_to(iter, "  Position saving:                       {}\n", rules.saving_enabled);
    if (base || rules.flag_dropping != b.flag_dropping)
        std::format_to(iter, "  Flag dropping (CTF):                   {}\n", rules.flag_dropping);
    if (base || rules.flag_captures_while_stolen != b.flag_captures_while_stolen)
        std::format_to(iter, "  Flag captures while stolen (CTF):      {}\n", rules.flag_captures_while_stolen);
    if (base || rules.ctf_flag_return_time_ms != b.ctf_flag_return_time_ms)
        std::format_to(iter, "  Flag return time (CTF):                {} sec\n", rules.ctf_flag_return_time_ms / 1000.0f);
    if (base || rules.pvp_damage_modifier != b.pvp_damage_modifier)
        std::format_to(iter, "  PvP damage modifier:                   {}\n", rules.pvp_damage_modifier);
    if (base || rules.drop_amps != b.drop_amps)
        std::format_to(iter, "  Drop amps:                             {}\n", rules.drop_amps);
    if (base || rules.drop_weapons != b.drop_weapons)
        std::format_to(iter, "  Drop weapons:                          {}\n", rules.drop_weapons);
    if (base || rules.no_player_collide != b.no_player_collide)
        std::format_to(iter, "  No player collide:                     {}\n", rules.no_player_collide);
    if (base || rules.location_pinging != b.location_pinging)
        std::format_to(iter, "  Location pinging:                      {}\n", rules.location_pinging);
    if (base || rules.weapon_items_give_full_ammo != b.weapon_items_give_full_ammo)
        std::format_to(iter, "  Weapon pickups give full ammo:         {}\n", rules.weapon_items_give_full_ammo);
    if (base || rules.weapon_infinite_magazines != b.weapon_infinite_magazines)
        std::format_to(iter, "  Infinite reloads:                      {}\n", rules.weapon_infinite_magazines);

    if (base || rules.welcome_message.enabled != b.welcome_message.enabled ||
        (rules.welcome_message.enabled && rules.welcome_message.welcome_message != b.welcome_message.welcome_message)) {
        std::format_to(iter, "  Welcome message:                       {}\n", rules.welcome_message.enabled);
        if (rules.welcome_message.enabled) {
            std::format_to(iter, "    Text:                                {}\n", rules.welcome_message.welcome_message);
        }
    }

    // spawn weapon
    if (base || rules.default_player_weapon.index != b.default_player_weapon.index ||
        (rules.default_player_weapon.num_clips != b.default_player_weapon.num_clips)) {
        std::format_to(iter, "  Spawn weapon:                          {}\n", rules.default_player_weapon.weapon_name);
        std::format_to(iter, "    Reserve clips:                       {}\n", rules.default_player_weapon.num_clips);
    }

    // spawn life
    if (base || rules.spawn_life.enabled != b.spawn_life.enabled ||
        (rules.spawn_life.enabled && rules.spawn_life.value != b.spawn_life.value)) {
        std::format_to(iter, "  Custom spawn health:                   {}\n", rules.spawn_life.enabled);
        if (rules.spawn_life.enabled) {
            std::format_to(iter, "    Value:                               {}\n", rules.spawn_life.value);
        }
    }

    // spawn armour
    if (base || rules.spawn_armour.enabled != b.spawn_armour.enabled ||
        (rules.spawn_armour.enabled && rules.spawn_armour.value != b.spawn_armour.value)) {
        std::format_to(iter, "  Custom spawn armor:                    {}\n", rules.spawn_armour.enabled);
        if (rules.spawn_armour.enabled) {
            std::format_to(iter, "    Value:                               {}\n", rules.spawn_armour.value);
        }
    }

    // spawn delay
    if (base || rules.spawn_delay.enabled != b.spawn_delay.enabled ||
        (rules.spawn_delay.enabled && rules.spawn_delay.base_value != b.spawn_delay.base_value)) {
        std::format_to(iter, "  Spawn delay:                           {}\n", rules.spawn_delay.enabled);
        if (rules.spawn_delay.enabled) {
            std::format_to(iter, "    Base seconds:                        {} sec\n", rules.spawn_delay.base_value / 1000.0f);
        }
    }

    // spawn loadout
    bool anySpawnLoadoutChanged = std::any_of(
        rules.spawn_loadout.red_weapons.begin(), rules.spawn_loadout.red_weapons.end(),
        [&](auto const& e){
            auto it = std::find_if(
                b.spawn_loadout.red_weapons.begin(), b.spawn_loadout.red_weapons.end(), [&](auto const& be) {
                    return be.weapon_name == e.weapon_name && be.reserve_ammo == e.reserve_ammo && be.enabled == e.enabled; }
            );
            return it == b.spawn_loadout.red_weapons.end();
        }
    );

    if (base || anySpawnLoadoutChanged) {
        std::format_to(iter, "  Spawn loadout:\n");
        for (auto const& e : rules.spawn_loadout.red_weapons) {
            bool unchanged = std::any_of(
                b.spawn_loadout.red_weapons.begin(), b.spawn_loadout.red_weapons.end(), [&](auto const& be) {
                    return be.weapon_name == e.weapon_name && be.reserve_ammo == e.reserve_ammo && be.enabled == e.enabled;
                }
            );
            if (base || !unchanged) {
                std::format_to(iter, "    {:<20}                 {}\n", e.weapon_name + ':', e.enabled);
                std::format_to(iter, "      Extra ammo:                        {}\n", e.reserve_ammo);
            }
        }
    }

     // spawn protection
    if (base || rules.spawn_protection.enabled != b.spawn_protection.enabled ||
        (rules.spawn_protection.enabled && (rules.spawn_protection.duration != b.spawn_protection.duration ||
                                            rules.spawn_protection.use_powerup != b.spawn_protection.use_powerup))) {
        std::format_to(iter, "  Spawn protection:                      {}\n", rules.spawn_protection.enabled);
        if (rules.spawn_protection.enabled) {
            std::format_to(iter, "    Duration:                            {} sec\n", rules.spawn_protection.duration / 1000.0f);
            std::format_to(iter, "    Use powerup:                         {}\n", rules.spawn_protection.use_powerup);
        }
    }

    // spawn logic
    bool spawnLogicChanged =
        rules.spawn_logic.respect_team_spawns != b.spawn_logic.respect_team_spawns ||
        rules.spawn_logic.try_avoid_players != b.spawn_logic.try_avoid_players ||
        rules.spawn_logic.always_avoid_last != b.spawn_logic.always_avoid_last ||
        rules.spawn_logic.always_use_furthest != b.spawn_logic.always_use_furthest ||
        rules.spawn_logic.only_avoid_enemies != b.spawn_logic.only_avoid_enemies ||
        rules.spawn_logic.dynamic_respawns != b.spawn_logic.dynamic_respawns ||
        rules.spawn_logic.dynamic_respawn_items.size() != b.spawn_logic.dynamic_respawn_items.size();

    if (base || spawnLogicChanged) {
        std::format_to(iter, "  Spawn logic:\n");
        if (base || rules.spawn_logic.respect_team_spawns != b.spawn_logic.respect_team_spawns)
            std::format_to(iter, "    Respect team spawns:                 {}\n", rules.spawn_logic.respect_team_spawns);
        if (base || rules.spawn_logic.try_avoid_players != b.spawn_logic.try_avoid_players)
            std::format_to(iter, "    Try avoid players:                   {}\n", rules.spawn_logic.try_avoid_players);
        if (base || rules.spawn_logic.always_avoid_last != b.spawn_logic.always_avoid_last)
            std::format_to(iter, "    Always avoid last:                   {}\n", rules.spawn_logic.always_avoid_last);
        if (base || rules.spawn_logic.always_use_furthest != b.spawn_logic.always_use_furthest)
            std::format_to(iter, "    Always use furthest:                 {}\n", rules.spawn_logic.always_use_furthest);
        if (base || rules.spawn_logic.only_avoid_enemies != b.spawn_logic.only_avoid_enemies)
            std::format_to(iter, "    Only avoid enemies:                  {}\n", rules.spawn_logic.only_avoid_enemies);
        if (base || rules.spawn_logic.dynamic_respawns != b.spawn_logic.dynamic_respawns)
            std::format_to(iter, "    Create item dynamic respawns:        {}\n", rules.spawn_logic.dynamic_respawns);

        if ((base || (rules.spawn_logic.dynamic_respawns != b.spawn_logic.dynamic_respawns ||
                      rules.spawn_logic.dynamic_respawn_items.size() != b.spawn_logic.dynamic_respawn_items.size())) &&
            rules.spawn_logic.dynamic_respawns) {
            for (auto const& item : rules.spawn_logic.dynamic_respawn_items) {
                bool unchanged = std::any_of(b.spawn_logic.dynamic_respawn_items.begin(),
                                             b.spawn_logic.dynamic_respawn_items.end(), [&](auto const& bi) {
                                                 return bi.item_name == item.item_name &&
                                                        bi.min_respawn_points == item.min_respawn_points;
                                             });
                if (base || !unchanged) {
                    std::format_to(iter, "      Dynamic respawn item:              {} (threshold: {})\n", item.item_name,
                                       item.min_respawn_points);
                }
            }
        }
    }

    // kill rewards
    bool rewardDiff = rules.kill_rewards.kill_reward_health != b.kill_rewards.kill_reward_health ||
                      rules.kill_rewards.kill_reward_armor != b.kill_rewards.kill_reward_armor ||
                      rules.kill_rewards.kill_reward_effective_health != b.kill_rewards.kill_reward_effective_health ||
                      rules.kill_rewards.kill_reward_health_super != b.kill_rewards.kill_reward_health_super ||
                      rules.kill_rewards.kill_reward_armor_super != b.kill_rewards.kill_reward_armor_super;

    if (base || rewardDiff) {
        std::format_to(iter, "  Kill rewards:\n");
        std::format_to(iter, "    Health:                              {}\n", rules.kill_rewards.kill_reward_health);
        std::format_to(iter, "    Armor:                               {}\n", rules.kill_rewards.kill_reward_armor);
        std::format_to(iter, "    Effective health:                    {}\n", rules.kill_rewards.kill_reward_effective_health);
        std::format_to(iter, "    Health is super:                     {}\n", rules.kill_rewards.kill_reward_health_super);
        std::format_to(iter, "    Armor is super:                      {}\n", rules.kill_rewards.kill_reward_armor_super);
    }

    // Weapon stay exemptions
    bool anyExemptionChanged = std::any_of(
        rules.weapon_stay_exemptions.exemptions.begin(),
        rules.weapon_stay_exemptions.exemptions.end(),
        [&](auto const& e){
            auto it = std::find_if(
                b.weapon_stay_exemptions.exemptions.begin(),
                b.weapon_stay_exemptions.exemptions.end(),
                [&](auto const& be){ return be.weapon_name == e.weapon_name
                                       && be.exemption_enabled == e.exemption_enabled; }
            );
            return it == b.weapon_stay_exemptions.exemptions.end();
        }
    );

    if (base || anyExemptionChanged) {
        std::format_to(iter, "  Weapon stay exemptions:\n");
        for (auto const& e : rules.weapon_stay_exemptions.exemptions) {
            bool unchanged = std::any_of(
                b.weapon_stay_exemptions.exemptions.begin(),
                b.weapon_stay_exemptions.exemptions.end(),
                [&](auto const& be){ return be.weapon_name == e.weapon_name
                                       && be.exemption_enabled == e.exemption_enabled; }
            );
            if (base || !unchanged) {
                std::string weap_name_string = e.weapon_name + ":";
                std::format_to(iter, "    {:<20}                 {}\n", weap_name_string, e.exemption_enabled ? "exempt" : "not exempt"
                );
            }
        }
    }

    // Item replacements
    bool anyReplacementChanged = std::any_of(
        rules.item_replacements.begin(),
        rules.item_replacements.end(),
        [&](auto const& kv){
            auto it = b.item_replacements.find(kv.first);
            return it == b.item_replacements.end() || it->second != kv.second;
        }
    );

    if (base || anyReplacementChanged) {
        std::format_to(iter, "  Item replacements:\n");
        for (auto const& [orig, repl] : rules.item_replacements) {
            auto it = b.item_replacements.find(orig);
            bool unchanged = (it != b.item_replacements.end() && it->second == repl);
            if (base || !unchanged) {
                std::format_to(iter, "    {:<20}     ->          {}\n", orig, repl.empty() ? "<none>" : repl);
            }
        }
    }

    // Item respawn time overrides
    bool anyRespawnChanged = std::any_of(
        rules.item_respawn_time_overrides.begin(),
        rules.item_respawn_time_overrides.end(),
        [&](auto const& kv){
            auto it = b.item_respawn_time_overrides.find(kv.first);
            return it == b.item_respawn_time_overrides.end() || it->second != kv.second;
        }
    );

    if (base || anyRespawnChanged) {
        std::format_to(iter, "  Item respawn time overrides:\n");
        for (auto const& [item, ms] : rules.item_respawn_time_overrides) {
            auto it = b.item_respawn_time_overrides.find(item);
            bool unchanged = (it != b.item_respawn_time_overrides.end() && it->second == ms);
            if (base || !unchanged) {
                std::string item_name_string = item + ":";
                std::format_to(iter, "    {:<20}                 {} ms\n", item_name_string, ms);
            }
        }
    }

    // force character
    if (base || rules.force_character.enabled != b.force_character.enabled ||
        (rules.force_character.enabled && rules.force_character.character_index != b.force_character.character_index)) {
        std::format_to(iter, "  Forced character:                      {}\n", rules.force_character.enabled);
        if (rules.force_character.enabled) {
            std::format_to(iter, "    Character:                           {} ({})\n", rules.force_character.character_name, rules.force_character.character_index);
        }
    }

    // critical hits
    if (base || rules.critical_hits.enabled != b.critical_hits.enabled ||
        (rules.critical_hits.enabled && rules.critical_hits.reward_duration != b.critical_hits.reward_duration) ||
        (rules.critical_hits.enabled && rules.critical_hits.base_chance != b.critical_hits.base_chance) ||
        (rules.critical_hits.enabled && rules.critical_hits.dynamic_scale != b.critical_hits.dynamic_scale) ||
        (rules.critical_hits.enabled && rules.critical_hits.dynamic_scale &&
         rules.critical_hits.dynamic_damage_bonus_ceiling != b.critical_hits.dynamic_damage_bonus_ceiling)
        ) {
        std::format_to(iter, "  Critical hits:                         {}\n", rules.critical_hits.enabled);
        if (rules.critical_hits.enabled) {
            std::format_to(iter, "    Reward duration:                     {} ms\n", rules.critical_hits.reward_duration);
            std::format_to(iter, "    Base chance:                         {:.1f}%\n", rules.critical_hits.base_chance * 100.0f);
            std::format_to(iter, "    Dynamic scale:                       {}\n", rules.critical_hits.dynamic_scale);
            if (rules.critical_hits.dynamic_scale) {
                std::format_to(iter, "      Dynamic damage bonus ceiling:      {}\n", rules.critical_hits.dynamic_damage_bonus_ceiling);
            }
        }
    }

    // gungame
    print_gungame(output, rules.gungame, b.gungame, base);
}

void print_rules_with_presets(std::string& output, const AlpineServerConfigRules& rules, const std::vector<std::pair<std::filesystem::path, std::optional<std::string>>>& preset_paths, bool base, const bool sanitize = false)
{
    const auto iter = std::back_inserter(output);
    if (!preset_paths.empty()) {
        std::format_to(iter, "  Rules presets applied:\n");
        for (const auto& [preset_path, preset_alias] : preset_paths) {
            const std::string path = sanitize
                ? preset_path.filename().generic_string()
                : preset_path.generic_string();
            if (preset_alias) {
                std::format_to(iter, "    {} (alias '{}')\n", path, preset_alias.value());
            } else {
                std::format_to(iter, "    {}\n", path);
            }
        }
    }
    print_rules(output, rules, base);
}

void print_alpine_dedicated_server_config_info(std::string& output, bool verbose, const bool sanitize) {
    auto& netgame = rf::netgame;
    const auto& cfg = g_alpine_server_config;

    const auto iter = std::back_inserter(output);
    std::format_to(iter, "\n---- Core configuration ----\n");
    std::format_to(iter, "  Server port:                           {} (UDP)\n", netgame.server_addr.port);
    std::format_to(iter, "  Server name:                           {}\n", netgame.name);
    if (!sanitize) {
        std::format_to(iter, "  Password:                              {}\n", netgame.password);
        std::format_to(iter, "  Rcon password:                         {}\n", rf::rcon_password);
    }
    std::format_to(iter, "  Max players:                           {}\n", netgame.max_players);
    std::format_to(iter, "  Levels in rotation:                    {}\n", cfg.levels.size());
    std::format_to(iter, "  Dynamic rotation:                      {}\n", cfg.dynamic_rotation);

    if (rf::mod_param.found()) {
        std::format_to(iter, "  TC mod loaded:                         {}\n", rf::mod_param.get_arg());
        std::format_to(iter, "  Clients must match TC mod:             {}\n", cfg.require_client_mod);
    }

    // minimal server config printing
    if (!verbose) {
        std::format_to(iter, "\n----> Enter sv_printconfig to print verbose server config.\n\n");
        return;
    }

    std::format_to(iter, "  Gaussian bullet spread:                {}\n", cfg.gaussian_spread);
    std::format_to(iter, "  End of round stats message:            {}\n", cfg.stats_message_enabled);
    std::format_to(iter, "  Allow fullbright meshes:               {}\n", cfg.allow_fullbright_meshes);
    std::format_to(iter, "  Allow disable screenshake:             {}\n", cfg.allow_disable_screenshake);
    std::format_to(iter, "  Allow lightmap only mode:              {}\n", cfg.allow_lightmaps_only);
    std::format_to(iter, "  Allow disable muzzle flash:            {}\n", cfg.allow_disable_muzzle_flash);
    std::format_to(iter, "  Allow disable 240 FPS cap:             {}\n", cfg.allow_unlimited_fps);
    std::format_to(iter, "  SP-style damage calculation:           {}\n", cfg.use_sp_damage_calculation);

    // inactivity
    std::format_to(iter, "  Kick inactive players:                 {}\n", cfg.inactivity_config.enabled);
    if (cfg.inactivity_config.enabled) {
        std::format_to(iter, "    New player grace period:             {} sec\n", cfg.inactivity_config.new_player_grace_ms / 1000.0f);
        std::format_to(iter, "    Allowed inactivity time:             {} sec\n", cfg.inactivity_config.allowed_inactive_ms / 1000.0f);
        std::format_to(iter, "    Warning duration:                    {} sec\n", cfg.inactivity_config.warning_duration_ms / 1000.0f);
        std::format_to(iter, "    Kick message:                        {}\n", cfg.inactivity_config.kick_message);
    }

    // click limiter
    std::format_to(iter, "  Click limiter:                         {}\n", cfg.click_limiter_config.enabled);
    if (cfg.click_limiter_config.enabled) {
        std::format_to(iter, "    Cooldown:                            {} ms\n", cfg.click_limiter_config.cooldown);
    }

    // damage notifications
    std::format_to(iter, "  Damage notifications:                  {}\n", cfg.damage_notification_config.enabled);
    if (cfg.damage_notification_config.enabled) {
        std::format_to(iter, "    Legacy client compatibility:         {}\n", cfg.damage_notification_config.support_legacy_clients);
    }

    // alpine restrict
    std::format_to(iter, "  Advertise Alpine:                      {}\n", cfg.alpine_restricted_config.advertise_alpine);
    std::format_to(iter, "  Only welcome Alpine players:           {}\n", cfg.alpine_restricted_config.only_welcome_alpine);
    std::format_to(iter, "  Clients require Alpine:                {}\n", cfg.alpine_restricted_config.clients_require_alpine);
    if (cfg.alpine_restricted_config.clients_require_alpine) {
        std::format_to(iter, "    Reject non-Alpine clients:           {}\n", cfg.alpine_restricted_config.reject_non_alpine_clients);
        std::format_to(iter, "    Require release build:               {}\n", cfg.alpine_restricted_config.alpine_require_release_build);
    }

    // votes
    auto& vm = cfg.vote_match;
    std::format_to(iter, "  Vote match:                            {}\n", vm.enabled);
    if (vm.enabled) {
        std::format_to(iter, "    Ignore nonvoters:                    {}\n", vm.ignore_nonvoters);
        std::format_to(iter, "    Time limit:                          {} sec\n", vm.time_limit_seconds);
    }

    auto print_vote = [&](std::string name, const VoteConfig& v) {
        std::format_to(iter, "  {}                         {}\n", name, v.enabled);
        if (v.enabled) {
            std::format_to(iter, "    Ignore nonvoters:                    {}\n", v.ignore_nonvoters);
            std::format_to(iter, "    Time limit:                          {} sec\n", v.time_limit_seconds);
        }
    };

    print_vote("Vote kick:    ", cfg.vote_kick);
    print_vote("Vote level:   ", cfg.vote_level);
    print_vote("Vote gametype:", cfg.vote_gametype);
    print_vote("Vote extend:  ", cfg.vote_extend);
    print_vote("Vote restart: ", cfg.vote_restart);
    print_vote("Vote next:    ", cfg.vote_next);
    print_vote("Vote random:  ", cfg.vote_rand);
    print_vote("Vote previous:", cfg.vote_previous);

    
    if (!cfg.rules_preset_aliases.empty()) {
        std::format_to(iter, "\n---- Rules preset alias mappings ----\n");
        for (const auto& [alias, path] : cfg.rules_preset_aliases) {
            if (sanitize) {
                std::format_to(iter, "  {} -> {}\n", alias, path.filename().generic_string());
            } else {
                std::format_to(iter, "  {} -> {}\n", alias, path.generic_string());
            }
        }
    }

    std::format_to(iter, "\n---- Base rules ----\n");
    print_rules_with_presets(output, cfg.base_rules, cfg.base_rules_preset_paths, true, sanitize);

    std::format_to(iter, "\n---- Level rotation ----\n");
    for (size_t i = 0; i < cfg.levels.size(); ++i) {
        const auto& lvl = cfg.levels[i];
        std::format_to(iter, "{} ({})\n", lvl.level_filename, i);
        print_rules_with_presets(output, lvl.rule_overrides, lvl.applied_rules_preset_paths, false, sanitize);
    }
    std::format_to(iter, "\n");
}

void initialize_core_alpine_dedicated_server_settings(rf::NetGameInfo& netgame, const AlpineServerConfig& cfg, bool on_launch) {
    netgame.name = cfg.server_name.c_str();

    netgame.password = cfg.password.c_str();

    // note: length is truncated before saving to g_additional_server_config.rcon_password
    std::strncpy(rf::rcon_password, cfg.rcon_password.c_str(), sizeof(rf::rcon_password) - 1);
    rf::rcon_password[sizeof(rf::rcon_password) - 1] = '\0'; // null terminator
    
    netgame.max_players = cfg.max_players;

    // other core settings are referenced directly in the structure and do not need to be initialized here
}

void apply_alpine_dedicated_server_rules(rf::NetGameInfo& netgame, const AlpineServerConfigRules& r)
{
    netgame.max_time_seconds = r.time_limit;
    switch (netgame.type) {
        case rf::NetGameType::NG_TYPE_TEAMDM:
            netgame.max_kills = r.team_kill_limit;
            break;
        case rf::NetGameType::NG_TYPE_CTF:
            netgame.max_captures = r.cap_limit;
            break;
        default:
            netgame.max_kills = r.individual_kill_limit;
            break;
    }

    netgame.geomod_limit = r.geo_limit;

    netgame.flags &= ~(rf::NG_FLAG_TEAM_DAMAGE
                     | rf::NG_FLAG_FALL_DAMAGE
                     | rf::NG_FLAG_WEAPON_STAY
                     | rf::NG_FLAG_FORCE_RESPAWN
                     | rf::NG_FLAG_BALANCE_TEAMS);

    if (r.team_damage)   netgame.flags |= rf::NG_FLAG_TEAM_DAMAGE;
    if (r.fall_damage)   netgame.flags |= rf::NG_FLAG_FALL_DAMAGE;
    if (r.weapons_stay)  netgame.flags |= rf::NG_FLAG_WEAPON_STAY;
    if (r.force_respawn) netgame.flags |= rf::NG_FLAG_FORCE_RESPAWN;
    if (r.balance_teams) netgame.flags |= rf::NG_FLAG_BALANCE_TEAMS;
}

// keep the netgame levels array synced with level+rules array
void rebuild_rotation_from_cfg()
{
    auto& levels_arr = rf::netgame.levels;
    levels_arr.clear();
    for (const auto& lvlEntry : g_alpine_server_config.levels) {
        levels_arr.add(lvlEntry.level_filename.c_str());
    }
}

void load_and_print_alpine_dedicated_server_config(std::string ads_config_name, bool on_launch) {
    auto& netgame = rf::netgame;
    auto& cfg = g_alpine_server_config;

    // parse toml file and update values
    // on launch does this before tracker registration
    if (!on_launch) {
        load_ads_server_config(ads_config_name);
        g_alpine_server_config.printed_cfg.clear();
        cfg.signal_cfg_changed = true;
    }

    initialize_core_alpine_dedicated_server_settings(netgame, cfg, on_launch);

    apply_alpine_dedicated_server_rules(netgame, cfg.base_rules); // base rules

    if (g_alpine_server_config.dynamic_rotation) {
        shuffle_level_array();
    }
    else {
        rebuild_rotation_from_cfg();
    }

    std::string output{};
    print_alpine_dedicated_server_config_info(output, !g_ads_minimal_server_info);
    rf::console::print("{}", output.c_str());
}

bool apply_game_type_for_current_level() {
    if (!g_dedicated_launched_from_ads)
        return false;

    auto &netgame = rf::netgame;
    auto &cfg     = g_alpine_server_config;
    const int idx = netgame.current_level_index;
    const auto upcoming = get_upcoming_game_type();
    const bool has_already_queued_change = (upcoming != netgame.type)
        || (get_upcoming_game_type_selection() == UpcomingGameTypeSelection::ExplicitRequest);
    const bool manual_load = was_level_loaded_manually();
    rf::NetGameType desired = rf::NetGameType::NG_TYPE_DM;

    if (manual_load) {
        const AlpineServerConfigRules& manual_rules =
            g_manual_rules_override ? g_manual_rules_override->rules : cfg.base_rules;

        desired = has_already_queued_change ? upcoming : manual_rules.game_type;

        if (!g_ads_minimal_server_info && !has_already_queued_change && desired != upcoming) {
            if (g_manual_rules_override && g_manual_rules_override->preset_alias) {
                rf::console::print("Applying rules preset '{}' game type {} for manually loaded level {}...\n",
                    *g_manual_rules_override->preset_alias, multi_game_type_name_short(desired), rf::level_filename_to_load);
            }
            else if (g_manual_rules_override) {
                rf::console::print("Applying manual override game type {} for manually loaded level {}...\n",
                    multi_game_type_name_short(desired), rf::level_filename_to_load);
            }
            else {
                rf::console::print("Applying base game type {} for manually loaded level {}...\n",
                    multi_game_type_name_short(desired), rf::level_filename_to_load);
            }
        }
    }
    else { // in rotation
        const bool idx_valid = (idx >= 0 && idx < static_cast<int>(cfg.levels.size()));
        const AlpineServerConfigRules& rules = (!has_already_queued_change && idx_valid)
            ? cfg.levels[idx].rule_overrides
            : cfg.base_rules;

        desired = has_already_queued_change ? upcoming : rules.game_type;

        if (!g_ads_minimal_server_info && !has_already_queued_change && desired != upcoming) {
            std::string_view level_name = idx_valid ? std::string_view(cfg.levels[idx].level_filename) : std::string_view("UNKNOWN");
            rf::console::print("Applying game type {} for server rotation index {} ({})...\n",
                multi_game_type_name_short(desired), idx, level_name);
        }
    }

    bool changed_this_call = false;
    if (!has_already_queued_change) {
        changed_this_call = set_upcoming_game_type(desired);
    }
    //else
    //    xlog::warn("apply_game_type_for_current_level: Skipping set, upcoming GT already queued to {}", multi_game_type_name_short(upcoming));


    /*xlog::warn("apply_game_type_for_current_level: desired={}, upcoming={}, current={}, changed={}, alreadyQueued={}",
                multi_game_type_name_short(desired),
                multi_game_type_name_short(upcoming),
                multi_game_type_name_short(netgame.type),
                changed_this_call,
                has_already_queued_change);*/

    return changed_this_call;
}

void apply_rules_for_current_level()
{
    auto &netgame = rf::netgame;
    auto &cfg     = g_alpine_server_config;

    // prevent a crash
    if (cfg.levels.size() < 1) {
        return;
    }

    int idx = netgame.current_level_index;
    // level manually loaded
    if (was_level_loaded_manually()) {
        if (g_manual_rules_override) {
            g_alpine_server_config_active_rules = g_manual_rules_override->rules;
            if (!g_ads_minimal_server_info) {
                if (g_manual_rules_override->preset_alias)
                    rf::console::print("Applying rules preset '{}' for manually loaded level {}...\n",
                                       *g_manual_rules_override->preset_alias, rf::level_filename_to_load);
                else
                    rf::console::print("Applying manual rules override for manually loaded level {}...\n",
                                       rf::level_filename_to_load);
            }
        }
        else {
            g_alpine_server_config_active_rules = cfg.base_rules;
            if (!g_ads_minimal_server_info)
                rf::console::print("Applying base rules for manually loaded level {}...\n", rf::level_filename_to_load);
        }
    }
    else { // level is in rotation
        AlpineServerConfigRules const &override_rules =
            (idx >= 0 && idx < (int)cfg.levels.size())
              ? cfg.levels[idx].rule_overrides
              : cfg.base_rules;

        g_alpine_server_config_active_rules = override_rules;

        if (!g_ads_minimal_server_info)
            rf::console::print("Applying level-specific rules for server rotation index {} ({})...\n", idx, cfg.levels[idx].level_filename);
    }

    // respect game type specific base rules (eg. koth spawn loadout) for voted or manually loaded maps
    const rf::NetGameType active_game_type = rf::netgame.type;
    if (g_alpine_server_config_active_rules.game_type != active_game_type) {
        g_alpine_server_config_active_rules.game_type = active_game_type;
        apply_defaults_for_game_type(active_game_type, g_alpine_server_config_active_rules);
    }

    // apply the rules
    apply_alpine_dedicated_server_rules(netgame, g_alpine_server_config_active_rules);
}

void init_alpine_dedicated_server() {
    // remove stock game weapon stay exemption for fusion
    AsmWriter(0x00459834).jmp(0x00459836);
    AsmWriter(0x004596BA).jmp(0x004596BC);
}

void launch_alpine_dedicated_server() {
    if (g_ads_full_console_log) {
        console_start_server_log();
    }
    rf::console::print("==================================================================\n");
    rf::console::print("================  Alpine Faction Dedicated Server ================\n");
    rf::console::print("==================================================================\n\n");

    auto& netgame = rf::netgame;

    load_ads_server_config(g_ads_config_name);
    const auto& cfg = g_alpine_server_config;

    if (!rf::lan_only_cmd_line_param.found()) {
        rf::console::print("Public game tracker:                     {}\n", g_alpine_game_config.multiplayer_tracker);
        rf::console::print("Attempt auto-forward via UPnP:           {}\n\n", cfg.upnp_enabled);

        rf::console::print("Attempting to register server with public game tracker...\n");
        rf::console::print("If it's not visible, visit alpinefaction.com/help for resources.\n\n");
    }
    else {
        rf::console::print("Not attempting to register server with public game tracker.\n\n");
    }

    load_and_print_alpine_dedicated_server_config(g_ads_config_name, true);

    if (netgame.levels.size() <= 0) {
        rf::console::print("----> No valid level files were specified!\n");
        rf::console::print("----> Launching server on Glass House...\n\n");
        netgame.levels.add("glass_house.rfl");
    }

    g_alpine_server_config_active_rules = cfg.base_rules; // initialize rules with base in case it is checked before first level loads
    init_alpine_dedicated_server();
    netgame.current_level_index = 0;
    rf::multi_level_switch_queued = -1;
}

ConsoleCommand2 print_server_config_cmd{
    "sv_printconfig",
    []() {
        if (g_dedicated_launched_from_ads) {
            std::string output{};
            print_alpine_dedicated_server_config_info(output, true);
            rf::console::print("{}", output.c_str());
        }
        else {
            rf::console::print("This command is only available for Alpine Faction dedicated servers launched with the -ads switch.\n");
        }
    },
    "Print the current server configuration. Only available for ADS dedicated servers.",
};

ConsoleCommand2 print_level_rules_cmd{
    "sv_printrules",
    [](std::optional<std::string> maybe_filename) {
        if (!g_dedicated_launched_from_ads) {
            rf::console::print("This command is only available for Alpine Faction dedicated servers launched with the -ads switch.\n");
            return;
        }

        const auto& cfg = g_alpine_server_config;
        std::vector<int> matches;

        if (maybe_filename) {
            // find all occurrences
            for (int i = 0; i < (int)cfg.levels.size(); ++i) {
                if (cfg.levels[i].level_filename == *maybe_filename)
                    matches.push_back(i);
            }
            if (matches.empty()) {
                rf::console::print("Level {} not found in rotation. If manually loaded, base rules would be used.\n", *maybe_filename);
                return;
            }
        } else {
            // use current index
            int idx = rf::netgame.current_level_index;
            if (idx < 0 || idx >= (int)cfg.levels.size()) {
                return;
            }
            matches.push_back(idx);
        }

        for (int idx : matches) {
            const auto& entry = cfg.levels[idx];

            bool manual_load = false;
            if (!maybe_filename) {
                // check if the current level was manually loaded
                manual_load = (rf::level_filename_to_load != entry.level_filename.c_str());
            }

            if (manual_load) {
                rf::console::print("\n---- Rules for level {} ----\n", rf::level_filename_to_load, idx);
                if (g_manual_rules_override) {
                    if (g_manual_rules_override->preset_alias)
                        rf::console::print("  (manually loaded {} is using rules preset '{}')\n\n", rf::level_filename_to_load, *g_manual_rules_override->preset_alias);
                    else
                        rf::console::print("  (manually loaded {} has a manual rules override)\n\n", rf::level_filename_to_load);
                    std::string output{};
                    print_rules_with_presets(output, g_manual_rules_override->rules, g_manual_rules_override->applied_preset_paths, true);
                    rf::console::print("{}", output.c_str());
                }
                else {
                    rf::console::print("  (manually loaded {} is using base rules)\n\n", rf::level_filename_to_load);
                    std::string output{};
                    print_rules_with_presets(output, cfg.base_rules, cfg.base_rules_preset_paths, true);
                    rf::console::print("{}", output.c_str());
                }
            }
            else {
                rf::console::print("\n---- Rules for level {} (index {}) ----\n", entry.level_filename, idx);
                std::string output{};
                print_rules_with_presets(output, entry.rule_overrides, entry.applied_rules_preset_paths, true);
                rf::console::print("{}", output.c_str());
            }
        }
    },
    "Print the rules for a level by filename, or the active rules for the current level if no filename specified"
};

ConsoleCommand2 load_server_config_cmd{
    "sv_loadconfig",
    [](std::optional<std::string> new_config) {
        if (g_dedicated_launched_from_ads) {
            if (rf::gameseq_get_state() != rf::GameState::GS_GAMEPLAY) {
                rf::console::print("You cannot load server config while between levels.\n");
                return;
            }

            load_and_print_alpine_dedicated_server_config(new_config.value_or(g_ads_config_name), false);
            bool changed_game_type = apply_game_type_for_current_level();
            apply_rules_for_current_level();
            initialize_game_info_server_flags();
            af_send_server_info_packet_to_all();
            enforce_alpine_hard_reject_for_all_players_on_current_level();
            if (changed_game_type)
                restart_current_level();
        }
        else {
            rf::console::print("This command is only available for Alpine Faction dedicated servers launched with the -ads switch.\n");
        }
    },
    "Load specified server config. If none specified, reloads the server launch config. Only available for ADS dedicated servers.",
};

void dedi_cfg_init() {

    // register console commands
    print_server_config_cmd.register_cmd();
    print_level_rules_cmd.register_cmd();
    load_server_config_cmd.register_cmd();
}
