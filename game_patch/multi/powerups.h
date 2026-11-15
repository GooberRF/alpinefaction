#pragma once

#include <cstdint>
#include <vector>
#include <common/alpine_powerup_items.h>

namespace rf
{
    struct Entity;
    struct Player;
}

namespace rf::gr
{
    struct Color;
}

// Timed powerups granted by the item classes in common/alpine_powerup_items.h. The server owns the timers and sends
// each change to the clients, keyed by player id.

// Wire-frozen, append-only. The value is also the powerup's index in alpine_powerup_item_classes, which is why the
// shards, which have no timer and are never sent, are listed.
enum class AlpinePowerup : uint8_t
{
    jetpack = 0,
    haste = 1,
    vampire = 2,
    health_shard = 3,
    armor_shard = 4,
};

// Haste scales movement, jump height and weapon timing by this factor.
inline constexpr float haste_speed_factor = 1.3f;

// The super item classes, highest priority first where one is picked, e.g. as a map's central item.
inline constexpr const char* super_item_class_names[] = {
    "Multi Damage Amplifier",
    "Multi Invulnerability",
    alpine_powerup_item_classes[static_cast<size_t>(AlpinePowerup::haste)].class_name,
    alpine_powerup_item_classes[static_cast<size_t>(AlpinePowerup::vampire)].class_name,
    alpine_powerup_item_classes[static_cast<size_t>(AlpinePowerup::jetpack)].class_name,
    "Multi Super Armor",
    "Multi Super Health",
    "shoulder cannon",
};

// Index of `item_type` in super_item_class_names, or its size when the item is not super.
size_t super_item_priority(int item_type);
bool item_type_is_super(int item_type);

bool powerup_is_active(const rf::Entity* ep, AlpinePowerup powerup);
// Seconds left on the powerup, 0 when `ep` does not hold it.
float powerup_time_left(const rf::Entity* ep, AlpinePowerup powerup);
bool powerup_any_active(AlpinePowerup powerup);
// item_info index of the powerup's item class, -1 when items.tbl left no room for it.
int powerup_item_type(AlpinePowerup powerup);
// The powerup's table colour, black where it has none.
const rf::gr::Color& powerup_color(AlpinePowerup powerup);
// True for the Jetpack, which bots never fly, so they leave its pickups for players.
bool item_type_is_bot_unusable_powerup(int item_type);
bool item_type_is_alpine_shard(int item_type);
// `ms` shortened by Haste when `ep` holds it.
int haste_scale_ms(const rf::Entity* ep, int ms);
// Appends the HUD icon of each powerup `ep` holds that has one.
void powerup_append_hud_icons(const rf::Entity* ep, std::vector<int>& icons);

// RFL item load, on every side: -1 drops a powerup item a mutator makes redundant. Every powerup item kept is noted
// for level_has_alpine_powerups.
int powerups_filter_level_item(int item_type);
bool level_has_alpine_powerups();

void powerups_apply_remote_state(uint8_t player_id, uint8_t powerup_id, uint32_t time_left_ms);
void powerups_send_state_to(rf::Player* player);
// Server: relays a jump by `jumper` to the other clients for its sound, if `jumper` is hasted.
void powerups_server_on_haste_jump(rf::Player* jumper);
void powerups_on_remote_haste_jump(uint8_t player_id);

void powerups_level_init();
// Multiplayer client, after level load: makes the sped-up continuous-fire loops Haste plays, so none is made mid-fight.
void powerups_level_init_post();
void powerups_do_frame();
void powerups_render_hud();
void powerups_do_patch();
