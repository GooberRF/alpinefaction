#include "waypoints_vehicle.h"
#include "alpine_settings.h"
#include "level.h"
#include "waypoints.h"
#include "../graphics/gr.h"
#include "../graphics/weather.h"
#include "../multi/bots/bot_math.h"
#include "../multi/bots/bot_vehicle_nav.h"
#include "../multi/vehicles/vehicle.h"
#include "../object/alpine_waypoint_helper.h"
#include "../rf/entity.h"
#include "../rf/geometry.h"
#include "../rf/gr/gr.h"
#include "../rf/level.h"
#include "../rf/player/camera.h"
#include "../rf/player/player.h"
#include <xlog/xlog.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <cstddef>
#include <numbers>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace awh = alpine_waypoint_helper;

namespace
{

// Ground links are split into nodes about this far apart, each this high over the ground under it.
constexpr float kGroundNodeSpacing = 8.0f;
constexpr float kGroundNodeHeight = 1.0f;
// A ground node's floor is looked for from this far above (when nothing is in the way) down to this far below. The
// lift clears the steepest piece a link may keep, so the floor of the next piece is never above the start.
constexpr float kGroundSnapLift = kVehicleNavCarMaxDropGrade * kGroundNodeSpacing + kVehicleNavCarStepClimb + 1.0f;
constexpr float kGroundSnapDepth = 16.0f;
// Helpers this close together (after a ground helper is put on the floor) are one node.
constexpr float kCoincidentDist = 0.25f;
// Nodes may lie this far outside the level's geometry box, and no farther.
constexpr float kLevelMargin = 64.0f;
// How far past a landing node's radius its exit waypoint may be.
constexpr float kLandingExitReach = 16.0f;
constexpr int kDebugCircleSegments = 16;
// The debug view draws nodes and links this near the camera, and works its factory lines out at most this often
// while the layer changes (a gizmo drag changes it every frame).
constexpr float kDebugDrawDistance = 250.0f;
constexpr auto kFactoryLinesRefresh = std::chrono::milliseconds{500};

VehicleWaypointLayer g_layer;
int g_layer_version = 0;
int g_warned_helpers_serial = -1;

// The debug view's lines from each factory to the node its hull's layer route would start at (the planner's rule);
// nothing else needs them.
struct FactoryLines
{
    int layer_version = -1;
    std::chrono::steady_clock::time_point computed_at{};
    std::vector<std::pair<rf::Vector3, int>> lines;
};
FactoryLines g_factory_lines;

void mark_changed()
{
    ++g_layer_version;
}

bool valid_node(const int node)
{
    return node >= 0 && node < static_cast<int>(g_layer.nodes.size());
}

// The corridor radius (ground: half width per size class) of the widest hull of each kind the level's factories spawn;
// 0 where none does.
struct LevelHulls
{
    float air = 0.0f;
    float water = 0.0f;
    std::array<float, awh::size_class_max + 1> ground{};

    bool has(const awh::Type type) const
    {
        switch (type) {
            case awh::Type::air:
                return air > 0.0f;
            case awh::Type::water:
                return water > 0.0f;
            case awh::Type::ground:
                return std::any_of(ground.begin(), ground.end(), [](float width) { return width > 0.0f; });
        }
        return false;
    }

    // Some ground hull the level spawns is no bigger than cap.
    bool ground_fits(const awh::SizeClass cap) const
    {
        return std::any_of(ground.begin(), ground.begin() + static_cast<int>(cap) + 1,
                           [](float width) { return width > 0.0f; });
    }
};

LevelHulls level_hulls()
{
    LevelHulls hulls;
    for (int i = 0; const AlpineVehicleFactoryInfo* info = vehicle_factory(i); ++i) {
        BotVehicleNavClass cls;
        if (!bot_vehicle_nav_class_for_type(rf::entity_lookup_type(info->vehicle_class.c_str()), cls)) {
            continue;
        }
        float& slot = cls.layer == awh::Type::air ? hulls.air
            : cls.layer == awh::Type::water       ? hulls.water
                                                  : hulls.ground[static_cast<int>(cls.size)];
        slot = std::max(slot, cls.corridor_radius);
    }
    return hulls;
}

// The floor under pos, looked for from lift above it when nothing is in the way down to depth below that, raised to
// where a ground node sits.
bool snap_to_ground(const rf::Vector3& pos, float lift, float depth, rf::Vector3& out)
{
    rf::Vector3 start = pos + rf::Vector3{0.0f, lift, 0.0f};
    if (!bot_vehicle_nav_segment_clear(pos, start)) {
        start = pos;
    }
    rf::Vector3 floor;
    if (!bot_vehicle_nav_floor_below(start, start.y - pos.y + depth, floor)) {
        return false;
    }
    out = floor + rf::Vector3{0.0f, kGroundNodeHeight, 0.0f};
    return true;
}

// Past the per-node cap the loader would drop the link, so it is never made.
void add_link(const int from, const int to, const awh::SizeClass size)
{
    auto& links = g_layer.nodes[from].links;
    if (links.size() >= awh::max_links) {
        xlog::warn("[Waypoints] vehicle node at ({:.1f}, {:.1f}, {:.1f}) has {} links already; one more dropped",
                   g_layer.nodes[from].pos.x, g_layer.nodes[from].pos.y, g_layer.nodes[from].pos.z, awh::max_links);
        return;
    }
    links.push_back({to, size});
}

struct GenerationCounts
{
    int dropped_helpers = 0;
    int dropped_links = 0;
    int one_way_ground_links = 0;
};

// The largest size class up to cap whose hull fits along the piece, or -1. Only the sizes the level spawns are
// measured, and a wider hull never fits where a narrower one does not.
int piece_fit(const rf::Vector3& from, const rf::Vector3& to, const LevelHulls& hulls, const awh::SizeClass cap,
              rf::Vector3& out_hit)
{
    int fit = -1;
    for (int size = 0; size <= static_cast<int>(cap); ++size) {
        const float half_width = hulls.ground[size];
        if (half_width <= 0.0f) {
            continue;
        }
        if (!bot_vehicle_nav_ground_hull_clear(from, to, half_width, &out_hit)) {
            break;
        }
        fit = size;
    }
    return fit;
}

// Higher first (then by x and z), so a link is walked the same way whichever helper it was drawn from.
bool walks_from(const rf::Vector3& a, const rf::Vector3& b)
{
    return std::tie(a.y, a.x, a.z) >= std::tie(b.y, b.x, b.z);
}

// Nodes about kGroundNodeSpacing apart on the floor from `from` to `to`, both ends included. Each is looked for from
// above the higher of the last node and the straight line, walking downhill where the link slopes, so a climb never
// starts the search inside the hill.
bool ground_points(const rf::Vector3& from, const rf::Vector3& to, std::vector<rf::Vector3>& out,
                   rf::Vector3& out_failed)
{
    const bool reversed = !walks_from(from, to);
    const rf::Vector3& start = reversed ? to : from;
    const rf::Vector3& end = reversed ? from : to;
    const int pieces = std::max(1, static_cast<int>(std::ceil(flat_vector(end - start).len() / kGroundNodeSpacing)));
    out.assign(1, start);
    for (int k = 1; k < pieces; ++k) {
        rf::Vector3 guess = start + (end - start) * (static_cast<float>(k) / static_cast<float>(pieces));
        const float line_y = guess.y;
        const float last_y = out.back().y;
        guess.y = std::max(last_y, line_y);
        rf::Vector3 point;
        if (!snap_to_ground(guess, kGroundSnapLift, guess.y - std::min(last_y, line_y) + kGroundSnapDepth, point)) {
            out_failed = guess;
            return false;
        }
        out.push_back(point);
    }
    out.push_back(end);
    if (reversed) {
        std::reverse(out.begin(), out.end());
    }
    return true;
}

// A ground link a <-> b, both ends on the floor: every piece is measured for the hulls that fit it, and each
// direction is kept only if every piece passes it. A link kept one way takes only the hulls its narrowest piece takes
// all along, since a hull cannot turn back on it; so no piece leaves a dead end or a node a hull of some size can
// drive into and not out of.
void build_ground_link(int a, int b, int32_t uid_a, int32_t uid_b, const LevelHulls& hulls, GenerationCounts& counts)
{
    const awh::SizeClass cap = std::min(g_layer.nodes[a].size, g_layer.nodes[b].size);
    if (!hulls.ground_fits(cap)) {
        xlog::warn("[Waypoints] ground link {} - {}: its helpers take up to {}, but the level spawns no ground vehicle "
                   "that small; dropped", uid_a, uid_b, waypoints_vehicle_size_name(cap));
        ++counts.dropped_links;
        return;
    }
    std::vector<rf::Vector3> points;
    rf::Vector3 failed{};
    if (!ground_points(g_layer.nodes[a].pos, g_layer.nodes[b].pos, points, failed)) {
        xlog::warn("[Waypoints] ground link {} - {}: no ground under ({:.1f}, {:.1f}, {:.1f}); dropped", uid_a, uid_b,
                   failed.x, failed.y, failed.z);
        ++counts.dropped_links;
        return;
    }
    const int pieces = static_cast<int>(points.size()) - 1;
    if (static_cast<int>(g_layer.nodes.size()) + pieces - 1 > kMaxVehicleWaypointNodes) {
        xlog::warn("[Waypoints] ground link {} - {}: the vehicle layer is full; dropped", uid_a, uid_b);
        ++counts.dropped_links;
        return;
    }

    std::vector<int> fits(pieces, -1);
    rf::Vector3 blocker{};
    int narrow = -1;
    int forward_fail = -1;
    int backward_fail = -1;
    for (int k = 0; k < pieces && narrow < 0 && (forward_fail < 0 || backward_fail < 0); ++k) {
        const rf::Vector3& p = points[k];
        const rf::Vector3& q = points[k + 1];
        fits[k] = piece_fit(p, q, hulls, cap, blocker);
        if (fits[k] < 0) {
            narrow = k;
            break;
        }
        if (forward_fail < 0 && !bot_vehicle_nav_ground_drive_clear(p, q)) {
            forward_fail = k;
        }
        if (backward_fail < 0 && !bot_vehicle_nav_ground_drive_clear(q, p)) {
            backward_fail = k;
        }
    }
    if (narrow >= 0 || (forward_fail >= 0 && backward_fail >= 0)) {
        const int k = narrow >= 0 ? narrow : std::min(forward_fail, backward_fail);
        const std::string reason = narrow >= 0
            ? std::format("too narrow for every vehicle the level spawns (blocked at ({:.1f}, {:.1f}, {:.1f}))",
                          blocker.x, blocker.y, blocker.z)
            : std::string{"too steep or wet (each way somewhere along the link)"};
        xlog::warn("[Waypoints] ground link {} - {}: piece {}/{} at ({:.1f}, {:.1f}, {:.1f}) is {}; dropped", uid_a,
                   uid_b, k + 1, pieces, points[k].x, points[k].y, points[k].z, reason);
        ++counts.dropped_links;
        return;
    }
    if (forward_fail >= 0 || backward_fail >= 0) {
        const int k = std::max(forward_fail, backward_fail);
        xlog::info("[Waypoints] ground link {} - {}: one-way {} -> {} (piece {}/{} at ({:.1f}, {:.1f}, {:.1f}) is too "
                   "steep the other way)", uid_a, uid_b, forward_fail < 0 ? uid_a : uid_b,
                   forward_fail < 0 ? uid_b : uid_a, k + 1, pieces, points[k].x, points[k].y, points[k].z);
        ++counts.one_way_ground_links;
    }

    const float radius_a = g_layer.nodes[a].radius;
    const float radius_b = g_layer.nodes[b].radius;
    std::vector<int> chain{a};
    for (int k = 1; k < pieces; ++k) {
        VehicleWaypointNode node;
        node.pos = points[k];
        node.type = awh::Type::ground;
        node.size = cap;
        const float t = static_cast<float>(k) / static_cast<float>(pieces);
        node.radius = awh::sanitize_radius(radius_a + (radius_b - radius_a) * t);
        chain.push_back(static_cast<int>(g_layer.nodes.size()));
        g_layer.nodes.push_back(std::move(node));
    }
    chain.push_back(b);
    const bool one_way = forward_fail >= 0 || backward_fail >= 0;
    const int narrowest = *std::min_element(fits.begin(), fits.end());
    for (int k = 0; k < pieces; ++k) {
        const auto size = static_cast<awh::SizeClass>(one_way ? narrowest : fits[k]);
        if (forward_fail < 0) {
            add_link(chain[k], chain[k + 1], size);
        }
        if (backward_fail < 0) {
            add_link(chain[k + 1], chain[k], size);
        }
    }
}

// Why a fighter's or sub's link is not clear, or empty when it is. Near a landing end the hull is setting down (or
// surfacing), so only the centre line counts over its approach (bot_vehicle_nav_landing_approach). Only the centre
// line sees a wall inside such a stretch; past the radius, a water stretch ends at dry land 2 u or more across. The
// corridor skips at most half the link from each end, so a link whose stretches each reach its middle is checked by
// its centre line alone, the rest of any other by the corridor at the hull's radius; a sub's link must be in the water
// from the first sample past each stretch (which may lie past half the link), or midway between the stretches when
// those samples cross.
std::string flight_link_blocker(int a, int b, const BotVehicleNavClass& cls)
{
    const VehicleWaypointNode& node_a = g_layer.nodes[a];
    const VehicleWaypointNode& node_b = g_layer.nodes[b];
    const rf::Vector3 delta = node_b.pos - node_a.pos;
    const float len = delta.len();
    if (len < 1e-3f) {
        return {};
    }
    const bool water = cls.layer == awh::Type::water;
    constexpr float kApproachStep = kVehicleNavLandingApproachStep;
    const float half = 0.5f * len;
    const auto approach = [&](const VehicleWaypointNode& node, const rf::Vector3& toward) {
        return node.landing() ? bot_vehicle_nav_landing_approach(node.pos, node.radius, toward, cls) : 0.0f;
    };
    const float reach_a = approach(node_a, node_b.pos);
    const float reach_b = approach(node_b, node_a.pos);
    const float zone_a = std::min(reach_a, half);
    const float zone_b = std::min(reach_b, half);
    const rf::Vector3 dir = delta / len;
    rf::Vector3 hit{};
    if (!bot_vehicle_nav_segment_clear(node_a.pos, node_b.pos)) {
        bot_vehicle_nav_corridor_hit(node_a.pos, node_b.pos, 0.0f, hit);
        return std::format("its centre line is blocked at ({:.1f}, {:.1f}, {:.1f})", hit.x, hit.y, hit.z);
    }
    if (zone_a + zone_b >= len) {
        return {};
    }
    const rf::Vector3 p = node_a.pos + dir * zone_a;
    const rf::Vector3 q = node_b.pos - dir * zone_b;
    if (bot_vehicle_nav_corridor_hit(p, q, cls.corridor_radius, hit)) {
        return std::format("a ray at radius {:.2f} is blocked at ({:.1f}, {:.1f}, {:.1f})", cls.corridor_radius, hit.x,
                           hit.y, hit.z);
    }
    if (water && reach_a + reach_b < len) {
        // From the sample a landing end's stretch stopped at, which approach saw leave the band: in the water on any
        // link worth keeping. When those samples cross, only the point midway between them is tested; it lies past
        // both stretches.
        const float from_d = node_a.landing() ? std::min(reach_a + kApproachStep, len) : 0.0f;
        const float to_d = len - (node_b.landing() ? std::min(reach_b + kApproachStep, len) : 0.0f);
        const float first = from_d <= to_d ? from_d : 0.5f * (from_d + to_d);
        const float last = from_d <= to_d ? to_d : first;
        const int wet_samples = std::clamp(static_cast<int>(std::ceil((last - first) / kVehicleNavWaterProbeStep)), 1,
                                           kVehicleNavWaterProbeMax);
        for (int k = 0; k <= wet_samples; ++k) {
            const rf::Vector3 point =
                node_a.pos + dir * (first + (last - first) * (static_cast<float>(k) / static_cast<float>(wet_samples)));
            if (!bot_vehicle_nav_point_in_liquid(point)) {
                return std::format("it leaves the water at ({:.1f}, {:.1f}, {:.1f}) away from a landing end", point.x,
                                   point.y, point.z);
            }
        }
    }
    return {};
}

// A node a layer route of the class may start at, as the nav's planner takes one.
bool node_starts(const VehicleWaypointNode& node, const BotVehicleNavClass& cls)
{
    const int need = cls.layer == awh::Type::ground ? static_cast<int>(cls.size) : 0;
    return node.type == cls.layer && static_cast<int>(node.size) >= need
        && std::any_of(node.links.begin(), node.links.end(), [need](const VehicleWaypointLink& link) {
               return static_cast<int>(std::min(link.size, g_layer.nodes[link.to].size)) >= need;
           });
}

// The node a factory's hull would start a layer route from, by the planner's reach and start rule. A flyer's or sub's
// corridor is traced from a corridor radius above the factory.
int attach_factory(const rf::Vector3& pos, const BotVehicleNavClass& cls)
{
    const bool ground = cls.layer == awh::Type::ground;
    rf::Vector3 start = pos;
    if (!ground) {
        const rf::Vector3 lifted = pos + rf::Vector3{0.0f, cls.corridor_radius, 0.0f};
        if (bot_vehicle_nav_segment_clear(pos, lifted)) {
            start = lifted;
        }
    }
    const float reach = ground ? kVehicleNavGroundStartReach : kVehicleNavFlightStartReach;
    std::vector<std::pair<float, int>> candidates;
    for (int i = 0; i < static_cast<int>(g_layer.nodes.size()); ++i) {
        const float dist_sq = (g_layer.nodes[i].pos - pos).len_sq();
        if (node_starts(g_layer.nodes[i], cls) && dist_sq <= reach * reach) {
            candidates.emplace_back(dist_sq, i);
        }
    }
    std::sort(candidates.begin(), candidates.end());
    // The planner traces the 4 nearest. A sub's spawn point may stand out of the water, so only the hull's clearance
    // counts (the air corridor).
    BotVehicleNavClass clearance = cls;
    clearance.layer = awh::Type::air;
    for (std::size_t c = 0; c < candidates.size() && c < kVehicleNavStartCandidates; ++c) {
        const rf::Vector3& node_pos = g_layer.nodes[candidates[c].second].pos;
        if (ground ? bot_vehicle_nav_ground_corridor_clear(start, node_pos, cls.corridor_radius)
                   : bot_vehicle_nav_flight_corridor_clear(start, node_pos, clearance)) {
            return candidates[c].second;
        }
    }
    return -1;
}

void update_factory_lines()
{
    const auto now = std::chrono::steady_clock::now();
    if (g_factory_lines.layer_version == g_layer_version
        || (g_factory_lines.layer_version >= 0 && now - g_factory_lines.computed_at < kFactoryLinesRefresh)) {
        return;
    }
    g_factory_lines.layer_version = g_layer_version;
    g_factory_lines.computed_at = now;
    g_factory_lines.lines.clear();
    for (int i = 0; const AlpineVehicleFactoryInfo* info = vehicle_factory(i); ++i) {
        BotVehicleNavClass cls;
        if (!bot_vehicle_nav_class_for_type(rf::entity_lookup_type(info->vehicle_class.c_str()), cls)) {
            continue;
        }
        if (const int node = attach_factory(info->pos, cls); node >= 0) {
            g_factory_lines.lines.emplace_back(info->pos, node);
        }
    }
}

bool landing_spot_ok(const VehicleWaypointNode& node)
{
    switch (node.type) {
        case awh::Type::air:
            return trace_ground_below_point(node.pos, kVehicleNavLandingGroundProbe);
        case awh::Type::water: {
            rf::GRoom* room = rf::level.geometry ? rf::find_room(rf::level.geometry, &node.pos) : nullptr;
            float surface_y = 0.0f;
            return room && room_liquid_surface_y(room, surface_y)
                && std::fabs(node.pos.y - surface_y) <= kVehicleNavLandingFloatBand;
        }
        case awh::Type::ground:
            return false;
    }
    return false;
}

// The nearest persistent foot waypoint within reach that a clear line joins to the landing spot.
bool find_exit(const VehicleWaypointNode& node, rf::Vector3& out)
{
    if (!landing_spot_ok(node)) {
        return false;
    }
    const float reach = node.radius + kLandingExitReach;
    std::vector<std::pair<float, int>> candidates;
    const int count = waypoints_count();
    for (int i = 1; i < count; ++i) {
        rf::Vector3 pos;
        bool temporary = false;
        if (!waypoints_get_pos(i, pos) || !waypoints_get_temporary(i, temporary) || temporary) {
            continue;
        }
        const float dist_sq = (pos - node.pos).len_sq();
        if (dist_sq <= reach * reach) {
            candidates.emplace_back(dist_sq, i);
        }
    }
    std::sort(candidates.begin(), candidates.end());
    for (std::size_t c = 0; c < candidates.size() && c < kVehicleNavTraceCandidates; ++c) {
        rf::Vector3 pos;
        waypoints_get_pos(candidates[c].second, pos);
        if (bot_vehicle_nav_segment_clear(node.pos, pos)) {
            out = pos;
            return true;
        }
    }
    return false;
}

rf::Color type_color(const awh::Type type, const awh::SizeClass size)
{
    switch (type) {
        case awh::Type::air:
            return {0, 220, 255, 255};
        case awh::Type::water:
            return {90, 90, 255, 255};
        case awh::Type::ground:
            break;
    }
    switch (size) {
        case awh::SizeClass::jeep:
            return {255, 220, 0, 255};
        case awh::SizeClass::apc:
            return {255, 140, 0, 255};
        case awh::SizeClass::driller:
            break;
    }
    return {60, 220, 60, 255};
}

void draw_circle(const rf::Vector3& center, float radius)
{
    rf::Vector3 prev = center + rf::Vector3{radius, 0.0f, 0.0f};
    for (int i = 1; i <= kDebugCircleSegments; ++i) {
        const float angle = 2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / kDebugCircleSegments;
        const rf::Vector3 point = center + rf::Vector3{std::cos(angle) * radius, 0.0f, std::sin(angle) * radius};
        rf::gr::line_vec(prev, point, no_overdraw_2d_line);
        prev = point;
    }
}

} // namespace

const VehicleWaypointLayer& waypoints_vehicle_layer()
{
    return g_layer;
}

int waypoints_vehicle_layer_version()
{
    return g_layer_version;
}

VehicleWaypointLayer& waypoints_vehicle_edit_layer()
{
    mark_changed();
    return g_layer;
}

void waypoints_vehicle_set_layer(VehicleWaypointLayer layer)
{
    g_layer = std::move(layer);
    mark_changed();
}

void waypoints_vehicle_clear()
{
    g_layer = {};
    mark_changed();
}

rf::Vector3 waypoints_vehicle_clamp_to_level(const rf::Vector3& pos)
{
    if (!rf::level.geometry) {
        return pos;
    }
    const rf::Vector3 margin{kLevelMargin, kLevelMargin, kLevelMargin};
    const rf::Vector3 lo = rf::level.geometry->bbox_min - margin;
    const rf::Vector3 hi = rf::level.geometry->bbox_max + margin;
    return rf::Vector3{std::clamp(pos.x, lo.x, hi.x), std::clamp(pos.y, lo.y, hi.y), std::clamp(pos.z, lo.z, hi.z)};
}

bool waypoints_vehicle_generation_wanted()
{
    const auto& helpers = alpine_waypoint_helpers();
    if (helpers.empty() || !vehicle_level_has_factories()) {
        return false;
    }
    const LevelHulls hulls = level_hulls();
    return std::any_of(helpers.begin(), helpers.end(),
                       [&hulls](const AlpineWaypointHelper& helper) { return hulls.has(helper.type); });
}

bool waypoints_vehicle_helpers_unused()
{
    return g_layer.nodes.empty() && waypoints_vehicle_generation_wanted();
}

void waypoints_vehicle_on_awp_loaded(const bool has_layer)
{
    if (has_layer || !g_alpine_game_config.dbg_bot || !waypoints_vehicle_generation_wanted()) {
        return;
    }
    const int serial = alpine_waypoint_helpers_serial();
    if (serial == g_warned_helpers_serial) {
        return;
    }
    g_warned_helpers_serial = serial;
    xlog::warn("[Waypoints] {} has {} Waypoint Helper(s) but its AWP has no vehicle layer: bots in vehicles ignore the "
               "helpers until the AWP is regenerated", rf::level.filename, alpine_waypoint_helpers().size());
}

VehicleWaypointGenerationStats waypoints_vehicle_generate(const std::function<bool(int, int)>& out_of_time)
{
    VehicleWaypointGenerationStats stats;
    waypoints_vehicle_clear();
    if (!waypoints_vehicle_generation_wanted()) {
        return stats;
    }

    const LevelHulls hulls = level_hulls();
    const auto& helpers = alpine_waypoint_helpers();
    GenerationCounts counts;
    std::unordered_map<int32_t, int> node_of;
    for (const AlpineWaypointHelper& helper : helpers) {
        if (!hulls.has(helper.type)) {
            ++stats.unused_helpers;
            continue;
        }
        VehicleWaypointNode node;
        node.pos = waypoints_vehicle_clamp_to_level(helper.pos);
        node.type = helper.type;
        node.radius = helper.radius;
        // A ground helper is put on the floor below it instead, which lies inside the level.
        if (node.pos != helper.pos && helper.type != awh::Type::ground) {
            xlog::warn("[Waypoints] helper {} lies outside the level; moved to its edge", helper.uid);
        }
        if (helper.type == awh::Type::ground) {
            node.size = helper.size_class;
            if (!snap_to_ground(helper.pos, kGroundSnapLift, kGroundSnapDepth, node.pos)) {
                xlog::warn("[Waypoints] ground helper {} has no ground within {:.0f} u below it; dropped", helper.uid,
                           kGroundSnapDepth);
                ++counts.dropped_helpers;
                continue;
            }
        }
        else {
            node.flags = helper.flags & awh::flag_landing;
        }
        // Helpers stacked on one spot make one node; the link between them has no length to drive.
        const auto same = std::find_if(g_layer.nodes.begin(), g_layer.nodes.end(), [&](const VehicleWaypointNode& n) {
            return n.type == node.type && (n.pos - node.pos).len_sq() <= kCoincidentDist * kCoincidentDist;
        });
        if (same != g_layer.nodes.end()) {
            same->radius = std::max(same->radius, node.radius);
            same->size = std::min(same->size, node.size);
            same->flags |= node.flags;
            node_of.emplace(helper.uid, static_cast<int>(same - g_layer.nodes.begin()));
            ++stats.merged_helpers;
            continue;
        }
        node_of.emplace(helper.uid, static_cast<int>(g_layer.nodes.size()));
        g_layer.nodes.push_back(std::move(node));
    }

    // A link drawn either way, or both ways, joins the pair once.
    std::vector<std::pair<int32_t, int32_t>> pairs;
    std::unordered_set<uint64_t> seen;
    for (const AlpineWaypointHelper& helper : helpers) {
        for (const int32_t link : helper.links) {
            const auto lo = static_cast<uint32_t>(std::min(helper.uid, link));
            const auto hi = static_cast<uint32_t>(std::max(helper.uid, link));
            if (seen.insert((static_cast<uint64_t>(lo) << 32) | hi).second) {
                pairs.emplace_back(helper.uid, link);
            }
        }
    }
    // Merged helpers can map two uid pairs onto one node pair, which is built once.
    std::unordered_set<uint64_t> built;
    const int total = static_cast<int>(pairs.size());
    for (int p = 0; p < total; ++p) {
        if (out_of_time(p, total)) {
            break;
        }
        const auto [uid_a, uid_b] = pairs[p];
        const auto it_a = node_of.find(uid_a);
        const auto it_b = node_of.find(uid_b);
        if (it_a == node_of.end() || it_b == node_of.end()) {
            ++stats.skipped_links;
            continue;
        }
        const int a = it_a->second;
        const int b = it_b->second;
        const auto lo = static_cast<uint64_t>(std::min(a, b));
        const auto hi = static_cast<uint64_t>(std::max(a, b));
        if (a == b || !built.insert((lo << 32) | hi).second) {
            continue;
        }
        const awh::Type type = g_layer.nodes[a].type;
        if (type != g_layer.nodes[b].type) {
            xlog::warn("[Waypoints] link {} - {} joins a {} helper to a {} one; ignored", uid_a, uid_b,
                       waypoints_vehicle_type_name(type), waypoints_vehicle_type_name(g_layer.nodes[b].type));
            ++counts.dropped_links;
            continue;
        }
        if (type == awh::Type::ground) {
            build_ground_link(a, b, uid_a, uid_b, hulls, counts);
            continue;
        }
        BotVehicleNavClass cls;
        cls.layer = type;
        cls.corridor_radius = type == awh::Type::air ? hulls.air : hulls.water;
        if (const std::string blocker = flight_link_blocker(a, b, cls); !blocker.empty()) {
            xlog::warn("[Waypoints] {} link {} - {}: {}; dropped", waypoints_vehicle_type_name(type), uid_a, uid_b,
                       blocker);
            ++counts.dropped_links;
            continue;
        }
        add_link(a, b, awh::SizeClass::driller);
        add_link(b, a, awh::SizeClass::driller);
    }

    // A ground helper every link of which generation kept one way only, all inward or all outward.
    std::vector<int> inbound(g_layer.nodes.size(), 0);
    for (const VehicleWaypointNode& node : g_layer.nodes) {
        for (const VehicleWaypointLink& link : node.links) {
            ++inbound[link.to];
        }
    }
    std::unordered_set<int> warned;
    for (const AlpineWaypointHelper& helper : helpers) {
        const auto it = node_of.find(helper.uid);
        if (it == node_of.end() || !warned.insert(it->second).second) {
            continue;
        }
        const VehicleWaypointNode& node = g_layer.nodes[it->second];
        const bool in = inbound[it->second] > 0;
        const bool out = !node.links.empty();
        if (node.type == awh::Type::ground && in != out) {
            xlog::warn("[Waypoints] ground helper {} can be driven {} but not {}", helper.uid, in ? "to" : "from",
                       in ? "from" : "to");
        }
    }

    for (VehicleWaypointNode& node : g_layer.nodes) {
        if (!node.landing()) {
            continue;
        }
        ++stats.landing_nodes;
        node.has_exit = find_exit(node, node.exit_pos);
        stats.landing_exits += node.has_exit ? 1 : 0;
    }

    stats.nodes = static_cast<int>(g_layer.nodes.size());
    std::vector<int> parent(g_layer.nodes.size());
    for (int i = 0; i < stats.nodes; ++i) {
        parent[i] = i;
    }
    const auto root = [&parent](int i) {
        while (parent[i] != i) {
            parent[i] = parent[parent[i]];
            i = parent[i];
        }
        return i;
    };
    for (int i = 0; i < stats.nodes; ++i) {
        stats.links += static_cast<int>(g_layer.nodes[i].links.size());
        for (const VehicleWaypointLink& link : g_layer.nodes[i].links) {
            parent[root(i)] = root(link.to);
        }
    }
    for (int i = 0; i < stats.nodes; ++i) {
        if (root(i) == i) {
            ++stats.networks[static_cast<int>(g_layer.nodes[i].type)];
        }
    }
    const int ground_networks = stats.networks[static_cast<int>(awh::Type::ground)];
    if (ground_networks > 1) {
        xlog::warn("[Waypoints] the ground vehicle layer falls into {} separate road networks; a dropped link above "
                   "may have split it", ground_networks);
    }
    stats.one_way_ground_links = counts.one_way_ground_links;
    stats.dropped_helpers = counts.dropped_helpers;
    stats.dropped_links = counts.dropped_links;
    mark_changed();
    return stats;
}

int waypoints_vehicle_add_node(const rf::Vector3& pos, const awh::Type type)
{
    if (static_cast<int>(g_layer.nodes.size()) >= kMaxVehicleWaypointNodes) {
        return -1;
    }
    VehicleWaypointNode node;
    node.pos = waypoints_vehicle_clamp_to_level(pos);
    node.type = type;
    g_layer.nodes.push_back(std::move(node));
    mark_changed();
    return static_cast<int>(g_layer.nodes.size()) - 1;
}

bool waypoints_vehicle_remove_node(const int node)
{
    if (!valid_node(node)) {
        return false;
    }
    g_layer.nodes.erase(g_layer.nodes.begin() + node);
    for (VehicleWaypointNode& other : g_layer.nodes) {
        std::erase_if(other.links, [node](const VehicleWaypointLink& link) { return link.to == node; });
        for (VehicleWaypointLink& link : other.links) {
            link.to -= link.to > node ? 1 : 0;
        }
    }
    mark_changed();
    return true;
}

const VehicleWaypointLink* waypoints_vehicle_find_link(const int from, const int to)
{
    if (!valid_node(from)) {
        return nullptr;
    }
    const auto& links = g_layer.nodes[from].links;
    const auto it = std::find_if(links.begin(), links.end(), [to](const VehicleWaypointLink& l) { return l.to == to; });
    return it != links.end() ? &*it : nullptr;
}

bool waypoints_vehicle_set_link(const int from, const int to, const bool present, const awh::SizeClass size)
{
    if (!valid_node(from) || !valid_node(to) || from == to) {
        return false;
    }
    auto& links = g_layer.nodes[from].links;
    const auto it = std::find_if(links.begin(), links.end(), [to](const VehicleWaypointLink& l) { return l.to == to; });
    if (!present) {
        if (it == links.end()) {
            return false;
        }
        links.erase(it);
    }
    else if (it != links.end()) {
        it->size = size;
    }
    else if (links.size() < awh::max_links) {
        links.push_back({to, size});
    }
    else {
        return false;
    }
    mark_changed();
    return true;
}

int waypoints_vehicle_drop_cross_type_links(const int node)
{
    if (!valid_node(node)) {
        return 0;
    }
    const awh::Type type = g_layer.nodes[node].type;
    int dropped = static_cast<int>(std::erase_if(g_layer.nodes[node].links, [type](const VehicleWaypointLink& link) {
        return g_layer.nodes[link.to].type != type;
    }));
    for (VehicleWaypointNode& other : g_layer.nodes) {
        if (other.type != type) {
            dropped += static_cast<int>(std::erase_if(
                other.links, [node](const VehicleWaypointLink& link) { return link.to == node; }));
        }
    }
    mark_changed();
    return dropped;
}

bool waypoints_vehicle_update_exit(const int node)
{
    if (!valid_node(node)) {
        return false;
    }
    VehicleWaypointNode& target = g_layer.nodes[node];
    target.has_exit = target.landing() && find_exit(target, target.exit_pos);
    mark_changed();
    return target.has_exit;
}

const char* waypoints_vehicle_type_name(const awh::Type type)
{
    switch (type) {
        case awh::Type::air:
            return "air";
        case awh::Type::water:
            return "water";
        case awh::Type::ground:
            return "ground";
    }
    return "?";
}

const char* waypoints_vehicle_size_name(const awh::SizeClass size)
{
    switch (size) {
        case awh::SizeClass::jeep:
            return "jeep";
        case awh::SizeClass::apc:
            return "APC";
        case awh::SizeClass::driller:
            return "any";
    }
    return "?";
}

void waypoints_vehicle_render_debug(const int highlighted_node)
{
    bool cull = false;
    rf::Vector3 eye{};
    if (rf::local_player && rf::local_player->cam) {
        eye = rf::camera_get_pos(rf::local_player->cam);
        cull = true;
    }
    const auto in_range = [&](const rf::Vector3& pos) {
        return !cull || (pos - eye).len_sq() <= kDebugDrawDistance * kDebugDrawDistance;
    };
    const int count = static_cast<int>(g_layer.nodes.size());
    for (int i = 0; i < count; ++i) {
        const VehicleWaypointNode& node = g_layer.nodes[i];
        const bool node_in_range = in_range(node.pos);
        const bool highlighted = i == highlighted_node;
        if (node_in_range) {
            rf::Color color = type_color(node.type, node.size);
            if (!highlighted) {
                color.alpha = 160;
            }
            rf::gr::set_color(color);
            draw_circle(node.pos, node.radius);
            rf::gr::sphere(node.pos, highlighted ? 0.6f : 0.3f, no_overdraw_2d_line);
            if (node.landing() && node.has_exit) {
                rf::gr::set_color(255, 255, 0, 255);
                rf::gr::line_vec(node.pos, node.exit_pos, no_overdraw_2d_line);
            }
        }
        for (const VehicleWaypointLink& link : node.links) {
            if (link.to < 0 || link.to >= count) {
                continue;
            }
            const rf::Vector3& to = g_layer.nodes[link.to].pos;
            if (!node_in_range && !in_range(to)) {
                continue;
            }
            const rf::Color link_color = type_color(node.type, link.size);
            rf::gr::line_arrow(node.pos.x, node.pos.y, node.pos.z, to.x, to.y, to.z, link_color.red,
                               link_color.green, link_color.blue);
        }
    }
    update_factory_lines();
    rf::gr::set_color(255, 255, 255, 255);
    for (const auto& [factory_pos, node] : g_factory_lines.lines) {
        if (node < count && in_range(factory_pos)) {
            rf::gr::line_vec(factory_pos, g_layer.nodes[node].pos, no_overdraw_2d_line);
        }
    }
}
