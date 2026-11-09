#pragma once

#include <cstdint>
#include <span>

struct KillInfoPayload;

namespace rf
{
    struct Entity;
    struct Player;
}

void distribute_effective_health(rf::Entity* ep, float amount, float max_life_cap, float max_armor_cap);
// False if the player has no live entity to heal
bool apply_effective_health_reward(rf::Player* player, float amount);

// Client: stash the server's kill attribution until the matching obj_kill arrives.
void multi_kill_set_pending_attribution(const KillInfoPayload& payload, std::span<const uint8_t> assist_player_ids);
