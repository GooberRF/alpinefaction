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
// Distance field from a waypoint with a sorted avoid set, cached for the current waypoint graph version. The reference
// stays valid until eight other fields have been computed; bot_waypoint_route computes some too, so do not hold one
// across a route search.
const BotWaypointDistanceField& bot_waypoint_distance_field(int source_waypoint, const std::vector<int>& avoidset = {});

// Shortest route length from one waypoint to a goal waypoint, infinite without a route. A goal not asked for recently
// costs one A*; once it repeats, its field is built and shared, so many starts against a few goals cost one search per
// goal.
float bot_waypoint_destination_distance(int from, int goal);
// Whether some route leads from one waypoint to the other, under the same link rules as bot_waypoint_route.
bool bot_waypoint_reachable(int from, int to);

void bot_waypoint_invalidate_components();
bool bot_waypoint_same_component(int a, int b);
