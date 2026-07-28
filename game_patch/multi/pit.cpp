#include <algorithm>
#include <deque>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <xlog/xlog.h>
#include <common/utils/list-utils.h>
#include <common/utils/iterable-utils.h>
#include "pit.h"
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

struct PitInfo
{
    std::deque<rf::Player*> queue; // waiting players, front = next up (duelers are NOT in here)
    rf::Player* dueler[2] = {};    // current pairing; slot 0 == champion seat
    bool duel_started = false;     // latched once BOTH duelers have alive entities this round
    bool match_decided = false;    // latched once a winner reaches the score limit (survives a winner DC)
};

PitInfo g_pit;

// True while we call multi_spawn_player_server_side ourselves (round-start batch
// / auto-spawn). Lets the spawn gate permit those regardless of round state.
bool g_internal_spawn_in_progress = false;

bool player_has_alive_entity(rf::Player* p)
{
    if (!p) return false;
    if (p->is_browser) return false;
    rf::Entity* ep = rf::entity_from_handle(p->entity_handle);
    if (!ep) return false;
    if (rf::entity_is_dying(ep)) return false;
    return true;
}

// A player has finished loading when the engine set NPF_CLIENT_IS_LOADED. The
// listen-server host is always considered loaded; bots never carry the client
// loaded flag but are always ready, so treat them as loaded too.
bool player_is_loaded(rf::Player* p)
{
    if (!p) return false;
    if (p == rf::local_player) return true;
    if (p->is_bot) return true;
    if (!p->net_data) return false;
    return (p->net_data->flags & rf::NPF_CLIENT_IS_LOADED) != 0;
}

// Eligible to be queued/dueled: a real (non-browser) player who hasn't opted
// out. Note we intentionally do NOT exclude engine spectators — queued players
// may be spectating, and we clear their spectator fields when promoting them.
// Spawn-disabled bots are excluded: the spawn hook denies their spawn before
// Pit's internal-spawn bypass, so seating one would stall the round. Once a bot
// is re-enabled, the auto-enqueue pass picks it up again.
bool is_eligible(rf::Player* p)
{
    if (!p || p->is_browser || p->pit_queue_opt_out) return false;
    if (p->is_bot && p->is_spawn_disabled) return false;
    return true;
}

bool is_dueler(rf::Player* p)
{
    return p && (p == g_pit.dueler[0] || p == g_pit.dueler[1]);
}

bool in_queue(rf::Player* p)
{
    return iterable_contains(g_pit.queue, p);
}

bool player_still_connected(rf::Player* p)
{
    if (!p) return false;
    for (rf::Player& q : SinglyLinkedList{rf::player_list}) {
        if (&q == p) return true;
    }
    return false;
}

void erase_from_queue(rf::Player* p)
{
    std::erase(g_pit.queue, p);
}

// 1-based position of p among the waiting queue (0 if not queued) and the
// total waiting-queue size.
void queue_position(rf::Player* p, int& pos, int& total)
{
    total = static_cast<int>(g_pit.queue.size());
    pos = 0;
    int i = 1;
    for (rf::Player* q : g_pit.queue) {
        if (q == p) {
            pos = i;
            break;
        }
        ++i;
    }
}

// Scan the queue front-to-back: discard disconnected/ineligible entries, but
// leave eligible-but-still-loading players in place (they keep their queue
// position and get seated once loaded), and erase + return the first LOADED
// eligible player. Returns nullptr if none are loaded yet — the slot is left
// empty and pit_can_round_start (which requires 2 loaded) gates the round.
rf::Player* pop_queue_front()
{
    size_t i = 0;
    while (i < g_pit.queue.size()) {
        rf::Player* p = g_pit.queue[i];
        if (!player_still_connected(p) || !is_eligible(p)) {
            g_pit.queue.erase(g_pit.queue.begin() + i); // remove; don't advance
            continue;
        }
        if (!player_is_loaded(p)) {
            ++i; // still loading — keep their place, skip past for now
            continue;
        }
        g_pit.queue.erase(g_pit.queue.begin() + i);
        return p;
    }
    return nullptr;
}

// Server-side: strip any engine-spectate state so the spawn gate at
// server.cpp (`if (player->is_spectator) return;`) doesn't block a promoted
// dueler. Mirrors af_process_spectate_start_packet's "leaving spectate" path.
void clear_spectator_fields(rf::Player* p)
{
    if (!p) return;
    rf::Player* old_target = p->spectatee.value_or(nullptr);
    if (old_target && rf::is_server) {
        af_send_spectate_notify_packet(old_target, p, false);
    }
    p->is_spectator = false;
    p->spectatee = std::nullopt;
    p->spectate_start_time = std::nullopt;
}

// Validate/fill both dueler slots. Slot 0 is the "champion seat" by convention
// (on_round_end parks the winner there), but the fill logic is identical for
// both slots: keep the current occupant if still connected + eligible,
// otherwise pop the next queued player. Only assigns slots — never spawns.
void select_pairing()
{
    for (int i = 0; i < 2; ++i) {
        rf::Player*& slot = g_pit.dueler[i];
        if (slot && (!player_still_connected(slot) || !is_eligible(slot))) {
            slot = nullptr;
        }
        if (!slot) {
            slot = pop_queue_front();
        }
    }
}

// Prepare a freshly-promoted dueler: clear elimination/participation, any stale
// respawn timer, and engine-spectate state so the spawn pipeline accepts them.
void prepare_dueler(rf::Player* p)
{
    if (!p) return;
    p->round_is_out = false;
    p->round_participated = false;
    p->respawn_timer.invalidate();
    clear_spectator_fields(p);
}

// === Round callbacks ============================================

void pit_on_round_begin()
{
    if (!rf::is_server) return;

    g_pit.duel_started = false;

    // Validate/fill the pairing (on_round_cleanup usually pre-selected it).
    select_pairing();

    g_internal_spawn_in_progress = true;
    for (int i = 0; i < 2; ++i) {
        rf::Player* p = g_pit.dueler[i];
        if (!p) continue;
        prepare_dueler(p);
        if (player_is_loaded(p) && !player_has_alive_entity(p)) {
            rf::multi_spawn_player_server_side(p);
        }
    }
    g_internal_spawn_in_progress = false;

    // Everyone who isn't a dueler sits this round out (denies their spawns).
    for (rf::Player& p : SinglyLinkedList{rf::player_list}) {
        if (is_dueler(&p)) continue;
        p.round_is_out = true;
    }

    pit_broadcast_queue_states();

    // Match point: if either dueler is one duel win away from the score limit,
    // play the "one kill remaining" announcer cue for everyone in the server.
    // Guarded on limit >= 2 so a score_limit of 1 (where score-1 == 0) doesn't
    // fire the cue every round.
    const int score_limit = g_alpine_server_config_active_rules.pit_score_limit;
    if (score_limit >= 2) {
        for (int i = 0; i < 2; ++i) {
            rf::Player* p = g_pit.dueler[i];
            if (p && p->stats && p->stats->score == score_limit - 1) {
                af_broadcast_play_custom_sound(custom_sound_id::ann_one_kill_left);
                break;
            }
        }
    }

    xlog::info("Pit: round {} begin, {} vs {}", rounds_get_current(),
               g_pit.dueler[0] ? g_pit.dueler[0]->name.c_str() : "(none)",
               g_pit.dueler[1] ? g_pit.dueler[1]->name.c_str() : "(none)");
}

bool pit_should_end_round(rf::Player** out_winner)
{
    if (!rf::is_server) return false;
    if (out_winner) *out_winner = nullptr;

    if (!g_pit.duel_started) {
        // Before the duel has genuinely started, backfill any slot whose player
        // disconnected or opted out (a pre-start forfeit would otherwise pin an
        // ineligible player in the slot and stall the round until the timer).
        // If a slot can't be filled (queue empty), the round is abandoned with
        // no winner.
        bool changed = false;
        for (int i = 0; i < 2; ++i) {
            rf::Player* d = g_pit.dueler[i];
            if (d && player_still_connected(d) && is_eligible(d)) {
                // A dueler who died before the duel started (e.g. environmental
                // death while the opponent was still loading) isn't eliminated:
                // clear the out flag so do_frame respawns them.
                if (d->round_is_out && !player_has_alive_entity(d)) {
                    d->round_is_out = false;
                }
                continue;
            }

            // Duelers are never in the queue (pop removes them, auto-enqueue
            // skips them), so the popped replacement can't collide with the
            // other slot — no dupe-check needed.
            rf::Player* repl = pop_queue_front();
            if (!repl) {
                return true; // *out_winner stays null: round abandoned
            }
            g_pit.dueler[i] = repl;
            prepare_dueler(repl); // do_frame spawns them
            changed = true;
        }
        if (changed) pit_broadcast_queue_states();

        // Latch once both duelers actually have live entities.
        if (player_has_alive_entity(g_pit.dueler[0]) && player_has_alive_entity(g_pit.dueler[1])) {
            g_pit.duel_started = true;
        }
        return false;
    }

    // Duel is under way: a disconnected dueler counts as dead.
    int alive = 0;
    rf::Player* survivor = nullptr;
    for (int i = 0; i < 2; ++i) {
        rf::Player* p = g_pit.dueler[i];
        if (p && player_still_connected(p) && player_has_alive_entity(p)) {
            ++alive;
            survivor = p;
        }
    }

    if (alive <= 1) {
        if (out_winner) *out_winner = (alive == 1) ? survivor : nullptr;
        return true;
    }
    return false;
}

rf::Player* pit_resolve_timeout_winner()
{
    // Timeout is a no-winner draw by design.
    return nullptr;
}

void pit_on_round_end(rf::Player* winner, RoundEndReason reason)
{
    if (!rf::is_server) return;

    rf::Player* d0 = g_pit.dueler[0];
    rf::Player* d1 = g_pit.dueler[1];

    // Announce the result as a HUD notification.
    if (winner) {
        // Kill-driven scoring is suppressed for Pit (gt_uses_custom_scoring), so
        // this is the only place the scoreboard field changes.
        if (winner->stats) {
            rf::player_add_score(winner, 1);
        }
        af_broadcast_hud_notification(
            std::format("{} wins the duel (round {}).", winner->name.c_str(), rounds_get_current()),
            3, static_cast<int>(HudNotificationType::Round), true);

        // Announce the match result up front if this duel decided it; the
        // rounds system rotates the level once the celebration window closes
        // (governed by our is_match_over arbiter). Latch it so rotation still
        // happens if the winner disconnects during the post-round window.
        if (winner->stats && winner->stats->score >= g_alpine_server_config_active_rules.pit_score_limit) {
            g_pit.match_decided = true;
            af_broadcast_hud_notification(
                std::format("{} wins the match!", winner->name.c_str()),
                4, static_cast<int>(HudNotificationType::Round), true);
        }
    }
    else if (reason == RoundEndReason::TimeUp) {
        af_broadcast_hud_notification(
            "Time's up! No winner - both duelers go to the back of the queue.",
            3, static_cast<int>(HudNotificationType::Round), true);
    }
    else {
        af_broadcast_hud_notification(
            std::format("Round {}: no winner - both duelers go to the back of the queue.", rounds_get_current()),
            3, static_cast<int>(HudNotificationType::Round), true);
    }

    // Per-recipient announcer sounds. Winner hears the stock "winner" cue;
    // everyone else hears "time expired" (timer) or "match over" (any other
    // reason).
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

    if (winner) {
        // Loser is the other dueler; push them to the back of the queue unless
        // they opted out / disconnected. Winner stays as champion in slot 0.
        rf::Player* loser = (winner == d0) ? d1 : d0;
        if (loser && player_still_connected(loser) && !loser->pit_queue_opt_out) {
            g_pit.queue.push_back(loser);
        }
        g_pit.dueler[0] = winner; // ensure champion occupies slot 0
        g_pit.dueler[1] = nullptr;
    }
    else {
        // No winner: both duelers go to the back of the queue (slot 0 then 1).
        for (int i = 0; i < 2; ++i) {
            rf::Player* p = g_pit.dueler[i];
            if (p && player_still_connected(p) && !p->pit_queue_opt_out) {
                g_pit.queue.push_back(p);
            }
            g_pit.dueler[i] = nullptr;
        }
    }

    // Deny spawns during the celebration window for everyone but a surviving
    // winner (whose entity is still alive until on_round_cleanup).
    for (rf::Player& p : SinglyLinkedList{rf::player_list}) {
        if (p.is_browser) continue;
        if (&p == winner) continue;
        p.round_is_out = true;
    }

    pit_broadcast_queue_states();
}

void pit_on_round_cleanup()
{
    if (!rf::is_server) return;
    if (!gt_is_pit()) return;

    // Kill any survivors through the full death pipeline so the next round
    // starts everyone fresh (killer info cleared so no stale obituary fires).
    for (rf::Player& p : SinglyLinkedList{rf::player_list}) {
        if (p.is_browser) continue;
        p.round_is_out = true;
        rounds_kill_entity_silent(rf::entity_from_handle(p.entity_handle));
    }

    pit_reset_world_items();

    // Pre-select the next pairing so we can announce it during intermission.
    select_pairing();
    if (g_pit.dueler[0] && g_pit.dueler[1]) {
        af_broadcast_hud_notification(
            std::format("Up next: {} vs {}.", g_pit.dueler[0]->name.c_str(), g_pit.dueler[1]->name.c_str()),
            3, static_cast<int>(HudNotificationType::Round), true);
    }

    pit_broadcast_queue_states();
}

bool pit_can_round_start()
{
    // Need at least 2 eligible players who have finished loading; any state
    // (they may be engine-spectating in the queue) counts.
    int loaded = 0;
    for (rf::Player& p : SinglyLinkedList{rf::player_list}) {
        if (!is_eligible(&p)) continue;
        if (!player_is_loaded(&p)) continue;
        if (++loaded >= 2) return true;
    }
    return false;
}

void pit_on_late_join(rf::Player* player)
{
    // Sit the late joiner out for the current round; the auto-enqueue pass in
    // do_frame adds them to the queue, and on_round_begin re-includes them when
    // it's their turn.
    if (player) player->round_is_out = true;
}

bool pit_wants_round_start_notification(rf::Player* player)
{
    // Only the two duelers get the "Round X - fight!" notification; queued and
    // opted-out players keep their persistent queue overlay uninterrupted.
    return player && !player->is_browser && is_dueler(player);
}

bool pit_is_match_over()
{
    // The match ends (and the level rotates) once any player has reached the
    // duel-win score limit. As the registered is_match_over arbiter this fully
    // governs rotation — the stock max_rounds fallback is not used for Pit.
    // The latch covers the case where the deciding winner disconnects during
    // the post-round window (they'd no longer be in the live scan below).
    if (g_pit.match_decided) return true;
    const int limit = g_alpine_server_config_active_rules.pit_score_limit;
    for (rf::Player& p : SinglyLinkedList{rf::player_list}) {
        if (p.is_browser) continue;
        if (p.stats && p.stats->score >= limit) return true;
    }
    return false;
}

} // namespace

void pit_level_init()
{
    // Real level boundary: fresh queue and pairing.
    g_pit = PitInfo{};
}

void pit_level_init_post()
{
    if (!rf::is_server) return;
    if (!gt_is_pit()) return;

    // Re-register on every level load (rounds_level_init clears callbacks so the
    // previous gametype's hooks don't linger across gametype changes).
    RoundCallbacks cb{};
    cb.on_round_begin = &pit_on_round_begin;
    cb.on_round_end = &pit_on_round_end;
    cb.on_round_cleanup = &pit_on_round_cleanup;
    cb.should_end_round = &pit_should_end_round;
    cb.resolve_timeout_winner = &pit_resolve_timeout_winner;
    cb.can_round_start = &pit_can_round_start;
    cb.on_late_join = &pit_on_late_join;
    cb.wants_round_start_notification = &pit_wants_round_start_notification;
    cb.is_match_over = &pit_is_match_over;
    rounds_register_callbacks(cb);

    // Clear per-round state. stats->score (round wins) is managed by the
    // engine's standard level-load reset path — we don't touch it here.
    for (rf::Player& p : SinglyLinkedList{rf::player_list}) {
        p.round_is_out = false;
        p.round_participated = false;
    }

    pit_reset_world_items();
}

void pit_do_frame()
{
    if (!rf::is_server) return;
    if (!gt_is_pit()) return;
    // Only act during live gameplay; rounds_do_frame stops pumping the state
    // machine outside GS_GAMEPLAY, so rounds_is_active() can linger through a
    // mid-round level-change limbo window without this guard.
    if (rf::gameseq_get_state() != rf::GameState::GS_GAMEPLAY) return;

    bool queue_changed = false;

    // (a) Auto-enqueue: any eligible player (non-browser, not opted out, not a
    //     spawn-disabled bot) who isn't a dueler and isn't already queued joins
    //     the back of the queue. A bot re-enabled later is picked up here.
    //     Queue integrity on disconnect is owned by pit_on_player_disconnect
    //     (fires on every player destroy; also nulls dueler slots), so no
    //     per-frame prune is needed here.
    for (rf::Player& p : SinglyLinkedList{rf::player_list}) {
        if (!is_eligible(&p)) continue;
        if (is_dueler(&p)) continue;
        if (in_queue(&p)) continue;
        g_pit.queue.push_back(&p);
        queue_changed = true;
    }

    if (queue_changed) pit_broadcast_queue_states();

    // (b) During an active round, auto-spawn any dueler who is loaded, not out,
    //     and lacks a live entity (handles the round-start / backfill spawns).
    if (rounds_is_active()) {
        g_internal_spawn_in_progress = true;
        for (int i = 0; i < 2; ++i) {
            rf::Player* p = g_pit.dueler[i];
            if (!p) continue;
            if (player_has_alive_entity(p)) {
                p->round_participated = true;
                continue;
            }
            if (p->round_is_out) continue;
            if (!player_is_loaded(p)) continue;
            rf::multi_spawn_player_server_side(p);
        }
        g_internal_spawn_in_progress = false;

        if (!g_pit.duel_started
            && player_has_alive_entity(g_pit.dueler[0])
            && player_has_alive_entity(g_pit.dueler[1])) {
            g_pit.duel_started = true;
        }
    }
}

bool pit_can_player_spawn(rf::Player* player)
{
    if (!gt_is_pit()) return true; // not our problem
    if (!player) return true;
    if (g_internal_spawn_in_progress) return true;

    // Level still initializing — allow the engine's normal spawn flow so the
    // pre-round warmup can seed entities.
    if (rounds_get_state() == RoundState::Inactive) return true;

    // The client re-requests a spawn every frame while fire is held, so throttle
    // the denial notice. Check the gate BEFORE building any message so a denied
    // spawn costs only a timer read in steady state.
    const bool throttle_open =
        !player->waiting_msg_timer.valid() || player->waiting_msg_timer.elapsed();
    auto send_throttled = [&](std::string_view msg) {
        af_send_automated_chat_msg(msg, player);
        player->waiting_msg_timer.set(3000);
    };

    if (rounds_is_between_rounds()) {
        if (throttle_open) send_throttled("Wait - the next duel is starting shortly.");
        return false;
    }

    if (player->round_is_out) {
        if (throttle_open) {
            if (player->pit_queue_opt_out) {
                send_throttled("You are not queued. Press your Ready key to join the queue.");
            }
            else {
                int pos = 0, total = 0;
                queue_position(player, pos, total);
                if (pos > 0) {
                    send_throttled(std::format(
                        "You're in the queue ({}/{}). You'll play when it's your turn.", pos, total));
                }
                else {
                    // Not enqueued yet (e.g. just went round_is_out this frame) —
                    // avoid the misleading "(0/N)" with a neutral line.
                    send_throttled("Waiting for your turn in the queue.");
                }
            }
        }
        return false;
    }

    return true;
}

void pit_on_entity_will_die(rf::Entity* ep)
{
    if (!rf::is_server || !gt_is_pit() || !ep) return;
    rf::Player* player = rf::player_from_entity_handle(ep->handle);
    if (!player) return;

    // Mark out so the respawn gate blocks any client-driven respawn request;
    // the round end is detected on the next tick.
    player->round_is_out = true;
}

void pit_on_player_disconnect(rf::Player* player)
{
    // Gate to Pit so we don't broadcast Pit packets in other gametypes (g_pit is
    // empty outside Pit anyway). Matches bagman_on_player_disconnect convention.
    if (!rf::is_server || !gt_is_pit()) return;
    if (!player) return;

    erase_from_queue(player);
    for (int i = 0; i < 2; ++i) {
        if (g_pit.dueler[i] == player) g_pit.dueler[i] = nullptr;
    }

    pit_broadcast_queue_states();
}

void pit_handle_queue_request(rf::Player* player, uint8_t action)
{
    if (!rf::is_server) return;
    if (!gt_is_pit()) return;
    if (!player || player->is_browser) return;

    // 0 = leave, 1 = join, 2 = toggle. A player is "in" when not opted out.
    bool want_join;
    if (action == 2) {
        want_join = player->pit_queue_opt_out;
    } else {
        want_join = (action != 0);
    }

    // Only broadcast when the request actually mutates state — no-ops (join
    // while already queued, leave while already opted out) must not fan out a
    // reliable broadcast to every client.
    bool changed = false;

    if (!want_join) {
        // Opting out.
        if (!player->pit_queue_opt_out) {
            player->pit_queue_opt_out = true;
            changed = true;
        }
        if (in_queue(player)) {
            erase_from_queue(player);
            changed = true;
        }

        if (is_dueler(player) && rounds_is_active()) {
            // Active dueler forfeits: kill their entity (killer cleared) so
            // should_end_round awards the other dueler on the next tick.
            player->round_is_out = true;
            rounds_kill_entity_silent(rf::entity_from_handle(player->entity_handle));
            af_send_automated_chat_msg("You forfeited the duel.", player);
            changed = true;
        }
        else if (is_dueler(player)) {
            // Pre-selected dueler between rounds: just clear the slot; the next
            // on_round_begin backfills from the queue.
            for (int i = 0; i < 2; ++i) {
                if (g_pit.dueler[i] == player) {
                    g_pit.dueler[i] = nullptr;
                    changed = true;
                }
            }
        }
    }
    else {
        // Opting in.
        if (player->pit_queue_opt_out) {
            player->pit_queue_opt_out = false;
            changed = true;
        }
        if (!is_dueler(player) && !in_queue(player)) {
            g_pit.queue.push_back(player);
            changed = true;
        }
    }

    if (changed) pit_broadcast_queue_states();
}

void pit_reset_world_items()
{
    if (!rf::is_server) return;

    // Resolve the configured allowed-item cls_names to item type indices ONCE
    // per reset (the established item_lookup_type pattern). Comparing
    // it->info_index against these resolved indices — rather than matching
    // cls_name strings — respects the item_replacements config (a replaced
    // Shotgun still resolves to the same type) and lets ops run all-items /
    // sniper Pit via pit_allowed_items.
    std::vector<int> allowed_indices;
    allowed_indices.reserve(g_alpine_server_config_active_rules.pit_allowed_items.size());
    for (const std::string& name : g_alpine_server_config_active_rules.pit_allowed_items) {
        const int idx = rf::item_lookup_type(name.c_str());
        if (idx >= 0) allowed_indices.push_back(idx);
    }

    // Keep only the allowed level pickups; hide everything else.
    multi_hide_level_items(allowed_indices);
}

void pit_send_queue_state(rf::Player* p)
{
    if (!rf::is_server || !p || p->is_browser) return;

    const uint8_t total = static_cast<uint8_t>(std::min<size_t>(g_pit.queue.size(), 255));

    uint8_t flags = 0;
    uint8_t position = 0;
    if (is_dueler(p)) {
        flags |= 0x2; // bit1 = is_dueler
    }
    else {
        int pos = 0, tot = 0;
        queue_position(p, pos, tot);
        if (pos > 0) {
            flags |= 0x1; // bit0 = queued
            position = static_cast<uint8_t>(std::min(pos, 255));
            // Queued players auto-spectate once rounds are running. Never during
            // Inactive — the pre-round warmup keeps everyone alive and playing,
            // and the client-side spectate entry would kill their entity. Bots
            // are real clients but must NEVER be told to spectate: in free-look
            // their synthetic secondary-attack input spams camera drops. bit0
            // (queued) still applies so bots show correctly on the scoreboard.
            if (!p->is_bot && rounds_get_state() != RoundState::Inactive) {
                flags |= 0x4; // bit2 = should spectate
            }
        }
    }
    af_send_pit_queue_state(p, flags, position, total);
}

// Server-authoritative roster: role for every non-browser player with a valid
// net_data id. is_dueler/in_queue are TU-visible (anonymous-namespace helpers).
static std::vector<af_pit_roster_entry> build_pit_roster()
{
    std::vector<af_pit_roster_entry> roster;
    for (rf::Player& p : SinglyLinkedList{rf::player_list}) {
        if (p.is_browser) continue;
        if (!p.net_data) continue;
        uint8_t role = PIT_ROLE_NOT_QUEUED;
        uint8_t order = 0; // 1-based queue position for queued players; 0 otherwise
        if (is_dueler(&p)) {
            role = PIT_ROLE_DUELER;
        }
        else {
            int pos = 0, tot = 0;
            queue_position(&p, pos, tot); // pos = 1-based deque index, 0 if not queued
            if (pos > 0) {
                role = PIT_ROLE_QUEUED;
                order = static_cast<uint8_t>(std::min(pos, 255));
            }
        }
        roster.push_back(af_pit_roster_entry{p.net_data->player_id, role, order});
    }
    return roster;
}

void pit_broadcast_queue_states()
{
    if (!rf::is_server) return;

    for (rf::Player& p : SinglyLinkedList{rf::player_list}) {
        if (p.is_browser) continue;
        pit_send_queue_state(&p);
    }

    // Keep the roster (scoreboard grouping) in lockstep with per-player queue
    // state — broadcast it here on every mutation.
    af_broadcast_pit_roster(build_pit_roster());
}

void pit_send_roster_to(rf::Player* player)
{
    if (!rf::is_server || !player) return;
    af_send_pit_roster(player, build_pit_roster());
}
