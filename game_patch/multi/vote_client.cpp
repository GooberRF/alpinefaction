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

// Reassembly buffer for the chunked vote-options blob. Chunks of a different
// generation than the one in progress restart the assembly.
struct VoteOptionsCache
{
    VoteOptionsData data;
    bool loaded = false;
    bool stale = true;
    int loaded_generation = -1;

    int pending_generation = -1;
    uint8_t pending_total = 0;
    std::vector<std::vector<uint8_t>> pending_chunks;
    std::vector<bool> pending_received;

    int64_t last_request_ms = 0;
};

VoteOptionsCache g_vote_options;
std::optional<ActiveVoteState> g_active_vote;

// Don't re-ask more than this often; the server also guards per generation.
constexpr int64_t vote_options_request_cooldown_ms = 3000;

// Bounds-checked little-endian reader over the reassembled blob.
class BlobReader
{
public:
    BlobReader(const uint8_t* data, size_t len) : m_data(data), m_len(len) {}

    [[nodiscard]] bool ok() const { return m_ok; }

    uint8_t u8()
    {
        if (!m_ok || m_pos + 1 > m_len) {
            m_ok = false;
            return 0;
        }
        return m_data[m_pos++];
    }

    uint16_t u16()
    {
        if (!m_ok || m_pos + 2 > m_len) {
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
        if (!m_ok || m_pos + 4 > m_len) {
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
        if (!m_ok || m_pos + 4 > m_len) {
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
        if (!m_ok || m_pos + 4 > m_len) {
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
        if (!m_ok || m_pos + len > m_len) {
            m_ok = false;
            return {};
        }
        std::string value(reinterpret_cast<const char*>(m_data + m_pos), len);
        m_pos += len;
        return value;
    }

private:
    const uint8_t* m_data;
    size_t m_len;
    size_t m_pos = 0;
    bool m_ok = true;
};

bool parse_vote_options_blob(const uint8_t* data, size_t len, VoteOptionsData& out)
{
    BlobReader r{data, len};

    const uint8_t version = r.u8();
    if (!r.ok() || version != af_vote_options_blob_version) {
        xlog::warn("vote options: unsupported blob version {}", version);
        return false;
    }

    VoteOptionsData parsed;
    parsed.enabled_vote_mask = r.u16();
    parsed.gametype_prefix_restricted = (r.u8() & AF_VOTE_SERVER_FLAG_GAMETYPE_PREFIX) != 0;

    const uint8_t gametype_count = r.u8();
    parsed.gametypes.reserve(gametype_count);
    for (uint8_t i = 0; i < gametype_count && r.ok(); ++i) {
        VoteGametypeInfo gt;
        gt.id = r.u8();
        gt.is_team_type = (r.u8() & AF_VOTE_GAMETYPE_FLAG_TEAM) != 0;
        gt.name = r.str();
        parsed.gametypes.push_back(std::move(gt));
    }

    const uint8_t mutator_count = r.u8();
    parsed.mutators.reserve(mutator_count);
    for (uint8_t i = 0; i < mutator_count && r.ok(); ++i) {
        VoteMutatorSchema mutator;
        mutator.id = r.u8();
        mutator.name = r.str();
        mutator.label = r.str();

        const uint8_t option_count = r.u8();
        mutator.options.reserve(option_count);
        for (uint8_t o = 0; o < option_count && r.ok(); ++o) {
            VoteMutatorOptionSchema opt;
            opt.id = r.u8();
            opt.name = r.str();
            opt.label = r.str();
            opt.type = static_cast<MutatorOptionType>(r.u8());

            switch (opt.type) {
                case MutatorOptionType::Bool:
                    opt.default_bool = r.u8() != 0;
                    break;
                case MutatorOptionType::Choice:
                    opt.default_choice = r.u8();
                    break;
                case MutatorOptionType::Int:
                    opt.default_int = r.i32();
                    break;
                case MutatorOptionType::Float:
                    opt.default_float = r.f32();
                    break;
                case MutatorOptionType::String:
                    opt.default_string = r.str();
                    break;
                default:
                    xlog::warn("vote options: unknown option type {}", static_cast<int>(opt.type));
                    return false;
            }

            if (opt.type == MutatorOptionType::Choice) {
                const uint8_t choice_count = r.u8();
                opt.choices.reserve(choice_count);
                for (uint8_t c = 0; c < choice_count && r.ok(); ++c) {
                    opt.choices.push_back(r.str());
                }
            }

            mutator.options.push_back(std::move(opt));
        }

        parsed.mutators.push_back(std::move(mutator));
    }

    const uint16_t level_count = r.u16();
    parsed.levels.reserve(level_count);
    for (uint16_t i = 0; i < level_count && r.ok(); ++i) {
        VoteLevelInfo level;
        level.filename = r.str();
        level.natural_gametype = r.u8();
        level.valid_gametype_mask = r.u32();
        parsed.levels.push_back(std::move(level));
    }

    if (!r.ok()) {
        xlog::warn("vote options: truncated blob ({} bytes)", len);
        return false;
    }

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

const char* vote_result_text(AfVoteResult result)
{
    switch (result) {
        case AfVoteResult::Passed:
            return "Vote passed!";
        case AfVoteResult::Failed:
            return "Vote failed!";
        case AfVoteResult::Canceled:
            return "Vote canceled!";
        case AfVoteResult::TimedOut:
            return "Vote timed out!";
    }
    return "Vote ended.";
}

} // namespace

bool vote_options_are_loaded()
{
    return g_vote_options.loaded && !g_vote_options.stale;
}

const VoteOptionsData* vote_options_get()
{
    return g_vote_options.loaded ? &g_vote_options.data : nullptr;
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
    if (!g_vote_options.loaded) {
        return false;
    }
    const auto bit = static_cast<unsigned>(type);
    if (bit >= 16) {
        return false;
    }
    return (g_vote_options.data.enabled_vote_mask & (1u << bit)) != 0;
}

void vote_options_request_if_needed()
{
    if (!rf::is_multi) {
        return;
    }
    if (vote_options_are_loaded()) {
        return;
    }

    const int64_t now = timer::get_i64(1000);
    if (g_vote_options.last_request_ms != 0 &&
        now - g_vote_options.last_request_ms < vote_options_request_cooldown_ms) {
        return;
    }
    g_vote_options.last_request_ms = now;

    if (rf::is_server) {
        af_send_vote_options_data(rf::local_player); // listen host builds it locally
    }
    else {
        af_send_vote_options_request();
    }
}

void vote_options_mark_stale()
{
    g_vote_options.stale = true;
    g_vote_options.last_request_ms = 0;
}

void vote_options_handle_chunk(uint8_t generation, uint8_t seq, uint8_t total,
                               const uint8_t* data, size_t len)
{
    if (total == 0 || seq >= total) {
        xlog::warn("vote options: bad chunk framing (seq={}, total={})", seq, total);
        return;
    }

    // A different generation invalidates whatever is being assembled.
    if (g_vote_options.pending_generation != static_cast<int>(generation) ||
        g_vote_options.pending_total != total) {
        g_vote_options.pending_generation = static_cast<int>(generation);
        g_vote_options.pending_total = total;
        g_vote_options.pending_chunks.assign(total, {});
        g_vote_options.pending_received.assign(total, false);
    }

    g_vote_options.pending_chunks[seq].assign(data, data + len);
    g_vote_options.pending_received[seq] = true;

    for (uint8_t i = 0; i < total; ++i) {
        if (!g_vote_options.pending_received[i]) {
            return; // still waiting for more chunks
        }
    }

    std::vector<uint8_t> blob;
    for (const auto& chunk : g_vote_options.pending_chunks) {
        blob.insert(blob.end(), chunk.begin(), chunk.end());
    }

    g_vote_options.pending_generation = -1;
    g_vote_options.pending_total = 0;
    g_vote_options.pending_chunks.clear();
    g_vote_options.pending_received.clear();

    VoteOptionsData parsed;
    if (!parse_vote_options_blob(blob.data(), blob.size(), parsed)) {
        return;
    }

    g_vote_options.data = std::move(parsed);
    g_vote_options.loaded = true;
    g_vote_options.stale = false;
    g_vote_options.loaded_generation = static_cast<int>(generation);
}

const std::optional<ActiveVoteState>& vote_state_get()
{
    return g_active_vote;
}

int vote_state_seconds_remaining()
{
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

void vote_state_on_start(AfVoteType type, uint16_t time_remaining_sec, uint8_t yes, uint8_t no,
                         uint8_t remaining, bool is_owner, std::string initiator_name,
                         std::string title)
{
    ActiveVoteState state;
    state.type = type;
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

    // Mirrors the legacy "<title> vote started by <name>." line. The name is
    // absent when the server couldn't resolve the owner (late joiner sync).
    std::string by_clause;
    if (g_active_vote->is_owner) {
        by_clause = " by you";
    }
    else if (!g_active_vote->initiator_name.empty()) {
        by_clause = std::format(" by {}", g_active_vote->initiator_name);
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

void vote_state_on_end(AfVoteResult result)
{
    if (g_active_vote) {
        print_vote_chat_line(vote_result_text(result));
    }
    g_active_vote.reset();
    remove_hud_vote_notification();
}

void vote_client_reset()
{
    g_active_vote.reset();
    g_vote_options = VoteOptionsCache{};
}
