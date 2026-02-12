#pragma once

#include "math/vector.h"
#include "gr/gr.h"

namespace rf
{
    struct GRoom;

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
        //uint8_t padding[3];             // 0x4D - 3 bytes of padding
        void* replacement_materials;
        char use_replacement_materials;
        //uint8_t _padding[3];            // 0x55 - Padding bytes to align to size 0x58
    };
    static_assert(sizeof(VMesh) == 0x58);   

    struct VMeshCollisionInput
    {
        Vector3 mesh_pos;
        Matrix3 mesh_orient;
        Vector3 start_pos;
        Vector3 dir;
        float radius;
        int flags;
        Vector3 transformed_start_pos;
        Vector3 transformed_pos_diff;

        VMeshCollisionInput()
        {
            AddrCaller{0x00416190}.this_call(this);
        }

    };
    static_assert(sizeof(VMeshCollisionInput) == 0x68);

    struct VMeshCollisionOutput
    {
        float fraction;
        Vector3 hit_point;
        Vector3 hit_normal;
        short *triangle_indices;

        VMeshCollisionOutput()
        {
            AddrCaller{0x004161D0}.this_call(this);
        }
    };
    static_assert(sizeof(VMeshCollisionOutput) == 0x20);

    struct TexMap
    {
        int tex_handle;
        char name[33];
        int start_frame;
        float playback_rate;
        int anim_type;
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
    };
    static_assert(sizeof(MeshMaterial) == 0xC8);

    static auto& vmesh_get_type = addr_as_ref<VMeshType(VMesh *vmesh)>(0x00502B00);
    static auto& vmesh_get_name = addr_as_ref<const char*(VMesh* vmesh)>(0x00503470);
    static auto& vmesh_get_num_cspheres = addr_as_ref<int(VMesh *vmesh)>(0x00503250);
    static auto& vmesh_get_bbox = addr_as_ref<void(VMesh*, Vector3*, Vector3*)>(0x00503310);
    static auto& vmesh_get_csphere = addr_as_ref<bool(VMesh *vmesh, int index, Vector3 *pos, float *radius)>(0x00503270);
    static auto& vmesh_collide = addr_as_ref<bool(VMesh *vmesh, VMeshCollisionInput *in, VMeshCollisionOutput *out, bool clear)>(0x005031F0);
    static auto& vmesh_calc_lighting_data_size = addr_as_ref<int(VMesh *vmesh)>(0x00503F50);
    static auto& vmesh_update_lighting_data = addr_as_ref<int(VMesh *vmesh, GRoom *room, const Vector3 &pos, const Matrix3 &orient, void *mesh_lighting_data)>(0x00504000);
    static auto& vmesh_stop_all_actions = addr_as_ref<void(VMesh* vmesh)>(0x00503400);
    static auto& vmesh_get_materials_array = addr_as_ref<void(VMesh *vmesh, int *num_materials_out, MeshMaterial **materials_array_out)>(0x00503650);
    static auto& vmesh_process = addr_as_ref<void(VMesh* vmesh, float frametime, int increment_only, Vector3* pos, Matrix3* orient, int lod_level)>(0x00503360);
    static auto& vmesh_anim_length = addr_as_ref<double(VMesh* vmesh, int anim_index)>(0x005033E0);
    static auto& vmesh_play_action = addr_as_ref<void(VMesh* vmesh, int anim_index, float percent, bool hold_last_frame)>(0x005033B0);
    static auto& vmesh_create_anim_fx = addr_as_ref<VMesh*(const char *filename, int path_id)>(0x00502A60);
    static auto& vclip_lookup = addr_as_ref<int(const char* name)>(0x004C1D00);
    static auto& vclip_play_3d =
        addr_as_ref<void(int index, GRoom* src_room, Vector3* src_pos, Vector3* pos, float radius,
            int parent_handle, Vector3* dir, bool play_sound)>(0x004C16E0);

}
