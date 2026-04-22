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

// ─── Texture browser modal panel (FUN_0046fd10 — refresh-list dialog) ──────

struct TextureListNode {
    TextureListNode* next;      // 0x00
    TextureListNode* prev;      // 0x04
    char name[0x27];            // 0x08 — inline filename (strncpy cap = 39, NUL-terminated)
    uint8_t flag;               // 0x2f
    uint32_t attr_flags;        // 0x30 — bit1 = read-only, bit2 = hidden (from WIN32_FIND_DATA)
    uint32_t file_size;         // 0x34 — secondary sort key in sort-by-size mode
};
static_assert(sizeof(TextureListNode) == 0x38);
static_assert(offsetof(TextureListNode, name) == 0x08);
static_assert(offsetof(TextureListNode, file_size) == 0x34);

// Sentinel for the intrusive list. Layout matches a node ({next, prev}) so that
// node->prev == &sentinel works as the "begin" indicator and unlink-at-tail
// auto-updates `tail` through node->next->prev rewrites.
struct TextureListSentinel {
    TextureListNode* head;      // = &self when empty
    TextureListNode* tail;      // = &self when empty
};

struct TextureBrowserPanel {
    char pad_0[0x278];
    TextureListSentinel master_list;    // 0x278 — currently-displayed entries
    char pad_280[0x34];
    void* category_holder;              // 0x2b4 — VArray<TextureCategory*> at +0x7C
    char pad_2b8[0x04];
    uint8_t listbox_dirty;              // 0x2bc — non-zero forces listbox repaint
};
static_assert(offsetof(TextureBrowserPanel, master_list) == 0x278);
static_assert(offsetof(TextureBrowserPanel, category_holder) == 0x2b4);
static_assert(offsetof(TextureBrowserPanel, listbox_dirty) == 0x2bc);

inline VArray<TextureCategory*>* texture_browser_categories(TextureBrowserPanel* panel)
{
    return reinterpret_cast<VArray<TextureCategory*>*>(
        static_cast<char*>(panel->category_holder) + 0x7C);
}

// FUN_004712d0: __thiscall returning the file-scan flags byte (5 or 6) for this panel.
// Bit 0 selects sort-by-name; passed straight to texture_browser_scan_path.
inline uint8_t texture_browser_get_scan_flags(TextureBrowserPanel* panel)
{
    return AddrCaller{0x004712d0}.this_call<uint8_t>(panel);
}

// per-entry validator (filename substring filter, ext checks, etc.).
// Stock browser-refresh always passes this thunk to the disk-enumerate helper.
using TextureScanValidator = bool(__cdecl*)(void* entry);
static auto& texture_browser_validator = addr_as_ref<bool __cdecl(void* entry)>(0x00470330);

// enumerate `path_handle` directory matching `pattern`, run each entry
// through `validator`, allocate a node per accepted entry, and splice into `out_list`
// (sorted by name when flags bit 0 is set).
static auto& texture_browser_scan_path = addr_as_ref<void __cdecl(
    TextureListSentinel* out_list, int path_handle, const char* pattern,
    uint8_t flags, TextureScanValidator validator)>(0x004c3ec0);

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
