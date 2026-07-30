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

// Clients that consume af_sreq_vote_state instead of the legacy chat text. The
// listen-server host always does (its packets are applied locally).
static bool player_uses_vote_packets(rf::Player* player)
{
    if (!player) {
        return false;
    }
    if (player == rf::local_player) {
        return true;
    }
    return is_player_minimum_af_client_version(player, 1, 4, 0);
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
        announced = true;
        send_vote_starting_msg(source);

        start_time = std::time(nullptr);
        early_finish_check_timer.set(1000);

        players_who_voted.insert({source, true});

        return check_for_early_vote_finish();
    }

    virtual bool on_player_leave(rf::Player* player)
    {
        if (player == owner) {
            early_finish_check_timer.invalidate();
            broadcast_vote_legacy_msg("Vote canceled: owner left the game!");
            broadcast_vote_end(AfVoteResult::Canceled);
            return false;
        }
        players_who_voted.erase(player);
        return check_for_early_vote_finish();
    }

    [[nodiscard]] virtual bool is_allowed_in_limbo_state() const
    {
        return true;
    }

    bool add_player_vote(bool is_yes_vote, rf::Player* source)
    {
        if (players_who_voted.count(source) == 1) {
            af_send_automated_chat_msg("You already voted!", source);
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
            af_send_automated_chat_msg("You cannot cancel a vote you didn't start!", source);
            return false;
        }

        early_finish_check_timer.invalidate();
        broadcast_vote_legacy_msg("Vote canceled!");
        broadcast_vote_end(AfVoteResult::Canceled);
        return true;
    }

    void cancel_for_limbo()
    {
        early_finish_check_timer.invalidate();
        broadcast_vote_legacy_msg("Vote canceled!");
        broadcast_vote_end(AfVoteResult::Canceled);
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
                                 player == owner, initiator, get_title());
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

    // Tell every structured client the vote is over. Idempotent: a vote that
    // disappears for any other reason (kick target left, limbo, ...) still gets
    // exactly one end event.
    void broadcast_vote_end(AfVoteResult result)
    {
        if (end_event_sent || !announced) {
            return; // a vote rejected during validation was never announced
        }
        end_event_sent = true;

        for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
            if (player_uses_vote_packets(&player)) {
                af_send_vote_state_end(&player, result);
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

    virtual void on_accepted()
    {
        broadcast_vote_legacy_msg("Vote passed!");
    }

    virtual void on_rejected()
    {
        broadcast_vote_legacy_msg("Vote failed!");
    }

    static uint8_t clamp_to_u8(int value)
    {
        return static_cast<uint8_t>(std::clamp(value, 0, 255));
    }

    // Vote chat text goes only to clients that can't receive the structured
    // events; their sniffing contract depends on these exact strings. The
    // console line matches af_broadcast_automated_chat_msg so dedicated server
    // output is unchanged.
    static void broadcast_vote_legacy_msg(std::string_view msg)
    {
        rf::console::print("Server: {}", msg);

        for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
            if (&player == rf::local_player) {
                continue;
            }
            if (player_uses_vote_packets(&player)) {
                continue;
            }
            af_send_automated_chat_msg(msg, &player);
        }
    }

    void broadcast_vote_update(const VoteTally& tally)
    {
        for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
            if (player_uses_vote_packets(&player)) {
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

        // print to server console
        rf::console::printf(base_msg.c_str());

        // Prepare messages for legacy players
        std::string msg_non_alpine = "\n=============== VOTE STARTING ===============\n" + base_msg +
                                     "Send message \"/vote yes\" or \"/vote no\" to participate.";

        std::string msg_alpine = "\n=============== VOTE STARTING ===============\n" + base_msg;

        const int time_limit = std::max(0, get_config().time_limit_seconds);

        for (rf::Player* player : get_clients(false, false)) {
            if (!player) {
                continue;
            }

            if (player != source && !player_meets_alpine_restrict(player)) {
                continue;
            }

            if (player_uses_vote_packets(player)) {
                // The vote owner's own yes vote is already counted.
                af_send_vote_state_start(player, vote_type_to_wire(get_type()),
                                         static_cast<uint16_t>(time_limit), 1, 0,
                                         clamp_to_u8(count_eligible_voters() - 1), player == source,
                                         owner_name, title);
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

        broadcast_vote_end(result.value_or(is_accepted ? AfVoteResult::Passed : AfVoteResult::Failed));

        pending_outcome = is_accepted ? Outcome::Accepted : Outcome::Rejected;
    }

    static bool is_eligible_voter(rf::Player* const p) {
        if (!p) {
            return false;
        }
        if (p->version_info.software == ClientSoftware::Browser
            || p->is_bot
            || player_is_idle(p)
            || !player_meets_alpine_restrict(p)) {
            return false;
        }
        return true;
    }

    static int count_eligible_voters()
    {
        int count = 0;
        for (auto* p : get_clients(false, false)) {
            if (is_eligible_voter(p)) {
                ++count;
            }
        }
        return count;
    }

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
    std::string map_name = level_name;

    if (!string_iends_with(map_name, ".rfl")) {
        map_name += ".rfl";
    }

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

    if (game_type == rf::NG_TYPE_CTF && matches_prefix("pctf")) {
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
// 32 bits so future game types beyond NG_TYPE_GG still fit.
static uint32_t build_level_valid_gametype_mask(const std::string& level_name)
{
    uint32_t mask = 0;
    for (int i = 0; i <= static_cast<int>(rf::NG_TYPE_GG); ++i) {
        if (does_level_match_gametype_prefix(level_name, static_cast<rf::NetGameType>(i))) {
            mask |= (1u << i);
        }
    }
    return mask;
}

static bool is_level_allowed_for_vote(const std::string& level_name, rf::Player* source,
                                      rf::NetGameType effective_game_type)
{
    const auto& vote_level_cfg = g_alpine_server_config.vote_level;

    // Checked against the game type the level will run with once the vote
    // applies, not the one currently running.
    if (!is_level_valid_for_vote_gametype(level_name, effective_game_type)) {
        auto msg = std::format("Cannot start vote: level {} does not match the {} gametype!", level_name,
                               multi_game_type_name_short(effective_game_type));
        af_send_automated_chat_msg(msg, source);
        return false; // level does not match gametype prefix
    }

    if (vote_level_cfg.allowed_maps.empty() && !vote_level_cfg.add_rotation_to_allowed_levels) {
        return true; // no allowed_levels configured and not adding rotation, so all levels are allowed
    }

    std::vector<std::string> allowed_maps = vote_level_cfg.allowed_maps;
    if (vote_level_cfg.add_rotation_to_allowed_levels) {
        for (const auto& level_entry : g_alpine_server_config.levels) {
            allowed_maps.push_back(level_entry.level_filename);
        }
    }

    if (allowed_maps.empty()) {
        return true; // still empty after all, so all levels are allowed
    }

    const bool is_allowed = std::any_of(
        allowed_maps.begin(), allowed_maps.end(),
        [&](const std::string& allowed_name) { return string_iequals(allowed_name, level_name); });

    if (!is_allowed) {
        auto msg = std::format("Cannot start vote: the server does not allow voting for level {}!", level_name);
        af_send_automated_chat_msg(msg, source);
        return false; // level not in allowed_levels
    }

    return true;
}

// "(CTF)" / "[Instagib, Rails]" suffixes appended to level and match vote titles.
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
            af_send_automated_chat_msg("Invalid match size! Supported sizes are 1v1 up to 8v8.", source);
            return false;
        }

        if (m_level_name.empty()) {
            m_level_name = rf::level.filename.c_str();
        }
        else {
            auto [is_valid, normalized_name] = is_level_name_valid(m_level_name);
            if (!is_valid) {
                af_send_automated_chat_msg(
                    "Invalid level specified! Try again, or omit level filename to use the current level.", source);
                return false;
            }
            m_level_name = std::move(normalized_name);
        }

        // A match on the current level with no rules override keeps the level
        // (and therefore its active rules) exactly as they are; anything else
        // re-resolves from the level's natural rules.
        const bool builds_override = !m_mutators.empty() || m_gametype.has_value();
        const bool using_current_level = m_level_name == rf::level.filename.c_str();
        const rf::NetGameType effective_game_type = resolve_effective_vote_game_type(
            m_level_name, m_gametype, builds_override, using_current_level);

        if (!is_level_allowed_for_vote(m_level_name, source, effective_game_type)) {
            return false;
        }

        if (!multi_game_type_is_team_type(g_alpine_server_config.base_rules.game_type)) {
            af_send_automated_chat_msg("Cannot start vote: server base game type is not a team game type.", source);
            return false;
        }

        // The match must end up on a team game type — evaluated against what will
        // actually apply, not the base rules.
        if (!multi_game_type_is_team_type(effective_game_type)) {
            af_send_automated_chat_msg("Cannot start vote: matches must be played on a team game type.", source);
            return false;
        }

        m_manual_rules_override = load_vote_rules_override(m_level_name, m_mutators, m_gametype);
        m_mutator_labels = mutators_join_labels(m_mutators);

        // Only touch the shared match state once every check has passed.
        g_match_info.team_size = m_team_size;
        g_match_info.match_level_name = m_level_name;
        return true;
    }

    [[nodiscard]] std::string get_title() const override
    {
        return std::format("START {}v{} MATCH on {}{}", m_team_size, m_team_size, m_level_name,
                           build_rules_title_suffix(m_gametype, m_mutator_labels));
    }

    void on_accepted() override
    {
        const bool match_level_is_current = (g_match_info.match_level_name == rf::level.filename.c_str());

        bool match_game_type_matches_current = true;
        if (match_level_is_current) {
            rf::NetGameType desired_game_type = rf::netgame.type;
            if (m_manual_rules_override)
                desired_game_type = m_manual_rules_override->rules.game_type;
            else
                desired_game_type = g_alpine_server_config_active_rules.game_type;

            match_game_type_matches_current = (desired_game_type == rf::netgame.type);
        }

        const bool using_current_level = match_level_is_current && match_game_type_matches_current;
        const char* detail = using_current_level ? "Entering pre-match ready up phase"
                             : match_level_is_current
                                 ? "Restarting level to apply match game type, then entering pre-match ready up phase"
                                 : "Changing to match level, then entering pre-match ready up phase";

        std::string msg;
        if (!m_mutator_labels.empty())
            msg = std::format("Vote passed. {} (mutators: {}).", detail, m_mutator_labels);
        else
            msg = std::format("Vote passed. {}.", detail);
        broadcast_vote_legacy_msg(msg);

        g_match_info.pre_match_queued = true;

        if (using_current_level) {
            if (m_manual_rules_override) {
                set_manual_rules_override(std::move(*m_manual_rules_override));
                apply_rules_for_current_level();
                m_manual_rules_override.reset();
            }
            start_pre_match();
        }
        else if (!g_match_info.match_level_name.empty()) {
            if (!m_manual_rules_override)
                clear_manual_rules_override();
            if (m_gametype)
                set_upcoming_game_type(*m_gametype, UpcomingGameTypeSelection::ExplicitRequest);
            multi_change_level_alpine(g_match_info.match_level_name.c_str());
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
            af_send_automated_chat_msg("No active or queued match to cancel.", source);
            return false;
        }

        return true;
    }

    void on_accepted() override
    {
        broadcast_vote_legacy_msg("Vote passed: The match has been canceled.");

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

    explicit VoteKick(rf::Player* target) : m_target_player(target) {}

    VoteType get_type() const override
    {
        return VoteType::Kick;
    }

    bool validate(rf::Player* source) override
    {
        if (!m_target_player) {
            af_send_automated_chat_msg("Cannot start vote: that player is no longer on the server.", source);
            return false;
        }
        return true;
    }

    [[nodiscard]] std::string get_title() const override
    {
        return std::format("KICK PLAYER '{}'", m_target_player->name);
    }

    void on_accepted() override
    {
        broadcast_vote_legacy_msg("Vote passed: kicking player");
        rf::multi_kick_player(m_target_player);
    }

    bool on_player_leave(rf::Player* player) override
    {
        if (m_target_player == player) {
            return false; // the end event goes out when the vote is destroyed
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
    VoteType get_type() const override
    {
        return VoteType::Extend;
    }

    [[nodiscard]] std::string get_title() const override
    {
        return "EXTEND ROUND BY 5 MINUTES";
    }

    void on_accepted() override
    {
        broadcast_vote_legacy_msg("Vote passed: extending round");
        extend_round_time(5);
    }

    [[nodiscard]] bool is_allowed_in_limbo_state() const override
    {
        return false;
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
            af_send_automated_chat_msg(msg, source);
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

    void on_accepted() override
    {
        clear_manual_rules_override();

        std::string msg = std::format("Vote passed: changing level to {}{}", m_level_name,
                                      build_rules_title_suffix(m_gametype, m_mutator_labels));
        broadcast_vote_legacy_msg(msg);

        if (m_gametype) {
            set_upcoming_game_type(*m_gametype, UpcomingGameTypeSelection::ExplicitRequest);
        }

        multi_change_level_alpine(m_level_name.c_str());

        if (m_manual_rules_override) {
            set_manual_rules_override(std::move(*m_manual_rules_override));
            m_manual_rules_override.reset();
        }
    }

    [[nodiscard]] bool is_allowed_in_limbo_state() const override
    {
        return false;
    }

    [[nodiscard]] const VoteConfig& get_config() const override
    {
        return g_alpine_server_config.vote_level;
    }
};

struct VoteRestart : public Vote
{

    VoteType get_type() const override
    {
        return VoteType::Restart;
    }

    [[nodiscard]] std::string get_title() const override
    {
        return "RESTART LEVEL";
    }

    void on_accepted() override
    {
        broadcast_vote_legacy_msg("Vote passed: restarting level");
        restart_current_level();
    }

    [[nodiscard]] bool is_allowed_in_limbo_state() const override
    {
        return false;
    }

    [[nodiscard]] const VoteConfig& get_config() const override
    {
        return g_alpine_server_config.vote_restart;
    }
};

struct VoteNext : public Vote
{
    VoteType get_type() const override
    {
        return VoteType::Next;
    }

    [[nodiscard]] std::string get_title() const override
    {
        return "LOAD NEXT LEVEL";
    }

    void on_accepted() override
    {
        broadcast_vote_legacy_msg("Vote passed: loading next level");
        load_next_level();
    }

    [[nodiscard]] bool is_allowed_in_limbo_state() const override
    {
        return false;
    }

    [[nodiscard]] const VoteConfig& get_config() const override
    {
        return g_alpine_server_config.vote_next;
    }
};

struct VoteRandom : public Vote
{
    VoteType get_type() const override
    {
        return VoteType::Random;
    }

    [[nodiscard]] std::string get_title() const override
    {
        return "LOAD RANDOM LEVEL";
    }

    void on_accepted() override
    {
        broadcast_vote_legacy_msg("Vote passed: loading random level from rotation");

        // if dynamic rotation is on, just load the next level
        g_alpine_server_config.dynamic_rotation ? load_next_level() : load_rand_level();
    }

    [[nodiscard]] bool is_allowed_in_limbo_state() const override
    {
        return false;
    }

    [[nodiscard]] const VoteConfig& get_config() const override
    {
        return g_alpine_server_config.vote_rand;
    }
};

struct VotePrevious : public Vote
{
    VoteType get_type() const override
    {
        return VoteType::Previous;
    }

    [[nodiscard]] std::string get_title() const override
    {
        return "LOAD PREV LEVEL";
    }

    void on_accepted() override
    {
        broadcast_vote_legacy_msg("Vote passed: loading previous level");
        load_prev_level();
    }

    [[nodiscard]] bool is_allowed_in_limbo_state() const override
    {
        return false;
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
        // including the ones with no outcome (kick target left, limbo).
        vote->broadcast_vote_end(AfVoteResult::Canceled);
        vote->run_pending_outcome();
    }

public:
    template<typename T, typename... Args>
    bool StartVote(rf::Player* source, Args&&... args)
    {
        if (active_vote) {
            af_send_automated_chat_msg("Another vote is currently in progress!", source);
            return false;
        }

        auto vote = std::make_unique<T>(std::forward<Args>(args)...);

        if (!vote->get_config().enabled) {
            af_send_automated_chat_msg("This vote type is disabled!", source);
            return false;
        }

        if (!vote->is_allowed_in_limbo_state() && rf::gameseq_get_state() != rf::GS_GAMEPLAY) {
            af_send_automated_chat_msg("Vote cannot be started now!", source);
            return false;
        }

        if (vote->get_type() == VoteType::Match && (g_match_info.pre_match_active || g_match_info.match_active)) {
            af_send_automated_chat_msg(
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
        if (active_vote && !active_vote->is_allowed_in_limbo_state()) {
            std::unique_ptr<Vote> vote = std::move(active_vote);
            vote->cancel_for_limbo();
        }
    }

    void add_player_vote(bool is_yes_vote, rf::Player* source)
    {
        if (!active_vote) {
            af_send_automated_chat_msg("No vote in progress!", source);
            return;
        }

        if (!active_vote->add_player_vote(is_yes_vote, source)) {
            conclude(std::move(active_vote));
        }
    }

    void try_cancel_vote(rf::Player* source)
    {
        if (!active_vote) {
            af_send_automated_chat_msg("No vote in progress!", source);
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
static uint8_t g_vote_options_generation = 0;
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

static uint16_t build_enabled_vote_mask()
{
    const auto& cfg = g_alpine_server_config;
    const auto bit = [](AfVoteType type) { return static_cast<uint16_t>(1u << static_cast<unsigned>(type)); };

    uint16_t mask = 0;
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
    blob_u16(blob, build_enabled_vote_mask());

    // server-wide vote flags
    uint8_t server_flags = 0;
    if (g_alpine_server_config.vote_level.only_allow_gametype_prefix) {
        server_flags |= AF_VOTE_SERVER_FLAG_GAMETYPE_PREFIX;
    }
    blob_u8(blob, server_flags);

    // game types
    const int gametype_count = static_cast<int>(rf::NG_TYPE_GG) + 1;
    blob_u8(blob, static_cast<uint8_t>(gametype_count));
    for (int i = 0; i < gametype_count; ++i) {
        const auto game_type = static_cast<rf::NetGameType>(i);
        blob_u8(blob, static_cast<uint8_t>(i));
        blob_u8(blob, multi_game_type_is_team_type(game_type) ? AF_VOTE_GAMETYPE_FLAG_TEAM : 0);
        blob_str(blob, multi_game_type_name(game_type));
    }

    // mutators
    const auto& registry = mutators_get_registry();
    blob_u8(blob, static_cast<uint8_t>(std::min<size_t>(registry.size(), 255)));
    for (const auto& mutator : registry) {
        blob_u8(blob, static_cast<uint8_t>(mutator.id));
        blob_str(blob, mutator.name);
        blob_str(blob, mutator.label);
        blob_u8(blob, static_cast<uint8_t>(std::min<size_t>(mutator.options.size(), 255)));
        for (const auto& option : mutator.options) {
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
                blob_u8(blob, static_cast<uint8_t>(std::min<size_t>(option.choices.size(), 255)));
                for (const auto& choice : option.choices) {
                    blob_str(blob, choice.label);
                }
            }
        }
    }

    // votable levels, each with the game type it would run with by default and
    // the set of game types whose prefix rules it matches
    const std::vector<std::string> levels = build_votable_level_list();
    blob_u16(blob, static_cast<uint16_t>(std::min<size_t>(levels.size(), 65535)));
    for (const auto& level : levels) {
        blob_str(blob, level);
        blob_u8(blob, static_cast<uint8_t>(vote_natural_rules_for_level(level).game_type));
        blob_u32(blob, build_level_valid_gametype_mask(level));
    }
}

void server_vote_invalidate_options_blob()
{
    g_vote_options_blob_valid = false;
}

const std::vector<uint8_t>& server_vote_get_options_blob(uint8_t& generation)
{
    if (!g_vote_options_blob_valid) {
        build_vote_options_blob(g_vote_options_blob);
        ++g_vote_options_generation;
        g_vote_options_blob_valid = true;
        xlog::debug("vote options: rebuilt blob ({} bytes, generation {})",
                    g_vote_options_blob.size(), g_vote_options_generation);
    }
    generation = g_vote_options_generation;
    return g_vote_options_blob;
}

// ============================================================================
// Entry points
// ============================================================================

// Shared gate for chat and packet vote actions.
static bool check_voter_eligibility(rf::Player* sender)
{
    if (sender->version_info.software == ClientSoftware::Browser || sender->is_bot) {
        af_send_automated_chat_msg("Browsers and bots are not allowed to vote!", sender, true);
        return false;
    }
    if (!Vote::player_meets_alpine_restrict(sender)) {
        af_send_automated_chat_msg(
            "You can't vote, because your client does not meet the server's requirements. Visit alpinefaction.com to upgrade.",
            sender, true
        );
        return false;
    }
    if (player_is_idle(sender)) {
        af_send_automated_chat_msg("Idle players are not allowed to vote!", sender, true);
        return false;
    }
    return true;
}

void handle_vote_command(std::string_view vote_name, [[maybe_unused]] std::string_view vote_arg, rf::Player* sender)
{
    if (!check_voter_eligibility(sender)) {
        return;
    }

    if (vote_name == "yes" || vote_name == "y")
        g_vote_mgr.add_player_vote(true, sender);
    else if (vote_name == "no" || vote_name == "n")
        g_vote_mgr.add_player_vote(false, sender);
    else if (vote_name == "cancel")
        g_vote_mgr.try_cancel_vote(sender);
    else
        af_send_automated_chat_msg(
            "Calling votes via chat is no longer supported. Vote calling requires Alpine Faction 1.4+ - "
            "upgrade at alpinefaction.com. You can still vote with /vote yes or /vote no.",
            sender, true);
}

// 0xFF means "server default rules"; anything else must name a real game type.
static bool resolve_vote_gametype(uint8_t wire_value, rf::Player* sender, std::optional<rf::NetGameType>& out)
{
    if (wire_value == af_vote_gametype_none) {
        out = std::nullopt;
        return true;
    }
    if (wire_value > static_cast<uint8_t>(rf::NG_TYPE_GG)) {
        af_send_automated_chat_msg("Cannot start vote: this server does not support that game type.", sender);
        return false;
    }
    out = static_cast<rf::NetGameType>(wire_value);
    return true;
}

static bool resolve_vote_mutators(const std::vector<VoteMutatorInput>& input, rf::Player* sender,
                                  std::vector<MutatorDeclaration>& out)
{
    if (auto error = mutators_build_declarations_from_vote(input, out)) {
        af_send_automated_chat_msg(std::format("Cannot start vote: {}", *error), sender);
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
                af_send_automated_chat_msg("Cannot start vote: no level was specified.", sender);
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
            g_vote_mgr.StartVote<VoteExtend>(sender);
            break;
        case AfVoteType::Restart:
            g_vote_mgr.StartVote<VoteRestart>(sender);
            break;
        case AfVoteType::Next:
            g_vote_mgr.StartVote<VoteNext>(sender);
            break;
        case AfVoteType::Random:
            g_vote_mgr.StartVote<VoteRandom>(sender);
            break;
        case AfVoteType::Previous:
            g_vote_mgr.StartVote<VotePrevious>(sender);
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
