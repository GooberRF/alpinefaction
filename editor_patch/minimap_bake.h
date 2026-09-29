#pragma once

#include <string>
#include "mfc_types.h"

struct CDedLevel;

struct MinimapBakeParams
{
    int resolution = 1024;
    // 0, or anything at or below the lowest room, means no cut.
    float cut_height = 0.0f;
    // Bake into these x/z bounds (finite, at least 1 unit wide) instead of the level's.
    bool use_bounds = false;
    Vector3 bounds_min{};
    Vector3 bounds_max{};
};

struct MinimapBakeResult
{
    std::string bitmap_name;
    Vector3 world_min{};
    Vector3 world_max{};
    int faces_drawn = 0;
    int faces_lightmapped = 0;
    // The level was loaded without stock lightmaps and not lit since: brushes were baked unlit.
    bool lightmaps_placeholder = false;
    // Terrains drawn with the level ambient and sun for want of baked lighting
    int terrains_unlit = 0;
    int decorations_drawn = 0;
    int decorations_blended = 0;
    bool cut_applied = false;
    float cut_height = 0.0f;
    double seconds = 0.0;
};

// The file a bake writes: <RF root>\user_maps\textures\<bitmap name>. Fails for an unsaved level.
bool minimap_bake_target(std::string& out_bitmap_name, std::string& out_path, std::string& out_error);

// Renders the level's last built solid top-down into minimap_bake_target's file.
// Image top-left is world (world_min.x, world_max.z), +x right, +z up the image.
bool minimap_bake(CDedLevel& level, const MinimapBakeParams& p, MinimapBakeResult& out, std::string& out_error);
