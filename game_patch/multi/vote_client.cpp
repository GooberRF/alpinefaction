#include <cstring>
#include <format>
#include <string_view>
#include <utility>
#include <xlog/xlog.h>
#include "vote_client.h"
#include "alpine_packets.h"
#include "../hud/hud.h"
#include "../misc/alpine_settings.h"
#include "../os/os.h"
#include "../rf/multi.h"
#include "../rf/player/player.h"
#include "../rf/sound/sound.h"
#include "../sound/sound.h"

namespace
{

// Accumulator for the streamed vote-options blob plus the parsed result.
//
// `loaded` data stays usable while `stale` is set: a stale cache means "the server
// told us its config changed", and the refresh request it triggers is answered
// with silence when the server's generation is in fact unchanged.
struct VoteOptionsCache
{
    VoteOptionsData data;
    bool loaded = false;
    bool stale = true;
    // Generation of `data`. Sent back with every request so the server can skip
    // re-sending an identical blob.
    uint32_t loaded_generation = 0;

    // Stream in progress (Begin seen, End not yet).
    bool streaming = false;
    uint32_t stream_generation = 0;
    std::vector<uint8_t> stream_bytes;

    int64_t last_request_ms = 0;
};

VoteOptionsCache g_vote_options;
std::optional<ActiveVoteState> g_active_vote;

// Retry interval while we have no blob at all. Once one is loaded a stale marker
// costs exactly one request, so this only paces the initial fetch.
constexpr int64_t vote_options_request_cooldown_ms = 3000;

void vote_options_stream_discard(std::string_view reason)
{
    if (!g_vote_options.streaming) {
        return;
    }
    xlog::warn("vote options: discarding partial stream (generation {}, {} bytes): {}",
               g_vote_options.stream_generation, g_vote_options.stream_bytes.size(), reason);
    g_vote_options.streaming = false;
    g_vote_options.stream_bytes.clear();
    g_vote_options.stream_bytes.shrink_to_fit();
}

// Bounds-checked little-endian reader over the reassembled blob.
class BlobReader
{
public:
    BlobReader(const uint8_t* data, size_t len) : m_data(data), m_len(len) {}

    [[nodiscard]] bool ok() const { return m_ok; }

    uint8_t u8()
    {
        if (!m_ok || 1 > m_len - m_pos) {
            m_ok = false;
            return 0;
        }
        return m_data[m_pos++];
    }

    uint16_t u16()
    {
        if (!m_ok || 2 > m_len - m_pos) {
            m_ok = false;
            return 0;
        }
        uint16_t value = 0;
        std::memcpy(&value, m_data + m_pos, sizeof(value));
        m_pos += sizeof(value);
        return value;
    }

    uint32_t u32()
    {
        if (!m_ok || 4 > m_len - m_pos) {
            m_ok = false;
            return 0;
        }
        uint32_t value = 0;
        std::memcpy(&value, m_data + m_pos, sizeof(value));
        m_pos += sizeof(value);
        return value;
    }

    int32_t i32()
    {
        if (!m_ok || 4 > m_len - m_pos) {
            m_ok = false;
            return 0;
        }
        int32_t value = 0;
        std::memcpy(&value, m_data + m_pos, sizeof(value));
        m_pos += sizeof(value);
        return value;
    }

    float f32()
    {
        if (!m_ok || 4 > m_len - m_pos) {
            m_ok = false;
            return 0.0f;
        }
        float value = 0.0f;
        std::memcpy(&value, m_data + m_pos, sizeof(value));
        m_pos += sizeof(value);
        return value;
    }

    std::string str()
    {
        const uint8_t len = u8();
        if (!m_ok || len > m_len - m_pos) {
            m_ok = false;
            return {};
        }
        std::string value(reinterpret_cast<const char*>(m_data + m_pos), len);
        m_pos += len;
        return value;
    }

    [[nodiscard]] size_t remaining() const { return m_ok ? m_len - m_pos : 0; }
    [[nodiscard]] const uint8_t* cur() const { return m_data + m_pos; }

    // Step over a length-prefixed descriptor body (or its unparsed tail). The
    // caller has already checked `n <= remaining()`.
    void skip(size_t n)
    {
        if (!m_ok || n > m_len - m_pos) {
            m_ok = false;
            return;
        }
        m_pos += n;
    }

private:
    const uint8_t* m_data;
    size_t m_len;
    size_t m_pos = 0;
    bool m_ok = true;
};

// One option_descriptor body. Returns false when the option cannot be
// represented — an unknown option TYPE, or a malformed body. The caller then
// omits the option and the server's own default applies to any vote that doesn't
// mention it, so the mutator stays fully usable.
bool parse_option_descriptor(BlobReader& r, VoteMutatorOptionSchema& out)
{
    out.id = r.u8();
    out.name = r.str();
    out.label = r.str();
    const uint8_t type_raw = r.u8();
    if (!r.ok()) {
        return false;
    }
    out.type = static_cast<MutatorOptionType>(type_raw);

    switch (out.type) {
        case MutatorOptionType::Bool:
            out.default_bool = r.u8() != 0;
            break;
        case MutatorOptionType::Choice:
            out.default_choice = r.u8();
            break;
        case MutatorOptionType::Int:
            out.default_int = r.i32();
            break;
        case MutatorOptionType::Float:
            out.default_float = r.f32();
            break;
        case MutatorOptionType::String:
            out.default_string = r.str();
            break;
        default:
            // A newer server added an option type this build has no encoding for.
            // Nothing after the type byte can be interpreted, which is exactly why
            // the descriptor is length-prefixed.
            xlog::info("vote options: option '{}' has unknown type {}; omitting it", out.name, type_raw);
            return false;
    }

    if (out.type == MutatorOptionType::Choice) {
        const uint8_t choice_count = r.u8();
        if (!r.ok()) {
            return false;
        }
        out.choices.reserve(choice_count);
        for (uint8_t c = 0; c < choice_count && r.ok(); ++c) {
            out.choices.push_back(r.str());
        }
    }

    // Trailing bytes inside the body are fields a newer server added; ignored.
    return r.ok();
}

// One mutator_descriptor body. Returns false when the descriptor itself is
// malformed, in which case the whole mutator is dropped (the rest of the blob is
// still intact because the descriptor is length-prefixed).
bool parse_mutator_descriptor(BlobReader& r, VoteMutatorSchema& out)
{
    out.id = r.u8();
    out.name = r.str();
    out.label = r.str();
    const uint8_t option_count = r.u8();
    if (!r.ok()) {
        return false;
    }

    out.options.reserve(option_count);
    for (uint8_t o = 0; o < option_count; ++o) {
        const uint8_t body_len = r.u8();
        if (!r.ok() || body_len > r.remaining()) {
            return false; // the declared option count doesn't fit the body
        }
        BlobReader body{r.cur(), body_len};
        VoteMutatorOptionSchema opt;
        if (parse_option_descriptor(body, opt)) {
            out.options.push_back(std::move(opt));
        }
        r.skip(body_len); // unconditional: parsed or not, the next option starts here
    }

    // Game type restriction, appended after the options.
    if (r.remaining() >= sizeof(uint32_t)) {
        const uint32_t mask = r.u32();
        if (r.ok()) {
            out.valid_gametype_mask = mask;
        }
    }

    return true;
}

// One option value inside a declaration body. Values are packed back to back, so
// an unknown TYPE makes everything after it unreadable and the caller drops the
// whole declaration rather than just this value.
bool parse_declaration_value(BlobReader& r, VoteMutatorDeclValue& out)
{
    out.option_id = r.u8();
    const uint8_t type_raw = r.u8();
    if (!r.ok()) {
        return false;
    }
    out.type = static_cast<MutatorOptionType>(type_raw);

    switch (out.type) {
        case MutatorOptionType::Bool:
            out.bool_value = r.u8() != 0;
            break;
        case MutatorOptionType::Choice:
            out.choice_index = r.u8();
            break;
        case MutatorOptionType::Int:
            out.int_value = r.i32();
            break;
        case MutatorOptionType::Float:
            out.float_value = r.f32();
            break;
        case MutatorOptionType::String:
            out.string_value = r.str();
            break;
        default:
            xlog::debug("vote options: declared option {} has unknown type {}; dropping its declaration",
                        out.option_id, type_raw);
            return false;
    }

    return r.ok();
}

// One declaration body. Returns false when it cannot be decoded, in which case
// the caller drops that mutator alone - the rest of the set is still intact
// because every declaration is length-prefixed.
bool parse_declaration(BlobReader& r, VoteMutatorDecl& out)
{
    out.mutator_id = r.u8();
    const uint8_t value_count = r.u8();
    if (!r.ok()) {
        return false;
    }

    out.values.reserve(value_count);
    for (uint8_t v = 0; v < value_count; ++v) {
        VoteMutatorDeclValue value;
        if (!parse_declaration_value(r, value)) {
            return false;
        }
        out.values.push_back(std::move(value));
    }

    // Trailing bytes inside the body are fields a newer server added; ignored.
    return true;
}

// A whole declaration set (the config-declared mutators of one rules scope).
// Returns false only when the set itself is unreadable - a truncated count or a
// declaration length that overruns what is left.
bool parse_declaration_set(BlobReader& r, std::vector<VoteMutatorDecl>& out)
{
    const uint8_t decl_count = r.u8();
    if (!r.ok()) {
        return false;
    }

    out.reserve(decl_count);
    for (uint8_t d = 0; d < decl_count; ++d) {
        const uint16_t body_len = r.u16();
        if (!r.ok() || body_len > r.remaining()) {
            return false;
        }
        BlobReader body{r.cur(), body_len};
        VoteMutatorDecl decl;
        if (parse_declaration(body, decl)) {
            out.push_back(std::move(decl));
        }
        else {
            xlog::debug("vote options: unparseable mutator declaration ({} bytes); skipping it", body_len);
        }
        r.skip(body_len); // unconditional: the next declaration starts here either way
    }

    return true;
}

bool parse_vote_options_blob(const uint8_t* data, size_t len, VoteOptionsData& out)
{
    BlobReader r{data, len};

    const uint8_t version = r.u8();
    if (!r.ok()) {
        xlog::warn("vote options: empty blob");
        return false;
    }
    // MAJOR version: anything at or below what this build knows is parseable,
    // because every additive change is skippable (see af_vote_options_blob_version).
    if (version > af_vote_options_blob_version) {
        xlog::warn("vote options: blob version {} is newer than this client understands ({})", version,
                   af_vote_options_blob_version);
        return false;
    }

    VoteOptionsData parsed;
    parsed.enabled_vote_mask = r.u32();
    const uint8_t server_flags = r.u8();
    parsed.gametype_prefix_restricted = (server_flags & AF_VOTE_SERVER_FLAG_GAMETYPE_PREFIX) != 0;
    parsed.rotation_preserve_supported = (server_flags & AF_VOTE_SERVER_FLAG_ROTATION_PRESERVE) != 0;

    // Game type entries are length-prefixed, so fields a newer server appends
    // inside one are skipped instead of desyncing everything behind it.
    const uint8_t gametype_count = r.u8();
    if (!r.ok()) {
        xlog::warn("vote options: truncated blob before the game type section ({} bytes)", len);
        return false;
    }
    parsed.gametypes.reserve(gametype_count);
    for (uint8_t i = 0; i < gametype_count; ++i) {
        const uint16_t body_len = r.u16();
        if (!r.ok() || body_len > r.remaining()) {
            xlog::warn("vote options: truncated blob in the game type section ({} bytes)", len);
            return false;
        }
        // Bounded to the body, so a bad inner length can't reach the next entry.
        BlobReader body{r.cur(), body_len};
        VoteGametypeInfo gt;
        gt.id = body.u8();
        gt.is_team_type = (body.u8() & AF_VOTE_GAMETYPE_FLAG_TEAM) != 0;
        gt.name = body.str();
        if (body.ok()) {
            parsed.gametypes.push_back(std::move(gt));
        }
        else {
            xlog::warn("vote options: unparseable game type entry ({} bytes); skipping it", body_len);
        }
        r.skip(body_len); // unconditional: the next entry starts here either way
    }

    // Mutator and option descriptors are length-prefixed, so a mutator this build
    // can't make sense of is skipped instead of desyncing everything after it.
    const uint8_t mutator_count = r.u8();
    if (!r.ok()) {
        xlog::warn("vote options: truncated blob before the mutator section ({} bytes)", len);
        return false;
    }
    parsed.mutators.reserve(mutator_count);
    for (uint8_t i = 0; i < mutator_count; ++i) {
        const uint16_t body_len = r.u16();
        if (!r.ok() || body_len > r.remaining()) {
            xlog::warn("vote options: truncated blob in the mutator section ({} bytes)", len);
            return false;
        }
        BlobReader body{r.cur(), body_len};
        VoteMutatorSchema mutator;
        if (parse_mutator_descriptor(body, mutator)) {
            parsed.mutators.push_back(std::move(mutator));
        }
        else {
            xlog::warn("vote options: unparseable mutator descriptor ({} bytes); skipping it", body_len);
        }
        r.skip(body_len); // unconditional: the next descriptor starts here either way
    }

    // Level entries are length-prefixed too, same contract as the entries above.
    const uint16_t level_count = r.u16();
    if (!r.ok()) {
        xlog::warn("vote options: truncated blob before the level section ({} bytes)", len);
        return false;
    }
    // Smallest possible entry: u16 body len + str len byte + natural gametype + mask + flags.
    constexpr size_t min_level_entry = 2 + 1 + 1 + 4 + 1;
    parsed.levels.reserve(std::min<size_t>(level_count, r.remaining() / min_level_entry));
    for (uint16_t i = 0; i < level_count; ++i) {
        const uint16_t body_len = r.u16();
        if (!r.ok() || body_len > r.remaining()) {
            xlog::warn("vote options: truncated blob in the level section ({} bytes)", len);
            return false;
        }
        BlobReader body{r.cur(), body_len};
        VoteLevelInfo level;
        level.filename = body.str();
        level.natural_gametype = body.u8();
        level.valid_gametype_mask = body.u32();
        level.allowed_for_vote = (body.u8() & AF_VOTE_LEVEL_FLAG_ALLOWED) != 0;
        // Read the entry's own success BEFORE the appended baseline set below, so
        // trouble in the addition can only cost the pre-selection, never the level.
        const bool entry_ok = body.ok();

        // Baseline mutator set, appended after the flags byte. Absent from a blob
        // built before it existed, which reads as "inherit the base set" - the
        // same thing an old server implied by having no per-level sets at all.
        if (entry_ok && body.remaining() > 0
            && body.u8() == static_cast<uint8_t>(AfVoteLevelBaseline::Explicit)) {
            std::vector<VoteMutatorDecl> decls;
            if (parse_declaration_set(body, decls)) {
                // An explicit EMPTY set is meaningful ("runs no mutators") and is
                // deliberately not the same as leaving this nullopt.
                level.mutator_decls = std::move(decls);
            }
            else {
                xlog::debug("vote options: unparseable baseline mutator set for level '{}'; "
                            "falling back to the base set", level.filename);
            }
        }

        if (entry_ok) {
            parsed.levels.push_back(std::move(level));
        }
        else {
            xlog::warn("vote options: unparseable level entry ({} bytes); skipping it", body_len);
        }
        r.skip(body_len); // unconditional: the next entry starts here either way
    }

    // The base mutator set, appended after the level section. Failing to read it
    // costs the vote panel's pre-selection and nothing else, so it never fails the
    // blob: a blob from a server built before it existed simply ends here.
    if (r.remaining() > 0 && !parse_declaration_set(r, parsed.base_mutator_decls)) {
        xlog::debug("vote options: unparseable base mutator set; the vote panel will pre-select nothing");
        parsed.base_mutator_decls.clear();
    }

    // Anything left over is a section a newer server appended; ignored on purpose.
    out = std::move(parsed);
    return true;
}

// Structured servers send no vote chat text, so mirror the legacy wording
// locally to keep chat history the same as what pre-1.4 clients see.
void print_vote_chat_line(std::string_view msg)
{
    rf::multi_chat_print(rf::String{msg}, rf::ChatMsgColor::gold_white, rf::String{"Server: "});
    if (!g_alpine_game_config.simple_server_chat_msgs) {
        rf::snd_play(stock_sound_id::end_voice, 0, 0.0f, 1.0f);
    }
}

// Fallback wording when the server sent no outcome detail. Normally the detail
// string carries the exact line legacy clients receive.
const char* vote_result_text(AfVoteResult result, bool passed)
{
    switch (result) {
        case AfVoteResult::Passed:
        case AfVoteResult::TimedOut:
            return passed ? "Vote passed!" : "Vote failed!";
        case AfVoteResult::Failed:
            return "Vote failed!";
        case AfVoteResult::Canceled:
            return "Vote canceled!";
    }
    return "Vote ended.";
}

} // namespace

bool vote_options_are_loaded()
{
    // Stale-but-loaded still counts: see VoteOptionsCache. The refresh happens in
    // the background and replaces the data in place when it arrives.
    return g_vote_options.loaded;
}

const VoteOptionsData* vote_options_get()
{
    return g_vote_options.loaded ? &g_vote_options.data : nullptr;
}

uint32_t vote_options_loaded_generation()
{
    return g_vote_options.loaded_generation;
}

bool vote_level_allows_gametype(const VoteLevelInfo& level, uint8_t game_type)
{
    if (game_type >= 32) {
        return false;
    }
    return (level.valid_gametype_mask & (1u << game_type)) != 0;
}

bool vote_level_allows_default_gametype(const VoteLevelInfo& level)
{
    return vote_level_allows_gametype(level, level.natural_gametype);
}

bool vote_options_is_type_enabled(AfVoteType type)
{
    // Deliberately keyed on `loaded` alone, matching vote_options_are_loaded():
    // a stale mask is the best answer available and the server validates anyway.
    if (!g_vote_options.loaded) {
        return false;
    }
    // The mask is a u32; shifting by 32 or more is undefined, and a bit that far
    // out cannot be a type this build knows anyway.
    const auto bit = static_cast<unsigned>(type);
    if (bit >= 32) {
        return false;
    }
    return (g_vote_options.data.enabled_vote_mask & (1u << bit)) != 0;
}

void vote_options_request_if_needed()
{
    // The listen-server host has no vote UI at all (it can neither call nor cast),
    // so it never needs the schema and there is no local build path.
    if (!rf::is_multi || rf::is_server) {
        return;
    }
    if (g_vote_options.loaded && !g_vote_options.stale) {
        return;
    }

    const int64_t now = timer::get_i64(1000);
    if (g_vote_options.last_request_ms != 0 &&
        now - g_vote_options.last_request_ms < vote_options_request_cooldown_ms) {
        return;
    }
    g_vote_options.last_request_ms = now;

    // Asking clears `stale` even though nothing has arrived yet: the request tells
    // the server the generation we hold, and the server answers with silence when
    // that is already current. Retrying would then loop forever. A cache we don't
    // have yet keeps retrying on the cooldown above.
    if (g_vote_options.loaded) {
        g_vote_options.stale = false;
    }

    af_send_vote_options_request(g_vote_options.loaded, g_vote_options.loaded_generation);
}

void vote_options_mark_stale()
{
    if (!g_vote_options.loaded) {
        // Nothing cached yet, so there is nothing to refresh; the UI asks on demand
        // (`stale` is already true in this state).
        return;
    }

    g_vote_options.stale = true;
    g_vote_options.last_request_ms = 0;

    // Refresh right away rather than waiting for the UI to be opened. A UI only
    // asks while it has nothing to show, and stale data now counts as loaded, so
    // otherwise nothing would ever ask. The request costs 5 bytes and the server
    // answers with silence when its generation hasn't actually changed.
    vote_options_request_if_needed();
}

void vote_options_stream_begin(uint32_t generation, uint32_t total_bytes)
{
    vote_options_stream_discard("superseded by a new stream");

    if (total_bytes > af_vote_options_max_blob_size) {
        xlog::error("vote options: server announced a {} byte blob, above the {} byte cap; ignoring it",
                    total_bytes, af_vote_options_max_blob_size);
        return;
    }

    g_vote_options.streaming = true;
    g_vote_options.stream_generation = generation;
    g_vote_options.stream_bytes.clear();
    g_vote_options.stream_bytes.reserve(total_bytes);
}

void vote_options_stream_data(uint32_t generation, const uint8_t* data, size_t len)
{
    if (!g_vote_options.streaming) {
        xlog::warn("vote options: data frame with no stream in progress (generation {})", generation);
        return;
    }
    if (generation != g_vote_options.stream_generation) {
        // The server rebuilt the blob mid-stream. Everything accumulated belongs to
        // the old generation and the new one will arrive with its own Begin.
        vote_options_stream_discard("generation changed mid-stream");
        return;
    }
    if (g_vote_options.stream_bytes.size() + len > af_vote_options_max_blob_size) {
        vote_options_stream_discard("accumulation exceeded the blob size cap");
        return;
    }
    g_vote_options.stream_bytes.insert(g_vote_options.stream_bytes.end(), data, data + len);
}

void vote_options_stream_end(uint32_t generation)
{
    if (!g_vote_options.streaming) {
        xlog::warn("vote options: end frame with no stream in progress (generation {})", generation);
        return;
    }
    if (generation != g_vote_options.stream_generation) {
        vote_options_stream_discard("generation changed before the end frame");
        return;
    }

    std::vector<uint8_t> blob = std::move(g_vote_options.stream_bytes);
    g_vote_options.streaming = false;
    g_vote_options.stream_bytes.clear();
    g_vote_options.stream_bytes.shrink_to_fit();

    VoteOptionsData parsed;
    if (!parse_vote_options_blob(blob.data(), blob.size(), parsed)) {
        return; // keep whatever was already loaded rather than blanking the UI
    }

    g_vote_options.data = std::move(parsed);
    g_vote_options.loaded = true;
    g_vote_options.stale = false;
    g_vote_options.loaded_generation = generation;
    xlog::debug("vote options: loaded {} bytes (generation {}): {} levels, {} mutators", blob.size(),
                generation, g_vote_options.data.levels.size(), g_vote_options.data.mutators.size());
}

// Client-side safety net only. If the server's End event never arrives (dropped,
// or a server path that fails to emit one), a stuck g_active_vote would pin the
// HUD at "0s left" and make CALL VOTE unreachable for the rest of the session.
// The server stays authoritative: this fires only well past the deadline, and a
// late End is still handled because vote_state_on_end guards on g_active_vote.
// It cannot fire early for a late-joiner sync — end_timestamp_ms is derived from
// the actual remaining seconds the server sent.
static constexpr int64_t vote_state_expiry_grace_ms = 5000;

static void vote_state_expire_if_overdue()
{
    if (!g_active_vote) {
        return;
    }
    if (timer::get_i64(1000) <= g_active_vote->end_timestamp_ms + vote_state_expiry_grace_ms) {
        return;
    }
    xlog::warn("vote state: no end event within {} ms of the deadline; clearing locally",
               vote_state_expiry_grace_ms);
    g_active_vote.reset();
    remove_hud_vote_notification();
}

const std::optional<ActiveVoteState>& vote_state_get()
{
    vote_state_expire_if_overdue();
    return g_active_vote;
}

int vote_state_seconds_remaining()
{
    vote_state_expire_if_overdue();
    if (!g_active_vote) {
        return 0;
    }
    const int64_t left_ms = g_active_vote->end_timestamp_ms - timer::get_i64(1000);
    if (left_ms <= 0) {
        return 0;
    }
    return static_cast<int>((left_ms + 999) / 1000);
}

void vote_state_mark_local_voted()
{
    if (g_active_vote) {
        g_active_vote->has_voted = true;
    }
}

void vote_state_on_start(uint8_t type_raw, uint16_t time_remaining_sec, uint8_t yes, uint8_t no,
                         uint8_t remaining, bool is_owner, bool is_sync, std::string initiator_name,
                         std::string title)
{
    ActiveVoteState state;
    state.type_raw = type_raw;
    if (!state.known_type()) {
        // A newer server added a vote type this build predates. The vote still
        // runs normally here: the server counts this client as an eligible voter
        // either way, so refusing the event would only make it a silent
        // non-voter that drags the tally out to the deadline.
        xlog::info("vote state: server started vote type {}, which this build does not know; "
                   "showing it as '{}'", type_raw, title.empty() ? "Vote in progress" : title);
    }
    state.title = std::move(title);
    state.initiator_name = std::move(initiator_name);
    state.yes = yes;
    state.no = no;
    state.remaining = remaining;
    // 64-bit monotonic ms; rf::timer::get truncates to 32 bits and wraps.
    state.end_timestamp_ms = timer::get_i64(1000) + static_cast<int64_t>(time_remaining_sec) * 1000;
    state.is_owner = is_owner;
    state.has_voted = is_owner; // the caller's own vote is counted server-side
    g_active_vote = std::move(state);

    draw_hud_vote_notification(g_active_vote->title);

    // "<title> vote started by <name>." The name is absent when the
    // server couldn't resolve the owner (late joiner sync).
    std::string by_clause;
    if (g_active_vote->is_owner) {
        by_clause = " by you";
    }
    else if (!g_active_vote->initiator_name.empty()) {
        by_clause = std::format(" by {}", g_active_vote->initiator_name);
    }

    if (is_sync) {
        // Joined while the vote was already running: the "VOTE STARTING" banner
        // would be a lie, so report the state instead.
        print_vote_chat_line(std::format("Vote in progress: {}{} - Yes: {} No: {} Waiting: {} ({}s left)",
            g_active_vote->title, by_clause, yes, no, remaining, time_remaining_sec));
        return;
    }

    print_vote_chat_line(std::format("\n=============== VOTE STARTING ===============\n{} vote started{}.",
        g_active_vote->title, by_clause));
}

void vote_state_on_update(uint8_t yes, uint8_t no, uint8_t remaining)
{
    if (!g_active_vote) {
        return;
    }
    g_active_vote->yes = yes;
    g_active_vote->no = no;
    g_active_vote->remaining = remaining;

    print_vote_chat_line(std::format("Vote status: Yes: {} No: {} Waiting: {}", yes, no, remaining));
}

void vote_state_on_end(AfVoteResult result, bool passed, std::string detail)
{
    if (g_active_vote) {
        // Legacy clients get "Vote timed out!" and then the outcome line, so print
        // both. Without the second line a timed-out vote that PASSED would leave
        // chat saying "timed out" while the server changes the level.
        if (result == AfVoteResult::TimedOut) {
            print_vote_chat_line("Vote timed out!");
        }
        print_vote_chat_line(detail.empty() ? std::string_view{vote_result_text(result, passed)}
                                            : std::string_view{detail});
    }
    g_active_vote.reset();
    remove_hud_vote_notification();
}

void vote_client_reset()
{
    g_active_vote.reset();
    // Drops any partial stream along with the parsed cache: the accumulated bytes
    // belong to the server we just left.
    g_vote_options = VoteOptionsCache{};
    // Also drop the HUD prompt: without this a vote left running on the server we
    // just left would keep its notification up across a reconnect elsewhere.
    remove_hud_vote_notification();
}
