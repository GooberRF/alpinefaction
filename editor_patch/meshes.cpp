#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_set>
#include <windows.h>
#include <xlog/xlog.h>
#include <patch_common/MemUtils.h>
#include "meshes.h"
#include "vtypes.h"

// ─── VFS path management ───────────────────────────────────────────────────

static const char* const MESH_EXTENSIONS = ".v3m .v3c .vfx .rfa";
static constexpr int MESH_SUBDIR_MAX_DEPTH = 8;

// VFS slots returned by file_add_path, rescanned on reload
static std::vector<int> g_mesh_path_slots;
// Exe-relative directories (with trailing backslash) probed by find_mesh_on_disk, in priority order
static std::vector<std::string> g_mesh_search_dirs;
// Exe-relative directories (no trailing backslash) already passed to file_add_path.
// file_add_path does not deduplicate: calling it again with an already registered path appends
// the extension list to that slot's extensions instead of creating a new slot, growing it without
// bound. Every path must therefore be registered exactly once.
static std::unordered_set<std::string> g_registered_mesh_dirs;

static std::string get_exe_dir()
{
    char exe_path[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return {};
    char* last_sep = std::strrchr(exe_path, '\\');
    if (!last_sep) return {};
    *(last_sep + 1) = '\0';
    return std::string{exe_path};
}

// Append every subdirectory of `rel_dir` (exe-relative, no trailing backslash) to `out` as
// exe-relative paths without trailing backslash, recursing depth-first in sorted order.
static void collect_subdirs_recursive(const std::string& exe_dir, const std::string& rel_dir, int depth, std::vector<std::string>& out)
{
    if (depth >= MESH_SUBDIR_MAX_DEPTH) return;

    std::string search_pattern = exe_dir + rel_dir + "\\*";
    WIN32_FIND_DATAA find_data;
    HANDLE find_handle = FindFirstFileA(search_pattern.c_str(), &find_data);
    if (find_handle == INVALID_HANDLE_VALUE) return;

    std::vector<std::string> names;
    do {
        if (!(find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            continue;
        }
        // Reparse points can form cycles that would make this recursion unbounded
        if (find_data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            continue;
        }
        if (std::strcmp(find_data.cFileName, ".") == 0 || std::strcmp(find_data.cFileName, "..") == 0) {
            continue;
        }
        names.emplace_back(find_data.cFileName);
    } while (FindNextFileA(find_handle, &find_data));
    FindClose(find_handle);

    // Sorted so registration order (and therefore VFS lookup order) is deterministic
    std::sort(names.begin(), names.end());

    for (const auto& name : names) {
        std::string sub_rel = rel_dir + "\\" + name;
        out.push_back(sub_rel);
        collect_subdirs_recursive(exe_dir, sub_rel, depth + 1, out);
    }
}

// Enumerate every subdirectory (any depth) of the exe-relative mesh root `root`
static std::vector<std::string> enumerate_mesh_subdirs(const char* root)
{
    std::vector<std::string> subdirs;
    std::string exe_dir = get_exe_dir();
    if (exe_dir.empty()) return subdirs;
    collect_subdirs_recursive(exe_dir, root, 0, subdirs);
    return subdirs;
}

// Register `rel_dir` with the VFS unless it is already registered, recording it for reload.
static void register_mesh_dir(const std::string& rel_dir)
{
    if (!g_registered_mesh_dirs.insert(rel_dir).second) return;

    int slot = file_add_path(rel_dir.c_str(), MESH_EXTENSIONS, false);
    if (slot >= 0) {
        g_mesh_path_slots.push_back(slot);
    }
    else {
        xlog::warn("Failed to register mesh path '{}' (VFS path table full)", rel_dir);
    }
}

void meshes_init_paths()
{
    register_mesh_dir("red\\meshes");
    register_mesh_dir("user_maps\\meshes");

    // user_maps\meshes is searched first so map-local meshes win over the stock tree
    g_mesh_search_dirs.emplace_back("user_maps\\meshes\\");
    g_mesh_search_dirs.emplace_back("red\\meshes\\");

    // Both roots stay ahead of every subdirectory (a mesh sitting directly in a root is by far the
    // common case), and within the subdirectories user_maps still wins over the stock tree
    std::vector<std::string> subdirs = enumerate_mesh_subdirs("user_maps\\meshes");
    std::vector<std::string> red_subdirs = enumerate_mesh_subdirs("red\\meshes");
    subdirs.insert(subdirs.end(), red_subdirs.begin(), red_subdirs.end());

    for (const auto& subdir : subdirs) {
        register_mesh_dir(subdir);
        g_mesh_search_dirs.push_back(subdir + "\\");
    }

    xlog::info("Registered {} mesh subdirectories under user_maps\\meshes and red\\meshes", subdirs.size());
}

void reload_custom_meshes()
{
    // Pick up subdirectories created since init; already registered ones must be skipped
    for (const char* root : {"user_maps\\meshes", "red\\meshes"}) {
        for (const auto& subdir : enumerate_mesh_subdirs(root)) {
            if (g_registered_mesh_dirs.count(subdir)) continue;
            register_mesh_dir(subdir);
            g_mesh_search_dirs.push_back(subdir + "\\");
            xlog::info("Registered new mesh subdirectory '{}'", subdir);
        }
    }

    for (int slot : g_mesh_path_slots) {
        file_scan_path(slot);
    }
}

// ─── Mesh file disk lookup ─────────────────────────────────────────────────

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

    std::string exe_dir = get_exe_dir();
    if (exe_dir.empty()) return {};

    for (const auto& search_dir : g_mesh_search_dirs) {
        std::string full_path = exe_dir + search_dir + bare;
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

// ─── VFX texture extraction ────────────────────────────────────────────────

// Consume the version-gated header fields that follow the signature and version.
static bool skip_vfx_header_fields(FILE* fp, uint32_t ver)
{
    int num_fields = 1 + 6; // num_materials + per-chunk-type counts
    if (ver >= 0x30008) num_fields += 1;
    if (ver >= 0x3000f) num_fields += 1;
    if (ver >= 0x40000) num_fields += 1;
    if (ver >= 0x40002) num_fields += 1;
    if (ver >= 0x40003) num_fields += 1;
    if (ver >= 0x40005) num_fields += 1;
    if (!fskip(fp, num_fields * 4)) return false;

    // Older versions have an extra field here
    if (ver < 0x3000a && !fskip(fp, 4)) return false;

    num_fields = 5 + 5;
    if (ver >= 0x3000d) num_fields += 1;
    if (ver >= 0x30009) num_fields += 5;
    if (ver >= 0x3000f) num_fields += 1;
    return fskip(fp, num_fields * 4);
}

// Read a zero-terminated name. The engine clamps what it stores to 32 chars but
// always consumes up to the terminator.
static bool read_vfx_name(FILE* fp, char* out, size_t out_size)
{
    size_t len = 0;
    char ch;
    do {
        if (!fread_exact(&ch, 1, fp)) return false;
        if (ch != '\0' && len + 1 < out_size) out[len++] = ch;
    } while (ch != '\0');
    out[len] = '\0';
    return true;
}

static bool skip_vfx_name(FILE* fp)
{
    char discard[1];
    return read_vfx_name(fp, discard, sizeof(discard));
}

// Read a texture name and keep it if it names a file.
// Names starting with '$' are internal refs ("$original_map"), not files.
static bool read_vfx_texture_name(FILE* fp, std::vector<std::string>& out)
{
    char name[VFX_MATERIAL_NAME_SIZE];
    if (!read_vfx_name(fp, name, sizeof(name))) return false;

    if (name[0] != '\0' && name[0] != '$') {
        out.emplace_back(name);
    }
    return true;
}

// Parse a MATL chunk far enough to collect its texture names; the caller seeks
// past the remainder using the chunk size.
static bool parse_vfx_material(FILE* fp, uint32_t ver, std::vector<std::string>& out)
{
    int32_t mat_type;
    if (!fread_exact(&mat_type, 4, fp)) return false;
    if (ver >= 0x40003 && !fskip(fp, 4)) return false;

    // Other material types carry no texture names
    if (mat_type != 0 && mat_type != 1) return true;

    if (!fskip(fp, 1)) return false;
    if (!read_vfx_texture_name(fp, out)) return false;
    if (!fskip(fp, 12)) return false;

    // Type 1 (vmix) blends a second texture over a list of mix frames
    if (mat_type == 1) {
        if (!read_vfx_texture_name(fp, out)) return false;
        if (!fskip(fp, 12)) return false;

        int32_t num_mix_frames;
        if (!fread_exact(&num_mix_frames, 4, fp)) return false;
        if (ver < 0x40003 && !fskip(fp, 4)) return false;
        if (num_mix_frames > VFX_MAX_MIX_FRAMES) return false;
        if (num_mix_frames > 0 && !fskip(fp, num_mix_frames * 4)) return false;
    }

    if (!fskip(fp, 12)) return false;
    return read_vfx_texture_name(fp, out);
}

// Parse a CHNE chunk far enough to collect its glow texture name.
static bool parse_vfx_chain(FILE* fp, uint32_t ver, std::vector<std::string>& out)
{
    if (!skip_vfx_name(fp)) return false; // name
    if (!skip_vfx_name(fp)) return false; // parent_name
    if (!fskip(fp, 1)) return false;      // save_parent

    int32_t num_vertices;
    if (!fread_exact(&num_vertices, 4, fp)) return false;
    if (ver < 0x3000a) {
        if (num_vertices < 0 || num_vertices > VFX_MAX_CHAIN_VERTICES) return false;
        if (!fskip(fp, num_vertices * 12)) return false;
    }

    if (!fskip(fp, 4)) return false; // width
    return read_vfx_texture_name(fp, out);
}

std::vector<std::string> extract_vfx_texture_names(const char* filepath)
{
    std::vector<std::string> textures;

    FILE* fp = std::fopen(filepath, "rb");
    if (!fp) return textures;

    long file_size = 0;
    if (std::fseek(fp, 0, SEEK_END) == 0) file_size = std::ftell(fp);
    if (file_size <= 0 || std::fseek(fp, 0, SEEK_SET) != 0) {
        std::fclose(fp);
        return textures;
    }

    uint32_t signature, version;
    if (!fread_exact(&signature, 4, fp) || !fread_exact(&version, 4, fp)) {
        std::fclose(fp);
        return textures;
    }
    if (signature != VFX_SIGNATURE) {
        std::fclose(fp);
        return textures;
    }
    // Versions 0x40000-0x40004 are rejected by the engine as incompatible
    if ((version < 0x30000 || version > 0x3ffff) && version < 0x40005) {
        std::fclose(fp);
        return textures;
    }
    if (!skip_vfx_header_fields(fp, version)) {
        std::fclose(fp);
        return textures;
    }

    while (true) {
        uint32_t chunk_id, chunk_size;
        if (!fread_exact(&chunk_id, 4, fp) || !fread_exact(&chunk_size, 4, fp)) {
            break; // end of file
        }

        // chunk_size covers the size field itself, so the payload is 4 bytes shorter
        long data_start = std::ftell(fp);
        if (data_start < 0 || chunk_size < 4 || chunk_size - 4 > static_cast<uint32_t>(file_size - data_start)) {
            xlog::warn("VFX: Invalid chunk size in '{}'", filepath);
            break;
        }
        long next_chunk = data_start + static_cast<long>(chunk_size) - 4;

        if (chunk_id == VFX_CHUNK_MATL) {
            if (!parse_vfx_material(fp, version, textures)) {
                xlog::warn("VFX: Failed to parse material in '{}'", filepath);
                break;
            }
        }
        else if (chunk_id == VFX_CHUNK_CHNE) {
            if (!parse_vfx_chain(fp, version, textures)) {
                xlog::warn("VFX: Failed to parse chain in '{}'", filepath);
                break;
            }
        }

        if (std::fseek(fp, next_chunk, SEEK_SET) != 0) break;
    }

    std::fclose(fp);

    // Deduplicate
    std::sort(textures.begin(), textures.end());
    textures.erase(std::unique(textures.begin(), textures.end()), textures.end());

    return textures;
}
