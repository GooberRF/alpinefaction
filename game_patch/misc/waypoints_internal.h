#pragma once

#include "waypoints.h"
#include <optional>
#include <string_view>
#include <vector>

extern std::vector<WaypointNode> g_waypoints;
extern std::vector<WaypointZoneDefinition> g_waypoint_zones;
extern std::vector<WaypointTargetDefinition> g_waypoint_targets;
extern int g_debug_waypoints_mode;
extern bool g_drop_waypoints;

bool is_waypoint_bot_mode_active();
bool is_multiplayer_client();
bool are_waypoint_commands_enabled_for_local_client();
float sanitize_waypoint_link_radius(float link_radius);
int cycle_waypoint_dropped_subtype(int raw_subtype, int direction);
std::string_view waypoint_dropped_subtype_name(int raw_subtype);
std::string_view waypoint_type_name(WaypointType type);
bool waypoint_type_is_standard(WaypointType type);
std::string_view waypoint_zone_type_name(WaypointZoneType type);
std::string_view waypoint_target_type_name(WaypointTargetType type);
WaypointZoneSource resolve_waypoint_zone_source(const WaypointZoneDefinition& zone);
bool waypoint_zone_type_is_bridge(WaypointZoneType type);
rf::Vector3 point_min(const rf::Vector3& a, const rf::Vector3& b);
rf::Vector3 point_max(const rf::Vector3& a, const rf::Vector3& b);
float distance_sq(const rf::Vector3& a, const rf::Vector3& b);
bool remove_waypoint_by_uid(int uid);
int add_waypoint_zone_definition(WaypointZoneDefinition zone);
int add_waypoint_target(
    const rf::Vector3& pos,
    WaypointTargetType type,
    std::optional<int> preferred_uid = std::nullopt);
bool remove_waypoint_zone_definition(int zone_uid);
bool remove_waypoint_target_by_uid(int target_uid);
int remove_waypoint_links_from_all(int from);
int remove_waypoint_links_to_from_all(int to);
void link_waypoint(int from, int to);
bool waypoint_has_link_to(int from, int to);
void refresh_all_waypoint_zone_refs();
std::vector<int> collect_target_waypoint_uids(const rf::Vector3& pos);
std::vector<int> collect_target_link_waypoint_uids(const rf::Vector3& pos);
void normalize_target_waypoint_uids(std::vector<int>& waypoint_uids);
WaypointTargetDefinition* find_waypoint_target_by_uid(int target_uid);
