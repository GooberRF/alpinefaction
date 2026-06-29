#include <algorithm>
#include <format>
#include <random>
#include <vector>
#include <xlog/xlog.h>
#include <patch_common/FunHook.h>
#include <common/utils/list-utils.h>
#include "lms.h"
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
#include "../main/main.h"

namespace
{

// True while we're calling multi_spawn_player_server_side ourselves from the
// round-start respawn batch / auto-spawn. Allows our spawn gate to permit
// those calls regardless of the per-player out flag.
bool g_lms_internal_spawn_in_progress = false;

// Last alive-count we chat-broadcast a milestone for this round.
int g_last_alive_announced = -1;


bool player_has_alive_entity(rf::Player* p)
{
    if (!p) return false;
    if (p->is_browser) return false;
    rf::Entity* ep = rf::entity_from_handle(p->entity_handle);
    if (!ep) return false;
    if (rf::entity_is_dying(ep)) return false;
    return true;
}

// A player has "finished loading" the level when the engine has set
// NPF_CLIENT_IS_LOADED. The listen-server host is always considered loaded.
bool player_is_loaded(rf::Player* p)
{
    if (!p) return false;
    if (p == rf::local_player) return true;
    if (!p->net_data) return false;
    return (p->net_data->flags & rf::NPF_CLIENT_IS_LOADED) != 0;
}

// All non-browser players (humans + bots). Bots are intentional LMS round
// participants — round-start eligibility, alive counts, and tiebreaks all
// treat them as first-class.
std::vector<rf::Player*> collect_round_participants()
{
    std::vector<rf::Player*> out;
    for (rf::Player& p : SinglyLinkedList{rf::player_list}) {
        if (p.is_browser) continue;
        out.push_back(&p);
    }
    return out;
}

int count_alive()
{
    int n = 0;
    for (rf::Player& p : SinglyLinkedList{rf::player_list}) {
        if (p.is_browser) continue;
        if (p.round_is_out) continue;
        if (!player_has_alive_entity(&p)) continue;
        ++n;
    }
    return n;
}

// Players who actually participated in the current round (spawned at least
// once). Used to determine round-end eligibility — a round can't "end" while
// players are still mid-connect / mid-load.
int count_round_participants()
{
    int n = 0;
    for (rf::Player& p : SinglyLinkedList{rf::player_list}) {
        if (p.is_browser) continue;
        if (!p.round_participated) continue;
        ++n;
    }
    return n;
}

rf::Player* find_only_alive()
{
    rf::Player* found = nullptr;
    for (rf::Player& p : SinglyLinkedList{rf::player_list}) {
        if (p.is_browser) continue;
        if (p.round_is_out) continue;
        if (!player_has_alive_entity(&p)) continue;
        if (found) {
            // more than one alive
            return nullptr;
        }
        found = &p;
    }
    return found;
}

// === Round callbacks ============================================

void lms_on_round_begin()
{
    if (!rf::is_server) return;

    g_last_alive_announced = -1;

    // Reset per-round state for all connected participants (humans + bots).
    // Only spawn players who have actually finished loading the level —
    // late-loaders are picked up by lms_do_frame's auto-spawn pass once
    // they're ready.
    std::vector<rf::Player*> participants = collect_round_participants();

    g_lms_internal_spawn_in_progress = true;
    int spawned = 0;
    for (rf::Player* p : participants) {
        p->round_is_out = false;
        p->round_participated = false;
        p->lms_round_damage_dealt = 0.0f;

        if (!player_is_loaded(p)) {
            continue;
        }

        if (!player_has_alive_entity(p)) {
            rf::multi_spawn_player_server_side(p);
            ++spawned;
        }
    }
    g_lms_internal_spawn_in_progress = false;

    xlog::info("LMS: round begin, {} total / {} spawned",
               static_cast<int>(participants.size()), spawned);
}

void lms_on_round_end(rf::Player* winner, RoundEndReason reason)
{
    if (!rf::is_server) return;

    // Announce the round result as a HUD notification (replaces any prior
    // round-state notification). Standings are visible on the FFA scoreboard
    // widget — no need to spam them through chat.
    if (winner) {
        // Engine-native: stats->score is the canonical scoreboard field and
        // is broadcast to clients via the engine's natural sync. Kill-driven
        // score changes are suppressed in kill.cpp for LMS, so this single
        // increment is the only way the score field changes.
        rf::player_add_score(winner, 1);

        const char* reason_str = (reason == RoundEndReason::EarlyEnd) ? "eliminated all opponents"
                               : (reason == RoundEndReason::TimeUp) ? "won on time"
                               : "won";
        af_broadcast_hud_notification(
            std::format("{} {} (round {}).", winner->name.c_str(), reason_str, rounds_get_current()),
            3,
            static_cast<int>(HudNotificationType::Round),
            true);
    } else {
        af_broadcast_hud_notification(
            std::format("Round {} ended with no winner.", rounds_get_current()),
            3,
            static_cast<int>(HudNotificationType::Round),
            true);
    }

    // Per-recipient announcer sounds. Winner hears the stock "winner" cue
    // (MP_ANN_ALLYOURBASE.wav, sent via the engine's RF_SoundPacket with the
    // hardcoded sounds.tbl id — the server doesn't need sounds.tbl loaded to
    // construct the packet; the client resolves the id locally). Everyone
    // else hears "time expired" (if the round ended on timer) or "match over"
    // (any other reason — eliminated, no winner, etc.).
    const int loser_sound = (reason == RoundEndReason::TimeUp)
                              ? custom_sound_id::ann_time_expired
                              : custom_sound_id::ann_match_over;
    for (rf::Player& p : SinglyLinkedList{rf::player_list}) {
        if (p.is_browser) continue;
        if (&p == winner) {
            if (&p == rf::local_player) {
                play_local_sound_2d(static_cast<uint16_t>(stock_sound_id::ann_winner), 0, 1.0f);
            } else {
                send_sound_packet_throwaway(&p, stock_sound_id::ann_winner);
            }
        } else {
            af_send_play_custom_sound(loser_sound, &p);
        }
    }

    // The winner's entity remains ALIVE here. The actual teardown happens in
    // lms_on_round_cleanup after the celebration window expires. We
    // only mark the round-loser-set as out (everyone besides the winner) so
    // late spawn requests during the celebration window are denied.
    for (rf::Player& p : SinglyLinkedList{rf::player_list}) {
        if (p.is_browser) continue;
        if (&p == winner) continue; // winner stays available
        p.round_is_out = true;
    }
}

void lms_reset_world_items()
{
    // The engine periodically broadcasts (every ~250ms) the visibility state
    // of every level item (the array at DAT_006d5dc0) to every client as a
    // bitmask — server-side obj_unhide is automatically replicated by that
    // mechanism via the client-side receiver at FUN_0047A220, which calls
    // either obj_unhide or obj_hide based on the bitmask. So we DON'T need
    // to destroy + recreate level items here — that would just create
    // duplicates (the original items stay tracked client-side via the
    // broadcast). We just clear the hidden flag on hidden level items.
    //
    // Dropped weapons (IF_DROPPED) aren't level items — they're created
    // dynamically and the visibility broadcast doesn't cover them. They get
    // the explicit apply-packet + obj_flag_dead pair (Bagman pattern) to
    // tell clients to remove them locally.
    int restored = 0;
    int dropped_destroyed = 0;
    rf::Item* it = rf::item_list.next;
    while (it && it != &rf::item_list) {
        rf::Item* next = it->next;
        const uint32_t flags = it->item_flags;
        const bool is_dropped  = (flags & rf::IF_DROPPED)  != 0;
        const bool is_ctf_flag = (flags & rf::IF_CTF_FLAG) != 0;

        if (is_dropped) {
            rf::send_item_apply_packet(nullptr, it->handle, 0, -1, -1, -1);
            rf::obj_flag_dead(it);
            ++dropped_destroyed;
        } else if (!is_ctf_flag) {
            // Level item: clear pending respawn timer and make it visible
            // again. The engine's 250ms visibility broadcast replicates.
            it->respawn_next.invalidate();
            rf::obj_unhide(it);
            ++restored;
        }

        it = next;
    }
    xlog::info("LMS reset items: restored {} level items, destroyed {} dropped weapons",
               restored, dropped_destroyed);
}

void lms_on_round_cleanup()
{
    if (!rf::is_server) return;
    if (!gt_is_lms()) return;

    // Tear down the round winner's entity (and any other still-alive entity
    // edge case) so the next round starts everyone fresh. Full engine death
    // pipeline so the kill is properly replicated to all clients. Clear
    // killer info first so the obituary doesn't credit a stale attacker.
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

    // Clean up dropped items from the just-ended round and force all level
    // items back to the available state for a fresh start.
    lms_reset_world_items();
}

bool lms_should_end_round(rf::Player** out_winner)
{
    if (!rf::is_server) return false;

    // A round can only end once at least 2 players have actually participated
    // in it (spawned in). This prevents the race where the first-loaded
    // player gets declared "winner" while the others are still mid-connect.
    const int participants = count_round_participants();
    if (participants < 2) return false;

    const int alive = count_alive();
    if (alive <= 1) {
        // Either one player remains (winner) or everyone died simultaneously (no winner)
        rf::Player* w = find_only_alive();
        if (out_winner) *out_winner = w;
        return true;
    }
    return false;
}

bool lms_can_round_start()
{
    // Need at least 2 participants (humans or bots) WHO HAVE FINISHED LOADING
    // THE LEVEL to have a real LMS round. Counting raw connections would let
    // us start while players are still mid-load, racing the spawn pipeline.
    int loaded = 0;
    for (rf::Player& p : SinglyLinkedList{rf::player_list}) {
        if (p.is_browser) continue;
        if (!player_is_loaded(&p)) continue;
        if (++loaded >= 2) return true;
    }
    return false;
}

rf::Player* lms_resolve_timeout_winner()
{
    // Sudden-death tiebreak by round damage dealt. Random pick on a tie.
    rf::Player* best = nullptr;
    float best_damage = -1.0f;
    std::vector<rf::Player*> tied;

    for (rf::Player& p : SinglyLinkedList{rf::player_list}) {
        if (p.is_browser) continue;
        if (p.round_is_out) continue;
        if (!player_has_alive_entity(&p)) continue;

        if (p.lms_round_damage_dealt > best_damage) {
            best_damage = p.lms_round_damage_dealt;
            best = &p;
            tied.clear();
            tied.push_back(&p);
        } else if (p.lms_round_damage_dealt == best_damage) {
            tied.push_back(&p);
        }
    }

    if (tied.size() > 1) {
        std::uniform_int_distribution<size_t> dist(0, tied.size() - 1);
        best = tied[dist(g_rng)];
    }
    return best; // may be null if literally no one is alive
}

void lms_on_late_join(rf::Player* player)
{
    // Mark the late-joiner as out for the current round. They'll be reset to
    // round_is_out=false at the next on_round_begin alongside everyone else,
    // and lms_can_player_spawn will deny spawn attempts in the meantime with
    // the standard "you're out for this round" message. Server-only gating
    // and the Active-state check are done by rounds_on_player_init before
    // this fires.
    if (player) player->round_is_out = true;
}

} // namespace

void lms_level_init_post()
{
    if (!rf::is_server) return;
    if (!gt_is_lms()) return;

    // Re-register on every level load (rounds_level_init clears callbacks
    // so the previous gametype's hooks don't linger across gametype changes).
    RoundCallbacks cb{};
    cb.on_round_begin = &lms_on_round_begin;
    cb.on_round_end = &lms_on_round_end;
    cb.on_round_cleanup = &lms_on_round_cleanup;
    cb.should_end_round = &lms_should_end_round;
    cb.resolve_timeout_winner = &lms_resolve_timeout_winner;
    cb.can_round_start = &lms_can_round_start;
    cb.on_late_join = &lms_on_late_join;
    rounds_register_callbacks(cb);

    // Clear per-round state. stats->score (round wins) is managed by the
    // engine's standard level-load reset path — we don't touch it here.
    for (rf::Player& p : SinglyLinkedList{rf::player_list}) {
        p.round_is_out = false;
        p.lms_round_damage_dealt = 0.0f;
    }
}

void lms_do_frame()
{
    if (!rf::is_server) return;
    if (!gt_is_lms()) {
        return;
    }
    if (!rounds_is_active()) {
        return;
    }

    // Per-player housekeeping:
    //   1) Auto-spawn loaded players who don't yet have an entity (handles
    //      late-loaders who finished connecting after on_round_begin's
    //      synchronous spawn batch ran).
    //   2) Set round_participated on any player who has a live entity this round.
    //      should_end_round uses this to know who's actually playing.
    g_lms_internal_spawn_in_progress = true;
    for (rf::Player& p : SinglyLinkedList{rf::player_list}) {
        if (p.is_browser) continue;

        if (player_has_alive_entity(&p)) {
            p.round_participated = true;
            continue;
        }

        // No live entity. Spawn iff they're loaded and not eliminated yet.
        if (p.round_is_out) continue;
        if (!player_is_loaded(&p)) continue;
        rf::multi_spawn_player_server_side(&p);
    }
    g_lms_internal_spawn_in_progress = false;

    // Milestone chat: announce when the alive count crosses a notable
    // threshold (one-shot per round per threshold). Don't fire before the
    // round has at least 2 participants, otherwise round-1 race conditions
    // trigger spurious "Final duel!" announcements.
    const int participants = count_round_participants();
    if (participants < 2) return;

    const int alive = count_alive();
    if (alive != g_last_alive_announced) {
        g_last_alive_announced = alive;
        const char* msg = nullptr;
        if (alive == 4) msg = "4 players remain.";
        else if (alive == 3) msg = "3 players remain.";
        else if (alive == 2) msg = "Final duel!";
        if (msg) {
            af_broadcast_hud_notification(msg, 3,
                static_cast<int>(HudNotificationType::Round), true);
        }

        // Announcer cues — kills-remaining is (alive - 1), since the round
        // ends when alive drops to 1. So alive=6 → 5 more kills to end,
        // alive=2 → 1 more kill to end.
        if (alive == 6) {
            af_broadcast_play_custom_sound(custom_sound_id::ann_five_kills_left);
        } else if (alive == 2) {
            af_broadcast_play_custom_sound(custom_sound_id::ann_one_kill_left);
        }
    }
}

void lms_on_entity_will_die(rf::Entity* ep)
{
    if (!rf::is_server || !gt_is_lms() || !ep) return;
    rf::Player* player = rf::player_from_entity_handle(ep->handle);
    if (!player) return;

    // Mark out so respawn gate blocks any client-driven respawn request.
    player->round_is_out = true;
}

void lms_on_pvp_damage(rf::Player* dealer, float real_damage)
{
    if (!rf::is_server || !gt_is_lms() || !dealer) return;
    if (!rounds_is_active()) return;
    if (real_damage <= 0.0f) return;
    dealer->lms_round_damage_dealt += real_damage;
}

bool lms_can_player_spawn(rf::Player* player)
{
    if (!gt_is_lms()) return true; // not our problem
    if (!player) return true;
    if (g_lms_internal_spawn_in_progress) return true;

    // No round yet (level still initializing) — allow the engine's normal
    // spawn flow so players have entities by the time round 1 begins.
    if (rounds_get_state() == RoundState::Inactive) return true;

    if (rounds_is_between_rounds()) {
        af_send_automated_chat_msg("Wait - the next round is starting shortly.", player);
        return false;
    }

    if (player->round_is_out) {
        af_send_automated_chat_msg("You're out for this round. You'll spawn at the next round start.", player);
        return false;
    }

    return true;
}

void lms_on_player_disconnect(rf::Player* /*player*/)
{
    // Nothing to clean up — alive count is derived from the live player_list.
    // Callbacks may detect a round-end transition on the next tick.
}

int lms_alive_count()
{
    return count_alive();
}
