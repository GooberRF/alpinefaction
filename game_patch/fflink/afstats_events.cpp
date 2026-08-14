#include "afstats_events.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <format>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <json.hpp>
#include <xlog/xlog.h>

#include <common/HttpRequest.h>
#include <common/utils/list-utils.h>
#include <common/utils/os-utils.h>
#include <common/version/version.h>

#include "../misc/player.h"
#include "../multi/alpine_packets.h"
#include "../multi/bagman.h"
#include "../multi/gametype.h"
#include "../multi/multi.h"
#include "../multi/salvage.h"
#include "../multi/server_internal.h"
#include "../multi/wipeout.h"
#include "../os/console.h"
#include "../rf/clutter.h"
#include "../rf/level.h"
#include "../rf/multi.h"
#include "../rf/os/os.h"
#include "../rf/player/player.h"
#include "afstats_client.h" // AFSTATS_VERIFICATION_LOGGING
#include "fflink_session.h"
#include "fflink_utils.h"

namespace afstats {

namespace {

constexpr const char* k_events_url = "https://link.factionfiles.com/afstats/v1/events.php";
constexpr const char* k_user_agent = AF_USER_AGENT_SUFFIX("AFStatsEvents");

constexpr int k_envelope_version = 1;

// Hard caps FactionFiles enforces. Staying under them client-side means
// the 413 split path only ever fires on a contract mismatch.
constexpr size_t k_max_events_per_batch = 500;
constexpr size_t k_max_queued_events = 20000;

// The ack body is a few dozen bytes; cap the read so a broken or hostile endpoint
// can't stream unbounded data into worker memory.
constexpr size_t k_max_response_bytes = 256 * 1024;

constexpr auto k_pulse_interval = std::chrono::seconds(5);
constexpr auto k_rate_limit_backoff = std::chrono::seconds(30);

// Minimum spacing between successive event POSTs, enforced even while draining a backlog
// at ack rate. Without it, a client packet flood grows the queue fast enough that every
// ack immediately re-arms the pulse, turning post->ack->post into a POST (and TLS
// handshake) storm at RTT frequency. With a 500 ms floor and 500 events per batch even a
// full 20k backlog still drains in tens of seconds, but the outbound rate stays capped
// well below RTT. A round_end flush is not client-floodable and deliberately bypasses this.
constexpr auto k_min_post_interval = std::chrono::milliseconds(500);
constexpr int64_t k_ping_sample_interval_ms = 10000;

constexpr unsigned long k_connect_timeout_ms = 3000;
constexpr unsigned long k_receive_timeout_ms = 5000;

// The shutdown path gets its own short timeouts so one unreachable host cannot hold
// the process open past the ~2s shutdown wall: a single unreachable attempt stays
// under the deadline, and the flush breaks on the first failure.
constexpr unsigned long k_shutdown_connect_timeout_ms = 500;
constexpr unsigned long k_shutdown_receive_timeout_ms = 1000;

constexpr size_t k_max_name_len = 32;
constexpr size_t k_max_string_len = 64;

// -------------------------------------------------------------------------
// Event records. Gameplay hooks fill these in; JSON is only ever built when a
// batch is serialized for transmission.
// -------------------------------------------------------------------------

struct Vec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// Never true, but dependent on T so it only fires when a std::visit branch is actually
// instantiated. Backs the final-else static_assert that fails the build if an event
// payload alternative is added without a serialization branch.
template <typename>
inline constexpr bool always_false_v = false;

// Roster/player `flags` bitfield.
constexpr uint8_t k_player_flag_bot = 1;
constexpr uint8_t k_player_flag_spectating = 2;
constexpr uint8_t k_player_flag_idle = 4;

// `platform` registry.
constexpr uint8_t k_platform_windows = 0;
constexpr uint8_t k_platform_wine = 1;

// `kind` registry for player_join.
constexpr uint8_t k_join_kind_human = 0;
constexpr uint8_t k_join_kind_bot = 1;

// A roster entry plus the summary block round_end and player_leave
// append to it. `has_summary` selects which of the two shapes is serialized.
struct RosterEntry
{
    std::string player;
    std::string name;
    uint8_t team = team_none;
    uint8_t flags = 0;
    int handicap = 0;

    bool has_summary = false;
    int score = 0;
    uint32_t kills = 0;
    uint32_t deaths = 0;
    uint32_t assists = 0;
    uint32_t caps = 0;
    uint32_t shots_fired = 0;
    uint32_t shots_hit = 0;
    float damage_dealt = 0.0f;
    float damage_taken = 0.0f;
    uint32_t highest_streak = 0;
    int64_t time_played_ms = 0;
    int64_t time_spectating_ms = 0;
    int64_t time_idle_ms = 0;
    int avg_ping = 0;
};

using SettingValue = std::variant<bool, int64_t, double, std::string>;

struct MutatorRecord
{
    std::string name;
    std::vector<std::pair<std::string, SettingValue>> settings;
};

struct EvServerHello
{
    std::string server_name;
    uint16_t port = 0;
    std::string af_version;
    uint8_t platform = k_platform_windows;
    uint8_t max_players = 0;
};

struct EvGap
{
    uint64_t dropped = 0;
    uint64_t first_seq = 0;
    uint64_t last_seq = 0;
};

struct EvPlayerJoin
{
    std::string upssk;
    std::string name;
    uint8_t kind = k_join_kind_human;
    std::string client;
};

struct EvPlayerRemediate
{
    std::string upssk;
    std::string pssk;
};

struct EvPlayerRename
{
    std::string player;
    std::string name;
};

struct EvPlayerLeave
{
    uint8_t reason = 0;
    RosterEntry stats;
};

struct EvRoundStart
{
    std::string tc_mod;
    std::string level_file;
    std::string level_name;
    uint8_t gametype = 0;
    uint32_t time_limit_s = 0;
    int64_t win_condition = 0;
    bool overtime_enabled = false;
    uint32_t af_flags = 0;
    uint8_t rf_flags = 0;
    uint32_t gi_flags = 0;
    uint8_t match_state = 0;
    std::vector<MutatorRecord> mutators;
    std::vector<std::pair<std::string, SettingValue>> gametype_settings;
    std::vector<RosterEntry> roster;
};

struct EvRoundEnd
{
    uint8_t end_type = 0;
    uint64_t duration_ms = 0;
    bool overtime = false;
    int red_score = 0;
    int blue_score = 0;
    std::vector<RosterEntry> players;
};

struct EvKill
{
    std::string victim;
    std::string killer; // empty = world/environment death
    int weapon = -1;      // negative = unknown, reported as null
    uint8_t damage_type = 0;
    uint8_t flags = 0; // af_kill_info_flags, verbatim
    std::vector<std::string> assists;
    Vec3 victim_pos;
    bool has_killer_pos = false;
    Vec3 killer_pos;
};

struct EvSpawn
{
    std::string player;
    Vec3 pos;
};

struct EvDamage
{
    std::string attacker; // empty = environmental
    std::string victim;
    int weapon = -1;
    uint8_t damage_type = 0;
    float amount = 0.0f;
    uint32_t hits = 0;
};

struct EvAccuracy
{
    std::string player;
    int weapon = -1;
    uint32_t fired = 0;
    uint32_t hit = 0;
    uint32_t hit_head = 0;
};

struct EvStatus
{
    std::string player;
    uint8_t kind = 0;
    bool has_value = false;
    int value = 0;
};

struct EvItemPickup
{
    std::string player;
    int item = -1;
    Vec3 pos;
    uint32_t respawn_ms = 0;
};

struct EvFlagEvent
{
    uint8_t kind = 0;
    uint8_t team = team_none;
    std::string player; // empty = null (timeout / reset)
    Vec3 pos;
};

struct EvPointEvent
{
    uint8_t hill = 0;
    uint8_t kind = 0;
    uint8_t owner = team_none;
    std::vector<std::string> players;
    bool locked = false;
};

struct EvBagmanEvent
{
    uint8_t kind = 0;
    std::string player; // empty = null
    bool has_from = false;
    uint8_t from = 0;
    Vec3 pos;
};

struct EvGgLevelup
{
    std::string player;
    uint32_t level = 0;
    int weapon = -1;
};

struct EvMatchStart
{
    uint32_t team_size = 0;
    std::vector<std::string> participants;
};

struct EvMatchEnd
{
    uint8_t result = 0;
    uint8_t winner_team = team_none;
    std::string winner_player; // empty = null
};

struct EvVoteCalled
{
    uint8_t vote_type = 0;
    std::string initiator;
    std::string target; // empty = null
    std::string detail;
};

struct EvVoteEnded
{
    uint8_t vote_type = 0;
    uint8_t result = 0;
    uint32_t yes = 0;
    uint32_t no = 0;
    uint32_t eligible = 0;
};

struct EvGeomod
{
    Vec3 pos;
    float scale = 0.0f;
    bool rf2_style = false;
    std::string player; // empty = null (unresolved shooter)
    int weapon = -1;    // negative = null; paired with player (null when player is)
};

struct EvClutterDestroyed
{
    bool has_clutter_type = false;
    uint32_t clutter_type = 0; // clutter.tbl info_index; absent for an alpine mesh
    bool has_uid = false;
    int uid = 0;
    std::string player; // empty = null (world / environmental)
    int weapon = -1;    // negative = null; paired with player
    uint8_t damage_type = 0;
    Vec3 pos;
};

struct EvDetailBrushDestroyed
{
    uint8_t material = 0; // rf::DetailMaterial registry
    bool has_room_uid = false;
    int room_uid = 0;
    std::string player; // empty = null (world / environmental)
    int weapon = -1;    // negative = null; paired with player
    uint8_t damage_type = 0;
    Vec3 pos;
};

using EventPayload =
    std::variant<EvServerHello, EvGap, EvPlayerJoin, EvPlayerRemediate, EvPlayerRename, EvPlayerLeave,
                 EvRoundStart, EvRoundEnd, EvKill, EvSpawn, EvDamage, EvAccuracy, EvStatus,
                 EvItemPickup, EvFlagEvent, EvPointEvent, EvBagmanEvent, EvGgLevelup, EvMatchStart,
                 EvMatchEnd, EvVoteCalled, EvVoteEnded, EvGeomod, EvClutterDestroyed,
                 EvDetailBrushDestroyed>;

struct Event
{
    uint64_t seq = 0;
    uint64_t t = 0;
    uint32_t round = 0;
    EventPayload payload;
};

struct PendingBatch
{
    uint32_t batch = 0;
    uint64_t first_seq = 0;
    uint64_t last_seq = 0;
    uint64_t frozen_at_ms = 0; // uptime when the events were drained out of the queue
    int attempts = 0;
    std::vector<Event> events;
};

// -------------------------------------------------------------------------
// Module state. Everything below is main-thread-owned unless noted.
// -------------------------------------------------------------------------

bool g_session_started = false;
// Re-entrancy guard for the post-hello identity re-announce.
bool g_reannouncing = false;
std::string g_session_id;
// Bumped every time a session starts. A frozen batch carries the generation it was
// built under; a completion from a previous session's worker is ignored instead of
// being applied to the new session's state.
uint32_t g_session_generation = 0;
uint64_t g_next_seq = 1;
uint32_t g_next_batch = 1;
uint32_t g_round = 1;
bool g_round_open = false;
bool g_any_round_started = false;
// A cancel and the limbo that follows it both reach the match-end path; this makes
// the second one a no-op.
bool g_match_open = false;
std::optional<RoundEndType> g_pending_end_type;

std::deque<Event> g_queue;
std::deque<PendingBatch> g_pending;

// Windowed aggregates. Per-hit volume would dominate the stream, so
// these accumulate beside the queue and only materialize at batch build time.
using DamageKey = std::tuple<std::string, std::string, int, uint8_t>; // attacker, victim, weapon, dt
using AccuracyKey = std::pair<std::string, int>;                      // player, weapon

struct DamageAccum
{
    float amount = 0.0f;
    uint32_t hits = 0;
};

struct AccuracyAccum
{
    uint32_t fired = 0;
    uint32_t hit = 0;
    uint32_t hit_head = 0;
};

// Non-owning lookup keys. on_damage and on_weapon_fired run on every damage
// application and every trigger pull, and stats keys are 30-32 chars -- past the small
// string buffer -- so materializing the owning key just to index the map cost two heap
// allocations per damage tick even when the entry already existed. The maps use a
// transparent comparator so the hot path can look up with these instead and only build
// the owning key when a genuinely new bucket is created.
struct DamageKeyView
{
    std::string_view attacker;
    std::string_view victim;
    int weapon = -1;
    uint8_t damage_type = 0;
};

struct AccuracyKeyView
{
    std::string_view player;
    int weapon = -1;
};

// Written out elementwise rather than leaning on heterogeneous tuple/pair comparison,
// which is not portable between the MSVC and MinGW builds.
struct DamageKeyLess
{
    using is_transparent = void;

    static DamageKeyView view(const DamageKey& k)
    {
        return DamageKeyView{std::get<0>(k), std::get<1>(k), std::get<2>(k), std::get<3>(k)};
    }
    static bool less(const DamageKeyView& a, const DamageKeyView& b)
    {
        if (a.attacker != b.attacker) {
            return a.attacker < b.attacker;
        }
        if (a.victim != b.victim) {
            return a.victim < b.victim;
        }
        if (a.weapon != b.weapon) {
            return a.weapon < b.weapon;
        }
        return a.damage_type < b.damage_type;
    }
    bool operator()(const DamageKey& a, const DamageKey& b) const { return less(view(a), view(b)); }
    bool operator()(const DamageKey& a, const DamageKeyView& b) const { return less(view(a), b); }
    bool operator()(const DamageKeyView& a, const DamageKey& b) const { return less(a, view(b)); }
};

struct AccuracyKeyLess
{
    using is_transparent = void;

    static AccuracyKeyView view(const AccuracyKey& k)
    {
        return AccuracyKeyView{k.first, k.second};
    }
    static bool less(const AccuracyKeyView& a, const AccuracyKeyView& b)
    {
        if (a.player != b.player) {
            return a.player < b.player;
        }
        return a.weapon < b.weapon;
    }
    bool operator()(const AccuracyKey& a, const AccuracyKey& b) const { return less(view(a), view(b)); }
    bool operator()(const AccuracyKey& a, const AccuracyKeyView& b) const { return less(view(a), b); }
    bool operator()(const AccuracyKeyView& a, const AccuracyKey& b) const { return less(a, view(b)); }
};

std::map<DamageKey, DamageAccum, DamageKeyLess> g_damage_accum;
std::map<AccuracyKey, AccuracyAccum, AccuracyKeyLess> g_accuracy_accum;

// Finds the bucket for a lookup view, creating it (and only then paying for the owning
// key) if it does not exist yet.
DamageAccum& damage_bucket(const DamageKeyView& k)
{
    const auto it = g_damage_accum.find(k);
    if (it != g_damage_accum.end()) {
        return it->second;
    }
    return g_damage_accum
        .emplace(DamageKey{std::string{k.attacker}, std::string{k.victim}, k.weapon, k.damage_type},
                 DamageAccum{})
        .first->second;
}

AccuracyAccum& accuracy_bucket(const AccuracyKeyView& k)
{
    const auto it = g_accuracy_accum.find(k);
    if (it != g_accuracy_accum.end()) {
        return it->second;
    }
    return g_accuracy_accum.emplace(AccuracyKey{std::string{k.player}, k.weapon}, AccuracyAccum{})
        .first->second;
}

// A single in-progress overflow episode. Instead of pushing one EvGap per evicted
// event, evictions fold into this and materialize as at most one gap event per batch
// build. Cleared at materialize time so a later, disjoint loss becomes its own gap
// rather than merging into one span.
struct PendingGap
{
    bool active = false;
    uint64_t dropped = 0;
    uint64_t first_seq = 0;
    uint64_t last_seq = 0;
};
PendingGap g_pending_gap;

bool g_send_in_flight = false;
std::chrono::steady_clock::time_point g_next_pulse{};
// When the last event POST was handed to the worker. Floors how soon the ack-driven
// fast-drain may re-arm the pulse (see k_min_post_interval).
std::chrono::steady_clock::time_point g_last_post_at{};
std::chrono::steady_clock::duration g_pulse_interval = k_pulse_interval;
bool g_paused_401 = false;
std::string g_paused_gssk;
std::string g_last_response = "none yet";
std::chrono::steady_clock::time_point g_last_permanent_status_warn{};
bool g_trace = false;

uint64_t g_events_emitted = 0;
uint64_t g_events_dropped = 0;
uint64_t g_batches_acked = 0;

std::chrono::steady_clock::time_point g_process_start = std::chrono::steady_clock::now();

// Worker thread plumbing. One persistent thread with a single job slot; outcomes
// come back through fflink::enqueue_main_thread_task.
struct SendJob
{
    std::string body;
    uint32_t batch = 0;
    uint32_t generation = 0;
};

struct SendResult
{
    uint32_t batch = 0;
    uint32_t generation = 0;
    int status = 0;
    int retry_after_s = 0;
    bool network_error = false;
    // A 200 only counts as an ack when the body is valid JSON with ok==true; the
    // acknowledged batch number is then in ack_batch. Any other 200
    // (non-JSON, empty, ok!=true, wrong batch) is a transient failure, not a drop.
    bool ack_ok = false;
    uint32_t ack_batch = 0;
    std::string detail;
};

std::mutex g_worker_mutex;
std::condition_variable g_worker_cv;
std::optional<SendJob> g_worker_job;
bool g_worker_stop = false;
bool g_worker_started = false;
// Identifies the one worker that is currently the live one. Bumped every time a worker
// is spawned, so a predecessor still blocked inside a post retires as soon as it loops
// back instead of lingering as a second consumer of the job slot.
uint32_t g_worker_generation = 0;

// -------------------------------------------------------------------------
// Small helpers
// -------------------------------------------------------------------------

uint64_t uptime_ms()
{
    const auto delta = std::chrono::steady_clock::now() - g_process_start;
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(delta).count());
}

std::string random_chars(const char* alphabet, size_t alphabet_size, size_t count)
{
    // std::random_device is the OS CSPRNG here; a few dozen draws per join is nowhere
    // near hot enough to justify seeding a cheaper engine from it. It is opened once
    // rather than per call because its constructor is the part that can fail.
    //
    // libstdc++ THROWS from that constructor when the target has no entropy source, and
    // the shipped Linux/Wine build is MinGW/libstdc++, so a throw here would escape
    // through mint_upssk into the join packet handler. These values are opaque
    // identifiers rather than secrets, so a degraded seed is an acceptable trade against
    // taking the server down; the firewall at the module boundary is the backstop, this
    // removes the throw at the source.
    static std::optional<std::random_device> rd;
    static const bool rd_ready = [] {
        try {
            rd.emplace();
        }
        catch (const std::exception& e) {
            xlog::error("[afstats-ev] no OS entropy source ({}); stats identifiers fall back to a "
                        "time-seeded engine",
                        e.what());
        }
        catch (...) {
            xlog::error("[afstats-ev] no OS entropy source; stats identifiers fall back to a "
                        "time-seeded engine");
        }
        return rd.has_value();
    }();
    static std::mt19937 fallback{[] {
        const auto steady =
            static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        const auto wall = static_cast<uint64_t>(std::time(nullptr));
        // The module is in an ASLR'd DLL, so a static's address adds per-process entropy.
        const auto addr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&rd));
        return static_cast<std::mt19937::result_type>(steady ^ (wall << 17) ^ addr);
    }()};

    std::uniform_int_distribution<size_t> dist(0, alphabet_size - 1);
    std::string out;
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        out.push_back(alphabet[rd_ready ? dist(*rd) : dist(fallback)]);
    }
    return out;
}

std::string mint_session_id()
{
    return random_chars("0123456789abcdef", 16, 16);
}

std::string mint_upssk()
{
    // "u-" plus 30 alphanumerics, structurally disjoint from the PSSK
    // alphabet so any key self-identifies as tracked or untracked.
    return "u-" + random_chars("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz", 62, 30);
}

std::string sanitize_string(std::string_view in, size_t max_len)
{
    // Guarantees valid UTF-8 output: every codepoint that reaches JSON is a whole,
    // well-formed sequence, so json::dump() can never throw on this string (the
    // build enables nlohmann exceptions and a throw here would unwind into the stock
    // game loop). max_len is a byte cap; truncation stops on a sequence boundary so a
    // codepoint is never bisected. C0 controls and DEL are stripped as before.
    std::string out;
    out.reserve(std::min(in.size(), max_len));
    const size_t n = in.size();
    size_t i = 0;
    while (i < n) {
        const unsigned char c0 = static_cast<unsigned char>(in[i]);
        size_t seq_len = 0;
        uint32_t cp = 0;
        if (c0 < 0x80) {
            seq_len = 1;
            cp = c0;
        }
        else if ((c0 & 0xE0) == 0xC0) {
            seq_len = 2;
            cp = c0 & 0x1Fu;
        }
        else if ((c0 & 0xF0) == 0xE0) {
            seq_len = 3;
            cp = c0 & 0x0Fu;
        }
        else if ((c0 & 0xF8) == 0xF0) {
            seq_len = 4;
            cp = c0 & 0x07u;
        }

        bool valid = seq_len != 0 && i + seq_len <= n;
        for (size_t k = 1; valid && k < seq_len; ++k) {
            const unsigned char cc = static_cast<unsigned char>(in[i + k]);
            if ((cc & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if (valid) {
            // Reject overlong encodings, UTF-16 surrogates, and out-of-range values.
            static constexpr uint32_t min_cp[5] = {0, 0x0, 0x80, 0x800, 0x10000};
            if (cp < min_cp[seq_len] || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
                valid = false;
            }
        }

        if (!valid) {
            // Replace exactly one bad byte and resync; never emit a partial sequence.
            if (out.size() + 1 > max_len) {
                break;
            }
            out.push_back('?');
            ++i;
            continue;
        }
        if (seq_len == 1 && (cp < 0x20 || cp == 0x7F)) {
            ++i;
            continue;
        }
        if (out.size() + seq_len > max_len) {
            break;
        }
        out.append(in.substr(i, seq_len));
        i += seq_len;
    }
    return out;
}

// A stats key reduced to a form that identifies it in a log without being usable: the
// last four characters and nothing else. Used wherever a key is incidental to a
// lifecycle log rather than being the subject of the verification-phase handshake
// logging, so an operator pasting a server log never leaks a live PSSK.
std::string key_suffix(std::string_view key)
{
    if (key.empty()) {
        return "(none)";
    }
    if (key.size() <= 4) {
        return "***";
    }
    return "***" + std::string{key.substr(key.size() - 4)};
}

double round_coord(float v)
{
    // Positions are rounded to one decimal on the wire. Going through double keeps
    // the shortest-round-trip JSON representation at one decimal too.
    return std::round(static_cast<double>(v) * 10.0) / 10.0;
}

bool emitting_enabled()
{
    return rf::is_multi && rf::is_server && fflink::afstats_server_enabled();
}

uint8_t platform_id()
{
    static const bool is_wine = get_wine_version().has_value();
    return is_wine ? k_platform_wine : k_platform_windows;
}

std::string client_string(const rf::Player* player)
{
    const auto& v = player->version_info;
    switch (v.software) {
        case ClientSoftware::AlpineFaction:
            return std::format("af {}.{}.{}", v.major, v.minor, v.patch);
        case ClientSoftware::DashFaction:
            return std::format("df {}.{}", v.major, v.minor);
        case ClientSoftware::PureFaction:
            return std::format("pf {}.{}", v.major, v.minor);
        case ClientSoftware::Browser:
            return "browser";
        default:
            return "legacy";
    }
}

uint8_t team_id(const rf::Player* player)
{
    if (!multi_is_team_game_type()) {
        return team_none;
    }
    return player->team == rf::TEAM_BLUE ? team_blue : team_red;
}

// -------------------------------------------------------------------------
// Queue and round counters
// -------------------------------------------------------------------------

// Folds a loss into the current overflow episode. No queue event and at most one
// log line per episode; the gap event is materialized later.
void accumulate_gap(uint64_t dropped, uint64_t first_seq, uint64_t last_seq)
{
    if (!g_pending_gap.active) {
        g_pending_gap.active = true;
        g_pending_gap.dropped = dropped;
        g_pending_gap.first_seq = first_seq;
        g_pending_gap.last_seq = last_seq;
        xlog::warn("[afstats-ev] queue full; dropping events (episode start, seq {}-{})", first_seq,
                   last_seq);
        return;
    }
    g_pending_gap.dropped += dropped;
    g_pending_gap.first_seq = std::min(g_pending_gap.first_seq, first_seq);
    g_pending_gap.last_seq = std::max(g_pending_gap.last_seq, last_seq);
}

void push_event(EventPayload payload)
{
    Event e;
    e.seq = g_next_seq++;
    e.t = uptime_ms();
    e.round = g_round;
    e.payload = std::move(payload);
    g_queue.push_back(std::move(e));
    ++g_events_emitted;

    if (g_queue.size() <= k_max_queued_events) {
        return;
    }
    // Bound memory by dropping the oldest events, leaving the queue holding
    // exactly the documented cap. Evicting to a reserved slot below the cap instead
    // would drop two events per push once full, so the queue would oscillate around
    // cap-1 and never actually hold the advertised 20000. materialize_pending_gap
    // appends its marker directly rather than through here, so it can carry the queue
    // one event past the cap for the moment between the fold and the batch cut -- that
    // is bounded at +1 and drains on the same call.
    const size_t to_drop = g_queue.size() - k_max_queued_events;
    const uint64_t first = g_queue.front().seq;
    uint64_t last = first;
    for (size_t i = 0; i < to_drop; ++i) {
        last = g_queue.front().seq;
        g_queue.pop_front();
    }
    g_events_dropped += to_drop;
    accumulate_gap(to_drop, first, last);
}

// Turns the current overflow episode into a single gap event and clears it, so a
// later disjoint loss starts a fresh episode. Minted at the tail with a fresh seq:
// FF reads the loss from the range fields, not the event's position.
void materialize_pending_gap()
{
    if (!g_pending_gap.active) {
        return;
    }
    EvGap gap;
    gap.dropped = g_pending_gap.dropped;
    gap.first_seq = g_pending_gap.first_seq;
    gap.last_seq = g_pending_gap.last_seq;
    xlog::warn("[afstats-ev] gap materialized: {} events lost, seq {}-{}", gap.dropped, gap.first_seq,
               gap.last_seq);
    g_pending_gap = PendingGap{};

    Event e;
    e.seq = g_next_seq++;
    e.t = uptime_ms();
    e.round = g_round;
    e.payload = std::move(gap);
    g_queue.push_back(std::move(e));
}

// Materializes the windowed aggregates into events. Runs immediately before a
// batch is cut and before a round closes, so an aggregate never lands under the
// round number of the round after the one it happened in.
void flush_accumulators()
{
    // extract() yields a node whose key() is mutable, so the captured-at-damage-time
    // key strings move out instead of being copied, with no allocation per
    // entry and no writing through a const map key.
    while (!g_damage_accum.empty()) {
        auto node = g_damage_accum.extract(g_damage_accum.begin());
        auto& key = node.key();
        EvDamage ev;
        ev.attacker = std::move(std::get<0>(key));
        ev.victim = std::move(std::get<1>(key));
        ev.weapon = std::get<2>(key);
        ev.damage_type = std::get<3>(key);
        ev.amount = node.mapped().amount;
        ev.hits = node.mapped().hits;
        push_event(std::move(ev));
    }

    while (!g_accuracy_accum.empty()) {
        auto node = g_accuracy_accum.extract(g_accuracy_accum.begin());
        EvAccuracy ev;
        ev.player = std::move(node.key().first);
        ev.weapon = node.key().second;
        ev.fired = node.mapped().fired;
        ev.hit = node.mapped().hit;
        ev.hit_head = node.mapped().hit_head;
        push_event(std::move(ev));
    }
}

void reset_round_counters(rf::Player* player)
{
    const int64_t now = static_cast<int64_t>(uptime_ms());
    player->afstats_round = AfstatsRoundCounters{};
    player->afstats_round.present_since_ms = now;
    player->afstats_round.last_sample_ms = now;
    player->afstats_round.last_ping_sample_ms = now;
}

// Advances the time accumulators by sampling the player's current state, and
// edge-detects idle. Called on the sender pulse and again whenever a summary is
// about to be serialized, so the coarse totals need no per-transition hook. Idle
// is sampled rather than hooked because the engine stores no idle flag: it is a
// predicate that flips when an inactivity timer expires, with nothing to hook.
void sample_player_state(rf::Player* player)
{
    if (player->is_browser) {
        return;
    }
    const int64_t now = static_cast<int64_t>(uptime_ms());
    auto& c = player->afstats_round;
    const bool idle = player_is_idle(player);
    const int64_t delta = now - c.last_sample_ms;
    if (delta > 0) {
        c.last_sample_ms = now;
        // time_played_ms is total connected time; time_spectating_ms is a subset of
        // it, not a separate bucket; this is definitional, FF nets them if it wants.
        c.time_played_ms += delta;
        if (player->is_spectator) {
            c.time_spectating_ms += delta;
        }
        if (idle) {
            c.time_idle_ms += delta;
        }
    }
    if (idle != c.was_idle) {
        c.was_idle = idle;
        on_status(player, idle ? StatusKind::idle_start : StatusKind::idle_stop);
    }
    if (now - c.last_ping_sample_ms >= k_ping_sample_interval_ms && player->net_data) {
        c.ping_sum += player->net_data->ping;
        ++c.ping_samples;
        c.last_ping_sample_ms = now;
    }
}

RosterEntry build_roster_entry(rf::Player* player, bool with_summary)
{
    RosterEntry e;
    e.player = player->afstats_key;
    e.name = sanitize_string(player->name.c_str(), k_max_name_len);
    e.team = team_id(player);
    if (player->is_bot) {
        e.flags |= k_player_flag_bot;
    }
    if (player->is_spectator) {
        e.flags |= k_player_flag_spectating;
    }
    if (player_is_idle(player)) {
        e.flags |= k_player_flag_idle;
    }
    e.handicap = player->damage_handicap;
    if (!with_summary) {
        return e;
    }

    sample_player_state(player);
    const auto& c = player->afstats_round;
    e.has_summary = true;
    // Score and caps are the engine's own per-level numbers; duplicating them into
    // the round counters would just give them a second chance to disagree.
    if (player->stats) {
        e.score = player->stats->score;
        e.caps = static_cast<uint32_t>(std::max<int>(player->stats->caps, 0));
    }
    e.kills = c.kills;
    e.deaths = c.deaths;
    e.assists = c.assists;
    e.shots_fired = c.shots_fired;
    e.shots_hit = c.shots_hit;
    e.damage_dealt = c.damage_dealt;
    e.damage_taken = c.damage_taken;
    e.highest_streak = c.highest_streak;
    e.time_played_ms = c.time_played_ms;
    e.time_spectating_ms = c.time_spectating_ms;
    e.time_idle_ms = c.time_idle_ms;
    // 0 until the first ~10s ping sample lands, so short-lived connections report 0
    // rather than a spuriously precise value; this is definitional, not a defect.
    e.avg_ping = c.ping_samples > 0 ? static_cast<int>(c.ping_sum / c.ping_samples) : 0;
    return e;
}

std::vector<RosterEntry> build_roster(bool with_summary)
{
    std::vector<RosterEntry> roster;
    for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
        if (player.is_browser || player.afstats_key.empty()) {
            continue;
        }
        roster.push_back(build_roster_entry(&player, with_summary));
    }
    return roster;
}

// -------------------------------------------------------------------------
// JSON serialization (batch time only)
// -------------------------------------------------------------------------

nlohmann::json setting_to_json(const SettingValue& value)
{
    return std::visit(
        [](const auto& v) -> nlohmann::json {
            return nlohmann::json(v);
        },
        value);
}

nlohmann::json pos_to_json(const Vec3& p)
{
    return nlohmann::json::array({round_coord(p.x), round_coord(p.y), round_coord(p.z)});
}

nlohmann::json roster_entry_to_json(const RosterEntry& e)
{
    nlohmann::json j{
        {"player", e.player},
        {"name", e.name},
        {"team", e.team},
        {"flags", e.flags},
        {"handicap", e.handicap},
    };
    if (!e.has_summary) {
        return j;
    }
    j["score"] = e.score;
    j["kills"] = e.kills;
    j["deaths"] = e.deaths;
    j["assists"] = e.assists;
    j["caps"] = e.caps;
    j["shots_fired"] = e.shots_fired;
    j["shots_hit"] = e.shots_hit;
    j["damage_dealt"] = e.damage_dealt;
    j["damage_taken"] = e.damage_taken;
    j["highest_streak"] = e.highest_streak;
    j["time_played_ms"] = e.time_played_ms;
    j["time_spectating_ms"] = e.time_spectating_ms;
    j["time_idle_ms"] = e.time_idle_ms;
    j["avg_ping"] = e.avg_ping;
    return j;
}

nlohmann::json event_to_json(const Event& e)
{
    nlohmann::json j{
        {"seq", e.seq},
        {"t", e.t},
        {"round", e.round},
    };

    std::visit(
        [&j](const auto& p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, EvServerHello>) {
                j["type"] = "server_hello";
                j["server_name"] = p.server_name;
                j["port"] = p.port;
                j["af_version"] = p.af_version;
                j["platform"] = p.platform;
                j["max_players"] = p.max_players;
            }
            else if constexpr (std::is_same_v<T, EvGap>) {
                j["type"] = "gap";
                j["dropped"] = p.dropped;
                j["first_seq"] = p.first_seq;
                j["last_seq"] = p.last_seq;
            }
            else if constexpr (std::is_same_v<T, EvPlayerJoin>) {
                j["type"] = "player_join";
                j["upssk"] = p.upssk;
                j["name"] = p.name;
                j["kind"] = p.kind;
                j["client"] = p.client;
            }
            else if constexpr (std::is_same_v<T, EvPlayerRemediate>) {
                j["type"] = "player_remediate";
                j["upssk"] = p.upssk;
                j["pssk"] = p.pssk;
            }
            else if constexpr (std::is_same_v<T, EvPlayerRename>) {
                j["type"] = "player_rename";
                j["player"] = p.player;
                j["name"] = p.name;
            }
            else if constexpr (std::is_same_v<T, EvPlayerLeave>) {
                j["type"] = "player_leave";
                j["player"] = p.stats.player;
                j["reason"] = p.reason;
                j["stats"] = roster_entry_to_json(p.stats);
            }
            else if constexpr (std::is_same_v<T, EvRoundStart>) {
                j["type"] = "round_start";
                j["tc_mod"] = p.tc_mod;
                j["level_file"] = p.level_file;
                j["level_name"] = p.level_name;
                j["gametype"] = p.gametype;
                j["time_limit_s"] = p.time_limit_s;
                j["win_condition"] = p.win_condition;
                j["overtime_enabled"] = p.overtime_enabled;
                j["af_flags"] = p.af_flags;
                j["rf_flags"] = p.rf_flags;
                j["gi_flags"] = p.gi_flags;
                j["match_state"] = p.match_state;

                auto mutators = nlohmann::json::array();
                for (const auto& m : p.mutators) {
                    auto settings = nlohmann::json::object();
                    for (const auto& [key, value] : m.settings) {
                        settings[key] = setting_to_json(value);
                    }
                    nlohmann::json entry;
                    entry["name"] = m.name;
                    entry["settings"] = std::move(settings);
                    mutators.push_back(std::move(entry));
                }
                j["mutators"] = std::move(mutators);

                auto gametype_settings = nlohmann::json::object();
                for (const auto& [key, value] : p.gametype_settings) {
                    gametype_settings[key] = setting_to_json(value);
                }
                j["gametype_settings"] = std::move(gametype_settings);

                auto roster = nlohmann::json::array();
                for (const auto& entry : p.roster) {
                    roster.push_back(roster_entry_to_json(entry));
                }
                j["roster"] = std::move(roster);
            }
            else if constexpr (std::is_same_v<T, EvRoundEnd>) {
                j["type"] = "round_end";
                j["end_type"] = p.end_type;
                j["duration_ms"] = p.duration_ms;
                j["overtime"] = p.overtime;
                j["red_score"] = p.red_score;
                j["blue_score"] = p.blue_score;
                auto players = nlohmann::json::array();
                for (const auto& entry : p.players) {
                    players.push_back(roster_entry_to_json(entry));
                }
                j["players"] = std::move(players);
            }
            else if constexpr (std::is_same_v<T, EvKill>) {
                j["type"] = "kill";
                j["victim"] = p.victim;
                j["killer"] = p.killer.empty() ? nlohmann::json(nullptr) : nlohmann::json(p.killer);
                j["weapon"] = p.weapon < 0 ? nlohmann::json(nullptr) : nlohmann::json(p.weapon);
                j["damage_type"] = p.damage_type;
                j["flags"] = p.flags;
                j["assists"] = p.assists;
                j["victim_pos"] = pos_to_json(p.victim_pos);
                j["killer_pos"] = p.has_killer_pos ? pos_to_json(p.killer_pos) : nlohmann::json(nullptr);
            }
            else if constexpr (std::is_same_v<T, EvSpawn>) {
                j["type"] = "spawn";
                j["player"] = p.player;
                j["pos"] = pos_to_json(p.pos);
            }
            else if constexpr (std::is_same_v<T, EvDamage>) {
                j["type"] = "damage";
                j["attacker"] = p.attacker.empty() ? nlohmann::json(nullptr) : nlohmann::json(p.attacker);
                j["victim"] = p.victim;
                j["weapon"] = p.weapon < 0 ? nlohmann::json(nullptr) : nlohmann::json(p.weapon);
                j["damage_type"] = p.damage_type;
                j["amount"] = p.amount;
                j["hits"] = p.hits;
            }
            else if constexpr (std::is_same_v<T, EvAccuracy>) {
                j["type"] = "accuracy";
                j["player"] = p.player;
                j["weapon"] = p.weapon;
                j["fired"] = p.fired;
                j["hit"] = p.hit;
                j["hit_head"] = p.hit_head;
            }
            else if constexpr (std::is_same_v<T, EvStatus>) {
                j["type"] = "status";
                j["player"] = p.player;
                j["kind"] = p.kind;
                if (p.has_value) {
                    j["value"] = p.value;
                }
            }
            else if constexpr (std::is_same_v<T, EvItemPickup>) {
                j["type"] = "item_pickup";
                j["player"] = p.player;
                j["item"] = p.item;
                j["pos"] = pos_to_json(p.pos);
                j["respawn_ms"] = p.respawn_ms;
            }
            else if constexpr (std::is_same_v<T, EvFlagEvent>) {
                j["type"] = "flag_event";
                j["kind"] = p.kind;
                j["team"] = p.team;
                j["player"] = p.player.empty() ? nlohmann::json(nullptr) : nlohmann::json(p.player);
                j["pos"] = pos_to_json(p.pos);
            }
            else if constexpr (std::is_same_v<T, EvPointEvent>) {
                j["type"] = "point_event";
                j["hill"] = p.hill;
                j["kind"] = p.kind;
                j["owner"] = p.owner;
                j["players"] = p.players;
                j["locked"] = p.locked;
            }
            else if constexpr (std::is_same_v<T, EvBagmanEvent>) {
                j["type"] = "bagman_event";
                j["kind"] = p.kind;
                j["player"] = p.player.empty() ? nlohmann::json(nullptr) : nlohmann::json(p.player);
                if (p.has_from) {
                    j["from"] = p.from;
                }
                j["pos"] = pos_to_json(p.pos);
            }
            else if constexpr (std::is_same_v<T, EvGgLevelup>) {
                j["type"] = "gg_levelup";
                j["player"] = p.player;
                j["level"] = p.level;
                j["weapon"] = p.weapon < 0 ? nlohmann::json(nullptr) : nlohmann::json(p.weapon);
            }
            else if constexpr (std::is_same_v<T, EvMatchStart>) {
                j["type"] = "match_start";
                j["team_size"] = p.team_size;
                j["participants"] = p.participants;
            }
            else if constexpr (std::is_same_v<T, EvMatchEnd>) {
                j["type"] = "match_end";
                j["result"] = p.result;
                j["winner_team"] = p.winner_team;
                j["winner_player"] =
                    p.winner_player.empty() ? nlohmann::json(nullptr) : nlohmann::json(p.winner_player);
            }
            else if constexpr (std::is_same_v<T, EvVoteCalled>) {
                j["type"] = "vote_called";
                j["vote_type"] = p.vote_type;
                j["initiator"] = p.initiator;
                j["target"] = p.target.empty() ? nlohmann::json(nullptr) : nlohmann::json(p.target);
                j["detail"] = p.detail;
            }
            else if constexpr (std::is_same_v<T, EvVoteEnded>) {
                j["type"] = "vote_ended";
                j["vote_type"] = p.vote_type;
                j["result"] = p.result;
                j["yes"] = p.yes;
                j["no"] = p.no;
                j["eligible"] = p.eligible;
            }
            else if constexpr (std::is_same_v<T, EvGeomod>) {
                j["type"] = "geomod";
                j["pos"] = pos_to_json(p.pos);
                j["scale"] = p.scale;
                j["rf2_style"] = p.rf2_style;
                j["player"] = p.player.empty() ? nlohmann::json(nullptr) : nlohmann::json(p.player);
                j["weapon"] = p.weapon < 0 ? nlohmann::json(nullptr) : nlohmann::json(p.weapon);
            }
            else if constexpr (std::is_same_v<T, EvClutterDestroyed>) {
                j["type"] = "clutter_destroyed";
                j["clutter_type"] =
                    p.has_clutter_type ? nlohmann::json(p.clutter_type) : nlohmann::json(nullptr);
                j["uid"] = p.has_uid ? nlohmann::json(p.uid) : nlohmann::json(nullptr);
                j["player"] = p.player.empty() ? nlohmann::json(nullptr) : nlohmann::json(p.player);
                j["weapon"] = p.weapon < 0 ? nlohmann::json(nullptr) : nlohmann::json(p.weapon);
                j["damage_type"] = p.damage_type;
                j["pos"] = pos_to_json(p.pos);
            }
            else if constexpr (std::is_same_v<T, EvDetailBrushDestroyed>) {
                j["type"] = "detail_brush_destroyed";
                j["material"] = p.material;
                j["room_uid"] = p.has_room_uid ? nlohmann::json(p.room_uid) : nlohmann::json(nullptr);
                j["player"] = p.player.empty() ? nlohmann::json(nullptr) : nlohmann::json(p.player);
                j["weapon"] = p.weapon < 0 ? nlohmann::json(nullptr) : nlohmann::json(p.weapon);
                j["damage_type"] = p.damage_type;
                j["pos"] = pos_to_json(p.pos);
            }
            else {
                // Every variant alternative must have a branch above; a new event without
                // one would otherwise ship a typeless event silently. Fail the build.
                static_assert(always_false_v<T>, "unhandled event payload in event_to_json");
            }
        },
        e.payload);

    return j;
}

nlohmann::json build_envelope_json(const PendingBatch& batch, const std::string& gssk)
{
    nlohmann::json j{
        {"v", k_envelope_version},
        {"gssk", gssk},
        {"session", g_session_id},
        {"batch", batch.batch},
        {"sent_at", static_cast<uint64_t>(std::time(nullptr))},
        {"uptime_ms", uptime_ms()},
    };
    auto events = nlohmann::json::array();
    for (const Event& e : batch.events) {
        events.push_back(event_to_json(e));
    }
    j["events"] = std::move(events);
    return j;
}

std::string build_envelope(const PendingBatch& batch, const std::string& gssk)
{
    // error_handler_t::replace is defense in depth: sanitize_string already
    // guarantees valid UTF-8, but a stray bad byte must degrade to U+FFFD rather
    // than throw out of the serializer.
    return build_envelope_json(batch, gssk)
        .dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

// Masks the GSSK and every stats-key field so the trace log can carry a batch's
// shape without leaking credentials. Names and non-key fields are kept.
void redact_credentials(nlohmann::json& node)
{
    // Fields whose value is a single key string.
    static constexpr std::string_view key_fields[] = {
        "gssk",   "player",   "victim", "killer",       "attacker",
        "initiator", "target", "upssk", "pssk",         "winner_player"};
    // Fields whose value is an array of key strings (round_end.players is an array of
    // roster objects instead, so it is handled by the recursion below, not here).
    static constexpr std::string_view key_list_fields[] = {"assists", "participants"};

    if (node.is_object()) {
        for (auto it = node.begin(); it != node.end(); ++it) {
            const std::string& k = it.key();
            nlohmann::json& v = it.value();
            if (std::find(std::begin(key_fields), std::end(key_fields), k) != std::end(key_fields)
                && v.is_string()) {
                v = "***";
            }
            else if (k == "players" && v.is_array()) {
                // point_event.players is a key list; round_end.players is roster objects.
                for (auto& e : v) {
                    if (e.is_string()) {
                        e = "***";
                    }
                    else {
                        redact_credentials(e);
                    }
                }
            }
            else if (std::find(std::begin(key_list_fields), std::end(key_list_fields), k)
                         != std::end(key_list_fields)
                     && v.is_array()) {
                for (auto& e : v) {
                    if (e.is_string()) {
                        e = "***";
                    }
                }
            }
            else {
                redact_credentials(v);
            }
        }
    }
    else if (node.is_array()) {
        for (auto& e : node) {
            redact_credentials(e);
        }
    }
}

// -------------------------------------------------------------------------
// Sender
// -------------------------------------------------------------------------

SendResult do_one_post(const SendJob& job, unsigned long connect_timeout_ms, unsigned long receive_timeout_ms)
{
    SendResult out;
    out.batch = job.batch;
    out.generation = job.generation;

    std::string response;
    try {
        HttpSession session(k_user_agent);
        session.set_connect_timeout(connect_timeout_ms);
        session.set_receive_timeout(receive_timeout_ms);

        HttpRequest req(k_events_url, "POST", session);
        req.set_content_type("application/json");
        out.status = req.send_no_check(job.body);

        try {
            if (auto retry_after = req.get_header("Retry-After")) {
                // Clamp so a hostile/garbled header can't wedge the batch behind an
                // absurd backoff (spec allows honoring Retry-After; cap at 1 hour).
                out.retry_after_s = std::clamp(std::atoi(retry_after->c_str()), 0, 3600);
            }
        }
        catch (const std::exception&) {
            // get_header uses a fixed 1024-byte buffer and throws on a longer header;
            // an oversized Retry-After must not abort the whole response read.
        }

        char buf[1024];
        std::ostringstream stream;
        size_t total = 0;
        while (size_t n = req.read(buf, sizeof(buf))) {
            total += n;
            if (total > k_max_response_bytes) {
                // A compromised/broken endpoint must not stream unbounded data into
                // worker memory; treat an over-cap body as a transient failure.
                out.network_error = true;
                out.detail = std::format("response exceeded {} byte cap", k_max_response_bytes);
                return out;
            }
            stream.write(buf, n);
        }
        response = stream.str();
    }
    catch (const std::exception& e) {
        out.network_error = true;
        out.detail = std::string{"network error: "} + e.what();
        return out;
    }

    // One parse of the body: a 200 must carry {ok:true, batch:N} to be an ack, and a
    // non-200 may carry an {error:...} code for diagnostics. A non-JSON
    // or empty body leaves ack_ok false, so an any-200-is-ack drop can't happen.
    std::string error_code;
    try {
        auto j = nlohmann::json::parse(response);
        if (out.status == 200) {
            if (j.contains("ok") && j["ok"].is_boolean() && j["ok"].get<bool>()
                && j.contains("batch") && j["batch"].is_number_integer()) {
                const auto acked = j["batch"].get<int64_t>();
                if (acked >= 0) {
                    out.ack_ok = true;
                    out.ack_batch = static_cast<uint32_t>(acked);
                }
            }
        }
        else if (j.contains("error") && j["error"].is_string()) {
            error_code = fflink::sanitize_for_log(j["error"].get<std::string>());
        }
    }
    catch (const std::exception&) {
        // Body wasn't JSON; the status code alone drives the state machine.
    }

    out.detail = error_code.empty() ? std::format("HTTP {}", out.status)
                                    : std::format("HTTP {} ({})", out.status, error_code);
    return out;
}

void on_send_result(SendResult result);

void worker_main(uint32_t my_generation)
{
    for (;;) {
        SendJob job;
        {
            std::unique_lock lock(g_worker_mutex);
            g_worker_cv.wait(lock, [my_generation] {
                return g_worker_stop || g_worker_generation != my_generation
                    || g_worker_job.has_value();
            });
            // Retire on either signal. Checking the generation as well as the stop flag
            // is what makes shutdown sound: a worker blocked in a post outlives the
            // shutdown window, and by the time it gets here the next session may already
            // have cleared g_worker_stop -- but it can never be the current generation
            // again, so exactly one worker survives.
            if (g_worker_stop || g_worker_generation != my_generation) {
                return;
            }
            job = std::move(*g_worker_job);
            g_worker_job.reset();
        }

        SendResult result;
        try {
            result = do_one_post(job, k_connect_timeout_ms, k_receive_timeout_ms);
        }
        catch (const std::exception& e) {
            result.batch = job.batch;
            result.generation = job.generation;
            result.network_error = true;
            result.detail = std::string{"sender thread error: "} + e.what();
        }
        catch (...) {
            result.batch = job.batch;
            result.generation = job.generation;
            result.network_error = true;
            result.detail = "sender thread error: unknown exception";
        }

        fflink::enqueue_main_thread_task([result = std::move(result)]() mutable {
            on_send_result(std::move(result));
        });
    }
}

bool ensure_worker_started()
{
    if (g_worker_started) {
        return true;
    }
    uint32_t my_generation = 0;
    {
        std::lock_guard lock(g_worker_mutex);
        // Claiming a new generation supersedes any predecessor still finishing a post
        // from the previous session; clearing the stop flag here (rather than at the end
        // of shutdown) means that predecessor had the whole gap to observe it.
        my_generation = ++g_worker_generation;
        g_worker_stop = false;
        g_worker_job.reset();
    }
    g_worker_cv.notify_all(); // retire a predecessor parked on the CV
    try {
        std::thread(worker_main, my_generation).detach();
        g_worker_started = true;
    }
    catch (const std::exception& e) {
        xlog::warn("[afstats-ev] failed to start the sender thread: {}", e.what());
    }
    return g_worker_started;
}

void dispatch(PendingBatch& batch, const std::string& gssk)
{
    if (!ensure_worker_started()) {
        return;
    }

    // body carries the real GSSK for transmission; the trace log below uses a
    // separately redacted copy, so the key never reaches the log.
    std::string body = build_envelope(batch, gssk);
    ++batch.attempts;

    if constexpr (AFSTATS_VERIFICATION_LOGGING) {
        if (batch.attempts == 1) {
            xlog::warn("[afstats-ev] batch {} built: {} events (seq {}-{}), {} bytes", batch.batch,
                       batch.events.size(), batch.first_seq, batch.last_seq, body.size());
        }
        else {
            // What is waiting behind this batch and will go out in the next one. Each
            // accumulator entry materializes into exactly one event at the next flush,
            // so this is a count rather than an estimate.
            const size_t queued_behind =
                g_queue.size() + g_damage_accum.size() + g_accuracy_accum.size();

            // Backlog growth over the retry window, from the monotonic event clock. The
            // denominator has to run to now rather than to the newest queued event, or a
            // queue that stopped growing would keep reporting the rate it grew at.
            const uint64_t now_ms = uptime_ms();
            const uint64_t since_freeze_ms =
                now_ms > batch.frozen_at_ms ? now_ms - batch.frozen_at_ms : 0;
            std::string rate;
            if (queued_behind >= 2 && since_freeze_ms >= 1000) {
                rate = std::format(" (~{:.1f}/s)",
                                   static_cast<double>(queued_behind) * 1000.0
                                       / static_cast<double>(since_freeze_ms));
            }

            xlog::warn("[afstats-ev] retrying batch {} ({} events, {} bytes, attempt {}); "
                       "{} events queued awaiting ack{}",
                       batch.batch, batch.events.size(), body.size(), batch.attempts, queued_behind,
                       rate);
        }
    }
    if (g_trace) {
        nlohmann::json redacted = build_envelope_json(batch, gssk);
        redact_credentials(redacted);
        xlog::warn("[afstats-ev] trace batch {}: {}", batch.batch,
                   redacted.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
    }

    {
        std::lock_guard lock(g_worker_mutex);
        g_worker_job = SendJob{std::move(body), batch.batch, g_session_generation};
    }
    g_worker_cv.notify_one();
    g_send_in_flight = true;
    g_last_post_at = std::chrono::steady_clock::now();
}

void build_batch_from_queue()
{
    flush_accumulators();
    // Fold any overflow episode into a single gap event before the batch is cut, so
    // it goes out with (not after) the events that survived the loss.
    materialize_pending_gap();
    if (g_queue.empty()) {
        return;
    }
    PendingBatch batch;
    batch.batch = g_next_batch++;
    const size_t count = std::min(g_queue.size(), k_max_events_per_batch);
    batch.events.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        batch.events.push_back(std::move(g_queue.front()));
        g_queue.pop_front();
    }
    batch.first_seq = batch.events.front().seq;
    batch.last_seq = batch.events.back().seq;
    batch.frozen_at_ms = uptime_ms();
    g_pending.push_back(std::move(batch));
}

void pump()
{
    if (g_pending.empty()) {
        build_batch_from_queue();
    }
    if (g_pending.empty()) {
        return;
    }
    const std::string gssk = fflink::get_gssk();
    if (gssk.empty()) {
        // No session key yet: the queue keeps filling and nothing goes out.
        return;
    }
    dispatch(g_pending.front(), gssk);
}

void drop_front_batch_with_gap(const char* why)
{
    PendingBatch batch = std::move(g_pending.front());
    g_pending.pop_front();
    xlog::warn("[afstats-ev] dropping batch {} without retry ({}); {} events lost", batch.batch, why,
               batch.events.size());
    accumulate_gap(batch.events.size(), batch.first_seq, batch.last_seq);
}

void split_front_batch()
{
    PendingBatch& front = g_pending.front();
    if (front.events.size() < 2) {
        drop_front_batch_with_gap("413 on a single-event batch, nothing left to split");
        return;
    }

    const size_t half = front.events.size() / 2;
    PendingBatch tail;
    tail.batch = g_next_batch++;
    tail.frozen_at_ms = front.frozen_at_ms; // both halves were frozen together
    tail.events.assign(std::make_move_iterator(front.events.begin() + half),
                       std::make_move_iterator(front.events.end()));
    front.events.erase(front.events.begin() + half, front.events.end());

    front.last_seq = front.events.back().seq;
    front.attempts = 0;
    tail.first_seq = tail.events.front().seq;
    tail.last_seq = tail.events.back().seq;

    xlog::warn("[afstats-ev] batch {} was too large; split into batch {} ({} events) and batch {} ({} events)",
               front.batch, front.batch, front.events.size(), tail.batch, tail.events.size());
    g_pending.insert(g_pending.begin() + 1, std::move(tail));
}

void on_send_result(SendResult result)
{
    // A completion from a previous session's worker: the module was
    // reset out from under it, so it must not touch the new session's in-flight state.
    if (!g_session_started || result.generation != g_session_generation) {
        return;
    }

    g_send_in_flight = false;

    if (g_pending.empty() || g_pending.front().batch != result.batch) {
        xlog::warn("[afstats-ev] discarding a result for batch {} that is no longer in flight", result.batch);
        return;
    }

    g_last_response = result.detail;

    if (result.network_error) {
        xlog::warn("[afstats-ev] batch {} failed: {}; retrying on the next pulse", result.batch,
                   result.detail);
        return;
    }

    if (result.status == 200) {
        // Only a well-formed ack for THIS batch discards it. A 200 with a
        // non-JSON/empty body, ok!=true, or a mismatched batch number is transient:
        // retry the identical batch, never silently drop it.
        if (!result.ack_ok || result.ack_batch != g_pending.front().batch) {
            xlog::warn("[afstats-ev] batch {} got a 200 that was not a valid ack ({}); retrying "
                       "on the next pulse",
                       result.batch, result.detail);
            return;
        }
        if constexpr (AFSTATS_VERIFICATION_LOGGING) {
            const PendingBatch& front = g_pending.front();
            xlog::warn("[afstats-ev] batch {} acked ({} events)", front.batch, front.events.size());
        }
        g_pending.pop_front();
        ++g_batches_acked;
        g_pulse_interval = k_pulse_interval;
        // Drain a backlog faster than one batch per pulse, but never post more often than
        // the floor: floor the re-arm to k_min_post_interval after the last POST so a
        // client flood can't reflect into a POST/handshake storm at RTT frequency.
        if (!g_pending.empty() || g_queue.size() >= k_max_events_per_batch) {
            g_next_pulse = std::max(std::chrono::steady_clock::now(), g_last_post_at + k_min_post_interval);
        }
        return;
    }

    switch (result.status) {
        case 400:
            // Poison pill: a batch FactionFiles can never accept must not wedge the
            // stream forever.
            drop_front_batch_with_gap(result.detail.c_str());
            break;
        case 401:
            g_paused_401 = true;
            g_paused_gssk = fflink::get_gssk();
            xlog::warn("[afstats-ev] batch {} rejected ({}); pausing the stream and re-running the "
                       "session key exchange",
                       result.batch, result.detail);
            fflink::start_session_exchange();
            break;
        case 413:
            split_front_batch();
            break;
        case 429: {
            g_pulse_interval = result.retry_after_s > 0 ? std::chrono::seconds(result.retry_after_s)
                                                        : k_rate_limit_backoff;
            const auto backoff_s =
                std::chrono::duration_cast<std::chrono::seconds>(g_pulse_interval).count();
            xlog::warn("[afstats-ev] rate limited ({}); backing off to a {}s pulse", result.detail,
                       backoff_s);
            g_next_pulse = std::chrono::steady_clock::now() + g_pulse_interval;
            break;
        }
        default:
            xlog::warn("[afstats-ev] batch {} rejected ({}); retrying on the next pulse", result.batch,
                       result.detail);
            // Spec keeps retrying unknown codes, but 404/405/501 read as a misrouted
            // or disabled endpoint rather than a blip; surface that, throttled, so it
            // is diagnosable without spamming the log every pulse.
            if (result.status == 404 || result.status == 405 || result.status == 501) {
                const auto now = std::chrono::steady_clock::now();
                if (now - g_last_permanent_status_warn >= std::chrono::seconds(60)) {
                    g_last_permanent_status_warn = now;
                    xlog::warn("[afstats-ev] endpoint returned {} repeatedly; the events URL may be "
                               "misrouted or disabled",
                               result.status);
                }
            }
            break;
    }
}

// -------------------------------------------------------------------------
// Session
// -------------------------------------------------------------------------

void emit_server_hello()
{
    EvServerHello hello;
    hello.server_name = sanitize_string(rf::netgame.name.c_str(), k_max_name_len * 2);
    hello.port = rf::net_port;
    hello.af_version = VERSION_STR;
    hello.platform = platform_id();
    hello.max_players =
        static_cast<uint8_t>(std::clamp(rf::netgame.max_players, 0, 32));
    push_event(std::move(hello));
}

// Returns the module to a clean slate so a second session in the same process (a
// listen-server re-host, or a resume after a permanent 401) starts fresh. Does not
// touch the worker thread; the worker is managed separately by on_shutdown.
void reset_module_state()
{
    g_queue.clear();
    g_pending.clear();
    g_damage_accum.clear();
    g_accuracy_accum.clear();
    g_pending_gap = PendingGap{};
    g_next_seq = 1;
    g_next_batch = 1;
    g_round = 1;
    g_round_open = false;
    g_any_round_started = false;
    g_match_open = false;
    g_pending_end_type.reset();
    g_send_in_flight = false;
    g_paused_401 = false;
    g_paused_gssk.clear();
    g_pulse_interval = k_pulse_interval;
    g_session_started = false;
    g_session_id.clear();
    g_reannouncing = false;

    // Diagnostics describe one session's stream, so they go with it: otherwise the
    // status console reports the previous session's totals against a fresh session id.
    g_last_response = "none yet";
    g_events_emitted = 0;
    g_events_dropped = 0;
    g_batches_acked = 0;

    // Per-player identity belongs to the session that minted it. Leaving keys behind
    // would strand everyone still connected: on_player_join refuses to mint over an
    // existing key, so a recovered session would carry rosters, kills and damage keyed
    // to identities it never announced. Clearing them here makes the next session
    // re-mint and re-announce (see reannounce_connected_players).
    //
    // afstats_pssk is deliberately kept: the client sends it once per join and will not
    // resend, so dropping it would permanently downgrade a tracked player to untracked.
    // The re-announce re-applies it through on_pssk_received instead.
    for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
        player.afstats_key.clear();
        player.afstats_round = AfstatsRoundCounters{};
        player.afstats_leave_reason = -1;
    }
}

// Re-establishes identity for everyone already connected when a session starts. Normally
// a no-op (players are announced as they join); it does the work after a mid-session
// teardown, where every key was cleared and nothing else would ever re-mint one.
void reannounce_connected_players()
{
    // on_player_join -> on_status -> ensure_session can re-enter this. The
    // g_session_started short-circuit in ensure_session already prevents it, but the
    // guard keeps that from being a load-bearing detail of an unrelated function.
    if (g_reannouncing) {
        return;
    }
    g_reannouncing = true;
    for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
        if (player.is_browser || !player.afstats_key.empty()) {
            continue;
        }
        on_player_join(&player);
        // Re-apply a PSSK this connection already delivered, so a player who was tracked
        // before the teardown is tracked again rather than being stuck on the fresh UPSSK.
        if (player.afstats_pssk) {
            on_pssk_received(&player);
        }
    }
    g_reannouncing = false;
}

// Starts the session lazily on the first event, so `server_hello` is always seq 1
// and always carries a populated netgame.
bool ensure_session()
{
    if (g_session_started) {
        return true;
    }
    if (!emitting_enabled()) {
        return false;
    }
    g_session_started = true;
    ++g_session_generation;
    g_session_id = mint_session_id();
    g_process_start = std::chrono::steady_clock::now();
    g_next_pulse = std::chrono::steady_clock::now() + k_pulse_interval;
    if constexpr (AFSTATS_VERIFICATION_LOGGING) {
        xlog::warn("[afstats-ev] session {} started", g_session_id);
    }
    emit_server_hello();
    reannounce_connected_players();
    return true;
}

// -------------------------------------------------------------------------
// round_start / round_end content
// -------------------------------------------------------------------------

std::vector<MutatorRecord> build_mutators()
{
    std::vector<MutatorRecord> out;
    for (const MutatorDeclaration& decl : g_alpine_server_config_active_rules.mutators.declarations) {
        MutatorRecord record;
        // Mutator names, option keys, and string values are operator-authored config,
        // so they go through sanitize_string like every other external string.
        record.name = sanitize_string(decl.name, k_max_string_len);
        for (const auto& [key, value] : decl.options) {
            std::string clean_key = sanitize_string(key, k_max_string_len);
            std::visit(
                [&record, &clean_key](const auto& v) {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, bool>) {
                        record.settings.emplace_back(clean_key, SettingValue{v});
                    }
                    else if constexpr (std::is_same_v<T, int32_t>) {
                        record.settings.emplace_back(clean_key, SettingValue{static_cast<int64_t>(v)});
                    }
                    else if constexpr (std::is_same_v<T, float>) {
                        record.settings.emplace_back(clean_key, SettingValue{static_cast<double>(v)});
                    }
                    else {
                        record.settings.emplace_back(clean_key,
                                                     SettingValue{sanitize_string(v, k_max_string_len)});
                    }
                },
                value);
        }
        out.push_back(std::move(record));
    }
    return out;
}

std::vector<std::pair<std::string, SettingValue>> build_gametype_settings(rf::NetGameType type)
{
    const auto& rules = g_alpine_server_config_active_rules;
    std::vector<std::pair<std::string, SettingValue>> out;
    const auto add = [&out](const char* key, int64_t value) {
        out.emplace_back(key, SettingValue{value});
    };

    switch (type) {
        case rf::NG_TYPE_BAG:
        case rf::NG_TYPE_TBAG:
            add("score_tick_ms", bagman_get_score_tick_ms());
            add("bag_return_time_ms", rules.bagman.bag_return_time_ms);
            add("bag_spawn_delay_ms", rules.bagman.bag_spawn_delay_ms);
            break;
        case rf::NG_TYPE_SAL:
            add("flag_spawn_delay_ms", rules.salvage.flag_spawn_delay_ms);
            add("flag_capture_respawn_delay_ms", rules.salvage.flag_capture_respawn_delay_ms);
            add("flag_return_time_ms", rules.salvage.flag_return_time_ms);
            break;
        case rf::NG_TYPE_CTF:
            add("flag_return_time_ms", rules.ctf_flag_return_time_ms);
            break;
        case rf::NG_TYPE_KOTH:
        case rf::NG_TYPE_DC:
        case rf::NG_TYPE_REV:
        case rf::NG_TYPE_ESC:
            add("grow_rate", g_koth_info.rules.grow_rate);
            add("drain_empty_rate", g_koth_info.rules.drain_empty_rate);
            add("drain_defended_rate", g_koth_info.rules.drain_defended_rate);
            add("ms_per_point", g_koth_info.rules.ms_per_point);
            break;
        case rf::NG_TYPE_PIT:
        case rf::NG_TYPE_WO:
            add("max_rounds", rules.rounds.max_rounds);
            add("round_time_s", rules.rounds.round_time);
            add("post_round_time_s", rules.rounds.post_round_time);
            add("intermission_time_s", rules.rounds.intermission_time);
            break;
        default:
            break;
    }
    return out;
}

uint8_t build_rf_flags()
{
    // Recomputed rather than read back from build_af_server_info_packet, which is
    // not idempotent.
    uint8_t flags = 0;
    if (rf::netgame.flags & rf::NG_FLAG_WEAPON_STAY) {
        flags |= rf_server_info_flags::RFSIF_WEAPON_STAY;
    }
    if (rf::netgame.flags & rf::NG_FLAG_FORCE_RESPAWN) {
        flags |= rf_server_info_flags::RFSIF_FORCE_RESPAWN;
    }
    if (rf::netgame.flags & rf::NG_FLAG_TEAM_DAMAGE) {
        flags |= rf_server_info_flags::RFSIF_TEAM_DAMAGE;
    }
    if (rf::netgame.flags & rf::NG_FLAG_FALL_DAMAGE) {
        flags |= rf_server_info_flags::RFSIF_FALL_DAMAGE;
    }
    if (rf::netgame.flags & rf::NG_FLAG_BALANCE_TEAMS) {
        flags |= rf_server_info_flags::RFSIF_BALANCE_TEAMS;
    }
    return flags;
}

void team_scores(rf::NetGameType type, int& red, int& blue)
{
    red = 0;
    blue = 0;
    switch (type) {
        case rf::NG_TYPE_CTF:
            red = rf::multi_ctf_get_red_team_score();
            blue = rf::multi_ctf_get_blue_team_score();
            break;
        case rf::NG_TYPE_TEAMDM:
            red = rf::multi_tdm_get_red_team_score();
            blue = rf::multi_tdm_get_blue_team_score();
            break;
        case rf::NG_TYPE_KOTH:
        case rf::NG_TYPE_DC:
        case rf::NG_TYPE_REV:
        case rf::NG_TYPE_ESC:
            red = multi_koth_get_red_team_score();
            blue = multi_koth_get_blue_team_score();
            break;
        case rf::NG_TYPE_SAL:
            red = salvage_get_red_team_score();
            blue = salvage_get_blue_team_score();
            break;
        case rf::NG_TYPE_BAG:
        case rf::NG_TYPE_TBAG:
            red = bagman_get_red_team_score();
            blue = bagman_get_blue_team_score();
            break;
        case rf::NG_TYPE_WO:
            red = wipeout_get_red_team_score();
            blue = wipeout_get_blue_team_score();
            break;
        default:
            break;
    }
}

// The match system stores no winner, so it is derived the same way the endgame HUD
// does: team scores in team types, top score otherwise. A tie reports no winner.
void derive_winner(uint8_t& winner_team, rf::Player*& winner_player)
{
    winner_team = team_none;
    winner_player = nullptr;

    const auto game_type = rf::multi_get_game_type();
    if (multi_is_team_game_type()) {
        int red = 0;
        int blue = 0;
        team_scores(game_type, red, blue);
        if (red > blue) {
            winner_team = team_red;
        }
        else if (blue > red) {
            winner_team = team_blue;
        }
        return;
    }

    int best = 0;
    bool have_best = false;
    bool tied = false;
    for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
        if (player.is_browser || !player.stats || player.afstats_key.empty()) {
            continue;
        }
        const int score = player.stats->score;
        if (!have_best || score > best) {
            best = score;
            winner_player = &player;
            have_best = true;
            tied = false;
        }
        else if (score == best) {
            tied = true;
        }
    }
    if (tied) {
        winner_player = nullptr;
    }
}

void emit_round_end(RoundEndType end_type)
{
    // Aggregates belong to the round that is closing, not the one that follows.
    flush_accumulators();

    const auto game_type = rf::multi_get_game_type();

    EvRoundEnd ev;
    ev.end_type = static_cast<uint8_t>(end_type);
    ev.duration_ms = static_cast<uint64_t>(std::max(rf::level.time, 0.0f) * 1000.0f);
    ev.overtime = g_is_overtime;
    team_scores(game_type, ev.red_score, ev.blue_score);
    ev.players = build_roster(true);

    if constexpr (AFSTATS_VERIFICATION_LOGGING) {
        xlog::warn("[afstats-ev] round {} ended ({}, {} ms, {} players)", g_round, ev.end_type,
                   ev.duration_ms, ev.players.size());
    }
    push_event(std::move(ev));

    g_round_open = false;
    g_pending_end_type.reset();

    // Build immediately at round end so the website can show finished
    // rounds promptly instead of waiting out the pulse.
    g_next_pulse = std::chrono::steady_clock::now();
}

// -------------------------------------------------------------------------
// Console commands
// -------------------------------------------------------------------------

ConsoleCommand2 sv_afstats_events_status_cmd{
    "sv_afstats_events_status",
    []() {
        if (!g_session_started) {
            rf::console::print("AF stats event stream: not started ({})",
                               fflink::afstats_server_enabled() ? "no events yet" : "stats not enabled");
            return;
        }
        rf::console::print("AF stats event stream: session {}", g_session_id);
        rf::console::print("  Round: {} ({})", g_round, g_round_open ? "open" : "closed");
        rf::console::print("  Events emitted: {} (dropped {})", g_events_emitted, g_events_dropped);
        rf::console::print("  Queue depth: {} / {}", g_queue.size(), k_max_queued_events);
        rf::console::print("  Batches acked: {} (next batch number {})", g_batches_acked, g_next_batch);
        if (g_pending.empty()) {
            rf::console::print("  In flight: none");
        }
        else {
            const auto& front = g_pending.front();
            rf::console::print("  In flight: batch {} ({} events, {} attempts){}", front.batch,
                               front.events.size(), front.attempts,
                               g_pending.size() > 1
                                   ? std::format(", {} more queued", g_pending.size() - 1)
                                   : std::string{});
        }
        rf::console::print("  Last response: {}", g_last_response);
        if (g_paused_401) {
            rf::console::print("  Paused: waiting for a new session key after a 401");
        }
        const auto pulse_s = std::chrono::duration_cast<std::chrono::seconds>(g_pulse_interval).count();
        rf::console::print("  Pulse: {}s, trace {}", pulse_s, g_trace ? "on" : "off");
    },
    "Show the status of the FactionFiles stats event stream.",
};

ConsoleCommand2 sv_afstats_trace_cmd{
    "sv_afstats_trace",
    [](std::optional<int> enabled) {
        if (enabled) {
            g_trace = *enabled != 0;
        }
        rf::console::print("AF stats event batch tracing is {}.", g_trace ? "enabled" : "disabled");
    },
    "Log the full JSON of every built stats batch.",
    "sv_afstats_trace <0|1>",
};

} // namespace

// -------------------------------------------------------------------------
// Emission API
// -------------------------------------------------------------------------

void on_player_join(rf::Player* player)
{
    if (!player || player->is_browser) {
        return; // browsers never join and never appear in the stream
    }
    if (!ensure_session()) {
        return;
    }
    if (!player->afstats_key.empty()) {
        return; // a resend of the join request must not mint a second key
    }

    player->afstats_key = mint_upssk();
    reset_round_counters(player);
    player->afstats_leave_reason = -1;

    EvPlayerJoin ev;
    ev.upssk = player->afstats_key;
    ev.name = sanitize_string(player->name.c_str(), k_max_name_len);
    ev.kind = player->is_bot ? k_join_kind_bot : k_join_kind_human;
    ev.client = client_string(player);

    if constexpr (AFSTATS_VERIFICATION_LOGGING) {
        // VERIFICATION PHASE ONLY: logs the session key verbatim, by explicit request.
        // A UPSSK is a server-minted throwaway for an untracked player, not a client
        // credential, but it is the identity the remediate line below pairs against.
        xlog::warn("[afstats-ev] minted UPSSK {} for {} ({})", ev.upssk, ev.name,
                   player->is_bot ? "bot" : "human");
    }
    push_event(std::move(ev));

    // A player joining mid-round is in no round_start roster until the next one, so
    // without this FF derives team=none for them. Team is finalized by the
    // join flow before this runs; fire only for team gametypes, once per join.
    if (multi_is_team_game_type()) {
        on_status(player,
                  player->team == rf::TEAM_BLUE ? StatusKind::team_blue : StatusKind::team_red);
    }
}

void on_pssk_received(rf::Player* player)
{
    if (!player || !player->afstats_pssk || !ensure_session()) {
        return;
    }
    const std::string& pssk = *player->afstats_pssk;
    // First-write-wins: once remediated to a real PSSK, never overwrite it. A
    // second, different PSSK for the same connection must not rewrite identity or
    // leak a live key into the archived upssk field.
    if (!player->afstats_key.empty() && !player->afstats_key.starts_with("u-")) {
        return;
    }
    if (player->afstats_key == pssk) {
        return; // duplicate delivery of a key we already report under
    }
    if (player->afstats_key.empty()) {
        // No UPSSK to fold in (join predates the module being active); just adopt it.
        player->afstats_key = pssk;
        if constexpr (AFSTATS_VERIFICATION_LOGGING) {
            // VERIFICATION PHASE ONLY: logs the session key verbatim, by explicit request.
            xlog::warn("[afstats-ev] adopted PSSK {} for {} with no prior key", pssk, player->name);
        }
        return;
    }

    // Ship everything accumulated under the UPSSK keyed by the UPSSK before the
    // swap, so pre-remediation damage/accuracy attaches to the untracked identity and
    // only later activity ships under the PSSK. Ordering: flush, emit, swap.
    flush_accumulators();

    EvPlayerRemediate ev;
    ev.upssk = player->afstats_key;
    ev.pssk = pssk;
    if constexpr (AFSTATS_VERIFICATION_LOGGING) {
        // VERIFICATION PHASE ONLY: logs the session key verbatim, by explicit request.
        xlog::warn("[afstats-ev] remediating {} : UPSSK {} -> PSSK {}", player->name, ev.upssk, ev.pssk);
    }
    push_event(std::move(ev));
    player->afstats_key = pssk;
}

void note_leave_reason(rf::Player* player, LeaveReason reason)
{
    if (!emitting_enabled()) {
        return; // never write the latch on a stats-disabled server
    }
    if (!player || player->afstats_leave_reason >= 0) {
        return;
    }
    player->afstats_leave_reason = static_cast<int8_t>(reason);
}

void on_player_leave(rf::Player* player)
{
    if (!player || player->afstats_key.empty() || !g_session_started) {
        return;
    }

    EvPlayerLeave ev;
    // Nothing local marked this player: they went away without a left_game packet
    // and without a kick this build initiated, which is a connection timeout.
    ev.reason = player->afstats_leave_reason >= 0
        ? static_cast<uint8_t>(player->afstats_leave_reason)
        : static_cast<uint8_t>(LeaveReason::timeout);
    ev.stats = build_roster_entry(player, true);

    if constexpr (AFSTATS_VERIFICATION_LOGGING) {
        // Redacted, not marked: the key is incidental to a leave record and this fires for
        // every disconnect, so a live PSSK would end up in the log of every server forever.
        xlog::warn("[afstats-ev] {} left (reason {}), key {}", ev.stats.name, ev.reason,
                   key_suffix(ev.stats.player));
    }
    push_event(std::move(ev));
    player->afstats_key.clear();
}

void on_player_rename(rf::Player* player, const char* name, std::string_view prev_name)
{
    if (!player || !ensure_session() || player->afstats_key.empty()) {
        return;
    }
    const std::string new_name = sanitize_string(name ? name : "", k_max_name_len);

    // Only the edge matters. A client can resend its current name, and the roster
    // already carries whatever the player holds, so an unchanged name is not a stream
    // event. Compare the sanitized forms so a change confined to bytes the wire never
    // shows is also a no-op. This intentionally does not touch the rate floor below.
    if (new_name == sanitize_string(prev_name, k_max_name_len)) {
        return;
    }

    // Floor the emission rate so a rename flood cannot become an event flood. The stock
    // handler already applied and rebroadcast the name; only the stats event is gated,
    // and the next roster still reports the live name regardless. uptime_ms() resets when
    // a new session starts, so a stored value ahead of `now` is treated as out of the
    // window and allowed through rather than wrongly suppressed.
    const int64_t now = static_cast<int64_t>(uptime_ms());
    constexpr int64_t k_rename_floor_ms = 1000;
    if (player->last_rename_ms && now >= *player->last_rename_ms
        && now - *player->last_rename_ms < k_rename_floor_ms) {
        return;
    }
    player->last_rename_ms = now;

    EvPlayerRename ev;
    ev.player = player->afstats_key;
    ev.name = new_name;
    if constexpr (AFSTATS_VERIFICATION_LOGGING) {
        // Redacted: the key is incidental here, the name change is the subject.
        xlog::warn("[afstats-ev] rename {} -> '{}'", key_suffix(ev.player), ev.name);
    }
    push_event(std::move(ev));
}

void on_round_start()
{
    if (!ensure_session()) {
        return;
    }
    if (g_round_open) {
        // A round that never reported an end (level reload during limbo, etc.).
        emit_round_end(g_pending_end_type.value_or(RoundEndType::map_change_manual));
    }
    flush_accumulators();
    if (g_any_round_started) {
        ++g_round;
    }
    g_any_round_started = true;
    g_round_open = true;
    g_pending_end_type.reset();

    for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
        if (!player.is_browser) {
            reset_round_counters(&player);
        }
    }

    const auto game_type = rf::multi_get_game_type();

    EvRoundStart ev;
    ev.tc_mod = rf::mod_param.found() ? sanitize_string(rf::mod_param.get_arg(), k_max_string_len)
                                      : std::string{};
    ev.level_file = sanitize_string(rf::level.filename.c_str(), k_max_string_len);
    ev.level_name = sanitize_string(rf::level.name.c_str(), k_max_string_len);
    ev.gametype = static_cast<uint8_t>(game_type);
    ev.time_limit_s = rf::multi_time_limit > 0.0f ? static_cast<uint32_t>(rf::multi_time_limit) : 0;
    ev.win_condition = g_alpine_server_config_active_rules.get_score_limit(game_type).value_or(0);
    ev.overtime_enabled = g_alpine_server_config_active_rules.overtime.enabled;
    // Computed fresh from config/state rather than read back from the cached
    // server-info static, which is 0 until the first server-info packet is built:
    // a dedicated server's first round_start can precede any such packet.
    ev.af_flags = af_compute_server_info_flags();
    ev.rf_flags = build_rf_flags();
    ev.gi_flags = server_get_game_info_flags().game_info_flags_to_uint32();
    // Always stamped, so warmup and match rounds are separable even when the
    // match_start / match_end pair was lost to a gap.
    ev.match_state = af_match_state_for_stats();
    ev.mutators = build_mutators();
    ev.gametype_settings = build_gametype_settings(game_type);
    ev.roster = build_roster(false);

    if constexpr (AFSTATS_VERIFICATION_LOGGING) {
        xlog::warn("[afstats-ev] round {} started: {} (gametype {}), {} players, {} mutators", g_round,
                   ev.level_file, ev.gametype, ev.roster.size(), ev.mutators.size());
    }
    push_event(std::move(ev));
}

void note_round_end_type(RoundEndType end_type)
{
    if (!emitting_enabled()) {
        return; // never write the latch on a stats-disabled server
    }
    if (!g_pending_end_type) {
        g_pending_end_type = end_type;
    }
}

void on_round_end()
{
    if (!g_session_started || !g_round_open) {
        return;
    }
    emit_round_end(g_pending_end_type.value_or(RoundEndType::map_change_manual));
}

void on_kill(rf::Player* victim, rf::Player* killer, int weapon_type, int damage_type, uint8_t kill_flags,
             const std::vector<uint8_t>& assist_player_ids, const rf::Vector3& victim_pos,
             const rf::Vector3* killer_pos)
{
    if (!victim || !ensure_session() || victim->afstats_key.empty()) {
        return;
    }

    EvKill ev;
    ev.victim = victim->afstats_key;
    if (killer) {
        ev.killer = killer->afstats_key;
    }
    ev.weapon = weapon_type;
    // The registry is uint8; anything outside it would fail FF-side validation, so
    // an out-of-range engine value is reported as the unknown-damage sentinel.
    ev.damage_type = (damage_type >= 0 && damage_type <= 0xFF) ? static_cast<uint8_t>(damage_type) : 0;
    ev.flags = kill_flags;
    ev.victim_pos = Vec3{victim_pos.x, victim_pos.y, victim_pos.z};
    if (killer_pos) {
        ev.has_killer_pos = true;
        ev.killer_pos = Vec3{killer_pos->x, killer_pos->y, killer_pos->z};
    }

    for (uint8_t assist_id : assist_player_ids) {
        rf::Player* assister = rf::multi_find_player_by_id(assist_id);
        if (assister && !assister->afstats_key.empty()) {
            ev.assists.push_back(assister->afstats_key);
            ++assister->afstats_round.assists;
        }
    }

    ++victim->afstats_round.deaths;
    victim->afstats_round.current_streak = 0;
    if (killer && killer != victim) {
        auto& c = killer->afstats_round;
        ++c.kills;
        ++c.current_streak;
        c.highest_streak = std::max(c.highest_streak, c.current_streak);
    }

    push_event(std::move(ev));
}

void on_spawn(rf::Player* player, const rf::Vector3& pos)
{
    if (!player || !ensure_session() || player->afstats_key.empty()) {
        return;
    }
    EvSpawn ev;
    ev.player = player->afstats_key;
    ev.pos = Vec3{pos.x, pos.y, pos.z};
    push_event(std::move(ev));
}

void on_damage(rf::Player* attacker, rf::Player* victim, int weapon_type, int damage_type, float amount,
               bool direct_hit, bool headshot)
{
    if (!victim || amount <= 0.0f || !ensure_session() || victim->afstats_key.empty()) {
        return;
    }

    // An attacker with no key is environmental as far as the stream is concerned. A view,
    // not a copy: this function runs on every damage application and the string it would
    // copy is longer than the small-string buffer. Both players outlive this call.
    const std::string_view attacker_key = (attacker && !attacker->afstats_key.empty())
        ? std::string_view{attacker->afstats_key}
        : std::string_view{};
    const uint8_t dt = (damage_type >= 0 && damage_type <= 0xFF) ? static_cast<uint8_t>(damage_type) : 0;

    DamageAccum& acc =
        damage_bucket(DamageKeyView{attacker_key, victim->afstats_key, weapon_type, dt});
    acc.amount += amount;
    ++acc.hits;

    victim->afstats_round.damage_taken += amount;
    // Count self-splash toward damage_dealt too: the stream emits a damage
    // event for attacker==victim, and FF folds that into damage_dealt, so the summary
    // counter must match or it diverges from the stream by exactly the self-damage.
    // Keyed on attacker_key rather than the pointer so this matches the shots_hit guard
    // below and the victim-side guard above: a player with no stats key never appears in
    // a roster, so accumulating a summary for them can only ever be dead work.
    if (!attacker_key.empty()) {
        attacker->afstats_round.damage_dealt += amount;
    }

    // Only a direct projectile hit is an accuracy hit. Splash still
    // counts toward `damage`, which is why this is not the same condition. `hit`
    // counts damage-applications, so a projectile that damages two players counts
    // twice; that is definitional, accuracy is only compared within a weapon.
    if (direct_hit && weapon_type >= 0 && !attacker_key.empty()) {
        AccuracyAccum& shots = accuracy_bucket(AccuracyKeyView{attacker_key, weapon_type});
        ++shots.hit;
        if (headshot) {
            ++shots.hit_head;
        }
        ++attacker->afstats_round.shots_hit;
    }
}

void on_weapon_fired(rf::Player* player, int weapon_type, uint32_t projectiles)
{
    if (!player || weapon_type < 0 || projectiles == 0 || !ensure_session()
        || player->afstats_key.empty()) {
        return;
    }
    accuracy_bucket(AccuracyKeyView{player->afstats_key, weapon_type}).fired += projectiles;
    player->afstats_round.shots_fired += projectiles;
}

void on_status(rf::Player* player, StatusKind kind, int value)
{
    if (!player || !ensure_session() || player->afstats_key.empty()) {
        return;
    }
    EvStatus ev;
    ev.player = player->afstats_key;
    ev.kind = static_cast<uint8_t>(kind);
    if (kind == StatusKind::handicap) {
        ev.has_value = true;
        ev.value = value;
    }
    if (g_trace) {
        xlog::warn("[afstats-ev] status kind {} for {}", ev.kind, player->name);
    }
    push_event(std::move(ev));
}

void on_item_pickup(rf::Player* player, int item_type, const rf::Vector3& pos, int respawn_ms)
{
    if (!player || !ensure_session() || player->afstats_key.empty()) {
        return;
    }
    EvItemPickup ev;
    ev.player = player->afstats_key;
    ev.item = item_type;
    ev.pos = Vec3{pos.x, pos.y, pos.z};
    ev.respawn_ms = respawn_ms > 0 ? static_cast<uint32_t>(respawn_ms) : 0;
    push_event(std::move(ev));
}

void on_flag_event(FlagEventKind kind, uint8_t team, rf::Player* player, const rf::Vector3& pos)
{
    if (!ensure_session()) {
        return;
    }
    EvFlagEvent ev;
    ev.kind = static_cast<uint8_t>(kind);
    ev.team = team;
    if (player && !player->afstats_key.empty()) {
        ev.player = player->afstats_key;
    }
    ev.pos = Vec3{pos.x, pos.y, pos.z};
    if (g_trace) {
        xlog::warn("[afstats-ev] flag_event kind {} team {}", ev.kind, ev.team);
    }
    push_event(std::move(ev));
}

void on_point_event(int hill_uid, PointEventKind kind, uint8_t owner,
                    const std::vector<rf::Player*>& credited, bool locked)
{
    if (!ensure_session()) {
        return;
    }
    EvPointEvent ev;
    ev.hill = static_cast<uint8_t>(std::clamp(hill_uid, 0, 255));
    ev.kind = static_cast<uint8_t>(kind);
    ev.owner = owner;
    ev.locked = locked;
    for (rf::Player* player : credited) {
        if (player && !player->afstats_key.empty()) {
            ev.players.push_back(player->afstats_key);
        }
    }
    if (g_trace) {
        xlog::warn("[afstats-ev] point_event hill {} kind {} owner {}", ev.hill, ev.kind, ev.owner);
    }
    push_event(std::move(ev));
}

void on_bagman_event(BagmanEventKind kind, rf::Player* player, const rf::Vector3& pos)
{
    if (!ensure_session()) {
        return;
    }
    EvBagmanEvent ev;
    ev.kind = static_cast<uint8_t>(kind);
    if (player && !player->afstats_key.empty()) {
        ev.player = player->afstats_key;
    }
    ev.pos = Vec3{pos.x, pos.y, pos.z};
    if (g_trace) {
        xlog::warn("[afstats-ev] bagman_event kind {}", ev.kind);
    }
    push_event(std::move(ev));
}

void on_bagman_pickup(rf::Player* player, const rf::Vector3& pos, BagmanFrom from)
{
    if (!ensure_session()) {
        return;
    }
    EvBagmanEvent ev;
    ev.kind = static_cast<uint8_t>(BagmanEventKind::pickup);
    if (player && !player->afstats_key.empty()) {
        ev.player = player->afstats_key;
    }
    ev.has_from = true;
    ev.from = static_cast<uint8_t>(from);
    ev.pos = Vec3{pos.x, pos.y, pos.z};
    if (g_trace) {
        xlog::warn("[afstats-ev] bagman_event pickup from {}", ev.from);
    }
    push_event(std::move(ev));
}

void on_gg_levelup(rf::Player* player, int level, int weapon_type)
{
    if (!player || !ensure_session() || player->afstats_key.empty()) {
        return;
    }
    EvGgLevelup ev;
    ev.player = player->afstats_key;
    ev.level = static_cast<uint32_t>(std::max(level, 0));
    ev.weapon = weapon_type;
    if (g_trace) {
        xlog::warn("[afstats-ev] gg_levelup {} to level {}", player->name, ev.level);
    }
    push_event(std::move(ev));
}

void on_match_start(int team_size, const std::vector<rf::Player*>& participants)
{
    if (!ensure_session()) {
        return;
    }
    // A prior match that never saw its end still holds the shared latch: an external
    // level change or admin map mid-match closes the round path but not the match path,
    // leaving g_match_open set. Close it as canceled before opening the new one so the
    // stream never carries two match_start without an end between them. This is done here
    // rather than in on_round_start on purpose: a ready-up match spans multiple rounds
    // (start_match restarts the level, firing on_round_start while the match is live), so
    // closing on round_start would wrongly cancel a live ready-up match. on_match_start
    // fires once per match for both producers, so it is the safe single close point.
    if (g_match_open) {
        on_match_end(MatchResult::canceled, team_none, nullptr);
    }
    EvMatchStart ev;
    ev.team_size = static_cast<uint32_t>(std::max(team_size, 0));
    for (rf::Player* player : participants) {
        if (player && !player->afstats_key.empty()) {
            ev.participants.push_back(player->afstats_key);
        }
    }
    g_match_open = true;
    if (g_trace) {
        xlog::warn("[afstats-ev] match_start {}v{} with {} participants", ev.team_size, ev.team_size,
                   ev.participants.size());
    }
    push_event(std::move(ev));
}

void on_match_end(MatchResult result, uint8_t winner_team, rf::Player* winner_player)
{
    if (!ensure_session() || !g_match_open) {
        return; // a cancel and the limbo that follows it both reach this
    }
    g_match_open = false;

    EvMatchEnd ev;
    ev.result = static_cast<uint8_t>(result);
    ev.winner_team = winner_team;
    if (winner_player && !winner_player->afstats_key.empty()) {
        ev.winner_player = winner_player->afstats_key;
    }
    if (g_trace) {
        xlog::warn("[afstats-ev] match_end result {} winner_team {}", ev.result, ev.winner_team);
    }
    push_event(std::move(ev));
}

void on_match_end_derived(MatchResult result)
{
    if (!ensure_session() || !g_match_open) {
        return;
    }
    uint8_t winner_team = team_none;
    rf::Player* winner_player = nullptr;
    derive_winner(winner_team, winner_player);
    on_match_end(result, winner_team, winner_player);
}

void on_vote_called(uint8_t vote_type, rf::Player* initiator, rf::Player* target, const char* detail)
{
    if (!ensure_session()) {
        return;
    }
    EvVoteCalled ev;
    ev.vote_type = vote_type;
    if (initiator && !initiator->afstats_key.empty()) {
        ev.initiator = initiator->afstats_key;
    }
    if (target && !target->afstats_key.empty()) {
        ev.target = target->afstats_key;
    }
    if (detail) {
        ev.detail = sanitize_string(detail, k_max_string_len);
    }
    if (g_trace) {
        xlog::warn("[afstats-ev] vote_called type {} detail '{}'", ev.vote_type, ev.detail);
    }
    push_event(std::move(ev));
}

void on_vote_ended(uint8_t vote_type, uint8_t result, int yes, int no, int eligible)
{
    if (!ensure_session()) {
        return;
    }
    EvVoteEnded ev;
    ev.vote_type = vote_type;
    ev.result = result;
    ev.yes = static_cast<uint32_t>(std::max(yes, 0));
    ev.no = static_cast<uint32_t>(std::max(no, 0));
    ev.eligible = static_cast<uint32_t>(std::max(eligible, 0));
    if (g_trace) {
        xlog::warn("[afstats-ev] vote_ended type {} result {} ({} yes / {} no / {} eligible)",
                   ev.vote_type, ev.result, ev.yes, ev.no, ev.eligible);
    }
    push_event(std::move(ev));
}

void on_geomod(const rf::Vector3& pos, float scale, bool rf2_style, rf::Player* shooter,
               int weapon_type)
{
    if (!ensure_session()) {
        return;
    }
    EvGeomod ev;
    ev.pos = Vec3{pos.x, pos.y, pos.z};
    ev.scale = scale;
    ev.rf2_style = rf2_style;
    if (shooter && !shooter->afstats_key.empty()) {
        ev.player = shooter->afstats_key;
    }
    // The weapon is reported only alongside a resolved shooter.
    ev.weapon = ev.player.empty() ? -1 : weapon_type;
    if (g_trace) {
        xlog::warn("[afstats-ev] geomod scale {:.1f} rf2 {}", ev.scale, ev.rf2_style);
    }
    push_event(std::move(ev));
}

void on_clutter_destroyed(rf::Clutter* clutter, int killer_handle, int weapon_type, int damage_type)
{
    if (!clutter || !ensure_session()) {
        return;
    }
    EvClutterDestroyed ev;
    // info_index -1 is an alpine mesh with no clutter.tbl type; report that as null.
    if (clutter->info_index >= 0) {
        ev.has_clutter_type = true;
        ev.clutter_type = static_cast<uint32_t>(clutter->info_index);
    }
    // A runtime-spawned clutter carries a negative uid; only report a placed object's uid.
    if (clutter->uid >= 0) {
        ev.has_uid = true;
        ev.uid = clutter->uid;
    }
    if (rf::Player* attacker = rf::player_from_entity_handle(killer_handle)) {
        if (!attacker->afstats_key.empty()) {
            ev.player = attacker->afstats_key;
        }
    }
    // The weapon is reported only alongside a resolved attacker.
    ev.weapon = ev.player.empty() ? -1 : weapon_type;
    ev.damage_type = (damage_type >= 0 && damage_type <= 0xFF) ? static_cast<uint8_t>(damage_type) : 0;
    ev.pos = Vec3{clutter->pos.x, clutter->pos.y, clutter->pos.z};
    if (g_trace) {
        xlog::warn("[afstats-ev] clutter_destroyed info_index {} uid {}", clutter->info_index,
                   clutter->uid);
    }
    push_event(std::move(ev));
}

void on_detail_brush_destroyed(uint8_t material, int room_uid, int killer_handle, int weapon_type,
                               int damage_type, const rf::Vector3& pos)
{
    if (!ensure_session()) {
        return;
    }
    EvDetailBrushDestroyed ev;
    ev.material = material;
    if (room_uid >= 0) {
        ev.has_room_uid = true;
        ev.room_uid = room_uid;
    }
    if (rf::Player* attacker = rf::player_from_entity_handle(killer_handle)) {
        if (!attacker->afstats_key.empty()) {
            ev.player = attacker->afstats_key;
        }
    }
    // The weapon is reported only alongside a resolved attacker.
    ev.weapon = ev.player.empty() ? -1 : weapon_type;
    ev.damage_type = (damage_type >= 0 && damage_type <= 0xFF) ? static_cast<uint8_t>(damage_type) : 0;
    ev.pos = Vec3{pos.x, pos.y, pos.z};
    if (g_trace) {
        xlog::warn("[afstats-ev] detail_brush_destroyed material {} room_uid {}", ev.material,
                   room_uid);
    }
    push_event(std::move(ev));
}

void do_patch()
{
    sv_afstats_events_status_cmd.register_cmd();
    sv_afstats_trace_cmd.register_cmd();
}

namespace {

void do_frame_impl()
{
    if (!g_session_started) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const bool pulse_due = now >= g_next_pulse;

    // A revoked or malformed GSK never yields a GSSK, so stop the stream cleanly
    // instead of caching forever. Checked here rather than only on the paused-401
    // path below because a session starts on the first event, which can land while
    // the key exchange is still pending: if that exchange then hard-rejects, nothing
    // is ever posted, no 401 ever comes back, and the pause path is unreachable while
    // the queue and the per-frame sampling run on. A genuinely new GSSK later restarts
    // a fresh session through ensure_session().
    // Only on the pulse: snapshot_state() is a mutex plus a copy.
    if (pulse_due) {
        const auto status = fflink::snapshot_state().status;
        if (status == fflink::SessionStatus::rejected_by_server
            || status == fflink::SessionStatus::bad_gsk_format) {
            // The GSSK is contractually rejected (a 4xx, or a malformed configured GSK),
            // so nothing more can be transmitted: an open round, an open match, and the
            // whole queue terminate silently by necessity. Unlike the shutdown flush path
            // above, no closing round_end / match_end is emitted, because there is no
            // session left to carry it — this is the spec's orphaned-session behavior, not
            // a dropped event we could recover. reset_module_state() clears g_match_open
            // (and g_round_open) so a genuinely new GSSK later starts a clean session.
            xlog::warn("[afstats-ev] session key permanently rejected; stopping the event stream");
            reset_module_state();
            return;
        }
    }

    for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
        if (!player.is_browser && !player.afstats_key.empty()) {
            sample_player_state(&player);
        }
    }

    if (pulse_due) {
        g_next_pulse = now + g_pulse_interval;
        // Move the damage/accuracy accumulators into the live queue every pulse,
        // even while a batch is in flight or the stream is paused, so they can't grow
        // for a whole round and then dump thousands of events in one frame at its end.
        flush_accumulators();
    }

    if (g_paused_401) {
        // Only re-check the session key on the pulse, not every frame, so a long
        // pause is not a per-frame get_gssk() mutex + string copy. A permanent
        // rejection has already been handled above.
        if (!pulse_due) {
            return;
        }
        const std::string gssk = fflink::get_gssk();
        if (gssk.empty() || gssk == g_paused_gssk) {
            return; // the cache keeps filling while we wait for a new key
        }
        xlog::warn("[afstats-ev] a new session key arrived; resuming the stream");
        g_paused_401 = false;
        g_paused_gssk.clear();
        g_next_pulse = now;
        return;
    }

    if (g_send_in_flight) {
        return;
    }
    if (!pulse_due) {
        return;
    }
    pump();
}

void on_shutdown_impl()
{
    if (!g_session_started) {
        return;
    }

    if (g_round_open) {
        note_round_end_type(RoundEndType::server_shutdown);
        emit_round_end(RoundEndType::server_shutdown);
    }

    // The shutdown flush below can still transmit, so close an open match rather than
    // dropping it silently. Its match_end is queued here and goes out with the flush.
    if (g_match_open) {
        on_match_end(MatchResult::canceled, team_none, nullptr);
    }

    // Tell the worker to stop; the final attempt runs inline so its timeouts, not a
    // thread handshake, bound how long shutdown can take. g_worker_stop stays set
    // through the flush below so a worker caught mid-post exits when it loops back.
    {
        std::lock_guard lock(g_worker_mutex);
        g_worker_stop = true;
        g_worker_job.reset();
    }
    g_worker_cv.notify_all();

    if (g_pending.empty()) {
        build_batch_from_queue();
    }

    const std::string gssk = g_pending.empty() ? std::string{} : fflink::get_gssk();
    if (g_pending.empty()) {
        if constexpr (AFSTATS_VERIFICATION_LOGGING) {
            xlog::warn("[afstats-ev] shutdown with nothing left to send");
        }
    }
    else if (gssk.empty()) {
        xlog::warn("[afstats-ev] shutdown with {} unsent events and no session key; discarding",
                   g_queue.size() + g_pending.front().events.size());
    }
    else {
        // A hard wall on the flush so multi_stop (and a listen-server host
        // returning to menu) can't block. Event loss here is acceptable.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
        while (!g_pending.empty() && std::chrono::steady_clock::now() < deadline) {
            PendingBatch& front = g_pending.front();
            SendJob job{build_envelope(front, gssk), front.batch, g_session_generation};
            if constexpr (AFSTATS_VERIFICATION_LOGGING) {
                xlog::warn("[afstats-ev] shutdown flush: sending batch {} ({} events)", front.batch,
                           front.events.size());
            }
            const SendResult result =
                do_one_post(job, k_shutdown_connect_timeout_ms, k_shutdown_receive_timeout_ms);
            if (result.status != 200 || !result.ack_ok || result.ack_batch != front.batch) {
                xlog::warn("[afstats-ev] shutdown flush failed ({}); {} events lost", result.detail,
                           front.events.size());
                break;
            }
            if constexpr (AFSTATS_VERIFICATION_LOGGING) {
                xlog::warn("[afstats-ev] shutdown flush: batch {} acked", front.batch);
            }
            g_pending.pop_front();
            if (g_pending.empty()) {
                build_batch_from_queue();
            }
        }
    }

    // Full reset so a second session in this process starts clean, and let the
    // worker respawn next session. g_worker_stop deliberately stays SET here: a worker
    // blocked in a post can outlive the bounded flush window above, and clearing the
    // flag now would let it loop back, see no stop, and park as a second live consumer.
    // ensure_worker_started clears it under the mutex when it claims the next
    // generation, which is also what retires any predecessor.
    reset_module_state();
    {
        std::lock_guard lock(g_worker_mutex);
        g_worker_job.reset();
    }
    g_worker_started = false;
}

} // namespace

void do_frame()
{
    // The pulse runs inside the stock frame loop, so a stats exception is
    // swallowed at the module boundary, never unwound into rf_do_frame_hook. This and
    // on_shutdown are deliberately the module's only firewalls: they wrap the JSON
    // serialization, which is the one part that genuinely throws. The emission entry
    // points are held to the same no-throw-by-construction standard as the rest of the
    // hook code rather than being blanketed in try/catch.
    try {
        do_frame_impl();
    }
    catch (const std::exception& e) {
        xlog::warn("[afstats-ev] swallowed exception in do_frame: {}", e.what());
    }
    catch (...) {
        xlog::warn("[afstats-ev] swallowed unknown exception in do_frame");
    }
}

void on_shutdown()
{
    try {
        on_shutdown_impl();
    }
    catch (const std::exception& e) {
        xlog::warn("[afstats-ev] swallowed exception in on_shutdown: {}", e.what());
    }
    catch (...) {
        xlog::warn("[afstats-ev] swallowed unknown exception in on_shutdown");
    }
}

} // namespace afstats
