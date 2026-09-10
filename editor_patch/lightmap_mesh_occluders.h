#pragma once

#include <vector>
#include "mfc_types.h"

// One world-space LOD0 triangle of an Alpine mesh object, ready to enter the bake's occluder
// tree. uid names the object that produced it so a later per-object filter has something to
// select on; alpha marks a triangle whose chunk texture carries an alpha channel, which is the
// class the level's "Alpha-textured faces block light" property governs.
struct MeshOccluderTri
{
    Vector3 v0, v1, v2;
    int uid;
    bool alpha;
};

// Appends every Alpine mesh object's triangles when the level sets "Meshes block light".
// Returns false, leaving out untouched, when the property is off.
bool lightmap_collect_mesh_occluders(std::vector<MeshOccluderTri>& out);

// Drops the per-filename extraction cache; call whenever the occluder trees are released.
void lightmap_mesh_occluders_release();

// Post-bake diagnostics for the last collection.
void lightmap_mesh_occluder_report();
