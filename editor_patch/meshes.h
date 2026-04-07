#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#pragma pack(push, 1)

struct v3d_file_header
{
    int32_t signature;
    int32_t version;
    int32_t num_submeshes;
    int32_t num_all_vertices;
    int32_t num_all_triangles;
    int32_t unknown0;
    int32_t num_all_materials;
    int32_t unknown1;
    int32_t unknown2;
    int32_t num_colspheres;
};

struct v3d_section_header
{
    int32_t type;
    int32_t size;
};

struct v3d_batch_info
{
    uint16_t num_vertices;
    uint16_t num_triangles;
    uint16_t positions_size;
    uint16_t indices_size;
    uint16_t same_pos_vertex_offsets_size;
    uint16_t bone_links_size;
    uint16_t tex_coords_size;
    uint32_t render_flags;
};

struct v3d_material
{
    char diffuse_map_name[32];
    float emissive_factor;
    float unknown[2];
    float ref_cof;
    char ref_map_name[32];
    uint32_t flags;
};

#pragma pack(pop)

// ─── V3D format constants ──────────────────────────────────────────────────

static constexpr int32_t V3M_SIGNATURE = 0x52463344; // 'RF3D'
static constexpr int32_t V3C_SIGNATURE = 0x5246434D; // 'RFCM'
static constexpr int32_t V3D_VERSION   = 0x40000;

enum v3d_section_type
{
    V3D_END       = 0x00000000,
    V3D_SUBMESH   = 0x5355424D, // 'SUBM'
    V3D_COLSPHERE = 0x43535048, // 'CSPH'
    V3D_BONE      = 0x424F4E45, // 'BONE'
    V3D_DUMB      = 0x44554D42, // 'DUMB', removed by ccrunch
};

static constexpr int V3D_ALIGNMENT = 16;
static constexpr int V3D_MAX_LODS = 3;
static constexpr int V3D_MAX_TEXTURES_PER_LOD = 7;
static constexpr int V3D_MAX_SUBMESHES = 8192;
static constexpr int V3D_SUBMESH_NAME_SIZE = 24;
static constexpr int V3D_SUBMESH_TRAILING_ENTRY_SIZE = 28; // name[24] + float(4)
static constexpr uint32_t V3D_LOD_TRIANGLE_PLANES = 0x20;

// v3d_batch_info stores positions_size as uint16_t (num_vertices * 12, aligned to 16).
// 5461 * 12 = 65532, which aligns up to 65536 and overflows uint16_t.
// 5460 * 12 = 65520, already 16-byte aligned, fits in uint16_t.
static constexpr size_t V3D_MAX_BATCH_VERTICES = 5460;

// ─── Functions ─────────────────────────────────────────────────────────────

void meshes_init_paths();
void reload_custom_meshes();

// Find a mesh file (.v3m, .v3c, .vfx, .rfa) on disk by searching mesh directories.
// Returns the full absolute path, or empty string if not found.
// Stock meshes only exist inside VPP archives and won't be found as loose files.
std::string find_mesh_on_disk(const char* filename);

// Extract diffuse texture names from a V3M/V3C file by parsing the binary format.
// Returns a deduplicated list of texture filenames referenced by materials.
std::vector<std::string> extract_v3d_texture_names(const char* filepath);
