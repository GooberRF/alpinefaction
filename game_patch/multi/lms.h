#pragma once

namespace rf
{
    struct Player;
    struct Entity;
}

// Called on the dedicated server right after the level finishes initializing.
// Registers LMS's round callbacks if the active gametype is LMS.
void lms_level_init_post();

// Per-frame pump on the server. Handles late-join spectate gating and any
// LMS-specific bookkeeping outside the rounds state machine.
void lms_do_frame();

// Pre-death hook: entity is about to die. Marks the player as out and
// suppresses any future respawn until the next round.
void lms_on_entity_will_die(rf::Entity* ep);

// Damage hook: record damage dealt by killer toward damaged player for the
// timer-expiry tiebreak. Called from entity_damage_hook *after* PvP
// modifiers have been applied.
void lms_on_pvp_damage(rf::Player* dealer, float real_damage);

// Spawn gate: returns true if the player is permitted to spawn right now in
// LMS. False in mid-round states (late-joiners + eliminated players wait).
bool lms_can_player_spawn(rf::Player* player);

// Cleanup on disconnect — clear any references and recompute alive count if
// needed (handled implicitly by removing the player from the global list).
void lms_on_player_disconnect(rf::Player* player);

// HUD support: server-side count of currently alive (non-out, has-entity) players.
int lms_alive_count();
