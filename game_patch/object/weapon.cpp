#include <patch_common/FunHook.h>
#include <patch_common/AsmWriter.h>
#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/ShortTypes.h>
#include <algorithm>
#include <array>
#include <deque>
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
#include "../multi/kill_attribution.h"
#include "../multi/mutators.h"
#include "../misc/misc.h"
#include "../misc/alpine_settings.h"
#include "../os/os.h"

static std::array<uint8_t, 64U> weapon_reticle_custom_mask{}; // bit 0 = _0, bit 1 = _1
static std::pair<bool, bool> rocket_locked_custom_reticle = {false, false};

static bool weapon_is_excluded_from_gaussian_rng(int weapon_type)
{
    return weapon_type == rf::shotgun_weapon_type;
}

static bool weapon_is_excluded_from_first_shot_accuracy(int weapon_type)
{
    return weapon_type == rf::shotgun_weapon_type
        || weapon_type == rf::sniper_rifle_weapon_type
        || weapon_type == rf::scope_assault_rifle_weapon_type;
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
    [] {
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
        // Covers fused and detonator-triggered explosions, which detonate from inside the mover.
        // Constructed EVERY frame for EVERY live weapon, so the scope ctor must stay a pure lookup.
        SplashWeaponScope splash_scope{weapon};
        const CritWeaponScope crit_scope{weapon};
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

        // Skip collision with debris that has OF_NO_COLLIDE_REGISTER (e.g. geomod rock debris).
        // The flag only prevents pair registration from the debris side; weapons still try to
        // register pairs with all physics objects, so we must reject the pair here.
        if (obj->type == rf::OT_DEBRIS && (obj->obj_flags & rf::OF_NO_COLLIDE_REGISTER)) {
            regs.eip = 0x0048C82A;
            return;
        }

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

// 0x004C53A8 is also the Critical Hits mutator's weapon_hit_level detonation site, so it calls
// crits_on_explosion from here rather than putting a second CallHook on the same address
// (see crits_explosion_hook in multi/mutators.cpp).
CallHook<void(rf::Vector3&, float, float, int, int)> weapon_hit_wall_obj_apply_radius_damage_hook{
    0x004C53A8,
    [](rf::Vector3& epicenter, float damage, float radius, int killer_handle, int damage_type) {
        auto& collide_out = *reinterpret_cast<rf::PCollisionOut*>(&epicenter);
        auto new_epicenter = epicenter + collide_out.hit_normal * 0.0001f;
        const float crit_scale = crits_on_explosion(&new_epicenter, radius);
        weapon_hit_wall_obj_apply_radius_damage_hook.call_target(new_epicenter, damage, radius * crit_scale, killer_handle, damage_type);
    },
};

// Server-confirmed hit FX: when the local player's projectile hits another player, park the
// impact FX (blood, vclip, foley sound) instead of playing them immediately, then replay them
// when the server confirms the hit via af_damage_notify. Markers for hits the server rejects
// (lag compensation disagreement) expire silently.
struct ConfirmedHitMarker
{
    int victim_handle;
    rf::Vector3 hit_point;
    rf::Vector3 hit_normal;
    rf::Vector3 dir;
    int weapon_type;
    int parent_handle;
    int impact_sound; // sound handle resolved at park time, -1 = none
    bool impact_vclip;
    int64_t expiry;
};
static std::deque<ConfirmedHitMarker> g_confirmed_hit_markers;
static constexpr size_t confirmed_hit_max_markers = 32;
static constexpr int64_t confirmed_hit_marker_ms = 500; // povcomp_max_ms (450) + margin

static bool confirmed_hit_fx_should_park(rf::Weapon* wp, rf::Object* victim)
{
    if (!rf::is_multi || rf::is_server || !g_alpine_game_config.confirmed_hit_fx) {
        return false;
    }
    // without server damage notifications no confirmation ever arrives - keep stock instant FX
    const auto& server_info = get_af_server_info();
    if (!server_info || !server_info->damage_notifications) {
        return false;
    }
    if (!rf::local_player || wp->parent_handle != rf::local_player->entity_handle) {
        return false;
    }
    // only the flesh FX branch of weapon_hit_obj is deferred, and only for living player victims
    if (victim->type != rf::OT_ENTITY || victim->material != 3) {
        return false;
    }
    rf::Entity* ep = rf::entity_from_handle(victim->handle);
    if (!ep || rf::entity_is_dying(ep) || !rf::player_from_entity_handle(victim->handle)) {
        return false;
    }
    // melee/sticky/remote charge/flamethrower and the riot stick special sound path keep stock FX
    if (wp->info->flags & (rf::WTF_MELEE | rf::WTF_STICKY | rf::WTF_REMOTE_CHARGE)) {
        return false;
    }
    if (wp->info_index == rf::flamethrower_weapon_type || (wp->weapon_flags & 0x8)) {
        return false;
    }
    return true;
}

// FX block of weapon_hit_obj (0x004C59F0). At this address ESI = weapon and EBX = hit object on
// both the client path (jump from 0x004C5CE3) and the server path (EBX reload at 0x004C62FA).
CodeInjection weapon_hit_obj_confirmed_hit_fx_injection{
    0x004C6301,
    [](auto& regs) {
        rf::Weapon* wp = regs.esi;
        rf::Object* victim = regs.ebx;
        if (!confirmed_hit_fx_should_park(wp, victim)) {
            return;
        }
        // resolve the impact foley sound like the stock selection at 0x004C6463
        int material = std::clamp(wp->p_data.collide_out.material, 0, 9);
        int sound = rf::foley_get_sound_handle(wp->info->impact_foley_id[material]);
        if (sound == -1) {
            sound = rf::foley_get_sound_handle(wp->info->impact_foley_id[0]);
        }
        if (g_confirmed_hit_markers.size() >= confirmed_hit_max_markers) {
            g_confirmed_hit_markers.pop_front();
        }
        g_confirmed_hit_markers.push_back({
            victim->handle,
            wp->p_data.collide_out.hit_point,
            wp->p_data.collide_out.hit_normal,
            wp->orient.fvec,
            wp->info_index,
            wp->parent_handle,
            sound,
            wp->info->crater_radius > addr_as_ref<float>(0x005893F8), // stock flesh vclip gate
            timer::get_i64(1000) + confirmed_hit_marker_ms,
        });
        regs.eip = 0x004C655D; // skip the FX block to the epilogue; FX replayed on confirmation
    },
};

void confirmed_hit_fx_on_damage_notify(rf::Entity* victim)
{
    if (!g_alpine_game_config.confirmed_hit_fx || rf::entity_is_local_player(victim)) {
        return;
    }
    const auto& server_info = get_af_server_info();
    if (!server_info || !server_info->damage_notifications) {
        return;
    }

    const int64_t now = timer::get_i64(1000);
    std::erase_if(g_confirmed_hit_markers, [now](const ConfirmedHitMarker& m) { return now > m.expiry; });

    for (auto it = g_confirmed_hit_markers.begin(); it != g_confirmed_hit_markers.end(); ++it) {
        if (it->victim_handle != victim->handle) {
            continue;
        }
        ConfirmedHitMarker m = *it;
        g_confirmed_hit_markers.erase(it);
        // mirror the stock flesh FX block of weapon_hit_obj (0x004C6321); the multi client path
        // always passes damage 0.0 there, so replaying with 0.0 matches stock visuals
        rf::entity_blood_maybe_splatter(0.0f, &m.hit_point, &m.dir);
        rf::entity_blood_do_hit_effect(&m.hit_point, victim->room, &victim->pos, 0.0f);
        if (m.impact_vclip) {
            rf::weapon_create_impact_vclip(m.weapon_type, reinterpret_cast<int>(victim->room), nullptr,
                                           &m.hit_point, &m.hit_normal, m.parent_handle);
        }
        if (m.impact_sound != -1) {
            rf::snd_play_3d(m.impact_sound, m.hit_point, 1.0f, rf::Vector3{}, 0);
        }
        return;
    }

    // No parked impact (very high ping, or a hit this client never predicted): approximate at the
    // victim's current position so the confirmed hit still shows blood
    rf::Entity* local_entity = rf::local_player ? rf::entity_from_handle(rf::local_player->entity_handle) : nullptr;
    if (!local_entity) {
        return;
    }
    rf::Vector3 dir = victim->pos - local_entity->pos;
    dir.normalize_safe();
    rf::entity_blood_maybe_splatter(0.0f, &victim->pos, &dir);
    rf::entity_blood_do_hit_effect(&victim->pos, victim->room, &victim->pos, 0.0f);
    int weapon_type = local_entity->ai.current_primary_weapon;
    if (weapon_type >= 0 && weapon_type < rf::num_weapon_types) {
        int sound = rf::foley_get_sound_handle(rf::weapon_types[weapon_type].impact_foley_id[3]); // flesh
        if (sound != -1) {
            rf::snd_play_3d(sound, victim->pos, 1.0f, rf::Vector3{}, 0);
        }
    }
}

ConsoleCommand2 confirmed_hits_cmd{
    "cl_confirmedhits",
    []() {
        g_alpine_game_config.confirmed_hit_fx = !g_alpine_game_config.confirmed_hit_fx;
        rf::console::print("Server-confirmed hit effects are {}",
                           g_alpine_game_config.confirmed_hit_fx ? "enabled" : "disabled");
    },
    "Toggles whether blood and impact effects on other players are delayed until the server confirms the hit",
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
                if (!weapon_is_excluded_from_first_shot_accuracy(weapon_type) && entity->last_fired_timestamp.time_since() > (fire_wait2 * 2)) {
                    entity->rapid_fire_spread_modifier = 0.0f;
                }
                else {
                    entity->rapid_fire_spread_modifier = 1.0f;
                }
            }
        }
    },
};

CodeInjection weapon_spread_gaussian_rng_patch{
    0x00426639,
    [](auto& regs) {
        int weapon_type = regs.ebx;
        if (should_use_gaussian_spread() && !weapon_is_excluded_from_gaussian_rng(weapon_type)) {
            auto self = static_cast<rf::Vector3*>(regs.ecx);
            auto& dir = *static_cast<rf::Vector3*>(regs.esp);
            float dotfactor = *reinterpret_cast<float*>(regs.esp + 12);
            self->rand_around_dir_gaussian(dir, dotfactor);
            regs.esp += 0x10;
            regs.eip = 0x0042663E;
        }
    },
};

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
    weapon_spread_gaussian_rng_patch.install();
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

    // Delay blood/impact FX on other players until the server confirms the hit
    weapon_hit_obj_confirmed_hit_fx_injection.install();

    // commands
    multi_ricochet_cmd.register_cmd();
    confirmed_hits_cmd.register_cmd();
    show_enemy_bullets_cmd.register_cmd();
    gaussian_spread_cmd.register_cmd();
    unlimited_semi_auto_cmd.register_cmd();
    always_autoswitch_empty_cmd.register_cmd();
}
