#pragma once

#include <cstddef>
#include <vector>
#include <common/alpine_waypoint_helper.h>
#include "../../rf/math/vector.h"

// The graph vehicle bots navigate on: the AWP's vehicle layer (made from the level's Waypoint Helpers when the AWP is
// generated), its links one-way where it says so and each taking hulls up to a size, and its landing nodes' exits
// found among the foot waypoints. Every query rebuilds the graph first if the layer changed since (a load, a
// generation or an edit).

// Nearest-first candidates a corridor or exit search traces before giving up.
inline constexpr std::size_t kVehicleNavTraceCandidates = 8;
// A rider is let out over dry ground this close below.
inline constexpr float kVehicleNavLandingGroundProbe = 3.0f;
// Rise per unit run a car climbs (about 33 degrees) and drop per unit run it drives down (45 degrees: steeper than it
// climbs, so a slope too steep to drive up can still be driven down, yet short of the jeep tipping forward near 56),
// plus a step it rolls over.
inline constexpr float kVehicleNavCarMaxGrade = 0.65f;
inline constexpr float kVehicleNavCarMaxDropGrade = 1.0f;
inline constexpr float kVehicleNavCarStepClimb = 0.6f;
// A sub's line is sampled for water this far apart, at most this many times.
inline constexpr float kVehicleNavWaterProbeStep = 5.0f;
inline constexpr int kVehicleNavWaterProbeMax = 64;
// A layer route starts at one of this many nodes nearest the hull, within this reach (a ground hull, a fighter or
// sub), that a clear corridor reaches. The ground reach takes a hull from its factory onto a node a mapper put nearby.
inline constexpr std::size_t kVehicleNavStartCandidates = 4;
inline constexpr float kVehicleNavGroundStartReach = 80.0f;
inline constexpr float kVehicleNavFlightStartReach = 200.0f;

// The part of the graph a hull can use.
struct BotVehicleNavClass
{
    alpine_waypoint_helper::Type layer = alpine_waypoint_helper::Type::air;
    // Ground only: the hull's own size; a node or link takes it when its size is at least this.
    alpine_waypoint_helper::SizeClass size = alpine_waypoint_helper::SizeClass::jeep;
    // vehicle_physics_type_corridor_radius of the hull's type.
    float corridor_radius = 0.0f;
};

// False for a type that does not drive or fly itself (a turret, anything not a vehicle).
bool bot_vehicle_nav_class_for_type(int entity_type, BotVehicleNavClass& out);

void bot_vehicle_nav_level_init();
// Bumped on every rebuild: a node index or path from an older version is stale.
int bot_vehicle_nav_version();

struct BotVehicleNavNode
{
    rf::Vector3 pos{};
    // Open space around the point; a follower may cut corners inside it.
    float radius = 0.0f;
    bool landing = false;
};
bool bot_vehicle_nav_node(int index, BotVehicleNavNode& out);

// A* over the class's nodes, along links in their direction that take the hull's size; the path runs from `from` to
// `to`, both included. False when there is none.
bool bot_vehicle_nav_find_path(int from, int to, const BotVehicleNavClass& cls, std::vector<int>& out_path);
// A path from one of the nodes nearest pos a route of the class may start at (the goal itself counts) to goal, tried
// nearest first, as a planned route starts. No traces. False when none reaches it.
bool bot_vehicle_nav_find_path_near(const rf::Vector3& pos, int goal, const BotVehicleNavClass& cls,
                                    std::vector<int>& out_path);
// The landing node of the class nearest pos within max_dist, or -1. Asked for its exit waypoint, only a node whose exit
// is found counts.
int bot_vehicle_nav_landing_node_near(const rf::Vector3& pos, const BotVehicleNavClass& cls, float max_dist,
                                      int* out_exit_waypoint = nullptr);

// A hull-wide drive from `from` to `to`: the three rays a little above the ground under both ends, and the ground
// sampled along it, with no drop or climb steeper than a car takes and no liquid deeper than a car wades.
bool bot_vehicle_nav_ground_corridor_clear(const rf::Vector3& from, const rf::Vector3& to, float half_width);
// Room for a hull of half_width along the floor from `from` to `to`: rays 1.25 u over the floor, sampled every 2 u or
// so under the centre and under each hull edge, along the centre and each edge between samples and across the hull at
// each sample. An edge follows its own floor only across an even slope a hull can sit on (rise up to half_width x the
// climb grade, drop up to half_width x the drop grade, the floor halfway out within 0.3 u of halfway between), so bumps
// and such slopes stay below the rays while walls, banks, barriers and posts in the hull's path taller than 1.25 u
// over the floor the rays follow (the centre floor, or the even slope they tilt with) block them, whatever the hull's
// width. out_hit, when given, gets a point a blocked ray hit (the start when an end has no ground under it).
bool bot_vehicle_nav_ground_hull_clear(const rf::Vector3& from, const rf::Vector3& to, float half_width,
                                      rf::Vector3* out_hit = nullptr);
// The flight corridor's five rays alone (no water test): true and a point one hit when one is blocked.
bool bot_vehicle_nav_corridor_hit(const rf::Vector3& from, const rf::Vector3& to, float radius, rf::Vector3& out_hit);
// The ground corridor's ground checks for a drive in this direction only, sampled finer: a drop may be steeper than a
// climb, but no single drop between samples deeper than a car takes.
bool bot_vehicle_nav_ground_drive_clear(const rf::Vector3& from, const rf::Vector3& to);
bool bot_vehicle_nav_segment_clear(const rf::Vector3& from, const rf::Vector3& to);

// How a route was found: straight at the destination, over the vehicle layer, or (ground, where no layer road reaches)
// over the terrain waypoints.
enum class BotVehicleRouteKind
{
    none,
    direct,
    layer,
    terrain,
};

// A graph link a route used, by layer node index or waypoint index as its kind numbers nodes.
struct BotVehicleNavLink
{
    BotVehicleRouteKind kind = BotVehicleRouteKind::none;
    int from = -1;
    int to = -1;
};

struct BotVehicleRoute
{
    BotVehicleRouteKind kind = BotVehicleRouteKind::none;
    // The bot_vehicle_nav_version its layer node indices number.
    int nav_version = 0;
    // The points to steer through, the node each one is (-1: the destination itself), and how far a follower may
    // cut the corner at each.
    std::vector<rf::Vector3> points;
    std::vector<int> nodes;
    std::vector<float> slack;
};

// A ground hull's route from `from` towards dest, ending at dest or at the drivable point nearest it within
// max_end_dist. Never uses an excluded link. False when there is none. A terrain route checks only its first links for
// the hull's width; a later repath finds any narrow one further on.
bool bot_vehicle_nav_plan_ground_route(const rf::Vector3& from, const rf::Vector3& dest, const BotVehicleNavClass& cls,
                                       float max_end_dist, const std::vector<BotVehicleNavLink>& excluded,
                                       BotVehicleRoute& out);
// A random point the class can drive to, between min_dist and max_dist from pos.
bool bot_vehicle_nav_random_ground_point(const rf::Vector3& pos, const BotVehicleNavClass& cls, float min_dist,
                                         float max_dist, rf::Vector3& out);
// Within radius of a ground link: a road hulls take.
bool bot_vehicle_nav_near_vehicle_road(const rf::Vector3& pos, float radius);
// Changes only when the ground links do, so an answer of bot_vehicle_nav_near_vehicle_road holds while it does.
int bot_vehicle_nav_road_version();

// A fighter's or sub's straight flight: the corridor at its radius, and for a sub the water all along the way (it
// strands out of it).
bool bot_vehicle_nav_flight_corridor_clear(const rf::Vector3& from, const rf::Vector3& to,
                                           const BotVehicleNavClass& cls);
// A water landing spot lies within this of the surface; a landing approach is sampled this far apart.
inline constexpr float kVehicleNavLandingFloatBand = 2.0f;
inline constexpr float kVehicleNavLandingApproachStep = 2.0f;
// How far from a landing spot toward `toward` its approach reaches (the whole way when it never leaves), where the hull
// is setting down or surfacing and only the centre line counts: within the spot's radius (at least the hull's), and on
// while within a hull radius of the spot's height and one more hull radius of it across (air), or out of the water but
// within the float band over it (water).
float bot_vehicle_nav_landing_approach(const rf::Vector3& landing, float landing_radius, const rf::Vector3& toward,
                                       const BotVehicleNavClass& cls);
// A fighter's or sub's route from `from` toward dest: straight when the corridor is clear, else over the nodes of its
// layer to one of those nearest dest within max_end_dist, closing on dest when the last has a clear corridor to it.
// A line ending at a landing node needs only its centre line over that node's approach. Never uses an excluded link.
// False when there is none.
bool bot_vehicle_nav_plan_flight_route(const rf::Vector3& from, const rf::Vector3& dest, const BotVehicleNavClass& cls,
                                       float max_end_dist, const std::vector<BotVehicleNavLink>& excluded,
                                       BotVehicleRoute& out);
// A random node the class can both reach and leave (an air or water landing node regardless), between min_dist and
// max_dist from pos.
bool bot_vehicle_nav_random_node(const rf::Vector3& pos, float min_dist, float max_dist,
                                 const BotVehicleNavClass& cls, rf::Vector3& out);
int bot_vehicle_nav_layer_node_count(const BotVehicleNavClass& cls);
bool bot_vehicle_nav_point_in_liquid(const rf::Vector3& pos);
// The nearest solid level or mover surface within depth straight below pos, from a trace without CF_ANY_HIT, which
// keeps the nearest hit (a mover hulls pass through is skipped, as the hull physics does). The foot generator's
// ground trace sets CF_ANY_HIT, so it does not promise the nearest.
bool bot_vehicle_nav_floor_below(const rf::Vector3& pos, float depth, rf::Vector3& out);
// The surface of the liquid pos is in; false out of any liquid.
bool bot_vehicle_nav_liquid_surface_at(const rf::Vector3& pos, float& out_y);
// Open air (a room, no liquid) just over the surface above pos, which nothing blocks from pos.
bool bot_vehicle_nav_air_above(const rf::Vector3& pos, float surface_y);
// For a sub with no landing node: a spot just under the surface by the nearest of the 8 waterline foot waypoints
// (shore or swim nodes) nearest pos, which a water corridor reaches from pos and which has air above.
bool bot_vehicle_nav_shore_spot(const rf::Vector3& pos, const BotVehicleNavClass& cls, float surface_y,
                                rf::Vector3& out);
// The nearest of the 8 dry foot waypoints nearest pos (within max_dist, looking at the 256 nearest) a rider can be let
// out at: dry ground right under it, not in liquid, and a clear flight corridor from pos to lift above it.
// Never one within avoid_radius of avoid (a spot a landing already failed at), when given.
bool bot_vehicle_nav_dry_foot_spot(const rf::Vector3& pos, float max_dist, const BotVehicleNavClass& cls, float lift,
                                   const rf::Vector3* avoid, float avoid_radius, rf::Vector3& out);
const char* bot_vehicle_nav_route_kind_name(BotVehicleRouteKind kind);

void bot_vehicle_nav_render_debug();
void bot_vehicle_nav_apply_patches();
