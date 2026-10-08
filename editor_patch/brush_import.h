#pragma once

#include <vector>
#include <windows.h>
#include "mfc_types.h"

struct CDedLevel;
struct DedMesh;
struct BrushNode;
struct GSolid;

// One face of a brush solid: a planar convex loop of `count` indices into the position list, wound
// so Newell's normal points out of the brush, with texture UVs.
struct BrushSolidFace
{
    int bitmap_id;
    int count; // 3 or 4
    int v[4];
    float uv[4][2];
};

// Builds a brush GSolid holding every position as a vertex and one room owning every face. Faces
// that cannot be planed are dropped and counted in `dropped`. nullptr when allocation fails or no
// face survives.
GSolid* build_brush_solid(const std::vector<Vector3>& positions, const std::vector<BrushSolidFace>& faces,
                          int& dropped);

// Wraps `solid` in a solid detail brush at pos/orient and links it into the level without an undo
// record. Takes ownership of `solid` either way.
BrushNode* insert_detail_solid_brush(CDedLevel* level, GSolid* solid, const Vector3& pos, const Matrix3& orient);

// Brush selection lives in BrushNode::state, not CDedLevel::selection: every other brush is deselected
// and `brushes` selected, then the level is marked for rebuild and redrawn.
void select_inserted_brushes(CDedLevel* level, const std::vector<BrushNode*>& brushes);

// "To Brush" conversion options, gathered by mesh_to_brush_options_dialog.
struct MeshToBrushOptions
{
    bool keep_mesh = false;               // leave the source Mesh object in the level
    bool duplicate_double_sided = false;  // emit a reversed copy of every double-sided face
};

// LOD 0 triangle count of a Mesh object, for the options dialog summary. 0 when the object holds
// no static mesh geometry.
int mesh_to_brush_triangle_count(DedMesh* mesh);

// Options prompt. Returns false when the user cancels. max_mesh_triangle_count is the largest
// per-mesh count in the selection, which is what the per-brush size warning is about.
bool mesh_to_brush_options_dialog(HWND parent, int mesh_count, int triangle_count,
                                  int max_mesh_triangle_count, MeshToBrushOptions& opts);

// Converts each Mesh object into one solid detail brush and inserts it into the level, logging the
// batch's losses once. Meshes without usable LOD 0 geometry are skipped, so the result can be
// shorter than the input; `converted` receives exactly the meshes that produced a brush, in the
// same order, so the caller can delete those and only those.
std::vector<BrushNode*> meshes_to_brushes(CDedLevel* level, const std::vector<DedMesh*>& meshes,
                                          const MeshToBrushOptions& opts,
                                          std::vector<DedMesh*>& converted);
