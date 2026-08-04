#pragma once

#include <cstdint>

namespace rf
{
    struct Player;
    struct Vector3;
}

int spray_count();
bool is_valid_spray_id(uint16_t spray_id);
const char* spray_texture_name(uint16_t spray_id);
int spray_get_bitmap(uint16_t id);
void sprays_do_patch();
void sprays_level_init();
void sprays_on_player_destroyed(rf::Player* player);
void sprays_force_state_sync_to(rf::Player* player);
void sprays_handle_spray_action();
void sprays_handle_spray_request(rf::Player* player, uint16_t texture_id, const rf::Vector3& pos, const rf::Vector3& normal);
void sprays_apply_client_state(uint8_t player_id, uint16_t texture_id, const rf::Vector3& pos, const rf::Vector3& normal, bool play_sound);
void sprays_apply_display_toggle();
