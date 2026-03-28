#pragma once

// Main per-frame rumble coordinator. Called once per frame from gamepad_do_frame().
void rumble_do_frame();

// Called from entity_damage_hook when the local player is hit.
// Only reacts to melee (DT_BASH) and explosive (DT_EXPLOSIVE) damage.
void rumble_on_player_hit(float damage, int damage_type);
