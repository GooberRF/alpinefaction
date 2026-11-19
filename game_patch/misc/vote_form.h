#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include "saved_votes.h"
#include "../multi/vote_client.h"
#include "../ui/mutator_editors.h"

// The Call Vote form: the vote being set up, the server's vote tabs, the saved votes, and calling or saving them. Its
// view changes it in the hit-test pass only, so the draw pass that follows shows what was hit-tested.

enum class VoteFormMode
{
    NotSupported,
    Loading,
    Form,
};

struct VoteTypeInfo
{
    AfVoteType type;
    const char* label;
    const char* description;
};

// The Extend tab's listed lengths, in minutes; any other is typed.
inline constexpr int vote_extend_presets[] = {5, 10, 15, 20, 30, 60};

// One tab. Every tab but Rotation covers exactly one vote type.
struct VoteTab
{
    const char* label = "";
    bool is_rotation = false;
    std::vector<const VoteTypeInfo*> types; // display order; never empty
};

struct VoteMutatorSelection
{
    bool enabled = false;
    std::vector<MutatorEditorValue> options;
};

struct VoteForm
{
    bool built = false;
    // The schema the form was built for: a blob can keep its counts and still reorder or rename mutators.
    uint32_t built_fingerprint = 0;

    // Beside type_index: the Saved tab is there even on a server with no votes enabled.
    bool on_saved_tab = false;

    int type_index = 0;
    // The Rotation tab's vote type, and whether the session's voted rules ride along.
    int rotation_index = 0;
    bool rotation_preserve = true;
    int kick_index = 0;
    std::string level_selection; // level NAME; empty = the "Current level" row
    bool manual_level = false;
    std::string manual_level_name;
    // Typed level-name filter, applied to the level list after the gametype filter.
    std::string level_filter;
    int gametype_index = 0; // index into vote_selectable_gametypes(); the form always picks a concrete type
    int team_size = 4;
    int extend_minutes = af_vote_extend_default_minutes;
    // Typed instead of picked from vote_extend_presets.
    bool extend_manual = false;
    std::string extend_text; // the box's text; shows extend_minutes while not being typed in
    std::vector<VoteMutatorSelection> mutators;

    // Re-derived whenever its context moves, but only until the player touches it.
    bool mutators_touched = false;
    std::string mutators_baseline_key;         // only used on a server too old to push an active set
    uint32_t mutators_baseline_generation = 0; // options generation it came from
    uint32_t mutators_baseline_revision = 0;   // active-set revision it came from

    // Same model for the game type, un-pinned again by its reset.
    bool gametype_touched = false;
    std::string gametype_key;
    bool gametype_key_valid = false;
    bool gametype_key_match = false;      // the key was resolved for a Match vote
    uint32_t gametype_key_generation = 0; // options generation the key was resolved against

    float level_scroll = 0.0f;
    float kick_scroll = 0.0f;

    int saved_selected = -1;
    int saved_hovered = -1; // recomputed on every hit-test pass
    float saved_scroll = 0.0f;
    // One-shot "center the selection" requests, so they cannot fight the wheel.
    bool saved_scroll_to_selection = false;
    bool level_scroll_to_selection = false;
};

struct VoteKickCandidate
{
    uint8_t id = 0xFF;
    std::string name;
};

// Filtered, sorted and deduped rows of the level list plus the selection's row, rebuilt only when one of the inputs
// they were built against changes.
struct VoteLevelList
{
    bool valid = false;
    uint32_t levels_fp = 0;
    bool filter_active = false;
    std::string name_filter;
    uint8_t gametype = af_vote_gametype_none;
    bool allow_current = false;

    std::vector<std::string> items; // display rows, pinned "Current level" included
    int first_level_row = 0;
    int gametype_hidden = 0;    // dropped by the gametype mask (opt-in or enforced)
    int not_allowed_hidden = 0; // dropped by allowed_for_vote (always applied)
    int name_filter_hidden = 0; // dropped by the typed name filter

    // Selection row resolved from the stored name, cached against it.
    bool sel_valid = false;
    std::string sel_name;
    bool sel_manual = false;
    int sel_row = -1;
};

// The most tabs a server can enable: one per vote type, the rotation votes sharing one.
inline constexpr int vote_form_max_tabs = 7;

VoteForm& vote_form();
VoteFormMode vote_form_mode();
void vote_form_on_open();
void vote_form_on_close();
// Drops the whole form (leaving a server). The saved votes stay.
void vote_form_reset();

// A schema the form was not built for rebuilds it, which moves its mutator values.
bool vote_form_needs_build(const VoteOptionsData& options);
void vote_form_ensure(const VoteOptionsData& options);
// Re-derives untouched mutators from what the level in context runs.
void vote_form_sync_baseline(const VoteOptionsData& options);

std::vector<VoteTab> vote_form_tabs();
// A blob refresh disabling a vote type shrinks the list under the form.
void vote_form_clamp_tab(const std::vector<VoteTab>& tabs);
void vote_form_select_tab(int index);
void vote_form_select_saved();
const VoteTypeInfo* vote_tab_type(const VoteTab& tab);
const char* vote_form_no_tabs_text();

std::vector<const VoteGametypeInfo*> vote_selectable_gametypes(const VoteOptionsData& options, bool team_only);
// Pre-selects the game type the picked level runs under until the player picks one, and clamps the pick.
void vote_form_sync_gametype(const VoteOptionsData& options, bool is_match);
void vote_form_pick_gametype(int index);
// Back to the level's own game type, following the level again.
void vote_form_reset_gametype(const VoteOptionsData& options, bool is_match);
// "none" only when the server offered nothing to pick.
uint8_t vote_form_selected_gametype(const std::vector<const VoteGametypeInfo*>& gametypes);
uint8_t vote_form_effective_gametype(const VoteOptionsData& options,
                                     const std::vector<const VoteGametypeInfo*>& gametypes);

// The player's own choice, which a server that restricts levels by prefix overrides.
void vote_form_set_gametype_filter(bool on);
bool vote_form_gametype_filter_active(const VoteOptionsData& options);
// `fix_selection` (hit-test pass) drops a selection the list no longer shows and gives a Level vote a default.
const VoteLevelList& vote_form_level_list(const VoteOptionsData& options, bool is_match, uint8_t gametype,
                                          bool fix_selection);
void vote_form_pick_level_row(int row, bool is_match);
void vote_form_level_filter_changed();

// A mutator a game type does nothing in is shown grayed out, and never sent.
bool vote_mutator_offered(const VoteMutatorSchema& schema, uint8_t game_type);
// Score Limit Override follows the limit `game_type` runs, until the player sets it.
void vote_form_sync_score_limit(const VoteOptionsData& options, uint8_t game_type);
// What the session runs now, which the form follows again.
void vote_form_use_current_mutators(const VoteOptionsData& options);
// The server's base rules, which pins the form.
void vote_form_use_base_mutators(const VoteOptionsData& options);

// Out-of-range and unparsable text keep what was set.
void vote_form_set_extend_minutes(const std::string& text);
// `refresh` (hit-test pass) re-reads the player list, which changes while the form is open.
const std::vector<VoteKickCandidate>& vote_form_kick_candidates(bool refresh);

bool vote_form_can_call(const VoteOptionsData& options);
void vote_form_call(const VoteOptionsData& options);
bool vote_form_can_cancel();
void vote_form_cancel();
bool vote_form_can_save(const VoteOptionsData& options);
// Snapshots the form for the name prompt; false when there is nothing to save.
bool vote_form_begin_save(const VoteOptionsData& options);
// Returns false when the name was empty or the store full.
bool vote_form_finish_save(const std::string& name);
void vote_form_drop_save();

// Hit-test pass: brings the availability of every saved vote up to date and drops a selection that cannot be called.
void vote_form_saved_refresh(const VoteOptionsData* options, VoteFormMode mode);
const SavedVoteAvailability* vote_form_saved_availability(int index);
bool vote_form_saved_callable(int index);
void vote_form_saved_delete(int index);
// The entry the preview shows: the hovered one, else the selection; -1 for none.
int vote_form_saved_previewed();
bool vote_form_saved_can_edit(const VoteOptionsData* options, VoteFormMode mode);
// Loads the selection into the form and moves to its tab.
void vote_form_saved_edit(const VoteOptionsData& options);
bool vote_form_saved_can_call(const VoteOptionsData* options);
void vote_form_saved_call(const VoteOptionsData& options);
std::vector<std::string> vote_form_saved_printout(const SavedVote& vote, const VoteOptionsData* options);
