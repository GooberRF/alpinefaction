#pragma once

#include <vector>

bool bot_waypoint_route(
    int from,
    int to,
    const std::vector<int>& avoidset,
    std::vector<int>& out_path);

void bot_waypoint_invalidate_components();
bool bot_waypoint_same_component(int a, int b);
