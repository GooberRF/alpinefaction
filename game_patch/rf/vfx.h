#pragma once

#include "math/vector.h"

namespace rf
{
    // VFX sub-object: represents a single triangle face in VFX geometry.
    // These are stored in a shared pool at VfxGeo::sub_objects.
    // Size: 0x90 bytes (stride used in the sub-object pool).
    //
    // Loaded by FUN_0053d0c0 (SFXO chunk parser). Each face stores 3 per-chunk
    // vertex indices at offset 0x14, referencing the chunk's decompressed
    // vertex position array (Vector3 per vertex, stride 0x0C).
    struct VfxSubObject
    {
        char pad_00[0x14];          // 0x00: per-face bookkeeping
        int vertex_indices[3];      // 0x14: triangle corner indices into per-chunk vertex array
        char pad_20[0x70];          // 0x20: colors, normals, face connections, material refs, etc.
    };
    static_assert(sizeof(VfxSubObject) == 0x90);

    // VFX SFXO chunk: one mesh component within a .vfx file.
    // Multiple SFXO chunks may exist per file (e.g. "Box02", "Box03", "Cylinder01").
    // Size: 0x124 bytes. Stored in array at VfxGeo::sfxo_chunks.
    //
    // Loaded by FUN_0053d0c0 (SFXO type) or FUN_0053e440 (CHNE type).
    // Face data references the shared sub-object pool in VfxGeo.
    struct VfxSfxoChunk
    {
        char name[65];              // 0x00: chunk name (null-terminated)
        char parent_name[65];       // 0x41: parent node name (null-terminated)
        char pad_82[0x06];          // 0x82
        unsigned short flags;       // 0x88: chunk flags
        char pad_8A[0x26];          // 0x8A
        int num_vertices;           // 0xB0: vertex count for this chunk
        int num_faces;              // 0xB4: triangle count for this chunk
        VfxSubObject* faces;        // 0xB8: pointer into VfxGeo::sub_objects pool
        int num_materials;          // 0xBC
        int* material_indices;      // 0xC0: indices into VfxGeo material array
        int num_joints;             // 0xC4
        void* joints;               // 0xC8: joint data array (0x2C bytes each)
        void* anim_data;            // 0xCC: per-frame animation entries (0x28 bytes each)
        void* compressed_verts;     // 0xD0: compressed vertex data (ushort[3] per vertex)
        void* vertex_data_ptrs;     // 0xD4: vertex data indirection pointers
        void* vertex_positions;     // 0xD8
        char pad_DC[0x48];          // 0xDC
    };
    static_assert(sizeof(VfxSfxoChunk) == 0x124);

    // VFX geometry base structure: the shared template loaded from .vfx files.
    // Size: 0x134 bytes. Stored in VMesh::mesh for MESH_TYPE_ANIM_FX.
    //
    // Created by FUN_0053c9e0, constructor FUN_0054af80, loaded by FUN_0054b5b0.
    // Contains chunk types: SFXO, CHNE, MMOD, MATL, PART, SELS, ALGT, PRAW, DMMY, ARMC.
    struct VfxGeo
    {
        char filename[64];          // 0x00: vfx filename
        char pad_40[0x04];          // 0x40
        int flags_44;               // 0x44
        VfxSfxoChunk* sfxo_chunks;  // 0x48: array of SFXO/CHNE chunks
        int num_sfxo_chunks;        // 0x4C
        void* algt_chunks;          // 0x50: ALGT chunk array (0xB4 bytes each)
        int num_algt;               // 0x54
        void* part_chunks;          // 0x58: PART particle chunks (0xDC bytes each)
        int num_parts;              // 0x5C
        void* dmmy_chunks;          // 0x60: DMMY dummy chunks (0xAC bytes each)
        int num_dmmy;               // 0x64
        void* praw_chunks;          // 0x68: PRAW chunks (0xE0 bytes each)
        int num_praw;               // 0x6C
        void* armc_chunks;          // 0x70: ARMC chunks (0x94 bytes each)
        int num_armc;               // 0x74
        void* sels_chunks;          // 0x78: SELS chunks (0x90 bytes each)
        int num_sels;               // 0x7C
        void* mmod;                 // 0x80: MMOD mesh model (0x8C bytes, optional)
        int num_materials_total;    // 0x84
        int num_vertices_alloc;     // 0x88: allocated vertex slot count
        void* materials;            // 0x8C: material array (0xC8 bytes each)
        int num_material_indices;   // 0x90
        void* material_index_buf;   // 0x94
        int num_vertex_slots;       // 0x98
        void* vertex_slot_buf;      // 0x9C
        int num_frame_vertices;     // 0xA0
        void* frame_vertex_buf;     // 0xA4
        int total_sub_objects;      // 0xA8: total face count (across all SFXO chunks)
        VfxSubObject* sub_objects;  // 0xAC: shared sub-object (face) pool
        int vertex_count;           // 0xB0: total unique vertex count
        char pad_B4[0x80];          // 0xB4: remaining fields (bones, joints, anim data, etc.)
    };
    static_assert(sizeof(VfxGeo) == 0x134);

    // Per-chunk SFXO render instance: runtime state for one SFXO chunk.
    // Size: 0x98 bytes. Array stored at VfxInstance::sfxo_instances.
    //
    // Used as 'this' in FUN_0053ee90 (render setup) and FUN_00553ee0 (mesh renderer).
    // Constructed by FUN_0054d290, one per VfxSfxoChunk.
    // Vertex positions are decompressed per-frame during animation update.
    struct VfxSfxoRenderObj
    {
        void* base_ptr;             // 0x00: pointer to VfxGeo (dereferenced in FUN_0053ee90)
        char pad_04[0x18];          // 0x04
        int flags;                  // 0x1C: flags (bit 31 checked in FUN_0053ee90)
        char pad_20[0x60];          // 0x20
        Vector3* vertex_positions;  // 0x80: decompressed vertex positions (stride 0x0C)
        void* vertex_normals;       // 0x84: vertex normal/tangent data (stride 0x18)
        char pad_88[0x08];          // 0x88
        char active;                // 0x90: non-zero if render object is active
        char pad_91[0x07];          // 0x91
    };
    static_assert(sizeof(VfxSfxoRenderObj) == 0x98);

    // VFX instance: per-object wrapper stored at VMesh::instance for MESH_TYPE_ANIM_FX.
    // Size: 0x24 bytes. Allocated and constructed by FUN_0054b0d0.
    //
    // FUN_0054d0a0 checks instance flags before rendering:
    //   if ((flags & 4) == 0 || (flags & 8) != 0) { ... render chunks ... }
    // Bit 2 (0x4): set after construction (not yet ready for rendering)
    // Bit 3 (0x8): force-render override
    struct VfxInstance
    {
        int flags;                      // 0x00: instance flags
        int field_04;                   // 0x04: initialized to 0
        VfxGeo* vfx_geo;               // 0x08: pointer to shared VFX geometry
        float field_0c;                 // 0x0C: num_materials * constant
        float field_10;                 // 0x10: num_materials as float (NOT a pointer)
        VfxSfxoRenderObj* sfxo_instances; // 0x14: per-chunk render instances (0x98 each)
        void* algt_instances;           // 0x18: per-ALGT chunk instances (0x3C each)
        void* part_instances;           // 0x1C: per-PART chunk instances (0xA4 each)
        void* dmmy_instances;           // 0x20: per-DMMY chunk instances (0x28 each)
    };
    static_assert(sizeof(VfxInstance) == 0x24);
}
