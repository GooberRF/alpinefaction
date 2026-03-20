#pragma once

#include "../../rf/entity.h"

void bot_refresh_goal_state(
    const rf::Entity& local_entity,
    rf::Entity* enemy_target,
    bool enemy_has_los);
