#pragma once

#include "../../rf/entity.h"

struct WaypointTargetDefinition;

bool bot_sync_pursuit_target(rf::Entity* enemy_target);
void bot_update_pursuit_recovery_timer();
void bot_update_move_target(
    const rf::Entity& local_entity,
    rf::Entity* enemy_target,
    bool enemy_has_los,
    bool pursuing_enemy_goal,
    rf::Vector3& move_target,
    bool& has_move_target);

// Waypoint to fire at a crater target from: a standoff clear of the crater weapon's blast, or one next to the target
// for weapons used up close. out_standoff says which.
bool bot_nav_find_crater_fire_waypoint(
    const rf::Entity& local_entity,
    const WaypointTargetDefinition& target,
    int& out_waypoint,
    rf::Vector3& out_pos,
    bool& out_standoff);
// A crater target the bot could not find a place to fire from is left alone for a while.
void bot_nav_note_failed_crater_target(int target_uid, int cooldown_ms);
bool bot_nav_crater_target_failed_recently(int target_uid);
