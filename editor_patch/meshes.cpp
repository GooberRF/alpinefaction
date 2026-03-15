#include <patch_common/MemUtils.h>
#include "meshes.h"

static int g_mesh_path_slots[2] = {-1, -1};

static auto& file_add_path = addr_as_ref<int(const char* path, const char* exts, bool cd)>(0x004C3950);
static auto& file_scan_path = addr_as_ref<void(int slot_index)>(0x004CF800);

void meshes_init_paths()
{
    g_mesh_path_slots[0] = file_add_path("red\\meshes", ".v3m .v3c .vfx .rfa", false);
    g_mesh_path_slots[1] = file_add_path("user_maps\\meshes", ".v3m .v3c .vfx .rfa", false);
}

void reload_custom_meshes()
{
    for (int slot : g_mesh_path_slots) {
        if (slot >= 0) {
            file_scan_path(slot);
        }
    }
}
