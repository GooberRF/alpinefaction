#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>
#include <xlog/xlog.h>
#include <patch_common/CallHook.h>
#include <patch_common/FunHook.h>
#include <common/utils/list-utils.h>
#include "vehicle.h"
#include "vehicle_physics.h"
#include "vehicle_internal.h"
#include "vehicle_seats.h"
#include "vehicle_sync.h"
#include "vehicle_damage.h"
#include "../alpine_packets.h"
#include "../gametype.h"
#include "../server.h"
#include "../../hud/hud.h"
#include "../../hud/multi_spectate.h"
#include "../../input/control_input_filter.h"
#include "../../input/input.h"
#include "../../misc/level.h"
#include "../../os/os.h"
#include "../../rf/ai.h"
#include "../../rf/entity.h"
#include "../../rf/gameseq.h"
#include "../../rf/geometry.h"
#include "../../rf/input.h"
#include "../../rf/multi.h"
#include "../../rf/object.h"
#include "../../rf/os/console.h"
#include "../../rf/physics.h"
#include "../../rf/player/camera.h"
#include "../../rf/player/control_config.h"
#include "../../rf/player/player.h"

namespace
{
    // Slack on the entity type's use_radius when the server re-checks the boarding distance.
    constexpr float board_range_slack = 1.5f;

    // How often one player may make a use request - board, seat swap or exit. Both sides of the wire.
    constexpr int vehicle_use_request_cooldown_ms = 200;

    // How long a stated occupancy naming an unresolvable rider keeps being re-applied.
    constexpr int64_t vehicle_pending_seats_ttl_ms = 5000;

    // Lowest UNOCCUPIED seat, or -1. Free == leech_handle -1, the condition entity_attach_leech takes.
    // Capped at the wire's seat count: a seat the occupancy packet cannot state would be released by
    // every client on every packet.
    int vehicle_lowest_free_seat(const rf::Entity* vehicle)
    {
        if (!vehicle) {
            return -1;
        }
        const int seats = std::min<int>(vehicle->interface_points.size(), af_vehicle_state_max_seats);
        for (int i = 0; i < seats; ++i) {
            if (vehicle_seat_leech(vehicle, i) == -1) {
                return i;
            }
        }
        return -1;
    }

    // Which seat of THIS hull holds this rider, or -1; the rider's own half is host_handle.
    int vehicle_seat_of_rider(const rf::Entity* vehicle, const rf::Entity* rider)
    {
        if (!vehicle || !rider) {
            return -1;
        }
        for (int i = 0; i < vehicle->interface_points.size(); ++i) {
            if (vehicle_seat_leech(vehicle, i) == rider->handle) {
                return i;
            }
        }
        return -1;
    }

    // 80 degrees, the free-look pitch a passenger seat grants.
    constexpr float vehicle_passenger_eye_pitch_limit = 1.39626f;

    // Vanilla boarding init, 0x004A1DD8..0x004A1F02; runs right after the attach on every machine.
    void vehicle_boarding_init(rf::Entity* vehicle, rf::Entity* rider)
    {
        const bool is_jeep_gunner = rf::entity_is_jeep_gunner(rider);
        if (!is_jeep_gunner) {
            rf::ai_set_mode(&vehicle->ai, rf::AI_MODE_CATATONIC, -1, -1);
        }
        rf::ai_set_submode(&vehicle->ai, rf::AI_SUBMODE_NONE);
        vehicle->entity_flags = (vehicle->entity_flags & ~0x20000u) | rf::EF_BOARDED;
        rf::obj_set_friendliness(vehicle, rf::OBJ_NEUTRAL);
        vehicle->min_rel_eye_phb = vehicle->info->min_rel_eye_phb;
        vehicle->max_rel_eye_phb = vehicle->info->max_rel_eye_phb;
        // For a Bullet-driven car the engine's own writers of these four are gone (vphys skips
        // physics_simulate_entity and 0x0049CF40 only ADDS), so whatever the hull carried out of its
        // last interp epoch would ride into the seat as a constant offset on the fire frame.
        if (vehicle_physics_class_syncs_driver_aim(vehicle)) {
            vehicle->control_data.eye_phb.zero();
            vehicle->control_data.delta_eye_phb.zero();
            vehicle->control_data.automobile_eye_phb.zero();
            vehicle->control_data.delta_phb.zero();
        }
        if (is_jeep_gunner) {
            rider->min_rel_eye_phb = rf::jeep_gunner_min_rel_eye_phb;
            rider->max_rel_eye_phb = rf::jeep_gunner_max_rel_eye_phb;
        }
        else if (vehicle_passenger_vehicle(rider) == vehicle) {
            // Only pitch is a limit: his HEADING lives in control_data.phb.y, which nothing clamps, so
            // the y/z zeros pin his bank and leave him free to turn right round.
            rider->min_rel_eye_phb = rf::Vector3{-vehicle_passenger_eye_pitch_limit, 0.0f, 0.0f};
            rider->max_rel_eye_phb = rf::Vector3{vehicle_passenger_eye_pitch_limit, 0.0f, 0.0f};
        }
        if (rf::entity_is_automobile(vehicle)) {
            rf::obj_physics_activate(vehicle);
        }
    }

    // The exit angles, sampled before the detach, off the hull matrix: an automobile never writes
    // control_data.phb.
    struct VehicleExitView
    {
        float eye_pitch = 0.0f;
        float heading = 0.0f;
        bool valid = false;
    };

    VehicleExitView vehicle_capture_exit_view(rf::Entity* vehicle)
    {
        if (!vehicle || !rf::entity_is_vehicle(vehicle)) {
            return {};
        }
        const rf::Vector3 hull = vehicle_make_orient_phb(vehicle->orient);
        return VehicleExitView{
            vehicle->control_data.eye_phb.x + hull.x,
            vehicle->control_data.eye_phb.y + hull.y,
            true,
        };
    }

    // Vanilla exit tail, 0x004A19EF..0x004A1A86, minus the riot shield visuals. The VEHICLE's own
    // motion reset is vehicle_update_interp_ownership's, or a gunner's exit would stop the jeep.
    void vehicle_exit_restore(rf::Entity* vehicle, rf::Entity* rider, const VehicleExitView& view)
    {
        rider->min_rel_eye_phb = rider->info->min_rel_eye_phb;
        rider->max_rel_eye_phb = rider->info->max_rel_eye_phb;
        if (view.valid) {
            rider->control_data.eye_phb.x = view.eye_pitch;
            rider->control_data.phb.y = view.heading;
        }
        // A seated rider is not simulated and detaching never touches his velocity.
        if (vehicle && rf::entity_is_vehicle(vehicle)) {
            rider->p_data.vel = rf::Vector3{};
            rider->p_data.rotvel = rf::Vector3{};
            rider->p_data.ang_momentum = rf::Vector3{};
        }
    }

    // Sampled BEFORE the detach, which zeroes the hull's p_data.vel and destroys the local Bullet
    // body. A DRIVER on a simulating machine reads the bt body; the entity's vel is a frame stale.
    rf::Vector3 vehicle_capture_exit_inertia(rf::Entity* vehicle, const rf::Entity* rider)
    {
        if (!vehicle_is_synced_entity_type(vehicle)) {
            return rf::Vector3{};
        }
        rf::Vector3 vel{};
        if (vehicle_driver_entity(vehicle) == rider
            && vehicle_physics_driven_velocity(vehicle->handle, &vel)) {
            return vel;
        }
        return vehicle->p_data.vel;
    }

    // Applied AFTER vehicle_exit_restore, which zeroes the freed rider's motion. Local player only:
    // movement is client-authoritative, so a server-side write dies on his next obj_update row.
    void vehicle_apply_exit_inertia(rf::Entity* rider, const rf::Vector3& vel)
    {
        if (rider && rider == rf::local_player_entity) {
            rider->p_data.vel = vel;
        }
    }

    void vehicle_local_rider_enter(rf::Entity* vehicle)
    {
        rf::Player* pp = rf::local_player;
        if (!pp) {
            return;
        }
        if (vehicle->info->use_function == rf::ENTITY_USE_VEHICLE) {
            rf::player_cockpit_reset(pp);
            pp->flags |= rf::PF_IN_ENCLOSED_VEHICLE;
        }
        rf::player_fpgun_reset_zoom(pp);
        // An orbit seat is claimed by the camera frame; forcing first person first would flash it.
        if (pp->cam && !vehicle_physics_camera_local_seat_is_orbit()) {
            rf::camera_enter_first_person(pp->cam);
        }
    }

    // THE hull velocity derivation, shared by every handoff point and by the wire: whichever of the
    // LOCAL driven body, a server body or the entity this machine actually keeps it in.
    rf::Vector3 vehicle_live_hull_velocity(const rf::Entity* vehicle)
    {
        rf::Vector3 vel{};
        if (vehicle_physics_driven_velocity(vehicle->handle, &vel)) {
            return vel;
        }
        if (vehicle_physics_server_velocity(vehicle->handle, &vel)) {
            return vel;
        }
        return vehicle->p_data.vel;
    }

    int16_t vehicle_quantize_hull_vel_axis(float v)
    {
        if (!std::isfinite(v)) {
            return 0;
        }
        return static_cast<int16_t>(
            std::lround(std::clamp(v * af_vehicle_state_vel_quant, -32767.0f, 32767.0f)));
    }

    // Held to the class cap on arrival, the same ceiling vehicle_clamp_row_velocity applies.
    rf::Vector3 vehicle_dequantize_hull_velocity(const rf::Entity* vehicle, const int16_t* q)
    {
        rf::Vector3 vel{static_cast<float>(q[0]) / af_vehicle_state_vel_quant,
                        static_cast<float>(q[1]) / af_vehicle_state_vel_quant,
                        static_cast<float>(q[2]) / af_vehicle_state_vel_quant};
        const float limit = vehicle_physics_class_max_speed(vehicle_damage_class(vehicle));
        const float speed = vel.len();
        if (speed > limit) {
            vel = speed > 0.0f ? vel * (limit / speed) : rf::Vector3{};
        }
        return vel;
    }

    // A turret cannot go anywhere and the jeep is open-topped, so a flag carried aboard either is
    // still where the other team can contest it. Every other hull would take it out of reach.
    bool vehicle_hull_may_carry_flags(const rf::Entity* vehicle)
    {
        return vehicle_hull_is_turret(vehicle) || vehicle_damage_class(vehicle) == VDC_JEEP;
    }

    bool vehicle_sub_water_gate_ok(const rf::Entity* vehicle)
    {
        if (!(vehicle->info->flags & rf::EIF_WATER_ONLY)) {
            return true;
        }
        return vehicle->room && rf::point_in_liquid(vehicle->room, &vehicle->pos);
    }

    void vehicle_deny(rf::Player* pp, std::string_view reason)
    {
        af_send_hud_notification(reason, 3, static_cast<int>(HudNotificationType::Generic), true, pp);
    }

    // entity_attach_leech plays the boarding foley unconditionally, so a mirror repair and the join
    // sweep can only be silenced at its snd_play_3d call. Scoped around the attach, never left set.
    bool g_boarding_foley_muted = false;

    CallHook<int(int, const rf::Vector3&, float, const rf::Vector3&, int)> entity_attach_leech_foley_hook{
        0x00427309,
        [](int handle, const rf::Vector3& pos, float vol_scale, const rf::Vector3& unused, int group) {
            if (g_boarding_foley_muted) {
                return -1;
            }
            return entity_attach_leech_foley_hook.call_target(handle, pos, vol_scale, unused, group);
        },
    };

    struct VehicleSilentBoardingScope
    {
        const bool prev = g_boarding_foley_muted;
        explicit VehicleSilentBoardingScope(bool mute)
        {
            if (mute) {
                g_boarding_foley_muted = true;
            }
        }
        ~VehicleSilentBoardingScope() { g_boarding_foley_muted = prev; }
        VehicleSilentBoardingScope(const VehicleSilentBoardingScope&) = delete;
        VehicleSilentBoardingScope& operator=(const VehicleSilentBoardingScope&) = delete;
    };

    // THE atomic seat change: the rider never leaves the hull, so no exit machinery may run, and
    // neither leech call places him. Eye limits are reset first because they belong to the SEAT.
    // False = still in from.
    bool vehicle_move_rider_to_seat(rf::Entity* vehicle, rf::Entity* rider, int from_seat,
                                    int to_seat)
    {
        int to_tag = -1;
        if (!vehicle_seat_tag_from_index(vehicle, to_seat, &to_tag)) {
            return false;
        }
        if (!rf::entity_detach_leech(vehicle, rider->handle, false)) {
            return false;
        }
        rider->min_rel_eye_phb = rider->info->min_rel_eye_phb;
        rider->max_rel_eye_phb = rider->info->max_rel_eye_phb;
        if (!rf::entity_attach_leech(vehicle, rider->handle, to_tag)) {
            // Put him back: a rider detached from a hull he never left is the one outcome to rule
            // out, because nothing would ever place him. The vacated seat and tag are still free.
            int from_tag = -1;
            if (vehicle_seat_tag_from_index(vehicle, from_seat, &from_tag)) {
                rf::entity_attach_leech(vehicle, rider->handle, from_tag);
            }
            vehicle_boarding_init(vehicle, rider);
            xlog::warn("[vehicle] seat swap {} -> {} refused by attach on handle {}", from_seat,
                       to_seat, vehicle->handle);
            return false;
        }
        vehicle_boarding_init(vehicle, rider);
        if (rider == rf::local_player_entity) {
            // The camera is not chosen here: it is re-derived every frame from the seat's role.
            vehicle_local_rider_enter(vehicle);
        }
        return true;
    }

    // Server: attach and tell everyone. The caller has validated the seat.
    void vehicle_server_attach(rf::Entity* vehicle, rf::Entity* rider, int seat_index, int tag_handle)
    {
        rf::entity_turn_weapon_off(rider->handle, rider->ai.current_primary_weapon);
        if (!rf::entity_attach_leech(vehicle, rider->handle, tag_handle)) {
            return;
        }
        // Neither entity_attach_leech nor vehicle_boarding_init places the rider, so his position
        // here is still the one he boarded from - which is where the flag has to land.
        if (!vehicle_hull_may_carry_flags(vehicle)) {
            if (rf::Player* carrier = rf::player_from_entity_handle(rider->handle)) {
                multi_force_drop_carried_flag(carrier);
            }
        }
        vehicle_boarding_init(vehicle, rider);
        if (rider == rf::local_player_entity) {
            vehicle_local_rider_enter(vehicle);
        }
        if (seat_index == 0) {
            // The server body is released below and its Bullet velocity is the only copy a coasting
            // hull has: publish it on the entity so the seed and the announcement below both see it.
            vehicle->p_data.vel = vehicle_live_hull_velocity(vehicle);
        }
        // A driver boarding is always a fresh takeover of a parked/coasting hull, so flush the ring.
        vehicle_update_interp_ownership(vehicle, seat_index == 0);
        // Before the announcement: it is what carries the boarder's team and the cleared auto-return.
        vehicle_on_hull_boarded(vehicle, rider);
        vehicle_broadcast_seat_occupancy(vehicle->handle, vehicle, seat_index);
        vehicle_broadcast_health(vehicle, true);
        // Dropped rather than seeded, so the next server frame repeats the value: a boarding
        // announcement that raced the create packet would otherwise never be resent.
        g_vehicle_state.health_sync.erase(vehicle->handle);
        // entity_die credits the last damager with every occupant it kills, and whoever softened
        // this hull while it stood empty is not responsible for the people who just got in.
        g_vehicle_state.last_damager.erase(vehicle->handle);
        // A new occupant in ANY seat takes the vehicle over.
        g_vehicle_state.kinematics.erase(vehicle->handle);
        g_vehicle_state.coast_memory.erase(vehicle->handle);
        // Seat 0 hands the hull to that driver's client; a gunner boarding changes nothing, which is
        // the whole reason a gunner-only jeep moves at all.
        if (seat_index == 0) {
            vehicle_physics_server_release(vehicle->handle);
        }
    }

    constexpr std::string_view vehicle_enemy_occupant_deny =
        "That vehicle is occupied by an enemy.";

    constexpr std::string_view vehicle_locked_team_deny =
        "That vehicle belongs to the other team.";

    // The explicit lock: an affiliated hull whose factory set "Lock to team" is the other team's.
    // Affiliation alone restricts nothing, and a lock only bites in a team gametype.
    bool vehicle_locked_against(const rf::Player* pp, int vehicle_handle)
    {
        if (!pp || !multi_is_team_game_type()) {
            return false;
        }
        const VehicleState* st = vehicle_hull_state(vehicle_handle);
        if (!st || !st->lock_to_team || st->team < 0) {
            return false;
        }
        return pp->team != static_cast<uint8_t>(st->team);
    }

    // The seat CROSS-CHECK: host_handle is one half of the attachment, the leech handles the other.
    bool vehicle_host_holds_rider(rf::Entity* host, const rf::Entity* rider)
    {
        return vehicle_seat_of_rider(host, rider) >= 0;
    }

    // THE 0x65 payload derivation, shared by both senders: the seats themselves, never an event.
    uint8_t vehicle_build_seat_occupancy(const rf::Entity* vehicle, int32_t* out)
    {
        if (!vehicle) {
            return 0;
        }
        const int seats =
            std::min<int>(vehicle->interface_points.size(), af_vehicle_state_max_seats);
        for (int i = 0; i < seats; ++i) {
            out[i] = vehicle_seat_leech(vehicle, i);
        }
        return static_cast<uint8_t>(seats);
    }

    uint8_t vehicle_changed_seat_byte(int changed_seat)
    {
        if (changed_seat < 0 || changed_seat >= af_vehicle_state_max_seats) {
            return af_vehicle_state_changed_none;
        }
        return static_cast<uint8_t>(changed_seat);
    }

    void vehicle_server_handle_enter(rf::Player* pp, rf::Entity* rider, int vehicle_handle,
                                     uint8_t seat_index)
    {
        vehicle_clear_stale_host(rider);
        if (rider->host_handle != -1) {
            vehicle_deny(pp, "You are already in a vehicle.");
            return;
        }

        rf::Entity* vehicle = vehicle_live_synced_entity(vehicle_handle);
        if (!vehicle) {
            return;
        }

        // vehicle_seat_auto is resolved HERE because occupancy is the server's to know: a client's
        // mirror can be a packet behind, and resolving there would race two players onto one seat.
        int resolved_seat = static_cast<int>(seat_index);
        if (seat_index == vehicle_seat_auto) {
            resolved_seat = vehicle_lowest_free_seat(vehicle);
            if (resolved_seat < 0) {
                vehicle_deny(pp, "That vehicle is full.");
                return;
            }
        }
        else if (resolved_seat >= af_vehicle_state_max_seats) {
            return; // the occupancy packet cannot state this seat, so no client could mirror it
        }

        int tag_handle = -1;
        if (!vehicle_seat_tag_from_index(vehicle, resolved_seat, &tag_handle)) {
            return;
        }
        if (vehicle_seat_leech(vehicle, resolved_seat) != -1) {
            vehicle_deny(pp, "That seat is taken.");
            return;
        }
        if (rider->eye_pos.distance_to(vehicle->pos) > vehicle->info->use_radius + board_range_slack) {
            return;
        }
        if (!vehicle_sub_water_gate_ok(vehicle)) {
            vehicle_deny(pp, "The sub only works in water.");
            return;
        }
        if (vehicle_locked_against(pp, vehicle->handle)) {
            vehicle_deny(pp, vehicle_locked_team_deny);
            return;
        }
        if (vehicle_occupied_by_enemy(pp, vehicle)) {
            vehicle_deny(pp, vehicle_enemy_occupant_deny);
            return;
        }

        vehicle_server_attach(vehicle, rider, resolved_seat, tag_handle);
    }

    void vehicle_server_handle_exit(rf::Player* pp, rf::Entity* rider)
    {
        // An af_client_req 0xC exit with vehicle_handle == -1 is sendable unconditionally, so guard
        // the type: vehicle_update_interp_ownership would otherwise zero a non-vehicle host's motion.
        rf::Entity* vehicle = vehicle_ridden_hull(rider);
        if (!vehicle) {
            return;
        }
        if (vehicle->entity_flags2 & rf::EF2_NO_EXIT) {
            vehicle_deny(pp, "You cannot get out right now.");
            return;
        }
        // No ground test here: only the driver's client can answer it. The engine's own exit-spot
        // search below still runs server-side and refuses an impossible exit.

        // Published BEFORE the detach: its ownership call's reconcile destroys a listen host's body.
        const bool driver_exit = vehicle_driver_entity(vehicle) == rider;
        const rf::Vector3 exit_vel = driver_exit ? vehicle_live_hull_velocity(vehicle) : rf::Vector3{};
        if (driver_exit) {
            vehicle->p_data.vel = exit_vel;
        }
        const rf::Vector3 rider_inertia = (rider == rf::local_player_entity)
            ? vehicle_capture_exit_inertia(vehicle, rider) : rf::Vector3{};

        const VehicleExitView view = vehicle_capture_exit_view(vehicle);
        // Taken before the detach, which clears it, for the fallback broadcast below.
        const int exit_seat = vehicle_seat_of_rider(vehicle, rider);
        // The detach FunHook broadcasts the seat release for every successful detach.
        if (!rf::entity_detach_from_host(rider)) {
            // The exit-spot search refused, so the rider stays put - but only while the VEHICLE is
            // still a thing he can sit in. A client applies exactly this fallback for an exit packet.
            if (rf::entity_is_dying(vehicle)) {
                rf::entity_detach_leech(vehicle, rider->handle, false);
                rider->host_handle = -1;
                rider->host_tag_handle = -1;
                vehicle_broadcast_seat_occupancy(vehicle->handle, vehicle, exit_seat);
                vehicle_update_interp_ownership(vehicle);
                return;
            }
            vehicle_deny(pp, "There is no room to get out.");
            return;
        }
        vehicle_exit_restore(vehicle, rider, view);
        vehicle_apply_exit_inertia(rider, rider_inertia);
        if (rider == rf::local_player_entity) {
            vehicle_clear_local_enclosed_flag();
        }
        vehicle_update_interp_ownership(vehicle);

        const bool now_driverless =
            vehicle_seat_leech(vehicle, 0) == -1 && !rf::entity_is_dying(vehicle);

        // The hull remembers the DRIVER who just stepped out, so the server's own body can run people
        // over in his name. No speed gate: an exit at walking pace may still pick up speed downhill.
        if (driver_exit && now_driverless) {
            vehicle_coast_memory_set(vehicle, pp, rider);
        }
        else {
            g_vehicle_state.coast_memory.erase(vehicle->handle);
        }

        // The new server body starts from exit_vel: the ownership call above zeroed the entity's.
        if (now_driverless && vehicle_physics_server_ensure(vehicle, driver_exit ? &exit_vel : nullptr)) {
            // Server-simulated from here, so any stale replication record must go;
            // vehicle_server_body_do_frame re-seeds its tick base from the ring on its first frame.
            g_vehicle_state.kinematics.erase(vehicle->handle);
        }
    }

    // A use request naming the hull the requester is already aboard is a SEAT SWAP. Every boarding
    // gate that still means something for a man already inside is applied; use_radius and
    // sub-in-water are dropped because he is past what they guard.
    void vehicle_server_handle_seat_swap(rf::Player* pp, rf::Entity* rider, rf::Entity* vehicle,
                                         uint8_t seat_index)
    {
        if (!pp->net_data) {
            return;
        }
        if (!vehicle_is_synced_entity_type(vehicle) || rf::entity_is_dying(vehicle)) {
            return;
        }
        // "Auto" is a BOARDING sentinel only: a hotkey always names the seat it means.
        if (seat_index == vehicle_seat_auto) {
            return;
        }
        if (seat_index >= af_vehicle_state_max_seats) {
            return; // the occupancy packet cannot state this seat, so no client could mirror it
        }
        const int from_seat = vehicle_seat_of_rider(vehicle, rider);
        if (from_seat < 0) {
            return; // half attachment; vehicle_release_orphaned_riders owns that divergence
        }
        if (from_seat == static_cast<int>(seat_index)) {
            return; // already there
        }
        int to_tag = -1;
        if (!vehicle_seat_tag_from_index(vehicle, seat_index, &to_tag)) {
            return; // this hull has no such seat: a "4" on a two-seat jeep says nothing
        }
        if (vehicle_seat_leech(vehicle, seat_index) != -1) {
            vehicle_deny(pp, "That seat is taken.");
            return;
        }
        if (vehicle_locked_against(pp, vehicle->handle)) {
            vehicle_deny(pp, vehicle_locked_team_deny);
            return;
        }
        if (vehicle_occupied_by_enemy(pp, vehicle)) {
            vehicle_deny(pp, vehicle_enemy_occupant_deny);
            return;
        }

        // BEFORE anything below touches it: vehicle_update_interp_ownership zeroes it for a hull left
        // with no driver, so a swap OUT of seat 0 must carry the momentum across the handoff.
        const rf::Vector3 exit_vel = vehicle_live_hull_velocity(vehicle);

        const rf::Entity* gun_before = vehicle_firing_seat_occupant(vehicle);

        if (!vehicle_move_rider_to_seat(vehicle, rider, from_seat, static_cast<int>(seat_index))) {
            return;
        }

        // The trigger belongs to the SEAT, so it is dropped exactly when the swap CHANGED HANDS.
        // Conditional because a THIRD party may be firing, and his client reports edges only: the
        // server's frame pass can clear a held trigger but never re-arm one.
        if (gun_before != vehicle_firing_seat_occupant(vehicle)) {
            vehicle_server_stop_fire(vehicle);
        }

        const bool now_driver = seat_index == 0;
        const bool was_driver = from_seat == 0;
        if (now_driver) {
            // Symmetric to the seat-0 exit above, and it MUST precede the ownership call: on a
            // listen host that reconcile destroys the server body this is the only reader of.
            vehicle->p_data.vel = vehicle_live_hull_velocity(vehicle);
        }
        vehicle_update_interp_ownership(vehicle, now_driver);
        // Somebody is aboard, so no ex-driver's coast claim survives - the boarding rule.
        g_vehicle_state.coast_memory.erase(vehicle->handle);
        if (now_driver) {
            // Seat 0 taken: that driver's client simulates from here and the server owns no body.
            g_vehicle_state.kinematics.erase(vehicle->handle);
            vehicle_physics_server_release(vehicle->handle);
        }
        else if (was_driver && !rf::entity_is_dying(vehicle)
                 && vehicle_physics_server_ensure(vehicle, &exit_vel)) {
            // Seat 0 emptied: the server takes the hull over carrying the velocity it had. Getting
            // in ahead of the frame reconcile is what makes the pre-handoff velocity reachable.
            g_vehicle_state.kinematics.erase(vehicle->handle);
        }

        // ONE announcement naming the new seat: the occupancy already shows the old seat empty and
        // the new one his, so a receiver makes the identical atomic move with no exit machinery.
        vehicle_broadcast_seat_occupancy(vehicle->handle, vehicle, static_cast<int>(seat_index));
    }

    // The is_multi gate is load bearing: vehicle_is_synced_entity_type asks only about use_function,
    // so without it the input veto below would fire for the SINGLE PLAYER jeep.
    rf::Entity* vehicle_local_seated_vehicle()
    {
        if (!rf::is_multi) {
            return nullptr;
        }
        rf::Entity* rider = rf::local_player_entity;
        if (!rider || rf::entity_is_dying(rider)) {
            return nullptr;
        }
        return vehicle_ridden_live_hull(rider);
    }

    // By ACTION rather than by key, so a rebound weapon slot is covered too. The upper bound is NOT
    // the stock 0x3C: AF appends its own actions after a variable-length per-weapon block, so on a
    // small weapon set claiming that far would eat Drop Flag, Spray and the vote keys.
    bool vehicle_blocks_weapon_select(rf::ControlConfig* ccp, rf::ControlConfigAction action)
    {
        if (action < rf::CC_ACTION_SELECT_WEAPON_FIRST) {
            return false;
        }
        const int af_base = static_cast<int>(get_af_control(rf::AF_ACTION_FLASHLIGHT));
        const int last = std::min(static_cast<int>(rf::CC_ACTION_SELECT_WEAPON_LAST), af_base - 1);
        if (static_cast<int>(action) > last) {
            return false;
        }
        if (!rf::local_player || ccp != &rf::local_player->settings.controls) {
            return false;
        }
        return vehicle_local_seated_vehicle() != nullptr;
    }

    // What seat a Use press asks for, or -1 when the winning tag is not a seat of this vehicle (let
    // the stock handler have it). The jeep's gunner point is the one carve-out: a scan landing on it
    // asks for seat 1 explicitly and gets it or nothing.
    int vehicle_use_request_seat(rf::Entity* vehicle, int tag_handle)
    {
        const int scan_seat = vehicle_seat_index_from_tag(vehicle, tag_handle);
        if (scan_seat < 0) {
            return -1;
        }
        if (scan_seat == 1 && rf::entity_is_jeep(vehicle)) {
            return 1;
        }
        return vehicle_seat_auto;
    }

    // True when the press was consumed, false to let the stock handler run (doors, items, ...).
    bool vehicle_client_handle_use_keypress(rf::Player* pp)
    {
        rf::Entity* rider = rf::entity_from_handle(pp->entity_handle);
        if (!rider || rf::entity_is_dying(rider)) {
            return false;
        }

        vehicle_clear_stale_host(rider);
        if (rider->host_handle != -1) {
            // No ground gate: only the driver could answer one - a gunner's copy of ground_material
            // is the stale interp-driven one - and the stock exit-spot search handles placement.
            af_send_vehicle_use_request(-1, 0);
            return true;
        }

        int tag_handle = -1;
        rf::Object* target = rf::player_find_use_target(rider, &tag_handle);
        if (!target || target->type != rf::OT_ENTITY) {
            return false;
        }
        rf::Entity* vehicle = vehicle_synced_entity(target->handle);
        if (!vehicle) {
            return false;
        }
        const int seat_index = vehicle_use_request_seat(vehicle, tag_handle);
        if (seat_index < 0) {
            return false;
        }
        // The same answer the server is about to give, said locally for instant feedback.
        if (vehicle_occupied_by_enemy(pp, vehicle)) {
            hud_notification_show(std::string{vehicle_enemy_occupant_deny}, 3,
                                  HudNotificationType::Generic, true);
            return true;
        }

        af_send_vehicle_use_request(vehicle->server_handle, static_cast<uint8_t>(seat_index));
        return true;
    }

    // player_handle_use_keypress2 (cdecl, 5-byte PUSH ESI + MOV ESI,[ESP+8] prologue). On an MP
    // client the stock body only forwards a use_key_pressed packet, whose server-side handler has the
    // vehicle branches gated off, so boarding has to start here.
    FunHook<void(rf::Player*)> player_handle_use_keypress2_hook{
        0x004A29F0,
        [](rf::Player* pp) {
            if (rf::is_multi && pp && pp == rf::local_player && vehicle_level_has_factories()) {
                if (rf::is_server) {
                    // Listen server host: no request to send, run the server path directly.
                    rf::Entity* rider = rf::entity_from_handle(pp->entity_handle);
                    if (rider && !rf::entity_is_dying(rider)) {
                        vehicle_clear_stale_host(rider);
                        if (rider->host_handle != -1) {
                            // Any other host falls through to the stock handler.
                            rf::Entity* vehicle = vehicle_ridden_hull(rider);
                            if (vehicle) {
                                vehicle_server_handle_exit(pp, rider);
                                return;
                            }
                        }
                        else {
                            int tag_handle = -1;
                            rf::Object* target = rf::player_find_use_target(rider, &tag_handle);
                            if (target && target->type == rf::OT_ENTITY) {
                                rf::Entity* vehicle = vehicle_synced_entity(target->handle);
                                const int seat =
                                    vehicle ? vehicle_use_request_seat(vehicle, tag_handle) : -1;
                                if (seat >= 0) {
                                    vehicle_server_handle_enter(pp, rider, vehicle->handle,
                                                                static_cast<uint8_t>(seat));
                                    return;
                                }
                            }
                        }
                    }
                }
                else if (vehicle_client_handle_use_keypress(pp)) {
                    return;
                }
            }
            player_handle_use_keypress2_hook.call_target(pp);
        },
    };

    // Every seat release that goes through the ENGINE's detach is announced from here. A disconnect
    // never reaches it (the engine only flags the entity for delete): that is
    // vehicle_on_player_disconnect's, with the vehicle_do_frame sweep as backstop.
    FunHook<bool(rf::Entity*)> entity_detach_from_host_hook{
        0x004279D0,
        [](rf::Entity* ep) -> bool {
            rf::Entity* vehicle =
                (ep && rf::is_multi) ? rf::entity_from_handle(ep->host_handle) : nullptr;
            const int seat_index = (vehicle && vehicle_is_synced_entity_type(vehicle))
                ? vehicle_seat_of_rider(vehicle, ep) : -1;
            // Both taken BEFORE the detach, which clears ep->host_handle.
            const rf::Entity* gun_before =
                seat_index >= 0 ? vehicle_firing_seat_occupant(vehicle) : nullptr;

            const bool result = entity_detach_from_host_hook.call_target(ep);

            if (result && seat_index >= 0) {
                if (rf::is_server) {
                    if (gun_before != vehicle_firing_seat_occupant(vehicle)) {
                        vehicle_server_stop_fire(vehicle);
                    }
                    vehicle_broadcast_seat_occupancy(vehicle->handle, vehicle, seat_index);
                }
                if (ep == rf::local_player_entity) {
                    vehicle_clear_local_enclosed_flag();
                }
                vehicle_update_interp_ownership(vehicle);
            }
            return result;
        },
    };

    // player_find_use_target sweeps every object and fires a world trace per candidate; stock ran it
    // on a keypress only, so the prompt polls it at 10 Hz and re-probes at once on any move or loss.
    constexpr int64_t use_probe_interval_ms = 100;
    constexpr float use_probe_move_sq = 0.25f; // 0.5 u

    // The cached answer is only ever a live synced hull, so a non-vehicle target caches as "none"
    // rather than re-probing every frame.
    rf::Entity* vehicle_use_probe(rf::Entity* rider, int* out_tag_handle)
    {
        const int64_t now = timer::get_i64(1000);
        rf::Entity* cached = g_vehicle_state.use_probe_handle != -1
            ? vehicle_live_synced_entity(g_vehicle_state.use_probe_handle) : nullptr;
        const bool lost = g_vehicle_state.use_probe_handle != -1 && !cached;
        const bool moved = (rider->pos - g_vehicle_state.use_probe_pos).len_sq() > use_probe_move_sq;
        if (now < g_vehicle_state.use_probe_next_ms && !moved && !lost) {
            *out_tag_handle = g_vehicle_state.use_probe_tag;
            return cached;
        }
        g_vehicle_state.use_probe_next_ms = now + use_probe_interval_ms;
        g_vehicle_state.use_probe_pos = rider->pos;
        int tag_handle = -1;
        rf::Object* target = rf::player_find_use_target(rider, &tag_handle);
        rf::Entity* vehicle = (target && target->type == rf::OT_ENTITY)
            ? vehicle_live_synced_entity(target->handle) : nullptr;
        g_vehicle_state.use_probe_handle = vehicle ? vehicle->handle : -1;
        g_vehicle_state.use_probe_tag = vehicle ? tag_handle : -1;
        *out_tag_handle = g_vehicle_state.use_probe_tag;
        return vehicle;
    }

    // The vehicle a Use press would act on right now, with the entry answer the server would give.
    // False when no prompt belongs on screen at all.
    bool vehicle_use_prompt_query(int* out_class, int* out_reason)
    {
        if (!vehicle_level_has_factories()) {
            return false; // no hull can exist, so the 10 Hz player_find_use_target sweep is pure cost
        }
        if (!rf::is_multi || rf::is_dedicated_server || !rf::local_player) {
            return false;
        }
        if (multi_spectate_is_spectating()) {
            return false;
        }
        rf::Entity* rider = rf::entity_from_handle(rf::local_player->entity_handle);
        if (!rider || rf::entity_is_dying(rider) || rider->host_handle != -1) {
            return false;
        }
        int tag_handle = -1;
        rf::Entity* vehicle = vehicle_use_probe(rider, &tag_handle);
        if (!vehicle) {
            return false;
        }
        // The same tag-to-seat gate the press makes: a scan landing off the seats is the stock
        // handler's press, not a board.
        const int seat = vehicle_use_request_seat(vehicle, tag_handle);
        if (seat < 0) {
            return false;
        }
        // vehicle_server_handle_enter's own range check, so the prompt never offers a board the
        // server silently drops.
        if (rider->eye_pos.distance_to(vehicle->pos) > vehicle->info->use_radius + board_range_slack) {
            return false;
        }
        const int vehicle_class = vehicle_damage_class(vehicle);
        if (vehicle_class < 0) {
            return false;
        }
        vehicle_hull_entry_allowed(vehicle, rf::local_player, out_reason, seat);
        *out_class = vehicle_class;
        return true;
    }

    std::string vehicle_use_prompt_build(int vehicle_class, int reason)
    {
        const std::string name{vehicle_class_display_name(vehicle_class)};
        switch (reason) {
        case VEHICLE_ENTRY_OCCUPIED_OTHER_TEAM:
            return "The " + name + " is occupied by the enemy team";
        case VEHICLE_ENTRY_LOCKED_OTHER_TEAM:
            return "The " + name + " belongs to the enemy team";
        case VEHICLE_ENTRY_NO_SEAT:
            return "The " + name + " is full";
        case VEHICLE_ENTRY_SUB_OUT_OF_WATER:
            return "The " + name + " only works in water";
        case VEHICLE_ENTRY_SEAT_TAKEN:
            return "That seat is taken";
        default:
            break;
        }
        const rf::String bind = get_action_bind_name(rf::CC_ACTION_USE);
        const char* key = bind.c_str();
        return "Press " + std::string{(key && *key) ? key : "?"} + " to enter the " + name;
    }

    // Release a rider the stated occupancy no longer seats. `announced` is the packet's changed
    // seat: that one exit is the event the server is reporting, so it runs the ENGINE's detach -
    // the exit-spot search and its cues. Any other seat is a mirror repair and is released in
    // place, silently. Either way the rider is fully un-welded and his eye limits restored.
    void vehicle_client_release_rider(rf::Entity* vehicle, rf::Entity* rider, bool announced)
    {
        const VehicleExitView view = vehicle_capture_exit_view(vehicle);
        const rf::Vector3 rider_inertia = (announced && rider == rf::local_player_entity)
            ? vehicle_capture_exit_inertia(vehicle, rider) : rf::Vector3{};
        const bool detached = announced && rider->host_handle == vehicle->handle
            && rf::entity_detach_from_host(rider);
        if (!detached) {
            rf::entity_detach_leech(vehicle, rider->handle, false);
            rider->host_handle = -1;
            rider->host_tag_handle = -1;
        }
        if (rider == rf::local_player_entity) {
            vehicle_clear_local_enclosed_flag();
        }
        vehicle_exit_restore(vehicle, rider, view);
        vehicle_apply_exit_inertia(rider, rider_inertia);
    }

    // Seat this rider, who is in no seat of this hull. Stale leeches were cleared by the release
    // pass, so an attach refusal here is a real one.
    bool vehicle_client_seat_rider(rf::Entity* vehicle, rf::Entity* rider, int seat_index)
    {
        int tag_handle = -1;
        if (!vehicle_seat_tag_from_index(vehicle, seat_index, &tag_handle)) {
            return false;
        }
        if (rider->host_handle == vehicle->handle) {
            // A half attachment, released by hand: entity_detach_from_host would run the exit-spot
            // search on a man with no seat.
            rider->host_handle = -1;
            rider->host_tag_handle = -1;
        }
        else if (rider->host_handle != -1) {
            // Aboard something else; the server is authoritative. The engine detach refuses when its
            // exit-spot search finds nowhere to stand, and leaving him leeched there would seat one
            // rider in two hulls at once.
            rf::Entity* old_host = rf::entity_from_handle(rider->host_handle);
            if (!rf::entity_detach_from_host(rider)) {
                if (old_host) {
                    rf::entity_detach_leech(old_host, rider->handle, false);
                }
                rider->host_handle = -1;
                rider->host_tag_handle = -1;
            }
        }
        rf::entity_turn_weapon_off(rider->handle, rider->ai.current_primary_weapon);
        if (!rf::entity_attach_leech(vehicle, rider->handle, tag_handle)) {
            xlog::warn("[vehicle] attach failed (seat {} of '{}')", seat_index, vehicle->name);
            return false;
        }
        vehicle_boarding_init(vehicle, rider);
        if (rider == rf::local_player_entity) {
            vehicle_local_rider_enter(vehicle);
        }
        return true;
    }
} // namespace

// You may share a vehicle only with people you are on a side with; the requester is never his
// own enemy, which is what keeps a seat swap working.
bool vehicle_occupied_by_enemy(const rf::Player* pp, const rf::Entity* vehicle)
{
    if (!pp || !vehicle) {
        return false;
    }
    const bool team_game = multi_is_team_game_type();
    for (int i = 0; i < vehicle->interface_points.size(); ++i) {
        // A stale handle or a non-player occupant is not an enemy.
        const rf::Player* occupant = vehicle_seat_occupant_player(vehicle, i);
        if (!occupant || occupant == pp || (team_game && occupant->team == pp->team)) {
            continue;
        }
        return true;
    }
    return false;
}

af_vehicle_state_attrs vehicle_build_hull_attrs(int vehicle_handle)
{
    af_vehicle_state_attrs attrs{};
    // Stated by every 0x65, but read only by a receiver this packet makes the new seat-0 rider.
    if (const rf::Entity* hull = vehicle_synced_entity(vehicle_handle)) {
        const rf::Vector3 vel = vehicle_live_hull_velocity(hull);
        attrs.vel[0] = vehicle_quantize_hull_vel_axis(vel.x);
        attrs.vel[1] = vehicle_quantize_hull_vel_axis(vel.y);
        attrs.vel[2] = vehicle_quantize_hull_vel_axis(vel.z);
    }
    const VehicleState* st = vehicle_hull_state(vehicle_handle);
    if (!st) {
        return attrs;
    }
    attrs.team = (st->team == 0 || st->team == 1) ? static_cast<uint8_t>(st->team)
                                                  : af_vehicle_state_team_none;
    if (st->lock_to_team) {
        attrs.flags |= AF_VEHICLE_STATE_LOCK_TO_TEAM;
    }
    if (st->entered_once) {
        attrs.flags |= AF_VEHICLE_STATE_ENTERED_ONCE;
    }
    if (st->unoccupied_since_ms != 0) {
        const int64_t elapsed_s = (timer::get_i64(1000) - st->unoccupied_since_ms) / 1000;
        // The flag, not a floor, carries "running", so the first second is not lost off the deadline.
        attrs.flags |= AF_VEHICLE_STATE_UNOCCUPIED_RUNNING;
        attrs.unoccupied_s = static_cast<uint16_t>(std::clamp<int64_t>(elapsed_s, 0, 65535));
    }
    return attrs;
}

bool vehicle_hull_occupied(const rf::Entity* vehicle)
{
    if (!vehicle) {
        return false;
    }
    const int seats = std::min<int>(vehicle->interface_points.size(), af_vehicle_state_max_seats);
    for (int i = 0; i < seats; ++i) {
        if (vehicle_seat_leech(vehicle, i) != -1) {
            return true;
        }
    }
    return false;
}

void vehicle_broadcast_seat_occupancy(int vehicle_handle, const rf::Entity* vehicle,
                                      int changed_seat)
{
    int32_t seats[af_vehicle_state_max_seats] = {-1, -1, -1, -1, -1, -1};
    const uint8_t count = vehicle_build_seat_occupancy(vehicle, seats);
    af_send_vehicle_state_packet_to_all(vehicle_handle, seats, count,
                                        vehicle_changed_seat_byte(changed_seat),
                                        vehicle_build_hull_attrs(vehicle_handle));
    // A turret factory's READY/IN USE line is its hull's occupancy, so it changes with this packet.
    vehicle_slot_announce_for_hull(vehicle_handle);
}

void vehicle_send_seat_occupancy_to(rf::Player* pp, const rf::Entity* vehicle, int changed_seat)
{
    if (!vehicle) {
        return;
    }
    int32_t seats[af_vehicle_state_max_seats] = {-1, -1, -1, -1, -1, -1};
    const uint8_t count = vehicle_build_seat_occupancy(vehicle, seats);
    af_send_vehicle_state_packet(pp, vehicle->handle, seats, count,
                                 vehicle_changed_seat_byte(changed_seat),
                                 vehicle_build_hull_attrs(vehicle->handle));
}

bool vehicle_hull_entry_allowed(const rf::Entity* hull, const rf::Player* pp, int* reason_out,
                                int seat)
{
    // THE ORDER IS vehicle_server_handle_enter's, so the prompt names the reason the server would:
    // its seat resolution rejects a full hull (or a taken explicit seat) first, then the sub gate,
    // then lock, then occupancy.
    const bool explicit_seat = seat != static_cast<int>(vehicle_seat_auto) && seat >= 0
        && seat < af_vehicle_state_max_seats;
    int reason = VEHICLE_ENTRY_OK;
    if (!hull || !pp) {
        reason = VEHICLE_ENTRY_NO_SEAT;
    }
    else if (explicit_seat && vehicle_seat_leech(hull, seat) != -1) {
        reason = VEHICLE_ENTRY_SEAT_TAKEN;
    }
    else if (!explicit_seat && vehicle_lowest_free_seat(hull) < 0) {
        reason = VEHICLE_ENTRY_NO_SEAT;
    }
    else if (!vehicle_sub_water_gate_ok(hull)) {
        reason = VEHICLE_ENTRY_SUB_OUT_OF_WATER;
    }
    else if (vehicle_locked_against(pp, hull->handle)) {
        reason = VEHICLE_ENTRY_LOCKED_OTHER_TEAM;
    }
    else if (vehicle_occupied_by_enemy(pp, hull)) {
        reason = VEHICLE_ENTRY_OCCUPIED_OTHER_TEAM;
    }
    if (reason_out) {
        *reason_out = reason;
    }
    return reason == VEHICLE_ENTRY_OK;
}

int vehicle_hull_team(int vehicle_handle)
{
    const VehicleState* st = vehicle_hull_state(vehicle_handle);
    return st ? st->team : -1;
}

bool vehicle_clear_stale_host(rf::Entity* rider)
{
    if (!rider || rider->host_handle == -1) {
        return false;
    }
    rf::Entity* host = rf::entity_from_handle(rider->host_handle);
    // A DYING host is not a valid host: entity_die frees the seats, but the entity survives its death
    // animation and still passes the synced-type test. The seat cross-check is SERVER-ONLY: on a
    // client the seat array is a packet mirror that can lag host_handle by a frame.
    const bool seated = !host || !rf::is_server || vehicle_host_holds_rider(host, rider);
    if (host && vehicle_is_synced_entity_type(host) && !rf::entity_is_dying(host) && seated) {
        return false;
    }
    rider->host_handle = -1;
    rider->host_tag_handle = -1;
    return true;
}

// The per-frame safety net over the whole player list: whatever removed the vehicle, the rider is
// recovered next frame and the exit is broadcast so clients converge on the server's answer.
void vehicle_release_orphaned_riders()
{
    for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
        rf::Entity* rider = rf::entity_from_handle(player.entity_handle);
        if (!rider || rider->host_handle == -1) {
            continue;
        }
        const int host_handle = rider->host_handle;
        rf::Entity* host = rf::entity_from_handle(host_handle);
        // Same dying-host rule as vehicle_clear_stale_host.
        if (host && vehicle_is_synced_entity_type(host) && !rf::entity_is_dying(host)
            && vehicle_host_holds_rider(host, rider)) {
            continue;
        }
        // The REAL seat when the host still names one; none when the hull is gone, and the
        // occupancy of a vanished hull is empty - which is what releases the rider on a watcher.
        const int seat = host ? vehicle_seat_of_rider(host, rider) : -1;
        rider->host_handle = -1;
        rider->host_tag_handle = -1;
        // The wire states occupancy, so a seat this rider is being released from must be free
        // before it is read - a dying host still holds him at this point.
        if (host && seat >= 0) {
            rf::entity_detach_leech(host, rider->handle, false);
        }
        vehicle_broadcast_seat_occupancy(host_handle, host, seat);
    }
}

void vehicle_server_free_seat(rf::Entity* vehicle, int seat_index, int rider_handle)
{
    if (!vehicle || seat_index < 0) {
        return;
    }
    // Read BEFORE the detach: vehicle_update_interp_ownership below zeroes a driverless hull's
    // velocity, and the broadcast between them is what states it.
    const bool freeing_driver = seat_index == 0 && !rf::entity_is_dying(vehicle);
    const rf::Vector3 exit_vel = freeing_driver ? vehicle_live_hull_velocity(vehicle) : rf::Vector3{};
    if (freeing_driver) {
        vehicle->p_data.vel = exit_vel;
    }
    rf::entity_detach_leech(vehicle, rider_handle, false);
    vehicle_broadcast_seat_occupancy(vehicle->handle, vehicle, seat_index);
    vehicle_update_interp_ownership(vehicle);
    if (!freeing_driver) {
        return;
    }
    // Seat 0 emptied by an occupant who is not there to be asked: no ex-driver may keep a coast
    // claim, but the hull keeps the momentum a dying or disconnecting driver left it with.
    g_vehicle_state.coast_memory.erase(vehicle->handle);
    if (vehicle_physics_server_ensure(vehicle, &exit_vel)) {
        g_vehicle_state.kinematics.erase(vehicle->handle);
    }
}

void vehicle_clear_local_enclosed_flag()
{
    if (rf::local_player) {
        rf::local_player->flags &= ~rf::PF_IN_ENCLOSED_VEHICLE;
    }
}

void vehicle_on_player_disconnect(rf::Player* pp)
{
    if (!rf::is_multi || !rf::is_server || !pp) {
        return;
    }
    // Keyed by net id, which the server recycles: a joiner taking this id must not inherit a window.
    if (pp->net_data) {
        const int player_id = pp->net_data->player_id;
        g_vehicle_state.use_cooldown.erase(player_id);
        g_vehicle_state.crush_report_cooldown.erase(player_id);
        g_vehicle_state.crash_report_cooldown.erase(player_id);
        g_vehicle_state.orient_relay_cooldown.erase(player_id);
    }
    // The engine flags a leaving player's entity for delayed delete WITHOUT detaching it, so the seat
    // would only read as free once that entity is gone - by which time no client can resolve the
    // exit announcement to anybody.
    rf::Entity* rider = rf::entity_from_handle(pp->entity_handle);
    rf::Entity* vehicle = vehicle_ridden_hull(rider);
    if (!vehicle) {
        return;
    }
    const int seat = vehicle_seat_of_rider(vehicle, rider);
    if (seat < 0) {
        return; // host_handle says aboard but no seat holds him: the frame reconcile owns that
    }
    vehicle_server_free_seat(vehicle, seat, rider->handle);
    rider->host_handle = -1;
    rider->host_tag_handle = -1;
}

// Raw key DOWN state rather than a control action, because stock RF leaves 5 and 6 bound to nothing.
// rf::key_is_down consumes nothing, unlike the down-counter control_config_check_pressed drains. The
// edge is taken against last frame's mask, maintained whether or not the press is acted on.
void vehicle_poll_seat_hotkeys()
{
    constexpr int seat_hotkey_count = 6;
    static_assert(rf::KEY_1 + seat_hotkey_count - 1 == rf::KEY_6);

    uint8_t mask = 0;
    for (int i = 0; i < seat_hotkey_count; ++i) {
        if (rf::key_is_down(static_cast<rf::Key>(rf::KEY_1 + i))) {
            mask |= static_cast<uint8_t>(1u << i);
        }
    }
    const uint8_t pressed = static_cast<uint8_t>(mask & ~g_vehicle_state.seat_key_down_mask);
    g_vehicle_state.seat_key_down_mask = mask;
    if (!pressed) {
        return;
    }
    if (rf::game_paused || rf::gameseq_get_state() != rf::GS_GAMEPLAY
        || rf::console::console_is_visible() || rf::multi_chat_is_say_visible()) {
        return;
    }
    rf::Entity* vehicle = vehicle_local_seated_vehicle();
    if (!vehicle || !rf::local_player) {
        return;
    }
    rf::Timestamp& cooldown = g_vehicle_state.seat_swap_local_cooldown;
    if (cooldown.valid() && !cooldown.elapsed()) {
        return;
    }
    int seat = -1;
    for (int i = 0; i < seat_hotkey_count; ++i) {
        if (pressed & (1u << i)) {
            seat = i; // lowest key wins if two land in the same frame
            break;
        }
    }
    if (seat < 0 || seat >= vehicle->interface_points.size()) {
        return; // this hull has no such seat
    }
    if (vehicle_seat_of_rider(vehicle, rf::local_player_entity) == seat) {
        return; // already there
    }
    cooldown.set(vehicle_use_request_cooldown_ms);
    if (rf::is_server) {
        // Listen host: no request to send, the server path takes it directly.
        vehicle_server_handle_use_request(rf::local_player, vehicle->handle,
                                          static_cast<uint8_t>(seat));
    }
    else {
        af_send_vehicle_use_request(vehicle->server_handle, static_cast<uint8_t>(seat));
    }
}

// Rebuilt only when the answer changes: get_action_bind_name allocates an engine-heap rf::String,
// which must not happen on every frame the prompt is up.
const std::string& vehicle_use_prompt_text()
{
    int vehicle_class = -1;
    int reason = VEHICLE_ENTRY_OK;
    if (!vehicle_use_prompt_query(&vehicle_class, &reason)) {
        g_vehicle_state.use_prompt_text.clear();
        g_vehicle_state.use_prompt_class = -1;
        g_vehicle_state.use_prompt_reason = -1;
        return g_vehicle_state.use_prompt_text;
    }
    if (vehicle_class != g_vehicle_state.use_prompt_class || reason != g_vehicle_state.use_prompt_reason) {
        g_vehicle_state.use_prompt_class = vehicle_class;
        g_vehicle_state.use_prompt_reason = reason;
        g_vehicle_state.use_prompt_text = vehicle_use_prompt_build(vehicle_class, reason);
    }
    return g_vehicle_state.use_prompt_text;
}

int vehicle_seat_index_from_tag(const rf::Entity* vehicle, int tag_handle)
{
    if (!vehicle || tag_handle == -1) {
        return -1;
    }
    for (int i = 0; i < vehicle->interface_points.size(); ++i) {
        const rf::EntityInterfacePoint* seat = vehicle_seat(vehicle, i);
        if (seat && seat->tag_handle == tag_handle) {
            return i;
        }
    }
    return -1;
}

bool vehicle_seat_tag_from_index(const rf::Entity* vehicle, int seat_index, int* out_tag_handle)
{
    const rf::EntityInterfacePoint* seat = vehicle_seat(vehicle, seat_index);
    if (!seat) {
        return false;
    }
    *out_tag_handle = seat->tag_handle;
    return true;
}

void vehicle_server_handle_use_request(rf::Player* pp, int vehicle_handle, uint8_t seat_index)
{
    if (!rf::is_server || !pp || !pp->net_data) {
        return;
    }
    // ONE cadence window for every use subtype, taken before any work and before any deny is sent,
    // so a flood costs a map lookup and nothing else.
    rf::Timestamp& cooldown = g_vehicle_state.use_cooldown[pp->net_data->player_id];
    if (cooldown.valid() && !cooldown.elapsed()) {
        return;
    }
    cooldown.set(vehicle_use_request_cooldown_ms);

    rf::Entity* rider = rf::entity_from_handle(pp->entity_handle);
    if (!rider || rf::entity_is_dying(rider)) {
        return;
    }

    if (vehicle_handle == -1) {
        if (rider->host_handle != -1) {
            vehicle_server_handle_exit(pp, rider);
        }
        return;
    }

    // A request naming the hull the requester is ALREADY aboard is a seat swap. The stale-host clear
    // runs first, so a rider welded to a hull that no longer holds him is released rather than
    // routed into a swap on seats that disagree with his host_handle.
    vehicle_clear_stale_host(rider);
    rf::Entity* vehicle = rf::entity_from_handle(vehicle_handle);
    if (rider->host_handle != -1 && vehicle && rider->host_handle == vehicle->handle) {
        vehicle_server_handle_seat_swap(pp, rider, vehicle, seat_index);
        return;
    }

    vehicle_server_handle_enter(pp, rider, vehicle_handle, seat_index);
}

void vehicle_apply_seat_occupancy_from_packet(int vehicle_handle, const int32_t* seat_rider,
                                              int seat_count, uint8_t changed_seat,
                                              const int16_t* hull_vel)
{
    rf::Object* vehicle_obj = rf::obj_from_remote_handle(vehicle_handle);
    rf::Entity* vehicle = vehicle_obj ? vehicle_synced_entity(vehicle_obj->handle) : nullptr;
    if (!vehicle) {
        return;
    }
    if (seat_count > af_vehicle_state_max_seats) {
        seat_count = af_vehicle_state_max_seats;
    }
    if (changed_seat != af_vehicle_state_changed_none && changed_seat >= seat_count) {
        xlog::warn("[vehicle] changed seat {} outside stated count {}", changed_seat, seat_count);
        return;
    }

    // Wire handles are the server's; everything below is in local ones. A rider this machine has
    // not created yet reads as an empty seat, and the statement is held pending below so the frame
    // loop can re-apply it once his entity_create lands.
    int want[af_vehicle_state_max_seats];
    bool unresolved = false;
    for (int i = 0; i < af_vehicle_state_max_seats; ++i) {
        want[i] = -1;
        if (i >= seat_count || seat_rider[i] == -1) {
            continue;
        }
        rf::Object* rider_obj = rf::obj_from_remote_handle(seat_rider[i]);
        // Only a live player entity, and never the hull itself: entity_attach_leech has no
        // self-check and would weld the hull into its own seat.
        if (!rider_obj || rider_obj->handle == vehicle->handle
            || !rf::entity_from_handle(rider_obj->handle)
            || !rf::player_from_entity_handle(rider_obj->handle)) {
            unresolved = true;
            continue;
        }
        want[i] = rider_obj->handle;
    }
    if (!unresolved) {
        g_vehicle_state.pending_seats.erase(vehicle->handle);
    }
    else {
        // The deadline is stamped on the FIRST stash only, so the re-apply below cannot renew its
        // own bound; a genuinely newer statement replaces the array inside the same window.
        auto [it, inserted] = g_vehicle_state.pending_seats.try_emplace(vehicle->handle);
        VehiclePendingSeats& pending = it->second;
        if (inserted) {
            pending.expiry_ms = timer::get_i64(1000) + vehicle_pending_seats_ttl_ms;
        }
        pending.wire_vehicle_handle = vehicle_handle;
        pending.local_vehicle_handle = vehicle->handle;
        pending.seat_count = seat_count;
        std::fill(std::begin(pending.seat_rider), std::end(pending.seat_rider), -1);
        std::copy(seat_rider, seat_rider + seat_count, pending.seat_rider);
        if (hull_vel) {
            std::copy(hull_vel, hull_vel + 3, pending.hull_vel);
        }
    }
    const auto wants_rider = [&want](int handle) {
        return handle != -1
            && std::find(std::begin(want), std::end(want), handle) != std::end(want);
    };

    const int driver_before = vehicle_seat_leech(vehicle, 0);
    bool changed = false;

    // 1. Empty every seat the occupancy disagrees with. A rider the occupancy still seats
    //    SOMEWHERE on this hull is moving, not leaving: pass 3 makes that move atomically.
    //    Only the seats the packet actually states: a seat past seat_count is one the occupancy says
    //    nothing about, and releasing it would repeat on every packet.
    for (int i = 0; i < seat_count && i < vehicle->interface_points.size(); ++i) {
        const int cur = vehicle_seat_leech(vehicle, i);
        if (cur == -1 || want[i] == cur) {
            continue;
        }
        if (wants_rider(cur)) {
            continue;
        }
        changed = true;
        rf::Entity* rider = rf::entity_from_handle(cur);
        if (!rider) {
            // A disconnected occupant was never detached, only flagged for delayed delete.
            rf::entity_detach_leech(vehicle, cur, false);
            continue;
        }
        vehicle_client_release_rider(vehicle, rider, i == static_cast<int>(changed_seat));
    }

    // 2. Anyone welded to this hull by host_handle alone - the half attachment a mirror can land in.
    for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
        rf::Entity* rider = rf::entity_from_handle(player.entity_handle);
        if (!rider || rider->host_handle != vehicle->handle || wants_rider(rider->handle)) {
            continue;
        }
        changed = true;
        vehicle_client_release_rider(vehicle, rider, false);
    }

    // 3. Moves first, then fresh boardings: a rider vacating seat N is what frees it for whoever
    //    the occupancy puts there.
    for (int pass = 0; pass < 2; ++pass) {
        for (int i = 0; i < seat_count && i < vehicle->interface_points.size(); ++i) {
            if (want[i] == -1 || vehicle_seat_leech(vehicle, i) == want[i]) {
                continue;
            }
            rf::Entity* rider = rf::entity_from_handle(want[i]);
            if (!rider) {
                continue;
            }
            const int from_seat = vehicle_seat_of_rider(vehicle, rider);
            if ((from_seat >= 0) != (pass == 0)) {
                continue;
            }
            changed = true;
            // Only the announced seat is a boarding; every other attach here is a mirror repair,
            // and the join sweep (changed_seat 0xFF) is all repair.
            const VehicleSilentBoardingScope silent{i != static_cast<int>(changed_seat)};
            if (from_seat >= 0) {
                vehicle_move_rider_to_seat(vehicle, rider, from_seat, i);
            }
            else {
                vehicle_client_seat_rider(vehicle, rider, i);
            }
        }
    }

    if (changed) {
        // Seat 0 changing hands flushes the interp ring, so a re-boarded coasting hull does not
        // blend the new driver's first rows against stale coast samples.
        const int driver_after = vehicle_seat_leech(vehicle, 0);
        const bool driver_boarding = driver_after != -1 && driver_after != driver_before;
        // The hull's momentum across the handoff. The entity cannot carry it - a server-simulated
        // hull replicates zero - and the ownership call below is what seeds the driven body from it.
        if (driver_boarding && hull_vel && changed_seat == 0
            && rf::entity_from_handle(driver_after) == rf::local_player_entity) {
            vehicle->p_data.vel = vehicle_dequantize_hull_velocity(vehicle, hull_vel);
        }
        vehicle_update_interp_ownership(vehicle, driver_boarding);
    }
}

void vehicle_retry_pending_seat_occupancy()
{
    if (g_vehicle_state.pending_seats.empty()) {
        return;
    }
    const int64_t now = timer::get_i64(1000);
    std::erase_if(g_vehicle_state.pending_seats, [now](const auto& kv) {
        if (now >= kv.second.expiry_ms || !vehicle_live_synced_entity(kv.first)) {
            return true;
        }
        // The re-apply resolves the wire handle afresh; if the server has recycled it onto another
        // hull inside the window, this statement is no longer about anything and must be dropped.
        const rf::Object* obj = rf::obj_from_remote_handle(kv.second.wire_vehicle_handle);
        return !obj || obj->handle != kv.second.local_vehicle_handle;
    });
    // A copy: the re-apply below rewrites the very map being walked. Anything past the bound stays
    // in the map and is retried next frame.
    constexpr int max_per_pass = 32;
    VehiclePendingSeats pass[max_per_pass];
    int pass_count = 0;
    for (const auto& entry : g_vehicle_state.pending_seats) {
        if (pass_count >= max_per_pass) {
            break;
        }
        pass[pass_count++] = entry.second;
    }
    for (int i = 0; i < pass_count; ++i) {
        // changed_seat none: a repair carries no cue, however late the rider turns up.
        vehicle_apply_seat_occupancy_from_packet(pass[i].wire_vehicle_handle, pass[i].seat_rider,
                                                 pass[i].seat_count, af_vehicle_state_changed_none,
                                                 pass[i].hull_vel);
    }
}

void vehicle_apply_hull_attrs_from_packet(int vehicle_handle, uint8_t team, uint8_t flags,
                                          uint16_t unoccupied_s, const int32_t* seat_rider,
                                          int seat_count)
{
    rf::Object* vehicle_obj = rf::obj_from_remote_handle(vehicle_handle);
    // LIVE, not merely synced: entity_on_dead has already erased this hull's record and a packet
    // landing after it must not re-create one that nothing will ever clean up.
    rf::Entity* vehicle = vehicle_obj ? vehicle_live_synced_entity(vehicle_obj->handle) : nullptr;
    if (!vehicle) {
        return;
    }
    if (seat_count > af_vehicle_state_max_seats) {
        seat_count = af_vehicle_state_max_seats;
    }
    bool any_rider = false;
    for (int i = 0; i < seat_count; ++i) {
        any_rider = any_rider || seat_rider[i] != -1;
    }

    VehicleState& st = g_vehicle_state.hull_state[vehicle->handle];
    st.team = (team == 0 || team == 1) ? static_cast<int>(team) : -1;
    st.lock_to_team = (flags & AF_VEHICLE_STATE_LOCK_TO_TEAM) != 0;
    st.entered_once = (flags & AF_VEHICLE_STATE_ENTERED_ONCE) != 0;
    // Ticked locally from here: the server states the timer once, when it starts.
    if (any_rider || !(flags & AF_VEHICLE_STATE_UNOCCUPIED_RUNNING)) {
        st.unoccupied_deadline_ms = 0;
    }
    else {
        const int64_t left_s = vehicle_unoccupied_destroy_s - static_cast<int64_t>(unoccupied_s);
        st.unoccupied_deadline_ms = timer::get_i64(1000) + std::max<int64_t>(left_s, 0) * 1000;
    }
}

int64_t vehicle_hull_unoccupied_deadline(int vehicle_handle)
{
    const VehicleState* st = vehicle_hull_state(vehicle_handle);
    return st ? st->unoccupied_deadline_ms : 0;
}

void vehicle_unoccupied_hulls(std::vector<std::pair<int, int64_t>>& out)
{
    for (const auto& [handle, st] : g_vehicle_state.hull_state) {
        if (st.unoccupied_deadline_ms != 0) {
            out.emplace_back(handle, st.unoccupied_deadline_ms);
        }
    }
}

void vehicle_seats_apply_patch()
{
    player_handle_use_keypress2_hook.install();
    entity_detach_from_host_hook.install();
    entity_attach_leech_foley_hook.install();

    // Claims the weapon-selection actions for the seat hotkeys while the local player is riding.
    control_input_filter_add_veto(&vehicle_blocks_weapon_select);
}
