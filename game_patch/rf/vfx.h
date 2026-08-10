#pragma once

#include <cstddef>
#include "math/vector.h"
#include "math/matrix.h"

namespace rf
{
    // VFX sub-object: represents a single triangle face in VFX geometry.
    // These are stored in a shared pool at VfxGeo::sub_objects.
    // Size: 0x90 bytes (stride used in the sub-object pool).
    //
    // Loaded by FUN_0053d0c0 (SFXO chunk parser). Each face stores 3 per-chunk
    // vertex indices at offset 0x14, referencing the chunk's decompressed
    // vertex position array (Vector3 per vertex, stride 0x0C).
    //
    // Fields pinned down from the renderer (FUN_00553ee0 / FUN_00554a80):
    //   0x14  int[3]   per-chunk vertex indices
    //   0x24  float[3] per-corner U, refilled every frame from VfxSfxoRenderObj::face_uvs
    //   0x30  float[3] per-corner V, same source
    //   0x60  facing plane, tested by gr::project_vertex-adjacent FUN_00518460
    //   0x84  ptr[3]   per-corner vertex records (each holds a vertex index at +0x04
    //                  and its lighting colour at +0x19..+0x1B)
    struct VfxSubObject
    {
        char pad_00[0x14];          // 0x00: per-face bookkeeping
        int vertex_indices[3];      // 0x14: triangle corner indices into per-chunk vertex array
        char pad_20[0x70];          // 0x20: uvs, facing plane, vertex records, material refs, etc.
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
        unsigned short flags;       // 0x88: bits 0-1 are load-time booleans, bits 2+ hold the
                                    //       animation frame count (FUN_0053d0c0 stores it as
                                    //       flags = (flags & 3) | num_frames * 4). NOT render flags.
        char pad_8A[0x02];          // 0x8A
        int render_type;            // 0x8C: 0 = triangle mesh, 1 = alternate object
                                    //       (FUN_0053ee90 switches on this)
        char pad_90[0x20];          // 0x90
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
        char pad_DC[0x38];          // 0xDC
        unsigned int render_flags;  // 0x114: render behaviour bits. FUN_0053ee90 sends a chunk with
                                    //        (render_flags & 0x801) down the facing/glow path
                                    //        (camera-aligned sprite) instead of the mesh path.
        char pad_118[0x0C];         // 0x118
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
    // Used as 'this' in FUN_0053ee90 (render setup) and FUN_00553ee0 (gr_d3d_vfx).
    // Constructed by FUN_0054d290, one per VfxSfxoChunk.
    // Vertex positions are decompressed per-frame during animation update.
    //
    // The transform the engine draws this chunk with lives at +0x50 / +0x5C.
    // FUN_0053ee90 copies the pos/orient that vmesh_render was called with into that
    // pair (Vector3::operator= at 0x00409F40, Matrix3::operator= at 0x0040A3B0) before
    // dispatching, and FUN_00553ee0 then feeds it to gr::start_instance (0x00517F00)
    // and hands vertex_positions straight to gr::rotate_vertex (0x00518360). There is
    // no per-chunk offset and no scale in that chain: vertex_positions is plain object
    // space, with the .vfx node hierarchy already baked in by the decompression.
    struct VfxSfxoRenderObj
    {
        VfxSfxoChunk* chunk;        // 0x00: the VfxSfxoChunk this instance renders. FUN_0053ee90
                                    //       reads chunk->render_type / chunk->render_flags off it.
        Vector3 pivot;              // 0x04: chunk pivot in object space. FUN_00553ee0 rotates it by
                                    //       render_orient and adds render_pos to get a world point.
        char pad_10[0x0C];          // 0x10
        unsigned int flags;         // 0x1C: bit 31 marks the chunk hidden - FUN_0053ee90 returns
                                    //       without drawing anything when it is set.
        Vector3 pos_20;             // 0x20: second cached transform pair, also built by FUN_0054d290
        Matrix3 orient_2c;          // 0x2C
        Vector3 render_pos;         // 0x50: object world position this chunk was last drawn at
        Matrix3 render_orient;      // 0x5C: object orientation this chunk was last drawn with
        Vector3* vertex_positions;  // 0x80: decompressed vertex positions, object space (stride 0x0C)
        void* face_uvs;             // 0x84: per-FACE uv array (stride 0x18: float u[3] then float v[3]),
                                    //       copied into VfxSubObject +0x24/+0x30 every frame
        char pad_88[0x08];          // 0x88
        char active;                // 0x90: non-zero if render object is active
        char pad_91[0x07];          // 0x91
    };
    static_assert(sizeof(VfxSfxoRenderObj) == 0x98);
    static_assert(offsetof(VfxSfxoRenderObj, flags) == 0x1C);
    static_assert(offsetof(VfxSfxoRenderObj, render_pos) == 0x50);
    static_assert(offsetof(VfxSfxoRenderObj, render_orient) == 0x5C);
    static_assert(offsetof(VfxSfxoRenderObj, vertex_positions) == 0x80);
    static_assert(offsetof(VfxSfxoRenderObj, active) == 0x90);

    // VFX instance: per-object wrapper stored at VMesh::instance for MESH_TYPE_ANIM_FX.
    // Size: 0x24 bytes. Allocated and constructed by FUN_0054b0d0.
    //
    // FUN_0054d0a0 runs two instance gates before it will dispatch an SFXO chunk:
    //   if ((flags & 4) == 0 || (flags & 8) != 0) {        // entry gate
    //       ... resolve chunk type ...
    //       if ((flags & 1) == 0 || (flags & 8) != 0) {    // SFXO gate
    //           FUN_0053ee90(&sfxo_instances[index], time, pos, orient);
    // so a chunk is drawn only when (flags & 8) != 0 || (flags & 5) == 0.
    // Bit 0 (0x1): suppresses the SFXO (triangle mesh) chunks
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
