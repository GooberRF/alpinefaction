#pragma once

#include <cstdint>
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

// ─── Bitmap manager (RED.exe bmpman) ────────────────────────────────────────

struct BitmapEntry {
    static constexpr int TYPE_USER = 3;
    static constexpr int FORMAT_888_RGB = 6;

    char name[32];                  // 0x00
    int name_checksum;              // 0x20
    int handle;                     // 0x24
    uint16_t orig_width;            // 0x28
    uint16_t orig_height;           // 0x2A
    uint16_t width;                 // 0x2C
    uint16_t height;                // 0x2E
    int num_pixels_in_all_levels;   // 0x30
    int bm_type;                    // 0x34
    int animated_entry_type;        // 0x38
    int format;                     // 0x3C
    uint8_t num_levels;             // 0x40
    uint8_t orig_num_levels;        // 0x41
    uint8_t num_levels_in_ext_files;// 0x42
    uint8_t num_frames;             // 0x43
    uint8_t vbm_version;            // 0x44
    char pad_45[3];
    void* locked_data;              // 0x48
    float frames_per_ms;            // 0x4C
    void* locked_palette;           // 0x50
    BitmapEntry* next;              // 0x54
    BitmapEntry* prev;              // 0x58
    char field_5C;                  // 0x5C
    uint8_t cached_material_idx;    // 0x5D
    char pad_5E[2];
    int total_bytes_for_all_levels; // 0x60
    int file_open_unk_arg;          // 0x64
    int resolution_level;           // 0x68

    inline static auto& load = addr_as_ref<int(const char* filename, int a2)>(0x004BBC30);
    inline static auto& handle_to_index = addr_as_ref<int(int bm_handle)>(0x004BB990);
    inline static auto& hash_table = addr_as_ref<BitmapEntry**>(0x014cfc24);
    inline static auto& hash_table_size_m1 = addr_as_ref<int>(0x0057dbb0);
    inline static auto& entries = addr_as_ref<BitmapEntry*>(0x014cfc4c);
};
static_assert(sizeof(BitmapEntry) == 0x6C);
static_assert(offsetof(BitmapEntry, name_checksum) == 0x20);
static_assert(offsetof(BitmapEntry, handle) == 0x24);
static_assert(offsetof(BitmapEntry, width) == 0x2C);
static_assert(offsetof(BitmapEntry, bm_type) == 0x34);
static_assert(offsetof(BitmapEntry, format) == 0x3C);
static_assert(offsetof(BitmapEntry, next) == 0x54);
static_assert(offsetof(BitmapEntry, prev) == 0x58);
static_assert(offsetof(BitmapEntry, total_bytes_for_all_levels) == 0x60);

// ─── D3D texture cache ──────────────────────────────────────────────────────

static auto& gr_d3d_mark_texture_dirty = addr_as_ref<void(int bm_handle)>(0x004F6000);

void reload_custom_textures();
