#pragma once
#include "../rf/multi.h"

// Forward declarations
namespace rf
{
    struct Player;
    struct Entity;
    struct RespawnPoint;
}

void server_init();
void dedi_cfg_init();
bool apply_game_type_for_current_level();
void apply_rules_for_current_level();
std::string get_game_type_string(rf::NetGameType game_type);
std::string get_game_type_string_long(rf::NetGameType game_type);
void server_do_frame();
bool check_server_chat_command(const char* msg, rf::Player* sender);
bool server_is_saving_enabled();
bool server_allow_fullbright_meshes();
bool server_allow_lightmaps_only();
bool server_allow_disable_screenshake();
bool server_no_player_collide();
bool server_location_pinging();
bool server_delayed_spawns();
bool server_allow_disable_muzzle_flash();
bool server_apply_click_limiter();
bool server_allow_unlimited_fps();
bool server_gaussian_spread();
void server_reliable_socket_ready(rf::Player* player);
bool server_weapon_items_give_full_ammo();
void server_add_player_weapon(rf::Player* player, int weapon_type, bool full_ammo);
void multi_reload_weapon_server_side(rf::Player* pp, int weapon_type);
void multi_update_gungame_weapon(rf::Player* player, bool force_notification);
void gungame_on_player_spawn(rf::Player* player);
void update_player_active_status(rf::Player* player);
void player_idle_check(rf::Player* player);
void send_sound_packet_throwaway(rf::Player* target, int sound_id);
void multi_change_level_alpine(const char* filename);
const char* get_rand_level_filename();
void shuffle_level_array();void process_queued_spawn_points_from_items();
std::vector<rf::RespawnPoint> get_new_multi_respawn_points();
