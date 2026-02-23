#pragma once

#include <string_view>
#include <string>
#include <set>
#include <map>
#include <optional>
#include <vector>
#include <filesystem>
#include "../rf/math/vector.h"
#include "../rf/math/matrix.h"
#include "../rf/os/string.h"
#include "../rf/parse.h"
#include "../rf/os/console.h"
#include "../rf/multi.h"
#include "../rf/weapon.h"

// Forward declarations
namespace rf
{
    struct Player;
}

// used for game_info packets
struct AFGameInfoFlags
{
    bool modded_server          = false;
    bool alpine_only            = false;
    bool reject_legacy_clients  = false;
    bool click_limiter          = false;
    bool no_player_collide      = false;
    bool match_mode             = false;
    bool saving_enabled         = false;
    bool gaussian_spread        = false;
    bool damage_notifications   = false;

    uint32_t game_info_flags_to_uint32() const
    {
        return (static_cast<uint32_t>(modded_server)            << 0) |
               (static_cast<uint32_t>(alpine_only)              << 1) |
               (static_cast<uint32_t>(reject_legacy_clients)    << 2) |
               (static_cast<uint32_t>(click_limiter)            << 3) |
               (static_cast<uint32_t>(no_player_collide)        << 4) |
               (static_cast<uint32_t>(match_mode)               << 5) |
               (static_cast<uint32_t>(saving_enabled)           << 6) |
               (static_cast<uint32_t>(gaussian_spread)          << 7) |
               (static_cast<uint32_t>(damage_notifications)     << 8);
    }
};

enum class ParsePass
{
    Core,
    Levels
};

struct SpawnProtectionConfig
{
    bool enabled = false;
    uint32_t duration = 1500;
    bool use_powerup = false;

    // =============================================

    void set_duration(float in_time)
    {
        duration = static_cast<int>(std::clamp(in_time * 1000.0f, 0.0f, 3600000.0f));
    }
};

struct InactivityConfig
{
    bool enabled = false;
    bool kick_after_warning = true;
    uint32_t new_player_grace_ms = 120000;
    uint32_t allowed_inactive_ms = 30000;
    uint32_t warning_duration_ms = 10000;
    std::string kick_message = "You have been marked as idle due to inactivity! You will be kicked from the game unless you respawn in the next 10 seconds.";

    // =============================================
    
    void set_new_player_grace(float in_time)
    {
        new_player_grace_ms = static_cast<int>(std::max(in_time * 1000.0f, 1000.0f));
    }
    void set_allowed_inactive(float in_time)
    {
        allowed_inactive_ms = static_cast<int>(std::max(in_time * 1000.0f, 1000.0f));
    }
    void set_warning_duration(float in_time)
    {
        warning_duration_ms = static_cast<int>(std::max(in_time * 1000.0f, 1000.0f));
    }
};

struct VoteConfig
{
    bool enabled = false;
    bool ignore_nonvoters = false;
    int time_limit_seconds = 60;
    std::vector<std::string> allowed_maps;
    bool add_rotation_to_allowed_levels = false;
    bool only_allow_gametype_prefix = false;

    // =============================================
    
    void set_time_limit_seconds(float in_time)
    {
        time_limit_seconds = static_cast<int>(std::max(in_time, 1.0f));
    }
};

struct GunGameLevelEntry
{
    int kills = -1;             // manual mode
    int tier = -1;              // dynamic mode
    std::string weapon_name;    // only used for config print on launch
    int weapon_index = -1;

    auto operator<=>(const GunGameLevelEntry&) const = default;
};

struct GunGameConfig
{
    bool enabled = false;
    bool dynamic_progression = false;
    bool rampage_rewards = false;

    std::optional<GunGameLevelEntry> final_level;
    std::vector<GunGameLevelEntry> levels;

    // =============================================

    static int resolve_weapon(std::string_view name)
    {
        int idx = rf::weapon_lookup_type(name.data());
        return idx;
    }

    bool set_final_level(int kills, std::string_view weapon)
    {
        int idx = resolve_weapon(weapon);
        if (idx < 0)
            return false;
        final_level = GunGameLevelEntry{kills, -1, std::string{weapon}, idx};
        return true;
    }

    // Manual mode entry
    bool add_level_by_kills(int kills, std::string_view weapon)
    {
        int idx = resolve_weapon(weapon);
        if (idx < 0)
            return false;
        // replace if same kills already present
        auto it = std::find_if(levels.begin(), levels.end(), [&](auto const& e) { return e.kills == kills; });
        if (it != levels.end()) {
            it->weapon_name = weapon;
            it->weapon_index = idx;
            it->tier = -1;
            return false;
        }
        levels.push_back(GunGameLevelEntry{kills, -1, std::string{weapon}, idx});
        return true;
    }

    // Dynamic mode entry
    bool add_level_by_tier(int tier, std::string_view weapon)
    {
        int idx = resolve_weapon(weapon);
        if (idx < 0)
            return false;
        levels.push_back(GunGameLevelEntry{-1, tier, std::string{weapon}, idx});
        return true;
    }

    void normalize_manual()
    {
        std::sort(levels.begin(), levels.end(), [](auto const& a, auto const& b) { return a.kills < b.kills; });
        // dedupe by kills (keep last)
        std::vector<GunGameLevelEntry> out;
        for (auto const& e : levels) {
            if (e.kills < 0)
                continue; // ignore tier entries in manual
            if (!out.empty() && out.back().kills == e.kills)
                out.back() = e;
            else
                out.push_back(e);
        }
        levels.swap(out);
    }
};

struct BagmanConfig
{
    bool enabled = false;
    float bag_return_time = 25000.0f;
};

struct DamageNotificationConfig
{
    bool enabled = true;
    bool support_legacy_clients = true;
};

struct CriticalHitsConfig
{
    bool enabled = false;
    //int sound_id = 35; // hardcoded
    //int rate_limit = 10; // hardcoded
    uint32_t reward_duration = 1500;
    float base_chance = 0.1f;
    bool dynamic_scale = true;
    float dynamic_damage_bonus_ceiling = 1200.0f;

    // =============================================

    void set_reward_duration(int new_ms)
    {
        reward_duration = std::clamp(new_ms, 50, 60000); // max 1 min
    }

    void set_base_chance(float in_value)
    {
        base_chance = std::clamp(in_value, 0.01f, 1.0f); // percentile
    }

    void set_damage_bonus_ceiling(float in_value)
    {
        dynamic_damage_bonus_ceiling = std::clamp(in_value, 100.0f, 100000.0f);
    }
};

struct WeaponStayExemptionConfigOld
{
    bool enabled = false;
    bool rocket_launcher = false;
    bool heavy_machine_gun = false;
    bool sniper_rifle = false;
    bool assault_rifle = false;
    bool machine_pistol = false;
    bool shotgun = false;
    bool scope_assault_rifle = false;
    bool grenade = false;
    bool remote_charge = false;
    bool handgun = false;
    bool flamethrower = false;
    bool riot_stick = false;
    bool riot_shield = false;
    bool rail_gun = false;
};

struct OvertimeConfig
{
    bool enabled = false;
    int additional_time = 5;
    bool consider_tie_if_flag_stolen = false;
    bool consider_tie_if_hill_contested = true;

    // =============================================

    void set_additional_time(int new_minutes)
    {
        additional_time = std::clamp(new_minutes, 0, 60);
    }
};

struct NewSpawnLogicRespawnItemConfig
{
    std::string item_name;
    int min_respawn_points = 8;
};

struct NewSpawnLogicConfig // defaults match stock game
{
    bool respect_team_spawns = true;    
    bool try_avoid_players = true;
    bool always_avoid_last = false;
    bool always_use_furthest = false;
    bool only_avoid_enemies = false;
    bool dynamic_respawns = false;
    std::vector<NewSpawnLogicRespawnItemConfig> dynamic_respawn_items;

    // =============================================

    bool add_dynamic_respawn_item(std::string_view name, int min_pts)
    {
        if (name.empty())
            return false;

        int idx = rf::item_lookup_type(name.data());
        if (idx < 0)
            return false;

        if (min_pts < 0)
            min_pts = 0;

        // update if it already exists
        auto it = std::find_if(dynamic_respawn_items.begin(), dynamic_respawn_items.end(),
                               [&](auto const& e) { return e.item_name == name; });
        if (it != dynamic_respawn_items.end()) {
            it->min_respawn_points = min_pts;
            return true;
        }

        dynamic_respawn_items.push_back(NewSpawnLogicRespawnItemConfig{
            std::string{name}, min_pts
        });
        return true;
    }

    void clear_dynamic_respawn_items() { dynamic_respawn_items.clear(); }
};

struct KillRewardConfig
{
    float kill_reward_health = 0.0f;
    float kill_reward_armor = 0.0f;
    float kill_reward_effective_health = 0.0f;
    bool kill_reward_health_super = false;
    bool kill_reward_armor_super = false;
};

struct WelcomeMessageConfig
{
    bool enabled = false;
    std::string welcome_message = "";

    // =============================================

    void set_welcome_message(std::string_view new_welcome_message)
    {
        bool was_trimmed = new_welcome_message.size() > 240;
        std::string_view to_use = was_trimmed ? new_welcome_message.substr(0, 240) : new_welcome_message;
        welcome_message.assign(to_use);
    }
};

struct WeaponStayExemptionEntry
{
    bool exemption_enabled = true;
    std::string weapon_name;
    int index;

    auto operator<=>(const WeaponStayExemptionEntry&) const = default;
};

struct WeaponStayExemptionConfig
{
    std::vector<WeaponStayExemptionEntry> exemptions;

    // =============================================

    // default true unless specified
    bool add(std::string_view name, bool exemption_enabled = true)
    {
        // see if we already have this weapon
        auto it =
            std::find_if(exemptions.begin(), exemptions.end(), [&](auto const& e) { return e.weapon_name == name; });

        if (it != exemptions.end()) {
            // already present, just update the enabled flag
            it->exemption_enabled = exemption_enabled;
            return false;
        }

        // not found, add a new one
        int idx = rf::weapon_lookup_type(name.data());
        if (idx < 0)
            return false;

        exemptions.emplace_back(exemption_enabled, std::string{name}, idx);
        return true;
    }
};

struct AlpineRestrictConfig
{
    bool advertise_alpine = true;
    bool only_welcome_alpine = false;
    bool clients_require_alpine = false;

    // below options are only used if clients_require_alpine = true
    bool reject_non_alpine_clients = false;
    bool alpine_require_release_build = false;
};

struct SpawnLifeConfig
{
    bool enabled = false;
    float value = 100.0f;

    // =============================================

    void set_value(float in_value)
    {
        value = std::clamp(in_value, 1.0f, 255.0f);
    }
};

struct SpawnDelayConfig
{
    bool enabled = false;
    int base_value = 5000;

    // =============================================

    void set_base_value(float in_value)
    {
        int value_int = static_cast<int>(in_value * 1000);
        base_value = std::clamp(value_int, 250, 60000);
    }
};

struct ForceCharacterConfig
{
    bool enabled = false;
    int character_index = 0;
    std::string character_name = "enviro_parker"; // only used for human-readable logging

    // =============================================

    void set_character(std::string_view in_character)
    {
        int idx = rf::multi_find_character(in_character.data());

        if (idx < 0) {
            character_index = 0;
            character_name = "enviro_parker";
        }
        else {
            character_index = idx;
            character_name = std::string(in_character);
        }
    }
};

struct WeaponLoadoutEntry
{
    std::string weapon_name;
    int index;
    int reserve_ammo;
    bool enabled = true;

    auto operator<=>(const WeaponLoadoutEntry&) const = default;
};

struct WeaponLoadoutConfig
{
    bool loadouts_active = false;
    std::vector<WeaponLoadoutEntry> red_weapons; // todo: teams
    std::vector<WeaponLoadoutEntry> blue_weapons;

    // =============================================

    bool add(std::string_view name, int ammo, bool blue_team, bool enabled = true)
    {
        auto weapons_array = blue_team ? &blue_weapons : &red_weapons;

        // only add one instance of the weapon
        auto it = std::find_if(weapons_array->begin(), weapons_array->end(), [&](auto const& e) { return e.weapon_name == name; });
        if (it != weapons_array->end()) {
            // already present, just update the enabled flag
            it->enabled = enabled;
            return false;
        }

        // not found, add a new one
        int idx = rf::weapon_lookup_type(name.data());
        if (idx < 0)
            return false;

        weapons_array->emplace_back(WeaponLoadoutEntry{std::string{name}, idx, ammo, enabled});
        return true;
    }

    bool remove(std::string_view name, bool blue_team)
    {
        auto& weapons_array = blue_team ? blue_weapons : red_weapons;
        auto it = std::find_if(weapons_array.begin(), weapons_array.end(),
            [&](auto const& e) { return e.weapon_name == name; });

        if (it == weapons_array.end())
            return false;

        weapons_array.erase(it);
        return true;
    }
};

struct DefaultPlayerWeaponConfig
{
    std::string weapon_name = "";
    int index = -1;
    int num_clips = 3;

    // =============================================

    void set_weapon(std::string_view in_weapon)
    {
        int idx = rf::weapon_lookup_type(in_weapon.data());
        if (idx >= 0) {
            index = idx;
            weapon_name = std::string(in_weapon);
        }
    }
};

struct ClickLimiterConfig
{
    bool enabled = true;
    int cooldown = 90;

    // =============================================

    void set_cooldown(int new_cooldown)
    {
        cooldown = std::clamp(new_cooldown, 0, 5000);
    }
};

struct GibConfig
{
    // Set `false` to match stock multiplayer.
    bool enabled = false;

    bool all_damage = false;
    float damage_threshold = 100.0f;

    // =============================================

    void set_damage_threshold(float threshold)
    {
        damage_threshold = std::clamp(threshold, 1.0f, 6000.0f);
    }
};

struct AlpineServerConfigRules
{
    // stock game rules
    rf::NetGameType game_type = rf::NetGameType::NG_TYPE_DM;
    float time_limit = 600.0f;
    OvertimeConfig overtime;
    int individual_kill_limit = 30;
    int team_kill_limit = 100;
    int cap_limit = 5;
    int koth_score_limit = 100;
    int dc_score_limit = 300;
    int geo_limit = 64;
    bool team_damage = false;
    bool fall_damage = false;
    bool weapons_stay = false;
    bool force_respawn = false;
    bool balance_teams = false;
    int ideal_player_count = 32;
    bool saving_enabled = false;
    bool flag_dropping = true;
    bool flag_captures_while_stolen = false;
    bool drop_amps = false;
    bool no_player_collide = false;
    bool location_pinging = false;
    int ctf_flag_return_time_ms = 25000;
    float pvp_damage_modifier = 1.0f;
    DefaultPlayerWeaponConfig default_player_weapon;
    SpawnLifeConfig spawn_life;
    SpawnLifeConfig spawn_armour;
    SpawnDelayConfig spawn_delay;
    WeaponLoadoutConfig spawn_loadout;
    SpawnProtectionConfig spawn_protection;
    NewSpawnLogicConfig spawn_logic;
    GibConfig gibbing;
    WelcomeMessageConfig welcome_message;
    bool weapon_items_give_full_ammo = false;
    bool weapon_infinite_magazines = false;
    bool drop_weapons = true;
    KillRewardConfig kill_rewards;
    WeaponStayExemptionConfig weapon_stay_exemptions;
    std::map<std::string, std::string> item_replacements;
    std::map<std::string, int> item_respawn_time_overrides;
    ForceCharacterConfig force_character;
    CriticalHitsConfig critical_hits;
    GunGameConfig gungame;

    // =============================================
    void set_time_limit(float count)
    {
        time_limit = std::max(count, 10.0f);
    }
    void set_individual_kill_limit(int count)
    {
        individual_kill_limit = std::clamp(count, 1, 65535);
    }
    void set_team_kill_limit(int count)
    {
        team_kill_limit = std::clamp(count, 1, 65535);
    }
    void set_cap_limit(int count)
    {
        cap_limit = std::clamp(count, 1, 65535);
    }
    void set_koth_score_limit(int count)
    {
        koth_score_limit = std::clamp(count, 1, 65535);
    }
    void set_dc_score_limit(int count)
    {
        dc_score_limit = std::clamp(count, 1, 65535);
    }
    void set_geo_limit(int count)
    {
        geo_limit = std::clamp(count, 0, 128);
    }
    void set_ideal_player_count(int count)
    {
        ideal_player_count = std::clamp(count, 1, 32);
    }
    void set_flag_return_time(float in_time)
    {
        ctf_flag_return_time_ms = static_cast<int>(std::max(in_time * 1000.0f, 1000.0f));
    }
    void set_pvp_damage_modifier(float modifier)
    {
        pvp_damage_modifier = std::clamp(modifier, 0.0f, 100.0f);
    }
    bool add_item_replacement(std::string_view original, std::string_view replacement)
    {
        int orig_idx = rf::item_lookup_type(original.data());
        int repl_idx = rf::item_lookup_type(replacement.data());
        if (orig_idx < 0) {
            // check if original name is invalid
            // replacement name being blank is fine, removes item
            return false;
        }
        item_replacements[std::string(original)] = std::string(replacement);
        return true;
    }
    bool set_item_respawn_time(std::string_view item_name, int respawn_time_ms)
    {
        int idx = rf::item_lookup_type(item_name.data());
        if (idx < 0) {
            return false;
        }
        item_respawn_time_overrides[std::string(item_name)] = respawn_time_ms;
        return true;
    }
};

struct AlpineServerConfigLevelEntry
{
    std::string level_filename;
    AlpineServerConfigRules rule_overrides;
    std::vector<std::pair<std::filesystem::path, std::optional<std::string>>> applied_rules_preset_paths;
};

struct AlpineRconProfile
{
    std::string name;
    std::string password;
    bool full_admin = false;
    bool allow_multiple = false;
    std::vector<std::string> allowed_commands;
};

struct AlpineServerConfig
{
    std::string server_name = "Alpine Faction Server";
    int max_players = 8;
    std::string password = "";
    std::string rcon_password = "";
    std::vector<AlpineRconProfile> rcon_profiles;
    uint32_t bot_shared_secret = 0;
    bool upnp_enabled = false;
    bool require_client_mod = true;
    bool dynamic_rotation = false;
    bool gaussian_spread = true;
    bool stats_message_enabled = true;
    bool allow_fullbright_meshes = true;
    bool allow_lightmaps_only = true;
    bool allow_disable_screenshake = true;
    bool allow_disable_muzzle_flash = true;
    bool allow_unlimited_fps = false;
    bool allow_footsteps = true;
    bool use_sp_damage_calculation = false;
    bool exclude_bots_from_player_count = false;
    AlpineRestrictConfig alpine_restricted_config;
    InactivityConfig inactivity_config;
    DamageNotificationConfig damage_notification_config;
    ClickLimiterConfig click_limiter_config;
    VoteConfig vote_match;
    VoteConfig vote_kick;
    VoteConfig vote_level;
    VoteConfig vote_gametype;
    VoteConfig vote_extend;
    VoteConfig vote_restart;
    VoteConfig vote_next;
    VoteConfig vote_rand;
    VoteConfig vote_previous;

    AlpineServerConfigRules base_rules;
    std::vector<std::pair<std::filesystem::path, std::optional<std::string>>> base_rules_preset_paths;
    std::map<std::string, std::filesystem::path> rules_preset_aliases;
    std::vector<AlpineServerConfigLevelEntry> levels;

    std::string printed_cfg{};
    bool signal_cfg_changed = false;

    // =============================================

    void set_max_players(int count)
    {
        max_players = std::clamp(count, 1, 32);
    }
    void set_password(std::string_view new_password)
    {
        bool was_trimmed = new_password.size() > 16;
        std::string_view to_use = was_trimmed ? new_password.substr(0, 16) : new_password;
        password.assign(to_use);
    }
    void set_rcon_password(std::string_view new_password)
    {
        bool was_trimmed = new_password.size() > 15;
        std::string_view to_use = was_trimmed ? new_password.substr(0, 15) : new_password;
        rcon_password.assign(to_use);
    }

    void set_bot_shared_secret(const uint32_t secret) {
        bot_shared_secret = secret;
    }
};

struct ManualRulesOverride
{
    AlpineServerConfigRules rules;
    std::vector<std::pair<std::filesystem::path, std::optional<std::string>>> applied_preset_paths;
    std::optional<std::string> preset_alias;
};

struct MatchInfo
{
    std::time_t last_match_reminder_time = 0;
    bool pre_match_queued = false;
    bool pre_match_active = false;
    std::time_t pre_match_start_time = 0;
    std::optional<float> time_limit_on_pre_match_start;
    std::time_t last_ready_reminder_time = 0;
    bool everyone_ready = false;
    bool match_active = false;
    int team_size = -1;
    std::set<rf::Player*> ready_players_red;
    std::set<rf::Player*> ready_players_blue;
    std::set<rf::Player*> active_match_players;
    std::string match_level_name;

    void reset()
    {
        last_match_reminder_time = 0;
        pre_match_queued = false;
        pre_match_active = false;
        pre_match_start_time = 0;
        time_limit_on_pre_match_start.reset();
        last_ready_reminder_time = 0;
        everyone_ready = false;
        match_active = false;
        team_size = -1;
        ready_players_red.clear();
        ready_players_blue.clear();
        active_match_players.clear();
        match_level_name.clear();
    }
};

extern AlpineServerConfig g_alpine_server_config;
extern AlpineServerConfigRules g_alpine_server_config_active_rules;
extern std::optional<ManualRulesOverride> g_manual_rules_override;
extern bool g_dedicated_launched_from_ads;
extern bool g_manually_loaded_level;
extern std::string g_ads_config_name;
extern AFGameInfoFlags g_game_info_server_flags;
extern std::string g_prev_level;
extern MatchInfo g_match_info;

enum class UpcomingGameTypeSelection {
    Rotation,
    ExplicitRequest,
};

const rf::NetGameType get_upcoming_game_type();
UpcomingGameTypeSelection get_upcoming_game_type_selection();
bool is_rcon_command_masterlisted(std::string_view command);
bool set_upcoming_game_type(rf::NetGameType gt, UpcomingGameTypeSelection selection = UpcomingGameTypeSelection::Rotation);
void apply_defaults_for_game_type(rf::NetGameType game_type, AlpineServerConfigRules& rules);
void cleanup_win32_server_console();
void handle_vote_command(std::string_view vote_name, std::string_view vote_arg, rf::Player* sender);
void handle_player_set_handicap(rf::Player* player, uint8_t amount);
std::vector<rf::Player*> get_clients(bool include_browsers, bool include_bots);
std::pair<bool, std::string> is_level_name_valid(std::string_view level_name_input);
std::optional<ManualRulesOverride> load_rules_preset_alias(std::string_view preset_name);
void set_manual_rules_override(ManualRulesOverride override_rules);
void clear_manual_rules_override();
bool is_player_in_match(rf::Player* player);
bool is_player_ready(rf::Player* player);
void update_pre_match_powerups(rf::Player* player);
void start_match();
void cancel_match();
void start_pre_match();
void set_ready_status(rf::Player* player, bool is_ready);
void remove_ready_player_silent(rf::Player* player);
void toggle_ready_status(rf::Player* player);
bool get_ready_status(const rf::Player* player);
void server_vote_do_frame();
void init_server_commands();
void rebuild_rotation_from_cfg();
void on_dedicated_server_launch_post();
void extend_round_time(int minutes);
void restart_current_level();
void load_next_level();
void load_rand_level();
void load_prev_level();
void server_vote_on_player_leave(rf::Player* player);
void server_vote_on_limbo_state_enter();
void process_delayed_kicks();
void kick_player_delayed(const rf::Player* player);
bool ends_with(const rf::String& str, const std::string& suffix);
const AlpineServerConfig& server_get_alpine_config();
rf::CmdLineParam& get_ads_cmd_line_param();
rf::CmdLineParam& get_min_cmd_line_param();
rf::CmdLineParam& get_log_cmd_line_param();
rf::CmdLineParam& get_nodl_cmd_line_param();
void handle_min_param();
void handle_log_param();
void handle_nodl_param();
const AFGameInfoFlags& server_get_game_info_flags();
void initialize_game_info_server_flags();
std::optional<rf::NetGameType> resolve_gametype_from_name(std::string_view gametype_name);
bool multi_set_gametype_alpine(std::string_view gametype_name);
bool is_gametype_name_valid(std::string_view gametype_name);
void launch_alpine_dedicated_server();
std::string build_info_command_output();
