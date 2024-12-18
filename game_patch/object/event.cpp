#include <patch_common/CodeInjection.h>
#include <patch_common/CallHook.h>
#include <patch_common/FunHook.h>
#include <patch_common/AsmWriter.h>
#include <common/version/version.h>
#include <common/utils/list-utils.h>
#include <xlog/xlog.h>
#include <cassert>
#include <unordered_set>
#include <algorithm>
#include <format>
#include "../misc/misc.h"
#include "../rf/object.h"
#include "../rf/event.h"
#include "../rf/entity.h"
#include "../rf/level.h"
#include "../rf/multi.h"
#include "../rf/player/player.h"
#include "../rf/os/console.h"
#include "../os/console.h"

bool event_debug_enabled;

namespace rf
{
    std::vector<Event*> find_all_events_by_type(EventType event_type)
    {
        VArray<Event*> full_event_list = event_list;
        std::vector<Event*> matching_event_list;

        for (int i = 0; i < full_event_list.size(); ++i) {
            auto* event = full_event_list[i];
            if (event && event->event_type == event_type_to_int(event_type)) {
                matching_event_list.push_back(event);
            }
        }

        return matching_event_list;
    }

    bool check_if_event_is_type(Event* event, EventType type)
    {
        return type == static_cast<EventType>(event->event_type);
    }

    bool check_if_object_is_event_type(Object* object, EventType type)
    {
        if (!object || object->type != OT_EVENT) {
            return false;
        }
        Event* event = static_cast<Event*>(object);
        return type == static_cast<EventType>(event->event_type);
    }

} // namespace rf

CodeInjection switch_model_event_custom_mesh_patch{
    0x004BB921,
    [](auto& regs) {
        auto& mesh_type = regs.ebx;
        if (mesh_type != rf::MESH_TYPE_UNINITIALIZED) {
            return;
        }
        auto& mesh_name = *static_cast<rf::String*>(regs.esi);
        std::string_view mesh_name_sv{mesh_name.c_str()};
        if (string_ends_with_ignore_case(mesh_name_sv, ".v3m")) {
            mesh_type = rf::MESH_TYPE_STATIC;
        }
        else if (string_ends_with_ignore_case(mesh_name_sv, ".v3c")) {
            mesh_type = rf::MESH_TYPE_CHARACTER;
        }
        else if (string_ends_with_ignore_case(mesh_name_sv, ".vfx")) {
            mesh_type = rf::MESH_TYPE_ANIM_FX;
        }
    },
};

CodeInjection switch_model_event_obj_lighting_and_physics_fix{
    0x004BB940,
    [](auto& regs) {
        rf::Object* obj = regs.edi;
        // Fix physics
        if (obj->vmesh) {
            rf::ObjectCreateInfo oci;
            oci.pos = obj->p_data.pos;
            oci.orient = obj->p_data.orient;
            oci.material = obj->material;
            oci.drag = obj->p_data.drag;
            oci.mass = obj->p_data.mass;
            oci.physics_flags = obj->p_data.flags;
            oci.radius = obj->radius;
            oci.vel = obj->p_data.vel;
            int num_vmesh_cspheres = rf::vmesh_get_num_cspheres(obj->vmesh);
            for (int i = 0; i < num_vmesh_cspheres; ++i) {
                rf::PCollisionSphere csphere;
                rf::vmesh_get_csphere(obj->vmesh, i, &csphere.center, &csphere.radius);
                csphere.spring_const = -1.0f;
                oci.spheres.add(csphere);
            }
            rf::physics_delete_object(&obj->p_data);
            rf::physics_create_object(&obj->p_data, &oci);
        }
    },
};

struct EventSetLiquidDepthHook : rf::Event
{
    float depth;
    float duration;
};

void __fastcall EventSetLiquidDepth_turn_on_new(EventSetLiquidDepthHook* this_)
{
    xlog::info("Processing Set_Liquid_Depth event: uid {} depth {:.2f} duration {:.2f}", this_->uid, this_->depth, this_->duration);
    if (this_->links.size() == 0) {
        xlog::trace("no links");
        rf::add_liquid_depth_update(this_->room, this_->depth, this_->duration);
    }
    else {
        for (auto room_uid : this_->links) {
            rf::GRoom* room = rf::level_room_from_uid(room_uid);
            xlog::trace("link {} {}", room_uid, room);
            if (room) {
                rf::add_liquid_depth_update(room, this_->depth, this_->duration);
            }
        }
    }
}

extern CallHook<void __fastcall (rf::GRoom*, int, rf::GSolid*)> liquid_depth_update_apply_all_GRoom_reset_liquid_hook;

void __fastcall liquid_depth_update_apply_all_GRoom_reset_liquid(rf::GRoom* room, int edx, rf::GSolid* solid) {
    liquid_depth_update_apply_all_GRoom_reset_liquid_hook.call_target(room, edx, solid);

    // check objects in room if they are in water
    auto* objp = rf::object_list.next_obj;
    while (objp != &rf::object_list) {
        if (objp->room == room) {
            if (objp->type == rf::OT_ENTITY) {
                auto* ep = static_cast<rf::Entity*>(objp);
                rf::entity_update_liquid_status(ep);
                bool is_in_liquid = ep->obj_flags & rf::OF_IN_LIQUID;
                // check if entity doesn't have 'swim' flag
                if (is_in_liquid && !rf::entity_can_swim(ep)) {
                    // he does not have swim animation - kill him
                    objp->life = 0.0f;
                }
            }
            else {
                rf::obj_update_liquid_status(objp);
            }
        }
        objp = objp->next_obj;
    }
}

CallHook<void __fastcall (rf::GRoom* room, int edx, rf::GSolid* geo)> liquid_depth_update_apply_all_GRoom_reset_liquid_hook{
    0x0045E4AC,
    liquid_depth_update_apply_all_GRoom_reset_liquid,
};

CallHook<int(rf::AiPathInfo*)> ai_path_release_on_load_level_event_crash_fix{
    0x004BBD99,
    [](rf::AiPathInfo* pathp) {
        // Clear GPathNode pointers before level load
        pathp->adjacent_node1 = nullptr;
        pathp->adjacent_node2 = nullptr;
        return ai_path_release_on_load_level_event_crash_fix.call_target(pathp);
    },
};

FunHook<void()> event_level_init_post_hook{
    0x004BD890,
    []() {
        event_level_init_post_hook.call_target();
        if (string_equals_ignore_case(rf::level.filename, "L5S2.rfl")) {
            // HACKFIX: make Set_Liquid_Depth events properties in lava control room more sensible
            xlog::trace("Changing Set_Liquid_Depth events in this level...");
            auto* event1 = static_cast<EventSetLiquidDepthHook*>(rf::event_lookup_from_uid(3940));
            auto* event2 = static_cast<EventSetLiquidDepthHook*>(rf::event_lookup_from_uid(4132));
            if (event1 && event2 && event1->duration == 0.15f && event2->duration == 0.15f) {
                event1->duration = 1.5f;
                event2->duration = 1.5f;
            }
        }
        if (string_equals_ignore_case(rf::level.filename, "L5S3.rfl")) {
            // Fix submarine exploding - change delay of two events to make submarine physics enabled later
            xlog::trace("Fixing Submarine exploding bug...");
            int uids[] = {4679, 4680};
            for (int uid : uids) {
                auto* event = rf::event_lookup_from_uid(uid);
                if (event && event->delay_seconds == 1.5f) {
                    event->delay_seconds += 1.5f;
                }
            }
        }
    },
};

extern FunHook<void __fastcall(rf::Event *)> EventMessage__turn_on_hook;
void __fastcall EventMessage__turn_on_new(rf::Event *this_)
{
    if (!rf::is_dedicated_server) EventMessage__turn_on_hook.call_target(this_);
}
FunHook<void __fastcall(rf::Event *this_)> EventMessage__turn_on_hook{
    0x004BB210,
    EventMessage__turn_on_new,
};

CodeInjection event_activate_injection{
    0x004B8BF4,
    [](auto& regs) {
        if (event_debug_enabled) {
            rf::Event* event = regs.esi;
            bool on = addr_as_ref<bool>(regs.esp + 0xC + 0xC);
            rf::console::print("Processing {} message in event {} ({})",
            on ? "ON" : "OFF", event->name, event->uid);
        }
    },
};

CodeInjection event_activate_injection2{
    0x004B8BE3,
    [](auto& regs) {
        if (event_debug_enabled) {
            rf::Event* event = regs.esi;
            bool on = regs.cl;
            rf::console::print("Delaying {} message in event {} ({})",
                on ? "ON" : "OFF", event->name, event->uid);
        }
    },
};

CodeInjection event_process_injection{
    0x004B8CF5,
    [](auto& regs) {
        if (event_debug_enabled) {
            rf::Event* event = regs.esi;
            rf::console::print("Processing {} message in event {} ({}) (delayed)",
                event->delayed_msg ? "ON" : "OFF", event->name, event->uid);
        }
    },
};

CodeInjection event_load_level_turn_on_injection{
    0x004BB9C9,
    [](auto& regs) {
        if (rf::local_player->flags & (rf::PF_KILL_AFTER_BLACKOUT|rf::PF_END_LEVEL_AFTER_BLACKOUT)) {
            // Ignore level transition if the player was going to die or game was going to end after a blackout effect
            regs.eip = 0x004BBA71;
        }
    }
};

ConsoleCommand2 debug_event_msg_cmd{
    "dbg_events",
    []() {
        event_debug_enabled = !event_debug_enabled;
    }
};

rf::Vector3 extract_yaw_vector(const rf::Matrix3& matrix)
{
    rf::Vector3 angles;
    if (matrix.fvec.x == 0.0f && matrix.fvec.z == 0.0f) {
        angles.y = 0.0f;
    }
    else {
        angles.y = std::atan2(matrix.fvec.x, matrix.fvec.z);
    }
    return angles;
}

rf::Vector3 rotate_velocity(const rf::Matrix3& old_orient, const rf::Matrix3& new_orient, const rf::Vector3& old_vel)
{
    // convert velocity to world space
    rf::Vector3 base_velocity = old_orient.copy_transpose().rvec * old_vel.x +
                                  old_orient.copy_transpose().uvec * old_vel.y +
                                  old_orient.copy_transpose().fvec * old_vel.z;

    // rotate by new orient
    return new_orient.rvec * base_velocity.x + new_orient.uvec * base_velocity.y + new_orient.fvec * base_velocity.z;
}

//todo: rewrite with codeinjection for just vel
FunHook<void(rf::Event*)> event_player_teleport_on_hook{
    0x004B9820,
    [](rf::Event* event) {
        rf::Entity* player_entity = rf::local_player_entity;

        if (!player_entity)
            return;

        // in a vehicle
        rf::Entity* host_entity = rf::entity_from_handle(player_entity->host_handle);

        if (host_entity) {
            // base game only cares about level filename, adding UID too is safer
            if (rf::level.filename == "l20s2.rfl" && event->uid == 18458) {
                player_entity = host_entity;
            }
            else {
                rf::entity_detach_from_host(player_entity);
            }
        }

        player_entity->p_data.pos = event->pos;
        player_entity->p_data.next_pos = event->pos;
        player_entity->pos = event->pos;

        // maintain relative velocity
        player_entity->p_data.vel = rotate_velocity(player_entity->p_data.orient, event->orient, player_entity->p_data.vel);

        player_entity->control_data.phb = extract_yaw_vector(event->orient);
        player_entity->orient = event->orient;
        player_entity->p_data.orient = event->orient;
        player_entity->p_data.next_orient = event->orient;
        player_entity->eye_orient = event->orient;

        // makes walking through teleporters smoother, base game turns this off for clients
        if (rf::entity_on_ground(player_entity)) {
            rf::physics_force_to_ground(player_entity);
        }
    }
};

static const std::unordered_set<rf::EventType> multiplayer_blocked_event_types = {
    rf::EventType::Load_Level,
    rf::EventType::Endgame,
    rf::EventType::Play_Video, // AF
    rf::EventType::Defuse_Nuke,
    rf::EventType::Drop_Point_Marker,
    rf::EventType::Go_Undercover,
    rf::EventType::Win_PS2_Demo
};

CodeInjection trigger_activate_linked_objects_patch{
    0x004C0383, [](auto& regs) {
        rf::Object* event_obj = regs.esi;
        rf::Event* event = static_cast<rf::Event*>(event_obj);

        xlog::warn("triggered event name: {}, uid: {}, type: {}", event->name, event->uid, event->event_type);

        // original code does not allow triggers to activate events in multiplayer
        // on AF levels, this is limited to a blocked list of events that would be problematic in multiplayer

        if (af_rfl_version(rf::level.version)) {
            if (multiplayer_blocked_event_types.find(static_cast<rf::EventType>(event->event_type)) ==
                multiplayer_blocked_event_types.end()) {
                regs.eip = 0x004C03C2; // allow activation if not in blocklist
            }
        }
    }
};

CodeInjection event_headlamp_state_on_patch{
    0x004B92EE, [](auto& regs) {
        rf::Event* event = regs.eax;

        if (af_rfl_version(rf::level.version) && event->links.empty())
        {
            rf::entity_headlamp_turn_on(rf::local_player_entity);
        }
    }
};

CodeInjection event_headlamp_state_off_patch {
    0x004BA0EE, [](auto& regs) {
        rf::Event* event = regs.eax;

        if (af_rfl_version(rf::level.version) && event->links.empty())
        {
            rf::entity_headlamp_turn_off(rf::local_player_entity);
        }
    }
};

void event_holster_player_weapon_turn_off()
{
    if (rf::local_player_entity) {
        rf::local_player_entity->entity_flags &= ~0x800;
    }
}

void event_holster_weapon_turn_off(rf::Event* event)
{
    if (!event) {
        return;
    }

    for (int link : event->links) {
        rf::Entity* ep = static_cast<rf::Entity*>(rf::entity_from_handle(link));
        if (ep) {
            ep->entity_flags &= ~0x800;
        }
    }
}

CodeInjection Event__turn_off_redirector_patch {
    0x004B9F80, [](auto& regs) {

        if (af_rfl_version(rf::level.version)) {

            rf::Event* event = regs.ecx;
            xlog::warn("event turned off {}, {}, {}", event->name, event->uid, event->event_type);

            if (event->event_type == rf::event_type_to_int(rf::EventType::Holster_Player_Weapon)) {
                event_holster_player_weapon_turn_off();
                regs.eip = 0x004BA008;
            }

            if (event->event_type == rf::event_type_to_int(rf::EventType::Holster_Weapon)) {
                event_holster_weapon_turn_off(event);
                regs.eip = 0x004BA008;
            }
        }
    }
};

void event_process_fixed_delay_patch(rf::GenericEvent* event) {

    // fixed delay handling for events broken in stock game
    if (af_rfl_version(rf::level.version) && event && event->delay_timestamp.elapsed()) {

        if (event_debug_enabled) {
            rf::console::print("Processing {} message in event {} ({}) (delayed)",
                event->delayed_msg ? "ON" : "OFF", event->name, event->uid);
        }

        if (event->delayed_msg) {
            switch (static_cast<rf::EventType>(event->event_type)) {
                case rf::EventType::UnHide: {
                    using EventTurnOnFn = void(__thiscall*)(rf::GenericEvent*);
                    static EventTurnOnFn event_turn_on = reinterpret_cast<EventTurnOnFn>(0x004BCDD0);
                    event_turn_on(event);
                    break;
                }
                case rf::EventType::Alarm_Siren: {
                    using EventTurnOnFn = void(__thiscall*)(rf::GenericEvent*);
                    static EventTurnOnFn event_turn_on = reinterpret_cast<EventTurnOnFn>(0x004BA830);
                    event_turn_on(event);
                    break;
                }
                case rf::EventType::Make_Invulnerable: {
                    using EventTurnOnFn = void(__thiscall*)(rf::GenericEvent*);
                    static EventTurnOnFn event_turn_on = reinterpret_cast<EventTurnOnFn>(0x004BC7C0);
                    event_turn_on(event);
                    break;
                }
                case rf::EventType::Cyclic_Timer: {
                    using EventTurnOnFn = void(__thiscall*)(rf::GenericEvent*);
                    static EventTurnOnFn event_turn_on = reinterpret_cast<EventTurnOnFn>(0x004BB7A0);
                    event_turn_on(event);
                    break;
                }
                case rf::EventType::Play_Sound: {
                    using EventTurnOnFn = void(__thiscall*)(rf::GenericEvent*);
                    static EventTurnOnFn event_turn_on = reinterpret_cast<EventTurnOnFn>(0x004BA440);
                    event_turn_on(event);
                    break;
                }
                default: {
                    xlog::warn("Something {} turned on", event->uid);
                }
            }
        }
        else {
            switch (static_cast<rf::EventType>(event->event_type)) {
                case rf::EventType::UnHide: {
                    using EventTurnOffFn = void(__thiscall*)(rf::GenericEvent*);
                    static EventTurnOffFn event_turn_off = reinterpret_cast<EventTurnOffFn>(0x004BCDE0);
                    event_turn_off(event);
                    break;
                }
                case rf::EventType::Alarm_Siren: {
                    using EventTurnOffFn = void(__thiscall*)(rf::GenericEvent*);
                    static EventTurnOffFn event_turn_off = reinterpret_cast<EventTurnOffFn>(0x004BA8C0);
                    event_turn_off(event);
                    break;
                }
                case rf::EventType::Make_Invulnerable: {
                    using EventTurnOffFn = void(__thiscall*)(rf::GenericEvent*);
                    static EventTurnOffFn event_turn_off = reinterpret_cast<EventTurnOffFn>(0x004BC880);
                    event_turn_off(event);
                    break;
                }
                case rf::EventType::Cyclic_Timer: {
                    using EventTurnOffFn = void(__thiscall*)(rf::GenericEvent*);
                    static EventTurnOffFn event_turn_off = reinterpret_cast<EventTurnOffFn>(0x004BB8A0);
                    event_turn_off(event);
                    break;
                }
                case rf::EventType::Play_Sound: {
                    using EventTurnOffFn = void(__thiscall*)(rf::GenericEvent*);
                    static EventTurnOffFn event_turn_off = reinterpret_cast<EventTurnOffFn>(0x004BA690);
                    event_turn_off(event);
                    break;
                }
                default: {
                    xlog::warn("Something {} turned off", event->uid);
                }
            }
        }

        if (rf::event_type_forwards_messages(event->event_type)) {
            using EventActivateLinksFn = void(__thiscall*)(rf::GenericEvent*, int, int, bool);
            static EventActivateLinksFn event_activate_links = reinterpret_cast<EventActivateLinksFn>(0x004B8B00);
            event_activate_links(event, event->trigger_handle, event->triggered_by_handle, event->delayed_msg);
        }

        event->delay_timestamp.invalidate();
    }
}

CodeInjection EventUnhide__process_patch {
    0x004BCDF0, [](auto& regs) {
        rf::GenericEvent* event = regs.ecx;
        event_process_fixed_delay_patch(event);
    }
};

CodeInjection EventAlarmSiren__process_patch {
    0x004BA880, [](auto& regs) {
        rf::GenericEvent* event = regs.ecx;
        event_process_fixed_delay_patch(event);
    }
};

CodeInjection EventMakeInvulnerable__process_patch {
    0x004BC8F0, [](auto& regs) {
        rf::GenericEvent* event = regs.ecx;
        event_process_fixed_delay_patch(event);
    }
};

CodeInjection EventCyclicTimer__process_patch {
    0x004BB7B0, [](auto& regs) {
        rf::GenericEvent* event = regs.ecx;
        event_process_fixed_delay_patch(event);
    }
};

CodeInjection EventSound__process_patch {
    0x004BA570, [](auto& regs) {
        rf::GenericEvent* event = regs.ecx;
        event_process_fixed_delay_patch(event);
    }
};

void apply_event_patches()
{
    // fix some events not working if delay value is specified (alpine levels only)
    EventUnhide__process_patch.install();
    EventAlarmSiren__process_patch.install();
    EventMakeInvulnerable__process_patch.install();
    EventCyclicTimer__process_patch.install();
    EventSound__process_patch.install();

    // allow Holster_Player_Weapon and Holster_Weapon to be turned off (alpine levels only)
    Event__turn_off_redirector_patch.install();

    // allow player flashlights via Headlamp_State (alpine levels only)
    event_headlamp_state_on_patch.install();
    event_headlamp_state_off_patch.install();

    // allow event activation from triggers in multiplayer (alpine levels only)
    trigger_activate_linked_objects_patch.install();

    // Improve player teleport behaviour
    // event_player_teleport_on_hook.install(); // disabled until teleport bug fixed

    // Allow custom mesh (not used in clutter.tbl or items.tbl) in Switch_Model event
    switch_model_event_custom_mesh_patch.install();
    switch_model_event_obj_lighting_and_physics_fix.install();

    // Fix Set_Liquid_Depth event
    AsmWriter(0x004BCBE0).jmp(&EventSetLiquidDepth_turn_on_new);
    liquid_depth_update_apply_all_GRoom_reset_liquid_hook.install();

    // Fix crash after level change (Load_Level event) caused by GNavNode pointers in AiPathInfo not being cleared for entities
    // being taken from the previous level
    ai_path_release_on_load_level_event_crash_fix.install();

    // Fix Message event crash on dedicated server
    EventMessage__turn_on_hook.install();

    // Level specific event fixes
    event_level_init_post_hook.install();

    // Debug event messages
    event_activate_injection.install();
    event_activate_injection2.install();
    event_process_injection.install();

    // Do not load next level if blackout is in progress
    event_load_level_turn_on_injection.install();

    // Register commands
    debug_event_msg_cmd.register_cmd();
}
