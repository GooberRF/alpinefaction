#pragma once

#include <vector>
#include <array>
#include <unordered_set>
#include "../rf/math/vector.h"

constexpr int kMaxWaypointLinks = 6;
constexpr float kWaypointRadius = 4.0f / 3.0f;
constexpr float kWaypointLinkRadius = kWaypointRadius * 3.0f;
constexpr int kWptVersion = 1;

enum class WaypointType : int
{
    dropped_legacy = 0,
    dropped = 1,
    item = 2,
    respawn = 3,
    jump_pad = 4,
    jump_pad_landing = 5,
    lift_entrance = 6,
    lift_exit = 7,
    ladder_entrance = 8,
    ctf_flag = 9,
    control_point = 10,
};

enum class WaypointDroppedSubtype : int
{
    normal = 0,
    crouch_needed = 1,
    swimming = 2,
    falling = 3,
};

struct WaypointNode
{
    rf::Vector3 pos{};
    std::array<int, kMaxWaypointLinks> links{};
    int num_links = 0;
    WaypointType type = WaypointType::dropped_legacy;
    int subtype = 0;
    float link_radius = kWaypointLinkRadius;
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
int waypoints_count();
bool waypoints_get_pos(int index, rf::Vector3& out_pos);
bool waypoints_get_type_subtype(int index, int& out_type, int& out_subtype);
int waypoints_get_links(int index, std::array<int, kMaxWaypointLinks>& out_links);
bool waypoints_has_direct_link(int from, int to);
bool waypoints_link_is_clear(int from, int to);
