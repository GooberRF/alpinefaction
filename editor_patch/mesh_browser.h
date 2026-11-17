#pragma once

#include <windows.h>
#include <string>
#include "mfc_types.h"

enum AlpineMeshKind : unsigned
{
    ALPINE_MESH_V3M = 0x1,
    ALPINE_MESH_V3C = 0x2,
    ALPINE_MESH_VFX = 0x4,
    ALPINE_MESH_ANY = ALPINE_MESH_V3M | ALPINE_MESH_V3C | ALPINE_MESH_VFX,
};

bool alpine_browse_mesh(HWND parent, std::string& filename, unsigned kinds = ALPINE_MESH_ANY, std::string* anim = nullptr);

struct EditorVMesh;

// True when the named animation file can drive this character.
bool alpine_anim_playable_on(EditorVMesh* vmesh, const char* anim_name);

// ─── Mesh previews ───────────────────────────────────────────────────────────

void ApplyMeshPreviewPatches();

// A .v3c's base character stays in a table of 64 slots that nothing frees, so a preview takes the slot
// its load claimed into owned_character (left untouched otherwise). mesh_preview_free releases the slot
// unless a later load elsewhere has picked up that character since.
EditorVMesh* mesh_preview_load(const char* filename, void*& owned_character);
void mesh_preview_free(EditorVMesh*& vmesh, void*& owned_character);

// Orbit camera around the mesh's bounding sphere; angles in degrees, distance in radii.
struct MeshPreviewCamera
{
    Vector3 bound_center;
    float bound_radius;
    float yaw;
    float pitch;
    float zoom;
};

// Draws vmesh (none: just the background) into ctrl's client area through gr, in a space of its own
// with fixed lighting; only between viewport frames.
void mesh_preview_draw(HWND ctrl, EditorVMesh* vmesh, const MeshPreviewCamera& camera, const Color& background);
// The viewport background colour of the editor preferences.
Color mesh_preview_editor_background();
