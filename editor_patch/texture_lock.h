#pragma once

#include <vector>
#include "mfc_types.h"
#include "uv_map.h"

struct BrushNode;
struct GFaceVertex;

// A brush face's texture held in place in the world while the brush's vertices change shape: its UVs as a function
// of world position on its plane, and its corners where they were.
struct FaceTextureLock
{
    struct Corner
    {
        GFaceVertex* face_vertex;
        Vector3 world;
        float u;
        float v;
    };
    std::vector<Corner> corners;
    Vector3 normal;
    Vector3 centroid;
    UvMap map;
};

// The brush's faces as they are now. Faces whose UVs are no flat map of their plane (warped by hand in UV Unwrap, or
// the face bent) are left out and keep following their vertices.
std::vector<FaceTextureLock> texture_lock_capture(const BrushNode& brush);

// New UVs for the captured faces whose shape changed, keeping the texture where it was in the world at the same
// density. A face that moved without changing shape keeps its UVs. `fixed`, when given, is the point the change was
// made about, for faces with no corner left in place.
void texture_lock_apply(BrushNode& brush, const std::vector<FaceTextureLock>& locks, const Vector3* fixed);

void ApplyTextureLockPatches();
