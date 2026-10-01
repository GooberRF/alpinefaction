#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <numbers>
#include <optional>
#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/FunHook.h>
#include <patch_common/MemUtils.h>
#include "vehicle.h"
#include "vehicle_physics.h"
#include "vehicle_internal.h"
#include "vehicle_sync.h"
#include "vehicle_damage.h"
#include "vehicle_view.h"
#include "../alpine_packets.h"
#include "../multi.h"
#include "../server_internal.h"
#include "../../hud/hud.h"
#include "../../hud/multi_spectate.h"
#include "../../misc/level.h"
#include "../../misc/player.h"
#include "../../os/console.h"
#include "../../os/os.h"
#include "../../rf/ai.h"
#include "../../rf/collide.h"
#include "../../rf/entity.h"
#include "../../rf/item.h"
#include "../../rf/multi.h"
#include "../../rf/object.h"
#include "../../rf/os/frametime.h"
#include "../../rf/physics.h"
#include "../../rf/player/camera.h"
#include "../../rf/player/player.h"
#include "../../rf/vmesh.h"
#include "../../rf/weapon.h"

namespace
{
    // Rate floor for the streaming health/ammo sends, and the hold-still time before the reliable one.
    constexpr int vehicle_health_send_interval_ms = 100;
    constexpr int64_t vehicle_health_settle_ms = 1000;

    // One supplement per obj_update sample (CLIENT_NET_FPS 40, 25 ms); only a faster client is capped.
    constexpr int vehicle_orient_relay_min_ms = 15;
    constexpr int vehicle_orient_keepalive_ms = 5000;
    // Past half the 16-bit tick range a tick comparison is meaningless; a record this old accepts anything.
    constexpr int64_t vehicle_orient_stale_ms = 30000;

    // Least time between two turn-ons of one hull's continuous gun, whatever weapons.tbl says.
    constexpr int vehicle_fire_rearm_floor_ms = 100;

    // pack_obj_update_data (cdecl: recipient, entity, out buffer) -> bytes written. network.cpp's
    // dedupe keys on the newest keyframe tick, so at most ONE keyframe per ms tick may be pushed.
    constexpr uintptr_t pack_obj_update_data_addr = 0x0047DB20;

    // __thiscall, eight stack args, RET 0x20; arg 8 is a FLOAT (FLD [ESP+0x1300] at 0x004834BA).
    constexpr uintptr_t obj_interp_set_next_pos_orient_addr = 0x00483360;
} // namespace

// Re-seed PF_NET_PLAYER ownership from the seats. driver_boarding is a seat-0 boarding, a fresh
// takeover, so the coast's keyframes must be flushed on every machine.
void vehicle_update_interp_ownership(rf::Entity* vehicle, bool driver_boarding)
{
    if (!vehicle) {
        return;
    }
    const rf::Entity* driver = vehicle_driver_entity(vehicle);
    if (driver && driver == rf::local_player_entity) {
        // The sim continues from control_data.phb through physics_make_orient, so this must be its
        // exact inverse of the hull, or the first frame teleports it.
        vehicle->control_data.phb = vehicle_make_orient_phb(vehicle->orient);
        vehicle->p_data.flags &= ~rf::PF_NET_PLAYER;
        g_vehicle_state.orient.erase(vehicle->handle);
        // Taking over a hull this machine watched coast: drop its keyframes.
        if (driver_boarding && vehicle->obj_interp) {
            vehicle->obj_interp->Clear();
            g_vehicle_state.orient_sent.erase(vehicle->handle);
        }
    }
    else {
        // Drop the previous owner's samples rather than interpolating a fresh row against them.
        if ((!(vehicle->p_data.flags & rf::PF_NET_PLAYER) || driver_boarding) && vehicle->obj_interp) {
            // An emptied ring makes control_data.phb the orientation through set_from_angles, whose
            // exact inverse this is.
            vehicle->control_data.phb = vehicle_matrix_phb(vehicle->orient);
            vehicle->obj_interp->Clear();
            // A stale coast supplement's tick was minted on the SERVER's clock, so the out-of-order
            // guard in vehicle_store_orient would reject fresh ones until the clocks crossed.
            g_vehicle_state.orient.erase(vehicle->handle);
            g_vehicle_state.orient_sent.erase(vehicle->handle);
        }
        vehicle->p_data.flags |= rf::PF_NET_PLAYER;
        // Nothing else damps a driverless vehicle, and this is the one point every seat change
        // goes through.
        if (!driver) {
            vehicle->p_data.vel = rf::Vector3{};
            vehicle->p_data.rotvel = rf::Vector3{};
            vehicle->p_data.ang_momentum = rf::Vector3{};
            vehicle->ai.ci.move = rf::Vector3{};
        }
    }

    // Where the Bullet body is created and destroyed.
    vehicle_physics_on_seat_change();
}

namespace
{
    // int16 across [-pi, pi], finer than the stock row's own angle slots.
    constexpr float vehicle_orient_quant = 32767.0f / std::numbers::pi_v<float>;

    int16_t vehicle_quantize_angle(float radians)
    {
        return static_cast<int16_t>(std::clamp(radians * vehicle_orient_quant, -32767.0f, 32767.0f));
    }

    float vehicle_dequantize_angle(int16_t quantized)
    {
        return static_cast<float>(quantized) / vehicle_orient_quant;
    }

    // WIRE-FROZEN range: one signed byte across a FIXED +-0.75 rad, NOT the sending class's
    // steer_lock, so a shipped receiver reconstructs the angle from the byte alone.
    constexpr float vehicle_steer_quant_range = 0.75f;
    constexpr float vehicle_steer_quant = 127.0f / vehicle_steer_quant_range;

    int8_t vehicle_quantize_steer(float radians)
    {
        return static_cast<int8_t>(std::clamp(radians * vehicle_steer_quant, -127.0f, 127.0f));
    }

    float vehicle_dequantize_steer(int8_t quantized)
    {
        return static_cast<float>(quantized) / vehicle_steer_quant;
    }

    // The interp matrix is authoritative for a watched hull; physics_update_entity's rebuild of it
    // must be undone.
    bool vehicle_interp_owns_orient(const rf::Entity* ep)
    {
        return ep && (ep->p_data.flags & rf::PF_NET_PLAYER) && vehicle_is_synced_entity_type(ep);
    }
} // namespace

// NO Euler decomposition: eye pitch/bank are applied as body-relative rotations about the hull's own
// right (0x004FD240) and forward (0x004FD310) axes, a decomposition being undefined near vertical.
void vehicle_rebuild_eye_orient(rf::Entity* ep, const rf::Matrix3& hull)
{
    const rf::EntityControlData& cd = ep->control_data;
    ep->eye_orient = hull;
    if (ep->p_data.flags & rf::PF_AUTOMOBILE) {
        AddrCaller{0x004FD240}.this_call<void>(&ep->eye_orient,
                                              cd.automobile_eye_phb.x + cd.eye_phb.x);
        AddrCaller{0x004FD310}.this_call<void>(&ep->eye_orient,
                                              cd.automobile_eye_phb.z + cd.eye_phb.z);
        return;
    }
    AddrCaller{0x004FD240}.this_call<void>(&ep->eye_orient, cd.eye_phb.x);
    AddrCaller{0x004FD310}.this_call<void>(&ep->eye_orient, cd.eye_phb.z);
}

namespace
{
    // Also true while Bullet drives: physics_make_orient carries no bank and flattens banked turns.
    bool vehicle_orient_rebuild_must_be_undone(const rf::Entity* ep)
    {
        return vehicle_interp_owns_orient(ep) || vehicle_physics_drives(ep);
    }

    // eased = the continuous view frame; raw = the newest angles, for the frame a shot is read off.
    enum class VehicleAimSource
    {
        eased,
        raw,
    };

    void vehicle_apply_aim_orient(rf::Entity* vehicle, VehicleAimSource source);

    FunHook<void(rf::Entity*)> physics_update_entity_hook{
        0x0049FE40,
        [](rf::Entity* ep) {
            // Every entity, every physics frame, in SP and in a vehicle-free match.
            if (!rf::is_multi || !vehicle_level_has_factories()) {
                physics_update_entity_hook.call_target(ep);
                return;
            }
            vehicle_feed_automobile_eye_input(ep);
            // A collide_out written into a hull this machine only watches clips its interp target.
            // Exempt a record naming the LOCAL player: that one fires entity_crush_damage.
            if (rf::is_multi && vehicle_is_synced_entity_type(ep)
                && (ep->p_data.flags & rf::PF_NET_PLAYER) && !vehicle_physics_drives(ep)
                && !(ep->p_data.collide_out.hit_time < 1.0f && rf::local_player_entity
                     && ep->p_data.collide_out.obj_handle == rf::local_player_entity->handle)) {
                ep->p_data.collide_out.hit_time = 1.0f;
            }
            if (!vehicle_orient_rebuild_must_be_undone(ep)) {
                physics_update_entity_hook.call_target(ep);
                return;
            }
            const rf::Matrix3 authoritative_orient = ep->p_data.orient;
            physics_update_entity_hook.call_target(ep);
            ep->p_data.orient = authoritative_orient;
            ep->p_data.next_orient = authoritative_orient;
            ep->orient = authoritative_orient;
            if (vehicle_hull_is_turret(ep)) {
                return;
            }
            vehicle_rebuild_eye_orient(ep, authoritative_orient);
            vehicle_apply_aim_orient(ep, VehicleAimSource::eased); // continuous view frame
        },
    };

    // The lag-comp rewind rebuilds eye_orient through the wrong convention, after the hook above.
    FunHook<void(rf::Entity*)> multi_lag_comp_rewind_entity_hook{
        0x0046FD00,
        [](rf::Entity* ep) {
            const bool turret = vehicle_is_synced_entity_type(ep) && vehicle_hull_is_turret(ep);
            // Stock never rewinds the local shooter, but a turret's shots name the hull, not him.
            if (turret && vehicle_local_owns_firing_seat(ep)) {
                return;
            }
            multi_lag_comp_rewind_entity_hook.call_target(ep);
            if (vehicle_is_synced_entity_type(ep) && !turret) {
                vehicle_rebuild_eye_orient(ep, ep->orient);
                // Raw, not eased: this runs only to read a muzzle at fire time.
                vehicle_apply_aim_orient(ep, VehicleAimSource::raw);
            }
        },
    };

    void vehicle_send_orient_supplement(rf::Entity* vehicle, const rf::Vector3& phb, uint16_t tick);

    bool vehicle_push_local_interp_sample(rf::Entity* vehicle,
                                          std::optional<uint16_t> tick_override = std::nullopt);
} // namespace

// Non-driving machines only: interp_rotation lerps the whole phb keyframe, so writing pitch and bank
// into the ring is all the stock reconstruction needs.
void vehicle_decorate_interp_orient(rf::Entity* vehicle)
{
    if (!vehicle || !vehicle->obj_interp || !(vehicle->p_data.flags & rf::PF_NET_PLAYER)) {
        return;
    }
    const VehicleOrientSupplement* supp_ptr = vehicle_orient_supplement(vehicle->handle);
    if (!supp_ptr) {
        return;
    }
    const VehicleOrientSupplement& supp = *supp_ptr;
    rf::ObjInterp* interp = vehicle->obj_interp;
    const int num = interp->num_frames();
    for (int i = 0; i < num; ++i) {
        // 16-bit tick subtraction: everything at or after this sample, nothing older.
        if (static_cast<int16_t>(interp->time_array[i] - supp.tick) >= 0) {
            interp->phb_array[i].x = supp.pitch;
            interp->phb_array[i].z = supp.bank;
        }
    }
    // interp_rotation falls back to the entity's own control_data.phb while the ring holds
    // fewer than two keyframes (0x00484300), so that copy needs the same two angles.
    vehicle->control_data.phb.x = supp.pitch;
    vehicle->control_data.phb.z = supp.bank;
}

namespace
{
    bool vehicle_vector_is_finite(const rf::Vector3& v)
    {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }

    // The keyframe came off a driving client's obj_update row, so nothing in it is trusted.
    bool vehicle_row_is_client_authored(const rf::Entity* ep)
    {
        return rf::is_server && vehicle_is_synced_entity_type(ep)
            && vehicle_seat_leech(ep, 0) != -1 && vehicle_local_driven_vehicle() != ep;
    }

    // Bytes (127, 127) drive the decoder's z = sqrt(1 - (x*x + y*y)) (0x0047E140-0x0047E1DD) to NaN.
    void vehicle_clamp_row_velocity(const rf::Entity* ep, rf::Vector3& vel)
    {
        const float limit = vehicle_physics_class_max_speed(vehicle_damage_class(ep));
        const float speed = vel.len();
        if (speed <= limit) {
            return;
        }
        vel = speed > 0.0f ? vel * (limit / speed) : rf::Vector3{};
    }

    // Above any live stream's arrival gap; a parked hull's next row always exceeds it.
    constexpr uint32_t vehicle_interp_rest_gap_ms = 500;

    // The insert logs a rest as one arrival gap, whose 20-sample average x2.2 (0x00483702) is the
    // re-anchor headroom: interp_time would then trail the row by up to the whole rest.
    void vehicle_interp_absorb_rest_gap(rf::ObjInterp* interp)
    {
        if (interp->num_frames() == 0 || interp->last_update_time == static_cast<uint32_t>(-1)) {
            return;
        }
        const auto now_ms = static_cast<uint32_t>(timer::get_i64(1000));
        if (now_ms - interp->last_update_time <= vehicle_interp_rest_gap_ms) {
            return;
        }
        const float typical_gap =
            std::clamp(interp->arrive_time_avg_diff, 0.0f, static_cast<float>(vehicle_interp_rest_gap_ms));
        interp->last_update_time = now_ms - static_cast<uint32_t>(typical_gap);
        interp->flags |= 1u; // force the re-anchor on this insert
    }

    // Reached only from the engine's RECEIVE path: this machine's own keyframes go in through
    // call_target below, so a row that gets here could not carry pitch or bank.
    FunHook<void __fastcall(rf::ObjInterp*, int, rf::Entity*, rf::Vector3*, rf::Vector3*,
                            rf::Vector3*, rf::Vector3*, rf::Vector3*, int, float)>
        obj_interp_set_next_pos_orient_hook{
            obj_interp_set_next_pos_orient_addr,
            [](rf::ObjInterp* self, int edx, rf::Entity* ep, rf::Vector3* pos, rf::Vector3* phb,
               rf::Vector3* eye_phb, rf::Vector3* vel, rf::Vector3* move, int time,
               float always_0) FASTCALL_LAMBDA {
                if (!rf::is_multi || !vehicle_level_has_factories()) {
                    obj_interp_set_next_pos_orient_hook.call_target(self, edx, ep, pos, phb, eye_phb,
                                                                    vel, move, time, always_0);
                    return;
                }
                if (ep && pos && vel && vehicle_row_is_client_authored(ep)) {
                    if (!vehicle_vector_is_finite(*pos) || !vehicle_vector_is_finite(*vel)) {
                        return;
                    }
                    vehicle_clamp_row_velocity(ep, *vel);
                    vehicle_note_observed_speed(ep->handle, vel->len());
                }
                if (ep && pos && phb && vehicle_is_synced_entity_type(ep) && !vehicle_physics_drives(ep)) {
                    vehicle_note_observed_pose(ep->handle, *pos, phb->y);
                }
                if (ep && phb && (ep->p_data.flags & rf::PF_NET_PLAYER)
                    && vehicle_is_synced_entity_type(ep)) {
                    if (const VehicleOrientSupplement* supp = vehicle_orient_supplement(ep->handle)) {
                        phb->x = supp->pitch;
                        phb->z = supp->bank;
                    }
                }
                if (ep && (ep->p_data.flags & rf::PF_NET_PLAYER) && vehicle_is_synced_entity_type(ep)) {
                    vehicle_interp_absorb_rest_gap(self);
                }
                obj_interp_set_next_pos_orient_hook.call_target(self, edx, ep, pos, phb, eye_phb,
                                                                vel, move, time, always_0);
            },
        };

    // pack_obj_update_data serializes a non-local entity out of its own ObjInterp ring, and the
    // vehicle this machine drives has an empty one - so seed it. True when a keyframe was inserted.
    bool vehicle_push_local_interp_sample(rf::Entity* vehicle, std::optional<uint16_t> tick_override)
    {
        if (!vehicle || !vehicle->obj_interp) {
            return false;
        }
        rf::Vector3 pos = vehicle->pos;
        rf::Vector3 phb = vehicle_matrix_phb(vehicle->orient);
        rf::Vector3 eye_phb = vehicle->control_data.eye_phb;
        rf::Vector3 vel = vehicle->p_data.vel;
        rf::Vector3 move = vehicle->ai.ci.move;
        const auto tick = tick_override.value_or(static_cast<uint16_t>(timer::get_i64(1000)));
        // The server packs one row per recipient; the tick keeps one frame from filling the ring.
        rf::ObjInterp* interp = vehicle->obj_interp;
        const bool inserted = interp->num_frames() == 0 || interp->newest_frame_time() != tick;
        if (inserted) {
            // call_target, never the raw address: an authored keyframe must not re-enter the hook.
            obj_interp_set_next_pos_orient_hook.call_target(interp, 0, vehicle, &pos, &phb,
                                                            &eye_phb, &vel, &move,
                                                            static_cast<int>(tick), 0.0f);
        }
        vehicle_send_orient_supplement(vehicle, phb, tick);
        return inserted;
    }

    // Where a SERVER-SIMULATED hull is called stopped. Angular is separate: a hull can spin in place.
    constexpr float vehicle_server_settle_speed = 0.35f;
    constexpr float vehicle_server_settle_ang = 0.25f; // rad/s
    // Below the settle speeds a row goes out only once the hull has crept this far from the last one.
    constexpr float vehicle_server_row_pos_tolerance = 0.01f;
    constexpr float vehicle_server_row_axis_tolerance = 0.005f; // axis chord, ~rad
} // namespace

namespace
{
    bool vehicle_server_samples_vehicle(rf::Entity* vehicle);
} // namespace

// Server: replication only, for a fully-unmanned non-dying hull the server simulates in Bullet. The
// physics post-step has already written pose, orientation and room into the entity.
void vehicle_server_body_do_frame(rf::Entity* ep)
{
    rf::Vector3 pos{};
    float linear_speed = 0.0f;
    float angular_speed = 0.0f;
    bool asleep = false;
    if (!vehicle_physics_server_state(ep->handle, &pos, &linear_speed, &angular_speed, &asleep)) {
        g_vehicle_state.kinematics.erase(ep->handle);
        return;
    }

    auto it = g_vehicle_state.kinematics.find(ep->handle);
    const bool has = it != g_vehicle_state.kinematics.end();
    const bool was_active = has && it->second.active;
    bool tick_seeded = has && it->second.tick_seeded;
    uint16_t base_tick = has ? it->second.base_tick : 0;
    int64_t base_time_ms = has ? it->second.base_time_ms : 0;

    // When a hull becomes unmanned the ring still holds the driver's last relayed keyframe, which is
    // what every watcher is anchored to. A never-driven hull has an empty ring: the live clock.
    if (!tick_seeded && ep->obj_interp && ep->obj_interp->num_frames() > 0) {
        base_tick = ep->obj_interp->newest_frame_time();
        base_time_ms = timer::get_i64(1000);
        tick_seeded = true;
    }
    std::optional<uint16_t> coast_tick;
    if (tick_seeded) {
        const int64_t now_ms = timer::get_i64(1000);
        coast_tick = static_cast<uint16_t>(base_tick + (now_ms - base_time_ms));
        // Strictly after the ring's newest in 16-bit order, or no row is inserted at all.
        const rf::ObjInterp* ring = ep->obj_interp;
        if (ring && ring->num_frames() > 0
            && static_cast<int16_t>(*coast_tick - ring->newest_frame_time()) <= 0) {
            base_tick = static_cast<uint16_t>(ring->newest_frame_time() + 1);
            base_time_ms = now_ms;
            coast_tick = base_tick;
        }
    }

    // The speed test is the settle verdict; Bullet sleeps only after 2 s below its own thresholds.
    const bool moving = !asleep
                     && (linear_speed > vehicle_server_settle_speed
                         || angular_speed > vehicle_server_settle_ang);

    // Exactly one author per ring: FULLY UNMANNED is ours, OCCUPIED is the send injection's. Two
    // clocks in one ring alternately freeze and warp the hull on every watcher.
    const bool ours_to_sample = !vehicle_server_samples_vehicle(ep);

    // Not "until asleep": a body touching a mover is re-activated every step and never sleeps.
    const bool drifted = !has || !it->second.row_valid
        || (ep->pos - it->second.row_pos).len() > vehicle_server_row_pos_tolerance
        || (ep->orient.fvec - it->second.row_orient.fvec).len() > vehicle_server_row_axis_tolerance
        || (ep->orient.uvec - it->second.row_orient.uvec).len() > vehicle_server_row_axis_tolerance;
    const bool fell_asleep = asleep && has && !it->second.asleep;
    const bool creep_row = !moving && !was_active && (drifted || fell_asleep);

    bool pushed = false;
    if (moving) {
        ep->move(&pos);
        ep->update_room();
        if (ours_to_sample) {
            pushed = vehicle_push_local_interp_sample(ep, coast_tick);
        }
    }
    else if (was_active) {
        // Just settled: zero ALL of the hull's motion and author a row at the current position.
        ep->p_data.vel = rf::Vector3{};
        ep->p_data.rotvel = rf::Vector3{};
        ep->p_data.ang_momentum = rf::Vector3{};
        ep->ai.ci.move = rf::Vector3{};
        ep->control_data.phb = vehicle_matrix_phb(ep->orient);
        ep->move(&pos);
        ep->update_room();
        if (ours_to_sample) {
            pushed = vehicle_push_local_interp_sample(ep, coast_tick);
        }
    }
    else if (creep_row && ours_to_sample) {
        pushed = vehicle_push_local_interp_sample(ep, coast_tick);
    }

    VehicleKinematics& k = g_vehicle_state.kinematics[ep->handle];
    k.pos = pos;
    k.pos_valid = true;
    k.active = moving;
    k.base_tick = base_tick;
    k.base_time_ms = base_time_ms;
    k.tick_seeded = tick_seeded;
    k.broadcast = moving || was_active || creep_row;
    k.asleep = asleep;
    // The send injection samples an occupied hull live, so its watchers are never behind.
    if (pushed || !ours_to_sample) {
        k.row_pos = ep->pos;
        k.row_orient = ep->orient;
        k.row_valid = true;
    }

    // The memory window and the crush window are ONE window, ended by the same settle verdict that
    // ends the full-rate samples. Last in the frame, so the record above is written whatever obj_damage does.
    if (!moving) {
        g_vehicle_state.coast_memory.erase(ep->handle);
    }
    else if (linear_speed > vehicle_crush_min_speed) {
        vehicle_server_coast_crush_sweep(ep, pos);
    }
}

namespace
{
    // True while a coasting/sinking hull is moving, and on the settle, creep and sleep-edge frames.
    bool vehicle_is_kinematically_broadcast(rf::Entity* ep)
    {
        auto it = g_vehicle_state.kinematics.find(ep->handle);
        return it != g_vehicle_state.kinematics.end() && it->second.broadcast;
    }

    // ~0.08 s time constant: bridges two obj_update samples without trailing the hull.
    constexpr float vehicle_aim_ease_rate = 13.0f;
} // namespace

// Wrap-aware on heading, which is an atan2 and discontinuous at +-pi; pitch is an asin and cannot
// wrap. The first sample snaps.
void vehicle_ease_aim_do_frame()
{
    // No view here, and leaving eased_valid false makes every application fall back to raw.
    if (rf::is_dedicated_server) {
        return;
    }
    const float t = 1.0f - std::exp(-vehicle_aim_ease_rate * rf::frametime);
    for (auto& entry : g_vehicle_state.orient) {
        VehicleOrientSupplement& supp = entry.second;
        if (!supp.valid) {
            continue;
        }
        if (!supp.eased_valid) {
            supp.eased_aim_pitch = supp.aim_pitch;
            supp.eased_aim_head = supp.aim_head;
            supp.eased_valid = true;
            continue;
        }
        float head_delta = supp.aim_head - supp.eased_aim_head;
        while (head_delta > vehicle_two_pi * 0.5f) {
            head_delta -= vehicle_two_pi;
        }
        while (head_delta < -vehicle_two_pi * 0.5f) {
            head_delta += vehicle_two_pi;
        }
        supp.eased_aim_head += head_delta * t;
        // Fold back into [-pi, pi]: an unbounded value loses trig precision over a long session.
        if (supp.eased_aim_head > vehicle_two_pi * 0.5f) {
            supp.eased_aim_head -= vehicle_two_pi;
        }
        else if (supp.eased_aim_head < -vehicle_two_pi * 0.5f) {
            supp.eased_aim_head += vehicle_two_pi;
        }
        supp.eased_aim_pitch += (supp.aim_pitch - supp.eased_aim_pitch) * t;
    }
}

namespace
{
    // Rebuild BOTH frames a vehicle's guns can fire along - the hull's and the rider's, which
    // entity_get_weapon_fire_pos_orient (0x0041B040) picks between on WTF_FROM_EYE. On the DRIVING
    // machine only a third-person convergence aim replaces the rebuilt frame.
    void vehicle_apply_aim_orient(rf::Entity* vehicle, VehicleAimSource source)
    {
        if (!vehicle || !vehicle_physics_class_syncs_driver_aim(vehicle)) {
            return;
        }
        // An unmanned hull has no consumer: the supplement is the ex-driver's last aim.
        rf::Entity* driver = vehicle_driver_entity(vehicle);
        if (!driver) {
            return;
        }
        // make_quick (0x004FCFA0) builds an UPRIGHT frame, so the roll is recovered below; fvec is
        // never touched (the fire-agreement contract). ONE read, shared with the cockpit pose.
        const rf::Matrix3 hull = vehicle->orient;
        rf::Vector3 dir{};
        if (driver == rf::local_player_entity) {
            if (!vehicle_physics_camera_driver_aim(vehicle, driver, &dir)) {
                return;
            }
        }
        else {
            VehicleOrientSupplement* supp_ptr = vehicle_orient_supplement(vehicle->handle);
            if (!supp_ptr) {
                return;
            }
            const VehicleOrientSupplement& supp = *supp_ptr;
            const bool use_eased = source == VehicleAimSource::eased && supp.eased_valid;
            const float aim_pitch = use_eased ? supp.eased_aim_pitch : supp.aim_pitch;
            const float aim_head = use_eased ? supp.eased_aim_head : supp.aim_head;
            const float cp = std::cos(aim_pitch);
            dir = rf::Vector3{cp * std::sin(aim_head), std::sin(aim_pitch), cp * std::cos(aim_head)};
            supp_ptr->eye_hull_orient = hull;
            supp_ptr->eye_hull_valid = true;
        }

        rf::Matrix3& eye = vehicle->eye_orient;
        eye.make_quick(dir);
        rf::Vector3 up = hull.uvec;
        up -= eye.fvec * up.dot_prod(eye.fvec);
        // Aiming along the hull's own up axis leaves no roll to recover: keep make_quick's frame.
        constexpr float min_up_len = 0.01f;
        const float up_len = up.len();
        if (up_len >= min_up_len) {
            up /= up_len;
            eye.uvec = up;
            eye.rvec = up.cross(eye.fvec); // r = u x f, the convention make_quick itself builds
        }
        driver->eye_orient = eye;
    }

    void vehicle_store_orient(rf::Entity* vehicle, uint16_t tick, float pitch, float bank,
                              float aim_pitch, float aim_head, float steer)
    {
        VehicleOrientSupplement& supp = g_vehicle_state.orient[vehicle->handle];
        // Out-of-order arrivals must not undo a newer sample.
        const int64_t now_ms = timer::get_i64(1000);
        if (supp.valid && now_ms - supp.stored_ms < vehicle_orient_stale_ms
            && static_cast<int16_t>(tick - supp.tick) < 0) {
            return;
        }
        // interp_rotation lerps phb component-wise, so bank needs a continuous branch across +-pi.
        if (supp.valid) {
            while (bank - supp.bank > vehicle_two_pi * 0.5f) {
                bank -= vehicle_two_pi;
            }
            while (bank - supp.bank < -vehicle_two_pi * 0.5f) {
                bank += vehicle_two_pi;
            }
        }
        supp.pitch = pitch;
        supp.bank = bank;
        supp.aim_pitch = aim_pitch;
        supp.aim_head = aim_head;
        supp.steer = steer;
        supp.tick = tick;
        supp.stored_ms = now_ms;
        supp.valid = true;
        vehicle_decorate_interp_orient(vehicle);
        // Immediately: a trigger can be pulled on a frame with no physics tick behind it.
        vehicle_apply_aim_orient(vehicle, VehicleAimSource::eased);
    }

    // Driver side. One supplement per obj_update sample, deduped by tick.
    void vehicle_send_orient_supplement(rf::Entity* vehicle, const rf::Vector3& phb, uint16_t tick)
    {
        VehicleOrientSend& sent = g_vehicle_state.orient_sent[vehicle->handle];
        if (sent.valid && sent.tick == tick) {
            return;
        }
        const int16_t pitch = vehicle_quantize_angle(phb.x);
        const int16_t bank = vehicle_quantize_angle(phb.z);
        // Hull-forward for a class whose aim is not synced and for a driverless hull, so a receiver
        // never has to guess.
        float aim_pitch_f = 0.0f;
        float aim_head_f = 0.0f;
        const rf::Entity* driver = vehicle_physics_class_syncs_driver_aim(vehicle)
            ? vehicle_driver_entity(vehicle)
            : nullptr;
        // The HULL's eye frame, NEVER the rider's copy: the on-foot look path rewrites the rider's
        // from angles that stop receiving input the moment he boards.
        const rf::Vector3& f = driver ? vehicle->eye_orient.fvec : vehicle->orient.fvec;
        aim_pitch_f = std::asin(std::clamp(f.y, -1.0f, 1.0f));
        aim_head_f = std::atan2(f.x, f.z);
        const int16_t aim_pitch = vehicle_quantize_angle(aim_pitch_f);
        const int16_t aim_head = vehicle_quantize_angle(aim_head_f);
        // From the Bullet body this machine is stepping; zero everywhere else means wheels straight.
        float steer_f = 0.0f;
        vehicle_physics_driven_steer_angle(vehicle->handle, &steer_f);
        const int8_t steer = vehicle_quantize_steer(steer_f);
        // A level hull has nothing the stock row cannot carry - but aim and steer have no other
        // carrier at all, so all three must be unchanged before a supplement may be skipped.
        // Receivers compare 16-bit ticks, so the keep-alive stays well inside half their range.
        if (pitch == 0 && bank == 0 && sent.valid && sent.pitch == 0 && sent.bank == 0
            && sent.aim_pitch == aim_pitch && sent.aim_head == aim_head && sent.steer == steer
            && sent.keepalive.valid() && !sent.keepalive.elapsed()) {
            sent.tick = tick;
            return;
        }
        sent = VehicleOrientSend{pitch, bank, aim_pitch, aim_head, steer, tick, true};
        sent.keepalive.set(vehicle_orient_keepalive_ms);

        // Stored on the sending machine too, for the keyframe decoration and the last-sent
        // bookkeeping; the eye_orient half only re-asserts a third-person APC driver's convergence aim.
        vehicle_store_orient(vehicle, tick, vehicle_dequantize_angle(pitch),
                             vehicle_dequantize_angle(bank), vehicle_dequantize_angle(aim_pitch),
                             vehicle_dequantize_angle(aim_head), vehicle_dequantize_steer(steer));

        if (rf::is_server) {
            af_send_vehicle_orient_packet_to_all(nullptr, vehicle->handle, tick, pitch, bank,
                                                 aim_pitch, aim_head, steer);
        }
        else {
            af_send_vehicle_orient_request(vehicle->server_handle, tick, pitch, bank, aim_pitch,
                                           aim_head, steer);
        }
    }
} // namespace

// For the eye_orient rebuilder in the other translation unit: the driver's aim goes back on top.
void vehicle_refresh_aim_orient(rf::Entity* vehicle)
{
    vehicle_apply_aim_orient(vehicle, VehicleAimSource::eased);
}

// The vehicle whose weapon the local player's own fire input drives, or null.
rf::Entity* vehicle_local_firing_vehicle()
{
    rf::Entity* rider = rf::local_player_entity;
    if (!rider || rf::entity_is_dying(rider)) {
        return nullptr;
    }
    rf::Entity* vehicle = vehicle_ridden_live_hull(rider);
    return vehicle && vehicle_firing_seat_occupant(vehicle) == rider ? vehicle : nullptr;
}

namespace
{
    // force skips the engine's own fire-wait gate, which is what replaying a server-paced shot
    // needs. The two fields read below are written by the engine only if a projectile was created.
    bool vehicle_fire_discrete(rf::Entity* vehicle, bool alt, bool force)
    {
        // The one place every authoritative mint AND every watcher replay passes through.
        if (vphys_hull_submerged(vehicle)) {
            return false;
        }
        // Only the server's ammo is authoritative; every client call is a replay of a shot or burst
        // the server already validated.
        if (!force && rf::is_server) {
            const int weapon = alt && vehicle->info->use_function == rf::ENTITY_USE_VEHICLE
                ? vehicle->ai.current_secondary_weapon
                : vehicle->ai.current_primary_weapon;
            if (vehicle_weapon_ammo(vehicle, weapon) == 0) { // -1 = no pool, never empty
                return false;
            }
        }
        // RAW for the instant the muzzle is read; the next frame's application puts the eased back.
        vehicle_apply_aim_orient(vehicle, VehicleAimSource::raw);
        if (alt && vehicle->info->use_function == rf::ENTITY_USE_VEHICLE) {
            const int before = vehicle->ai.next_fire_secondary.value;
            rf::entity_fire_secondary_weapon(vehicle, force ? 1 : 0);
            return vehicle->ai.next_fire_secondary.value != before;
        }
        const float before = vehicle->ai.last_fire_time;
        rf::entity_fire_weapon(vehicle, force ? 1 : 0, alt ? 1 : 0, nullptr, nullptr, nullptr);
        return vehicle->ai.last_fire_time != before;
    }

    // The two triggers drive different fire-wait timestamps (AiInfo::next_fire_primary /
    // next_fire_secondary), so holding both is not a conflict.
    void vehicle_server_apply_trigger(rf::Entity* vehicle, bool alt, bool held, uint8_t requester_id)
    {
        // Only a vehicle's alt trigger has a weapon of its own; a turret's is a mode of its primary.
        const bool use_secondary = alt && vehicle->info->use_function == rf::ENTITY_USE_VEHICLE;
        const int weapon = use_secondary ? vehicle->ai.current_secondary_weapon
                                         : vehicle->ai.current_primary_weapon;
        if (weapon < 0) {
            return;
        }

        // RELEASED, not refused: the engine's dry branch creates no projectile but never lets go of
        // the trigger. Exactly 0, never <= 0: -1 means "no ammo pool at all".
        if (held && vehicle_weapon_ammo(vehicle, weapon) == 0) {
            if (rf::entity_weapon_is_on(vehicle->handle, weapon)) {
                rf::entity_turn_weapon_off(vehicle->handle, weapon);
            }
            return;
        }

        if (!held) {
            // A trigger that shares the other's weapon must not cancel it.
            if ((!alt || use_secondary) && rf::entity_weapon_is_on(vehicle->handle, weapon)) {
                rf::entity_turn_weapon_off(vehicle->handle, weapon);
            }
            return;
        }

        if (!use_secondary && rf::weapon_is_on_off_weapon(weapon, alt)) {
            // Turning the gun on sends the START other machines replay. The direct fire below shares
            // next_fire_primary with entity_process_post's loop-fire, so the two never double the rate.
            if (!rf::entity_weapon_is_on(vehicle->handle, weapon)) {
                // Every turn-on is a reliable START to every client, so a trigger toggled each frame
                // must not re-arm faster than this. Held, it re-arms the frame the window lapses.
                rf::Timestamp& rearm = g_vehicle_state.fire_rearm[vehicle->handle];
                if (rearm.valid() && !rearm.elapsed()) {
                    return;
                }
                rf::entity_turn_weapon_on(vehicle->handle, weapon, alt);
                // The drill ignores its fire-wait and makes no projectile, so only the floor applies to it.
                const int fire_wait = rf::entity_is_driller(vehicle) ? 0 : rf::weapon_get_fire_wait_ms(weapon, alt);
                rearm.set(std::max(fire_wait, vehicle_fire_rearm_floor_ms));
            }
            // Stock entity_process_post carves too (0x0041EA82), but this direct call is
            // load-bearing: keep BOTH. A double carve is harmless, the internal pacing bounds it.
            if (rf::is_server && rf::entity_is_driller(vehicle)) {
                rf::entity_maybe_drill_geomod(vehicle);
                // Stock has no drill damage at all, so this is the whole of it.
                vehicle_server_drill_damage_sweep(vehicle);
            }
            vehicle_fire_discrete(vehicle, alt, false);
            return;
        }

        if (!vehicle_fire_discrete(vehicle, alt, false)) {
            return;
        }
        // Nothing stock announces a shot from a non-player entity; the requester already fired his.
        af_send_vehicle_fire_packet_to_all(rf::multi_find_player_by_id(requester_id),
                                           vehicle->handle, AF_VEHICLE_FIRE_SHOT, alt ? 1 : 0);
    }
} // namespace

void vehicle_server_apply_fire(rf::Entity* vehicle, const VehicleFireState& state)
{
    // Blacked out: hand both channels a RELEASED trigger rather than returning - a continuous weapon
    // already on stays on. The stored state is left alone, so surfacing re-asserts a held trigger.
    if (vphys_hull_submerged(vehicle)) {
        vehicle_server_apply_trigger(vehicle, false, false, state.requester_id);
        vehicle_server_apply_trigger(vehicle, true, false, state.requester_id);
        return;
    }
    // A turret's two channels address ONE weapon, so alt-held + primary-released would switch it off
    // then on every frame. One decision from either trigger, primary winning when both are down.
    if (vehicle->info->use_function != rf::ENTITY_USE_VEHICLE) {
        const bool alt_mode = state.alt_held && !state.primary_held;
        vehicle_server_apply_trigger(vehicle, alt_mode, state.any_held(), state.requester_id);
        return;
    }
    vehicle_server_apply_trigger(vehicle, false, state.primary_held, state.requester_id);
    vehicle_server_apply_trigger(vehicle, true, state.alt_held, state.requester_id);
}

// Server: drop both channels whether or not a fire report was ever recorded - a weapon left on with
// nobody in the firing seat fires forever, here and, through its START, on every watcher.
void vehicle_server_stop_fire(rf::Entity* vehicle)
{
    if (!rf::is_server || !vehicle) {
        return;
    }
    vehicle_server_apply_fire(vehicle, VehicleFireState{});
    g_vehicle_state.fire.erase(vehicle->handle);
}

// Everyone, not just the occupants: a spectator's HUD draws the bar for the man he is watching, and
// the demo recorder is a virtual observer player.
void vehicle_broadcast_health(rf::Entity* vehicle, bool is_reliable)
{
    af_send_vehicle_health_packet_to_all(vehicle->handle, vehicle->life, vehicle_hud_max_life(vehicle),
                                         vehicle_weapon_ammo(vehicle, vehicle->ai.current_primary_weapon),
                                         vehicle_weapon_ammo(vehicle, vehicle->ai.current_secondary_weapon),
                                         is_reliable);
}

// Streaming UNRELIABLE, settling RELIABLE: each unreliable packet supersedes the last, so only the
// LAST packet of a run is not self-correcting. Ammo shares the packet, compared for ANY change.
void vehicle_server_sync_health(rf::Entity* vehicle)
{
    VehicleHealthSync& sync = g_vehicle_state.health_sync[vehicle->handle];
    const int primary = vehicle_weapon_ammo(vehicle, vehicle->ai.current_primary_weapon);
    const int secondary = vehicle_weapon_ammo(vehicle, vehicle->ai.current_secondary_weapon);
    const int64_t now = timer::get_i64(1000);

    const bool changed = std::fabs(vehicle->life - sync.last_sent_life) >= 1.0f
        || primary != sync.last_sent_primary_ammo || secondary != sync.last_sent_secondary_ammo;

    if (changed) {
        if (sync.next_send.valid() && !sync.next_send.elapsed()) {
            return; // the rate floor; the change goes out next window
        }
        sync.last_sent_life = vehicle->life;
        sync.last_sent_primary_ammo = primary;
        sync.last_sent_secondary_ammo = secondary;
        sync.next_send.set(vehicle_health_send_interval_ms);
        sync.last_change_ms = now;
        sync.settled_sent = false;
        vehicle_broadcast_health(vehicle, false);
        return;
    }

    if (!sync.settled_sent && now - sync.last_change_ms >= vehicle_health_settle_ms) {
        sync.settled_sent = true;
        vehicle_broadcast_health(vehicle, true);
    }
}

void vehicle_drop_combat_state(int vehicle_handle)
{
    g_vehicle_state.fire.erase(vehicle_handle);
    g_vehicle_state.fire_rearm.erase(vehicle_handle);
    g_vehicle_state.health_sync.erase(vehicle_handle);
    g_vehicle_state.last_damager.erase(vehicle_handle);
    g_vehicle_state.lethal_killer.erase(vehicle_handle);
    g_vehicle_state.observed_speed.erase(vehicle_handle);
    g_vehicle_state.crash_cooldown.erase(vehicle_handle);
    g_vehicle_state.health.erase(vehicle_handle);
    g_vehicle_state.ammo_mirror.erase(vehicle_handle);
    g_vehicle_state.hull_state.erase(vehicle_handle);
    g_vehicle_state.orient.erase(vehicle_handle);
    g_vehicle_state.orient_sent.erase(vehicle_handle);
    g_vehicle_state.kinematics.erase(vehicle_handle);
    g_vehicle_state.void_timer.erase(vehicle_handle);
    g_vehicle_state.hazard_since.erase(vehicle_handle);
    g_vehicle_state.coast_memory.erase(vehicle_handle);
    g_vehicle_state.regen.erase(vehicle_handle);
    g_vehicle_state.pending_seats.erase(vehicle_handle);
    g_vehicle_state.wheel_spin.erase(vehicle_handle);
    g_vehicle_state.tread_scroll.erase(vehicle_handle);
    // Swept, not erased: a latched pair left behind would suppress a later ram on a recycled handle.
    std::erase_if(g_vehicle_state.ram_pairs, [vehicle_handle](const auto& entry) {
        return entry.first.first == vehicle_handle || entry.first.second == vehicle_handle;
    });
    vehicle_physics_server_release(vehicle_handle);
}

namespace
{
    // In MP the physics step runs world collision for the local player entity and non-entity objects
    // only. Injected on the OT_ENTITY half of that filter; EAX is the object. Remote entities must
    // keep being skipped or they fight ObjInterp.
    CodeInjection physics_world_collision_vehicle_injection{
        0x0048788E,
        [](auto& regs) {
            rf::Object* obj = regs.eax;
            if (obj && obj->type == rf::OT_ENTITY && vehicle_level_has_factories()
                && vehicle_local_driven_vehicle() == static_cast<rf::Entity*>(obj)) {
                regs.eip = 0x00487895; // run the collision
            }
        },
    };

    // A slept body never evaluates its interp, which is what froze the driverless coast. Clients
    // only, unmanned only: force-simulating an occupied hull skips the driller's halt at 0x00488097.
    bool vehicle_interp_needs_physics_wake(rf::Entity* ep)
    {
        if (rf::is_server || !(ep->p_data.flags & rf::PF_NET_PLAYER)
            || !vehicle_is_synced_entity_type(ep) || rf::entity_get_first_leech(ep) != -1) {
            return false;
        }
        const rf::ObjInterp* ring = ep->obj_interp;
        if (!ring || ring->num_frames() == 0) {
            return false;
        }
        // 16-bit tick arithmetic: strictly positive means keyframe still ahead of the evaluation
        // clock. Also the bound - once the server stops authoring, the hull may sleep again.
        return static_cast<int16_t>(ring->newest_frame_time() - ring->interp_time) > 0;
    }

    // A force-sim returns 1 without running the stock body and so skips the drill-contact halt. On
    // the server that halt is what parks the driller against the wall so the carve raycast hits.
    bool vehicle_driller_contact_halt(rf::Entity* ep)
    {
        return rf::is_server && (ep->entity_flags2 & 0x40) && rf::entity_is_driller(ep);
    }

    // Keeps the driven vehicle out of the idle/distance culling in obj_move_all.
    FunHook<int(rf::Object*)> obj_should_sim_physics_hook{
        0x00488030,
        [](rf::Object* obj) -> int {
            if (rf::is_multi && obj && obj->type == rf::OT_ENTITY && vehicle_level_has_factories()
                && !vehicle_driller_contact_halt(static_cast<rf::Entity*>(obj))
                && (vehicle_local_driven_vehicle() == static_cast<rf::Entity*>(obj)
                    || vehicle_interp_needs_physics_wake(static_cast<rf::Entity*>(obj)))) {
                return 1;
            }
            return obj_should_sim_physics_hook.call_target(obj);
        },
    };

    // The stock drill-contact halt suspends physics while a crater forms, which on an interp-driven
    // copy freezes ObjInterp instead. Ignored for that copy, CLIENTS ONLY. ESI is the object;
    // 0x004880AF is the "not contacting" continuation.
    CodeInjection driller_interp_no_movement_halt_injection{
        0x00488097,
        [](auto& regs) {
            rf::Object* obj = regs.esi;
            if (rf::is_multi && !rf::is_server && obj && obj->type == rf::OT_ENTITY
                && (static_cast<rf::Entity*>(obj)->p_data.flags & rf::PF_NET_PLAYER)
                && rf::entity_is_driller(static_cast<rf::Entity*>(obj))) {
                regs.eip = 0x004880AF;
            }
        },
    };

    // Lift the driller's 25-hole lifetime cap in MP by reporting driller_geomod_count as zero at the
    // load feeding its compare. eip skips the original MOV so EAX carries ours. is_multi, not
    // is_server: a client that still capped at 25 would stop carving craters the server has.
    CodeInjection driller_mp_geomod_cap_injection{
        0x004214E9,
        [](auto& regs) {
            rf::Entity* ep = static_cast<rf::Entity*>(regs.esi);
            regs.eax = rf::is_multi ? 0 : ep->driller_geomod_count;
            regs.eip = 0x004214EF;
        },
    };

    // A vehicle weapon's projectile spawns INSIDE the firing hull's collision, and stock drops only
    // the weapon<->parent pair, never the weapon<->OCCUPANT one. Keyed off the projectile's parent,
    // so an enemy hull has a different parent and stays hittable.
    // Stock also gives only a PLAYER's shot the mesh test against an entity (bit 2: entity is a, bit 4:
    // entity is b); a hull's shot met just the victim's cspheres, so rockets flew through hull gaps.
    FunHook<bool(rf::Object*, rf::Object*, unsigned*)> obj_pair_should_skip_hook{
        0x0048BE00,
        [](rf::Object* a, rf::Object* b, unsigned* out_flags) -> bool {
            rf::Object* weapon = nullptr;
            rf::Object* other = nullptr;
            rf::Entity* parent = nullptr;
            // Every collision pair in the level reaches this; the handle lookup below must not.
            if (rf::is_multi && a && b && vehicle_level_has_factories()) {
                weapon = a->type == rf::OT_WEAPON ? a
                       : b->type == rf::OT_WEAPON ? b
                                                  : nullptr;
                if (weapon) {
                    other = weapon == a ? b : a;
                    if (other->type == rf::OT_ENTITY) {
                        parent = vehicle_synced_entity(weapon->parent_handle);
                        if (parent
                            && (other->handle == parent->handle
                                || other->host_handle == parent->handle)) {
                            return true; // skip: the projectile passes through its own hull/riders
                        }
                    }
                }
            }
            const bool skip = obj_pair_should_skip_hook.call_target(a, b, out_flags);
            if (!skip && parent && other->vmesh) {
                *out_flags |= weapon == a ? 4u : 2u;
            }
            return skip;
        },
    };

    // "hide enemy bullets" answers "did the local player fire it?" by comparing an ENTITY against
    // local_player_entity, and a vehicle gun's shooter is the HULL. Both readers are corrected below.

    // MOV AL,[hide_enemy_bullets] in weapon_create; EBX is the parent object and 0x004C7C9A is the
    // not-hidden join, where obj_collision_register(projectile) runs.
    CodeInjection weapon_create_own_vehicle_bullet_injection{
        0x004C7C78,
        [](auto& regs) {
            rf::Object* parent = regs.ebx;
            if (parent && parent->type == rf::OT_ENTITY
                && vehicle_local_owns_firing_seat(static_cast<rf::Entity*>(parent))) {
                regs.eip = 0x004C7C9A;
            }
        },
    };

    // Same instruction in entity_fire_weapon, whose chain ends in obj_flag_dead(projectile). ESI is
    // the shooting hull; 0x004266C8 is the join past that call.
    CodeInjection entity_fire_weapon_own_vehicle_bullet_injection{
        0x0042668D,
        [](auto& regs) {
            rf::Entity* shooter = regs.esi;
            if (vehicle_local_owns_firing_seat(shooter)) {
                regs.eip = 0x004266C8;
            }
        },
    };

    // A watcher's fire is a replay of a shot the server already paid for; his ammo copy must not
    // refuse it. Stock's discrete remote replay passes pos, which skips the primary's gate, but its
    // loop-fire passes none and does gate a remote continuous weapon.
    bool vehicle_fire_skips_local_ammo_gate(rf::Entity* ep)
    {
        return rf::is_multi && !rf::is_server && vehicle_is_synced_entity_type(ep)
            && !vehicle_local_owns_firing_seat(ep);
    }

    CallHook<int(rf::Entity*)> entity_fire_weapon_watcher_ammo_gate_hook{
        0x00425BFB,
        [](rf::Entity* ep) -> int {
            const int total = entity_fire_weapon_watcher_ammo_gate_hook.call_target(ep);
            return vehicle_fire_skips_local_ammo_gate(ep) ? std::max(total, 1) : total;
        },
    };

    CallHook<int(rf::Entity*, int)> entity_fire_secondary_watcher_ammo_gate_hook{
        0x00426D0C,
        [](rf::Entity* ep, int weapon_type) -> int {
            const int reserve = entity_fire_secondary_watcher_ammo_gate_hook.call_target(ep, weapon_type);
            return vehicle_fire_skips_local_ammo_gate(ep) ? std::max(reserve, 1) : reserve;
        },
    };

    // ai_do_frame turns a CATATONIC entity's gun off every frame unless it is or hosts a local player,
    // which on a server would cut a synced hull's continuous gun right after its pass turned it on.
    CallHook<void(int, int)> ai_catatonic_weapon_off_hook{
        0x00403642,
        [](int entity_handle, int weapon_type) {
            if (rf::is_multi && vehicle_is_synced_entity_type(rf::entity_from_handle(entity_handle))) {
                return; // vehicle_server_apply_trigger owns a synced hull's weapon
            }
            ai_catatonic_weapon_off_hook.call_target(entity_handle, weapon_type);
        },
    };

    // The vehicle a projectile came from: its parent when the shot was fired by the hull, else the
    // vehicle the parent OCCUPANT is riding (a from_eye gun parents its round to the rider).
    rf::Entity* vehicle_weapon_own_vehicle(rf::Object* weapon)
    {
        if (!weapon) {
            return nullptr;
        }
        rf::Entity* parent = rf::entity_from_handle(weapon->parent_handle);
        if (!parent) {
            return nullptr;
        }
        if (vehicle_is_synced_entity_type(parent)) {
            return parent;
        }
        return vehicle_ridden_hull(parent);
    }

    // True when a handle names that vehicle or someone riding in it.
    bool vehicle_weapon_hit_is_own(rf::Entity* own_vehicle, int hit_handle)
    {
        if (!own_vehicle || hit_handle == -1) {
            return false;
        }
        if (hit_handle == own_vehicle->handle) {
            return true;
        }
        rf::Entity* hit_ent = rf::entity_from_handle(hit_handle);
        return hit_ent && hit_ent->host_handle == own_vehicle->handle;
    }

    // +0x2E8 is weapon_process's homing target handle, past the Object fields; -1 for none.
    int& vehicle_weapon_homing_target(rf::Object* weapon)
    {
        return *reinterpret_cast<int*>(reinterpret_cast<char*>(weapon) + 0x2E8);
    }

    // Target SELECTION only, and the ONLY non-stock thing on a vehicle round: the stock scan excludes
    // the round's parent but not anyone RIDING it, so a sub's torpedo locks onto its own driver.
    FunHook<void(rf::Object*)> weapon_update_homing_target_hook{
        0x004C6D70,
        [](rf::Object* weapon) {
            weapon_update_homing_target_hook.call_target(weapon);
            if (!rf::is_multi || !weapon) {
                return;
            }
            rf::Entity* own = vehicle_weapon_own_vehicle(weapon);
            if (!own) {
                return;
            }
            int& target = vehicle_weapon_homing_target(weapon);
            if (!vehicle_weapon_hit_is_own(own, target)) {
                return;
            }
            target = -1;
        },
    };

    // Cap on how many occupants one lag-comp trace may hide and re-trace through.
    constexpr int vehicle_max_masked_occupants = 8;

    // The engine's entity test for a lag-compensated shot is a human-sized pillar at the origin, not
    // the mesh, so vehicles are re-tested against their own mesh and merged by distance.

    // Distance from p0 at which the ray enters this vehicle's collision mesh, or -1 for a miss.
    float vehicle_lag_comp_mesh_hit(rf::Entity* ep, const rf::Vector3& p0, const rf::Vector3& p1,
                                    float collision_radius, rf::Vector3& hit_point_out)
    {
        const rf::Vector3 seg = p1 - p0;
        const float seg_len_sq = seg.len_sq();
        if (seg_len_sq <= 0.0f) {
            return -1.0f;
        }
        // Centred on p_data.pos and NOT Object::pos: the lag-comp rewind writes p_data only, so the
        // two disagree by the rewind distance for a moving hull. Object::radius encloses any pose.
        const rf::Vector3 to_center = ep->p_data.pos - p0;
        const float t = std::clamp(to_center.dot_prod(seg) / seg_len_sq, 0.0f, 1.0f);
        const float reach = ep->radius + collision_radius;
        if ((to_center - seg * t).len_sq() > reach * reach) {
            return -1.0f;
        }

        rf::VMeshCollisionInput in;
        rf::VMeshCollisionOutput out;
        in.flags = 0;
        in.start_pos = p0;
        in.dir = seg;
        in.radius = collision_radius;
        in.mesh_pos = ep->p_data.pos;
        in.mesh_orient = ep->p_data.orient;
        if (!rf::vmesh_collide(ep->vmesh, &in, &out, true)) {
            return -1.0f;
        }
        hit_point_out = in.mesh_orient.transform_vector(out.hit_point) + in.mesh_pos;
        return (hit_point_out - p0).len();
    }

    // The shot multi_lag_comp_weapon_fire is resolving. That function is a LOOP and a PIERCING weapon
    // walks it damaging each thing in turn, so this carries the pierce flag and the hulls already
    // hit. Wrapped at the CALL because the weapon is not reachable from the trace site.
    struct VehicleLagCompShot
    {
        bool pierces = false;
        int hit_hulls[vehicle_max_masked_occupants]{};
        int num_hit_hulls = 0;

        bool hull_already_hit(int handle) const
        {
            return std::find(hit_hulls, hit_hulls + num_hit_hulls, handle) != hit_hulls + num_hit_hulls;
        }

        void note_hull_hit(int handle)
        {
            if (num_hit_hulls < vehicle_max_masked_occupants && !hull_already_hit(handle)) {
                hit_hulls[num_hit_hulls++] = handle;
            }
        }
    };
    VehicleLagCompShot g_lag_comp_shot;

    CallHook<void(rf::Entity*, rf::Weapon*)> vehicle_lag_comp_shot_hook{
        0x00426708,
        [](rf::Entity* shooter, rf::Weapon* wp) {
            g_lag_comp_shot = VehicleLagCompShot{};
            g_lag_comp_shot.pierces = wp && wp->info && (wp->info->flags & rf::WTF_PIERCING) != 0;
            vehicle_lag_comp_shot_hook.call_target(shooter, wp);
            g_lag_comp_shot = VehicleLagCompShot{};
        },
    };

    // True for a hit the simulated-projectile path would never have delivered.
    bool vehicle_lag_comp_hit_is_shielded(const rf::Object* obj, const rf::Object* shooter)
    {
        if (!obj || obj->host_handle == -1) {
            return false;
        }
        if (shooter && obj->host_handle == shooter->handle) {
            return true; // weapon_hit_obj's parent_handle == hit->host_handle early-out
        }
        rf::Entity* host = rf::entity_from_handle(obj->host_handle);
        if (!host || !host->info || host->info->use_function != rf::ENTITY_USE_VEHICLE) {
            return false; // a turret gunner stays directly shootable, exactly as in stock
        }
        if (g_lag_comp_shot.pierces) {
            // Deliberately BELOW the shooter's-own-vehicle test: a hull is no shelter from a round
            // that goes through walls.
            return false;
        }
        // Only an ENCLOSED hull shields: the jeep's riders are reachable by a simulated projectile,
        // so hiding them here would make them immune to fast weapons and hittable by slow ones.
        return !vehicle_class_open_seats(host);
    }

    CallHook<int(rf::Vector3*, rf::Vector3*, rf::Object*, rf::Object*, rf::LevelCollisionOut*,
                 float, int, float)>
        vehicle_lag_comp_trace_hook{
        {0x0046FA8F, 0x0046FB80},
        [](rf::Vector3* p0, rf::Vector3* p1, rf::Object* shooter, rf::Object* ignore2,
           rf::LevelCollisionOut* out, float collision_radius, int use_mesh_collide,
           float bbox_size_factor) -> int {
            // The callee is free to clip the segment, so a re-trace must restart from these points.
            const rf::Vector3 ray_start = *p0;
            const rf::Vector3 ray_end = *p1;
            int hit = vehicle_lag_comp_trace_hook.call_target(p0, p1, shooter, ignore2, out,
                                                              collision_radius, use_mesh_collide,
                                                              bbox_size_factor);
            rf::Object* masked[vehicle_max_masked_occupants]{};
            int num_masked = 0;
            while ((hit & 0xFF) != 0 && num_masked < vehicle_max_masked_occupants) {
                rf::Object* obj = rf::obj_from_handle(out->obj_handle);
                if (!vehicle_lag_comp_hit_is_shielded(obj, shooter)) {
                    break;
                }
                obj->obj_flags =
                    static_cast<rf::ObjectFlags>(obj->obj_flags | rf::OF_DELAYED_DELETE);
                masked[num_masked++] = obj;
                *p0 = ray_start;
                *p1 = ray_end;
                // The stock caller clears the face before every trace: the callee only resets the
                // object handle and the distance, and reports "hit" when either is set.
                out->face = nullptr;
                hit = vehicle_lag_comp_trace_hook.call_target(p0, p1, shooter, ignore2, out,
                                                              collision_radius, use_mesh_collide,
                                                              bbox_size_factor);
            }
            for (int i = 0; i < num_masked; ++i) {
                masked[i]->obj_flags =
                    static_cast<rf::ObjectFlags>(masked[i]->obj_flags & ~rf::OF_DELAYED_DELETE);
            }

            // The segment is taken as the callee left it: *p1 is already clipped to the face the
            // world collide stopped on.
            if (use_mesh_collide == 0) {
                for (int handle : g_vehicle_state.synced_handles) {
                    rf::Entity* veh = rf::entity_from_handle(handle);
                    if (!veh || !veh->vmesh || static_cast<rf::Object*>(veh) == shooter
                        || static_cast<rf::Object*>(veh) == ignore2) {
                        continue;
                    }
                    if ((veh->obj_flags & (rf::OF_DELAYED_DELETE | rf::OF_HIDDEN))
                        || rf::entity_is_dying(veh)) {
                        continue;
                    }
                    // A piercing shot walks the loop with only ONE ignore slot, so a hull it entered
                    // two passes ago is a legal target again; stock damages a pierced thing once.
                    if (g_lag_comp_shot.hull_already_hit(veh->handle)) {
                        continue;
                    }
                    rf::Vector3 hit_point;
                    const float dist =
                        vehicle_lag_comp_mesh_hit(veh, *p0, *p1, collision_radius, hit_point);
                    if (dist < 0.0f || dist >= out->distance) {
                        continue;
                    }
                    out->distance = dist;
                    out->obj_handle = veh->handle;
                    out->hit_point = hit_point;
                    out->face = nullptr;
                    hit = 1;
                }
            }

            // Recording the merged result keeps the mesh test above from offering one hull twice.
            if ((hit & 0xFF) != 0) {
                if (rf::Entity* hit_veh = vehicle_synced_entity(out->obj_handle)) {
                    g_lag_comp_shot.note_hull_hit(hit_veh->handle);
                }
            }

            return hit;
        },
    };

    // Append the driven vehicle's row to the local player's obj_update packet, injected right after
    // send_obj_update_packet packs that row: EAX is its length, the buffer is ESP+0xC, and the
    // terminator and size field are written from EAX after we return.
    CodeInjection send_obj_update_packet_vehicle_row_injection{
        0x0047E5FF,
        [](auto& regs) {
            rf::Entity* vehicle = vehicle_local_driven_vehicle();
            if (!vehicle) {
                return;
            }
            auto* buf = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(regs.esp) + 0xC);
            const int player_row_len = regs.eax;
            const int offset = 3 + player_row_len;
            // Worst case from pack_obj_update_data.
            constexpr int max_row_size = 0x90;
            if (player_row_len <= 0 || offset + max_row_size + 4 > 0x200) {
                return;
            }

            vehicle_push_local_interp_sample(vehicle);
            const int row_len = AddrCaller{pack_obj_update_data_addr}
                                    .c_call<int>(rf::local_player, vehicle, buf + offset);
            if (row_len <= 0) {
                return;
            }
            // Fire bits carry no payload bytes, so clearing them keeps the row length valid.
            // Vehicle weapons are the server's; the server rejects rows that carry them.
            buf[offset + 4] &= static_cast<uint8_t>(~(0x40 | 0x10));
            regs.eax = player_row_len + row_len;
        },
    };

    // A vehicle with an empty driver seat has nobody reporting it, so while another seat is taken
    // (a gunner-only jeep) the server samples its row itself.
    bool vehicle_server_samples_vehicle(rf::Entity* vehicle)
    {
        if (!rf::is_server || !vehicle || rf::entity_is_dying(vehicle)) {
            return false;
        }
        if (vehicle_seat_leech(vehicle, 0) != -1) {
            return false; // the driver's own rows are this vehicle's report
        }
        return rf::entity_get_first_leech(vehicle) != -1;
    }

    // The server broadcast loop only serializes entities that received keyframes, which a listen
    // host's own vehicle never does. Injected on the load of obj_interp for that test.
    CodeInjection server_obj_update_send_vehicle_injection{
        0x0047E6E9,
        [](auto& regs) {
            if (g_vehicle_state.synced_handles.empty()) {
                return; // per-entity, every broadcast tick: nothing to do with no hulls alive
            }
            rf::Entity* ep = regs.ebx;
            if (ep && vehicle_is_synced_entity_type(ep)
                && (vehicle_local_driven_vehicle() == ep || vehicle_server_samples_vehicle(ep))) {
                // pack_obj_update_data would otherwise serialize it out of a ring nothing fills.
                vehicle_push_local_interp_sample(ep);
                regs.eip = 0x0047E701; // serialize it
                return;
            }
            // vehicle_server_body_do_frame has already authored this frame's keyframe for it.
            if (ep && vehicle_is_synced_entity_type(ep) && vehicle_is_kinematically_broadcast(ep)) {
                regs.eip = 0x0047E701; // serialize it
                return;
            }
        },
    };

    // send_entity_create_packet refuses any entity that is not a player's, but reaches the refusal
    // with the packet already serialized up to the player_id byte - so finish it with 0xFF, the
    // shipped NPC branch. Buffer at ESP+0x18, ESI the write cursor, EBP the entity, pp at ESP+0x220.
    CodeInjection send_entity_create_packet_vehicle_injection{
        0x00475356,
        [](auto& regs) {
            rf::Entity* ep = regs.ebp;
            if (!vehicle_is_synced_entity_type(ep)) {
                return;
            }

            auto* buf = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(regs.esp) + 0x18);
            int cursor = regs.esi;
            constexpr int trailer_size = 1 + 3 * static_cast<int>(sizeof(int));
            if (cursor < 3 || cursor + trailer_size > 0x200) {
                return;
            }

            const int mp_character = -1;
            const int primary_weapon = ep->ai.current_primary_weapon;
            const int secondary_weapon = ep->ai.current_secondary_weapon;

            buf[cursor++] = 0xFF;
            std::memcpy(buf + cursor, &mp_character, sizeof(mp_character));
            cursor += sizeof(mp_character);
            std::memcpy(buf + cursor, &primary_weapon, sizeof(primary_weapon));
            cursor += sizeof(primary_weapon);
            std::memcpy(buf + cursor, &secondary_weapon, sizeof(secondary_weapon));
            cursor += sizeof(secondary_weapon);

            const auto payload_size = static_cast<uint16_t>(cursor - 3);
            std::memcpy(buf + 1, &payload_size, sizeof(payload_size));

            rf::Player* pp = addr_as_ref<rf::Player*>(regs.esp + 0x220);
            rf::multi_io_send_reliable(pp, buf, cursor, 0);

            regs.eip = 0x00475363; // stock epilogue, skipping the refusal message
        },
    };

    // entity_create uprights the wire orient in place for non-flyer movemodes, and its ground drop
    // (0x004A0770) re-seats a "run"/"apc" hull on its cspheres, up to 0.30 above the wire pos.
    CallHook<rf::Entity*(int, const char*, int, const rf::Vector3&, rf::Matrix3&, int, int)>
        process_entity_create_packet_vehicle_hook{
            0x0047565E,
            [](int entity_type, const char* name, int parent_handle, const rf::Vector3& pos,
               rf::Matrix3& orient, int create_flags, int mp_character) -> rf::Entity* {
                const rf::Vector3 wire_pos = pos;
                const rf::Matrix3 wire_orient = orient;
                rf::Entity* ep = process_entity_create_packet_vehicle_hook.call_target(
                    entity_type, name, parent_handle, pos, orient, create_flags, mp_character);
                if (!ep || !vehicle_is_synced_entity_type(ep)) {
                    return ep;
                }
                rf::Vector3 restored_pos = wire_pos;
                ep->move(&restored_pos);
                if (vehicle_orient_is_orthonormal(wire_orient)) {
                    ep->orient = wire_orient;
                    ep->p_data.orient = wire_orient;
                    ep->p_data.next_orient = wire_orient;
                }
                vehicle_init_synced_entity(ep);
                return ep;
            },
        };

    rf::Player* vehicle_firing_seat_player(rf::Entity* vehicle)
    {
        const rf::Entity* holder = vehicle_firing_seat_occupant(vehicle);
        return holder ? rf::player_from_entity_handle(holder->handle) : nullptr;
    }
} // namespace

void vehicle_send_seat_states_to(rf::Player* pp)
{
    if (!rf::is_server || !pp) {
        return;
    }
    for (int handle : g_vehicle_state.synced_handles) {
        rf::Entity* vehicle = rf::entity_from_handle(handle);
        if (!vehicle) {
            continue;
        }
        // One occupancy assert per hull, not one packet per occupied seat: the wire states seats,
        // so the join sweep is the same statement every seat change sends.
        vehicle_send_seat_occupancy_to(pp, vehicle, -1);
        // Health too: the broadcast only fires on a change watermark, so an untouched hull would
        // never send this man anything.
        af_send_vehicle_health_packet(pp, vehicle->handle, vehicle->life, vehicle_hud_max_life(vehicle),
                                      vehicle_weapon_ammo(vehicle, vehicle->ai.current_primary_weapon),
                                      vehicle_weapon_ammo(vehicle, vehicle->ai.current_secondary_weapon));
        // A gun already firing sent its START before this man existed.
        const int primary = vehicle->ai.current_primary_weapon;
        if (primary >= 0 && rf::entity_weapon_is_on(vehicle->handle, primary)
            && vehicle_firing_seat_player(vehicle) != pp) {
            af_send_vehicle_fire_packet(pp, vehicle->handle, AF_VEHICLE_FIRE_START,
                                        (vehicle->ai.ai_flags & rf::AIF_ALT_FIRE) ? 1 : 0);
        }
    }
    vehicle_send_factory_states_to(pp);
}

bool vehicle_is_driver_obj_update_row(const rf::Player* pp, const rf::Entity* ep, int flags)
{
    // A dead player's entity_handle is -1, which is also what an empty seat holds: without this
    // a spectating client could claim the driver seat of any unmanned vehicle.
    if (!pp || pp->entity_handle == -1 || !vehicle_is_synced_entity_type(ep)) {
        return false;
    }
    if (vehicle_seat_leech(ep, 0) != pp->entity_handle) {
        return false;
    }
    // OUF_POS_ROT_ANIM, the connection counter and the weapon lag compensation block, which carry no
    // entity state. Weapon type, health, armor state and fire stay server-owned.
    constexpr int permitted_flags = 0x01 | 0x02 | 0x08;
    return (flags & ~permitted_flags) == 0;
}

rf::Entity* vehicle_firing_seat_occupant(rf::Entity* vehicle)
{
    if (!vehicle || vehicle->interface_points.size() < 1) {
        return nullptr;
    }
    // The jeep gun belongs to the gunner's seat alone; every other class holds its gun on seat 0.
    if (rf::entity_is_jeep(vehicle)) {
        return rf::entity_from_handle(vehicle_seat_leech(vehicle, 1));
    }
    return rf::entity_from_handle(vehicle_seat_leech(vehicle, 0));
}

bool vehicle_local_owns_firing_seat(rf::Entity* ep)
{
    rf::Entity* rider = rf::local_player_entity;
    if (!rider || !vehicle_is_synced_entity_type(ep)) {
        return false;
    }
    return vehicle_firing_seat_occupant(ep) == rider;
}

bool vehicle_suppress_local_fire(const rf::Player* pp)
{
    if (!rf::is_multi || !pp) {
        return false;
    }
    rf::Entity* rider = rf::entity_from_handle(pp->entity_handle);
    rf::Entity* vehicle = vehicle_ridden_hull(rider);
    if (!vehicle) {
        return false;
    }
    // The local pipeline produces the flash, the sound and the predicted projectile, so through a
    // drown blackout it would show this machine shots nothing else will see.
    if (vphys_hull_submerged(vehicle)) {
        return true;
    }
    // Listen host: the module's server-side pass owns vehicle weapons outright, for ANY seat.
    if (rf::is_server) {
        return true;
    }
    // Client: the stock body substitutes the HOST as the shooter for every rider of a use_function
    // 1/4 host, with no notion of which seat owns the gun.
    return vehicle_firing_seat_occupant(vehicle) != rider;
}

void vehicle_server_handle_fire_request(rf::Player* pp, int vehicle_handle, uint8_t action,
                                        uint8_t alt_fire)
{
    if (!rf::is_server || !pp || !pp->net_data) {
        return;
    }
    if (action != AF_VEHICLE_FIRE_STOP && action != AF_VEHICLE_FIRE_START) {
        return; // discrete shots are the server's to announce, never a client's to demand
    }

    rf::Entity* rider = rf::entity_from_handle(pp->entity_handle);
    rf::Entity* vehicle = vehicle_live_synced_entity(vehicle_handle);
    if (!rider || !vehicle) {
        return;
    }
    if (vehicle_firing_seat_occupant(vehicle) != rider) {
        return;
    }

    const bool held = action == AF_VEHICLE_FIRE_START;
    const bool alt = alt_fire != 0;

    // No rate limit: the client reports EDGES and never re-asserts, so a dropped START holds the
    // trigger down forever, and the engine's fire-wait already bounds the per-frame apply pass.
    VehicleFireState& state = g_vehicle_state.fire[vehicle->handle];
    (alt ? state.alt_held : state.primary_held) = held;
    state.requester_id = pp->net_data->player_id;
    if (!held) {
        // Stop this trigger immediately, do not wait for the frame - and leave the other alone.
        vehicle_server_apply_trigger(vehicle, alt, false, state.requester_id);
    }
}

// Every off path (release, ammo, blackout, seat change, death) funnels through the engine's turn-off,
// so hooking the edges themselves is what keeps a watcher's gun from outliving the server's.
void vehicle_server_announce_weapon_edge(int entity_handle, int weapon_type, bool on, bool alt_fire)
{
    if (!rf::is_multi || !rf::is_server) {
        return;
    }
    rf::Entity* vehicle = vehicle_synced_entity(entity_handle);
    if (!vehicle || weapon_type != vehicle->ai.current_primary_weapon) {
        return;
    }
    // STOP for either mode, so no START can go unpaired.
    const bool continuous = on ? rf::weapon_is_on_off_weapon(weapon_type, alt_fire)
                               : rf::weapon_is_on_off_weapon(weapon_type, false)
                                     || rf::weapon_is_on_off_weapon(weapon_type, true);
    if (!continuous) {
        return;
    }
    af_send_vehicle_fire_packet_to_all(vehicle_firing_seat_player(vehicle), vehicle->handle,
                                       on ? AF_VEHICLE_FIRE_START : AF_VEHICLE_FIRE_STOP,
                                       on && alt_fire ? 1 : 0);
}

void vehicle_apply_fire_from_packet(int vehicle_handle, uint8_t action, uint8_t alt_fire)
{
    rf::Object* obj = rf::obj_from_remote_handle(vehicle_handle);
    if (!obj) {
        return;
    }
    const bool alt = alt_fire != 0;
    if (action == AF_VEHICLE_FIRE_STOP) {
        // Unconditional, dying hull included: a spare turn-off is a no-op, a missed one fires forever.
        if (rf::Entity* vehicle = vehicle_synced_entity(obj->handle)) {
            multi_turn_weapon_off(vehicle);
        }
        return;
    }
    rf::Entity* vehicle = vehicle_live_synced_entity(obj->handle);
    if (!vehicle) {
        return;
    }
    if (action == AF_VEHICLE_FIRE_SHOT) {
        vehicle_fire_discrete(vehicle, alt, true);
        return;
    }
    // Only a turret's alt is a mode of its continuous primary. The firing seat predicts its own.
    if (action != AF_VEHICLE_FIRE_START || vehicle_local_owns_firing_seat(vehicle)
        || (alt && vehicle->info->use_function == rf::ENTITY_USE_VEHICLE)) {
        return;
    }
    multi_turn_weapon_on(vehicle, nullptr, alt);
    // The server's own on-branch fires at once too; unforced, so the fire-wait it sets keeps the
    // loop-fire from doubling it, and a STOP in the same batch still leaves exactly this round.
    if (rf::entity_weapon_is_on(vehicle->handle, vehicle->ai.current_primary_weapon)) {
        vehicle_fire_discrete(vehicle, alt, false);
    }
}

void vehicle_server_handle_orient_report(rf::Player* pp, int vehicle_handle, uint16_t tick,
                                         int16_t pitch_q, int16_t bank_q, int16_t aim_pitch_q,
                                         int16_t aim_head_q, int8_t steer_q)
{
    if (!rf::is_server || !pp || !pp->net_data || pp->entity_handle == -1) {
        return;
    }
    rf::Entity* vehicle = vehicle_synced_entity(vehicle_handle);
    if (!vehicle) {
        return;
    }
    // Same authority rule the driver's obj_update row goes through: seat 0 owns the hull.
    if (vehicle_seat_leech(vehicle, 0) != pp->entity_handle) {
        return;
    }
    // Stricter than vehicle_store_orient's guard, which lets an equal tick correct a supplement in
    // place. Peeked, not inserted, so the first report for a hull is always relayed.
    const VehicleOrientSupplement* supp = vehicle_orient_supplement(vehicle->handle);
    if (supp && timer::get_i64(1000) - supp->stored_ms < vehicle_orient_stale_ms
        && static_cast<int16_t>(tick - supp->tick) <= 0) {
        return;
    }
    // Stored before the relay bound: the server mints this hull's shots and rebuilds its lag-comp
    // eye frame from the stored aim, so a throttled report may cost bandwidth but never accuracy.
    vehicle_store_orient(vehicle, tick, vehicle_dequantize_angle(pitch_q),
                         vehicle_dequantize_angle(bank_q), vehicle_dequantize_angle(aim_pitch_q),
                         vehicle_dequantize_angle(aim_head_q), vehicle_dequantize_steer(steer_q));

    rf::Timestamp& relay_cooldown = g_vehicle_state.orient_relay_cooldown[pp->net_data->player_id];
    if (relay_cooldown.valid() && !relay_cooldown.elapsed()) {
        return;
    }
    relay_cooldown.set(vehicle_orient_relay_min_ms);

    // Relayed as it arrived, so every watcher reconstructs the same angles the driver sent.
    af_send_vehicle_orient_packet_to_all(pp, vehicle->handle, tick, pitch_q, bank_q, aim_pitch_q,
                                         aim_head_q, steer_q);
}

void vehicle_apply_orient_from_packet(int vehicle_handle, uint16_t tick, int16_t pitch_q,
                                      int16_t bank_q, int16_t aim_pitch_q, int16_t aim_head_q,
                                      int8_t steer_q)
{
    rf::Object* obj = rf::obj_from_remote_handle(vehicle_handle);
    rf::Entity* vehicle = obj ? vehicle_synced_entity(obj->handle) : nullptr;
    if (!vehicle) {
        return;
    }
    vehicle_store_orient(vehicle, tick, vehicle_dequantize_angle(pitch_q),
                         vehicle_dequantize_angle(bank_q), vehicle_dequantize_angle(aim_pitch_q),
                         vehicle_dequantize_angle(aim_head_q), vehicle_dequantize_steer(steer_q));
}

void vehicle_store_health_from_packet(int vehicle_handle, float life, float max_life, int primary_ammo,
                                      int secondary_ammo)
{
    rf::Object* obj = rf::obj_from_remote_handle(vehicle_handle);
    if (!obj) {
        return;
    }
    // A non-finite life reaches the HUD bar and the cockpit armor readout, which writes it into
    // Entity::life for the duration of the engine call.
    if (!std::isfinite(life) || !std::isfinite(max_life) || max_life < 0.0f) {
        return;
    }
    g_vehicle_state.health[obj->handle] = VehicleHealth{life, max_life};

    // Ammo goes into the entity's own AiInfo rather than a side store: the cockpit readout
    // (0x004A7F60) and the stock HUD both read ai.ammo off the entity.
    rf::Entity* vehicle = vehicle_synced_entity(obj->handle);
    if (!vehicle) {
        return;
    }
    VehicleAmmoMirror& mirror = g_vehicle_state.ammo_mirror[obj->handle];
    const int64_t now = timer::get_i64(1000);
    // Only the firing seat predicts shots; a watcher's replays are the server's own and must take its value.
    const bool predicts = vehicle_local_owns_firing_seat(vehicle);
    const auto set_ammo = [vehicle, &mirror, now, predicts](int slot, int weapon_type, int total) {
        constexpr int vehicle_wire_ammo_max = 10000;
        if (total < 0 || total > vehicle_wire_ammo_max || weapon_type < 0 || weapon_type >= 64) {
            return; // -1 total = the hull has no such weapon; leave the mirror alone
        }
        const int ammo_type = rf::weapon_types[weapon_type].ammo_type;
        if (ammo_type < 0 || ammo_type >= 32) {
            return;
        }
        // The server's number is the TOTAL the fire gate uses: clip + reserve.
        const int clip = vehicle->ai.clip_ammo[weapon_type];
        const int local = clip + vehicle->ai.ammo[ammo_type];
        if (predicts && mirror.total[slot] >= 0 && local < mirror.total[slot]) {
            mirror.last_drop_ms[slot] = now; // shots fired here since the last packet
        }
        // A raise this soon after a local shot is a packet from before it; applying it would feed shots the
        // server never fires.
        if (predicts && total > local && mirror.total[slot] >= 0
            && now - mirror.last_drop_ms[slot] < vehicle_ammo_stale_raise_ms) {
            mirror.total[slot] = local;
            return;
        }
        vehicle->ai.ammo[ammo_type] = std::max(total - clip, 0);
        mirror.total[slot] = total;
    };
    set_ammo(0, vehicle->ai.current_primary_weapon, primary_ammo);
    set_ammo(1, vehicle->ai.current_secondary_weapon, secondary_ammo);
}

void vehicle_sync_apply_patch()
{
    send_entity_create_packet_vehicle_injection.install();
    process_entity_create_packet_vehicle_hook.install();
    physics_world_collision_vehicle_injection.install();
    obj_should_sim_physics_hook.install();
    driller_interp_no_movement_halt_injection.install();
    driller_mp_geomod_cap_injection.install();
    send_obj_update_packet_vehicle_row_injection.install();
    server_obj_update_send_vehicle_injection.install();
    obj_interp_set_next_pos_orient_hook.install();
    physics_update_entity_hook.install();
    multi_lag_comp_rewind_entity_hook.install();
    vehicle_lag_comp_shot_hook.install();
    vehicle_lag_comp_trace_hook.install();
    obj_pair_should_skip_hook.install();
    weapon_create_own_vehicle_bullet_injection.install();
    entity_fire_weapon_own_vehicle_bullet_injection.install();
    entity_fire_weapon_watcher_ammo_gate_hook.install();
    entity_fire_secondary_watcher_ammo_gate_hook.install();
    ai_catatonic_weapon_off_hook.install();
    weapon_update_homing_target_hook.install();
}
