#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <utility>
#include <vector>
#include "route_arrows.h"
#include "hud_world.h"
#include "multi_spectate.h"
#include "zone_marker.h"
#include "../graphics/gr.h"
#include "../misc/alpine_settings.h"
#include "../misc/waypoints.h"
#include "../multi/bots/bot_waypoint_route.h"
#include "../multi/demo/demo.h"
#include "../multi/gametype.h"
#include "../multi/multi.h"
#include "../multi/payload.h"
#include "../rf/entity.h"
#include "../rf/gr/gr.h"
#include "../rf/level.h"
#include "../rf/multi.h"
#include "../rf/player/player.h"

namespace
{

constexpr int max_arrows = 60;
constexpr float arrow_spacing = 2.0f;
// Close enough to show at once, then every arrow_spacing
constexpr float first_arrow_offset = 1.0f;
constexpr float route_max_length = max_arrows * arrow_spacing;
// The trail first runs this far the way the player faces, so its start is in view, else short_lead_in_length
constexpr float lead_in_length = 3.0f;
constexpr float short_lead_in_length = 1.5f;
// A route heading more than this far from the lead-in, over turn_back_window of it, is turned into with a U-turn
constexpr float turn_back_angle_deg = 100.0f;
constexpr float turn_back_window = 3.0f;
// Keeps the U-turn's two legs more than an arrow's width apart
constexpr float u_turn_radius = 1.4f;
// The lead-in and the U-turn are checked for floor and clearance this often
constexpr float u_turn_sample_step = 0.75f;
constexpr float lay_arrows_per_s = 11.25f;
constexpr float arrow_drop_s = 0.15f;
constexpr float arrow_drop_height = 0.5f;
// Headings are taken over this much route either side, so the arrows don't snap round where the waypoints wiggle.
// Near the middle of the U-turn's arc that is the arc's tangent; nearer its ends it blends with the straight beside it.
constexpr float arrow_heading_window = 2.0f;
// Corner-cutting passes over each walkable run, each taking at most max_corner_cut off a corner, so the trail stays on
// a narrow walkway
constexpr int route_smoothing_passes = 2;
constexpr float max_corner_cut = 0.5f;

constexpr float arrow_half_width = 0.75f;
constexpr float arrow_half_length = 0.5f;
constexpr float arrow_bar_width = 0.25f;
constexpr float arrow_depth = 0.1875f;
constexpr float arrow_floor_gap = 0.1f;
constexpr float arrow_brightness = 0.8f;
// As the medallion rim
constexpr float arrow_side_brightness = 0.6f;
// An arrow nearer than this to any but the one before it is left out, so a route that doubles back never stacks them
const float arrow_overlap_distance = 2.0f * std::hypot(arrow_half_width, arrow_half_length);

// A trail ends short of its target, the cart or the control point, by an arrow's half length and this
constexpr float target_clearance = 0.3f;

constexpr float start_waypoint_radius = 6.0f;
constexpr float goal_waypoint_radius = 8.0f;
constexpr int max_skipped_start_waypoints = 2;
// Of the waypoints within start_waypoint_radius, the nearest this many are tried for a clear line
constexpr int max_visible_start_candidates = 4;
// Times a spawn prunes the blocked links on its planned route, from a graph loaded for the arrows alone, and plans
// again: once for the route it found, and once more should the new route meet others
constexpr int max_route_prune_passes = 2;
// Corner cutting shortens the route the arrows are laid on, so the links tested for blocking run this much further
constexpr float prune_length_factor = 1.5f;
// ground_arrow probes from this far over the route point, about 1.5 m over the floor as waypoints sit about
// kWaypointGenerateGroundOffset over it, so the probe stays under low ceilings
constexpr float floor_probe_above = 0.5f;
// The furthest under the lower end of an arrow's link that a floor is taken: a step under the floor the route expects
// there
constexpr float floor_probe_below = kWaypointGenerateGroundOffset + kWalkDirectMaxStep;
// Lead-in and U-turn probes start this far over the highest floor a sample may step to
constexpr float floor_probe_margin = 0.1f;
// Spawn points can float a few meters over their floor
constexpr float spawn_floor_probe_below = 5.0f;
static_assert(kWalkDirectMaxStep + floor_probe_margin < kWaypointGenerateMinHeadroom,
              "the floor probe must start under the lowest ceiling a waypoint allows");
static_assert(u_turn_sample_step * kWaypointGenerateMaxInclineRise <= kWalkDirectMaxStep,
              "the walk incline over a sample must not lift the floor probe's start past kWalkDirectMaxStep");

struct RouteArrow
{
    rf::Vector3 pos{}; // on the route until grounded, then the arrow's bottom centre
    rf::Vector3 dir{}; // horizontal
    // The route height at the lower end of the link it lies on, which a ledge drop can put well under pos
    float low_route_y = 0.0f;
    bool grounded = false;
};

struct RouteArrowsState
{
    int entity_handle = -1;
    int64_t spawn_ms = 0; // world_hud_clock_ms
    int count = 0;
    std::array<RouteArrow, max_arrows> arrows{};
};

// The points from the spawn to where the waypoint route takes over, and that route
struct RoutePlan
{
    std::vector<rf::Vector3> lead{};
    std::vector<int> path{};
};

struct RouteLayout
{
    float length = 0.0f; // walked along the route so far, its special steps left out
    float next_arrow = first_arrow_offset;

    bool done(const RouteArrowsState& state) const
    {
        return state.count == max_arrows || next_arrow > route_max_length || length >= route_max_length;
    }
};

RouteArrowsState g_route_arrows;
BotWaypointDistanceField g_route_field;

// The control point `team` heads for from `from`, or null outside KOTH, DC, REV and ESC
const HillInfo* route_target_hill(int team, const rf::Vector3& from)
{
    const bool koth = gt_is_koth();
    const bool dc = gt_is_dc();
    if (!koth && !dc && !gt_is_rev() && !gt_is_esc()) {
        return nullptr;
    }
    // Revolt's points are red's to take, and blue defends the same ones
    const HillOwner own = team_hill_owner(team);
    const HillOwner attacker = gt_is_rev() ? HillOwner::HO_Red : own;
    const HillInfo* best = nullptr;
    float best_dist_sq = std::numeric_limits<float>::max();
    for (HillInfo& hill : g_koth_info.hills) {
        // DC also sends the team to one of its own points the enemy is taking, as its medallion shows
        const bool wanted = koth || control_point_team_can_capture_hill(hill, attacker)
            || (dc && hill.ownership == own && hill.lock_status == HillLockStatus::HLS_Available
                && hill_vis_contested(hill));
        if (!hill.trigger || !wanted) {
            continue;
        }
        const float dist_sq = (koth_hill_icon_pos(hill) - from).len_sq();
        if (dist_sq < best_dist_sq) {
            best = &hill;
            best_dist_sq = dist_sq;
        }
    }
    return best;
}

struct RouteTarget
{
    rf::Vector3 pos{};
    const HillInfo* hill = nullptr; // null for the Payload cart
};

// A player at the route point `p` spans its floor up to about there, so a thin box on the floor counts too
bool route_point_in_hill(const HillInfo& hill, const rf::Vector3& p)
{
    return point_inside_hill_trigger(hill, p)
        || point_inside_hill_trigger(hill, {p.x, p.y - kWaypointGenerateGroundOffset + arrow_floor_gap, p.z});
}

// The waypoint inside the hill nearest its trigger's centre, as its icon can float nearer another floor's; 0 with none
int hill_goal_waypoint(const HillInfo& hill)
{
    int best = 0;
    float best_dist_sq = std::numeric_limits<float>::max();
    for (int i = 1; i < waypoints_count(); ++i) {
        rf::Vector3 pos{};
        bool temporary = false;
        if (!waypoints_get_pos(i, pos) || !waypoints_get_temporary(i, temporary) || temporary
            || !route_point_in_hill(hill, pos)) {
            continue;
        }
        const float dist_sq = (pos - hill.trigger->pos).len_sq();
        if (dist_sq < best_dist_sq) {
            best = i;
            best_dist_sq = dist_sq;
        }
    }
    return best;
}

// Where `team` heads from `from`, chosen once per spawn
std::optional<RouteTarget> route_target(int team, const rf::Vector3& from)
{
    if (gt_is_payload()) {
        if (const auto center = payload_cart_center()) {
            return RouteTarget{*center};
        }
        return std::nullopt;
    }
    if (const HillInfo* hill = route_target_hill(team, from)) {
        return RouteTarget{koth_hill_icon_pos(*hill), hill};
    }
    return std::nullopt;
}

// Ladders, jump pads, teleporters, lifts, zip lines, swims, jumps and falls are steps the arrows leave out
bool route_link_is_walk(int from, int to)
{
    int from_type = 0;
    int to_type = 0;
    int subtype = 0;
    int from_movement = 0;
    int to_movement = 0;
    rf::Vector3 from_pos{};
    rf::Vector3 to_pos{};
    if (!waypoints_get_type_subtype(from, from_type, subtype) || !waypoints_get_type_subtype(to, to_type, subtype)
        || !waypoints_get_movement_subtype(from, from_movement) || !waypoints_get_movement_subtype(to, to_movement)
        || !waypoints_get_pos(from, from_pos) || !waypoints_get_pos(to, to_pos)) {
        return false;
    }
    const auto from_kind = static_cast<WaypointType>(from_type);
    const auto to_kind = static_cast<WaypointType>(to_type);
    const auto airborne_or_swimming = [](int movement) {
        return movement == static_cast<int>(WaypointDroppedSubtype::falling)
            || movement == static_cast<int>(WaypointDroppedSubtype::swimming);
    };
    if ((from_kind == WaypointType::ladder && to_kind == WaypointType::ladder) || from_kind == WaypointType::jump_pad
        || from_kind == WaypointType::tele_entrance || from_kind == WaypointType::lift_entrance
        || from_kind == WaypointType::lift_body || from_kind == WaypointType::water || to_kind == WaypointType::water
        || airborne_or_swimming(from_movement) || airborne_or_swimming(to_movement)
        || waypoints_get_zipline_ride(from, to, nullptr, nullptr, nullptr)
        || waypoints_link_has_target_type(from, to, WaypointTargetType::jump)) {
        return false;
    }
    // A drop steeper than the walk incline is a fall
    const float horizontal = std::hypot(to_pos.x - from_pos.x, to_pos.z - from_pos.z);
    return from_pos.y - to_pos.y <= std::max(kRouteMaxUpwardStepHeight, horizontal * kWaypointGenerateMaxInclineRise);
}

// One corner-cutting pass over a polyline, its ends kept. low_y carries, for each point, the lowest route height of the
// links it came from.
void cut_corners(std::vector<rf::Vector3>& points, std::vector<float>& low_y)
{
    std::vector<rf::Vector3> out;
    std::vector<float> out_low;
    out.reserve(points.size() * 2);
    out_low.reserve(points.size() * 2);
    out.push_back(points.front());
    out_low.push_back(low_y.front());
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const rf::Vector3 step = points[i + 1] - points[i];
        const float length = step.len();
        const float cut = length > max_corner_cut * 4.0f ? max_corner_cut / length : 0.25f;
        const float low = std::min(low_y[i], low_y[i + 1]);
        if (i > 0) {
            out.push_back(points[i] + step * cut);
            out_low.push_back(low);
        }
        if (i + 2 < points.size()) {
            out.push_back(points[i] + step * (1.0f - cut));
            out_low.push_back(low);
        }
    }
    out.push_back(points.back());
    out_low.push_back(low_y.back());
    points = std::move(out);
    low_y = std::move(out_low);
}

// Arrows every arrow_spacing of the route that falls on this walkable run
void lay_run(const std::vector<rf::Vector3>& waypoint_points, RouteLayout& layout, RouteArrowsState& out)
{
    // So the arrows follow a natural line rather than zig-zag between the waypoints
    std::vector<rf::Vector3> points = waypoint_points;
    std::vector<float> low_y(points.size());
    for (std::size_t i = 0; i < points.size(); ++i) {
        low_y[i] = points[i].y;
    }
    for (int pass = 0; pass < route_smoothing_passes; ++pass) {
        cut_corners(points, low_y);
    }

    std::vector<float> along(points.size(), 0.0f);
    for (std::size_t i = 1; i < points.size(); ++i) {
        along[i] = along[i - 1] + (points[i] - points[i - 1]).len();
    }
    const float run_length = along.back();
    // The end of the segment `s` falls on
    const auto segment_end = [&](float s) {
        const auto next = static_cast<std::size_t>(std::upper_bound(along.begin(), along.end(), s) - along.begin());
        return std::clamp<std::size_t>(next, 1, points.size() - 1);
    };
    const auto point_at = [&](float s) {
        s = std::clamp(s, 0.0f, run_length);
        const std::size_t i = segment_end(s);
        const float segment = along[i] - along[i - 1];
        const float t = segment > 1e-6f ? (s - along[i - 1]) / segment : 0.0f;
        return points[i - 1] + (points[i] - points[i - 1]) * t;
    };

    while (layout.next_arrow < layout.length) {
        layout.next_arrow += arrow_spacing;
    }
    rf::Vector3 heading = out.count > 0 ? out.arrows[out.count - 1].dir : rf::Vector3{0.0f, 0.0f, 1.0f};
    while (!layout.done(out) && layout.next_arrow <= layout.length + run_length) {
        const float s = layout.next_arrow - layout.length;
        rf::Vector3 ahead = point_at(s + arrow_heading_window) - point_at(s - arrow_heading_window);
        ahead.y = 0.0f;
        if (ahead.len_sq() > 1e-6f) {
            ahead.normalize();
            heading = ahead;
        }
        const rf::Vector3 pos = point_at(s);
        const auto overlaps = [&](const RouteArrow& placed) {
            return (placed.pos - pos).len() < arrow_overlap_distance;
        };
        if (out.count < 2 || std::none_of(out.arrows.begin(), out.arrows.begin() + out.count - 1, overlaps)) {
            const std::size_t i = segment_end(s);
            out.arrows[out.count++] = RouteArrow{pos, heading, std::min(low_y[i - 1], low_y[i])};
        }
        layout.next_arrow += arrow_spacing;
    }
    layout.length += run_length;
}

// `flat` at the route's height over the floor below it, or nullopt where that floor rises or drops from the one under
// `prev` by more than kWalkDirectMaxStep or the walk incline. The probe starts just over the highest floor allowed,
// which keeps it under the waypoints' minimum headroom.
std::optional<rf::Vector3> route_point_on_floor(const rf::Vector3& flat, const rf::Vector3& prev)
{
    const float prev_floor = prev.y - kWaypointGenerateGroundOffset;
    const float max_rise =
        std::max(kWalkDirectMaxStep, std::hypot(flat.x - prev.x, flat.z - prev.z) * kWaypointGenerateMaxInclineRise);
    rf::Vector3 floor{};
    if (!trace_ground_below_point({flat.x, prev_floor + max_rise + floor_probe_margin, flat.z},
                                  2.0f * (max_rise + floor_probe_margin), &floor, true)
        || std::abs(floor.y - prev_floor) > max_rise) {
        return std::nullopt;
    }
    return rf::Vector3{flat.x, floor.y + kWaypointGenerateGroundOffset, flat.z};
}

// Appends `flat_at(k / steps)` for k = 1 to steps, each on the floor and in clear sight of the point before it; false
// where one fails
template<typename FlatAt>
bool append_on_floor(int steps, const FlatAt& flat_at, std::vector<rf::Vector3>& out)
{
    for (int k = 1; k <= steps; ++k) {
        const rf::Vector3 prev = out.back();
        const auto point = route_point_on_floor(flat_at(static_cast<float>(k) / static_cast<float>(steps)), prev);
        if (!point || !can_link_waypoints(prev, *point)) {
            return false;
        }
        out.push_back(*point);
    }
    return true;
}

// The nearest waypoint within start_waypoint_radius that `from` has a clear line to, or 0. With `ahead`, one that lies
// that way from `from` is taken first.
int visible_start_waypoint(const rf::Vector3& from, const rf::Vector3& ahead = {})
{
    std::vector<std::pair<float, int>> nearby;
    for (int i = 1; i < waypoints_count(); ++i) {
        rf::Vector3 pos{};
        if (waypoints_get_pos(i, pos)) {
            const float dist_sq = (pos - from).len_sq();
            if (dist_sq <= start_waypoint_radius * start_waypoint_radius) {
                nearby.emplace_back(dist_sq, i);
            }
        }
    }
    std::sort(nearby.begin(), nearby.end());
    const auto nearest_visible = [&](bool ahead_only) {
        int tried = 0;
        for (const auto& [dist_sq, waypoint] : nearby) {
            if (tried == max_visible_start_candidates) {
                break;
            }
            rf::Vector3 pos{};
            if (!waypoints_get_pos(waypoint, pos) || (ahead_only && !((pos - from).dot_prod(ahead) > 0.0f))) {
                continue;
            }
            ++tried;
            if (can_link_waypoints(from, pos)) {
                return waypoint;
            }
        }
        return 0;
    };
    const int preferred = ahead.len_sq() > 0.0f ? nearest_visible(true) : 0;
    return preferred > 0 ? preferred : nearest_visible(false);
}

// Where to start along `path` from `origin`: its nearest waypoint can lie behind `origin`, so a later one `origin`
// sees is taken
std::size_t first_route_waypoint(const rf::Vector3& origin, const std::vector<int>& path)
{
    std::size_t first = 0;
    rf::Vector3 next{};
    while (first + 1 < path.size() && first < max_skipped_start_waypoints
           && route_link_is_walk(path[first], path[first + 1]) && waypoints_get_pos(path[first + 1], next)
           && (next - origin).len() <= kWaypointLinkRadius && can_link_waypoints(origin, next)) {
        ++first;
    }
    return first;
}

// The way from `from` over the route's first turn_back_window, as build_route lays it, horizontal and not normalised
rf::Vector3 route_heading(const rf::Vector3& from, const std::vector<int>& path)
{
    rf::Vector3 at = from;
    float left = turn_back_window;
    for (std::size_t i = first_route_waypoint(from, path); i < path.size(); ++i) {
        rf::Vector3 pos{};
        if (!waypoints_get_pos(path[i], pos)) {
            break;
        }
        const float step = (pos - at).len();
        if (step >= left) {
            at = at + (pos - at) * (left / step);
            break;
        }
        left -= step;
        at = pos;
    }
    rf::Vector3 heading = at - from;
    heading.y = 0.0f;
    return heading;
}

// A half circle from `end` toward `side` that comes back against `facing`, appended to `out`; false where its way is
// blocked or has no floor
bool append_u_turn(const rf::Vector3& end, const rf::Vector3& facing, const rf::Vector3& side,
                   std::vector<rf::Vector3>& out)
{
    constexpr float pi = std::numbers::pi_v<float>;
    const int steps = static_cast<int>(std::ceil(pi * u_turn_radius / u_turn_sample_step));
    const rf::Vector3 center = end + side * u_turn_radius;
    return append_on_floor(steps, [&](float t) {
        return center - side * (u_turn_radius * std::cos(pi * t)) + facing * (u_turn_radius * std::sin(pi * t));
    }, out);
}

// A lead-in `length` long the way the player faces, U-turning where the route heads back; false where none fits.
// `path_from` finds the route from a start waypoint.
template<typename PathFrom>
bool plan_lead_in(const rf::Entity& ep, const rf::Vector3& target, float length, const PathFrom& path_from,
                  RoutePlan& plan)
{
    rf::Vector3 facing{ep.orient.fvec.x, 0.0f, ep.orient.fvec.z};
    // Not past an objective about as near as the lead-in's end
    if (facing.len_sq() < 1e-6f || (target - ep.pos).len() <= length + first_arrow_offset) {
        return false;
    }
    facing.normalize();
    // Sampled from the floor under the spawn, so a pit or wall on the way fails it, not only one at its end
    rf::Vector3 spawn_floor{};
    if (!trace_ground_below_point(ep.pos, spawn_floor_probe_below, &spawn_floor, true)) {
        return false;
    }
    std::vector<rf::Vector3> leg{{ep.pos.x, spawn_floor.y + kWaypointGenerateGroundOffset, ep.pos.z}};
    const int leg_steps = static_cast<int>(std::ceil(length / u_turn_sample_step));
    if (!append_on_floor(leg_steps, [&](float t) { return ep.pos + facing * (length * t); }, leg)) {
        return false;
    }
    const rf::Vector3 end = leg.back();
    std::vector<int> path;
    const int start = visible_start_waypoint(end);
    if (start <= 0 || !path_from(start, path)) {
        return false;
    }
    const rf::Vector3 heading = route_heading(end, path);
    const float heading_len = heading.len();
    const float turn_back_cos = std::cos(turn_back_angle_deg * std::numbers::pi_v<float> / 180.0f);
    if (!(heading_len > 1e-3f) || facing.dot_prod(heading) >= heading_len * turn_back_cos) {
        plan = {std::move(leg), std::move(path)};
        return true;
    }
    // Toward the side the route goes, else the other
    const rf::Vector3 right{facing.z, 0.0f, -facing.x};
    const rf::Vector3 toward = right.dot_prod(heading) >= 0.0f ? right : right * -1.0f;
    for (const rf::Vector3& side : {toward, toward * -1.0f}) {
        std::vector<rf::Vector3> lead = leg;
        if (!append_u_turn(end, facing, side, lead)) {
            continue;
        }
        // Onward from the arc's exit, rather than back along the lead-in
        const int rejoin = visible_start_waypoint(lead.back(), facing * -1.0f);
        if (rejoin > 0 && path_from(rejoin, path)) {
            plan = {std::move(lead), std::move(path)};
            return true;
        }
    }
    return false;
}

void build_route(const rf::Vector3& spawn, const RoutePlan& plan, RouteArrowsState& out)
{
    const std::vector<int>& path = plan.path;
    std::vector<rf::Vector3> nodes(path.size());
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (!waypoints_get_pos(path[i], nodes[i])) {
            return;
        }
    }
    const std::size_t first = first_route_waypoint(plan.lead.empty() ? spawn : plan.lead.back(), path);

    RouteLayout layout;
    std::vector<rf::Vector3> run;
    if (!plan.lead.empty()) {
        run = plan.lead;
    }
    else if (can_link_waypoints(spawn, nodes[first])) {
        run.push_back(spawn);
    }
    run.push_back(nodes[first]);
    for (std::size_t i = first; i + 1 < path.size() && !layout.done(out); ++i) {
        if (route_link_is_walk(path[i], path[i + 1])) {
            run.push_back(nodes[i + 1]);
            continue;
        }
        if (run.size() > 1) {
            lay_run(run, layout, out);
        }
        run.assign(1, nodes[i + 1]);
    }
    if (run.size() > 1 && !layout.done(out)) {
        lay_run(run, layout, out);
    }
}

// The trail ends before the first arrow `reaches` says is at the target, as the route runs on to the waypoint nearest
// its centre
template<typename Reaches>
void end_short_of_target(RouteArrowsState& state, const Reaches& reaches)
{
    for (int i = 0; i < state.count; ++i) {
        if (reaches(state.arrows[i])) {
            state.count = i;
            return;
        }
    }
}

// Short of the Payload cart, or the edge of the control point the trail leads to
void end_short_of_objective(RouteArrowsState& state, const RouteTarget& target)
{
    constexpr float margin = arrow_half_length + target_clearance;
    if (!target.hill) {
        rf::Vector3 lo{};
        rf::Vector3 hi{};
        if (!payload_cart_bounds(lo, hi)) {
            return;
        }
        // By the floor under the arrow, so a route under a raised cart is not cut
        end_short_of_target(state, [&](const RouteArrow& arrow) {
            const rf::Vector3& p = arrow.pos;
            const float floor_y = p.y - kWaypointGenerateGroundOffset;
            return p.x >= lo.x - margin && p.x <= hi.x + margin && p.z >= lo.z - margin && p.z <= hi.z + margin
                && floor_y >= lo.y - margin && floor_y <= hi.y + margin;
        });
        return;
    }
    // At the arrow and at its tip plus the clearance
    end_short_of_target(state, [&](const RouteArrow& arrow) {
        return route_point_in_hill(*target.hill, arrow.pos)
            || route_point_in_hill(*target.hill, arrow.pos + arrow.dir * margin);
    });
}

// Placed when it first shows, which spreads the probes over the lay-down. A floor more than floor_probe_below under the
// lower end of the arrow's link is another level, such as the one under a grating the arrow has come off, and is not
// taken.
void ground_arrow(RouteArrow& arrow)
{
    rf::Vector3 floor{};
    const rf::Vector3 probe_from = arrow.pos + rf::Vector3{0.0f, floor_probe_above, 0.0f};
    const float reach = probe_from.y - (arrow.low_route_y - floor_probe_below);
    const bool found = trace_ground_below_point(probe_from, reach, &floor, true);
    arrow.pos.y = (found ? floor.y : arrow.pos.y - kWaypointGenerateGroundOffset) + arrow_floor_gap;
    arrow.grounded = true;
}

float smoothstep01(float x)
{
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

// A flat chevron pointing along `dir`, its bottom centre at `base`, in the medallions' see-through style
void draw_arrow(int white_bm, const rf::Vector3& base, const rf::Vector3& dir, float scale, const rf::Color& top_color,
                const rf::Color& side_color)
{
    const rf::Vector3 right{dir.z, 0.0f, -dir.x};
    // Each arm is arrow_bar_width across, which sets the notch behind the tip and the arms' inner back corners
    const float notch = arrow_bar_width * std::hypot(arrow_half_width, 2.0f * arrow_half_length) / arrow_half_width;
    const float inner_x = arrow_half_width * (1.0f - notch / (2.0f * arrow_half_length));
    // Clockwise seen from above, x to the right and z forward
    const std::array<std::array<float, 2>, 6> outline{{
        {0.0f, arrow_half_length},
        {arrow_half_width, -arrow_half_length},
        {inner_x, -arrow_half_length},
        {0.0f, arrow_half_length - notch},
        {-inner_x, -arrow_half_length},
        {-arrow_half_width, -arrow_half_length},
    }};
    const auto corner = [&](int i, float height) {
        return base + (right * outline[i][0] + dir * outline[i][1] + rf::Vector3{0.0f, height, 0.0f}) * scale;
    };
    zone_marker_draw_quad(white_bm, {corner(0, arrow_depth), corner(1, arrow_depth), corner(2, arrow_depth),
        corner(3, arrow_depth)}, top_color);
    zone_marker_draw_quad(white_bm, {corner(0, arrow_depth), corner(3, arrow_depth), corner(4, arrow_depth),
        corner(5, arrow_depth)}, top_color);

    // Only the sides toward the eye, as the rim does: additive faces would double up where they overlap
    for (int i = 0; i < static_cast<int>(outline.size()); ++i) {
        const int j = (i + 1) % static_cast<int>(outline.size());
        const rf::Vector3 outward = right * (outline[i][1] - outline[j][1]) + dir * (outline[j][0] - outline[i][0]);
        const rf::Vector3 bottom_i = corner(i, 0.0f);
        const rf::Vector3 bottom_j = corner(j, 0.0f);
        if (outward.dot_prod(rf::gr::eye_pos - (bottom_i + bottom_j) * 0.5f) <= 0.0f) {
            continue;
        }
        zone_marker_draw_quad(white_bm, {corner(i, arrow_depth), corner(j, arrow_depth), bottom_j, bottom_i},
            side_color);
    }
}

// The route from `start` toward the target, with a lead-in where one fits; false without one
bool plan_route(const rf::Entity& ep, const RouteTarget& target, int hill_goal, int start, RoutePlan& plan)
{
    // Every start below walks the goal's shared field, so a spawn costs at most one search for a goal not seen since
    // the graph last changed
    int goal = hill_goal > 0 ? hill_goal : waypoints_closest(target.pos, goal_waypoint_radius);
    if (goal <= 0 || !bot_waypoint_destination_path(start, goal, plan.path)) {
        // Short of the objective, the route ends at the reachable waypoint nearest it, measured from the hill's own
        // floor rather than its icon
        rf::Vector3 aim = target.hill ? target.hill->trigger->pos : target.pos;
        if (hill_goal > 0) {
            waypoints_get_pos(hill_goal, aim);
        }
        bot_waypoint_compute_distance_field(start, {}, g_route_field);
        const std::vector<float>& dist = g_route_field.dist;
        goal = 0;
        float best_dist_sq = std::numeric_limits<float>::max();
        for (int i = 1; i < static_cast<int>(dist.size()); ++i) {
            rf::Vector3 pos{};
            if (!std::isfinite(dist[i]) || !waypoints_get_pos(i, pos)) {
                continue;
            }
            const float dist_sq = (pos - aim).len_sq();
            if (dist_sq < best_dist_sq) {
                goal = i;
                best_dist_sq = dist_sq;
            }
        }
        if (!bot_waypoint_distance_field_path(g_route_field, goal, plan.path)) {
            return false;
        }
    }
    const std::vector<int> spawn_path = plan.path;
    const auto path_from = [&](int from, std::vector<int>& path) {
        if (from == start) {
            path = spawn_path;
            return true;
        }
        return bot_waypoint_destination_path(from, goal, path);
    };
    for (const float length : {lead_in_length, short_lead_in_length}) {
        if (plan_lead_in(ep, target.pos, length, path_from, plan)) {
            break;
        }
    }
    return true;
}

// On a graph loaded for the arrows alone, which skipped sanitize, prunes every link sanitize would drop on the stretch
// of the planned route the arrows can reach. Returns the count pruned; any at all means the route must be planned
// again.
int prune_blocked_route_links(const RoutePlan& plan)
{
    int pruned = 0;
    float walked = 0.0f;
    for (std::size_t i = 0; i + 1 < plan.path.size() && walked <= route_max_length * prune_length_factor; ++i) {
        const int from = plan.path[i];
        const int to = plan.path[i + 1];
        if (waypoints_prune_blocked_link(from, to)) {
            ++pruned;
            continue;
        }
        rf::Vector3 from_pos{};
        rf::Vector3 to_pos{};
        if (route_link_is_walk(from, to) && waypoints_get_pos(from, from_pos) && waypoints_get_pos(to, to_pos)) {
            walked += (to_pos - from_pos).len();
        }
    }
    return pruned;
}

} // namespace

bool route_arrows_wanted_for_level()
{
    return g_alpine_game_config.world_hud_route_arrows && rf::is_multi && !rf::is_dedicated_server
        && !is_headless_mode() && !demo_playback_active()
        && (gt_is_payload() || gt_is_koth() || gt_is_dc() || gt_is_rev() || gt_is_esc());
}

void route_arrows_on_local_spawn()
{
    route_arrows_clear();
    rf::Player* const player = rf::local_player;
    rf::Entity* const ep = player ? rf::entity_from_handle(player->entity_handle) : nullptr;
    // level_load makes a listen host's entity before the new level's hills and waypoints exist and only then sets
    // LEVEL_LOADED; level_init_post runs the spawn hook again for it
    if (!ep || !(rf::level.flags & rf::LEVEL_LOADED) || !route_arrows_wanted_for_level()
        || is_waypoint_local_bot_mode_active()) {
        return;
    }
    const int start = waypoints_closest(ep->pos, start_waypoint_radius);
    const std::optional<RouteTarget> target = start > 0 ? route_target(player->team, ep->pos) : std::nullopt;
    if (!target) {
        return;
    }

    // A prune changes links, never positions, so the hill's goal holds; the route and the fields it came from are
    // planned afresh
    const int hill_goal = target->hill ? hill_goal_waypoint(*target->hill) : 0;
    RoutePlan plan;
    for (int pass = 0;; ++pass) {
        plan = {};
        if (!plan_route(*ep, *target, hill_goal, start, plan)) {
            return;
        }
        if (pass == max_route_prune_passes || prune_blocked_route_links(plan) == 0) {
            break;
        }
    }
    build_route(ep->pos, plan, g_route_arrows);
    end_short_of_objective(g_route_arrows, *target);
    if (g_route_arrows.count > 0) {
        g_route_arrows.entity_handle = ep->handle;
        g_route_arrows.spawn_ms = world_hud_clock_ms();
    }
}

void route_arrows_render()
{
    RouteArrowsState& state = g_route_arrows;
    if (state.count == 0) {
        return;
    }
    const rf::Player* const player = rf::local_player;
    rf::Entity* const ep = player ? rf::entity_from_handle(player->entity_handle) : nullptr;
    const float clock = static_cast<float>(world_hud_clock_ms() - state.spawn_ms) / 1000.0f;
    const float hold = g_alpine_game_config.world_hud_route_arrow_time;
    const float last_gone = (state.count - 1) / lay_arrows_per_s + 2.0f * arrow_drop_s + hold;
    // Death, the next spawn or spectating takes the whole trail at once
    if (!rf::is_multi || !g_alpine_game_config.world_hud_route_arrows || !ep || ep->handle != state.entity_handle
        || rf::entity_is_dying(ep) || multi_spectate_is_spectating() || clock >= last_gone) {
        route_arrows_clear();
        return;
    }
    const int white_bm = world_hud_white_bitmap();
    if (white_bm == -1) {
        return;
    }

    const rf::Color team_color = hud_team_color(player->team);
    const float far_clip = level_projection_far();
    const float reach = std::hypot(arrow_half_width, arrow_half_length) + arrow_depth;
    for (int i = 0; i < state.count; ++i) {
        const float since_start = clock - static_cast<float>(i) / lay_arrows_per_s;
        if (!(since_start > 0.0f)) {
            break;
        }
        RouteArrow& arrow = state.arrows[i];
        if (!arrow.grounded) {
            ground_arrow(arrow);
        }
        // Dropped in, held, then reeled back in the order they landed
        const float landed = smoothstep01(since_start / arrow_drop_s);
        const float reeled = smoothstep01((since_start - arrow_drop_s - hold) / arrow_drop_s);
        const float presence = landed * (1.0f - reeled);
        if (!(presence > 0.01f)) {
            continue;
        }
        rf::Vector3 back = arrow.pos - arrow.dir * arrow_spacing;
        if (i > 0 && (state.arrows[i - 1].pos - arrow.pos).len() < 1.5f * arrow_spacing) {
            back = state.arrows[i - 1].pos;
        }
        rf::Vector3 base = arrow.pos + (back - arrow.pos) * reeled;
        base.y += arrow_drop_height * (1.0f - landed + reeled);

        // Not fogged, so faded by hand, and dropped past the far plane, where it would depth-test against the
        // cleared sky
        const float dist = (base - rf::gr::eye_pos).len();
        if (dist >= far_clip) {
            continue;
        }
        const float fade = gr_fog_fade(dist);
        const rf::Vector3 extent{reach, reach, reach};
        if (!(fade > 0.0f) || rf::gr::cull_bounding_box(base - extent, base + extent)) {
            continue;
        }
        const float brightness = arrow_brightness * presence * fade;
        draw_arrow(white_bm, base, arrow.dir, presence, zone_marker_scaled_color(team_color, brightness),
            zone_marker_scaled_color(team_color, brightness * arrow_side_brightness));
    }
}

void route_arrows_clear()
{
    g_route_arrows = {};
}
