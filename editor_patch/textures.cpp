#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <windows.h>
#include <xlog/xlog.h>
#include <patch_common/MemUtils.h>
#include <patch_common/FunHook.h>
#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include "mfc_types.h"
#include "level.h"
#include "textures.h"
#include "event.h"

// Subdirectory names registered during init, used by VPP packing fix
static std::vector<std::string> custom_texture_subdirs;
// Texture manager pointer, stored at init for reload support
static void* g_texture_manager = nullptr;

static void register_custom_texture_subdirectories(void* texture_manager)
{
    // Resolve path relative to executable directory
    char exe_dir[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, exe_dir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return;
    char* last_sep = strrchr(exe_dir, '\\');
    if (last_sep) *(last_sep + 1) = '\0';

    std::string search_pattern = std::string(exe_dir) + "user_maps\\textures\\*";
    xlog::info("Enumerating custom texture subdirectories: {}", search_pattern);

    WIN32_FIND_DATAA find_data;
    HANDLE hFind = FindFirstFileA(search_pattern.c_str(), &find_data);
    if (hFind == INVALID_HANDLE_VALUE) {
        xlog::info("No user_maps\\textures directory found (or it is empty)");
        return;
    }

    std::vector<std::string> subdirs;
    do {
        if (!(find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            continue;
        }
        if (strcmp(find_data.cFileName, ".") == 0 || strcmp(find_data.cFileName, "..") == 0) {
            continue;
        }
        subdirs.emplace_back(find_data.cFileName);
    } while (FindNextFileA(hFind, &find_data));
    FindClose(hFind);

    if (subdirs.empty()) {
        xlog::info("No subdirectories found in user_maps\\textures");
        return;
    }

    std::sort(subdirs.begin(), subdirs.end());

    // Store for later use by VPP packing path fix
    custom_texture_subdirs = subdirs;

    auto* category_array = reinterpret_cast<VArray<TextureCategory*>*>(
        static_cast<char*>(texture_manager) + 0x7C);

    // Find "Custom" category index so subcategories can be inserted right after it
    int custom_idx = -1;
    for (int i = 0; i < category_array->get_size(); i++) {
        if (strcmp((*category_array)[i]->name.c_str(), "Custom") == 0) {
            custom_idx = i;
            break;
        }
    }
    int old_size = category_array->get_size();

    for (const auto& dirname : subdirs) {
        std::string display_name = "Custom - " + dirname;
        std::string subdir_path = "user_maps\\textures\\" + dirname;

        // Allocate and construct category object (matches stock FUN_004776b0 pattern)
        void* mem = rf_alloc(sizeof(TextureCategory));
        if (!mem) {
            continue;
        }
        auto* cat = AddrCaller{0x00478740}.this_call<TextureCategory*>(mem);

        // Set the display name via VString::assign_0
        cat->name.assign_0(display_name.c_str());

        // Register the subdirectory path with the VFS
        cat->path_handle = file_add_path(subdir_path.c_str(), ".tga .vbm", false);

        // Append to the manager's category array at this+0x7C
        AddrCaller{0x00491020}.this_call<void>(category_array, cat);

        xlog::info("Registered custom texture category: '{}' (path_handle={})", display_name, cat->path_handle);
    }

    // Reorder: move newly appended subcategories to right after "Custom"
    if (custom_idx >= 0 && category_array->size > old_size) {
        int insert_pos = custom_idx + 1;
        std::rotate(
            category_array->data_ptr + insert_pos,
            category_array->data_ptr + old_size,
            category_array->data_ptr + category_array->size
        );
    }

    xlog::info("Registered {} custom texture subdirectories (total categories: {})",
               subdirs.size(), category_array->size);
}

// Hook FUN_0046ac30: the master config/init function that either loads categories from
// red.cfg or falls through to default initialization. By hooking here (instead of
// init_texture_categories at 0x004778e0), custom subdirectory categories are registered
// regardless of whether red.cfg exists.
void __fastcall texture_config_init_new(void* self, int edx);
FunHook texture_config_init_hook{0x0046ac30, texture_config_init_new};

void __fastcall texture_config_init_new(void* self, int edx)
{
    // Call original: loads from red.cfg if present, otherwise initializes defaults
    texture_config_init_hook.call_target(self, edx);

    // The texture manager (with category array at +0x7C) lives at [self + 0x9C].
    // FUN_0046ac30's this is a parent object; the texture manager sub-object is dereferenced
    // through FUN_0046ad00 -> FUN_00478320([this+0x9C]) -> FUN_004778e0 (init_texture_categories).
    void* texture_manager = *reinterpret_cast<void**>(static_cast<char*>(self) + 0x9C);
    g_texture_manager = texture_manager;
    register_custom_texture_subdirectories(texture_manager);
}

// The texture refresh function (FUN_0046fd10) compares category names with "Custom" to decide
// whether to use file enumeration (for custom textures on disk) vs searching loaded texture groups.
// Our subdirectory categories are named "Custom - <dirname>" which fails the exact match.
// Replace the comparison with a prefix check so "Custom - *" categories use the file path too.
static char __cdecl is_custom_category(VString* name, const char* /*cstr*/)
{
    const char* buf = name->c_str();
    return strncmp(buf, "Custom", 6) == 0 ? 1 : 0;
}

// All call sites in RED.exe where FUN_004b7560 compares a category name against "Custom":
//   0x0046ff05 - texture refresh "All" mode (FUN_0046fd10)
//   0x00470027 - texture refresh specific category (FUN_0046fd10)
//   0x004776e9 - texture category loader at init (FUN_004776b0)
//   0x004774d6 - texture folder serialization for save/VPP (FUN_00477460)
//   0x00445403 - texture browser sidebar category init (FUN_004452f0)
CallHook<char __cdecl(VString*, const char*)> custom_category_check_hook{
    {0x0046ff05, 0x00470027, 0x004776e9, 0x004774d6, 0x00445403},
    is_custom_category
};

// FUN_00477460 saves texture categories to red.cfg. Skip "Custom - " subdirectory
// categories so they don't pollute red.cfg (they're re-added dynamically at startup).
// Inject at 0x004774b2 (after ESI = category pointer, before name is written).
// Jump to 0x0047755d (loop increment) to skip the entry entirely.
CodeInjection config_save_skip_custom_subdirs{
    0x004774b2,
    [](auto& regs) {
        auto* cat = reinterpret_cast<TextureCategory*>(static_cast<int>(regs.esi));
        const char* name = cat->name.c_str();
        if (strncmp(name, "Custom - ", 9) == 0) {
            regs.eip = 0x0047755d;
        }
    }
};

// FUN_004452f0 (texture mode sidebar) uses a fixed path_handle from [dialog+0x98] for all
// custom categories. This is the root "Custom" directory handle. For our subdirectory categories
// ("Custom - <dir>"), we need to use the selected category's own path_handle instead.
// Inject at 0x0044540f to replace: MOV EDX, [ESI+0x98]
// At this point: ESI = dialog object, [ESI+0x94] = selected category index,
//                [ESI+0xa4] = texture manager ptr, category array at tex_mgr+0x7C
CodeInjection sidebar_custom_texture_path_injection{
    0x0044540f,
    [](auto& regs) {
        auto* panel = reinterpret_cast<TextureModePanel*>(static_cast<uintptr_t>(regs.esi));
        auto* cat_array = reinterpret_cast<VArray<TextureCategory*>*>(
            static_cast<char*>(panel->texture_manager) + 0x7C);
        regs.edx = (*cat_array)[panel->category_index]->path_handle;
        // Original MOV EDX,[ESI+0x98] is 6 bytes; continue at next instruction
        regs.eip = 0x00445415;
    },
    false // skip original instruction
};

// FUN_00445910 (texture→category reverse lookup) is called when the texture browser returns.
// It searches loaded texture groups for the selected texture filename. Custom category textures
// are not in any loaded group, so the lookup falls through to a fallback that calls FUN_004cf9a0
// which searches ALL VFS paths. The function succeeds but the code then uses [panel+0x98] (base
// "Custom" path) for file enumeration, so subcategory textures aren't found → "Missing".
// Fix: inject at 0x445a3f (AFTER file_search succeeds). Read the found VFS path slot from
// search_ctx[0], find which category owns it, and update [panel+0x98] to the correct path.
static int g_custom_lookup_cat_index = -1;
static const char* g_custom_lookup_cat_name = nullptr;

CodeInjection texture_reverse_lookup_fix{
    0x445a3f,
    [](auto& regs) {
        g_custom_lookup_cat_index = -1;
        g_custom_lookup_cat_name = nullptr;

        if (!g_texture_manager) return;

        auto stack = static_cast<uintptr_t>(regs.esp);

        // search_ctx lives at [ESP + 0x1c]. FUN_004cf9a0 stores the found VFS path
        // slot index at [search_ctx + 0] (verified at 0x4cfbc3: MOV [EBP], EDI).
        int found_path = *reinterpret_cast<int*>(stack + 0x1c);

        auto* cat_array = reinterpret_cast<VArray<TextureCategory*>*>(
            static_cast<char*>(g_texture_manager) + 0x7C);

        for (int i = 0; i < cat_array->get_size(); i++) {
            TextureCategory* cat = (*cat_array)[i];
            if (cat->path_handle == found_path) {
                // Update panel's path_handle so file enumeration at 0x445a92 uses
                // the correct subdirectory
                auto* panel = reinterpret_cast<TextureModePanel*>(static_cast<uintptr_t>(regs.esi));
                panel->custom_path_handle = found_path;
                g_custom_lookup_cat_index = i;
                g_custom_lookup_cat_name = cat->name.c_str();
                return;
            }
        }
    }
};

// At 0x445a4a in FUN_00445910: the stock code does MOV [ESI+0x94], ECX where ECX = path_handle
// (from [ESI+0x98]) and the stack has a pushed "Custom" string for the combo.
// Fix both: set ECX to the correct category array index, and replace the combo text on stack.
CodeInjection texture_reverse_lookup_index_fix{
    0x445a4a,
    [](auto& regs) {
        if (g_custom_lookup_cat_index >= 0) {
            // Fix category index: ECX is about to be stored into [ESI+0x94]
            regs.ecx = g_custom_lookup_cat_index;

            // Fix combo text: [ESP] = "Custom" pointer, replace with actual category name
            auto stack = static_cast<uintptr_t>(regs.esp);
            *reinterpret_cast<const char**>(stack) = g_custom_lookup_cat_name;

            g_custom_lookup_cat_index = -1;
            g_custom_lookup_cat_name = nullptr;
        }
    }
};

// VPP packfile creation (FUN_004482c0) constructs custom texture paths by combining a
// fixed base directory ("user_maps\textures\") with the bare filename via FUN_004b6ee0.
// For textures in subdirectories, this produces the wrong path. Inject at 0x004485a2
// (after path construction at 0x0044859a, before adding to the file list at 0x004485a7)
// to check if the file exists and search through registered subdirectories if not.
// At this point ESP points to the 8-byte output VString containing the constructed path.
CodeInjection vpp_texture_path_fix{
    0x004485a2,
    [](auto& regs) {
        auto* path_vstr = reinterpret_cast<VString*>(static_cast<int>(regs.esp));
        const char* path = path_vstr->c_str();

        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
            return; // File exists at default path, nothing to fix
        }

        // Find the "user_maps\textures\" segment in the constructed path
        const char* marker = strstr(path, "user_maps\\textures\\");
        if (!marker) {
            return;
        }

        // Split into prefix (everything up to and including "user_maps\textures\")
        // and the bare filename after it
        constexpr size_t marker_len = 19; // strlen("user_maps\\textures\\")
        std::string prefix(path, (marker - path) + marker_len);
        std::string filename(marker + marker_len);

        for (const auto& subdir : custom_texture_subdirs) {
            std::string try_path = prefix + subdir + "\\" + filename;
            if (GetFileAttributesA(try_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
                path_vstr->assign_0(try_path.c_str());
                xlog::info("VPP: Resolved texture '{}' in subdirectory '{}'", filename, subdir);
                return;
            }
        }
    }
};

static bool has_texture_extension(const char* filename)
{
    if (!filename || !filename[0]) return false;
    const char* ext = strrchr(filename, '.');
    if (!ext) return false;
    return (_stricmp(ext, ".tga") == 0 ||
            _stricmp(ext, ".vbm") == 0 ||
            _stricmp(ext, ".dds") == 0);
}

// Add a texture filename to the VPP temp file list if it has a valid texture extension.
// FUN_00438640 is __thiscall(void* list, VString by_value); it checks for duplicates internally.
static void add_texture_to_pack_list(void* temp_list, const char* filename)
{
    if (!has_texture_extension(filename)) return;
    VString str;
    str.assign_0(filename);
    // FUN_00438640 is __thiscall(list, VString by value) where VString = {int, char*}.
    // VString's copy constructor is deleted, so pass the two fields directly — the 8 bytes
    // on the stack match the VString layout. Transfer buffer ownership to the list.
    int ml = str.max_len;
    char* b = str.buf;
    str.max_len = 0;
    str.buf = nullptr;
    AddrCaller{0x00438640}.this_call<int>(temp_list, ml, b);
}

// Clear the RED console log at the start of VPP packfile creation (FUN_0044cb10),
// before "Creating Packfile..." is printed. FUN_00444940 is __thiscall LogDlg_Clear.
CodeInjection vpp_clear_log_injection{
    0x0044cb10,
    [](auto& regs) {
        void* log_dlg = *reinterpret_cast<void**>(
            *reinterpret_cast<uintptr_t*>(0x006f9e68) + 0x2b4);
        log_dlg_clear(log_dlg);
    }
};

// FUN_0044c7a0 (process single VPP file) fails fatally when fopen returns NULL, which
// causes two bugs: a blank VPP is left on disk, and the file list is never cleaned up
// (FUN_004386f0 only runs on the success path) so packfiles can't be created again.
// Inject at 0x0044c8bf (the fopen-failure error path) to log a warning and skip the
// file instead of failing. The fopen check is before any counting/writing, so all 3
// passes (count, directory, data) consistently skip the same missing files.
CodeInjection vpp_skip_missing_file_injection{
    0x0044c8bf,
    [](auto& regs) {
        auto* vstr = reinterpret_cast<VString*>(static_cast<int>(regs.esp) + 0x20);
        const char* filename = vstr->c_str();

        // Silently skip known stock textures that the editor incorrectly tries to pack
        const char* bare_name = std::strrchr(filename, '\\');
        bare_name = bare_name ? bare_name + 1 : filename;
        if (_stricmp(bare_name, "rock02.tga") != 0) {
            // Only log during the data pass (param_4 != 0, param_5 == 0) to avoid
            // duplicate warnings from count and directory passes
            int file_handle = *reinterpret_cast<int*>(static_cast<int>(regs.esp) + 0x2C);
            int mode = *reinterpret_cast<int*>(static_cast<int>(regs.esp) + 0x30);
            if (file_handle != 0 && mode == 0) {
                xlog::warn("Packfile: Skipping missing file: {}", filename);

                void* log_dlg = *reinterpret_cast<void**>(
                    *reinterpret_cast<uintptr_t*>(0x006f9e68) + 0x2b4);
                log_dlg_append(log_dlg, "Warning: Skipping missing file %s\n", filename);
            }
        }

        // Redirect to success return (return 1) instead of error return (return 0)
        regs.eip = 0x0044c898;
    }
};

// Stock VPP packing (FUN_004482c0) gathers textures from geometry, particle emitters,
// and decals, but misses bolt emitters, room effect liquid surfaces,
// Display_Fullscreen_Image events, and the geomod default crater texture.
// Inject at 0x004484FE (before Loop 5's stock-texture filtering) to add the missing
// textures to the temp list so they get the same filtering and path resolution.
// At this point: EBX = CDedLevel*, [ESP+0x30] = temp file list.

CodeInjection vpp_extra_textures_injection{
    0x004484FE,
    [](auto& regs) {
        auto* level = reinterpret_cast<CDedLevel*>(static_cast<int>(regs.ebx));
        auto* temp_list = reinterpret_cast<void*>(static_cast<int>(regs.esp) + 0x30);

        // Bolt Emitter textures (VString at +0xC4)
        for (int i = 0; i < level->bolt_emitters.get_size(); i++) {
            auto* obj = static_cast<DedBoltEmitter*>(level->bolt_emitters[i]);
            add_texture_to_pack_list(temp_list, obj->bitmap.c_str());
        }

        // Room Effect liquid surface textures (VString at +0xA8, type 2 = Liquid Room)
        for (int i = 0; i < level->room_effects.get_size(); i++) {
            auto* obj = static_cast<DedRoomEffect*>(level->room_effects[i]);
            if (obj->effect_type == static_cast<int>(DedRoomEffectType::Liquid)) {
                add_texture_to_pack_list(temp_list, obj->liquid_bitmap.c_str());
            }
        }

        // Event textures
        for (int i = 0; i < level->events.get_size(); i++) {
            auto* evt = static_cast<DedEvent*>(level->events[i]);
            if (evt->event_type == af_ded_event_to_int(AlpineDedEventID::Display_Fullscreen_Image)) {
                add_texture_to_pack_list(temp_list, evt->str1.c_str());
            }
            else if (evt->event_type == af_ded_event_to_int(AlpineDedEventID::Swap_Textures)) {
                add_texture_to_pack_list(temp_list, evt->str1.c_str());
                add_texture_to_pack_list(temp_list, evt->str2.c_str());
            }
        }

        // Geomod default crater texture (VString at CDedLevel+0x24)
        add_texture_to_pack_list(temp_list, level->geomod_texture.c_str());
    }
};

// ─── Mesh file VPP packing ──────────────────────────────────────────────────
//
// The stock VPP packing function only gathers textures. Custom mesh files
// (.v3m, .v3c, .vfx, .rfa) referenced by Mesh objects and events also need
// to be included. We inject right before the VPP is written (0x004485e2) and
// add mesh file paths to the global file list at 0x006c9ba8.

static const char* mesh_search_dirs[] = {
    "user_maps\\meshes\\",
    "red\\meshes\\",
};

static bool has_mesh_extension(const char* filename)
{
    if (!filename || !filename[0]) return false;
    const char* ext = strrchr(filename, '.');
    if (!ext) return false;
    return (_stricmp(ext, ".v3m") == 0 ||
            _stricmp(ext, ".v3c") == 0 ||
            _stricmp(ext, ".vfx") == 0 ||
            _stricmp(ext, ".rfa") == 0);
}

// Add a mesh file to the global VPP file list (0x006c9ba8) if it exists on disk
// in one of the mesh search directories. Stock meshes only exist inside VPP archives
// and won't be found as loose files, so this naturally filters them out.
static void add_mesh_to_vpp_list(const char* filename)
{
    if (!has_mesh_extension(filename)) return;

    // Strip any path prefix to get bare filename
    const char* bare = std::strrchr(filename, '\\');
    if (!bare) bare = std::strrchr(filename, '/');
    bare = bare ? bare + 1 : filename;
    if (!bare[0]) return;

    // Get exe directory for constructing absolute paths
    char exe_dir[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, exe_dir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return;
    char* last_sep = std::strrchr(exe_dir, '\\');
    if (last_sep) *(last_sep + 1) = '\0';

    for (const char* search_dir : mesh_search_dirs) {
        std::string full_path = std::string(exe_dir) + search_dir + bare;
        if (GetFileAttributesA(full_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            // File exists on disk — add absolute path to VPP file list
            // (matches stock texture path format used by the VPP writer)
            VString str;
            str.assign_0(full_path.c_str());
            int ml = str.max_len;
            char* b = str.buf;
            str.max_len = 0;
            str.buf = nullptr;
            AddrCaller{0x00438640}.this_call<int>(reinterpret_cast<void*>(0x006c9ba8), ml, b);
            xlog::info("VPP: Added mesh file '{}'", full_path);
            return;
        }
    }
}

CodeInjection vpp_mesh_files_injection{
    0x004485e2,
    [](auto& regs) {
        auto* level = CDedLevel::Get();
        if (!level) return;

        // Mesh objects: mesh_filename, state_anim, debris, corpse mesh, corpse state anim
        for (auto* mesh : level->GetAlpineLevelProperties().mesh_objects) {
            add_mesh_to_vpp_list(mesh->mesh_filename.c_str());
            add_mesh_to_vpp_list(mesh->state_anim.c_str());
            add_mesh_to_vpp_list(mesh->clutter_props.debris_filename.c_str());
            add_mesh_to_vpp_list(mesh->clutter_props.corpse_filename.c_str());
            add_mesh_to_vpp_list(mesh->clutter_props.corpse_state_anim.c_str());
        }

        // Events: Switch_Model (str1=mesh), Play_Animation (str1=anim),
        // Mesh_Animate (str1=anim)
        for (int i = 0; i < level->events.get_size(); i++) {
            auto* evt = static_cast<DedEvent*>(level->events[i]);
            if (evt->event_type == af_ded_event_to_int(AlpineDedEventID::Switch_Model)) {
                add_mesh_to_vpp_list(evt->str1.c_str());
            }
            else if (evt->event_type == af_ded_event_to_int(AlpineDedEventID::Play_Animation)) {
                add_mesh_to_vpp_list(evt->str1.c_str());
            }
            else if (evt->event_type == af_ded_event_to_int(AlpineDedEventID::Mesh_Animate)) {
                add_mesh_to_vpp_list(evt->str1.c_str());
            }
        }
    }
};

// ─── Texture reload ─────────────────────────────────────────────────────────

// Reload bitmap manager placeholder entries in-place.
// bm_load creates a 32x32 TYPE_USER placeholder with the texture's name on failure
// (FUN_004bc9c0). Subsequent loads find this cached entry and never retry from disk.
// We reload each placeholder by: temporarily hiding it from name lookup, calling bm_load
// to create a real entry, then copying the real metadata into the original entry so the
// handle stays valid (faces referencing it don't break on save/load).
static void reload_bm_placeholders()
{
    int table_size = BitmapEntry::hash_table_size_m1 + 1;

    struct Placeholder {
        BitmapEntry* entry;
        int original_checksum;
        char name[32];
    };
    std::vector<Placeholder> placeholders;

    // Phase 1: Find placeholders and temporarily invalidate their checksums
    // so bm_load won't match them during the reload pass
    for (int i = 0; i < table_size; i++) {
        BitmapEntry* entry = BitmapEntry::hash_table[i];
        if (!entry) continue;

        // Placeholder signature: TYPE_USER + FORMAT_888_RGB + 32x32
        // bm_create sets width/height (not orig_width/orig_height)
        if (entry->bm_type == BitmapEntry::TYPE_USER &&
            entry->format == BitmapEntry::FORMAT_888_RGB &&
            entry->width == 32 && entry->height == 32) {
            if (entry->name[0] == '\0') continue; // skip already-cleared entries

            Placeholder ph;
            ph.entry = entry;
            ph.original_checksum = entry->name_checksum;
            memcpy(ph.name, entry->name, 31);
            ph.name[31] = '\0';

            // Invalidate checksum so bm_load's name lookup skips this entry.
            // Entry stays in its hash slot (preserving linear probe chain).
            entry->name_checksum = ~ph.original_checksum;

            placeholders.push_back(ph);
        }
    }

    // Phase 2: Reload each placeholder in-place
    int reloaded = 0;
    for (auto& ph : placeholders) {
        int new_handle = BitmapEntry::load(ph.name, -1);

        // bm_load never returns < 0 — if the file can't be read, it creates another
        // placeholder. Check the new entry's type to detect this.
        int new_index = BitmapEntry::handle_to_index(new_handle);
        BitmapEntry* new_entry = &BitmapEntry::entries[new_index];

        if (new_entry->bm_type == BitmapEntry::TYPE_USER) {
            // File still can't be loaded — bm_load created another placeholder.
            // Restore old entry's checksum and invalidate the redundant new one.
            ph.entry->name_checksum = ph.original_checksum;
            new_entry->name_checksum = ~ph.original_checksum;
            continue;
        }

        // Preserve the old entry's handle and linked list pointers
        int old_handle = ph.entry->handle;
        BitmapEntry* old_next = ph.entry->next;
        BitmapEntry* old_prev = ph.entry->prev;

        // Copy all bitmap data from the new (real) entry into the old (placeholder) entry
        memcpy(ph.entry, new_entry, sizeof(BitmapEntry));

        // Restore the fields that must stay tied to the old entry's position
        ph.entry->handle = old_handle;
        ph.entry->next = old_next;
        ph.entry->prev = old_prev;

        // The old entry now has real texture metadata with the original handle.
        // Checksum and name were copied from the new entry (same filename = same values).

        // Invalidate the new entry's checksum so hash lookups find the old entry, not this one
        new_entry->name_checksum = ~ph.original_checksum;

        // Invalidate the cached D3D texture so the renderer recreates it from the real data
        gr_d3d_mark_texture_dirty(old_handle);

        xlog::info("Reloaded bmpman placeholder '{}' in-place (handle=0x{:x})", ph.name, old_handle);
        reloaded++;
    }

    if (!placeholders.empty()) {
        xlog::info("Reloaded {}/{} bmpman placeholder(s)", reloaded, placeholders.size());
    }
}

void reload_custom_textures()
{
    if (!g_texture_manager) return;

    auto* category_array = reinterpret_cast<VArray<TextureCategory*>*>(
        static_cast<char*>(g_texture_manager) + 0x7C);

    for (int i = 0; i < category_array->get_size(); i++) {
        const char* name = (*category_array)[i]->name.c_str();
        if (strncmp(name, "Custom", 6) == 0) {
            int handle = (*category_array)[i]->path_handle;
            if (handle >= 0) {
                file_scan_path(handle);
            }
        }
    }

    // Reload placeholder bitmap entries so previously-failed textures load from disk.
    // This updates entries in-place (same handle) so faces referencing them stay valid.
    reload_bm_placeholders();
}

void ApplyTexturesPatches() {
    texture_config_init_hook.install();
    custom_category_check_hook.install();
    sidebar_custom_texture_path_injection.install();
    texture_reverse_lookup_fix.install();
    texture_reverse_lookup_index_fix.install();
    vpp_texture_path_fix.install();
    vpp_extra_textures_injection.install();
    vpp_skip_missing_file_injection.install();
    vpp_clear_log_injection.install();
    vpp_mesh_files_injection.install();
    config_save_skip_custom_subdirs.install();
}
