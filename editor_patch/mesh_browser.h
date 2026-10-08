#pragma once

#include <windows.h>
#include <string>

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
