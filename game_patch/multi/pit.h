#pragma once

#include <cstdint>

namespace rf
{
    struct Player;
    struct Entity;
}

// Replicated Pit role for the scoreboard grouping. Wire values are stable.
enum PitRole : uint8_t
{
    PIT_ROLE_DUELER = 0,
    PIT_ROLE_QUEUED = 1,
    PIT_ROLE_NOT_QUEUED = 2,
};

// Reset all Pit state on a real level boundary. Called from the pre-level-init
// dispatch alongside the other gametypes' level_init hooks.
void pit_level_init();

// Called on the dedicated server right after the level finishes initializing.
// Registers Pit's round callbacks if the active gametype is Pit.
void pit_level_init_post();

// Per-frame pump on the server. Handles auto-enqueue of new players, prunes
// disconnected queue entries, and auto-spawns the current duelers.
void pit_do_frame();

// Pre-death hook: entity is about to die. Marks the player as out so the
// respawn gate blocks a respawn until the next round; round end is detected on
// the next tick.
void pit_on_entity_will_die(rf::Entity* ep);

// Spawn gate: returns true if the player is permitted to spawn right now in
// Pit. Only the two current duelers may spawn during a round; queued and
// opted-out players are denied with a throttled message.
bool pit_can_player_spawn(rf::Player* player);

// Cleanup on disconnect — remove the player from the queue and clear any
// dueler slot holding them. should_end_round handles the forfeit/backfill.
void pit_on_player_disconnect(rf::Player* player);

// Server-side join/leave/toggle of the duel queue (0 = leave, 1 = join,
// 2 = toggle). Called from the af_req_pit_queue packet handler and directly
// for a listen-server host pressing the Ready key.
void pit_handle_queue_request(rf::Player* player, uint8_t action);

// Hide every level item except Shotgun / Rocket Launcher pickups, and destroy
// any dropped weapons, so the arena only offers those two pickups.
void pit_reset_world_items();

// Recompute and send queue state (dueler / queued position) to a single
// player. Used by the broadcast and for late-joiner initial state sync.
void pit_send_queue_state(rf::Player* player);

// Recompute and broadcast queue state (dueler / queued position) to every
// connected non-browser player. Call on every queue/pairing mutation. Also
// broadcasts the full Pit roster so all clients can group the scoreboard.
void pit_broadcast_queue_states();

// Send the full Pit roster to a single player (late-joiner initial sync).
void pit_send_roster_to(rf::Player* player);
