#pragma once

#include <cstdint>

namespace rf
{
    struct Entity;
    struct Player;
}

// Timed powerups granted by the item classes in common/alpine_powerup_items.h. The server owns the timers and sends
// each change to the clients, keyed by player id.

// Wire-frozen, append-only. The value is also the powerup's index in alpine_powerup_item_classes.
enum class AlpinePowerup : uint8_t
{
    jetpack = 0,
};

bool powerup_is_active(const rf::Entity* ep, AlpinePowerup powerup);
// Seconds left on the powerup, 0 when `ep` does not hold it.
float powerup_time_left(const rf::Entity* ep, AlpinePowerup powerup);
bool powerup_any_active(AlpinePowerup powerup);
bool item_type_is_alpine_powerup(int item_type);

// Server, RFL item load: -1 drops a powerup item a mutator makes redundant. Every powerup item kept is noted for
// level_has_alpine_powerups.
int powerups_filter_level_item(int item_type);
bool level_has_alpine_powerups();

void powerups_apply_remote_state(uint8_t player_id, uint8_t powerup_id, uint32_t time_left_ms);
void powerups_send_state_to(rf::Player* player);

void powerups_level_init();
void powerups_do_frame();
void powerups_do_patch();
