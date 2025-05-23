#include <patch_common/CodeInjection.h>
#include <patch_common/FunHook.h>
#include <patch_common/CallHook.h>
#include <patch_common/AsmWriter.h>
#include <xlog/xlog.h>
#include "../misc/achievements.h"
#include "../misc/alpine_settings.h"
#include "../os/console.h"
#include "../rf/entity.h"
#include "../rf/event.h"
#include "../rf/corpse.h"
#include "../rf/multi.h"
#include "../rf/weapon.h"
#include "../rf/player/player.h"
#include "../rf/particle_emitter.h"
#include "../rf/os/frametime.h"
#include "../rf/os/os.h"
#include "../rf/sound/sound.h"

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

// avoids gibbing if gore level is too low or if this specific corpse shouldn't gib
/* CodeInjection corpse_damage_patch{
    0x00417C6A,
    [](auto& regs) {
        rf::Corpse* cp = regs.esi;

        // don't destroy corpses that should persist
        if (cp->corpse_flags & 0x400 ||         // drools_slime (used by snakes)
            cp->corpse_flags & 0x4)             // custom_state_anim (used by sea creature)
        {
            regs.eip = 0x00417C97;
        }
    }
};*/

// avoids playing pain sounds for gibbing entities (broken atm)
CodeInjection entity_damage_gib_no_pain_sound_patch {
    0x0041A51F,
    [](auto& regs) {        
        rf::Entity* ep = regs.esi;
        //float pain_sound_volume = regs.eax;
        if (ep->entity_flags & 0x80) {
            //pain_sound_volume = 0.0f;
            //xlog::warn("nosound");
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

        // should only gib on gore level 2 or higher
        if (rf::game_get_gore_level() < 2) {
            return;
        }

        // only gib flesh entities and corpses
        if (!objp || (objp->type != rf::OT_ENTITY && objp->type != rf::OT_CORPSE) || objp->material != 3) {
            return;
        }

        // skip entities with ambient flag (is in original but maybe not necessary?)
        rf::Entity* entity = (objp->type == rf::OT_ENTITY) ? static_cast<rf::Entity*>(objp) : nullptr;
        if (entity && (entity->info->flags & 0x800000)) {
            return;
        }

        // skip corpses that shouldn't explode (drools_slime or custom_state_anim)
        rf::Corpse* corpse = (objp->type == rf::OT_CORPSE) ? static_cast<rf::Corpse*>(objp) : nullptr;
        if (corpse && (corpse->corpse_flags & 0x400 || corpse->corpse_flags & 0x4)) {
            return;
        }

        static constexpr int gib_count = 14; // 7 in original
        static constexpr float velocity_scale = 15.0f;
        static constexpr float spin_scale_min = 10.0f;
        static constexpr float spin_scale_max = 25.0f;
        static constexpr int lifetime_ms = 7000;
        static constexpr float velocity_factor = 0.5f;
        static const char* snd_set = "gib bounce";
        static const std::vector<const char*> gib_filenames = {
            "meatchunk1.v3m",
            "meatchunk2.v3m",
            "meatchunk3.v3m",
            "meatchunk4.v3m",
            "meatchunk5.v3m"};

        for (int i = 0; i < gib_count; ++i) {
            rf::DebrisCreateStruct debris_info;

            // random velocity
            rf::Vector3 vel;
            vel.rand_quick();
            debris_info.vel = vel;
            debris_info.vel *= velocity_scale;
            debris_info.vel += objp->p_data.vel * velocity_factor;

            // random spin
            rf::Vector3 spin;
            spin.rand_quick();
            debris_info.spin = spin;
            std::uniform_real_distribution<float> range_dist(spin_scale_min, spin_scale_max);
            debris_info.spin *= range_dist(g_rng);

            // random orient
            rf::Matrix3 orient;
            orient.rand_quick();
            debris_info.orient = orient;

            // sound set
            rf::ImpactSoundSet* iss = rf::material_find_impact_sound_set(snd_set);
            debris_info.iss = iss;

            // other properties
            debris_info.pos = objp->pos;
            debris_info.lifetime_ms = lifetime_ms;
            debris_info.debris_flags = 0x4;
            debris_info.obj_flags = 0x8000; // start_hidden
            debris_info.material = objp->material;
            debris_info.room = objp->room;

            // pick a random gib filename
            std::uniform_int_distribution<size_t> dist(0, gib_filenames.size() - 1);
            const char* gib_filename = gib_filenames[dist(g_rng)];

            rf::Debris* gib = rf::debris_create(objp->handle, gib_filename, 0.3f, &debris_info, 0, -1.0f);
            if (gib) {
                gib->obj_flags |= rf::OF_INVULNERABLE;
            }
        }

        if (objp->type == rf::OT_ENTITY) {
            grant_achievement_sp(AchievementName::GibEnemy);
        }
    }
};

ConsoleCommand2 cl_gorelevel_cmd{
    "cl_gorelevel",
    [](std::optional<int> gore_setting) {
        if (gore_setting) {
            if (*gore_setting >= 0 && *gore_setting <= 2) {
                rf::game_set_gore_level(*gore_setting);
                rf::console::print("Set gore level to {}", rf::game_get_gore_level());
            }
            else {
                rf::console::print("Invalid gore level specified. Allowed range is 0 (minimal) to 2 (maximum).");
            }
        }
        else {
            rf::console::print("Gore level is {}", rf::game_get_gore_level());
        }
    },
    "Set gore level.",
    "cl_gorelevel [level]"
};

// makes some entities red - unfinished
CodeInjection player_create_entity_patch {
    0x004A4234,
    [](auto& regs) {        
        rf::Entity* ep = regs.ebx;
        xlog::warn("entity: {} skin", ep->name);
        regs.eip = 0x004A42CE;
    }
};

void apply_entity_sim_distance() {
    rf::entity_sim_distance = g_alpine_game_config.entity_sim_distance;
}

FunHook<void(rf::Entity*, float)> entity_maybe_play_pain_sound_hook{
    0x004196F0, [](rf::Entity* ep, float percent_damage) {
        if (g_alpine_game_config.entity_pain_sounds) {
            entity_maybe_play_pain_sound_hook.call_target(ep, percent_damage);
        }
    }
};

ConsoleCommand2 cl_painsounds_cmd{
    "cl_painsounds",
    []() {
        g_alpine_game_config.entity_pain_sounds = !g_alpine_game_config.entity_pain_sounds;
        rf::console::print("Entity pain sounds are {}", g_alpine_game_config.entity_pain_sounds ? "enabled" : "disabled");
    },
    "Toggle pain sounds",
};

void entity_do_patch()
{
    //player_create_entity_patch.install(); // force team skin experiment

    // Handle toggle for pain sounds
    entity_maybe_play_pain_sound_hook.install();

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
    //corpse_damage_patch.install();
    //entity_damage_gib_no_pain_sound_patch.install();

    // Commands
    cl_gorelevel_cmd.register_cmd();
    cl_painsounds_cmd.register_cmd();
}
