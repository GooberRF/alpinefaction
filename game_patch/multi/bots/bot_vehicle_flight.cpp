#include "bot_vehicle_flight.h"

#include "bot_context.h"
#include "bot_goals.h"
#include "bot_internal.h"
#include "bot_math.h"
#include "bot_personality.h"
#include "bot_vehicle.h"
#include "bot_vehicle_drive.h"
#include "bot_vehicle_nav.h"
#include "../vehicles/vehicle.h"
#include "../vehicles/vehicle_physics.h"
#include "../../graphics/weather.h"
#include "../../misc/alpine_settings.h"
#include "../../misc/level.h"
#include "../../misc/waypoints.h"
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
#include <utility>
#include <vector>

namespace awh = alpine_waypoint_helper;

namespace
{
// A moving destination (a hunted enemy) repaths at most this often.
constexpr int kRepathMinMs = 750;
constexpr int kProbeIntervalMs = 150;
constexpr int kAvoidIntervalMs = 100;
constexpr int kSpotCheckMs = 250;
constexpr int kSpotBlockedCheckMs = 1000;
constexpr float kProgressMinDistance = 2.0f;
constexpr int kUnstickMs = 1500;
constexpr float kUnstickBack = 6.0f;
constexpr float kUnstickClimb = 4.0f;
// With no node of its layer a pilot only hunts what a straight line reaches, so it gives up sooner.
constexpr int kHuntIdleMs = 45000;
constexpr int kHuntIdleNoLayerMs = 20000;
constexpr float kHuntRange = 200.0f;
// A fighter hunts from this far above its target, and climbs this far to wait on station.
constexpr float kHuntHeight = 8.0f;
constexpr float kPatrolMinDistance = 40.0f;
constexpr float kPatrolMaxDistance = 200.0f;
// Air support: with a team objective, patrols keep to the layer nodes around it.
constexpr float kAirSupportRadius = 80.0f;
constexpr float kPatrolEndReach = 12.0f;
constexpr float kHuntEndReach = 60.0f;
constexpr float kTransportEndReach = 1.0f;
constexpr float kLandingEndReach = 12.0f;
// A landing node this near the objective makes a fighter or sub a ride there.
constexpr float kLandingObjectiveReach = 50.0f;
constexpr float kArriveRadius = 4.0f;
constexpr float kPointReachRadius = 3.0f;
constexpr float kRoutePassRadius = 15.0f;

// Flight law (vphys_flight apply_flight_model): rot.y and rot.x are rate commands a stiff servo holds, move.z/move.x
// push along the hull's nose and side, move.y along world up, and hover is free. So: velocity loops on thrust and
// strafe (horizontal) and lift (vertical) under position loops, and rate commands from the heading and nose errors.
constexpr float kCruiseShare = 0.8f;
constexpr float kVelocityGain = 1.5f;
constexpr float kPositionGain = 0.8f;
constexpr float kArrivalAccelShare = 0.4f;
constexpr float kAltitudeGain = 1.0f;
constexpr float kClimbGain = 3.0f;
constexpr float kMaxClimbShare = 0.3f;
constexpr float kMaxDescentShare = 0.25f;
constexpr float kLandingDescent = 3.0f;
constexpr float kAttitudeGain = 3.0f;
constexpr float kMaxRateShare = 0.8f;
constexpr float kMaxAimPitch = 1.0f;
constexpr float kLookaheadMin = 6.0f;
constexpr float kLookaheadSeconds = 0.7f;
constexpr float kTurnSlowAngle = 2.0f;
constexpr float kMinSpeedShare = 0.4f;
// A banked or steep hull still gets a usable share of each horizontal axis.
constexpr float kMinAxisShare = 0.25f;
// Clearance over the floor beyond the hull's standoff; the slack that counts as set down; the speed below which it
// counts as at rest.
constexpr float kFloorClearance = 3.0f;
constexpr float kLandedSlack = 1.5f;
constexpr float kLandedMaxSpeed = 3.0f;
// Down on its spot the hull must hold still (ground speed, sink rate) for this long without a break before the pilot
// gets out, so it does not slide off under him; one that never quite stops (a slope) is let out after the longer wait.
constexpr float kSetDownGroundSpeed = 0.5f;
constexpr float kSetDownSinkSpeed = 1.0f;
constexpr int kSetDownStillMs = 500;
constexpr int kSetDownRestMs = 4000;
// A hull this near the floor beyond its standoff is taking off: it climbs before moving off.
constexpr float kTakeoffHeight = 1.5f;
constexpr float kFloorProbeDepth = 100.0f;
constexpr float kSurfaceMargin = 1.0f;
constexpr float kCeilingMargin = 2.0f;
// Climb fades through the ceiling band; half way into it a hull still has half its lift, plenty to hover.
constexpr float kCeilingBandShare = 0.5f;
// A fighter's pilot only ever drops this far onto a dry floor: fall damage.
constexpr float kSafeDropHeight = 8.0f;
constexpr int kLandingStallMs = 8000;
constexpr float kLandingMinGain = 1.0f;
constexpr int kLandingRetryMs = 15000;
// The descent starts this near the spot and is let go only past the release, so a drift does not flip it.
constexpr float kLandAlignRadius = 2.5f;
constexpr float kLandAlignRelease = 4.0f;
// A fighter sets down where it is over a dry floor this near; farther, it looks for a spot.
constexpr float kLandHereMaxHeight = 60.0f;
// A hull at rest on its physics sphere touches the floor within this of the sphere; the floor probe can miss the face
// it sits on (a slope, an edge), so that contact counts as set down too.
constexpr float kRestContactSlack = 0.3f;
constexpr float kRestNormalY = 0.6f;
// A spot within this of one a landing was given up at counts as the same spot.
constexpr float kFailedSpotRadius = 6.0f;
// Out from under cover before planning again: straight up this far, else sideways this far.
constexpr float kEscapeClimb = 12.0f;
// Above the pad's surroundings first, where corridors to the layer nodes are likelier to clear.
constexpr float kEscapeHighClimb = 30.0f;
constexpr float kEscapeSlide = 15.0f;
constexpr int kEscapeTries = 2;
constexpr int kEscapeHoldMs = 3000;
constexpr float kLandingNodeSearch = 300.0f;
constexpr float kFailingDescent = 8.0f;

// The avoidance probe reaches further the faster the hull goes: its stopping distance grows with speed.
constexpr float kProbeBase = 5.0f;
constexpr float kProbePerSpeed = 0.9f;
constexpr float kProbeMax = 28.0f;
constexpr float kAvoidMinSpeed = 3.0f;
constexpr float kAvoidShift = 8.0f;
constexpr float kAvoidClimb = 6.0f;
constexpr float kAvoidSideShare = 0.8f;
constexpr float kAvoidClimbShare = 0.8f;
constexpr float kAvoidSpeedShare = 0.5f;
constexpr float kAvoidBlockedSpeed = 2.0f;

constexpr float kEngageRangeScale = 1.6f;
constexpr float kMinEngageRange = 30.0f;
constexpr float kStandoffShare = 0.6f;
constexpr float kMinStandoff = 10.0f;
constexpr float kMaxStandoff = 35.0f;
// A hover keeps its own height within this angle off the target, inside the nose's reach (kMaxAimPitch).
constexpr float kHoverMaxPitch = 0.7f;
constexpr float kStrafeRunAggression = 1.3f;
// A pass holds its height over the target in this band; the top keeps the target in the nose's reach at the break.
constexpr float kRunHeight = 6.0f;
constexpr float kRunMaxHeight = 14.0f;
constexpr float kRunBreakDistance = 10.0f;
constexpr float kRunExtendDistance = 40.0f;
constexpr float kRunResetDistance = 35.0f;
constexpr int kRunInMs = 8000;
constexpr int kRunOutMs = 4000;
// A hover whose target takes no damage this long (the guns never line up, or the shots hit cover, foliage or water)
// moves to another bearing once, then gives the hunt up. Lining up is no evidence: only damage is.
constexpr int kNoHitMs = 6000;
constexpr int kNoHitMoves = 1;
constexpr int kNoHitTurn = 3;
constexpr float kHitHealthDrop = 0.5f;
constexpr int kGivenUpTargetMs = 60000;
// Team-mates hunting one target take different bearings, and no spot this near another friendly hull.
constexpr float kHoverBearingStep = 25.0f * std::numbers::pi_v<float> / 180.0f;
constexpr float kHoverSpacing = 8.0f;

constexpr int kAimLogMs = 1000;

const rf::Vector3 kUp{0.0f, 1.0f, 0.0f};

bool flight_class(const rf::Entity& hull, BotVehicleNavClass& cls)
{
    return !vehicle_hull_is_turret(&hull) && bot_vehicle_nav_class_for_type(hull.info_index, cls)
        && cls.layer != awh::Type::ground;
}

bool is_water(const BotVehicleNavClass& cls)
{
    return cls.layer == awh::Type::water;
}

bool given_up_target(const BotVehicleFlightState& flight, const rf::Entity& enemy)
{
    return enemy.handle == flight.given_up_target && timer_running(flight.given_up_target_timer);
}

float hull_standoff(const rf::Entity& hull)
{
    return std::max(vehicle_physics_hull_standoff(&hull), 0.5f);
}

// The nose angle the law holds: onto the aim while it faces the aim, else level.
float aim_pitch_target(const BotVehicleFlightState& flight)
{
    const float flat = flat_vector(flight.aim_dir).len();
    if (!flight.face_aim || !flight.aim_valid || flat < 0.01f) {
        return 0.0f;
    }
    return std::clamp(std::atan2(flight.aim_dir.y, flat), -kMaxAimPitch, kMaxAimPitch);
}

const char* attack_name(BotVehicleFlightAttack attack)
{
    switch (attack) {
        case BotVehicleFlightAttack::hover:
            return "hover";
        case BotVehicleFlightAttack::run_in:
            return "run in";
        case BotVehicleFlightAttack::run_out:
            return "run out";
        default:
            return "none";
    }
}

// The route polyline in 3D: a flyer climbs and dives along it.
BotVehicleRouteSample sample_route(const BotVehicleDriveState& drive, const rf::Vector3& pos, float lookahead)
{
    return bot_vehicle_drive_sample_route(drive, pos, lookahead, false);
}

rf::Vector3 heading_point(const BotVehicleDriveState& drive, const rf::Entity& hull)
{
    const BotVehicleFlightState& flight = drive.flight;
    if (flight.follow_route) {
        const float speed = vehicle_live_hull_velocity(&hull).len();
        return sample_route(drive, hull.pos, std::max(kLookaheadMin, kLookaheadSeconds * speed)).lookahead;
    }
    return flight.point_valid ? flight.point : hull.pos;
}

bool resting_on_floor(const rf::Entity& hull, float standoff, rf::Vector3& out_point)
{
    std::array<VehiclePhysicsContact, 4> contacts{};
    const int count = vehicle_physics_sphere_contacts(hull.pos, standoff + kRestContactSlack, contacts.data(),
                                                      static_cast<int>(contacts.size()));
    for (int i = 0; i < count; ++i) {
        if (contacts[i].normal.y >= kRestNormalY) {
            out_point = contacts[i].point;
            return true;
        }
    }
    return false;
}

void update_probes(BotVehicleDriveState& drive, const rf::Entity& hull, const BotVehicleNavClass& cls,
                   const VehicleFlightTuning& tuning, float standoff)
{
    BotVehicleFlightState& flight = drive.flight;
    if (timer_running(flight.probe_timer)) {
        return;
    }
    flight.probe_timer.set(kProbeIntervalMs);
    const bool water = is_water(cls);
    // A fighter's floor is a liquid surface where there is one: its origin under it drowns the hull.
    const auto floor_below = [&](const rf::Vector3& from, float& out_y, bool& out_wet) {
        rf::Vector3 hit{};
        if (!bot_vehicle_nav_floor_below(from, kFloorProbeDepth, hit)) {
            return false;
        }
        float surface = 0.0f;
        out_wet = !water && bot_vehicle_nav_liquid_surface_at(hit + kUp * 0.1f, surface);
        out_y = out_wet ? std::max(hit.y, surface) : hit.y;
        return true;
    };
    flight.floor_valid = floor_below(hull.pos, flight.floor_y, flight.floor_wet);
    flight.surface_valid = room_liquid_surface_y(hull.room, flight.surface_y);
    // Deep liquid may hide the bed from the probe; its surface is still a fighter's floor.
    if (!water && flight.surface_valid && (!flight.floor_valid || flight.floor_y < flight.surface_y)) {
        flight.floor_valid = true;
        flight.floor_wet = true;
        flight.floor_y = flight.surface_y;
    }
    float floor_top = flight.floor_valid ? flight.floor_y : -std::numeric_limits<float>::infinity();
    rf::Vector3 ahead = heading_point(drive, hull);
    ahead.y = std::max(ahead.y, hull.pos.y);
    float ahead_y = 0.0f;
    bool ahead_wet = false;
    if (floor_below(ahead, ahead_y, ahead_wet)) {
        floor_top = std::max(floor_top, ahead_y);
    }

    float min_alt = floor_top + standoff + kFloorClearance;
    float max_alt = std::numeric_limits<float>::infinity();
    if (water) {
        if (flight.surface_valid) {
            max_alt = flight.surface_y - standoff - kSurfaceMargin;
        }
    }
    else if (const AlpineLevelProperties& props = AlpineLevelProperties::instance();
             props.vehicle_flight_ceiling_enabled) {
        max_alt = props.vehicle_flight_ceiling - kCeilingBandShare * tuning.ceiling_band - kCeilingMargin;
    }
    // A tight band: under the ceiling, or in the water, wins over the floor clearance.
    flight.min_alt = std::min(min_alt, max_alt);
    flight.max_alt = max_alt;
    // A pad the floor probe misses (a prop, or an edge off the line under the origin) still holds a parked hull down.
    rf::Vector3 rest_point{};
    flight.takeoff = !flight.landing
        && ((flight.floor_valid && hull.pos.y - flight.floor_y < standoff + kTakeoffHeight)
            || (vehicle_live_hull_velocity(&hull).len() < kLandedMaxSpeed
                && resting_on_floor(hull, standoff, rest_point)));
}

void hold_here(BotVehicleFlightState& flight, const rf::Entity& hull)
{
    flight.follow_route = false;
    flight.point_valid = true;
    flight.point_pass = false;
    flight.point = hull.pos;
}

// A pilot who boarded for a ride (the seat scored as transport) flies to the landing node by the objective; any other
// gives air support, so a landing node near the objective does not pull every fighter down to it.
BotVehicleExitReason choose_destination(BotVehicleDriveState& drive, const rf::Entity& rider, const rf::Entity& hull,
                                        const BotVehicleNavClass& cls, const rf::Entity* enemy, bool transport_seat)
{
    rf::Vector3 pos{};
    rf::Vector3 objective{};
    const bool has_objective = bot_goals_transport_objective(rider, objective);
    if (has_objective && transport_seat && !drive.flight.transport_given_up) {
        int exit_waypoint = 0;
        const int landing = bot_vehicle_nav_landing_node_near(objective, cls, kLandingObjectiveReach, &exit_waypoint);
        BotVehicleNavNode landing_node{};
        if (landing >= 0 && exit_waypoint > 0 && bot_vehicle_nav_node(landing, landing_node)) {
            bot_vehicle_drive_set_destination(drive, BotVehicleDriveMode::transport, landing_node.pos, true);
            return BotVehicleExitReason::none;
        }
    }
    const bool hunt_blocked = timer_running(drive.hunt_blocked_timer);
    if (enemy && !hunt_blocked && !given_up_target(drive.flight, *enemy)
        && rf::vec_dist_squared(&enemy->pos, &hull.pos) <= kHuntRange * kHuntRange
        && (!is_water(cls) || bot_vehicle_nav_point_in_liquid(enemy->pos))) {
        bot_vehicle_drive_set_destination(drive, BotVehicleDriveMode::hunt,
                                          enemy->pos + kUp * (is_water(cls) ? 0.0f : kHuntHeight), false);
        return BotVehicleExitReason::none;
    }
    if (drive.contact_timer.elapsed()) {
        return BotVehicleExitReason::hunt_idle;
    }
    if (drive.mode != BotVehicleDriveMode::patrol || !drive.dest_valid) {
        const bool found =
            (has_objective && bot_vehicle_nav_random_node(objective, 0.0f, kAirSupportRadius, cls, pos))
            || bot_vehicle_nav_random_node(hull.pos, kPatrolMinDistance, kPatrolMaxDistance, cls, pos);
        // No node to patrol to: wait on station for a target a straight line reaches.
        if (!found) {
            pos = hull.pos + kUp * (is_water(cls) ? 0.0f : kHuntHeight);
        }
        bot_vehicle_drive_set_destination(drive, BotVehicleDriveMode::patrol, pos, !found);
    }
    return BotVehicleExitReason::none;
}

// A hull standing where no route starts (under a roof, in a hangar): straight up when that is open, else out sideways
// to the first open bearing; the think holds the point a while before it plans again.
bool start_escape(BotVehicleFlightState& flight, const rf::Entity& hull, const BotVehicleNavClass& cls)
{
    rf::Vector3 out{};
    bool found = false;
    for (const float climb : {kEscapeHighClimb, kEscapeClimb}) {
        out = hull.pos + kUp * climb;
        found = bot_vehicle_nav_flight_corridor_clear(hull.pos, out, cls);
        if (found) {
            break;
        }
    }
    for (int i = 0; i < 8 && !found; ++i) {
        const float angle = static_cast<float>(i) * std::numbers::pi_v<float> / 4.0f;
        out = hull.pos + rf::Vector3{std::sin(angle), 0.0f, std::cos(angle)} * kEscapeSlide + kUp;
        found = bot_vehicle_nav_flight_corridor_clear(hull.pos + kUp, out, cls);
    }
    if (!found) {
        return false;
    }
    flight.follow_route = false;
    flight.point_valid = true;
    flight.point_pass = false;
    flight.point = out;
    flight.unstick_timer.set(kEscapeHoldMs);
    return true;
}

// Nowhere safe to get out within reach: the pilot keeps the seat and flies on, and waits before trying again.
void abandon_landing(BotVehicleDriveState& drive, const rf::Player& pp, const char* why)
{
    BotVehicleFlightState& flight = drive.flight;
    if (g_alpine_game_config.dbg_bot) {
        xlog::info("[BotVeh] {} gives up landing ({}): {}", pp.name, bot_vehicle_exit_reason_name(drive.exit_reason),
                   why);
    }
    drive.exit_reason = BotVehicleExitReason::none;
    flight.landing_skip_node = flight.landing && flight.landing_to_node;
    flight.transport_given_up =
        flight.transport_given_up || (flight.landing_skip_node && drive.mode == BotVehicleDriveMode::transport);
    if (flight.landing) {
        flight.failed_spot = drive.dest;
        flight.failed_spot_valid = true;
    }
    flight.landing = false;
    flight.landing_descent = false;
    flight.exit_ready = false;
    flight.landing_retry_timer.set(kLandingRetryMs);
    drive.dest_valid = false;
    drive.plan_timer.invalidate();
    drive.route = {};
    drive.repath_timer.invalidate();
    drive.contact_timer.invalidate();
}

BotVehicleExitReason plan_route(BotVehicleDriveState& drive, const rf::Player& pp, const rf::Entity& hull,
                                const BotVehicleNavClass& cls, float standoff)
{
    BotVehicleFlightState& flight = drive.flight;
    float end_reach = kPatrolEndReach;
    if (flight.landing) {
        end_reach = kLandingEndReach;
    }
    else if (drive.mode == BotVehicleDriveMode::transport) {
        end_reach = kTransportEndReach;
    }
    else if (drive.mode == BotVehicleDriveMode::hunt) {
        end_reach = kHuntEndReach;
    }
    // A hull on the floor starts its corridors a standoff up.
    rf::Vector3 start = hull.pos;
    if (flight.takeoff) {
        const rf::Vector3 lifted = hull.pos + kUp * standoff;
        if (bot_vehicle_nav_segment_clear(hull.pos, lifted)) {
            start = lifted;
        }
    }
    BotVehicleRoute route;
    if (!bot_vehicle_nav_plan_flight_route(
            start, drive.dest, cls, end_reach, bot_vehicle_drive_excluded_links(drive), route)) {
        if (g_alpine_game_config.dbg_bot) {
            xlog::info("[BotVeh] {} found no {} route from ({:.0f}, {:.0f}, {:.0f}){}{}", pp.name,
                       bot_vehicle_drive_mode_name(drive.mode), start.x, start.y, start.z,
                       flight.takeoff ? " on the floor" : "", drive.route.points.empty() ? "" : ", flies on");
        }
        // Still on or just off its pad (it lifts off between plans) with no route in hand: out from under cover first.
        const bool low = flight.takeoff || (flight.floor_valid && hull.pos.y < flight.min_alt);
        if (!flight.landing && low && drive.route.points.empty() && flight.escape_tries < kEscapeTries) {
            ++flight.escape_tries;
            if (start_escape(flight, hull, cls)) {
                if (g_alpine_game_config.dbg_bot) {
                    xlog::info("[BotVeh] {} lifts hull {} out to ({:.0f}, {:.0f}, {:.0f}) before planning", pp.name,
                               hull.handle, flight.point.x, flight.point.y, flight.point.z);
                }
                return BotVehicleExitReason::none;
            }
            if (g_alpine_game_config.dbg_bot) {
                xlog::info("[BotVeh] {} cannot lift hull {} out: no clear way up or aside", pp.name, hull.handle);
            }
        }
        if (!bot_vehicle_drive_plan_failed(drive)) {
            return BotVehicleExitReason::none;
        }
        if (flight.landing) {
            abandon_landing(drive, pp, "no route to the spot");
            return BotVehicleExitReason::none;
        }
        return bot_vehicle_drive_give_up_route(drive);
    }
    if (g_alpine_game_config.dbg_bot) {
        xlog::info("[BotVeh] {} flies {}{} route {} ({} points) to ({:.0f}, {:.0f}, {:.0f})", pp.name,
                   bot_vehicle_drive_mode_name(drive.mode), flight.landing ? " (landing)" : "",
                   bot_vehicle_nav_route_kind_name(route.kind), route.points.size(), drive.dest.x, drive.dest.y,
                   drive.dest.z);
    }
    bot_vehicle_drive_commit_route(drive, std::move(route));
    return BotVehicleExitReason::none;
}

void probe_ahead(BotVehicleFlightState& flight, const rf::Entity& hull, const BotVehicleNavClass& cls,
                 const VehicleFlightTuning& tuning, const rf::Vector3& toward)
{
    flight.avoid_shift = {};
    flight.avoid_climb = 0.0f;
    flight.speed_cap = std::numeric_limits<float>::infinity();
    const rf::Vector3 vel = vehicle_live_hull_velocity(&hull);
    const float speed = vel.len();
    rf::Vector3 dir = speed > kAvoidMinSpeed ? vel : toward - hull.pos;
    if (dir.len_sq() < 0.01f) {
        return;
    }
    dir.normalize_safe();
    const float reach = std::clamp(kProbeBase + kProbePerSpeed * speed, kProbeBase, kProbeMax);
    const auto clear = [&](rf::Vector3 d) {
        d.normalize_safe();
        return bot_vehicle_nav_flight_corridor_clear(hull.pos, hull.pos + d * reach, cls);
    };
    if (clear(dir)) {
        return;
    }
    const float cruise = kCruiseShare * tuning.top_speed;
    if (!is_water(cls) && clear(dir + kUp * kAvoidClimbShare)) {
        flight.avoid_climb = kAvoidClimb;
        flight.speed_cap = kAvoidSpeedShare * cruise;
        return;
    }
    rf::Vector3 side = dir.cross(kUp);
    if (side.len_sq() < 1e-4f) {
        side = hull.orient.rvec;
    }
    side.normalize_safe();
    const bool right_clear = clear(dir + side * kAvoidSideShare);
    const bool left_clear = clear(dir - side * kAvoidSideShare);
    if (right_clear || left_clear) {
        float sign = right_clear ? 1.0f : -1.0f;
        if (right_clear && left_clear) {
            sign = (toward - hull.pos).dot_prod(side) >= 0.0f ? 1.0f : -1.0f;
        }
        flight.avoid_shift = side * (sign * kAvoidShift);
        flight.speed_cap = kAvoidSpeedShare * cruise;
        return;
    }
    flight.speed_cap = kAvoidBlockedSpeed;
    flight.avoid_climb = is_water(cls) ? 0.0f : kAvoidClimb;
}

bool strafe_runs()
{
    return get_active_bot_personality().raw_aggression_bias > kStrafeRunAggression;
}

void set_attack(BotVehicleFlightState& flight, const rf::Player& pp, BotVehicleFlightAttack attack)
{
    if (flight.attack != attack && g_alpine_game_config.dbg_bot) {
        xlog::info("[BotVeh] {} attack {}", pp.name, attack_name(attack));
    }
    flight.attack = attack;
}

bool friendly_hull_near(const rf::Player& pp, const rf::Entity& hull, const rf::Vector3& pos, float radius)
{
    for (const rf::Player& other : SinglyLinkedList{rf::player_list}) {
        if (&other == &pp || bot_vehicle_is_enemy(pp, other)) {
            continue;
        }
        const rf::Entity* const other_hull = vehicle_ridden_live_hull(rf::entity_from_handle(other.entity_handle));
        if (other_hull && other_hull != &hull && rf::vec_dist_squared(&other_hull->pos, &pos) <= radius * radius) {
            return true;
        }
    }
    return false;
}

// A standoff spot off the enemy, on a bearing of the pilot's own (so team-mates spread round the target) turned
// further either way when that one is blocked or taken; the hull's own position when none is.
rf::Vector3 hover_spot(const rf::Player& pp, const rf::Entity& hull, const BotVehicleNavClass& cls,
                       const rf::Entity& enemy, float standoff, float height, int turn_steps)
{
    rf::Vector3 away = flat_vector(hull.pos - enemy.pos);
    if (away.len_sq() < 0.25f) {
        away = flat_vector(hull.orient.fvec) * -1.0f;
    }
    away.normalize_safe();
    const int slot = pp.net_data ? pp.net_data->player_id : 0;
    const float base = static_cast<float>(slot % 5 - 2 + turn_steps) * kHoverBearingStep;
    for (const float turn : {0.0f, 2.0f, -2.0f, 4.0f, -4.0f}) {
        const float angle = base + turn * kHoverBearingStep;
        const rf::Vector3 dir{away.x * std::cos(angle) - away.z * std::sin(angle), 0.0f,
                              away.x * std::sin(angle) + away.z * std::cos(angle)};
        const rf::Vector3 spot = enemy.pos + dir * standoff + kUp * height;
        if (!friendly_hull_near(pp, hull, spot, kHoverSpacing)
            && bot_vehicle_nav_flight_corridor_clear(hull.pos, spot, cls)) {
            return spot;
        }
    }
    return hull.pos;
}

// A target's life and armour; a hull's as its riders see it, so a client's view counts too. A rider of an open-seat
// hull stays the target, and shots that land on his hull count as hits too.
float target_health(const rf::Entity& target)
{
    if (vehicle_is_synced_entity_type(&target)) {
        return vehicle_hud_life(&target);
    }
    const rf::Entity* const hull = vehicle_ridden_live_hull(&target);
    return target.life + target.armor + (hull ? vehicle_hud_life(hull) : 0.0f);
}

// While hunting an enemy in sight and in reach: true while engaged, with the point the law flies to and the
// hull turned to the seated aim. Firing is the mounted combat's, off the same aim.
bool update_attack(BotVehicleDriveState& drive, const rf::Player& pp, const rf::Entity& hull,
                   const BotVehicleNavClass& cls, const rf::Entity* enemy, bool enemy_in_sight, bool aligned)
{
    BotVehicleFlightState& flight = drive.flight;
    const bool water = is_water(cls);
    const float range = enemy ? bot_vehicle_mounted_weapon_range(hull, *enemy) : 0.0f;
    const bool engage = drive.mode == BotVehicleDriveMode::hunt && enemy && enemy_in_sight
        && !given_up_target(flight, *enemy)
        && (enemy->pos - hull.pos).len() <= std::max(kEngageRangeScale * range, kMinEngageRange)
        && (!water || bot_vehicle_nav_point_in_liquid(enemy->pos));
    if (!engage) {
        if (flight.attack != BotVehicleFlightAttack::none) {
            set_attack(flight, pp, BotVehicleFlightAttack::none);
            drive.repath_timer.invalidate();
        }
        flight.face_aim = false;
        return false;
    }
    flight.follow_route = false;
    flight.point_valid = true;
    flight.face_aim = true;
    if (water || !strafe_runs()) {
        const float health = target_health(*enemy);
        // A gap in the engagement keeps the bearing count and a running no-hit timer; one that ran out while away
        // starts again.
        if (flight.attack != BotVehicleFlightAttack::hover) {
            flight.spot_timer.invalidate();
            if (flight.no_hit_timer.elapsed()) {
                flight.no_hit_timer.set(kNoHitMs);
            }
        }
        if (flight.hit_target_handle != enemy->handle) {
            flight.no_hit_timer.set(kNoHitMs);
            flight.hit_target_handle = enemy->handle;
            flight.no_hit_moves = 0;
            flight.spot_turn = 0;
        }
        else if (aligned && health < flight.hit_target_health - kHitHealthDrop) {
            flight.no_hit_timer.set(kNoHitMs);
        }
        flight.hit_target_health = health;
        set_attack(flight, pp, BotVehicleFlightAttack::hover);
        if (flight.no_hit_timer.elapsed()) {
            if (flight.no_hit_moves >= kNoHitMoves) {
                if (g_alpine_game_config.dbg_bot) {
                    xlog::info("[BotVeh] {} gives up hovering on {}: no hits", pp.name, enemy->handle);
                }
                set_attack(flight, pp, BotVehicleFlightAttack::none);
                flight.face_aim = false;
                flight.given_up_target = enemy->handle;
                flight.given_up_target_timer.set(kGivenUpTargetMs);
                flight.hit_target_handle = -1;
                bot_vehicle_drive_block_hunt(drive);
                return false;
            }
            ++flight.no_hit_moves;
            flight.spot_turn += kNoHitTurn;
            flight.spot_timer.invalidate();
            flight.no_hit_timer.set(kNoHitMs);
        }
        flight.point_pass = false;
        if (timer_running(flight.spot_timer)) {
            return true;
        }
        const float standoff = std::clamp(kStandoffShare * range, kMinStandoff, kMaxStandoff);
        // The hull holds its height within the nose's reach (sinking to it from higher), and the nose pitches onto the
        // target. A fighter below the target climbs to its level: above it, no ledge hides the target.
        const float reach = standoff * std::tan(kHoverMaxPitch);
        const float height = std::clamp(hull.pos.y - enemy->pos.y, water ? -reach : 0.0f, reach);
        flight.point = hover_spot(pp, hull, cls, *enemy, standoff, height, flight.spot_turn);
        // Every bearing blocked costs five corridors: look again less often.
        flight.spot_timer.set(flight.point == hull.pos ? kSpotBlockedCheckMs : kSpotCheckMs);
        return true;
    }

    const float dist = flat_vector(enemy->pos - hull.pos).len();
    if (flight.attack == BotVehicleFlightAttack::run_out) {
        if (!flight.attack_timer.elapsed() && dist < kRunResetDistance
            && (flight.point - hull.pos).len() > kPointReachRadius) {
            flight.face_aim = false;
            return true;
        }
    }
    else if (flight.attack == BotVehicleFlightAttack::run_in
             && (dist <= kRunBreakDistance || flight.attack_timer.elapsed())) {
        // Over the target: carry on past it, climbing, to turn for the next run.
        rf::Vector3 out_dir = flat_vector(vehicle_live_hull_velocity(&hull));
        if (out_dir.len_sq() < 1.0f) {
            out_dir = flat_vector(hull.orient.fvec);
        }
        out_dir.normalize_safe();
        // Ahead, else back the way it came; in a space too tight for either it turns where it is rather than climb
        // a little more on every run.
        rf::Vector3 out = hull.pos + out_dir * kRunExtendDistance + kUp * kRunHeight;
        if (!bot_vehicle_nav_flight_corridor_clear(hull.pos, out, cls)) {
            out = hull.pos - out_dir * kRunExtendDistance + kUp * kRunHeight;
            if (!bot_vehicle_nav_flight_corridor_clear(hull.pos, out, cls)) {
                out = hull.pos;
            }
        }
        set_attack(flight, pp, BotVehicleFlightAttack::run_out);
        flight.attack_timer.set(kRunOutMs);
        flight.point = out;
        flight.point_pass = true;
        flight.face_aim = false;
        return true;
    }
    if (flight.attack != BotVehicleFlightAttack::run_in) {
        set_attack(flight, pp, BotVehicleFlightAttack::run_in);
        flight.attack_timer.set(kRunInMs);
    }
    // Level at the height it comes in at, within the band, so the nose pitches down onto the target as the pass
    // closes.
    flight.point = enemy->pos + kUp * std::clamp(hull.pos.y - enemy->pos.y, kRunHeight, kRunMaxHeight);
    flight.point_pass = true;
    return true;
}

void log_aim(BotVehicleFlightState& flight, const rf::Player& pp, const rf::Entity& hull, const rf::Entity* enemy,
             bool aligned)
{
    if (!g_alpine_game_config.dbg_bot || timer_running(flight.aim_log_timer)) {
        return;
    }
    flight.aim_log_timer.set(kAimLogMs);
    const bool on_aim = flight.face_aim && flight.aim_valid;
    const rf::Vector3 face = on_aim ? flight.aim_dir : flight.point - hull.pos;
    const rf::Vector3& fwd = hull.orient.fvec;
    const float yaw_error = normalize_angle(std::atan2(face.x, face.z) - std::atan2(fwd.x, fwd.z));
    constexpr float deg = 180.0f / std::numbers::pi_v<float>;
    xlog::info("[BotVeh] {} aims pitch {:.0f}/{:.0f} deg, yaw err {:.0f} deg, aim {} ({}, {:.0f} u over), aligned {}",
               pp.name, aim_pitch_target(flight) * deg, std::asin(std::clamp(fwd.y, -1.0f, 1.0f)) * deg,
               yaw_error * deg, on_aim ? "enemy" : "route", attack_name(flight.attack),
               enemy ? hull.pos.y - enemy->pos.y : 0.0f, aligned);
}

// Where the pilot may get out: low over a dry floor (a fighter; fall damage above that), or at the float line with
// air above (a sub; a flooded room has none).
bool exit_spot_safe(const BotVehicleFlightState& flight, const rf::Entity& hull, const BotVehicleNavClass& cls,
                    float standoff)
{
    if (is_water(cls)) {
        return flight.surface_valid && hull.pos.y >= flight.surface_y - standoff - kSurfaceMargin
            && bot_vehicle_nav_air_above(hull.pos, flight.surface_y);
    }
    return flight.floor_valid && !flight.floor_wet && hull.pos.y - flight.floor_y <= kSafeDropHeight;
}

// Getting out (the drive state holds the reason): the transport's landing node on arrival; a fighter over a dry floor
// near enough (a failing one at any height) sets down where it is; otherwise (fighter) a dry foot node nearer than
// the landing node the hull's nearest node connects to, else that landing node, else (sub) a shore spot; after a
// landing at a layer node was given up, no layer node.
// Nowhere to reach, or no progress toward it: the landing is given up.
void update_landing(BotVehicleDriveState& drive, const rf::Player& pp, const rf::Entity& hull,
                    const BotVehicleNavClass& cls, float standoff)
{
    BotVehicleFlightState& flight = drive.flight;
    const bool water = is_water(cls);
    const float height = flight.floor_valid ? hull.pos.y - flight.floor_y : std::numeric_limits<float>::infinity();
    if (!flight.landing) {
        flight.landing = true;
        flight.landing_descent = false;
        flight.exit_ready = false;
        flight.face_aim = false;
        flight.attack = BotVehicleFlightAttack::none;
        flight.landing_timer.set(kLandingStallMs);
        flight.landing_best = std::numeric_limits<float>::infinity();
        flight.landing_to_node = false;
        flight.descent_cap = kLandingDescent;
        flight.down_timer.invalidate();
        flight.still_timer.invalidate();
        const auto failed_at = [&](const rf::Vector3& pos) {
            return flight.failed_spot_valid && flat_vector(pos - flight.failed_spot).len() <= kFailedSpotRadius;
        };
        const bool dry_here = !water && flight.floor_valid && !flight.floor_wet && !failed_at(hull.pos);
        rf::Vector3 spot = hull.pos;
        bool found = true;
        if (drive.mode == BotVehicleDriveMode::transport && drive.dest_valid
            && flat_vector(drive.dest - hull.pos).len() <= kArriveRadius && !failed_at(drive.dest)) {
            spot = drive.dest;
            flight.landing_to_node = true;
        }
        else if (dry_here && (bot_vehicle_hull_failing(hull) || height <= kLandHereMaxHeight)) {
            if (bot_vehicle_hull_failing(hull)) {
                flight.descent_cap = kFailingDescent;
            }
        }
        else {
            // The landing node is checked over the vehicle graph only (no traces); a fighter takes a dry foot spot
            // nearer than it first, as over a lake the landing node may lie far beyond the shore.
            BotVehicleNavNode landing_node{};
            const int landing = flight.landing_skip_node
                ? -1
                : bot_vehicle_nav_landing_node_near(hull.pos, cls, kLandingNodeSearch);
            std::vector<int> path;
            const bool node_ok = landing >= 0 && bot_vehicle_nav_find_path_near(hull.pos, landing, cls, path)
                && bot_vehicle_nav_node(landing, landing_node) && !failed_at(landing_node.pos);
            const float node_dist = node_ok ? (landing_node.pos - hull.pos).len() : kLandingNodeSearch;
            rf::Vector3 foot{};
            if (!water
                && bot_vehicle_nav_dry_foot_spot(hull.pos, node_dist, cls, standoff + kFloorClearance,
                                                 flight.failed_spot_valid ? &flight.failed_spot : nullptr,
                                                 kFailedSpotRadius, foot)) {
                spot = foot + kUp * (standoff + kFloorClearance);
            }
            else if (node_ok) {
                spot = landing_node.pos;
                flight.landing_to_node = true;
            }
            else if (water) {
                found = flight.surface_valid && bot_vehicle_nav_shore_spot(hull.pos, cls, flight.surface_y, spot)
                    && !failed_at(spot);
            }
            else {
                found = false;
            }
        }
        if (!found) {
            abandon_landing(drive, pp, "nowhere to set down");
            return;
        }
        drive.dest = spot;
        drive.dest_valid = true;
        drive.stop_at_dest = true;
        drive.route = {};
        drive.repath_timer.invalidate();
        drive.progress_timer.invalidate();
        if (g_alpine_game_config.dbg_bot) {
            xlog::info("[BotVeh] {} lands hull {} at ({:.0f}, {:.0f}, {:.0f}): {}", pp.name, hull.handle, spot.x,
                       spot.y, spot.z, bot_vehicle_exit_reason_name(drive.exit_reason));
        }
    }

    const float to_spot = flat_vector(drive.dest - hull.pos).len();
    const bool was_descending = flight.landing_descent;
    flight.landing_descent = to_spot <= (was_descending ? kLandAlignRelease : kLandAlignRadius);
    if (flight.landing_descent != was_descending) {
        // The approach and the descent measure different ways to go: each starts its own stall clock.
        flight.landing_best = std::numeric_limits<float>::infinity();
        flight.landing_timer.set(kLandingStallMs);
    }
    if (flight.landing_descent) {
        hold_here(flight, hull);
        flight.point = drive.dest;
        // A sub asks to rise past the surface: the model floats it at the float line.
        flight.land_alt = water ? (flight.surface_valid ? flight.surface_y + standoff : hull.pos.y)
                                : (flight.floor_valid ? flight.floor_y + standoff : hull.pos.y - standoff);
    }
    const rf::Vector3 vel = vehicle_live_hull_velocity(&hull);
    const float speed = vel.len();
    rf::Vector3 rest_point{};
    const bool resting = !water && speed < kLandedMaxSpeed && resting_on_floor(hull, standoff, rest_point);
    const bool down = flight.landing_descent && speed < kLandedMaxSpeed
        && (water || height <= standoff + kLandedSlack || resting);
    const bool still = flat_vector(vel).len() < kSetDownGroundSpeed && std::fabs(vel.y) < kSetDownSinkSpeed;
    if (!down) {
        flight.down_timer.invalidate();
    }
    else if (!flight.down_timer.valid()) {
        flight.down_timer.set(0);
    }
    if (!(down && still)) {
        flight.still_timer.invalidate();
    }
    else if (!flight.still_timer.valid()) {
        flight.still_timer.set(0);
    }
    const bool still_long = flight.still_timer.valid() && flight.still_timer.time_since() >= kSetDownStillMs;
    const bool down_long = flight.down_timer.valid() && flight.down_timer.time_since() >= kSetDownRestMs;
    const bool settled = down && (still_long || down_long);
    const bool safe = exit_spot_safe(flight, hull, cls, standoff)
        || (resting && !bot_vehicle_nav_point_in_liquid(rest_point + kUp * 0.1f));
    const bool ready = (settled || bot_vehicle_hull_failing(hull)) && safe;
    if (ready && !flight.exit_ready && g_alpine_game_config.dbg_bot) {
        xlog::info("[BotVeh] {} set down hull {} (height {:.1f}, speed {:.1f}, resting {})", pp.name, hull.handle,
                   height, speed, resting);
    }
    flight.exit_ready = ready;
    if (ready) {
        return;
    }
    // Progress: the route left to the spot (a detour on a layer route is no stall), then the drop left.
    float left = std::fabs(hull.pos.y - flight.land_alt);
    if (!flight.landing_descent) {
        left = flight.follow_route ? sample_route(drive, hull.pos, 0.0f).remaining : to_spot;
    }
    if (left < flight.landing_best - kLandingMinGain) {
        flight.landing_best = left;
        flight.landing_timer.set(kLandingStallMs);
    }
    else if (flight.landing_timer.elapsed()) {
        if (g_alpine_game_config.dbg_bot) {
            xlog::info("[BotVeh] {} landing stalled: height {:.1f} (floor {}, wet {}), resting {}, left {:.1f}",
                       pp.name, height, flight.floor_valid, flight.floor_wet, resting, left);
        }
        abandon_landing(drive, pp, "no progress");
    }
}

void start_unstick(BotVehicleFlightState& flight, const rf::Entity& hull, const BotVehicleNavClass& cls,
                   const rf::Vector3& toward)
{
    rf::Vector3 back = flat_vector(hull.pos - toward);
    if (back.len_sq() < 0.01f) {
        back = flat_vector(hull.orient.fvec) * -1.0f;
    }
    back.normalize_safe();
    hold_here(flight, hull);
    flight.point = hull.pos + back * kUnstickBack + kUp * (is_water(cls) ? 0.0f : kUnstickClimb);
    flight.unstick_timer.set(kUnstickMs);
}

BotVehicleExitReason monitor_progress(BotVehicleDriveState& drive, const rf::Entity& hull,
                                      const BotVehicleNavClass& cls, bool at_end)
{
    BotVehicleFlightState& flight = drive.flight;
    // Waiting at the end of the route (on station) is not being stuck.
    if (!flight.follow_route || drive.route.points.empty() || at_end) {
        drive.progress_timer.invalidate();
        return BotVehicleExitReason::none;
    }
    const BotVehicleProgress progress = bot_vehicle_drive_progress_window(
        drive, hull.pos, sample_route(drive, hull.pos, 0.0f).remaining, heading_point(drive, hull),
        kProgressMinDistance);
    if (progress != BotVehicleProgress::stalled) {
        return BotVehicleExitReason::none;
    }
    if (drive.progress_realigns < 1) {
        ++drive.progress_realigns;
        start_unstick(flight, hull, cls, heading_point(drive, hull));
        return BotVehicleExitReason::none;
    }
    return bot_vehicle_drive_repath_or_stuck(drive);
}
}

bool bot_vehicle_flight_hull(const rf::Entity& hull)
{
    BotVehicleNavClass cls{};
    return flight_class(hull, cls);
}

bool bot_vehicle_flight_transport_estimate(
    const rf::Entity& hull, const rf::Vector3& objective, float& out_flight_dist, float& out_exit_walk)
{
    BotVehicleNavClass cls{};
    int exit_waypoint = 0;
    const int landing = flight_class(hull, cls)
        ? bot_vehicle_nav_landing_node_near(objective, cls, kLandingObjectiveReach, &exit_waypoint)
        : -1;
    rf::Vector3 exit_pos{};
    if (landing < 0 || exit_waypoint <= 0 || !waypoints_get_pos(exit_waypoint, exit_pos)) {
        return false;
    }
    // The vehicle graph alone (no traces): the think finds the real route, and gives the ride up if there is none.
    std::vector<int> path;
    BotVehicleNavNode at{};
    if (!bot_vehicle_nav_find_path_near(hull.pos, landing, cls, path) || !bot_vehicle_nav_node(path.front(), at)) {
        return false;
    }
    out_flight_dist = (at.pos - hull.pos).len();
    for (const int node : path) {
        BotVehicleNavNode next{};
        if (bot_vehicle_nav_node(node, next)) {
            out_flight_dist += (next.pos - at.pos).len();
            at = next;
        }
    }
    out_exit_walk = (objective - exit_pos).len();
    return true;
}

bool bot_vehicle_flight_hunt_worthwhile(const rf::Player& pp, const rf::Entity& hull)
{
    BotVehicleNavClass cls{};
    if (!flight_class(hull, cls)) {
        return false;
    }
    const bool water = is_water(cls);
    // Air nodes let a fighter patrol and give air support, so it is always worth one there. With none it only hunts
    // what a straight line from its hull reaches now (its nearest enemy); a sub hunts only in the water.
    if (!water && bot_vehicle_nav_layer_node_count(cls) > 0) {
        return true;
    }
    const rf::Entity* nearest = nullptr;
    float nearest_dist_sq = kHuntRange * kHuntRange;
    for (const rf::Player& other : SinglyLinkedList{rf::player_list}) {
        const rf::Entity* const entity =
            bot_vehicle_is_enemy(pp, other) ? rf::entity_from_handle(other.entity_handle) : nullptr;
        if (!entity || rf::entity_is_dying(const_cast<rf::Entity*>(entity))) {
            continue;
        }
        const float dist_sq = rf::vec_dist_squared(&entity->pos, &hull.pos);
        if (water) {
            if (dist_sq <= kHuntRange * kHuntRange && bot_vehicle_nav_point_in_liquid(entity->pos)) {
                return true;
            }
        }
        else if (dist_sq <= nearest_dist_sq) {
            nearest = entity;
            nearest_dist_sq = dist_sq;
        }
    }
    return !water && nearest
        && bot_vehicle_nav_flight_corridor_clear(hull.pos + kUp * hull_standoff(hull),
                                                 nearest->pos + kUp * kHuntHeight, cls);
}

BotVehicleExitReason bot_vehicle_flight_think(
    BotContext& ctx, const rf::Player& pp, const rf::Entity& rider, const rf::Entity& hull, const rf::Entity* enemy,
    const bool enemy_in_sight)
{
    BotVehicleDriveState& drive = ctx.state.vehicle.drive;
    BotVehicleFlightState& flight = drive.flight;
    BotVehicleNavClass cls{};
    VehicleFlightTuning tuning{};
    if (!flight_class(hull, cls) || !vehicle_physics_class_flight_tuning(vehicle_damage_class(&hull), tuning)) {
        return BotVehicleExitReason::none;
    }
    const float standoff = hull_standoff(hull);
    // A failing hull may get out on this think's floor: it must be the floor under it now.
    if (flight.landing && bot_vehicle_hull_failing(hull)) {
        flight.probe_timer.invalidate();
    }
    update_probes(drive, hull, cls, tuning, standoff);

    if (drive.exit_reason != BotVehicleExitReason::none && timer_running(flight.landing_retry_timer)) {
        drive.exit_reason = BotVehicleExitReason::none;
    }
    if (drive.exit_reason != BotVehicleExitReason::none) {
        update_landing(drive, pp, hull, cls, standoff);
    }
    else if (flight.landing) {
        abandon_landing(drive, pp, "the exit was refused");
    }

    if (!flight.landing) {
        if (enemy_in_sight || !drive.contact_timer.valid()) {
            drive.contact_timer.set(bot_vehicle_nav_layer_node_count(cls) > 0 ? kHuntIdleMs : kHuntIdleNoLayerMs);
        }
        if (bot_vehicle_drive_plan_due(drive)) {
            const bool transport_seat = ctx.state.active_goal == BotGoalType::vehicle_acquire;
            if (const BotVehicleExitReason reason = choose_destination(drive, rider, hull, cls, enemy, transport_seat);
                reason != BotVehicleExitReason::none) {
                return reason;
            }
        }
        if (update_attack(drive, pp, hull, cls, enemy, enemy_in_sight, ctx.state.vehicle.aim_aligned)) {
            log_aim(flight, pp, hull, enemy, ctx.state.vehicle.aim_aligned);
            drive.progress_timer.invalidate();
            if (!timer_running(flight.avoid_timer)) {
                flight.avoid_timer.set(kAvoidIntervalMs);
                probe_ahead(flight, hull, cls, tuning, flight.point);
            }
            return BotVehicleExitReason::none;
        }
    }
    else if (flight.exit_ready || flight.landing_descent) {
        flight.avoid_shift = {};
        flight.avoid_climb = 0.0f;
        flight.speed_cap = std::numeric_limits<float>::infinity();
        return drive.exit_reason;
    }

    if (timer_running(flight.unstick_timer)) {
        return flight.landing ? drive.exit_reason : BotVehicleExitReason::none;
    }
    if (!drive.dest_valid) {
        hold_here(flight, hull);
        return flight.landing ? drive.exit_reason : BotVehicleExitReason::none;
    }
    if (bot_vehicle_drive_repath_due(drive, hull.pos, false, kRepathMinMs)) {
        if (const BotVehicleExitReason reason = plan_route(drive, pp, hull, cls, standoff);
            reason != BotVehicleExitReason::none) {
            return reason;
        }
    }
    if (drive.route.points.empty()) {
        hold_here(flight, hull);
        return flight.landing ? drive.exit_reason : BotVehicleExitReason::none;
    }
    flight.follow_route = true;
    flight.point_valid = false;
    bot_vehicle_drive_advance_route(drive, hull.pos, cls, kRoutePassRadius);

    // The law holds the hull inside its altitude band, so an end below the floor clearance (a landing node a helper's
    // height over the floor) is reached over it.
    rf::Vector3 route_end = drive.route.points.back();
    route_end.y = std::clamp(route_end.y, flight.min_alt, flight.max_alt);
    const bool at_end = drive.route_index + 1 >= static_cast<int>(drive.route.points.size())
        && (route_end - hull.pos).len() <= kArriveRadius;
    if (at_end && flight.landing) {
        // A route that ends short of the spot: straight on to it.
        hold_here(flight, hull);
        flight.point = drive.dest;
    }
    else if (at_end) {
        switch (drive.mode) {
            case BotVehicleDriveMode::transport:
                return BotVehicleExitReason::arrived;
            case BotVehicleDriveMode::hunt:
                // Where the target was last known, and not in sight: patrol a while.
                bot_vehicle_drive_block_hunt(drive);
                break;
            case BotVehicleDriveMode::patrol:
                // Waiting on station has no next point to fly to.
                if (!drive.stop_at_dest) {
                    drive.dest_valid = false;
                }
                break;
            default:
                break;
        }
    }

    if (!timer_running(flight.avoid_timer)) {
        flight.avoid_timer.set(kAvoidIntervalMs);
        probe_ahead(flight, hull, cls, tuning, heading_point(drive, hull));
    }
    if (flight.landing) {
        return drive.exit_reason;
    }
    const int realigns_before = drive.progress_realigns;
    const BotVehicleExitReason reason = monitor_progress(drive, hull, cls, at_end);
    if (g_alpine_game_config.dbg_bot && drive.progress_realigns != realigns_before) {
        xlog::info("[BotVeh] {} backs hull {} off (no progress)", pp.name, hull.handle);
    }
    return reason;
}

bool bot_vehicle_flight_exit_ready(const BotVehicleState& vehicle)
{
    return vehicle.drive.flight.landing && vehicle.drive.flight.exit_ready;
}

void bot_vehicle_flight_step(BotVehicleState& vehicle, const rf::Entity& hull, const float dt)
{
    vehicle.move_cmd = {};
    vehicle.pitch_cmd = 0.0f;
    vehicle.yaw_cmd = 0.0f;
    const int vdc = vehicle_damage_class(&hull);
    VehicleFlightTuning tuning{};
    if (vehicle.role != BotVehicleRole::driver || !vehicle_physics_class_flight_tuning(vdc, tuning) || !(dt > 0.0f)
        || rf::entity_is_dying(const_cast<rf::Entity*>(&hull))) {
        return;
    }
    const BotVehicleDriveState& drive = vehicle.drive;
    const BotVehicleFlightState& flight = drive.flight;
    const rf::Vector3 vel = vehicle_live_hull_velocity(&hull);
    const rf::Vector3 vel_flat = flat_vector(vel);
    const float speed = vel.len();

    // Before the first think there is no plan: hold still where it is.
    rf::Vector3 target = hull.pos;
    bool stop = true;
    float remaining = 0.0f;
    if (flight.follow_route) {
        const BotVehicleRouteSample sample =
            sample_route(drive, hull.pos, std::max(kLookaheadMin, kLookaheadSeconds * speed));
        target = sample.lookahead;
        remaining = sample.remaining;
        stop = drive.stop_at_dest;
    }
    else if (flight.point_valid) {
        target = flight.point;
        remaining = flat_vector(flight.point - hull.pos).len();
        stop = !flight.point_pass;
    }
    target += flight.avoid_shift;
    const float alt = flight.landing_descent
        ? flight.land_alt
        : std::clamp(target.y + flight.avoid_climb, flight.min_alt, flight.max_alt);

    const rf::Vector3 to = flat_vector(target - hull.pos);
    const float dist = to.len();
    const rf::Vector3 fwd = hull.orient.fvec;
    const rf::Vector3 right = hull.orient.rvec;

    rf::Vector3 face{};
    if (flight.face_aim && flight.aim_valid) {
        face = flight.aim_dir;
    }
    else if (dist > 1.0f && !flight.landing_descent) {
        face = to;
    }
    const rf::Vector3 face_flat = flat_vector(face);
    float yaw_error = 0.0f;
    if (face_flat.len_sq() > 1e-4f) {
        yaw_error = normalize_angle(std::atan2(face_flat.x, face_flat.z) - std::atan2(fwd.x, fwd.z));
    }
    const float pitch_target = aim_pitch_target(flight);

    float want = std::min(kCruiseShare * tuning.top_speed, flight.speed_cap);
    if (stop) {
        const float brake = kArrivalAccelShare * tuning.thrust_accel;
        want = std::min({want, std::sqrt(2.0f * brake * remaining), kPositionGain * remaining});
    }
    if (!flight.face_aim) {
        want *= std::clamp(1.0f - std::fabs(yaw_error) / kTurnSlowAngle, kMinSpeedShare, 1.0f);
    }
    if (flight.takeoff && alt > hull.pos.y) {
        want = 0.0f;
    }
    const rf::Vector3 want_vel = dist > 0.01f ? to * (want / dist) : rf::Vector3{};

    // Each axis is divided by its horizontal share, so a pitched nose or a banked side still delivers the wanted push.
    const rf::Vector3 accel = (want_vel - vel_flat) * kVelocityGain;
    const rf::Vector3 fwd_flat = flat_vector(fwd);
    const rf::Vector3 right_flat = flat_vector(right);
    const float thrust = std::clamp(
        accel.dot_prod(fwd_flat) / (std::max(tuning.thrust_accel, 0.01f) * std::max(fwd_flat.len_sq(), kMinAxisShare)),
        -1.0f, 1.0f);
    const float strafe = std::clamp(
        accel.dot_prod(right_flat)
            / (std::max(tuning.strafe_accel, 0.01f) * std::max(right_flat.len_sq(), kMinAxisShare)),
        -1.0f, 1.0f);

    // Lift holds the altitude, net of what a pitched nose or a banked side already pushes up or down.
    const float max_descent = flight.landing_descent ? flight.descent_cap : kMaxDescentShare * tuning.lift_accel;
    const float climb =
        std::clamp(kAltitudeGain * (alt - hull.pos.y), -max_descent, kMaxClimbShare * tuning.lift_accel);
    const float accel_up = kClimbGain * (climb - vel.y) - thrust * tuning.thrust_accel * fwd.y
        - strafe * tuning.strafe_accel * right.y;
    const float lift = std::clamp(accel_up / std::max(tuning.lift_accel, 0.01f), -1.0f, 1.0f);

    const float yaw_limit = kMaxRateShare * tuning.yaw_rate;
    const float pitch_limit = kMaxRateShare * tuning.pitch_rate;
    const float yaw_rate = std::clamp(kAttitudeGain * yaw_error, -yaw_limit, yaw_limit);
    const float pitch_now = std::asin(std::clamp(fwd.y, -1.0f, 1.0f));
    const float pitch_rate = std::clamp(kAttitudeGain * (pitch_target - pitch_now), -pitch_limit, pitch_limit);

    // The tuning's signs undo themselves: apply_flight_model multiplies each input by its sign.
    vehicle.move_cmd =
        rf::Vector3{strafe * tuning.strafe_sign, lift * tuning.lift_sign, thrust * tuning.thrust_sign};
    vehicle.pitch_cmd = pitch_rate / std::max(tuning.pitch_rate, 0.01f) * tuning.pitch_sign;
    vehicle.yaw_cmd = yaw_rate / std::max(tuning.yaw_rate, 0.01f) * tuning.yaw_sign;
}
