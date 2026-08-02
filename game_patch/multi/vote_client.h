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

// One option value of a mutator the server has declared in its config, as
// opposed to the schema's factory defaults above. Same value-field shape as
// VoteMutatorOptionSchema: only the member matching `type` is meaningful.
struct VoteMutatorDeclValue
{
    uint8_t option_id = 0;
    MutatorOptionType type = MutatorOptionType::Bool;

    bool bool_value = false;
    uint8_t choice_index = 0;
    int32_t int_value = 0;
    float float_value = 0.0f;
    std::string string_value;
};

// One config-declared mutator. The panel pre-selects these so that submitting an
// untouched vote reproduces what the level would run anyway (votes replace the
// configured mutator set rather than stacking on it).
struct VoteMutatorDecl
{
    uint8_t mutator_id = 0;
    std::vector<VoteMutatorDeclValue> values;
};

struct VoteLevelInfo
{
    std::string filename;
    // Game type this level runs with when the vote picks "Server default".
    uint8_t natural_gametype = 0; // rf::NetGameType
    // bit N (matching NetGameType N) = this level matches that game type's level
    // prefix rules.
    uint32_t valid_gametype_mask = 0;
    // The server's vote_level allow-list accepts this level.
    bool allowed_for_vote = true;
    // Mutators this level is configured to run. `nullopt` means it inherits
    // VoteOptionsData::base_mutator_decls; an empty vector means it explicitly
    // runs none. Also nullopt for a server built before the blob carried this.
    std::optional<std::vector<VoteMutatorDecl>> mutator_decls;
};

struct VoteOptionsData
{
    // bit N = AfVoteType N is enabled.
    // Bits for types this build does not know are simply never queried.
    uint32_t enabled_vote_mask = 0;
    // vote_level.only_allow_gametype_prefix is on: the server will REJECT a vote
    // whose level fails valid_gametype_mask, so the filter must be applied. When
    // false the mask is still populated and a client may offer it as an opt-in
    // filter, but off-prefix votes are accepted.
    bool gametype_prefix_restricted = false;
    bool rotation_preserve_supported = false;
    std::vector<VoteGametypeInfo> gametypes;
    std::vector<VoteMutatorSchema> mutators;
    std::vector<VoteLevelInfo> levels; // rotation order, then vote-allowed extras
    // Mutators the server's base rules declare; the baseline for every level that
    // does not carry its own. Empty for a server built before the blob carried it.
    std::vector<VoteMutatorDecl> base_mutator_decls;
};

// Does this level match the given game type's level prefix rules? Whether that
// is a hard restriction depends on VoteOptionsData::gametype_prefix_restricted.
bool vote_level_allows_gametype(const VoteLevelInfo& level, uint8_t game_type);
// Same, for the "Server default" (no gametype override) selection.
bool vote_level_allows_default_gametype(const VoteLevelInfo& level);

struct ActiveVoteState
{
    // The RAW wire byte, deliberately NOT an AfVoteType: a newer server may run a
    // vote type added after this build, and an old client must still show it.
    uint8_t type_raw = static_cast<uint8_t>(AfVoteType::Kick);
    std::string title;          // server-composed, display only
    std::string initiator_name; // empty if the server couldn't name the owner
    uint8_t yes = 0;
    uint8_t no = 0;
    uint8_t remaining = 0;
    int64_t end_timestamp_ms = 0; // timer::get_i64(1000) based
    bool is_owner = false;
    bool has_voted = false; // set locally when this client casts

    // The vote type when this build recognises it. `nullopt` means the server is
    // running a type added after this build: everything else about the vote still
    // works, so any type-keyed presentation must degrade to a neutral fallback
    // (normally just `title`) rather than refusing to display the vote. Going
    // through this accessor is what keeps a `switch` on the type honest.
    [[nodiscard]] std::optional<AfVoteType> known_type() const
    {
        if (type_raw >= af_vote_type_count) {
            return std::nullopt;
        }
        return static_cast<AfVoteType>(type_raw);
    }
};

// --- vote options cache ---
// True once a blob has been parsed. A cache marked stale still answers true: the
// data stays usable while a refresh is in flight, because the server answers a
// refresh request with nothing at all when its generation hasn't changed.
bool vote_options_are_loaded();
const VoteOptionsData* vote_options_get();
// Generation of the loaded data, 0 until a blob has been parsed. A consumer that
// derives state from the options watches this so a background refresh can be
// picked up without re-deriving every frame.
uint32_t vote_options_loaded_generation();
bool vote_options_is_type_enabled(AfVoteType type);
// Ask the server for the blob if it isn't loaded (or went stale). Rate limited.
void vote_options_request_if_needed();
void vote_options_mark_stale();

// Blob stream (af_sreq_vote_options_data). Ordered reliable delivery, so Begin ->
// Data* -> End arrive in that order; anything out of order is a protocol error and
// discards the stream.
void vote_options_stream_begin(uint32_t generation, uint32_t total_bytes);
void vote_options_stream_data(uint32_t generation, const uint8_t* data, size_t len);
void vote_options_stream_end(uint32_t generation);

// --- active vote state ---
const std::optional<ActiveVoteState>& vote_state_get();
int vote_state_seconds_remaining();
void vote_state_mark_local_voted();
// `type_raw` is the unvalidated wire byte on purpose; see ActiveVoteState.
void vote_state_on_start(uint8_t type_raw, uint16_t time_remaining_sec, uint8_t yes, uint8_t no,
                         uint8_t remaining, bool is_owner, bool is_sync, std::string initiator_name,
                         std::string title);
void vote_state_on_update(uint8_t yes, uint8_t no, uint8_t remaining);
// `passed` is independent of `result`: a timed-out vote can still pass. `detail`
// is the server-composed outcome line (empty falls back to generic wording).
void vote_state_on_end(AfVoteResult result, bool passed, std::string detail);

// Drop every cached vote thing (leaving a server).
void vote_client_reset();
