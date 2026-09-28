#pragma once

#include <vector>
#include "mfc_types.h"

// One world-space LOD0 triangle of an Alpine mesh object, ready to enter the bake's occluder
// tree.
struct MeshOccluderTri
{
    Vector3 v0, v1, v2;
    int uid;
    bool alpha;
};

// Appends every Alpine mesh object's triangles when the level sets "Meshes block light".
// Leaves out untouched when the property is off.
void lightmap_collect_mesh_occluders(std::vector<MeshOccluderTri>& out);

// Appends the triangles of every terrain decoration instance that casts shadows, at its mesh's lowest
// detail level (uid -1), up to a triangle budget, reporting once when the budget cuts them short.
void lightmap_collect_decoration_occluders(std::vector<MeshOccluderTri>& out);

// Drops the per-filename extraction cache; call whenever the occluder trees are released.
void lightmap_mesh_occluders_release();

// Post-bake diagnostics for the last collection.
void lightmap_mesh_occluder_report();
