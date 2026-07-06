#pragma once

namespace rf
{
    struct Player;
    struct Entity;
}

// Wipeout (NG_TYPE_WO): a round-based TEAM mode where players respawn repeatedly
// within a round on an escalating delay (5s * deaths, reset each round). A team
// loses the round the instant its entire roster is simultaneously down (a
// "wipe"); the survivors are awarded the round. Simultaneous double-wipes are a
// no-contest. Match is best-of-7 with early clinch + sudden death, implemented
// via the rounds.cpp is_match_over arbiter.

// Called on real level load (pre-init) on server and client. Resets scores and
// match state (a new map == a new match).
void wipeout_level_init();

// Called after the level finishes initializing. On the server, registers
// Wipeout's round callbacks and clears per-player round state.
void wipeout_level_init_post();

// Per-frame pump on the server: auto-respawns players whose escalating delay has
// elapsed (the repeated mid-round respawn) and tracks per-round participation.
void wipeout_do_frame();

// Spawn gate: false while a player must wait (late joiner / between rounds).
// The escalating per-death delay itself is enforced by the server-side
// respawn_timer check, not here.
bool wipeout_can_player_spawn(rf::Player* player);

// Disconnect hook — no per-player teardown needed (counts derive from the list).
void wipeout_on_player_disconnect(rf::Player* player);

// True when the given player's next spawn is a mid-round respawn (cluster near
// teammates) rather than the round's first spawn (standard TDM logic). Used by
// the spawn-point selection hook in server.cpp.
bool wipeout_is_subsequent_spawn(rf::Player* player);

// Team round-win scores (shown on the HUD, synced via the stock team_scores
// packet). Setters are client-only; the server is authoritative.
int wipeout_get_red_team_score();
int wipeout_get_blue_team_score();
void wipeout_set_red_team_score(int v);
void wipeout_set_blue_team_score(int v);

// HUD helpers (client-safe): how many players on a team are currently alive vs.
// waiting (dead/awaiting-respawn or a late joiner not yet in the round).
int wipeout_count_team_alive(int team);
int wipeout_count_team_waiting(int team);
