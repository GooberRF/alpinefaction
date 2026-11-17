#pragma once

#include <cstdint>
#include <functional>
#include <vector>
#include <common/alpine_waypoint_helper.h>
#include "../rf/math/vector.h"

// The AWP's vehicle layer: the navigation graph bots in vehicles use. Waypoint generation builds it from the level's
// Waypoint Helpers (their links still decide what connects); the in-game waypoint editor can change it, until the next
// generation replaces it.

struct VehicleWaypointLink
{
    int to = -1;
    // The largest hull the link takes (ground links); air and water links take any.
    alpine_waypoint_helper::SizeClass size = alpine_waypoint_helper::SizeClass::driller;
};

struct VehicleWaypointNode
{
    rf::Vector3 pos{};
    alpine_waypoint_helper::Type type = alpine_waypoint_helper::Type::air;
    // Ground nodes: the largest hull the mapper allowed there.
    alpine_waypoint_helper::SizeClass size = alpine_waypoint_helper::SizeClass::driller;
    float radius = alpine_waypoint_helper::default_radius;
    uint8_t flags = 0;
    // Outgoing only: a link the other way is a link of its own.
    std::vector<VehicleWaypointLink> links;
    // Landing nodes: the foot waypoint riders get out to, by position since foot indices move.
    bool has_exit = false;
    rf::Vector3 exit_pos{};

    bool landing() const
    {
        return (flags & alpine_waypoint_helper::flag_landing) != 0;
    }
};

struct VehicleWaypointLayer
{
    std::vector<VehicleWaypointNode> nodes;
};

inline constexpr int kMaxVehicleWaypointNodes = 65536;

const VehicleWaypointLayer& waypoints_vehicle_layer();
// Changes with every change to the layer: a node index from another version may be stale.
int waypoints_vehicle_layer_version();
// Mutable access for an edit; every call bumps the layer version.
VehicleWaypointLayer& waypoints_vehicle_edit_layer();
void waypoints_vehicle_set_layer(VehicleWaypointLayer layer);
void waypoints_vehicle_clear();
// The one rule for where a node may lie: inside the level's geometry box plus a margin. Generation, the editor and the
// loader all clamp to it, so a saved layer always loads.
rf::Vector3 waypoints_vehicle_clamp_to_level(const rf::Vector3& pos);
// After an AWP loads: on a level whose helpers could make a layer, an AWP without one leaves them unused.
void waypoints_vehicle_on_awp_loaded(bool has_layer);

// A level with Waypoint Helpers of a kind its vehicle factories spawn.
bool waypoints_vehicle_generation_wanted();
// Such a level with no vehicle layer: its helpers are of no use until the AWP is regenerated.
bool waypoints_vehicle_helpers_unused();

struct VehicleWaypointGenerationStats
{
    int nodes = 0;
    // Directed: a link both ways counts twice.
    int links = 0;
    int one_way_ground_links = 0;
    // Helpers of a kind no factory of the level spawns, and mapper links to anything not made a node.
    int unused_helpers = 0;
    int skipped_links = 0;
    int merged_helpers = 0;
    int dropped_helpers = 0;
    int dropped_links = 0;
    // Groups of nodes joined by links (whichever way), by alpine_waypoint_helper::Type.
    int networks[alpine_waypoint_helper::type_max + 1] = {};
    int landing_nodes = 0;
    int landing_exits = 0;
};
// Builds the layer from the helpers; the landing exits are looked for on the foot graph as it stands, so this runs
// once that is final. out_of_time(done, total) stops it.
VehicleWaypointGenerationStats waypoints_vehicle_generate(const std::function<bool(int, int)>& out_of_time);

// Editing.
int waypoints_vehicle_add_node(const rf::Vector3& pos, alpine_waypoint_helper::Type type);
bool waypoints_vehicle_remove_node(int node);
const VehicleWaypointLink* waypoints_vehicle_find_link(int from, int to);
// Adds, re-sizes or (present false) removes the link from -> to.
bool waypoints_vehicle_set_link(int from, int to, bool present, alpine_waypoint_helper::SizeClass size);
// Drops the node's links to and from nodes of another type.
int waypoints_vehicle_drop_cross_type_links(int node);
// A landing node's exit: the nearest foot waypoint it can let riders out to now. False when there is none.
bool waypoints_vehicle_update_exit(int node);
const char* waypoints_vehicle_type_name(alpine_waypoint_helper::Type type);
const char* waypoints_vehicle_size_name(alpine_waypoint_helper::SizeClass size);

// Nodes, directed links (coloured by layer, ground by size), landing exits and a line from each factory to the node its
// hull would reach first, near the camera; the highlighted node drawn brighter.
void waypoints_vehicle_render_debug(int highlighted_node);
