#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <windows.h>
#include <xlog/xlog.h>
#include <patch_common/MemUtils.h>
#include "meshes.h"
#include "vtypes.h"

// ─── VFS path management ───────────────────────────────────────────────────

static int g_mesh_path_slots[2] = {-1, -1};

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

// ─── Mesh file disk lookup ─────────────────────────────────────────────────

static const char* mesh_search_dirs[] = {
    "user_maps\\meshes\\",
    "red\\meshes\\",
};

static bool has_mesh_extension(const char* filename)
{
    if (!filename || !filename[0]) return false;
    const char* ext = std::strrchr(filename, '.');
    if (!ext) return false;
    return (_stricmp(ext, ".v3m") == 0 ||
            _stricmp(ext, ".v3c") == 0 ||
            _stricmp(ext, ".vfx") == 0 ||
            _stricmp(ext, ".rfa") == 0);
}

std::string find_mesh_on_disk(const char* filename)
{
    if (!has_mesh_extension(filename)) return {};

    // Strip path prefix to get bare filename
    const char* bare = std::strrchr(filename, '\\');
    if (!bare) bare = std::strrchr(filename, '/');
    bare = bare ? bare + 1 : filename;
    if (!bare[0]) return {};

    char exe_dir[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, exe_dir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return {};
    char* last_sep = std::strrchr(exe_dir, '\\');
    if (last_sep) *(last_sep + 1) = '\0';

    for (const char* search_dir : mesh_search_dirs) {
        std::string full_path = std::string(exe_dir) + search_dir + bare;
        if (GetFileAttributesA(full_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return full_path;
        }
    }
    return {};
}

// ─── V3D texture extraction ────────────────────────────────────────────────

// Helpers for sequential binary reading
static bool fread_exact(void* buf, size_t size, FILE* fp)
{
    return std::fread(buf, size, 1, fp) == 1;
}

static bool fskip(FILE* fp, long offset)
{
    return std::fseek(fp, offset, SEEK_CUR) == 0;
}

// Parse a single submesh section, extracting diffuse texture names into `out`.
// File position must be immediately after the section header.
// Returns true if the submesh was fully parsed, false if malformed.
static bool parse_submesh_textures(FILE* fp, std::vector<std::string>& out)
{
    // name[24] + unknown0[24] + version(4)
    if (!fskip(fp, V3D_SUBMESH_NAME_SIZE * 2 + 4)) return false;

    uint32_t num_lods;
    if (!fread_exact(&num_lods, 4, fp)) return false;
    if (num_lods == 0 || num_lods > V3D_MAX_LODS) return false;

    // lod_distances[num_lods] + offset(vec3=12) + radius(4) + aabb(2*vec3=24)
    if (!fskip(fp, static_cast<long>(num_lods) * 4 + 40)) return false;

    // Walk each LOD's variable-length data
    for (uint32_t lod = 0; lod < num_lods; lod++) {
        uint32_t lod_flags, num_vertices;
        uint16_t num_batches;
        uint32_t data_size;

        if (!fread_exact(&lod_flags, 4, fp)) return false;
        if (!fread_exact(&num_vertices, 4, fp)) return false;
        if (!fread_exact(&num_batches, 2, fp)) return false;
        if (!fread_exact(&data_size, 4, fp)) return false;

        // Sanity check: data_size should be reasonable for a mesh geometry blob
        if (data_size > 64 * 1024 * 1024) return false;

        // Skip: geometry data blob + unknown1(4) + batch_info array
        if (!fskip(fp, static_cast<long>(data_size) + 4 + static_cast<long>(num_batches) * sizeof(v3d_batch_info)))
            return false;

        uint32_t num_prop_points, num_textures;
        if (!fread_exact(&num_prop_points, 4, fp)) return false;
        if (!fread_exact(&num_textures, 4, fp)) return false;
        if (num_textures > V3D_MAX_TEXTURES_PER_LOD) return false;

        // LOD textures: each is 1-byte material index + zero-terminated filename
        // (filename is a copy of diffuse_map_name which is max 32 bytes including NUL)
        for (uint32_t t = 0; t < num_textures; t++) {
            uint8_t id;
            if (!fread_exact(&id, 1, fp)) return false;
            char ch;
            int len = 0;
            do {
                if (!fread_exact(&ch, 1, fp)) return false;
                if (++len > 32) return false;
            } while (ch != '\0');
        }
    }

    // Read materials array
    uint32_t num_materials;
    if (!fread_exact(&num_materials, 4, fp)) return false;
    if (num_materials > 1000) return false;

    for (uint32_t m = 0; m < num_materials; m++) {
        v3d_material mat;
        if (!fread_exact(&mat, sizeof(mat), fp)) return false;

        mat.diffuse_map_name[31] = '\0';
        if (mat.diffuse_map_name[0] != '\0') {
            out.emplace_back(mat.diffuse_map_name);
        }
    }

    // Trailing section: count(4) + entries[n] (name[24] + float(4) each)
    uint32_t num_trailing;
    if (!fread_exact(&num_trailing, 4, fp)) return false;
    if (num_trailing > 100) return false;
    if (!fskip(fp, num_trailing * V3D_SUBMESH_TRAILING_ENTRY_SIZE)) return false;

    return true;
}

std::vector<std::string> extract_v3d_texture_names(const char* filepath)
{
    std::vector<std::string> textures;

    FILE* fp = std::fopen(filepath, "rb");
    if (!fp) return textures;

    v3d_file_header hdr;
    if (!fread_exact(&hdr, sizeof(hdr), fp)) {
        std::fclose(fp);
        return textures;
    }
    if (hdr.signature != V3M_SIGNATURE && hdr.signature != V3C_SIGNATURE) {
        std::fclose(fp);
        return textures;
    }
    if (hdr.version != V3D_VERSION) {
        std::fclose(fp);
        return textures;
    }
    if (hdr.num_submeshes < 0 || hdr.num_submeshes > V3D_MAX_SUBMESHES) {
        std::fclose(fp);
        return textures;
    }
    if (hdr.num_colspheres < 0 || hdr.num_colspheres > 1000) {
        std::fclose(fp);
        return textures;
    }

    // Total sections: submeshes + colspheres + bone(0-1) + V3D_END + margin for unknown types
    int max_sections = hdr.num_submeshes + hdr.num_colspheres + 4;
    for (int s = 0; s < max_sections; s++) {
        v3d_section_header sec;
        if (!fread_exact(&sec, sizeof(sec), fp)) break;

        if (sec.type == V3D_END) break;

        if (sec.type != V3D_SUBMESH) {
            // BONE, COLSPHERE, etc. — skip using section size
            if (sec.size > 0 && !fskip(fp, sec.size)) break;
            continue;
        }

        if (!parse_submesh_textures(fp, textures)) {
            xlog::warn("V3D: Failed to parse submesh textures in '{}'", filepath);
            break;
        }
    }

    std::fclose(fp);

    // Deduplicate
    std::sort(textures.begin(), textures.end());
    textures.erase(std::unique(textures.begin(), textures.end()), textures.end());

    return textures;
}
