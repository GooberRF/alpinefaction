#pragma once

#include <vector>
#include <array>
#include <unordered_set>
#include "../rf/math/vector.h"

namespace rf
{
    struct Object;
}

constexpr int kMaxWaypointLinks = 6;
constexpr float kWaypointRadius = 4.0f / 3.0f;
constexpr float kWaypointLinkRadius = kWaypointRadius * 3.0f;
constexpr float kWaypointLinkRadiusEpsilon = 0.001f;
constexpr float kWaypointRadiusCompressionScale = 100.0f;
constexpr float kJumpPadAutoLinkRangeScale = 0.5f;
constexpr float kTeleEntranceAutoLinkRangeScale = 1.0f;
constexpr float kBridgeWaypointMaxGroundDistance = 2.5f;
constexpr int kMaxAutoSeedBridgeDrops = 5;
constexpr int kMaxAutoSeedBridgeWaypoints = 2048;
constexpr int kWptVersion = 1;

enum class WaypointType : int
{
    std = 0,
    std_new = 1,
    item = 2,
    respawn = 3,
    jump_pad = 4,
    lift_body = 5,
    lift_entrance = 6,
    lift_exit = 7,
    ladder = 8,
    ctf_flag = 9,
    crater = 10,
    tele_entrance = 11,
    tele_exit = 12,
};

enum class WaypointDroppedSubtype : int
{
    normal = 0,
    crouch_needed = 1,
    swimming = 2,
    falling = 3,
};

enum class WaypointItemSubtype : int
{
    // Item waypoint subtype maps directly to rf::item_info index.
    invalid = -1,
};

enum class WaypointRespawnSubtype : int
{
    all_teams = 0,
    red_team = 1,
    blue_team = 2,
    neutral = 3,
};

enum class WaypointJumpPadSubtype : int
{
    default_pad = 0,
};

enum class WaypointCtfFlagSubtype : int
{
    red = 0,
    blue = 1,
};

enum class WaypointZoneType : int
{
    control_point = 0,
    damaging_liquid_room = 1,
    damage_zone = 2,
    instant_death_zone = 3,
};

enum class WaypointZoneSource : int
{
    trigger_uid = 0,
    room_uid = 1,
    box_extents = 2,
};

enum class WaypointTargetType : int
{
    explosion = 0,
};

struct WaypointZoneDefinition
{
    WaypointZoneType type = WaypointZoneType::control_point;
    int trigger_uid = -1;
    int room_uid = -1;
    int identifier = -1;
    rf::Vector3 box_min{};
    rf::Vector3 box_max{};
};

struct WaypointNode
{
    rf::Vector3 pos{};
    std::array<int, kMaxWaypointLinks> links{};
    int num_links = 0;
    WaypointType type = WaypointType::std;
    int subtype = 0;
    int identifier = -1;
    std::vector<int> zones{};
    float link_radius = kWaypointLinkRadius;
    float cur_score = 0.0f;
    float est_score = 0.0f;
    int route = -1;
    int prev = -1;
    bool valid = true;
};

struct WaypointTargetDefinition
{
    int uid = -1;
    rf::Vector3 pos{};
    WaypointTargetType type = WaypointTargetType::explosion;
    std::vector<int> waypoint_uids{};
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

int add_waypoint(
    const rf::Vector3& pos, WaypointType type, int subtype, bool link_to_nearest, bool bidirectional_link,
    float link_radius = kWaypointLinkRadius, int identifier = -1, const rf::Object* source_object = nullptr,
    bool auto_assign_zones = true);
bool link_waypoint_if_clear(int from, int to);
bool can_link_waypoints(const rf::Vector3& a, const rf::Vector3& b);
int closest_waypoint(const rf::Vector3& pos, float radius);
void on_geomod_crater_created(const rf::Vector3& crater_pos, float crater_radius = 0.0f);
