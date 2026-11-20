#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <format>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <common/utils/list-utils.h>
#include <common/utils/string-utils.h>
#include "vote_form.h"
#include "player.h"
#include "../multi/alpine_packets.h"
#include "../multi/multi.h"
#include "../rf/level.h"
#include "../rf/multi.h"
#include "../rf/player/player.h"

namespace
{

VoteForm g_form;

// Session only; a server that restricts levels by prefix forces it on.
bool g_filter_gametype_local = false;

VoteLevelList g_level_cache;

// Refreshed by the hit-test pass for the draw pass that follows.
std::vector<VoteKickCandidate> g_kick_cache;

// Checking every entry against the whole blob is real work, so it is rebuilt only when its key moves.
std::vector<SavedVoteAvailability> g_saved_availability;

struct SavedAvailabilityKey
{
    bool valid = false; // false forces the first rebuild after a reset
    uint32_t store_revision = 0;
    uint32_t options_generation = 0;
    bool has_options = false;
    bool server_supported = false;
    // The Match "current level" row is checked against the running level and type.
    std::string level;
    uint8_t gametype = 0;

    bool operator==(const SavedAvailabilityKey&) const = default;
};

SavedAvailabilityKey g_saved_availability_key;

// Taken on the SAVE click: the blob can refresh before the name prompt returns.
SavedVote g_pending_save;
bool g_pending_save_valid = false;

// Case-insensitive, with the separator characters dropped from both sides.
std::string level_filter_normalize(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        if (c == '-' || c == '_' || c == ' ') {
            continue;
        }
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

bool level_matches_name_filter(const std::string& filename)
{
    if (g_form.level_filter.empty()) {
        return true;
    }
    return level_filter_normalize(filename).find(level_filter_normalize(g_form.level_filter)) != std::string::npos;
}

// Allocation-free: a name's length and end bytes stand in for it, which no real map-list change keeps along with the
// flags.
uint32_t level_list_fingerprint(const VoteOptionsData& options)
{
    uint32_t h = 2166136261u; // FNV-1a
    const auto mix = [&h](uint32_t v) {
        h = (h ^ v) * 16777619u;
    };
    mix(static_cast<uint32_t>(options.levels.size()));
    for (const auto& level : options.levels) {
        mix(level.natural_gametype);
        mix(level.valid_gametype_mask);
        mix(level.allowed_for_vote ? 1u : 0u);
        mix(static_cast<uint32_t>(level.filename.size()));
        if (!level.filename.empty()) {
            mix(static_cast<uint8_t>(level.filename.front()));
            mix(static_cast<uint8_t>(level.filename.back()));
        }
    }
    return h;
}

// Display order of the vote types; vote_form_tabs() preserves it and the sequence
// flows up to fill gaps left by disabled types.
constexpr VoteTypeInfo vote_type_infos[] = {
    {AfVoteType::Level, "Level", "Load a specific level, optionally with a game type and mutators."},
    {AfVoteType::Restart, "Restart", "Restart the level that is currently loaded."},
    {AfVoteType::Next, "Next", "Load the next level in the server's rotation."},
    {AfVoteType::Previous, "Previous", "Load the previous level in the server's rotation."},
    {AfVoteType::Random, "Random", "Load a random level from the server's rotation."},
    {AfVoteType::Extend, "Extend", "Extend the time limit of the round in progress."},
    {AfVoteType::Match, "Match", "Set up a match on the chosen level with the chosen team size."},
    {AfVoteType::CancelMatch, "Cancel Match", "Cancel the match that is currently set up."},
    {AfVoteType::Surrender, "Surrender", "Concede the match. Passes only if every player on your team agrees."},
    {AfVoteType::Kick, "Kick", "Kick the selected player from the server."},
};

// Short tag for a game type. Deliberately not multi_game_type_name_short(): that
// maps every unrecognized id to "TDM" and logs a warning, whereas a game type this
// build predates has to fall back to the server-sent name.
const char* gametype_short_name(uint8_t id)
{
    switch (static_cast<rf::NetGameType>(id)) {
        case rf::NG_TYPE_DM: return "DM";
        case rf::NG_TYPE_CTF: return "CTF";
        case rf::NG_TYPE_TEAMDM: return "TDM";
        case rf::NG_TYPE_KOTH: return "KOTH";
        case rf::NG_TYPE_DC: return "DC";
        case rf::NG_TYPE_REV: return "REV";
        case rf::NG_TYPE_RUN: return "RUN";
        case rf::NG_TYPE_ESC: return "ESC";
        case rf::NG_TYPE_BAG: return "BAG";
        case rf::NG_TYPE_TBAG: return "TBAG";
        case rf::NG_TYPE_PIT: return "PIT";
        case rf::NG_TYPE_WO: return "WO";
        case rf::NG_TYPE_GG: return "GG";
        case rf::NG_TYPE_SAL: return "SAL";
        case rf::NG_TYPE_SCAV: return "SCAV";
        case rf::NG_TYPE_PAY: return "PAY";
        default: return nullptr; // unknown to this build
    }
}

// The four level-rotation votes share one tab; the form picks which of them is
// actually called.
constexpr bool is_rotation_vote_type(AfVoteType type)
{
    return type == AfVoteType::Restart || type == AfVoteType::Next
        || type == AfVoteType::Previous || type == AfVoteType::Random;
}

static_assert(std::ssize(vote_type_infos) + 1
                  - std::ranges::count_if(vote_type_infos,
                                          [](const VoteTypeInfo& info) { return is_rotation_vote_type(info.type); })
              == vote_form_max_tabs);

// Null on the Saved tab or when the list has shrunk under the form.
const VoteTab* selected_tab(const std::vector<VoteTab>& tabs)
{
    if (g_form.on_saved_tab || g_form.type_index < 0 || g_form.type_index >= static_cast<int>(tabs.size())) {
        return nullptr;
    }
    return &tabs[g_form.type_index];
}

const VoteTypeInfo* selected_type()
{
    const auto tabs = vote_form_tabs();
    const VoteTab* tab = selected_tab(tabs);
    return tab ? vote_tab_type(*tab) : nullptr;
}

void clear_pending_save()
{
    g_pending_save = SavedVote{};
    g_pending_save_valid = false;
}

std::vector<VoteKickCandidate> build_kick_candidates()
{
    std::vector<VoteKickCandidate> out;
    if (!rf::player_list) {
        return out;
    }
    for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
        if (&player == rf::local_player || !player.net_data || player.is_bot) {
            continue;
        }
        out.push_back(VoteKickCandidate{player.net_data->player_id, player.name.c_str()});
    }
    return out;
}

// Cheap order-sensitive hash over the parts of the schema the form mirrors:
// gametype ids, and every mutator id plus each option's id and type.
uint32_t schema_fingerprint(const VoteOptionsData& options)
{
    uint32_t h = 2166136261u; // FNV-1a
    const auto mix = [&h](uint32_t v) {
        h = (h ^ v) * 16777619u;
    };
    mix(static_cast<uint32_t>(options.gametypes.size()));
    for (const auto& gametype : options.gametypes) {
        mix(gametype.id);
    }
    mix(static_cast<uint32_t>(options.mutators.size()));
    for (const auto& mutator : options.mutators) {
        mix(mutator.id);
        mix(static_cast<uint32_t>(mutator.options.size()));
        for (const auto& option : mutator.options) {
            mix(option.id);
            mix(static_cast<uint32_t>(option.type));
        }
    }
    return h;
}

// The level string sent on the wire. Tracked by NAME so a re-sort or blob refresh
// cannot desync it; empty is Match's "keep the current level".
std::string selected_level()
{
    if (g_form.manual_level) {
        return g_form.manual_level_name;
    }
    return g_form.level_selection;
}

std::vector<VoteMutatorDecl> resolve_baseline(const VoteOptionsData& options, const std::string& level_string)
{
    // What the session runs now, so an untouched form reproduces it. The level only
    // matters on a server too old to push an active set.
    if (const std::vector<VoteMutatorDecl>* active = vote_active_mutators_get()) {
        return *active;
    }

    // Empty is Match's "Current level" row. Normalized like a voted level name.
    const std::string wanted = normalize_level_filename(level_string.empty()
        ? std::string_view{rf::level.filename.c_str()}
        : std::string_view{level_string});

    for (const auto& level : options.levels) {
        if (string_iequals(level.filename, wanted)) {
            return level.mutator_decls ? *level.mutator_decls : options.base_mutator_decls;
        }
    }
    // A manually named level outside the server's list runs on the base rules.
    return options.base_mutator_decls;
}

// Pin the context so the pre-selection stops re-deriving over the form.
void pin_mutator_baseline_context()
{
    g_form.mutators_baseline_key = selected_level();
    g_form.mutators_baseline_generation = vote_options_loaded_generation();
    g_form.mutators_baseline_revision = vote_active_mutators_revision();
}

void pin_gametype_context(bool is_match)
{
    g_form.gametype_key = selected_level();
    g_form.gametype_key_match = is_match;
    g_form.gametype_key_generation = vote_options_loaded_generation();
    g_form.gametype_key_valid = true;
}

void reset_gametype_selection()
{
    g_form.gametype_touched = false;
    g_form.gametype_key_valid = false;
    g_form.gametype_index = 0;
}

// Every mutator deselected and back on the schema (factory) defaults. Shared by
// apply_baseline and the saved-vote load, which both need a clean slate first.
void reset_mutators_to_defaults(const VoteOptionsData& options)
{
    for (size_t i = 0; i < options.mutators.size() && i < g_form.mutators.size(); ++i) {
        const VoteMutatorSchema& schema = options.mutators[i];
        VoteMutatorSelection& selection = g_form.mutators[i];

        selection.enabled = false;
        const size_t option_count = std::min(schema.options.size(), selection.options.size());
        for (size_t o = 0; o < option_count; ++o) {
            const VoteMutatorOptionSchema& option = schema.options[o];
            MutatorEditorValue& value = selection.options[o];
            value.bool_value = option.default_bool;
            value.choice_index = option.default_choice;
            value.int_value = option.default_int;
            value.float_value = option.default_float;
            value.player_set = false;
        }
    }
}

// Reset every mutator to the schema (factory) defaults, then switch on the ones
// the baseline declares and overlay their configured values.
void apply_baseline(const VoteOptionsData& options, const std::vector<VoteMutatorDecl>& baseline)
{
    reset_mutators_to_defaults(options);

    for (size_t i = 0; i < options.mutators.size() && i < g_form.mutators.size(); ++i) {
        const VoteMutatorSchema& schema = options.mutators[i];
        VoteMutatorSelection& selection = g_form.mutators[i];
        const size_t option_count = std::min(schema.options.size(), selection.options.size());

        const VoteMutatorDecl* decl = nullptr;
        for (const auto& candidate : baseline) {
            if (candidate.mutator_id == schema.id) {
                decl = &candidate;
                break;
            }
        }
        // Baseline entries for a mutator this schema doesn't list are ignored:
        // there is no row to select.
        selection.enabled = decl != nullptr;
        if (!decl) {
            continue;
        }

        for (size_t o = 0; o < option_count; ++o) {
            const VoteMutatorOptionSchema& option = schema.options[o];
            MutatorEditorValue& value = selection.options[o];
            for (const auto& declared : decl->values) {
                // A value whose type disagrees with the schema keeps the default:
                // writing it into the wrong slot would send nonsense back.
                if (declared.option_id != option.id || declared.type != option.type) {
                    continue;
                }
                switch (option.type) {
                    case MutatorOptionType::Bool:
                        value.bool_value = declared.bool_value;
                        break;
                    case MutatorOptionType::Choice:
                        // A choice list that shrank since the config was written
                        // would otherwise leave an index with nothing to display.
                        if (declared.choice_index < option.choices.size()) {
                            value.choice_index = declared.choice_index;
                        }
                        break;
                    case MutatorOptionType::Int:
                        value.int_value = declared.int_value;
                        value.player_set = true;
                        break;
                    case MutatorOptionType::Float:
                        value.float_value = declared.float_value;
                        value.player_set = true;
                        break;
                    default:
                        // String has no editor and no slot in MutatorEditorValue, so
                        // the server's default applies to whatever the vote sends.
                        break;
                }
                break;
            }
        }
    }
}

void build_form(const VoteOptionsData& options)
{
    g_form.built = true;
    g_form.built_fingerprint = schema_fingerprint(options);

    g_form.type_index = 0;
    g_form.rotation_index = 0;
    g_form.rotation_preserve = true;
    g_form.kick_index = 0;
    g_form.level_selection.clear();
    g_form.manual_level = false;
    g_form.manual_level_name.clear();
    g_form.team_size = 4;
    g_form.extend_minutes = af_vote_extend_default_minutes;
    g_form.extend_manual = false;
    g_form.level_scroll = 0.0f;
    g_form.kick_scroll = 0.0f;

    g_form.mutators.clear();
    g_form.mutators.reserve(options.mutators.size());
    for (const auto& mutator : options.mutators) {
        VoteMutatorSelection selection;
        selection.options.reserve(mutator.options.size());
        for (const auto& option : mutator.options) {
            MutatorEditorValue value;
            value.bool_value = option.default_bool;
            value.choice_index = option.default_choice;
            value.int_value = option.default_int;
            value.float_value = option.default_float;
            selection.options.push_back(value);
        }
        g_form.mutators.push_back(std::move(selection));
    }

    // Pre-select what the level in context runs anyway. Every selection field was
    // just reset, so selected_level() is empty here and this resolves against the
    // current level; vote_form_sync_baseline re-derives it as soon as that changes.
    apply_baseline(options, resolve_baseline(options, selected_level()));
    g_form.mutators_touched = false;
    pin_mutator_baseline_context();
    reset_gametype_selection();
}

uint8_t selected_gametype(const VoteOptionsData& options, bool team_only)
{
    return vote_form_selected_gametype(vote_selectable_gametypes(options, team_only));
}

uint8_t effective_gametype(const VoteOptionsData& options, bool team_only)
{
    return vote_form_effective_gametype(options, vote_selectable_gametypes(options, team_only));
}

// The game type a level selection pre-selects. Match's "Current level" row (an
// empty name) goes through the shared derivation, so a saved record of that row
// resolves to the same byte this form submits.
uint8_t form_default_gametype(const VoteOptionsData& options, bool is_match, const std::string& level)
{
    if (is_match && level.empty()) {
        return vote_match_current_level_gametype(options);
    }
    return vote_default_gametype_for_level(options, level);
}

int gametype_index_in(const std::vector<const VoteGametypeInfo*>& gametypes, uint8_t game_type)
{
    for (size_t i = 0; i < gametypes.size(); ++i) {
        if (gametypes[i]->id == game_type) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// The pick index of `game_type`; when this vote does not offer it, TeamDM for a match vote
// or for DM, else `current_index` held rather than moved under a player who did not ask.
int gametype_pick_index(const std::vector<const VoteGametypeInfo*>& gametypes, bool team_only,
                        uint8_t game_type, int current_index)
{
    if (const int index = gametype_index_in(gametypes, game_type); index >= 0) {
        return index;
    }
    if (team_only || game_type == static_cast<uint8_t>(rf::NG_TYPE_DM)) {
        const int team_dm = gametype_index_in(gametypes, static_cast<uint8_t>(rf::NG_TYPE_TEAMDM));
        if (team_dm >= 0) {
            return team_dm;
        }
    }
    const int count = static_cast<int>(gametypes.size());
    return count > 0 ? std::clamp(current_index, 0, count - 1) : 0;
}

int gametype_pick_index(const VoteOptionsData& options, bool team_only, uint8_t game_type, int current_index)
{
    return gametype_pick_index(vote_selectable_gametypes(options, team_only), team_only, game_type, current_index);
}

std::vector<VoteMutatorInput> build_mutator_inputs(const VoteOptionsData& options, uint8_t game_type)
{
    std::vector<VoteMutatorInput> out;
    for (size_t i = 0; i < options.mutators.size() && i < g_form.mutators.size(); ++i) {
        // Never send a mutator the form is graying out, but it may still be
        // checked from a pre-selection or an earlier game type choice.
        if (!g_form.mutators[i].enabled || !vote_mutator_offered(options.mutators[i], game_type)) {
            continue;
        }
        const VoteMutatorSchema& schema = options.mutators[i];
        VoteMutatorInput input;
        input.mutator_id = schema.id;
        for (size_t o = 0; o < schema.options.size() && o < g_form.mutators[i].options.size(); ++o) {
            const VoteMutatorOptionSchema& option = schema.options[o];
            const MutatorEditorValue& value = g_form.mutators[i].options[o];
            VoteMutatorOptionInput option_input;
            option_input.option_id = option.id;
            option_input.type = option.type;
            option_input.bool_value = value.bool_value;
            option_input.choice_index = value.choice_index;
            option_input.int_value = value.int_value;
            option_input.float_value = value.float_value;
            input.options.push_back(std::move(option_input));
        }
        out.push_back(std::move(input));
    }
    return out;
}

// Enabled mutators as names/labels rather than the schema's ids, so the entry
// stays meaningful on a server whose blob numbers them differently.
std::vector<SavedVoteMutator> build_saved_mutators(const VoteOptionsData& options, uint8_t game_type)
{
    std::vector<SavedVoteMutator> out;
    for (size_t i = 0; i < options.mutators.size() && i < g_form.mutators.size(); ++i) {
        // Same filter build_mutator_inputs uses.
        if (!g_form.mutators[i].enabled || !vote_mutator_offered(options.mutators[i], game_type)) {
            continue;
        }
        const VoteMutatorSchema& schema = options.mutators[i];
        if (schema.name.empty()) {
            continue;
        }
        SavedVoteMutator mutator;
        mutator.name = schema.name;
        for (size_t o = 0; o < schema.options.size() && o < g_form.mutators[i].options.size(); ++o) {
            const VoteMutatorOptionSchema& option = schema.options[o];
            const MutatorEditorValue& value = g_form.mutators[i].options[o];
            if (option.name.empty()) {
                continue; // the parser refuses a nameless option
            }
            SavedVoteOptionValue saved;
            saved.name = option.name;
            saved.type = option.type;
            switch (option.type) {
                case MutatorOptionType::Bool:
                    saved.bool_value = value.bool_value;
                    break;
                case MutatorOptionType::Int:
                    saved.int_value = value.int_value;
                    break;
                case MutatorOptionType::Float:
                    saved.float_value = value.float_value;
                    break;
                case MutatorOptionType::Choice:
                    if (value.choice_index >= option.choices.size()) {
                        continue; // no label to store
                    }
                    saved.choice_label = option.choices[value.choice_index];
                    break;
                default:
                    continue; // String has no editor in the form and no slot here
            }
            mutator.values.push_back(std::move(saved));
        }
        out.push_back(std::move(mutator));
    }
    return out;
}

// Mirrors vote_form_call's validation so the button can be grayed out
// instead of silently doing nothing when pressed. `for_save` drops the checks that
// only apply to calling it here and now; saved_vote_check judges a stored record.
bool form_is_sendable(const VoteOptionsData& options, bool for_save)
{
    const VoteTypeInfo* selected = selected_type();
    if (!selected) {
        return false;
    }
    switch (selected->type) {
        case AfVoteType::Kick: {
            const auto candidates = build_kick_candidates();
            return g_form.kick_index >= 0
                && g_form.kick_index < static_cast<int>(candidates.size());
        }
        case AfVoteType::Level:
            // Empty means the list had nothing to offer for this game type, or
            // manual entry is on with nothing typed into it yet.
            return !selected_level().empty();
        case AfVoteType::Match: {
            // A named level is left to the server; VoteMatch::validate gates the running
            // level for the empty one, so that case is judged here.
            if (!selected_level().empty() || for_save) {
                return true;
            }
            const std::string running = normalize_level_filename(rf::level.filename.c_str());
            for (const auto& entry : options.levels) {
                if (!string_iequals(entry.filename, running)) {
                    continue;
                }
                if (!entry.allowed_for_vote) {
                    return false;
                }
                return !options.gametype_prefix_restricted
                    || vote_level_allows_gametype(entry, effective_gametype(options, true));
            }
            return true; // not listed: the server adjudicates, like manual entry
        }
        default:
            return true; // parameterless, or Extend
    }
}

const char* saved_vote_type_label(AfVoteType type)
{
    switch (type) {
        case AfVoteType::Match: return "Match";
        case AfVoteType::Extend: return "Extend";
        default: return "Level"; // only these three are savable
    }
}

// Short tag when this build knows the game type, else the name the current blob
// gives it (a saved vote outlives the server it was made on), else the raw id.
std::string saved_vote_gametype_text(uint8_t id, const VoteOptionsData* options)
{
    if (id == af_vote_gametype_none) {
        return "Server default";
    }
    if (const char* short_name = gametype_short_name(id)) {
        return short_name;
    }
    if (options) {
        for (const auto& gametype : options->gametypes) {
            if (gametype.id == id) {
                return gametype.name;
            }
        }
    }
    return std::format("Game type {}", static_cast<unsigned>(id));
}

std::string saved_option_value_text(const SavedVoteOptionValue& value)
{
    switch (value.type) {
        case MutatorOptionType::Bool: return value.bool_value ? "On" : "Off";
        case MutatorOptionType::Choice: return value.choice_label;
        case MutatorOptionType::Int: return std::format("{}", value.int_value);
        case MutatorOptionType::Float: return std::format("{:.2f}", value.float_value);
        default: return "?"; // nothing else is ever stored
    }
}

// Load a saved vote back into the form for editing. Unlike calling one, this is
// constructive: anything the current server no longer offers is silently dropped
// instead of refusing the whole entry.
void load_saved_vote_into_form(const SavedVote& vote, const VoteOptionsData& options)
{
    g_form.on_saved_tab = false;

    // Only Level/Match/Extend are savable, so this never has to target the
    // Rotation tab's grouped subtypes.
    const auto tabs = vote_form_tabs();
    for (int i = 0; i < static_cast<int>(tabs.size()); ++i) {
        if (!tabs[i].is_rotation && tabs[i].types.front()->type == vote.type) {
            g_form.type_index = i;
            break;
        }
    }

    if (vote.type == AfVoteType::Level || vote.type == AfVoteType::Match) {
        const bool is_match = vote.type == AfVoteType::Match;

        // Resolved BEFORE the level: the list is filtered by whichever type lands here.
        const uint8_t wanted_gametype = vote.gametype != af_vote_gametype_none
            ? vote.gametype
            : form_default_gametype(options, is_match, vote.level);
        g_form.gametype_index = gametype_pick_index(options, is_match, wanted_gametype, g_form.gametype_index);
        // Exactly what the level list is filtered with.
        const uint8_t list_gametype = selected_gametype(options, is_match);

        g_form.manual_level = false;
        g_form.manual_level_name.clear();
        g_form.level_selection.clear();
        g_form.level_scroll_to_selection = false;
        if (!vote.level.empty()) {
            const std::string wanted = normalize_level_filename(vote.level);
            const VoteLevelInfo* match = nullptr;
            for (const auto& level : options.levels) {
                if (string_iequals(level.filename, wanted)) {
                    match = &level;
                    break;
                }
            }
            // Being in the blob is not enough: the level list only displays a
            // level that passes allowed_for_vote plus, when the filter is on, the
            // gametype mask - and it drops (and for a Level vote replaces) a
            // selection that is not on display.
            const bool displayable = match != nullptr && match->allowed_for_vote
                && (!vote_form_gametype_filter_active(options)
                    || (list_gametype == af_vote_gametype_none
                            ? vote_level_allows_default_gametype(*match)
                            : vote_level_allows_gametype(*match, list_gametype)));
            if (displayable) {
                // The list rows carry the blob's spelling, so store that spelling.
                g_form.level_selection = match->filename;
                g_form.level_scroll_to_selection = true;
            }
            else {
                g_form.manual_level = true;
                g_form.manual_level_name = vote.level;
            }
        }
        else if (is_match) {
            // Match's pinned "Current level" row; bring it into view like any
            // other resolved list selection.
            g_form.level_scroll_to_selection = true;
        }
    }
    if (vote.type == AfVoteType::Match) {
        g_form.team_size = std::clamp(static_cast<int>(vote.team_size), 1, 8);
    }
    if (vote.type == AfVoteType::Extend) {
        g_form.extend_minutes = std::clamp(static_cast<int>(vote.extend_minutes),
            static_cast<int>(af_vote_extend_min_minutes),
            static_cast<int>(af_vote_extend_max_minutes));
        g_form.extend_manual = std::ranges::find(vote_extend_presets, g_form.extend_minutes)
            == std::end(vote_extend_presets);
    }

    // Only Level and Match carry mutators. An Extend entry has none, so it must
    // leave the section completely alone.
    if (vote.type == AfVoteType::Level || vote.type == AfVoteType::Match) {
        reset_mutators_to_defaults(options);
        for (const auto& saved : vote.mutators) {
            size_t index = options.mutators.size();
            for (size_t i = 0; i < options.mutators.size() && i < g_form.mutators.size(); ++i) {
                if (string_iequals(options.mutators[i].name, saved.name)) {
                    index = i;
                    break;
                }
            }
            if (index >= options.mutators.size() || index >= g_form.mutators.size()) {
                continue;
            }
            const VoteMutatorSchema& schema = options.mutators[index];
            VoteMutatorSelection& selection = g_form.mutators[index];
            selection.enabled = true;

            for (const auto& value : saved.values) {
                for (size_t o = 0; o < schema.options.size() && o < selection.options.size(); ++o) {
                    const VoteMutatorOptionSchema& option = schema.options[o];
                    // Type must agree as well: a saved int written into what is now a
                    // choice would leave the form describing something else.
                    if (!string_iequals(option.name, value.name) || option.type != value.type) {
                        continue;
                    }
                    switch (option.type) {
                        case MutatorOptionType::Bool:
                            selection.options[o].bool_value = value.bool_value;
                            break;
                        case MutatorOptionType::Int:
                            selection.options[o].int_value = value.int_value;
                            selection.options[o].player_set = true;
                            break;
                        case MutatorOptionType::Float:
                            selection.options[o].float_value = value.float_value;
                            selection.options[o].player_set = true;
                            break;
                        case MutatorOptionType::Choice:
                            for (size_t c = 0; c < option.choices.size(); ++c) {
                                if (string_iequals(option.choices[c], value.choice_label)) {
                                    selection.options[o].choice_index = static_cast<uint8_t>(c);
                                    break;
                                }
                            }
                            break;
                        default:
                            break;
                    }
                    break;
                }
            }
        }

        // The loaded values are the player's from here on, so the baseline logic must
        // never re-derive over them: mark them touched and pin its context to now.
        g_form.mutators_touched = true;
        pin_mutator_baseline_context();
    }
    // Same for the game type, so the level it just resolved cannot re-derive over it.
    if (vote.type == AfVoteType::Level || vote.type == AfVoteType::Match) {
        g_form.gametype_touched = true;
        pin_gametype_context(vote.type == AfVoteType::Match);
    }
    g_level_cache.sel_valid = false;
}

} // namespace

VoteForm& vote_form()
{
    return g_form;
}

VoteFormMode vote_form_mode()
{
    if (!is_server_minimum_af_version(1, 4)) {
        return VoteFormMode::NotSupported;
    }
    // A running vote leaves the form in place and is surfaced by the footer button
    // states, so a replacement vote can be set up the moment the current one ends.
    if (!vote_options_are_loaded()) {
        return VoteFormMode::Loading;
    }
    return VoteFormMode::Form;
}

void vote_form_on_open()
{
    clear_pending_save();
    g_form.saved_hovered = -1;
    g_level_cache.valid = false; // never show another blob's rows
    // Re-default the level pick to the running level on every open.
    g_form.level_selection.clear();
    g_form.manual_level = false;
    g_form.manual_level_name.clear();
}

void vote_form_on_close()
{
    // A filter is a transient search; carrying one across opens would read as a
    // mysteriously shrunken map list next time.
    g_form.level_filter.clear();
    clear_pending_save();
}

void vote_form_reset()
{
    // Every field of the form describes the server being left.
    g_form = VoteForm{};
    // Transient state only. The saved-vote store is global user data and
    // deliberately outlives every server.
    clear_pending_save();
    g_saved_availability.clear();
    // Invalidated alongside the vector it describes.
    g_saved_availability_key = SavedAvailabilityKey{};
}

bool vote_form_needs_build(const VoteOptionsData& options)
{
    // Deliberately NOT keyed on the level list: the selection is tracked by name,
    // so a background stale-refresh that only changes levels must not wipe the
    // player's in-progress form. The level cache picks that up instead.
    return !g_form.built || g_form.built_fingerprint != schema_fingerprint(options);
}

void vote_form_ensure(const VoteOptionsData& options)
{
    if (vote_form_needs_build(options)) {
        build_form(options);
    }
}

void vote_form_sync_baseline(const VoteOptionsData& options)
{
    // The level key only matters on a server too old to push an active set.
    if (g_form.mutators_touched) {
        return;
    }
    std::string baseline_key = selected_level();
    const uint32_t generation = vote_options_loaded_generation();
    const uint32_t revision = vote_active_mutators_revision();
    if (baseline_key != g_form.mutators_baseline_key || generation != g_form.mutators_baseline_generation
        || revision != g_form.mutators_baseline_revision) {
        apply_baseline(options, resolve_baseline(options, baseline_key));
        g_form.mutators_baseline_key = std::move(baseline_key);
        g_form.mutators_baseline_generation = generation;
        g_form.mutators_baseline_revision = revision;
    }
}

std::vector<VoteTab> vote_form_tabs()
{
    std::vector<VoteTab> out;
    int rotation_tab = -1;
    for (const auto& info : vote_type_infos) {
        if (!vote_options_is_type_enabled(info.type)) {
            continue;
        }
        if (is_rotation_vote_type(info.type)) {
            if (rotation_tab < 0) {
                // Lands where the first enabled rotation type sat, i.e. after Level.
                out.push_back(VoteTab{"Rotation", true, {}});
                rotation_tab = static_cast<int>(out.size()) - 1;
            }
            out[rotation_tab].types.push_back(&info);
            continue;
        }
        out.push_back(VoteTab{info.label, false, {&info}});
    }
    return out;
}

void vote_form_clamp_tab(const std::vector<VoteTab>& tabs)
{
    if (tabs.empty()) {
        return;
    }
    const int clamped = std::clamp(g_form.type_index, 0, static_cast<int>(tabs.size()) - 1);
    if (clamped != g_form.type_index) {
        g_form.type_index = clamped;
        reset_gametype_selection();
    }
}

void vote_form_select_tab(int index)
{
    if (!g_form.on_saved_tab && g_form.type_index == index) {
        return;
    }
    g_form.on_saved_tab = false;
    if (g_form.type_index != index) {
        reset_gametype_selection();
    }
    g_form.type_index = index;
}

void vote_form_select_saved()
{
    if (g_form.on_saved_tab) {
        return;
    }
    g_form.on_saved_tab = true;
    // Come back to where the selection is rather than to the top of a list
    // the selected row may be nowhere near.
    if (g_form.saved_selected >= 0) {
        g_form.saved_scroll_to_selection = true;
    }
    else {
        g_form.saved_scroll = 0.0f;
    }
}

const VoteTypeInfo* vote_tab_type(const VoteTab& tab)
{
    if (tab.types.empty()) {
        return nullptr;
    }
    const int index =
        tab.is_rotation ? std::clamp(g_form.rotation_index, 0, static_cast<int>(tab.types.size()) - 1) : 0;
    return tab.types[index];
}

const char* vote_form_no_tabs_text()
{
    return vote_client_spectating_match() ? "Only players in the current match can call votes."
                                          : "This server has no votes enabled.";
}

std::vector<const VoteGametypeInfo*> vote_selectable_gametypes(const VoteOptionsData& options, bool team_only)
{
    // Match votes need a team type.
    std::vector<const VoteGametypeInfo*> out;
    for (const auto& gametype : options.gametypes) {
        if (team_only && !gametype.is_team_type) {
            continue;
        }
        out.push_back(&gametype);
    }
    return out;
}

void vote_form_sync_gametype(const VoteOptionsData& options, bool is_match)
{
    if (!g_form.gametype_touched) {
        const std::string key = selected_level();
        // Keyed on the generation too: a blob refresh can change what a level resolves
        // to, and an untouched pick has to follow it.
        if (!g_form.gametype_key_valid || key != g_form.gametype_key || is_match != g_form.gametype_key_match
            || g_form.gametype_key_generation != vote_options_loaded_generation()) {
            g_form.gametype_index = gametype_pick_index(options, is_match,
                form_default_gametype(options, is_match, key), g_form.gametype_index);
            pin_gametype_context(is_match);
        }
    }
    const int count = static_cast<int>(vote_selectable_gametypes(options, is_match).size());
    g_form.gametype_index = count > 0 ? std::clamp(g_form.gametype_index, 0, count - 1) : 0;
}

void vote_form_pick_gametype(int index)
{
    g_form.gametype_index = index;
    g_form.gametype_touched = true;
}

void vote_form_reset_gametype(const VoteOptionsData& options, bool is_match)
{
    g_form.gametype_index = gametype_pick_index(options, is_match,
        form_default_gametype(options, is_match, selected_level()), g_form.gametype_index);
    g_form.gametype_touched = false;
    pin_gametype_context(is_match);
}

uint8_t vote_form_selected_gametype(const std::vector<const VoteGametypeInfo*>& gametypes)
{
    if (g_form.gametype_index < 0 || g_form.gametype_index >= static_cast<int>(gametypes.size())) {
        return af_vote_gametype_none;
    }
    return gametypes[g_form.gametype_index]->id;
}

uint8_t vote_form_effective_gametype(const VoteOptionsData& options,
                                     const std::vector<const VoteGametypeInfo*>& gametypes)
{
    const uint8_t chosen = vote_form_selected_gametype(gametypes);
    if (chosen != af_vote_gametype_none) {
        return chosen;
    }
    return vote_default_gametype_for_level(options, selected_level());
}

void vote_form_set_gametype_filter(bool on)
{
    g_filter_gametype_local = on;
}

bool vote_form_gametype_filter_active(const VoteOptionsData& options)
{
    return options.gametype_prefix_restricted || g_filter_gametype_local;
}

const VoteLevelList& vote_form_level_list(const VoteOptionsData& options, bool is_match, uint8_t gametype,
                                          bool fix_selection)
{
    const bool filter_active = vote_form_gametype_filter_active(options);
    // With the filter off the type is not consulted, so keying the cache on it would
    // rebuild an identical list.
    const uint8_t filter_gametype = filter_active ? gametype : af_vote_gametype_none;

    // The allow-list applies whatever the game type, so it is not gated on the filter.
    const uint32_t levels_fp = level_list_fingerprint(options);
    VoteLevelList& cache = g_level_cache;
    if (!cache.valid || cache.levels_fp != levels_fp || cache.filter_active != filter_active
        || cache.name_filter != g_form.level_filter || cache.gametype != filter_gametype
        || cache.allow_current != is_match) {
        cache.levels_fp = levels_fp;
        cache.filter_active = filter_active;
        cache.name_filter = g_form.level_filter;
        cache.gametype = filter_gametype;
        cache.allow_current = is_match;
        cache.gametype_hidden = 0;
        cache.not_allowed_hidden = 0;
        cache.name_filter_hidden = 0;
        cache.sel_valid = false;

        std::vector<std::string> levels;
        levels.reserve(options.levels.size());
        for (const auto& level : options.levels) {
            if (!level.allowed_for_vote) {
                ++cache.not_allowed_hidden;
                continue;
            }
            if (filter_active) {
                const bool matches = filter_gametype == af_vote_gametype_none
                    ? vote_level_allows_default_gametype(level)
                    : vote_level_allows_gametype(level, filter_gametype);
                if (!matches) {
                    ++cache.gametype_hidden;
                    continue;
                }
            }
            // After the gametype filter: the typed filter narrows within
            // whatever set the gametype rules leave votable.
            if (!level_matches_name_filter(level.filename)) {
                ++cache.name_filter_hidden;
                continue;
            }
            levels.push_back(level.filename);
        }

        // Alphabetical rather than the blob order (rotation order, then extras).
        std::sort(levels.begin(), levels.end(), string_iless);
        // The blob can list the same file twice (rotation entry + vote-allowed
        // extra); two identical rows would both resolve to the first by name.
        levels.erase(std::unique(levels.begin(), levels.end(), string_iequals), levels.end());

        cache.items.clear();
        cache.items.reserve(levels.size() + 1);
        if (is_match) {
            // Pinned and always visible; the server adjudicates it at call time.
            cache.items.emplace_back("Current level");
        }
        for (auto& level : levels) {
            cache.items.push_back(std::move(level));
        }
        cache.first_level_row = is_match ? 1 : 0;
        cache.valid = true;
    }

    const int level_row_count = static_cast<int>(cache.items.size()) - cache.first_level_row;

    if (fix_selection) {
        // A game type change (or a refreshed allow-list) can hide the selected map;
        // drop the selection so it can never be sent. Match then falls back to the
        // pinned "Current level" row.
        // Compared case-insensitively, like the dedup above: when the blob lists the
        // same file in two spellings, the row that survived the dedup may not be the
        // spelling stored here, and an exact compare would drop a selection that IS on
        // display (and, for a Level vote, silently substitute the first row for it).
        if (!g_form.level_selection.empty()
            && std::find_if(cache.items.begin() + cache.first_level_row, cache.items.end(),
                   [](const std::string& item) { return string_iequals(item, g_form.level_selection); })
                == cache.items.end()) {
            g_form.level_selection.clear();
            cache.sel_valid = false;
        }
        // A Level vote has no pinned row, so keep an entry active by default: the
        // running level when the list offers it (normalized the way the server
        // normalizes a voted name, so both resolve the same file), else the top
        // entry. The list is scrolled to whichever that lands on, since a default
        // sitting off-screen reads as no selection at all.
        if (!is_match && g_form.level_selection.empty() && level_row_count > 0) {
            const std::string current = normalize_level_filename(std::string_view{rf::level.filename.c_str()});
            size_t row = static_cast<size_t>(cache.first_level_row);
            for (size_t i = row; i < cache.items.size(); ++i) {
                if (string_iequals(cache.items[i], current)) {
                    row = i;
                    break;
                }
            }
            g_form.level_selection = cache.items[row];
            g_form.level_scroll_to_selection = true;
            cache.sel_valid = false;
        }
    }

    // Resolve the highlighted row from the stored name so it follows the map
    // through the sort/filter instead of pointing at whatever sits there now.
    if (!cache.sel_valid || cache.sel_name != g_form.level_selection || cache.sel_manual != g_form.manual_level) {
        cache.sel_name = g_form.level_selection;
        cache.sel_manual = g_form.manual_level;
        cache.sel_row = -1;
        if (!g_form.manual_level) {
            if (g_form.level_selection.empty()) {
                cache.sel_row = is_match ? 0 : -1;
            }
            else {
                for (size_t i = cache.first_level_row; i < cache.items.size(); ++i) {
                    // Case-insensitive for the same reason as the drop-check above.
                    if (string_iequals(cache.items[i], g_form.level_selection)) {
                        cache.sel_row = static_cast<int>(i);
                        break;
                    }
                }
            }
        }
        cache.sel_valid = true;
    }
    return cache;
}

void vote_form_pick_level_row(int row, bool is_match)
{
    if (row < 0 || row >= static_cast<int>(g_level_cache.items.size())) {
        return;
    }
    g_form.level_selection = (is_match && row == 0) ? std::string{} : g_level_cache.items[row];
    g_form.manual_level = false;
    g_level_cache.sel_valid = false;
}

void vote_form_level_filter_changed()
{
    // The rows under the list just changed wholesale; the wheel position
    // in the old set means nothing in the new one.
    g_form.level_scroll = 0.0f;
}

bool vote_mutator_offered(const VoteMutatorSchema& schema, uint8_t game_type)
{
    return mutator_gametype_mask_allows(schema.valid_gametype_mask, game_type);
}

void vote_form_sync_score_limit(const VoteOptionsData& options, uint8_t game_type)
{
    // Defaults to the limit the chosen game type already runs, so enabling it
    // without touching the number changes nothing. Each game type keeps its own
    // limit, so the default follows the pick rather than being fixed at form build.
    const VoteGametypeInfo* gametype = nullptr;
    for (const auto& entry : options.gametypes) {
        if (entry.id == game_type) {
            gametype = &entry;
            break;
        }
    }
    if (!gametype || gametype->score_limit <= 0) {
        return;
    }

    for (size_t i = 0; i < options.mutators.size() && i < g_form.mutators.size(); ++i) {
        const VoteMutatorSchema& schema = options.mutators[i];
        if (schema.name != "scorelimit") {
            continue;
        }
        for (size_t o = 0; o < schema.options.size() && o < g_form.mutators[i].options.size(); ++o) {
            if (schema.options[o].name != "score_limit") {
                continue;
            }
            MutatorEditorValue& value = g_form.mutators[i].options[o];
            if (value.player_set) {
                return; // an explicit source owns this number
            }
            value.int_value = gametype->score_limit;
            return;
        }
        return;
    }
}

void vote_form_use_current_mutators(const VoteOptionsData& options)
{
    // Current IS the auto baseline, so it re-arms.
    apply_baseline(options, resolve_baseline(options, selected_level()));
    g_form.mutators_touched = false;
    pin_mutator_baseline_context();
}

void vote_form_use_base_mutators(const VoteOptionsData& options)
{
    // Base is a deviation from the baseline, so it pins.
    apply_baseline(options, options.base_mutator_decls);
    g_form.mutators_touched = true;
    pin_mutator_baseline_context();
}

void vote_form_set_extend_minutes(const std::string& text)
{
    try {
        g_form.extend_minutes = std::clamp(std::stoi(text), static_cast<int>(af_vote_extend_min_minutes),
                                           static_cast<int>(af_vote_extend_max_minutes));
    }
    catch (const std::exception&) {
        // Empty or out of int range: keep whatever was already selected.
    }
}

const std::vector<VoteKickCandidate>& vote_form_kick_candidates(bool refresh)
{
    if (refresh) {
        g_kick_cache = build_kick_candidates();
        g_form.kick_index =
            std::clamp(g_form.kick_index, 0, std::max(0, static_cast<int>(g_kick_cache.size()) - 1));
    }
    return g_kick_cache;
}

bool vote_form_can_call(const VoteOptionsData& options)
{
    // Cannot call one while one runs.
    return !vote_state_get().has_value() && form_is_sendable(options, false);
}

void vote_form_call(const VoteOptionsData& options)
{
    const VoteTypeInfo* selected = selected_type();
    if (!selected) {
        return;
    }

    AfVoteCallParams params;
    params.type = selected->type;

    switch (params.type) {
        case AfVoteType::Kick: {
            const auto candidates = build_kick_candidates();
            if (g_form.kick_index < 0 || g_form.kick_index >= static_cast<int>(candidates.size())) {
                return;
            }
            params.target_player_id = candidates[g_form.kick_index].id;
            break;
        }
        case AfVoteType::Level: {
            params.level = selected_level();
            if (params.level.empty()) {
                return;
            }
            params.gametype = selected_gametype(options, false);
            params.mutators = build_mutator_inputs(options, effective_gametype(options, false));
            params.mutators_explicit = true;
            break;
        }
        case AfVoteType::Match: {
            params.team_size = static_cast<uint8_t>(g_form.team_size);
            params.level = selected_level();
            params.gametype = selected_gametype(options, true);
            params.mutators = build_mutator_inputs(options, effective_gametype(options, true));
            params.mutators_explicit = true;
            break;
        }
        case AfVoteType::Extend:
            params.extend_minutes = static_cast<uint8_t>(std::clamp(g_form.extend_minutes,
                static_cast<int>(af_vote_extend_min_minutes),
                static_cast<int>(af_vote_extend_max_minutes)));
            break;
        case AfVoteType::Restart:
        case AfVoteType::Next:
        case AfVoteType::Random:
        case AfVoteType::Previous:
            params.preserve = g_form.rotation_preserve;
            break;
        default:
            break; // parameterless
    }
    af_send_vote_call(params);
}

bool vote_form_can_cancel()
{
    // Can only cancel one you own.
    const auto& state = vote_state_get();
    return state.has_value() && state->is_owner;
}

void vote_form_cancel()
{
    af_send_vote_cancel();
}

bool vote_form_can_save(const VoteOptionsData& options)
{
    // Saving is local, so it is deliberately NOT gated on a running vote; only
    // the three savable types offer it.
    const VoteTypeInfo* selected = selected_type();
    return selected != nullptr && saved_vote_type_is_savable(selected->type) && form_is_sendable(options, true)
        && !saved_votes_is_full();
}

bool vote_form_begin_save(const VoteOptionsData& options)
{
    const VoteTypeInfo* selected = selected_type();
    if (!selected || !saved_vote_type_is_savable(selected->type)) {
        return false;
    }
    const AfVoteType type = selected->type;

    SavedVote vote;
    vote.type = type;
    if (type == AfVoteType::Extend) {
        vote.extend_minutes = static_cast<uint8_t>(std::clamp(g_form.extend_minutes,
            static_cast<int>(af_vote_extend_min_minutes),
            static_cast<int>(af_vote_extend_max_minutes)));
    }
    else {
        const bool is_match = type == AfVoteType::Match;
        vote.level = selected_level();
        vote.gametype = selected_gametype(options, is_match);
        if (is_match) {
            vote.team_size = static_cast<uint8_t>(std::clamp(g_form.team_size, 1, 8));
        }
        vote.mutators = build_saved_mutators(options, effective_gametype(options, is_match));
        vote.mutators_explicit = true;
    }
    g_pending_save = std::move(vote);
    g_pending_save_valid = true;
    return true;
}

bool vote_form_finish_save(const std::string& name)
{
    if (!g_pending_save_valid) {
        return false; // the form was closed or reset while the prompt was up
    }
    const std::string sanitized = saved_vote_sanitize_name(name);
    if (sanitized.empty()) {
        clear_pending_save(); // nothing typed: discard silently
        return false;
    }
    g_pending_save.name = sanitized;
    // saved_votes_add does the auto-rename and persists, and hands back where the
    // entry landed so the list can highlight it - or -1 when the store is full, in
    // which case nothing was stored and there is nothing to select or announce.
    const int index = saved_votes_add(std::move(g_pending_save));
    clear_pending_save();
    if (index < 0) {
        return false;
    }
    g_form.saved_selected = index;
    // The new entry is appended, so on a long list it lands off-screen.
    g_form.saved_scroll_to_selection = true;
    return true;
}

void vote_form_drop_save()
{
    clear_pending_save();
}

void vote_form_saved_refresh(const VoteOptionsData* options, VoteFormMode mode)
{
    const auto& store = saved_votes_get();
    const SavedAvailabilityKey key{true, saved_votes_revision(), vote_options_loaded_generation(), options != nullptr,
                                   mode != VoteFormMode::NotSupported, rf::level.filename.c_str(),
                                   static_cast<uint8_t>(rf::multi_get_game_type())};
    if (g_saved_availability_key != key) {
        g_saved_availability_key = key;
        g_saved_availability.clear();
        g_saved_availability.reserve(store.size());
        for (const auto& vote : store) {
            g_saved_availability.push_back(saved_vote_check(vote, options, key.server_supported));
        }
    }
    if (g_form.saved_selected >= static_cast<int>(store.size())) {
        g_form.saved_selected = -1;
    }
    // "Uncallable means unselectable" has to survive a refresh that flips the
    // selected entry uncallable, not just the click that selected it.
    else if (g_form.saved_selected >= 0 && !vote_form_saved_callable(g_form.saved_selected)) {
        g_form.saved_selected = -1;
    }
    if (g_form.saved_hovered >= static_cast<int>(store.size())) {
        g_form.saved_hovered = -1;
    }
}

const SavedVoteAvailability* vote_form_saved_availability(int index)
{
    if (index < 0 || index >= static_cast<int>(g_saved_availability.size())) {
        return nullptr;
    }
    return &g_saved_availability[index];
}

bool vote_form_saved_callable(int index)
{
    const SavedVoteAvailability* availability = vote_form_saved_availability(index);
    return availability && availability->callable;
}

void vote_form_saved_delete(int index)
{
    if (index < 0 || index >= static_cast<int>(saved_votes_get().size())) {
        return;
    }
    saved_votes_delete(static_cast<size_t>(index));
    if (index < static_cast<int>(g_saved_availability.size())) {
        // Keep the frame cache aligned with the store for the draw pass
        // that follows this one.
        g_saved_availability.erase(g_saved_availability.begin() + index);
    }
    if (g_form.saved_selected == index) {
        g_form.saved_selected = -1;
    }
    else if (g_form.saved_selected > index) {
        --g_form.saved_selected;
    }
    g_form.saved_hovered = -1;
}

int vote_form_saved_previewed()
{
    // Hover drives the preview, with the selection as the resting state.
    const int count = static_cast<int>(saved_votes_get().size());
    if (g_form.saved_hovered >= 0 && g_form.saved_hovered < count) {
        return g_form.saved_hovered;
    }
    if (g_form.saved_selected >= 0 && g_form.saved_selected < count) {
        return g_form.saved_selected;
    }
    return -1;
}

bool vote_form_saved_can_edit(const VoteOptionsData* options, VoteFormMode mode)
{
    // EDIT loads the SELECTED entry (not the hovered one) back into the form,
    // which only exists in Form mode.
    const auto& store = saved_votes_get();
    return g_form.saved_selected >= 0 && g_form.saved_selected < static_cast<int>(store.size())
        && mode == VoteFormMode::Form && options != nullptr
        && vote_options_is_type_enabled(store[g_form.saved_selected].type);
}

void vote_form_saved_edit(const VoteOptionsData& options)
{
    const auto& store = saved_votes_get();
    if (g_form.saved_selected >= 0 && g_form.saved_selected < static_cast<int>(store.size())) {
        load_saved_vote_into_form(store[g_form.saved_selected], options);
    }
}

bool vote_form_saved_can_call(const VoteOptionsData* options)
{
    return !vote_state_get().has_value() && options != nullptr
        && g_form.saved_selected < static_cast<int>(saved_votes_get().size())
        && vote_form_saved_callable(g_form.saved_selected);
}

void vote_form_saved_call(const VoteOptionsData& options)
{
    if (vote_form_saved_can_call(&options)) {
        af_send_vote_call(saved_vote_build_params(saved_votes_get()[g_form.saved_selected], options));
    }
}

std::vector<std::string> vote_form_saved_printout(const SavedVote& vote, const VoteOptionsData* options)
{
    std::vector<std::string> lines;
    lines.push_back(std::format("Type: {}", saved_vote_type_label(vote.type)));

    if (vote.type == AfVoteType::Level || vote.type == AfVoteType::Match) {
        // Only Match can carry an empty level, where it means "keep this one".
        lines.push_back(std::format("Level: {}", vote.level.empty() ? std::string{"Current level"} : vote.level));
        lines.push_back(std::format("Game type: {}", saved_vote_gametype_text(vote.gametype, options)));
    }
    if (vote.type == AfVoteType::Match) {
        lines.push_back(std::format("Team size: {}v{}", vote.team_size, vote.team_size));
    }
    if (vote.type == AfVoteType::Extend) {
        lines.push_back(std::format("Duration: +{} min", vote.extend_minutes));
    }
    if (!vote.mutators.empty()) {
        lines.emplace_back("Mutators:");
        // The record keeps names; the server's schema, once it has arrived, has their labels.
        for (const auto& mutator : vote.mutators) {
            const VoteMutatorSchema* schema = nullptr;
            if (options) {
                const auto it = std::ranges::find_if(options->mutators,
                    [&](const VoteMutatorSchema& m) { return string_iequals(m.name, mutator.name); });
                schema = it != options->mutators.end() ? &*it : nullptr;
            }
            lines.push_back(std::format("  {}", schema ? schema->label : mutator.name));
            for (const auto& value : mutator.values) {
                const std::string* label = &value.name;
                if (schema) {
                    const auto it = std::ranges::find_if(schema->options,
                        [&](const VoteMutatorOptionSchema& o) { return string_iequals(o.name, value.name); });
                    label = it != schema->options.end() ? &it->label : label;
                }
                lines.push_back(std::format("    {}: {}", *label, saved_option_value_text(value)));
            }
        }
    }
    return lines;
}
