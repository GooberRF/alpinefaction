#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <windows.h>
#include <xlog/xlog.h>
#include <patch_common/MemUtils.h>
#include <common/utils/string-utils.h>
#include "meshes.h"
#include "search_paths.h"
#include "vtypes.h"

// ─── VFS path management ───────────────────────────────────────────────────

static const char* const MESH_EXTENSIONS = ".v3m .v3c .vfx .rfa";

static SearchPathTree g_mesh_paths{MESH_EXTENSIONS, "mesh"};

void meshes_init_paths()
{
    // user_maps\meshes is searched first so map-local meshes win over the stock tree. Both roots stay
    // ahead of every subdirectory (a mesh sitting directly in a root is by far the common case), and
    // within the subdirectories user_maps still wins over the stock tree
    g_mesh_paths.init({"red\\meshes", "user_maps\\meshes"}, {"user_maps\\meshes", "red\\meshes"});
}

void reload_custom_meshes()
{
    g_mesh_paths.reload();
}

static std::string canonical_dir(const std::string& dir)
{
    char full[MAX_PATH];
    DWORD len = GetFullPathNameA(dir.c_str(), MAX_PATH, full, nullptr);
    std::string text = (len > 0 && len < MAX_PATH) ? std::string{full} : dir;

    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        char c = text[i] == '/' ? '\\' : text[i];
        // A UNC prefix is the one place a separator run is meaningful
        if (c == '\\' && i > 1 && !out.empty() && out.back() == '\\') continue;
        out.push_back(c);
    }
    while (out.size() > 1 && out.back() == '\\') out.pop_back();
    return out;
}

// file_scan_path (0x004CF800) lowercases each name it finds, cuts it at its FIRST dot and takes the
// file only if the slot's extension list contains that suffix. A registration outside a scan has to
// decide the same way or it admits files a rescan would drop.
static bool register_in_search_path(const std::string& dir, const std::string& name)
{
    const char* ext = std::strchr(name.c_str(), '.');
    if (!ext) return false;

    char path[1024];
    for (int slot = 0; slot < editor_vfs_path_count; ++slot) {
        if (slot != 0 && !vfs_paths[slot].path) continue;
        const char* exts = vfs_paths[slot].extensions;
        if (!exts || !std::strstr(exts, ext)) continue;
        file_make_path(slot, nullptr, path);
        path[sizeof(path) - 1] = '\0';
        if (canonical_dir(path) != dir) continue;
        file_add_loose_file(name.c_str(), slot);
        xlog::info("Registered '{}' in search path '{}'", name,
                   vfs_paths[slot].path ? vfs_paths[slot].path : "");
        return true;
    }
    return false;
}

bool register_written_file(const char* full_path)
{
    if (!full_path || !full_path[0]) return false;

    const char* bare = std::strrchr(full_path, '\\');
    if (const char* slash = std::strrchr(full_path, '/'); slash && (!bare || slash > bare)) {
        bare = slash;
    }
    if (!bare || !bare[1]) return false;

    const std::string name = string_to_lower(bare + 1);
    const std::string dir =
        canonical_dir(std::string{full_path, static_cast<std::size_t>(bare - full_path)});

    if (register_in_search_path(dir, name)) return true;
    // The directory may have been made after the last scan, in which case it is not a search path
    // yet and the reload is what turns it into one.
    reload_custom_meshes();
    return register_in_search_path(dir, name);
}

// ─── Mesh file disk lookup ─────────────────────────────────────────────────

std::string find_mesh_on_disk(const char* filename)
{
    return g_mesh_paths.find_on_disk(filename);
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
