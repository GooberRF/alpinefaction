#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include <patch_common/AsmWriter.h>
#include <common/utils/list-utils.h>
#include "vehicle.h"
#include "vehicle_physics.h"
#include "vehicle_internal.h"
#include "vehicle_seats.h"
#include "vehicle_sync.h"
#include "vehicle_damage.h"
#include "vehicle_view.h"
#include "vehicle_markers.h"
#include "vehicle_render.h"
#include "vehicle_spawn.h"
#include "vehicle_tracers.h"
#include "../alpine_packets.h"
#include "../multi.h"
#include "../server_internal.h"
#include "../../hud/hud.h"
#include "../../hud/multi_spectate.h"
#include "../../misc/level.h"
#include "../../misc/alpine_settings.h"
#include "../../misc/player.h"
#include "../../sound/sound.h"
#include "../../os/console.h"
#include "../../os/os.h"
#include "../../rf/ai.h"
#include "../../rf/collide.h"
#include "../../rf/entity.h"
#include "../../rf/gameseq.h"
#include "../../rf/geometry.h"
#include "../../rf/level.h"
#include "../../rf/multi.h"
#include "../../rf/object.h"
#include "../../rf/os/frametime.h"
#include "../../rf/physics.h"
#include "../../rf/sound/sound.h"
#include "../../rf/player/camera.h"
#include "../../rf/player/control_config.h"
#include "../../rf/player/player.h"
#include "../../rf/v3d.h"
#include "../../rf/vmesh.h"

VehicleModuleState g_vehicle_state;

// Cleared by the level loader, not by vehicle_level_init: it is parsed before the module runs.
std::vector<AlpineVehicleFactoryInfo> g_vehicle_factories;

// The (pitch, heading, bank) that Matrix3::set_from_angles (0x004FBEE0) rebuilds this matrix from -
// NOT extract_angles, whose pitch is wrong.
rf::Vector3 vehicle_matrix_phb(const rf::Matrix3& orient)
{
    const float horiz =
        std::sqrt(orient.fvec.x * orient.fvec.x + orient.fvec.z * orient.fvec.z);
    return rf::Vector3{
        std::atan2(-orient.fvec.y, horiz),
        std::atan2(orient.fvec.x, orient.fvec.z),
        std::atan2(orient.rvec.y, orient.uvec.y),
    };
}

// physics_make_orient's convention, which carries no bank at all - hence the zero.
rf::Vector3 vehicle_make_orient_phb(const rf::Matrix3& orient)
{
    const float horiz =
        std::sqrt(orient.fvec.x * orient.fvec.x + orient.fvec.z * orient.fvec.z);
    const float k = horiz > 0.0001f ? orient.fvec.y / horiz : (orient.fvec.y >= 0.0f ? 1e4f : -1e4f);
    const float sin_pitch = std::clamp(k / (1.0f + std::fabs(k)), -0.9999f, 0.9999f);
    return rf::Vector3{
        std::asin(sin_pitch),
        std::atan2(orient.fvec.x, orient.fvec.z),
        0.0f,
    };
}

namespace
{
    // Far above any vehicle's max_life (5000), so it kills through the non-player damage scale.
    constexpr float vehicle_drown_damage = 1000000.0f;
    constexpr int64_t vehicle_drown_time_ms = 2000;
    constexpr int64_t vehicle_strand_time_ms = 3000; // a sub out of the water this long is scuttled

    constexpr float vehicle_void_probe = 300.0f; // world units of clear air below = out of the level
    constexpr float vehicle_void_time = 2.0f;    // held this long before the kill

    constexpr float vehicle_auto_return_pad_radius = 10.0f; // no auto-return this close to its factory
    constexpr float vehicle_auto_return_leave_radius = 12.0f; // hysteresis: the timer restarts only past this

    bool is_synced_use_function(int use_function)
    {
        // A MP level without factories has no synced hulls; its level-placed turrets stay stock props.
        if (rf::is_multi && !vehicle_level_has_factories()) {
            return false;
        }
        return use_function == rf::ENTITY_USE_VEHICLE || use_function == rf::ENTITY_USE_TURRET;
    }

    // Indexed by VehicleDamageClass; already cased for use after a possessive ("Bob's APC").
    constexpr const char* vehicle_class_display_names[VDC_COUNT] = {
        "jeep", "APC", "driller", "aesir", "submarine", "turret",
    };
} // namespace

const char* vehicle_class_display_name(int vehicle_class)
{
    if (vehicle_class < 0 || vehicle_class >= VDC_COUNT) {
        return "";
    }
    return vehicle_class_display_names[vehicle_class];
}

namespace
{
    // THE class ladder - every other taxonomy in this module is a function of its result. The flag
    // tests match the engine's own entity_is_* helpers. Any other use_function-1 hull is VDC_FIGHTER.
    int vehicle_damage_class_from_info(const rf::EntityInfo& info)
    {
        if (info.use_function == rf::ENTITY_USE_TURRET) {
            return VDC_TURRET;
        }
        if (info.flags & rf::EIF_SUB) {
            return VDC_SUB;
        }
        if (info.flags & rf::EIF_JEEP) {
            return VDC_JEEP;
        }
        if (info.flags & rf::EIF_DRILLER) {
            return VDC_DRILLER;
        }
        if (info.flags & rf::EIF_APC) {
            return VDC_APC;
        }
        return VDC_FIGHTER;
    }
} // namespace

int vehicle_damage_class_from_type(int entity_type)
{
    if (!vehicle_is_synced_entity_type(entity_type)) {
        return -1;
    }
    return vehicle_damage_class_from_info(rf::entity_types[entity_type]);
}

int vehicle_damage_class(const rf::Entity* ep)
{
    if (!vehicle_is_synced_entity_type(ep)) {
        return -1;
    }
    return vehicle_damage_class_from_info(*ep->info);
}

// Riders in the OPEN, where a simulated projectile reaches them - so lag-comp must reach them too.
bool vehicle_class_open_seats(const rf::Entity* vehicle)
{
    return vehicle_damage_class(vehicle) == VDC_JEEP;
}

namespace
{
    // A roomless hull is never submitted by the room-keyed render walk.
    bool vehicle_assign_null_room_fallback(rf::Entity& entity)
    {
        if (!rf::level.geometry) {
            return false;
        }

        rf::GRoom* assigned = nullptr;

        const rf::Vector3 up{0.0f, 1.0f, 0.0f};
        float reach = vehicle_collision_reach_along(&entity, up);
        if (reach <= 0.0f) {
            reach = entity.radius > 0.0f ? entity.radius : 1.0f;
        }
        for (const float mult : {1.0f, 2.0f, 4.0f, 8.0f}) {
            rf::Vector3 probe = entity.pos;
            probe.y += reach * mult + 0.5f;
            rf::GRoom* r = rf::find_room(rf::level.geometry, &probe);
            if (r && !r->is_sky && !r->is_invisible) {
                assigned = r;
                break;
            }
        }

        if (!assigned) {
            rf::GRoom** vis_rooms = nullptr;
            int num_vis = 0;
            rf::g_get_room_render_list(&vis_rooms, &num_vis);
            rf::GRoom* best = nullptr;
            float best_vol = -1.0f;
            rf::GRoom* best_vis = nullptr;
            float best_vis_vol = -1.0f;
            for (rf::GRoom* r : rf::level.geometry->all_rooms) {
                if (!r || r->is_detail || r->is_sky || r->is_invisible) {
                    continue;
                }
                if (entity.pos.x < r->bbox_min.x || entity.pos.x > r->bbox_max.x
                    || entity.pos.y < r->bbox_min.y || entity.pos.y > r->bbox_max.y
                    || entity.pos.z < r->bbox_min.z || entity.pos.z > r->bbox_max.z) {
                    continue;
                }
                const rf::Vector3 d = r->bbox_max - r->bbox_min;
                const float vol = d.x * d.y * d.z;
                bool in_vis = false;
                for (int i = 0; i < num_vis; ++i) {
                    if (vis_rooms[i] == r) {
                        in_vis = true;
                        break;
                    }
                }
                if (in_vis && (best_vis_vol < 0.0f || vol < best_vis_vol)) {
                    best_vis_vol = vol;
                    best_vis = r;
                }
                if (best_vol < 0.0f || vol < best_vol) {
                    best_vol = vol;
                    best = r;
                }
            }
            assigned = best_vis ? best_vis : best;
        }

        if (!assigned) {
            return false;
        }

        entity.set_room(assigned);
        return true;
    }

    // LOD 0 collision via the engine's own per-submesh opt-out at 0x0054DAA0. A one-way latch on
    // SHARED cached mesh data; never reverted.
    void vehicle_use_lod0_collision(rf::VMesh* vmesh)
    {
        if (!vmesh || rf::vmesh_get_type(vmesh) != rf::MESH_TYPE_STATIC) {
            return;
        }
        auto* v3d = static_cast<rf::V3d*>(vmesh->instance);
        if (!v3d || !v3d->meshes) {
            return;
        }
        for (int i = 0; i < v3d->num_meshes; ++i) {
            rf::VifLodMesh* lod_mesh = v3d->meshes[i].vu;
            if (!lod_mesh || lod_mesh->num_levels < 2 || lod_mesh->num_levels > 3) {
                continue;
            }
            if (rf::VifMesh* last = lod_mesh->meshes[lod_mesh->num_levels - 1]) {
                last->flags |= rf::VIF_COLLIDE_LOD0;
            }
        }
    }
} // namespace

// Room, mass and cspheres a bare entity_create can leave unset, plus PF_NET_PLAYER. The one place
// every synced hull passes through on creation, on both sides.
void vehicle_init_synced_entity(rf::Entity* ep)
{
    vehicle_use_lod0_collision(ep->vmesh); // before anything reads the mesh

    rf::Vector3 pos = ep->pos;
    ep->move(&pos);

    // entity_create's room lookup answers null for a hull whose origin is embedded in solid.
    if (!ep->room) {
        vehicle_assign_null_room_fallback(*ep);
    }

    if (ep->p_data.mass <= 0.0f && ep->info->mass > 0.0f) {
        ep->p_data.mass = ep->info->mass;
    }

    if (ep->p_data.cspheres.size() == 0) {
        const int num_cspheres = ep->vmesh ? rf::vmesh_get_num_cspheres(ep->vmesh) : 0;
        for (int i = 0; i < num_cspheres; ++i) {
            rf::PCollisionSphere sphere{};
            if (rf::vmesh_get_csphere(ep->vmesh, i, &sphere.center, &sphere.radius)) {
                sphere.spring_const = -1.0f;
                ep->p_data.cspheres.add(sphere);
            }
        }
        if (ep->p_data.cspheres.size() == 0) {
            rf::PCollisionSphere sphere{};
            sphere.radius = ep->radius;
            sphere.spring_const = -1.0f;
            ep->p_data.cspheres.add(sphere);
        }
        ep->p_data.radius = ep->radius;
    }

    // ObjInterp rebuilds the hull from control_data.phb until a keyframe arrives, so the seed must
    // be the exact set_from_angles inverse; entity_create leaves extract_angles' wrong pitch here.
    ep->control_data.phb = vehicle_matrix_phb(ep->orient);

    ep->p_data.flags |= rf::PF_NET_PLAYER;

    // CATATONIC is what stops an unmanned turret firing and a damaged one retaliating: ai_on_damage
    // (0x00407FB0) runs in the server's damage path regardless of PF_NET_PLAYER.
    rf::ai_set_mode(&ep->ai, rf::AI_MODE_CATATONIC, -1, -1);
    rf::ai_set_submode(&ep->ai, rf::AI_SUBMODE_NONE);
    rf::ai_set_target(&ep->ai, -1);
    rf::obj_set_friendliness(ep, rf::OBJ_NEUTRAL);

    // Here rather than on the first draw: the resolve is a File::find sweep plus bm::load.
    vehicle_team_textures_prime(ep);
}

void track_synced_entity(rf::Entity* ep)
{
    if (std::find(g_vehicle_state.synced_handles.begin(), g_vehicle_state.synced_handles.end(),
                  ep->handle) == g_vehicle_state.synced_handles.end()) {
        g_vehicle_state.synced_handles.push_back(ep->handle);
    }
    // Assigned, not emplaced: a recycled handle must not inherit the dead hull's ceiling.
    VehicleRegen& regen = g_vehicle_state.regen[ep->handle];
    regen = VehicleRegen{ep->life, ep->life, timer::get_i64(1000), false};
    // After the assignment above, which value-initialises the slots this fills.
    vehicle_capture_spawn_ammo(ep);
}

// interface_points is an array of POINTERS engine-side; never index it as if it held the structs.
const rf::EntityInterfacePoint* vehicle_seat(const rf::Entity* vehicle, int index)
{
    if (!vehicle || index < 0 || index >= vehicle->interface_points.size()) {
        return nullptr;
    }
    return vehicle->interface_points[index];
}

int vehicle_seat_leech(const rf::Entity* vehicle, int index)
{
    const rf::EntityInterfacePoint* seat = vehicle_seat(vehicle, index);
    return seat ? seat->leech_handle : -1;
}

rf::Entity* vehicle_driver_entity(const rf::Entity* vehicle)
{
    return rf::entity_from_handle(vehicle_seat_leech(vehicle, 0));
}

rf::Player* vehicle_seat_occupant_player(const rf::Entity* vehicle, int seat)
{
    // The entity hop is load bearing: player_from_entity_handle does not validate, so a stale seat
    // handle fed to it straight would match a live row.
    rf::Entity* occupant = rf::entity_from_handle(vehicle_seat_leech(vehicle, seat));
    return occupant ? rf::player_from_entity_handle(occupant->handle) : nullptr;
}

// From the cspheres, not p_data.radius: a fighter reaches far further below its origin than that.
float vehicle_collision_reach_along(rf::Entity* ep, const rf::Vector3& dir)
{
    float reach = 0.0f;
    for (int i = 0; i < ep->p_data.cspheres.size(); ++i) {
        const rf::PCollisionSphere& s = ep->p_data.cspheres[i];
        reach = std::max(reach, ep->orient.transform_vector(s.center).dot_prod(dir) + s.radius);
    }
    if (reach <= 0.0f) {
        reach = ep->p_data.radius > 0.0f ? ep->p_data.radius : ep->radius;
    }
    return reach;
}

bool vehicle_hull_is_turret(const rf::Entity* ep)
{
    return ep && ep->info && ep->info->use_function == rf::ENTITY_USE_TURRET;
}

bool vehicle_is_flyer(rf::Entity* ep)
{
    return ep && ep->info && ep->info->use_function == rf::ENTITY_USE_VEHICLE
        && !rf::entity_is_automobile(ep) && !rf::entity_is_sub(ep);
}

bool vehicle_is_synced_entity_type(int entity_type_index)
{
    if (entity_type_index < 0 || entity_type_index >= rf::num_entity_types
        || entity_type_index >= rf::MAX_ENTITY_TYPES) {
        return false;
    }
    return is_synced_use_function(rf::entity_types[entity_type_index].use_function);
}

bool vehicle_is_synced_entity_type(const rf::Entity* ep)
{
    return ep && ep->info && is_synced_use_function(ep->info->use_function);
}

rf::Entity* vehicle_ridden_hull(const rf::Entity* rider)
{
    return rider ? vehicle_synced_entity(rider->host_handle) : nullptr;
}

rf::Entity* vehicle_ridden_live_hull(const rf::Entity* rider)
{
    return rider ? vehicle_live_synced_entity(rider->host_handle) : nullptr;
}

void vehicle_client_do_frame()
{
    if (!rf::is_multi) {
        return;
    }
    vehicle_sync_rider_orient_flags();

    // Ahead of everything that reads a watched driver's eye frame this frame.
    vehicle_ease_aim_do_frame();

    // Render decoration only, so a dedicated server neither loads the mesh nor tracks the roll.
    if (!rf::is_dedicated_server) {
        vehicle_update_jeep_wheels();
        vehicle_update_treads();
        // Ahead of the engine frame the hook wraps (0x004B2D90), so the cockpit is stepped first.
        vehicle_process_spectated_cockpit();
    }

    // Client half of the rider reconcile; local player only.
    if (rf::local_player_entity) {
        vehicle_clear_stale_host(rf::local_player_entity);
    }

    // After the seat bookkeeping above is known good.
    if (!rf::is_dedicated_server) {
        vehicle_poll_seat_hotkeys();
    }

    // update_room's distance early-out never re-looks-up a hull that holds still, so force it the
    // way the engine does: set_room(nullptr) then update_room. Unmanned non-dying hulls, twice a second.
    if (!rf::is_dedicated_server) {
        const int64_t now_ms = timer::get_i64(1000);
        if (now_ms >= g_vehicle_state.next_room_check_ms) {
            g_vehicle_state.next_room_check_ms = now_ms + 500;
            for (rf::Entity& entity : DoublyLinkedList{rf::entity_list}) {
                if (!vehicle_is_synced_entity_type(&entity) || rf::entity_is_dying(&entity)
                    || rf::entity_get_first_leech(&entity) != -1) {
                    continue;
                }
                rf::GRoom* const old_room = entity.room;
                entity.set_room(nullptr);
                entity.update_room();
                if (!entity.room && !vehicle_assign_null_room_fallback(entity)) {
                    entity.set_room(old_room);
                }
            }
        }
    }

    // Keyed by VICTIM, so no hull teardown reaches it, and both the server mint and the client
    // report write it: without this every roadkill of the level leaves a row behind, and a recycled
    // victim handle inherits somebody else's window.
    std::erase_if(g_vehicle_state.crush_cooldown, [](const auto& kv) {
        return !kv.second.valid() || kv.second.elapsed();
    });

    if (!rf::is_server) {
        // A vehicle deleted without dying leaves stale entries a recycled handle would read.
        auto handle_is_gone = [](const auto& kv) {
            return !vehicle_is_synced_entity_type(rf::entity_from_handle(kv.first));
        };
        std::erase_if(g_vehicle_state.health, handle_is_gone);
        std::erase_if(g_vehicle_state.ammo_mirror, handle_is_gone);
        vehicle_tick_ammo_refill_mirrors();
        std::erase_if(g_vehicle_state.orient, handle_is_gone);
        std::erase_if(g_vehicle_state.orient_sent, handle_is_gone);
        std::erase_if(g_vehicle_state.observed_speed, handle_is_gone);
        // The mirror the auto-return label and the entry rules read; nothing else drops it on a
        // client, since vehicle_do_frame's own sweep is server side.
        std::erase_if(g_vehicle_state.hull_state, handle_is_gone);
        // After the entity creates of this frame's packets, so a rider named by an earlier
        // af_vehicle_state is seated the moment he exists.
        vehicle_retry_pending_seat_occupancy();
        // Safety net for a supplement that overtook the row it belongs to.
        for (rf::Entity& entity : DoublyLinkedList{rf::entity_list}) {
            if (vehicle_is_synced_entity_type(&entity)) {
                vehicle_decorate_interp_orient(&entity);
            }
        }
    }

    // Each trigger is reported on its OWN edges; an "else if" here loses the other trigger's press.
    rf::Entity* vehicle = vehicle_local_firing_vehicle();
    bool primary_held = false;
    bool alt_held = false;
    if (vehicle && rf::local_player && !rf::game_paused) {
        rf::ControlConfig* controls = &rf::local_player->settings.controls;
        primary_held = rf::control_is_control_down(controls, rf::CC_ACTION_PRIMARY_ATTACK);
        alt_held = rf::control_is_control_down(controls, rf::CC_ACTION_SECONDARY_ATTACK);
    }

    auto report = [](rf::Entity* target, bool fire_held, bool fire_alt) {
        if (!target) {
            return;
        }
        const uint8_t action = fire_held ? AF_VEHICLE_FIRE_START : AF_VEHICLE_FIRE_STOP;
        if (rf::is_server) {
            // Listen host: the local trigger takes the same request path a client's packet does.
            vehicle_server_handle_fire_request(rf::local_player, target->handle, action,
                                               fire_alt ? 1 : 0);
        }
        else {
            af_send_vehicle_fire_request(target->server_handle, action, fire_alt ? 1 : 0);
        }
    };

    const int handle = vehicle ? vehicle->handle : -1;
    if (handle != g_vehicle_state.reported_fire_vehicle) {
        rf::Entity* previous = rf::entity_from_handle(g_vehicle_state.reported_fire_vehicle);
        // The server leaves this machine out of every edge while it holds the gun, so a predicted
        // burst still on at a death in the seat would never hear its STOP. Not once someone else
        // holds it: his bursts are server edges.
        if (!rf::is_server && vehicle_is_synced_entity_type(previous)) {
            const rf::Entity* holder = vehicle_firing_seat_occupant(previous);
            if (!holder || holder == rf::local_player_entity) {
                multi_turn_weapon_off(previous);
            }
        }
        if (g_vehicle_state.reported_primary_held) {
            report(previous, false, false);
        }
        if (g_vehicle_state.reported_alt_held) {
            report(previous, false, true);
        }
        g_vehicle_state.reported_fire_vehicle = handle;
        g_vehicle_state.reported_primary_held = false;
        g_vehicle_state.reported_alt_held = false;
    }

    if (primary_held != g_vehicle_state.reported_primary_held) {
        report(vehicle, primary_held, false);
        g_vehicle_state.reported_primary_held = primary_held;
    }
    if (alt_held != g_vehicle_state.reported_alt_held) {
        report(vehicle, alt_held, true);
        g_vehicle_state.reported_alt_held = alt_held;
    }
}

namespace
{
    // Stock cues reused as the "a hull just materialised" moment: the CTF flag return sound, and the
    // Capek cane's cyan-white energy burst. Both are optional - absent entries simply do not play.
    constexpr uint16_t vehicle_factory_spawn_sound_id = stock_sound_id::flag_respawn;
    constexpr const char* vehicle_factory_spawn_vclip = "NanoAttackHit";

    void vehicle_factory_play_spawn_cues(const AlpineVehicleFactoryInfo& info)
    {
        if (!g_alpine_game_config.vehicle_respawn_markers || rf::is_dedicated_server) {
            return;
        }
        if (vehicle_factory_spawn_sound_id < rf::g_num_sounds) {
            play_local_sound_3d(vehicle_factory_spawn_sound_id, info.pos, rf::SOUND_GROUP_EFFECTS, 1.0f);
        }
        const int vclip_id = rf::vclip_lookup(vehicle_factory_spawn_vclip);
        if (vclip_id > -1) {
            rf::Vector3 pos = info.pos;
            rf::Vector3 dir = info.orient.uvec;
            rf::GRoom* room = rf::g_level_solid ? rf::find_room(rf::g_level_solid, &pos) : nullptr;
            rf::vclip_play_3d(vclip_id, room, &pos, &pos, 1.0f, -1, &dir, 0);
        }
    }

    // Sized from the victim: nothing may survive a scuttling with its rider released.
    float vehicle_scuttle_lethal_damage(const rf::Entity* ep)
    {
        return std::max(vehicle_drown_damage, ep->life + ep->armor + vehicle_drown_damage);
    }

    // The Bullet body is released first so nothing fights the death animation; nobody is credited.
    void vehicle_server_scuttle(rf::Entity* ep, float damage)
    {
        const int handle = ep->handle;
        g_vehicle_state.kinematics.erase(handle);
        g_vehicle_state.hazard_since.erase(handle);
        g_vehicle_state.void_timer.erase(handle);
        g_vehicle_state.coast_memory.erase(handle);
        vehicle_physics_server_release(handle);
        rf::obj_damage(handle, damage, -1, -1, rf::DT_ENERGY, nullptr, -1, 0);
    }
} // namespace

void vehicle_apply_factory_state_from_packet(uint16_t factory_index, uint8_t state,
                                             uint16_t s_remaining, uint8_t team)
{
    if (factory_index >= g_vehicle_state.factory_ui.size()
        || factory_index >= g_vehicle_factories.size()) {
        return;
    }
    // Before the state gate: a capture point can re-team a factory whose respawn state is unchanged.
    if (team == 0 || team == 1) {
        g_vehicle_factories[factory_index].team = static_cast<int32_t>(team);
    }
    else if (team == af_vehicle_state_team_none) {
        g_vehicle_factories[factory_index].team = -1;
    }
    if (state > AF_VEHICLE_FACTORY_ALIVE_TAKEN) {
        return; // an unknown state must not be taken for "alive", which would fire the spawn cues
    }

    VehicleFactoryUi& ui = g_vehicle_state.factory_ui[factory_index];
    const uint8_t previous = ui.state;
    const int64_t now = timer::get_i64(1000);
    const auto is_alive = [](uint8_t s) {
        return s == AF_VEHICLE_FACTORY_ALIVE_READY || s == AF_VEHICLE_FACTORY_ALIVE_TAKEN;
    };

    if (state == AF_VEHICLE_FACTORY_PENDING) {
        ui.state = AF_VEHICLE_FACTORY_PENDING;
        ui.deadline_ms = now + static_cast<int64_t>(s_remaining) * 1000;
    }
    else if (state == AF_VEHICLE_FACTORY_GIVEN_UP) {
        ui.state = AF_VEHICLE_FACTORY_GIVEN_UP;
        ui.deadline_ms = 0;
    }
    else {
        ui.state = state;
        ui.deadline_ms = 0;
        // Never on the join sweep, which only confirms the alive the mirror already defaults to,
        // and never on the READY -> TAKEN transition, which spawns nothing.
        if (!is_alive(previous)) {
            ui.spawned_ms = now;
            vehicle_factory_play_spawn_cues(g_vehicle_factories[factory_index]);
        }
    }
}

int vehicle_factory_ui_count()
{
    return static_cast<int>(g_vehicle_state.factory_ui.size());
}

const VehicleFactoryUi* vehicle_factory_ui(int i)
{
    if (i < 0 || i >= vehicle_factory_ui_count()) {
        return nullptr;
    }
    return &g_vehicle_state.factory_ui[i];
}

const AlpineVehicleFactoryInfo* vehicle_factory(int i)
{
    if (i < 0 || i >= static_cast<int>(g_vehicle_factories.size())) {
        return nullptr;
    }
    return &g_vehicle_factories[i];
}

float vehicle_factory_ui_progress(int i, int64_t now)
{
    const VehicleFactoryUi* ui = vehicle_factory_ui(i);
    const AlpineVehicleFactoryInfo* info = vehicle_factory(i);
    if (!ui || !info) {
        return 1.0f;
    }
    if (ui->state == AF_VEHICLE_FACTORY_GIVEN_UP) {
        return 0.0f;
    }
    if (ui->state != AF_VEHICLE_FACTORY_PENDING) {
        return 1.0f;
    }
    const float total_ms = info->respawn_delay_s * 1000.0f;
    if (total_ms <= 0.0f) {
        return 1.0f;
    }
    const float left_ms = static_cast<float>(ui->deadline_ms - now);
    return std::clamp(1.0f - left_ms / total_ms, 0.0f, 1.0f);
}

void vehicle_level_init()
{
    g_vehicle_state = VehicleModuleState{};
    vehicle_clear_local_enclosed_flag();
    vehicle_drop_jeep_tire_mesh();
    vehicle_view_level_init();
    vehicle_markers_level_init();
    vehicle_tracers_level_init();
    // The bm cache is rebuilt per level, so a handle resolved for the last one means nothing here.
    vehicle_tread_runtime_reset();
    vehicle_ammo_regen_runtime_reset();
}

void vehicle_do_frame()
{
    if (!rf::is_multi || !rf::is_server) {
        return;
    }

    // The rider -> vehicle reconcile, and it must run BEFORE the sweep below: the sweep's first act
    // on a vanished vehicle is to forget its handle, after which nothing can discover who rode it.
    vehicle_release_orphaned_riders();

    // A SNAPSHOT, drops applied after the pass: the drill sweep and the lag-comp trace iterate
    // synced_handles themselves, and compacting the live vector under them shows a live hull twice.
    const std::vector<int> pass = g_vehicle_state.synced_handles;
    std::vector<int> dropped;
    for (int handle : pass) {
        rf::Entity* ep = rf::entity_from_handle(handle);
        if (!ep) {
            vehicle_drop_combat_state(handle);
            g_vehicle_state.void_timer.erase(handle);
            g_vehicle_state.hazard_since.erase(handle);
            dropped.push_back(handle);
            continue;
        }
        // Catch-all for a rider whose entity vanished by a route other than disconnect.
        for (int i = 0; i < ep->interface_points.size(); ++i) {
            const int leech_handle = vehicle_seat_leech(ep, i);
            if (leech_handle == -1 || rf::entity_from_handle(leech_handle)) {
                continue;
            }
            vehicle_server_free_seat(ep, i, leech_handle);
        }

        // Only the current driver's machine ever clears the flag.
        const bool unmanned = rf::entity_get_first_leech(ep) == -1;
        if (unmanned) {
            ep->p_data.flags |= rf::PF_NET_PLAYER;
        }

        if (rf::entity_is_dying(ep)) {
            g_vehicle_state.kinematics.erase(handle); // a dying vehicle neither coasts nor sinks
            g_vehicle_state.hazard_since.erase(handle);
            g_vehicle_state.coast_memory.erase(handle); // ...nor runs anybody over

            // A Bullet body still writing pos/orient into the wreck would fight the death animation.
            vehicle_physics_server_release(handle);
            continue;
        }

        // A turret never moves, so it neither drowns nor falls into the void.
        const bool is_turret = vehicle_hull_is_turret(ep);

        // Submerged = the hull ORIGIN below this room's liquid plane, the threshold the sub's board
        // gate uses. A non-sub drowns under water, a sub strands out of it; the dwell absorbs a splash,
        // a landing bounce or a breach. Killer -1 credits nobody.
        if (!is_turret) {
            const bool is_sub = rf::entity_is_sub(ep);
            const bool submerged = ep->room && ep->room->contains_liquid
                                && ep->pos.y < ep->room->bbox_min.y + ep->room->liquid_depth;
            if (submerged == is_sub) {
                g_vehicle_state.hazard_since.erase(handle);
            }
            else {
                const int64_t now = timer::get_i64(1000);
                const int64_t since =
                    g_vehicle_state.hazard_since.try_emplace(handle, now).first->second;
                if (now - since >= (is_sub ? vehicle_strand_time_ms : vehicle_drown_time_ms)) {
                    vehicle_server_scuttle(ep, vehicle_scuttle_lethal_damage(ep));
                    continue;
                }
            }
        }

        // Void fall. A liquid room is excluded because the drown test above owns that case. Invisible
        // faces count: the chassis rests on them.
        if (!is_turret && !(ep->room && ep->room->contains_liquid)) {
            rf::Vector3 vfrom = ep->pos;
            rf::Vector3 vto = ep->pos;
            vto.y -= vehicle_void_probe;
            rf::PCollisionOut vo{};
            vo.obj_handle = -1;
            const bool nothing_below =
                !rf::collide_linesegment_world(vfrom, vto, rf::CF_PROCESS_INVISIBLE_FACES, &vo);
            float& held = g_vehicle_state.void_timer[handle];
            held = nothing_below ? held + rf::frametime : 0.0f;
            if (held >= vehicle_void_time) {
                // The flat amount, not the victim-sized one: a void fall has no rider left to free.
                vehicle_server_scuttle(ep, vehicle_drown_damage);
                continue;
            }
        }

        // Auto-return: a hull somebody has used and then left standing empty goes back to its
        // factory. Turrets never move, so an empty one is simply available where it stands.
        VehicleState* hull_state = is_turret ? nullptr : vehicle_hull_state(handle);
        if (hull_state && hull_state->entered_once) {
            const int64_t now = timer::get_i64(1000);
            const VehicleSpawnSlot* slot = vehicle_slot_for_hull(handle);
            const float pad_dist = slot ? ep->pos.distance_to(slot->pos) : 0.0f;
            const bool on_pad = slot && pad_dist <= vehicle_auto_return_pad_radius;
            const bool off_pad = !slot || pad_dist > vehicle_auto_return_leave_radius;
            if (!unmanned || on_pad) {
                const bool was_running = hull_state->unoccupied_since_ms != 0;
                hull_state->unoccupied_since_ms = 0;
                hull_state->unoccupied_deadline_ms = 0;
                // Boarding states its own occupancy; a hull back on its pad has to say so.
                if (was_running && unmanned) {
                    vehicle_broadcast_seat_occupancy(handle, ep, -1);
                }
            }
            else if (hull_state->unoccupied_since_ms == 0) {
                if (off_pad) {
                    hull_state->unoccupied_since_ms = now;
                    // A listen host draws the hull label off the same field a client mirrors.
                    hull_state->unoccupied_deadline_ms =
                        now + static_cast<int64_t>(vehicle_unoccupied_destroy_s) * 1000;
                    // The one statement clients tick their own countdown from.
                    vehicle_broadcast_seat_occupancy(handle, ep, -1);
                }
            }
            else if (now - hull_state->unoccupied_since_ms
                     >= static_cast<int64_t>(vehicle_unoccupied_destroy_s) * 1000) {
                // The drown kill, so the factory respawn follows by the same route.
                vehicle_server_scuttle(ep, vehicle_scuttle_lethal_damage(ep));
                continue;
            }
        }

        vehicle_decorate_interp_orient(ep);

        // `driverless`, not `unmanned`: a gunner is a passenger, not an owner.
        const bool driverless = vehicle_seat_leech(ep, 0) == -1;
        if (driverless && vehicle_physics_server_ensure(ep, nullptr)) {
            vehicle_server_body_do_frame(ep);
        }
        else {
            // Nothing to author: a driven hull, or a driverless one that is not server-simulated.
            vehicle_physics_server_release(handle);
            g_vehicle_state.kinematics.erase(handle);
            g_vehicle_state.coast_memory.erase(handle); // ends the ex-driver's window
        }

        // Driven hulls only: the coast sweep above owns the driverless case.
        if (!driverless) {
            vehicle_server_driven_crush_sweep(ep);
        }

        auto fire = g_vehicle_state.fire.find(handle);
        if (fire != g_vehicle_state.fire.end()) {
            // A dying occupant still holds his seat, so the seat test alone leaves the gun firing.
            rf::Player* requester = rf::multi_find_player_by_id(fire->second.requester_id);
            rf::Entity* occupant = vehicle_firing_seat_occupant(ep);
            if (!requester || !occupant || occupant->handle != requester->entity_handle
                || rf::entity_is_dying(occupant)) {
                fire->second.primary_held = false;
                fire->second.alt_held = false;
            }
            // From a copy: a fire whose recoil reaches entity_detach_from_host runs
            // vehicle_server_stop_fire, which erases this very entry. Re-find before erase.
            const VehicleFireState state = fire->second;
            vehicle_server_apply_fire(ep, state);
            if (!state.any_held()) {
                fire = g_vehicle_state.fire.find(handle);
                if (fire != g_vehicle_state.fire.end()) {
                    g_vehicle_state.fire.erase(fire);
                }
            }
        }

        vehicle_regen_do_frame(ep);
        vehicle_server_sync_health(ep);
    }

    if (!dropped.empty()) {
        std::erase_if(g_vehicle_state.synced_handles, [&dropped](int handle) {
            return std::find(dropped.begin(), dropped.end(), handle) != dropped.end();
        });
    }

    // After the per-vehicle pass, never inside it: PAIRS need every dead hull already dropped and
    // every pose already moved from its body.
    vehicle_server_ram_sweep();

    for (VehicleSpawnSlot& slot : g_vehicle_state.factories) {
        vehicle_slot_do_frame(slot);
    }
}

void vehicle_on_multi_shutdown()
{
    vehicle_tbl_overrides_revert(); // put weapons.tbl/entity.tbl back before the session ends
    vehicle_level_init();
}

namespace
{
    // The factory chunk is always parsed first: level_read_data's first chunk loop (Alpine chunks,
    // 0x00460912) ends at geometry 0x100, and entities (0x30000) are only read by the second.
    bool __cdecl vehicle_mp_level_entity_filtered(int entity_type_index)
    {
        return vehicle_level_has_factories() ? vehicle_is_synced_entity_type(entity_type_index)
                                             : rf::entity_type_has_vehicle_use(entity_type_index);
    }
} // namespace

void vehicle_apply_patches()
{
    vehicle_sync_apply_patch();
    vehicle_seats_apply_patch();
    vehicle_view_apply_patch();
    vehicle_damage_apply_patch();
    vehicle_render_install();
    vehicle_markers_apply_patch();

    // The stock MP level entity filter calls entity_type_has_vehicle_use (use_function == 1) here;
    // on a factory level, factories are the sole source of hulls AND turrets.
    AsmWriter{0x0046464d}.call(&vehicle_mp_level_entity_filtered);

    // The generic-movemode rotation branch that flies the fighter and the sub gates on obj_is_player
    // in MP, which no vehicle carries. Broadening it to the local-player-or-ride predicate is safe:
    // physics_simulate_entity diverts remote bodies at 0x0049F3F1 and automobiles at 0x0049F409.
    AsmWriter{0x0049F475}.call(&rf::obj_is_local_player_or_mount);

    vehicle_spawn_install();
    vehicle_tracers_install();
}
