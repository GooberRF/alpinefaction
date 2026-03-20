#pragma once

#include "../../rf/entity.h"
#include "../../rf/object.h"

void clear_waypoint_route();
bool is_following_waypoint_link(const rf::Entity& entity);
bool start_recovery_anchor_reroute(const rf::Entity& entity, int avoid_waypoint);
void clear_synthetic_movement_controls();
void reset_client_bot_state();

bool try_recover_from_corner_probe(
    const rf::Entity& entity,
    const rf::Vector3& move_target);

bool update_waypoint_target(const rf::Entity& entity);
bool update_waypoint_target_towards(
    const rf::Entity& entity,
    const rf::Vector3& destination,
    const rf::Vector3* los_target_eye_pos,
    const rf::Object* los_target_obj,
    int repath_ms);

bool update_waypoint_target_for_local_los_reposition(
    const rf::Entity& entity,
    const rf::Entity& enemy_target,
    bool enemy_has_los);

bool bot_nav_try_emergency_direct_move_target(
    const rf::Entity& entity,
    rf::Vector3& out_move_target);
