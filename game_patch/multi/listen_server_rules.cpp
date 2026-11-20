#include <algorithm>
#include <array>
#include <initializer_list>
#include <common/payload.h>
#include <common/utils/string-utils.h>
#include "listen_server_rules.h"
#include "gametype.h"
#include "mutators.h"
#include "../rf/multi.h"
#include "../rf/weapon.h"

namespace
{
using Value = std::optional<ListenServerRuleValue>;
using Widget = ListenServerRuleWidget;
using rf::NetGameType;

constexpr uint32_t types(std::initializer_list<NetGameType> list)
{
    uint32_t mask = 0;
    for (const NetGameType type : list) {
        mask |= 1u << type;
    }
    return mask;
}

constexpr uint32_t any_type = MUTATOR_GAMETYPE_MASK_ANY;
const uint32_t team_types = mutator_gametype_mask_for_req(MutatorGametypeReq::TeamOnly);
const uint32_t round_types = gametype_mask_where(gt_type_uses_rounds);
constexpr uint32_t bag_types = types({rf::NG_TYPE_BAG, rf::NG_TYPE_TBAG});

Value seconds(int ms)
{
    return static_cast<float>(ms) / 1000.0f;
}

Value optional_float(const std::optional<float>& value)
{
    return value ? Value{*value} : std::nullopt;
}

constexpr float max_respawn_extra = payload::max_respawn_extra;

// A stock multiplayer spawn: miner1's 100 life, and the armor entity_create zeroes in multiplayer.
constexpr float stock_spawn_life = 100.0f;
constexpr float stock_spawn_armor = 0.0f;

constexpr bool hidden = true;

const AlpineServerConfig& server_defaults()
{
    static const AlpineServerConfig cfg;
    return cfg;
}

const std::array g_rules{
    // Server
    ListenServerRule{.key = "demo_auto_record", .label = "Record demos",
        .description = "Record each level to a demo in the demos folder.", .widget = Widget::Toggle,
        .group = "Server",
        .effective = [](const AlpineServerConfigRules&) -> Value { return server_defaults().demo_auto_record; },
        .root = true},

    // Limits
    ListenServerRule{"time_limit", "Time limit", "Level time limit in seconds.", Widget::Float, 10.0f, 86400.0f,
        60.0f, {}, any_type, true, "Limits", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.time_limit; }},
    ListenServerRule{"individual_kill_limit", "Frag limit", "Frags that win a DM level.", Widget::Int, 1.0f,
        32767.0f, 5.0f, {}, types({rf::NG_TYPE_DM}), true, "Limits", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.individual_kill_limit; }},
    ListenServerRule{"team_kill_limit", "Team frag limit", "Team frags that win a TDM level.", Widget::Int, 1.0f,
        65535.0f, 10.0f, {}, types({rf::NG_TYPE_TEAMDM}), true, "Limits", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.team_kill_limit; }},
    ListenServerRule{"cap_limit", "Capture limit", "Flag captures that win a CTF level.", Widget::Int, 1.0f,
        65535.0f, 1.0f, {}, types({rf::NG_TYPE_CTF}), true, "Limits", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.cap_limit; }},
    ListenServerRule{"koth_score_limit", "Score limit", "Team score that wins a KOTH level.", Widget::Int, 1.0f,
        65535.0f, 10.0f, {}, types({rf::NG_TYPE_KOTH}), true, "Limits", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.koth_score_limit; }},
    ListenServerRule{"dc_score_limit", "Score limit", "Team score that wins a DC level.", Widget::Int, 1.0f,
        65535.0f, 10.0f, {}, types({rf::NG_TYPE_DC}), true, "Limits", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.dc_score_limit; }},
    ListenServerRule{"pit_score_limit", "Score limit", "Player score that wins a PIT level.", Widget::Int, 1.0f,
        32767.0f, 1.0f, {}, types({rf::NG_TYPE_PIT}), true, "Limits", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.pit_score_limit; }},
    ListenServerRule{"gg_score_limit", "Score limit", "Player score that wins a GG level.", Widget::Int, 1.0f,
        32767.0f, 1.0f, {}, types({rf::NG_TYPE_GG}), true, "Limits", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.gungame_score_limit; }},
    ListenServerRule{"bag_score_limit", "Score limit", "Player score that wins a BAG level.", Widget::Int, 1.0f,
        32767.0f, 10.0f, {}, types({rf::NG_TYPE_BAG}), true, "Limits", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.bagman.bag_score_limit; }},
    ListenServerRule{"tbag_score_limit", "Score limit", "Team score that wins a TBAG level.", Widget::Int, 1.0f,
        65535.0f, 10.0f, {}, types({rf::NG_TYPE_TBAG}), true, "Limits", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.bagman.tbag_score_limit; }},
    ListenServerRule{"sal_cap_limit", "Capture limit", "Flag captures that win a SAL level.", Widget::Int, 1.0f,
        65535.0f, 1.0f, {}, types({rf::NG_TYPE_SAL}), true, "Limits", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.salvage.cap_limit; }},
    ListenServerRule{"scav_cap_limit", "Capture limit", "Cartridge captures that win a SCAV level.", Widget::Int,
        1.0f, 65535.0f, 1.0f, {}, types({rf::NG_TYPE_SCAV}), true, "Limits", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.scavenger.cap_limit; }},
    ListenServerRule{"rounds.max_rounds", "Rounds", "Rounds per level; in WO a team needs half of them plus one.",
        Widget::Int, 1.0f, 999.0f, 1.0f, {}, round_types, true, "Limits", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.rounds.max_rounds; }},
    ListenServerRule{"geo_limit", "Geomod limit", "Geomod craters allowed per level.", Widget::Int, 0.0f, 128.0f,
        8.0f, {}, any_type, false, "Limits", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.geo_limit; }, hidden},
    ListenServerRule{"rf2_geo_limit", "RF2-style geomod limit",
        "RF2-style geomod craters allowed per level; -1 is unlimited and 0 turns them off.", Widget::Int, -1.0f,
        10000.0f, 1.0f, {}, any_type, false, "Limits", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.rf2_geo_limit; }, hidden},

    // Rounds
    ListenServerRule{"rounds.round_time", "Round time", "Seconds per round.", Widget::Int, 10.0f, 3600.0f, 10.0f,
        {}, round_types, false, "Rounds", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return static_cast<int>(r.rounds.round_time); }},
    ListenServerRule{"rounds.post_round_time", "Post-round time", "Seconds of celebration after a round ends.",
        Widget::Int, 0.0f, 10.0f, 1.0f, {}, round_types, false, "Rounds", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return static_cast<int>(r.rounds.post_round_time); }},
    ListenServerRule{"rounds.intermission_time", "Intermission", "Seconds of countdown between rounds.",
        Widget::Int, 0.0f, 10.0f, 1.0f, {}, round_types, false, "Rounds", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return static_cast<int>(r.rounds.intermission_time); }},

    // Overtime
    ListenServerRule{"overtime.enabled", "Overtime", "Add overtime when the time limit runs out on a tie.",
        Widget::Toggle, 0.0f, 0.0f, 0.0f, {}, any_type, false, "Overtime", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.overtime.enabled; }},
    ListenServerRule{"overtime.time", "Overtime length", "Overtime length in minutes.", Widget::Int, 0.0f, 60.0f,
        1.0f, {}, any_type, false, "Overtime", "overtime.enabled",
        [](const AlpineServerConfigRules& r) -> Value { return r.overtime.additional_time; }},
    ListenServerRule{"overtime.tie_when_flag_stolen", "Tie while a flag is stolen",
        "Treat the game as tied while a flag is stolen.", Widget::Toggle, 0.0f, 0.0f, 0.0f, {},
        types({rf::NG_TYPE_CTF}), false, "Overtime", "overtime.enabled",
        [](const AlpineServerConfigRules& r) -> Value { return r.overtime.consider_tie_if_flag_stolen; }, hidden},
    ListenServerRule{"overtime.tie_when_hill_contested", "Tie while a hill is contested",
        "Treat the game as tied while a hill is contested.", Widget::Toggle, 0.0f, 0.0f, 0.0f, {},
        types({rf::NG_TYPE_KOTH}), false, "Overtime", "overtime.enabled",
        [](const AlpineServerConfigRules& r) -> Value { return r.overtime.consider_tie_if_hill_contested; }},

    // Teams
    ListenServerRule{"team_damage", "Team damage", "Players can damage teammates.", Widget::Toggle, 0.0f, 0.0f,
        0.0f, {}, team_types, true, "Teams", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.team_damage; }},
    ListenServerRule{"balance_teams", "Balance teams", "Balance the teams when a team level starts.",
        Widget::Toggle, 0.0f, 0.0f, 0.0f, {}, team_types, true, "Teams", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.balance_teams; }},
    ListenServerRule{"auto_team_balance", "Auto team balance",
        "During play, move players when the team sizes differ by 2 or more.", Widget::Toggle, 0.0f, 0.0f, 0.0f,
        {}, team_types, false, "Teams", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.auto_team_balance; }},

    // Gameplay
    ListenServerRule{"fall_damage", "Fall damage", "Players take fall damage.", Widget::Toggle, 0.0f, 0.0f, 0.0f,
        {}, any_type, true, "Gameplay", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.fall_damage; }},
    ListenServerRule{"weapons_stay", "Weapons stay", "Weapon pickups stay after being picked up.",
        Widget::Toggle, 0.0f, 0.0f, 0.0f, {}, any_type, true, "Gameplay", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.weapons_stay; }},
    ListenServerRule{"force_respawn", "Force respawn", "Respawn dead players automatically.", Widget::Toggle,
        0.0f, 0.0f, 0.0f, {}, any_type, true, "Gameplay", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.force_respawn; }},
    ListenServerRule{"exposure_damage", "Exposure damage",
        "Players take exposure damage when outside without armor.", Widget::Toggle, 0.0f, 0.0f, 0.0f, {},
        any_type, false, "Gameplay", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.exposure_damage; }, hidden},
    ListenServerRule{"pvp_damage_modifier", "PvP damage", "Multiplier for damage between players.",
        Widget::Float, 0.0f, 100.0f, 0.1f, {}, any_type, false, "Gameplay", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.pvp_damage_modifier; }},
    ListenServerRule{"no_player_collide", "No player collision", "Players pass through each other.",
        Widget::Toggle, 0.0f, 0.0f, 0.0f, {}, any_type, false, "Gameplay", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.no_player_collide; }},
    ListenServerRule{"location_pinging", "Location pinging", "Allow location pings.", Widget::Toggle, 0.0f, 0.0f,
        0.0f, {}, any_type, false, "Gameplay", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.location_pinging; }, hidden},
    ListenServerRule{"saving_enabled", "Position saving", "Let players save and load their position.",
        Widget::Toggle, 0.0f, 0.0f, 0.0f, {}, any_type, false, "Gameplay", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.saving_enabled; }, hidden},
    ListenServerRule{"geo_chunk_physics", "Geomod chunk physics", "Geomod chunks that are cut loose fall down.",
        Widget::Toggle, 0.0f, 0.0f, 0.0f, {}, any_type, false, "Gameplay", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.geo_chunk_physics; }, hidden},
    ListenServerRule{"clear_stale_movement_input", "Clear stale movement input",
        "Clients clear their movement input whenever they leave gameplay.", Widget::Toggle, 0.0f, 0.0f, 0.0f, {},
        any_type, false, "Gameplay", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.clear_stale_movement_input; }, hidden},
    ListenServerRule{.key = "gaussian_spread", .label = "Gaussian spread",
        .description = "Cluster shots toward the crosshair instead of spreading them evenly.",
        .widget = Widget::Toggle, .group = "Gameplay",
        .effective = [](const AlpineServerConfigRules&) -> Value { return server_defaults().gaussian_spread; },
        .root = true},
    ListenServerRule{.key = "use_sp_damage_calculation", .label = "Single player damage",
        .description = "Damage players as single player does, where armor does not fully protect health.",
        .widget = Widget::Toggle, .group = "Gameplay",
        .effective = [](const AlpineServerConfigRules&) -> Value {
            return server_defaults().use_sp_damage_calculation;
        },
        .root = true},

    // Weapons
    ListenServerRule{"spawn_weapon.weapon_name", "Spawn weapon", "The weapon players spawn holding.",
        Widget::Choice, 0.0f, 0.0f, 0.0f, ListenServerRuleChoices::Weapons, any_type, false, "Weapons", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.default_player_weapon.weapon_name; }},
    ListenServerRule{"spawn_weapon.clips", "Spawn weapon clips", "Reserve ammo for the spawn weapon, in clips.",
        Widget::Int, 0.0f, 99.0f, 1.0f, {}, any_type, false, "Weapons", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.default_player_weapon.num_clips; }},
    ListenServerRule{"drop_weapons", "Drop weapons", "Players drop their weapon when they die.", Widget::Toggle,
        0.0f, 0.0f, 0.0f, {}, any_type, false, "Weapons", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.drop_weapons; }},
    ListenServerRule{"drop_amps", "Drop powerups", "Players drop their powerups when they die.", Widget::Toggle,
        0.0f, 0.0f, 0.0f, {}, any_type, false, "Weapons", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.drop_amps; }},
    ListenServerRule{"infinite_reloads", "Infinite magazines", "Reloading does not use reserve ammo.",
        Widget::Toggle, 0.0f, 0.0f, 0.0f, {}, any_type, false, "Weapons", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.weapon_infinite_magazines; }},
    ListenServerRule{"weapon_pickups_give_full_ammo", "Full ammo pickups", "Weapon pickups fill the weapon's ammo.",
        Widget::Toggle, 0.0f, 0.0f, 0.0f, {}, any_type, false, "Weapons", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.weapon_items_give_full_ammo; }},
    ListenServerRule{"force_rail_reload", "Force rail reload",
        "Players must finish reloading the rail driver before switching weapons.", Widget::Toggle, 0.0f, 0.0f,
        0.0f, {}, any_type, false, "Weapons", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.force_rail_reload; }, hidden},

    // Spawning
    ListenServerRule{"spawn_protection.enabled", "Spawn protection", "Protect newly spawned players.",
        Widget::Toggle, 0.0f, 0.0f, 0.0f, {}, any_type, true, "Spawning", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.spawn_protection.enabled; }},
    ListenServerRule{"spawn_protection.duration", "Spawn protection time", "Protection length in seconds.",
        Widget::Float, 0.0f, 3600.0f, 0.5f, {}, any_type, false, "Spawning", "spawn_protection.enabled",
        [](const AlpineServerConfigRules& r) -> Value { return seconds(r.spawn_protection.duration); }},
    ListenServerRule{"spawn_protection.use_powerup", "Spawn protection powerup",
        "Grant an invulnerability powerup instead of plain spawn protection.", Widget::Toggle, 0.0f, 0.0f, 0.0f,
        {}, any_type, false, "Spawning", "spawn_protection.enabled",
        [](const AlpineServerConfigRules& r) -> Value { return r.spawn_protection.use_powerup; }, hidden},
    ListenServerRule{"spawn_delay.enabled", "Spawn delay", "Make players wait before respawning.", Widget::Toggle,
        0.0f, 0.0f, 0.0f, {}, any_type, false, "Spawning", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.spawn_delay.enabled; }},
    ListenServerRule{"spawn_delay.base_seconds", "Spawn delay time", "Respawn delay in seconds.", Widget::Float,
        0.25f, 60.0f, 0.25f, {}, any_type, false, "Spawning", "spawn_delay.enabled",
        [](const AlpineServerConfigRules& r) -> Value { return seconds(r.spawn_delay.base_value); }},
    ListenServerRule{"spawn_delay.wave_respawns", "Wave respawns", "Respawn each team in waves.", Widget::Toggle,
        0.0f, 0.0f, 0.0f, {}, team_types, false, "Spawning", "spawn_delay.enabled",
        [](const AlpineServerConfigRules& r) -> Value { return r.spawn_delay.wave_respawns; }},
    ListenServerRule{"spawn_life.enabled", "Custom spawn health", "Use a custom spawn health.", Widget::Toggle,
        0.0f, 0.0f, 0.0f, {}, any_type, false, "Spawning", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.spawn_life.enabled; }, hidden},
    ListenServerRule{.key = "spawn_life.value", .label = "Spawn health", .description = "Health players spawn with.",
        .widget = Widget::Float, .min = 1.0f, .max = 255.0f, .step = 5.0f, .group = "Spawning",
        .enabled_by = "spawn_life.enabled",
        .effective = [](const AlpineServerConfigRules& r) -> Value {
            return r.spawn_life.enabled ? r.spawn_life.value : stock_spawn_life;
        },
        .off_value = stock_spawn_life},
    ListenServerRule{"spawn_armor.enabled", "Custom spawn armor", "Use a custom spawn armor.", Widget::Toggle,
        0.0f, 0.0f, 0.0f, {}, any_type, false, "Spawning", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.spawn_armour.enabled; }, hidden},
    ListenServerRule{.key = "spawn_armor.value", .label = "Spawn armor", .description = "Armor players spawn with.",
        .widget = Widget::Float, .min = 0.0f, .max = 255.0f, .step = 5.0f, .group = "Spawning",
        .enabled_by = "spawn_armor.enabled",
        .effective = [](const AlpineServerConfigRules& r) -> Value {
            return r.spawn_armour.enabled ? r.spawn_armour.value : stock_spawn_armor;
        },
        .off_value = stock_spawn_armor},
    ListenServerRule{"spawn_selection.respect_team_spawns", "Team respawn points",
        "Use only the player's team's respawn points.", Widget::Toggle, 0.0f, 0.0f, 0.0f, {}, team_types, false,
        "Spawning", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.spawn_logic.respect_team_spawns; }},
    ListenServerRule{"spawn_selection.try_avoid_players", "Avoid players",
        "Prefer respawn points far from other players.", Widget::Toggle, 0.0f, 0.0f, 0.0f, {}, any_type, false,
        "Spawning", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.spawn_logic.try_avoid_players; }},
    ListenServerRule{"spawn_selection.always_avoid_last", "Avoid last respawn point",
        "Never reuse the player's previous respawn point.", Widget::Toggle, 0.0f, 0.0f, 0.0f, {}, any_type, false,
        "Spawning", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.spawn_logic.always_avoid_last; }},
    ListenServerRule{"spawn_selection.always_use_furthest", "Furthest respawn point",
        "Always pick the respawn point furthest from other players.", Widget::Toggle, 0.0f, 0.0f, 0.0f, {},
        any_type, false, "Spawning", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.spawn_logic.always_use_furthest; }, hidden},
    ListenServerRule{"spawn_selection.only_avoid_enemies", "Only avoid enemies",
        "Ignore teammates when measuring distance to other players.", Widget::Toggle, 0.0f, 0.0f, 0.0f, {},
        team_types, false, "Spawning", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.spawn_logic.only_avoid_enemies; }, hidden},
    ListenServerRule{"force_character.enabled", "Force character", "Make every player use one character.",
        Widget::Toggle, 0.0f, 0.0f, 0.0f, {}, any_type, false, "Spawning", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.force_character.enabled; }, hidden},
    ListenServerRule{"force_character.character", "Forced character", "The character every player uses.",
        Widget::Choice, 0.0f, 0.0f, 0.0f, ListenServerRuleChoices::Characters, any_type, false, "Spawning",
        "force_character.enabled",
        [](const AlpineServerConfigRules& r) -> Value { return r.force_character.character_name; }, hidden},

    // Kill rewards
    ListenServerRule{"kill_rewards.health", "Health per kill", "Health added per kill.", Widget::Float, 0.0f,
        200.0f, 5.0f, {}, any_type, false, "Kill rewards", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.kill_rewards.kill_reward_health; }, hidden},
    ListenServerRule{"kill_rewards.armor", "Armor per kill", "Armor added per kill.", Widget::Float, 0.0f, 200.0f,
        5.0f, {}, any_type, false, "Kill rewards", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.kill_rewards.kill_reward_armor; }, hidden},
    ListenServerRule{"kill_rewards.effective_health", "Effective health per kill",
        "Points that fill health first, the rest becoming armor at half rate.", Widget::Float, 0.0f, 400.0f, 5.0f,
        {}, any_type, false, "Kill rewards", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.kill_rewards.kill_reward_effective_health; }},
    ListenServerRule{"kill_rewards.health_is_super", "Super health rewards",
        "Let rewards raise health up to 200.", Widget::Toggle, 0.0f, 0.0f, 0.0f, {}, any_type, false,
        "Kill rewards", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.kill_rewards.kill_reward_health_super; }, hidden},
    ListenServerRule{"kill_rewards.armor_is_super", "Super armor rewards", "Let rewards raise armor up to 200.",
        Widget::Toggle, 0.0f, 0.0f, 0.0f, {}, any_type, false, "Kill rewards", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.kill_rewards.kill_reward_armor_super; }, hidden},

    // Gibbing
    ListenServerRule{"gibbing.enabled", "Gibbing", "Overkill deaths explode into gibs.", Widget::Toggle, 0.0f,
        0.0f, 0.0f, {}, any_type, false, "Gibbing", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.gibbing.enabled; }},
    ListenServerRule{"gibbing.damage_threshold", "Gib damage threshold",
        "Damage a killing hit must exceed to gib.", Widget::Float, 1.0f, 6000.0f, 10.0f, {}, any_type, false,
        "Gibbing", "gibbing.enabled",
        [](const AlpineServerConfigRules& r) -> Value { return r.gibbing.damage_threshold; }},
    ListenServerRule{"gibbing.all_damage_types", "Gib from any damage", "Any damage type can gib.",
        Widget::Toggle, 0.0f, 0.0f, 0.0f, {}, any_type, false, "Gibbing", "gibbing.enabled",
        [](const AlpineServerConfigRules& r) -> Value { return r.gibbing.all_damage; }},

    // Vehicles
    ListenServerRule{"vehicles.crash_damage", "Vehicle crash damage",
        "Vehicles take damage from hard impacts, hard landings and scraping.", Widget::Toggle, 0.0f, 0.0f, 0.0f,
        {}, any_type, false, "Vehicles", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.vehicles.crash_damage; }},
    ListenServerRule{"vehicles.bots_use_vehicles", "Bots use vehicles", "Bots may board vehicles.",
        Widget::Toggle, 0.0f, 0.0f, 0.0f, {}, any_type, false, "Vehicles", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.vehicles.bots_use_vehicles; }},
    ListenServerRule{"vehicles.bot_max_drivers", "Max bot drivers", "Most vehicles bots may drive at once.",
        Widget::Int, 0.0f, 32.0f, 1.0f, {}, any_type, false, "Vehicles", "vehicles.bots_use_vehicles",
        [](const AlpineServerConfigRules& r) -> Value { return r.vehicles.bot_max_drivers; }},

    // Objectives
    ListenServerRule{"flag_dropping", "Flag dropping", "Let flag carriers drop the flag.", Widget::Toggle, 0.0f,
        0.0f, 0.0f, {}, types({rf::NG_TYPE_CTF}), false, "Objectives", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.flag_dropping; }},
    ListenServerRule{"flag_captures_while_stolen", "Capture while stolen",
        "Let a team capture while its own flag is stolen.", Widget::Toggle, 0.0f, 0.0f, 0.0f, {},
        types({rf::NG_TYPE_CTF}), false, "Objectives", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.flag_captures_while_stolen; }},
    ListenServerRule{"flag_return_time", "Flag return time", "Seconds before a dropped flag returns.",
        Widget::Float, 1.0f, 600.0f, 1.0f, {}, types({rf::NG_TYPE_CTF}), false, "Objectives", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return seconds(r.ctf_flag_return_time_ms); }},
    ListenServerRule{"bag_return_time", "Bag return time", "Seconds before a dropped bag returns.", Widget::Float,
        1.0f, 60.0f, 0.5f, {}, bag_types, false, "Objectives", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return seconds(r.bagman.bag_return_time_ms); }},
    ListenServerRule{"bag_spawn_delay", "Bag spawn delay", "Seconds before the bag spawns.", Widget::Float, 0.0f,
        60.0f, 0.5f, {}, bag_types, false, "Objectives", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return seconds(r.bagman.bag_spawn_delay_ms); }},
    ListenServerRule{"bag_multiple_spawns", "Random bag spawn",
        "Spawn the bag at a random home, picked again each time it returns.", Widget::Toggle, 0.0f, 0.0f, 0.0f,
        {}, bag_types, false, "Objectives", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.bagman.bag_multiple_spawns; }},
    ListenServerRule{"sal_flag_spawn_delay", "Flag spawn delay", "Seconds before the flag spawns.", Widget::Float,
        0.0f, 60.0f, 1.0f, {}, types({rf::NG_TYPE_SAL}), false, "Objectives", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return seconds(r.salvage.flag_spawn_delay_ms); }},
    ListenServerRule{"sal_flag_capture_respawn_delay", "Flag respawn delay",
        "Seconds before the flag respawns after a capture.", Widget::Float, 0.0f, 60.0f, 1.0f, {},
        types({rf::NG_TYPE_SAL}), false, "Objectives", nullptr,
        [](const AlpineServerConfigRules& r) -> Value {
            return seconds(r.salvage.flag_capture_respawn_delay_ms);
        }},
    ListenServerRule{"sal_flag_return_time", "Flag return time", "Seconds before a dropped flag returns.",
        Widget::Float, 1.0f, 60.0f, 1.0f, {}, types({rf::NG_TYPE_SAL}), false, "Objectives", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return seconds(r.salvage.flag_return_time_ms); }},
    ListenServerRule{"scav_cartridge_spawn_delay", "Cartridge spawn delay", "Seconds before the cartridge spawns.",
        Widget::Float, 0.0f, 60.0f, 1.0f, {}, types({rf::NG_TYPE_SCAV}), false, "Objectives", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return seconds(r.scavenger.objective_spawn_delay_ms); }},
    ListenServerRule{"scav_cartridge_capture_respawn_delay", "Cartridge respawn delay",
        "Seconds before the cartridge respawns after a capture.", Widget::Float, 0.0f, 60.0f, 1.0f, {},
        types({rf::NG_TYPE_SCAV}), false, "Objectives", nullptr,
        [](const AlpineServerConfigRules& r) -> Value {
            return seconds(r.scavenger.objective_capture_respawn_delay_ms);
        }},
    ListenServerRule{"scav_cartridge_return_time", "Cartridge return time",
        "Seconds before a dropped cartridge returns.", Widget::Float, 1.0f, 60.0f, 1.0f, {},
        types({rf::NG_TYPE_SCAV}), false, "Objectives", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return seconds(r.scavenger.objective_return_time_ms); }},
    ListenServerRule{"scav_target_timeout", "Target timeout", "Seconds before the target moves.", Widget::Float,
        10.0f, 600.0f, 10.0f, {}, types({rf::NG_TYPE_SCAV}), false, "Objectives", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return seconds(r.scavenger.zone_timeout_ms); }},
    ListenServerRule{"scav_target_radius", "Target radius", "Radius of the target in meters.", Widget::Float,
        1.0f, 10.0f, 0.5f, {}, types({rf::NG_TYPE_SCAV}), false, "Objectives", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.scavenger.zone_radius; }},

    // Payload
    ListenServerRule{"pay_setup_time", "Setup time", "Seconds the cart stays put at the start of a round.",
        Widget::Float, 0.0f, PayloadConfig::max_setup_time, 1.0f, {}, types({rf::NG_TYPE_PAY}), false, "Payload",
        nullptr, [](const AlpineServerConfigRules& r) -> Value { return r.payload.setup_time; }},
    ListenServerRule{"pay_checkpoint_time_bonus", "Checkpoint time bonus",
        "Seconds added to the time limit when the cart captures a checkpoint.", Widget::Float, 0.0f,
        payload::max_checkpoint_time_bonus, 10.0f, {}, types({rf::NG_TYPE_PAY}), false, "Payload", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return optional_float(r.payload.checkpoint_time_bonus); }},
    ListenServerRule{"pay_overtime_grace", "Overtime grace",
        "Seconds of overtime with no attacker at the cart before the defenders win.", Widget::Float, 0.0f,
        payload::max_overtime_grace, 1.0f, {}, types({rf::NG_TYPE_PAY}), false, "Payload", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return optional_float(r.payload.overtime_grace); }},
    ListenServerRule{"pay_heal_per_tick", "Pusher healing",
        "Effective health given each second to every attacker at the cart.", Widget::Float, 0.0f,
        payload::max_heal_per_second, 1.0f, {}, types({rf::NG_TYPE_PAY}), false, "Payload", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return optional_float(r.payload.heal_per_tick); }},
    ListenServerRule{"pay_attacker_respawn_extra", "Attacker respawn extra",
        "Seconds added to the attackers' respawn delay.", Widget::Float, -max_respawn_extra, max_respawn_extra,
        0.5f, {}, types({rf::NG_TYPE_PAY}), false, "Payload", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return optional_float(r.payload.attacker_respawn.base); }},
    ListenServerRule{"pay_attacker_respawn_per_checkpoint", "Attacker respawn per checkpoint",
        "Seconds added to the attackers' respawn delay for each checkpoint passed.", Widget::Float,
        -max_respawn_extra, max_respawn_extra, 0.5f, {}, types({rf::NG_TYPE_PAY}), false, "Payload", nullptr,
        [](const AlpineServerConfigRules& r) -> Value {
            return optional_float(r.payload.attacker_respawn.per_checkpoint);
        }},
    ListenServerRule{"pay_attacker_respawn_final_stretch", "Attacker respawn final stretch",
        "Seconds added to the attackers' respawn delay once the cart reaches the final stretch.", Widget::Float,
        -max_respawn_extra, max_respawn_extra, 0.5f, {}, types({rf::NG_TYPE_PAY}), false, "Payload", nullptr,
        [](const AlpineServerConfigRules& r) -> Value {
            return optional_float(r.payload.attacker_respawn.final_stretch);
        }},
    ListenServerRule{"pay_defender_respawn_extra", "Defender respawn extra",
        "Seconds added to the defenders' respawn delay.", Widget::Float, -max_respawn_extra, max_respawn_extra,
        0.5f, {}, types({rf::NG_TYPE_PAY}), false, "Payload", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return optional_float(r.payload.defender_respawn.base); }},
    ListenServerRule{"pay_defender_respawn_per_checkpoint", "Defender respawn per checkpoint",
        "Seconds added to the defenders' respawn delay for each checkpoint passed.", Widget::Float,
        -max_respawn_extra, max_respawn_extra, 0.5f, {}, types({rf::NG_TYPE_PAY}), false, "Payload", nullptr,
        [](const AlpineServerConfigRules& r) -> Value {
            return optional_float(r.payload.defender_respawn.per_checkpoint);
        }},
    ListenServerRule{"pay_defender_respawn_final_stretch", "Defender respawn final stretch",
        "Seconds added to the defenders' respawn delay once the cart reaches the final stretch.", Widget::Float,
        -max_respawn_extra, max_respawn_extra, 0.5f, {}, types({rf::NG_TYPE_PAY}), false, "Payload", nullptr,
        [](const AlpineServerConfigRules& r) -> Value {
            return optional_float(r.payload.defender_respawn.final_stretch);
        }},

    // Gun Game
    ListenServerRule{"gg_rampage_rewards", "Rampage rewards",
        "Reward players who advance through several weapons in one life.", Widget::Toggle, 0.0f, 0.0f, 0.0f, {},
        types({rf::NG_TYPE_GG}), false, "Gun Game", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.gungame_rampage_rewards; }},
    ListenServerRule{"gg_final_weapon", "Final weapon", "Weapon for the final level.", Widget::Choice, 0.0f, 0.0f,
        0.0f, ListenServerRuleChoices::Weapons, types({rf::NG_TYPE_GG}), false, "Gun Game", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.gungame_final_weapon; }},
    ListenServerRule{"gg_end_with_fusion", "End with fusion",
        "After the final weapon, add a one-kill Fusion Rocket Launcher level.", Widget::Toggle, 0.0f, 0.0f, 0.0f,
        {}, types({rf::NG_TYPE_GG}), false, "Gun Game", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.gungame_end_with_fusion; }},

    // Bots
    ListenServerRule{"ideal_player_count", "Ideal player count",
        "Player count bots fill up to; at 32 they never leave to make room.", Widget::Int, 1.0f, 32.0f, 1.0f, {},
        mutator_gametype_mask_for_req(MutatorGametypeReq::BotsSupported), true, "Bots", nullptr,
        [](const AlpineServerConfigRules& r) -> Value { return r.ideal_player_count; }, hidden},
};
} // namespace

std::span<const ListenServerRule> listen_server_rules()
{
    return g_rules;
}

const ListenServerRule* listen_server_find_rule(std::string_view key)
{
    const auto it = std::find_if(g_rules.begin(), g_rules.end(), [&](const ListenServerRule& rule) {
        return key == rule.key;
    });
    return it != g_rules.end() ? &*it : nullptr;
}

bool listen_server_rule_applies(const ListenServerRule& rule, rf::NetGameType game_type)
{
    return mutator_gametype_mask_allows(rule.game_type_mask, static_cast<uint8_t>(game_type));
}

std::vector<MutatorOptionChoice> listen_server_rule_choices(const ListenServerRule& rule)
{
    std::vector<MutatorOptionChoice> choices;
    if (rule.choices == ListenServerRuleChoices::Weapons) {
        choices = mutators_pickup_weapon_choices();
        // The Riot Stick has no pickup but is every player's melee weapon.
        const int stick = rf::riot_stick_weapon_type;
        if (stick >= 0 && stick < rf::num_weapon_types) {
            const std::string name = rf::weapon_types[stick].name.c_str();
            if (std::none_of(choices.begin(), choices.end(),
                    [&](const MutatorOptionChoice& c) { return string_iequals(c.value, name); })) {
                const std::string label = rf::weapon_types[stick].display_name.c_str();
                choices.insert(choices.begin(), MutatorOptionChoice{label.empty() ? name : label, name});
            }
        }
    }
    else if (rule.choices == ListenServerRuleChoices::Characters) {
        for (int i = 0; i < rf::num_multi_characters; ++i) {
            const rf::MpCharacterInfo& info = rf::mp_characters[i];
            const char* screen_name = info.screen_name && info.screen_name[0] ? info.screen_name : info.name;
            choices.push_back(MutatorOptionChoice{screen_name, info.name});
        }
    }
    return choices;
}
