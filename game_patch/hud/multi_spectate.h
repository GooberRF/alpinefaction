#pragma once

#include <utility>
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
    int static_index = -1;     // detached submode: static camera index, -1 = freelook
};

void multi_spectate_set_target_player(rf::Player* player);
rf::Player* multi_spectate_get_target_player();
SpectateCameraState multi_spectate_get_camera_state();
// Applies a remembered camera state: attaches to target, or enters the remembered static
// camera / freelook when state.attached is false or target is unusable.
void multi_spectate_apply_camera_state(const SpectateCameraState& state, rf::Player* target);
void multi_spectate_appy_patch();
void multi_spectate_after_full_game_init();
void multi_spectate_level_init();
void multi_spectate_render();
// A column of bind / label rows at the spectate hints' place, raised clear of the bottom-left HUD.
void hud_draw_control_hints(const std::pair<const char*, const char*>* hints, int count);
// Right edge of the hint column drawn this frame or the last, 0 when none is up.
int hud_control_hints_right_x();
// The name plaque at the bottom of the screen; demo playback's controls panel draws it as its
// bottom row with multi_spectate_render_nameplate_row (the camera label in free look / static).
int multi_spectate_nameplate_top_y();
int multi_spectate_nameplate_height();
// Width of the stock plaque around the row's text (0 when there is nothing to name).
// bare: just the name, without the "Spectating" lead of the label line (the title centers when
// none is left) or the player's quick-bind numbers.
int multi_spectate_nameplate_row_width(bool bare = false);
void multi_spectate_render_nameplate_row(int center_x, int top_y, bool bare = false);
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
// Attached third person, shoulder or orbit.
bool multi_spectate_is_third_person();
// The spectator's own look input this frame, pumped and drained; no other path reads it while spectating.
void multi_spectate_consume_look_deltas(float& dpitch, float& dyaw);
// Live spectate's ping compensation, once per frame for the local camera, whoever then poses it.
void multi_spectate_povcomp_frame(rf::Camera* camera);
// Re-seeds the third-person orbit from what is on screen, after another camera owned the view.
void multi_spectate_reseed_orbit();
float multi_spectate_get_view_fov_scale();
bool multi_spectate_camera_do_frame(rf::Camera* camera);
bool multi_spectate_execute_action(rf::ControlConfigAction action, bool was_pressed);
void multi_spectate_process_bind_input();
void multi_spectate_sync_crouch_anim();
void multi_spectate_on_obj_update_fire(rf::Entity* entity, bool alt_fire);
void multi_spectate_reset_action_anim_edge_state();
void multi_spectate_resume_reload_anim(rf::Entity* entity, float elapsed_secs);
