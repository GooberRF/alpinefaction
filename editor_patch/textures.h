#pragma once

#include "vtypes.h"
#include "mfc_types.h"

// Texture category object: VString display name (8 bytes) + int path_handle (4 bytes)
struct TextureCategory {
    VString name;
    int path_handle;
};
static_assert(sizeof(TextureCategory) == 0xC, "TextureCategory size mismatch!");
