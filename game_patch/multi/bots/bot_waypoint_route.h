#pragma once

#include <vector>

bool bot_waypoint_route(
    int from,
    int to,
    const std::vector<int>& avoidset,
    std::vector<int>& out_path);

// Shortest route length from one waypoint to every waypoint, under the same link rules and costs as
// bot_waypoint_route. Unreachable waypoints keep an infinite distance.
struct BotWaypointDistanceField
{
    int source = 0;
    std::vector<int> avoidset{};
    std::vector<float> dist{};
    std::vector<int> prev{};
};

void bot_waypoint_compute_distance_field(int from, const std::vector<int>& avoidset, BotWaypointDistanceField& out);
bool bot_waypoint_distance_field_path(const BotWaypointDistanceField& field, int to, std::vector<int>& out_path);

void bot_waypoint_invalidate_components();
bool bot_waypoint_same_component(int a, int b);
