#include <string_view>
#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <ctime>
#include <format>
#include <optional>
#include <cctype>
#include <utility>
#include <vector>
#include "../rf/player/player.h"
#include "../rf/level.h"
#include "../rf/multi.h"
#include "../rf/gameseq.h"
#include "../rf/misc.h"
#include "../rf/os/timestamp.h"
#include "../os/console.h"
#include "../misc/alpine_options.h"
#include "../misc/player.h"
#include "../main/main.h"
#include <common/utils/list-utils.h>
#include <common/utils/string-utils.h>
#include <xlog/xlog.h>
#include "server_internal.h"
#include "multi.h"
#include "gametype.h"
#include "mutators.h"
#include "server.h"
#include "alpine_packets.h"

MatchInfo g_match_info;

bool ends_with(const rf::String& str, const std::string& suffix)
{
    std::string name_str = str.c_str();
    if (name_str.length() >= suffix.length()) {
        return (name_str.compare(name_str.length() - suffix.length(), suffix.length(), suffix) == 0);
    }
    return false;
}

enum class VoteType
{
    Kick,
    Level,
    Restart,
    Extend,
    Next,
    Random,
    Previous,
    Match,
    CancelMatch,
    Unknown
};

static AfVoteType vote_type_to_wire(VoteType type)
{
    switch (type) {
        case VoteType::Kick: return AfVoteType::Kick;
        case VoteType::Level: return AfVoteType::Level;
        case VoteType::Match: return AfVoteType::Match;
        case VoteType::Extend: return AfVoteType::Extend;
        case VoteType::Restart: return AfVoteType::Restart;
        case VoteType::Next: return AfVoteType::Next;
        case VoteType::Random: return AfVoteType::Random;
        case VoteType::Previous: return AfVoteType::Previous;
        case VoteType::CancelMatch: return AfVoteType::CancelMatch;
        default: return AfVoteType::Kick;
    }
}

// The listen-server host is excluded from the vote system entirely: it can
// neither call a vote (the panel and the F4 bind are gated on !rf::is_server) nor
// cast one (F1/F2, same gate). Counting it as an eligible voter would dilute every
// tally and make the "everyone has voted" early finish unreachable forever.
static bool is_listen_server_host(const rf::Player* player)
{
    return rf::is_server && player == rf::local_player;
}

// Clients that consume af_sreq_vote_state instead of the legacy chat text.
static bool player_uses_vote_packets(rf::Player* player)
{
    if (!player || is_listen_server_host(player)) {
        return false;
    }
    return is_player_minimum_af_client_version(player, 1, 4, 0);
}

// Every "you can't do that" reply to a vote action. Each one costs a reliable
// packet to the sender, and the eligibility ones also pass tell_server=true, which
// writes a server console line and a log entry — so a client that spams
// af_req_vote_call / _cast / _cancel floods the server's own output, not just its
// own chat. Throttled per player, pattern of vote_options_req_timer.
//
// Only the REPLY is throttled. The action still runs (and is still rejected) every
// time, so nothing about the vote's behaviour depends on this.
static constexpr int vote_reject_msg_cooldown_ms = 1000;

static void send_vote_reject_msg(std::string_view msg, rf::Player* player, bool tell_server = false)
{
    if (!player) {
        return;
    }
    if (player->vote_reject_msg_timer.valid() && !player->vote_reject_msg_timer.elapsed()) {
        return;
    }
    player->vote_reject_msg_timer.set(vote_reject_msg_cooldown_ms);
    af_send_automated_chat_msg(msg, player, tell_server);
}

static bool player_is_connected(const rf::Player* target)
{
    if (!target) {
        return false;
    }
    for (const rf::Player& player : SinglyLinkedList{rf::player_list}) {
        if (&player == target) {
            return true;
        }
    }
    return false;
}

struct VoteTally
{
    int yes = 0;
    int no = 0;
    int remaining = 0;
};

struct Vote
{
private:
    std::time_t start_time = 0;
    bool reminder_sent = false;
    bool announced = false;
    bool end_event_sent = false;
    rf::Timestamp early_finish_check_timer;
    std::map<rf::Player*, bool> players_who_voted;
    rf::Player* owner = nullptr;
    // Copied at start time so the wire name never depends on the owner's
    // rf::Player still being alive.
    std::string owner_name;

    enum class Outcome
    {
        None,
        Accepted,
        Rejected,
    };
    Outcome pending_outcome = Outcome::None;

public:
    virtual ~Vote() = default;

    virtual VoteType get_type() const = 0;

    // Type-specific validation, run before the vote is announced. Rejection
    // messages are sent to `source` here.
    virtual bool validate([[maybe_unused]] rf::Player* source)
    {
        return true;
    }

    bool start(rf::Player* source)
    {
        owner = source;
        owner_name = source ? source->name.c_str() : "";
        start_time = std::time(nullptr);

        // The owner's own yes vote is recorded BEFORE the announcement so the
        // announced tally is the same compute_tally() every later event uses.
        players_who_voted.insert({source, true});

        announced = true;
        send_vote_starting_msg(source);

        early_finish_check_timer.set(1000);

        return check_for_early_vote_finish();
    }

    virtual bool on_player_leave(rf::Player* player)
    {
        if (player == owner) {
            early_finish_check_timer.invalidate();
            emit_vote_end(AfVoteResult::Canceled, false, "Vote canceled: owner left the game!");
            return false;
        }
        players_who_voted.erase(player);
        return check_for_early_vote_finish();
    }

    bool add_player_vote(bool is_yes_vote, rf::Player* source)
    {
        if (players_who_voted.count(source) == 1) {
            send_vote_reject_msg("You already voted!", source);
        }
        else {
            players_who_voted[source] = is_yes_vote;

            const VoteTally tally = compute_tally();

            auto msg = std::format("Vote status: Yes: {} No: {} Waiting: {}", tally.yes, tally.no, tally.remaining);
            broadcast_vote_legacy_msg(msg);
            broadcast_vote_update(tally);
            return check_for_early_vote_finish();
        }
        return true;
    }

    bool do_frame()
    {
        const auto& vote_config = get_config();

        if (!early_finish_check_timer.valid() || early_finish_check_timer.elapsed()) {
            if (!check_for_early_vote_finish()) {
                early_finish_check_timer.invalidate();
                return false;
            }
            early_finish_check_timer.set(1000);
        }

        std::time_t passed_time_sec = std::time(nullptr) - start_time;
        if (passed_time_sec >= vote_config.time_limit_seconds) {
            VoteTally tally = compute_tally();

            if (!vote_config.ignore_nonvoters) {
                tally.no += tally.remaining;
            }

            broadcast_vote_legacy_msg("Vote timed out!");
            finish_vote(tally.yes > tally.no, AfVoteResult::TimedOut);
            return false;
        }
        if (passed_time_sec >= vote_config.time_limit_seconds / 2 && !reminder_sent) {
            const auto current_player_list = get_clients(false, false);

            for (rf::Player* player : current_player_list) {
                if (players_who_voted.find(player) == players_who_voted.end()) {
                    if (player->version_info.software != ClientSoftware::AlpineFaction) { // don't send reminder pings to alpine clients
                        af_send_automated_chat_msg("Send message \"/vote yes\" or \"/vote no\" to vote.", player);
                    }
                }
            }
            reminder_sent = true;
        }
        return true;
    }

    bool try_cancel_vote(rf::Player* source)
    {
        if (owner != source) {
            send_vote_reject_msg("You cannot cancel a vote you didn't start!", source);
            return false;
        }

        early_finish_check_timer.invalidate();
        emit_vote_end(AfVoteResult::Canceled, false, "Vote canceled!");
        return true;
    }

    void cancel_for_limbo()
    {
        early_finish_check_timer.invalidate();
        emit_vote_end(AfVoteResult::Canceled, false, "Vote canceled!");
    }

    // Bring a player who joined mid-vote up to date.
    void send_start_state_to(rf::Player* player)
    {
        if (!player || !player_uses_vote_packets(player) || !player_meets_alpine_restrict(player)) {
            return;
        }

        const auto elapsed = static_cast<int>(std::time(nullptr) - start_time);
        const int seconds_left = std::max(0, get_config().time_limit_seconds - elapsed);
        const VoteTally tally = compute_tally();

        // An owner who left cancels the vote, but never trust a stale pointer:
        // an unresolvable owner is reported as an empty name.
        const std::string_view initiator = player_is_connected(owner) ? std::string_view{owner_name}
                                                                     : std::string_view{};

        af_send_vote_state_start(player, vote_type_to_wire(get_type()),
                                 static_cast<uint16_t>(seconds_left), clamp_to_u8(tally.yes),
                                 clamp_to_u8(tally.no), clamp_to_u8(tally.remaining),
                                 player == owner, /*is_sync*/ true, initiator, get_title());
    }

    // Run whatever finish_vote() decided. Only VoteMgr calls this, and only on a
    // vote it has already detached from `active_vote`.
    void run_pending_outcome()
    {
        const Outcome outcome = std::exchange(pending_outcome, Outcome::None);
        if (outcome == Outcome::Accepted) {
            on_accepted();
        }
        else if (outcome == Outcome::Rejected) {
            on_rejected();
        }
    }

    // Tell every structured client the vote is over.
    // `passed` is carried separately from `result` because they are independent: a
    // vote that TIMES OUT can still pass. `detail` is the same line legacy clients
    // are sent as chat, so a 1.4 client can print text equivalent to theirs.
    void broadcast_vote_end(AfVoteResult result, bool passed, std::string_view detail)
    {
        if (end_event_sent || !announced) {
            return; // a vote rejected during validation was never announced
        }
        end_event_sent = true;

        for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
            // Same filter as the start event: a client that never got a start
            // event must not get updates or an end event either.
            if (player_uses_vote_packets(&player) && player_meets_alpine_restrict(&player)) {
                af_send_vote_state_end(&player, result, passed, detail);
            }
        }
    }

    static bool player_meets_alpine_restrict(rf::Player* p) {
        const auto [verdict, verdict_string, hard_reject] =
            evaluate_alpine_restrict_status(p->version_info, false);
        return verdict == AlpineRestrictVerdict::ok;
    }

protected:
    [[nodiscard]] virtual std::string get_title() const = 0;
    [[nodiscard]] virtual const VoteConfig& get_config() const = 0;

    // The one line that describes the outcome. Broadcast as chat to legacy clients
    // and carried in the structured end event, so both see identical wording.
    // Called just before the outcome handler runs, so it may read the same state.
    [[nodiscard]] virtual std::string get_outcome_text(bool accepted) const
    {
        return accepted ? "Vote passed!" : "Vote failed!";
    }

    // Performs the outcome. Deliberately silent: finish_vote() has already
    // broadcast get_outcome_text().
    virtual void on_accepted() {}
    virtual void on_rejected() {}

    static uint8_t clamp_to_u8(int value)
    {
        return static_cast<uint8_t>(std::clamp(value, 0, 255));
    }

    // Vote chat text goes only to clients that can't receive the structured
    // events; their sniffing contract depends on these exact strings. The console
    // line matches af_broadcast_automated_chat_msg so dedicated server output is
    // unchanged, and the packet is built once for the whole broadcast.
    static void broadcast_vote_legacy_msg(std::string_view msg)
    {
        af_broadcast_vote_legacy_chat_msg(msg);
    }

    // Ends the vote for everyone: the legacy chat line (which is also the server
    // console line) and the structured end event carrying that same text, so both
    // client generations are told the same thing.
    void emit_vote_end(AfVoteResult result, bool passed, std::string_view detail)
    {
        broadcast_vote_legacy_msg(detail);
        broadcast_vote_end(result, passed, detail);
    }

    void broadcast_vote_update(const VoteTally& tally)
    {
        for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
            if (player_uses_vote_packets(&player) && player_meets_alpine_restrict(&player)) {
                af_send_vote_state_update(&player, clamp_to_u8(tally.yes), clamp_to_u8(tally.no),
                                          clamp_to_u8(tally.remaining));
            }
        }
    }

    void send_vote_starting_msg(rf::Player* source)
    {
        if (!source) {
            return; // should never happen
        }

        auto title = get_title();
        std::string base_msg = std::format("{} vote started by {}.\n", title, source->name);

        // print to server console. NEVER pass this through console::printf: it is
        // a varargs vsnprintf, and base_msg embeds the player name and the voted
        // level name, so a remote client could supply the format string.
        rf::console::print("{}", base_msg);

        // Prepare messages for legacy players
        std::string msg_non_alpine = "\n=============== VOTE STARTING ===============\n" + base_msg +
                                     "Send message \"/vote yes\" or \"/vote no\" to participate.";

        std::string msg_alpine = "\n=============== VOTE STARTING ===============\n" + base_msg;

        const int time_limit = std::max(0, get_config().time_limit_seconds);

        // start() already recorded the owner's yes vote, so this is the exact same
        // tally the update/timeout paths compute — not an open-coded
        // "eligible voters - 1", which disagreed whenever the owner was not
        // themselves an eligible voter.
        const VoteTally tally = compute_tally();

        for (rf::Player* player : get_clients(false, false)) {
            if (!player || is_listen_server_host(player)) {
                continue;
            }

            if (player != source && !player_meets_alpine_restrict(player)) {
                continue;
            }

            if (player_uses_vote_packets(player)) {
                af_send_vote_state_start(player, vote_type_to_wire(get_type()),
                                         static_cast<uint16_t>(time_limit), clamp_to_u8(tally.yes),
                                         clamp_to_u8(tally.no), clamp_to_u8(tally.remaining),
                                         player == source, /*is_sync*/ false, owner_name, title);
                continue;
            }

            if (player == source) {
                af_send_automated_chat_msg(base_msg, source);
                continue;
            }

            af_send_automated_chat_msg(player->version_info.software == ClientSoftware::AlpineFaction
                                           ? msg_alpine
                                           : msg_non_alpine,
                                       player);
        }
    }

    // Concludes the vote and records what should happen. The outcome handler
    // itself is deliberately NOT run here: it changes the level / kicks a player
    // / starts a match, any of which can re-enter the vote system. VoteMgr
    // detaches the vote first and then calls run_pending_outcome().
    void finish_vote(bool is_accepted, std::optional<AfVoteResult> result = std::nullopt)
    {
        early_finish_check_timer.invalidate();

        // The outcome TEXT is produced here rather than inside on_accepted() so the
        // end event can carry it. It only reads state the outcome handler has not
        // touched yet (both run in the same frame, back to back via conclude()).
        emit_vote_end(result.value_or(is_accepted ? AfVoteResult::Passed : AfVoteResult::Failed),
                      is_accepted, get_outcome_text(is_accepted));

        pending_outcome = is_accepted ? Outcome::Accepted : Outcome::Rejected;
    }

    static bool is_eligible_voter(rf::Player* const p) {
        if (!p) {
            return false;
        }
        if (is_listen_server_host(p)) {
            return false; // has no way to vote at all; see is_listen_server_host
        }
        if (p->version_info.software == ClientSoftware::Browser
            || p->is_bot
            || player_is_idle(p)
            || !player_meets_alpine_restrict(p)) {
            return false;
        }
        return true;
    }

    // Every count that is shown or acted on comes from here, so the announcement,
    // the live updates, the early-finish check and the timeout can never disagree.
    // (There used to be a separate count_eligible_voters() used only by the
    // announcement, which produced a different "waiting" number.)
    [[nodiscard]] VoteTally compute_tally() const
    {
        VoteTally tally;

        for (const auto& [player, is_yes] : players_who_voted) {
            if (!is_eligible_voter(player)) {
                continue;
            }
            if (is_yes) {
                ++tally.yes;
            }
            else {
                ++tally.no;
            }
        }

        for (auto* p : get_clients(false, false)) {
            if (is_eligible_voter(p) && players_who_voted.count(p) == 0) {
                ++tally.remaining;
            }
        }

        return tally;
    }

    bool check_for_early_vote_finish()
    {
        const VoteTally tally = compute_tally();

        const bool can_pass = tally.yes > tally.no + tally.remaining;
        const bool can_fail = tally.no > tally.yes + tally.remaining;
        const bool all_have_voted = tally.remaining == 0;

        if (can_pass) {
            finish_vote(true);
            return false;
        }
        if (can_fail) {
            finish_vote(false);
            return false;
        }
        if (all_have_voted) {
            finish_vote(tally.yes > tally.no);
            return false;
        }

        return true;
    }
};

static bool does_level_match_gametype_prefix(const std::string& level_name, rf::NetGameType game_type)
{
    const std::string map_name = normalize_level_filename(level_name);

    if (game_type == rf::NG_TYPE_RUN && is_known_run_level(level_name)) {
        return true;
    }

    // Accept any standard MP level.
    if (multi_game_type_uses_any_level(game_type)
        && multi_level_name_matches_any_mp_prefix(map_name.c_str())) {
        return true;
    }

    const auto base_prefix = multi_game_type_prefix(game_type);

    auto matches_prefix = [&](std::string_view prefix) {
        return string_istarts_with(map_name, prefix);
    };

    if (matches_prefix(base_prefix)) {
        return true;
    }

    if ((game_type == rf::NG_TYPE_DM || game_type == rf::NG_TYPE_TEAMDM) && matches_prefix("pdm")) {
        return true;
    }

    if ((game_type == rf::NG_TYPE_CTF || game_type == rf::NG_TYPE_SAL) && matches_prefix("pctf")) {
        return true;
    }

    return false;
}

// The union of the rotation and the vote-allowed list, in rotation order, with
// case-insensitive dedup. Also what the vote-options blob advertises.
static std::vector<std::string> build_votable_level_list()
{
    const auto& vote_level_cfg = g_alpine_server_config.vote_level;

    std::vector<std::string> levels;
    std::set<std::string> seen;

    const auto add = [&](const std::string& name) {
        if (name.empty()) {
            return;
        }
        if (seen.insert(string_to_lower(name)).second) {
            levels.push_back(name);
        }
    };

    for (const auto& level_entry : g_alpine_server_config.levels) {
        add(level_entry.level_filename);
    }
    for (const auto& allowed : vote_level_cfg.allowed_maps) {
        add(allowed);
    }

    return levels;
}

// The game type the voted level will actually run with, mirroring
// load_vote_rules_override's inheritance: a voted game type wins; otherwise a
// vote that builds an override rebases onto the level's natural rules, and a
// vote that builds no override leaves the level running exactly as it is.
static rf::NetGameType resolve_effective_vote_game_type(const std::string& level_name,
                                                        std::optional<rf::NetGameType> gametype,
                                                        bool builds_override, bool keeps_current_level)
{
    if (gametype) {
        return *gametype;
    }
    if (!builds_override && keeps_current_level) {
        return g_alpine_server_config_active_rules.game_type;
    }
    return vote_natural_rules_for_level(level_name).game_type;
}

// Enforcement-aware answer to "would the server accept this level voted with this
// game type?" — the call-time validation gate. A server with
// only_allow_gametype_prefix off accepts everything.
static bool is_level_valid_for_vote_gametype(const std::string& level_name, rf::NetGameType game_type)
{
    if (!g_alpine_server_config.vote_level.only_allow_gametype_prefix) {
        return true;
    }
    return does_level_match_gametype_prefix(level_name, game_type);
}

// bit N (matching NetGameType N) = this level matches that game type's level
// prefix rules. Deliberately NOT enforcement-aware: the mask always describes
// prefix-match validity so a client can offer an opt-in "filter by gametype" view
// even on a server that doesn't enforce it. Whether the server actually enforces
// the rules is advertised separately via AF_VOTE_SERVER_FLAG_GAMETYPE_PREFIX.
static uint32_t build_level_valid_gametype_mask(const std::string& level_name)
{
    uint32_t mask = 0;
    for (int i = 0; i <= static_cast<int>(rf::NG_TYPE_SAL); ++i) {
        if (does_level_match_gametype_prefix(level_name, static_cast<rf::NetGameType>(i))) {
            mask |= (1u << i);
        }
    }
    return mask;
}

// The allow-list arm of vote validation, as a pure predicate: is this level one
// the server permits votes for at all, independent of game type? Note that a level
// merely being in the ROTATION is not enough — with a non-empty allowed_maps and
// add_rotation_to_allowed_levels off (the default) rotation levels are refused.
// The blob advertises the answer per level so a client can filter its map list
// instead of offering votes the server will reject.
static bool is_level_in_vote_allow_list(const std::string& level_name)
{
    const auto& vote_level_cfg = g_alpine_server_config.vote_level;

    if (vote_level_cfg.allowed_maps.empty() && !vote_level_cfg.add_rotation_to_allowed_levels) {
        return true; // no allowed_levels configured and not adding rotation, so all levels are allowed
    }

    const auto matches = [&](const std::string& allowed_name) {
        return string_iequals(allowed_name, level_name);
    };

    if (std::any_of(vote_level_cfg.allowed_maps.begin(), vote_level_cfg.allowed_maps.end(), matches)) {
        return true;
    }

    if (vote_level_cfg.add_rotation_to_allowed_levels) {
        for (const auto& level_entry : g_alpine_server_config.levels) {
            if (matches(level_entry.level_filename)) {
                return true;
            }
        }
    }

    // allowed_maps empty but add_rotation_to_allowed_levels on and the rotation is
    // empty too: nothing was configured, so everything is allowed.
    return vote_level_cfg.allowed_maps.empty() && g_alpine_server_config.levels.empty();
}

static bool is_level_allowed_for_vote(const std::string& level_name, rf::Player* source,
                                      rf::NetGameType effective_game_type)
{
    // Checked against the game type the level will run with once the vote
    // applies, not the one currently running.
    if (!is_level_valid_for_vote_gametype(level_name, effective_game_type)) {
        auto msg = std::format("Cannot start vote: level {} does not match the {} gametype!", level_name,
                               multi_game_type_name_short(effective_game_type));
        send_vote_reject_msg(msg, source);
        return false; // level does not match gametype prefix
    }

    if (!is_level_in_vote_allow_list(level_name)) {
        auto msg = std::format("Cannot start vote: the server does not allow voting for level {}!", level_name);
        send_vote_reject_msg(msg, source);
        return false; // level not in allowed_levels
    }

    return true;
}

// "(GAMETYPE)" / "[MUTATOR]" suffixes appended to level and match vote titles.
static std::string build_rules_title_suffix(std::optional<rf::NetGameType> gametype,
                                            const std::string& mutator_labels)
{
    std::string suffix;
    if (gametype) {
        suffix += std::format(" ({})", multi_game_type_name_short(*gametype));
    }
    if (!mutator_labels.empty()) {
        suffix += std::format(" [{}]", mutator_labels);
    }
    return suffix;
}

struct VoteMatch : public Vote
{
    int m_team_size;
    std::string m_level_name;
    std::optional<rf::NetGameType> m_gametype;
    std::vector<MutatorDeclaration> m_mutators;
    std::optional<ManualRulesOverride> m_manual_rules_override;
    std::string m_mutator_labels;

    VoteMatch(int team_size, std::string level_name, std::optional<rf::NetGameType> gametype,
              std::vector<MutatorDeclaration> mutators)
        : m_team_size(team_size), m_level_name(std::move(level_name)), m_gametype(gametype),
          m_mutators(std::move(mutators))
    {}

    VoteType get_type() const override
    {
        return VoteType::Match;
    }

    bool validate(rf::Player* source) override
    {
        if (m_team_size < 1 || m_team_size > 8) {
            send_vote_reject_msg("Invalid match size! Supported sizes are 1v1 up to 8v8.", source);
            return false;
        }

        if (m_level_name.empty()) {
            m_level_name = rf::level.filename.c_str();
        }
        else {
            auto [is_valid, normalized_name] = is_level_name_valid(m_level_name);
            if (!is_valid) {
                send_vote_reject_msg(
                    "Invalid level specified! Try again, or omit level filename to use the current level.", source);
                return false;
            }
            m_level_name = std::move(normalized_name);
        }

        // A match on the current level with no rules override keeps the level
        // (and therefore its active rules) exactly as they are; anything else
        // re-resolves from the level's natural rules. Level names are compared
        // case-insensitively: they come from a client packet, a config file and the
        // engine, none of which agree on case.
        const bool builds_override = !m_mutators.empty() || m_gametype.has_value();
        const bool using_current_level = string_iequals(m_level_name, rf::level.filename.c_str());
        const rf::NetGameType effective_game_type = resolve_effective_vote_game_type(
            m_level_name, m_gametype, builds_override, using_current_level);

        if (!is_level_allowed_for_vote(m_level_name, source, effective_game_type)) {
            return false;
        }

        if (!multi_game_type_is_team_type(g_alpine_server_config.base_rules.game_type)) {
            send_vote_reject_msg("Cannot start vote: server base game type is not a team game type.", source);
            return false;
        }

        // The match must end up on a team game type — evaluated against what will
        // actually apply, not the base rules.
        if (!multi_game_type_is_team_type(effective_game_type)) {
            send_vote_reject_msg("Cannot start vote: matches must be played on a team game type.", source);
            return false;
        }

        m_manual_rules_override = load_vote_rules_override(m_level_name, m_mutators, m_gametype);
        m_mutator_labels = mutators_join_labels(m_mutators);

        // Deliberately does NOT touch g_match_info: validation passing only means
        // the vote may be PUT, not that it wins. Writing team_size /
        // match_level_name here left them pointing at a match nobody agreed to for
        // every vote that was voted down. on_accepted() publishes them instead.
        return true;
    }

    [[nodiscard]] std::string get_title() const override
    {
        return std::format("START {}v{} MATCH on {}{}", m_team_size, m_team_size, m_level_name,
                           build_rules_title_suffix(m_gametype, m_mutator_labels));
    }

    // How the accepted match will actually start. Computed by both the outcome text
    // and the outcome action; the two run back to back on unchanged state.
    struct MatchStartPlan
    {
        bool level_is_current = false;
        bool using_current_level = false;
    };

    [[nodiscard]] MatchStartPlan plan_match_start() const
    {
        MatchStartPlan plan;
        // m_level_name, not g_match_info.match_level_name: this runs before
        // on_accepted() publishes the match state, and it is the vote's own level
        // that matters anyway.
        plan.level_is_current = string_iequals(m_level_name, rf::level.filename.c_str());

        bool game_type_matches_current = true;
        if (plan.level_is_current) {
            const rf::NetGameType desired_game_type =
                m_manual_rules_override ? m_manual_rules_override->rules.game_type
                                        : g_alpine_server_config_active_rules.game_type;
            game_type_matches_current = (desired_game_type == rf::netgame.type);
        }

        plan.using_current_level = plan.level_is_current && game_type_matches_current;
        return plan;
    }

    [[nodiscard]] std::string get_outcome_text(bool accepted) const override
    {
        if (!accepted) {
            return Vote::get_outcome_text(false);
        }

        const MatchStartPlan plan = plan_match_start();
        const char* detail = plan.using_current_level
                                 ? "Entering pre-match ready up phase"
                                 : plan.level_is_current
                                       ? "Restarting level to apply match game type, then entering pre-match ready up phase"
                                       : "Changing to match level, then entering pre-match ready up phase";

        if (!m_mutator_labels.empty()) {
            return std::format("Vote passed. {} (mutators: {}).", detail, m_mutator_labels);
        }
        return std::format("Vote passed. {}.", detail);
    }

    void on_accepted() override
    {
        const bool using_current_level = plan_match_start().using_current_level;

        // Publish the match state only now that the vote has actually passed.
        g_match_info.team_size = m_team_size;
        g_match_info.match_level_name = m_level_name;
        g_match_info.pre_match_queued = true;

        if (using_current_level) {
            if (m_manual_rules_override) {
                set_manual_rules_override(std::move(*m_manual_rules_override));
                apply_rules_for_current_level();
                // Clients need the new mutator flags now, not at the match-start restart.
                af_send_server_info_packet_to_all();
                m_manual_rules_override.reset();
            }
            start_pre_match();
        }
        else if (!m_level_name.empty()) {
            if (!m_manual_rules_override)
                clear_manual_rules_override();
            if (m_gametype)
                set_upcoming_game_type(*m_gametype, UpcomingGameTypeSelection::ExplicitRequest);
            multi_change_level_alpine(m_level_name.c_str());
            if (m_manual_rules_override) {
                set_manual_rules_override(std::move(*m_manual_rules_override));
                m_manual_rules_override.reset();
            }
        }
    }

    [[nodiscard]] const VoteConfig& get_config() const override
    {
        return g_alpine_server_config.vote_match;
    }
};

struct VoteCancelMatch : public Vote
{
    VoteType get_type() const override
    {
        return VoteType::CancelMatch;
    }

    [[nodiscard]] std::string get_title() const override
    {
        return "CANCEL CURRENT MATCH";
    }

    bool validate(rf::Player* source) override
    {
        if (!g_match_info.match_active && !g_match_info.pre_match_active) {
            send_vote_reject_msg("No active or queued match to cancel.", source);
            return false;
        }

        return true;
    }

    [[nodiscard]] std::string get_outcome_text(bool accepted) const override
    {
        return accepted ? "Vote passed: The match has been canceled." : Vote::get_outcome_text(false);
    }

    void on_accepted() override
    {
        cancel_match();
    }

    [[nodiscard]] const VoteConfig& get_config() const override
    {
        return g_alpine_server_config.vote_match;
    }
};


struct VoteKick : public Vote
{
    rf::Player* m_target_player;
    // Captured once, at construction, and used instead of m_target_player for the
    // kick itself: the outcome runs a frame later, by which time the pointer could
    // name a destroyed player. -1 means "never resolvable".
    int m_target_player_id = -1;

    explicit VoteKick(rf::Player* target)
        : m_target_player(target),
          m_target_player_id(target && target->net_data ? static_cast<int>(target->net_data->player_id) : -1)
    {}

    VoteType get_type() const override
    {
        return VoteType::Kick;
    }

    bool validate(rf::Player* source) override
    {
        if (!m_target_player || m_target_player_id < 0) {
            send_vote_reject_msg("Cannot start vote: that player is no longer on the server.", source);
            return false;
        }
        // A kick target arrives as a raw player id, so it can name things the chat
        // path could never reach by name.
        if (is_listen_server_host(m_target_player)) {
            send_vote_reject_msg("Cannot start vote: the server host cannot be kicked.", source);
            return false;
        }
        if (m_target_player->is_browser) {
            send_vote_reject_msg("Cannot start vote: that connection is a server browser, not a player.",
                                       source);
            return false;
        }
        // Self-kick is disallowed: it is a no-op the caller can already do by
        // disconnecting, it passes instantly (the caller's own yes vote decides it),
        // and it matches the `kick` console command's "You cannot kick yourself!".
        if (m_target_player == source) {
            send_vote_reject_msg("Cannot start vote: you cannot vote to kick yourself.", source);
            return false;
        }
        return true;
    }

    [[nodiscard]] std::string get_title() const override
    {
        return std::format("KICK PLAYER '{}'", m_target_player->name);
    }

    [[nodiscard]] std::string get_outcome_text(bool accepted) const override
    {
        return accepted ? "Vote passed: kicking player" : Vote::get_outcome_text(false);
    }

    void on_accepted() override
    {
        // NEVER destroy a player from a vote outcome. on_accepted() normally runs
        // inside the engine's packet receive loop (0x004791F0) — the deciding vote
        // arrives as a packet — and that loop caches player_list->next BEFORE
        // dispatching each packet. rf::multi_kick_player unlinks the target, frees its
        // net_data and deletes the rf::Player, so if the target happened to be the
        // cached successor the loop then reads net_data off freed memory on its next
        // iteration: access violation at 0x00479299. (The engine re-checks only the
        // player it is currently servicing, via 0x004A4E10.)
        //
        // Queue by player id — not a pointer — and let process_delayed_kicks() in
        // server_do_frame() do it once packet dispatch has unwound.
        if (m_target_player_id < 0) {
            return;
        }
        if (rf::Player* target = rf::multi_find_player_by_id(static_cast<uint8_t>(m_target_player_id))) {
            kick_player_delayed(target);
        }
    }

    bool on_player_leave(rf::Player* player) override
    {
        if (m_target_player == player) {
            // The player being voted on left, so the vote is moot. Announce it like
            // any other cancellation rather than letting the vote vanish with no
            // explanation (legacy clients used to be told nothing at all here).
            // m_target_player is not dereferenced: it may already be destroyed.
            emit_vote_end(AfVoteResult::Canceled, false, "Vote canceled: the player left the game!");
            return false;
        }
        return Vote::on_player_leave(player);
    }

    [[nodiscard]] const VoteConfig& get_config() const override
    {
        return g_alpine_server_config.vote_kick;
    }
};

struct VoteExtend : public Vote
{
    // Minutes to add.
    int m_minutes;

    explicit VoteExtend(int minutes) : m_minutes(minutes) {}

    VoteType get_type() const override
    {
        return VoteType::Extend;
    }

    [[nodiscard]] std::string get_title() const override
    {
        return std::format("EXTEND ROUND BY {} {}", m_minutes, m_minutes == 1 ? "MINUTE" : "MINUTES");
    }

    [[nodiscard]] std::string get_outcome_text(bool accepted) const override
    {
        if (!accepted) {
            return Vote::get_outcome_text(false);
        }
        return std::format("Vote passed: extending round by {} {}", m_minutes,
                           m_minutes == 1 ? "minute" : "minutes");
    }

    void on_accepted() override
    {
        // extend_round_time takes MINUTES.
        extend_round_time(m_minutes);
    }

    [[nodiscard]] const VoteConfig& get_config() const override
    {
        return g_alpine_server_config.vote_extend;
    }
};

struct VoteLevel : public Vote
{
    std::string m_level_name;
    std::optional<rf::NetGameType> m_gametype;
    std::vector<MutatorDeclaration> m_mutators;
    std::optional<ManualRulesOverride> m_manual_rules_override;
    std::string m_mutator_labels;

    VoteLevel(std::string level_name, std::optional<rf::NetGameType> gametype,
              std::vector<MutatorDeclaration> mutators)
        : m_level_name(std::move(level_name)), m_gametype(gametype), m_mutators(std::move(mutators))
    {}

    VoteType get_type() const override
    {
        return VoteType::Level;
    }

    bool validate(rf::Player* source) override
    {
        auto [is_valid, level_name] = is_level_name_valid(m_level_name);

        if (!is_valid) {
            auto msg = std::format("Cannot start vote: level {} is not available on the server!", level_name);
            send_vote_reject_msg(msg, source);
            return false;
        }

        m_level_name = std::move(level_name);

        // A level vote always reloads the level, so it never keeps the currently
        // active rules — the target always re-resolves from rotation/base.
        const bool builds_override = !m_mutators.empty() || m_gametype.has_value();
        const rf::NetGameType effective_game_type = resolve_effective_vote_game_type(
            m_level_name, m_gametype, builds_override, /*keeps_current_level*/ false);

        if (!is_level_allowed_for_vote(m_level_name, source, effective_game_type)) {
            return false;
        }

        m_manual_rules_override = load_vote_rules_override(m_level_name, m_mutators, m_gametype);
        m_mutator_labels = mutators_join_labels(m_mutators);
        return true;
    }

    [[nodiscard]] std::string get_title() const override
    {
        return std::format("LOAD LEVEL '{}'{}", m_level_name,
                           build_rules_title_suffix(m_gametype, m_mutator_labels));
    }

    [[nodiscard]] std::string get_outcome_text(bool accepted) const override
    {
        if (!accepted) {
            return Vote::get_outcome_text(false);
        }
        return std::format("Vote passed: changing level to {}{}", m_level_name,
                           build_rules_title_suffix(m_gametype, m_mutator_labels));
    }

    void on_accepted() override
    {
        clear_manual_rules_override();

        if (m_gametype) {
            set_upcoming_game_type(*m_gametype, UpcomingGameTypeSelection::ExplicitRequest);
        }

        multi_change_level_alpine(m_level_name.c_str());

        if (m_manual_rules_override) {
            set_manual_rules_override(std::move(*m_manual_rules_override));
            m_manual_rules_override.reset();
        }
    }

    [[nodiscard]] const VoteConfig& get_config() const override
    {
        return g_alpine_server_config.vote_level;
    }
};

// Shared by the four rotation votes (restart / next / random / previous), which
// can carry the session's vote-set rules onto the level they load.
struct VoteRotation : public Vote
{
    bool m_preserve;
    std::vector<MutatorDeclaration> m_carried_mutators;
    std::optional<rf::NetGameType> m_carried_gametype;
    std::string m_carried_labels;

    explicit VoteRotation(bool preserve) : m_preserve(preserve) {}

    [[nodiscard]] bool carries_anything() const
    {
        return !m_carried_mutators.empty() || m_carried_gametype.has_value();
    }

    bool validate([[maybe_unused]] rf::Player* source) override
    {
        // Preserve means "keep the overrides a vote set for this session", not
        // "propagate whatever is running": with no session override in play it is
        // a no-op, so a default-checked Next vote cannot stamp this level's
        // configured game type onto the operator's next rotation entry.
        if (!m_preserve || !g_manual_rules_override) {
            return true;
        }

        const auto& active = g_alpine_server_config_active_rules;
        m_carried_mutators = active.mutators.declarations;
        // Only carry a game type that actually deviates from what this level runs
        // on its own; otherwise the target level's own configured type must win.
        if (active.game_type != vote_natural_rules_for_level(rf::level.filename.c_str()).game_type) {
            m_carried_gametype = active.game_type;
        }
        m_carried_labels = mutators_join_labels(m_carried_mutators);
        return true;
    }

    [[nodiscard]] std::string carry_suffix() const
    {
        if (!carries_anything()) {
            return {};
        }
        return build_rules_title_suffix(m_carried_gametype, m_carried_labels);
    }

    // Rotation loads advance the cursor through a non-manual level change, which
    // ignores g_manual_rules_override, so what is carried goes into the one-shot
    // stash apply_rules_for_current_level consumes.
    void stash_carry() const
    {
        if (!carries_anything()) {
            return;
        }
        PendingRotationPreserve pending;
        pending.declarations = m_carried_mutators;
        pending.gametype = m_carried_gametype;
        set_pending_rotation_preserve(std::move(pending));

        if (m_carried_gametype) {
            set_upcoming_game_type(*m_carried_gametype, UpcomingGameTypeSelection::ExplicitRequest);
        }
    }
};

struct VoteRestart : public VoteRotation
{
    using VoteRotation::VoteRotation;

    VoteType get_type() const override
    {
        return VoteType::Restart;
    }

    [[nodiscard]] std::string get_title() const override
    {
        return std::format("RESTART LEVEL{}", carry_suffix());
    }

    [[nodiscard]] std::string get_outcome_text(bool accepted) const override
    {
        if (!accepted) {
            return Vote::get_outcome_text(false);
        }
        return std::format("Vote passed: restarting level{}", carry_suffix());
    }

    void on_accepted() override
    {
        // restart_current_level() round-trips the session override itself, so the
        // stash is not needed here — only the configured-rules reload is new.
        if (m_preserve) {
            restart_current_level();
        }
        else {
            restart_current_level_configured();
        }
    }

    [[nodiscard]] const VoteConfig& get_config() const override
    {
        return g_alpine_server_config.vote_restart;
    }
};

struct VoteNext : public VoteRotation
{
    using VoteRotation::VoteRotation;

    VoteType get_type() const override
    {
        return VoteType::Next;
    }

    [[nodiscard]] std::string get_title() const override
    {
        return std::format("LOAD NEXT LEVEL{}", carry_suffix());
    }

    [[nodiscard]] std::string get_outcome_text(bool accepted) const override
    {
        if (!accepted) {
            return Vote::get_outcome_text(false);
        }
        return std::format("Vote passed: loading next level{}", carry_suffix());
    }

    void on_accepted() override
    {
        stash_carry();
        load_next_level();
    }

    [[nodiscard]] const VoteConfig& get_config() const override
    {
        return g_alpine_server_config.vote_next;
    }
};

struct VoteRandom : public VoteRotation
{
    using VoteRotation::VoteRotation;

    VoteType get_type() const override
    {
        return VoteType::Random;
    }

    [[nodiscard]] std::string get_title() const override
    {
        return std::format("LOAD RANDOM LEVEL{}", carry_suffix());
    }

    [[nodiscard]] std::string get_outcome_text(bool accepted) const override
    {
        if (!accepted) {
            return Vote::get_outcome_text(false);
        }
        return std::format("Vote passed: loading random level from rotation{}", carry_suffix());
    }

    void on_accepted() override
    {
        stash_carry();
        // if dynamic rotation is on, just load the next level
        g_alpine_server_config.dynamic_rotation ? load_next_level() : load_rand_level();
    }

    [[nodiscard]] const VoteConfig& get_config() const override
    {
        return g_alpine_server_config.vote_rand;
    }
};

struct VotePrevious : public VoteRotation
{
    using VoteRotation::VoteRotation;

    VoteType get_type() const override
    {
        return VoteType::Previous;
    }

    [[nodiscard]] std::string get_title() const override
    {
        return std::format("LOAD PREV LEVEL{}", carry_suffix());
    }

    [[nodiscard]] std::string get_outcome_text(bool accepted) const override
    {
        if (!accepted) {
            return Vote::get_outcome_text(false);
        }
        return std::format("Vote passed: loading previous level{}", carry_suffix());
    }

    void on_accepted() override
    {
        stash_carry();
        load_prev_level();
    }

    [[nodiscard]] const VoteConfig& get_config() const override
    {
        return g_alpine_server_config.vote_previous;
    }
};

class VoteMgr
{
private:
    std::unique_ptr<Vote> active_vote;

    // A concluded vote's outcome handler changes the level, kicks a player or
    // starts a match — any of which can re-enter the vote system (a kick makes
    // the engine call server_vote_on_player_leave; a level change eventually
    // reaches server_vote_on_limbo_state_enter). Detaching the vote from the
    // manager BEFORE running its outcome means those re-entrant paths see no
    // active vote and can never destroy the object whose method is running.
    static void conclude(std::unique_ptr<Vote> vote)
    {
        if (!vote) {
            return;
        }
        // Guarantees exactly one end event on every path that drops a vote,
        // including the ones with no outcome (kick target left, limbo). Idempotent,
        // so a path that already emitted its own end event is unaffected.
        vote->broadcast_vote_end(AfVoteResult::Canceled, false, "Vote canceled!");
        vote->run_pending_outcome();
    }

public:
    template<typename T, typename... Args>
    bool StartVote(rf::Player* source, Args&&... args)
    {
        if (active_vote) {
            send_vote_reject_msg("Another vote is currently in progress!", source);
            return false;
        }

        auto vote = std::make_unique<T>(std::forward<Args>(args)...);

        if (!vote->get_config().enabled) {
            send_vote_reject_msg("This vote type is disabled!", source);
            return false;
        }

        // No vote of any type may be called between levels / at end of match: the
        // level it would act on is already gone, and OnLimboStateEnter would
        // cancel it immediately anyway.
        if (rf::gameseq_get_state() != rf::GS_GAMEPLAY) {
            send_vote_reject_msg("Votes cannot be called between levels. Try again once the next level starts.",
                                       source);
            return false;
        }

        if (vote->get_type() == VoteType::Match && (g_match_info.pre_match_active || g_match_info.match_active)) {
            send_vote_reject_msg(
                "A match is already queued or in progress. Finish it before starting a new one.", source);
            return false;
        }

        if (!vote->validate(source)) {
            return false;
        }

        // A vote that resolves immediately (sole eligible voter) never becomes
        // the active vote: start() reports it finished and conclude() runs the
        // outcome on the detached object.
        if (!vote->start(source)) {
            conclude(std::move(vote));
            return false;
        }

        active_vote = std::move(vote);
        return true;
    }

    void on_player_leave(rf::Player* player)
    {
        if (active_vote && !active_vote->on_player_leave(player)) {
            conclude(std::move(active_vote));
        }
    }

    void OnLimboStateEnter()
    {
        // Any active vote dies with the level it was called on, whatever its type
        // — a vote's premise (this level, these players) no longer holds after a
        // map change. Routed through conclude() so the exactly-once end event is
        // structural rather than something each drop site has to remember.
        if (active_vote) {
            std::unique_ptr<Vote> vote = std::move(active_vote);
            vote->cancel_for_limbo();
            conclude(std::move(vote));
        }
    }

    void add_player_vote(bool is_yes_vote, rf::Player* source)
    {
        if (!active_vote) {
            send_vote_reject_msg("No vote in progress!", source);
            return;
        }

        if (!active_vote->add_player_vote(is_yes_vote, source)) {
            conclude(std::move(active_vote));
        }
    }

    void try_cancel_vote(rf::Player* source)
    {
        if (!active_vote) {
            send_vote_reject_msg("No vote in progress!", source);
            return;
        }

        if (active_vote->try_cancel_vote(source)) {
            conclude(std::move(active_vote));
        }
    }

    void send_state_to_player(rf::Player* player)
    {
        if (active_vote) {
            active_vote->send_start_state_to(player);
        }
    }

    void do_frame()
    {
        if (!active_vote)
            return;

        if (!active_vote->do_frame()) {
            conclude(std::move(active_vote));
        }
    }
};

VoteMgr g_vote_mgr;

// ============================================================================
// Vote-options blob (server -> client schema of what can be voted for)
// ============================================================================

static std::vector<uint8_t> g_vote_options_blob;
// Wide enough that it can never wrap. A u8 wrapped every 256 rebuilds (a rotation
// shuffle bumps it once per cycle), and a client holding a stale blob at a
// recurring generation would then never refresh.
static uint32_t g_vote_options_generation = 0;
static bool g_vote_options_blob_valid = false;

static void blob_u8(std::vector<uint8_t>& blob, uint8_t value)
{
    blob.push_back(value);
}

static void blob_u16(std::vector<uint8_t>& blob, uint16_t value)
{
    blob.push_back(static_cast<uint8_t>(value & 0xFF));
    blob.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

static void blob_u32(std::vector<uint8_t>& blob, uint32_t value)
{
    uint8_t bytes[sizeof(value)];
    std::memcpy(bytes, &value, sizeof(value));
    blob.insert(blob.end(), bytes, bytes + sizeof(value));
}

static void blob_i32(std::vector<uint8_t>& blob, int32_t value)
{
    uint8_t bytes[sizeof(value)];
    std::memcpy(bytes, &value, sizeof(value));
    blob.insert(blob.end(), bytes, bytes + sizeof(value));
}

static void blob_f32(std::vector<uint8_t>& blob, float value)
{
    uint8_t bytes[sizeof(value)];
    std::memcpy(bytes, &value, sizeof(value));
    blob.insert(blob.end(), bytes, bytes + sizeof(value));
}

static void blob_str(std::vector<uint8_t>& blob, std::string_view value)
{
    const size_t len = std::min<size_t>(value.size(), 255);
    blob.push_back(static_cast<uint8_t>(len));
    blob.insert(blob.end(), value.begin(), value.begin() + len);
}

// Writes a record whose body is preceded by its own length, so a client that
// cannot make sense of the body (unknown option type, extra trailing fields a
// newer server added) can step over it and keep the rest of the blob intact.
// `write_body` appends the body; the length is patched in afterwards by INDEX,
// never through a pointer, because the body write reallocates the vector.
// EVERY repeated record in the blob goes through one of these two helpers.
template<typename WriteBody>
static void blob_sized_u8(std::vector<uint8_t>& blob, WriteBody&& write_body)
{
    const size_t len_pos = blob.size();
    blob.push_back(0);
    write_body();
    const size_t body_len = blob.size() - len_pos - 1;
    // A descriptor body that doesn't fit its length prefix would desync every
    // client, so truncate the body rather than lie about its length.
    if (body_len > 255) {
        xlog::error("vote options: descriptor body of {} bytes exceeds the u8 length prefix; truncating",
                    body_len);
        blob.resize(len_pos + 1 + 255);
        blob[len_pos] = 255;
        return;
    }
    blob[len_pos] = static_cast<uint8_t>(body_len);
}

template<typename WriteBody>
static void blob_sized_u16(std::vector<uint8_t>& blob, WriteBody&& write_body)
{
    const size_t len_pos = blob.size();
    blob.push_back(0);
    blob.push_back(0);
    write_body();
    const size_t body_len = blob.size() - len_pos - 2;
    if (body_len > 65535) {
        xlog::error("vote options: descriptor body of {} bytes exceeds the u16 length prefix; truncating",
                    body_len);
        blob.resize(len_pos + 2 + 65535);
        blob[len_pos] = 0xFF;
        blob[len_pos + 1] = 0xFF;
        return;
    }
    blob[len_pos] = static_cast<uint8_t>(body_len & 0xFF);
    blob[len_pos + 1] = static_cast<uint8_t>((body_len >> 8) & 0xFF);
}

// The stored variant alternative has to match the option's registry type before
// anything is written: a value the config parsed as a different TOML type would
// otherwise be encoded as whatever the emit switch below happens to read.
static bool declaration_value_matches_type(MutatorOptionType type, const MutatorOptionValue& value)
{
    switch (type) {
        case MutatorOptionType::Bool:
            return std::holds_alternative<bool>(value);
        case MutatorOptionType::Choice:
        case MutatorOptionType::String:
            return std::holds_alternative<std::string>(value);
        case MutatorOptionType::Int:
            return std::holds_alternative<int32_t>(value);
        case MutatorOptionType::Float:
            return std::holds_alternative<float>(value);
    }
    return false;
}

// A declaration stores a Choice option's VALUE string (the TOML spelling), while
// the schema sends the choice LABELS and every wire form carries an index, so the
// index has to be resolved here. A value that no longer names a choice (registry
// rebuilt against different weapon/item tables) falls back to the default.
static uint8_t declaration_choice_index(const MutatorOptionInfo& option, const std::string& value)
{
    const size_t choice_count = std::min<size_t>(option.choices.size(), 255);
    for (size_t c = 0; c < choice_count; ++c) {
        if (option.choices[c].value == value) {
            return static_cast<uint8_t>(c);
        }
    }
    xlog::debug("vote options: option '{}' has no choice '{}'; sending its default instead", option.name,
                value);
    return option.default_choice;
}

// One mutator declaration set: the config-declared mutators of a rules scope,
// in apply order, with their option values. The vote panel pre-selects this so
// an untouched submission reproduces what the level would run anyway.
static void blob_declaration_set(std::vector<uint8_t>& blob, const std::vector<MutatorDeclaration>& decls)
{
    // Resolved before anything is written: the count precedes the entries, so a
    // declaration the registry does not know has to be dropped before it is
    // counted rather than while writing.
    std::vector<std::pair<const MutatorInfo*, const MutatorDeclaration*>> resolved;
    resolved.reserve(decls.size());
    for (const MutatorDeclaration& decl : decls) {
        const MutatorInfo* info = mutators_find_by_name(decl.name);
        if (!info) {
            xlog::debug("vote options: declared mutator '{}' is not in the registry; omitting it", decl.name);
            continue;
        }
        resolved.push_back({info, &decl});
    }

    const size_t decl_count = std::min<size_t>(resolved.size(), 255);
    blob_u8(blob, static_cast<uint8_t>(decl_count));
    for (size_t d = 0; d < decl_count; ++d) {
        const MutatorInfo& info = *resolved[d].first;
        const MutatorDeclaration& decl = *resolved[d].second;
        // u16, not u8: a single 255-byte String option value plus the headers
        // around it already overflows a u8 body.
        blob_sized_u16(blob, [&] {
            blob_u8(blob, static_cast<uint8_t>(info.id));

            // Same reason as the declaration list above: the option count is
            // written before the options, so resolve them all first.
            struct ResolvedOption
            {
                const MutatorOptionInfo* option;
                const MutatorOptionValue* value;
            };
            std::vector<ResolvedOption> options;
            options.reserve(decl.options.size());
            for (const auto& [key, value] : decl.options) {
                const MutatorOptionInfo* option = nullptr;
                for (const MutatorOptionInfo& candidate : info.options) {
                    if (candidate.name == key) {
                        option = &candidate;
                        break;
                    }
                }
                if (!option) {
                    xlog::debug("vote options: mutator '{}' has no option '{}'; omitting it", info.name, key);
                    continue;
                }
                if (!declaration_value_matches_type(option->type, value)) {
                    xlog::debug("vote options: option '{}' of mutator '{}' holds a value of the wrong type; "
                                "omitting it", key, info.name);
                    continue;
                }
                options.push_back({option, &value});
            }

            const size_t option_count = std::min<size_t>(options.size(), 255);
            blob_u8(blob, static_cast<uint8_t>(option_count));
            for (size_t o = 0; o < option_count; ++o) {
                const MutatorOptionInfo& option = *options[o].option;
                const MutatorOptionValue& value = *options[o].value;
                blob_u8(blob, option.id);
                // Same typed encoding as the schema defaults above, so an option
                // type this client predates makes only that declaration
                // undecodable rather than the whole set.
                blob_u8(blob, static_cast<uint8_t>(option.type));
                switch (option.type) {
                    case MutatorOptionType::Bool:
                        blob_u8(blob, std::get<bool>(value) ? 1 : 0);
                        break;
                    case MutatorOptionType::Choice:
                        blob_u8(blob, declaration_choice_index(option, std::get<std::string>(value)));
                        break;
                    case MutatorOptionType::Int:
                        blob_i32(blob, std::get<int32_t>(value));
                        break;
                    case MutatorOptionType::Float:
                        blob_f32(blob, std::get<float>(value));
                        break;
                    case MutatorOptionType::String:
                        blob_str(blob, std::get<std::string>(value));
                        break;
                }
            }
        });
    }
}

// u32 rather than u16: 16 bits left only seven spare vote types, and widening it
// after release would be a breaking blob change. Same reasoning as
// build_level_valid_gametype_mask.
static uint32_t build_enabled_vote_mask()
{
    const auto& cfg = g_alpine_server_config;
    const auto bit = [](AfVoteType type) { return static_cast<uint32_t>(1u << static_cast<unsigned>(type)); };

    uint32_t mask = 0;
    if (cfg.vote_kick.enabled) mask |= bit(AfVoteType::Kick);
    if (cfg.vote_level.enabled) mask |= bit(AfVoteType::Level);
    if (cfg.vote_match.enabled) mask |= bit(AfVoteType::Match) | bit(AfVoteType::CancelMatch);
    if (cfg.vote_extend.enabled) mask |= bit(AfVoteType::Extend);
    if (cfg.vote_restart.enabled) mask |= bit(AfVoteType::Restart);
    if (cfg.vote_next.enabled) mask |= bit(AfVoteType::Next);
    if (cfg.vote_rand.enabled) mask |= bit(AfVoteType::Random);
    if (cfg.vote_previous.enabled) mask |= bit(AfVoteType::Previous);
    return mask;
}

static void build_vote_options_blob(std::vector<uint8_t>& blob)
{
    blob.clear();
    blob_u8(blob, af_vote_options_blob_version);
    blob_u32(blob, build_enabled_vote_mask());

    // server-wide vote flags
    uint8_t server_flags = 0;
    if (g_alpine_server_config.vote_level.only_allow_gametype_prefix) {
        server_flags |= AF_VOTE_SERVER_FLAG_GAMETYPE_PREFIX;
    }
    server_flags |= AF_VOTE_SERVER_FLAG_ROTATION_PRESERVE;
    blob_u8(blob, server_flags);

    // Game types. Length-prefixed per entry (u16, not u8: display_name alone can be
    // 255 bytes plus the fixed fields), so per-gametype data can be appended inside
    // the entry later without desyncing an older client.
    const int gametype_count = static_cast<int>(rf::NG_TYPE_SAL) + 1;
    blob_u8(blob, static_cast<uint8_t>(gametype_count));
    for (int i = 0; i < gametype_count; ++i) {
        const auto game_type = static_cast<rf::NetGameType>(i);
        blob_sized_u16(blob, [&] {
            blob_u8(blob, static_cast<uint8_t>(i));
            blob_u8(blob, multi_game_type_is_team_type(game_type) ? AF_VOTE_GAMETYPE_FLAG_TEAM : 0);
            blob_str(blob, multi_game_type_name(game_type));
        });
    }

    // Mutators. Every loop below iterates the CLAMPED count, not the container, so
    // the declared count always matches the number of entries written.
    //
    // Both descriptor levels are length-prefixed, which is what makes the schema
    // additive: a future mutator with an option type this client has never heard of
    // still parses, minus that option, and a mutator whose descriptor makes no
    // sense at all is skipped without desyncing the levels section behind it.
    const auto& registry = mutators_get_registry();
    const size_t mutator_count = std::min<size_t>(registry.size(), 255);
    blob_u8(blob, static_cast<uint8_t>(mutator_count));
    for (size_t m = 0; m < mutator_count; ++m) {
        const MutatorInfo& mutator = registry[m];
        blob_sized_u16(blob, [&] {
            blob_u8(blob, static_cast<uint8_t>(mutator.id));
            blob_str(blob, mutator.name);
            blob_str(blob, mutator.label);
            const size_t option_count = std::min<size_t>(mutator.options.size(), 255);
            blob_u8(blob, static_cast<uint8_t>(option_count));
            for (size_t o = 0; o < option_count; ++o) {
                const MutatorOptionInfo& option = mutator.options[o];
                blob_sized_u8(blob, [&] {
                    blob_u8(blob, option.id);
                    blob_str(blob, option.name);
                    blob_str(blob, option.label);
                    blob_u8(blob, static_cast<uint8_t>(option.type));
                    switch (option.type) {
                        case MutatorOptionType::Bool:
                            blob_u8(blob, option.default_bool ? 1 : 0);
                            break;
                        case MutatorOptionType::Choice:
                            blob_u8(blob, option.default_choice);
                            break;
                        case MutatorOptionType::Int:
                            blob_i32(blob, option.default_int);
                            break;
                        case MutatorOptionType::Float:
                            blob_f32(blob, option.default_float);
                            break;
                        case MutatorOptionType::String:
                            blob_str(blob, option.default_string);
                            break;
                    }
                    if (option.type == MutatorOptionType::Choice) {
                        const size_t choice_count = std::min<size_t>(option.choices.size(), 255);
                        blob_u8(blob, static_cast<uint8_t>(choice_count));
                        for (size_t c = 0; c < choice_count; ++c) {
                            blob_str(blob, option.choices[c].label);
                        }
                    }
                });
            }
            // game types this mutator can apply in.
            blob_u32(blob, mutator.valid_gametype_mask);
        });
    }

    // Votable levels: the game type each would run with by default, the set of
    // game types whose prefix rules it matches, and whether the server's
    // vote_level allow-list accepts it at all. Length-prefixed per entry (u16, not
    // u8: filename alone can be 255 bytes plus six fixed bytes), so per-level data
    // can be appended inside the entry later. Two bytes an entry buys in-place
    // extensibility on the one section that has no size ceiling.
    const std::vector<std::string> levels = build_votable_level_list();
    const size_t level_count = std::min<size_t>(levels.size(), 65535);
    blob_u16(blob, static_cast<uint16_t>(level_count));
    for (size_t i = 0; i < level_count; ++i) {
        const std::string& level = levels[i];
        blob_sized_u16(blob, [&] {
            blob_str(blob, level);
            blob_u8(blob, static_cast<uint8_t>(vote_natural_rules_for_level(level).game_type));
            blob_u32(blob, build_level_valid_gametype_mask(level));
            // Derived from the SAME predicate the call-time gate uses, so the blob can
            // never advertise a level the server would refuse.
            blob_u8(blob, is_level_in_vote_allow_list(level) ? AF_VOTE_LEVEL_FLAG_ALLOWED : 0);

            // Which mutator set the panel pre-selects for this level. A rotation
            // entry's rules start life as a copy of the base rules, so an entry
            // that declares nothing of its own compares equal to the base set and
            // simply inherits it; an entry whose set differs -- including one that
            // deliberately clears it -- carries its own.
            const std::vector<MutatorDeclaration>* level_decls = nullptr;
            for (const auto& entry : g_alpine_server_config.levels) {
                // Same lookup as vote_natural_rules_for_level: first match wins.
                if (string_iequals(entry.level_filename, level)) {
                    level_decls = &entry.rule_overrides.mutators.declarations;
                    break;
                }
            }
            const auto& base_decls = g_alpine_server_config.base_rules.mutators.declarations;
            if (!level_decls || *level_decls == base_decls) {
                blob_u8(blob, static_cast<uint8_t>(AfVoteLevelBaseline::InheritBase));
            }
            else {
                blob_u8(blob, static_cast<uint8_t>(AfVoteLevelBaseline::Explicit));
                blob_declaration_set(blob, *level_decls);
            }
        });
    }

    // The base mutator set, as a trailing section: every level that inherits
    // (kind 0 above) pre-selects this, and so does a manually named level outside
    // the rotation.
    blob_declaration_set(blob, g_alpine_server_config.base_rules.mutators.declarations);
}

void server_vote_invalidate_options_blob()
{
    g_vote_options_blob_valid = false;
}

const std::vector<uint8_t>& server_vote_get_options_blob(uint32_t& generation)
{
    if (!g_vote_options_blob_valid) {
        build_vote_options_blob(g_vote_options_blob);
        ++g_vote_options_generation; // starts at 1, so 0 means "nothing sent yet"
        g_vote_options_blob_valid = true;
        xlog::debug("vote options: rebuilt blob ({} bytes, generation {})",
                    g_vote_options_blob.size(), g_vote_options_generation);
    }
    generation = g_vote_options_generation;
    return g_vote_options_blob;
}

// Floor rate limit on af_req_vote_options. Applies to every request, accepted or
// not, so the packet itself costs a bounded amount of work.
static constexpr int vote_options_req_floor_ms = 1000;
// Longer window for a re-send of a generation this player already received. The
// only legitimate reason to ask again is a stream that never completed (the
// deferred reliable queue is cleared wholesale by af_send_server_cfg), so
// recovery stays possible without letting a client pull the blob on a 1s loop.
static constexpr int vote_options_req_repeat_ms = 10000;

void server_vote_handle_options_request(rf::Player* sender, bool has_cache, uint32_t known_generation)
{
    if (!rf::is_server || !sender) {
        return;
    }

    if (sender->vote_options_req_timer.valid() && !sender->vote_options_req_timer.elapsed()) {
        return; // still inside the floor window
    }

    // Do NOT build the blob for a client that cannot receive it: a rebuild bumps
    // the generation, which would invalidate every other client's cache.
    if (!is_player_minimum_af_client_version(sender, 1, 4, 0)) {
        sender->vote_options_req_timer.set(vote_options_req_floor_ms);
        return;
    }

    uint32_t generation = 0;
    server_vote_get_options_blob(generation);

    // The client told us what it already has. This is the case a plain
    // once-per-generation guard cannot distinguish: a cfg change that does not
    // affect the vote schema (sv_netfps, maxfps) sets signal_cfg_changed, which
    // marks every client's cache stale, but leaves the generation alone. Answering
    // with silence is what stops a needless identical re-download.
    if (has_cache && known_generation == generation) {
        sender->vote_options_sent_generation = generation;
        sender->vote_options_req_timer.set(vote_options_req_floor_ms);
        return;
    }

    const bool repeat = sender->vote_options_sent_generation == generation;
    sender->vote_options_req_timer.set(repeat ? vote_options_req_repeat_ms : vote_options_req_floor_ms);
    if (repeat) {
        xlog::debug("vote options: re-streaming generation {} to {} (their copy did not arrive)",
                    generation, sender->name);
    }

    af_send_vote_options_data(sender);
    sender->vote_options_sent_generation = generation;
}

// ============================================================================
// Entry points
// ============================================================================

// Shared gate for chat and packet vote actions.
static bool check_voter_eligibility(rf::Player* sender)
{
    if (is_listen_server_host(sender)) {
        return false; // silently: the host has no vote UI to reply to
    }
    if (sender->version_info.software == ClientSoftware::Browser || sender->is_bot) {
        send_vote_reject_msg("Browsers and bots are not allowed to vote!", sender, true);
        return false;
    }
    if (!Vote::player_meets_alpine_restrict(sender)) {
        send_vote_reject_msg(
            "You can't vote, because your client does not meet the server's requirements. Visit alpinefaction.com to upgrade.",
            sender, true
        );
        return false;
    }
    if (player_is_idle(sender)) {
        send_vote_reject_msg("Idle players are not allowed to vote!", sender, true);
        return false;
    }
    return true;
}

// Chat is a legacy path: 1.4+ clients call, cast and cancel with packets. All that
// survives here is casting, for clients too old to have af_req_vote_cast.
bool handle_vote_command(std::string_view vote_name, rf::Player* sender)
{
    const bool is_yes = vote_name == "yes" || vote_name == "y";
    if (!is_yes && vote_name != "no" && vote_name != "n") {
        return false;
    }

    // Recognized either way: an ineligible voter has already been told why.
    if (check_voter_eligibility(sender)) {
        g_vote_mgr.add_player_vote(is_yes, sender);
    }
    return true;
}

// A packet-supplied level name reaches both c_str() paths (is_level_name_valid,
// the engine level load) and std::string comparisons (rotation lookup, the
// allow-list). An embedded NUL makes those two disagree — "dm02.rfl\0x" passes
// the checksum check on the truncated name but misses the rotation lookup — so
// reject anything that isn't a plain filename before it goes any further.
static bool is_vote_level_string_sane(std::string_view level)
{
    if (level.size() > 128) {
        return false;
    }
    if (level.find("..") != std::string_view::npos) {
        return false;
    }
    for (const unsigned char c : level) {
        if (c < 0x20 || c == 0x7F) {
            return false; // NUL and control bytes
        }
        if (c == '/' || c == '\\' || c == ':') {
            return false; // path traversal / drive-relative
        }
    }
    return true;
}

// 0xFF means "server default rules"; anything else must name a real game type.
static bool resolve_vote_gametype(uint8_t wire_value, rf::Player* sender, std::optional<rf::NetGameType>& out)
{
    if (wire_value == af_vote_gametype_none) {
        out = std::nullopt;
        return true;
    }
    if (wire_value > static_cast<uint8_t>(rf::NG_TYPE_SAL)) {
        send_vote_reject_msg("Cannot start vote: this server does not support that game type.", sender);
        return false;
    }
    out = static_cast<rf::NetGameType>(wire_value);
    return true;
}

static bool resolve_vote_mutators(const std::vector<VoteMutatorInput>& input, rf::Player* sender,
                                  std::vector<MutatorDeclaration>& out)
{
    if (auto error = mutators_build_declarations_from_vote(input, out)) {
        send_vote_reject_msg(std::format("Cannot start vote: {}", *error), sender);
        return false;
    }
    return true;
}

void handle_vote_call_packet(rf::Player* sender, AfVoteCallParams&& params)
{
    if (!rf::is_server || !sender) {
        return;
    }

    if (!check_voter_eligibility(sender)) {
        return;
    }

    // Sanitize the level string once, before any type-specific handling.
    if ((params.type == AfVoteType::Level || params.type == AfVoteType::Match)
        && !is_vote_level_string_sane(params.level)) {
        send_vote_reject_msg("Cannot start vote: that level name is not valid.", sender);
        return;
    }

    switch (params.type) {
        case AfVoteType::Kick: {
            // The id is a parameter, not an identity claim; the sender is always
            // resolved from the packet source address.
            rf::Player* target = rf::multi_find_player_by_id(params.target_player_id);
            g_vote_mgr.StartVote<VoteKick>(sender, target);
            break;
        }
        case AfVoteType::Level: {
            std::optional<rf::NetGameType> gametype;
            if (!resolve_vote_gametype(params.gametype, sender, gametype)) {
                return;
            }
            std::vector<MutatorDeclaration> mutators;
            if (!resolve_vote_mutators(params.mutators, sender, mutators)) {
                return;
            }
            if (params.level.empty()) {
                send_vote_reject_msg("Cannot start vote: no level was specified.", sender);
                return;
            }
            g_vote_mgr.StartVote<VoteLevel>(sender, std::move(params.level), gametype, std::move(mutators));
            break;
        }
        case AfVoteType::Match: {
            std::optional<rf::NetGameType> gametype;
            if (!resolve_vote_gametype(params.gametype, sender, gametype)) {
                return;
            }
            std::vector<MutatorDeclaration> mutators;
            if (!resolve_vote_mutators(params.mutators, sender, mutators)) {
                return;
            }
            g_vote_mgr.StartVote<VoteMatch>(sender, static_cast<int>(params.team_size),
                                            std::move(params.level), gametype, std::move(mutators));
            break;
        }
        case AfVoteType::Extend:
            if (params.extend_minutes < af_vote_extend_min_minutes
                || params.extend_minutes > af_vote_extend_max_minutes) {
                send_vote_reject_msg(
                    std::format("Cannot start vote: the round can only be extended by {} to {} minutes.",
                                static_cast<int>(af_vote_extend_min_minutes),
                                static_cast<int>(af_vote_extend_max_minutes)),
                    sender);
                return;
            }
            g_vote_mgr.StartVote<VoteExtend>(sender, static_cast<int>(params.extend_minutes));
            break;
        case AfVoteType::Restart:
            g_vote_mgr.StartVote<VoteRestart>(sender, params.preserve);
            break;
        case AfVoteType::Next:
            g_vote_mgr.StartVote<VoteNext>(sender, params.preserve);
            break;
        case AfVoteType::Random:
            g_vote_mgr.StartVote<VoteRandom>(sender, params.preserve);
            break;
        case AfVoteType::Previous:
            g_vote_mgr.StartVote<VotePrevious>(sender, params.preserve);
            break;
        case AfVoteType::CancelMatch:
            g_vote_mgr.StartVote<VoteCancelMatch>(sender);
            break;
        default:
            xlog::warn("handle_vote_call_packet: unhandled vote type {}", static_cast<int>(params.type));
            break;
    }
}

void handle_vote_cast_packet(rf::Player* sender, bool is_yes_vote)
{
    if (!rf::is_server || !sender) {
        return;
    }
    if (!check_voter_eligibility(sender)) {
        return;
    }
    g_vote_mgr.add_player_vote(is_yes_vote, sender);
}

void handle_vote_cancel_packet(rf::Player* sender)
{
    if (!rf::is_server || !sender) {
        return;
    }
    // Same eligibility gate as the call and cast packets. try_cancel_vote already
    // rejects anyone who isn't the owner, so this only matters for an owner who has
    // since become ineligible, but the entry points must not diverge.
    if (!check_voter_eligibility(sender)) {
        return;
    }
    g_vote_mgr.try_cancel_vote(sender);
}

void server_vote_send_state_to_new_player(rf::Player* player)
{
    if (!rf::is_server) {
        return;
    }
    g_vote_mgr.send_state_to_player(player);
}

void server_vote_do_frame()
{
    g_vote_mgr.do_frame();
}

void server_vote_on_player_leave(rf::Player* player)
{
    g_vote_mgr.on_player_leave(player);
}

void server_vote_on_limbo_state_enter()
{
    g_vote_mgr.OnLimboStateEnter();
}
