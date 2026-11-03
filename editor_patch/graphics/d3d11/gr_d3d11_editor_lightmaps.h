#pragma once

#include <cstdint>
#include <vector>
#include <common/lightmap/af_lightmap_runtime.h>
#include "../../lightmap_pages.h"

struct GSolid;

namespace rf
{
    struct GSolid;
    struct GFace;
    struct Vector3;
}

// The Alpine lightmap section the Direct3D 11 viewport draws the level solid with: the editor's twin of the game's
// lightmap runtime (game_patch/graphics/af_lightmap.cpp) for the surface and overflow charts. Movers and terrains keep
// their stock lighting and the terrain preview's.

// Draws section `body`, as stored, over the surfaces of `solid`. A bake's body (`fresh`) carries no surface fingerprint
// yet, so its surface count is taken as the solid's. `layer` holds a bake's pages when it still has them, a page per
// buffer, and is consumed; without it the section's layer is decompressed. Nothing happens without a Direct3D 11
// renderer.
void editor_af_lightmap_show(std::vector<std::uint8_t>& body, bool fresh, const GSolid* solid, LightmapPages* layer);
// Stops drawing the section; with `flush_caches` the level's room caches are rebuilt without it.
void editor_af_lightmap_hide(bool flush_caches);
// ~GSolid: a section drawn over this solid's surfaces goes with it.
void editor_af_lightmap_solid_destroyed(const GSolid* solid);

// The shim's af_lightmap bindings (gr_d3d11_bindings.h)
AfLightmapConstants editor_af_lightmap_constants();
bool editor_af_lightmap_face_setup(const rf::GSolid* solid, int surface_index, AfLightmapFace& out);
void editor_af_lightmap_face_texel(const AfLightmapFace& face, const rf::Vector3& pos, float& out_u, float& out_v);
bool editor_af_lightmap_overflow_face(const rf::GSolid* solid, const rf::GFace* face, AfOverflowFace& out);
bool editor_af_lightmap_overflow_texel(const AfOverflowFace& face, const rf::Vector3& pos, float& out_u, float& out_v);
