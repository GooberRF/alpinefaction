#include <patch_common/FunHook.h>
#include <patch_common/AsmWriter.h>
#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/ShortTypes.h>
#include <array>
#include <xlog/xlog.h>
#include "../multi/server.h"
#include "../rf/player/player.h"
#include "../rf/weapon.h"
#include "../rf/entity.h"
#include "../rf/multi.h"
#include "../rf/level.h"
#include "../os/console.h"
#include "../main/main.h"
#include "../multi/multi.h"
#include "../misc/misc.h"
#include "../misc/alpine_settings.h"

static std::array<uint8_t, 64U> weapon_reticle_custom_mask{}; // bit 0 = _0, bit 1 = _1
static std::pair<bool, bool> rocket_locked_custom_reticle = {false, false};

bool entity_is_reloading_player_select_weapon_new(rf::Entity* entity)
{
    if (rf::entity_is_reloading(entity))
        return true;

    int weapon_type = entity->ai.current_primary_weapon;
    if (weapon_type >= 0) {
        rf::WeaponInfo& wi = rf::weapon_types[weapon_type];
        if (entity->ai.clip_ammo[weapon_type] == 0 && entity->ai.ammo[wi.ammo_type] > 0)
            return true;
    }
    return false;
}

CodeInjection weapons_tbl_buffer_overflow_fix_1{
    0x004C6855,
    [](auto& regs) {
        if (rf::num_weapon_types == 64) {
            xlog::warn("weapons.tbl limit of 64 definitions has been reached!");
            regs.eip = 0x004C6881;
        }
    },
};

CodeInjection weapons_tbl_buffer_overflow_fix_2{
    0x004C68AD,
    [](auto& regs) {
        if (rf::num_weapon_types == 64) {
            xlog::warn("weapons.tbl limit of 64 definitions has been reached!");
            regs.eip = 0x004C68D9;
        }
    },
};

CodeInjection weapon_init_track_reticle_bitmap_injection{
    0x004C6756,
    [](auto& regs) {
        weapon_reticle_custom_mask.fill(0);
        rocket_locked_custom_reticle = {false, false};

        for (int i = 0; i < 64; ++i) {
            const int bm_handle = rf::weapon_types[i].hud_reticle_bitmap;
            if (bm_handle > 0) {
                const char* bm_filename = rf::bm::get_filename(bm_handle);
                if (!bm_filename)
                    continue;

                auto bm_filename_base = string_remove_any_suffix_before_extension(bm_filename, {"_0", "_1"});
                auto bm_filename_0 = string_add_suffix_before_extension(bm_filename_base, "_0");
                auto bm_filename_1 = string_add_suffix_before_extension(bm_filename_base, "_1");
                bool customized_0 = !file_loaded_from_alpinefaction_vpp(bm_filename_0.c_str());
                bool customized_1 = !file_loaded_from_alpinefaction_vpp(bm_filename_1.c_str());

                //xlog::warn("weap {} ({}), bmh {}, bmf {}, custom? 0:{} 1:{}", i, rf::weapon_types[i].name, bm_handle, bm_filename_base, customized_0, customized_1);

                weapon_reticle_custom_mask[i] = (customized_0 ? 0x1 : 0) | (customized_1 ? 0x2 : 0);
            }

            // special case for rocket lock on reticle
            // if there were more than one of these, we'd have another array, but that would be a waste here
            if (i == rf::rocket_launcher_weapon_type) {
                const int bm_locked_handle = rf::weapon_types[i].hud_locked_reticle_bitmap;
                if (bm_locked_handle > 0) {
                    const char* bm_filename = rf::bm::get_filename(bm_locked_handle);
                    if (!bm_filename)
                        continue;

                    auto bm_filename_base = string_remove_any_suffix_before_extension(bm_filename, {"_0", "_1"});
                    auto bm_filename_0 = string_add_suffix_before_extension(bm_filename_base, "_0");
                    auto bm_filename_1 = string_add_suffix_before_extension(bm_filename_base, "_1");
                    bool customized_0 = !file_loaded_from_alpinefaction_vpp(bm_filename_0.c_str());
                    bool customized_1 = !file_loaded_from_alpinefaction_vpp(bm_filename_1.c_str());

                    //xlog::warn("LOCKED{}, bmh {}, bmf {}, custom? 0:{} 1:{}", i, bm_locked_handle, bm_filename_base, customized_0, customized_1);

                    rocket_locked_custom_reticle = {customized_0, customized_1};
                }
            }
        }
    },
};

bool weapon_reticle_is_customized(int weapon_id, bool bighud) {
    if (static_cast<unsigned>(weapon_id) >= weapon_reticle_custom_mask.size())
        return false;

    const uint8_t mask = weapon_reticle_custom_mask[weapon_id];
    return (mask & (bighud ? 0x2 : 0x1)) != 0;
}

bool rocket_locked_reticle_is_customized(bool bighud) {
    return bighud ? rocket_locked_custom_reticle.second : rocket_locked_custom_reticle.first;
}

FunHook<void(rf::Weapon*)> weapon_move_one_hook{
    0x004C69A0,
    [](rf::Weapon* weapon) {
        weapon_move_one_hook.call_target(weapon);
        auto& level_aabb_min = rf::level.geometry->bbox_min;
        auto& level_aabb_max = rf::level.geometry->bbox_max;
        float margin = weapon->vmesh ? 275.0f : 10.0f;
        bool has_gravity_flag = weapon->p_data.flags & 1;
        bool check_y_axis = !(has_gravity_flag || weapon->info->thrust_lifetime_seconds > 0.0f);
        auto& pos = weapon->pos;
        if (pos.x < level_aabb_min.x - margin || pos.x > level_aabb_max.x + margin
        || pos.z < level_aabb_min.z - margin || pos.z > level_aabb_max.z + margin
        || (check_y_axis && (pos.y < level_aabb_min.y - margin || pos.y > level_aabb_max.y + margin))) {
            // Weapon is outside the level - delete it
            rf::obj_flag_dead(weapon);
        }
    },
};

CodeInjection weapon_vs_obj_collision_fix{
    0x0048C803,
    [](auto& regs) {
        rf::Object* obj = regs.edi;
        rf::Object* weapon = regs.ebp;
        auto dir = obj->pos - weapon->pos;
        // Take into account weapon and object radius
        float rad = weapon->radius + obj->radius;
        if (dir.dot_prod(weapon->orient.fvec) < -rad) {
            // Ignore this pair
            regs.eip = 0x0048C82A;
        }
        else {
            // Continue processing this pair
            regs.eip = 0x0048C834;
        }
    },
};

CodeInjection muzzle_flash_light_not_disabled_fix{
    0x0041E806,
    [](auto& regs) {
        rf::Timestamp* primary_muzzle_timestamp = regs.ecx;
        if (!primary_muzzle_timestamp->valid()) {
            regs.eip = 0x0041E969;
        }
    },
};

CallHook<void(rf::Player*, int)> process_create_entity_packet_switch_weapon_fix{
    0x004756B7,
    [](rf::Player* player, int weapon_type) {
        process_create_entity_packet_switch_weapon_fix.call_target(player, weapon_type);
        // Check if local player is being spawned
        if (!rf::is_server && player == rf::local_player) {
            // Update requested weapon to make sure server does not auto-switch the weapon during item pickup
            rf::multi_set_next_weapon(weapon_type);
        }
    },
};

void apply_show_enemy_bullets() {
    rf::hide_enemy_bullets = !g_alpine_game_config.show_enemy_bullets;
}

ConsoleCommand2 show_enemy_bullets_cmd{
    "cl_showenemybullets",
    []() {
        g_alpine_game_config.show_enemy_bullets = !g_alpine_game_config.show_enemy_bullets;
        apply_show_enemy_bullets();
        rf::console::print("Enemy bullet impact effects are {}", g_alpine_game_config.show_enemy_bullets ? "enabled" : "disabled");
    },
    "Toggles visibility of enemy bullet impacts",
};

CallHook<void(rf::Vector3&, float, float, int, int)> weapon_hit_wall_obj_apply_radius_damage_hook{
    0x004C53A8,
    [](rf::Vector3& epicenter, float damage, float radius, int killer_handle, int damage_type) {
        auto& collide_out = *reinterpret_cast<rf::PCollisionOut*>(&epicenter);
        auto new_epicenter = epicenter + collide_out.hit_normal * 0.0001f;
        weapon_hit_wall_obj_apply_radius_damage_hook.call_target(new_epicenter, damage, radius, killer_handle, damage_type);
    },
};

ConsoleCommand2 multi_ricochet_cmd{
    "mp_ricochet",
    []() {
        g_alpine_game_config.multi_ricochet = !g_alpine_game_config.multi_ricochet;
        rf::console::print("Multiplayer ricochets are {}", g_alpine_game_config.multi_ricochet ? "enabled" : "disabled");
    },
    "Toggles whether bullets ricochet in multiplayer (strictly visual, they deal no damage regardless)",
};

FunHook<bool(rf::Weapon*)> weapon_possibly_richochet {
    0x004C9D30,
    [](rf::Weapon* weapon) {
        if (rf::is_multi && !g_alpine_game_config.multi_ricochet) {
            return false;
        }

        return weapon_possibly_richochet.call_target(weapon);
    },
};

ConsoleCommand2 gaussian_spread_cmd{
    "sp_spreadmode",
    []() {
        g_alpine_game_config.gaussian_spread = !g_alpine_game_config.gaussian_spread;
        rf::console::print("Random bullet spread calculation is using the {} method",
            g_alpine_game_config.gaussian_spread ? "new (gaussian)" : "legacy (uniform)");
    },
    "Toggles whether bullet spread randomness uses the new gaussian method or the legacy uniform method",
};

bool should_use_gaussian_spread()
{
    if (!rf::is_multi && g_alpine_game_config.gaussian_spread) {
        return true;
    }
    else if ((rf::is_dedicated_server || rf::is_server) && g_alpine_server_config.gaussian_spread) {
        return true;
    }
    else if (rf::is_multi && get_af_server_info().has_value() && get_af_server_info()->gaussian_spread) {
        return true;
    }

    return false;
}

ConsoleCommand2 unlimited_semi_auto_cmd{
    "sp_unlimitedsemiauto",
    []() {
        g_alpine_game_config.unlimited_semi_auto = !g_alpine_game_config.unlimited_semi_auto;
        rf::console::print("Fire rate cooldown for semi-automatic weapons in single player is {}",
            g_alpine_game_config.unlimited_semi_auto ? "disabled" : "enabled");
    },
    "Toggles whether the fire rate for semi-automatic weapons in single player has a cooldown",
};

bool should_apply_click_limiter() {
    if (!rf::is_multi && !g_alpine_game_config.unlimited_semi_auto) {
        return true;
    }

    if (rf::is_multi && get_af_server_info().has_value() && get_af_server_info()->click_limit) {
        return true;
    }

    return false;
}

// hack approach - fire wait override for player-controlled semi auto weapons that have
// default fire wait 500ms like pistol and PR in stock weapons.tbl
int get_semi_auto_fire_wait_override() {
    if (rf::is_multi && get_af_server_info().has_value()) {
        return get_af_server_info()->semi_auto_cooldown.value_or(90);
    }
    else {
        return 90;
    }
}

CodeInjection fire_primary_weapon_semi_auto_patch {
    0x004A50BB,
    [](auto& regs) {
        rf::Entity* entity = regs.esi;
        if (should_apply_click_limiter() && !entity->ai.next_fire_primary.elapsed()) {
            regs.eip = 0x004A58B8;
        }
    },
};

CodeInjection entity_fire_primary_weapon_semi_auto_patch {
    0x004259B8,
    [](auto& regs) {
        // override fire wait for stock semi auto weapons (hack, to avoid needing to modify weapons.tbl)
        // Note this is relevant both to first shot accuracy and semi auto click limit
        int fire_wait = regs.eax;
        int weapon_type = regs.ebx;
        rf::Entity* entity = regs.esi;
        if (rf::obj_is_player(entity) && rf::weapon_is_semi_automatic(weapon_type) && fire_wait == 500) {
            regs.eax = get_semi_auto_fire_wait_override();
        }

        // apply first shot accuracy if 2x the weapon's fire wait has elapsed since the last shot
        // Note this also disables the difficulty-based rapid fire spread increase for the pistol in SP
        if (should_use_gaussian_spread()) {
            int fire_wait2 = regs.eax;
            if (rf::obj_is_player(entity)){
                if (!rf::weapon_is_shotgun(weapon_type) && entity->last_fired_timestamp.time_since() > (fire_wait2 * 2)) {
                    entity->rapid_fire_spread_modifier = 0.0f;
                }
                else {
                    entity->rapid_fire_spread_modifier = 1.0f;
                }
            }
        }
    },
};

using Vector3_rand_around_dir = void __fastcall(rf::Vector3*, rf::Vector3, float);
extern CallHook<Vector3_rand_around_dir> Vector3_rand_around_dir_hook;
void __fastcall Vector3_rand_around_dir_new (rf::Vector3* self, rf::Vector3 dir, float dotfactor)
{
    if (should_use_gaussian_spread()) {
        self->rand_around_dir_gaussian(dir, dotfactor); // replace stock RNG function
    }
    else {
        Vector3_rand_around_dir_hook.call_target(self, dir, dotfactor); // maintain stock behaviour
    }
}
CallHook<Vector3_rand_around_dir> Vector3_rand_around_dir_hook{0x00426639, Vector3_rand_around_dir_new};

CodeInjection entity_get_weapon_spread_first_shot_patch {
    0x0042D0C2,
    [](auto& regs) {
        // apply rapid_fire_spread_modifier to all weapons, not just pistol
        if (should_use_gaussian_spread()) {
            regs.esp += 0x4;
            regs.eip = 0x0042D0CE;
        }
    },
};

CodeInjection autoswitch_empty_weapon_patch {
    0x004A6F41,
    [](auto& regs) {
        if (g_alpine_game_config.always_autoswitch_empty) {
            regs.eip = 0x004A6F57;
        }
    },
};

ConsoleCommand2 always_autoswitch_empty_cmd{
    "cl_autoswitchempty",
    []() {
        g_alpine_game_config.always_autoswitch_empty = !g_alpine_game_config.always_autoswitch_empty;
        rf::console::print("Always autoswitch empty weapons is {}",
                           g_alpine_game_config.always_autoswitch_empty ? "enabled" : "disabled");
    },
    "Toggles whether weapons with no ammo will autoswitch even if autoswitch is turned off",
};

void apply_weapon_patches()
{
    // Enable autoswitching when weapon ammo is empty, even when autoswitch is turned off
    autoswitch_empty_weapon_patch.install();

    // Apply new spread method using gaussian distribution and first shot accuracy
    Vector3_rand_around_dir_hook.install();
    entity_get_weapon_spread_first_shot_patch.install();

    // Apply fire wait to semi auto weapons and adjust values to be reasonable
    fire_primary_weapon_semi_auto_patch.install();
    entity_fire_primary_weapon_semi_auto_patch.install();

    // Stop weapons visually richocheting in multiplayer
    weapon_possibly_richochet.install();

    // Fix crashes caused by too many records in weapons.tbl file
    weapons_tbl_buffer_overflow_fix_1.install();
    weapons_tbl_buffer_overflow_fix_2.install();

    // Track which weapon reticles are customized
    weapon_init_track_reticle_bitmap_injection.install();

#if 0
    // Fix weapon switch glitch when reloading (should be used on Match Mode)
    AsmWriter(0x004A4B4B).call(entity_is_reloading_player_select_weapon_new);
    AsmWriter(0x004A4B77).call(entity_is_reloading_player_select_weapon_new);
#endif

    // Delete weapons (projectiles) that reach bounding box of the level
    weapon_move_one_hook.install();

    // Fix weapon vs object collisions for big objects
    weapon_vs_obj_collision_fix.install();

    // Fix muzzle flash light sometimes not getting disabled (e.g. when weapon is switched during riot stick attack
    // in multiplayer)
    muzzle_flash_light_not_disabled_fix.install();

    // Fix weapon being auto-switched to previous one after respawn even when auto-switch is disabled
    process_create_entity_packet_switch_weapon_fix.install();

    // Show enemy bullets
    apply_show_enemy_bullets();

    // Fix rockets not making damage after hitting a detail brush
    weapon_hit_wall_obj_apply_radius_damage_hook.install();

    // commands
    multi_ricochet_cmd.register_cmd();
    show_enemy_bullets_cmd.register_cmd();
    gaussian_spread_cmd.register_cmd();
    unlimited_semi_auto_cmd.register_cmd();
    always_autoswitch_empty_cmd.register_cmd();
}
