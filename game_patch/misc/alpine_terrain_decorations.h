#pragma once

#include <vector>
#include "../rf/math/vector.h"
#include <common/terrain/alpine_terrain_decorations_runtime.h>
#include "alpine_terrain.h"
#include "decoration_mesh_cache.h"

namespace rf
{
    struct GRoom;
}

// Places every resolved terrain's decorations and loads their meshes. Level init only, on a client that
// renders: mesh loads at render time corrupt the bitmap manager.
void alpine_terrain_decorations_level_init();
void alpine_terrain_decorations_clear_state();
// Removes the instances within radius of pos on the terrain chunks among rooms. Recorded, so a crater replayed
// before level init still applies.
void alpine_terrain_decorations_notify_crater(const rf::Vector3& pos, float radius,
                                              const std::vector<rf::GRoom*>& rooms);
// Direct3D 8/9: the nearest instances through vmesh_render.
void alpine_terrain_decorations_render_legacy();
void alpine_terrain_decorations_apply_patch();

// Drawing is on and something was placed.
bool alpine_terrain_decorations_active();
// Indexed like alpine_terrain_get_all.
std::vector<TerrainDecorations>& alpine_terrain_decorations_get_all();
// Null for a chunk without instances.
const DecorationChunk* alpine_terrain_decorations_chunk(const AlpineTerrainRoomRef& ref);
const DecorationMesh& alpine_terrain_decorations_mesh(int slot);
// This frame's counters, reset on the first call of a frame.
DecorationFrameStats& alpine_terrain_decorations_frame_stats();
