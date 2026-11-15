#pragma once

#include <cstdint>

// Powerup item classes Alpine Faction appends after the items.tbl entries, shared by the game and the level editor.
// Levels store items by class name, but multiplayer sends the item table index, so entries are append-only. An
// entry's index is its AlpinePowerup id.

enum class AlpinePowerupEffect : uint8_t
{
    timed,  // held for `count` seconds
    health, // `count` health, up to the Multi Super Health level
    armor,  // `count` armor, up to the Multi Super Armor level
};

struct AlpinePowerupItemClass
{
    const char* class_name;
    const char* hud_msg_name;
    const char* pickup_msg;
    const char* v3d_filename;          // static mesh
    const char* hud_icon_filename;     // added to the stock powerup icon row in table order, null for none
    const char* overlay_filename;      // drawn over the holder's mesh and first-person weapon, null for none
    const char* pickup_sound_filename; // null for the generic powerup pickup sound
    uint32_t color;                    // 0xRRGGBB holder light and overlay tint, 0 for none
    AlpinePowerupEffect effect;
    int count;
    int respawn_time_s;
};

inline constexpr AlpinePowerupItemClass alpine_powerup_item_classes[] = {
    {
        .class_name = "Alpine Jetpack",
        .hud_msg_name = "Jetpack",
        .pickup_msg = "Jetpack picked up",
        .v3d_filename = "af-jetpack1.v3m",
        .hud_icon_filename = "hud_pow_jetpack_0.tga",
        .overlay_filename = nullptr,
        .pickup_sound_filename = nullptr,
        .color = 0,
        .effect = AlpinePowerupEffect::timed,
        .count = 30,
        .respawn_time_s = 60,
    },
    {
        .class_name = "Alpine Haste",
        .hud_msg_name = "Haste",
        .pickup_msg = "Haste picked up",
        .v3d_filename = "af_haste1.v3m",
        .hud_icon_filename = "hud_pow_haste_0.tga",
        .overlay_filename = "af_powerup-bag.vbm",
        .pickup_sound_filename = "af_haste_pickup1.wav",
        .color = 0xFFD020,
        .effect = AlpinePowerupEffect::timed,
        .count = 30,
        .respawn_time_s = 60,
    },
    {
        .class_name = "Alpine Vampire",
        .hud_msg_name = "Vampire",
        .pickup_msg = "Vampire picked up",
        .v3d_filename = "af_vampire1.v3m",
        .hud_icon_filename = "hud_pow_vampire_0.tga",
        .overlay_filename = "af_powerup-bag.vbm",
        .pickup_sound_filename = "af_vampire_pickup1.wav",
        .color = 0xFF2020,
        .effect = AlpinePowerupEffect::timed,
        .count = 30,
        .respawn_time_s = 60,
    },
    {
        .class_name = "Alpine Health Shard",
        .hud_msg_name = "Health Shard",
        .pickup_msg = "Health Shard picked up",
        .v3d_filename = "af_healthshard1.v3m",
        .hud_icon_filename = nullptr,
        .overlay_filename = nullptr,
        .pickup_sound_filename = nullptr,
        .color = 0,
        .effect = AlpinePowerupEffect::health,
        .count = 10,
        .respawn_time_s = 10,
    },
    {
        .class_name = "Alpine Armor Shard",
        .hud_msg_name = "Armor Shard",
        .pickup_msg = "Armor Shard picked up",
        .v3d_filename = "af_armorshard1.v3m",
        .hud_icon_filename = nullptr,
        .overlay_filename = nullptr,
        .pickup_sound_filename = nullptr,
        .color = 0,
        .effect = AlpinePowerupEffect::armor,
        .count = 10,
        .respawn_time_s = 10,
    },
};
