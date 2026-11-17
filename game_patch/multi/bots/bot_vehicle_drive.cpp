#include "bot_vehicle_drive.h"

#include "bot_context.h"
#include "bot_goals.h"
#include "bot_internal.h"
#include "bot_math.h"
#include "bot_personality.h"
#include "bot_vehicle.h"
#include "bot_vehicle_nav.h"
#include "../gametype.h"
#include "../vehicles/vehicle.h"
#include "../vehicles/vehicle_internal.h"
#include "../vehicles/vehicle_physics.h"
#include "../../misc/alpine_settings.h"
#include "../../rf/entity.h"
#include "../../rf/multi.h"
#include "../../rf/player/player.h"
#include <common/utils/list-utils.h>
#include <xlog/xlog.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <utility>
#include <vector>

namespace awh = alpine_waypoint_helper;

namespace
{
constexpr int kPlanIntervalMs = 1000;
constexpr int kRepathIntervalMs = 5000;
constexpr int kRepathRetryMs = 1000;
constexpr int kProbeIntervalMs = 100;
constexpr int kSkipAheadIntervalMs = 500;
constexpr int kMaxRouteFailures = 3;
constexpr int kFailedLinkMs = 30000;
constexpr int kGunnerWaitMs = 2500;
constexpr float kGunnerWaitRadius = 8.0f;
constexpr int kProgressWindowMs = 6000;
constexpr float kProgressMinDistance = 1.5f;
// The way the route leads from where a progress window opens: toward the route this far ahead.
constexpr float kProgressLookahead = 8.0f;
constexpr int kProgressRealigns = 2;
constexpr int kHuntIdleMs = 45000;
constexpr int kHuntBlockedMs = 10000;
constexpr float kHuntRange = 150.0f;
constexpr float kHuntStandoff = 14.0f;
// A hunt's route ends this near the quarry.
constexpr float kHuntEndReach = 2.0f * kHuntStandoff;
constexpr float kPatrolMinDistance = 40.0f;
constexpr float kPatrolMaxDistance = 160.0f;
constexpr float kPatrolEndReach = 12.0f;
// A transport ends within this of the objective, must bring the bot this much nearer, and is not worth it this close.
constexpr float kTransportEndReach = 40.0f;
constexpr float kTransportMinGain = 15.0f;
constexpr float kTransportWalkDistance = 20.0f;
// A transport route check holds this long, for an objective within the first and a hull within the second of where it
// was asked; the newest kTransportCheckSlots answers are kept.
constexpr int kTransportCheckMs = 10000;
constexpr float kTransportCheckObjectiveMove = 8.0f;
constexpr float kTransportCheckHullMove = 4.0f;
constexpr std::size_t kTransportCheckSlots = 16;
constexpr float kArriveRadius = 4.0f;
// A long hull's origin stops this many hull lengths short of a point its nose reaches; a hull held up by another hull
// or a team-mate in the way this near its stop has arrived.
constexpr float kArriveHullLengths = 0.75f;
constexpr float kArriveBlockedSlack = 10.0f;
constexpr float kArriveBlockedCap = 1.5f;
constexpr float kRouteReachRadius = 3.0f;
constexpr float kRoutePassRadius = 12.0f;
constexpr float kRepathDestMove = 8.0f;
constexpr float kRepathDestMoveShare = 0.15f;

// Car law (vphys_car apply_car_controls): the held steer position is an integrator the input winds at gain/s, so the
// law asks for the held position the heading error needs and winds there in one frame where it can.
constexpr float kSteerErrorGain = 1.2f;
// Above apply_car_controls' 0.001 dead band, so a held position does not unwind.
constexpr float kSteerHoldInput = 0.0011f;
// Throttle is a force, so the speed loop never rests at zero throttle (the idle brake) while it wants speed: it
// cruises just past the 0.05 dead band, idle-brakes when a little fast and brakes hard (throttle against the motion)
// when well over.
constexpr float kSpeedGain = 0.35f;
constexpr float kCruiseThrottle = 0.07f;
constexpr float kCoastBand = 1.5f;
constexpr float kIdleBrakeBand = 5.0f;
constexpr float kStopSpeed = 0.5f;
// Pulling away from (nearly) standing: the heavier hulls need more than the cruise trickle to roll at all.
constexpr float kStartSpeed = 1.0f;
constexpr float kStartThrottle = 0.4f;
constexpr float kTurnSlowAngle = 1.6f;
constexpr float kMinSpeedShare = 0.3f;
constexpr float kArrivalBrakeShare = 0.6f;
constexpr float kLookaheadHullLengths = 1.5f;
constexpr float kLookaheadSeconds = 0.6f;
constexpr float kLookaheadMin = 3.0f;
constexpr float kReverseThrottle = -0.7f;
constexpr float kRealignEnterAngle = 100.0f * std::numbers::pi_v<float> / 180.0f;
constexpr float kRealignExitAngle = 45.0f * std::numbers::pi_v<float> / 180.0f;
constexpr float kAsternAngle = 150.0f * std::numbers::pi_v<float> / 180.0f;
constexpr float kRealignMaxSpeed = 1.0f;
constexpr int kReverseMaxMs = 2000;
constexpr int kReverseMinMs = 800;
constexpr int kBlockedMs = 1500;
constexpr float kBlockedSpeed = 0.3f;
constexpr float kBlockedThrottle = 0.5f;

// The avoidance probe reaches further the faster the hull goes: its stopping distance grows with speed.
constexpr float kProbeBase = 4.0f;
constexpr float kProbePerSpeed = 0.7f;
constexpr float kProbeMax = 16.0f;
constexpr float kAvoidShift = 3.0f;
constexpr float kAvoidSideShare = 0.6f;
constexpr float kAvoidMinSpeed = 3.0f;
constexpr float kAvoidClearance = 0.5f;
constexpr float kRoadkillAggression = 1.2f;
constexpr float kRoadkillRange = 25.0f;
constexpr float kRoadkillCone = 20.0f * std::numbers::pi_v<float> / 180.0f;
constexpr float kRoadkillMaxShift = 3.0f;

float heading_error(const rf::Entity& hull, const rf::Vector3& target)
{
    const rf::Vector3 local = target - hull.pos;
    return std::atan2(local.dot_prod(hull.orient.rvec), local.dot_prod(hull.orient.fvec));
}

float hull_length(const rf::Entity& hull)
{
    rf::Vector3 half{};
    vehicle_physics_entity_hull_box(&hull, &half, nullptr);
    return 2.0f * half.z;
}

float arrive_radius(const rf::Entity& hull)
{
    return std::max(kArriveRadius, kArriveHullLengths * hull_length(hull));
}

// The input that moves the held steer position to `target` this frame; a nudge holds it there against the unwind.
float steer_toward(float target, float held, float gain, float dt)
{
    const float delta = target - held;
    float input = gain > 0.0f ? std::clamp(delta / (gain * dt), -1.0f, 1.0f) : 0.0f;
    if (std::fabs(input) <= 0.001f && std::fabs(target) > 0.02f) {
        input = std::copysign(kSteerHoldInput, delta);
    }
    return input;
}

float speed_throttle(float target, float speed)
{
    if (target < kStopSpeed) {
        return 0.0f;
    }
    const float error = target - speed;
    if (error >= 0.0f) {
        const float floor = speed < kStartSpeed && target > kStartSpeed ? kStartThrottle : kCruiseThrottle;
        return std::clamp(kCruiseThrottle + kSpeedGain * error, floor, 1.0f);
    }
    if (error > -kCoastBand) {
        return kCruiseThrottle;
    }
    return error > -kIdleBrakeBand ? 0.0f : -1.0f;
}

// Answers of the transport route check by hull and objective, shared by every bot, for one vehicle nav version.
struct TransportRouteCheck
{
    int hull_handle = -1;
    rf::Vector3 hull_pos{};
    rf::Vector3 objective{};
    bool exists = false;
    rf::Timestamp timer{};
};
std::array<TransportRouteCheck, kTransportCheckSlots> g_transport_checks{};
int g_transport_checks_version = -1;

void start_reverse(BotVehicleDriveState& drive)
{
    drive.reversing = true;
    drive.reverse_side = 0.0f;
    drive.reverse_timer.set(kReverseMaxMs);
    drive.blocked_timer.invalidate();
    ++drive.realign_attempts;
}

bool gunner_seat_free(const rf::Entity& hull)
{
    const int seat = vehicle_firing_seat_index(&hull);
    return seat > 0 && seat < vehicle_seat_count(&hull) && vehicle_seat_leech(&hull, seat) == -1;
}

BotVehicleExitReason choose_destination(BotVehicleDriveState& drive, const rf::Entity& rider, const rf::Entity& hull,
                                        const BotVehicleNavClass& cls, const rf::Entity* enemy)
{
    rf::Vector3 pos{};
    if (bot_goals_transport_objective(rider, pos)) {
        if (flat_vector(pos - hull.pos).len() <= kTransportWalkDistance) {
            return BotVehicleExitReason::arrived;
        }
        bot_vehicle_drive_set_destination(drive, BotVehicleDriveMode::transport, pos, true);
        return BotVehicleExitReason::none;
    }
    if (enemy && !timer_running(drive.hunt_blocked_timer)
        && rf::vec_dist_squared(&enemy->pos, &hull.pos) <= kHuntRange * kHuntRange) {
        bot_vehicle_drive_set_destination(drive, BotVehicleDriveMode::hunt, enemy->pos, false);
        return BotVehicleExitReason::none;
    }
    if (drive.contact_timer.elapsed()) {
        return BotVehicleExitReason::hunt_idle;
    }
    if (drive.mode != BotVehicleDriveMode::patrol || !drive.dest_valid) {
        if (!bot_vehicle_nav_random_ground_point(hull.pos, cls, kPatrolMinDistance, kPatrolMaxDistance, pos)) {
            return BotVehicleExitReason::no_route;
        }
        bot_vehicle_drive_set_destination(drive, BotVehicleDriveMode::patrol, pos, false);
    }
    return BotVehicleExitReason::none;
}

BotVehicleExitReason plan_route(BotVehicleDriveState& drive, const rf::Player& pp, const rf::Entity& hull,
                                const BotVehicleNavClass& cls)
{
    float end_reach = kPatrolEndReach;
    if (drive.mode == BotVehicleDriveMode::transport) {
        end_reach = kTransportEndReach;
    }
    else if (drive.mode == BotVehicleDriveMode::hunt) {
        end_reach = kHuntEndReach;
    }
    BotVehicleRoute route;
    if (!bot_vehicle_nav_plan_ground_route(
            hull.pos, drive.dest, cls, end_reach, bot_vehicle_drive_excluded_links(drive), route)) {
        return bot_vehicle_drive_plan_failed(drive) ? bot_vehicle_drive_give_up_route(drive)
                                                    : BotVehicleExitReason::none;
    }
    if (drive.mode == BotVehicleDriveMode::transport
        && flat_vector(hull.pos - drive.dest).len() - flat_vector(route.points.back() - drive.dest).len()
            < kTransportMinGain) {
        return BotVehicleExitReason::objective_close;
    }
    if (g_alpine_game_config.dbg_bot) {
        xlog::info("[BotVeh] {} drives {} route {} ({} points) to ({:.0f}, {:.0f}, {:.0f})", pp.name,
                   bot_vehicle_drive_mode_name(drive.mode), bot_vehicle_nav_route_kind_name(route.kind),
                   route.points.size(), drive.dest.x, drive.dest.y, drive.dest.z);
    }
    bot_vehicle_drive_commit_route(drive, std::move(route));
    return BotVehicleExitReason::none;
}

// Think-time corrections: geometry, other hulls and team-mates ahead push the steering target aside and cap the
// speed; with nothing in the way an aggressive driver leans toward an enemy on foot.
void probe_ahead(BotVehicleDriveState& drive, const rf::Player& pp, const rf::Entity& hull,
                 const BotVehicleNavClass& cls)
{
    drive.target_shift = {};
    drive.speed_cap = std::numeric_limits<float>::infinity();
    rf::Vector3 dir = flat_vector(hull.orient.fvec);
    if (dir.len_sq() < 0.01f) {
        return;
    }
    dir.normalize_safe();
    const rf::Vector3 side{dir.z, 0.0f, -dir.x};
    const float speed = vehicle_live_hull_velocity(&hull).len();
    const float half_length = 0.5f * hull_length(hull);
    const float reach = std::clamp(kProbeBase + kProbePerSpeed * speed, kProbeBase, kProbeMax) + half_length;
    const float half_width = cls.corridor_radius;

    // The side probes, traced once each when needed. A shift toward an unprobed or shut side stays within the
    // route's slack, the corridor the route was checked for.
    std::optional<bool> side_clear[2];
    const auto probe_side = [&](float sign) {
        std::optional<bool>& known = side_clear[sign > 0.0f ? 0 : 1];
        if (!known) {
            known = bot_vehicle_nav_ground_corridor_clear(
                hull.pos, hull.pos + (dir + side * (sign * kAvoidSideShare)) * reach, half_width);
        }
        return *known;
    };
    const std::size_t k = static_cast<std::size_t>(drive.route_index);
    const float slack = k < drive.route.slack.size() ? drive.route.slack[k] : 0.0f;
    const auto shift_toward = [&](float sign) {
        return side * (sign * (probe_side(sign) ? kAvoidShift : std::min(slack, kAvoidShift)));
    };

    if (!bot_vehicle_nav_ground_corridor_clear(hull.pos, hull.pos + dir * reach, half_width)) {
        const bool right_clear = probe_side(1.0f);
        const bool left_clear = probe_side(-1.0f);
        float sign = right_clear ? 1.0f : -1.0f;
        if (right_clear == left_clear && drive.route_index < static_cast<int>(drive.route.points.size())) {
            sign = flat_vector(drive.route.points[drive.route_index] - hull.pos).dot_prod(side) >= 0.0f ? 1.0f : -1.0f;
        }
        drive.target_shift = side * (sign * kAvoidShift);
        drive.speed_cap = kAvoidMinSpeed + 0.5f * reach;
    }

    for (const int handle : vehicle_hull_handles()) {
        const rf::Entity* const other = vehicle_live_synced_entity(handle);
        if (!other || other == &hull) {
            continue;
        }
        const rf::Vector3 local = flat_vector(other->pos - hull.pos);
        const float ahead = local.dot_prod(dir);
        const float lateral = local.dot_prod(side);
        rf::Vector3 other_half{};
        vehicle_physics_entity_hull_box(other, &other_half, nullptr);
        if (ahead <= 0.0f || ahead > reach + other_half.z
            || std::fabs(lateral) > half_width + std::max(other_half.x, other_half.z) + kAvoidClearance) {
            continue;
        }
        drive.speed_cap = std::min(drive.speed_cap, std::max(ahead - half_length - other_half.z, 0.0f));
        drive.target_shift = shift_toward(lateral > 0.0f ? -1.0f : 1.0f);
    }

    const BotPersonality& personality = get_active_bot_personality();
    const bool roadkill = personality.raw_aggression_bias > kRoadkillAggression && drive.target_shift.len_sq() == 0.0f;
    float roadkill_dist = kRoadkillRange;
    for (const rf::Player& other : SinglyLinkedList{rf::player_list}) {
        const bool friend_ahead = &other != &pp && multi_is_team_game_type() && other.team == pp.team;
        if (!friend_ahead && !(roadkill && bot_vehicle_is_enemy(pp, other))) {
            continue;
        }
        const rf::Entity* const entity = bot_vehicle_on_foot_entity(other);
        if (!entity) {
            continue;
        }
        const rf::Vector3 local = flat_vector(entity->pos - hull.pos);
        const float ahead = local.dot_prod(dir);
        const float lateral = local.dot_prod(side);
        if (friend_ahead) {
            if (ahead > 0.0f && ahead <= reach && std::fabs(lateral) <= half_width + 1.5f) {
                drive.speed_cap = std::min(drive.speed_cap, std::max(ahead - half_length - 1.0f, 0.0f));
                drive.target_shift = shift_toward(lateral > 0.0f ? -1.0f : 1.0f);
            }
            continue;
        }
        const float dist = local.len();
        if (dist > roadkill_dist || ahead <= 0.0f || std::atan2(std::fabs(lateral), ahead) > kRoadkillCone) {
            continue;
        }
        // Never off the corridor the route was checked for.
        float limit = slack;
        if (drive.route.kind == BotVehicleRouteKind::direct
            && bot_vehicle_nav_ground_corridor_clear(hull.pos, entity->pos, half_width)) {
            limit = kRoadkillMaxShift;
        }
        roadkill_dist = dist;
        drive.target_shift = side * std::clamp(lateral, -std::min(limit, kRoadkillMaxShift),
                                               std::min(limit, kRoadkillMaxShift));
    }
}

// Excludes the route link the hull is on for a while (the progress monitor's repath).
void note_failed_link(BotVehicleDriveState& drive)
{
    const std::size_t index = static_cast<std::size_t>(drive.route_index);
    if (index == 0 || index >= drive.route.nodes.size() || drive.route.nodes[index - 1] < 0
        || drive.route.nodes[index] < 0
        || (drive.route.kind == BotVehicleRouteKind::layer && drive.route.nav_version != bot_vehicle_nav_version())) {
        return;
    }
    BotVehicleFailedLink* slot = &drive.failed_links[0];
    for (BotVehicleFailedLink& entry : drive.failed_links) {
        if (!timer_running(entry.timer)) {
            slot = &entry;
            break;
        }
        if (entry.timer.time_until() < slot->timer.time_until()) {
            slot = &entry;
        }
    }
    slot->link = BotVehicleNavLink{drive.route.kind, drive.route.nodes[index - 1], drive.route.nodes[index]};
    slot->nav_version = drive.route.nav_version;
    slot->timer.set(kFailedLinkMs);
}

BotVehicleExitReason monitor_progress(BotVehicleDriveState& drive, const rf::Entity& hull, const bool at_end)
{
    // A reverse runs inside the window: a back-and-forth against something the probes miss covers no net ground. A
    // hunter at its route's end, or near its quarry, circles and turns by design: that is fighting, not being stuck.
    const bool hunting_close = drive.mode == BotVehicleDriveMode::hunt
        && (at_end || (drive.dest - hull.pos).len() <= kHuntEndReach);
    if (drive.hold || hunting_close) {
        drive.progress_timer.invalidate();
        return BotVehicleExitReason::none;
    }
    const int law_reverses = drive.realign_attempts - drive.progress_realign_mark;
    const BotVehicleProgress progress = bot_vehicle_drive_progress_window(
        drive, hull.pos, bot_vehicle_drive_sample_route(drive, hull.pos, 0.0f, true).remaining,
        bot_vehicle_drive_sample_route(drive, hull.pos, kProgressLookahead, true).lookahead, kProgressMinDistance);
    if (progress == BotVehicleProgress::open) {
        return BotVehicleExitReason::none;
    }
    const bool realign = progress == BotVehicleProgress::stalled && drive.progress_realigns < kProgressRealigns;
    if (realign) {
        if (law_reverses > 0) {
            drive.progress_realigns = std::min(drive.progress_realigns + law_reverses, kProgressRealigns);
        }
        else {
            ++drive.progress_realigns;
            start_reverse(drive);
        }
    }
    drive.progress_realign_mark = drive.realign_attempts;
    return progress == BotVehicleProgress::stalled && !realign ? bot_vehicle_drive_repath_or_stuck(drive)
                                                                : BotVehicleExitReason::none;
}
}

void bot_vehicle_drive_set_destination(
    BotVehicleDriveState& drive, const BotVehicleDriveMode mode, const rf::Vector3& pos, const bool stop_at_dest)
{
    if (drive.mode != mode) {
        drive.route_failures = 0;
        drive.route = {};
        drive.repath_timer.invalidate();
    }
    drive.mode = mode;
    drive.dest = pos;
    drive.dest_valid = true;
    drive.stop_at_dest = stop_at_dest;
}

BotVehicleRouteSample bot_vehicle_drive_sample_route(
    const BotVehicleDriveState& drive, const rf::Vector3& pos, const float lookahead, const bool flat)
{
    const auto metric = [flat](const rf::Vector3& v) { return flat ? flat_vector(v) : v; };
    BotVehicleRouteSample out;
    const std::vector<rf::Vector3>& points = drive.route.points;
    const std::size_t first = static_cast<std::size_t>(drive.route_index);
    if (first >= points.size()) {
        out.lookahead = drive.dest;
        out.remaining = metric(drive.dest - pos).len();
        return out;
    }
    out.lookahead = points.back();
    rf::Vector3 at = pos;
    float left = lookahead;
    std::size_t vertex = points.size();
    for (std::size_t k = first; k < points.size(); ++k) {
        const float len = metric(points[k] - at).len();
        if (vertex == points.size()) {
            if (len >= left && len > 0.0f) {
                out.lookahead = at + (points[k] - at) * (left / len);
                vertex = k;
            }
            else {
                left -= len;
            }
        }
        out.remaining += len;
        at = points[k];
    }
    if (vertex + 1 < points.size()) {
        const rf::Vector3 into = metric(points[vertex] - (vertex == first ? pos : points[vertex - 1]));
        const rf::Vector3 out_of = metric(points[vertex + 1] - points[vertex]);
        const float lengths = into.len() * out_of.len();
        if (lengths > 0.0001f && metric(points[vertex] - out.lookahead).len() <= lookahead) {
            out.turn_ahead = std::acos(std::clamp(into.dot_prod(out_of) / lengths, -1.0f, 1.0f));
        }
    }
    return out;
}

void bot_vehicle_drive_advance_route(
    BotVehicleDriveState& drive, const rf::Vector3& pos, const BotVehicleNavClass& cls, const float pass_radius)
{
    const bool flat = cls.layer == awh::Type::ground;
    const auto metric = [flat](const rf::Vector3& v) { return flat ? flat_vector(v) : v; };
    const std::vector<rf::Vector3>& points = drive.route.points;
    const int count = static_cast<int>(points.size());
    while (drive.route_index + 1 < count) {
        const int k = drive.route_index;
        const float dist = metric(points[k] - pos).len();
        const float reach = std::max(drive.route.slack[k], kRouteReachRadius);
        // Past the point along the next leg counts as reached, so a missed point is not driven back to.
        const bool passed =
            dist <= pass_radius && metric(points[k + 1] - points[k]).dot_prod(metric(pos - points[k])) > 0.0f;
        if (dist > reach && !passed) {
            break;
        }
        ++drive.route_index;
    }
    if (drive.route_index + 1 < count && !timer_running(drive.skip_ahead_timer)) {
        drive.skip_ahead_timer.set(kSkipAheadIntervalMs);
        const rf::Vector3& next = points[drive.route_index + 1];
        if (flat ? bot_vehicle_nav_ground_corridor_clear(pos, next, cls.corridor_radius)
                 : bot_vehicle_nav_flight_corridor_clear(pos, next, cls)) {
            ++drive.route_index;
        }
    }
}

bool bot_vehicle_drive_plan_due(BotVehicleDriveState& drive)
{
    if (timer_running(drive.plan_timer) && drive.dest_valid) {
        return false;
    }
    drive.plan_timer.set(kPlanIntervalMs);
    return true;
}

bool bot_vehicle_drive_repath_due(
    const BotVehicleDriveState& drive, const rf::Vector3& pos, const bool flat, const int min_interval_ms)
{
    if (!timer_running(drive.repath_timer)) {
        return true;
    }
    const float dest_dist = (flat ? flat_vector(drive.dest - pos) : drive.dest - pos).len();
    const bool dest_moved =
        (drive.dest - drive.route_dest).len() > std::max(kRepathDestMove, kRepathDestMoveShare * dest_dist);
    return dest_moved && !drive.route.points.empty()
        && drive.repath_timer.time_until() <= kRepathIntervalMs - min_interval_ms;
}

bool bot_vehicle_drive_plan_failed(BotVehicleDriveState& drive)
{
    // A route in hand is driven on: a search that gives up (the terrain search after a few rounds of links found too
    // narrow, a corridor another hull blocks) is no proof there is no way on; the progress monitor catches a route
    // that really is shut.
    if (!drive.route.points.empty()) {
        // Taken as planned for, or the moved destination would ask again every min_interval_ms.
        drive.route_dest = drive.dest;
        drive.repath_timer.set(kRepathIntervalMs);
        return false;
    }
    drive.repath_timer.set(kRepathRetryMs);
    if (++drive.route_failures < kMaxRouteFailures) {
        return false;
    }
    drive.route_failures = 0;
    return true;
}

BotVehicleExitReason bot_vehicle_drive_give_up_route(BotVehicleDriveState& drive)
{
    switch (drive.mode) {
        case BotVehicleDriveMode::hunt:
            bot_vehicle_drive_block_hunt(drive);
            return BotVehicleExitReason::none;
        case BotVehicleDriveMode::patrol:
            drive.dest_valid = false;
            drive.plan_timer.invalidate();
            return BotVehicleExitReason::none;
        default:
            return BotVehicleExitReason::no_route;
    }
}

void bot_vehicle_drive_commit_route(BotVehicleDriveState& drive, BotVehicleRoute&& route)
{
    drive.route = std::move(route);
    drive.route_index = 0;
    drive.route_dest = drive.dest;
    drive.route_failures = 0;
    drive.repath_timer.set(kRepathIntervalMs);
}

void bot_vehicle_drive_block_hunt(BotVehicleDriveState& drive)
{
    drive.hunt_blocked_timer.set(kHuntBlockedMs);
    drive.dest_valid = false;
    drive.plan_timer.invalidate();
}

BotVehicleProgress bot_vehicle_drive_progress_window(
    BotVehicleDriveState& drive, const rf::Vector3& pos, const float remaining, const rf::Vector3& toward,
    const float min_distance)
{
    const auto open_window = [&]() {
        drive.progress_timer.set(kProgressWindowMs);
        drive.progress_start = remaining;
        drive.progress_anchor = pos;
        drive.progress_dir = toward - pos;
        drive.progress_dir.normalize_safe();
    };
    if (!drive.progress_timer.valid()) {
        open_window();
        return BotVehicleProgress::opened;
    }
    if (!drive.progress_timer.elapsed()) {
        return BotVehicleProgress::open;
    }
    // Net gains only, so a realign's shuffle back and forth counts for nothing. A moving destination can recede (or a
    // repath lengthen the route), so ground made along the way the route led counts as well.
    const float progress =
        std::max(drive.progress_start - remaining, (pos - drive.progress_anchor).dot_prod(drive.progress_dir));
    open_window();
    if (progress >= min_distance) {
        drive.progress_realigns = 0;
        drive.progress_repathed = false;
        return BotVehicleProgress::progressing;
    }
    return BotVehicleProgress::stalled;
}

BotVehicleExitReason bot_vehicle_drive_repath_or_stuck(BotVehicleDriveState& drive)
{
    if (drive.progress_repathed) {
        return BotVehicleExitReason::stuck;
    }
    drive.progress_repathed = true;
    note_failed_link(drive);
    drive.route = {};
    drive.repath_timer.invalidate();
    return BotVehicleExitReason::none;
}

std::vector<BotVehicleNavLink> bot_vehicle_drive_excluded_links(const BotVehicleDriveState& drive)
{
    std::vector<BotVehicleNavLink> excluded;
    const int nav_version = bot_vehicle_nav_version();
    for (const BotVehicleFailedLink& entry : drive.failed_links) {
        if (timer_running(entry.timer)
            && (entry.link.kind != BotVehicleRouteKind::layer || entry.nav_version == nav_version)) {
            excluded.push_back(entry.link);
        }
    }
    return excluded;
}

bool bot_vehicle_drive_transport_route_exists(const rf::Entity& hull, const rf::Vector3& objective)
{
    // A new level or vehicle layer (load, generation, edit) leaves every answer stale.
    if (g_transport_checks_version != bot_vehicle_nav_version()) {
        g_transport_checks_version = bot_vehicle_nav_version();
        g_transport_checks.fill(TransportRouteCheck{});
    }
    TransportRouteCheck* slot = &g_transport_checks[0];
    for (TransportRouteCheck& entry : g_transport_checks) {
        const bool live = timer_running(entry.timer);
        if (live && entry.hull_handle == hull.handle
            && (entry.objective - objective).len() <= kTransportCheckObjectiveMove
            && (entry.hull_pos - hull.pos).len() <= kTransportCheckHullMove) {
            return entry.exists;
        }
        if (!live) {
            slot = &entry;
        }
        else if (timer_running(slot->timer) && entry.timer.time_until() < slot->timer.time_until()) {
            slot = &entry;
        }
    }
    BotVehicleNavClass cls{};
    BotVehicleRoute route;
    TransportRouteCheck& check = *slot;
    check.hull_handle = hull.handle;
    check.hull_pos = hull.pos;
    check.objective = objective;
    check.timer.set(kTransportCheckMs);
    check.exists = bot_vehicle_nav_class_for_type(hull.info_index, cls)
        && bot_vehicle_nav_plan_ground_route(hull.pos, objective, cls, kTransportEndReach, {}, route)
        && flat_vector(hull.pos - objective).len() - flat_vector(route.points.back() - objective).len()
            >= kTransportMinGain;
    return check.exists;
}

const char* bot_vehicle_drive_mode_name(const BotVehicleDriveMode mode)
{
    switch (mode) {
        case BotVehicleDriveMode::transport:
            return "transport";
        case BotVehicleDriveMode::hunt:
            return "hunt";
        case BotVehicleDriveMode::patrol:
            return "patrol";
        default:
            return "none";
    }
}

BotVehicleExitReason bot_vehicle_drive_think(
    BotContext& ctx, const rf::Player& pp, const rf::Entity& rider, const rf::Entity& hull, const rf::Entity* enemy,
    const bool enemy_in_sight)
{
    BotVehicleDriveState& drive = ctx.state.vehicle.drive;
    drive.hold = false;
    BotVehicleNavClass cls{};
    if (!bot_vehicle_nav_class_for_type(hull.info_index, cls) || cls.layer != awh::Type::ground) {
        return BotVehicleExitReason::none;
    }
    if (drive.exit_reason != BotVehicleExitReason::none) {
        drive.hold = true;
        return drive.exit_reason;
    }
    if (enemy_in_sight || !drive.contact_timer.valid()) {
        drive.contact_timer.set(kHuntIdleMs);
    }

    if (!drive.gunner_wait_done) {
        if (!drive.gunner_wait_timer.valid() && gunner_seat_free(hull)
            && bot_vehicle_team_mate_on_foot_within(pp, hull.pos, kGunnerWaitRadius, false)) {
            drive.gunner_wait_timer.set(kGunnerWaitMs);
        }
        drive.gunner_wait_done = !timer_running(drive.gunner_wait_timer) || !gunner_seat_free(hull);
        if (!drive.gunner_wait_done) {
            drive.hold = true;
            return BotVehicleExitReason::none;
        }
    }

    if (bot_vehicle_drive_plan_due(drive)) {
        if (const BotVehicleExitReason reason = choose_destination(drive, rider, hull, cls, enemy);
            reason != BotVehicleExitReason::none) {
            return reason;
        }
    }
    if (!drive.dest_valid) {
        drive.hold = true;
        return BotVehicleExitReason::none;
    }
    if (bot_vehicle_drive_repath_due(drive, hull.pos, true, kRepathRetryMs)) {
        if (const BotVehicleExitReason reason = plan_route(drive, pp, hull, cls);
            reason != BotVehicleExitReason::none) {
            return reason;
        }
    }
    if (drive.route.points.empty()) {
        drive.hold = true;
        return BotVehicleExitReason::none;
    }
    bot_vehicle_drive_advance_route(drive, hull.pos, cls, kRoutePassRadius);

    const float end_dist = flat_vector(drive.route.points.back() - hull.pos).len();
    const bool at_end = drive.route_index + 1 >= static_cast<int>(drive.route.points.size())
        && (end_dist <= arrive_radius(hull)
            || (end_dist <= arrive_radius(hull) + kArriveBlockedSlack && drive.speed_cap < kArriveBlockedCap));
    if (at_end) {
        switch (drive.mode) {
            case BotVehicleDriveMode::transport:
                return BotVehicleExitReason::arrived;
            case BotVehicleDriveMode::patrol:
                drive.dest_valid = false;
                break;
            default:
                break;
        }
    }
    // A hull with the driver's gun holds off an enemy and shoots; one without drives through him. A client bot has no
    // driver aim to fire with, so it never holds.
    if (ctx.is_server_bot && drive.mode == BotVehicleDriveMode::hunt && enemy && vehicle_firing_seat_index(&hull) == 0
        && !rf::entity_is_driller(const_cast<rf::Entity*>(&hull)) && enemy_in_sight
        && rf::vec_dist_squared(&enemy->pos, &hull.pos) <= kHuntStandoff * kHuntStandoff) {
        drive.hold = true;
    }

    if (!timer_running(drive.probe_timer)) {
        drive.probe_timer.set(kProbeIntervalMs);
        probe_ahead(drive, pp, hull, cls);
    }
    const int realigns_before = drive.realign_attempts;
    const BotVehicleExitReason reason = monitor_progress(drive, hull, at_end);
    if (g_alpine_game_config.dbg_bot && drive.realign_attempts != realigns_before) {
        xlog::info("[BotVeh] {} realigns hull {} (no progress)", pp.name, hull.handle);
    }
    return reason;
}

void bot_vehicle_drive_step(BotVehicleState& vehicle, const rf::Entity& hull, const float dt)
{
    vehicle.move_cmd = {};
    vehicle.pitch_cmd = 0.0f;
    vehicle.yaw_cmd = 0.0f;
    BotVehicleDriveState& drive = vehicle.drive;
    const int vdc = vehicle_damage_class(&hull);
    VehicleCarTuning tuning{};
    if (vehicle.role != BotVehicleRole::driver || !vehicle_physics_class_car_tuning(vdc, tuning) || !(dt > 0.0f)
        || rf::entity_is_dying(const_cast<rf::Entity*>(&hull))) {
        return;
    }
    if (drive.hold || !drive.dest_valid || drive.route.points.empty()) {
        vehicle.move_cmd.y = 1.0f;
        drive.reversing = false;
        drive.blocked_timer.invalidate();
        return;
    }
    float held = 0.0f;
    vehicle_physics_steer_held(hull.handle, &held);
    const rf::Vector3 vel = vehicle_live_hull_velocity(&hull);
    const float forward_speed = vel.dot_prod(hull.orient.fvec);
    const float ground_speed = std::hypot(forward_speed, vel.dot_prod(hull.orient.rvec));

    const float lookahead =
        std::max({kLookaheadMin, kLookaheadHullLengths * hull_length(hull), kLookaheadSeconds * ground_speed});
    const BotVehicleRouteSample sample = bot_vehicle_drive_sample_route(drive, hull.pos, lookahead, true);
    float error = heading_error(hull, sample.lookahead + drive.target_shift);
    if (std::fabs(error) > kAsternAngle) {
        if (drive.turn_side == 0.0f) {
            drive.turn_side = error > 0.0f ? 1.0f : -1.0f;
        }
        error = drive.turn_side * std::fabs(error);
    }
    else {
        drive.turn_side = 0.0f;
    }

    if (drive.reversing) {
        // A blocked hull may already face its target, so it backs off a little before the heading can end it.
        const bool backed_off = drive.reverse_timer.time_until() <= kReverseMaxMs - kReverseMinMs;
        if (drive.reverse_timer.elapsed() || (backed_off && std::fabs(error) < kRealignExitAngle)) {
            drive.reversing = false;
        }
    }
    else if ((std::fabs(error) > kRealignEnterAngle && std::fabs(forward_speed) < kRealignMaxSpeed)
             || (drive.blocked_timer.valid() && drive.blocked_timer.elapsed())) {
        start_reverse(drive);
    }

    float steer = 0.0f;
    float throttle = 0.0f;
    if (drive.reversing) {
        // Backing up on the opposite lock swings the nose toward the target.
        if (drive.reverse_side == 0.0f) {
            drive.reverse_side = error > 0.0f ? -1.0f : 1.0f;
        }
        steer = steer_toward(drive.reverse_side, held, tuning.steer_input_gain, dt);
        throttle = kReverseThrottle;
        drive.blocked_timer.invalidate();
    }
    else {
        const float lock = std::max(tuning.steer_angle_max / (1.0f + tuning.steer_speed_falloff * ground_speed), 0.01f);
        const float target_held = std::clamp(error / lock * kSteerErrorGain, -1.0f, 1.0f);
        steer = steer_toward(target_held, held, tuning.steer_input_gain, dt);
        const float turn = std::max(std::fabs(error), sample.turn_ahead);
        float target_speed =
            tuning.top_speed * std::clamp(1.0f - turn / kTurnSlowAngle, kMinSpeedShare, 1.0f);
        target_speed = std::min(target_speed, drive.speed_cap);
        if (drive.stop_at_dest) {
            const float stop_dist = std::max(sample.remaining - 0.5f * arrive_radius(hull), 0.0f);
            target_speed =
                std::min(target_speed, std::sqrt(2.0f * kArrivalBrakeShare * tuning.brake_accel * stop_dist));
        }
        throttle = speed_throttle(target_speed, forward_speed);
        if (std::fabs(throttle) > kBlockedThrottle && ground_speed < kBlockedSpeed) {
            if (!drive.blocked_timer.valid()) {
                drive.blocked_timer.set(kBlockedMs);
            }
        }
        else {
            drive.blocked_timer.invalidate();
        }
    }
    // The tuning's signs undo themselves: apply_car_controls multiplies each input by its sign.
    vehicle.move_cmd = rf::Vector3{steer * tuning.yaw_sign, 0.0f, throttle * tuning.thrust_sign};
    vehicle.yaw_cmd = steer * tuning.yaw_sign;
}
