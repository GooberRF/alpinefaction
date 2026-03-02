#include "multi_spectate.h"
#include "hud.h"
#include "multi_scoreboard.h"
#include "../input/input.h"
#include "../os/console.h"
#include "../rf/entity.h"
#include "../rf/level.h"
#include "../rf/player/player.h"
#include "../rf/multi.h"
#include "../rf/gameseq.h"
#include "../rf/weapon.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../rf/hud.h"
#include "../rf/player/camera.h"
#include "../rf/player/player_fpgun.h"
#include "../main/main.h"
#include "../misc/player.h"
#include "../misc/alpine_settings.h"
#include <common/config/BuildConfig.h>
#include <xlog/xlog.h>
#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/FunHook.h>
#include <patch_common/AsmWriter.h>
#include <shlwapi.h>
#include <windows.h>
#include "../multi/alpine_packets.h"

static rf::Player* g_spectate_mode_target;
static rf::Camera* g_old_target_camera = nullptr;
static bool g_spectate_mode_enabled = false;
static bool g_spectate_mode_follow_killer = false;

// Edge-detection state for spectated player action animations
static bool g_prev_weapon_is_on = false;
static bool g_prev_is_reloading = false;
static bool g_prev_alt_fire_is_on = false;
static int g_prev_weapon_type = -1;

void player_fpgun_set_player(rf::Player* pp);

static void set_camera_target(rf::Player* player)
{
    // Based on function set_camera1_view
    if (!rf::local_player || !rf::local_player->cam || !player)
        return;

    rf::Camera* camera = rf::local_player->cam;
    camera->mode = rf::CAMERA_FIRST_PERSON;
    camera->player = player;

    g_old_target_camera = player->cam;
    player->cam = camera; // fix crash 0040D744

    rf::camera_enter_first_person(camera);
}

static bool is_force_respawn()
{
    return g_spawned_in_current_level && (rf::netgame.flags & rf::NG_FLAG_FORCE_RESPAWN);
}

static void spectate_entity_translate_stand_state_to_crouch_state(int* state_anim_index)
{
    if (!state_anim_index) {
        return;
    }

    switch (static_cast<rf::EntityState>(*state_anim_index)) {
    case rf::ENTITY_STATE_STAND:
        *state_anim_index = rf::ENTITY_STATE_CROUCH;
        break;
    case rf::ENTITY_STATE_ATTACK_STAND:
        *state_anim_index = rf::ENTITY_STATE_ATTACK_CROUCH;
        break;
    case rf::ENTITY_STATE_WALK:
        *state_anim_index = rf::ENTITY_STATE_ATTACK_CROUCH_WALK;
        break;
    default:
        break;
    }
}

static bool state_animation_is_crouch(int state)
{
    return state >= rf::ENTITY_STATE_CROUCH && state <= rf::ENTITY_STATE_ATTACK_CROUCH_WALK;
}

// Hook entity_set_next_state_anim to remap non-crouch animations to crouch
// variants for the spectated entity when it's crouching. This prevents the
// movement state machine from constantly overriding the crouch animation.
FunHook<void(rf::Entity*, int, float)> spectate_entity_set_next_state_anim_hook{
    0x0042A580,
    [](rf::Entity* entity, int state_anim_index, float transition_time) {
        if (g_spectate_mode_enabled && g_spectate_mode_target && rf::entity_is_crouching(entity)
            && entity->current_state_anim != rf::ENTITY_STATE_FREEFALL) {
            rf::Entity* target = rf::entity_from_handle(g_spectate_mode_target->entity_handle);
            if (entity == target) {
                spectate_entity_translate_stand_state_to_crouch_state(&state_anim_index);
            }
        }
        spectate_entity_set_next_state_anim_hook.call_target(entity, state_anim_index, transition_time);
    },
};

void multi_spectate_sync_crouch_anim()
{
    // The animation hook handles remapping automatically. We just need to
    // kick-start the transition for entities already in a non-crouch anim.
    if (!g_spectate_mode_enabled || !g_spectate_mode_target)
        return;

    rf::Entity* entity = rf::entity_from_handle(g_spectate_mode_target->entity_handle);
    if (!entity)
        return;

    if (rf::entity_is_crouching(entity) && !state_animation_is_crouch(entity->current_state_anim)
        && entity->current_state_anim != rf::ENTITY_STATE_FREEFALL) {
        int next_state_anim = entity->current_state_anim;
        spectate_entity_translate_stand_state_to_crouch_state(&next_state_anim);
        if (next_state_anim == entity->current_state_anim) {
            next_state_anim = rf::ENTITY_STATE_CROUCH;
        }
        rf::entity_set_next_state_anim(entity, next_state_anim, 0.15f);
    }
}

void multi_spectate_set_target_player(rf::Player* player)
{
    if (!player)
        player = rf::local_player;

    if (!rf::local_player || !rf::local_player->cam || !g_spectate_mode_target || g_spectate_mode_target == player)
        return;

    if (is_force_respawn()) {
        rf::String msg{"You cannot use Spectate Mode because Force Respawn is enabled in this server!"};
        rf::String prefix;
        rf::multi_chat_print(msg, rf::ChatMsgColor::white_white, prefix);
        return;
    }

    // fix old target
    if (g_spectate_mode_target && g_spectate_mode_target != rf::local_player) {
        g_spectate_mode_target->cam = g_old_target_camera;
        g_old_target_camera = nullptr;

#if SPECTATE_MODE_SHOW_WEAPON
        g_spectate_mode_target->flags &= ~(1u << 4);
        rf::Entity* entity = rf::entity_from_handle(g_spectate_mode_target->entity_handle);
        if (entity)
            entity->local_player = nullptr;
#endif // SPECTATE_MODE_SHOW_WEAPON
    }

    bool entering_player_spectate = (player != rf::local_player);

    if (entering_player_spectate) {
        g_local_queued_delayed_spawn = false;
        stop_draw_respawn_timer_notification();
    }
    else {
        // Clear scanner state when leaving spectate
        rf::local_player->fpgun_data.scanning_for_target = false;
    }

    g_spectate_mode_enabled = entering_player_spectate;
    if (g_spectate_mode_target != player) {
        af_send_spectate_start_packet(player);
        g_spectate_mode_target = player;
        // Reset action animation edge-detection state for new target
        g_prev_weapon_is_on = false;
        g_prev_is_reloading = false;
        g_prev_alt_fire_is_on = false;
        g_prev_weapon_type = -1;
    }

    rf::multi_kill_local_player();
    set_camera_target(player);

#if SPECTATE_MODE_SHOW_WEAPON
    player->flags |= 1u << 4;
    player->fpgun_data.fpgun_weapon_type = -1;
    player->weapon_mesh_handle = nullptr;
    rf::Entity* entity = rf::entity_from_handle(player->entity_handle);
    if (entity) {
        // make sure weapon mesh is loaded now
        rf::player_fpgun_set_state(player, entity->ai.current_primary_weapon);
        xlog::trace("FpgunMesh {}", player->weapon_mesh_handle);

        // Hide target player from camera
        entity->local_player = player;
    }
    player_fpgun_set_player(player);
#endif // SPECTATE_MODE_SHOW_WEAPON
}

static void spectate_next_player(const bool dir, const bool try_alive_players_first = false) {
    rf::Player* new_target = g_spectate_mode_enabled
        ? g_spectate_mode_target
        : rf::local_player;
    while (true) {
        new_target = dir ? new_target->next : new_target->prev;
        if (!new_target) {
            break;
        }
        if (new_target == g_spectate_mode_target) {
            break; // nothing found
        } else if (new_target->is_browser) {
            continue;
        } else if (try_alive_players_first && rf::player_is_dead(new_target)) {
            continue;
        } else if (new_target != rf::local_player) {
            multi_spectate_set_target_player(new_target);
            return;
        }
    }

    if (try_alive_players_first) {
        spectate_next_player(dir, false);
    }
}

void multi_spectate_enter_freelook()
{
    if (!rf::local_player || !rf::local_player->cam || !rf::is_multi)
        return;

    rf::multi_kill_local_player();
    rf::camera_enter_freelook(rf::local_player->cam);
    g_local_queued_delayed_spawn = false;
    stop_draw_respawn_timer_notification();
    af_send_spectate_start_packet(nullptr);

    // auto& hud_msg_current_index = addr_as_ref<int>(0x00597104);
    // hud_msg_current_index = -1;
}

bool multi_spectate_is_freelook()
{
    if (!rf::local_player || !rf::local_player->cam || !rf::is_multi)
        return false;

    auto camera_mode = rf::local_player->cam->mode;
    return camera_mode == rf::CAMERA_FREELOOK;
}

bool multi_spectate_is_spectating()
{
    return g_spectate_mode_enabled || multi_spectate_is_freelook();
}

rf::Player* multi_spectate_get_target_player()
{
    return g_spectate_mode_target;
}

void multi_spectate_leave()
{
    if (g_spectate_mode_enabled) {
        multi_spectate_set_target_player(nullptr);
    } else {
        set_camera_target(rf::local_player);
        af_send_spectate_start_packet(rf::local_player);
    }
}

bool multi_spectate_execute_action(rf::ControlConfigAction action, bool was_pressed)
{
    if (!rf::is_multi) {
        return false;
    }

    if (g_spectate_mode_enabled) {
        if (action == rf::CC_ACTION_PRIMARY_ATTACK || action == rf::CC_ACTION_SLIDE_RIGHT) {
            if (was_pressed)
                spectate_next_player(true);
            return true; // dont allow spawn
        }
        if (action == rf::CC_ACTION_SECONDARY_ATTACK || action == rf::CC_ACTION_SLIDE_LEFT) {
            if (was_pressed)
                spectate_next_player(false);
            return true;
        }
        if (action == rf::CC_ACTION_JUMP) {
            if (was_pressed)
                multi_spectate_leave();
            return true;
        }
    }
    else if (multi_spectate_is_freelook()) {
        // don't allow respawn in freelook spectate
        if (action == rf::CC_ACTION_PRIMARY_ATTACK || action == rf::CC_ACTION_SECONDARY_ATTACK) {
            if (was_pressed)
                multi_spectate_leave();
            return true;
        }
    }
    else if (!g_spectate_mode_enabled) {
        if (action == rf::CC_ACTION_JUMP && was_pressed && rf::player_is_dead(rf::local_player)) {
            multi_spectate_set_target_player(rf::local_player);
            spectate_next_player(true, true);
            return true;
        }
    }

    return false;
}

void multi_spectate_on_player_kill(rf::Player* victim, rf::Player* killer)
{
    if (!g_spectate_mode_enabled) {
        return;
    }
    if (g_spectate_mode_follow_killer && g_spectate_mode_target == victim && killer != rf::local_player) {
        // spectate killer if we were spectating victim
        // avoid spectating ourselves if we somehow managed to kill the victim
        multi_spectate_set_target_player(killer);
    }
}

void multi_spectate_on_destroy_player(rf::Player* player)
{
    if (player != rf::local_player) {
        if (g_spectate_mode_target == player)
            spectate_next_player(true);
        if (g_spectate_mode_target == player)
            multi_spectate_set_target_player(nullptr);
    }
}

// draw reticle
FunHook<void(rf::Player*)> render_reticle_hook{
    0x0043A2C0,
    [](rf::Player* player) {
        if (rf::gameseq_get_state() == rf::GS_MULTI_LIMBO)
            return;
        if (g_spectate_mode_enabled)
            render_reticle_hook.call_target(g_spectate_mode_target);
        else
            render_reticle_hook.call_target(player);
    },
};

// draw ammo
FunHook<void(rf::Player*)> hud_weapons_render_hook{
    0x0043B020,
    [](rf::Player* player) {
        if (rf::gameseq_get_state() == rf::GS_MULTI_LIMBO)
            return;
        // only show ammo counters in AF 1.1+ servers because ammo is not synced in legacy servers
        if (g_spectate_mode_enabled && is_server_minimum_af_version(1, 1) && !rf::player_is_dead(g_spectate_mode_target))
            hud_weapons_render_hook.call_target(g_spectate_mode_target);
        else
            hud_weapons_render_hook.call_target(player);
    },
};

// draw health/armour
FunHook<void(rf::Player*)> hud_status_render_spectate_hook{
    0x00439D80,
    [](rf::Player* player) {
        if (rf::gameseq_get_state() == rf::GS_MULTI_LIMBO)
            return;
        if (g_spectate_mode_enabled && !rf::player_is_dead(g_spectate_mode_target) && !rf::player_is_dying(g_spectate_mode_target))
            hud_status_render_spectate_hook.call_target(g_spectate_mode_target);
        else
            hud_status_render_spectate_hook.call_target(player);
    },
};

ConsoleCommand2 spectate_cmd{
    "spectate",
    [](std::optional<std::string> player_name) {
        if (!(rf::level.flags & rf::LEVEL_LOADED)) {
            rf::console::output("No level loaded!", nullptr);
            return;
        }

        if (!rf::is_multi) {
            // in single player, just enter free look mode
            rf::console::output("Camera mode set to free look. Use `camera1` to return to first person.", nullptr);
            rf::camera_enter_freelook(rf::local_player->cam);
            return;
        }

        if (is_force_respawn()) {
            rf::console::output("Spectate mode is disabled because of Force Respawn server option!", nullptr);
            return;
        }

        if (player_name) {
            // spectate player using 1st person view
            rf::Player* player = find_best_matching_player(player_name.value().c_str());
            if (!player) {
                // player not found
                return;
            }
            // player found - spectate
            multi_spectate_set_target_player(player);
        }
        else if (g_spectate_mode_enabled || multi_spectate_is_freelook()) {
            // leave spectate mode
            multi_spectate_leave();
        }
        else {
            // enter freelook spectate mode
            multi_spectate_enter_freelook();
        }
    },
    "Toggles spectate mode (first person or free-look depending on the argument)",
    "spectate [player_name]",
};

static ConsoleCommand2 spectate_mode_minimal_ui_cmd{
    "spectate_minui",
    []() {
        g_alpine_game_config.spectate_mode_minimal_ui = !g_alpine_game_config.spectate_mode_minimal_ui;
        rf::console::print("Spectate mode minimal UI is {}",
                           g_alpine_game_config.spectate_mode_minimal_ui ? "enabled" : "disabled");
    },
    "Toggles spectate mode minimal UI",
};

static ConsoleCommand2 spectate_mode_follow_killer_cmd{
    "spectate_followkiller",
    []() {
        g_spectate_mode_follow_killer = !g_spectate_mode_follow_killer;
        rf::console::printf("Follow killer mode is %s", g_spectate_mode_follow_killer ? "enabled" : "disabled");
    },
    "When a player you're spectating dies, automatically spectate their killer",
};

// gameplay_render_frame checks scanning_for_target at 0x00431CCC for FOV and scanner overlay
// rendering, BEFORE player_render_new (0x0043285D) runs. The game loop clears scanning_for_target
// on local_player each frame (FUN_004ad410) because local_player isn't holding the rail driver.
// This early injection derives scanner state from the spectate target before the first read.
// The rail driver scanner toggle (FUN_004ad560) sets scanning_for_target but NOT zooming_in.
// Entity state flags (FUN_00475930) only pack RF_ES_ZOOMING from zooming_in. This injection
// also sets RF_ES_ZOOMING (bit 0x08) when scanning_for_target is true, so the scanner state
// is synced to other clients via stock entity_update packets.
// Wraps entity state flags sync (FUN_00475930) to handle scanner state on both sides:
// SENDING: includes scanning_for_target as RF_ES_ZOOMING in the flags
// RECEIVING: converts RF_ES_ZOOMING back to scanning_for_target for scanner weapons
static FunHook<void(rf::Entity*, uint8_t*, bool)> entity_state_flags_sync_hook{
    0x00475930,
    [](rf::Entity* entity, uint8_t* flags, bool is_sending) {
        // PRE-CALL (sending side): set scanning_for_target in entity state so it gets packed
        // as RF_ES_ZOOMING by the original function
        rf::Player* player = entity ? rf::player_from_entity_handle(entity->handle) : nullptr;
        bool was_scanning = false;
        if (is_sending && player && player->fpgun_data.scanning_for_target) {
            // Temporarily set zooming_in so the stock code packs RF_ES_ZOOMING
            was_scanning = true;
            player->fpgun_data.zooming_in = true;
        }

        entity_state_flags_sync_hook.call_target(entity, flags, is_sending);

        // POST-CALL (sending side): restore zooming_in
        if (was_scanning && player) {
            player->fpgun_data.zooming_in = false;
        }

        // POST-CALL (receiving side): convert zooming_in to scanning_for_target for scanners
        if (!is_sending && player) {
            if (player->fpgun_data.zooming_in &&
                rf::weapon_has_scanner(entity->ai.current_primary_weapon)) {
                player->fpgun_data.scanning_for_target = true;
                player->fpgun_data.zooming_in = false;
            } else if (!player->fpgun_data.zooming_in) {
                player->fpgun_data.scanning_for_target = false;
            }
        }
    },
};

// gameplay_render_frame checks scanning_for_target at 0x00431CCC for FOV and scanner overlay
// rendering, BEFORE player_render_new (0x0043285D) runs. The game loop clears scanning_for_target
// on local_player each frame (FUN_004ad410) because local_player isn't holding the rail driver.
// This early injection derives scanner state from the spectate target before the first read.
static CodeInjection gameplay_render_frame_early_scanner_sync{
    0x00431CCC,
    []() {
        if (g_spectate_mode_enabled && rf::local_player && g_spectate_mode_target) {
            // The receiving-side injection sets scanning_for_target directly on the target.
            // Copy it to local_player so gameplay_render_frame's scanner overlay code sees it.
            bool scanning = g_spectate_mode_target->fpgun_data.scanning_for_target;
            rf::local_player->fpgun_data.scanning_for_target = scanning;
        }
    },
};

// The scope overlay block at 0x00431D1C-0x00431E4C renders based on EDI (camera scope object),
// regardless of scanning state. When the rail scanner is active, we must suppress the scope
// overlay so it doesn't render on top of (or instead of) the scanner. Force EDI=0 to skip it.
static CodeInjection gameplay_render_frame_skip_scope_when_scanning{
    0x00431D1C,
    [](auto& regs) {
        if (g_spectate_mode_enabled && rf::local_player &&
            rf::local_player->fpgun_data.scanning_for_target) {
            regs.edi = 0;
        }
    },
};

// gameplay_render_frame skips the HUD render (FUN_00437ba0) when scanning_for_target is true.
// Since multi_spectate_render is called from inside that function, the spectate HUD never draws
// when the rail scanner overlay is active. This injection runs right after the skip point and
// draws the spectate HUD on top of the scanner overlay.
static CodeInjection gameplay_render_frame_spectate_hud_over_scanner{
    0x00432A20,
    []() {
        if (g_spectate_mode_enabled && rf::local_player &&
            rf::local_player->fpgun_data.scanning_for_target) {
            multi_spectate_render();
        }
    },
};

#if SPECTATE_MODE_SHOW_WEAPON

// Hook entity_play_attack_anim (0x0042C3C0) — called from the obj_update processing path
// when a remote entity's attack animation bits change. This fires at the correct time for
// thrown projectile weapons (grenade, C4, flamethrower canister), before the projectile
// itself arrives. For non-thrown weapons the existing multi_process_remote_weapon_fire_hook
// path also calls multi_spectate_on_obj_update_fire, but the !is_playing guard prevents
// double-triggering.
FunHook<void(rf::Entity*, bool)> entity_play_attack_anim_spectate_hook{
    0x0042C3C0,
    [](rf::Entity* entity, bool alt_fire) {
        entity_play_attack_anim_spectate_hook.call_target(entity, alt_fire);
        if (!g_spectate_mode_enabled || !g_spectate_mode_target || !entity || rf::is_server)
            return;
        rf::Entity* target = rf::entity_from_handle(g_spectate_mode_target->entity_handle);
        if (entity != target || g_spectate_mode_target == rf::local_player)
            return;
        multi_spectate_on_obj_update_fire(entity, alt_fire);
    },
};

static void player_render_new(rf::Player* player)
{
    if (g_spectate_mode_enabled) {
        rf::Entity* entity = rf::entity_from_handle(g_spectate_mode_target->entity_handle);

        // HACKFIX: RF uses function player_fpgun_set_remote_charge_visible for local player only
        g_spectate_mode_target->fpgun_data.remote_charge_in_hand =
            (entity && entity->ai.current_primary_weapon == rf::remote_charge_weapon_type);

        if (g_spectate_mode_target != rf::local_player && entity) {
            // Clear jump/land flags so player_fpgun_process doesn't play WA_JUMP
            // which cancels action anims (reload, fire, draw, etc.)
            entity->entity_flags &= ~rf::EF_JUMP_START_ANIM;
            g_spectate_mode_target->just_landed = false;

            int weapon_type = entity->ai.current_primary_weapon;
            bool valid_weapon = weapon_type >= 0 && weapon_type < rf::num_weapon_types;

            if (valid_weapon) {
                // Detect weapon switch and play draw animation for the new weapon
                if (weapon_type != g_prev_weapon_type && g_prev_weapon_type != -1) {
                    if (rf::player_fpgun_action_anim_exists(weapon_type, rf::WA_DRAW)) {
                        rf::player_fpgun_play_anim(g_spectate_mode_target, rf::WA_DRAW);
                    }
                }

                // Detect weapon fire rising/falling edge.
                // AIF_ALT_FIRE distinguishes primary vs alt fire on the same weapon_is_on state.
                // For continuous alt fire weapons (baton taser): skip WA_CUSTOM_START intro, go
                // straight to WS_LOOP_FIRE on rising edge, play WA_CUSTOM_LEAVE on falling edge.
                bool weapon_is_on = rf::entity_weapon_is_on(entity->handle, weapon_type);
                bool is_alt_fire = (entity->ai.ai_flags & rf::AIF_ALT_FIRE) != 0;
                bool is_continuous_alt_fire_weapon =
                    rf::weapon_is_on_off_weapon(weapon_type, true);

                if (weapon_is_on && !g_prev_weapon_is_on) {
                    // Rising edge - weapon just started firing
                    if (is_alt_fire && is_continuous_alt_fire_weapon) {
                        // Continuous alt fire (baton taser): skip intro, go straight to looping fire
                        rf::player_fpgun_set_next_state_anim(g_spectate_mode_target, rf::WS_LOOP_FIRE);
                    }
                    else if (is_alt_fire &&
                             rf::player_fpgun_action_anim_exists(weapon_type, rf::WA_ALT_FIRE)) {
                        rf::player_fpgun_play_anim(g_spectate_mode_target, rf::WA_ALT_FIRE);
                    }
                    else if (!is_alt_fire &&
                             rf::player_fpgun_action_anim_exists(weapon_type, rf::WA_FIRE)) {
                        rf::player_fpgun_play_anim(g_spectate_mode_target, rf::WA_FIRE);
                    }
                    // Reset firing timer so muzzle flash renders (used by rail gun glow,
                    // shoulder cannon boom, and other time-based effects in player_fpgun_render)
                    g_spectate_mode_target->fpgun_data.time_elapsed_since_firing = 0.0f;
                }
                else if (!weapon_is_on && g_prev_weapon_is_on) {
                    // Falling edge - weapon stopped firing
                    if (g_prev_alt_fire_is_on && is_continuous_alt_fire_weapon &&
                        rf::player_fpgun_action_anim_exists(weapon_type, rf::WA_CUSTOM_LEAVE)) {
                        rf::player_fpgun_play_anim(g_spectate_mode_target, rf::WA_CUSTOM_LEAVE);
                    }
                }
                g_prev_weapon_is_on = weapon_is_on;
                g_prev_alt_fire_is_on = is_alt_fire;

                // Detect reload rising edge and play fpgun reload action animation
                // Skip if the player has no reserve ammo (empty weapon causes continuous reload flag)
                bool is_reloading = rf::entity_is_reloading(entity);
                if (is_reloading && !g_prev_is_reloading) {
                    int ammo_type = rf::weapon_types[weapon_type].ammo_type;
                    bool has_reserve_ammo = ammo_type >= 0 && entity->ai.ammo[ammo_type] > 0;
                    if (has_reserve_ammo && rf::player_fpgun_action_anim_exists(weapon_type, rf::WA_RELOAD)) {
                        rf::player_fpgun_play_anim(g_spectate_mode_target, rf::WA_RELOAD);
                    }
                }
                g_prev_is_reloading = is_reloading;
            }
            else {
                // Invalid weapon (unarmed) - reset edge-detection state
                g_prev_weapon_is_on = false;
                g_prev_alt_fire_is_on = false;
                g_prev_is_reloading = false;
            }
            g_prev_weapon_type = weapon_type;
        }

        if (g_spectate_mode_target->fpgun_data.zooming_in)
            g_spectate_mode_target->fpgun_data.zoom_factor = 5.0f;
        rf::local_player->fpgun_data.zooming_in = g_spectate_mode_target->fpgun_data.zooming_in;
        rf::local_player->fpgun_data.zoom_factor = g_spectate_mode_target->fpgun_data.zoom_factor;

        // Copy scanner state from target (set by receiving-side entity state flags injection)
        rf::local_player->fpgun_data.scanning_for_target =
            g_spectate_mode_target->fpgun_data.scanning_for_target;

        rf::player_fpgun_process(g_spectate_mode_target);

        // Force WS_LOOP_FIRE state after process so the render function sees it for muzzle flash.
        // The state anim hook inside process should already set this, but the animation transition
        // system may not complete in time for the render check. Directly writing the state fields
        // guarantees player_fpgun_render's is_in_state_anim(WS_LOOP_FIRE) check passes.
        if (entity && rf::entity_weapon_is_on(entity->handle, entity->ai.current_primary_weapon)) {
            g_spectate_mode_target->fpgun_current_state_anim = rf::WS_LOOP_FIRE;
        }

        rf::player_render(g_spectate_mode_target);
    }
    else
        rf::player_render(player);
}

CallHook<float(rf::Player*)> gameplay_render_frame_player_fpgun_get_zoom_hook{
    0x00431B6D,
    [](rf::Player* pp) {
        if (g_spectate_mode_enabled) {
            // Rail driver scanner has its own FOV (set via the scanning_for_target path).
            // Return 0 so the sniper scope overlay doesn't render.
            if (g_spectate_mode_target->fpgun_data.scanning_for_target) {
                return 0.0f;
            }
            return gameplay_render_frame_player_fpgun_get_zoom_hook.call_target(g_spectate_mode_target);
        }
        return gameplay_render_frame_player_fpgun_get_zoom_hook.call_target(pp);
    },
};

// render_to_dynamic_textures (0x00431820) iterates all players and calls player_fpgun_render_for_rail_gun
// for each player with scanning_for_target=true. It runs BEFORE gameplay_render_frame, so our early
// injection there is too late. This hook sets scanning_for_target on local_player before the iteration,
// so the scanner texture gets rendered. Our existing hook on player_fpgun_render_for_rail_gun then
// redirects the render to use the spectate target's viewpoint.
static FunHook<void()> render_to_dynamic_textures_hook{
    0x00431820,
    []() {
        if (g_spectate_mode_enabled && rf::local_player && g_spectate_mode_target) {
            // The receiving-side injection sets scanning_for_target directly on the target.
            // Copy it to local_player so render_to_dynamic_textures renders the scanner texture.
            bool scanning = g_spectate_mode_target->fpgun_data.scanning_for_target;
            rf::local_player->fpgun_data.scanning_for_target = scanning;
        }
        render_to_dynamic_textures_hook.call_target();
    },
};

#endif // SPECTATE_MODE_SHOW_WEAPON

void multi_spectate_on_obj_update_fire(rf::Entity* entity, bool alt_fire)
{
#if SPECTATE_MODE_SHOW_WEAPON
    if (!g_spectate_mode_enabled || !g_spectate_mode_target || !entity)
        return;

    rf::Entity* target_entity = rf::entity_from_handle(g_spectate_mode_target->entity_handle);
    if (entity != target_entity)
        return;

    if (g_spectate_mode_target == rf::local_player)
        return;

    int weapon_type = entity->ai.current_primary_weapon;
    if (weapon_type < 0 || weapon_type >= rf::num_weapon_types)
        return;

    // Continuous alt fire weapons (baton taser): skip intro, go straight to looping fire
    if (alt_fire && rf::weapon_is_on_off_weapon(weapon_type, true)) {
        rf::player_fpgun_set_next_state_anim(g_spectate_mode_target, rf::WS_LOOP_FIRE);
    }
    else {
        rf::WeaponAction action = alt_fire ? rf::WA_ALT_FIRE : rf::WA_FIRE;
        if (rf::player_fpgun_action_anim_exists(weapon_type, action)) {
            bool should_play = rf::weapon_is_semi_automatic(weapon_type)
                || !rf::player_fpgun_action_anim_is_playing(g_spectate_mode_target, action);
            if (should_play) {
                rf::player_fpgun_play_anim(g_spectate_mode_target, action);
            }
        }
    }

    g_spectate_mode_target->fpgun_data.time_elapsed_since_firing = 0.0f;
#endif
}

void multi_spectate_appy_patch()
{
    render_reticle_hook.install();
    hud_weapons_render_hook.install();
    hud_status_render_spectate_hook.install();
    spectate_entity_set_next_state_anim_hook.install();

    spectate_cmd.register_cmd();
    spectate_mode_minimal_ui_cmd.register_cmd();
    spectate_mode_follow_killer_cmd.register_cmd();

    // Handle scanner state in entity state flags (both sending and receiving)
    entity_state_flags_sync_hook.install();

    // Sync scanner state early in gameplay_render_frame before the first scanning_for_target check
    gameplay_render_frame_early_scanner_sync.install();

    // Suppress scope overlay when rail scanner is active in spectate
    gameplay_render_frame_skip_scope_when_scanning.install();

    // Draw spectate HUD over rail scanner overlay (scanner skips the normal HUD render path)
    gameplay_render_frame_spectate_hud_over_scanner.install();

#if SPECTATE_MODE_SHOW_WEAPON

    AsmWriter(0x0043285D).call(player_render_new);
    gameplay_render_frame_player_fpgun_get_zoom_hook.install();
    entity_play_attack_anim_spectate_hook.install();
    render_to_dynamic_textures_hook.install();

    write_mem_ptr(0x0048857E + 2, &g_spectate_mode_target); // obj_mark_all_for_room
    write_mem_ptr(0x00488598 + 1, &g_spectate_mode_target); // obj_mark_all_for_room
    write_mem_ptr(0x00421889 + 2, &g_spectate_mode_target); // entity_render
    write_mem_ptr(0x004218A2 + 2, &g_spectate_mode_target); // entity_render
    write_mem_ptr(0x00458FB0 + 2, &g_spectate_mode_target); // item_render
    write_mem_ptr(0x00458FDF + 2, &g_spectate_mode_target); // item_render

    // Note: additional patches are in player_fpgun.cpp
#endif // SPECTATE_MODE_SHOW_WEAPON
}

void multi_spectate_after_full_game_init()
{
    g_spectate_mode_target = rf::local_player;
    player_fpgun_set_player(rf::local_player);
}

void multi_spectate_player_create_entity_post(rf::Player* player, rf::Entity* entity)
{
    // hide target player from camera after respawn
    if (g_spectate_mode_enabled && player == g_spectate_mode_target) {
        entity->local_player = player;
        // When entering limbo state the game changes camera mode to fixed
        // Make sure we are in first person mode when target entity spawns
        rf::Camera* cam = rf::local_player->cam;
        if (cam->mode != rf::CAMERA_FIRST_PERSON) {
            rf::camera_enter_first_person(cam);
        }
    }
    // Do not allow spectating in Force Respawn game after spawning for the first time
    if (player == rf::local_player) {
        g_spawned_in_current_level = true;
    }
}

void multi_spectate_level_init()
{
    g_spawned_in_current_level = false;
}

template<typename F>
static void draw_with_shadow(int x, int y, int shadow_dx, int shadow_dy, rf::Color clr, rf::Color shadow_clr, F fun)
{
    rf::gr::set_color(shadow_clr);
    fun(x + shadow_dx, y + shadow_dy);
    rf::gr::set_color(clr);
    fun(x, y);
}

void multi_spectate_render() {
    if (rf::hud_disabled
        || rf::gameseq_get_state() != rf::GS_GAMEPLAY
        || multi_spectate_is_freelook())
    {
        return;
    }

    int large_font = hud_get_large_font();
    int large_font_h = rf::gr::get_font_height(large_font);
    int medium_font = hud_get_default_font();
    int medium_font_h = rf::gr::get_font_height(medium_font);

    int scr_w = rf::gr::screen_width();
    int scr_h = rf::gr::screen_height();

    if (!g_spectate_mode_enabled) {
        if (rf::player_is_dead(rf::local_player)
            && !g_remote_server_cfg_popup.is_active()) {
            rf::gr::set_color(0xFF, 0xFF, 0xFF, 0xC0);
            const int bottom_death_bar_y = rf::gr::screen_height()
                - static_cast<int>(rf::gr::screen_height() * .125f);
            const int y = bottom_death_bar_y - rf::gr::get_font_height(medium_font) - 5;
            rf::gr::string(10, y, "Press Jump to enter Spectate Mode", medium_font);
        }
        return;
    }

    rf::Color white_clr{255, 255, 255, 255};
    rf::Color shadow_clr{0, 0, 0, 128};

    if (!g_alpine_game_config.spectate_mode_minimal_ui) {
        int title_x = scr_w / 2;
        int title_y = g_alpine_game_config.big_hud ? 250 : 150;
        draw_with_shadow(
            title_x,
            title_y,
            2,
            2,
            white_clr,
            shadow_clr,
            [=] (int x, int y) {
                rf::gr::string_aligned(
                    rf::gr::ALIGN_CENTER,
                    x,
                    y,
                    "SPECTATE MODE",
                    large_font
                );
            }
        );

        if (!g_remote_server_cfg_popup.is_active()) {
            int hints_y = scr_h - (g_alpine_game_config.big_hud ? 200 : 120);
            int hints_left_x = g_alpine_game_config.big_hud ? 120 : 70;
            int hints_right_x = g_alpine_game_config.big_hud ? 140 : 80;
            std::string next_player_text =
                get_action_bind_name(rf::ControlConfigAction::CC_ACTION_PRIMARY_ATTACK);
            std::string prev_player_text = get_action_bind_name(
                rf::ControlConfigAction::CC_ACTION_SECONDARY_ATTACK
            );
            std::string exit_spec_text =
                get_action_bind_name(rf::ControlConfigAction::CC_ACTION_JUMP);
            std::string spec_menu_text = get_action_bind_name(
                get_af_control(rf::AlpineControlConfigAction::AF_ACTION_SPECTATE_MENU)
            );
            const char* hints[][3] = {
                {next_player_text.c_str(), "Next Player"},
                {prev_player_text.c_str(), "Previous Player"},
                {spec_menu_text.c_str(), "Open Spectate Options Menu"},
                {exit_spec_text.c_str(), "Exit Spectate Mode"},
            };
            for (auto& hint : hints) {
                rf::gr::set_color(0xFF, 0xFF, 0xFF, 0xC0);
                rf::gr::string_aligned(
                    rf::gr::ALIGN_RIGHT,
                    hints_left_x,
                    hints_y,
                    hint[0],
                    medium_font
                );
                rf::gr::set_color(0xFF, 0xFF, 0xFF, 0x80);
                rf::gr::string(hints_right_x, hints_y, hint[1], medium_font);
                hints_y += medium_font_h;
            }
        }
    }

    int small_font = hud_get_small_font();
    int small_font_h = rf::gr::get_font_height(small_font);

    const char* target_name = g_spectate_mode_target->name;
    const char* spectating_label = "Spectating:";
    const auto [spectating_label_w, spectating_label_h] =
        rf::gr::get_string_size(spectating_label, small_font);
    auto [target_name_w, target_name_h] =
        rf::gr::get_string_size(target_name, large_font);
    const auto [bot_w, bot_h] = rf::gr::get_string_size(" bot", large_font);
    const bool is_bot = g_spectate_mode_target->is_bot;
    if (is_bot) {
        target_name_w += bot_w;
    }

    const int padding_y = g_alpine_game_config.big_hud ? 4 : 2;
    const int padding_x = g_alpine_game_config.big_hud ? 24 : 16;
    const int line_gap = g_alpine_game_config.big_hud ? 2 : 1;
    const int bar_h = spectating_label_h + target_name_h + (padding_y * 2) + line_gap;

    int max_bar_w = scr_w - 20;
    int content_w = std::max(target_name_w, spectating_label_w);
    int bar_w = std::min(content_w + padding_x * 2, max_bar_w);

    int bar_x = (scr_w - bar_w) / 2;
    int bar_y = scr_h - (g_alpine_game_config.big_hud ? 15 : 10) - bar_h;
    rf::gr::set_color(0, 0, 0x00, 150);
    rf::gr::rect(bar_x, bar_y, bar_w, bar_h);

    const int label_y = bar_y + padding_y;
    const int name_y = label_y + spectating_label_h + line_gap;

    rf::gr::set_color(0xFF, 0xFF, 0xFF, 0xFF);
    rf::gr::string_aligned(
        rf::gr::ALIGN_CENTER,
        bar_x + bar_w / 2,
        label_y,
        spectating_label,
        small_font
    );
    if (multi_is_team_game_type()) {
        if (g_spectate_mode_target->team) {
            rf::gr::set_color(0x34, 0x4E, 0xA7, 0xFF);
        }
        else {
            rf::gr::set_color(0xA7, 0x00, 0x00, 0xFF);
        }
    }
    else {
        rf::gr::set_color(0xFF, 0x88, 0x22, 0xFF);
    }
    const int x = target_name_w / -2 + (bar_x + bar_w / 2);
    rf::gr::string_aligned(
        rf::gr::ALIGN_LEFT,
        x,
        name_y,
        target_name,
        large_font
    );
    if (is_bot) {
        const rf::gr::Color saved_color = rf::gr::screen.current_color;
        rf::gr::set_color(255, 250, 205, 255);
        rf::gr::string(rf::gr::current_string_x, name_y, " bot", large_font);
        rf::gr::set_color(saved_color);
    }

    rf::Entity* entity = rf::entity_from_handle(g_spectate_mode_target->entity_handle);
    if (!entity) {
        rf::gr::set_color(0xFF, 0xFF, 0xFF, 0xFF);
        static int blood_bm = rf::bm::load("bloodsmear07_A.tga", -1, true);
        int blood_w, blood_h;
        rf::bm::get_dimensions(blood_bm, &blood_w, &blood_h);
        rf::gr::bitmap_scaled(
            blood_bm,
            (scr_w - blood_w * 2) / 2,
            (scr_h - blood_h * 2) / 2,
            blood_w * 2,
            blood_h * 2,
            0,
            0,
            blood_w,
            blood_h,
            false,
            false,
            rf::gr::bitmap_clamp_mode
        );

        rf::Color dead_clr{0xF0, 0x20, 0x10, 0xC0};
        draw_with_shadow(
            scr_w / 2,
            scr_h / 2,
            2,
            2,
            dead_clr,
            shadow_clr,
            [=] (int x, int y) {
                rf::gr::string_aligned(rf::gr::ALIGN_CENTER, x, y, "DEAD", large_font);
            }
        );
    }
}
