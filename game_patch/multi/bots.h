#pragma once

#include <vector>
#include "../misc/waypoints.h"
#include "../rf/math/vector.h"
#include "../rf/os/timestamp.h"

struct BotNavigationState
{
    std::vector<int> path{};
    int next_index = 0;
    int goal_waypoint = 0;
    rf::Timestamp repath_timer{};
    rf::Timestamp stuck_timer{};
    rf::Vector3 last_pos{};
};

constexpr float kBotWaypointSearchRadius = 60.0f;
constexpr float kBotWaypointReachRadius = kWaypointRadius * 1.5f;
constexpr float kBotWaypointStuckDistance = 0.75f;
constexpr int kBotWaypointPickAttempts = 8;
constexpr int kBotWaypointRepathMs = 6000;
constexpr int kBotWaypointStuckCheckMs = 1500;

void bot_ai_do_frame();
