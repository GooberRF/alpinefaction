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
#include "../rf/file/file.h"
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

    // used by HUD Messages, including HUD_Message events
    std::optional<int> get_named_goal_value(std::string_view goal_name)
    {
        rf::String rf_goal_name(goal_name.data());

        // Try normal goal
        if (auto* named_event = rf::event_find_named_event(&rf_goal_name)) {
            int* current_value_ptr = reinterpret_cast<int*>(&named_event->event_specific_data[8]);
            return *current_value_ptr;
        }

        // Try persistent goal
        if (auto* persistent = rf::event_lookup_persistent_goal_event(rf_goal_name)) {
            return persistent->count;
        }

        return std::nullopt;
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
        rf::console::print("Printing of event debug information is {}.", event_debug_enabled ? "enabled" : "disabled");
    },
    "Print debug information for events",
    "dbg_events",
};

ConsoleCommand2 debug_goal_cmd{
    "dbg_goal",
    [](std::string name) {
        rf::console::print("Checking for a goal named '{}'...", name);
        rf::String goal_name = name.c_str();
        bool found_a_goal = 0;

        // try to find a normal goal
        rf::GenericEvent* named_event = rf::event_find_named_event(&goal_name);
        if (named_event) {
            found_a_goal = true;
            int* goal_count_ptr = reinterpret_cast<int*>(&named_event->event_specific_data[8]);
            int* goal_initial_ptr = reinterpret_cast<int*>(&named_event->event_specific_data[0]);            

            rf::console::print("Non-persistent goal named '{}' found! Initial value: {}, current value: {}",
                name, *goal_initial_ptr, *goal_count_ptr);
        }

        // try to find a persistent goal
        rf::PersistentGoalEvent* persist_event = rf::event_lookup_persistent_goal_event(goal_name);
        if (persist_event) {
            found_a_goal = true;

            rf::console::print("Persistent goal named '{}' found! Initial value: {}, current value: {}",
                name, persist_event->initial_count, persist_event->count);
        }

        if (!found_a_goal) {
            rf::console::print("No goals named '{}' were found.", name);
        }
    },
    "Look up the current value of a named goal",
    "dbg_goal <name>",
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
        if (af_rfl_version(rf::level.version)) {

            rf::Object* event_obj = regs.esi;
            rf::Event* event = static_cast<rf::Event*>(event_obj);

            //xlog::warn("triggered event name: {}, uid: {}, type: {}", event->name, event->uid, event->event_type);

            // original code does not allow triggers to activate events in multiplayer
            // on AF levels, this is limited to a blocked list of events that would be problematic in multiplayer        
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
            //xlog::warn("event turned off {}, {}, {}", event->name, event->uid, event->event_type);

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

CodeInjection EventUnhide__process_patch{
    0x004BCDF0, 
    [] (const auto& regs) {
        rf::Event* const event = regs.ecx;
        rf::Event__process(event);
    }
};

CodeInjection EventMakeInvulnerable__process_patch{
    0x004BC8F0,
    [] (const auto& regs) {
        rf::Event* const event = regs.ecx;
        rf::Event__process(event);
    }
};

//directional events
CodeInjection level_read_events_patch {
    0x0046231D, [](auto& regs) {

        rf::String* class_name = regs.edx;
        rf::File* file = regs.edi;

        if (class_name) {
            //xlog::warn("Class name: {}", class_name->c_str());
            int event_type = rf::event_lookup_type(class_name);

            if (event_type == static_cast<int>(rf::EventType::AF_Teleport_Player) ||
                event_type == static_cast<int>(rf::EventType::Clone_Entity)) {
                regs.eax = reinterpret_cast<rf::Matrix3*>(regs.esp + 0x9C - 0x30);
                file->read_matrix(regs.eax, 300, &rf::file_default_matrix);
            }

            if (event_type == static_cast<int>(rf::EventType::Anchor_Marker_Orient)) {
                regs.eax = reinterpret_cast<rf::Matrix3*>(regs.esp + 0x9C - 0x30);
                file->read_matrix(regs.eax, 301, &rf::file_default_matrix);
            }
        }
    }
};

FunHook<char*(char*)> hud_translate_special_character_token_hook{
    0x004385C0,
    [](char* token) {
        std::string token_str{token};
        std::string replacement;

        // Safely retrieve a string for the key/mouse binding for a given action name
        const auto get_binding_or_unbound = [](const char* action_name) -> std::string {
            if (!rf::local_player)
                return "UNBOUND";

            int action_index = rf::control_config_find_action_by_name(&rf::local_player->settings, action_name);
            if (action_index < 0)
                return "UNBOUND";

            const auto& binding = rf::local_player->settings.controls.bindings[action_index];
            std::string result;

            if (binding.scan_codes[0] >= 0) {
                rf::String key_name;
                rf::control_config_get_key_name(&key_name, binding.scan_codes[0]);
                result = std::string(key_name.c_str());
            }

            if (binding.mouse_btn_id >= 0) {
                rf::String mouse_name;
                rf::control_config_get_mouse_button_name(&mouse_name, binding.mouse_btn_id);
                if (!result.empty())
                    result += ", ";
                result += std::string(mouse_name.c_str());
            }

            return result.empty() ? "UNBOUND" : result;
        };

        // Match known HUD tokens
        if (string_equals_ignore_case(token_str, "FIRE"))
            replacement = get_binding_or_unbound(rf::strings::fire);
        else if (string_equals_ignore_case(token_str, "ALT_FIRE"))
            replacement = get_binding_or_unbound(rf::strings::alt_fire);
        else if (string_equals_ignore_case(token_str, "USE"))
            replacement = get_binding_or_unbound(rf::strings::use);
        else if (string_equals_ignore_case(token_str, "JUMP"))
            replacement = get_binding_or_unbound(rf::strings::jump);
        else if (string_equals_ignore_case(token_str, "CROUCH"))
            replacement = get_binding_or_unbound(rf::strings::crouch);
        else if (string_equals_ignore_case(token_str, "HOLSTER"))
            replacement = get_binding_or_unbound(rf::strings::holster);
        else if (string_equals_ignore_case(token_str, "RELOAD"))
            replacement = get_binding_or_unbound(rf::strings::reload);
        else if (string_equals_ignore_case(token_str, "NEXT_WEAPON"))
            replacement = get_binding_or_unbound(rf::strings::next_weapon);
        else if (string_equals_ignore_case(token_str, "PREV_WEAPON"))
            replacement = get_binding_or_unbound(rf::strings::prev_weapon);
        else if (string_equals_ignore_case(token_str, "MESSAGE_LOG"))
            replacement = get_binding_or_unbound(rf::strings::message_log);
        else if (string_equals_ignore_case(token_str, "QUICK_SAVE"))
            replacement = get_binding_or_unbound(rf::strings::quick_save);
        else if (string_equals_ignore_case(token_str, "QUICK_LOAD"))
            replacement = get_binding_or_unbound(rf::strings::quick_load);
        else if (string_equals_ignore_case(token_str, "HEADLAMP"))
            replacement = get_binding_or_unbound("Toggle headlamp");
        else if (string_equals_ignore_case(token_str, "SKIP_CUTSCENE"))
            replacement = get_binding_or_unbound("Skip cutscene");
        else if (string_starts_with_ignore_case(token_str, "goal_")) {
            std::string goal_name = token_str.substr(5);
            if (auto goal_val = rf::get_named_goal_value(goal_name)) {
                replacement = std::to_string(*goal_val);
            } else {
                replacement = "UNKNOWN GOAL";
            }
        } else {
            // Unrecognized token, just echo it
            replacement = token_str;
        }

        // Append trailing $ and copy into provided buffer
        replacement += "$";
        size_t copy_len = std::min(replacement.size(), size_t(255));
        std::memcpy(token, replacement.data(), copy_len);
        token[copy_len] = '\0';

        return token;
    },
};

void apply_event_patches()
{
    // allow custom directional events
    level_read_events_patch.install();

    // HUD Message magic word handling
    hud_translate_special_character_token_hook.install();

    // fix some events not working if delay value is specified (alpine levels only)
    EventUnhide__process_patch.install();
    EventMakeInvulnerable__process_patch.install();

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
    debug_goal_cmd.register_cmd();
}
