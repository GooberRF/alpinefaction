#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>
#include "alpine_packets.h"

// Client-side mirror of the server's vote system: the vote-options schema blob
// (what can be voted for) and the state of the vote currently running.

struct VoteGametypeInfo
{
    uint8_t id = 0; // rf::NetGameType
    bool is_team_type = false;
    std::string name;
};

struct VoteMutatorOptionSchema
{
    uint8_t id = 0;
    std::string name;  // toml key
    std::string label; // UI text
    MutatorOptionType type = MutatorOptionType::Bool;

    // Only the member matching `type` is meaningful.
    bool default_bool = false;
    uint8_t default_choice = 0;
    int32_t default_int = 0;
    float default_float = 0.0f;
    std::string default_string;

    std::vector<std::string> choices; // Choice only
};

struct VoteMutatorSchema
{
    uint8_t id = 0;
    std::string name;  // canonical, keys the client-local description table
    std::string label; // UI text
    std::vector<VoteMutatorOptionSchema> options;
};

struct VoteLevelInfo
{
    std::string filename;
    // Game type this level runs with when the vote picks "Server default".
    uint8_t natural_gametype = 0; // rf::NetGameType
    // bit N (matching NetGameType N) = this level matches that game type's level
    // prefix rules (computed server-side; prefix rules are server-only
    // knowledge). Always populated, even when the server does not enforce them —
    // see VoteOptionsData::gametype_prefix_restricted for whether it does.
    uint32_t valid_gametype_mask = 0;
};

struct VoteOptionsData
{
    uint16_t enabled_vote_mask = 0; // bit N = AfVoteType N is enabled
    // vote_level.only_allow_gametype_prefix is on: the server will REJECT a vote
    // whose level fails valid_gametype_mask, so the filter must be applied. When
    // false the mask is still populated and a client may offer it as an opt-in
    // filter, but off-prefix votes are accepted.
    bool gametype_prefix_restricted = false;
    std::vector<VoteGametypeInfo> gametypes;
    std::vector<VoteMutatorSchema> mutators;
    std::vector<VoteLevelInfo> levels; // rotation order, then vote-allowed extras
};

// Does this level match the given game type's level prefix rules? Whether that
// is a hard restriction depends on VoteOptionsData::gametype_prefix_restricted.
bool vote_level_allows_gametype(const VoteLevelInfo& level, uint8_t game_type);
// Same, for the "Server default" (no gametype override) selection.
bool vote_level_allows_default_gametype(const VoteLevelInfo& level);

struct ActiveVoteState
{
    AfVoteType type = AfVoteType::Kick;
    std::string title;          // server-composed, display only
    std::string initiator_name; // empty if the server couldn't name the owner
    uint8_t yes = 0;
    uint8_t no = 0;
    uint8_t remaining = 0;
    int64_t end_timestamp_ms = 0; // timer::get_i64(1000) based
    bool is_owner = false;
    bool has_voted = false; // set locally when this client casts
};

// --- vote options cache ---
bool vote_options_are_loaded();
const VoteOptionsData* vote_options_get();
bool vote_options_is_type_enabled(AfVoteType type);
// Ask the server for the blob if it isn't loaded (or went stale). Rate limited.
void vote_options_request_if_needed();
void vote_options_mark_stale();
void vote_options_handle_chunk(uint8_t generation, uint8_t seq, uint8_t total,
                               const uint8_t* data, size_t len);

// --- active vote state ---
const std::optional<ActiveVoteState>& vote_state_get();
int vote_state_seconds_remaining();
void vote_state_mark_local_voted();
void vote_state_on_start(AfVoteType type, uint16_t time_remaining_sec, uint8_t yes, uint8_t no,
                         uint8_t remaining, bool is_owner, std::string initiator_name,
                         std::string title);
void vote_state_on_update(uint8_t yes, uint8_t no, uint8_t remaining);
void vote_state_on_end(AfVoteResult result);

// Drop every cached vote thing (leaving a server).
void vote_client_reset();
