#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include "../multi/vote_client.h"

// Everything is stored by name, not by ID - other than things like gametype, where
// IDs are well established and can never change.

struct SavedVoteOptionValue
{
    std::string name; // schema option name (toml key)
    MutatorOptionType type = MutatorOptionType::Bool;

    // Only the member matching `type` is meaningful.
    bool bool_value = false;
    int32_t int_value = 0;
    float float_value = 0.0f;
    std::string choice_label;
};

struct SavedVoteMutator
{
    std::string name; // canonical mutator name (VoteMutatorSchema::name)
    std::vector<SavedVoteOptionValue> values;
};

struct SavedVote
{
    std::string name;
    AfVoteType type = AfVoteType::Level;
    std::string level; // Level (required) / Match (empty = keep the current level)
    uint8_t gametype = af_vote_gametype_none;
    uint8_t team_size = 4;
    uint8_t extend_minutes = af_vote_extend_default_minutes;
    std::vector<SavedVoteMutator> mutators;
};

// The only three types worth saving: everything else is either parameterless
// (nothing to snapshot) or aimed at a player who will not be there later.
bool saved_vote_type_is_savable(AfVoteType type);

// Trimmed, with '|' and control characters dropped and the result truncated to
// the name cap.
std::string saved_vote_sanitize_name(std::string_view name);

const std::vector<SavedVote>& saved_votes_get();
// Sanitizes the name, auto-renames on a case-insensitive clash
// appends and persists. Returns the index of the new entry, or -1 when the
// store is full -- callers must treat that as a no-op.
int saved_votes_add(SavedVote vote);
// Append during a settings load: same sanitize/auto-rename, but does NOT write
// the settings file back (it is mid-parse).
void saved_votes_load_add(SavedVote vote);
void saved_votes_delete(size_t index);
// Drops every entry (and every preserved unparsed line) without persisting, so
// reloading the settings file cannot duplicate the store. Never called on
// disconnect: the store is user data.
void saved_votes_clear();
// The store refuses further entries. The settings loader stops parsing here and
// hands the remaining lines to saved_votes_add_unparsed instead of losing them.
bool saved_votes_is_full();

// Bumped by every mutation of the store (add, load-add, delete, clear). A
// consumer that derives per-entry state (the panel's availability cache) watches
// this instead of recomputing it every frame.
uint32_t saved_votes_revision();

// Raw SavedVoteN values this build could not parse, or that arrived after the
// store filled up. They are written back out verbatim, keyed AFTER the valid
// entries, so a hand-edit typo (or a line only a newer build understands) is
// never destroyed and can never collide with a reindexed real entry -- which is
// what relying on the generic [OrphanedSettings] mechanism would have done,
// since that section is written above [SavedVotes] and the store reindexes
// from 0.
void saved_votes_add_unparsed(std::string encoded);
const std::vector<std::string>& saved_votes_unparsed();

bool saved_vote_parse(std::string_view encoded, SavedVote& out);
std::string saved_vote_encode(const SavedVote& vote);

// Why a saved vote cannot be called here, if it cannot. STRICT: every component
// (type, level, game type, mutator, option, choice) has to exist on the current
// server, so a saved vote either reproduces exactly what was saved or is refused
// with reasons rather than being silently degraded.
struct SavedVoteAvailability
{
    bool callable = false;
    std::vector<std::string> reasons;
};

SavedVoteAvailability saved_vote_check(const SavedVote& vote, const VoteOptionsData* options, bool server_supported);

// Names resolved to the current schema's ids/indices. Only meaningful for an
// entry saved_vote_check has cleared.
AfVoteCallParams saved_vote_build_params(const SavedVote& vote, const VoteOptionsData& options);
