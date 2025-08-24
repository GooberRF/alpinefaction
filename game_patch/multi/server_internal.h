#pragma once

#include <string_view>
#include <string>
#include <set>
#include <map>
#include <optional>
#include <vector>
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
    bool enabled = true;
    int duration = 1500;
    bool use_powerup = false;

    // =============================================

    void set_duration(float in_time)
    {
        duration = static_cast<int>(std::clamp(in_time * 1000.0f, 0.0f, 3600000.0f) + 0.5f);
    }
};

struct InactivityConfig
{
    bool enabled = false;
    int new_player_grace_ms = 120000;
    int allowed_inactive_ms = 30000;
    int warning_duration_ms = 10000;
    std::string kick_message = "You have been marked as idle due to inactivity! You will be kicked from the game unless you respawn in the next 10 seconds.";

    // =============================================
    
    void set_new_player_grace(float in_time)
    {
        new_player_grace_ms = static_cast<int>(std::max(in_time * 1000.0f, 1000.0f) + 0.5f);
    }
    void set_allowed_inactive(float in_time)
    {
        allowed_inactive_ms = static_cast<int>(std::max(in_time * 1000.0f, 1000.0f) + 0.5f);
    }
    void set_warning_duration(float in_time)
    {
        warning_duration_ms = static_cast<int>(std::max(in_time * 1000.0f, 1000.0f) + 0.5f);
    }
};

struct VoteConfig
{
    bool enabled = false;
    bool ignore_nonvoters = false;
    int time_limit_seconds = 60;

    // =============================================
    
    void set_time_limit_seconds(float in_time)
    {
        time_limit_seconds = static_cast<int>(std::max(in_time, 1.0f) + 0.5f);
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
    int reward_duration = 1500;
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

        exemptions.emplace_back(WeaponStayExemptionEntry{exemption_enabled, std::string{name}, idx});
        return true;
    }
};

struct AlpineRestrictConfig
{
    bool clients_require_alpine = false;
    bool reject_non_alpine_clients = false;
    bool alpine_server_version_enforce_min = false;
    bool alpine_require_release_build = false;
    bool only_welcome_alpine = false;
    bool advertise_alpine = false;
    bool no_player_collide = false;
    bool location_pinging = true;
    VoteConfig vote_match;
    OvertimeConfig overtime;
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
};

struct DefaultPlayerWeaponConfig
{
    std::string weapon_name = "12mm handgun";
    int index = 3;
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

struct AlpineServerConfigRules
{
    // stock game rules
    float time_limit = 600.0f;
    int individual_kill_limit = 30;
    int team_kill_limit = 100;
    int cap_limit = 5;
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
    int ctf_flag_return_time_ms = 25000;
    float pvp_damage_modifier = 1.0f;
    DefaultPlayerWeaponConfig default_player_weapon;
    SpawnLifeConfig spawn_life;
    SpawnLifeConfig spawn_armour;
    WeaponLoadoutConfig spawn_loadout;
    SpawnProtectionConfig spawn_protection;
    NewSpawnLogicConfig spawn_logic;
    WelcomeMessageConfig welcome_message;
    bool weapon_items_give_full_ammo = false;
    bool weapon_infinite_magazines = false;
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
        ctf_flag_return_time_ms = static_cast<int>(std::max(in_time * 1000.0f, 1000.0f) + 0.5f);
    }
    void set_pvp_damage_modifier(float modifier)
    {
        pvp_damage_modifier = std::clamp(modifier, 0.0f, 100.0f);
    }
    bool add_item_replacement(std::string_view original, std::string_view replacement)
    {
        int orig_idx = rf::item_lookup_type(original.data());
        int repl_idx = rf::item_lookup_type(replacement.data());
        if (orig_idx < 0 || repl_idx < 0) {
            // check if either name is invalid
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
};

struct AlpineServerConfig
{
    std::string server_name = "Alpine Faction Server";
    rf::NetGameType game_type = rf::NetGameType::NG_TYPE_DM;
    int max_players = 8;
    std::string password = "";
    std::string rcon_password = "";
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
    bool use_sp_damage_calculation = false;
    AlpineRestrictConfig alpine_restricted_config;
    InactivityConfig inactivity_config;
    DamageNotificationConfig damage_notification_config;
    ClickLimiterConfig click_limiter_config;
    VoteConfig vote_kick;
    VoteConfig vote_level;
    VoteConfig vote_extend;
    VoteConfig vote_restart;
    VoteConfig vote_next;
    VoteConfig vote_rand;
    VoteConfig vote_previous;


    AlpineServerConfigRules base_rules;
    std::vector<AlpineServerConfigLevelEntry> levels;

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
extern bool g_dedicated_launched_from_ads;
extern std::string g_ads_config_name;
extern AFGameInfoFlags g_game_info_server_flags;
extern std::string g_prev_level;
extern MatchInfo g_match_info;

void cleanup_win32_server_console();
void handle_vote_command(std::string_view vote_name, std::string_view vote_arg, rf::Player* sender);
void handle_player_set_handicap(rf::Player* player, uint8_t amount);
std::vector<rf::Player*> get_current_player_list(bool include_browsers);
std::pair<bool, std::string> is_level_name_valid(std::string_view level_name_input);
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
void extend_round_time(int minutes);
void restart_current_level();
void load_next_level();
void load_rand_level();
void load_prev_level();
void server_vote_on_player_leave(rf::Player* player);
void server_vote_on_limbo_state_enter();
void process_delayed_kicks();
void kick_player_delayed(rf::Player* player);
bool ends_with(const rf::String& str, const std::string& suffix);
const AlpineServerConfig& server_get_alpine_config();
rf::CmdLineParam& get_ads_cmd_line_param();
rf::CmdLineParam& get_min_cmd_line_param();
void handle_min_param();
const AFGameInfoFlags& server_get_game_info_flags();
void initialize_game_info_server_flags();
void load_ads_server_config();
void launch_alpine_dedicated_server();
void load_additional_server_config(rf::Parser& parser);
