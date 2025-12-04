#include <patch_common/AsmWriter.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/FunHook.h>
#include <patch_common/CallHook.h>
#include "../main/main.h"
#include "../rf/multi.h"
#include "../os/console.h"
#include "../input/input.h"
#include "../misc/misc.h"
#include "../misc/alpine_settings.h"
#include "../misc/alpine_options.h"
#include "../multi/multi.h"
#include "../rf/player/player.h"
#include "../rf/player/camera.h"
#include "../rf/player/control_config.h"
#include "../rf/os/frametime.h"

constexpr auto screen_shake_fps = 150.0f;
static float g_camera_shake_factor = 0.6f;
static float freelook_cam_base_accel = 10.0f; // set on freelook camera entity creation
static float freelook_cam_accel_scale = 1.0f;

constexpr float freelook_accel_min_scale = 0.25f;
constexpr float freelook_accel_max_scale = 20.0f;
constexpr float freelook_accel_scroll_step = 0.25f;

bool server_side_restrict_disable_ss = false;

FunHook<void(rf::Camera*)> camera_update_shake_hook{
    0x0040DB70,
    [](rf::Camera *camera) {
        float frame_time = rf::frametime;
        if (frame_time > 0.0001f) { // < 1000FPS
            // Fix screen shake caused by some weapons (eg. Assault Rifle)
            g_camera_shake_factor = std::pow(0.6f, frame_time / (1 / screen_shake_fps));
        }

        camera_update_shake_hook.call_target(camera);
    },
};

// camera shake for FP weapons
CallHook<void(rf::Camera*, float, float)> camera_shake_hook{
    0x00426C79,
    [](rf::Camera* cp, float amplitude, float time_seconds) {

        if (g_alpine_game_config.try_disable_weapon_shake && !server_side_restrict_disable_ss) {
            return;
        }

        camera_shake_hook.call_target(cp, amplitude, time_seconds);
    }
};

// camera shake for everything
FunHook<void(rf::Camera*, float, float)> camera_shake_global_hook{
    0x0040E0B0,
    [](rf::Camera* cp, float amplitude, float time_seconds) {

        if (g_alpine_game_config.screen_shake_force_off && !rf::is_multi) {
            return;
        }

        camera_shake_global_hook.call_target(cp, amplitude, time_seconds);
    }
};

void evaluate_restrict_disable_ss()
{
    server_side_restrict_disable_ss =
        rf::is_multi && !rf::is_server && get_df_server_info() && !get_df_server_info()->allow_no_ss;

    if (server_side_restrict_disable_ss) {
        if (g_alpine_game_config.try_disable_weapon_shake) {
            rf::console::print("This server does not allow you to disable weapon camera shake!");
        }
    }
}

CallHook<void(rf::Camera*)> camera_enter_first_person_level_post{
    {
        0x0040D727,
        0x004A43AB
    },
    [](rf::Camera* camera) {
        const bool default_third_person =
            g_alpine_options_config.is_option_loaded(AlpineOptionID::DefaultThirdPerson) &&
            std::get<bool>(g_alpine_options_config.options.at(AlpineOptionID::DefaultThirdPerson));

        if ((!rf::is_multi && camera->mode == rf::CameraMode::CAMERA_THIRD_PERSON) || default_third_person) {
            rf::camera_enter_third_person(camera);
        }
        else {
            rf::camera_enter_first_person(camera);
        }
    }
};

CallHook<rf::CameraMode(rf::Camera*)> camera_get_mode_emit_sound_hook{
    {
        0x0048A967, // object_emit_sound
        0x0048A9DF  // object_emit_sound2
    },
    [](rf::Camera* camera) {
        if (camera)
            return camera_get_mode_emit_sound_hook.call_target(camera);
        else // prevent a crash if the camera is momentarily invalid in FP spectate mode
            return rf::CameraMode::CAMERA_FIRST_PERSON;
    }
};

ConsoleCommand2 disable_weaphake_cmd{
    "cl_weapshake",
    []() {
        g_alpine_game_config.try_disable_weapon_shake = !g_alpine_game_config.try_disable_weapon_shake;

        evaluate_restrict_disable_ss();

        rf::console::print("Camera shake from weapon fire is {}",
                           g_alpine_game_config.try_disable_weapon_shake
                               ? "disabled. In multiplayer, this will only apply if the server allows it."
                               : "enabled.");
    },
    "Disable camera shake from weapon firing. In multiplayer, this is only applied if the server allows it.",
};

ConsoleCommand2 force_disable_camerashake_cmd{
    "sp_camerashake",
    []() {
        g_alpine_game_config.screen_shake_force_off = !g_alpine_game_config.screen_shake_force_off;

        rf::console::print("All instances of camera shake are {}being forcefully turned off in single player.",
                           g_alpine_game_config.screen_shake_force_off
                               ? ""
                               : "NOT ");
    },
    "Forcefully disable all forms of camera shake. Only applies to single player.",
};

CodeInjection footsteps_get_camera_mode_patch{
    0x0048A967,
    [](auto& regs) {
        rf::Camera* cp = regs.ecx;
        if (!cp) {
            regs.eax = 0; // use 2d footsteps
            regs.eip = 0x0048A96C;
        }
    },
};

CodeInjection camera_create_for_player_freelook_camera_patch{
    0x0040D64B,
    [](auto& regs) {
        rf::Entity* camera_entity = regs.esi;
        if (camera_entity) {
            // store the base accel value for freelook camera, ensures compatibility with
            // mods that change the acceleration value of the "Freelook camera" entity
            freelook_cam_base_accel = camera_entity->info->acceleration;
            freelook_cam_accel_scale = 1.0f;
        }
    },
};

CodeInjection free_camera_do_frame_patch{
    0x0040D9CC,
    [](auto& regs) {
        rf::Camera* cp = regs.ebx;
        if (cp && cp->mode == rf::CameraMode::CAMERA_FREELOOK) {
            if (auto* cep = cp->camera_entity) {
                if (!cep || !cep->info)
                    return;

                if (!rf::is_multi) {
                    cep->info->acceleration = freelook_cam_base_accel; // `camera2` in SP ignores accel scale
                    return;
                }
                else {
                    auto* player = rf::local_player;
                    int mouse_dz = get_mouse_scroll_wheel_value();

                    if (mouse_dz != 0) {
                        // normalize at 120.0 units per scroll notch
                        const float scroll_notches = static_cast<float>(mouse_dz) / 120.0f;
                        freelook_cam_accel_scale += freelook_accel_scroll_step * scroll_notches;
                        freelook_cam_accel_scale = std::clamp(
                            freelook_cam_accel_scale, freelook_accel_min_scale, freelook_accel_max_scale);
                    }

                    if (rf::control_is_control_down(&player->settings.controls,
                        get_af_control(rf::AlpineControlConfigAction::AF_ACTION_PING_LOCATION))) {
                        freelook_cam_accel_scale = 1.0f;
                    }

                    cep->info->acceleration = freelook_cam_base_accel * freelook_cam_accel_scale;
                }
            }
        }
    },
};

void camera_do_patch()
{
    // Fix crash when executing camera2 command in main menu
    AsmWriter(0x0040DCFC).nop(5);

    // Maintain third person camera mode if set
    camera_enter_first_person_level_post.install();

    // Prevent a rare crash when using FP spectate
    camera_get_mode_emit_sound_hook.install();

    // Fix screen shake caused by some weapons (eg. Assault Rifle)
    write_mem_ptr(0x0040DBCC + 2, &g_camera_shake_factor);

    // Fix crash when camera is invalid in footstep emit sound function
    footsteps_get_camera_mode_patch.install();

    // Freelook camera accel and modifier
    camera_create_for_player_freelook_camera_patch.install();
    free_camera_do_frame_patch.install();

    // handle turning off screen shake
    disable_weaphake_cmd.register_cmd();
    camera_shake_hook.install();
    force_disable_camerashake_cmd.register_cmd();
    camera_shake_global_hook.install();
}
