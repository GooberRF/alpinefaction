#pragma once

#include <cstdint>

constexpr uint32_t AFD_MAGIC = 0x4D444641; // "AFDM" little-endian
constexpr uint16_t AFD_FORMAT_MAJOR = 1;
// 1.1: team-scoped packet flags (DEMO_PKT_TEAM_*); 1.2: required_features TLV;
// 1.3: server_info record (0x02) — additive, display-only, no required_features bit needed;
// 1.4: afstats identity TLVs (afstats_session_id + afstats_game) — additive, display-only,
// stamped on auto-recorded segments for FactionFiles upload attribution
constexpr uint16_t AFD_FORMAT_MINOR = 4;

// Feature bits this build understands. A demo whose required_features TLV has any other
// bit set is refused (missing_features). Bits are permanent like TLV ids: define new ones
// here (from bit 0 upward) as must-understand features are added, and never reuse them.
constexpr uint32_t AFD_KNOWN_FEATURES = 0;

enum class DemoHeaderTlvType : uint8_t
{
    af_version_major = 0x01,
    af_version_minor = 0x02,
    af_version_patch = 0x03,
    level_filename = 0x04,
    level_checksum = 0x05,
    game_type = 0x06,
    mod_name = 0x07,
    server_name = 0x08,
    server_netfps = 0x09,
    start_time_unix = 0x0A,
    server_max_players = 0x0B,
    demo_player_id = 0x0C,
    required_features = 0x0D, // u32 bitmask; unknown bits => missing_features on open
    afstats_session_id = 0x0E, // string; the afstats session the game was reported under (empty = none)
    afstats_game = 0x0F,       // u32; the afstats per-session game counter (0 = none)
};
