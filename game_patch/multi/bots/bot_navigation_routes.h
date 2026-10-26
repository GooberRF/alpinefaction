#pragma once

#include "bot_waypoint_route.h"
#include "../../rf/entity.h"
#include <vector>

float bot_nav_compute_route_score(
    const std::vector<int>& path,
    float target_distance = 0.0f);
// Never below bot_nav_compute_route_score(path).
float bot_nav_compute_route_score_upper_bound(const std::vector<int>& path);

// Distance field from a waypoint with a sorted avoid set, cached for the current waypoint graph version. The reference
// stays valid until eight other fields have been computed.
const BotWaypointDistanceField& bot_nav_distance_field(int source_waypoint, const std::vector<int>& avoidset = {});
// Answered from the distance field of from_waypoint.
bool bot_nav_is_waypoint_reachable(int from_waypoint, int to_waypoint);

void bot_nav_prune_failed_edge_cooldowns();
void bot_nav_register_failed_edge_cooldown(
    int from_waypoint,
    int to_waypoint,
    int cooldown_ms);
void bot_nav_register_failed_edge_cooldown_bidirectional(
    int waypoint_a,
    int waypoint_b,
    int cooldown_ms);
bool bot_nav_is_failed_edge_cooldown_active_no_prune(
    int from_waypoint,
    int to_waypoint);
bool bot_nav_path_contains_failed_edge_cooldown(const std::vector<int>& path);
void bot_nav_prune_failed_waypoint_blacklist();
int bot_nav_choose_blacklist_waypoint_for_failed_link(int from_waypoint, int to_waypoint);
void bot_nav_blacklist_waypoint_temporarily(int waypoint, int cooldown_ms);
bool bot_nav_is_waypoint_blacklisted_no_prune(int waypoint);
void bot_nav_append_blacklisted_waypoints_to_avoidset(
    std::vector<int>& avoidset,
    int start_waypoint,
    int goal_waypoint);
bool bot_nav_is_link_traversable_for_route(int from_waypoint, int to_waypoint);
bool bot_nav_path_has_unclimbable_upward_links(const std::vector<int>& path);
bool bot_nav_link_midpoint_has_support(int from_waypoint, int to_waypoint);

void bot_nav_record_waypoint_visit(int waypoint);
bool bot_nav_detect_recent_waypoint_ping_pong_loop(
    int& out_waypoint_a,
    int& out_waypoint_b);

bool bot_nav_pick_waypoint_route_to_goal_randomized_after_stuck(
    int start_waypoint,
    int goal_waypoint,
    int avoid_waypoint,
    int repath_ms);
bool bot_nav_pick_waypoint_route_to_goal_long_detour(
    int start_waypoint,
    int goal_waypoint,
    int repath_ms);
bool bot_nav_pick_waypoint_route(int start_waypoint);
bool bot_nav_pick_waypoint_route_to_goal(
    int start_waypoint,
    int goal_waypoint,
    int repath_ms);
