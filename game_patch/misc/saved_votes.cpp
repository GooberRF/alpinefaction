#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include <common/utils/string-utils.h>
#include <xlog/xlog.h>
#include "saved_votes.h"
#include "alpine_settings.h"
#include "../multi/multi.h"
#include "../multi/vote_client.h"

namespace
{

std::vector<SavedVote> g_saved_votes;
std::vector<std::string> g_saved_unparsed;
uint32_t g_saved_revision = 0;

constexpr size_t max_name_len = 64;
constexpr size_t max_field_len = 255;   // matches the blob's u8 length prefix
constexpr size_t max_mutators_per_vote = 255;
constexpr size_t max_options_per_mutator = 255;
constexpr size_t max_saved_votes = 200;
constexpr size_t max_unparsed_lines = 50;

// ---------------------------------------------------------------------------
// One-line INI encoding
//
//   1|<name>|<type>|<level>|<gametype>|<team_size>|<extend_minutes>|<mutators>
//
// <mutators> is entries joined by ';', each `name` or `name:opt=Xval,opt=Xval`,
// where the value's first character types it: b0/b1 bool, i<n> int, f<n> float,
// c<label> choice. Every free-text field percent-encodes the separators plus any
// control character, so a level or choice label containing one round-trips.
// ---------------------------------------------------------------------------

constexpr std::string_view reserved_chars = "%|;:,=";

bool char_needs_escape(unsigned char ch)
{
    return ch < 0x20 || reserved_chars.find(static_cast<char>(ch)) != std::string_view::npos;
}

std::string encode_field(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        const auto uch = static_cast<unsigned char>(ch);
        if (char_needs_escape(uch)) {
            out += std::format("%{:02X}", static_cast<unsigned>(uch));
        }
        else {
            out += ch;
        }
    }
    return out;
}

// A truncated or non-hex escape fails the whole entry rather than silently
// dropping characters: a half-decoded level name would vote for the wrong map.
bool decode_field(std::string_view text, std::string& out)
{
    out.clear();
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '%') {
            out += text[i];
            continue;
        }
        if (i + 2 >= text.size()) {
            return false;
        }
        const std::string_view hex = text.substr(i + 1, 2);
        unsigned value = 0;
        const auto result = std::from_chars(hex.data(), hex.data() + hex.size(), value, 16);
        if (result.ec != std::errc{} || result.ptr != hex.data() + hex.size()) {
            return false;
        }
        out += static_cast<char>(value);
        i += 2;
    }
    return true;
}

// Post-decode check for a free-text field. Raw control characters are rejected
// before decoding, so one here can only come from a deliberately crafted escape:
// %00 in particular smuggles a NUL that every display truncates at while the wire
// still carries the whole string.
bool decoded_field_is_sane(std::string_view text, size_t max_len)
{
    return text.size() <= max_len
        && std::none_of(text.begin(), text.end(),
               [](char ch) { return static_cast<unsigned char>(ch) < 0x20; });
}

// Always yields at least one (possibly empty) piece, so a trailing separator is
// a parse error rather than a silently dropped entry.
std::vector<std::string_view> split_view(std::string_view text, char separator)
{
    std::vector<std::string_view> out;
    size_t pos = 0;
    while (true) {
        const size_t next = text.find(separator, pos);
        if (next == std::string_view::npos) {
            out.push_back(text.substr(pos));
            break;
        }
        out.push_back(text.substr(pos, next - pos));
        pos = next + 1;
    }
    return out;
}

bool parse_uint_field(std::string_view text, unsigned long& out)
{
    if (text.empty()) {
        return false;
    }
    const auto result = std::from_chars(text.data(), text.data() + text.size(), out, 10);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool parse_int_field(std::string_view text, int32_t& out)
{
    const std::string buffer{text};
    try {
        size_t consumed = 0;
        const long long value = std::stoll(buffer, &consumed);
        if (consumed != buffer.size() || value < std::numeric_limits<int32_t>::min()
            || value > std::numeric_limits<int32_t>::max()) {
            return false;
        }
        out = static_cast<int32_t>(value);
        return true;
    }
    catch (const std::exception&) {
        return false;
    }
}

// std::stof rather than from_chars: floating-point from_chars is not available on
// every toolchain this project builds with.
bool parse_float_field(std::string_view text, float& out)
{
    const std::string buffer{text};
    try {
        size_t consumed = 0;
        const float value = std::stof(buffer, &consumed);
        if (consumed != buffer.size() || !std::isfinite(value)) {
            return false;
        }
        out = value;
        return true;
    }
    catch (const std::exception&) {
        return false;
    }
}

bool name_is_taken(std::string_view name)
{
    return std::any_of(g_saved_votes.begin(), g_saved_votes.end(),
        [name](const SavedVote& entry) { return string_iequals(entry.name, name); });
}

// Shared by both append paths so the store's "names are unique, case-insensitively"
// invariant holds however an entry got in (typed, or loaded from a hand-edited file).
// Returns false when the store is full, in which case nothing was appended.
bool append_with_unique_name(SavedVote vote)
{
    if (g_saved_votes.size() >= max_saved_votes) {
        return false;
    }
    vote.name = saved_vote_sanitize_name(vote.name);
    if (vote.name.empty()) {
        vote.name = "Saved vote";
    }
    if (name_is_taken(vote.name)) {
        const std::string base = vote.name;
        // Bounded: at most one clash per existing entry.
        for (int suffix = 2; suffix <= static_cast<int>(g_saved_votes.size()) + 2; ++suffix) {
            const std::string tag = std::format(" ({})", suffix);
            // The tag must not push the name past the cap: saved_vote_parse refuses
            // an over-long name, and a line this store wrote must never be one its
            // own parser rejects (it would be preserved verbatim but disappear from
            // the list). Only reachable from a hand-edited file -- the name popup is
            // 31 characters -- so it costs the tail of a very long duplicate name.
            // The BASE is trimmed rather than the tag, so every candidate stays
            // distinct and the loop still terminates.
            std::string candidate = base.substr(0, max_name_len - std::min(tag.size(), max_name_len));
            candidate += tag;
            if (!name_is_taken(candidate)) {
                vote.name = std::move(candidate);
                break;
            }
        }
    }
    g_saved_votes.push_back(std::move(vote));
    ++g_saved_revision;
    return true;
}

int find_choice_index(const VoteMutatorOptionSchema& option, std::string_view label)
{
    for (size_t i = 0; i < option.choices.size(); ++i) {
        if (string_iequals(option.choices[i], label)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

const VoteMutatorSchema* find_mutator_schema(const VoteOptionsData& options, std::string_view name)
{
    for (const auto& schema : options.mutators) {
        if (string_iequals(schema.name, name)) {
            return &schema;
        }
    }
    return nullptr;
}

const VoteMutatorOptionSchema* find_option_schema(const VoteMutatorSchema& schema,
                                                  const SavedVoteOptionValue& value)
{
    for (const auto& option : schema.options) {
        // Type must agree too: writing a saved int into what is now a choice
        // would send nonsense back to the server.
        if (string_iequals(option.name, value.name) && option.type == value.type) {
            return &option;
        }
    }
    return nullptr;
}

} // namespace

bool saved_vote_type_is_savable(AfVoteType type)
{
    return type == AfVoteType::Level || type == AfVoteType::Match || type == AfVoteType::Extend;
}

std::string saved_vote_sanitize_name(std::string_view name)
{
    std::string out;
    out.reserve(name.size());
    for (const char ch : name) {
        const auto uch = static_cast<unsigned char>(ch);
        // '|' is the record separator.
        if (uch < 0x20 || ch == '|') {
            continue;
        }
        out += ch;
    }
    const size_t first = out.find_first_not_of(" \t");
    if (first == std::string::npos) {
        return {};
    }
    const size_t last = out.find_last_not_of(" \t");
    // Truncated last, so the cap applies to what is actually stored. The popup
    // input is 31 characters, so this only ever bites a hand-edited file.
    return out.substr(first, std::min(last - first + 1, max_name_len));
}

const std::vector<SavedVote>& saved_votes_get()
{
    return g_saved_votes;
}

int saved_votes_add(SavedVote vote)
{
    if (!append_with_unique_name(std::move(vote))) {
        xlog::warn("Saved vote store is full ({} entries); not saving another", max_saved_votes);
        return -1;
    }
    alpine_core_config_save();
    return static_cast<int>(g_saved_votes.size()) - 1;
}

void saved_votes_load_add(SavedVote vote)
{
    // The loader stops before it gets here (see saved_votes_is_full) so the excess
    // lines are preserved rather than dropped; the cap is still enforced below in
    // case another caller appears.
    static_cast<void>(append_with_unique_name(std::move(vote)));
}

void saved_votes_delete(size_t index)
{
    if (index >= g_saved_votes.size()) {
        return;
    }
    g_saved_votes.erase(g_saved_votes.begin() + static_cast<ptrdiff_t>(index));
    ++g_saved_revision;
    alpine_core_config_save();
}

void saved_votes_clear()
{
    g_saved_votes.clear();
    g_saved_unparsed.clear();
    ++g_saved_revision;
}

bool saved_votes_is_full()
{
    return g_saved_votes.size() >= max_saved_votes;
}

uint32_t saved_votes_revision()
{
    return g_saved_revision;
}

void saved_votes_add_unparsed(std::string encoded)
{
    // Bounded so a crafted file cannot grow the section without limit on every
    // save/load cycle.
    if (g_saved_unparsed.size() >= max_unparsed_lines) {
        xlog::warn("Too many unreadable saved votes; dropping '{}'", encoded);
        return;
    }
    g_saved_unparsed.push_back(std::move(encoded));
}

const std::vector<std::string>& saved_votes_unparsed()
{
    return g_saved_unparsed;
}

std::string saved_vote_encode(const SavedVote& vote)
{
    std::string out = "1|";
    out += encode_field(vote.name);
    out += '|';
    out += std::format("{}", static_cast<unsigned>(vote.type));
    out += '|';
    out += encode_field(vote.level);
    out += '|';
    out += std::format("{}", static_cast<unsigned>(vote.gametype));
    out += '|';
    out += std::format("{}", static_cast<unsigned>(vote.team_size));
    out += '|';
    out += std::format("{}", static_cast<unsigned>(vote.extend_minutes));
    out += '|';

    bool first_mutator = true;
    for (const auto& mutator : vote.mutators) {
        if (!first_mutator) {
            out += ';';
        }
        first_mutator = false;
        out += encode_field(mutator.name);

        bool first_value = true;
        for (const auto& value : mutator.values) {
            out += first_value ? ':' : ',';
            first_value = false;
            out += encode_field(value.name);
            out += '=';
            switch (value.type) {
                case MutatorOptionType::Bool:
                    out += value.bool_value ? "b1" : "b0";
                    break;
                case MutatorOptionType::Choice:
                    out += 'c';
                    out += encode_field(value.choice_label);
                    break;
                case MutatorOptionType::Int:
                    out += std::format("i{}", value.int_value);
                    break;
                case MutatorOptionType::Float:
                    // saved_vote_parse refuses a non-finite value, so the encoder
                    // must never emit one: the whole entry would vanish on the
                    // next launch. The form's own input guard is the real fix;
                    // this is the backstop at the point of no return.
                    if (std::isfinite(value.float_value)) {
                        out += std::format("f{}", value.float_value);
                    }
                    else {
                        out += "f0";
                    }
                    break;
                default:
                    // Unreachable: nothing else is ever stored (String has no
                    // widget in the form and no slot here).
                    out += "b0";
                    break;
            }
        }
    }
    return out;
}

bool saved_vote_parse(std::string_view encoded, SavedVote& out)
{
    // Every control character is escaped on write, so a raw one means the line was
    // mangled (a stray CR from a hand edit, a truncated write) - reject it rather
    // than let it end up inside a name the server is then asked to match.
    if (std::any_of(encoded.begin(), encoded.end(),
            [](char ch) { return static_cast<unsigned char>(ch) < 0x20; })) {
        return false;
    }

    const auto fields = split_view(encoded, '|');
    if (fields.size() != 8 || fields[0] != "1") {
        return false;
    }

    SavedVote vote;
    // Checked before sanitizing, so an over-long or escape-smuggled name is
    // refused (and preserved verbatim by the caller) rather than quietly reshaped.
    if (!decode_field(fields[1], vote.name) || !decoded_field_is_sane(vote.name, max_name_len)) {
        return false;
    }
    vote.name = saved_vote_sanitize_name(vote.name);
    if (vote.name.empty()) {
        return false;
    }

    unsigned long type_raw = 0;
    if (!parse_uint_field(fields[2], type_raw) || type_raw >= af_vote_type_count) {
        return false;
    }
    vote.type = static_cast<AfVoteType>(type_raw);
    if (!saved_vote_type_is_savable(vote.type)) {
        return false;
    }

    if (!decode_field(fields[3], vote.level) || !decoded_field_is_sane(vote.level, max_field_len)) {
        return false;
    }

    unsigned long gametype = 0;
    if (!parse_uint_field(fields[4], gametype) || gametype > 0xFF) {
        return false;
    }
    vote.gametype = static_cast<uint8_t>(gametype);

    unsigned long team_size = 0;
    if (!parse_uint_field(fields[5], team_size)) {
        return false;
    }
    vote.team_size = static_cast<uint8_t>(std::clamp<unsigned long>(team_size, 1, 8));

    unsigned long extend_minutes = 0;
    if (!parse_uint_field(fields[6], extend_minutes)) {
        return false;
    }
    vote.extend_minutes = static_cast<uint8_t>(std::clamp<unsigned long>(extend_minutes,
        af_vote_extend_min_minutes, af_vote_extend_max_minutes));

    if (!fields[7].empty()) {
        for (const std::string_view entry : split_view(fields[7], ';')) {
            if (entry.empty()) {
                return false;
            }
            SavedVoteMutator mutator;
            const size_t colon = entry.find(':');
            if (!decode_field(entry.substr(0, colon), mutator.name) || mutator.name.empty()
                || !decoded_field_is_sane(mutator.name, max_field_len)) {
                return false;
            }
            if (colon != std::string_view::npos) {
                for (const std::string_view pair : split_view(entry.substr(colon + 1), ',')) {
                    const size_t equals = pair.find('=');
                    // A value has to be present: the type prefix is what says
                    // which member of the option is meaningful.
                    if (equals == std::string_view::npos || equals + 1 >= pair.size()) {
                        return false;
                    }
                    if (mutator.values.size() >= max_options_per_mutator) {
                        return false;
                    }
                    SavedVoteOptionValue value;
                    if (!decode_field(pair.substr(0, equals), value.name) || value.name.empty()
                        || !decoded_field_is_sane(value.name, max_field_len)) {
                        return false;
                    }
                    const std::string_view raw = pair.substr(equals + 1);
                    switch (raw.front()) {
                        case 'b':
                            if (raw != "b0" && raw != "b1") {
                                return false;
                            }
                            value.type = MutatorOptionType::Bool;
                            value.bool_value = raw == "b1";
                            break;
                        case 'i':
                            value.type = MutatorOptionType::Int;
                            if (!parse_int_field(raw.substr(1), value.int_value)) {
                                return false;
                            }
                            break;
                        case 'f':
                            value.type = MutatorOptionType::Float;
                            if (!parse_float_field(raw.substr(1), value.float_value)) {
                                return false;
                            }
                            break;
                        case 'c':
                            value.type = MutatorOptionType::Choice;
                            if (!decode_field(raw.substr(1), value.choice_label)
                                || !decoded_field_is_sane(value.choice_label, max_field_len)) {
                                return false;
                            }
                            break;
                        default:
                            return false;
                    }
                    mutator.values.push_back(std::move(value));
                }
            }
            if (vote.mutators.size() >= max_mutators_per_vote) {
                return false;
            }
            vote.mutators.push_back(std::move(mutator));
        }
    }

    // A Level vote with no level could never be called, so it is rejected here
    // rather than being listed as permanently broken.
    if (vote.type == AfVoteType::Level && vote.level.empty()) {
        return false;
    }

    out = std::move(vote);
    return true;
}

SavedVoteAvailability saved_vote_check(const SavedVote& vote, const VoteOptionsData* options,
                                      bool server_supported)
{
    SavedVoteAvailability out;

    // Both of these make every other check meaningless, so they short-circuit.
    if (!server_supported) {
        out.reasons.emplace_back("This server does not support menu voting");
        return out;
    }
    if (!options) {
        out.reasons.emplace_back("Vote options not loaded yet");
        return out;
    }

    if (!vote_options_is_type_enabled(vote.type)) {
        out.reasons.emplace_back("This vote type is disabled on this server");
    }

    if (vote.type == AfVoteType::Level || vote.type == AfVoteType::Match) {
        if (vote.level.empty()) {
            if (vote.type == AfVoteType::Level) {
                out.reasons.emplace_back("No level is saved for this vote");
            }
            // Match's empty level is the "current level" row: always valid.
        }
        else {
            const std::string wanted = level_filename_with_rfl(vote.level);
            const VoteLevelInfo* found = nullptr;
            for (const auto& level : options->levels) {
                if (string_iequals(level.filename, wanted)) {
                    found = &level;
                    break;
                }
            }
            // A level the blob does not list at all is still allowed: that is
            // exactly what the live form's manual entry does, and the server
            // adjudicates it at call time.
            if (found) {
                if (!found->allowed_for_vote) {
                    out.reasons.emplace_back("Level is not on the server's vote list");
                }
                else if (options->gametype_prefix_restricted) {
                    const bool matches = vote.gametype == af_vote_gametype_none
                        ? vote_level_allows_default_gametype(*found)
                        : vote_level_allows_gametype(*found, vote.gametype);
                    if (!matches) {
                        out.reasons.emplace_back("Level is not valid for this game type here");
                    }
                }
            }
        }
    }

    if (vote.gametype != af_vote_gametype_none) {
        const VoteGametypeInfo* found = nullptr;
        for (const auto& gametype : options->gametypes) {
            if (gametype.id == vote.gametype) {
                found = &gametype;
                break;
            }
        }
        if (!found) {
            out.reasons.emplace_back("Game type not offered on this server");
        }
        else if (vote.type == AfVoteType::Match && !found->is_team_type) {
            out.reasons.emplace_back("Not a team game type");
        }
    }

    for (const auto& mutator : vote.mutators) {
        const VoteMutatorSchema* schema = find_mutator_schema(*options, mutator.name);
        if (!schema) {
            out.reasons.push_back(std::format("Mutator '{}' is not available here", mutator.name));
            continue;
        }
        for (const auto& value : mutator.values) {
            const VoteMutatorOptionSchema* option = find_option_schema(*schema, value);
            if (!option) {
                // One line per mutator: listing every mismatched option of a
                // mutator that simply changed shape says nothing extra.
                out.reasons.push_back(
                    std::format("Mutator '{}' has different options here", mutator.name));
                break;
            }
            if (value.type == MutatorOptionType::Choice
                && find_choice_index(*option, value.choice_label) < 0) {
                out.reasons.push_back(std::format("Mutator '{}': choice '{}' not available here",
                    mutator.name, value.choice_label));
            }
        }
    }

    out.callable = out.reasons.empty();
    return out;
}

AfVoteCallParams saved_vote_build_params(const SavedVote& vote, const VoteOptionsData& options)
{
    AfVoteCallParams params;
    params.type = vote.type;

    switch (vote.type) {
        case AfVoteType::Level:
        case AfVoteType::Match: {
            params.level = vote.level;
            params.gametype = vote.gametype;
            if (vote.type == AfVoteType::Match) {
                params.team_size = static_cast<uint8_t>(std::clamp<int>(vote.team_size, 1, 8));
            }
            for (const auto& mutator : vote.mutators) {
                const VoteMutatorSchema* schema = find_mutator_schema(options, mutator.name);
                if (!schema) {
                    continue; // saved_vote_check refuses this case before we get here
                }
                VoteMutatorInput input;
                input.mutator_id = schema->id;
                for (const auto& value : mutator.values) {
                    const VoteMutatorOptionSchema* option = find_option_schema(*schema, value);
                    if (!option) {
                        continue;
                    }
                    VoteMutatorOptionInput option_input;
                    option_input.option_id = option->id;
                    option_input.type = option->type;
                    option_input.bool_value = value.bool_value;
                    option_input.int_value = value.int_value;
                    option_input.float_value = value.float_value;
                    if (option->type == MutatorOptionType::Choice) {
                        const int index = find_choice_index(*option, value.choice_label);
                        if (index < 0) {
                            continue;
                        }
                        option_input.choice_index = static_cast<uint8_t>(index);
                    }
                    input.options.push_back(std::move(option_input));
                }
                params.mutators.push_back(std::move(input));
            }
            break;
        }
        case AfVoteType::Extend:
            params.extend_minutes = static_cast<uint8_t>(std::clamp<int>(vote.extend_minutes,
                af_vote_extend_min_minutes, af_vote_extend_max_minutes));
            break;
        default:
            break; // not savable, so not reachable
    }
    return params;
}
