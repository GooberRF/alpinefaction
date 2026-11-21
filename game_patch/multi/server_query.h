#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "network.h"
#include "../rf/multi.h"

// Wire formats of the out-of-band server queries: game_info, Pure Faction's player list (as Alpine
// servers answer it) and the tracker's server list. Every parser takes untrusted input.

constexpr uint16_t default_server_port = 7755;

// gi_req_ext_ver of Alpine's game_info_request tail: 4 asks for the v2 extension with bot counts.
constexpr uint8_t af_game_info_req_ext_ver = 4;

// A game_info payload (after the packet header).
struct GameInfoAnswer
{
    // False for a protocol this build cannot join; nothing else is read then.
    bool compatible = false;
    std::string name;
    std::string level;
    std::string mod;
    uint8_t game_type = 0; // RF_GT_UNK for a type this build does not know
    bool unknown_game_type = false;
    uint8_t players = 0;
    uint8_t max_players = 0;
    uint8_t flags = 0; // RF_SF_*
    std::optional<AFGameInfoExtra> af;
};

std::optional<GameInfoAnswer> parse_game_info(std::span<const uint8_t> payload);

// One player of a pf_players answer.
struct ServerRosterPlayer
{
    uint8_t flags = 0; // af_player_info_flags
    int16_t score = 0;
    uint16_t kills = 0;
    uint16_t deaths = 0;
    uint16_t caps = 0;
    std::string name;
};

// A reassembled pf_players answer (af_send_player_info_response).
struct ServerRoster
{
    uint16_t red_score = 0;
    uint16_t blue_score = 0;
    uint32_t time_left_seconds = 0; // UINT32_MAX: no time limit
    uint32_t af_flags = 0;
    uint8_t game_type = 0;
    std::string level_filename;
    std::vector<ServerRosterPlayer> players;
};

struct PfPlayersSegment
{
    uint8_t response_id = 0;
    uint8_t sequence = 0;
    uint8_t total_segments = 0;
    std::span<const uint8_t> slice;
};

// The 0xA1 packet's payload (after the packet header); the slice points into `payload`.
std::optional<PfPlayersSegment> parse_pf_players_segment(std::span<const uint8_t> payload);
// The segments' slices joined in sequence order.
std::optional<ServerRoster> parse_pf_players(std::span<const uint8_t> joined);

struct TrackerPacket
{
    uint16_t type = 0; // RF_TrackerPacketType
    uint32_t seq = 0;
    std::vector<rf::NetAddr> servers; // RF_TPT_SERVER_LIST only
};

// A whole datagram, main packet type included.
std::optional<TrackerPacket> parse_tracker_packet(std::span<const uint8_t> datagram);

// Whole datagrams, main packet type included.
std::vector<uint8_t> make_game_info_request();
std::vector<uint8_t> make_pf_players_request();
std::vector<uint8_t> make_tracker_list_request();
std::vector<uint8_t> make_tracker_ack(uint32_t seq);

// "a.b.c.d" or "a.b.c.d:port" (default_server_port without one). Host names are refused.
std::optional<rf::NetAddr> parse_server_address(std::string_view text);
