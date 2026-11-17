#pragma once

#include <memory>
#include <string>

struct CDedLevel;
struct DedTerrain;
struct DedTerrainData;
struct EditorVMesh;
struct TerrainCellRect;
struct TerrainGrid;

// The viewport draws the decoration meshes nearest the camera, placed as the game places them, from `data`
// (the terrain's own, or the properties dialog's staged copy). Called after the terrain's preview is drawn.
void terrain_decorations_collect(const DedTerrain& terrain, const DedTerrainData& data);

// After the terrain surfaces pass: places and draws what was collected, then frees the caches of terrains no longer
// in the level and the meshes no decoration names (`staged`, the dialog's data, may be null).
void terrain_decorations_frame_end(CDedLevel& level, const DedTerrainData* staged);

// Whether a paint since the last call left instances or meshes for another paint; clears the flag.
bool terrain_decorations_take_pending_work();

// Cells [x0, x1) x [z0, z1) of the terrain changed (every cell when null): their instances are placed again.
void terrain_decorations_invalidate(const DedTerrain* terrain, const TerrainCellRect* cells);

// A paint stroke or undo swapped the terrain's grid for a copy that differs only where
// terrain_decorations_invalidate says.
void terrain_decorations_rebind_grid(const DedTerrain* terrain, const TerrainGrid* old_grid,
                                     const std::shared_ptr<const TerrainGrid>& new_grid);

// Called before the terrain object is freed.
void terrain_decorations_forget(const DedTerrain* terrain);

// The level is being emptied: every cached instance and mesh is released.
void terrain_decorations_level_reset();

// Terrain Tools' "Show decorations"
bool terrain_decorations_visible();
void terrain_decorations_set_visible(bool visible);

// The static mesh a decoration names, loaded on first use; null when the name is not a valid mesh file name
// or it cannot be loaded (reported once).
EditorVMesh* terrain_decorations_mesh(const std::string& name);
