#pragma once

#include <cstdint>

namespace rf
{
    struct Player;
    struct Entity;
}

// Replicated Pit role for the scoreboard grouping. Wire values are stable.
enum PitRole : uint8_t
{
    PIT_ROLE_DUELER = 0,
    PIT_ROLE_QUEUED = 1,
    PIT_ROLE_NOT_QUEUED = 2,
};

void pit_level_init();
void pit_level_init_post();
void pit_do_frame();
void pit_on_entity_will_die(rf::Entity* ep);
bool pit_can_player_spawn(rf::Player* player, bool notify = true);
void pit_on_player_disconnect(rf::Player* player);
void pit_handle_queue_request(rf::Player* player, uint8_t action);
void pit_reset_world_items();
void pit_send_queue_state(rf::Player* player);
void pit_broadcast_queue_states();
void pit_send_roster_to(rf::Player* player);
