#include "bot_vehicle_nav.h"
#include "bot_math.h"
#include "../vehicles/vehicle.h"
#include "../vehicles/vehicle_physics.h"
#include "../../graphics/weather.h"
#include "../../main/main.h"
#include "../../misc/alpine_settings.h"
#include "../../misc/level.h"
#include "../../misc/waypoints.h"
#include "../../misc/waypoints_vehicle.h"
#include "../../os/console.h"
#include "../../rf/collide.h"
#include "../../rf/geometry.h"
#include "../../rf/level.h"
#include "../../rf/physics.h"
#include "../../rf/os/console.h"
#include <xlog/xlog.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace awh = alpine_waypoint_helper;

namespace
{

constexpr int kNavTraceFlags = rf::CF_ANY_HIT | rf::CF_PROCESS_INVISIBLE_FACES;
// A baked landing exit is the foot waypoint this near its stored position (the AWP rounds foot positions).
constexpr float kExitMatchRadius = 0.5f;
// A ground corridor runs this far above the ground under its ends, and samples the ground about this often.
constexpr float kGroundRayHeight = 1.0f;
constexpr float kGroundProbeStep = 5.0f;
constexpr int kGroundProbeMax = 16;
// Ground more than this below the corridor line is a drop.
constexpr float kGroundProbeDepth = 3.0f;
constexpr float kGroundFindDepth = 4.0f;
// A one-way drive check samples the ground this often (a curb or ledge between samples goes unseen), and allows no
// single drop deeper than this between two samples.
constexpr float kDriveProbeStep = 2.0f;
constexpr int kDriveProbeMax = 64;
constexpr float kCarMaxSampleDrop = 2.0f;
// The generator's clearance rays run this high over the floor under them: about a hull's middle, above the bumps of
// uneven ground.
constexpr float kHullClearanceHeight = 1.25f;
// Liquid this far above the ground drowns a car (vphys_hull_submerged tests the hull origin).
constexpr float kCarWadeDepth = 0.8f;
constexpr float kDirectRouteMaxDist = 60.0f;
// A flyer or sub flies straight this far when its corridor is clear.
constexpr float kFlightDirectMaxDist = 300.0f;
// Where a sub with no landing node looks for a waterline foot node.
constexpr float kShoreSearchDist = 150.0f;
constexpr float kShoreBand = 3.0f;
constexpr float kShoreBackStep = 2.0f;
constexpr int kShoreBackSteps = 8;
// A dry landing spot search looks at most this many of the nearest foot nodes.
constexpr std::size_t kDrySpotMaxLookups = 256;
// A terrain route starts at a terrain node this near the hull.
constexpr float kTerrainStartReach = 24.0f;
// How many of the layer nodes nearest the destination a route may end at, nearest first.
constexpr std::size_t kRouteGoalCandidates = 4;
// Terrain links are checked for a hull's width lazily: at most this many unchecked links per plan, in up to this many
// A* rounds, each searching at most this many nodes.
constexpr int kTerrainLinkChecksPerPlan = 16;
constexpr int kTerrainPlanRounds = 4;
constexpr int kMaxSearchExpansions = 40000;
constexpr float kTerrainCornerSlack = 1.5f;
constexpr int kPatrolSamples = 48;
constexpr float kRoadCellSize = 16.0f;
// A road segment covering more cells than this on x or z is checked on every query instead of filling the grid.
constexpr int kRoadMaxCellSpan = 4;
// Air, water, then ground by size class.
constexpr int kNavClassCount = 2 + awh::size_class_max + 1;

struct NavEdge
{
    int to = -1;
    awh::SizeClass size = awh::SizeClass::driller;
};

struct NavNode
{
    rf::Vector3 pos{};
    float radius = 0.0f;
    awh::Type type = awh::Type::air;
    awh::SizeClass size = awh::SizeClass::driller;
    bool landing = false;
    // Outgoing.
    std::vector<NavEdge> edges;
    // The largest size class a link into / out of it takes (-1: none): no route of a bigger hull ends / starts there.
    int in_size = -1;
    int out_size = -1;
    // By class_index: nodes the class's links join, whichever way, share an id, so a search between two ids is futile.
    std::array<int, kNavClassCount> component{};
    bool has_exit = false;
    rf::Vector3 exit_pos{};
    int exit_waypoint = 0;
};

struct NavGraph
{
    bool built = false;
    int layer_version = -1;
    int version = 0;
    std::vector<NavNode> nodes;
    int num_edges = 0;
    // The foot graph the landing exits were found on.
    int exits_persistent_version = -1;
};

NavGraph g_graph;
int g_last_version = 0;
// The level checksum, node and link count the graph summary line was last logged for.
std::array<uint32_t, 3> g_last_summary{};
bool g_debug_draw = false;

// The ground links (one segment per joined pair) foot routes keep off. Its version changes only when they do, so an
// edit that moves no ground node keeps the foot route caches.
struct RoadIndex
{
    int version = 0;
    std::vector<std::pair<rf::Vector3, rf::Vector3>> segments;
    // Segment indices by kRoadCellSize cell over x and z, built by the first query after the segments change, and
    // the segments too long for it.
    std::unordered_map<uint64_t, std::vector<int>> cells;
    std::vector<int> long_segments;
    bool cells_built = false;
};
RoadIndex g_roads;

// A* scratch, reused between searches; a node's entries count only when its stamp is the current search's.
struct SearchBuffers
{
    std::vector<float> cost;
    std::vector<int> came_from;
    std::vector<int> stamp;
    std::vector<bool> closed;
    std::vector<std::pair<float, int>> open;
    int current = 0;
};
SearchBuffers g_search;

struct TerrainNav
{
    int waypoint_version = -1;
    std::vector<int> nodes;
    std::vector<bool> usable;
    // Hull-width link checks by size class and node pair, kept while the persistent foot graph version holds. A crater
    // adds only temporary nodes (no persistent version bump) but can cut a link, so the checks also go with the level's
    // geomod count.
    std::unordered_map<uint64_t, bool> link_clear;
    int geomods = -1;
};
TerrainNav g_terrain;
SearchBuffers g_terrain_search;

// out_hit, when given, gets a point the segment hits (any one: the trace stops at the first it finds).
bool segment_hits_level(const rf::Vector3& a, const rf::Vector3& b, rf::Vector3* out_hit = nullptr)
{
    rf::Vector3 p0 = a;
    rf::Vector3 p1 = b;
    rf::GCollisionOutput collision{};
    if (!rf::collide_linesegment_level_solid(p0, p1, kNavTraceFlags, &collision)) {
        return false;
    }
    if (out_hit) {
        *out_hit = collision.hit_point;
    }
    return true;
}

bool point_in_liquid_at(const rf::Vector3& pos)
{
    rf::GRoom* room = rf::level.geometry ? rf::find_room(rf::level.geometry, &pos) : nullptr;
    return room && rf::point_in_liquid(room, &pos);
}

bool ground_below(const rf::Vector3& pos, rf::Vector3& out)
{
    return trace_ground_below_point(pos + rf::Vector3{0.0f, 0.5f, 0.0f}, kGroundFindDepth, &out);
}

// Clear when a ray along the path and two pairs offset by radius across it all miss the level. A ground corridor
// drops the vertical pair, which the road itself would block.
bool corridor_clear(const rf::Vector3& from, const rf::Vector3& to, float radius, bool ground,
                    rf::Vector3* out_hit = nullptr)
{
    rf::Vector3 dir = to - from;
    const float len = dir.len();
    if (!(len > 1e-3f)) {
        return true;
    }
    dir /= len;
    rf::Vector3 side = dir.cross(rf::Vector3{0.0f, 1.0f, 0.0f});
    if (side.len_sq() < 1e-6f) {
        side = rf::Vector3{1.0f, 0.0f, 0.0f};
    }
    side.normalize();
    rf::Vector3 vertical = side.cross(dir);
    vertical.normalize_safe();

    if (segment_hits_level(from, to, out_hit)) {
        return false;
    }
    for (const float sign : {1.0f, -1.0f}) {
        const rf::Vector3 offset = side * (radius * sign);
        if (segment_hits_level(from + offset, to + offset, out_hit)) {
            return false;
        }
        if (!ground) {
            const rf::Vector3 lift = vertical * (radius * sign);
            if (segment_hits_level(from + lift, to + lift, out_hit)) {
                return false;
            }
        }
    }
    return true;
}

// The hull-wide rays a little above the ground points a and b.
bool ground_hull_rays_clear(const rf::Vector3& a, const rf::Vector3& b, float half_width)
{
    const rf::Vector3 lift{0.0f, kGroundRayHeight, 0.0f};
    return corridor_clear(a + lift, b + lift, half_width, true);
}

// The ground from a to b, sampled about probe_step apart: found all the way, rising no steeper than a car climbs,
// dropping no steeper than max_drop_grade nor more than max_sample_drop between samples, and no liquid deeper than a
// car wades.
bool ground_profile_clear(const rf::Vector3& a, const rf::Vector3& b, float max_drop_grade, float probe_step,
                          int probe_max, float max_sample_drop)
{
    const rf::Vector3 lift{0.0f, kGroundRayHeight, 0.0f};
    const rf::Vector3 run = flat_vector(b - a);
    const int samples = std::clamp(static_cast<int>(std::ceil(run.len() / probe_step)), 1, probe_max);
    const float step = run.len() / static_cast<float>(samples);
    const rf::Vector3 wade{0.0f, kCarWadeDepth, 0.0f};
    rf::Vector3 prev = a;
    for (int i = 1; i <= samples; ++i) {
        rf::Vector3 ground = b;
        if (i < samples) {
            const rf::Vector3 line = a + (b - a) * (static_cast<float>(i) / static_cast<float>(samples)) + lift;
            if (!trace_ground_below_point(line, kGroundRayHeight + kGroundProbeDepth, &ground)) {
                return false;
            }
        }
        const float rise = ground.y - prev.y;
        if (rise > kVehicleNavCarMaxGrade * step + kVehicleNavCarStepClimb
            || -rise > std::min(max_drop_grade * step + kVehicleNavCarStepClimb, max_sample_drop)
            || point_in_liquid_at(ground + wade)) {
            return false;
        }
        prev = ground;
    }
    return true;
}

// A* over n nodes: pos_of(node) for the heuristic, for_each_edge(node, visit) calls visit(next, cost) for every edge
// the search may take. Gives up after max_expansions nodes.
template<typename PosOf, typename ForEachEdge>
bool astar(SearchBuffers& s, int n, int from, int to, PosOf pos_of, ForEachEdge for_each_edge, int max_expansions,
           std::vector<int>& out_path)
{
    out_path.clear();
    if (static_cast<int>(s.stamp.size()) != n) {
        s.cost.assign(n, 0.0f);
        s.came_from.assign(n, -1);
        s.stamp.assign(n, 0);
        s.closed.assign(n, false);
        s.current = 0;
    }
    if (++s.current == std::numeric_limits<int>::max()) {
        std::fill(s.stamp.begin(), s.stamp.end(), 0);
        s.current = 1;
    }
    auto touch = [&](int node) {
        if (s.stamp[node] != s.current) {
            s.stamp[node] = s.current;
            s.cost[node] = std::numeric_limits<float>::infinity();
            s.came_from[node] = -1;
            s.closed[node] = false;
        }
    };
    const rf::Vector3 goal = pos_of(to);
    auto heuristic = [&](int node) { return (pos_of(node) - goal).len(); };
    auto heap_less = [](const std::pair<float, int>& a, const std::pair<float, int>& b) { return a.first > b.first; };

    s.open.clear();
    touch(from);
    s.cost[from] = 0.0f;
    s.open.emplace_back(heuristic(from), from);
    int expansions = 0;
    while (!s.open.empty()) {
        std::pop_heap(s.open.begin(), s.open.end(), heap_less);
        const int node = s.open.back().second;
        s.open.pop_back();
        if (s.closed[node]) {
            continue;
        }
        s.closed[node] = true;
        if (node == to) {
            for (int at = to; at >= 0; at = s.came_from[at]) {
                out_path.push_back(at);
            }
            std::reverse(out_path.begin(), out_path.end());
            return true;
        }
        if (++expansions > max_expansions) {
            return false;
        }
        for_each_edge(node, [&](int next, float edge_cost) {
            touch(next);
            if (s.closed[next]) {
                return;
            }
            const float cost = s.cost[node] + edge_cost;
            if (cost < s.cost[next]) {
                s.cost[next] = cost;
                s.came_from[next] = node;
                s.open.emplace_back(cost + heuristic(next), next);
                std::push_heap(s.open.begin(), s.open.end(), heap_less);
            }
        });
    }
    return false;
}

bool link_excluded(const std::vector<BotVehicleNavLink>& excluded, BotVehicleRouteKind kind, int a, int b)
{
    return std::any_of(excluded.begin(), excluded.end(), [&](const BotVehicleNavLink& link) {
        return link.kind == kind && ((link.from == a && link.to == b) || (link.from == b && link.to == a));
    });
}

int class_index(const BotVehicleNavClass& cls)
{
    switch (cls.layer) {
        case awh::Type::air:
            return 0;
        case awh::Type::water:
            return 1;
        case awh::Type::ground:
            break;
    }
    return 2 + static_cast<int>(cls.size);
}

int required_size(const BotVehicleNavClass& cls)
{
    return cls.layer == awh::Type::ground ? static_cast<int>(cls.size) : 0;
}

// A node a route of the class may pass. An air or water node with no link still serves as a landing spot, which a
// straight flight reaches.
bool usable(const NavNode& node, const BotVehicleNavClass& cls)
{
    return node.type == cls.layer
        && (std::max(node.in_size, node.out_size) >= required_size(cls)
            || (cls.layer != awh::Type::ground && node.landing));
}

// A node a hull of the class can drive (fly) to and on from.
bool usable_through(const NavNode& node, const BotVehicleNavClass& cls)
{
    return node.type == cls.layer
        && (std::min(node.in_size, node.out_size) >= required_size(cls)
            || (cls.layer != awh::Type::ground && node.landing));
}

bool usable_start(const NavNode& node, const BotVehicleNavClass& cls)
{
    return node.type == cls.layer && node.out_size >= required_size(cls);
}

bool usable_goal(const NavNode& node, const BotVehicleNavClass& cls)
{
    return node.type == cls.layer && node.in_size >= required_size(cls);
}

bool edge_usable(const NavEdge& edge, const BotVehicleNavClass& cls)
{
    return static_cast<int>(edge.size) >= required_size(cls);
}

bool has_edge(int from, int to)
{
    const auto& edges = g_graph.nodes[from].edges;
    return std::any_of(edges.begin(), edges.end(), [to](const NavEdge& edge) { return edge.to == to; });
}

// Only the nearest kVehicleNavTraceCandidates are ever traced, so only they are put in order.
void sort_nearest(std::vector<std::pair<float, int>>& candidates)
{
    const std::size_t n = std::min(candidates.size(), kVehicleNavTraceCandidates);
    std::partial_sort(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(n), candidates.end());
}

int find_root(std::vector<int>& parent, int i)
{
    while (parent[i] != i) {
        parent[i] = parent[parent[i]];
        i = parent[i];
    }
    return i;
}

void compute_components()
{
    const int n = static_cast<int>(g_graph.nodes.size());
    std::vector<int> parent(n);
    for (int c = 0; c < kNavClassCount; ++c) {
        BotVehicleNavClass cls;
        cls.layer = c == 0 ? awh::Type::air : c == 1 ? awh::Type::water : awh::Type::ground;
        cls.size = static_cast<awh::SizeClass>(std::max(c - 2, 0));
        for (int i = 0; i < n; ++i) {
            parent[i] = i;
        }
        for (int i = 0; i < n; ++i) {
            if (g_graph.nodes[i].type != cls.layer) {
                continue;
            }
            for (const NavEdge& edge : g_graph.nodes[i].edges) {
                if (edge_usable(edge, cls)) {
                    parent[find_root(parent, i)] = find_root(parent, edge.to);
                }
            }
        }
        for (int i = 0; i < n; ++i) {
            g_graph.nodes[i].component[c] = find_root(parent, i);
        }
    }
}

void update_roads()
{
    std::vector<std::pair<rf::Vector3, rf::Vector3>> segments;
    for (int i = 0; i < static_cast<int>(g_graph.nodes.size()); ++i) {
        const NavNode& node = g_graph.nodes[i];
        if (node.type != awh::Type::ground) {
            continue;
        }
        for (const NavEdge& edge : node.edges) {
            if (edge.to > i || !has_edge(edge.to, i)) {
                segments.emplace_back(node.pos, g_graph.nodes[edge.to].pos);
            }
        }
    }
    const auto same = [](const std::pair<rf::Vector3, rf::Vector3>& a, const std::pair<rf::Vector3, rf::Vector3>& b) {
        return a.first == b.first && a.second == b.second;
    };
    if (!std::equal(segments.begin(), segments.end(), g_roads.segments.begin(), g_roads.segments.end(), same)) {
        g_roads.segments = std::move(segments);
        g_roads.cells.clear();
        g_roads.long_segments.clear();
        g_roads.cells_built = false;
        ++g_roads.version;
    }
}

uint64_t road_cell_key(int x, int z)
{
    return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) | static_cast<uint32_t>(z);
}

int road_cell(float coord)
{
    return static_cast<int>(std::floor(coord / kRoadCellSize));
}

void ensure_road_cells()
{
    if (g_roads.cells_built) {
        return;
    }
    g_roads.cells_built = true;
    for (int k = 0; k < static_cast<int>(g_roads.segments.size()); ++k) {
        const auto& [a, b] = g_roads.segments[k];
        const int x0 = road_cell(std::min(a.x, b.x));
        const int x1 = road_cell(std::max(a.x, b.x));
        const int z0 = road_cell(std::min(a.z, b.z));
        const int z1 = road_cell(std::max(a.z, b.z));
        if (x1 - x0 >= kRoadMaxCellSpan || z1 - z0 >= kRoadMaxCellSpan) {
            g_roads.long_segments.push_back(k);
            continue;
        }
        for (int x = x0; x <= x1; ++x) {
            for (int z = z0; z <= z1; ++z) {
                g_roads.cells[road_cell_key(x, z)].push_back(k);
            }
        }
    }
}

void build_graph()
{
    g_graph = NavGraph{};
    g_graph.layer_version = waypoints_vehicle_layer_version();
    g_graph.version = ++g_last_version;
    g_graph.built = true;
    if (!vehicle_level_has_factories()) {
        update_roads();
        return;
    }

    const VehicleWaypointLayer& layer = waypoints_vehicle_layer();
    const int n = static_cast<int>(layer.nodes.size());
    g_graph.nodes.reserve(n);
    for (const VehicleWaypointNode& source : layer.nodes) {
        NavNode node;
        node.pos = source.pos;
        node.radius = source.radius;
        node.type = source.type;
        node.size = source.type == awh::Type::ground ? source.size : awh::SizeClass::driller;
        node.landing = source.landing() && source.type != awh::Type::ground;
        node.has_exit = source.has_exit;
        node.exit_pos = source.exit_pos;
        g_graph.nodes.push_back(std::move(node));
    }
    for (int i = 0; i < n; ++i) {
        for (const VehicleWaypointLink& link : layer.nodes[i].links) {
            if (link.to < 0 || link.to >= n || link.to == i || layer.nodes[link.to].type != layer.nodes[i].type) {
                continue;
            }
            NavNode& from = g_graph.nodes[i];
            NavNode& to = g_graph.nodes[link.to];
            // A node's own size bounds every link at it (an edit may have narrowed it).
            const NavEdge edge{link.to, std::min({link.size, from.size, to.size})};
            from.edges.push_back(edge);
            from.out_size = std::max(from.out_size, static_cast<int>(edge.size));
            to.in_size = std::max(to.in_size, static_cast<int>(edge.size));
            ++g_graph.num_edges;
        }
    }
    compute_components();
    update_roads();

    int per_layer[awh::type_max + 1] = {};
    int landing = 0;
    int one_way = 0;
    for (int i = 0; i < n; ++i) {
        const NavNode& node = g_graph.nodes[i];
        ++per_layer[static_cast<int>(node.type)];
        landing += node.landing ? 1 : 0;
        for (const NavEdge& edge : node.edges) {
            one_way += has_edge(edge.to, i) ? 0 : 1;
        }
    }
    // Once per level and layer shape: an edit moving nodes rebuilds the graph as often as every frame.
    const std::array<uint32_t, 3> summary{rf::level.checksum, static_cast<uint32_t>(n),
                                          static_cast<uint32_t>(g_graph.num_edges)};
    if (n > 0 && summary != g_last_summary) {
        g_last_summary = summary;
        xlog::info("[BotVehNav] graph v{}: {} nodes (air {}, water {}, ground {}, landing {}), {} links ({} one-way)",
                   g_graph.version, n, per_layer[0], per_layer[1], per_layer[2], landing, g_graph.num_edges, one_way);
    }
}

// Rebuilt whenever the vehicle layer changed (a load, a generation or an edit) since the last build.
bool ensure_graph()
{
    if (!(rf::level.flags & rf::LEVEL_LOADED)) {
        return false;
    }
    if (!g_graph.built || g_graph.layer_version != waypoints_vehicle_layer_version()) {
        build_graph();
    }
    return true;
}

// The nearest persistent foot waypoint within radius of pos, or 0; a temporary one (a crater node) never counts.
int nearest_persistent_waypoint(const rf::Vector3& pos, float radius)
{
    int best = 0;
    float best_dist_sq = radius * radius;
    const int count = waypoints_count();
    for (int i = 1; i < count; ++i) {
        rf::Vector3 wp;
        bool temporary = false;
        if (!waypoints_get_pos(i, wp) || !waypoints_get_temporary(i, temporary) || temporary) {
            continue;
        }
        const float dist_sq = (wp - pos).len_sq();
        if (dist_sq <= best_dist_sq) {
            best = i;
            best_dist_sq = dist_sq;
        }
    }
    return best;
}

// Foot indices move with every remap, so a baked exit is looked up by its position again whenever the persistent foot
// graph changes (an exit is never a temporary node).
void ensure_landing_exits()
{
    const int persistent_version = waypoints_persistent_graph_version();
    if (g_graph.exits_persistent_version == persistent_version) {
        return;
    }
    const bool foot_graph_changed = g_graph.exits_persistent_version != -1;
    g_graph.exits_persistent_version = persistent_version;
    for (int i = 0; i < static_cast<int>(g_graph.nodes.size()); ++i) {
        NavNode& node = g_graph.nodes[i];
        if (!node.landing) {
            continue;
        }
        const int exit = node.has_exit ? nearest_persistent_waypoint(node.exit_pos, kExitMatchRadius) : 0;
        if (foot_graph_changed && exit == 0 && node.exit_waypoint > 0 && g_alpine_game_config.dbg_bot) {
            xlog::info("[BotVehNav] landing node {} ({}) lost its exit waypoint", i,
                       waypoints_vehicle_type_name(node.type));
        }
        node.exit_waypoint = exit;
    }
}

bool find_layer_path(int from, int to, const BotVehicleNavClass& cls, const std::vector<BotVehicleNavLink>& excluded,
                     std::vector<int>& out_path)
{
    out_path.clear();
    if (!ensure_graph()) {
        return false;
    }
    const int n = static_cast<int>(g_graph.nodes.size());
    if (from < 0 || from >= n || to < 0 || to >= n || !usable(g_graph.nodes[from], cls)
        || !usable(g_graph.nodes[to], cls)) {
        return false;
    }
    const auto pos_of = [](int node) { return g_graph.nodes[node].pos; };
    const auto for_each_edge = [&](int node, auto&& visit) {
        for (const NavEdge& edge : g_graph.nodes[node].edges) {
            if (edge_usable(edge, cls) && !link_excluded(excluded, BotVehicleRouteKind::layer, node, edge.to)) {
                visit(edge.to, (g_graph.nodes[edge.to].pos - g_graph.nodes[node].pos).len());
            }
        }
    };
    return astar(g_search, n, from, to, pos_of, for_each_edge, kMaxSearchExpansions, out_path);
}

template<typename PosOf>
std::vector<std::pair<float, int>> nearest_of(const std::vector<int>& ids, PosOf pos_of, const rf::Vector3& pos,
                                              float max_dist)
{
    std::vector<std::pair<float, int>> out;
    for (const int id : ids) {
        const float dist_sq = (pos_of(id) - pos).len_sq();
        if (dist_sq <= max_dist * max_dist) {
            out.emplace_back(dist_sq, id);
        }
    }
    sort_nearest(out);
    return out;
}

rf::Vector3 waypoint_pos(int index)
{
    rf::Vector3 pos{};
    waypoints_get_pos(index, pos);
    return pos;
}

// The terrain waypoints follow the persistent foot graph: a temporary node never carries the flag, and the indices
// stay put while that version holds.
void ensure_terrain_nodes()
{
    if (g_terrain.geomods != rf::g_num_geomods_this_level) {
        g_terrain.geomods = rf::g_num_geomods_this_level;
        g_terrain.link_clear.clear();
    }
    const int version = waypoints_persistent_graph_version();
    if (g_terrain.waypoint_version == version) {
        return;
    }
    g_terrain.waypoint_version = version;
    g_terrain.nodes.clear();
    g_terrain.link_clear.clear();
    const int count = waypoints_count();
    g_terrain.usable.assign(std::max(count, 0), false);
    const rf::Vector3 wade{0.0f, kCarWadeDepth - kWaypointGenerateGroundOffset, 0.0f};
    for (int i = 1; i < count; ++i) {
        rf::Vector3 pos;
        bool temporary = false;
        if (!waypoints_is_terrain_node(i) || !waypoints_get_pos(i, pos) || !waypoints_get_temporary(i, temporary)
            || temporary || point_in_liquid_at(pos + wade)) {
            continue;
        }
        g_terrain.usable[i] = true;
        g_terrain.nodes.push_back(i);
    }
}

uint64_t terrain_link_key(int a, int b, const BotVehicleNavClass& cls)
{
    const auto lo = static_cast<uint64_t>(std::min(a, b));
    const auto hi = static_cast<uint64_t>(std::max(a, b));
    return (static_cast<uint64_t>(cls.size) << 62) | (lo << 31) | hi;
}

bool terrain_link_clear(int a, int b, const BotVehicleNavClass& cls)
{
    const uint64_t key = terrain_link_key(a, b, cls);
    const auto it = g_terrain.link_clear.find(key);
    if (it != g_terrain.link_clear.end()) {
        return it->second;
    }
    const bool clear = bot_vehicle_nav_ground_corridor_clear(waypoint_pos(a), waypoint_pos(b), cls.corridor_radius);
    g_terrain.link_clear.emplace(key, clear);
    return clear;
}

void fill_route(BotVehicleRoute& out, BotVehicleRouteKind kind, const std::vector<int>& path,
                const std::vector<rf::Vector3>& positions, const std::vector<float>& slack)
{
    out.kind = kind;
    out.nav_version = g_graph.version;
    out.points = positions;
    out.nodes = path;
    out.slack = slack;
}

template<typename Clear>
void append_destination(BotVehicleRoute& out, const rf::Vector3& dest, float max_dist, Clear clear)
{
    if (!out.points.empty() && (out.points.back() - dest).len() <= max_dist && clear(out.points.back(), dest)) {
        out.points.push_back(dest);
        out.nodes.push_back(-1);
        out.slack.push_back(0.0f);
    }
}

// From one of the nodes nearest `from` within start_reach that clear(from, node) reaches, to the nearest of the nodes
// nearest dest within max_end_dist that a search reaches; closed on dest when clear within direct_dist.
template<typename Clear>
bool plan_over_layer(const rf::Vector3& from, const rf::Vector3& dest, const BotVehicleNavClass& cls,
                     float max_end_dist, const std::vector<BotVehicleNavLink>& excluded, float start_reach,
                     float direct_dist, Clear clear, BotVehicleRoute& out)
{
    std::vector<int> start_ids;
    std::vector<int> goal_ids;
    for (int i = 0; i < static_cast<int>(g_graph.nodes.size()); ++i) {
        if (usable_start(g_graph.nodes[i], cls)) {
            start_ids.push_back(i);
        }
        if (usable_goal(g_graph.nodes[i], cls)) {
            goal_ids.push_back(i);
        }
    }
    const auto pos_of = [](int node) { return g_graph.nodes[node].pos; };
    const auto goals = nearest_of(goal_ids, pos_of, dest, max_end_dist);
    if (goals.empty()) {
        return false;
    }
    const auto starts = nearest_of(start_ids, pos_of, from, start_reach);
    const int c = class_index(cls);
    std::vector<int> path;
    for (std::size_t s = 0; s < starts.size() && s < kVehicleNavStartCandidates; ++s) {
        const int start = starts[s].second;
        if (!clear(from, g_graph.nodes[start].pos)) {
            continue;
        }
        for (std::size_t g = 0; g < goals.size() && g < kRouteGoalCandidates; ++g) {
            const int goal = goals[g].second;
            if (g_graph.nodes[goal].component[c] != g_graph.nodes[start].component[c]
                || !find_layer_path(start, goal, cls, excluded, path)) {
                continue;
            }
            std::vector<rf::Vector3> positions;
            std::vector<float> slack;
            for (const int node : path) {
                positions.push_back(g_graph.nodes[node].pos);
                slack.push_back(std::max(g_graph.nodes[node].radius - cls.corridor_radius, 0.0f));
            }
            fill_route(out, BotVehicleRouteKind::layer, path, positions, slack);
            append_destination(out, dest, direct_dist, clear);
            return true;
        }
    }
    return false;
}

// Hull width is checked only on the links a path uses; the cache keeps the answers while the foot graph and geomod
// count hold.
bool plan_over_terrain(const rf::Vector3& from, const rf::Vector3& dest, const BotVehicleNavClass& cls,
                       float max_end_dist, const std::vector<BotVehicleNavLink>& excluded, BotVehicleRoute& out)
{
    ensure_terrain_nodes();
    if (g_terrain.nodes.empty()) {
        return false;
    }
    const auto goals = nearest_of(g_terrain.nodes, waypoint_pos, dest, max_end_dist);
    if (goals.empty()) {
        return false;
    }
    const int goal = goals.front().second;
    const auto starts = nearest_of(g_terrain.nodes, waypoint_pos, from, kTerrainStartReach);
    int start = -1;
    for (std::size_t c = 0; c < starts.size() && c < kVehicleNavStartCandidates; ++c) {
        if (bot_vehicle_nav_ground_corridor_clear(from, waypoint_pos(starts[c].second), cls.corridor_radius)) {
            start = starts[c].second;
            break;
        }
    }
    if (start < 0) {
        return false;
    }
    const auto for_each_edge = [&](int node, auto&& visit) {
        std::array<int, kMaxWaypointLinks> links{};
        const int num_links = waypoints_get_persistent_links(node, links);
        for (int i = 0; i < num_links; ++i) {
            const int next = links[i];
            if (next <= 0 || next >= static_cast<int>(g_terrain.usable.size()) || !g_terrain.usable[next]
                || link_excluded(excluded, BotVehicleRouteKind::terrain, node, next)) {
                continue;
            }
            const auto known = g_terrain.link_clear.find(terrain_link_key(node, next, cls));
            if (known == g_terrain.link_clear.end() || known->second) {
                visit(next, (waypoint_pos(next) - waypoint_pos(node)).len());
            }
        }
    };
    std::vector<int> path;
    int checks = 0;
    for (int round = 0; round < kTerrainPlanRounds; ++round) {
        if (!astar(g_terrain_search, waypoints_count(), start, goal, waypoint_pos, for_each_edge,
                   kMaxSearchExpansions, path)) {
            return false;
        }
        bool failed = false;
        for (std::size_t k = 0; k + 1 < path.size() && checks < kTerrainLinkChecksPerPlan; ++k) {
            if (!g_terrain.link_clear.contains(terrain_link_key(path[k], path[k + 1], cls))) {
                ++checks;
            }
            if (!terrain_link_clear(path[k], path[k + 1], cls)) {
                failed = true;
                break;
            }
        }
        if (failed) {
            continue;
        }
        std::vector<rf::Vector3> positions;
        for (const int node : path) {
            positions.push_back(waypoint_pos(node));
        }
        fill_route(out, BotVehicleRouteKind::terrain, path, positions,
                   std::vector<float>(path.size(), kTerrainCornerSlack));
        append_destination(out, dest, kDirectRouteMaxDist, [&](const rf::Vector3& a, const rf::Vector3& b) {
            return bot_vehicle_nav_ground_corridor_clear(a, b, cls.corridor_radius);
        });
        return true;
    }
    return false;
}

void print_summary(int first)
{
    if (!ensure_graph()) {
        rf::console::print("No level is loaded.");
        return;
    }
    if (!vehicle_level_has_factories()) {
        rf::console::print("[BotVehNav] no vehicle factories: graph not built");
        return;
    }
    if (waypoints_vehicle_helpers_unused()) {
        rf::console::print("[BotVehNav] the level has Waypoint Helpers but the AWP has no vehicle layer: "
                           "regenerate it");
    }
    ensure_landing_exits();
    rf::console::print("[BotVehNav] graph v{}: {} nodes, {} links", g_graph.version, g_graph.nodes.size(),
                       g_graph.num_edges);
    constexpr int kMaxListedNodes = 200;
    const int count = static_cast<int>(g_graph.nodes.size());
    first = std::clamp(first, 0, count);
    const int last = std::min(first + kMaxListedNodes, count);
    for (int i = first; i < last; ++i) {
        const NavNode& node = g_graph.nodes[i];
        std::string edges;
        for (const NavEdge& edge : node.edges) {
            edges += std::to_string(edge.to);
            if (node.type == awh::Type::ground) {
                edges += std::format("({})", waypoints_vehicle_size_name(edge.size));
            }
            edges += has_edge(edge.to, i) ? " " : "> ";
        }
        rf::console::print("  node {} {} r={:.1f}{}{} links: {}", i, waypoints_vehicle_type_name(node.type),
                           node.radius,
                           node.type == awh::Type::ground
                               ? std::format(" size {}", waypoints_vehicle_size_name(node.size)) : std::string{},
                           node.landing ? std::format(" landing exit wp {}", node.exit_waypoint) : std::string{},
                           edges.empty() ? "none" : edges);
    }
    if (last < count || first > 0) {
        rf::console::print("  nodes {}-{} of {}; dbg_vehnav list <first_node> lists others", first, last - 1, count);
    }
}

void print_path(int from, int to, int size)
{
    if (!ensure_graph()) {
        rf::console::print("No level is loaded.");
        return;
    }
    if (!vehicle_level_has_factories()) {
        rf::console::print("[BotVehNav] no vehicle factories: graph not built");
        return;
    }
    const int count = static_cast<int>(g_graph.nodes.size());
    if (from < 0 || from >= count || to < 0 || to >= count) {
        rf::console::print("Unknown node.");
        return;
    }
    BotVehicleNavClass cls;
    cls.layer = g_graph.nodes[from].type;
    cls.size = static_cast<awh::SizeClass>(std::clamp(size, 0, static_cast<int>(awh::size_class_max)));
    std::vector<int> path;
    if (!bot_vehicle_nav_find_path(from, to, cls, path)) {
        rf::console::print("[BotVehNav] no {} path from {} to {}", waypoints_vehicle_type_name(cls.layer), from, to);
        return;
    }
    std::string nodes;
    for (const int n : path) {
        nodes += std::to_string(n) + " ";
    }
    rf::console::print("[BotVehNav] path: {}", nodes);
}

ConsoleCommand2 dbg_vehnav_cmd{
    "dbg_vehnav",
    [](std::optional<std::string> action, std::optional<int> a, std::optional<int> b, std::optional<int> size) {
        if (action && *action == "path") {
            if (!a || !b) {
                rf::console::print("Usage: dbg_vehnav path <from_node> <to_node> [size 0 jeep | 1 APC | 2 driller]");
                return;
            }
            print_path(*a, *b, size.value_or(0));
            return;
        }
        if (action && *action == "list") {
            print_summary(a.value_or(0));
            return;
        }
        if (action && (*action == "0" || *action == "1")) {
            g_debug_draw = *action == "1";
        }
        else if (!action) {
            g_debug_draw = !g_debug_draw;
        }
        rf::console::print("Vehicle navigation graph drawing is {}.", g_debug_draw ? "on" : "off");
        print_summary(0);
    },
    "Draw the vehicle layer of the AWP that vehicle bots navigate on and list it",
    "dbg_vehnav [0|1] | list [first_node] | path <from_node> <to_node> [size]",
};

} // namespace

bool bot_vehicle_nav_class_for_type(int entity_type, BotVehicleNavClass& out)
{
    out = {};
    switch (vehicle_damage_class_from_type(entity_type)) {
        case VDC_FIGHTER:
            out.layer = awh::Type::air;
            break;
        case VDC_SUB:
            out.layer = awh::Type::water;
            break;
        case VDC_JEEP:
            out.layer = awh::Type::ground;
            out.size = awh::SizeClass::jeep;
            break;
        case VDC_APC:
            out.layer = awh::Type::ground;
            out.size = awh::SizeClass::apc;
            break;
        case VDC_DRILLER:
            out.layer = awh::Type::ground;
            out.size = awh::SizeClass::driller;
            break;
        default:
            return false;
    }
    out.corridor_radius = vehicle_physics_type_corridor_radius(entity_type);
    return true;
}

void bot_vehicle_nav_level_init()
{
    g_graph.built = false;
    if (ensure_graph()) {
        ensure_landing_exits();
    }
}

int bot_vehicle_nav_version()
{
    ensure_graph();
    return g_graph.version;
}

bool bot_vehicle_nav_node(int index, BotVehicleNavNode& out)
{
    if (!ensure_graph() || index < 0 || index >= static_cast<int>(g_graph.nodes.size())) {
        return false;
    }
    const NavNode& node = g_graph.nodes[index];
    out.pos = node.pos;
    out.radius = node.radius;
    out.landing = node.landing;
    return true;
}

bool bot_vehicle_nav_find_path(int from, int to, const BotVehicleNavClass& cls, std::vector<int>& out_path)
{
    return find_layer_path(from, to, cls, {}, out_path);
}

bool bot_vehicle_nav_find_path_near(const rf::Vector3& pos, int goal, const BotVehicleNavClass& cls,
                                    std::vector<int>& out_path)
{
    out_path.clear();
    if (!ensure_graph() || goal < 0 || goal >= static_cast<int>(g_graph.nodes.size())
        || !usable(g_graph.nodes[goal], cls)) {
        return false;
    }
    std::vector<int> ids;
    for (int i = 0; i < static_cast<int>(g_graph.nodes.size()); ++i) {
        if (i == goal || usable_start(g_graph.nodes[i], cls)) {
            ids.push_back(i);
        }
    }
    const auto starts =
        nearest_of(ids, [](int node) { return g_graph.nodes[node].pos; }, pos, std::numeric_limits<float>::max());
    const int c = class_index(cls);
    for (std::size_t k = 0; k < starts.size() && k < kVehicleNavStartCandidates; ++k) {
        const int start = starts[k].second;
        if (start == goal) {
            out_path.push_back(goal);
            return true;
        }
        if (g_graph.nodes[start].component[c] == g_graph.nodes[goal].component[c]
            && find_layer_path(start, goal, cls, {}, out_path)) {
            return true;
        }
    }
    return false;
}

bool bot_vehicle_nav_ground_corridor_clear(const rf::Vector3& from, const rf::Vector3& to, float half_width)
{
    rf::Vector3 a;
    rf::Vector3 b;
    return ground_below(from, a) && ground_below(to, b) && ground_hull_rays_clear(a, b, half_width)
        && ground_profile_clear(a, b, kVehicleNavCarMaxGrade, kGroundProbeStep, kGroundProbeMax,
                                std::numeric_limits<float>::infinity());
}

bool bot_vehicle_nav_ground_hull_clear(const rf::Vector3& from, const rf::Vector3& to, float half_width,
                                      rf::Vector3* out_hit)
{
    rf::Vector3 a;
    rf::Vector3 b;
    if (!ground_below(from, a) || !ground_below(to, b)) {
        if (out_hit) {
            *out_hit = from;
        }
        return false;
    }
    const rf::Vector3 lift{0.0f, kGroundRayHeight, 0.0f};
    const rf::Vector3 up{0.0f, kHullClearanceHeight, 0.0f};
    rf::Vector3 side = flat_vector(b - a).cross(rf::Vector3{0.0f, 1.0f, 0.0f});
    const bool has_side = side.len_sq() > 1e-6f;
    if (has_side) {
        side.normalize();
    }
    // The hull's middle at each edge, over the floor under that edge, so a road across a slope a hull can sit on tilts
    // the rays with it. The floor halfway out must lie about halfway between, so a step (the top of a barrier or post
    // the edge stands on) is no slope. Anything else (a steeper rise or drop, a step, a rise above the search start,
    // which starts the search inside it, or no floor found) keeps the centre's height.
    const auto edge_point = [&](const rf::Vector3& floor, float sign) {
        constexpr float kSlopeTolerance = 0.3f;
        const rf::Vector3 level = floor + side * (half_width * sign);
        const rf::Vector3 mid_level = floor + side * (0.5f * half_width * sign);
        const float search = 4.0f * kHullClearanceHeight;
        rf::Vector3 edge_floor;
        rf::Vector3 mid_floor;
        if (!trace_ground_below_point(level + up * 2.0f, search, &edge_floor)
            || !trace_ground_below_point(mid_level + up * 2.0f, search, &mid_floor)
            || edge_floor.y - floor.y > half_width * kVehicleNavCarMaxGrade
            || floor.y - edge_floor.y > half_width * kVehicleNavCarMaxDropGrade
            || std::fabs((mid_floor.y - floor.y) - 0.5f * (edge_floor.y - floor.y)) > kSlopeTolerance) {
            edge_floor = level;
        }
        return edge_floor + up;
    };
    const float run = flat_vector(b - a).len();
    const int samples = std::clamp(static_cast<int>(std::ceil(run / kDriveProbeStep)), 1, kDriveProbeMax);
    rf::Vector3 prev = a + up;
    rf::Vector3 prev_left{};
    rf::Vector3 prev_right{};
    for (int i = 0; i <= samples; ++i) {
        rf::Vector3 floor = a;
        if (i > 0) {
            const rf::Vector3 line = a + (b - a) * (static_cast<float>(i) / static_cast<float>(samples));
            floor = b;
            // Missing ground is the drive check's to judge; here the straight line stands in for it.
            if (i < samples && !trace_ground_below_point(line + lift, kGroundRayHeight + kGroundProbeDepth, &floor)) {
                floor = line;
            }
        }
        const rf::Vector3 center = floor + up;
        if (i > 0 && segment_hits_level(prev, center, out_hit)) {
            return false;
        }
        if (has_side) {
            const rf::Vector3 left = edge_point(floor, 1.0f);
            const rf::Vector3 right = edge_point(floor, -1.0f);
            if (segment_hits_level(center, left, out_hit) || segment_hits_level(center, right, out_hit)) {
                return false;
            }
            if (i > 0
                && (segment_hits_level(prev_left, left, out_hit) || segment_hits_level(prev_right, right, out_hit))) {
                return false;
            }
            prev_left = left;
            prev_right = right;
        }
        prev = center;
    }
    return true;
}

bool bot_vehicle_nav_corridor_hit(const rf::Vector3& from, const rf::Vector3& to, float radius, rf::Vector3& out_hit)
{
    return !corridor_clear(from, to, radius, false, &out_hit);
}

bool bot_vehicle_nav_ground_drive_clear(const rf::Vector3& from, const rf::Vector3& to)
{
    rf::Vector3 a;
    rf::Vector3 b;
    if (!ground_below(from, a) || !ground_below(to, b)) {
        return false;
    }
    // The whole run too: per sample the step allowance adds up.
    const float run = flat_vector(b - a).len();
    const float rise = b.y - a.y;
    return rise <= kVehicleNavCarMaxGrade * run + kVehicleNavCarStepClimb
        && -rise <= kVehicleNavCarMaxDropGrade * run + kVehicleNavCarStepClimb
        && ground_profile_clear(a, b, kVehicleNavCarMaxDropGrade, kDriveProbeStep, kDriveProbeMax, kCarMaxSampleDrop);
}

bool bot_vehicle_nav_segment_clear(const rf::Vector3& from, const rf::Vector3& to)
{
    return !segment_hits_level(from, to);
}

bool bot_vehicle_nav_plan_ground_route(const rf::Vector3& from, const rf::Vector3& dest, const BotVehicleNavClass& cls,
                                       float max_end_dist, const std::vector<BotVehicleNavLink>& excluded,
                                       BotVehicleRoute& out)
{
    out = BotVehicleRoute{};
    if (cls.layer != awh::Type::ground || !ensure_graph()) {
        return false;
    }
    const auto clear = [&](const rf::Vector3& a, const rf::Vector3& b) {
        return bot_vehicle_nav_ground_corridor_clear(a, b, cls.corridor_radius);
    };
    if ((dest - from).len() <= kDirectRouteMaxDist && clear(from, dest)) {
        fill_route(out, BotVehicleRouteKind::direct, {-1}, {dest}, {0.0f});
        return true;
    }
    return plan_over_layer(from, dest, cls, max_end_dist, excluded, kVehicleNavGroundStartReach, kDirectRouteMaxDist,
                           clear, out)
        || plan_over_terrain(from, dest, cls, max_end_dist, excluded, out);
}

bool bot_vehicle_nav_flight_corridor_clear(const rf::Vector3& from, const rf::Vector3& to,
                                           const BotVehicleNavClass& cls)
{
    if (!corridor_clear(from, to, cls.corridor_radius, false)) {
        return false;
    }
    if (cls.layer != awh::Type::water) {
        return true;
    }
    const int samples =
        std::clamp(static_cast<int>(std::ceil((to - from).len() / kVehicleNavWaterProbeStep)), 1,
                   kVehicleNavWaterProbeMax);
    for (int i = 0; i <= samples; ++i) {
        if (!point_in_liquid_at(from + (to - from) * (static_cast<float>(i) / static_cast<float>(samples)))) {
            return false;
        }
    }
    return true;
}

float bot_vehicle_nav_landing_approach(const rf::Vector3& landing, float landing_radius, const rf::Vector3& toward,
                                       const BotVehicleNavClass& cls)
{
    const float len = (toward - landing).len();
    if (len < 1e-3f) {
        return 0.0f;
    }
    const bool water = cls.layer == awh::Type::water;
    const float trim = std::max(landing_radius, cls.corridor_radius);
    const rf::Vector3 float_band{0.0f, kVehicleNavLandingFloatBand, 0.0f};
    const rf::Vector3 dir_out = (toward - landing) / len;
    float zone = 0.0f;
    for (float d = kVehicleNavLandingApproachStep; d < len; d += kVehicleNavLandingApproachStep) {
        const rf::Vector3 point = landing + dir_out * d;
        const bool close = d <= trim;
        const bool settling = water
            ? !point_in_liquid_at(point) && point_in_liquid_at(point - float_band)
            : std::fabs(point.y - landing.y) <= cls.corridor_radius
                && flat_vector(point - landing).len() <= trim + cls.corridor_radius;
        if (!close && !settling) {
            return zone;
        }
        zone = d;
    }
    return len;
}

bool bot_vehicle_nav_plan_flight_route(const rf::Vector3& from, const rf::Vector3& dest, const BotVehicleNavClass& cls,
                                       float max_end_dist, const std::vector<BotVehicleNavLink>& excluded,
                                       BotVehicleRoute& out)
{
    out = BotVehicleRoute{};
    if (cls.layer == awh::Type::ground || !ensure_graph()) {
        return false;
    }
    // As generation checks a landing link: the full corridor would hit the floor the hull sets down on. A sub's wet
    // test starts a sample past the approach, which has left the water.
    const auto clear = [&](const rf::Vector3& a, const rf::Vector3& b) {
        const auto landing = std::find_if(g_graph.nodes.begin(), g_graph.nodes.end(), [&](const NavNode& node) {
            return node.landing && node.type == cls.layer && (node.pos - b).len_sq() < 1e-4f;
        });
        if (landing == g_graph.nodes.end()) {
            return bot_vehicle_nav_flight_corridor_clear(a, b, cls);
        }
        const float len = (b - a).len();
        const float skip = bot_vehicle_nav_landing_approach(b, landing->radius, a, cls)
            + (cls.layer == awh::Type::water ? kVehicleNavLandingApproachStep : 0.0f);
        return !segment_hits_level(a, b)
            && (skip >= len || bot_vehicle_nav_flight_corridor_clear(a, b + (a - b) * (skip / len), cls));
    };
    if ((dest - from).len() <= kFlightDirectMaxDist && clear(from, dest)) {
        fill_route(out, BotVehicleRouteKind::direct, {-1}, {dest}, {0.0f});
        return true;
    }
    return plan_over_layer(from, dest, cls, max_end_dist, excluded, kVehicleNavFlightStartReach, kFlightDirectMaxDist,
                           clear, out);
}

bool bot_vehicle_nav_random_node(const rf::Vector3& pos, float min_dist, float max_dist,
                                 const BotVehicleNavClass& cls, rf::Vector3& out)
{
    if (!ensure_graph()) {
        return false;
    }
    std::vector<rf::Vector3> nodes;
    for (const NavNode& node : g_graph.nodes) {
        const float dist_sq = (node.pos - pos).len_sq();
        if (usable_through(node, cls) && dist_sq >= min_dist * min_dist && dist_sq <= max_dist * max_dist) {
            nodes.push_back(node.pos);
        }
    }
    if (nodes.empty()) {
        return false;
    }
    out = nodes[std::uniform_int_distribution<std::size_t>(0, nodes.size() - 1)(g_rng)];
    return true;
}

int bot_vehicle_nav_layer_node_count(const BotVehicleNavClass& cls)
{
    if (!ensure_graph()) {
        return 0;
    }
    return static_cast<int>(std::count_if(g_graph.nodes.begin(), g_graph.nodes.end(),
                                          [&](const NavNode& node) { return usable(node, cls); }));
}

bool bot_vehicle_nav_point_in_liquid(const rf::Vector3& pos)
{
    return point_in_liquid_at(pos);
}

bool bot_vehicle_nav_floor_below(const rf::Vector3& pos, float depth, rf::Vector3& out)
{
    rf::PCollisionOut collision{};
    if (!vphys_collide_solid_segment(pos, pos - rf::Vector3{0.0f, depth, 0.0f}, rf::CF_PROCESS_INVISIBLE_FACES,
                                     collision)) {
        return false;
    }
    out = collision.hit_point;
    return true;
}

bool bot_vehicle_nav_liquid_surface_at(const rf::Vector3& pos, float& out_y)
{
    rf::GRoom* room = rf::level.geometry ? rf::find_room(rf::level.geometry, &pos) : nullptr;
    return room && rf::point_in_liquid(room, &pos) && room_liquid_surface_y(room, out_y);
}

bool bot_vehicle_nav_air_above(const rf::Vector3& pos, float surface_y)
{
    const rf::Vector3 air{pos.x, surface_y + 1.0f, pos.z};
    rf::GRoom* room = rf::level.geometry ? rf::find_room(rf::level.geometry, &air) : nullptr;
    return room && !rf::point_in_liquid(room, &air) && !segment_hits_level(pos, air);
}

bool bot_vehicle_nav_shore_spot(const rf::Vector3& pos, const BotVehicleNavClass& cls, float surface_y,
                                rf::Vector3& out)
{
    std::vector<std::pair<float, int>> candidates;
    const int count = waypoints_count();
    for (int i = 1; i < count; ++i) {
        rf::Vector3 wp;
        bool temporary = false;
        if (!waypoints_get_pos(i, wp) || !waypoints_get_temporary(i, temporary) || temporary
            || std::fabs(wp.y - surface_y) > kShoreBand) {
            continue;
        }
        const float dist_sq = (wp - pos).len_sq();
        if (dist_sq <= kShoreSearchDist * kShoreSearchDist) {
            candidates.emplace_back(dist_sq, i);
        }
    }
    sort_nearest(candidates);
    const float depth = surface_y - cls.corridor_radius;
    for (std::size_t c = 0; c < candidates.size() && c < kVehicleNavTraceCandidates; ++c) {
        rf::Vector3 wp;
        waypoints_get_pos(candidates[c].second, wp);
        // A node ashore: back off toward the hull until the spot is in the water.
        rf::Vector3 back = flat_vector(pos - wp);
        back.normalize_safe();
        // Only the first spot in the water with air above is tried for the corridor, so a call costs at most one
        // corridor per candidate.
        for (int step = 0; step <= kShoreBackSteps; ++step) {
            const rf::Vector3 spot =
                rf::Vector3{wp.x, depth, wp.z} + back * (kShoreBackStep * static_cast<float>(step));
            if (!point_in_liquid_at(spot) || !bot_vehicle_nav_air_above(spot, surface_y)) {
                continue;
            }
            if (bot_vehicle_nav_flight_corridor_clear(pos, spot, cls)) {
                out = spot;
                return true;
            }
            break;
        }
    }
    return false;
}

bool bot_vehicle_nav_dry_foot_spot(const rf::Vector3& pos, float max_dist, const BotVehicleNavClass& cls, float lift,
                                   const rf::Vector3* avoid, float avoid_radius, rf::Vector3& out)
{
    std::vector<std::pair<float, int>> candidates;
    const int count = waypoints_count();
    for (int i = 1; i < count; ++i) {
        rf::Vector3 wp;
        bool temporary = false;
        if (!waypoints_get_pos(i, wp) || !waypoints_get_temporary(i, temporary) || temporary) {
            continue;
        }
        const float dist_sq = (wp - pos).len_sq();
        if (dist_sq <= max_dist * max_dist && (!avoid || (wp - *avoid).len_sq() > avoid_radius * avoid_radius)) {
            candidates.emplace_back(dist_sq, i);
        }
    }
    // Over a lake the nearest nodes are swim nodes: skip those (one room lookup each) before the traced candidates.
    std::sort(candidates.begin(), candidates.end());
    std::size_t traced = 0;
    std::size_t looked = 0;
    for (const auto& candidate : candidates) {
        if (traced >= kVehicleNavTraceCandidates || ++looked > kDrySpotMaxLookups) {
            break;
        }
        rf::Vector3 wp;
        rf::Vector3 ground;
        waypoints_get_pos(candidate.second, wp);
        if (point_in_liquid_at(wp)) {
            continue;
        }
        ++traced;
        if (trace_ground_below_point(wp + rf::Vector3{0.0f, 0.5f, 0.0f}, kVehicleNavLandingGroundProbe, &ground)
            && !point_in_liquid_at(ground + rf::Vector3{0.0f, 0.1f, 0.0f})
            && bot_vehicle_nav_flight_corridor_clear(pos, wp + rf::Vector3{0.0f, lift, 0.0f}, cls)) {
            out = wp;
            return true;
        }
    }
    return false;
}

const char* bot_vehicle_nav_route_kind_name(BotVehicleRouteKind kind)
{
    switch (kind) {
        case BotVehicleRouteKind::direct:
            return "direct";
        case BotVehicleRouteKind::layer:
            return "layer";
        case BotVehicleRouteKind::terrain:
            return "terrain";
        default:
            return "none";
    }
}

bool bot_vehicle_nav_random_ground_point(const rf::Vector3& pos, const BotVehicleNavClass& cls, float min_dist,
                                         float max_dist, rf::Vector3& out)
{
    if (cls.layer != awh::Type::ground || !ensure_graph()) {
        return false;
    }
    if (bot_vehicle_nav_random_node(pos, min_dist, max_dist, cls, out)) {
        return true;
    }
    const auto in_band = [&](const rf::Vector3& p) {
        const float dist_sq = (p - pos).len_sq();
        return dist_sq >= min_dist * min_dist && dist_sq <= max_dist * max_dist;
    };
    ensure_terrain_nodes();
    if (g_terrain.nodes.empty()) {
        return false;
    }
    std::uniform_int_distribution<std::size_t> pick(0, g_terrain.nodes.size() - 1);
    for (int i = 0; i < kPatrolSamples; ++i) {
        const rf::Vector3 p = waypoint_pos(g_terrain.nodes[pick(g_rng)]);
        if (in_band(p)) {
            out = p;
            return true;
        }
    }
    return false;
}

bool bot_vehicle_nav_near_vehicle_road(const rf::Vector3& pos, float radius)
{
    if (!ensure_graph() || g_roads.segments.empty()) {
        return false;
    }
    ensure_road_cells();
    const auto near_segment = [&](int k) {
        const auto& [a, b] = g_roads.segments[k];
        const rf::Vector3 ab = b - a;
        const float t = std::clamp((pos - a).dot_prod(ab) / std::max(ab.len_sq(), 1e-6f), 0.0f, 1.0f);
        return (a + ab * t - pos).len_sq() <= radius * radius;
    };
    if (std::any_of(g_roads.long_segments.begin(), g_roads.long_segments.end(), near_segment)) {
        return true;
    }
    for (int x = road_cell(pos.x - radius); x <= road_cell(pos.x + radius); ++x) {
        for (int z = road_cell(pos.z - radius); z <= road_cell(pos.z + radius); ++z) {
            const auto it = g_roads.cells.find(road_cell_key(x, z));
            if (it != g_roads.cells.end() && std::any_of(it->second.begin(), it->second.end(), near_segment)) {
                return true;
            }
        }
    }
    return false;
}

int bot_vehicle_nav_road_version()
{
    ensure_graph();
    return g_roads.version;
}

int bot_vehicle_nav_landing_node_near(const rf::Vector3& pos, const BotVehicleNavClass& cls, float max_dist,
                                      int* out_exit_waypoint)
{
    if (out_exit_waypoint) {
        *out_exit_waypoint = 0;
    }
    if (!ensure_graph()) {
        return -1;
    }
    if (out_exit_waypoint) {
        ensure_landing_exits();
    }
    int best = -1;
    float best_dist_sq = max_dist * max_dist;
    for (int i = 0; i < static_cast<int>(g_graph.nodes.size()); ++i) {
        const NavNode& node = g_graph.nodes[i];
        const float dist_sq = (node.pos - pos).len_sq();
        if (node.landing && usable(node, cls) && dist_sq <= best_dist_sq
            && (!out_exit_waypoint || node.exit_waypoint > 0)) {
            best = i;
            best_dist_sq = dist_sq;
        }
    }
    if (best >= 0 && out_exit_waypoint) {
        *out_exit_waypoint = g_graph.nodes[best].exit_waypoint;
    }
    return best;
}

void bot_vehicle_nav_render_debug()
{
    if (g_debug_draw && ensure_graph()) {
        waypoints_vehicle_render_debug(-1);
    }
}

void bot_vehicle_nav_apply_patches()
{
    dbg_vehnav_cmd.register_cmd();
}
