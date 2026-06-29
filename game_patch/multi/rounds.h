#pragma once

#include "../rf/os/timestamp.h"

namespace rf
{
    struct Player;
}

enum class RoundState : uint8_t
{
    Inactive = 0,    // not in a round-based gametype, or level not yet loaded
    Active = 1,      // round in progress
    PostRound = 2,   // brief celebration window after a round ends (winner still alive)
    Intermission = 3 // countdown to next round (winner has been killed)
};

enum class RoundEndReason : uint8_t
{
    EarlyEnd = 0,    // gametype-defined end condition met (e.g. LMS: one player left)
    TimeUp = 1,      // round time expired; tiebreak resolves winner
    LevelChange = 3  // level rotation triggered; not a real end
};

// Callbacks registered by the consuming gametype. None are required.
struct RoundCallbacks
{
    // Fired once when a fresh round begins (after intermission, after the
    // server has respawned everyone). Use to clear per-round state.
    void (*on_round_begin)() = nullptr;

    // Fired when the round is ending and a winner has been determined (may be
    // null for a no-winner timeout). Use to increment round_wins, broadcast
    // results, etc. The winner's entity is still ALIVE at this point — the
    // celebration window (PostRound state) hasn't ended yet.
    void (*on_round_end)(rf::Player* winner, RoundEndReason reason) = nullptr;

    // Fired when the celebration window (PostRound) expires, just before the
    // intermission countdown begins. This is the gametype's "between rounds
    // cleanup" hook: kill any surviving entities, clean up dropped items,
    // reset level items, etc.
    void (*on_round_cleanup)() = nullptr;

    // Per-frame check the gametype can use to declare an early round end
    // (e.g. LMS detecting <=1 alive). If returning true, *out_winner may be
    // set to the surviving player (or left null). Called only while the
    // round is Active.
    bool (*should_end_round)(rf::Player** out_winner) = nullptr;

    // Resolve the tiebreak winner when the round timer expires with no other
    // condition met. Returning nullptr means "no winner this round".
    rf::Player* (*resolve_timeout_winner)() = nullptr;

    // Called every server frame regardless of round state. Use for gametype
    // bookkeeping that should not gate round transitions.
    void (*on_server_tick)() = nullptr;

    // Returning false keeps the rounds system idle: round 1 won't start, and
    // an in-progress intermission won't transition into the next round.
    // Used to gate on minimum player count, warmup, etc.
    bool (*can_round_start)() = nullptr;

    // Fired when a player joins while a round is already Active. The
    // gametype's job is to mark the player as "out for this round" — the
    // canonical marker is rf::Player::round_is_out, which the spawn gate
    // typically checks. The player is reset/reincluded normally at the next
    // on_round_begin. Not called for joins during Inactive/PostRound/
    // Intermission — the spawn gate handles those cases naturally.
    void (*on_late_join)(rf::Player* player) = nullptr;
};

// Register the gametype's hooks. Pass {} to clear. Safe to call repeatedly;
// typically a gametype installs its callbacks at server startup.
void rounds_register_callbacks(const RoundCallbacks& cb);

// Called from server_do_frame; pumps the state machine and gametype hooks.
void rounds_do_frame();

// Called from multi_init_player when a new Player object is created. Fires
// the gametype's on_late_join callback if a round is currently Active.
// No-op on clients and when no round-based gametype is loaded.
void rounds_on_player_init(rf::Player* player);

// Called on real level load (when filename changes). Resets the round counter
// and starts a fresh series if the active gametype is round-based.
void rounds_level_init();

// Called after the engine has finished spawning players on a new level so the
// initial round can fire its on_round_begin callback.
void rounds_level_init_post();

// Public state accessors.
RoundState rounds_get_state();
int  rounds_get_current(); // 1-indexed for display
int  rounds_get_max();
float rounds_get_remaining_seconds(); // remaining time in current round
bool rounds_is_active(); // active round (not post-round, not intermission, not inactive)
bool rounds_is_intermission();
bool rounds_is_post_round();
bool rounds_is_between_rounds(); // true during post-round OR intermission

// Force end the current round. Safe to call from anywhere; defers to next
// frame. winner may be null. No-op outside Active state.
void rounds_request_end(rf::Player* winner, RoundEndReason reason);

// Install engine hooks.
void rounds_do_patch();

// --- Client-only ---
// Called from the af_server_msg packet handler when the server has signalled
// a round-start countdown. Activates the in-engine big-number countdown render
// for the given duration without touching multi_time_limit. Pass 0 / negative
// to cancel an in-flight countdown immediately.
void rounds_client_set_countdown(int duration_seconds);
