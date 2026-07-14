#include <algorithm>
#include <format>
#include <vector>
#include <xlog/xlog.h>
#include <common/utils/list-utils.h>
#include "wipeout.h"
#include "rounds.h"
#include "gametype.h"
#include "multi.h"
#include "server.h"
#include "server_internal.h"
#include "alpine_packets.h"
#include "../hud/hud.h"
#include "../sound/sound.h"
#include "../rf/multi.h"
#include "../rf/entity.h"
#include "../rf/object.h"
#include "../rf/item.h"
#include "../rf/player/player.h"
#include "../rf/level.h"
#include "../rf/gameseq.h"

namespace
{

// Best-of-7: match is decided once a team's lead can't be caught, with sudden
// death if the seven rounds finish level (possible because double-wipes are
// no-contest rounds that advance the count without changing either score).
constexpr int WIPEOUT_MATCH_ROUNDS = 7;

struct WipeoutInfo
{
    int red_team_score = 0;   // rounds won by red
    int blue_team_score = 0;  // rounds won by blue
    int rounds_completed = 0; // rounds finished this match (incl. no-contest)
    bool match_over = false;
    int match_winner_team = -1; // -1 none, 0 red, 1 blue
};

WipeoutInfo g_info;

// True while we call multi_spawn_player_server_side ourselves (round-start batch
// / auto-respawn). Lets the spawn gate permit those regardless of round state.
bool g_internal_spawn_in_progress = false;

// Winner team latched by should_end_round at the wipe instant, consumed by
// on_round_end a frame later. -1 == double wipe (no contest).
int g_pending_winner_team = -1;

// Latches once both teams have had a live player this round; keeps a round from
// "ending" during the round-1 spawn race before everyone is in.
bool g_round_had_both_teams = false;

bool player_has_alive_entity(rf::Player* p)
{
    if (!p) return false;
    if (p->is_browser) return false;
    if (p->is_spectator) return false; // spectators never count toward wipe detection
    rf::Entity* ep = rf::entity_from_handle(p->entity_handle);
    if (!ep) return false;
    if (rf::entity_is_dying(ep)) return false;
    return true;
}

// A player has finished loading when the engine set NPF_CLIENT_IS_LOADED. The
// listen-server host is always considered loaded.
bool player_is_loaded(rf::Player* p)
{
    if (!p) return false;
    if (p == rf::local_player) return true;
    if (!p->net_data) return false;
    return (p->net_data->flags & rf::NPF_CLIENT_IS_LOADED) != 0;
}

int count_team_alive(int team)
{
    int n = 0;
    for (rf::Player& p : SinglyLinkedList{rf::player_list}) {
        if (p.is_browser) continue;
        if (p.team != team) continue;
        if (player_has_alive_entity(&p)) ++n;
    }
    return n;
}

// Connected, non-browser, non-spectator players on a team without a live entity:
// dead and awaiting respawn, or a late joiner not yet in the round. Spectators
// are excluded so they don't inflate the alive/waiting HUD labels.
int count_team_waiting(int team)
{
    int n = 0;
    for (rf::Player& p : SinglyLinkedList{rf::player_list}) {
        if (p.is_browser) continue;
        if (p.is_spectator) continue;
        if (p.team != team) continue;
        if (!player_has_alive_entity(&p)) ++n;
    }
    return n;
}

bool match_is_decided()
{
    const int remaining = std::max(0, WIPEOUT_MATCH_ROUNDS - g_info.rounds_completed);
    const int r = g_info.red_team_score;
    const int b = g_info.blue_team_score;
    return (r > b + remaining) || (b > r + remaining);
}

// Hide every level item and destroy any dropped weapons so the arena stays
// item-free. Mirrors lms_reset_world_items but hides instead of restoring. The
// engine's periodic visibility broadcast replicates the hidden state to clients.
void hide_all_items()
{
    if (!rf::is_server) return;

    rf::Item* it = rf::item_list.next;
    while (it && it != &rf::item_list) {
        rf::Item* next = it->next;
        const uint32_t flags = it->item_flags;
        const bool is_dropped = (flags & rf::IF_DROPPED) != 0;
        const bool is_ctf_flag = (flags & rf::IF_CTF_FLAG) != 0;

        if (is_dropped) {
            rf::send_item_apply_packet(nullptr, it->handle, 0, -1, -1, -1);
            rf::obj_flag_dead(it);
        }
        else if (!is_ctf_flag) {
            it->respawn_next.invalidate();
            rf::obj_hide(it);
        }

        it = next;
    }
}

// === Round callbacks ============================================

bool wipeout_can_round_start()
{
    // Need at least one loaded player on EACH team for a real team round.
    // Spectators carry a team value but never spawn, so a round gated only on
    // their presence would start unwinnable and hang; exclude them here.
    int red_loaded = 0;
    int blue_loaded = 0;
    for (rf::Player& p : SinglyLinkedList{rf::player_list}) {
        if (p.is_browser) continue;
        if (p.is_spectator) continue;
        if (!player_is_loaded(&p)) continue;
        if (p.team == rf::TEAM_RED) ++red_loaded;
        else ++blue_loaded;
    }
    return red_loaded >= 1 && blue_loaded >= 1;
}

void wipeout_on_round_begin()
{
    if (!rf::is_server) return;

    g_round_had_both_teams = false;
    g_pending_winner_team = -1;

    g_internal_spawn_in_progress = true;
    int spawned = 0;
    for (rf::Player& p : SinglyLinkedList{rf::player_list}) {
        if (p.is_browser) continue;

        // Fresh round: clear elimination, participation, the per-round death
        // counter (resets the escalating delay) and any lingering respawn timer
        // from the between-round cleanup kill (otherwise it would block the
        // round-start spawn below).
        p.round_is_out = false;
        p.round_participated = false;
        p.wipeout_round_deaths = 0;
        p.wipeout_spawned_this_round = false;
        p.respawn_timer.invalidate();

        if (!player_is_loaded(&p)) continue;

        if (!player_has_alive_entity(&p)) {
            // First spawn of the round uses standard (TDM) spawn logic because
            // wipeout_spawned_this_round is still false at selection time; the
            // spawn hook flips it to true once the point is chosen.
            rf::multi_spawn_player_server_side(&p);
            ++spawned;
        }
        else {
            // Already holds a live entity (e.g. spawned during the pre-round
            // Inactive window). That counts as this round's first spawn, so
            // their next death-respawn correctly clusters on teammates.
            p.wipeout_spawned_this_round = true;
        }
    }
    g_internal_spawn_in_progress = false;

    // Arena stays item-free every round.
    hide_all_items();

    xlog::info("Wipeout: round {} begin, {} spawned (R {} - B {})",
               g_info.rounds_completed + 1, spawned, g_info.red_team_score, g_info.blue_team_score);
}

bool wipeout_should_end_round(rf::Player** out_winner)
{
    if (!rf::is_server) return false;
    if (out_winner) *out_winner = nullptr; // team-based, not player-based

    const int red_alive = count_team_alive(rf::TEAM_RED);
    const int blue_alive = count_team_alive(rf::TEAM_BLUE);

    // Don't evaluate the wipe condition until both teams have genuinely had a
    // live player this round (guards the round-start spawn race).
    if (!g_round_had_both_teams) {
        if (red_alive > 0 && blue_alive > 0) g_round_had_both_teams = true;
        return false;
    }

    if (red_alive > 0 && blue_alive > 0) return false; // both still standing

    // A team has been wiped. Winner is whoever is still up; both down in the
    // same frame (same event) is a no-contest.
    if (red_alive == 0 && blue_alive == 0) g_pending_winner_team = -1;
    else g_pending_winner_team = (red_alive > 0) ? rf::TEAM_RED : rf::TEAM_BLUE;

    return true;
}

void announce_round_sounds(int winner_team)
{
    // Winners hear the stock "winner" cue; everyone else hears "match over".
    for (rf::Player& p : SinglyLinkedList{rf::player_list}) {
        if (p.is_browser) continue;
        const bool on_winning = (p.team == winner_team);
        if (on_winning) {
            if (&p == rf::local_player) {
                play_local_sound_2d(static_cast<uint16_t>(stock_sound_id::ann_winner), 0, 1.0f);
            }
            else {
                send_sound_packet_throwaway(&p, stock_sound_id::ann_winner);
            }
        }
        else {
            af_send_play_custom_sound(custom_sound_id::ann_match_over, &p);
        }
    }
}

void wipeout_on_round_end(rf::Player* /*winner*/, RoundEndReason /*reason*/)
{
    if (!rf::is_server) return;

    ++g_info.rounds_completed;
    const int winner_team = g_pending_winner_team;
    g_pending_winner_team = -1;

    if (winner_team == rf::TEAM_RED) ++g_info.red_team_score;
    else if (winner_team == rf::TEAM_BLUE) ++g_info.blue_team_score;

    if (winner_team == rf::TEAM_RED || winner_team == rf::TEAM_BLUE) {
        const char* tname = (winner_team == rf::TEAM_RED) ? "Red" : "Blue";
        af_broadcast_hud_notification(
            std::format("{} team wins round {}!  (Red {} - {} Blue)",
                        tname, g_info.rounds_completed, g_info.red_team_score, g_info.blue_team_score),
            3, static_cast<int>(HudNotificationType::Round), true);
        announce_round_sounds(winner_team);
    }
    else {
        af_broadcast_hud_notification(
            std::format("Round {} was a double wipe - no point awarded.  (Red {} - {} Blue)",
                        g_info.rounds_completed, g_info.red_team_score, g_info.blue_team_score),
            3, static_cast<int>(HudNotificationType::Round), true);
    }

    // Announce the match result up front if this round decided it; the rounds
    // system rotates the level once the celebration window closes (governed by
    // our is_match_over arbiter below).
    if (match_is_decided()) {
        g_info.match_over = true;
        g_info.match_winner_team =
            (g_info.red_team_score > g_info.blue_team_score) ? rf::TEAM_RED : rf::TEAM_BLUE;
        const char* tname = (g_info.match_winner_team == rf::TEAM_RED) ? "Red" : "Blue";
        af_broadcast_hud_notification(
            std::format("{} team wins the match {} - {}!",
                        tname, g_info.red_team_score, g_info.blue_team_score),
            4, static_cast<int>(HudNotificationType::Round), true);
    }
}

void wipeout_on_round_cleanup()
{
    if (!rf::is_server) return;
    if (!gt_is_wipeout()) return;

    // Kill any survivors through the full death pipeline so the next round
    // starts everyone fresh. Clear killer info first so no stale obituary fires.
    for (rf::Player& p : SinglyLinkedList{rf::player_list}) {
        if (p.is_browser) continue;
        p.round_is_out = true;
        rf::Entity* ep = rf::entity_from_handle(p.entity_handle);
        if (ep && !rf::entity_is_dying(ep)) {
            ep->killer_handle = 0;
            ep->killer_netid = -1;
            rf::entity_maybe_die(ep);
        }
    }

    hide_all_items();
}

bool wipeout_is_match_over()
{
    return match_is_decided();
}

// Only reached if a server admin overrides the untimed default (round_time > 0)
// and a round hits the clock. Resolve by who has more players standing; a tie in
// alive count is a no-contest. Sets the team latch the way should_end_round would
// and returns null (Wipeout resolves by team, not by a single player).
rf::Player* wipeout_resolve_timeout_winner()
{
    const int red_alive = count_team_alive(rf::TEAM_RED);
    const int blue_alive = count_team_alive(rf::TEAM_BLUE);
    if (red_alive > blue_alive) g_pending_winner_team = rf::TEAM_RED;
    else if (blue_alive > red_alive) g_pending_winner_team = rf::TEAM_BLUE;
    else g_pending_winner_team = -1;
    return nullptr;
}

void wipeout_on_late_join(rf::Player* player)
{
    // Sit the late joiner out until the next round begins; wipeout_can_player_spawn
    // denies their spawn attempts in the meantime, and on_round_begin re-includes
    // them alongside everyone else.
    if (player) player->round_is_out = true;
}

} // namespace

void wipeout_level_init()
{
    // Real level boundary == a new match.
    g_info = WipeoutInfo{};
}

void wipeout_level_init_post()
{
    if (!rf::is_server) return;
    if (!gt_is_wipeout()) return;

    RoundCallbacks cb{};
    cb.on_round_begin = &wipeout_on_round_begin;
    cb.on_round_end = &wipeout_on_round_end;
    cb.on_round_cleanup = &wipeout_on_round_cleanup;
    cb.should_end_round = &wipeout_should_end_round;
    cb.can_round_start = &wipeout_can_round_start;
    cb.on_late_join = &wipeout_on_late_join;
    cb.is_match_over = &wipeout_is_match_over;
    cb.resolve_timeout_winner = &wipeout_resolve_timeout_winner;
    rounds_register_callbacks(cb);

    for (rf::Player& p : SinglyLinkedList{rf::player_list}) {
        p.round_is_out = false;
        p.round_participated = false;
        p.wipeout_round_deaths = 0;
        p.wipeout_spawned_this_round = false;
    }

    hide_all_items();
}

void wipeout_do_frame()
{
    if (!rf::is_server) return;
    if (!gt_is_wipeout()) return;
    // Only act during live gameplay; rounds_do_frame stops pumping the state
    // machine outside GS_GAMEPLAY, so rounds_is_active() can linger through a
    // mid-round level-change limbo window without this guard.
    if (rf::gameseq_get_state() != rf::GameState::GS_GAMEPLAY) return;
    if (!rounds_is_active()) return;

    // Repeated mid-round respawns: bring back any player whose escalating delay
    // has elapsed. Players stay in the "waiting" pool only for the length of
    // their respawn_timer, which is what makes a full-team wipe achievable.
    g_internal_spawn_in_progress = true;
    for (rf::Player& p : SinglyLinkedList{rf::player_list}) {
        if (p.is_browser) continue;

        if (player_has_alive_entity(&p)) {
            p.round_participated = true;
            continue;
        }

        if (p.round_is_out) continue;              // late joiner / between rounds
        if (!player_is_loaded(&p)) continue;
        if (p.respawn_timer.valid() && !p.respawn_timer.elapsed()) continue; // still waiting out the delay

        rf::multi_spawn_player_server_side(&p);
    }
    g_internal_spawn_in_progress = false;
}

bool wipeout_can_player_spawn(rf::Player* player)
{
    if (!gt_is_wipeout()) return true; // not our problem
    if (!player) return true;
    if (g_internal_spawn_in_progress) return true;

    // Level still initializing — allow the engine's normal spawn flow so players
    // have entities by the time round 1 begins.
    if (rounds_get_state() == RoundState::Inactive) return true;

    const bool between_rounds = rounds_is_between_rounds();
    if (between_rounds || player->round_is_out) {
        // Throttle the notice: the client re-requests a spawn every frame while
        // the fire button is held, which would otherwise spam chat.
        if (!player->wipeout_waiting_msg_timer.valid() || player->wipeout_waiting_msg_timer.elapsed()) {
            af_send_automated_chat_msg(
                between_rounds ? "Wait - the next round is starting shortly."
                               : "You're waiting for the next round to begin.",
                player);
            player->wipeout_waiting_msg_timer.set(3000);
        }
        return false;
    }

    // Otherwise allowed; the escalating per-death delay is enforced by the
    // server-side respawn_timer check in multi_spawn_player_server_side_hook.
    return true;
}

void wipeout_on_player_disconnect(rf::Player* /*player*/)
{
    // Nothing to clean up — alive/waiting counts derive from the live list, and
    // a round-end transition (if a team just emptied) is caught next tick.
}

bool wipeout_is_subsequent_spawn(rf::Player* player)
{
    return gt_is_wipeout() && player && player->wipeout_spawned_this_round;
}

int wipeout_get_red_team_score()
{
    return g_info.red_team_score;
}

int wipeout_get_blue_team_score()
{
    return g_info.blue_team_score;
}

void wipeout_set_red_team_score(int v)
{
    if (rf::is_server) return; // server is authoritative; clients receive via packet
    g_info.red_team_score = v;
}

void wipeout_set_blue_team_score(int v)
{
    if (rf::is_server) return;
    g_info.blue_team_score = v;
}

int wipeout_count_team_alive(int team)
{
    return count_team_alive(team);
}

int wipeout_count_team_waiting(int team)
{
    return count_team_waiting(team);
}
