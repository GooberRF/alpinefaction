#pragma once

namespace rf
{
    struct Entity;
    struct Weapon;
    struct Vector3;
}

void projectile_lag_comp_init();
void projectile_lag_comp_on_level_init();
void projectile_lag_comp_record_positions();
// Server: advance a freshly created projectile by the shooter's half ping. Client: advance a remote
// player's relayed projectile by own half ping + shooter half ping so it lines up with the server's.
void projectile_lag_comp_advance_weapon(rf::Entity* shooter, rf::Weapon* wp);
// Rewinds entities to the killer's view if the killer is a player entity; returns true if it did,
// in which case restore_entities_after_projectile must be called after the damage is applied.
// full_ping: the triggering action was a separate, un-advanced packet (remote charge detonation),
// so the shooter's view is a full round trip behind the server instead of half.
bool projectile_lag_comp_rewind_for_killer(int killer_handle, bool full_ping = false);
void restore_entities_after_projectile();
// While rewound: how far this entity was moved from its live position (zero if it was not rewound)
rf::Vector3 projectile_lag_comp_rewound_offset(int handle);
