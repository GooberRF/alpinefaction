#pragma once

// RED's mesh, character, .vfx and particle types under the game's rf:: names and members (game_patch/rf/v3d.h,
// vmesh.h, character.h, vfx.h, particle_emitter.h), for the shared Direct3D 11 core. The shim asserts them
// against the editor's own Editor* views (vtypes.h).

#include <cstddef>
#include <cstdint>
#include <patch_common/MemUtils.h>
#include "rf_math.h"
#include "rf_gr.h"

namespace rf
{
    struct GRoom;
    struct V3dMesh;
    struct LodMesh;
    struct V3DCSphere;
    struct MaterialClass;
    struct VifLodMesh;
    struct VifPropPoint;

    struct V3d
    {
        char v3d_filename[65];
        int version;
        int num_meshes;
        V3dMesh *meshes;
        int num_lod_meshes;
        LodMesh *lod_meshes;
        int num_navpoints;
        void *navpoints;
        int num_cspheres;
        V3DCSphere *cspheres;
        int total_vertices_count_ccrunch_zeroed;
        int unk_vertices_array_ccrunch_zeroed;
        int total_triangles_count_ccrunch_zeroed;
        int unk_triangle_array_ccrunch_zeroed;
        int num_mesh_materials;
        MaterialClass *mesh_materials;
        int hdr_field_14_ccrunch_zeroed;
        int unk_array_hdr_field_14_ccrunch_zeroed;
        int field_88;
        int flags;
    };
    static_assert(sizeof(V3d) == 0x90);

    struct V3dMesh
    {
        char name[65];
        char parent_name[65];
        int num_materials;
        MaterialClass *materials;
        VifLodMesh *vu;
    };
    static_assert(sizeof(V3dMesh) == 0x90);
    static_assert(offsetof(V3dMesh, num_materials) == 0x84);
    static_assert(offsetof(V3dMesh, materials) == 0x88);
    static_assert(offsetof(V3dMesh, vu) == 0x8C);

    struct WeightIndexArray
    {
        ubyte weights[4];
        ubyte indices[4];
    };

    struct VifFace
    {
        ushort vindex1;
        ushort vindex2;
        ushort vindex3;
        ushort flags;
    };
    static_assert(sizeof(VifFace) == 0x8);

    struct VifChunk
    {
        gr::Mode mode;
        Vector3 *vecs;
        Vector3 *norms;
        Vector2 *uvs;
        Plane *face_planes;
        VifFace *faces;
        short *same_vertex_offsets;
        WeightIndexArray *wi;
        int texture_idx;
        short *orig_map;
        ushort num_vecs;
        ushort num_faces;
        ushort vecs_alloc;
        ushort faces_alloc;
        ushort uvs_alloc;
        ushort wi_alloc;
        ushort same_vertex_offsets_alloc;
    };
    static_assert(sizeof(VifChunk) == 0x38);

    // render_cache is the game's Alpine field past the stock 0x4C: RED's allocation must be enlarged as the game's
    // is before the core sees a mesh.
    struct VifMesh
    {
        int data_block_size;
        void *data_block;
        VifChunk *chunks;
        ushort num_chunks;
        VifPropPoint *prop_points;
        int num_prop_points;
        ubyte tex_ids[7];
        int tex_handles[7];
        int num_textures_handles;
        int flags;
        int num_original_vecs;
        int unk_field_from_v3d_file;
        void *render_cache;
    };
    static_assert(sizeof(VifMesh) == 0x50);
    static_assert(offsetof(VifMesh, flags) == 0x40);
    static_assert(offsetof(VifMesh, render_cache) == 0x4C);

    constexpr int VIF_FACE_DOUBLE_SIDED = 0x20;

    // render_cache as VifMesh's
    struct VifLodMesh
    {
        int num_levels;
        VifMesh *meshes[3];
        float distances[3];
        Vector3 center;
        float radius;
        Vector3 bbox_min;
        Vector3 bbox_max;
        void *render_cache;
    };
    static_assert(sizeof(VifLodMesh) == 0x48);
    static_assert(offsetof(VifLodMesh, meshes) == 0x4);
    static_assert(offsetof(VifLodMesh, render_cache) == 0x44);

    struct MeshRenderParams
    {
        int flags;
        int lod_level;
        int alpha;
        gr::Color self_illum; // unused
        int *alt_tex;
        float field_14_unk_fire;
        gr::Color field_18;
        ubyte* vertex_colors;
        int powerup_bitmaps[2];
        gr::Color ambient_color;
        Matrix3 orient;
    };
    static_assert(sizeof(MeshRenderParams) == 0x50);
    static_assert(offsetof(MeshRenderParams, ambient_color) == 0x28);

    enum MeshRenderFlags
    {
        MRF_SCANNER_1 = 1,
        MRF_SCANNER_2 = 8,
        MRF_CUSTOM_AMBIENT_COLOR = 0x80,
        MRF_CLIP_VERTICES = 0x100,
        MRF_FIRST_PERSON = 0x400,
    };

    enum VMeshType
    {
        MESH_TYPE_UNINITIALIZED = 0,
        MESH_TYPE_STATIC = 1,
        MESH_TYPE_CHARACTER = 2,
        MESH_TYPE_ANIM_FX = 3,
    };

    struct VMesh
    {
        VMeshType type;
        void* instance;
        void* mesh;
        char filename[65];
        void* replacement_materials;
        bool use_replacement_materials;
    };
    static_assert(sizeof(VMesh) == 0x58);

    struct TexMap
    {
        int tex_handle;
        char name[33];
        int start_frame;
        float playback_rate;
        int anim_type;

        // frame is in 15 fps units; FUN_004f80e0 (RF 0x0054A630)
        int get_bitmap(float frame) const
        {
            return AddrCaller{0x004F80E0}.this_call<int>(this, frame);
        }
    };
    static_assert(sizeof(TexMap) == 0x34);

    struct MeshMaterial
    {
        int material_type;
        int flags;
        bool use_additive_blending;
        Color diffuse_color;
        TexMap texture_maps[2];
        int framerate;
        int num_mix_frames;
        int *mix;
        float specular_level;
        float glossiness;
        float reflection_amount;
        char refl_tex_name[36];
        int refl_tex_handle;
        int num_self_illumination_frames;
        float *self_illumination;
        int num_opacity_frames;
        int *opacity;

        // frame is in 15 fps units; FUN_004f8530 (RF 0x0054AA80)
        float get_opacity(float frame) const
        {
            return AddrCaller{0x004F8530}.this_call<float>(this, frame);
        }

        // FUN_004f8490 (RF 0x0054A9E0)
        float get_self_illumination(float frame) const
        {
            return AddrCaller{0x004F8490}.this_call<float>(this, frame);
        }

        // FUN_004f83e0 (RF 0x0054A930)
        float get_crossfade(float t) const
        {
            return AddrCaller{0x004F83E0}.this_call<float>(this, t);
        }
    };
    static_assert(sizeof(MeshMaterial) == 0xC8);
    static_assert(offsetof(MeshMaterial, diffuse_color) == 0x09);
    static_assert(offsetof(MeshMaterial, texture_maps) == 0x10);
    static_assert(offsetof(MeshMaterial, specular_level) == 0x84);
    static_assert(offsetof(MeshMaterial, refl_tex_handle) == 0xB4);
    static_assert(offsetof(MeshMaterial, self_illumination) == 0xBC);

    struct MvfAnimTrigger
    {
        char name[16] = {0};
        int value = 0;
    };
    static_assert(sizeof(MvfAnimTrigger) == 0x14);

    struct Skeleton
    {
        char mvf_filename[64] = {0};
        MvfAnimTrigger triggers[2];
        int data_size = 0;
        bool internally_allocated = 0;
        int base_usage_count = 0;
        int instance_usage_count = 0;
        void *animation_data = nullptr;

        bool has_morph_vertices() const
        {
            // FUN_005001a0 (RF 0x00539DF0); null for an unloaded skeleton
            const int* morph_data = AddrCaller{0x005001A0}.this_call<const int*>(this);
            return morph_data != nullptr && morph_data[7] > 0;
        }

        // FUN_00500720 (RF 0x0053A370)
        void morph(rf::Vector3 *morphed_vecs, int num_vecs, int time, short *orig_vecs_map, int num_orig_vecs) const
        {
            AddrCaller{0x00500720}.this_call(this, morphed_vecs, num_vecs, time, orig_vecs_map, num_orig_vecs);
        }
    };
    static_assert(sizeof(Skeleton) == 0x7C);

    struct CiAnimInfo
    {
        int anim_index;
        int cur_time;
        float mix_percent;
    };
    static_assert(sizeof(CiAnimInfo) == 0xC);

    struct CiBoneInfo
    {
        short combined_update_iteration;
        short final_update_iteration;
        Matrix3 field_4;
        int field_28;
        float field_2C;
    };
    static_assert(sizeof(CiBoneInfo) == 0x30);

    struct CharacterTag
    {
        const char *name;
        Matrix43 trans;
        int parent_index;
    };

    struct Bone
    {
        char name[24];
        Matrix43 transform;
        int parent;
    };

    struct CharacterMesh
    {
        V3d v3d_file;
        V3dMesh *mesh;
    };

    struct Character
    {
        char name[65];
        int flags;
        int num_bones;
        Bone bones[50];
        ubyte bone_order_list[50];
        int num_anims;
        Skeleton *animations[172];
        bool anim_is_state[172];
        int num_tags;
        CharacterTag tags[32];
        int num_character_meshes;
        CharacterMesh character_meshes[1];
        int root;
    };
    static_assert(sizeof(Character) == 0x1A58);
    static_assert(offsetof(Character, flags) == 0x44);
    static_assert(offsetof(Character, num_bones) == 0x48);
    static_assert(offsetof(Character, animations) == 0xF5C);
    static_assert(offsetof(Character, num_tags) == 0x12B8);

    struct CharacterInstance
    {
        Matrix43 bone_transforms_combined[50];
        Matrix43 bone_transforms_final[50];
        Vector3 root_offset;
        bool fast_anim;
        int num_active_anims;
        CiAnimInfo active_anims[16];
        CiBoneInfo bone_info[50];
        int field_1CF4;
        short iteration_counter;
        int animation_to_freeze_index;
        int animation_alone_index;
        float cyclic_percent;
        Vector3 field_1D08;
        int field_1D14;
        int field_1D18;
        int field_1D1C;
        Vector3 field_1D20;
        Vector3 field_1D2C;
        Vector3 field_1D38;
        bool state_trigger_elapsed[2];
        int dominant_state_index;
        bool holding_last_action_frame;
        Character *base_character;
        CharacterInstance *next;
        CharacterInstance *prev;
    };
    static_assert(sizeof(CharacterInstance) == 0x1D5C);
    static_assert(offsetof(CharacterInstance, bone_transforms_final) == 0x960);
    static_assert(offsetof(CharacterInstance, num_active_anims) == 0x12D0);
    static_assert(offsetof(CharacterInstance, active_anims) == 0x12D4);
    static_assert(offsetof(CharacterInstance, field_1CF4) == 0x1CF4);
    static_assert(offsetof(CharacterInstance, holding_last_action_frame) == 0x1D4C);
    static_assert(offsetof(CharacterInstance, base_character) == 0x1D50);

    struct VfxGeo;
    struct VfxSubObject;

    // Per-vertex normal/lighting record shared by the face corners of one smoothing group
    struct VfxVertexRecord
    {
        char pad_00[0x04];
        int vertex_index; // index into VfxSfxoRenderObj::vertex_positions
        int normal_valid;
        Vector3 normal;
        char lit;
        unsigned char lit_rgb[3];
        char pad_1C[0x08];
        int num_adjacent_faces;
        VfxSubObject** adjacent_faces;
    };
    static_assert(sizeof(VfxVertexRecord) == 0x2C);
    static_assert(offsetof(VfxVertexRecord, normal) == 0x0C);
    static_assert(offsetof(VfxVertexRecord, adjacent_faces) == 0x28);

    struct VfxFaceUv
    {
        float u[3];
        float v[3];
    };
    static_assert(sizeof(VfxFaceUv) == 0x18);

    // One triangle face of .vfx geometry
    struct VfxSubObject
    {
        char pad_00[0x14];
        int vertex_indices[3];
        int material_slot; // index into VfxSfxoChunk::material_indices, -1 for none
        float u[3];
        float v[3];
        char pad_3C[0x24];
        Vector3 normal;
        char pad_6C[0x10];
        unsigned int normal_flags;
        char pad_80[0x04];
        VfxVertexRecord* corner_records[3];
    };
    static_assert(sizeof(VfxSubObject) == 0x90);
    static_assert(offsetof(VfxSubObject, material_slot) == 0x20);
    static_assert(offsetof(VfxSubObject, normal) == 0x60);
    static_assert(offsetof(VfxSubObject, corner_records) == 0x84);

    // One SFXO/CHNE chunk of a .vfx
    struct VfxSfxoChunk
    {
        char name[65];
        char parent_name[65];
        char pad_82[0x02];
        VfxGeo* geo;
        unsigned short flags;
        char pad_8A[0x02];
        int render_type;
        char pad_90[0x10];
        float radius;
        int num_anim_keys;
        float start_time;
        char pad_AC[0x04];
        int num_vertices;
        int num_faces;
        VfxSubObject* faces;
        int num_materials;
        int* material_indices;
        int num_vertex_records;
        VfxVertexRecord* vertex_records;
        void* anim_keys;
        void* compressed_verts;
        void* uv_frames;
        void* vertex_positions;
        char pad_DC[0x30];
        float ribbon_width;
        int glow_bitmap;
        unsigned int render_flags;
        char pad_118[0x0C];
    };
    static_assert(sizeof(VfxSfxoChunk) == 0x124);
    static_assert(offsetof(VfxSfxoChunk, geo) == 0x84);
    static_assert(offsetof(VfxSfxoChunk, radius) == 0xA0);
    static_assert(offsetof(VfxSfxoChunk, start_time) == 0xA8);
    static_assert(offsetof(VfxSfxoChunk, num_vertices) == 0xB0);
    static_assert(offsetof(VfxSfxoChunk, vertex_records) == 0xC8);
    static_assert(offsetof(VfxSfxoChunk, ribbon_width) == 0x10C);
    static_assert(offsetof(VfxSfxoChunk, render_flags) == 0x114);

    // The shared geometry of a .vfx
    struct VfxGeo
    {
        char filename[64];
        char pad_40[0x04];
        int flags_44;
        VfxSfxoChunk* sfxo_chunks;
        int num_sfxo_chunks;
        void* algt_chunks;
        int num_algt;
        void* part_chunks;
        int num_parts;
        void* dmmy_chunks;
        int num_dmmy;
        void* praw_chunks;
        int num_praw;
        void* armc_chunks;
        int num_armc;
        void* sels_chunks;
        int num_sels;
        void* mmod;
        int num_anim_frames;
        int num_vertices_alloc;
        MeshMaterial* materials;
        int num_material_indices;
        void* material_index_buf;
        int num_vertex_slots;
        void* vertex_slot_buf;
        int num_frame_vertices;
        void* frame_vertex_buf;
        int total_sub_objects;
        VfxSubObject* sub_objects;
        int vertex_count;
        char pad_B4[0x80];
    };
    static_assert(sizeof(VfxGeo) == 0x134);
    static_assert(offsetof(VfxGeo, materials) == 0x8C);

    // Per-instance render state of one SFXO chunk
    struct VfxSfxoRenderObj
    {
        VfxSfxoChunk* chunk;
        Vector3 pivot;
        float width;
        float height;
        char pad_18[0x04];
        unsigned int flags;
        Vector3 pos_20;
        Matrix3 orient_2c;
        Vector3 render_pos;
        Matrix3 render_orient;
        Vector3* vertex_positions;
        VfxFaceUv* face_uvs;
        char pad_88[0x08];
        char active;
        char pad_91[0x07];
    };
    static_assert(sizeof(VfxSfxoRenderObj) == 0x98);
    static_assert(offsetof(VfxSfxoRenderObj, flags) == 0x1C);
    static_assert(offsetof(VfxSfxoRenderObj, render_pos) == 0x50);
    static_assert(offsetof(VfxSfxoRenderObj, render_orient) == 0x5C);
    static_assert(offsetof(VfxSfxoRenderObj, vertex_positions) == 0x80);
    static_assert(offsetof(VfxSfxoRenderObj, face_uvs) == 0x84);
    static_assert(offsetof(VfxSfxoRenderObj, active) == 0x90);

    struct ParticleEmitter;

    enum ParticleFlags : unsigned
    {
        PTF_GLOW = 0x2,
        PTF_CLR_CHANGE = 0x4,
        PTF_GRAVITY = 0x8,
        PTF_COLLIDE = 0x10,
        PTF_HIT_CALLBACK = 0x20,
        PTF_ACCELERATE = 0x40,
        PTF_EXPLODE = 0x80,
        PTF_LOOP = 0x100,
        PTF_RANDOM_ORIENT = 0x200,
        PTF_COLLIDE_LIQUID = 0x400,
        PTF_COLLIDE_AND_DIE = 0x800,
        PTF_NO_Z_CHECK = 0x2000,
        PTF_VEL_STRETCH = 0x4000,
    };

    enum ParticleFlags2
    {
        PTF2_DAMAGES = 0x1,
        PTF2_HOLD_LAST_FRAME = 0x4,
        PTF2_FIRE_DAMAGE = 0x8,
    };

    struct ParticleCreateInfo
    {
        Vector3 pos;
        Vector3 vel;
        float radius;
        float growth_rate;
        float acceleration;
        float gravity_scale;
        float lifetime_seconds;
        int bitmap_handle;
        int num_frames;
        Color clr;
        Color clr_dest;
        int flags;
        int flags2;
        float age_pct_to_finish_vbm;
        int hit_callback;
    };
    static_assert(sizeof(ParticleCreateInfo) == 0x4C);

    struct Particle
    {
        Particle *next;
        Particle *prev;
        int parent_handle;
        Vector3 pos;
        Vector3 vel;
        float age;
        Color clr;
        Color clr_dest;
        Color clr_current;
        float max_life_seconds;
        float radius;
        float growth_rate;
        float acceleration;
        float gravity;
        int first_frame_bitmap;
        short num_frames;
        short flags2;
        char pool_id;
        float bitmap_orient;
        int flags;
        float age_pct_to_finish_vbm;
        void (*hit_callback)(Vector3 *hit_pos, const Vector3 *vel, Vector3 *normal, float *, int *is_liquid,
                             int *hit_obj_uid);
        GRoom *room;
        ParticleEmitter *emitter;
        Vector3 last_pos;
    };
    static_assert(sizeof(Particle) == 0x78);
    static_assert(offsetof(Particle, vel) == 0x18);

    struct ParticleEmitter
    {
        int uid;
        int parent_handle;
        Vector3 pos;
        Vector3 dir;
        float dir_rand;
        float min_vel;
        float max_vel;
        float spawn_radius;
        float min_spawn_delay;
        float max_spawn_delay;
        int emitter_flags;
        float min_life_secs;
        float max_life_secs;
        float min_pradius;
        float max_pradius;
        GRoom *room;
        ParticleCreateInfo pci;
        float cull_radius;
        float max_particle_dist_sq;
        Vector3 world_pos;
        Particle particle_list;
        float on_time;
        float on_time_variance;
        float off_time;
        float off_time_variance;
        float time_to_change;
        float current_state_time;
        bool active;
        float active_distance;
        ParticleEmitter *next;
        ParticleEmitter *prev;
        ParticleEmitter *next_entity_emitter;
        int spawn_timer; // Timestamp
    };
    static_assert(offsetof(ParticleEmitter, min_spawn_delay) == 0x30);
    static_assert(offsetof(ParticleEmitter, emitter_flags) == 0x38);
    static_assert(offsetof(ParticleEmitter, particle_list) == 0xB0);
    static_assert(offsetof(ParticleEmitter, next) == 0x148);
    static_assert(offsetof(ParticleEmitter, spawn_timer) == 0x154);
    static_assert(sizeof(ParticleEmitter) == 0x158);
}
