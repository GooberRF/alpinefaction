#pragma once

#include "../../rf/math/vector.h"
#include "../../rf/math/matrix.h"

// Client-side cosmetic tracers for vehicle guns in multiplayer: a streak launched from a
// vehicle weapon round's creation, flying at the weapon's speed to where a local ray ends.

void vehicle_tracers_level_init();
void vehicle_tracers_clear();
// From every entity_fire_weapon round creation, after the round exists.
void vehicle_tracers_on_weapon_created(int weapon_type, int parent_handle, const rf::Vector3& pos,
                                       const rf::Matrix3& orient);
void vehicle_tracers_render();
void vehicle_tracers_install();
