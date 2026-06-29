#include <algorithm>
#include <cmath>
#include <format>
#include <vector>
#include <xlog/xlog.h>
#include <patch_common/FunHook.h>
#include <common/utils/list-utils.h>
#include "rounds.h"
#include "gametype.h"
#include "multi.h"
#include "server.h"
#include "server_internal.h"
#include "alpine_packets.h"
#include "../hud/hud.h"
#include "../rf/multi.h"
#include "../rf/entity.h"
#include "../rf/gameseq.h"
#include "../rf/hud.h"
#include "../rf/level.h"
#include "../rf/os/console.h"
#include "../rf/os/frametime.h"
#include "../rf/player/player.h"

namespace
{

RoundCallbacks g_rounds_callbacks{};

// --- Client-side countdown state ---
// When a server tells us "show a round-start countdown for N ms," we record
// the deadline and let our FunHook on multi_hud_render_countdown rewrite
// time_left_seconds with the live remaining count on each frame. The engine
// otherwise reads time_left_seconds from its own (multi_time_limit-driven)
// math, so this override only kicks in while the deadline is live.
rf::TimestampRealtime g_client_countdown_deadline;

void reset_n_seconds_played_array()
{
    // The engine sets played_n_seconds_left_sound[N] = true after playing the
    // per-second tick. Reset so the engine plays the ticks again during our
    // countdown — its natural reset only fires when time_left rises >10s, which
    // doesn't happen during the round-start window because we're overriding
    // those globals directly.
    for (int i = 0; i < 10; ++i) {
        rf::played_n_seconds_left_sound[i] = false;
    }
}

FunHook<void(bool)> multi_hud_render_countdown_hook{
    0x00476EA0,
    [](bool visible) {
        if (g_client_countdown_deadline.valid() && !g_client_countdown_deadline.elapsed()) {
            const int ms_left = g_client_countdown_deadline.time_until();
            const int secs_left = (ms_left + 999) / 1000; // ceil
            if (secs_left >= 1 && secs_left <= 10) {
                rf::time_left_hours = 0;
                rf::time_left_minutes = 0;
                rf::time_left_seconds = secs_left;
            }
        }
        multi_hud_render_countdown_hook.call_target(visible);
    },
};

struct RoundsRuntime
{
    RoundState state = RoundState::Inactive;
    int current = 0;             // 0-indexed; round 1 == current == 0
    float round_start_level_time = 0.0f;
    float round_deadline_level_time = 0.0f;
    float post_round_deadline_level_time = 0.0f;
    float intermission_deadline_level_time = 0.0f;

    // Deferred end request — processed at the top of the next do_frame so we
    // never tear down state while inside an engine hook or per-frame check.
    bool end_pending = false;
    rf::Player* pending_winner = nullptr;
    RoundEndReason pending_reason = RoundEndReason::EarlyEnd;

    // One-shot flag: we've already broadcast "waiting for players" to chat.
    bool announced_waiting = false;

    // Set when we trigger a max-rounds level rotation. multi_change_level is
    // async, so without this latch rounds_do_frame's Inactive→start case
    // would fire before the new level actually loads and we'd kick off a
    // fresh round-set on the doomed map. Cleared by g_rounds_runtime.reset() (which
    // rounds_level_init calls when the new level finally arrives).
    bool pending_level_change = false;

    void reset()
    {
        state = RoundState::Inactive;
        current = 0;
        round_start_level_time = 0.0f;
        round_deadline_level_time = 0.0f;
        post_round_deadline_level_time = 0.0f;
        intermission_deadline_level_time = 0.0f;
        end_pending = false;
        pending_winner = nullptr;
        pending_reason = RoundEndReason::EarlyEnd;
        announced_waiting = false;
        pending_level_change = false;
    }
};

RoundsRuntime g_rounds_runtime;

const RoundConfig& cfg()
{
    return g_alpine_server_config_active_rules.rounds;
}

bool rounds_enabled_now()
{
    return gt_uses_rounds();
}

bool in_round_capable_gameplay()
{
    if (!rf::is_server) return false;
    if (!rf::is_multi) return false;
    if (rf::gameseq_get_state() != rf::GameState::GS_GAMEPLAY) return false;
    return true;
}

bool ready_to_start_round()
{
    return !g_rounds_callbacks.can_round_start || g_rounds_callbacks.can_round_start();
}

void announce_waiting_once()
{
    if (g_rounds_runtime.announced_waiting) return;
    g_rounds_runtime.announced_waiting = true;
    af_broadcast_automated_chat_msg("Waiting for more players to join...");
}

void start_round()
{
    g_rounds_runtime.state = RoundState::Active;
    g_rounds_runtime.round_start_level_time = rf::level.time;
    g_rounds_runtime.round_deadline_level_time = rf::level.time + static_cast<float>(cfg().round_time);
    g_rounds_runtime.announced_waiting = false;

    // The engine uses multi_time_limit (== netgame.max_time_seconds) for the
    // HUD countdown rendered as (max_time - level.time). Setting it to the
    // round's deadline makes the existing HUD show the correct remaining time
    // every round, with no new packet.
    rf::multi_time_limit = g_rounds_runtime.round_deadline_level_time;

    if (g_rounds_callbacks.on_round_begin) {
        g_rounds_callbacks.on_round_begin();
    }

    af_broadcast_hud_notification(
        std::format("Round {} of {} - fight!", g_rounds_runtime.current + 1, cfg().max_rounds),
        3,
        static_cast<int>(HudNotificationType::Round),
        true);
}

void enter_post_round()
{
    g_rounds_runtime.state = RoundState::PostRound;
    const float dur = static_cast<float>(cfg().post_round_time);
    g_rounds_runtime.post_round_deadline_level_time = rf::level.time + dur;

    // Park the HUD timer at 0 during the celebration window. Our gate on
    // multi_check_for_round_end_hook prevents engine-side reactions.
    rf::multi_time_limit = rf::level.time;
}

void enter_intermission()
{
    g_rounds_runtime.state = RoundState::Intermission;
    const uint8_t dur_seconds = cfg().intermission_time;
    g_rounds_runtime.intermission_deadline_level_time = rf::level.time + static_cast<float>(dur_seconds);

    rf::multi_time_limit = rf::level.time;

    if (g_rounds_callbacks.on_round_cleanup) {
        g_rounds_callbacks.on_round_cleanup();
    }

    // Drive the engine's stock big-number countdown via a client-side hook
    // instead of mutating multi_time_limit. The duration only takes effect if
    // it's in the engine's 1..10s render window, which is always the case for
    // typical intermission_time values.
    if (dur_seconds > 0) {
        af_broadcast_round_countdown(dur_seconds);
    }
}

void proceed_to_next_round_or_rotate()
{
    // Called after the PostRound celebration window expires (or directly if
    // post_round_time is 0). Decides whether to rotate the level (max rounds
    // hit) or continue into intermission + the next round.

    if (g_rounds_runtime.current >= cfg().max_rounds) {
        rf::console::print("Rounds: max rounds reached, advancing to next level.\n");
        af_broadcast_hud_notification(
            "All rounds played. Advancing to next level.",
            3,
            static_cast<int>(HudNotificationType::Round),
            true);
        g_rounds_runtime.state = RoundState::Inactive;
        g_rounds_runtime.current = 0;
        g_rounds_runtime.pending_level_change = true; // gate Inactive→start until level actually loads
        // Advance the rotation. multi_change_level is async; the latch above
        // keeps rounds_do_frame from restarting a round-set on the doomed level.
        set_manually_loaded_level(false);
        rf::multi_change_level(nullptr);
        return;
    }

    if (cfg().intermission_time > 0) {
        enter_intermission();
    } else {
        // No intermission configured — kill+cleanup batch + straight to next round.
        if (g_rounds_callbacks.on_round_cleanup) {
            g_rounds_callbacks.on_round_cleanup();
        }
        if (!ready_to_start_round()) {
            enter_intermission(); // hold here, no countdown
            return;
        }
        start_round();
    }
}

void advance_to_next_round_or_change_level()
{
    ++g_rounds_runtime.current;

    // The celebration window (PostRound) fires after EVERY round, including
    // the last one before a level rotation. proceed_to_next_round_or_rotate
    // is invoked once PostRound expires (from tick_post_round).
    if (cfg().post_round_time > 0) {
        enter_post_round();
    } else {
        // No celebration window configured — proceed immediately.
        proceed_to_next_round_or_rotate();
    }
}

void process_pending_end()
{
    if (!g_rounds_runtime.end_pending) return;

    rf::Player* winner = g_rounds_runtime.pending_winner;
    const RoundEndReason reason = g_rounds_runtime.pending_reason;
    g_rounds_runtime.end_pending = false;
    g_rounds_runtime.pending_winner = nullptr;
    g_rounds_runtime.pending_reason = RoundEndReason::EarlyEnd;

    if (g_rounds_runtime.state != RoundState::Active) return;

    if (g_rounds_callbacks.on_round_end) {
        g_rounds_callbacks.on_round_end(winner, reason);
    }

    advance_to_next_round_or_change_level();
}

void tick_active()
{
    // 1. Gametype-driven early end (e.g. LMS one-alive condition)
    if (g_rounds_callbacks.should_end_round) {
        rf::Player* w = nullptr;
        if (g_rounds_callbacks.should_end_round(&w)) {
            rounds_request_end(w, RoundEndReason::EarlyEnd);
            return;
        }
    }

    // 2. Time-up
    if (rf::level.time >= g_rounds_runtime.round_deadline_level_time) {
        rf::Player* w = nullptr;
        if (g_rounds_callbacks.resolve_timeout_winner) {
            w = g_rounds_callbacks.resolve_timeout_winner();
        }
        rounds_request_end(w, RoundEndReason::TimeUp);
        return;
    }
}

void tick_post_round()
{
    const float remaining = g_rounds_runtime.post_round_deadline_level_time - rf::level.time;
    if (remaining <= 0.0f) {
        // Celebration window over. Decide rotate (max rounds) vs continue
        // (next intermission + round).
        proceed_to_next_round_or_rotate();
    }
}

void tick_intermission()
{
    const float remaining = g_rounds_runtime.intermission_deadline_level_time - rf::level.time;
    if (remaining <= 0.0f) {
        // Intermission deadline reached. Only transition if the gametype says
        // we have enough players; otherwise hold here (no extension, no
        // re-countdown) and let rounds_do_frame check again next frame.
        if (!ready_to_start_round()) {
            announce_waiting_once();
            return;
        }
        start_round();
        return;
    }

    // Per-second countdown render + tick sounds are produced by the engine's
    // built-in last-10-seconds countdown (driven by multi_time_limit set in
    // enter_intermission). No explicit per-tick broadcast needed.
}

} // namespace

void rounds_register_callbacks(const RoundCallbacks& cb)
{
    g_rounds_callbacks = cb;
}

void rounds_do_frame()
{
    if (!g_rounds_callbacks.on_server_tick && !rounds_enabled_now()) {
        // Cheap exit when no gametype is using us and rounds aren't on.
        if (g_rounds_runtime.state != RoundState::Inactive) g_rounds_runtime.reset();
        return;
    }

    if (g_rounds_callbacks.on_server_tick && rf::is_server) {
        g_rounds_callbacks.on_server_tick();
    }

    if (!in_round_capable_gameplay()) return;
    if (!rounds_enabled_now()) {
        if (g_rounds_runtime.state != RoundState::Inactive) g_rounds_runtime.reset();
        return;
    }

    // Process any deferred end request first; it may transition the state.
    process_pending_end();

    switch (g_rounds_runtime.state) {
        case RoundState::Inactive:
            // pending_level_change: see RoundsRuntime field comment.
            if (g_rounds_runtime.pending_level_change) {
                break;
            }
            // Idle until enough players are present to start round 1.
            if (!ready_to_start_round()) {
                announce_waiting_once();
                break;
            }
            g_rounds_runtime.current = 0;
            // Use intermission as a pre-round-1 warmup so clients have time
            // to finish loading + first-spawn before the round timer starts.
            // on_round_cleanup runs in both branches so round 1 starts from
            // the same clean slate as subsequent rounds (matches the cleanup
            // pattern in proceed_to_next_round_or_rotate): any entities the
            // engine spawned during Inactive get killed and respawned cleanly
            // by on_round_begin; level items are restored to visible.
            if (cfg().intermission_time > 0) {
                enter_intermission();
            } else {
                if (g_rounds_callbacks.on_round_cleanup) {
                    g_rounds_callbacks.on_round_cleanup();
                }
                start_round();
            }
            break;
        case RoundState::Active:
            tick_active();
            break;
        case RoundState::PostRound:
            tick_post_round();
            break;
        case RoundState::Intermission:
            tick_intermission();
            break;
    }
}

void rounds_level_init()
{
    // Real level boundary: reset counter, state, AND drop any callbacks
    // registered by the previous gametype. The new gametype's level_init
    // hook re-registers them (e.g. lms_level_init_post for LMS).
    g_rounds_runtime.reset();
    g_rounds_callbacks = RoundCallbacks{};
}

void rounds_level_init_post()
{
    // No-op: rounds_do_frame() drives the Inactive -> Active transition once
    // the gametype's can_round_start() reports ready. This keeps level-load
    // and "enough players" gating in one place.
}

void rounds_on_player_init(rf::Player* player)
{
    if (!rf::is_server) return;
    if (!player) return;
    if (!gt_uses_rounds()) return;

    // Mid-round joins are treated as though that player is already out of the current round.
    if (g_rounds_runtime.state != RoundState::Active) return;
    if (g_rounds_callbacks.on_late_join) {
        g_rounds_callbacks.on_late_join(player);
    }
}

RoundState rounds_get_state() { return g_rounds_runtime.state; }
int  rounds_get_current() { return g_rounds_runtime.current + 1; }
int  rounds_get_max() { return cfg().max_rounds; }

float rounds_get_remaining_seconds()
{
    if (g_rounds_runtime.state == RoundState::Active) {
        return std::max(0.0f, g_rounds_runtime.round_deadline_level_time - rf::level.time);
    }
    if (g_rounds_runtime.state == RoundState::PostRound) {
        return std::max(0.0f, g_rounds_runtime.post_round_deadline_level_time - rf::level.time);
    }
    if (g_rounds_runtime.state == RoundState::Intermission) {
        return std::max(0.0f, g_rounds_runtime.intermission_deadline_level_time - rf::level.time);
    }
    return 0.0f;
}

bool rounds_is_active() { return g_rounds_runtime.state == RoundState::Active; }
bool rounds_is_intermission() { return g_rounds_runtime.state == RoundState::Intermission; }
bool rounds_is_post_round() { return g_rounds_runtime.state == RoundState::PostRound; }
bool rounds_is_between_rounds() {
    return g_rounds_runtime.state == RoundState::PostRound || g_rounds_runtime.state == RoundState::Intermission;
}

void rounds_request_end(rf::Player* winner, RoundEndReason reason)
{
    if (g_rounds_runtime.state != RoundState::Active) return;
    // Last writer wins; that's fine because process_pending_end runs before
    // tick_active so concurrent same-frame requests collapse to one.
    g_rounds_runtime.end_pending = true;
    g_rounds_runtime.pending_winner = winner;
    g_rounds_runtime.pending_reason = reason;
}

void rounds_client_set_countdown(int duration_seconds)
{
    if (duration_seconds > 0) {
        g_client_countdown_deadline.set(duration_seconds * 1000);
        // Allow the engine to play the per-second tick sounds for the seconds
        // we're about to display.
        reset_n_seconds_played_array();
    } else {
        g_client_countdown_deadline.invalidate();
    }
}

void rounds_do_patch()
{
    multi_hud_render_countdown_hook.install();
}
