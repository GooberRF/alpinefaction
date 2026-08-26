#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// One source of truth for the server's base config (mutators + gametype settings + a few
// flags), shared verbatim by the afstats event stream and the demo recorder so both report
// the same data. afstats serializes this to JSON for FactionFiles; the demo recorder encodes
// it into a versioned binary block (see demo/demo_server_config.h).
namespace server_config {

using SettingValue = std::variant<bool, int64_t, double, std::string>;

struct MutatorRecord
{
    std::string name;
    std::vector<std::pair<std::string, SettingValue>> settings;
};

struct ServerConfigSnapshot
{
    uint8_t rf_flags = 0;
    uint32_t gi_flags = 0;
    uint8_t match_state = 0;
    std::vector<MutatorRecord> mutators;
    std::vector<std::pair<std::string, SettingValue>> gametype_settings;
};

// Server settings reported by afstats only; deliberately outside ServerConfigSnapshot so the
// demo recorder's binary block and its version are untouched. Initializers mirror the real
// config defaults, so a default-constructed record equals a default server config.
struct ServerSettingsRecord
{
    double pvp_damage_modifier = 1.0;
    int64_t click_limiter_cooldown_ms = 90;
    bool use_sp_damage_calculation = false;
    bool flag_dropping = true;
    bool flag_captures_while_stolen = false;
    bool drop_amps = false;
    bool drop_weapons = true;
    bool weapon_items_give_full_ammo = false;
    bool weapon_infinite_magazines = false;
    bool spawn_respect_team_spawns = true;
    bool spawn_try_avoid_players = true;
    bool spawn_always_avoid_last = false;
    bool spawn_always_use_furthest = false;
    bool spawn_only_avoid_enemies = false;
    bool spawn_dynamic_respawns = false;
    std::vector<std::pair<std::string, int64_t>> dynamic_respawn_items;
    bool spawn_protection_enabled = false;
    int64_t spawn_protection_duration_ms = 1500;
    bool spawn_protection_use_powerup = false;
    bool force_character_enabled = false;
    int64_t force_character_index = 0;
    std::string force_character_name = "enviro_parker";
};

// Byte cap for operator-authored strings (mutator names, option keys, string values).
constexpr size_t k_max_string_len = 64;

// Guarantees valid UTF-8 output: every codepoint that reaches the output is a whole,
// well-formed sequence. max_len is a byte cap; truncation stops on a sequence boundary so a
// codepoint is never bisected. C0 controls and DEL are stripped.
std::string sanitize_string(std::string_view in, size_t max_len);

// Fills every field from current server state. Server-side only (reads live netgame/config).
ServerConfigSnapshot capture_server_config_snapshot();

// Same, for the settings afstats reports on game_start. Server-side only.
ServerSettingsRecord capture_server_settings();

} // namespace server_config
