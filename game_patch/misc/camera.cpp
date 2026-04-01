#include <cassert>
#include <algorithm>
#include <cmath>
#include <xlog/xlog.h>
#include <patch_common/AsmWriter.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/FunHook.h>
#include <patch_common/CallHook.h>
#include "../main/main.h"
#include "../rf/multi.h"
#include "../os/console.h"
#include "../input/input.h"
#include "../input/gamepad.h"
#include "../input/mouse.h"
#include "../rf/entity.h"
#include "../misc/misc.h"
#include "../misc/alpine_settings.h"
#include "../misc/alpine_options.h"
#include "../multi/multi.h"
#include "../rf/player/player.h"
#include "../rf/player/camera.h"
#include "../rf/player/control_config.h"
#include "../rf/os/frametime.h"
#include "player.h"
#include "../hud/multi_spectate.h"

constexpr auto screen_shake_fps = 150.0f;
static float g_camera_shake_factor = 0.6f;
static float freelook_cam_base_accel = 10.0f; // set on freelook camera entity creation
static float freelook_cam_accel_scale = 1.0f;

constexpr float freelook_accel_min_scale = 0.25f;
constexpr float freelook_accel_max_scale = 20.0f;
constexpr float freelook_accel_scroll_step = 0.25f;

bool server_side_restrict_disable_ss = false;

static bool s_camera_resetting = false;
static bool s_camera_reset_prev_down = false;
static float s_camera_reset_start_pitch = 0.0f;
static float s_camera_reset_elapsed = 0.0f;

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
        rf::is_multi && !rf::is_server && get_af_server_info() && !get_af_server_info()->allow_no_ss;

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
                    flush_freelook_gamepad_deltas();
                    return;
                }
                else {
                    auto* player = rf::local_player;
                    const int mouse_dz = rf::mouse_dz;

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
                    flush_freelook_gamepad_deltas();
                }
            }
        }
    },
};

// In the freelook camera control processing, crouch moves the camera down because it has
// press_mode 1 (hold). Jump has press_mode 0 (single press) so it only fires for one frame
// and has no visible effect. This patch runs after freelook controls are processed and adds
// jump-to-up vertical movement by checking if the jump key is held down.
CodeInjection freelook_camera_jump_vertical_patch{
    0x004A609C,
    [] (auto& regs) {
        rf::Player* player = regs.edi;
        const bool jumped =
            rf::control_is_control_down(&player->settings.controls, rf::CC_ACTION_JUMP);
        if (jumped) {
            player->cam->camera_entity->ai.ci.move.y += 1.f;
        }
    },
};

CodeInjection multi_get_state_info_camera_enter_fixed_patch{
    0x0048201F,
    [] (auto& regs) {
        rf::Entity* const camera_entity = rf::local_player->cam->camera_entity;
        // Our camera is set to a respawn point, but `CAMERA_FIXED_VIEW`
        // overwrites it.
        float pitch = .0f, yaw = .0f, roll = .0f;
        camera_entity->p_data.orient.extract_angles(
            &pitch,
            &roll,
            &yaw
        );
        camera_entity->control_data.phb.set(.0f, yaw, .0f);
        camera_entity->control_data.eye_phb.set(.0f, .0f, .0f);

        // We should reset velocity.
        camera_entity->p_data.vel.set(.0f, .0f, .0f);

        // Re-enter freelook spectate mode.
        const rf::Player* const target_player = multi_spectate_get_target_player();
        if (rf::local_player->is_spectator && target_player == rf::local_player) {
            rf::camera_enter_freelook(rf::local_player->cam);
            regs.eip = 0x00482024;
        }
    }
};

// Math helpers for converting between non-linear (RF engine) and linear pitch representations
static rf::Vector3 fw_vector_from_non_linear_yaw_pitch(float yaw, float pitch)
{
    // Based on RF code
    rf::Vector3 fvec0;
    fvec0.y = std::sin(pitch);
    float factor = 1.0f - std::abs(fvec0.y);
    fvec0.x = factor * std::sin(yaw);
    fvec0.z = factor * std::cos(yaw);

    rf::Vector3 fvec = fvec0;
    fvec.normalize(); // vector is never zero

    return fvec;
}

static float linear_pitch_from_forward_vector(const rf::Vector3& fvec)
{
    return std::asin(fvec.y);
}

static rf::Vector3 fw_vector_from_linear_yaw_pitch(float yaw, float pitch)
{
    rf::Vector3 fvec;
    fvec.y = std::sin(pitch);
    fvec.x = std::cos(pitch) * std::sin(yaw);
    fvec.z = std::cos(pitch) * std::cos(yaw);
    fvec.normalize();
    return fvec;
}

static float non_linear_pitch_from_fw_vector(rf::Vector3 fvec)
{
    float yaw = std::atan2(fvec.x, fvec.z);
    assert(!std::isnan(yaw));
    float fvec_y_2 = fvec.y * fvec.y;
    float y_sin = std::sin(yaw);
    float y_cos = std::cos(yaw);
    float y_sin_2 = y_sin * y_sin;
    float y_cos_2 = y_cos * y_cos;
    float p_sgn = std::signbit(fvec.y) ? -1.f : 1.f;
    if (fvec.y == 0.0f) {
        return 0.0f;
    }

    float a = 1.f / fvec_y_2 - y_sin_2 - 1.f - y_cos_2;
    float b = 2.f * p_sgn * y_sin_2 + 2.f * p_sgn * y_cos_2;
    float c = -y_sin_2 - y_cos_2;
    float delta = b * b - 4.f * a * c;
    // Note: delta is sometimes slightly below 0 - most probably because of precision error
    // To avoid NaN value delta is changed to 0 in that case
    float delta_sqrt = std::sqrt(std::max(delta, 0.0f));
    assert(!std::isnan(delta_sqrt));

    if (a == 0.0f) {
        return 0.0f;
    }

    float p_sin_1 = (-b - delta_sqrt) / (2.f * a);
    float p_sin_2 = (-b + delta_sqrt) / (2.f * a);

    float result;
    if (std::abs(p_sin_1) < std::abs(p_sin_2))
        result = std::asin(p_sin_1);
    else
        result = std::asin(p_sin_2);
    assert(!std::isnan(result));
    return result;
}

#ifdef DEBUG
static void linear_pitch_test()
{
    float yaw = 3.141592f / 4.0f;
    float pitch = 3.141592f / 4.0f;
    rf::Vector3 fvec = fw_vector_from_non_linear_yaw_pitch(yaw, pitch);
    float lin_pitch = linear_pitch_from_forward_vector(fvec);
    rf::Vector3 fvec2 = fw_vector_from_linear_yaw_pitch(yaw, lin_pitch);
    float pitch2 = non_linear_pitch_from_fw_vector(fvec2);
    assert(std::abs(pitch - pitch2) < 0.00001);
}
#endif // DEBUG

static float convert_pitch_delta_to_non_linear_space(
    const float current_yaw,
    const float current_pitch_non_lin,
    const float pitch_delta,
    const float yaw_delta
) {
    // Convert to linear space. See `physics_make_orient`.
    const rf::Vector3 fvec =
        fw_vector_from_non_linear_yaw_pitch(current_yaw, current_pitch_non_lin);
    const float current_pitch_lin = linear_pitch_from_forward_vector(fvec);

    // Calculate in linear space.
    constexpr float HALF_PI = 1.5707964f;
    const float new_pitch_lin =
        std::clamp(current_pitch_lin + pitch_delta, -HALF_PI, HALF_PI);
    const float new_yaw = current_yaw + yaw_delta;

    // Convert back to non-linear space.
    const rf::Vector3 fvec_new =
        fw_vector_from_linear_yaw_pitch(new_yaw, new_pitch_lin);
    const float new_pitch_non_lin = non_linear_pitch_from_fw_vector(fvec_new);

    // Update non-linear pitch delta.
    const float new_pitch_delta = new_pitch_non_lin - current_pitch_non_lin;
    xlog::trace(
        "non-lin {} lin {} delta {} new {}",
        current_pitch_non_lin,
        current_pitch_lin,
        pitch_delta,
        new_pitch_delta
    );

    return new_pitch_delta;
}

static void apply_camera_reset_to_horizon(rf::Entity* entity, float& pitch_delta)
{
    // Track button state every frame so a held button doesn't re-arm after completion.
    if (rf::local_player) {
        const auto action = get_af_control(rf::AlpineControlConfigAction::AF_ACTION_CENTER_VIEW);
        const bool down = rf::control_is_control_down(&rf::local_player->settings.controls, action);

        if (!s_camera_resetting && down && !s_camera_reset_prev_down) {
            // Rising edge: capture the starting pitch and begin the animation.
            s_camera_resetting = true;
            s_camera_reset_start_pitch = entity->control_data.eye_phb.x;
            s_camera_reset_elapsed = 0.0f;
        }

        s_camera_reset_prev_down = down;
    }

    if (!s_camera_resetting)
        return;

    // Advance time and compute a smoothstep [0,1] parameter.
    // Smoothstep has zero first-derivative at both endpoints, giving a natural ease-in/out feel.
    constexpr float duration = 0.3f;
    s_camera_reset_elapsed += rf::frametime;
    const float t = std::min(s_camera_reset_elapsed / duration, 1.0f);
    const float t_smooth = t * t * (3.0f - 2.0f * t);
    const float target_pitch = s_camera_reset_start_pitch * (1.0f - t_smooth);
    pitch_delta = target_pitch - entity->control_data.eye_phb.x;

    if (t >= 1.0f)
        s_camera_resetting = false;
}

// Applies raw/modern mouse deltas and linear pitch correction at the entity
// control injection point. For the player entity, this is where accumulated raw
// mouse deltas are consumed (freelook camera deltas are consumed earlier in
// mouse_get_delta_hook since the freelook camera has a separate control path).
CodeInjection linear_pitch_patch{
    0x0049DEC9,
    [](auto& regs) {
        rf::Entity* entity = regs.esi;
        float& pitch_delta = addr_as_ref<float>(regs.esp + 0x44 - 0x34);
        float& yaw_delta = addr_as_ref<float>(regs.esp + 0x44 + 0x4);

        if (entity && rf::entity_is_dying(entity)) {
            return;
        }

        // Consume accumulated raw mouse deltas (Raw/Modern mode only).
        // In Classic mode the accumulators are empty and this is a no-op.
        if (g_alpine_game_config.mouse_scale != 0) {
            float mouse_pitch = 0.0f, mouse_yaw = 0.0f;
            consume_raw_mouse_deltas(mouse_pitch, mouse_yaw, true);
            pitch_delta += mouse_pitch;
            yaw_delta += mouse_yaw;
        }

        // Add gamepad rotation deltas to the game's computed deltas
        float gamepad_pitch = 0.0f, gamepad_yaw = 0.0f;
        consume_raw_gamepad_deltas(gamepad_pitch, gamepad_yaw);
        pitch_delta += gamepad_pitch;
        yaw_delta += gamepad_yaw;

        // Apply linear pitch correction to combined delta
        if (g_alpine_game_config.mouse_linear_pitch && pitch_delta != 0.0f) {
            const float current_yaw = entity->control_data.phb.y;
            const float current_pitch_non_lin = entity->control_data.eye_phb.x;
            pitch_delta = convert_pitch_delta_to_non_linear_space(
                current_yaw,
                current_pitch_non_lin,
                pitch_delta,
                yaw_delta
            );
        }
        
        // Apply camera reset to horizon if Center View action is pressed
        apply_camera_reset_to_horizon(entity, pitch_delta);
    },
};

void camera_start_reset_to_horizon()
{
    s_camera_resetting = true;
}

ConsoleCommand2 linear_pitch_cmd{
    "cl_linearpitch",
    []() {
#ifdef DEBUG
        linear_pitch_test();
#endif
        g_alpine_game_config.mouse_linear_pitch = !g_alpine_game_config.mouse_linear_pitch;
        rf::console::print("Linear pitch is {}", g_alpine_game_config.mouse_linear_pitch ? "enabled" : "disabled");
    },
    "Toggles mouse linear pitch angle",
};

void camera_do_patch()
{
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

    // Allow jump button to move freelook camera up vertically
    freelook_camera_jump_vertical_patch.install();

    // handle turning off screen shake
    disable_weaphake_cmd.register_cmd();
    camera_shake_hook.install();
    force_disable_camerashake_cmd.register_cmd();
    camera_shake_global_hook.install();

    // Improve freelook spectate logic after level transition.
    multi_get_state_info_camera_enter_fixed_patch.install();

    // linear pitch correction
    linear_pitch_patch.install();
    linear_pitch_cmd.register_cmd();
}
