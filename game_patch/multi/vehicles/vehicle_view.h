#pragma once

#include "vehicle_internal.h"

// Driver's machine: write the delta_eye_phb store vphys skipped, so his view pitch still works.
void vehicle_feed_automobile_eye_input(rf::Entity* ep);
// Step the cockpit vmesh of a first-person spectate target; nothing else reaches it for him.
void vehicle_process_spectated_cockpit();
// Per frame, every remote rider: maintain OF_KEEP_ORIENT_ON_HOST, which stock keeps only locally.
void vehicle_sync_rider_orient_flags();

enum class VehicleFpShotStart
{
    stock,     // not his own first-person shot, or the muzzle pose is stale
    world,     // *out_pos: a turret's drawn muzzle_1, in world space
    eye_frame, // *out_pos: his fpgun's muzzle where he sees it, or zero with none; (right, up, fwd) of the scene eye
};

// Where this machine's first-person viewer sees his own from-eye round start, when shooter is him.
// A muzzle further than max_dist from fire_pos is a stale pose.
VehicleFpShotStart vehicle_fp_own_shot_start(rf::Entity* hull, rf::Entity* shooter, const rf::Vector3& fire_pos,
                                             float max_dist, rf::Vector3* out_pos);

// The memoized view_forward prop indices must not outlive the level's meshes.
void vehicle_view_level_init();

void vehicle_view_apply_patch();
