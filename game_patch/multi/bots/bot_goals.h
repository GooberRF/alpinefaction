#pragma once

#include "../../rf/entity.h"

void bot_refresh_goal_state(
    const rf::Entity& local_entity,
    rf::Entity* enemy_target,
    bool enemy_has_los);

struct BotObjectiveAnchor
{
    rf::Vector3 pos{};
    // An enemy is taking it: a stolen flag, a point of the team's being captured.
    bool threatened = false;
};

// The places the active bot's team holds or contests: its flag base, the control points. Returns how many it wrote.
int bot_goals_objective_anchors(const rf::Entity& local_entity, BotObjectiveAnchor* out, int max_count);
// Where the active bot's team objective is now, for a driver carrying the bot there: the capture point while it holds
// the enemy flag, its own flag while that is out, else the enemy flag (unless a team-mate carries it); the nearest
// control point under attack or to take, one under attack counted at half its distance.
// from: where the choice among control points is made from (the hull a driver will start in); the bot by default.
bool bot_goals_transport_objective(const rf::Entity& local_entity, rf::Vector3& out_pos,
                                   const rf::Vector3* from = nullptr);
