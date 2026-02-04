#pragma once

#include <vector>
#include <array>
#include <unordered_set>
#include "../rf/math/vector.h"

constexpr int kMaxWaypointLinks = 6;
constexpr float kWaypointRadius = 4.0f / 3.0f;
constexpr float kWaypointLinkRadius = kWaypointRadius * 3.0f;
constexpr int kWptVersion = 1;

struct WaypointNode
{
    rf::Vector3 pos{};
    std::array<int, kMaxWaypointLinks> links{};
    int num_links = 0;
    int type = 0;
    int subtype = 0;
    float cur_score = 0.0f;
    float est_score = 0.0f;
    int route = -1;
    int prev = -1;
    bool valid = true;
};

struct WpCacheNode
{
    int index = -1;
    int axis = 0;
    WpCacheNode* left = nullptr;
    WpCacheNode* right = nullptr;
    rf::Vector3 min{};
    rf::Vector3 max{};
};

void waypoints_init();
void waypoints_do_frame();
void waypoints_render_debug();
void waypoints_level_init();
void waypoints_level_reset();

int waypoints_closest(const rf::Vector3& pos, float radius);
bool waypoints_route(int from, int to, const std::unordered_set<int>& avoidset, std::vector<int>& out_path);
