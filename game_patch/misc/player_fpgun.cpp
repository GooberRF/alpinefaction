#include <patch_common/CodeInjection.h>
#include <patch_common/FunHook.h>
#include <patch_common/CallHook.h>
#include <patch_common/AsmWriter.h>
#include <common/config/BuildConfig.h>
#include "alpine_options.h"
#include "alpine_settings.h"
#include "../multi/gametype.h"
#include "../rf/player/player.h"
#include "../rf/player/camera.h"
#include "../rf/sound/sound.h"
#include "../rf/vmesh.h"
#include "../rf/weapon.h"
#include "../rf/entity.h"
#include "../rf/multi.h"
#include "../graphics/gr.h"
#include "../main/main.h"
#include "../os/console.h"

static std::vector<int> g_fpgun_sounds;
static rf::Player* g_fpgun_main_player = nullptr;

static FunHook<void(rf::Player*)> player_fpgun_update_state_anim_hook{
    0x004AA3A0,
    [](rf::Player* player) {
        if (player == rf::local_player) {
            // Only run the original state anim logic for the local player
            player_fpgun_update_state_anim_hook.call_target(player);
            return;
        }
        rf::Entity* entity = rf::entity_from_handle(player->entity_handle);
        if (!entity)
            return;
        int state = rf::WS_IDLE;
        if (rf::entity_weapon_is_on(entity->handle, entity->ai.current_primary_weapon))
            state = rf::WS_LOOP_FIRE;
        else if (!rf::entity_is_falling(entity) && !rf::entity_is_swimming(entity)) {
            // Only use the running animation when on the ground and moving.
            // While falling, stay in idle to match normal first-person behavior.
            float horz_speed_pow2 = entity->p_data.vel.x * entity->p_data.vel.x +
                                      entity->p_data.vel.z * entity->p_data.vel.z;
            if (horz_speed_pow2 > 0.2f)
                state = rf::WS_RUN;
        }
        if (!rf::player_fpgun_is_in_state_anim(player, state))
            rf::player_fpgun_set_next_state_anim(player, state);
    },
};

static FunHook<void(rf::Player*)> player_fpgun_render_ir_hook{
    0x004AEEF0,
    [](rf::Player* player) {
        if (player->cam) {
            player_fpgun_render_ir_hook.call_target(player->cam->player);
        }
    },
};

static FunHook<void(rf::Player*)> player_fpgun_render_for_rail_gun_hook{
    0x004ADC60,
    [](rf::Player* player) {
        if (player->cam) {
            rf::Player* target = player->cam->player;
            // render_to_dynamic_textures sets drawing_entity_bmp on local_player,
            // but the function stores param_1 in a global and entity render functions
            // read drawing_entity_bmp from there. When spectating, we redirect to the
            // spectate target, so we must propagate the flag.
            bool propagate = (target != player);
            if (propagate) {
                target->fpgun_data.drawing_entity_bmp = player->fpgun_data.drawing_entity_bmp;
            }
            player_fpgun_render_for_rail_gun_hook.call_target(target);
            if (propagate) {
                target->fpgun_data.drawing_entity_bmp = false;
            }
        }
    },
};

static CodeInjection player_fpgun_play_anim_injection{
    0x004A947B,
    [](auto& regs) {
        if (regs.eax >= 0) {
            g_fpgun_sounds.push_back(regs.eax);
        }
    },
};

void player_fpgun_move_sounds(const rf::Vector3& camera_pos, const rf::Vector3& camera_vel)
{
    // Update position of fpgun sound
    auto it = g_fpgun_sounds.begin();
    while (it != g_fpgun_sounds.end()) {
        int sound_handle = *it;
        if (rf::snd_is_playing(sound_handle)) {
            rf::snd_change_3d(sound_handle, camera_pos, camera_vel, 1.0f);
            ++it;
        }
        else {
            it = g_fpgun_sounds.erase(it);
        }
    }
}

void player_fpgun_reload_meshes(bool force)
{
    int fpgun_team = 999;
    if (multi_is_team_game_type()) {
        fpgun_team = g_fpgun_main_player->team;
    }
    auto& fpgun_meshes_current_mp_character = addr_as_ref<int>(0x005A0AEC);
    auto& fpgun_meshes_current_team = addr_as_ref<int>(0x005A0AF0);
    if (g_fpgun_main_player->settings.multi_character != fpgun_meshes_current_mp_character || fpgun_team != fpgun_meshes_current_team || force) {
        rf::player_fpgun_delete_meshes();
        rf::player_fpgun_load_meshes();
        auto* entity = rf::entity_from_handle(g_fpgun_main_player->entity_handle);
        if (entity) {
            auto weapon_type = entity->ai.current_primary_weapon;
            rf::player_fpgun_set_state(g_fpgun_main_player, weapon_type);
            rf::player_fpgun_page_in(g_fpgun_main_player, 0, weapon_type);
        }
    }
}

void fpgun_play_random_idle_anim()
{
    auto* pp = rf::local_player;
    if (!pp)
        return;

    const int weapon_cls_id = rf::player_get_current_weapon(pp);
    if (weapon_cls_id < 0)
        return;

    bool deny_play_idle =
        rf::player_fpgun_is_in_state_anim(pp, rf::WeaponState::WS_LOOP_FIRE) ||
        rf::player_fpgun_action_anim_is_playing(pp, rf::WeaponAction::WA_RELOAD) ||
        rf::player_fpgun_action_anim_is_playing(pp, rf::WeaponAction::WA_IDLE_1) ||
        rf::player_fpgun_action_anim_is_playing(pp, rf::WeaponAction::WA_IDLE_2) ||
        rf::player_fpgun_action_anim_is_playing(pp, rf::WeaponAction::WA_IDLE_3) ||
        rf::player_fpgun_action_anim_is_playing(pp, rf::WeaponAction::WA_HOLSTER) ||
        rf::player_fpgun_action_anim_is_playing(pp, rf::WeaponAction::WA_DRAW);

    if (deny_play_idle)
        return;

    rf::WeaponAction candidates[3];
    int count = 0;

    for (int action = static_cast<int>(rf::WeaponAction::WA_IDLE_1);
        action <= static_cast<int>(rf::WeaponAction::WA_IDLE_3);
        ++action)
    {
        auto wa = static_cast<rf::WeaponAction>(action);
        if (rf::player_fpgun_action_anim_exists(weapon_cls_id, wa)) {
            candidates[count++] = wa;
        }
    }

    if (count == 0)
        return;

    const double roll = std::generate_canonical<double, 16>(g_rng);
    int idx = 0;

    if (count == 1) {
        idx = 0;
    }
    else if (count == 2) {
        // 90% first, 10% second
        idx = (roll < 0.9) ? 0 : 1;
    }
    else { // count == 3
        // 60% first, 30% second, 10% third
        if (roll < 0.6)
            idx = 0;
        else if (roll < 0.9)
            idx = 1;
        else
            idx = 2;
    }

    rf::player_fpgun_play_anim(pp, candidates[idx]);
    rf::player_fpgun_reset_idle_timeout(pp);
}

CodeInjection player_fpgun_update_state_anim_stop_idle_injection{
    0x004AA3FA,
    [](auto& regs) {
        rf::Player* pp = regs.esi;
        if (pp) {
            rf::player_fpgun_stop_idle_actions(pp);
        }
    },
};

CallHook<void(rf::Player*)> player_fpgun_stop_idle_actions_hook{
    0x004AA4E4,
    [](rf::Player* pp) {
        return;
    },
};

// Fix weapon bob oscillation: the original code at 0x004AA409 sets the weapon to IDLE before
// checking if the player is running, causing constant IDLE<->RUN oscillation. This injection
// skips that premature IDLE transition; the second IDLE check at 0x004AA474 handles all
// legitimate idle cases.
CodeInjection player_fpgun_skip_premature_idle_injection{
    0x004AA409,
    [](auto& regs) {
        if (!g_alpine_game_config.legacy_bob) {
            regs.eip = 0x004AA423;
        }
    },
};

ConsoleCommand2 legacy_bob_cmd{
    "cl_legacy_bob",
    []() {
        g_alpine_game_config.legacy_bob = !g_alpine_game_config.legacy_bob;
        rf::console::print("Legacy weapon bob: {}",
            g_alpine_game_config.legacy_bob ? "enabled" : "disabled");
    },
    "Toggle legacy weapon bob (original stutters while running)",
};

#ifndef NDEBUG

ConsoleCommand2 reload_fpgun_cmd{
    "d_reload_fpgun",
    []() {
        player_fpgun_reload_meshes(true);
    },
};

#endif // NDEBUG

void player_fpgun_set_player(rf::Player* pp)
{
    if (g_fpgun_main_player == pp) {
        return;
    }
    g_fpgun_main_player = pp;
    player_fpgun_reload_meshes(false);
}

void player_fpgun_on_player_death(rf::Player* pp)
{
    // Reset fpgun animation when player dies
    if (pp->weapon_mesh_handle) {
        rf::vmesh_stop_all_actions(pp->weapon_mesh_handle); // prevent occasional crash where fpgun mesh is invalid
    }    
    rf::player_fpgun_clear_all_action_anim_sounds(pp);
}

CodeInjection railgun_scanner_start_render_to_texture{
    0x004ADD0A,
    [](auto& regs) {
        // Always render into local_player's bitmap. The HUD overlay in gameplay_render_frame
        // reads from local_player->ir_data.ir_bitmap_handle, so that's where the content must go.
        // In normal play EBX == local_player so this is equivalent. In spectate, EBX is the
        // spectate target (redirected by player_fpgun_render_for_rail_gun_hook) but the HUD
        // still reads from local_player.
        rf::Player* target = rf::local_player ? rf::local_player : static_cast<rf::Player*>(regs.ebx);
        gr_set_render_target(target->ir_data.ir_bitmap_handle);
    },
};

CodeInjection player_fpgun_render_ir_begin_render_to_texture{
    0x004AF0BC,
    [](auto& regs) {
        rf::Player* player = regs.esi;
        gr_set_render_target(player->ir_data.ir_bitmap_handle);
    },
};

CodeInjection after_game_render_to_dynamic_textures{
    0x00431890,
    []() {
        // Render to back-buffer from this point
        gr_set_render_target(-1);
    },
};

CallHook<void(rf::Matrix3&, rf::Vector3&, float, bool, bool)> player_fpgun_render_gr_setup_3d_hook{
    0x004AB411,
    [](rf::Matrix3& viewer_orient, rf::Vector3& viewer_pos, float horizontal_fov, bool zbuffer_flag, bool z_scale) {
        horizontal_fov *= g_alpine_game_config.fpgun_fov_scale;
        horizontal_fov = gr_scale_fov_hor_plus(horizontal_fov);
        player_fpgun_render_gr_setup_3d_hook
            .call_target(viewer_orient, viewer_pos, horizontal_fov, zbuffer_flag, z_scale);
    },
};

ConsoleCommand2 fpgun_fov_scale_cmd{
    "r_fpgunfov",
    [](std::optional<float> scale_opt) {
        if (scale_opt) {
            g_alpine_game_config.set_fpgun_fov_scale(scale_opt.value());
        }
        rf::console::print("Fpgun FOV scale: {:.4f}", g_alpine_game_config.fpgun_fov_scale);
    },
    "Set scale value applied to FOV setting for first person weapon models.",
    "r_fpgunfov [scale]",
};

CodeInjection player_fpgun_render_main_player_entity_injection{
    0x004ABB59,
    [](auto& regs) {
        // return entity that is used to determine what powerups are active during fpgun rendering
        regs.eax = rf::entity_from_handle(g_fpgun_main_player->entity_handle);
        regs.eip = 0x004ABB5E;
    },
};

CodeInjection player_fpgun_render_ir_cull_patch_1{
    0x004AF137,
    [] (auto& regs) {
        const rf::Object& object = addr_as_ref<rf::Object>(regs.ebx);
        rf::Vector3 root_bone_pos{};
        rf::obj_find_root_bone_pos(object, root_bone_pos);
        if (rf::gr::cull_sphere(root_bone_pos, object.p_data.radius)) {
            regs.eip = 0x004AF427;
        } else {
            regs.eip = 0x004AF164;
        }
        regs.esp += 8;
    },
};

CodeInjection player_fpgun_render_ir_cull_patch_2{
    0x004AF47F,
    [] (auto& regs) {
        const rf::Object& object = addr_as_ref<rf::Object>(regs.edi);
        rf::Vector3 root_bone_pos{};
        rf::obj_find_root_bone_pos(object, root_bone_pos);
        if (rf::gr::cull_sphere(root_bone_pos, object.p_data.radius)) {
            regs.eip = 0x004AF762;
        } else {
            regs.eip = 0x004AF4AC;
        }
        regs.esp += 8;
    },
};

CodeInjection players_cleanup_injection{
    0x004A259C,
    []() {
        g_fpgun_main_player = nullptr;
    },
};

void player_fpgun_do_patch()
{
#if SPECTATE_MODE_SHOW_WEAPON
    AsmWriter(0x004AB1B8).nop(6); // player_fpgun_render
    AsmWriter(0x004AA23E).nop(6); // player_fpgun_set_state
    AsmWriter(0x004AE0DF).nop(2); // player_fpgun_get_vmesh_handle

    AsmWriter(0x004A938F).nop(6);               // player_fpgun_play_anim
    write_mem<u8>(0x004A952C, asm_opcodes::jmp_rel_short); // player_fpgun_is_in_state_anim
    AsmWriter(0x004AA56D).nop(6);               // player_fpgun_set_next_state_anim
    AsmWriter(0x004AA6E7).nop(6);               // player_fpgun_process
    AsmWriter(0x004AE384).nop(6);               // player_fpgun_page_in
    write_mem<u8>(0x004ACE2C, asm_opcodes::jmp_rel_short); // player_fpgun_get_zoom
    write_mem<u8>(0x004AD6E0, asm_opcodes::jmp_rel_short); // player_fpgun_get_muzzle_tag_pos
    AsmWriter(0x004ACC8E).nop(6);               // player_fpgun_get_non_bullet_muzzle_flash_info
    AsmWriter(0x004AB03D).nop(2);                          // player_fpgun_is_firing_or_reloading: run checks for all players
    write_mem<u8>(0x004AB0CC, asm_opcodes::jmp_rel_short); // player_fpgun_is_holstering_or_drawing: run checks for all players
    write_mem<u8>(0x004ADB6C, asm_opcodes::jmp_rel_short); // player_fpgun_is_in_custom_anim: run checks for all players
    AsmWriter(0x004AD8CE).nop(6);               // player_fpgun_action_anim_is_playing: run checks for all players

    write_mem_ptr(0x004AE569 + 2, &g_fpgun_main_player); // player_fpgun_load_meshes
    write_mem_ptr(0x004AE5E3 + 2, &g_fpgun_main_player); // player_fpgun_load_meshes
    write_mem_ptr(0x004AE647 + 2, &g_fpgun_main_player); // player_fpgun_load_meshes
    write_mem_ptr(0x004AE6F1 + 2, &g_fpgun_main_player); // player_fpgun_load_meshes
    write_mem_ptr(0x004AEB86 + 1, &g_fpgun_main_player); // player_fpgun_delete_meshes
    write_mem_ptr(0x004A44BF + 2, &g_fpgun_main_player); // player_create_entity
    write_mem_ptr(0x004A44F7 + 2, &g_fpgun_main_player); // player_create_entity

    players_cleanup_injection.install(); // fixes crash at 0x004AEB8F in player_fpgun_delete_meshes

    player_fpgun_render_main_player_entity_injection.install();

    player_fpgun_update_state_anim_hook.install();

    // Fix weapon bob oscillation: skip premature IDLE transition in update_state_anim
    player_fpgun_skip_premature_idle_injection.install();
    legacy_bob_cmd.register_cmd();

    // Render IR for player that is currently being shown by camera - needed for spectate mode
    player_fpgun_render_ir_hook.install();

    // Render rail gun scanner for spectated player
    player_fpgun_render_for_rail_gun_hook.install();
    AsmWriter(0x004ADCB5).nop(6); // player_fpgun_render_for_rail_gun - remove local_player check
#endif // SPECTATE_MODE_SHOW_WEAPON

    // Update fpgun 3D sounds positions
    player_fpgun_play_anim_injection.install();

    // Use render target texture for IR and Railgun scanner bitmap update
    railgun_scanner_start_render_to_texture.install();
    player_fpgun_render_ir_begin_render_to_texture.install();
    after_game_render_to_dynamic_textures.install();
    AsmWriter{0x004AE0BF}.nop(5);
    AsmWriter{0x004AF7D7}.nop(5);
    AsmWriter{0x004AF868}.nop(5);

    if (g_game_config.high_scanner_res) {
        // Improved Railgun Scanner resolution
        constexpr int8_t scanner_resolution = 120;        // default is 64, max is 127 (signed byte)
        write_mem<u8>(0x004325E6 + 1, scanner_resolution); // gameplay_render_frame
        write_mem<u8>(0x004325E8 + 1, scanner_resolution);
        write_mem<u8>(0x004A34BB + 1, scanner_resolution); // player_allocate
        write_mem<u8>(0x004A34BD + 1, scanner_resolution);
        write_mem<u8>(0x004ADD70 + 1, scanner_resolution); // player_fpgun_render_for_rail_gun
        write_mem<u8>(0x004ADD72 + 1, scanner_resolution);
        write_mem<u8>(0x004AE0B7 + 1, scanner_resolution);
        write_mem<u8>(0x004AE0B9 + 1, scanner_resolution);
        write_mem<u8>(0x004AF0B0 + 1, scanner_resolution); // player_fpgun_render_ir
        write_mem<u8>(0x004AF0B4 + 1, scanner_resolution * 3 / 4);
        write_mem<u8>(0x004AF0B6 + 1, scanner_resolution);
        write_mem<u8>(0x004AF7B0 + 1, scanner_resolution);
        write_mem<u8>(0x004AF7B2 + 1, scanner_resolution);
        write_mem<u8>(0x004AF7CF + 1, scanner_resolution);
        write_mem<u8>(0x004AF7D1 + 1, scanner_resolution);
        write_mem<u8>(0x004AF818 + 1, scanner_resolution);
        write_mem<u8>(0x004AF81A + 1, scanner_resolution);
        write_mem<u8>(0x004AF820 + 1, scanner_resolution);
        write_mem<u8>(0x004AF822 + 1, scanner_resolution);
        write_mem<u8>(0x004AF855 + 1, scanner_resolution);
        write_mem<u8>(0x004AF860 + 1, scanner_resolution * 3 / 4);
        write_mem<u8>(0x004AF862 + 1, scanner_resolution);
    }

    // Render rocket launcher scanner image every frame
    // addr_as_ref<bool>(0x5A1020) = 0;

    // do not stop playing idle anim when fpgun state anim changes
    player_fpgun_stop_idle_actions_hook.install();

    // stop idle anims when starting auto fire (semi auto is already handled by stock game)
    player_fpgun_update_state_anim_stop_idle_injection.install();

    // Allow customizing fpgun fov
    player_fpgun_render_gr_setup_3d_hook.install();
    fpgun_fov_scale_cmd.register_cmd();

    // Do not cull entities too early.
    player_fpgun_render_ir_cull_patch_1.install();
    player_fpgun_render_ir_cull_patch_2.install();

#ifndef NDEBUG
    reload_fpgun_cmd.register_cmd();
#endif
}
