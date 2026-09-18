#pragma once
#include <cstdint>

namespace rf { struct Entity; }

struct RumbleEffect
{
    uint16_t lo_motor      = 0; // low-frequency (left) body motor
    uint16_t hi_motor      = 0; // high-frequency (right) body motor
    uint16_t trigger_motor = 0; // trigger motor strength (if supported by SDL_RumbleGamepadTriggers)
    uint32_t duration_ms   = 0;
};

// Main per-frame rumble coordinator. Called once per frame from gamepad_do_frame().
void rumble_do_frame();

// Called from entity_damage_hook when the local player takes damage.
// Handles melee (DT_BASH), explosive (DT_EXPLOSIVE), and fire (DT_FIRE) hits.
void rumble_on_player_hit(float damage, int damage_type);

// Called from the entity_fire_primary_weapon hook for every entity weapon fire event.
// Triggers a strong body-only rumble pulse when firer is the turret the local player is on.
void rumble_on_turret_fire(rf::Entity* firer);
