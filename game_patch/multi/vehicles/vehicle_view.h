#pragma once

#include "vehicle_internal.h"

// Driver's machine: write the delta_eye_phb store vphys skipped, so his view pitch still works.
void vehicle_feed_automobile_eye_input(rf::Entity* ep);
// Step the cockpit vmesh of a first-person spectate target; nothing else reaches it for him.
void vehicle_process_spectated_cockpit();
// Per frame, every remote rider: maintain OF_KEEP_ORIENT_ON_HOST, which stock keeps only locally.
void vehicle_sync_rider_orient_flags();

// The memoized driller view_forward prop index must not outlive the level's meshes.
void vehicle_view_level_init();

void vehicle_view_apply_patch();
