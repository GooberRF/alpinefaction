#include "multi_spectate.h"
#include "hud.h"
#include "hud_internal.h"
#include "multi_scoreboard.h"
#include "../input/input.h"
#include "../os/console.h"
#include "../rf/entity.h"
#include "../rf/level.h"
#include "../rf/file/file.h"
#include "../rf/player/player.h"
#include "../rf/multi.h"
#include "../rf/os/console.h"
#include "../rf/gameseq.h"
#include "../rf/weapon.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../rf/hud.h"
#include "../rf/bmpman.h"
#include "../rf/player/camera.h"
#include "../rf/player/player_fpgun.h"
#include "../rf/vmesh.h"
#include "../rf/v3d.h"
#include "../rf/collide.h"
#include "../rf/geometry.h"
#include "../rf/math/matrix.h"
#include "../main/main.h"
#include "../input/mouse.h"
#include "../misc/player.h"
#include "../misc/alpine_settings.h"
#include "../multi/gametype.h"
#include "../multi/saved_info.h"
#include <common/config/BuildConfig.h>
#include <xlog/xlog.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>
#include <common/utils/list-utils.h>
#include <common/utils/string-utils.h>
#include <common/version/version.h>
#include <toml++/toml.hpp>
#include "../rf/input.h"
#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/FunHook.h>
#include <patch_common/AsmWriter.h>
#include <shlwapi.h>
#include <windows.h>
#include "../multi/alpine_packets.h"

static rf::Player* g_spectate_mode_target;
static rf::Camera* g_old_target_camera = nullptr;
static bool g_spectate_mode_enabled = false; // attached camera spectate
static bool g_spectate_mode_follow_killer = false;
static rf::Player* g_spectate_freelook_saved_target = nullptr;

// Two spectate groups, each with its own submode. The "attached" group watches a player
// (first or third person); the "detached" group is a free camera (free look or static).
// The Attach bind swaps between groups; Change Spectate View flips the submode in the group.
enum class SpectateViewMode
{
    first_person,
    third_person,
    freelook,
    static_cam,
};
static SpectateViewMode g_spectate_view_mode = SpectateViewMode::first_person;
// Session-scoped submode memory: survives leaving/re-entering spectate and level changes.
static SpectateViewMode g_spectate_attached_submode = SpectateViewMode::first_person;
static SpectateViewMode g_spectate_detached_submode = SpectateViewMode::freelook;
static bool g_spectate_last_attached = true; // which group to restore on re-enter
static bool g_spectate_third_person_orbit = false; // MMB toggle; default off, non-persistent
static bool g_spectate_static_active = false; // in static-camera spectate

// Static camera selection. The cycle runs over [0, level cameras + dropped cameras). Indices below
// the level count are rendered by the engine's fixed view; higher ones are player-dropped cameras
// positioned by multi_spectate_camera_do_frame.
struct SpectateStaticCamera
{
    int id; // stable per-level identifier, persisted so numpad binds can reference dropped cameras
    rf::Vector3 pos;
    rf::Matrix3 orient;
};
static std::vector<SpectateStaticCamera> g_spectate_dropped_cameras;
static int g_spectate_static_index = 0;

// Marker mesh drawn at each static camera location while free look spectating.
static rf::VMesh* g_spectate_camera_mesh = nullptr;
static bool g_spectate_camera_mesh_load_attempted = false;

// Numpad quick-bind: numpad 0-9 jump to a bound player (first/third person/orbit) or static camera.
// Numpad Enter opens/closes a bind dialog; while it's open, a numpad number binds the current
// player/camera to that key.
static bool g_spectate_bind_dialog_open = false;
static constexpr int kSpectateNumpadCount = 10; // number of numpad quick-bind slots (0-9)
static rf::Player* g_spectate_player_binds[kSpectateNumpadCount] = {}; // numpad key -> spectated player
static int g_spectate_static_binds[kSpectateNumpadCount] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1}; // -> static index
static const rf::Key g_spectate_numpad_keys[kSpectateNumpadCount] = {
    rf::KEY_PAD0, rf::KEY_PAD1, rf::KEY_PAD2, rf::KEY_PAD3, rf::KEY_PAD4,
    rf::KEY_PAD5, rf::KEY_PAD6, rf::KEY_PAD7, rf::KEY_PAD8, rf::KEY_PAD9,
};

// Third person orbit state. Angles are absolute (world space); mouse moves them.
static float g_spectate_orbit_yaw = 0.0f;
static float g_spectate_orbit_pitch = 0.0f;
static constexpr float k_spectate_orbit_distance = 4.5f;
static constexpr float k_spectate_orbit_focus_height = 1.0f;
static constexpr float k_spectate_orbit_pitch_limit = 1.4f; // ~80 degrees

// Free look stepped zoom: tapping "next" (primary attack) steps through these FOV divisors and wraps.
static constexpr float k_spectate_freelook_zoom_steps[] = {1.0f, 2.0f, 4.0f};
static int g_spectate_freelook_zoom_index = 0;

static bool spectate_is_player_view(SpectateViewMode mode)
{
    return mode == SpectateViewMode::first_person || mode == SpectateViewMode::third_person;
}

// First-person-only render path (target's fpgun, scope, reticle) is active only in this view.
static bool spectate_first_person_render_active()
{
    return g_spectate_mode_enabled && g_spectate_view_mode == SpectateViewMode::first_person;
}

bool multi_spectate_is_third_person_orbit()
{
    return g_spectate_mode_enabled && g_spectate_view_mode == SpectateViewMode::third_person
        && g_spectate_third_person_orbit;
}

bool multi_spectate_is_static()
{
    return g_spectate_static_active;
}

float multi_spectate_get_view_fov_scale()
{
    if (g_spectate_view_mode == SpectateViewMode::freelook && multi_spectate_is_freelook()) {
        return k_spectate_freelook_zoom_steps[g_spectate_freelook_zoom_index];
    }
    return 1.0f;
}

// Edge-detection state for spectated player action animations
static bool g_prev_weapon_is_on = false;
static bool g_prev_is_reloading = false;
static bool g_prev_alt_fire_is_on = false;
static int g_prev_weapon_type = -1;

void player_fpgun_set_player(rf::Player* pp);

static void spectate_apply_player_view_mode();
static void spectate_init_orbit(rf::Camera* camera);
static void spectate_populate_default_binds();
static void spectate_save_afl();
static void spectate_delete_current_dropped_camera();

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
        g_spectate_mode_target->flags &= ~rf::PF_HIDE_FROM_CAMERA;
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
    if (entering_player_spectate && !spectate_is_player_view(g_spectate_view_mode)) {
        // Entering attached spectate from a free view (e.g. the `spectate <name>` console command
        // in free look) - default to first person so the view-mode state stays consistent.
        g_spectate_view_mode = SpectateViewMode::first_person;
    }
    if (entering_player_spectate) {
        spectate_populate_default_binds(); // seed numpad binds with the top players (if none set yet)
    }
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
    player->flags |= rf::PF_HIDE_FROM_CAMERA;
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

    // The block above sets up first-person spectate. If the active view is third person / orbit,
    // convert to it (show the body, attach the camera behind/around the target).
    if (spectate_is_player_view(g_spectate_view_mode)
        && g_spectate_view_mode != SpectateViewMode::first_person) {
        spectate_apply_player_view_mode();
    }
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
    else if (!g_spectate_mode_enabled) {
        // No other players to spectate - fall back to freelook
        multi_spectate_enter_freelook();
    }
}

void multi_spectate_enter_freelook()
{
    if (!rf::local_player || !rf::local_player->cam || !rf::is_multi)
        return;

    g_spectate_view_mode = SpectateViewMode::freelook;
    g_spectate_freelook_zoom_index = 0;
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
    return g_spectate_mode_enabled || multi_spectate_is_freelook() || g_spectate_static_active;
}

bool multi_spectate_is_first_person()
{
    return spectate_first_person_render_active();
}

// Restore the spectator's camera/target binding that a player view set up. Safe when no target.
static void spectate_unbind_target()
{
    if (g_spectate_mode_target && g_spectate_mode_target != rf::local_player) {
        g_spectate_mode_target->cam = g_old_target_camera;
        g_old_target_camera = nullptr;
        if (rf::local_player && rf::local_player->cam)
            rf::local_player->cam->player = rf::local_player;

#if SPECTATE_MODE_SHOW_WEAPON
        g_spectate_mode_target->flags &= ~rf::PF_HIDE_FROM_CAMERA;
        rf::Entity* entity = rf::entity_from_handle(g_spectate_mode_target->entity_handle);
        if (entity)
            entity->local_player = nullptr;
        player_fpgun_set_player(rf::local_player);
#endif
    }
}

static void spectate_init_orbit(rf::Camera* camera)
{
    // Seed yaw/pitch from the current look direction so entering orbit doesn't snap.
    rf::Vector3 fwd = rf::camera_get_orient(camera).fvec;
    g_spectate_orbit_yaw = std::atan2(fwd.x, fwd.z);
    g_spectate_orbit_pitch = std::clamp(std::asin(std::clamp(fwd.y, -1.0f, 1.0f)),
        -k_spectate_orbit_pitch_limit, k_spectate_orbit_pitch_limit);
}

// Configure the camera + target body visibility for the current player view. Assumes the camera
// is already bound to g_spectate_mode_target (via multi_spectate_set_target_player).
static void spectate_apply_player_view_mode()
{
    if (!rf::local_player || !rf::local_player->cam || !g_spectate_mode_target
        || !spectate_is_player_view(g_spectate_view_mode)) {
        return;
    }

    rf::Camera* camera = rf::local_player->cam;
    rf::Entity* entity = rf::entity_from_handle(g_spectate_mode_target->entity_handle);

    if (g_spectate_view_mode == SpectateViewMode::first_person) {
#if SPECTATE_MODE_SHOW_WEAPON
        // Hide the target's body and render their first-person weapon.
        g_spectate_mode_target->flags |= rf::PF_HIDE_FROM_CAMERA;
        g_spectate_mode_target->fpgun_data.fpgun_weapon_type = -1;
        g_spectate_mode_target->weapon_mesh_handle = nullptr;
        if (entity) {
            rf::player_fpgun_set_state(g_spectate_mode_target, entity->ai.current_primary_weapon);
            entity->local_player = g_spectate_mode_target;
        }
        player_fpgun_set_player(g_spectate_mode_target);
#endif
        rf::camera_enter_first_person(camera);
    }
    else {
        // Third person / orbit: show the target's body, drop the first-person weapon overlay.
#if SPECTATE_MODE_SHOW_WEAPON
        g_spectate_mode_target->flags &= ~rf::PF_HIDE_FROM_CAMERA;
        if (entity)
            entity->local_player = nullptr;
        player_fpgun_set_player(rf::local_player);
#endif
        rf::camera_enter_third_person(camera);
        if (g_spectate_view_mode == SpectateViewMode::third_person && g_spectate_third_person_orbit)
            spectate_init_orbit(camera);
    }
}

static int g_spectate_saved_fixed_look_target = -1;

static int spectate_static_camera_count()
{
    return rf::fixed_camera_count + static_cast<int>(g_spectate_dropped_cameras.size());
}

static int spectate_next_dropped_camera_id()
{
    int next = 0;
    for (const auto& cam : g_spectate_dropped_cameras) {
        if (cam.id >= std::numeric_limits<int>::max())
            continue; // avoid signed overflow on cam.id + 1
        next = std::max(next, cam.id + 1);
    }
    return next;
}

static void spectate_save_afl()
{
    if (!rf::is_multi)
        return;

    toml::array cameras;
    for (const auto& cam : g_spectate_dropped_cameras) {
        cameras.push_back(toml::table{
            {"id", cam.id},
            {"pos", toml::array{cam.pos.x, cam.pos.y, cam.pos.z}},
            {"rvec", toml::array{cam.orient.rvec.x, cam.orient.rvec.y, cam.orient.rvec.z}},
            {"uvec", toml::array{cam.orient.uvec.x, cam.orient.uvec.y, cam.orient.uvec.z}},
            {"fvec", toml::array{cam.orient.fvec.x, cam.orient.fvec.y, cam.orient.fvec.z}},
        });
    }

    toml::array binds;
    for (int i = 0; i < kSpectateNumpadCount; ++i) {
        const int idx = g_spectate_static_binds[i];
        if (idx < 0)
            continue;
        toml::table entry{{"key", i}};
        if (idx < rf::fixed_camera_count) {
            entry.insert("level_index", idx); // bound to a camera baked into the .rfl
        }
        else {
            const int di = idx - rf::fixed_camera_count;
            if (di < 0 || di >= static_cast<int>(g_spectate_dropped_cameras.size()))
                continue; // stale bind - skip
            entry.insert("camera_id", g_spectate_dropped_cameras[di].id);
        }
        binds.push_back(entry);
    }

    saved_info_write(toml::table{{"cameras", cameras}, {"binds", binds}});
}

static void spectate_load_afl()
{
    const auto root_opt = saved_info_read();
    if (!root_opt)
        return;
    const toml::table& root = *root_opt;

    if (const auto* cameras = root.get_as<toml::array>("cameras")) {
        for (auto&& node : *cameras) {
            const auto* entry = node.as_table();
            if (!entry)
                continue;
            SpectateStaticCamera cam{};
            const auto id = (*entry)["id"].value<int64_t>();
            cam.id = id ? static_cast<int>(*id) : spectate_next_dropped_camera_id();
            cam.orient.make_identity();
            if (!saved_info_read_vec3(entry->get_as<toml::array>("pos"), cam.pos))
                continue;
            saved_info_read_vec3(entry->get_as<toml::array>("rvec"), cam.orient.rvec);
            saved_info_read_vec3(entry->get_as<toml::array>("uvec"), cam.orient.uvec);
            const bool have_fvec = saved_info_read_vec3(entry->get_as<toml::array>("fvec"), cam.orient.fvec);
            // Re-derive a clean orthonormal basis from the stored forward vector so a partial, zero,
            // or degenerate saved orientation can't become a bad view matrix. make_quick builds a
            // full basis from fvec (the same call the orbit code uses); otherwise fall back to
            // identity. saved_info_read_vec3 already guarantees finite components when it succeeds.
            const rf::Vector3 fvec = cam.orient.fvec;
            if (have_fvec && fvec.len_sq() > 0.0f) {
                cam.orient.make_quick(fvec);
            }
            else {
                cam.orient.make_identity();
            }
            g_spectate_dropped_cameras.push_back(cam);
        }
    }

    if (const auto* binds = root.get_as<toml::array>("binds")) {
        for (auto&& node : *binds) {
            const auto* entry = node.as_table();
            if (!entry)
                continue;
            const auto key = (*entry)["key"].value<int64_t>();
            if (!key || *key < 0 || *key > 9)
                continue;
            const int k = static_cast<int>(*key);
            if (const auto level_index = (*entry)["level_index"].value<int64_t>()) {
                const int li = static_cast<int>(*level_index);
                if (li >= 0 && li < rf::fixed_camera_count)
                    g_spectate_static_binds[k] = li;
            }
            else if (const auto camera_id = (*entry)["camera_id"].value<int64_t>()) {
                const int cid = static_cast<int>(*camera_id);
                for (int j = 0; j < static_cast<int>(g_spectate_dropped_cameras.size()); ++j) {
                    if (g_spectate_dropped_cameras[j].id == cid) {
                        g_spectate_static_binds[k] = rf::fixed_camera_count + j;
                        break;
                    }
                }
            }
        }
    }
}

// Select the camera for g_spectate_static_index. Level cameras (index < level count) are rendered
// by the engine's fixed view; dropped cameras are positioned in multi_spectate_camera_do_frame.
static void spectate_apply_static_index()
{
    if (!rf::local_player || !rf::local_player->cam)
        return;
    rf::local_player->cam->mode = rf::CAMERA_FIXED_VIEW;
    rf::fixed_camera_look_target_handle = -1; // use stored (level) orientation, not a tracked target
    // Keep the engine's fixed-camera index in range. For a player-dropped camera there is no
    // matching .rfl entry, so clamp to a real one; the dropped camera itself is positioned each
    // frame by multi_spectate_camera_do_frame. Prevents an OOB read of the fixed-camera arrays if
    // the stock fixed-view path ever runs.
    if (rf::fixed_camera_count > 0)
        rf::fixed_camera_index = std::clamp(g_spectate_static_index, 0, rf::fixed_camera_count - 1);
}

static void spectate_enter_static()
{
    if (!rf::local_player || !rf::local_player->cam || spectate_static_camera_count() <= 0) {
        // No cameras to show - stay in free look instead. Keep the remembered submode honest.
        g_spectate_detached_submode = SpectateViewMode::freelook;
        multi_spectate_enter_freelook();
        return;
    }
    g_spectate_static_active = true;
    g_spectate_view_mode = SpectateViewMode::static_cam;
    g_spectate_saved_fixed_look_target = rf::fixed_camera_look_target_handle;
    g_spectate_static_index = 0;
    spectate_apply_static_index();
    af_send_spectate_start_packet(nullptr);
}

static void spectate_exit_static()
{
    if (!g_spectate_static_active)
        return;
    g_spectate_static_active = false;
    rf::fixed_camera_look_target_handle = g_spectate_saved_fixed_look_target;
}

static void spectate_cycle_static_camera(bool next)
{
    const int total = spectate_static_camera_count();
    if (total <= 0)
        return;
    int i = g_spectate_static_index + (next ? 1 : -1);
    if (i < 0)
        i = total - 1;
    else if (i >= total)
        i = 0;
    g_spectate_static_index = i;
    spectate_apply_static_index();
}

static void spectate_drop_freelook_camera()
{
    if (!rf::local_player || !rf::local_player->cam)
        return;
    SpectateStaticCamera cam;
    cam.id = spectate_next_dropped_camera_id();
    cam.pos = rf::camera_get_pos(rf::local_player->cam);
    cam.orient = rf::camera_get_orient(rf::local_player->cam);
    g_spectate_dropped_cameras.push_back(cam);
    spectate_save_afl();
    rf::console::print("Dropped spectate camera #{} ({} static cameras available)",
        static_cast<int>(g_spectate_dropped_cameras.size()), spectate_static_camera_count());
}

// Transition from the current spectate view to `to`, handling target binding/unbinding.
static void spectate_set_view_mode(SpectateViewMode to)
{
    if (!rf::local_player || !rf::local_player->cam)
        return;
    const SpectateViewMode from = g_spectate_view_mode;
    if (from == to)
        return;

    const bool from_player = g_spectate_mode_enabled && spectate_is_player_view(from);
    const bool to_player = spectate_is_player_view(to);

    if (g_spectate_static_active)
        spectate_exit_static();
    g_spectate_freelook_zoom_index = 0;

    if (to_player) {
        if (from_player) {
            // Switching among player views - keep the same target and binding.
            g_spectate_view_mode = to;
            spectate_apply_player_view_mode();
        }
        else {
            // Coming from a free view - acquire and bind a target.
            rf::Player* resume = (g_spectate_freelook_saved_target
                && g_spectate_freelook_saved_target != rf::local_player
                && !g_spectate_freelook_saved_target->is_browser)
                ? g_spectate_freelook_saved_target
                : nullptr;
            g_spectate_freelook_saved_target = nullptr;
            g_spectate_mode_target = rf::local_player;
            g_spectate_view_mode = to;
            if (resume)
                multi_spectate_set_target_player(resume);
            else
                spectate_next_player(true, true);
            if (g_spectate_mode_enabled)
                spectate_apply_player_view_mode();
            else
                g_spectate_view_mode = SpectateViewMode::freelook; // no players - fell back to free look
        }
    }
    else {
        // Entering a free view (free look or static) - release any player target.
        if (from_player) {
            g_spectate_freelook_saved_target = g_spectate_mode_target;
            spectate_unbind_target();
            g_spectate_mode_enabled = false;
            g_spectate_mode_target = rf::local_player;
        }
        if (to == SpectateViewMode::freelook)
            multi_spectate_enter_freelook();
        else
            spectate_enter_static();
    }
}

// Attach bind: swap between the attached (player) and detached (free camera) groups, restoring the
// submode last used in the group we're switching to.
void multi_spectate_toggle_attach()
{
    if (!multi_spectate_is_spectating())
        return;
    g_spectate_bind_dialog_open = false; // switching groups closes the numpad bind dialog
    const bool attached = g_spectate_mode_enabled && spectate_is_player_view(g_spectate_view_mode);
    if (attached) {
        g_spectate_attached_submode = g_spectate_view_mode;   // remember first/third
        g_spectate_last_attached = false;
        spectate_set_view_mode(g_spectate_detached_submode);  // restore free/static
    }
    else {
        g_spectate_detached_submode = g_spectate_static_active
            ? SpectateViewMode::static_cam : SpectateViewMode::freelook; // remember free/static
        g_spectate_last_attached = true;
        g_spectate_third_person_orbit = false;                // fresh entry to a player view
        spectate_set_view_mode(g_spectate_attached_submode);  // restore first/third
    }
}

// Change Spectate View bind: flip the submode within the active group.
void multi_spectate_change_view()
{
    if (!multi_spectate_is_spectating())
        return;
    g_spectate_bind_dialog_open = false; // switching submodes closes the numpad bind dialog
    const bool attached = g_spectate_mode_enabled && spectate_is_player_view(g_spectate_view_mode);
    if (attached) {
        const SpectateViewMode to = (g_spectate_view_mode == SpectateViewMode::first_person)
            ? SpectateViewMode::third_person : SpectateViewMode::first_person;
        if (to == SpectateViewMode::third_person)
            g_spectate_third_person_orbit = false;            // reset orbit on fresh third-person entry
        g_spectate_attached_submode = to;
        spectate_set_view_mode(to);
    }
    else {
        const SpectateViewMode to = g_spectate_static_active
            ? SpectateViewMode::freelook : SpectateViewMode::static_cam;
        if (to == SpectateViewMode::static_cam && spectate_static_camera_count() <= 0) {
            rf::console::print("No static cameras on this level. Drop one with [Secondary Attack] in free look.");
            return; // stay in free look
        }
        g_spectate_detached_submode = to;
        spectate_set_view_mode(to);
    }
}

// Delete the currently-selected player-dropped static camera (level .rfl cameras are not deletable),
// fix up numpad binds, and advance the view (or fall back to free look if none remain).
static void spectate_delete_current_dropped_camera()
{
    if (!g_spectate_static_active)
        return;
    const int di = g_spectate_static_index - rf::fixed_camera_count;
    if (di < 0) {
        // Current camera is a level (.rfl) camera - not deletable. Say so instead of a silent no-op.
        rf::console::print("Level cameras cannot be deleted.");
        return;
    }
    if (di >= static_cast<int>(g_spectate_dropped_cameras.size()))
        return; // out of range - nothing to delete
    const int del_idx = g_spectate_static_index; // absolute index being removed

    g_spectate_dropped_cameras.erase(g_spectate_dropped_cameras.begin() + di);

    // Fix up numpad->camera binds. Dropped-camera absolute indices above del_idx shift down by 1;
    // level-camera binds (< fixed_camera_count < del_idx) are unaffected.
    for (int k = 0; k < kSpectateNumpadCount; ++k) {
        if (g_spectate_static_binds[k] == del_idx)
            g_spectate_static_binds[k] = -1;
        else if (g_spectate_static_binds[k] > del_idx)
            g_spectate_static_binds[k] -= 1;
    }
    spectate_save_afl();

    const int total = spectate_static_camera_count();
    if (total <= 0) {
        // No cameras left - fall back to free look.
        spectate_exit_static();
        g_spectate_detached_submode = SpectateViewMode::freelook;
        multi_spectate_enter_freelook();
        rf::console::print("Deleted camera. No static cameras remain; returning to free look.");
        return;
    }
    if (g_spectate_static_index >= total)
        g_spectate_static_index = total - 1; // was the last camera; show the new last
    // otherwise the same index now shows the next camera that shifted into this slot
    spectate_apply_static_index();
    rf::console::print("Deleted camera ({} static cameras remain).", total);
}

// Per-frame camera positioning for third-person orbit (called from camera_do_frame_hook). Returns
// true if it positioned the camera, so the stock per-frame third-person logic is skipped.
bool multi_spectate_camera_do_frame(rf::Camera* camera)
{
    if (!rf::local_player || camera != rf::local_player->cam || !camera->camera_entity)
        return false;
    rf::Entity* ce = camera->camera_entity;

    // Static spectate on a player-dropped camera (level .rfl cameras are drawn by the engine's
    // fixed view). Position it every frame INCLUDING outside active gameplay - otherwise at round
    // end the stock fixed-view path runs for a camera index with no .rfl entry and OOB-reads the
    // fixed-camera arrays.
    if (g_spectate_static_active && g_spectate_static_index >= rf::fixed_camera_count) {
        const int di = g_spectate_static_index - rf::fixed_camera_count;
        if (di >= 0 && di < static_cast<int>(g_spectate_dropped_cameras.size())) {
            const SpectateStaticCamera& sc = g_spectate_dropped_cameras[di];
            ce->pos = sc.pos;
            ce->orient = sc.orient;
            ce->eye_pos = sc.pos;
            ce->eye_orient = sc.orient;
            ce->set_room(nullptr);
            ce->update_room();
            return true;
        }
    }

    // Third-person orbit is only driven during active gameplay; at round end let the engine run
    // its own endgame fixed-camera flyby.
    if (rf::gameseq_get_state() != rf::GS_GAMEPLAY)
        return false;

    if (multi_spectate_is_third_person_orbit()) {
        rf::Entity* target = g_spectate_mode_target
            ? rf::entity_from_handle(g_spectate_mode_target->entity_handle)
            : nullptr;
        if (!target) {
            // Target dead/gone this frame - drain the mouse accumulator so motion during the dead
            // interval doesn't bank up and snap the view on respawn; hold the camera in place.
            int mdx = 0, mdy = 0, mdz = 0;
            rf::mouse_get_delta(mdx, mdy, mdz);
            float dpitch = 0.0f, dyaw = 0.0f;
            consume_raw_mouse_deltas(dpitch, dyaw, false);
            return true;
        }

        // While we're a dead third-person spectator nothing else pumps mouse_get_delta, so the raw
        // delta accumulator stays empty. Pump it here, then read it. In Raw/Modern mouse mode the
        // hook fills the accumulator (read via consume_raw_mouse_deltas); Classic mode returns the
        // raw pixel delta in mdx/mdy instead.
        int mdx = 0, mdy = 0, mdz = 0;
        rf::mouse_get_delta(mdx, mdy, mdz);
        float dpitch = 0.0f, dyaw = 0.0f;
        consume_raw_mouse_deltas(dpitch, dyaw, false);
        if (dpitch == 0.0f && dyaw == 0.0f && (mdx != 0 || mdy != 0)) {
            const float sens = rf::local_player->settings.controls.mouse_sensitivity;
            constexpr float classic_scale = 0.0035f;
            float fy = static_cast<float>(mdy);
            if (rf::local_player->settings.controls.axes[1].invert)
                fy = -fy;
            dpitch = -fy * sens * classic_scale;
            dyaw = static_cast<float>(mdx) * sens * classic_scale;
        }

        g_spectate_orbit_yaw += dyaw;
        g_spectate_orbit_pitch = std::clamp(g_spectate_orbit_pitch + dpitch,
            -k_spectate_orbit_pitch_limit, k_spectate_orbit_pitch_limit);

        rf::Vector3 focus = target->pos;
        focus.y += k_spectate_orbit_focus_height;

        // Yaw/pitch parametrize the LOOK direction, exactly like normal mouselook, so the game's
        // y-invert (applied in consume_raw_mouse_deltas / the classic fallback) carries through.
        // The camera sits `distance` behind the focus along that direction.
        rf::Vector3 look_dir{
            std::cos(g_spectate_orbit_pitch) * std::sin(g_spectate_orbit_yaw),
            std::sin(g_spectate_orbit_pitch),
            std::cos(g_spectate_orbit_pitch) * std::cos(g_spectate_orbit_yaw),
        };
        rf::Vector3 cam_pos = focus - look_dir * k_spectate_orbit_distance;

        rf::Matrix3 orient;
        orient.make_quick(look_dir);

        ce->pos = cam_pos;
        ce->orient = orient;
        ce->eye_pos = cam_pos;
        ce->eye_orient = orient;
        ce->set_room(nullptr);
        ce->update_room();
        return true;
    }

    return false;
}

rf::Player* multi_spectate_get_target_player()
{
    return g_spectate_mode_target;
}

void multi_spectate_leave()
{
    // Remember which group we were in for next time we enter spectate; orbit never persists.
    g_spectate_last_attached = g_spectate_mode_enabled && spectate_is_player_view(g_spectate_view_mode);
    g_spectate_third_person_orbit = false;
    g_spectate_freelook_saved_target = nullptr;
    if (g_spectate_static_active)
        spectate_exit_static();
    g_spectate_view_mode = SpectateViewMode::first_person;
    if (g_spectate_mode_enabled) {
        multi_spectate_set_target_player(nullptr);
    } else {
        set_camera_target(rf::local_player);
        af_send_spectate_start_packet(rf::local_player);
    }
}

void multi_spectate_toggle()
{
    if (!rf::is_multi || rf::is_dedicated_server || !rf::player_is_dead(rf::local_player))
        return;

    if (multi_spectate_is_spectating()) {
        multi_spectate_leave();
    }
    else if (rf::player_is_dead(rf::local_player)) {
        multi_spectate_set_target_player(rf::local_player);
        spectate_next_player(true, true);
        // Restore the group + submode we were last using before leaving spectate.
        g_spectate_third_person_orbit = false;
        const SpectateViewMode want = g_spectate_last_attached
            ? g_spectate_attached_submode : g_spectate_detached_submode;
        if (multi_spectate_is_spectating() && g_spectate_view_mode != want)
            spectate_set_view_mode(want);
    }
}

bool multi_spectate_execute_action(rf::ControlConfigAction action, bool was_pressed)
{
    if (!rf::is_multi) {
        return false;
    }

    const bool primary = (action == rf::CC_ACTION_PRIMARY_ATTACK || action == rf::CC_ACTION_SLIDE_RIGHT);
    const bool secondary = (action == rf::CC_ACTION_SECONDARY_ATTACK || action == rf::CC_ACTION_SLIDE_LEFT);
    if (!primary && !secondary) {
        return false;
    }

    if (g_spectate_mode_enabled) {
        // Player views (first / third person / orbit): cycle the spectated player.
        // set_target_player re-applies the active view mode, so third person/orbit is preserved.
        if (was_pressed)
            spectate_next_player(primary); // primary = next, secondary = prev
        return true; // dont allow spawn
    }
    if (multi_spectate_is_static()) {
        // Static cameras: cycle through the level's fixed cameras.
        if (was_pressed)
            spectate_cycle_static_camera(primary);
        return true;
    }
    if (multi_spectate_is_freelook()) {
        // Only the literal attack buttons act here; strafe keys must remain free-look movement.
        // Primary steps the zoom level (wraps); secondary drops a static camera at this spot.
        if (action == rf::CC_ACTION_PRIMARY_ATTACK) {
            if (was_pressed) {
                constexpr int n = sizeof(k_spectate_freelook_zoom_steps) / sizeof(k_spectate_freelook_zoom_steps[0]);
                g_spectate_freelook_zoom_index = (g_spectate_freelook_zoom_index + 1) % n;
            }
            return true; // consume to prevent respawn
        }
        if (action == rf::CC_ACTION_SECONDARY_ATTACK) {
            if (was_pressed)
                spectate_drop_freelook_camera();
            return true; // consume to prevent respawn
        }
        return false; // slides fall through to free-look movement
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
        // Drop any numpad binds pointing at the leaving player so they can't dangle.
        for (int i = 0; i < kSpectateNumpadCount; ++i) {
            if (g_spectate_player_binds[i] == player)
                g_spectate_player_binds[i] = nullptr;
        }
        if (g_spectate_freelook_saved_target == player)
            g_spectate_freelook_saved_target = nullptr;
        if (g_spectate_mode_target == player)
            spectate_next_player(true);
        if (g_spectate_mode_target == player) {
            // The player we were watching left and there's no one else to spectate. Fall back to
            // free-look spectate instead of dropping out of spectate. Clear the target directly
            // (not via set_target_player, which early-returns under Force Respawn and would leave
            // g_spectate_mode_target dangling at the freed player).
            spectate_unbind_target();
            g_spectate_mode_enabled = false;
            g_spectate_mode_target = rf::local_player;
            g_spectate_freelook_saved_target = nullptr;
            g_spectate_last_attached = false;
            multi_spectate_enter_freelook();
        }
    }
}

// draw reticle
FunHook<void(rf::Player*)> render_reticle_hook{
    0x0043A2C0,
    [](rf::Player* player) {
        if (rf::gameseq_get_state() == rf::GS_MULTI_LIMBO)
            return;
        if (spectate_first_person_render_active())
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

        auto print_exit_hint = [] {
            std::string bind = get_action_bind_name(
                get_af_control(rf::AlpineControlConfigAction::AF_ACTION_SPECTATE_TOGGLE)
            );
            std::string msg = "Press " + bind + " to exit spectate mode.";
            rf::console::output(msg.c_str(), nullptr);
        };

        if (player_name) {
            // spectate player using 1st person view
            rf::Player* player = find_best_matching_player(player_name.value().c_str());
            if (!player) {
                // player not found
                return;
            }
            // player found - spectate
            multi_spectate_set_target_player(player);
            print_exit_hint();
        }
        else if (g_spectate_mode_enabled || multi_spectate_is_freelook()) {
            // leave spectate mode
            multi_spectate_leave();
        }
        else {
            // enter freelook spectate mode
            multi_spectate_enter_freelook();
            print_exit_hint();
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

static ConsoleCommand2 spectate_cameras_cmd{
    "spectate_cameras",
    []() {
        g_alpine_game_config.spectate_show_camera_meshes = !g_alpine_game_config.spectate_show_camera_meshes;
        rf::console::print("Static camera meshes in free look spectate are {}",
            g_alpine_game_config.spectate_show_camera_meshes ? "shown" : "hidden");
    },
    "Toggles showing camera meshes at static camera locations while free look spectating",
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
        if (spectate_first_person_render_active() && rf::local_player && g_spectate_mode_target) {
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
        if (spectate_first_person_render_active() && rf::local_player &&
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
        if (spectate_first_person_render_active() && rf::local_player &&
            rf::local_player->fpgun_data.scanning_for_target) {
            multi_spectate_render();
        }
    },
};

static void spectate_populate_default_binds()
{
    // Re-seed whenever no numpad player binds are set - covers the first spectate of a session and a
    // later server whose binds were cleared on join. Never clobbers binds the user has set.
    for (int i = 0; i < kSpectateNumpadCount; ++i) {
        if (g_spectate_player_binds[i])
            return;
    }

    // Seed numpad 0-9 with the current top-scoring spectatable players (highest first).
    std::vector<rf::Player*> players;
    for (rf::Player& p : SinglyLinkedList{rf::player_list}) {
        if (&p == rf::local_player || p.is_browser || !p.stats) {
            continue;
        }
        players.push_back(&p);
    }
    std::sort(players.begin(), players.end(), [](const rf::Player* a, const rf::Player* b) {
        return a->stats->score > b->stats->score;
    });
    const int n = std::min<int>(kSpectateNumpadCount, static_cast<int>(players.size()));
    for (int i = 0; i < n; ++i) {
        g_spectate_player_binds[i] = players[i];
    }
}

static bool spectate_project_to_screen(const rf::Vector3& world_pos, float& sx, float& sy)
{
    rf::gr::Vertex v{};
    if (!rf::gr::rotate_vertex(&v, &world_pos)) { // 0 => in front of the camera
        rf::gr::project_vertex(&v);
        if (v.flags & rf::gr::VF_PROJECTED) {
            sx = v.sx;
            sy = v.sy;
            return true;
        }
    }
    return false;
}

// Returns the numpad bind suffix for a player for the nameplate, e.g. " (1, 3)" (or "" if none).
static std::string spectate_player_bind_suffix(const rf::Player* player)
{
    std::string keys;
    for (int k = 0; k < kSpectateNumpadCount; ++k) {
        if (g_spectate_player_binds[k] == player) {
            if (!keys.empty()) {
                keys += ", ";
            }
            keys += std::to_string(k);
        }
    }
    return keys.empty() ? std::string{} : (" (" + keys + ")");
}

// Returns the numpad key(s) bound to the given static camera index (e.g. "3", or "" if none).
static std::string spectate_static_bind_label(int static_index)
{
    std::string label;
    for (int k = 0; k < kSpectateNumpadCount; ++k) {
        if (g_spectate_static_binds[k] == static_index) {
            if (!label.empty()) {
                label += ",";
            }
            label += std::to_string(k);
        }
    }
    return label;
}

static void spectate_render_camera_mesh(const rf::Vector3& pos, const rf::Matrix3& orient,
    int static_index, const rf::Vector3& eye)
{
    rf::MeshRenderParams params{};
    params.init_defaults();
    params.orient = orient;
    rf::Vector3 p = pos;
    rf::Matrix3 o = orient;
    rf::vmesh_render(g_spectate_camera_mesh, &p, &o, &params);

    // Draw the numpad bind number above the mesh so it's clear which camera is bound to which key.
    const std::string label = spectate_static_bind_label(static_index);
    if (label.empty()) {
        return;
    }
    rf::Vector3 label_pos = pos;
    label_pos.y += 0.6f;

    // Occlude the label like normal geometry: skip it if level solid blocks the line from the
    // viewer to it. (The mesh itself is already depth-tested by the renderer.)
    rf::Vector3 trace_start = eye;
    rf::Vector3 trace_end = label_pos;
    rf::GCollisionOutput col{};
    if (rf::collide_linesegment_level_solid(trace_start, trace_end,
            rf::CF_ANY_HIT | rf::CF_PROCESS_INVISIBLE_FACES, &col)) {
        return; // occluded
    }

    float sx = 0.0f, sy = 0.0f;
    if (spectate_project_to_screen(label_pos, sx, sy)) {
        rf::gr::set_color(0xFF, 0xF0, 0x50, 0xFF);
        rf::gr::string_aligned(rf::gr::ALIGN_CENTER, static_cast<int>(sx), static_cast<int>(sy),
            label.c_str(), hud_get_default_font());
    }
}

// Draw a marker mesh at each static camera location (level + player-dropped) so free look
// spectators can see where the static cameras are. Injected right after the world render in
// gameplay_render_frame (0x00431FF8 CALL scene render) while the 3D pipeline is still active.
// The mesh is loaded in the logic pass (multi_spectate_process_bind_input), NOT here - loading a
// mesh + its textures mid-render can corrupt the bitmap system and crash on shutdown.
static CodeInjection spectate_render_camera_meshes_patch{
    0x00432000,
    []() {
        if (!g_alpine_game_config.spectate_show_camera_meshes || !multi_spectate_is_freelook()
            || spectate_static_camera_count() <= 0 || !g_spectate_camera_mesh
            || !rf::local_player || !rf::local_player->cam) {
            return;
        }
        const rf::Vector3 eye = rf::camera_get_pos(rf::local_player->cam);
        for (int i = 0; i < rf::fixed_camera_count; ++i) {
            spectate_render_camera_mesh(*rf::fixed_camera_get_pos(i), *rf::fixed_camera_get_orient(i), i, eye);
        }
        for (int j = 0; j < static_cast<int>(g_spectate_dropped_cameras.size()); ++j) {
            const SpectateStaticCamera& sc = g_spectate_dropped_cameras[j];
            spectate_render_camera_mesh(sc.pos, sc.orient, rf::fixed_camera_count + j, eye);
        }
    },
};

// Poll the numpad keys each frame while spectating: Numpad Enter toggles the bind dialog; numpad
// numbers bind (dialog open) or jump to (dialog closed) a player/camera. Called from key.cpp.
void multi_spectate_process_bind_input()
{
    if (!rf::is_multi || !multi_spectate_is_spectating()) {
        return;
    }

    // Preload the camera marker mesh here (logic pass) rather than in the render injection - loading
    // a mesh + its textures mid-render is bitmap-creation-crash-prone (see the gr_d3d11_hooks note),
    // and manifested as a bitmap double-free on game exit.
    if (!g_spectate_camera_mesh_load_attempted && g_alpine_game_config.spectate_show_camera_meshes
        && multi_spectate_is_freelook() && spectate_static_camera_count() > 0) {
        g_spectate_camera_mesh_load_attempted = true;
        g_spectate_camera_mesh = rf::vmesh_load("camera_icon1.v3m", rf::MESH_TYPE_STATIC, -1);
    }

    const bool player_mode = g_spectate_mode_enabled;   // first/third person
    const bool static_mode = g_spectate_static_active;   // static cameras
    const bool bindable = player_mode || static_mode;
    // Still consume the numpad counters while typing (so they don't queue up), but don't act on
    // them - the console/chat-say box uses the separate character buffer for typing.
    const bool typing = rf::console::console_is_visible() || rf::multi_chat_is_say_visible();

    if (rf::key_get_and_reset_down_counter(rf::KEY_PADENTER) > 0 && !typing) {
        g_spectate_bind_dialog_open = bindable && !g_spectate_bind_dialog_open;
    }
    // Esc cancels/closes the bind dialog (only read its edge while the dialog is open and we're not
    // typing, so it doesn't swallow Esc anywhere else).
    if (g_spectate_bind_dialog_open && !typing
        && rf::key_get_and_reset_down_counter(rf::KEY_ESC) > 0) {
        g_spectate_bind_dialog_open = false;
    }
    if (!bindable) {
        g_spectate_bind_dialog_open = false;
    }

    // Middle mouse toggles third-person orbit (attached third person only). Read the edge every
    // frame so the latch is consumed regardless of the active view.
    const bool mmb_pressed = rf::mouse_was_button_pressed(2) > 0;
    if (mmb_pressed && !typing && g_spectate_mode_enabled
        && g_spectate_view_mode == SpectateViewMode::third_person) {
        g_spectate_third_person_orbit = !g_spectate_third_person_orbit;
        spectate_apply_player_view_mode();
    }

    // Delete removes the current player-dropped static camera (static mode only). It can flip
    // g_spectate_static_active off (last camera deleted), which would leave the captured
    // player_mode/static_mode flags stale, so return afterwards instead of falling into the numpad
    // loop this frame.
    if (rf::key_get_and_reset_down_counter(rf::KEY_DELETE) > 0 && static_mode && !typing) {
        spectate_delete_current_dropped_camera();
        return;
    }

    for (int i = 0; i < kSpectateNumpadCount; ++i) {
        if (rf::key_get_and_reset_down_counter(g_spectate_numpad_keys[i]) <= 0 || typing) {
            continue;
        }
        if (g_spectate_bind_dialog_open) {
            // Bind the current player/camera to this key, then close the dialog.
            if (player_mode && g_spectate_mode_target && g_spectate_mode_target != rf::local_player) {
                g_spectate_player_binds[i] = g_spectate_mode_target;
            }
            else if (static_mode) {
                g_spectate_static_binds[i] = g_spectate_static_index;
                spectate_save_afl(); // persist the new numpad->camera bind for this level
            }
            g_spectate_bind_dialog_open = false;
        }
        else if (player_mode) {
            rf::Player* p = g_spectate_player_binds[i];
            if (p && p != rf::local_player) {
                multi_spectate_set_target_player(p);
            }
        }
        else if (static_mode) {
            const int idx = g_spectate_static_binds[i];
            if (idx >= 0 && idx < spectate_static_camera_count()) {
                g_spectate_static_index = idx;
                spectate_apply_static_index();
            }
        }
    }
}

static void spectate_render_bind_dialog()
{
    const bool player_mode = g_spectate_mode_enabled;
    const bool static_mode = g_spectate_static_active;
    if (!g_spectate_bind_dialog_open || (!player_mode && !static_mode)) {
        return;
    }

    const int font = hud_get_default_font();
    const int font_h = rf::gr::get_font_height(font);
    const int scr_w = rf::gr::screen_width();
    const int scr_h = rf::gr::screen_height();

    const std::string title = std::string("Press a numpad number key to bind to this ")
        + (player_mode ? "player" : "camera");
    const std::string subtitle = "Press NUMPAD ENTER to cancel";

    // Build the value strings up front so the box can be sized to fit the widest content.
    std::string values[kSpectateNumpadCount];
    bool bound[kSpectateNumpadCount];
    for (int i = 0; i < kSpectateNumpadCount; ++i) {
        if (player_mode) {
            rf::Player* p = g_spectate_player_binds[i];
            bound[i] = (p != nullptr);
            values[i] = bound[i] ? p->name.c_str() : "(unbound)";
        }
        else {
            const int idx = g_spectate_static_binds[i];
            bound[i] = (idx >= 0 && idx < spectate_static_camera_count());
            values[i] = bound[i] ? ("Camera " + std::to_string(idx + 1)) : "(unbound)";
        }
    }

    auto str_w = [&](const char* s) {
        auto [w, h] = rf::gr::get_string_size(s, font);
        return w;
    };

    const int col_gap = 24;
    const int label_w = str_w("NUM 0");
    int max_value_w = 0;
    for (const std::string& v : values) {
        max_value_w = std::max(max_value_w, str_w(v.c_str()));
    }
    const int row_w = label_w + col_gap + max_value_w;

    const int pad = 20;
    const int content_w = std::max({str_w(title.c_str()), str_w(subtitle.c_str()), row_w});
    const int box_w = std::min(scr_w - 20, content_w + pad * 2);

    const int line_h = font_h + 3;
    const int box_h = pad + font_h * 2 + 12 + line_h * 10 + pad;
    const int box_x = (scr_w - box_w) / 2;
    const int box_y = (scr_h - box_h) / 2;

    rf::gr::set_color(0, 0, 0, 190);
    rf::gr::rect(box_x, box_y, box_w, box_h);

    const int cx = box_x + box_w / 2;
    int y = box_y + pad;
    rf::gr::set_color(0xFF, 0xFF, 0xFF, 0xFF);
    rf::gr::string_aligned(rf::gr::ALIGN_CENTER, cx, y, title.c_str(), font);
    y += font_h;
    rf::gr::set_color(0xFF, 0xFF, 0xFF, 0xB0);
    rf::gr::string_aligned(rf::gr::ALIGN_CENTER, cx, y, subtitle.c_str(), font);
    y += font_h + 12;

    // Center the fixed-width rows block within the box.
    const int label_x = box_x + (box_w - row_w) / 2;
    const int value_x = label_x + label_w + col_gap;
    for (int i = 0; i < kSpectateNumpadCount; ++i) {
        rf::gr::set_color(0xFF, 0xFF, 0xFF, bound[i] ? 0xFF : 0x60);
        const std::string key_label = "NUM " + std::to_string(i);
        rf::gr::string(label_x, y, key_label.c_str(), font);
        rf::gr::string(value_x, y, values[i].c_str(), font);
        y += line_h;
    }
}

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
    if (spectate_first_person_render_active()) {
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
        if (spectate_first_person_render_active()) {
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
        if (spectate_first_person_render_active() && rf::local_player && g_spectate_mode_target) {
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
    spectate_cameras_cmd.register_cmd();
    spectate_render_camera_meshes_patch.install();

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
    // New game/server session: drop any numpad player binds from a previous server so they can't
    // dangle, and allow the defaults to re-seed for this server (see spectate_populate_default_binds).
    for (int i = 0; i < kSpectateNumpadCount; ++i)
        g_spectate_player_binds[i] = nullptr;
}

void multi_spectate_player_create_entity_post(rf::Player* player, [[maybe_unused]] rf::Entity* entity)
{
    // Re-apply the active view mode after the target respawns (the game may have switched our
    // camera to fixed when it entered limbo). This hides the body + shows the weapon in first
    // person, or shows the body in third person / orbit.
    if (g_spectate_mode_enabled && player == g_spectate_mode_target) {
        spectate_apply_player_view_mode();
    }
    // Do not allow spectating in Force Respawn game after spawning for the first time
    if (player == rf::local_player) {
        g_spawned_in_current_level = true;
    }
}

void multi_spectate_level_init()
{
    g_spawned_in_current_level = false;
    g_spectate_freelook_saved_target = nullptr;
    g_spectate_view_mode = SpectateViewMode::first_person;
    g_spectate_third_person_orbit = false;
    g_spectate_static_active = false;
    g_spectate_freelook_zoom_index = 0;
    g_spectate_static_index = 0;
    g_spectate_dropped_cameras.clear(); // dropped cameras are level-specific
    // The vmesh handle is freed on level unload; force a reload on the next use.
    g_spectate_camera_mesh = nullptr;
    g_spectate_camera_mesh_load_attempted = false;
    // Static camera binds are level-specific and reset here; player binds intentionally persist
    // across map changes (players stay connected - disconnects are handled in on_destroy_player).
    g_spectate_bind_dialog_open = false;
    for (int i = 0; i < kSpectateNumpadCount; ++i) {
        g_spectate_static_binds[i] = -1;
    }
    // Restore this level's persisted dropped cameras and numpad binds (if a save file exists).
    spectate_load_afl();
}

template<typename F>
static void draw_with_shadow(int x, int y, int shadow_dx, int shadow_dy, rf::Color clr, rf::Color shadow_clr, F fun)
{
    rf::gr::set_color(shadow_clr);
    fun(x + shadow_dx, y + shadow_dy);
    rf::gr::set_color(clr);
    fun(x, y);
}

// Renders powerup icons to the left of the spectate nameplate bar.
// Detects powerup state from entity_flags2 which are already synced by the stock netcode.
static void render_spectate_powerup_icons(rf::Entity* entity, int bar_x, int bar_y, int bar_h)
{
    if (!entity)
        return;

    // Bagman has no powerup HUD icons
    if (gt_is_bagman_any())
        return;

    // Load bitmaps once
    static int bm_invuln = rf::bm::load("hud_pow_invuln.tga", -1, true);
    static int bm_amp = rf::bm::load("hud_pow_damage.tga", -1, true);

    // Collect active powerups right-to-left (rightmost icon is closest to bar)
    int active_bms[2];
    int count = 0;
    if ((entity->entity_flags2 & rf::EF2_POWERUP_DAMAGE_AMP) != 0 && bm_amp >= 0)
        active_bms[count++] = bm_amp;
    if ((entity->entity_flags2 & rf::EF2_POWERUP_INVULNERABLE) != 0 && bm_invuln >= 0)
        active_bms[count++] = bm_invuln;

    if (count == 0)
        return;

    float scale = g_alpine_game_config.big_hud ? 2.0f : 1.0f;
    int gap = static_cast<int>(4 * scale);

    // Measure icon size (both bitmaps are the same dimensions)
    int bm_w = 0, bm_h = 0;
    rf::bm::get_dimensions(active_bms[0], &bm_w, &bm_h);
    int icon_w = static_cast<int>(bm_w * scale);
    int icon_h = static_cast<int>(bm_h * scale);

    // Place icons to the left of the nameplate bar, right-aligned toward bar_x
    int icon_x = bar_x - gap;

    for (int i = 0; i < count; ++i) {
        icon_x -= icon_w;

        // Vertically center icon on the nameplate bar
        int icon_y = bar_y + (bar_h - icon_h) / 2;

        // Draw icon
        rf::gr::set_color(255, 255, 255, 255);
        hud_scaled_bitmap(active_bms[i], icon_x, icon_y, scale);

        icon_x -= gap;
    }
}

// Returns the screen-y of the top edge of the bottom-left HUD cluster for the current game type.
static int spectate_bottom_left_hud_top()
{
    const int clip_h = rf::gr::clip_height();
    const bool big = g_alpine_game_config.big_hud;

    switch (rf::multi_get_game_type()) {
    case rf::NG_TYPE_KOTH:
    case rf::NG_TYPE_DC:
    case rf::NG_TYPE_ESC:
    case rf::NG_TYPE_REV:
        // The control-point strip stacks above the scores box; its top is tracked while the
        // team-scores HUD renders. Fall back to the bottom margin if it hasn't been set yet.
        return g_multi_hud_cp_strip_y > 0 ? g_multi_hud_cp_strip_y : clip_h - 10;
    case rf::NG_TYPE_RUN:
        if (!g_alpine_game_config.show_run_timer) {
            return clip_h; // timer widget hidden
        }
        return clip_h - (big ? 60 : 40) - 10;
    case rf::NG_TYPE_DM:
        if (!g_alpine_game_config.show_mini_scoreboard_dm) {
            return clip_h; // mini scoreboard hidden in DM
        }
        return clip_h - (big ? 90 : 65) - 10;
    case rf::NG_TYPE_BAG:
    case rf::NG_TYPE_LMS:
        return clip_h - (big ? 90 : 65) - 10;
    case rf::NG_TYPE_CTF:
    case rf::NG_TYPE_TEAMDM:
    case rf::NG_TYPE_TBAG:
        return clip_h - (big ? 80 : 55) - 10;
    default:
        return clip_h; // no bottom-left cluster
    }
}

static int spectate_raise_hints_above_hud(int hints_y, int line_count, int line_h)
{
    const int block_bottom = hints_y + line_count * line_h;
    const int max_bottom = spectate_bottom_left_hud_top() - line_h; // keep a one-line gap
    if (block_bottom > max_bottom) {
        return max_bottom - line_count * line_h;
    }
    return hints_y;
}

// Draw a column of bind/label hint rows (bind right-aligned at left_x, label at right_x).
static void draw_spectate_hints(const std::pair<const char*, const char*>* hints, int count,
    int left_x, int right_x, int y, int font, int font_h)
{
    for (int i = 0; i < count; ++i) {
        rf::gr::set_color(0xFF, 0xFF, 0xFF, 0xC0);
        rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, left_x, y, hints[i].first, font);
        rf::gr::set_color(0xFF, 0xFF, 0xFF, 0x80);
        rf::gr::string(right_x, y, hints[i].second, font);
        y += font_h;
    }
}

void multi_spectate_render() {
    if (is_hud_effectively_hidden()
        || rf::gameseq_get_state() != rf::GS_GAMEPLAY)
    {
        return;
    }

    // Draw the numpad quick-bind dialog (if open). It doesn't replace the mode HUD - the spectate
    // nameplate stays visible underneath/around it (the two don't overlap on screen).
    spectate_render_bind_dialog();

    if (multi_spectate_is_static()) {
        if (!g_alpine_game_config.spectate_mode_minimal_ui && !g_remote_server_cfg_popup.is_active()) {
            int medium_font = hud_get_default_font();
            int medium_font_h = rf::gr::get_font_height(medium_font);
            int large_font = hud_get_large_font();
            int scr_w = rf::gr::screen_width();
            int scr_h = rf::gr::screen_height();

            rf::Color white_clr{255, 255, 255, 255};
            rf::Color shadow_clr{0, 0, 0, 128};

            int title_x = scr_w / 2;
            int title_y = g_alpine_game_config.big_hud ? 250 : 150;
            draw_with_shadow(
                title_x, title_y, 2, 2, white_clr, shadow_clr,
                [=](int x, int y) {
                    rf::gr::string_aligned(rf::gr::ALIGN_CENTER, x, y, "STATIC CAMERA", large_font);
                }
            );

            // Current-camera subtitle just below the title.
            int large_font_h = rf::gr::get_font_height(large_font);
            std::string cam_subtitle = "Camera " + std::to_string(g_spectate_static_index + 1)
                + " of " + std::to_string(spectate_static_camera_count());
            if (g_spectate_static_index >= rf::fixed_camera_count)
                cam_subtitle += " (dropped)";
            rf::gr::set_color(0xFF, 0xFF, 0xFF, 0xB0);
            rf::gr::string_aligned(rf::gr::ALIGN_CENTER, title_x, title_y + large_font_h,
                cam_subtitle.c_str(), medium_font);

            int hints_y = scr_h - (g_alpine_game_config.big_hud ? 200 : 120) + medium_font_h * 2;
            int hints_left_x = g_alpine_game_config.big_hud ? 120 : 70;
            int hints_right_x = g_alpine_game_config.big_hud ? 140 : 80;

            std::string attach_text = get_action_bind_name(
                get_af_control(rf::AlpineControlConfigAction::AF_ACTION_SPECTATE_ATTACH));
            std::string change_text = get_action_bind_name(
                get_af_control(rf::AlpineControlConfigAction::AF_ACTION_SPECTATE_CHANGE_VIEW));
            std::string exit_spec_text = get_action_bind_name(
                get_af_control(rf::AlpineControlConfigAction::AF_ACTION_SPECTATE_TOGGLE));
            std::string spec_menu_text = get_action_bind_name(
                get_af_control(rf::AlpineControlConfigAction::AF_ACTION_SPECTATE_MENU));
            std::string next_cam_text =
                get_action_bind_name(rf::ControlConfigAction::CC_ACTION_PRIMARY_ATTACK);
            std::string prev_cam_text =
                get_action_bind_name(rf::ControlConfigAction::CC_ACTION_SECONDARY_ATTACK);

            std::pair<const char*, const char*> hints[12];
            int nh = 0;
            hints[nh++] = {attach_text.c_str(), "Attach to Player"};
            hints[nh++] = {change_text.c_str(), "Free / Static Camera"};
            hints[nh++] = {next_cam_text.c_str(), "Next Camera"};
            hints[nh++] = {prev_cam_text.c_str(), "Previous Camera"};
            if (g_spectate_static_index >= rf::fixed_camera_count)
                hints[nh++] = {"DEL", "Delete This Camera"};
            hints[nh++] = {"NUM 0-9", "Jump to Camera"};
            hints[nh++] = {"NUM ENTER", "Camera Quick-Binds"};
            hints[nh++] = {spec_menu_text.c_str(), "Open Spectate Options Menu"};
            hints[nh++] = {exit_spec_text.c_str(), "Exit Spectate Mode"};
            hints_y = spectate_raise_hints_above_hud(hints_y, nh, medium_font_h);
            draw_spectate_hints(hints, nh, hints_left_x, hints_right_x, hints_y, medium_font, medium_font_h);
        }
        return;
    }

    if (multi_spectate_is_freelook()) {
        if (!g_alpine_game_config.spectate_mode_minimal_ui && !g_remote_server_cfg_popup.is_active()) {
            int medium_font = hud_get_default_font();
            int medium_font_h = rf::gr::get_font_height(medium_font);
            int large_font = hud_get_large_font();
            int scr_w = rf::gr::screen_width();
            int scr_h = rf::gr::screen_height();

            rf::Color white_clr{255, 255, 255, 255};
            rf::Color shadow_clr{0, 0, 0, 128};

            int title_x = scr_w / 2;
            int title_y = g_alpine_game_config.big_hud ? 250 : 150;
            draw_with_shadow(
                title_x, title_y, 2, 2, white_clr, shadow_clr,
                [=](int x, int y) {
                    rf::gr::string_aligned(rf::gr::ALIGN_CENTER, x, y, "FREELOOK SPECTATE", large_font);
                }
            );

            int hints_left_x = g_alpine_game_config.big_hud ? 120 : 70;
            int hints_right_x = g_alpine_game_config.big_hud ? 140 : 80;

            std::string attach_text = get_action_bind_name(
                get_af_control(rf::AlpineControlConfigAction::AF_ACTION_SPECTATE_ATTACH)
            );
            std::string change_text = get_action_bind_name(
                get_af_control(rf::AlpineControlConfigAction::AF_ACTION_SPECTATE_CHANGE_VIEW)
            );
            std::string exit_spec_text = get_action_bind_name(
                get_af_control(rf::AlpineControlConfigAction::AF_ACTION_SPECTATE_TOGGLE)
            );
            std::string spec_menu_text = get_action_bind_name(
                get_af_control(rf::AlpineControlConfigAction::AF_ACTION_SPECTATE_MENU)
            );

            std::string zoom_text =
                get_action_bind_name(rf::ControlConfigAction::CC_ACTION_PRIMARY_ATTACK);
            std::string drop_text =
                get_action_bind_name(rf::ControlConfigAction::CC_ACTION_SECONDARY_ATTACK);

            std::pair<const char*, const char*> hints[12];
            int nh = 0;
            hints[nh++] = {attach_text.c_str(), "Attach to Player"};
            hints[nh++] = {change_text.c_str(), "Free / Static Camera"};
            hints[nh++] = {zoom_text.c_str(), "Zoom"};
            hints[nh++] = {drop_text.c_str(), "Drop Camera"};
            hints[nh++] = {spec_menu_text.c_str(), "Open Spectate Options Menu"};
            hints[nh++] = {exit_spec_text.c_str(), "Exit Spectate Mode"};
            int hints_y = scr_h - (g_alpine_game_config.big_hud ? 200 : 120) + medium_font_h * 2;
            hints_y = spectate_raise_hints_above_hud(hints_y, nh, medium_font_h);
            draw_spectate_hints(hints, nh, hints_left_x, hints_right_x, hints_y, medium_font, medium_font_h);
        }
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
            const std::string spectate_bind_text = get_action_bind_name(
                get_af_control(rf::AlpineControlConfigAction::AF_ACTION_SPECTATE_TOGGLE)
            );
            const std::string hint_text =
                "Press "
                + spectate_bind_text
                + " to enter Spectate Mode";

            const rf::NetGameType game_type = rf::multi_get_game_type();
            const bool is_ctf = game_type == rf::NG_TYPE_CTF;
            const bool is_tdm = game_type == rf::NG_TYPE_TEAMDM;
            const bool is_koth = game_type == rf::NG_TYPE_KOTH;
            const bool is_dc = game_type == rf::NG_TYPE_DC;
            const bool is_esc = game_type == rf::NG_TYPE_ESC;
            const bool is_rev = game_type == rf::NG_TYPE_REV;
            const bool is_run = game_type == rf::NG_TYPE_RUN;

            const int font_h = rf::gr::get_font_height(medium_font);
            const int y = std::invoke([&] {
                const int low_death_bar_y =
                    rf::gr::clip_height()
                    - static_cast<int>(rf::gr::clip_height() * .125f);
                if (is_koth || is_dc || is_esc || is_rev) {
                    return g_alpine_game_config.death_bars
                        ? std::min(g_multi_hud_cp_strip_y, low_death_bar_y)
                        : g_multi_hud_cp_strip_y;
                } else if (is_run) {
                    const int y = rf::gr::clip_height()
                        - 10
                        - (g_alpine_game_config.big_hud ? 60 : 40);
                    return g_alpine_game_config.death_bars
                        ? std::min(y, low_death_bar_y)
                        : y;
                } else if (is_ctf || is_tdm) {
                    const int y = rf::gr::clip_height()
                        - 10
                        - (g_alpine_game_config.big_hud ? 80 : 55);
                    return g_alpine_game_config.death_bars
                        ? std::min(y, low_death_bar_y)
                        : y;
                } else {
                    return g_alpine_game_config.death_bars
                        ? low_death_bar_y
                        : rf::gr::clip_height();
                }
            });

            rf::gr::set_color(0xFF, 0xFF, 0xFF, 0xC0);
            rf::gr::string(10, y - 10 - font_h, hint_text.c_str(), medium_font);
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

        // Current-view subtitle just below the title.
        const char* view_subtitle = g_spectate_view_mode == SpectateViewMode::first_person
            ? "First Person"
            : (g_spectate_third_person_orbit ? "Third Person (Orbit)" : "Third Person");
        rf::gr::set_color(0xFF, 0xFF, 0xFF, 0xB0);
        rf::gr::string_aligned(rf::gr::ALIGN_CENTER, title_x, title_y + large_font_h,
            view_subtitle, medium_font);

        if (!g_remote_server_cfg_popup.is_active()) {
            int hints_left_x = g_alpine_game_config.big_hud ? 120 : 70;
            int hints_right_x = g_alpine_game_config.big_hud ? 140 : 80;
            std::string attach_text = get_action_bind_name(
                get_af_control(rf::AlpineControlConfigAction::AF_ACTION_SPECTATE_ATTACH)
            );
            std::string change_text = get_action_bind_name(
                get_af_control(rf::AlpineControlConfigAction::AF_ACTION_SPECTATE_CHANGE_VIEW)
            );
            std::string exit_spec_text = get_action_bind_name(
                get_af_control(rf::AlpineControlConfigAction::AF_ACTION_SPECTATE_TOGGLE)
            );
            std::string spec_menu_text = get_action_bind_name(
                get_af_control(rf::AlpineControlConfigAction::AF_ACTION_SPECTATE_MENU)
            );
            std::string next_player_text =
                get_action_bind_name(rf::ControlConfigAction::CC_ACTION_PRIMARY_ATTACK);

            std::pair<const char*, const char*> hints[12];
            int nh = 0;
            hints[nh++] = {attach_text.c_str(), "Detach Camera"};
            hints[nh++] = {change_text.c_str(), "First / Third Person View"};
            hints[nh++] = {next_player_text.c_str(), "Next / Previous Player"};
            if (g_spectate_view_mode == SpectateViewMode::third_person)
                hints[nh++] = {"MOUSE 3", "Toggle Camera Orbit"};
            hints[nh++] = {"NUM 0-9", "Jump to Player"};
            hints[nh++] = {"NUM ENTER", "Player Quick-Binds"};
            hints[nh++] = {spec_menu_text.c_str(), "Open Spectate Options Menu"};
            hints[nh++] = {exit_spec_text.c_str(), "Exit Spectate Mode"};
            int hints_y = scr_h - (g_alpine_game_config.big_hud ? 200 : 120) + medium_font_h * 2;
            hints_y = spectate_raise_hints_above_hud(hints_y, nh, medium_font_h);
            draw_spectate_hints(hints, nh, hints_left_x, hints_right_x, hints_y, medium_font, medium_font_h);
        }
    }

    int small_font = hud_get_small_font();
    int small_font_h = rf::gr::get_font_height(small_font);

    const std::string target_name_str = std::string(g_spectate_mode_target->name.c_str())
        + spectate_player_bind_suffix(g_spectate_mode_target);
    const char* target_name = target_name_str.c_str();
    const char* spectating_label = "Spectating:";
    const auto [spectating_label_w, spectating_label_h] =
        rf::gr::get_string_size(spectating_label, small_font);
    auto [target_name_w, target_name_h] =
        rf::gr::get_string_size(target_name, large_font);
    const auto [bot_w, bot_h] = rf::gr::get_string_size(" BOT", small_font);
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
        int bot_y = name_y + (rf::gr::get_font_height(large_font) - rf::gr::get_font_height(small_font)) / 2;
        rf::gr::string(rf::gr::current_string_x, bot_y, " BOT", small_font);
        rf::gr::set_color(saved_color);
    }

    rf::Entity* entity = rf::entity_from_handle(g_spectate_mode_target->entity_handle);

    render_spectate_powerup_icons(entity, bar_x, bar_y, bar_h);

    // Draw next/prev player hints flanking the nameplate bar
    if (!g_alpine_game_config.spectate_mode_minimal_ui && !g_remote_server_cfg_popup.is_active()) {
        std::string prev_player_text =
            get_action_bind_name(rf::ControlConfigAction::CC_ACTION_SECONDARY_ATTACK);
        std::string next_player_text =
            get_action_bind_name(rf::ControlConfigAction::CC_ACTION_PRIMARY_ATTACK);

        int nav_gap = g_alpine_game_config.big_hud ? 16 : 10;

        int nav_content_h = small_font_h * 2 + line_gap;
        int nav_y = bar_y + (bar_h - nav_content_h) / 2;

        // Left side: previous player
        int prev_x = bar_x - nav_gap;
        rf::gr::set_color(0xFF, 0xFF, 0xFF, 0xC0);
        rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, prev_x, nav_y, prev_player_text.c_str(), small_font);
        rf::gr::set_color(0xFF, 0xFF, 0xFF, 0x80);
        rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, prev_x, nav_y + small_font_h + line_gap, "Previous Player", small_font);

        // Right side: next player
        int next_x = bar_x + bar_w + nav_gap;
        rf::gr::set_color(0xFF, 0xFF, 0xFF, 0xC0);
        rf::gr::string_aligned(rf::gr::ALIGN_LEFT, next_x, nav_y, next_player_text.c_str(), small_font);
        rf::gr::set_color(0xFF, 0xFF, 0xFF, 0x80);
        rf::gr::string_aligned(rf::gr::ALIGN_LEFT, next_x, nav_y + small_font_h + line_gap, "Next Player", small_font);
    }

    if (!entity) {
        const PlayerStatsNew* const stats =
            static_cast<PlayerStatsNew*>(g_spectate_mode_target->stats);

        const std::string_view text = std::invoke([&] {
            if (g_spectate_mode_target->is_spectator) {
                return "SPECTATOR";
            } else if (player_is_idle(g_spectate_mode_target)) {
                return "IDLE";
            } else if (!stats->num_deaths) {
                return "NOT IN ROUND";
            } else {
                return "DEAD";
            }
        });

        const rf::Color red_clr{0xF0, 0x20, 0x10, 0xC0};
        draw_with_shadow(
            scr_w / 2,
            scr_h / 2,
            2,
            2,
            red_clr,
            shadow_clr,
            [=] (const int x, const int y) {
                rf::gr::string_aligned(
                    rf::gr::ALIGN_CENTER,
                    x,
                    y,
                    text.data(),
                    large_font
                );
            }
        );
    }
}
