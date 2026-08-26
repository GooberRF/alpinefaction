#pragma once

#include "../rf/player/control_config.h"

namespace rf
{
    struct Player;
    struct Entity;
    struct Camera;
}

// Snapshot of the user-visible spectate camera group, used by demo playback to
// persist the camera across seeks/session restarts.
struct SpectateCameraState
{
    bool attached = false;     // following a player (vs freelook/static)
    bool third_person = false; // attached submode
};

void multi_spectate_set_target_player(rf::Player* player);
rf::Player* multi_spectate_get_target_player();
SpectateCameraState multi_spectate_get_camera_state();
// Applies a remembered camera state: attaches to target, or enters freelook when
// state.attached is false or target is unusable.
void multi_spectate_apply_camera_state(const SpectateCameraState& state, rf::Player* target);
void multi_spectate_appy_patch();
void multi_spectate_after_full_game_init();
void multi_spectate_level_init();
void multi_spectate_render();
void multi_spectate_on_player_kill(rf::Player* player, rf::Player* killer);
void multi_spectate_on_destroy_player(rf::Player* player);
void multi_spectate_player_create_entity_post(rf::Player* player, rf::Entity* entity);
bool multi_spectate_is_spectating();
bool multi_spectate_is_first_person();
bool multi_spectate_is_following_player();
void multi_spectate_enter_freelook();
void multi_spectate_toggle_attach();
void multi_spectate_change_view();
void multi_spectate_toggle();
void multi_spectate_leave();
bool multi_spectate_is_freelook();
bool multi_spectate_is_static();
bool multi_spectate_is_third_person_orbit();
float multi_spectate_get_view_fov_scale();
bool multi_spectate_camera_do_frame(rf::Camera* camera);
bool multi_spectate_execute_action(rf::ControlConfigAction action, bool was_pressed);
void multi_spectate_process_bind_input();
void multi_spectate_sync_crouch_anim();
void multi_spectate_on_obj_update_fire(rf::Entity* entity, bool alt_fire);
void multi_spectate_reset_action_anim_edge_state();
