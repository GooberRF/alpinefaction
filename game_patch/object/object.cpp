#include <patch_common/FunHook.h>
#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/AsmWriter.h>
#include <patch_common/StaticBufferResizePatch.h>
#include <common/utils/string-utils.h>
#include <xlog/xlog.h>
#include "../rf/gr/gr_light.h"
#include "../rf/sound/sound.h"
#include "../rf/object.h"
#include "../rf/clutter.h"
#include "../rf/multi.h"
#include "../rf/event.h"
#include "../rf/level.h"
#include "../rf/particle_emitter.h"
#include "../rf/geometry.h"
#include "../rf/math/ix.h"
#include "../rf/gameseq.h"
#include "../misc/alpine_options.h"
#include "../misc/misc.h"
#include "../misc/achievements.h"
#include "event_alpine.h"
#include "object.h"
#include "object_private.h"

FunHook<rf::Object*(int, int, int, rf::ObjectCreateInfo*, int, rf::GRoom*)> obj_create_hook{
    0x00486DA0,
    [](int type, int sub_type, int parent, rf::ObjectCreateInfo* create_info, int flags, rf::GRoom* room) {
        rf::Object* objp = obj_create_hook.call_target(type, sub_type, parent, create_info, flags, room);
        //xlog::warn("Object create request: type={}, sub_type={}, owner_objh={}, flags={}", type, sub_type, parent, flags);

        if (!objp) {
            xlog::info("Failed to create object (type {})", type);
        }
        return objp;
    },
};

StaticBufferResizePatch<rf::Object*> obj_ptr_array_resize_patch{
    0x007394CC,
    old_obj_limit,
    obj_limit,
    {
         {0x0040A0F7},
         {0x004867B5},
         {0x00486854},
         {0x00486D51},
         {0x00486E38},
         {0x00487572},
         {0x0048759E},
         {0x004875B4},
         {0x0048A78A},
         {0x00486D70, true},
         {0x0048A7A1, true},
    },
};

StaticBufferResizePatch<int> obj_multi_handle_mapping_resize_patch{
    0x006FB428,
    old_obj_limit,
    obj_limit,
    {
        {0x0047D8C9},
        {0x00484B11},
        {0x00484B41},
        {0x00484B82},
        {0x00484BAE},
    },
};

static struct {
    int count;
    rf::Object* objects[obj_limit];
} g_sim_obj_array;

void sim_obj_array_add_hook(rf::Object* obj)
{
    g_sim_obj_array.objects[g_sim_obj_array.count++] = obj;
}

CodeInjection obj_create_find_slot_patch{
    0x00486DFC,
    [](auto& regs) {
        rf::ObjectType obj_type = regs.ebx;

        static int low_index_hint = 0;
        static int high_index_hint = old_obj_limit;
        rf::Object** objects = obj_ptr_array_resize_patch.get_buffer();

        int index_hint, min_index, max_index;
        bool use_low_index;
        if (rf::is_server && (obj_type == rf::OT_ENTITY || obj_type == rf::OT_ITEM)) {
            // Use low object numbers server-side for entities and items for better client compatibility
            use_low_index = true;
            index_hint = low_index_hint;
            min_index = 0;
            max_index = old_obj_limit - 1;
        }
        else {
            use_low_index = false;
            index_hint = high_index_hint;
            min_index = rf::is_server ? old_obj_limit : 0;
            max_index = obj_limit - 1;
        }

        int index = index_hint;
        while (objects[index]) {
            ++index;
            if (index > max_index) {
                index = min_index;
            }

            if (index == index_hint) {
                // failure: no free slots
                regs.eip = 0x00486E08;
                return;
            }
        }

        // success: current index is free
        xlog::trace("Using index {} for object type {}", index, obj_type);
        index_hint = index + 1;
        if (index_hint > max_index) {
            index_hint = min_index;
        }

        if (use_low_index) {
            low_index_hint = index_hint;
        }
        else {
            high_index_hint = index_hint;
        }

        regs.edi = index;
        regs.eip = 0x00486E15;
    },
};

CallHook<void*(size_t)> GPool_allocate_new_hook{
    {
        0x0048B5C6,
        0x0048B736,
        0x0048B8A6,
        0x0048BA16,
        0x004D7EF6,
        0x004E3C63,
        0x004E3DF3,
        0x004F97E3,
        0x004F9C63,
        0x005047B3,
    },
    [](size_t s) -> void* {
        void* result = GPool_allocate_new_hook.call_target(s);
        if (result) {
            // Zero memory allocated dynamically (static memory is zeroed by operating system automatically)
            std::memset(result, 0, s);
        }
        return result;
    },
};

CodeInjection sort_clutter_patch{
    0x004109D4,
    [](auto& regs) {
        rf::Clutter* clutter = regs.esi;
        rf::VMesh* vmesh = clutter->vmesh;
        const char* mesh_name = vmesh ? rf::vmesh_get_name(vmesh) : nullptr;
        if (!mesh_name) {
            // Sometimes on level change some objects can stay and have only vmesh destroyed
            return;
        }
        std::string_view mesh_name_sv = mesh_name;

        rf::Clutter* current = rf::clutter_list.next;
        while (current != &rf::clutter_list) {
            rf::VMesh* current_anim_mesh = current->vmesh;
            const char* current_mesh_name = current_anim_mesh ? rf::vmesh_get_name(current_anim_mesh) : nullptr;
            if (current_mesh_name && mesh_name_sv == current_mesh_name) {
                break;
            }
            if (current_mesh_name && std::string_view{current_mesh_name} == "LavaTester01.v3d") {
                // HACKFIX: place LavaTester01 at the end to fix alpha draw order issues in L5S2 (Geothermal Plant)
                // Note: OF_HAS_ALPHA cannot be used because it causes another draw-order issue when lava goes up
                break;
            }
            current = current->next;
        }
        // insert before current
        clutter->next = current;
        clutter->prev = current->prev;
        clutter->next->prev = clutter;
        clutter->prev->next = clutter;
        // Set up needed registers
        regs.eax = addr_as_ref<int>(regs.esp + 0xD0 + 0x18); // killable
        regs.ecx = addr_as_ref<int>(0x005C9358) + 1; // num_clutter_objs
        regs.eip = 0x00410A03;
    },
};

FunHook<rf::VMesh*(rf::Object*, const char*, rf::VMeshType)> obj_create_mesh_hook{
    0x00489FE0,
    [](rf::Object* objp, const char* name, rf::VMeshType type) {

        // handle per-map mesh replacements
        auto level_it = g_alpine_level_info_config.mesh_replacements.find(rf::level.filename);
        if (level_it != g_alpine_level_info_config.mesh_replacements.end()) {
            const auto& mesh_map = level_it->second;

            // convert original mesh name to lowercase
            std::string lower_name = string_to_lower(name);

            auto mesh_it = mesh_map.find(lower_name);
            if (mesh_it != mesh_map.end()) {
                name = mesh_it->second.c_str(); // Use replacement name
                xlog::debug("Replacing mesh {} with {}", name, mesh_it->second);
            }
        }

        rf::VMesh* mesh = obj_create_mesh_hook.call_target(objp, name, type);
        if (mesh && (rf::level.flags & rf::LEVEL_LOADED) != 0) {
            obj_mesh_lighting_maybe_update(objp);
        }
        
        return mesh;
    },
};

FunHook<void(rf::Object*)> obj_delete_mesh_hook{
    0x00489FC0,
    [](rf::Object* objp) {
        obj_delete_mesh_hook.call_target(objp);
        obj_mesh_lighting_free_one(objp);
    },
};

CodeInjection object_find_room_optimization{
    0x0048A1C9,
    [](auto& regs) {
        rf::Object* obj = regs.esi;
        // Check if object is in room bounding box to handle leaving room by a hole
        if (obj->room && rf::ix_point_in_box(obj->pos, obj->room->bbox_min, obj->room->bbox_max)) {
            // Pass original room to GSolid::find_new_room so it can execute a faster code path
            addr_as_ref<rf::GRoom*>(regs.esp) = obj->room; // orig_room
            addr_as_ref<rf::Vector3*>(regs.esp + 4) = &obj->correct_pos; // orig_pos
        }
    },
};

CodeInjection mover_process_post_patch{
    0x0046A98C,
    [](auto& regs) {
        rf::Object* object = regs.ecx;

        if (object && object->type == rf::OT_EVENT) {
            rf::Event* event = static_cast<rf::Event*>(object);

            if (event->event_type == rf::event_type_to_int(rf::EventType::Anchor_Marker)) {
                for (const auto& linked_uid : event->links) {
                    
                    // check for an object - Note objects store handles in link int rather than UID
                    if (auto* obj =
                            static_cast<rf::Object*>(rf::obj_from_handle(linked_uid))) {
                        obj->pos = event->pos;
                    }

                    // check for a light
                    if (auto* light = static_cast<rf::gr::Light*>(
                            rf::gr::light_get_from_handle(rf::gr::level_get_light_handle_from_uid(linked_uid)))) {
                        light->vec = event->pos;
                    }

                    // check for a particle emitter
                    if (auto* emitter =
                            static_cast<rf::ParticleEmitter*>(rf::level_get_particle_emitter_from_uid(linked_uid))) {
                        emitter->pos = event->pos;
                    }

                    // check for a push region
                    if (auto* push_region =
                            static_cast<rf::PushRegion*>(rf::level_get_push_region_from_uid(linked_uid))) {
                        push_region->pos = event->pos;
                    }
                }
            }

            if (event->event_type == rf::event_type_to_int(rf::EventType::Anchor_Marker_Orient)) {
                for (const auto& linked_uid : event->links) {
                    
                    // check for an object - Note objects store handles in link int rather than UID
                    if (auto* obj =
                            static_cast<rf::Object*>(rf::obj_from_handle(linked_uid))) {
                        rf::Vector3 new_obj_pos = event->pos;
                        obj->pos = new_obj_pos;
                        obj->p_data.pos = new_obj_pos;
                        obj->p_data.next_pos = new_obj_pos;

                        rf::Matrix3 new_obj_dir = event->orient;
                        obj->orient = new_obj_dir;
                        obj->p_data.next_orient = new_obj_dir;
                        obj->p_data.orient = new_obj_dir;
                    }

                    // check for a light
                    if (auto* light = static_cast<rf::gr::Light*>(
                            rf::gr::light_get_from_handle(rf::gr::level_get_light_handle_from_uid(linked_uid)))) {
                        light->vec = event->pos;
                    }

                    // check for a particle emitter
                    if (auto* emitter =
                            static_cast<rf::ParticleEmitter*>(rf::level_get_particle_emitter_from_uid(linked_uid))) {
                        emitter->pos = event->pos;

                        emitter->dir = event->orient.fvec;
                    }

                    // check for a push region
                    if (auto* push_region =
                            static_cast<rf::PushRegion*>(rf::level_get_push_region_from_uid(linked_uid))) {
                        push_region->pos = event->pos;

                        push_region->orient = event->orient;
                    }
                }
            }
        }
    }
};

FunHook<void(rf::Entity*)> entity_on_dead_hook{
    0x00418F80,
    [](rf::Entity* ep) {
        //xlog::warn("killing entity UID {}, name {}", ep->uid, ep->name);
        if (!rf::is_multi) {
            if (is_achievement_system_initialized()) {
                achievement_check_entity_death(ep);
            }

            rf::activate_all_events_of_type(rf::EventType::AF_When_Dead, ep->handle, -1, true);
        }

        entity_on_dead_hook.call_target(ep);
    },
};

CallHook<void(rf::Object*)> obj_flag_dead_clutter_hook{
    {
        0x0040FE31,
        0x00410208,
        0x0041009F,
        0x004101F4
    },
    [](rf::Object* objp) {
        //xlog::warn("killing clutter UID {}, name {}", objp->uid, objp->name);

        rf::Clutter* cp = reinterpret_cast<rf::Clutter*>(objp);

        if (!rf::is_multi && is_achievement_system_initialized() && cp) {
            achievement_check_clutter_death(cp);
        }

        rf::activate_all_events_of_type(rf::EventType::AF_When_Dead, cp->handle, -1, true);

        obj_flag_dead_clutter_hook.call_target(objp);
    },
};

FunHook<void(rf::Clutter*, float, int, int, rf::PCollisionOut*)> clutter_damage_hook{
    0x00410270,
    [](rf::Clutter* damaged_cp, float damage, int responsible_entity_handle, int damage_type, rf::PCollisionOut* collide_out) {

        if (rf::is_multi && damaged_cp->name == "riot_shield") {
            xlog::warn("damaged {}, info index {}", damaged_cp->name, damaged_cp->info_index);
            return;
        }

        clutter_damage_hook.call_target(damaged_cp, damage, responsible_entity_handle, damage_type, collide_out);
    },
};

void object_do_patch()
{
    // Disable damage for the riot shield in multiplayer (needs fix)
    //clutter_damage_hook.install(); // disabled for now, makes riot shields persist after player leaves

    // Support AF_When_Dead events
    entity_on_dead_hook.install();
    obj_flag_dead_clutter_hook.install();

    // Allow Anchor_Marker events to drag lights, particle emitters, and push regions on movers
    mover_process_post_patch.install();

    // Log error when object cannot be created
    obj_create_hook.install();

    // Change object limit
    //obj_free_slot_buffer_resize_patch.install();
    obj_ptr_array_resize_patch.install();
    obj_multi_handle_mapping_resize_patch.install();
    write_mem<u32>(0x0040A0F0 + 1, obj_limit);
    write_mem<u32>(0x0047D8C1 + 1, obj_limit);

    // Change object index allocation strategy
    obj_create_find_slot_patch.install();
    AsmWriter(0x00486E3F, 0x00486E61).nop();
    AsmWriter(0x0048685F, 0x0048687B).nop();
    AsmWriter(0x0048687C, 0x00486895).nop();

    // Remap simulated objects array
    AsmWriter(0x00487A6B, 0x00487A74).mov(asm_regs::ecx, &g_sim_obj_array);
    AsmWriter(0x00487C02, 0x00487C0B).mov(asm_regs::ecx, &g_sim_obj_array);
    AsmWriter(0x00487AD2, 0x00487ADB).call(sim_obj_array_add_hook).add(asm_regs::esp, 4);
    AsmWriter(0x00487BBA, 0x00487BC3).call(sim_obj_array_add_hook).add(asm_regs::esp, 4);

    // Allow pool allocation beyond the limit
    write_mem<u8>(0x0048B5BB, asm_opcodes::jmp_rel_short); // weapon
    write_mem<u8>(0x0048B72B, asm_opcodes::jmp_rel_short); // debris
    write_mem<u8>(0x0048B89B, asm_opcodes::jmp_rel_short); // corpse
    write_mem<u8>(0x004D7EEB, asm_opcodes::jmp_rel_short); // decal poly
    write_mem<u8>(0x004E3C5B, asm_opcodes::jmp_rel_short); // face
    write_mem<u8>(0x004E3DEB, asm_opcodes::jmp_rel_short); // face vertex
    write_mem<u8>(0x004F97DB, asm_opcodes::jmp_rel_short); // bbox
    write_mem<u8>(0x004F9C5B, asm_opcodes::jmp_rel_short); // vertex
    write_mem<u8>(0x005047AB, asm_opcodes::jmp_rel_short); // vmesh

    // Remove object type-specific limits
    AsmWriter(0x0048712A, 0x00487137).nop(); // corpse
    AsmWriter(0x00487173, 0x00487180).nop(); // debris
    AsmWriter(0x004871D9, 0x004871E9).nop(); // item
    AsmWriter(0x00487271, 0x0048727A).nop(); // weapon

    // Zero memory allocated from GPool dynamically
    GPool_allocate_new_hook.install();

    // Sort objects by mesh name to improve rendering performance
    sort_clutter_patch.install();

    // Calculate lighting when object mesh is changed
    obj_create_mesh_hook.install();
    obj_delete_mesh_hook.install();

    // Optimize Object::find_room function
    object_find_room_optimization.install();

    // Allow creating entity objects out of level bounds
    // Fixes loading a save game when player entity is out of bounds
    AsmWriter{0x00486DE5, 0x00486DEE}.nop();

    // Other files
    entity_do_patch();
    item_do_patch();
    cutscene_apply_patches();
    apply_event_patches();
    apply_alpine_events(); // Support custom alpine events
    glare_patches_patches();
    apply_weapon_patches();
    trigger_apply_patches();
    monitor_do_patch();
    particle_do_patch();
    obj_light_apply_patch();
}
