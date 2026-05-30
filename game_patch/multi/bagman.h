#pragma once

#include "../rf/math/vector.h"
#include "../rf/math/matrix.h"
#include "../rf/os/timestamp.h"

namespace rf
{
    struct Player;
    struct Entity;
    struct Item;
    struct NetAddr;
    struct VifLodMesh;
}

enum class BagState : uint8_t
{
    BS_At_Spawn = 0,
    BS_Carried = 1,
    BS_Dropped = 2,
    BS_Delayed = 3,  // initial spawn delay active; no bag item exists yet
};

struct BagmanInfo
{
    bool active = false;
    bool spawn_known = false;
    rf::Vector3 spawn_pos{}; // home
    rf::Matrix3 spawn_orient{};
    int bag_item_type = -1;
    int bag_item_handle = -1;
    rf::Vector3 bag_pos{}; // last known
    BagState state = BagState::BS_At_Spawn;
    rf::Player* carrier = nullptr;
    rf::Timestamp score_tick;
    rf::Timestamp return_timer;
    rf::Timestamp spawn_delay_timer; // initial bag spawn delay after level start
    rf::Timestamp pickup_unlock_timer; // brief window after spawn during which IF_NO_PICKUP is set
    rf::Timestamp carrier_amp_refresh;
    rf::Timestamp bag_respawn_retry_timer; // throttles re-spawn attempts when item_create failed
    int red_team_score = 0;
    int blue_team_score = 0;
};

extern BagmanInfo g_bagman_info;

void bagman_level_init();
void bagman_level_init_post();
void bagman_do_frame();
void bagman_on_player_disconnect(rf::Player* player);
void bagman_on_entity_will_die(rf::Entity* ep);
int bagman_get_red_team_score();
int bagman_get_blue_team_score();
void bagman_set_red_team_score(int v);
void bagman_set_blue_team_score(int v);
void bagman_force_state_sync_to(rf::Player* player);
void bagman_broadcast_state();
bool bagman_local_player_is_carrier();
bool bagman_viewer_is_carrier_first_person();
bool bagman_get_client_pickup_pos(rf::Vector3* out_pos);
bool bagman_query_pickup_bag_outline(rf::VifLodMesh** out_lod_mesh, rf::Vector3* out_pos, rf::Matrix3* out_orient);
bool bagman_query_carrier_bag_outline(rf::VifLodMesh** out_lod_mesh, rf::Vector3* out_pos, rf::Matrix3* out_orient);
void bagman_tick_pickup_spin();
void bagman_update_dynamic_light();
void bagman_play_return_sound();

void bagman_do_patch();
