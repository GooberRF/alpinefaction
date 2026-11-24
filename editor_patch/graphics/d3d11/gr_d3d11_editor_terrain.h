#pragma once

#include <cstdint>
#include "engine/rf_math.h"
#include <common/gr_d3d11/gr_d3d11_terrain_source.h>

struct TerrainCellRect;

// The terrain preview's Direct3D 11 draw: the game's terrain shader over the terrain's grid, as the game draws its
// chunks. These do nothing without a live Direct3D 11 renderer.
namespace gr::d3d11
{
    struct EditorTerrainDraw
    {
        // The DedTerrain, and the grid its arrays are read from: a grid the preview was not told it was swapped for
        // (editor_terrain_rebind_grid) is copied in full
        const void* key;
        const void* grid;
        TerrainSource source;
        // The same terrain's geometry
        alpine_terrain::GridView view;
        std::uint32_t chunk_cells;
        // Show Just Lightmaps
        bool lightmaps_only;
    };

    // Draws the terrain into the perspective view being painted. False when it cannot (no Direct3D 11 renderer, or its
    // textures could not be made): the preview then draws it itself.
    bool editor_terrain_draw(const EditorTerrainDraw& draw);

    // Weights, overlays or holes of cells `cells` (every cell when null) changed.
    void editor_terrain_cells_changed(const void* key, const TerrainCellRect* cells);
    // Heights or diagonals of vertices `verts` (every vertex when null) changed.
    void editor_terrain_heights_changed(const void* key, const TerrainCellRect* verts);
    // The terrain's grid was swapped for a copy that differs only where the calls above say.
    void editor_terrain_rebind_grid(const void* key, const void* old_grid, const void* new_grid);
    // Reload Textures ran: the layer bitmaps are loaded again.
    void editor_terrain_textures_reloaded();
    // Before the terrain object is freed.
    void editor_terrain_forget(const void* key);
}
