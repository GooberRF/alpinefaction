#include <patch_common/CodeInjection.h>
#include <patch_common/FunHook.h>
#include <patch_common/CallHook.h>
#include <patch_common/AsmWriter.h>
#include <xlog/xlog.h>
#include "../os/console.h"
#include "../rf/entity.h"
#include "../rf/event.h"
#include "../rf/corpse.h"
#include "../rf/multi.h"
#include "../rf/weapon.h"
#include "../rf/player/player.h"
#include "../rf/particle_emitter.h"
#include "../rf/os/frametime.h"
#include "../rf/sound/sound.h"
#include "../main/main.h"

rf::Timestamp g_player_jump_timestamp;

CodeInjection stuck_to_ground_when_jumping_fix{
    0x0042891E,
    [](auto& regs) {
        rf::Entity* entity = regs.esi;
        if (entity->local_player) {
            // Skip land handling code for next 64 ms (like in PF)
            g_player_jump_timestamp.set(64);
        }
    },
};

CodeInjection stuck_to_ground_when_using_jump_pad_fix{
    0x00486B60,
    [](auto& regs) {
        rf::Entity* entity = regs.esi;
        if (entity->local_player) {
            // Skip land handling code for next 64 ms
            g_player_jump_timestamp.set(64);
        }
    },
};

CodeInjection stuck_to_ground_fix{
    0x00487F82,
    [](auto& regs) {
        rf::Entity* entity = regs.esi;
        if (entity->local_player && g_player_jump_timestamp.valid() && !g_player_jump_timestamp.elapsed()) {
            // Jump to jump handling code that sets entity to falling movement mode
            regs.eip = 0x00487F7B;
        }
    },
};

CodeInjection entity_water_decelerate_fix{
    0x0049D82A,
    [](auto& regs) {
        rf::Entity* entity = regs.esi;
        float vel_factor = 1.0f - (rf::frametime * 4.5f);
        entity->p_data.vel.x *= vel_factor;
        entity->p_data.vel.y *= vel_factor;
        entity->p_data.vel.z *= vel_factor;
        regs.eip = 0x0049D835;
    },
};

FunHook<void(rf::Entity&, rf::Vector3&)> entity_on_land_hook{
    0x00419830,
    [](rf::Entity& entity, rf::Vector3& pos) {
        entity_on_land_hook.call_target(entity, pos);
        entity.p_data.vel.y = 0.0f;
    },
};

CallHook<void(rf::Entity&)> entity_make_run_after_climbing_patch{
    0x00430D5D,
    [](rf::Entity& entity) {
        entity_make_run_after_climbing_patch.call_target(entity);
        entity.p_data.vel.y = 0.0f;
    },
};

FunHook<void(rf::EntityFireInfo&, int)> entity_fire_switch_parent_to_corpse_hook{
    0x0042F510,
    [](rf::EntityFireInfo& fire_info, int corpse_handle) {
        rf::Corpse* corpse = rf::corpse_from_handle(corpse_handle);
        fire_info.parent_hobj = corpse_handle;
        rf::entity_fire_init_bones(&fire_info, corpse);
        for (auto& emitter_ptr : fire_info.emitters) {
            if (emitter_ptr) {
                emitter_ptr->parent_handle = corpse_handle;
            }
        }
        fire_info.time_limited = true;
        fire_info.time = 0.0f;
        corpse->corpse_flags |= 0x200;
    },
};

CallHook<bool(rf::Object*)> entity_update_liquid_status_obj_is_player_hook{
    {
        0x004292E3,
        0x0042932A,
        0x004293F4,
    },
    [](rf::Object* obj) {
        return obj == rf::local_player_entity;
    },
};

CallHook<bool(const rf::Vector3&, const rf::Vector3&, rf::PhysicsData*, rf::PCollisionOut*)> entity_maybe_stop_crouching_collide_spheres_world_hook{
    0x00428AB9,
    [](const rf::Vector3& p1, const rf::Vector3& p2, rf::PhysicsData* pd, rf::PCollisionOut* collision) {
        // Temporarly disable collisions with liquid faces
        auto collision_flags = pd->collision_flags;
        pd->collision_flags &= ~0x1000;
        bool result = entity_maybe_stop_crouching_collide_spheres_world_hook.call_target(p1, p2, pd, collision);
        pd->collision_flags = collision_flags;
        return result;
    },
};

CodeInjection entity_process_post_hidden_injection{
    0x0041E4C8,
    [](auto& regs) {
        rf::Entity* ep = regs.esi;
        // Make sure move sound is muted
        if (ep->move_sound_handle != -1) {
            rf::snd_change_3d(ep->move_sound_handle, ep->pos, rf::zero_vector, 0.0f);
        }
    },
};

CodeInjection entity_render_weapon_in_hands_silencer_visibility_injection{
    0x00421D39,
    [](auto& regs) {
        rf::Entity* ep = regs.esi;
        if (!rf::weapon_is_glock(ep->ai.current_primary_weapon)) {
            regs.eip = 0x00421D3F;
        }
    },
};

CodeInjection waypoints_read_lists_oob_fix{
    0x00468E54,
    [](auto& regs) {
        constexpr int max_waypoint_lists = 32;
        int& num_waypoint_lists = addr_as_ref<int>(0x0064E398);
        int index = regs.eax;
        int num_lists = regs.ecx;
        if (index >= max_waypoint_lists && index < num_lists) {
            xlog::error("Too many waypoint lists (limit is {})! Overwritting the last list.", max_waypoint_lists);
            // reduce count by one and keep index unchanged
            --num_waypoint_lists;
            regs.ecx = num_waypoint_lists;
            // skip EBP update to fix OOB write
            regs.eip = 0x00468E5B;
        }
    },
};

CodeInjection waypoints_read_nodes_oob_fix{
    0x00468DB1,
    [](auto& regs) {
        constexpr int max_waypoint_list_nodes = 128;
        int node_index = regs.eax + 1;
        int& num_nodes = *static_cast<int*>(regs.ebp);
        if (node_index >= max_waypoint_list_nodes && node_index < num_nodes) {
            xlog::error("Too many waypoint list nodes (limit is {})! Overwritting the last endpoint.", max_waypoint_list_nodes);
            // reduce count by one and keep node index unchanged
            --num_nodes;
            // Set EAX and ECX based on skipped instructions but do not update EBX to fix OOB write
            regs.eax = node_index - 1;
            regs.ecx = num_nodes;
            regs.eip = 0x00468DB8;
        }
    },
};

CodeInjection entity_fire_update_all_freeze_fix{
    0x0042EF31,
    [](auto& regs) {
        void* fire = regs.esi;
        void* next_fire = regs.ebp;
        if (fire == next_fire) {
            // only one object was on the list and it got deleted so exit the loop
            regs.eip = 0x0042F2AF;
        } else {
            // go to the next object
            regs.esi = next_fire;
        }
    },
};

CodeInjection entity_process_pre_hide_riot_shield_injection{
    0x0041DAFF,
    [](auto& regs) {
        rf::Entity* ep = regs.esi;
        int hidden = regs.eax;
        if (hidden) {
            auto shield = rf::obj_from_handle(ep->riot_shield_handle);
            if (shield) {
                rf::obj_hide(shield);
            }
        }
    },
};

CodeInjection entity_create_hook{
    0x004BC180,  // Address right after the call to entity_create
    [](BaseCodeInjection::Regs& regs) {
        // Cast the entity pointer using the workaround
        uintptr_t entity_addr = static_cast<uintptr_t>(regs.eax);
        rf::Object* created_entity = reinterpret_cast<rf::Object*>(entity_addr);

        // Log the pointer and additional details
        //xlog::warn("Entity created with pointer: 0x{:X}", entity_addr);

        if (created_entity) {
            xlog::warn("New entity! Pointer: {}, UID: {}, Position: x={}, y={}, z={}, life: {}, armor: {}, handle: {}", entity_addr,
                       created_entity->uid, created_entity->pos.x, created_entity->pos.y,
                       created_entity->pos.z, created_entity->life, created_entity->armor, created_entity->handle);
            //rf::Entity* testent = rf::local_player_entity;
            //xlog::warn("Entity UID: {}", testent->uid);
        }
    }
};

ConsoleCommand2 testlink_cmd{
    "dbg_make_link",
    [](std::optional<int> from, std::optional<int> to) {
        if (from && to) {
            xlog::warn("Attempting to create a link from UID {} to handle {}", from.value_or(-1), to.value_or(-1));

            rf::Event* from_event = rf::event_lookup_from_uid(from.value_or(-1));
            rf::Event* to_event = rf::event_lookup_from_uid(to.value_or(-1));

            rf::event_add_link(from_event->handle, to.value_or(-1));
            //int minutes = minutes_opt.value_or(5);
            //extend_round_time(minutes);
            //std::string msg = std::format("\xA6 Round extended by {} minutes", minutes);
            //rf::multi_chat_say(msg.c_str(), false);
        }
    },
    "make a link",
    "dbg_make_link",
};

// avoids gibbing if gore level is too low or if this specific corpse shouldn't gib
CodeInjection corpse_damage_patch{
    0x00417C6A,
    [](auto& regs) {
        rf::Corpse* cp = regs.esi;

        //xlog::warn("corpse {} {} {}", cp->uid, cp->corpse_pose_name, cp->corpse_flags);

        if (rf::game_get_gore_level() < 2 ||
            cp->corpse_flags & 0x400 ||         // drools_slime (used by snakes)
            cp->corpse_flags & 0x4)             // custom_state_anim (used by sea creature)
        {
            regs.eip = 0x00417C97;
        }
    }
};

// avoids playing pain sounds for gibbing entities
CodeInjection entity_damage_gib_no_pain_sound_patch {
    0x0041A51F,
    [](auto& regs) {        
        rf::Entity* ep = regs.esi;
        //float pain_sound_volume = regs.eax;
        if (ep->entity_flags & 0x80) {
            //pain_sound_volume = 0.0f;
            xlog::warn("nosound");
            regs.eip = 0x0041A550;
        }
    }
};

/* CodeInjection entity_damage_gib_no_pain_sound_patch{
    0x0041A548,
    [](auto& regs) {        
        rf::Entity* ep = regs.esi;
        float pain_sound_volume = regs.eax;
        if (ep->entity_flags & 0x80) {
            pain_sound_volume = 0.0f;
        }
    }
};*/

FunHook<void(int)> entity_blood_throw_gibs_hook{
    0x0042E3C0, [](int handle) {
        rf::Object* objp = rf::obj_from_handle(handle);

        if (!objp) {
            return; // invalid object
        }

        int explode_vclip_index = rf::vclip_lookup("bloodsplat");
        int chunk_explode_vclip_index = rf::vclip_lookup("bloodsplat");
        float explode_vclip_radius = 1.0f;
        const char* debris_filename = "df_meatchunks0.V3D";

        static const int debris_max_lifetime = 7000; // ms
        static const float debris_velocity = 8.5f;
        static const float damage_scale = 1.0f;
        static rf::String cust_snd_set = "gib bounce";

        if (objp->type == rf::OT_ENTITY) { // use overrides from associated entity.tbl class if present
            rf::Entity* ep = static_cast<rf::Entity*>(objp);

            explode_vclip_index = (ep->info->explode_vclip_index > 0)
				? ep->info->explode_vclip_index : explode_vclip_index;

            explode_vclip_radius = (ep->info->explode_vclip_radius > 0.0f)
				? ep->info->explode_vclip_radius : explode_vclip_radius;

            debris_filename = (!ep->info->debris_filename.empty())
                ? ep->info->debris_filename.c_str() : debris_filename;
        }
        else if (objp->type != rf::OT_CORPSE) { // do not gib anything except entities and corpses
            return;
        }

        rf::game_do_explosion(
            explode_vclip_index, objp->room, 0, &objp->pos, explode_vclip_radius, damage_scale, 0);

        rf::debris_spawn_from_object(
            objp, debris_filename, chunk_explode_vclip_index, debris_max_lifetime, debris_velocity, &cust_snd_set);
    }
};

ConsoleCommand2 cl_gorelevel_cmd{
    "cl_gorelevel",
    [](std::optional<int> gore_setting) {
        if (gore_setting.has_value()) {
            rf::game_set_gore_level(gore_setting.value_or(1));
            rf::console::print("Set gore level to {}", rf::game_get_gore_level());
        }
        else {
            rf::console::print("Gore level is {}", rf::game_get_gore_level());
        }
        
    },
    "Set gore level from 0 (minimal gore) to 2 (maximum gore).",
    "cl_gorelevel [level]"
};

// no idea
CallHook<void(rf::Entity*, float)> physics_calc_fall_damage_hook{
    0x0049D4B6,
    [](rf::Entity* entity, float rel_vel) {
        // Custom behavior: adjust the relative velocity if needed
        float adjusted_rel_vel = rel_vel;

        xlog::warn("A rel_vel is {}", rel_vel);

        // Call the original function with potentially modified parameters
        physics_calc_fall_damage_hook.call_target(entity, adjusted_rel_vel);
    }
};

// no idea
CallHook<void(rf::Entity*, float)> physics_calc_fall_damage_hookB{
    0x0049DE23,
    [](rf::Entity* entity, float rel_vel) {
        // Custom behavior: adjust the relative velocity if needed
        float adjusted_rel_vel = rel_vel;

        xlog::warn("Brel_vel is {}", rel_vel);

        // Call the original function with potentially modified parameters
        physics_calc_fall_damage_hookB.call_target(entity, adjusted_rel_vel);
    }
};

// fall damage when impacting
CallHook<void(rf::Entity*, float)> physics_calc_fall_damage_hookC{
    0x0049DE39,
    [](rf::Entity* entity, float rel_vel) {
        // Custom behavior: adjust the relative velocity if needed
        
        xlog::warn("Crel_vel is {}", rel_vel);

        float adjusted_rel_vel = rel_vel * 0; // disable

        // Call the original function with potentially modified parameters
        physics_calc_fall_damage_hookC.call_target(entity, adjusted_rel_vel);
    }
};

// fall damage when landing
CallHook<void(rf::Entity*, float)> physics_calc_fall_damage_hookD{
    0x004A0C28,
    [](rf::Entity* entity, float rel_vel) {
        // Custom behavior: adjust the relative velocity if needed
        float adjusted_rel_vel = rel_vel;

        xlog::warn("Drel_vel is {}", rel_vel);

        // Call the original function with potentially modified parameters
        physics_calc_fall_damage_hookD.call_target(entity, adjusted_rel_vel);
    }
};


void entity_do_patch()
{
    testlink_cmd.register_cmd();
    entity_create_hook.install();
    //physics_calc_fall_damage_hook.install();
    //physics_calc_fall_damage_hookB.install();
    //physics_calc_fall_damage_hookC.install();
    //physics_calc_fall_damage_hookD.install();

    // Fix player being stuck to ground when jumping, especially when FPS is greater than 200
    stuck_to_ground_when_jumping_fix.install();
    stuck_to_ground_when_using_jump_pad_fix.install();
    stuck_to_ground_fix.install();

    // Fix water deceleration on high FPS
    AsmWriter(0x0049D816).nop(5);
    entity_water_decelerate_fix.install();

    // Fix flee AI mode on high FPS by avoiding clearing velocity in Y axis in EntityMakeRun
    AsmWriter(0x00428121, 0x0042812B).nop();
    AsmWriter(0x0042809F, 0x004280A9).nop();
    entity_on_land_hook.install();
    entity_make_run_after_climbing_patch.install();

    // Increase entity simulation max distance
    // TODO: create a config property for this
    if (g_game_config.disable_lod_models) {
        write_mem<float>(0x00589548, 100.0f);
    }

    // Fix crash when particle emitter allocation fails during entity ignition
    entity_fire_switch_parent_to_corpse_hook.install();

    // Fix buzzing sound when some player is floating in water
    entity_update_liquid_status_obj_is_player_hook.install();

    // Fix entity staying in crouched state after entering liquid
    entity_maybe_stop_crouching_collide_spheres_world_hook.install();

    // Use local_player variable for weapon shell distance calculation instead of local_player_entity
    // in entity_eject_shell. Fixed debris pool being exhausted when local player is dead.
    AsmWriter(0x0042A223, 0x0042A232).mov(asm_regs::ecx, {&rf::local_player});

    // Fix move sound not being muted if entity is created hidden (example: jeep in L18S3)
    entity_process_post_hidden_injection.install();

    // Do not show glock with silencer in 3rd person view if current primary weapon is not a glock
    entity_render_weapon_in_hands_silencer_visibility_injection.install();

    // Fix OOB writes in waypoint list read code
    waypoints_read_lists_oob_fix.install();
    waypoints_read_nodes_oob_fix.install();

    // Fix possible freeze when burning entity is destroyed
    entity_fire_update_all_freeze_fix.install();

    // Hide riot shield third person model if entity is hidden (e.g. in cutscenes)
    entity_process_pre_hide_riot_shield_injection.install();

	// Restore cut stock game feature for entities and corpses exploding into chunks
	entity_blood_throw_gibs_hook.install();
    corpse_damage_patch.install();
    entity_damage_gib_no_pain_sound_patch.install();

    // Commands
    cl_gorelevel_cmd.register_cmd();
}
