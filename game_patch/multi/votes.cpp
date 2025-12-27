#include <string_view>
#include <algorithm>
#include <map>
#include <set>
#include <ctime>
#include <format>
#include <optional>
#include <cctype>
#include <utility>
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
    Gametype,
    Restart,
    Extend,
    Next,
    Random,
    Previous,
    Match,
    CancelMatch,
    Unknown
};

struct Vote
{
private:
    std::time_t start_time = 0;
    bool reminder_sent = false;
    rf::Timestamp early_finish_check_timer;
    std::map<rf::Player*, bool> players_who_voted;
    rf::Player* owner;

public:
    virtual ~Vote() = default;

    virtual VoteType get_type() const = 0;

    bool start(std::string_view arg, rf::Player* source)
    {
        if (!process_vote_arg(arg, source)) {
            return false;
        }
        owner = source;
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
            af_broadcast_automated_chat_msg("Vote canceled: owner left the game!");
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

            const auto current_player_list = get_clients(false, false);

            auto yes_votes = std::count_if(players_who_voted.begin(), players_who_voted.end(), [](const auto& pair) {
                return pair.second;
            });
            auto no_votes = players_who_voted.size() - yes_votes;

            auto msg = std::format("Vote status: Yes: {} No: {} Waiting: {}", yes_votes, no_votes,
                                   current_player_list.size() - players_who_voted.size());
            af_broadcast_automated_chat_msg(msg);
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
            int yes_votes = std::count_if(players_who_voted.begin(), players_who_voted.end(), [](auto& p) {
                return p.second;
            });
            int no_votes = std::count_if(players_who_voted.begin(), players_who_voted.end(), [](auto& p) {
                return !p.second;
            });

            const auto current = get_clients(false, false);
            int remaining = 0;
            for (auto* p : current) {
                if (is_eligible_voter(p) && players_who_voted.count(p) == 0) {
                    ++remaining;
                }
            }

            if (!vote_config.ignore_nonvoters) {
                no_votes += remaining;
            }

            af_broadcast_automated_chat_msg("Vote timed out!");
            finish_vote(yes_votes > no_votes);
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
        af_broadcast_automated_chat_msg("Vote canceled!");
        return true;
    }

    static bool player_meets_alpine_restrict(rf::Player* p) {
        const auto [verdict, verdict_string, hard_reject] =
            evaluate_alpine_restrict_status(p->version_info, false);
        return verdict == AlpineRestrictVerdict::ok;
    }

protected:
    [[nodiscard]] virtual std::string get_title() const = 0;
    [[nodiscard]] virtual const VoteConfig& get_config() const = 0;

    virtual bool process_vote_arg([[maybe_unused]] std::string_view arg, [[maybe_unused]] rf::Player* source)
    {
        return true;
    }

    virtual void on_accepted()
    {
        af_broadcast_automated_chat_msg("Vote passed!");
    }

    virtual void on_rejected()
    {
        af_broadcast_automated_chat_msg("Vote failed!");
    }

    void send_vote_starting_msg(rf::Player* source)
    {
        if (!source) {
            return; // should never happen
        }

        auto title = get_title();
        std::string base_msg = std::format("{} vote started by {}.\n", title, source->name);

        // Notify the player who started the vote
        af_send_automated_chat_msg(base_msg, source);

        // print to server console
        rf::console::printf(base_msg.c_str()); 

        // Prepare messages for other players
        std::string msg_non_alpine = "\n=============== VOTE STARTING ===============\n" + base_msg +
                                     "Send message \"/vote yes\" or \"/vote no\" to participate.";

        std::string msg_alpine = "\n=============== VOTE STARTING ===============\n" + base_msg;

        // Send the message to other players
        for (rf::Player* player : get_clients(false, false)) {
            if (!player || player == source) {
                continue; // skip the player who started the vote
            }

            if (!player_meets_alpine_restrict(player)) {
                continue;
            }

            const std::string& message_to_send =
                player->version_info.software == ClientSoftware::AlpineFaction
                    ? msg_alpine
                    : msg_non_alpine;

            af_send_automated_chat_msg(message_to_send, player);
        }
    }

    void finish_vote(bool is_accepted)
    {
        early_finish_check_timer.invalidate();

        if (is_accepted) {
            on_accepted();
        }
        else {
            on_rejected();
        }
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

    bool check_for_early_vote_finish()
    {
        int yes_votes = std::count_if(players_who_voted.begin(), players_who_voted.end(),
            [](const auto& p) {
                return p.second && is_eligible_voter(p.first);
            });

        int no_votes = std::count_if(players_who_voted.begin(), players_who_voted.end(),
            [](const auto& p) {
                return !p.second && is_eligible_voter(p.first);
            });

        const auto current = get_clients(false, false);
        int remaining = 0;
        for (auto* p : current) {
            if (is_eligible_voter(p) && players_who_voted.count(p) == 0)
                ++remaining;
        }

        const bool can_pass = yes_votes > no_votes + remaining;
        const bool can_fail = no_votes > yes_votes + remaining;
        const bool all_have_voted = remaining == 0;

        if (can_pass) {
            finish_vote(true);
            return false;
        }
        if (can_fail) {
            finish_vote(false);
            return false;
        }
        if (all_have_voted) {
            finish_vote(yes_votes > no_votes);
            return false;
        }

        return true;
    }
};

std::tuple<int, bool, std::string, std::optional<std::string>> parse_match_vote_info(std::string_view arg)
{
    arg = trim(arg);
    if (arg.size() < 3)
        return {-1, false, "", std::nullopt};

    auto [size_part, rest] = split_once_whitespace(arg);

    if (size_part.size() != 3 || size_part[1] != 'v' || size_part[0] < '1' || size_part[0] > '8' ||
        size_part[2] != size_part[0]) {
        return {-1, false, "", std::nullopt};
    }

    const int team_size = size_part[0] - '0';

    bool valid_level = false;
    std::string level_name;
    std::optional<std::string> preset_alias;

    if (!rest.empty()) {
        auto [level_part, preset_part] = split_once_whitespace(rest);

        if (!level_part.empty()) {
            auto [is_valid, normalized_name] = is_level_name_valid(level_part);
            valid_level = is_valid;
            level_name = std::move(normalized_name);
        }

        if (!preset_part.empty()) {
            // take the first token for preset alias
            auto [alias, _discard] = split_once_whitespace(preset_part);
            if (!alias.empty())
                preset_alias = std::string(alias);
        }
    }

    return {team_size, valid_level, level_name, preset_alias};
}

static bool does_level_match_gametype_prefix(const std::string& level_name)
{
    const auto game_type = rf::multi_get_game_type();
    std::string map_name = level_name;

    if (!string_iends_with(map_name, ".rfl")) {
        map_name += ".rfl";
    }

    if (game_type == rf::NG_TYPE_RUN && is_known_run_level(level_name)) {
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

static bool is_level_allowed_for_vote(const std::string& level_name, rf::Player* source, bool check_gametype = true)
{
    const auto& vote_level_cfg = g_alpine_server_config.vote_level;

    if (check_gametype && vote_level_cfg.only_allow_gametype_prefix && !does_level_match_gametype_prefix(level_name)) {
        auto msg = std::format("Cannot start vote: level {} does not match the current gametype!", level_name);
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

struct VoteMatch : public Vote
{
    std::optional<ManualRulesOverride> m_manual_rules_override;
    std::optional<std::string> m_manual_rules_alias;

    VoteType get_type() const override
    {
        return VoteType::Match;
    }

    bool process_vote_arg(std::string_view arg, rf::Player* source) override
    {
        auto [team_size, valid_level, match_level_name, preset_alias] = parse_match_vote_info(arg);
        g_match_info.team_size = team_size;

        if (valid_level) {
            g_match_info.match_level_name = match_level_name;
        }
        else if (match_level_name == "") {
            g_match_info.match_level_name = rf::level.filename.c_str();
        }
        else {
            af_send_automated_chat_msg("Invalid level specified! Try again, or omit level filename to use the current level.", source);
            return false;
        }

        if (!is_level_allowed_for_vote(g_match_info.match_level_name, source)) {
            return false;
        }

        if (g_match_info.team_size == -1) {
            af_send_automated_chat_msg("Invalid match size! Supported sizes are 1v1 up to 8v8.", source);
            return false;
        }

        m_manual_rules_override.reset();
        m_manual_rules_alias.reset();

        if (preset_alias) {
            auto alias_it = g_alpine_server_config.rules_preset_aliases.find(*preset_alias);
            if (alias_it == g_alpine_server_config.rules_preset_aliases.end()) {
                auto msg = std::format("Cannot start vote: rules preset '{}' is not defined!", *preset_alias);
                af_send_automated_chat_msg(msg, source);
                return false;
            }

            auto preset_result = load_rules_preset_alias(*preset_alias);
            if (!preset_result) {
                auto msg = std::format("Cannot start vote: failed to load rules preset '{}'", *preset_alias);
                af_send_automated_chat_msg(msg, source);
                return false;
            }

            m_manual_rules_alias = std::move(*preset_alias);
            m_manual_rules_override = std::move(*preset_result);
        }

        if (!multi_game_type_is_team_type(g_alpine_server_config.base_rules.game_type)) {
            af_send_automated_chat_msg("Cannot start vote: server base game type is not a team game type.", source);
            return false;
        }

        const bool using_current_level = g_match_info.match_level_name == rf::level.filename.c_str();

        const auto desired_game_type =
            m_manual_rules_override
            ? m_manual_rules_override->rules.game_type
            : (using_current_level ? g_alpine_server_config_active_rules.game_type
                                   : g_alpine_server_config.base_rules.game_type);

        if (!multi_game_type_is_team_type(desired_game_type)) {
            af_send_automated_chat_msg("Cannot start vote: matches must be played on a team game type.", source);
            return false;
        }

        return true;
    }

    [[nodiscard]] std::string get_title() const override
    {
        if (m_manual_rules_alias)
            return std::format("START {}v{} MATCH on {} (PRESET '{}')",
                               g_match_info.team_size, g_match_info.team_size,
                               g_match_info.match_level_name, *m_manual_rules_alias);
        return std::format("START {}v{} MATCH on {}",
            g_match_info.team_size, g_match_info.team_size, g_match_info.match_level_name);
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
        if (m_manual_rules_alias)
            msg = std::format("Vote passed. {} (rules preset '{}').", detail, *m_manual_rules_alias);
        else
            msg = std::format("Vote passed. {}.", detail);
        af_broadcast_automated_chat_msg(msg);

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
            multi_change_level_alpine(g_match_info.match_level_name.c_str());
            if (m_manual_rules_override) {
                set_manual_rules_override(std::move(*m_manual_rules_override));
                m_manual_rules_override.reset();
            }
        }
    }

    bool on_player_leave(rf::Player* player) override
    {        
        return Vote::on_player_leave(player);
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

    bool process_vote_arg(std::string_view arg, rf::Player* source) override
    {
        if (!g_match_info.match_active && !g_match_info.pre_match_active) {
            af_send_automated_chat_msg("No active or queued match to cancel.", source);
            return false;
        }

        return true;
    }

    void on_accepted() override
    {
        af_broadcast_automated_chat_msg("Vote passed: The match has been canceled.");

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

    VoteType get_type() const override
    {
        return VoteType::Kick;
    }

    bool process_vote_arg(std::string_view arg, [[ maybe_unused ]] rf::Player* source) override
    {
        std::string player_name{arg};
        m_target_player = find_best_matching_player(player_name.c_str());
        return m_target_player != nullptr;
    }

    [[nodiscard]] std::string get_title() const override
    {
        return std::format("KICK PLAYER '{}'", m_target_player->name);
    }

    void on_accepted() override
    {
        af_broadcast_automated_chat_msg("Vote passed: kicking player");
        rf::multi_kick_player(m_target_player);
    }

    bool on_player_leave(rf::Player* player) override
    {
        if (m_target_player == player) {
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
    rf::Player* m_target_player;

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
        af_broadcast_automated_chat_msg("Vote passed: extending round");
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
    std::optional<ManualRulesOverride> m_manual_rules_override;

    VoteType get_type() const override
    {
        return VoteType::Level;
    }

    bool process_vote_arg(std::string_view arg, rf::Player* source) override
    {
        arg = trim(arg);

        auto [level_part, preset_part] = split_once_whitespace(arg);
        auto [is_valid, level_name] = is_level_name_valid(level_part);

        if (!is_valid) {
            auto msg = std::format("Cannot start vote: level {} is not available on the server!", level_name);
            af_send_automated_chat_msg(msg, source);
            return false;
        }

        if (!is_level_allowed_for_vote(level_name, source)) {
            return false;
        }

        m_manual_rules_override.reset();

        if (!preset_part.empty()) {
            std::string preset_name{preset_part};
            auto alias_it = g_alpine_server_config.rules_preset_aliases.find(preset_name);
            if (alias_it == g_alpine_server_config.rules_preset_aliases.end()) {
                auto msg = std::format("Cannot start vote: rules preset '{}' is not defined!", preset_name);
                af_send_automated_chat_msg(msg, source);
                return false;
            }

            auto preset_result = load_rules_preset_alias(preset_name);
            if (!preset_result) {
                auto msg = std::format("Cannot start vote: failed to load rules preset '{}'", preset_name);
                af_send_automated_chat_msg(msg, source);
                return false;
            }

            m_manual_rules_override = std::move(*preset_result);
        }

        m_level_name = std::move(level_name);
        return true;
    }

    [[nodiscard]] std::string get_title() const override
    {
        if (m_manual_rules_override && m_manual_rules_override->preset_alias)
            return std::format("LOAD LEVEL '{}' (PRESET '{}')", m_level_name, *m_manual_rules_override->preset_alias);
        return std::format("LOAD LEVEL '{}'", m_level_name);
    }

    void on_accepted() override
    {
        clear_manual_rules_override();

        std::string msg;
        if (m_manual_rules_override && m_manual_rules_override->preset_alias)
            msg = std::format("Vote passed: changing level to {} with preset {}",
                              m_level_name, *m_manual_rules_override->preset_alias);
        else
            msg = std::format("Vote passed: changing level to {}", m_level_name);
        af_broadcast_automated_chat_msg(msg);
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

struct VoteGametype : public Vote
{
    std::string m_gametype_name;
    std::string m_level_name;

    VoteType get_type() const override
    {
        return VoteType::Gametype;
    }

    bool process_vote_arg(std::string_view arg, rf::Player* source) override
    {
        arg = trim(arg);
        if (arg.empty()) {
            af_send_automated_chat_msg("You must specify a gametype.", source);
            return false;
        }

        auto [gametype_part, level_part] = split_once_whitespace(arg);
        gametype_part = trim(gametype_part);

        if (gametype_part.empty()) {
            af_send_automated_chat_msg("You must specify a gametype name.", source);
            return false;
        }

        if (!is_gametype_name_valid(gametype_part)) {
            auto msg = std::format("Invalid gametype '{}'!", gametype_part);
            af_send_automated_chat_msg(msg, source);
            return false;
        }

        m_gametype_name.assign(gametype_part);

        if (level_part.empty()) {
            m_level_name = rf::level.filename.c_str();
        }
        else {
            auto [is_valid, normalized_level_name] = is_level_name_valid(level_part);
            if (!is_valid) {
                auto msg = std::format("Cannot start vote: level {} is not available on the server!", normalized_level_name);
                af_send_automated_chat_msg(msg, source);
                return false;
            }

            m_level_name = std::move(normalized_level_name);
        }

        if (string_iequals(m_level_name, rf::level.filename.c_str())) {
            return true; // level is current level, skip further checks
        }

        return is_level_allowed_for_vote(m_level_name, source, false); // skip gametype prefix check for gametype votes
    }

    [[nodiscard]] std::string get_title() const override
    {
        return std::format("SWITCH TO {} ON {}", string_to_upper(m_gametype_name), m_level_name);
    }

    void on_accepted() override
    {
        auto msg = std::format("Vote passed: switching to {} on {}", string_to_upper(m_gametype_name), m_level_name);
        af_broadcast_automated_chat_msg(msg);

        multi_set_gametype_alpine(m_gametype_name);

        clear_manual_rules_override();
        multi_change_level_alpine(m_level_name.c_str());
    }

    [[nodiscard]] bool is_allowed_in_limbo_state() const override
    {
        return false;
    }

    [[nodiscard]] const VoteConfig& get_config() const override
    {
        return g_alpine_server_config.vote_gametype;
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
        af_broadcast_automated_chat_msg("Vote passed: restarting level");
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
        af_broadcast_automated_chat_msg("Vote passed: loading next level");
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
        af_broadcast_automated_chat_msg("Vote passed: loading random level from rotation");

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
        af_broadcast_automated_chat_msg("Vote passed: loading previous level");
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
    std::optional<std::unique_ptr<Vote>> active_vote;

public:
    template<typename T>
    bool StartVote(std::string_view arg, rf::Player* source)
    {
        if (active_vote) {
            af_send_automated_chat_msg("Another vote is currently in progress!", source);
            return false;
        }

        auto vote = std::make_unique<T>();

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

        if (!vote->start(arg, source)) {
            return false;
        }

        active_vote = {std::move(vote)};
        return true;
    }    

    void on_player_leave(rf::Player* player)
    {
        if (active_vote && !active_vote.value()->on_player_leave(player)) {
            active_vote.reset();
        }
    }

    void OnLimboStateEnter()
    {
        if (active_vote && !active_vote.value()->is_allowed_in_limbo_state()) {
            af_broadcast_automated_chat_msg("Vote canceled!");
            active_vote.reset();
        }
    }

    void add_player_vote(bool is_yes_vote, rf::Player* source)
    {
        if (!active_vote) {
            af_send_automated_chat_msg("No vote in progress!", source);
            return;
        }

        if (!active_vote.value()->add_player_vote(is_yes_vote, source)) {
            active_vote.reset();
        }
    }

    void try_cancel_vote(rf::Player* source)
    {
        if (!active_vote) {
            af_send_automated_chat_msg("No vote in progress!", source);
            return;
        }

        if (active_vote.value()->try_cancel_vote(source)) {
            active_vote.reset();
        }
    }

    void do_frame()
    {
        if (!active_vote)
            return;

        if (!active_vote.value()->do_frame()) {
            active_vote.reset();
        }
    }
};

VoteMgr g_vote_mgr;

void handle_vote_command(std::string_view vote_name, std::string_view vote_arg, rf::Player* sender)
{
    if (sender->version_info.software == ClientSoftware::Browser || sender->is_bot) {
        af_send_automated_chat_msg("Browsers and bots are not allowed to vote!", sender, true);
        return;
    } else if (!Vote::player_meets_alpine_restrict(sender)) {
        af_send_automated_chat_msg(
            "You can't vote, because your client does not meet the server's requirements. Visit alpinefaction.com to upgrade.",
            sender, true
        );
        return;
    } else if (player_is_idle(sender)) {
        af_send_automated_chat_msg("Idle players are not allowed to vote!", sender, true);
        return;
    }

    if (vote_name == "kick")
        g_vote_mgr.StartVote<VoteKick>(vote_arg, sender);
    else if (vote_name == "level" || vote_name == "map")
        g_vote_mgr.StartVote<VoteLevel>(vote_arg, sender);
    else if (vote_name == "extend" || vote_name == "ext")
        g_vote_mgr.StartVote<VoteExtend>(vote_arg, sender);
    else if (vote_name == "gametype" || vote_name == "gamemode" || vote_name == "type" || vote_name == "gt")
        g_vote_mgr.StartVote<VoteGametype>(vote_arg, sender);
    else if (vote_name == "restart" || vote_name == "rest")
        g_vote_mgr.StartVote<VoteRestart>(vote_arg, sender);
    else if (vote_name == "next")
        g_vote_mgr.StartVote<VoteNext>(vote_arg, sender);
    else if (vote_name == "random" || vote_name == "rand")
        g_vote_mgr.StartVote<VoteRandom>(vote_arg, sender);
    else if (vote_name == "previous" || vote_name == "prev")
        g_vote_mgr.StartVote<VotePrevious>(vote_arg, sender);
    else if (vote_name == "match")
        g_vote_mgr.StartVote<VoteMatch>(vote_arg, sender);
    else if (vote_name == "cancelmatch" || vote_name == "nomatch")
        g_vote_mgr.StartVote<VoteCancelMatch>(vote_arg, sender);
    else if (vote_name == "yes" || vote_name == "y")
        g_vote_mgr.add_player_vote(true, sender);
    else if (vote_name == "no" || vote_name == "n")
        g_vote_mgr.add_player_vote(false, sender);
    else if (vote_name == "cancel")
        g_vote_mgr.try_cancel_vote(sender);
    else
        af_send_automated_chat_msg("Unrecognized vote type!", sender);
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
