#include <cmath>
#include <patch_common/FunHook.h>
#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/MemUtils.h>
#include <xlog/xlog.h>
#include "side_scroller.h"
#include "level.h"
#include "../rf/player/player.h"
#include "../rf/player/camera.h"
#include "../rf/entity.h"
#include "../rf/input.h"
#include "../rf/gr/gr.h"
#include "../rf/os/frametime.h"
#include "../rf/vmesh.h"
#include "../rf/weapon.h"

// Side-scroller camera orientation (fixed):
// Camera is at player.x + offset, looking in -X direction
// rvec = (0, 0, 1)  -> screen right is world +Z
// uvec = (0, 1, 0)  -> screen up is world +Y
// fvec = (-1, 0, 0) -> camera looks in world -X
static constexpr float camera_offset_x = 12.0f;

static const rf::Matrix3 g_side_scroller_orient = {
    {0.0f, 0.0f, 1.0f},   // rvec: right is +Z
    {0.0f, 1.0f, 0.0f},   // uvec: up is +Y
    {-1.0f, 0.0f, 0.0f},  // fvec: forward is -X
};

// Cached side-scroller camera position (updated each frame, read by getter hooks)
static rf::Vector3 g_ss_camera_pos = {0.0f, 0.0f, 0.0f};
static bool g_ss_camera_active = false;

// Reticle position in screen pixels (from top-left)
static float g_reticle_x = 0.0f;
static float g_reticle_y = 0.0f;
static bool g_reticle_initialized = false;

// Mouse deltas captured before stock processing
static int g_stolen_mouse_dx = 0;
static int g_stolen_mouse_dy = 0;

// Raw mouse delta globals (mouse_get_delta only copies, doesn't clear)
static auto& g_mouse_dx = addr_as_ref<int>(0x01885464);
static auto& g_mouse_dy = addr_as_ref<int>(0x01885468);

// True when reticle is behind the player (left of screen center), flips body to face -Z
static bool g_ss_aiming_backward = false;

// X-axis lock: player is always locked to X=0 in side-scroller maps
static constexpr float g_ss_locked_x = 0.0f;

// Debug: aim line from fire position along aim direction (red = apply_side_scroller_aim)
static rf::Vector3 g_dbg_aim_start = {0, 0, 0};
static rf::Vector3 g_dbg_aim_end = {0, 0, 0};
static bool g_dbg_aim_valid = false;

// Debug: fire line from actual fire pos hook output (green = entity_calc_fire_pos_hook)
static rf::Vector3 g_dbg_fire_start = {0, 0, 0};
static rf::Vector3 g_dbg_fire_end = {0, 0, 0};
static bool g_dbg_fire_valid = false;

// Cached hand position for debug line (updated in fire pos hook, read in aim function)
static rf::Vector3 g_ss_hand_pos = {0, 0, 0};
static bool g_ss_hand_pos_valid = false;

// Cached aim orient: computed from reticle each frame, used by fire pos hook.
// Stored separately because stock player_do_frame may overwrite entity->eye_orient
// between our pre-hook (where we set it) and weapon firing (where the hook reads it).
static rf::Matrix3 g_ss_aim_orient = {};
static bool g_ss_aim_orient_valid = false;

// Actual rendering FOV in degrees, captured from gr_setup_3d each frame.
// The rendering pipeline applies Hor+ scaling and user FOV settings, so this may differ
// from player->viewport.fov_h. We must use the same FOV for reticle unprojection.
static float g_ss_render_fov_h_deg = 90.0f;

// Side-scroller occlusion: dithered transparency for geometry between camera and player
static SsOcclusionParams g_ss_occlusion = {};
static float g_ss_occlusion_fade_current = 0.0f; // smoothly ramps toward target

bool is_side_scroller_mode()
{
    return AlpineLevelProperties::instance().game_style == 1;
}

const SsOcclusionParams& get_ss_occlusion_params()
{
    return g_ss_occlusion;
}

void side_scroller_on_level_load()
{
    g_reticle_initialized = false;
    g_reticle_x = 0.0f;
    g_reticle_y = 0.0f;
    g_stolen_mouse_dx = 0;
    g_stolen_mouse_dy = 0;
    g_ss_camera_active = false;
    g_ss_aiming_backward = false;
    g_ss_hand_pos_valid = false;
    g_ss_aim_orient_valid = false;
    g_ss_occlusion = {};
    g_ss_occlusion_fade_current = 0.0f;
}

bool side_scroller_get_reticle_offset(int& offset_x, int& offset_y)
{
    if (!is_side_scroller_mode()) {
        return false;
    }
    int half_w = rf::gr::clip_width() / 2;
    int half_h = rf::gr::clip_height() / 2;
    offset_x = static_cast<int>(g_reticle_x) - half_w;
    offset_y = static_cast<int>(g_reticle_y) - half_h;
    return true;
}

static void steal_mouse_deltas()
{
    // Read mouse deltas, then zero the globals so the stock code can't use them
    // for camera rotation. mouse_get_delta only copies, it doesn't clear.
    int dz = 0;
    rf::mouse_get_delta(g_stolen_mouse_dx, g_stolen_mouse_dy, dz);
    g_mouse_dx = 0;
    g_mouse_dy = 0;
}

static void update_reticle_from_stolen_mouse()
{
    int screen_w = rf::gr::clip_width();
    int screen_h = rf::gr::clip_height();

    if (!g_reticle_initialized) {
        g_reticle_x = static_cast<float>(screen_w) / 2.0f;
        g_reticle_y = static_cast<float>(screen_h) / 2.0f;
        g_reticle_initialized = true;
    }

    // Apply mouse sensitivity from player controls
    float sensitivity = 1.0f;
    if (rf::local_player) {
        sensitivity = rf::local_player->settings.controls.mouse_sensitivity;
    }

    g_reticle_x += static_cast<float>(g_stolen_mouse_dx) * sensitivity;
    g_reticle_y += static_cast<float>(g_stolen_mouse_dy) * sensitivity;

    // Clamp to screen bounds
    g_reticle_x = std::clamp(g_reticle_x, 0.0f, static_cast<float>(screen_w - 1));
    g_reticle_y = std::clamp(g_reticle_y, 0.0f, static_cast<float>(screen_h - 1));
}

// Pre-physics: zero X dynamics so input doesn't drive X movement,
// but leave positions alone so collision detection can work naturally.
static void pre_lock_entity_x(rf::Entity* entity)
{
    entity->p_data.vel.x = 0.0f;
    entity->p_data.force.x = 0.0f;
}

// Post-physics: measure X displacement from collision response,
// convert it to a Z impulse (pushing player back out of angled surfaces),
// then snap everything to X=0.
static void post_lock_entity_x(rf::Entity* entity)
{
    // Capture how much collision pushed us in X
    float dx = entity->pos.x - g_ss_locked_x;

    // Apply as Z correction: collision wanted to push us out in X,
    // convert that to pushing back in Z (opposite movement direction)
    float abs_dx = std::abs(dx);
    if (abs_dx > 0.0001f) {
        float sign = (entity->p_data.vel.z >= 0.0f) ? -1.0f : 1.0f;
        entity->pos.z += sign * abs_dx;
        entity->p_data.pos.z += sign * abs_dx;
        entity->p_data.next_pos.z += sign * abs_dx;
    }

    // Snap all X state to locked value
    entity->pos.x = g_ss_locked_x;
    entity->last_pos.x = g_ss_locked_x;
    entity->correct_pos.x = g_ss_locked_x;
    entity->p_data.pos.x = g_ss_locked_x;
    entity->p_data.next_pos.x = g_ss_locked_x;
    entity->p_data.vel.x = 0.0f;
    entity->p_data.force.x = 0.0f;
}

static void update_side_scroller_camera_pos(rf::Player* player)
{
    rf::Entity* entity = rf::entity_from_handle(player->entity_handle);
    if (!entity) {
        g_ss_camera_active = false;
        return;
    }

    // Compute desired camera position: offset to the right (+X) of the player, at eye height.
    // Camera Y at eye height is used as the reference for reticle unprojection.
    float eye_y = 0.0f;
    if (entity->info) {
        eye_y = entity->info->local_eye_offset.y;
    }
    g_ss_camera_pos.x = entity->pos.x + camera_offset_x;
    g_ss_camera_pos.y = entity->pos.y + eye_y;
    g_ss_camera_pos.z = entity->pos.z;
    g_ss_camera_active = true;
}

// Get the world position of the weapon muzzle in third-person.
// Two-step tag lookup: character hand tag → weapon muzzle tag (same as stock NPC code).
// Falls back to hand position if the weapon lacks a muzzle tag.
static bool get_muzzle_world_pos(rf::Entity* entity, rf::Vector3& out_pos)
{
    if (!entity->info || !entity->vmesh) return false;
    int hand_tag = entity->info->hand_tags[1]; // right hand
    if (hand_tag == -1) return false;

    // Step 1: character model hand tag → hand world pos/orient
    rf::Matrix3 hand_orient;
    rf::Vector3 hand_pos;
    rf::vmesh_get_tag_world_pos(entity->vmesh, hand_tag, entity->orient, entity->pos,
                                hand_orient, hand_pos);

    // Step 2: weapon third-person model muzzle tag → muzzle world pos
    int weapon_type = entity->ai.current_primary_weapon;
    if (weapon_type >= 0 && weapon_type < rf::num_weapon_types) {
        rf::WeaponInfo& wi = rf::weapon_types[weapon_type];
        if (wi.third_person_vmesh_handle && wi.third_person_muzzle_tag != -1) {
            rf::Matrix3 muzzle_orient;
            rf::vmesh_get_tag_world_pos(wi.third_person_vmesh_handle, wi.third_person_muzzle_tag,
                                        hand_orient, hand_pos, muzzle_orient, out_pos);
            return true;
        }
    }

    // Fallback: use hand position if weapon has no muzzle tag
    out_pos = hand_pos;
    return true;
}

// Hook entity_calc_fire_pos (0x0041b040) to override fire origin with hand tag position.
// Stock code falls back to eye_pos for player characters since they lack primary_fire_points.
// In side-scroller mode we want projectiles to spawn from the visible weapon position.
FunHook<void(rf::Entity*, int, rf::Vector3*, rf::Matrix3*, int, int)> entity_calc_fire_pos_hook{
    0x0041b040,
    [](rf::Entity* entity, int weapon_type, rf::Vector3* out_pos, rf::Matrix3* out_orient,
       int fire_slot, int is_primary) {
        // In side-scroller mode, stock code may have reset entity orient to +Z.
        // Temporarily restore the correct body orient so that both stock fire pos
        // calculation and get_muzzle_world_pos compute the muzzle on the correct side.
        bool is_local_ss = false;
        rf::Matrix3 saved_orient;
        if (is_side_scroller_mode() && rf::local_player) {
            rf::Entity* local_entity = rf::entity_from_handle(rf::local_player->entity_handle);
            if (entity == local_entity) {
                is_local_ss = true;
                saved_orient = entity->orient;
                if (g_ss_aiming_backward) {
                    entity->orient.fvec = {0.0f, 0.0f, -1.0f};
                    entity->orient.rvec = {-1.0f, 0.0f, 0.0f};
                    entity->orient.uvec = {0.0f, 1.0f, 0.0f};
                } else {
                    entity->orient.fvec = {0.0f, 0.0f, 1.0f};
                    entity->orient.rvec = {1.0f, 0.0f, 0.0f};
                    entity->orient.uvec = {0.0f, 1.0f, 0.0f};
                }
            }
        }

        entity_calc_fire_pos_hook.call_target(entity, weapon_type, out_pos, out_orient,
                                              fire_slot, is_primary);

        if (!is_local_ss) return;

        rf::Vector3 hand_pos;
        if (get_muzzle_world_pos(entity, hand_pos)) {
            *out_pos = hand_pos;
            g_ss_hand_pos = hand_pos;
            g_ss_hand_pos_valid = true;
        }

        // Restore original orient (stock code may need it intact)
        entity->orient = saved_orient;

        if (g_ss_aim_orient_valid) {
            *out_orient = g_ss_aim_orient;
        }

        // Debug: record actual fire pos hook output for green debug line
        g_dbg_fire_start = *out_pos;
        g_dbg_fire_end.x = out_pos->x + out_orient->fvec.x * 50.0f;
        g_dbg_fire_end.y = out_pos->y + out_orient->fvec.y * 50.0f;
        g_dbg_fire_end.z = out_pos->z + out_orient->fvec.z * 50.0f;
        g_dbg_fire_valid = true;
    },
};

static void apply_side_scroller_aim(rf::Player* player)
{
    rf::Entity* entity = rf::entity_from_handle(player->entity_handle);
    if (!entity) {
        return;
    }

    int screen_w = rf::gr::clip_width();
    int screen_h = rf::gr::clip_height();

    // Use the actual rendering FOV (captured from gr_setup_3d), not player->viewport.fov_h,
    // because the rendering pipeline applies Hor+ scaling and user FOV settings.
    constexpr float deg2rad = 3.14159265f / 180.0f;
    float fov_h = g_ss_render_fov_h_deg * deg2rad;
    float aspect = static_cast<float>(screen_w) / static_cast<float>(screen_h);
    float fov_v = 2.0f * std::atan(std::tan(fov_h * 0.5f) / aspect);

    // Normalized screen coordinates: -1 to +1 from center
    float half_w = static_cast<float>(screen_w) * 0.5f;
    float half_h = static_cast<float>(screen_h) * 0.5f;
    float norm_x = (g_reticle_x - half_w) / half_w;
    float norm_y = (g_reticle_y - half_h) / half_h;

    // Unproject reticle to world-space point on the X=0 plane.
    // Camera is at (player.x + offset, camera_y, player.z) looking in -X.
    float dz = norm_x * std::tan(fov_h * 0.5f) * camera_offset_x;   // screen right = +Z
    float dy = -norm_y * std::tan(fov_v * 0.5f) * camera_offset_x;  // screen up = +Y

    float reticle_world_y = g_ss_camera_pos.y + dy;
    float reticle_world_z = g_ss_camera_pos.z + dz;

    // Get muzzle position (where projectiles actually spawn from)
    rf::Vector3 muzzle_pos;
    if (!get_muzzle_world_pos(entity, muzzle_pos)) {
        muzzle_pos = entity->eye_pos;
    }

    // Aim direction from muzzle to reticle world point.
    // This ensures the debug line, bullets, and projectiles all pass through the reticle.
    float muzzle_dir_y = reticle_world_y - muzzle_pos.y;
    float muzzle_dir_z = reticle_world_z - muzzle_pos.z;
    float muzzle_dist = std::sqrt(muzzle_dir_y * muzzle_dir_y + muzzle_dir_z * muzzle_dir_z);

    // Camera-center direction (always points outward from the player, never flips)
    float cam_dir_y = dy;
    float cam_dir_z = dz;
    float cam_dist = std::sqrt(cam_dir_y * cam_dir_y + cam_dir_z * cam_dir_z);

    if (cam_dist < 0.001f) {
        return;
    }

    // Normalize both directions
    float mn_y = 0.0f, mn_z = 0.0f;
    if (muzzle_dist > 0.001f) {
        mn_y = muzzle_dir_y / muzzle_dist;
        mn_z = muzzle_dir_z / muzzle_dist;
    }
    float cn_y = cam_dir_y / cam_dist;
    float cn_z = cam_dir_z / cam_dist;

    // Blend: when the reticle is close to the muzzle, smoothly transition from
    // muzzle→reticle direction to camera-center direction to avoid abrupt jumps.
    constexpr float blend_min = 1.0f;  // full camera-center direction
    constexpr float blend_max = 3.0f;  // full muzzle→reticle direction
    float t = std::clamp((muzzle_dist - blend_min) / (blend_max - blend_min), 0.0f, 1.0f);

    rf::Vector3 dir;
    dir.x = 0.0f;
    dir.y = mn_y * t + cn_y * (1.0f - t);
    dir.z = mn_z * t + cn_z * (1.0f - t);

    float dir_len = std::sqrt(dir.y * dir.y + dir.z * dir.z);
    if (dir_len < 0.001f) {
        return;
    }
    dir.y /= dir_len;
    dir.z /= dir_len;

    // Build eye_orient matrix from aim direction.
    // fvec = aim direction in YZ plane
    // rvec = +X or -X (mirror when aiming left to keep character upright)
    // uvec = fvec × rvec
    rf::Matrix3 aim_orient;
    aim_orient.fvec = dir;

    if (dir.z >= 0.0f) {
        aim_orient.rvec = {1.0f, 0.0f, 0.0f};
    }
    else {
        aim_orient.rvec = {-1.0f, 0.0f, 0.0f};
    }

    // uvec = fvec × rvec
    aim_orient.uvec.x = aim_orient.fvec.y * aim_orient.rvec.z - aim_orient.fvec.z * aim_orient.rvec.y;
    aim_orient.uvec.y = aim_orient.fvec.z * aim_orient.rvec.x - aim_orient.fvec.x * aim_orient.rvec.z;
    aim_orient.uvec.z = aim_orient.fvec.x * aim_orient.rvec.y - aim_orient.fvec.y * aim_orient.rvec.x;

    // Set eye_orient directly (controls weapon aim direction).
    // Also cache in g_ss_aim_orient so the fire pos hook has a reliable copy
    // even if stock code overwrites eye_orient mid-frame.
    entity->eye_orient = aim_orient;
    g_ss_aim_orient = aim_orient;
    g_ss_aim_orient_valid = true;

    // Debug aim line: from muzzle along aim direction (through reticle world point)
    g_dbg_aim_start = muzzle_pos;
    g_dbg_aim_end.x = muzzle_pos.x + dir.x * 50.0f;
    g_dbg_aim_end.y = muzzle_pos.y + dir.y * 50.0f;
    g_dbg_aim_end.z = muzzle_pos.z + dir.z * 50.0f;
    g_dbg_aim_valid = true;

    // Zero all phb so the body stays facing +Z for movement.
    entity->control_data.phb = {0.0f, 0.0f, 0.0f};
    entity->control_data.eye_phb = {0.0f, 0.0f, 0.0f};
    entity->control_data.delta_phb = {0.0f, 0.0f, 0.0f};
    entity->control_data.delta_eye_phb = {0.0f, 0.0f, 0.0f};
}

// Hook the player control input reading function (FUN_004a6060). This runs in the game loop
// right after keyboard/mouse input is read into ci, BEFORE any game tick processing.
// This is the correct place to remap controls since ci.move is consumed later during
// player_do_frame → FUN_004a77a0 (physics processing).
FunHook<void(rf::Player*)> side_scroller_control_read_hook{
    0x004A6060,
    [](rf::Player* player) {
        side_scroller_control_read_hook.call_target(player);

        if (!is_side_scroller_mode() || player != rf::local_player) return;

        rf::Entity* entity = rf::entity_from_handle(player->entity_handle);
        if (!entity) return;

        auto& ci = entity->ai.ci;

        // A/D (strafe = ci.move.x) controls left/right movement on screen.
        // Remap to ci.move.z (forward/back along body's facing direction).
        // Body orient already handles direction — no negation needed.
        float original_strafe = ci.move.x;  // A/D
        ci.move.z = original_strafe;
        ci.move.x = 0.0f;                   // no toward/away-from-camera movement
        // ci.move.y preserved (jump/crouch)

        // Suppress mouse look rotation (camera is fixed)
        ci.mouse_dh = 0.0f;
        ci.mouse_dp = 0.0f;
        ci.rot = {0.0f, 0.0f, 0.0f};
    },
};

// Hook player_do_frame to steal mouse input and remap controls each frame
FunHook<void(rf::Player*)> side_scroller_player_do_frame_hook{
    0x004A2700,
    [](rf::Player* player) {
        bool is_local_ss = is_side_scroller_mode() && player == rf::local_player;

        if (is_local_ss) {
            // BEFORE stock processing: steal mouse deltas and zero the globals
            // so the stock code can't use them for camera rotation.
            steal_mouse_deltas();
            update_reticle_from_stolen_mouse();

            // Determine if aiming backward (reticle left of screen center = -Z)
            float half_w = static_cast<float>(rf::gr::clip_width()) * 0.5f;
            g_ss_aiming_backward = (g_reticle_x < half_w);

            // Set body orient BEFORE stock processing so movement uses correct direction.
            // Facing +Z: fvec=(0,0,1), rvec=(1,0,0), uvec=(0,1,0)
            // Facing -Z: fvec=(0,0,-1), rvec=(-1,0,0), uvec=(0,1,0)
            rf::Entity* pre_entity = rf::entity_from_handle(player->entity_handle);
            if (pre_entity) {
                // Zero X dynamics so input doesn't drive X movement,
                // but let collision response happen naturally
                pre_lock_entity_x(pre_entity);

                if (g_ss_aiming_backward) {
                    pre_entity->orient.fvec = {0.0f, 0.0f, -1.0f};
                    pre_entity->orient.rvec = {-1.0f, 0.0f, 0.0f};
                    pre_entity->orient.uvec = {0.0f, 1.0f, 0.0f};
                }
                else {
                    pre_entity->orient.fvec = {0.0f, 0.0f, 1.0f};
                    pre_entity->orient.rvec = {1.0f, 0.0f, 0.0f};
                    pre_entity->orient.uvec = {0.0f, 1.0f, 0.0f};
                }

                // Compute fresh aim orient from current reticle BEFORE stock processing,
                // so weapons firing mid-frame use the correct direction.
                update_side_scroller_camera_pos(player);
                apply_side_scroller_aim(player);
            }
        }

        side_scroller_player_do_frame_hook.call_target(player);

        if (!is_local_ss) {
            return;
        }

        rf::Entity* entity = rf::entity_from_handle(player->entity_handle);
        if (!entity) {
            return;
        }

        // AFTER stock processing: convert X collision displacement to Z,
        // then lock X to 0
        post_lock_entity_x(entity);

        // Re-set body orient BEFORE apply_side_scroller_aim so that
        // get_muzzle_world_pos() computes the muzzle on the correct side
        if (g_ss_aiming_backward) {
            entity->orient.fvec = {0.0f, 0.0f, -1.0f};
            entity->orient.rvec = {-1.0f, 0.0f, 0.0f};
            entity->orient.uvec = {0.0f, 1.0f, 0.0f};
        }
        else {
            entity->orient.fvec = {0.0f, 0.0f, 1.0f};
            entity->orient.rvec = {1.0f, 0.0f, 0.0f};
            entity->orient.uvec = {0.0f, 1.0f, 0.0f};
        }

        update_side_scroller_camera_pos(player);
        apply_side_scroller_aim(player);
    },
};

// Hook gr_setup_3d call inside gameplay_render_frame to override viewer position/orientation.
// This intercepts at the final call that sets up the D3D view matrix, bypassing all
// intermediate third-person camera adjustments that were overwriting our values.
CallHook<void(rf::Matrix3&, rf::Vector3&, float, bool, bool)> side_scroller_gr_setup_3d_hook{
    0x00431D14,
    [](rf::Matrix3& viewer_orient, rf::Vector3& viewer_pos, float horizontal_fov, bool zbuffer_flag, bool z_scale) {
        if (g_ss_camera_active && is_side_scroller_mode()) {
            viewer_pos = g_ss_camera_pos;
            viewer_orient = g_side_scroller_orient;
            // Capture the actual rendering FOV for reticle unprojection.
            // This includes Hor+ scaling and user FOV settings applied by the render pipeline.
            g_ss_render_fov_h_deg = horizontal_fov;

            // Update occlusion state for dithered transparency.
            // Ramp fade strength over 200ms (5.0 per second to reach 0.5 target in 100ms,
            // or decay at same rate).
            constexpr float fade_target = 0.5f;
            constexpr float fade_speed = fade_target / 0.2f; // reach target in 200ms
            float dt = rf::frametime;
            if (g_ss_occlusion_fade_current < fade_target) {
                g_ss_occlusion_fade_current = std::min(g_ss_occlusion_fade_current + fade_speed * dt, fade_target);
            }

            // Player position is camera pos with X offset removed
            g_ss_occlusion.active = true;
            g_ss_occlusion.player_x = g_ss_camera_pos.x - camera_offset_x;
            g_ss_occlusion.player_y = g_ss_camera_pos.y;
            g_ss_occlusion.player_z = g_ss_camera_pos.z;
            g_ss_occlusion.camera_x = g_ss_camera_pos.x;
            g_ss_occlusion.fade_strength = g_ss_occlusion_fade_current;
            g_ss_occlusion.radius = 2.5f;
        }
        else {
            // Fade out when leaving side-scroller mode
            if (g_ss_occlusion_fade_current > 0.0f) {
                constexpr float fade_speed = 0.5f / 0.2f;
                float dt = rf::frametime;
                g_ss_occlusion_fade_current = std::max(g_ss_occlusion_fade_current - fade_speed * dt, 0.0f);
                g_ss_occlusion.fade_strength = g_ss_occlusion_fade_current;
            }
            if (g_ss_occlusion_fade_current <= 0.0f) {
                g_ss_occlusion.active = false;
            }
        }
        side_scroller_gr_setup_3d_hook.call_target(viewer_orient, viewer_pos, horizontal_fov, zbuffer_flag, z_scale);

        // Draw debug aim line (red = apply_side_scroller_aim direction)
        if (g_dbg_aim_valid && is_side_scroller_mode()) {
            rf::gr::line_arrow(
                g_dbg_aim_start.x, g_dbg_aim_start.y, g_dbg_aim_start.z,
                g_dbg_aim_end.x, g_dbg_aim_end.y, g_dbg_aim_end.z,
                255, 0, 0);
        }
        // Draw debug fire line (green = entity_calc_fire_pos_hook actual output)
        if (g_dbg_fire_valid && is_side_scroller_mode()) {
            rf::gr::line_arrow(
                g_dbg_fire_start.x, g_dbg_fire_start.y, g_dbg_fire_start.z,
                g_dbg_fire_end.x, g_dbg_fire_end.y, g_dbg_fire_end.z,
                0, 255, 0);
        }
    },
};

void side_scroller_do_patch()
{
    side_scroller_player_do_frame_hook.install();
    side_scroller_control_read_hook.install();
    side_scroller_gr_setup_3d_hook.install();
    entity_calc_fire_pos_hook.install();
}
