#pragma once

// Powerup item classes Alpine Faction appends after the items.tbl entries, shared by the game and the level editor.
// Levels store items by class name, but multiplayer sends the item table index, so entries are append-only. An
// entry's index is its AlpinePowerup id.

struct AlpinePowerupItemClass
{
    const char* class_name;
    const char* hud_msg_name;
    const char* pickup_msg;
    const char* v3d_filename; // static mesh
    int count;                // seconds the powerup lasts
    int respawn_time_s;
};

inline constexpr AlpinePowerupItemClass alpine_powerup_item_classes[] = {
    {"Multi Jetpack", "Jetpack", "Jetpack picked up", "af-jetpack1.v3m", 30, 60},
};
