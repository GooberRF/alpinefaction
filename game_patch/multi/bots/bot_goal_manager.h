#pragma once

#include "../../rf/entity.h"

void bot_goal_manager_refresh(
    const rf::Entity& local_entity,
    rf::Entity* enemy_target,
    bool enemy_has_los);

void bot_goal_manager_ensure_active(
    const rf::Entity& local_entity,
    rf::Entity* enemy_target,
    bool enemy_has_los,
    int fallback_eval_delay_ms = 180);

void bot_goal_manager_refresh_and_ensure(
    const rf::Entity& local_entity,
    rf::Entity* enemy_target,
    bool enemy_has_los,
    int fallback_eval_delay_ms = 180);

void bot_goal_manager_force_refresh_and_ensure(
    const rf::Entity& local_entity,
    rf::Entity* enemy_target,
    bool enemy_has_los,
    int fallback_eval_delay_ms = 180);
