#include "bot_vehicle.h"

#include "bot_combat.h"
#include "bot_context.h"
#include "bot_fsm.h"
#include "bot_fsm_manager.h"
#include "bot_goals.h"
#include "bot_main.h"
#include "bot_math.h"
#include "bot_memory_manager.h"
#include "bot_navigation.h"
#include "bot_perception_manager.h"
#include "bot_state.h"
#include "bot_utils.h"
#include "bot_vehicle_drive.h"
#include "bot_vehicle_flight.h"
#include "bot_vehicle_nav.h"
#include "bot_waypoint_route.h"
#include "../alpine_packets.h"
#include "../gametype.h"
#include "../server_internal.h"
#include "../vehicles/vehicle.h"
#include "../vehicles/vehicle_internal.h"
#include "../vehicles/vehicle_physics.h"
#include "../../misc/alpine_settings.h"
#include "../../misc/waypoints.h"
#include "../../rf/entity.h"
#include "../../rf/multi.h"
#include "../../rf/os/frametime.h"
#include "../../rf/player/player.h"
#include <common/utils/list-utils.h>
#include <xlog/xlog.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <vector>

namespace
{
constexpr float kBoardRangeMargin = 0.3f;
constexpr int kSeatRequestRetryMs = 250;
constexpr int kSeatInRangeGiveUpMs = 1500;
constexpr int kSeatRefusedCooldownMs = 15000;
// A client bot refused the wheel at a full server cap assumes that cap for this long.
constexpr int kDriverCapGuessMs = 60000;
constexpr int kAcquireTimeoutMs = 30000;
constexpr int kEnemyContactMemoryMs = 20000;
constexpr int kExitRequestRetryMs = 300;
constexpr int kExitGiveUpMs = 2000;
constexpr int kExitCooldownMs = 20000;
// A drive that found no way on keeps the bot off that hull for longer: the level will not change soon.
constexpr int kNoRouteCooldownMs = 60000;
constexpr float kParkedSpeed = 0.5f;
constexpr int kTurretIdleMs = 25000;
constexpr int kTurretNoShotMs = 3000;
constexpr int kDriverGoneMs = 3000;
constexpr int kPassengerStillMs = 5000;
// A gunner in a parked hull gets out after this with nothing in sight, or after kGunnerThreatStillMs of it while a team
// objective is under attack.
constexpr int kGunnerStillMs = 20000;
constexpr int kGunnerThreatStillMs = 8000;
constexpr float kPassengerStillSpeed = 0.5f;
constexpr float kPassengerArrivedDistance = 25.0f;
constexpr float kPassengerDriverMaxDistance = 15.0f;
constexpr float kPassengerObjectiveMinDistance = 60.0f;
constexpr float kLowHullLifeRatio = 0.2f;
constexpr float kTurretObjectiveRadius = 40.0f;
constexpr float kGunnerMaxFootDistance = 30.0f;
constexpr float kFactoryNodeHullRadius = 3.0f;
constexpr float kContentionRadius = 6.0f;
constexpr int kMountedFireReleaseGuardMs = 350;
constexpr float kMountedHullRangeScale = 1.25f;
constexpr float kMountedFallbackRange = 60.0f;
constexpr int kMaxObjectiveAnchors = 8;
// No bot takes the wheel with a human team-mate on foot this near the hull.
constexpr float kHumanDriverRadius = 10.0f;
// Seat 0 for transport: the drive must save this share of the walk (a driller more), judged by a walking pace, a
// boarding and getting-out overhead, the drive stretched over the straight line, and a share of the top speed.
constexpr float kWalkSpeed = 7.0f;
constexpr float kFootRouteStretch = 1.3f;
constexpr float kBoardOverheadSeconds = 4.0f;
constexpr float kTransportMinWalk = 60.0f;
constexpr float kTransportExitWalk = 12.0f;
constexpr float kTransportDriveStretch = 1.3f;
constexpr float kTransportDriveSpeedShare = 0.7f;
constexpr float kTransportMinSaving = 0.25f;
constexpr float kDrillerMinSaving = 0.3f;
// A fighter or sub ride must save the driller's share too, counting its landing.
constexpr float kLandingOverheadSeconds = 5.0f;
constexpr float kHuntMaxFootDistance = 35.0f;
constexpr float kSeatSwapMaxSpeed = 2.0f;
constexpr int kSeatSwapRetryMs = 400;
// A driver gets out only this slow; until then he holds the handbrake.
constexpr float kDriverExitMaxSpeed = 4.0f;
constexpr float kRiderHurtRatio = 0.25f;
// An enemy hull this near and closing faster than a crush is a threat; standing in its path this near (within the
// cone off its heading) the bot steps aside, and foot routes keep off vehicle roads for a while.
constexpr float kHullThreatRange = 25.0f;
constexpr float kHullDodgeRange = 8.0f;
constexpr float kHullDodgeCone = 25.0f * std::numbers::pi_v<float> / 180.0f;
// A hull passing this far over or under the bot (a fighter) cannot run him down.
constexpr float kHullThreatHeight = 4.0f;
constexpr float kRoadAvoidRadius = 4.0f;

BotVehicleRole role_for_seat(const rf::Entity& hull, const int seat)
{
    if (seat == 0) {
        return vehicle_hull_is_turret(&hull) ? BotVehicleRole::turret : BotVehicleRole::driver;
    }
    return seat == vehicle_firing_seat_index(&hull) ? BotVehicleRole::gunner : BotVehicleRole::passenger;
}

const char* role_name(const BotVehicleRole role)
{
    switch (role) {
        case BotVehicleRole::driver:
            return "driver";
        case BotVehicleRole::gunner:
            return "gunner";
        case BotVehicleRole::turret:
            return "turret";
        case BotVehicleRole::passenger:
            return "passenger";
        default:
            return "none";
    }
}

BotFsmState fsm_state_for_role(const BotVehicleRole role)
{
    switch (role) {
        case BotVehicleRole::driver:
            return BotFsmState::vehicle_drive;
        case BotVehicleRole::passenger:
            return BotFsmState::vehicle_ride;
        default:
            return BotFsmState::vehicle_gun;
    }
}

float bias_norm(const float bias)
{
    return std::clamp((bias - 0.25f) / 2.25f, 0.0f, 1.0f);
}

void clear_fire_intent(BotState& state)
{
    state.vehicle.fire_primary = false;
    state.firing.wants_fire = false;
    state.firing.synthetic_primary_fire_down = false;
    state.firing.synthetic_secondary_fire_down = false;
    state.firing.synthetic_primary_fire_just_pressed = false;
    state.firing.synthetic_secondary_fire_just_pressed = false;
}

void report_fire_edge(rf::Player& pp, const int hull_handle, const bool held, bool& reported)
{
    if (held == reported) {
        return;
    }
    vehicle_server_handle_fire_request(&pp, hull_handle, held ? AF_VEHICLE_FIRE_START : AF_VEHICLE_FIRE_STOP, 0);
    reported = held;
}

void request_seat(rf::Player& pp, const rf::Entity& hull, const int seat)
{
    if (seat < 0 || seat >= af_vehicle_state_max_seats) {
        return;
    }
    if (rf::is_server) {
        vehicle_server_handle_use_request(&pp, hull.handle, static_cast<uint8_t>(seat));
    }
    else {
        af_send_vehicle_use_request(hull.server_handle, static_cast<uint8_t>(seat));
    }
}

void request_exit(rf::Player& pp)
{
    if (rf::is_server) {
        vehicle_server_handle_use_request(&pp, -1, 0);
    }
    else {
        af_send_vehicle_use_request(-1, 0);
    }
}

// Hulls a driver got out of for want of a way on, shared by every bot this machine runs, so the next one does not walk
// to the same pad for the same failure.
struct NoRouteHull
{
    int handle = -1;
    rf::Timestamp timer{};
};
std::array<NoRouteHull, 16> g_no_route_hulls{};
// The vehicle nav version the list is for: a new level reuses handles.
int g_no_route_hulls_version = -1;

void forget_no_route_hulls_of_old_level()
{
    if (g_no_route_hulls_version != bot_vehicle_nav_version()) {
        g_no_route_hulls_version = bot_vehicle_nav_version();
        g_no_route_hulls.fill(NoRouteHull{});
    }
}

void note_no_route_hull(int handle)
{
    forget_no_route_hulls_of_old_level();
    NoRouteHull* slot = &g_no_route_hulls[0];
    for (NoRouteHull& entry : g_no_route_hulls) {
        if (entry.handle == handle || !timer_running(entry.timer)) {
            slot = &entry;
            break;
        }
        if (entry.timer.time_until() < slot->timer.time_until()) {
            slot = &entry;
        }
    }
    slot->handle = handle;
    slot->timer.set(kNoRouteCooldownMs);
}

bool no_route_hull(int handle)
{
    forget_no_route_hulls_of_old_level();
    return std::any_of(g_no_route_hulls.begin(), g_no_route_hulls.end(),
                       [&](const NoRouteHull& entry) { return entry.handle == handle && timer_running(entry.timer); });
}

// The server's switch for bots, or for a client bot the courtesy zero affinity the server sends with it.
bool vehicles_enabled_for_active_bot()
{
    if (!vehicle_level_has_factories() || vehicle_hull_handles().empty()
        || get_active_bot_personality().vehicle_affinity_bias <= 0.0f) {
        return false;
    }
    return !rf::is_server || g_alpine_server_config_active_rules.vehicles.bots_use_vehicles;
}

bool driver_is_team_mate(const rf::Player& pp, const rf::Entity& hull)
{
    const rf::Player* const driver = vehicle_seat_occupant_player(&hull, 0);
    return driver && driver != &pp && multi_is_team_game_type() && driver->team == pp.team;
}

bool drivable_hull(const rf::Entity& hull)
{
    BotVehicleNavClass cls{};
    return !vehicle_hull_is_turret(&hull) && bot_vehicle_nav_class_for_type(hull.info_index, cls);
}

// Humans first, asked on every request for the wheel; a bot already driving keeps it. A client bot cannot see the
// server's cap, so it goes by the bot driver count it was last refused at.
bool driver_seat_allowed(const rf::Player& pp, const rf::Entity& hull)
{
    if (!drivable_hull(hull) || bot_personality_has_quirk(BotPersonalityQuirk::never_drives)
        || bot_vehicle_team_mate_on_foot_within(pp, hull.pos, kHumanDriverRadius, true)) {
        return false;
    }
    if (!rf::is_server) {
        const BotVehicleState& vehicle = bot_state().vehicle;
        return !timer_running(vehicle.driver_cap_timer) || vehicle_count_bot_driven_hulls() < vehicle.driver_cap;
    }
    return vehicle_count_bot_driven_hulls() < g_alpine_server_config_active_rules.vehicles.bot_max_drivers;
}

bool seat_usable(const rf::Player& pp, const rf::Entity& hull, const int seat, const BotGoalType goal)
{
    // A hull whose drown or strand clock runs dies with whoever boards it.
    VehicleHazardView hazard{};
    if (seat < 0 || seat >= vehicle_seat_count(&hull) || vehicle_seat_leech(&hull, seat) != -1
        || !vehicle_hull_entry_allowed(&hull, &pp, nullptr, seat)
        || vehicle_hazard_view(const_cast<rf::Entity*>(&hull), hazard)) {
        return false;
    }
    switch (goal) {
        case BotGoalType::vehicle_turret:
            return seat == 0 && vehicle_hull_is_turret(&hull);
        case BotGoalType::vehicle_gun:
            return seat == vehicle_firing_seat_index(&hull) && seat > 0 && driver_is_team_mate(pp, hull);
        case BotGoalType::vehicle_acquire:
            return seat > 0 ? driver_is_team_mate(pp, hull) : driver_seat_allowed(pp, hull);
        case BotGoalType::vehicle_hunt:
            return seat == 0 && driver_seat_allowed(pp, hull);
        default:
            return false;
    }
}

bool on_cooldown(const BotVehicleState& vehicle, const int hull_handle, const int seat)
{
    return std::any_of(vehicle.cooldowns.begin(), vehicle.cooldowns.end(), [&](const BotVehicleHullCooldown& entry) {
        return entry.hull_handle == hull_handle && entry.seat == seat && timer_running(entry.timer);
    });
}

void note_cooldown(BotVehicleState& vehicle, const int hull_handle, const int seat, const int cooldown_ms)
{
    if (hull_handle < 0) {
        return;
    }
    BotVehicleHullCooldown* slot = &vehicle.cooldowns[0];
    for (BotVehicleHullCooldown& entry : vehicle.cooldowns) {
        if ((entry.hull_handle == hull_handle && entry.seat == seat) || !timer_running(entry.timer)) {
            slot = &entry;
            break;
        }
        if (entry.timer.time_until() < slot->timer.time_until()) {
            slot = &entry;
        }
    }
    slot->hull_handle = hull_handle;
    slot->seat = seat;
    slot->timer.set(cooldown_ms);
}

bool team_mate_closer_to_hull(const rf::Player& pp, const rf::Entity& local_entity, const rf::Entity& hull)
{
    const float own_dist = (local_entity.pos - hull.pos).len();
    return bot_vehicle_team_mate_on_foot_within(pp, hull.pos, std::min(kContentionRadius, own_dist), false);
}

int factory_uid_under_hull(const rf::Entity& hull)
{
    for (int i = 0; const AlpineVehicleFactoryInfo* factory = vehicle_factory(i); ++i) {
        if (rf::vec_dist_squared(&factory->pos, &hull.pos) <= kFactoryNodeHullRadius * kFactoryNodeHullRadius) {
            return factory->uid;
        }
    }
    return -1;
}

bool candidate_board_waypoint(const rf::Entity& hull, int& out_waypoint, rf::Vector3& out_pos)
{
    const int factory_uid = factory_uid_under_hull(hull);
    if (factory_uid >= 0 && waypoints_find_vehicle_factory_waypoint(factory_uid, out_waypoint, out_pos)) {
        return true;
    }
    out_waypoint = bot_find_closest_waypoint_with_fallback(hull.pos);
    return out_waypoint > 0 && waypoints_get_pos(out_waypoint, out_pos);
}

// A seat boarded from the foot graph: standing at the boarding waypoint, the bot's eye must be within boarding range
// of the hull's height (it can step in under it, but not climb to a hull on a tower or a pad above the node).
bool boarding_spot_reaches(const rf::Entity& local_entity, const rf::Entity& hull, const rf::Vector3& board_pos)
{
    const float eye_y = board_pos.y + (local_entity.eye_pos.y - local_entity.pos.y);
    return std::fabs(eye_y - hull.pos.y) <= vehicle_board_range(&hull) - kBoardRangeMargin;
}

// The walk to a waypoint, when some route reaches it. The cheap reachability test first, so only reachable seats and
// objectives pay for a route search.
std::optional<float> foot_distance(int start_waypoint, int waypoint)
{
    if (waypoint <= 0 || !bot_waypoint_reachable(start_waypoint, waypoint)) {
        return std::nullopt;
    }
    const float dist = bot_waypoint_destination_distance(start_waypoint, waypoint);
    return std::isfinite(dist) ? std::optional<float>{dist} : std::nullopt;
}

bool objective_threatened(const rf::Entity& local_entity)
{
    std::array<BotObjectiveAnchor, kMaxObjectiveAnchors> anchors{};
    const int count = bot_goals_objective_anchors(local_entity, anchors.data(), static_cast<int>(anchors.size()));
    return std::any_of(anchors.begin(), anchors.begin() + count,
                       [](const BotObjectiveAnchor& anchor) { return anchor.threatened; });
}

float turret_cover_radius(const rf::Entity& hull)
{
    return std::max(kTurretObjectiveRadius, bot_combat_weapon_effective_range(hull.ai.current_primary_weapon));
}

bool objective_anchor_near(
    const rf::Entity& local_entity, const rf::Vector3& pos, const float radius, const bool threatened_only)
{
    std::array<BotObjectiveAnchor, kMaxObjectiveAnchors> anchors{};
    const int count = bot_goals_objective_anchors(local_entity, anchors.data(), static_cast<int>(anchors.size()));
    for (int i = 0; i < count; ++i) {
        if ((!threatened_only || anchors[i].threatened)
            && rf::vec_dist_squared(&anchors[i].pos, &pos) <= radius * radius) {
            return true;
        }
    }
    return false;
}

rf::Vector3 mounted_origin(const BotVehicleRole role, const rf::Entity& rider, const rf::Entity& hull)
{
    return role == BotVehicleRole::turret ? hull.eye_pos : rider.eye_pos;
}

void step_seated_aim(BotContext& ctx, rf::Entity& rider, rf::Entity& hull, const float dt)
{
    BotVehicleState& vehicle = ctx.state.vehicle;
    rf::Vector3 aim_point = vehicle.aim_point;
    if (vehicle.aim_enemy_handle >= 0) {
        rf::Entity* const enemy = rf::entity_from_handle(vehicle.aim_enemy_handle);
        if (!enemy || rf::entity_is_dying(enemy)) {
            vehicle.aim_valid = false;
        }
        else {
            const int weapon = vehicle.role == BotVehicleRole::passenger
                ? rider.ai.current_primary_weapon
                : hull.ai.current_primary_weapon;
            aim_point = bot_combat_enemy_aim_point(
                mounted_origin(vehicle.role, rider, hull), weapon, *enemy, bot_get_skill_factor());
        }
    }
    vehicle.aim_aligned = false;
    if (vehicle.role == BotVehicleRole::driver) {
        vehicle.driver_aim_valid = false;
        vehicle.drive.flight.aim_valid = false;
    }
    rf::Vector3 dir = aim_point - mounted_origin(vehicle.role, rider, hull);
    if (!vehicle.aim_valid || dir.len_sq() < 0.0001f) {
        return;
    }
    dir.normalize_safe();

    const float skill_factor = bot_get_skill_factor();
    if (vehicle.role == BotVehicleRole::driver && bot_vehicle_flight_hull(hull)) {
        // Hull-frame guns: the flight law turns the hull to this aim, and a shot leaves along its fire frame.
        vehicle.drive.flight.aim_dir = dir;
        vehicle.drive.flight.aim_valid = true;
        vehicle.aim_aligned = hull.eye_orient.fvec.dot_prod(dir) >= bot_aim_fire_alignment(skill_factor);
        return;
    }
    if (vehicle.role == BotVehicleRole::driver) {
        // An APC driver's gun follows the hull within its aim cap; a client bot has no such aim to hold (first person
        // only), so it never fires from the wheel.
        rf::Vector3 held = dir;
        if (ctx.is_server_bot && vehicle_physics_clamp_driver_aim(&hull, held)) {
            vehicle.driver_aim_dir = dir;
            vehicle.driver_aim_valid = true;
            vehicle.aim_aligned = held.dot_prod(dir) >= bot_aim_fire_alignment(skill_factor);
        }
        return;
    }
    float alignment = 0.0f;
    if (vehicle.role == BotVehicleRole::turret) {
        float yaw = hull.control_data.phb.y;
        float pitch = hull.control_data.eye_phb.x;
        // This frame's shot leaves along the frame the last physics step built, already held by the stops.
        vehicle.aim_aligned =
            forward_from_non_linear_yaw_pitch(yaw, pitch).dot_prod(dir) >= bot_aim_fire_alignment(skill_factor);
        bot_aim_step(yaw, pitch, dir, dt, skill_factor, alignment);
        if (ctx.is_server_bot) {
            vehicle_server_set_turret_aim(&hull, &rider, yaw, pitch);
        }
        else {
            ctx.state.mouse_delta_override_active = true;
            ctx.state.mouse_dh_input = normalize_angle(yaw - hull.control_data.phb.y);
            ctx.state.mouse_dp_input = pitch - hull.control_data.eye_phb.x;
        }
        return;
    }
    rf::EntityControlData& cd = rider.control_data;
    vehicle.aim_aligned = bot_aim_step(cd.phb.y, cd.eye_phb.x, dir, dt, skill_factor, alignment);
    const float clamped_pitch = vehicle_clamp_seat_eye_pitch(&rider, cd.eye_phb.x);
    if (clamped_pitch != cd.eye_phb.x) {
        cd.eye_phb.x = clamped_pitch;
        vehicle.aim_aligned = false;
    }
    if (ctx.is_server_bot) {
        // The jeep gun fires along its gunner's eye_orient, read before this frame's physics rebuilds it.
        rider.eye_orient.make_quick(forward_from_non_linear_yaw_pitch(cd.phb.y, cd.eye_phb.x));
    }
}

void process_mounted_combat(
    BotState& state, rf::Entity& rider, rf::Entity& hull, rf::Entity* enemy, const bool enemy_has_los)
{
    BotVehicleState& vehicle = state.vehicle;
    const bool gun_role = vehicle.role == BotVehicleRole::turret || vehicle.role == BotVehicleRole::gunner
        || (vehicle.role == BotVehicleRole::driver && vehicle_firing_seat_index(&hull) == 0);
    vehicle.aim_enemy_handle = enemy ? enemy->handle : -1;
    vehicle.aim_valid = enemy != nullptr;
    if (enemy) {
        vehicle.aim_point = enemy->pos;
    }
    if (!gun_role || !enemy || !enemy_has_los || rf::entity_is_driller(&hull)) {
        vehicle.fire_primary = false;
        vehicle.fire_release_guard_timer.invalidate();
        return;
    }
    const int weapon = hull.ai.current_primary_weapon;
    const rf::Vector3 origin = mounted_origin(vehicle.role, rider, hull);
    const float range = bot_vehicle_mounted_weapon_range(hull, *enemy);
    const bool exposed = vehicle_hull_is_turret(&hull) || vehicle_class_open_seats(&hull);
    const bool fire_now = weapon >= 0
        && vehicle.aim_aligned
        && rf::vec_dist_squared(&origin, &enemy->pos) <= range * range
        && vehicle_weapon_ammo(&hull, weapon) != 0
        && (!exposed || bot_combat_mounted_splash_is_safe(rider, weapon, enemy->pos));
    if (fire_now) {
        vehicle.fire_release_guard_timer.set(kMountedFireReleaseGuardMs);
    }
    vehicle.fire_primary = fire_now || timer_running(vehicle.fire_release_guard_timer);
}

BotVehicleExitReason exit_reason(
    const rf::Entity& rider, rf::Entity& hull, const BotVehicleState& vehicle, const bool enemy_in_sight)
{
    // A drowning car or a stranded sub dies with its riders.
    VehicleHazardView hazard{};
    if (vehicle_hazard_view(&hull, hazard)) {
        return BotVehicleExitReason::hazard;
    }
    if (enemy_in_sight && bot_vehicle_hull_failing(hull)) {
        return BotVehicleExitReason::hull_low;
    }
    switch (vehicle.role) {
        case BotVehicleRole::driver:
            if (!enemy_in_sight && rider.life < kRiderHurtRatio * kBotNominalMaxHealth) {
                return BotVehicleExitReason::rider_hurt;
            }
            break;
        case BotVehicleRole::turret:
            if (vehicle.enemy_seen_timer.elapsed()
                && !objective_anchor_near(rider, hull.pos, turret_cover_radius(hull), true)) {
                return BotVehicleExitReason::turret_idle;
            }
            break;
        case BotVehicleRole::gunner:
            if (vehicle.driver_gone_timer.valid() && vehicle.driver_gone_timer.elapsed()) {
                return BotVehicleExitReason::driver_gone;
            }
            if (!enemy_in_sight && vehicle.gunner_still_timer.valid()
                && (vehicle.gunner_still_timer.elapsed()
                    || (vehicle.gunner_still_timer.time_until() <= kGunnerStillMs - kGunnerThreatStillMs
                        && objective_threatened(rider)))) {
                return BotVehicleExitReason::hull_stopped;
            }
            break;
        case BotVehicleRole::passenger:
            if (vehicle.driver_gone_timer.valid() && vehicle.driver_gone_timer.elapsed()) {
                return BotVehicleExitReason::driver_gone;
            }
            if (vehicle.hull_still_timer.valid() && vehicle.hull_still_timer.elapsed()) {
                return BotVehicleExitReason::hull_stopped;
            }
            if (vehicle.objective_valid
                && rf::vec_dist_squared(&hull.pos, &vehicle.objective_pos)
                    <= kPassengerArrivedDistance * kPassengerArrivedDistance) {
                return BotVehicleExitReason::arrived;
            }
            break;
        default:
            break;
    }
    return BotVehicleExitReason::none;
}

void update_seat_swap(rf::Player& player, const rf::Entity& hull, BotVehicleState& vehicle, const float hull_speed)
{
    if (vehicle.role != BotVehicleRole::gunner || !vehicle.driver_gone_timer.valid()
        || hull_speed > kSeatSwapMaxSpeed || timer_running(vehicle.seat_swap_timer)
        || !seat_usable(player, hull, 0, BotGoalType::vehicle_acquire)
        || (timer_running(vehicle.failed_class_timer) && vehicle_damage_class(&hull) == vehicle.failed_class)
        || no_route_hull(hull.handle) || on_cooldown(vehicle, hull.handle, 0)) {
        return;
    }
    vehicle.seat_swap_timer.set(kSeatSwapRetryMs);
    request_seat(player, hull, 0);
}

void update_exit(BotContext& ctx, rf::Player& player, rf::Entity& rider, rf::Entity& hull, const bool enemy_in_sight,
                 const BotVehicleExitReason drive_reason)
{
    BotVehicleState& vehicle = ctx.state.vehicle;
    const bool driver_present = vehicle_seat_leech(&hull, 0) != -1;
    if (driver_present) {
        vehicle.driver_gone_timer.invalidate();
    }
    else if (!vehicle.driver_gone_timer.valid()) {
        vehicle.driver_gone_timer.set(kDriverGoneMs);
    }
    const float hull_speed = vehicle_live_hull_velocity(&hull).len();
    if (hull_speed >= kPassengerStillSpeed) {
        vehicle.hull_still_timer.invalidate();
        vehicle.gunner_still_timer.invalidate();
    }
    else if (!vehicle.hull_still_timer.valid()) {
        vehicle.hull_still_timer.set(kPassengerStillMs);
        vehicle.gunner_still_timer.set(kGunnerStillMs);
    }

    if (vehicle.exit_requested) {
        if (vehicle.exit_timeout_timer.elapsed()) {
            // Nowhere to get out: sit on and judge again.
            vehicle.exit_requested = false;
            vehicle.drive.exit_reason = BotVehicleExitReason::none;
            return;
        }
        if (vehicle.exit_retry_timer.elapsed()) {
            request_exit(player);
            vehicle.exit_retry_timer.set(kExitRequestRetryMs);
        }
        return;
    }
    update_seat_swap(player, hull, vehicle, hull_speed);
    const BotVehicleExitReason reason = drive_reason != BotVehicleExitReason::none
        ? drive_reason
        : exit_reason(rider, hull, vehicle, enemy_in_sight);
    if (reason == BotVehicleExitReason::none) {
        return;
    }
    // In its lethal medium the hull dies with him, sinking or falling as it may: out at once.
    VehicleHazardView hazard{};
    const rf::Vector3 ground_vel = flat_vector(vehicle_live_hull_velocity(&hull));
    const bool may_exit = vehicle.role != BotVehicleRole::driver || vehicle_hazard_view(&hull, hazard)
        || (bot_vehicle_flight_hull(hull) ? bot_vehicle_flight_exit_ready(vehicle)
                                          : ground_vel.len() <= kDriverExitMaxSpeed);
    if (!may_exit) {
        // No server gate on a fast or airborne exit: the driver brakes, sets down or surfaces first.
        vehicle.drive.exit_reason = reason;
        vehicle.drive.hold = true;
        return;
    }
    if (g_alpine_game_config.dbg_bot) {
        xlog::info("[BotVeh] {} exits hull {} seat {}: {}", player.name, hull.handle, vehicle.seat,
                   bot_vehicle_exit_reason_name(reason));
    }
    const bool no_way_on = reason == BotVehicleExitReason::no_route || reason == BotVehicleExitReason::stuck;
    note_cooldown(vehicle, hull.handle, vehicle.seat, no_way_on ? kNoRouteCooldownMs : kExitCooldownMs);
    if (no_way_on && vehicle.role == BotVehicleRole::driver) {
        vehicle.failed_class = vehicle_damage_class(&hull);
        vehicle.failed_class_timer.set(kNoRouteCooldownMs);
        note_no_route_hull(hull.handle);
    }
    vehicle.exit_requested = true;
    vehicle.exit_timeout_timer.set(kExitGiveUpMs);
    vehicle.exit_retry_timer.set(kExitRequestRetryMs);
    request_exit(player);
}

void mirror_client_outputs(BotState& state, const bool owns_gun)
{
    BotCombatFiringState& firing = state.firing;
    const bool primary = owns_gun && state.vehicle.fire_primary;
    firing.synthetic_primary_fire_just_pressed = primary && !firing.synthetic_primary_fire_down;
    firing.synthetic_primary_fire_down = primary;
    firing.wants_fire = primary;
    // A driver's keys reach the hull through the stock redirect (0x004A6101); any other seat's walk keys must not.
    const BotVehicleState& vehicle = state.vehicle;
    state.movement_override_active = true;
    state.move_input_x = vehicle.move_cmd.x;
    state.move_input_y = vehicle.move_cmd.y;
    state.move_input_z = vehicle.move_cmd.z;
    if (vehicle.role == BotVehicleRole::driver) {
        state.rot_override_active = true;
        // Written past the engine's 0.5 keyboard pitch scale, which the flight model doubles back (as the server's
        // vehicle_server_set_driver_input does).
        state.rot_input_x = 0.5f * vehicle.pitch_cmd;
        state.rot_input_y = vehicle.yaw_cmd;
    }
}

void step_driver_law(BotVehicleState& vehicle, const rf::Entity& hull, const float dt)
{
    if (bot_vehicle_flight_hull(hull)) {
        bot_vehicle_flight_step(vehicle, hull, dt);
    }
    else {
        bot_vehicle_drive_step(vehicle, hull, dt);
    }
}

void server_stop_fire(BotContext& ctx, rf::Player& pp)
{
    BotVehicleState& vehicle = ctx.state.vehicle;
    if (vehicle.reported_fire_hull != -1) {
        report_fire_edge(pp, vehicle.reported_fire_hull, false, vehicle.reported_primary);
    }
    vehicle.reported_fire_hull = -1;
    vehicle.reported_primary = false;
}

float transport_score(const BotVehicleGoalContext& context, const float saving, const float affinity_norm)
{
    return context.objective_score + std::lerp(40.0f, 140.0f, affinity_norm) * std::min(saving * 2.0f, 1.0f);
}

// A fighter or sub ride to a landing node near the objective when it beats the walk by the driller's share,
// else a hunt where there is anything to hunt. -inf when the hull is no candidate.
float flight_seat_score(const rf::Player& pp, const rf::Entity& hull, const BotVehicleGoalContext& context,
                        const rf::Vector3* objective, const float foot_dist, const float objective_walk,
                        const float affinity_norm, BotGoalType& out_goal)
{
    float flight_dist = 0.0f;
    float exit_walk = 0.0f;
    if (objective && bot_vehicle_flight_transport_estimate(hull, *objective, flight_dist, exit_walk)) {
        const float top_speed = vehicle_physics_class_top_speed(vehicle_damage_class(&hull));
        const float ride_seconds = (foot_dist + kFootRouteStretch * exit_walk) / kWalkSpeed
            + kBoardOverheadSeconds + kLandingOverheadSeconds
            + flight_dist / std::max(kTransportDriveSpeedShare * top_speed, 1.0f);
        const float saving = 1.0f - ride_seconds * kWalkSpeed / objective_walk;
        if (saving >= kDrillerMinSaving) {
            out_goal = BotGoalType::vehicle_acquire;
            return transport_score(context, saving, affinity_norm);
        }
    }
    if (foot_dist > kHuntMaxFootDistance || !bot_vehicle_flight_hunt_worthwhile(pp, hull)) {
        return -std::numeric_limits<float>::infinity();
    }
    out_goal = BotGoalType::vehicle_hunt;
    return std::lerp(50.0f, 160.0f, affinity_norm) + std::clamp(30.0f - foot_dist, -30.0f, 30.0f);
}

// The wheel of a ground hull: transport toward the point the driver will follow (bot_goals_transport_objective) when
// the drive beats the walk (the tier-1 objective's score plus a share for the time saved), else a hunt. A driller only
// when no other drivable hull is reachable. Fighters and subs by flight_seat_score.
void consider_driver_seats(const rf::Player& pp, const rf::Entity& local_entity, const BotVehicleGoalContext& context,
                           const int start_waypoint, BotVehicleGoalCandidate& out)
{
    const BotState& state = bot_state();
    const float affinity_norm = bias_norm(get_active_bot_personality().vehicle_affinity_bias);
    rf::Vector3 objective{};
    const bool transport = context.objective_valid && bot_goals_transport_objective(local_entity, objective);
    float objective_walk = std::numeric_limits<float>::infinity();
    if (transport) {
        objective_walk = foot_distance(start_waypoint, bot_find_closest_waypoint_with_fallback(objective))
                             .value_or(kFootRouteStretch * (objective - local_entity.pos).len());
        if (objective_walk < kTransportMinWalk) {
            return;
        }
    }
    else if (context.carrying_objective) {
        return;
    }
    const BotGoalType goal = transport ? BotGoalType::vehicle_acquire : BotGoalType::vehicle_hunt;
    BotVehicleGoalCandidate driller{};
    bool other_reachable = false;
    for (const int handle : std::vector<int>{vehicle_hull_handles()}) {
        const rf::Entity* const hull = vehicle_live_synced_entity(handle);
        if (!hull || (context.carrying_ctf_flag && !rf::entity_is_jeep(const_cast<rf::Entity*>(hull)))) {
            continue;
        }
        // A fighter's goal is not the ground one's, so either drive goal on this hull counts.
        const bool active_target = (state.active_goal == BotGoalType::vehicle_acquire
                                    || state.active_goal == BotGoalType::vehicle_hunt)
            && handle == state.goal_target_handle && state.goal_target_identifier == 0;
        if (!seat_usable(pp, *hull, 0, goal) || on_cooldown(state.vehicle, handle, 0)
            || (timer_running(state.vehicle.failed_class_timer)
                && vehicle_damage_class(hull) == state.vehicle.failed_class)
            || no_route_hull(handle)
            || (!active_target && team_mate_closer_to_hull(pp, local_entity, *hull))) {
            continue;
        }
        int waypoint = 0;
        rf::Vector3 waypoint_pos{};
        const std::optional<float> foot_dist =
            candidate_board_waypoint(*hull, waypoint, waypoint_pos)
                && boarding_spot_reaches(local_entity, *hull, waypoint_pos)
            ? foot_distance(start_waypoint, waypoint)
            : std::nullopt;
        if (!foot_dist) {
            continue;
        }
        if (bot_vehicle_flight_hull(*hull)) {
            // A bag carrier keeps to the ground, where he can hold a spot with it.
            if (context.carrying_bag) {
                continue;
            }
            BotGoalType flight_goal = BotGoalType::none;
            const float score = flight_seat_score(pp, *hull, context, transport ? &objective : nullptr, *foot_dist,
                                                  objective_walk, affinity_norm, flight_goal);
            other_reachable = other_reachable || std::isfinite(score);
            if (score > out.score) {
                out = BotVehicleGoalCandidate{flight_goal, handle, 0, waypoint, waypoint_pos, score};
            }
            continue;
        }
        const bool is_driller = rf::entity_is_driller(const_cast<rf::Entity*>(hull));
        float score = 0.0f;
        // The control point a seated driver picks is chosen from the hull: score that one.
        rf::Vector3 hull_objective = objective;
        if (transport) {
            bot_goals_transport_objective(local_entity, hull_objective, &hull->pos);
            const float top_speed = vehicle_physics_class_top_speed(vehicle_damage_class(hull));
            const float drive_seconds = (*foot_dist + kTransportExitWalk) / kWalkSpeed + kBoardOverheadSeconds
                + kTransportDriveStretch * (hull_objective - hull->pos).len()
                    / std::max(kTransportDriveSpeedShare * top_speed, 1.0f);
            const float saving = 1.0f - drive_seconds * kWalkSpeed / objective_walk;
            if (saving < (is_driller ? kDrillerMinSaving : kTransportMinSaving)) {
                continue;
            }
            score = transport_score(context, saving, affinity_norm);
            // Last, as it plans a route: only a hull that would win, and whose driver would find a way there.
            if (score <= (is_driller ? driller : out).score
                || !bot_vehicle_drive_transport_route_exists(*hull, hull_objective)) {
                continue;
            }
        }
        else {
            if (is_driller || *foot_dist > kHuntMaxFootDistance) {
                continue;
            }
            score = std::lerp(60.0f, 170.0f, affinity_norm) + std::clamp(30.0f - *foot_dist, -30.0f, 30.0f);
        }
        // Only a hull that passed every test (the route check included) rules the driller out.
        other_reachable = other_reachable || !is_driller;
        BotVehicleGoalCandidate& best = is_driller ? driller : out;
        if (score > best.score) {
            best = BotVehicleGoalCandidate{goal, handle, 0, waypoint, waypoint_pos, score};
        }
    }
    if (!other_reachable && driller.score > out.score) {
        out = driller;
    }
}

}

void bot_vehicle_run_seated_frame(BotContext& ctx, rf::Player& player, rf::Entity& rider, rf::Entity& hull)
{
    BotVehicleState& vehicle = ctx.state.vehicle;
    const int seat = vehicle_seat_index_from_tag(&hull, rider.host_tag_handle);
    if (vehicle.hull_handle != hull.handle || vehicle.seat != seat) {
        bot_reset_foot_watchdogs();
        clear_fire_intent(ctx.state);
        vehicle.hull_handle = hull.handle;
        vehicle.seat = seat;
        vehicle.role = role_for_seat(hull, seat);
        vehicle.enemy_seen_timer.set(kTurretIdleMs);
        vehicle.turret_no_shot_timer.invalidate();
        vehicle.driver_gone_timer.invalidate();
        vehicle.hull_still_timer.invalidate();
        vehicle.exit_requested = false;
        vehicle.aim_valid = false;
        vehicle.aim_enemy_handle = -1;
        vehicle.move_cmd = {};
        vehicle.pitch_cmd = 0.0f;
        vehicle.yaw_cmd = 0.0f;
        vehicle.driver_aim_valid = false;
        vehicle.target_hull_handle = -1;
        vehicle.target_seat = -1;
        vehicle.seat_swap_timer.invalidate();
        vehicle.drive = BotVehicleDriveState{};
        if (g_alpine_game_config.dbg_bot) {
            xlog::info("[BotVeh] {} seated: hull {} ({}) seat {} role {}", player.name, hull.handle,
                vehicle_class_display_name(vehicle_damage_class(&hull)), seat, role_name(vehicle.role));
        }
    }
    bot_fsm_manager_transition_state(fsm_state_for_role(vehicle.role));

    bot_memory_manager_update_context(player, rider);
    rf::Entity* const enemy_target = bot_perception_manager_select_enemy_target(player, rider);
    bool enemy_has_los = false;
    if (enemy_target) {
        enemy_has_los = bot_perception_manager_get_enemy_los_cached(rider, *enemy_target);
    }
    else {
        bot_perception_manager_reset_tracking();
    }
    process_mounted_combat(ctx.state, rider, hull, enemy_target, enemy_has_los);
    if (enemy_has_los) {
        // A turret that sees an enemy past its yaw stop or range never shoots; that must not hold it seated.
        if (vehicle.role != BotVehicleRole::turret || vehicle.fire_primary) {
            vehicle.turret_no_shot_timer.invalidate();
        }
        else if (!vehicle.turret_no_shot_timer.valid()) {
            vehicle.turret_no_shot_timer.set(kTurretNoShotMs);
        }
        if (!vehicle.turret_no_shot_timer.valid() || !vehicle.turret_no_shot_timer.elapsed()) {
            vehicle.enemy_seen_timer.set(kTurretIdleMs);
        }
    }
    BotVehicleExitReason drive_reason = BotVehicleExitReason::none;
    if (vehicle.role == BotVehicleRole::driver && !rf::entity_is_dying(&hull)) {
        drive_reason = bot_vehicle_flight_hull(hull)
            ? bot_vehicle_flight_think(ctx, player, rider, hull, enemy_target, enemy_has_los)
            : bot_vehicle_drive_think(ctx, player, rider, hull, enemy_target, enemy_has_los);
    }
    update_exit(ctx, player, rider, hull, enemy_has_los, drive_reason);

    if (!ctx.is_server_bot) {
        step_seated_aim(ctx, rider, hull, rf::frametime);
        step_driver_law(vehicle, hull, rf::frametime);
        mirror_client_outputs(ctx.state, vehicle_firing_seat_occupant(&hull) == &rider);
    }
}

void bot_vehicle_note_on_foot(BotContext& ctx)
{
    BotVehicleState& vehicle = ctx.state.vehicle;
    if (vehicle.hull_handle == -1) {
        return;
    }
    if (g_alpine_game_config.dbg_bot && ctx.player) {
        xlog::info("[BotVeh] {} left hull {}", ctx.player->name, vehicle.hull_handle);
    }
    vehicle.reset();
    clear_fire_intent(ctx.state);
    if (bot_goal_is_vehicle(ctx.state.active_goal)) {
        bot_state_set_roam_fallback_goal(0);
    }
    bot_reset_foot_watchdogs();
}

void bot_vehicle_server_apply_outputs(BotContext& ctx, rf::Player& pp, rf::Entity& rider, rf::Entity& hull)
{
    BotVehicleState& vehicle = ctx.state.vehicle;
    if (rf::entity_is_dying(&rider) || rf::entity_is_dying(&hull)) {
        bot_vehicle_server_stop_outputs(ctx, pp);
        return;
    }
    {
        BotActiveScope scope{ctx};
        step_seated_aim(ctx, rider, hull, rf::frametime);
    }
    // Every frame, a zero intent included: the hull's ci otherwise keeps its last author's input. The vehicle
    // module refuses anyone but the hull's server-local driver.
    const bool intent_for_hull = vehicle.hull_handle == hull.handle;
    if (intent_for_hull) {
        step_driver_law(vehicle, hull, rf::frametime);
    }
    vehicle_server_set_driver_input(&hull, &rider, intent_for_hull ? vehicle.move_cmd : rf::Vector3{},
        intent_for_hull ? vehicle.pitch_cmd : 0.0f, intent_for_hull ? vehicle.yaw_cmd : 0.0f);
    vehicle_server_set_driver_aim(
        &hull, &rider, intent_for_hull && vehicle.driver_aim_valid ? vehicle.driver_aim_dir : rf::Vector3{});
    if (vehicle.reported_fire_hull != hull.handle) {
        server_stop_fire(ctx, pp);
        vehicle.reported_fire_hull = hull.handle;
    }
    const bool owns_gun = vehicle_firing_seat_occupant(&hull) == &rider;
    report_fire_edge(pp, hull.handle, owns_gun && vehicle.fire_primary, vehicle.reported_primary);
}

void bot_vehicle_server_stop_outputs(BotContext& ctx, rf::Player& pp)
{
    server_stop_fire(ctx, pp);
    rf::Entity* const rider = rf::entity_from_handle(pp.entity_handle);
    if (rf::Entity* const hull = vehicle_ridden_hull(rider)) {
        vehicle_server_set_driver_input(hull, rider, rf::Vector3{}, 0.0f, 0.0f);
        vehicle_server_set_driver_aim(hull, rider, rf::Vector3{});
    }
}

bool bot_vehicle_find_goal_candidate(
    const rf::Player& pp, const rf::Entity& local_entity, const BotVehicleGoalContext& context,
    BotVehicleGoalCandidate& out)
{
    out = BotVehicleGoalCandidate{};
    if (!vehicles_enabled_for_active_bot() || (context.carrying_objective && !context.carrying_ctf_flag)) {
        return false;
    }
    const int start_waypoint = bot_find_closest_waypoint_with_fallback(local_entity.pos);
    if (start_waypoint <= 0) {
        return false;
    }
    const BotPersonality& personality = get_active_bot_personality();
    const float affinity_norm = bias_norm(personality.vehicle_affinity_bias);
    const float gunner_norm = bias_norm(personality.vehicle_gunner_bias);
    const BotState& state = bot_state();
    const BotVehicleState& vehicle = state.vehicle;
    rf::Vector3 start_pos{};
    const bool has_start_pos = waypoints_get_pos(start_waypoint, start_pos);

    for (const int handle : std::vector<int>{vehicle_hull_handles()}) {
        const rf::Entity* const hull = vehicle_live_synced_entity(handle);
        if (!hull || context.carrying_objective) {
            continue;
        }
        BotGoalType goal = BotGoalType::none;
        int seat = -1;
        if (vehicle_hull_is_turret(hull)) {
            goal = BotGoalType::vehicle_turret;
            seat = 0;
        }
        else if (rf::entity_is_jeep(const_cast<rf::Entity*>(hull))) {
            goal = BotGoalType::vehicle_gun;
            seat = vehicle_firing_seat_index(hull);
        }
        else {
            goal = BotGoalType::vehicle_acquire;
            for (int i = 1; i < vehicle_seat_count(hull); ++i) {
                if (vehicle_seat_leech(hull, i) == -1) {
                    seat = i;
                    break;
                }
            }
        }
        // Contention decides who sets out for a seat, not who keeps walking to it.
        const bool active_target = bot_goal_is_vehicle(state.active_goal) && handle == state.goal_target_handle
            && seat == state.goal_target_identifier;
        if (!seat_usable(pp, *hull, seat, goal) || on_cooldown(vehicle, handle, seat)
            || (!active_target && team_mate_closer_to_hull(pp, local_entity, *hull))) {
            continue;
        }
        int waypoint = 0;
        rf::Vector3 waypoint_pos{};
        if (!candidate_board_waypoint(*hull, waypoint, waypoint_pos)
            || !boarding_spot_reaches(local_entity, *hull, waypoint_pos)) {
            continue;
        }
        // The straight-line limits first, so a far seat costs no route search.
        if (goal == BotGoalType::vehicle_turret) {
            // Near a place the team holds or contests: one under attack, or any while enemies are about.
            const float cover_radius = turret_cover_radius(*hull);
            const bool covers_objective =
                objective_anchor_near(local_entity, hull->pos, cover_radius, !context.enemy_contact)
                || (context.enemy_contact && context.objective_valid
                    && rf::vec_dist_squared(&context.objective_pos, &hull->pos) <= cover_radius * cover_radius);
            if (!covers_objective) {
                continue;
            }
        }
        else if (goal == BotGoalType::vehicle_gun) {
            // Links cost their straight length, so the straight line between the waypoints bounds the walk.
            if (!has_start_pos
                || rf::vec_dist_squared(&start_pos, &waypoint_pos) > kGunnerMaxFootDistance * kGunnerMaxFootDistance) {
                continue;
            }
        }
        else if (rf::vec_dist_squared(&local_entity.pos, &hull->pos)
                     > kPassengerDriverMaxDistance * kPassengerDriverMaxDistance
                 || !context.objective_valid
                 || rf::vec_dist_squared(&local_entity.pos, &context.objective_pos)
                     < kPassengerObjectiveMinDistance * kPassengerObjectiveMinDistance) {
            continue;
        }
        const std::optional<float> foot_dist = foot_distance(start_waypoint, waypoint);
        if (!foot_dist) {
            continue;
        }

        float score = -std::numeric_limits<float>::infinity();
        if (goal == BotGoalType::vehicle_turret) {
            score = std::lerp(140.0f, 260.0f, affinity_norm) + std::clamp(60.0f - *foot_dist, -40.0f, 60.0f);
            if (bot_personality_has_quirk(BotPersonalityQuirk::turret_camper)) {
                score *= 1.3f;
            }
        }
        else if (goal == BotGoalType::vehicle_gun) {
            if (*foot_dist > kGunnerMaxFootDistance) {
                continue;
            }
            // A team-mate's jeep is a short chance (it drives off), so near one it outbids a turret.
            const rf::Player* const driver = vehicle_seat_occupant_player(hull, 0);
            score = std::lerp(200.0f, 320.0f, gunner_norm) + std::clamp(30.0f - *foot_dist, -20.0f, 30.0f)
                + (driver && !driver->is_bot ? 40.0f : 0.0f);
        }
        else {
            score = std::lerp(80.0f, 170.0f, affinity_norm);
        }
        if (score > out.score) {
            out = BotVehicleGoalCandidate{goal, handle, seat, waypoint, waypoint_pos, score};
        }
    }
    consider_driver_seats(pp, local_entity, context, start_waypoint, out);
    return out.hull_handle >= 0;
}

bool bot_vehicle_goal_still_valid(const rf::Player& pp)
{
    const BotState& state = bot_state();
    const rf::Entity* const hull = vehicle_live_synced_entity(state.goal_target_handle);
    return vehicles_enabled_for_active_bot() && hull
        && seat_usable(pp, *hull, state.goal_target_identifier, state.active_goal)
        && !on_cooldown(state.vehicle, hull->handle, state.goal_target_identifier);
}

bool bot_vehicle_enemy_contact(const bool contact_now)
{
    rf::Timestamp& memory = bot_state().vehicle.enemy_contact_timer;
    if (contact_now) {
        memory.set(kEnemyContactMemoryMs);
    }
    return timer_running(memory);
}

void bot_vehicle_dbg_goal_change(const BotGoalType goal, const int hull_handle, const int seat, const float score)
{
    const BotState& state = bot_state();
    const rf::Player* const pp = bot_player();
    if (!g_alpine_game_config.dbg_bot || !pp) {
        return;
    }
    if (bot_goal_is_vehicle(goal)) {
        xlog::info("[BotVeh] {} picks {} hull {} seat {} (score {:.0f})", pp->name, bot_goal_type_to_string(goal),
            hull_handle, seat, score);
    }
    else if (bot_goal_is_vehicle(state.active_goal)) {
        xlog::info("[BotVeh] {} drops {} hull {} seat {} for {} (score {:.0f}, seat {})", pp->name,
            bot_goal_type_to_string(state.active_goal), state.goal_target_handle, state.goal_target_identifier,
            bot_goal_type_to_string(goal), score, bot_vehicle_goal_still_valid(*pp) ? "still free" : "gone");
    }
}

BotVehicleAcquireStatus bot_vehicle_update_acquire(const rf::Entity& local_entity, BotVehicleApproach& out)
{
    BotState& state = bot_state();
    BotVehicleState& vehicle = state.vehicle;
    rf::Player* const pp = bot_player();
    rf::Entity* const hull = vehicle_live_synced_entity(state.goal_target_handle);
    const int seat = state.goal_target_identifier;
    if (!pp || !hull) {
        out.abort_reason = "hull gone";
        return BotVehicleAcquireStatus::abort;
    }
    if (!vehicles_enabled_for_active_bot()) {
        out.abort_reason = "vehicles off";
        return BotVehicleAcquireStatus::abort;
    }
    if (!seat_usable(*pp, *hull, seat, state.active_goal)) {
        out.abort_reason = "seat taken or refused";
        return BotVehicleAcquireStatus::abort;
    }
    if (vehicle.target_hull_handle != hull->handle || vehicle.target_seat != seat) {
        vehicle.target_hull_handle = hull->handle;
        vehicle.target_seat = seat;
        vehicle.use_retry_timer.invalidate();
        vehicle.in_range_timer.invalidate();
        vehicle.route_fail_timer.invalidate();
        vehicle.acquire_timeout_timer.set(kAcquireTimeoutMs);
    }
    if (vehicle.acquire_timeout_timer.elapsed()) {
        out.abort_reason = "timeout";
        return BotVehicleAcquireStatus::abort;
    }
    out.hull = hull;
    out.hull_pos = hull->pos;

    if (vehicle_board_in_range(&local_entity, hull, kBoardRangeMargin)) {
        if (!vehicle.in_range_timer.valid()) {
            vehicle.in_range_timer.set(kSeatInRangeGiveUpMs);
        }
        else if (vehicle.in_range_timer.elapsed()) {
            // Never seated at the wheel: on a client bot, most likely the server's cap of bot drivers.
            const int bot_drivers = rf::is_server || seat != 0 ? 0 : vehicle_count_bot_driven_hulls();
            if (bot_drivers > 0) {
                vehicle.driver_cap = bot_drivers;
                vehicle.driver_cap_timer.set(kDriverCapGuessMs);
            }
            out.abort_reason = "gave up boarding";
            return BotVehicleAcquireStatus::abort;
        }
        if (!timer_running(vehicle.use_retry_timer)) {
            request_seat(*pp, *hull, seat);
            vehicle.use_retry_timer.set(kSeatRequestRetryMs);
        }
        return BotVehicleAcquireStatus::boarding;
    }
    vehicle.in_range_timer.invalidate();

    const int factory_uid = factory_uid_under_hull(*hull);
    if (factory_uid >= 0 && waypoints_find_vehicle_factory_waypoint(factory_uid, out.board_waypoint, out.board_pos)) {
        return BotVehicleAcquireStatus::approach;
    }
    if (const int hull_waypoint = waypoints_ensure_hull_waypoint(*hull);
        hull_waypoint > 0 && waypoints_get_pos(hull_waypoint, out.board_pos)) {
        out.board_waypoint = hull_waypoint;
        return BotVehicleAcquireStatus::approach;
    }
    if (bot_nav_find_reachable_waypoint_near(local_entity, hull->pos, 0, out.board_waypoint, out.board_pos)) {
        return BotVehicleAcquireStatus::approach;
    }
    out.abort_reason = "no boarding spot";
    return BotVehicleAcquireStatus::abort;
}

void bot_vehicle_note_goal_abandoned(const char* const reason)
{
    BotState& state = bot_state();
    if (!bot_goal_is_vehicle(state.active_goal)) {
        return;
    }
    if (g_alpine_game_config.dbg_bot && bot_player()) {
        xlog::info("[BotVeh] {} abort hull {} seat {}: {}", bot_player()->name, state.goal_target_handle,
            state.goal_target_identifier, reason);
    }
    note_cooldown(state.vehicle, state.goal_target_handle, state.goal_target_identifier, kSeatRefusedCooldownMs);
    state.vehicle.target_hull_handle = -1;
    state.vehicle.target_seat = -1;
    state.vehicle.acquire_timeout_timer.invalidate();
    state.vehicle.route_fail_timer.invalidate();
}

bool bot_vehicle_goal_hull_parked()
{
    const rf::Entity* const hull = vehicle_live_synced_entity(bot_state().goal_target_handle);
    return hull && vehicle_live_hull_velocity(hull).len() < kParkedSpeed;
}

bool bot_vehicle_abort_acquire(const char* const reason)
{
    bot_vehicle_note_goal_abandoned(reason);
    bot_state_set_roam_fallback_goal(250);
    bot_state_clear_waypoint_route(true, true, false);
    return false;
}

bool bot_vehicle_is_boarding(const rf::Entity& local_entity)
{
    if (!bot_goal_is_vehicle(bot_state().active_goal)) {
        return false;
    }
    const rf::Entity* const hull = vehicle_live_synced_entity(bot_state().goal_target_handle);
    return hull && vehicle_board_in_range(&local_entity, hull, 0.0f);
}

const rf::Entity* bot_vehicle_on_foot_entity(const rf::Player& player)
{
    const rf::Entity* const entity = rf::entity_from_handle(player.entity_handle);
    if (!entity || entity->host_handle != -1 || rf::entity_is_dying(const_cast<rf::Entity*>(entity))) {
        return nullptr;
    }
    return entity;
}

rf::Entity* bot_vehicle_effective_target(rf::Entity* entity)
{
    rf::Entity* const hull = vehicle_ridden_live_hull(entity);
    if (hull && !vehicle_hull_is_turret(hull) && !vehicle_class_open_seats(hull)) {
        return hull;
    }
    return entity;
}

bool bot_vehicle_hull_without_enemy(const rf::Entity& entity, const rf::Player& pp)
{
    return vehicle_is_synced_entity_type(&entity) && !vehicle_occupied_by_enemy(&pp, &entity);
}

rf::Vector3 bot_vehicle_target_velocity(const rf::Entity& entity)
{
    const rf::Entity* const hull =
        vehicle_is_synced_entity_type(&entity) ? &entity : vehicle_ridden_live_hull(&entity);
    return hull ? vehicle_live_hull_velocity(hull) : entity.p_data.vel;
}

float bot_vehicle_health_ratio(const rf::Entity& hull)
{
    const float max_life = vehicle_hud_max_life(&hull);
    return max_life > 0.0f ? std::clamp(vehicle_hud_life(&hull) / max_life, 0.0f, 1.0f) : 1.0f;
}

bool bot_vehicle_hull_failing(const rf::Entity& hull)
{
    return bot_vehicle_health_ratio(hull) < kLowHullLifeRatio;
}

bool bot_vehicle_is_enemy(const rf::Player& pp, const rf::Player& other)
{
    return &other != &pp && !other.is_spectator && (!multi_is_team_game_type() || other.team != pp.team);
}

float bot_vehicle_hull_hit_radius(const rf::Entity& hull)
{
    rf::Vector3 half{};
    vehicle_physics_entity_hull_box(&hull, &half, nullptr);
    return std::min({half.x, half.y, half.z});
}

bool bot_vehicle_active_goal_carries_flag()
{
    const BotState& state = bot_state();
    const rf::Entity* const hull = vehicle_live_synced_entity(state.goal_target_handle);
    return state.active_goal == BotGoalType::vehicle_acquire && state.goal_target_identifier == 0 && hull
        && rf::entity_is_jeep(const_cast<rf::Entity*>(hull));
}

bool bot_vehicle_find_hull_threat(const rf::Player& pp, const rf::Entity& local_entity, BotVehicleHullThreat& out)
{
    out = BotVehicleHullThreat{};
    if (!vehicle_level_has_factories() || local_entity.host_handle != -1) {
        return false;
    }
    float best_dist_sq = kHullThreatRange * kHullThreatRange;
    for (const int handle : vehicle_hull_handles()) {
        rf::Entity* const hull = vehicle_live_synced_entity(handle);
        const rf::Player* const driver = hull ? vehicle_seat_occupant_player(hull, 0) : nullptr;
        if (!driver || vehicle_hull_is_turret(hull) || driver == &pp
            || (multi_is_team_game_type() && driver->team == pp.team)) {
            continue;
        }
        const rf::Vector3 offset = local_entity.pos - hull->pos;
        if (std::fabs(offset.y) > kHullThreatHeight + bot_vehicle_hull_hit_radius(*hull)) {
            continue;
        }
        const rf::Vector3 to_bot = flat_vector(offset);
        const float dist_sq = to_bot.len_sq();
        const rf::Vector3 vel = flat_vector(vehicle_live_hull_velocity(hull));
        const float dist = std::sqrt(dist_sq);
        if (dist_sq > best_dist_sq || dist < 0.01f || vel.dot_prod(to_bot) / dist <= vehicle_crush_min_speed) {
            continue;
        }
        rf::Entity* const target = bot_vehicle_effective_target(rf::entity_from_handle(driver->entity_handle));
        if (!target) {
            continue;
        }
        best_dist_sq = dist_sq;
        out.target = target;
        rf::Vector3 heading = vel;
        heading.normalize_safe();
        const rf::Vector3 side{heading.z, 0.0f, -heading.x};
        const float lateral = to_bot.dot_prod(side);
        out.in_path =
            dist <= kHullDodgeRange && std::atan2(std::fabs(lateral), to_bot.dot_prod(heading)) <= kHullDodgeCone;
        // Away from the line it drives along, on the side the bot already stands.
        out.dodge_dir = side * (lateral >= 0.0f ? 1.0f : -1.0f);
    }
    return out.target != nullptr;
}

bool bot_vehicle_foot_node_near_road(const rf::Vector3& pos)
{
    return bot_vehicle_nav_near_vehicle_road(pos, kRoadAvoidRadius);
}

float bot_vehicle_mounted_weapon_range(const rf::Entity& hull, const rf::Entity& target)
{
    float range = bot_combat_weapon_effective_range(hull.ai.current_primary_weapon);
    if (range <= 0.0f) {
        range = kMountedFallbackRange;
    }
    return vehicle_is_synced_entity_type(&target) ? range * kMountedHullRangeScale : range;
}

bool bot_vehicle_team_mate_on_foot_within(
    const rf::Player& pp, const rf::Vector3& pos, const float radius, const bool humans_only)
{
    if (!multi_is_team_game_type()) {
        return false;
    }
    for (const rf::Player& other : SinglyLinkedList{rf::player_list}) {
        if (&other == &pp || other.team != pp.team || other.is_spectator || (humans_only && other.is_bot)) {
            continue;
        }
        const rf::Entity* const entity = bot_vehicle_on_foot_entity(other);
        if (entity && rf::vec_dist_squared(&entity->pos, &pos) <= radius * radius) {
            return true;
        }
    }
    return false;
}

const char* bot_vehicle_exit_reason_name(const BotVehicleExitReason reason)
{
    switch (reason) {
        case BotVehicleExitReason::hazard:
            return "hazard";
        case BotVehicleExitReason::hull_low:
            return "hull low";
        case BotVehicleExitReason::rider_hurt:
            return "rider hurt";
        case BotVehicleExitReason::turret_idle:
            return "turret idle";
        case BotVehicleExitReason::driver_gone:
            return "driver gone";
        case BotVehicleExitReason::hull_stopped:
            return "hull stopped";
        case BotVehicleExitReason::arrived:
            return "arrived";
        case BotVehicleExitReason::objective_close:
            return "objective close";
        case BotVehicleExitReason::hunt_idle:
            return "hunt idle";
        case BotVehicleExitReason::no_route:
            return "no route";
        case BotVehicleExitReason::stuck:
            return "stuck";
        default:
            return "none";
    }
}
