#pragma once

#include "vtypes.h"
#include "mfc_types.h"

// Texture category object: VString display name (8 bytes) + int path_handle (4 bytes)
struct TextureCategory {
    VString name;
    int path_handle;
};
static_assert(sizeof(TextureCategory) == 0xC, "TextureCategory size mismatch!");

// Partial layout of the texture mode sidebar panel
struct TextureModePanel {
    char pad_0[0x94];
    int category_index;         // 0x94 — selected category array index
    int custom_path_handle;     // 0x98 — VFS path handle for custom texture enumeration
    char pad_9c[0x08];
    void* texture_manager;      // 0xA4 — pointer to texture manager (category array at +0x7C)
};
static_assert(offsetof(TextureModePanel, category_index) == 0x94);
static_assert(offsetof(TextureModePanel, custom_path_handle) == 0x98);
static_assert(offsetof(TextureModePanel, texture_manager) == 0xA4);

void reload_custom_textures();
