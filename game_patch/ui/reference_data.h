#pragma once

#include <string>
#include <string_view>
#include <vector>

// Entries for the Reference menu, built from the loaded game tables and the descriptions in
// af_reference.toml (alpinefaction.vpp, or a mod's own copy).

enum class ReferenceArea
{
    weapons,
    vehicles,
    gametypes,
    powerups,
};
inline constexpr int reference_area_count = static_cast<int>(ReferenceArea::powerups) + 1;

struct ReferenceStat
{
    std::string label;
    std::string value;    // single player, or the only value
    std::string mp_value; // multiplayer, only where it differs from `value`
};

struct ReferenceEntry
{
    std::string name;
    std::string subtitle;
    std::string description;
    std::string mesh; // preview mesh, empty for none
    float preview_zoom = 1.0f;
    int vehicle_type = -1; // entity type of a vehicle, whose hull carries its tires and weapons as separate meshes
    bool multiplayer_only = false; // stats then hold only multiplayer values, in `value`
    std::vector<ReferenceStat> stats;
};

// Builds every area once, when the game has finished starting up: the lists are of the game as installed, the same
// in and out of a level.
void reference_data_load();
const std::vector<ReferenceEntry>& reference_area_entries(ReferenceArea area);
// Upper case, as the menu column shows it.
const char* reference_area_label(ReferenceArea area);

// Multiplayer characters as the menus name them: af_reference.toml's name for the pc_multi.tbl $Name, else the
// screen name, else the table name, else nullptr.
int mp_character_count();
const char* mp_character_caption(int index);
