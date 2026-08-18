#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <common/utils/list-utils.h>
#include "../rf/entity.h"
#include "../rf/level.h"
#include "../rf/math/vector.h"
#include "../rf/multi.h"
#include "../rf/object.h"
#include "../rf/os/timestamp.h"
#include "../rf/player/player.h"
#include "../rf/weapon.h"
#include "../fflink/afstats_events.h"
#include "../hud/hud.h"
#include "../misc/alpine_settings.h"
#include "../os/console.h"
#include "../sound/sound.h"
#include "alpine_packets.h"
#include "awards.h"
#include "bagman.h"
#include "gametype.h"
#include "kill_attribution.h"
#include "multi.h"
#include "mutators.h"
#include "salvage.h"
#include "server.h"
#include "server_internal.h"

// Server-detected per-player awards. The server owns every condition below; the client is told an
// id and owns the text and the sound for it, so adding an award never needs a wire change.
// Awards are disabled during pre-match.

namespace
{

using Clock = std::chrono::steady_clock;

int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

bool awards_tracking_active()
{
    return rf::is_multi && rf::is_server && !g_match_info.pre_match_active;
}

int player_id_of(const rf::Player* player)
{
    if (!player || !player->net_data) {
        return -1;
    }
    const int id = player->net_data->player_id;
    return (id >= 0 && id < rf::multi_max_player_id) ? id : -1;
}

// "Enemy" excludes teammates in team modes and excludes the player themselves everywhere, so
// team kills and suicides advance nothing.
bool is_enemy_kill(const rf::Player* killer, const rf::Player* victim)
{
    if (!killer || !victim || killer == victim) {
        return false;
    }
    if (multi_is_team_game_type() && killer->team == victim->team) {
        return false;
    }
    return true;
}

bool is_sniper_or_rail(int weapon_type)
{
    if (!kill_attribution_is_valid_weapon_type(weapon_type)) {
        return false;
    }
    return weapon_type == rf::sniper_rifle_weapon_type || weapon_type == rf::rail_gun_weapon_type;
}

bool is_riot_weapon(int weapon_type)
{
    if (!kill_attribution_is_valid_weapon_type(weapon_type)) {
        return false;
    }
    if (rf::weapon_is_riot_stick(weapon_type)) {
        return true;
    }
    const int shield = kill_attribution_riot_shield_type();
    return shield >= 0 && weapon_type == shield;
}

// -------------------------------------------------------------------------
// Per-player state. Indexed by player id, which the engine REUSES - see
// awards_on_player_destroy.
// -------------------------------------------------------------------------

constexpr int64_t riot_control_window_ms = 10000;
constexpr int riot_control_kills_needed = 3;
constexpr int massacre_kills_needed = 8;
constexpr int64_t excellent_window_ms = 2000;
constexpr int overkill_streak = 10;
constexpr int unstoppable_streak_step = 10;
constexpr float last_stand_life = 2.0f;
constexpr float denied_base_radius = 2.0f;
constexpr int area_denied_capture_milli = 98000;
constexpr int64_t bag_check_window_ms = 1000;
constexpr float clutch_remaining_seconds = 2.0f;
constexpr uint8_t nemesis_kills_needed = 5;

struct AwardPlayerState
{
    // Riot Control: timestamps of the recent riot stick / riot shield kills.
    std::deque<int64_t> riot_kills;
    // Impressive: a sniper/rail shot is in flight, and the length of the current consecutive-hit
    // chain.
    bool shot_pending = false;
    int hit_chain = 0;
    // Excellent: the unconsumed half of a kill pair.
    bool has_last_kill = false;
    int64_t last_kill_ms = 0;
    // Overkill / Unstoppable. Deliberately the module's own counter: the scoreboard and afstats
    // counters are written by other paths with their own gating.
    int streak = 0;
    // Flag Runner: this player stole the flag and has neither died nor dropped it since.
    bool clean_steal = false;
    // Bag Check: when this player last took the bag.
    bool has_bag_pickup = false;
    int64_t bag_pickup_ms = 0;
};

std::vector<AwardPlayerState>& player_states()
{
    static std::vector<AwardPlayerState> states(rf::multi_max_player_id);
    return states;
}

AwardPlayerState* state_for(const rf::Player* player)
{
    const int id = player_id_of(player);
    return id < 0 ? nullptr : &player_states()[id];
}

// -------------------------------------------------------------------------
// Nemesis (a status, not an award) and the Revenge award it enables.
// Keyed by the ORDERED pair (killer, victim).
// -------------------------------------------------------------------------

struct NemesisPair
{
    uint8_t count = 0;
    bool active = false;
};

uint16_t nemesis_key(uint8_t killer_id, uint8_t victim_id)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(killer_id) << 8) | victim_id);
}

std::map<uint16_t, NemesisPair> g_nemesis;

// -------------------------------------------------------------------------
// Detonation frames (Massacre). One frame per open SplashWeaponScope; they nest, so chained
// explosions are separate detonations.
// -------------------------------------------------------------------------

struct DetonationKills
{
    uint8_t killer_id = 0xFF;
    int count = 0;
    rf::Player* last_victim = nullptr;
};

struct DetonationFrame
{
    std::vector<DetonationKills> killers;
};

// Kept as a stack with retained capacity: the scope is constructed very often, so neither entering
// nor leaving one may allocate in the common (no kills) case.
std::vector<DetonationFrame> g_detonation_stack;
int g_detonation_depth = 0;

// -------------------------------------------------------------------------
// Lag-compensated shot scope (2fer). One scope is one fired projectile: sniper and rail both
// create a single bullet per shot, and the whole flight - every pierced victim - resolves inside
// this call.
// -------------------------------------------------------------------------

struct ShotScopeState
{
    bool active = false;
    int shooter_id = -1;
    int weapon_type = -1;
    int enemy_kills = 0;
    rf::Player* last_victim = nullptr;
};

ShotScopeState g_shot_scope;

// -------------------------------------------------------------------------
// CTF flag stands. When a flag is home its position IS the stand position, so the cache is filled
// opportunistically from every moment one is observed at home.
// -------------------------------------------------------------------------

struct FlagHomeCache
{
    bool known = false;
    rf::Vector3 pos{};
};

FlagHomeCache g_flag_home[2]; // 0 = red, 1 = blue

void note_flag_home(int team, const rf::Vector3& pos)
{
    if (team != 0 && team != 1) {
        return;
    }
    g_flag_home[team].known = true;
    g_flag_home[team].pos = pos;
}

void sample_ctf_flag_homes()
{
    if (rf::multi_get_game_type() != rf::NG_TYPE_CTF) {
        return;
    }
    // The in-base getters are null-item safe (multi_ctf_is_*_flag_in_base_hook) and the position
    // getters copy from globals, so a one-flag level cannot reach a null item through here.
    rf::Vector3 pos{};
    if (rf::multi_ctf_is_red_flag_in_base()) {
        rf::multi_ctf_get_red_flag_pos(&pos);
        note_flag_home(0, pos);
    }
    if (rf::multi_ctf_is_blue_flag_in_base()) {
        rf::multi_ctf_get_blue_flag_pos(&pos);
        note_flag_home(1, pos);
    }
}

// -------------------------------------------------------------------------
// Clutch: the decisive scoring event of the game.
// -------------------------------------------------------------------------

// Sides are compared, never displayed: a team index in team modes, an offset player id otherwise.
constexpr int clutch_ffa_side_base = 100;

struct ClutchRecord
{
    bool valid = false;
    int side = -1;
    std::vector<uint8_t> player_ids;
    float remaining = -1.0f;
};

ClutchRecord g_clutch;
bool g_clutch_end_type_known = false;

int clutch_side_of(const rf::Player* player)
{
    if (!player) {
        return -1;
    }
    if (multi_is_team_game_type()) {
        return player->team == 0 ? 0 : 1;
    }
    const int id = player_id_of(player);
    return id < 0 ? -1 : clutch_ffa_side_base + id;
}

std::optional<int> strict_leader(int red, int blue)
{
    if (red > blue) {
        return 0;
    }
    if (blue > red) {
        return 1;
    }
    return {};
}

std::optional<int> ffa_strict_leader()
{
    int best = -1;
    int best_side = -1;
    bool tied = false;
    for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
        if (player.is_browser || !player.stats) {
            continue;
        }
        const int score = player.stats->score;
        if (score > best) {
            best = score;
            best_side = clutch_side_of(&player);
            tied = false;
        }
        else if (score == best) {
            tied = true;
        }
    }
    if (best_side < 0 || tied) {
        return {};
    }
    return best_side;
}

std::optional<int> hills_owning_team()
{
    bool any_red = false;
    bool any_blue = false;
    for (const HillInfo& hill : g_koth_info.hills) {
        any_red |= hill.ownership == HillOwner::HO_Red;
        any_blue |= hill.ownership == HillOwner::HO_Blue;
    }
    if (any_red && !any_blue) {
        return 0;
    }
    if (any_blue && !any_red) {
        return 1;
    }
    return {};
}

// The side that is strictly ahead right now, or nothing when the standings are tied. REV and ESC
// have no running score: their "lead" is the win condition itself, so a decisive record only ever
// exists there once a team has actually taken every point.
std::optional<int> current_strict_leader()
{
    switch (rf::multi_get_game_type()) {
    case rf::NG_TYPE_DM:
    case rf::NG_TYPE_GG:
    case rf::NG_TYPE_BAG:
        return ffa_strict_leader();
    case rf::NG_TYPE_TEAMDM:
        return strict_leader(rf::multi_tdm_get_red_team_score(), rf::multi_tdm_get_blue_team_score());
    case rf::NG_TYPE_CTF:
        return strict_leader(rf::multi_ctf_get_red_team_score(), rf::multi_ctf_get_blue_team_score());
    case rf::NG_TYPE_TBAG:
        return strict_leader(bagman_get_red_team_score(), bagman_get_blue_team_score());
    case rf::NG_TYPE_SAL:
        return strict_leader(salvage_get_red_team_score(), salvage_get_blue_team_score());
    case rf::NG_TYPE_KOTH:
    case rf::NG_TYPE_DC:
        return strict_leader(multi_koth_get_red_team_score(), multi_koth_get_blue_team_score());
    case rf::NG_TYPE_REV:
        return rev_all_points_permalocked() ? std::optional<int>{0} : std::optional<int>{};
    case rf::NG_TYPE_ESC:
        return esc_all_points_owned_by_one_team() ? hills_owning_team() : std::optional<int>{};
    default:
        return {};
    }
}

// Seconds left on the clock. extend_round_time pushes level.time back, so overtime needs no special
// case: a limitless overtime simply leaves this negative, which no Clutch test accepts.
std::optional<float> clutch_remaining()
{
    if (rf::multi_time_limit <= 0.0f) {
        return {};
    }
    return rf::multi_time_limit - rf::level.time;
}

// An attributable scoring event. Records the go-ahead event of whoever is leading after it: if the
// lead is later lost or tied the record is dropped, so what survives to game end is always the
// eventual winner's own decisive event.
void note_scoring_event(const std::vector<rf::Player*>& scorers)
{
    if (!awards_tracking_active() || gt_uses_rounds() || scorers.empty()) {
        return;
    }
    const std::optional<float> remaining = clutch_remaining();
    if (!remaining) {
        return; // no time limit: nothing to be clutch about
    }

    const std::optional<int> leader = current_strict_leader();
    if (!leader) {
        g_clutch = {};
        return;
    }
    const int scorer_side = clutch_side_of(scorers.front());
    if (*leader != scorer_side) {
        // Somebody else still leads; their own go-ahead event is the one that counts.
        if (!g_clutch.valid || g_clutch.side != *leader) {
            g_clutch = {};
        }
        return;
    }
    if (g_clutch.valid && g_clutch.side == scorer_side) {
        return; // already ahead, this is not the go-ahead event
    }

    g_clutch = {};
    g_clutch.valid = true;
    g_clutch.side = scorer_side;
    g_clutch.remaining = *remaining;
    for (rf::Player* scorer : scorers) {
        const int id = player_id_of(scorer);
        if (id >= 0) {
            g_clutch.player_ids.push_back(static_cast<uint8_t>(id));
        }
    }
}

} // namespace

// -------------------------------------------------------------------------
// Granting
// -------------------------------------------------------------------------

void grant_award(rf::Player* recipient, AwardId id, rf::Player* victim)
{
    if (!recipient || !awards_tracking_active()) {
        return;
    }

    afstats::on_award(recipient, static_cast<uint8_t>(id), victim);

    const int victim_id = player_id_of(victim);
    const uint8_t wire_victim =
        victim_id >= 0 ? static_cast<uint8_t>(victim_id) : award_no_victim;
    const auto notify = [&](rf::Player* target) {
        if (target == rf::local_player) {
            // Listen-server host: render locally instead of routing through the network path.
            awards_client_on_award_received(static_cast<uint8_t>(id), wire_victim);
        }
        else {
            af_send_award(target, static_cast<uint8_t>(id), wire_victim);
        }
    };

    // Bots earn awards, they just have nothing to show them on - but somebody spectating one does.
    if (!recipient->is_bot && !recipient->is_browser) {
        notify(recipient);
    }

    // Mirror to anyone watching the earner, the same way a damage notification is mirrored to the
    // dealer's spectators in entity_damage_hook. af_send_award applies the version gate and the
    // bot/browser skip itself.
    for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
        if (!player.net_data || &player == recipient) {
            continue;
        }
        if (player.spectatee.value_or(nullptr) == recipient) {
            notify(&player);
        }
    }
}

namespace
{

// -------------------------------------------------------------------------
// Individual conditions
// -------------------------------------------------------------------------

void check_riot_control(rf::Player* killer, rf::Player* victim, int weapon_type)
{
    if (!is_riot_weapon(weapon_type)) {
        return;
    }
    AwardPlayerState* state = state_for(killer);
    if (!state) {
        return;
    }
    const int64_t now = now_ms();
    while (!state->riot_kills.empty() && now - state->riot_kills.front() > riot_control_window_ms) {
        state->riot_kills.pop_front();
    }
    state->riot_kills.push_back(now);
    if (static_cast<int>(state->riot_kills.size()) >= riot_control_kills_needed) {
        // Cleared, not trimmed: the next award needs three fresh kills.
        state->riot_kills.clear();
        grant_award(killer, AwardId::riot_control, victim);
    }
}

void check_excellent(rf::Player* killer, rf::Player* victim)
{
    AwardPlayerState* state = state_for(killer);
    if (!state) {
        return;
    }
    const int64_t now = now_ms();
    if (state->has_last_kill && now - state->last_kill_ms <= excellent_window_ms) {
        // Pairs: the first kill is consumed, so three fast kills are one award.
        state->has_last_kill = false;
        grant_award(killer, AwardId::excellent, victim);
        return;
    }
    state->has_last_kill = true;
    state->last_kill_ms = now;
}

void check_streak(rf::Player* killer, rf::Player* victim)
{
    AwardPlayerState* state = state_for(killer);
    if (!state) {
        return;
    }
    ++state->streak;
    if (state->streak == overkill_streak) {
        grant_award(killer, AwardId::overkill, victim);
    }
    else if (state->streak > overkill_streak && state->streak % unstoppable_streak_step == 0) {
        grant_award(killer, AwardId::unstoppable, victim);
    }
}

void check_last_stand(rf::Player* killer, rf::Player* victim, int killer_entity_handle)
{
    rf::Entity* killer_ep = rf::entity_from_handle(killer_entity_handle);
    if (!killer_ep) {
        return;
    }
    if (killer_ep->life < last_stand_life && killer_ep->armor <= 0.0f) {
        grant_award(killer, AwardId::last_stand, victim);
    }
}

// CTF: the victim delivers to their OWN team's flag stand, and only if a capture would actually
// happen there at that instant.
bool ctf_capture_denied(rf::Player* killer, rf::Player* victim, rf::Vector3* out_base)
{
    if (rf::multi_get_game_type() != rf::NG_TYPE_CTF) {
        return false;
    }
    // The carried flag has to be the killer's team's, i.e. the one this victim scores with.
    rf::Player* const red_carrier = rf::multi_ctf_get_red_flag_player();
    rf::Player* const blue_carrier = rf::multi_ctf_get_blue_flag_player();
    const bool carries_red = red_carrier == victim;
    const bool carries_blue = blue_carrier == victim;
    if (!carries_red && !carries_blue) {
        return false;
    }
    const int stolen_flag_team = carries_red ? 0 : 1;
    if (killer->team != stolen_flag_team) {
        return false;
    }

    const int victim_team = victim->team == 0 ? 0 : 1;
    const bool own_flag_home =
        victim_team == 0 ? rf::multi_ctf_is_red_flag_in_base() : rf::multi_ctf_is_blue_flag_in_base();
    if (!own_flag_home && !g_alpine_server_config_active_rules.flag_captures_while_stolen) {
        return false; // a touch would not capture, so nothing was denied
    }

    if (own_flag_home) {
        rf::Vector3 pos{};
        if (victim_team == 0) {
            rf::multi_ctf_get_red_flag_pos(&pos);
        }
        else {
            rf::multi_ctf_get_blue_flag_pos(&pos);
        }
        note_flag_home(victim_team, pos);
    }
    if (!g_flag_home[victim_team].known) {
        return false;
    }
    *out_base = g_flag_home[victim_team].pos;
    return true;
}

bool sal_capture_denied(rf::Player* killer, rf::Player* victim, rf::Vector3* out_base)
{
    if (!gt_is_salvage() || salvage_get_carrier() != victim) {
        return false;
    }
    if (multi_is_team_game_type() && killer->team == victim->team) {
        return false;
    }
    rf::Vector3 red_base{};
    rf::Vector3 blue_base{};
    if (!salvage_get_base_positions(&red_base, &blue_base)) {
        return false;
    }
    // Salvage always allows a capture, so proximity to the victim's own base is the whole test.
    *out_base = victim->team == 0 ? red_base : blue_base;
    return true;
}

void check_capture_denied(rf::Player* killer, rf::Player* victim)
{
    rf::Vector3 base{};
    if (!ctf_capture_denied(killer, victim, &base) && !sal_capture_denied(killer, victim, &base)) {
        return;
    }

    // The carrier's own position at the moment of death; the killer may be anywhere.
    rf::Entity* victim_ep = rf::entity_from_handle(victim->entity_handle);
    if (!victim_ep) {
        return;
    }
    if ((victim_ep->pos - base).len() <= denied_base_radius) {
        grant_award(killer, AwardId::capture_denied, victim);
    }
}

void check_area_denied(rf::Player* killer, rf::Player* victim)
{
    if (!multi_game_type_has_hills(rf::multi_get_game_type())) {
        return;
    }
    const HillOwner victim_team = victim->team == 0 ? HillOwner::HO_Red : HillOwner::HO_Blue;
    const HillOwner killer_team = killer->team == 0 ? HillOwner::HO_Red : HillOwner::HO_Blue;

    for (const HillInfo& hill : g_koth_info.hills) {
        if (!hill.trigger || !hill.handler) {
            continue;
        }
        // Progress only counts when it belongs to the victim's team: steal_dir is who the hill is
        // currently leaning toward, whatever the mode's ownership rules are.
        if (hill.steal_dir != victim_team || hill.capture_milli <= area_denied_capture_milli) {
            continue;
        }
        if (!player_inside_hill_trigger(hill, *victim)) {
            continue;
        }

        bool other_teammate_on_hill = false;
        bool enemy_on_hill = false;
        for (rf::Player& other : SinglyLinkedList{rf::player_list}) {
            if (&other == victim || !player_is_countable(other)) {
                continue;
            }
            const HillOwner other_team = other.team == 0 ? HillOwner::HO_Red : HillOwner::HO_Blue;
            if (other_team != victim_team && other_team != killer_team) {
                continue;
            }
            if (!player_inside_hill_trigger(hill, other)) {
                continue;
            }
            if (other_team == victim_team) {
                other_teammate_on_hill = true;
                break;
            }
            enemy_on_hill = true;
        }
        if (other_teammate_on_hill || enemy_on_hill) {
            continue;
        }

        grant_award(killer, AwardId::area_denied, victim);
        return;
    }
}

// Nemesis is a private status message pair, never an award; Revenge is the award for breaking one.
void update_nemesis(rf::Player* killer, rf::Player* victim)
{
    const int killer_id = player_id_of(killer);
    const int victim_id = player_id_of(victim);
    if (killer_id < 0 || victim_id < 0) {
        return;
    }

    NemesisPair& forward = g_nemesis[nemesis_key(static_cast<uint8_t>(killer_id), static_cast<uint8_t>(victim_id))];
    if (forward.count < 0xFF) {
        ++forward.count;
    }
    if (forward.count == nemesis_kills_needed && !forward.active) {
        forward.active = true;
        af_broadcast_automated_chat_msg(
            std::format("{} is dominating {}!", killer->name.c_str(), victim->name.c_str()));
        // In addition to the broadcast, not instead of it. The `active` latch is what keeps this to
        // once per relationship: it only clears when the pair is erased (revenge, leave, level
        // end), so a re-established domination can grant again.
        grant_award(killer, AwardId::dominating, victim);
    }

    const uint16_t reverse_key = nemesis_key(static_cast<uint8_t>(victim_id), static_cast<uint8_t>(killer_id));
    auto reverse_it = g_nemesis.find(reverse_key);
    if (reverse_it == g_nemesis.end()) {
        return;
    }
    if (reverse_it->second.active) {
        g_nemesis.erase(reverse_it);
        af_broadcast_automated_chat_msg(
            std::format("{} got revenge on {}!", killer->name.c_str(), victim->name.c_str()));
        grant_award(killer, AwardId::revenge, victim);
    }
    else {
        reverse_it->second.count = 0;
    }
}

void clear_nemesis_pairs_for(uint8_t player_id)
{
    for (auto it = g_nemesis.begin(); it != g_nemesis.end();) {
        const uint8_t killer_id = static_cast<uint8_t>(it->first >> 8);
        const uint8_t victim_id = static_cast<uint8_t>(it->first & 0xFF);
        if (killer_id == player_id || victim_id == player_id) {
            it = g_nemesis.erase(it);
        }
        else {
            ++it;
        }
    }
}

// -------------------------------------------------------------------------
// Client display
// -------------------------------------------------------------------------

struct AwardDisplay
{
    const char* text;
    // std::format pattern naming the opposing player, for the awards whose callout does. Null for
    // the rest, and unused when the id did not resolve to a connected player.
    const char* victim_text;
    int custom_sound; // per-award on purpose: real files can replace the placeholder individually
};

constexpr std::array<AwardDisplay, award_id_count> award_display{{
    {"TOASTY!", nullptr, custom_sound_id::af_achievement},
    {"MASSACRE!", nullptr, custom_sound_id::af_achievement},
    {"RIOT CONTROL!", nullptr, custom_sound_id::af_achievement},
    {"2FER!", nullptr, custom_sound_id::af_achievement},
    {"IMPRESSIVE!", nullptr, custom_sound_id::af_achievement},
    {"EXCELLENT!", nullptr, custom_sound_id::af_achievement},
    {"OVERKILL!", nullptr, custom_sound_id::af_achievement},
    {"UNSTOPPABLE!", nullptr, custom_sound_id::af_achievement},
    {"LAST STAND!", nullptr, custom_sound_id::af_achievement},
    {"FLAG RUNNER!", nullptr, custom_sound_id::af_achievement},
    {"CAPTURE DENIED!", nullptr, custom_sound_id::af_achievement},
    {"AREA DENIED!", nullptr, custom_sound_id::af_achievement},
    {"BAG CHECK!", nullptr, custom_sound_id::af_achievement},
    {"CLUTCH!", nullptr, custom_sound_id::af_achievement},
    {"REVENGE!", "REVENGE ON {}!", custom_sound_id::af_achievement},
    {"DOMINATING!", "DOMINATING {}!", custom_sound_id::af_achievement},
    {"YOU'RE ON A RAMPAGE!", nullptr, custom_sound_id::af_achievement},
}};

constexpr int award_display_seconds = 3;
// The notification slot fades for 500 ms after its 3 s, so the next award waits for both.
constexpr int award_slot_hold_ms = award_display_seconds * 1000 + 500;
// A burst of awards is worth showing; an unbounded backlog is not.
constexpr size_t award_queue_cap = 8;

// The text is resolved when the award arrives, not when it reaches the front of the queue: a named
// player may well have left by then.
struct QueuedAward
{
    std::string text;
    int custom_sound = custom_sound_id::af_achievement;
};

std::deque<QueuedAward> g_award_queue;
rf::Timestamp g_award_slot_busy;

ConsoleCommand2 awards_display_cmd{
    "cl_awards",
    []() {
        g_alpine_game_config.show_awards = !g_alpine_game_config.show_awards;
        rf::console::print("Display of earned awards is {}",
                           g_alpine_game_config.show_awards ? "enabled" : "disabled");
    },
    "Toggle whether to display awards you earn in multiplayer",
    "cl_awards",
};

} // namespace

// -------------------------------------------------------------------------
// Server entry points
// -------------------------------------------------------------------------

void awards_on_kill(rf::Player* victim, rf::Player* killer, int weapon_type, bool splash,
                    int killer_entity_handle)
{
    if (!awards_tracking_active() || !victim) {
        return;
    }

    // Victim-side resets run for every death, including suicides and world deaths.
    if (AwardPlayerState* victim_state = state_for(victim)) {
        victim_state->streak = 0;
        victim_state->hit_chain = 0;
        victim_state->shot_pending = false;
        victim_state->clean_steal = false;
    }

    sample_ctf_flag_homes();

    if (!is_enemy_kill(killer, victim)) {
        return;
    }

    // Massacre: bank the kill against the detonation currently being resolved.
    if (splash && kill_attribution_in_splash_scope() && g_detonation_depth > 0) {
        const int killer_id = player_id_of(killer);
        if (killer_id >= 0) {
            DetonationFrame& frame = g_detonation_stack[g_detonation_depth - 1];
            auto it = std::find_if(frame.killers.begin(), frame.killers.end(),
                                   [&](const DetonationKills& k) { return k.killer_id == killer_id; });
            if (it == frame.killers.end()) {
                frame.killers.push_back({static_cast<uint8_t>(killer_id), 1, victim});
            }
            else {
                ++it->count;
                it->last_victim = victim;
            }
        }
    }

    // 2fer: kills of one lag-compensated sniper/rail bolt.
    if (!splash && g_shot_scope.active && g_shot_scope.shooter_id == player_id_of(killer)
        && is_sniper_or_rail(g_shot_scope.weapon_type)) {
        ++g_shot_scope.enemy_kills;
        g_shot_scope.last_victim = victim;
    }

    const int killer_id = player_id_of(killer);
    if (killer_id >= 0 && mutators_player_is_on_fire(static_cast<uint8_t>(killer_id))) {
        grant_award(killer, AwardId::toasty, victim);
    }

    check_riot_control(killer, victim, weapon_type);
    check_excellent(killer, victim);
    check_streak(killer, victim);
    check_last_stand(killer, victim, killer_entity_handle);
    check_capture_denied(killer, victim);
    check_area_denied(killer, victim);
    update_nemesis(killer, victim);

    // Frags only move the score in the frag-scored game types; elsewhere a kill is not a scoring
    // event and must not overwrite the real decisive one.
    const rf::NetGameType game_type = rf::multi_get_game_type();
    if (game_type == rf::NG_TYPE_DM || game_type == rf::NG_TYPE_TEAMDM || game_type == rf::NG_TYPE_GG) {
        note_scoring_event({killer});
    }
}

void awards_on_weapon_fired(rf::Player* player, int weapon_type)
{
    if (!awards_tracking_active()) {
        return;
    }
    AwardPlayerState* state = state_for(player);
    if (!state) {
        return;
    }
    if (!is_sniper_or_rail(weapon_type)) {
        // Any other shot ends the chain: Impressive is consecutive sniper/rail hits, nothing else.
        state->shot_pending = false;
        state->hit_chain = 0;
        return;
    }
    if (state->shot_pending) {
        state->hit_chain = 0; // the previous bolt never landed
    }
    state->shot_pending = true;
}

void awards_on_direct_hit(rf::Player* attacker, rf::Player* victim, int weapon_type)
{
    if (!awards_tracking_active() || !victim || !is_sniper_or_rail(weapon_type)) {
        return;
    }
    AwardPlayerState* state = state_for(attacker);
    if (!state) {
        return;
    }
    state->shot_pending = false;
    if (!is_enemy_kill(attacker, victim)) {
        state->hit_chain = 0; // a teammate is a miss as far as this chain is concerned
        return;
    }
    ++state->hit_chain;
    if (state->hit_chain % 2 == 0) {
        grant_award(attacker, AwardId::impressive, victim);
    }
}

AwardsShotScope::AwardsShotScope(rf::Entity* shooter, rf::Weapon* wp)
{
    if (!awards_tracking_active() || !shooter || !wp) {
        return;
    }
    rf::Player* shooter_player = rf::player_from_entity_handle(shooter->handle);
    const int shooter_id = player_id_of(shooter_player);
    if (shooter_id < 0) {
        return;
    }
    active_ = true;
    g_shot_scope = {true, shooter_id, wp->info_index, 0, nullptr};
}

AwardsShotScope::~AwardsShotScope()
{
    if (!active_) {
        return;
    }
    const ShotScopeState finished = g_shot_scope;
    // These do not nest in practice, but clearing rather than restoring keeps a stray nested scope
    // from being credited with the outer shot's kills.
    g_shot_scope = {};
    if (finished.enemy_kills >= 2) {
        grant_award(rf::multi_find_player_by_id(static_cast<uint8_t>(finished.shooter_id)),
                    AwardId::twofer, finished.last_victim);
    }
}

void awards_detonation_begin()
{
    ++g_detonation_depth;
    if (static_cast<int>(g_detonation_stack.size()) < g_detonation_depth) {
        g_detonation_stack.resize(g_detonation_depth);
    }
    g_detonation_stack[g_detonation_depth - 1].killers.clear();
}

void awards_detonation_end()
{
    if (g_detonation_depth <= 0) {
        return;
    }
    // Read out before leaving the frame: granting is not allowed to be looking at stack storage a
    // later scope could reuse.
    std::vector<DetonationKills> winners;
    for (const DetonationKills& entry : g_detonation_stack[g_detonation_depth - 1].killers) {
        if (entry.count >= massacre_kills_needed) {
            winners.push_back(entry);
        }
    }
    --g_detonation_depth;
    for (const DetonationKills& entry : winners) {
        grant_award(rf::multi_find_player_by_id(entry.killer_id), AwardId::massacre, entry.last_victim);
    }
}

void awards_on_ctf_flag_taken(rf::Player* player, bool red_flag, bool from_base, const rf::Vector3& pos)
{
    if (!awards_tracking_active()) {
        return;
    }
    if (from_base) {
        // A flag sitting at home is on its stand, so this is the one position that is always the
        // stand even after the flag leaves it.
        note_flag_home(red_flag ? 0 : 1, pos);
    }
    if (AwardPlayerState* state = state_for(player)) {
        // Only a steal starts a run; picking one off the ground does not.
        state->clean_steal = from_base;
    }
}

void awards_on_ctf_flag_dropped(rf::Player* player)
{
    if (AwardPlayerState* state = state_for(player)) {
        state->clean_steal = false;
    }
}

void awards_on_ctf_capture(rf::Player* player)
{
    if (!awards_tracking_active()) {
        return;
    }
    AwardPlayerState* state = state_for(player);
    if (state && state->clean_steal) {
        state->clean_steal = false;
        grant_award(player, AwardId::flag_runner);
    }
    note_scoring_event({player});
}

void awards_on_sal_flag_taken(rf::Player* player, bool from_spawn)
{
    if (!awards_tracking_active()) {
        return;
    }
    if (AwardPlayerState* state = state_for(player)) {
        state->clean_steal = from_spawn;
    }
}

void awards_on_sal_flag_dropped(rf::Player* player)
{
    if (AwardPlayerState* state = state_for(player)) {
        state->clean_steal = false;
    }
}

void awards_on_sal_capture(rf::Player* player)
{
    if (!awards_tracking_active()) {
        return;
    }
    AwardPlayerState* state = state_for(player);
    if (state && state->clean_steal) {
        state->clean_steal = false;
        grant_award(player, AwardId::flag_runner);
    }
    note_scoring_event({player});
}

void awards_on_bagman_pickup(rf::Player* player)
{
    if (!awards_tracking_active()) {
        return;
    }
    if (AwardPlayerState* state = state_for(player)) {
        state->has_bag_pickup = true;
        state->bag_pickup_ms = now_ms();
    }
}

void awards_on_bagman_carrier_death(rf::Player* carrier, rf::Player* killer)
{
    if (!awards_tracking_active() || !carrier || !killer || killer == carrier) {
        return;
    }
    if (gt_is_tbag() && killer->team == carrier->team) {
        return;
    }
    const AwardPlayerState* state = state_for(carrier);
    if (!state || !state->has_bag_pickup) {
        return;
    }
    if (now_ms() - state->bag_pickup_ms <= bag_check_window_ms) {
        grant_award(killer, AwardId::bag_check, carrier);
    }
}

void awards_on_bagman_score_tick(rf::Player* carrier)
{
    note_scoring_event({carrier});
}

void awards_on_hill_score_tick(const HillInfo& hill, int team)
{
    if (!awards_tracking_active()) {
        return;
    }
    const auto owner = static_cast<HillOwner>(team);
    if (owner != HillOwner::HO_Red && owner != HillOwner::HO_Blue) {
        return;
    }
    // The tick is the team's, but Clutch is per player: everyone holding the hill when it scored.
    std::vector<rf::Player*> scorers;
    for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
        if (!player_is_countable(player)) {
            continue;
        }
        const HillOwner player_team = player.team == 0 ? HillOwner::HO_Red : HillOwner::HO_Blue;
        if (player_team != owner || !player_inside_hill_trigger(hill, player)) {
            continue;
        }
        scorers.push_back(&player);
    }
    if (!scorers.empty()) {
        note_scoring_event(scorers);
    }
}

void awards_on_hill_owner_change(const HillInfo&, int team, const std::vector<uint8_t>& player_ids)
{
    if (!awards_tracking_active()) {
        return;
    }
    const auto owner = static_cast<HillOwner>(team);
    if (owner != HillOwner::HO_Red && owner != HillOwner::HO_Blue) {
        return;
    }
    std::vector<rf::Player*> scorers;
    for (uint8_t id : player_ids) {
        if (rf::Player* player = rf::multi_find_player_by_id(id)) {
            scorers.push_back(player);
        }
    }
    if (scorers.empty()) {
        return;
    }
    note_scoring_event(scorers);
}

void awards_note_game_end_type(bool time_limit)
{
    static_cast<void>(time_limit);
    // Both limits are decisive ends; a vote or level logic ending the game never latches this, so
    // Clutch cannot fire for one.
    g_clutch_end_type_known = true;
}

void awards_on_game_end()
{
    if (!rf::is_multi || !rf::is_server) {
        return;
    }
    if (!g_clutch_end_type_known || !g_clutch.valid || gt_uses_rounds()) {
        g_clutch = {};
        g_clutch_end_type_known = false;
        return;
    }

    const std::optional<int> winner = current_strict_leader();
    const bool in_window = g_clutch.remaining >= 0.0f && g_clutch.remaining < clutch_remaining_seconds;
    if (winner && *winner == g_clutch.side && in_window) {
        for (uint8_t id : g_clutch.player_ids) {
            grant_award(rf::multi_find_player_by_id(id), AwardId::clutch);
        }
    }

    g_clutch = {};
    g_clutch_end_type_known = false;
}

void awards_on_player_destroy(rf::Player* player)
{
    const int id = player_id_of(player);
    if (id < 0) {
        return;
    }
    player_states()[id] = AwardPlayerState{};
    clear_nemesis_pairs_for(static_cast<uint8_t>(id));

    // The decisive record holds ids, not pointers; a departing scorer must not be granted through
    // whoever inherits its id.
    std::erase(g_clutch.player_ids, static_cast<uint8_t>(id));
    if (g_clutch.valid && g_clutch.player_ids.empty()) {
        g_clutch = {};
    }
}

void awards_level_init()
{
    std::fill(player_states().begin(), player_states().end(), AwardPlayerState{});
    g_nemesis.clear();
    g_detonation_stack.clear();
    g_detonation_depth = 0;
    g_shot_scope = {};
    g_flag_home[0] = {};
    g_flag_home[1] = {};
    g_clutch = {};
    g_clutch_end_type_known = false;
    g_award_queue.clear();
    g_award_slot_busy.invalidate();
}

// -------------------------------------------------------------------------
// Client entry points
// -------------------------------------------------------------------------

void awards_client_on_award_received(uint8_t award_id, uint8_t victim_player_id)
{
    if (award_id >= award_id_count) {
        return; // fail closed on anything this build does not know
    }
    if (!g_alpine_game_config.show_awards || rf::is_dedicated_server) {
        return;
    }
    if (g_award_queue.size() >= award_queue_cap) {
        return;
    }

    const AwardDisplay& display = award_display[award_id];
    std::string text = display.text;
    if (display.victim_text && victim_player_id != award_no_victim) {
        // An unresolvable id (the named player left in the interim) falls back to the plain text
        // rather than dropping the award.
        if (rf::Player* victim = rf::multi_find_player_by_id(victim_player_id)) {
            const char* victim_name = victim->name.c_str();
            text = std::vformat(display.victim_text, std::make_format_args(victim_name));
        }
    }

    g_award_queue.push_back({std::move(text), display.custom_sound});
}

void awards_client_do_frame()
{
    if (g_award_queue.empty()) {
        return;
    }
    if (g_award_slot_busy.valid() && !g_award_slot_busy.elapsed()) {
        return; // the previous award still owns the slot
    }

    QueuedAward award = std::move(g_award_queue.front());
    g_award_queue.pop_front();

    hud_notification_show(std::move(award.text), award_display_seconds, HudNotificationType::Award, true);
    play_local_sound_2d(static_cast<uint16_t>(get_custom_sound_id(award.custom_sound)), 0, 1.0f);
    g_award_slot_busy.set(award_slot_hold_ms);
}

void awards_do_patch()
{
    awards_display_cmd.register_cmd();
}
