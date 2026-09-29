#include <algorithm>
#include <cmath>
#include <cstdint>
#include <patch_common/AsmWriter.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/FunHook.h>
#include <patch_common/MemUtils.h>
#include <common/utils/list-utils.h>
#include "vehicle.h"
#include "vehicle_physics.h"
#include "vehicle_internal.h"
#include "vehicle_view.h"
#include "../alpine_packets.h"
#include "../server_internal.h"
#include "../../hud/hud.h"
#include "../../hud/multi_spectate.h"
#include "../../misc/level.h"
#include "../../misc/player.h"
#include "../../os/console.h"
#include "../../rf/ai.h"
#include "../../rf/entity.h"
#include "../../rf/item.h"
#include "../../rf/multi.h"
#include "../../rf/object.h"
#include "../../rf/physics.h"
#include "../../rf/player/camera.h"
#include "../../rf/player/player.h"
#include "../../rf/vmesh.h"
#include "../../rf/weapon.h"

// Stock's automobile eye input, reinstated for a Bullet-driven car: vphys bypasses
// physics_simulate_entity, so the one store at 0x0049E2D6 that gives a driver his view pitch is gone.
void vehicle_feed_automobile_eye_input(rf::Entity* ep)
{
    // MP only: stock's own store is gated (0x0049E195/0x0049E1A6) and this one is not.
    if (!rf::is_multi) {
        return;
    }
    if (!(ep->p_data.flags & rf::PF_AUTOMOBILE) || !vehicle_physics_class_syncs_driver_aim(ep)) {
        return;
    }
    if (!rf::local_player_entity || vehicle_driver_entity(ep) != rf::local_player_entity) {
        return;
    }
    rf::EntityControlData& cd = ep->control_data;
    const float look = vehicle_physics_camera_owns_driver_look() ? 0.0f : ep->ai.ci.rot.x;
    cd.delta_eye_phb.x = ep->info->rot_acceleration * look * ep->p_data.frame_time_left;
    cd.delta_eye_phb.y = 0.0f;
    cd.delta_eye_phb.z = 0.0f;
}

// The vehicle the local player is currently driving (not riding as a gunner), or null.
rf::Entity* vehicle_local_driven_vehicle()
{
    // Deliberately the non-live resolver, and no rider-dying test: this gate must keep answering
    // through the hull's death animation.
    rf::Entity* rider = rf::local_player_entity;
    rf::Entity* vehicle = vehicle_ridden_hull(rider);
    return vehicle && vehicle_driver_entity(vehicle) == rider ? vehicle : nullptr;
}

// A passenger is defined by what his seat does NOT do: it neither drives nor owns the hull's weapon.
rf::Entity* vehicle_passenger_vehicle(rf::Entity* rider)
{
    if (!rider || rf::entity_is_dying(rider)) {
        return nullptr;
    }
    rf::Entity* vehicle = vehicle_ridden_live_hull(rider);
    if (!vehicle) {
        return nullptr;
    }
    if (vehicle_driver_entity(vehicle) == rider || vehicle_firing_seat_occupant(vehicle) == rider) {
        return nullptr;
    }
    return vehicle;
}

namespace
{
    // The entity whose first-person view this machine draws: the local player's, or the spectate
    // target's. Null when the view is nobody's eyes (third person, orbit, freelook, static camera).
    rf::Entity* vehicle_fp_view_entity()
    {
        if (!rf::local_player) {
            return nullptr;
        }
        if (multi_spectate_is_spectating()) {
            if (!multi_spectate_is_first_person()) {
                return nullptr;
            }
            rf::Player* target = multi_spectate_get_target_player();
            return target ? rf::entity_from_handle(target->entity_handle) : nullptr;
        }
        return rf::local_player_entity;
    }

    // The one case the stock reach test (0x0048AA30) cannot answer: it walks the LOCAL players array
    // and a spectator's own body is dead, so nothing but his seat identifies the spectated driver.
    bool vehicle_fp_spectated_ride(rf::Entity* ep)
    {
        if (!rf::is_multi || !multi_spectate_is_first_person()
            || !vehicle_is_synced_entity_type(ep)) {
            return false;
        }
        rf::Entity* viewer = vehicle_fp_view_entity();
        return viewer && vehicle_driver_entity(ep) == viewer;
    }

    // Widens the hide block's REACH to that driver; everything downstream stays stock.
    bool __cdecl vehicle_render_hide_reaches(rf::Entity* ep)
    {
        return vehicle_fp_spectated_ride(ep) || AddrCaller{0x0048AA30}.c_call<bool>(ep);
    }

    // 0x007C763C is the player the frame is rendered for and the caller compares this result against
    // it, so answering with it carries the spectated hull past that compare.
    rf::Player* __cdecl vehicle_render_hide_view_player(rf::Entity* ep)
    {
        if (vehicle_fp_spectated_ride(ep)) {
            if (rf::Player* render_player = addr_as_ref<rf::Player*>(0x007C763C)) {
                return render_player;
            }
        }
        return AddrCaller{0x0048AA90}.c_call<rf::Player*>(ep);
    }

    // entity_render's hide block exempts a jeep carrying a gunner, but keys on the SEAT being
    // occupied rather than on who is looking. The hook replaces ADD ESP,4 + TEST AL,AL, emulates the
    // cleanup and picks the branch itself rather than leaving the JZ to read flags it has clobbered.
    CodeInjection entity_render_fp_vehicle_hide_injection{
        0x004219F3,
        [](auto& regs) {
            regs.esp += 4; // the ADD ESP,4 this hook replaced
            auto* ep = reinterpret_cast<rf::Entity*>(regs.esi.value);
            const bool gunner_exemption = (regs.eax.value & 0xFF) != 0;
            bool force_hide = false;
            if (rf::is_multi && vehicle_is_synced_entity_type(ep)) {
                rf::Entity* viewer = vehicle_fp_view_entity();
                force_hide = viewer && vehicle_driver_entity(ep) == viewer;
            }
            regs.eip = gunner_exemption && !force_hide ? 0x004219FE : 0x00421C29;
        },
    };

    // gameplay_render_frame's cockpit gate (0x0043294F..0x0043297F) requires local_player_entity to
    // exist and be riding a use_function-1 vehicle, which a spectator fails outright.
    rf::Player* vehicle_cockpit_view_player()
    {
        if (!rf::is_multi || !multi_spectate_is_first_person()) {
            return nullptr;
        }
        rf::Player* target = multi_spectate_get_target_player();
        rf::Entity* viewer = target ? rf::entity_from_handle(target->entity_handle) : nullptr;
        if (!viewer) {
            return nullptr;
        }
        rf::Entity* vehicle = vehicle_synced_entity(viewer->host_handle);
        if (!vehicle) {
            return nullptr;
        }
        return vehicle_driver_entity(vehicle) == viewer ? target : nullptr;
    }

    // Replaces the MOV EAX,[local_player_entity] the gate opens with (five bytes either way) and
    // tightens the condition from RIDING to DRIVING.
    rf::Entity* __cdecl vehicle_cockpit_view_entity()
    {
        if (rf::Player* pp = vehicle_cockpit_view_player()) {
            return rf::entity_from_handle(pp->entity_handle);
        }
        rf::Entity* viewer = rf::local_player_entity;
        if (rf::is_multi && viewer) {
            rf::Entity* vehicle = vehicle_synced_entity(viewer->host_handle);
            if (vehicle && vehicle_driver_entity(vehicle) != viewer) {
                return nullptr; // a passenger or the jeep gunner: the gate's TEST EAX,EAX closes it
            }
        }
        return viewer;
    }

    void __cdecl vehicle_render_cockpit_for_view(rf::Player* render_player)
    {
        rf::Player* pp = vehicle_cockpit_view_player();
        AddrCaller{0x004A7860}.c_call(pp ? pp : render_player);
    }

    // A cheap same-level key only - VMesh::instance is the V3d pointer, not a generation counter -
    // so correctness rests on vehicle_view_level_init dropping the entry with the meshes. A -1 is
    // cached too, so a hull without the tag costs no lookup and keeps falling through to the stock
    // view below.
    struct VehicleViewPropCache
    {
        const rf::VMesh* vmesh = nullptr;
        const void* instance = nullptr;
        int index = -1;
    };
    VehicleViewPropCache g_driller_view_forward_cache;

    int vehicle_driller_view_forward_index(rf::VMesh* vmesh)
    {
        VehicleViewPropCache& cache = g_driller_view_forward_cache;
        if (cache.vmesh != vmesh || cache.instance != vmesh->instance) {
            cache.vmesh = vmesh;
            cache.instance = vmesh->instance;
            cache.index = rf::vmesh_lookup_prop_point(vmesh, "view_forward");
        }
        return cache.index;
    }

    void __cdecl vehicle_driller_view_apply(rf::Player* pp, rf::Entity* driller, rf::Vector3* out_pos, rf::Matrix3* out_orient)
    {
        if (rf::is_multi && pp && driller && driller->vmesh && pp->cockpit_data.camera_index == 0) {
            const int tag = vehicle_driller_view_forward_index(driller->vmesh);
            if (tag >= 0) {
                rf::Matrix3 tag_orient{};
                rf::vmesh_get_prop_point_transform(driller->vmesh, tag, &driller->orient, &driller->pos,
                                                   &tag_orient, out_pos);
            }
        }
        AddrCaller{0x004A8690}.c_call(pp, driller, out_pos, out_orient);
    }
} // namespace

void vehicle_view_level_init()
{
    g_driller_view_forward_cache = VehicleViewPropCache{};
}

// The transform a SPECTATED driver's cockpit mesh is posed with; false everywhere else, and the
// caller then keeps its own stock pointers. Both halves come off ONE sample of the hull frame
// (eye_hull_orient), so the cockpit cannot trail the view. NOT patched at the poser call 0x004A7907:
// player.cpp's widescreen hook owns that site and the two corrections are ORDERED, stretch last.
bool vehicle_cockpit_view_pose(rf::Vector3* out_pos, rf::Matrix3* out_orient)
{
    rf::Player* pp = vehicle_cockpit_view_player();
    if (!pp || !pp->cam) {
        return false;
    }
    // Re-resolved so the driller question below is asked about the same vehicle the gate passed.
    rf::Entity* rider = rf::entity_from_handle(pp->entity_handle);
    rf::Entity* vehicle = rider ? rf::entity_from_handle(rider->host_handle) : nullptr;
    if (!vehicle) {
        return false;
    }

    if (rf::entity_is_driller(vehicle)) {
        *out_pos = rf::camera_get_pos(pp->cam);
        *out_orient = rf::camera_get_orient(pp->cam);
        return true;
    }

    *out_pos = rider->eye_pos;
    // The fallback covers a class whose driver aim is not synced (sub, fighter): nothing publishes a
    // sample, and their eye_orient is built straight off this same live hull matrix.
    auto it = g_vehicle_state.orient.find(vehicle->handle);
    *out_orient = (it != g_vehicle_state.orient.end() && it->second.eye_hull_valid)
        ? it->second.eye_hull_orient
        : vehicle->orient;
    return true;
}

// The cockpit vmesh is stepped by 0x004A77A0, reached only from the LOCAL players sweep, which bails
// when the player's own entity is gone - so a spectator's target is never processed and never draws.
void vehicle_process_spectated_cockpit()
{
    if (rf::Player* pp = vehicle_cockpit_view_player()) {
        AddrCaller{0x004A77A0}.c_call(pp);
    }
}

bool vehicle_rider_pose_is_seat_locked(rf::Entity* ep)
{
    if (!rf::is_multi || !ep || ep->host_handle == -1) {
        return false;
    }
    rf::Entity* vehicle = rf::entity_from_handle(ep->host_handle);
    return vehicle && vehicle->info && vehicle->info->use_function == rf::ENTITY_USE_VEHICLE;
}

namespace
{
    // entity_update_liquid_status ejects the LOCAL player from a ridden mount the instant it dips
    // under water - client-only, so the server keeps his seat. The flags set before it are kept.
    CodeInjection entity_liquid_status_no_vehicle_eject_injection{
        0x00429187,
        [](auto& regs) {
            rf::Object* obj = regs.esi;
            if (rf::is_multi && obj && obj->type == rf::OT_ENTITY
                && vehicle_is_synced_entity_type(static_cast<rf::Entity*>(obj))) {
                regs.eip = 0x00429277;
            }
        },
    };

    // controls_process turns the weapon off on the entity's HOST when the fire control is not held,
    // so a jeep driver who is not shooting cancels his gunner's fire every frame. The block opens at
    // 0x00430E73 (6 bytes, fall-through, no incoming jumps; ESI is the entity), rejoins 0x00430EBF.
    CodeInjection controls_process_host_weapon_off_injection{
        0x00430E73,
        [](auto& regs) {
            if (!rf::is_multi) {
                return;
            }
            rf::Entity* rider = regs.esi;
            rf::Entity* vehicle = rider ? vehicle_synced_entity(rider->host_handle) : nullptr;
            if (vehicle
                && (rf::is_server || vehicle_firing_seat_occupant(vehicle) != rider)) {
                regs.eip = 0x00430EBF;
            }
        },
    };

    // For every seat but the jeep gunner's the rider's eye frame is a per-frame copy of the HULL's,
    // but 0x0049DE50 also rebuilds it from angles that stopped receiving input when he boarded - so
    // re-assert the copy immediately before the read rather than race the entity-list order.
    void vehicle_settle_occupant_eye_orient(rf::Entity* vehicle, rf::Entity* occupant)
    {
        if (occupant->host_handle != vehicle->handle
            || (occupant->obj_flags & rf::OF_KEEP_ORIENT_ON_HOST)) {
            return;
        }
        occupant->eye_orient = vehicle->eye_orient;
    }

    rf::Player* vehicle_weapon_eye_player(rf::Object* obj)
    {
        auto* ep = obj && obj->type == rf::OT_ENTITY ? static_cast<rf::Entity*>(obj) : nullptr;
        if (vehicle_is_synced_entity_type(ep)) {
            if (rf::Entity* occupant = vehicle_firing_seat_occupant(ep)) {
                if (rf::Player* pp = rf::player_from_entity_handle(occupant->handle)) {
                    vehicle_settle_occupant_eye_orient(ep, occupant);
                    return pp;
                }
            }
        }
        return AddrCaller{0x0048AA90}.c_call<rf::Player*>(obj);
    }

    // entity_get_weapon_fire_pos_orient dereferences this call's result unconditionally at
    // 0x0041B196 (MOV ECX,[EAX+0x14], 3 bytes), so the 5-byte CALL at 0x0041B191 is the last site
    // that can still refuse. No trampoline: the replaced CALL is rel32 and the handler always sets
    // eip. 0x0041B33F is the function's own "no rider entity" tail, which stock flow reaches after
    // the ADD ESP,8 at 0x0041B1A1 - hence the PUSH ESI at 0x0041B190 unwound by hand.
    CodeInjection entity_weapon_fire_pos_eye_player_injection{
        0x0041B191,
        [](auto& regs) {
            rf::Object* obj = regs.esi;
            rf::Player* pp = vehicle_weapon_eye_player(obj);
            if (!pp) {
                regs.esp += 4;
                regs.eip = 0x0041B33F;
                return;
            }
            regs.eax = pp;
            regs.eip = 0x0041B196;
        },
        false,
    };

    // player_fpgun_load_weapon_mesh: a vehicle weapon whose fpgun mesh is not loaded lands in the
    // engine's fatal error loop at 0x004AE1FA. 0x004AE1F3 is the MOV EAX,[ESI+0x34] that reads the
    // handle; bail through the function's own "return 0" tail (0x004AE0FA), same four pushed regs.
    CodeInjection player_fpgun_load_weapon_mesh_null_guard{
        0x004AE1F3,
        [](auto& regs) {
            if (rf::is_multi && addr_as_ref<int>(regs.esi + 0x34) == 0) {
                regs.eax = 0;
                regs.eip = 0x004AE0FA;
            }
        },
    };

    // entity_eject_shell dereferences a Player unconditionally, and for a vehicle weapon that player
    // is the jeep gunner, whose weapon_mesh_handle is null on a dedicated server. 0x0042A30E is the
    // last instruction before the branch commits; 0x0042A56E is the tail its own early-outs take.
    CodeInjection entity_eject_shell_fpgun_null_guard{
        0x0042A30E,
        [](auto& regs) {
            auto* pp = reinterpret_cast<rf::Player*>(static_cast<uintptr_t>(regs.ebx));
            if (!pp || !pp->weapon_mesh_handle) {
                regs.eip = 0x0042A56E;
            }
        },
    };

    // A body frame that IS the seat frame. Not the jeep gunner's: he aims the gun by turning.
    bool vehicle_rider_holds_seat_pose(rf::Entity* ep)
    {
        return vehicle_rider_pose_is_seat_locked(ep) && !rf::entity_is_jeep_gunner(ep);
    }

    // The seat supplies pitch and roll; the gunner's forward is the seat-plane direction on his aim's
    // compass heading, so the gun he turns stays on his aim. A hull on its side has no compass heading
    // to keep, so there his heading is simply projected into the seat plane, blended in as it tips.
    rf::Matrix3 vehicle_gunner_body_orient(const rf::Matrix3& seat, float heading)
    {
        const rf::Vector3 facing{std::sin(heading), 0.0f, std::cos(heading)};
        const rf::Vector3 side{std::cos(heading), 0.0f, -std::sin(heading)};
        auto unit = [](const rf::Vector3& v) {
            const float l = v.len();
            return l >= 1e-4f ? v * (1.0f / l) : rf::Vector3{0.0f, 0.0f, 0.0f};
        };
        rf::Vector3 compass = side.cross(seat.uvec);
        if (compass.dot_prod(facing) < 0.0f) {
            compass = compass * -1.0f;
        }
        const rf::Vector3 projected = facing - seat.uvec * facing.dot_prod(seat.uvec);
        const float upright = std::clamp((std::fabs(seat.uvec.y) - 0.1f) / 0.25f, 0.0f, 1.0f);
        const rf::Vector3 dir = unit(compass) * upright + unit(projected) * (1.0f - upright);
        float c = dir.dot_prod(seat.fvec);
        float s = dir.dot_prod(seat.rvec);
        const float len = std::sqrt(c * c + s * s);
        if (!(len >= 1e-4f)) {
            return seat;
        }
        c /= len;
        s /= len;
        rf::Matrix3 body;
        body.uvec = seat.uvec;
        body.fvec = seat.fvec * c + seat.rvec * s;
        body.rvec = seat.rvec * c - seat.fvec * s;
        return body;
    }

    // entity_should_bend_spine gates a pose modifier whose blend timer resets on every true->false
    // flip, and for a remote seated rider the answer is not steady, so the drawn pose snaps.
    FunHook<bool(rf::Entity*)> entity_should_bend_spine_hook{
        0x0041A200,
        [](rf::Entity* ep) -> bool {
            if (vehicle_rider_pose_is_seat_locked(ep)) {
                return false;
            }
            return entity_should_bend_spine_hook.call_target(ep);
        },
    };

    // Refuse only synced vehicles: a blanket is_multi refusal broke scripted entity_follow_path.
    FunHook<bool(int)> entity_process_path_hook{
        0x0040A9B0,
        [](int entity_handle) -> bool {
            if (rf::is_multi && vehicle_is_synced_entity_type(rf::entity_from_handle(entity_handle))) {
                return false;
            }
            return entity_process_path_hook.call_target(entity_handle);
        },
    };
} // namespace

rf::Entity* vehicle_fp_view_passenger_vehicle(rf::Entity** out_rider)
{
    // A DRIVER answers null here and keeps the stock first-person view.
    rf::Entity* viewer = vehicle_fp_view_entity();
    rf::Entity* vehicle = vehicle_passenger_vehicle(viewer);
    if (!vehicle) {
        return nullptr;
    }
    if (out_rider) {
        *out_rider = viewer;
    }
    return vehicle;
}

// The riders that keep an eye frame of their OWN while seated, rather than the per-frame copy of the
// hull's that entity_process_post hands every other rider (0x0041E81F). This is what the engine keys
// on OF_KEEP_ORIENT_ON_HOST: the eye copy skips them, and obj_should_sim_physics (0x00488030) does
// not, so their angles integrate from their own ControlInfo.
bool __cdecl vehicle_rider_keeps_own_orient(rf::Entity* ep)
{
    return rf::entity_is_jeep_gunner(ep) || vehicle_passenger_vehicle(ep) != nullptr;
}

// Stock maintains OF_KEEP_ORIENT_ON_HOST only for the machine's OWN local player, so elsewhere such
// a rider is never simulated and his aim angles freeze at their boarding values.
void vehicle_sync_rider_orient_flags()
{
    for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
        if (&player == rf::local_player) {
            continue; // stock owns this one, through the two retargeted call sites
        }
        rf::Entity* ep = rf::entity_from_handle(player.entity_handle);
        if (!ep) {
            continue;
        }
        const int flags = vehicle_rider_keeps_own_orient(ep)
            ? (ep->obj_flags | rf::OF_KEEP_ORIENT_ON_HOST)
            : (ep->obj_flags & ~rf::OF_KEEP_ORIENT_ON_HOST);
        ep->obj_flags = static_cast<rf::ObjectFlags>(flags);
    }
}

// A seat-locked rider's BODY follows the seat; only his LOOK is free. The tidier split (body from phb,
// look from a wide-clamped eye_phb) is foreclosed: 0x0049DE50 zeroes eye_phb.y/z for every on-foot
// player too. Writers that rebuild it upright from phb: 0x0049DE50 and multi_obj_interp_orient inside
// physics_simulate_entity (re-pinned by vphys_step's call hook), then the commit (0x0049D0A0 /
// 0x004A00FB); this pin is the last, and re-places the eye after it.
bool vehicle_pin_rider_body(rf::Entity* ep)
{
    if (!vehicle_rider_pose_is_seat_locked(ep)) {
        return false;
    }
    rf::Entity* vehicle = rf::entity_from_handle(ep->host_handle);
    if (!vehicle) {
        return false;
    }

    // The SEAT TAG's world transform, not the hull's orient: obj_attach_update (0x00487630) writes
    // these same three fields from the tag transform, so matching it is byte-identical.
    rf::Matrix3 body = vehicle->orient;
    if (ep->host_tag_handle >= 0 && vehicle->vmesh) {
        rf::Vector3 seat_pos{};
        rf::vmesh_get_prop_point_transform(vehicle->vmesh, ep->host_tag_handle, &vehicle->orient,
                                           &vehicle->pos, &body, &seat_pos);
    }
    if (rf::entity_is_jeep_gunner(ep)) {
        // From the HULL: stock never posed him from his seat tag (the flag skips it), and its axes are
        // not upright.
        body = vehicle_gunner_body_orient(vehicle->orient, ep->control_data.phb.y);
    }
    ep->orient = body;
    ep->p_data.orient = body;
    ep->p_data.next_orient = body;
    return true;
}

// Hooked at entity_process_post, the engine's own rider/host reconciliation point.
FunHook<void(rf::Entity*)> entity_process_post_passenger_orient_hook{
    0x0041E4B0,
    [](rf::Entity* ep) {
        entity_process_post_passenger_orient_hook.call_target(ep);

        // time_since_spine_bend must NOT be touched here: entity_apply_aim_bend_hook owns that field
        // and needs it to ADVANCE to ever finish fading the aim bend out.

        if (!vehicle_pin_rider_body(ep)) {
            return;
        }
        // The engine placed his eye from the upright body its commit rebuilt; not the driver's, which
        // is the hull's.
        if (ep->obj_flags & rf::OF_KEEP_ORIENT_ON_HOST) {
            AddrCaller{0x004194E0}.c_call(ep);
        }

        // multi_obj_interp_orient (0x004842E0) rebuilds the rider's body from control_data.phb, which
        // froze when he sat down (player_process_controls retargets his control block to the HULL),
        // so phb must stay the exact inverse of the seat matrix. DRIVER only: a passenger and the
        // jeep gunner carry OF_KEEP_ORIENT_ON_HOST, so 0x0049DE50 folds their LOOK into phb.y.
        if (vehicle_rider_holds_seat_pose(ep) && !(ep->obj_flags & rf::OF_KEEP_ORIENT_ON_HOST)) {
            ep->control_data.phb = vehicle_matrix_phb(ep->orient);
        }
    },
};

// Refused on the pose READ, not on the aim stream: a driver's eye_phb pitch is his live vehicle aim.
// Returning 0.0 is the engine's own completion signal, so the caller clears the timer and both bone
// overrides itself.
FunHook<float __cdecl(rf::Entity*, void*, int)> entity_apply_aim_bend_hook{
    0x0041DFB0,
    [](rf::Entity* ep, void* character_instance, int fade_in) -> float {
        if (vehicle_rider_holds_seat_pose(ep)) {
            return 0.0f;
        }
        return entity_apply_aim_bend_hook.call_target(ep, character_instance, fade_in);
    },
};

// The two seated-state gates, split by SEAT ROLE: seat 0 -> JEEP_DRIVE, the jeep gunner and every
// PASSENGER -> JEEP_GUN. Both pickers run the turret test first, which keeps turret occupants out.

// The JEEP_DRIVE gate: seat 0 and nobody else.
static bool __cdecl vehicle_state_is_vehicle_driver(rf::Entity* ep)
{
    if (rf::entity_is_jeep_driver(ep)) {
        return true; // the stock answer, single player included
    }
    if (!vehicle_rider_pose_is_seat_locked(ep)) {
        return false; // on foot, a turret occupant, or single player
    }
    rf::Entity* host = rf::entity_from_handle(ep->host_handle);
    return host && vehicle_driver_entity(host) == ep;
}

// The JEEP_GUN gate for the LOCAL player's picker, whose site (0x004A5D5C) really does ask
// entity_is_jeep_gunner in stock.
static bool __cdecl vehicle_state_is_gunner_or_passenger(rf::Entity* ep)
{
    if (rf::entity_is_jeep_gunner(ep)) {
        return true; // the stock answer, single player included
    }
    // An APC driver is seat 0 and the weapon owner, so the passenger test never catches him.
    return vehicle_rider_pose_is_seat_locked(ep) && vehicle_passenger_vehicle(ep) != nullptr;
}

// The same gate for the REMOTE picker's site (0x0041F4E8), where stock asks entity_is_jeep_driver and
// NOT the gunner test. Single player keeps the stock answer verbatim.
static bool __cdecl vehicle_state_is_gunner_or_passenger_remote(rf::Entity* ep)
{
    if (!rf::is_multi) {
        return rf::entity_is_jeep_driver(ep);
    }
    return vehicle_state_is_gunner_or_passenger(ep);
}

// Wider than vehicle_rider_pose_is_seat_locked on purpose: turret gunners included.
static bool vehicle_rider_of_synced_vehicle(rf::Entity* ep)
{
    return rf::is_multi && vehicle_ridden_hull(ep) != nullptr;
}

// entity_render's scanner block replaces the character's whole animation state with STAND at full
// weight, and the pose is evaluated from those weights at DRAW time. Answering yes to the
// entity_is_dying call it asks second skips that and leaves the seat animation in place.
static bool __cdecl vehicle_render_scanner_keeps_pose(rf::Entity* ep)
{
    if (vehicle_rider_of_synced_vehicle(ep)) {
        return true;
    }
    return rf::entity_is_dying(ep);
}

// Whether entity_render draws the rider of a use_function-1 mount at all. Stock only ever carried a
// player on a jeep, so every other class hides its occupants outright.
static bool __cdecl vehicle_render_rider_host_is_drawn(rf::Entity* host)
{
    if (rf::is_multi && vehicle_is_synced_entity_type(host)) {
        return true;
    }
    return host && rf::entity_is_jeep(host);
}

void vehicle_view_apply_patch()
{
    entity_render_fp_vehicle_hide_injection.install();
    player_fpgun_load_weapon_mesh_null_guard.install();
    entity_eject_shell_fpgun_null_guard.install();
    entity_process_path_hook.install();
    entity_should_bend_spine_hook.install();
    controls_process_host_weapon_off_injection.install();
    entity_liquid_status_no_vehicle_eject_injection.install();
    entity_process_post_passenger_orient_hook.install();
    entity_apply_aim_bend_hook.install();
    entity_weapon_fire_pos_eye_player_injection.install();

    // The two seated-state gates in both pickers: the remote one (0x0041F400) and the LOCAL player's
    // (0x004A5CD0). All four are 5-byte relative CALLs with no incoming jumps, fed by the PUSH ESI in
    // front (0x0041F4B0 / 0x004A5D25 and 0x0041F4E7 / 0x004A5D5B). Per-site retargets, so
    // entity_is_jeep_driver's and entity_is_jeep_gunner's other callers keep the stock answers.
    AsmWriter{0x0041F4B1}.call(&vehicle_state_is_vehicle_driver);
    AsmWriter{0x004A5D26}.call(&vehicle_state_is_vehicle_driver);
    AsmWriter{0x0041F4E8}.call(&vehicle_state_is_gunner_or_passenger_remote);
    AsmWriter{0x004A5D5C}.call(&vehicle_state_is_gunner_or_passenger);

    // The rail scanner's forced STAND pose and the rider-visibility gate, both inside entity_render
    // and both 5-byte relative CALLs with no incoming jumps. Fed by PUSH ESI at 0x00421AF6 and by
    // PUSH EAX at 0x00421985, whose push the ADD ESP,8 at 0x0042198B cleans with the one before it.
    AsmWriter{0x00421AF7}.call(&vehicle_render_scanner_keeps_pose);
    AsmWriter{0x00421986}.call(&vehicle_render_rider_host_is_drawn);

    // player_process_controls asks entity_is_jeep_gunner twice about the local player, and both
    // answers belong to every passenger too: 0x004A6101 is the control-block redirect (false
    // re-points EBP at the HOST's ControlInfo) and 0x004A61A2 the flag (false clears
    // OF_KEEP_ORIENT_ON_HOST at 0x004A61D0). The DRIVER answers false at both, exactly as stock.
    AsmWriter{0x004A6101}.call(&vehicle_rider_keeps_own_orient);
    AsmWriter{0x004A61A2}.call(&vehicle_rider_keeps_own_orient);

    // First-person SPECTATE of a vehicle driver: no hull, cockpit VFX instead. entity_render's reach
    // test (0x00421997, PUSH ESI at 0x00421996) and the view-player lookup behind it (0x004219A4,
    // PUSH ESI at 0x004219A3); both 5-byte CALLs of the same __cdecl(Entity*) shape.
    AsmWriter{0x00421997}.call(&vehicle_render_hide_reaches);
    AsmWriter{0x004219A4}.call(&vehicle_render_hide_view_player);

    // The cockpit: gameplay_render_frame's gate opener, a clean 5-byte MOV EAX,[local_player_entity]
    // reached only by fall-through (0x0043294F, with the five pushed arguments of the call before it
    // still live - a 0-arg __cdecl neither reads nor disturbs them), and the render call (0x0043297A).
    AsmWriter{0x0043294F}.call(&vehicle_cockpit_view_entity);
    AsmWriter{0x0043297A}.call(&vehicle_render_cockpit_for_view);

    // gameplay_render_frame's driller view apply.
    AsmWriter{0x00431C3D}.call(&vehicle_driller_view_apply);
}
