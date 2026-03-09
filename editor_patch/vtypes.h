#pragma once

#include <patch_common/MemUtils.h>

namespace rf
{
    struct String
    {
        int max_len;
        char* buf;

        operator const char*() const
        {
            return buf;
        }
    };

    struct File
    {
        enum SeekOrigin {
            seek_set = 0,
            seek_cur = 1,
            seek_end = 2,
        };

        [[nodiscard]] bool check_version(int min_ver) const
        {
            return AddrCaller{0x004CF650}.this_call<bool>(this, min_ver);
        }

        [[nodiscard]] int error() const
        {
            return AddrCaller{0x004D01F0}.this_call<bool>(this);
        }

        int seek(int pos, SeekOrigin origin)
        {
            return AddrCaller{0x004D00C0}.this_call<int>(this, pos, origin);
        }

        int read(void *buf, std::size_t buf_len, int min_ver = 0, int unused = 0)
        {
            return AddrCaller{0x004D0F40}.this_call<int>(this, buf, buf_len, min_ver, unused);
        }

        template<typename T>
        T read(int min_ver = 0, T def_val = 0)
        {
            if (check_version(min_ver)) {
                T val;
                read(&val, sizeof(val));
                if (!error()) {
                    return val;
                }
            }
            return def_val;
        }

        void write(const void *data, std::size_t data_len)
        {
            return AddrCaller{0x004D13F0}.this_call(this, data, data_len);
        }

        template<typename T>
        void write(T value)
        {
            write(static_cast<void*>(&value), sizeof(value));
        }
    };
}

// ─── Editor VMesh ────────────────────────────────────────────────────────────

// Editor-side VMesh struct (same layout as game rf::VMesh at 0x58 bytes)
struct EditorVMesh
{
    int type;                       // +0x00: 1=v3m, 2=v3c, 3=vfx
    void* instance;                 // +0x04: mesh instance data
    void* mesh;                     // +0x08: mesh definition data (mesh_data for v3c)
    char filename[65];              // +0x0C
    // 3 bytes padding             // +0x4D
    void* replacement_materials;    // +0x50
    char use_replacement_materials; // +0x54
    // 3 bytes padding to 0x58
};
static_assert(sizeof(EditorVMesh) == 0x58, "EditorVMesh size mismatch");

// Editor MeshMaterial (0xC8 bytes, same layout as game rf::MeshMaterial but with int diffuse_color)
struct EditorMeshMaterial {
    int material_type;
    int flags;
    bool use_additive_blending;
    char _pad[3];
    int diffuse_color;
    struct { int tex_handle; char name[33]; int start_frame; float playback_rate; int anim_type; } texture_maps[2];
    int framerate;
    int num_mix_frames;
    int* mix;
    float specular_level;
    float glossiness;
    float reflection_amount;
    char refl_tex_name[36];
    int refl_tex_handle;
    int num_self_illumination_frames;
    float* self_illumination;
    int num_opacity_frames;
    int* opacity;
};
static_assert(sizeof(EditorMeshMaterial) == 0xC8, "EditorMeshMaterial size mismatch");

// VMesh factory functions (all __cdecl)
static auto& vmesh_load_v3m = addr_as_ref<void*(const char* filename, int param2, int param3)>(0x004BFC30);
static auto& vmesh_load_v3c = addr_as_ref<void*(const char* filename, int param2, int param3)>(0x004BFD70);
static auto& vmesh_load_vfx = addr_as_ref<void*(const char* filename, int param2)>(0x004BFE10);
static auto& vmesh_free = addr_as_ref<void(void* vmesh)>(0x004BFEC0);
static auto& vmesh_render = addr_as_ref<void(void* vmesh, const void* pos, const void* orient, const void* flags)>(0x004C04B0);
static auto& vmesh_get_bound_sphere = addr_as_ref<void(void* vmesh, void* center_out, void* radius_out)>(0x004C0680);
static auto& vmesh_process = addr_as_ref<void(void* vmesh, float time, int param3, const void* pos, const void* orient, int param6)>(0x004C0710);
static auto& vmesh_anim_init = addr_as_ref<void(void* vmesh, int start_frame, float speed)>(0x004C0740);
static auto& vmesh_get_type = addr_as_ref<int(void* vmesh)>(0x004BFEB0);
static auto& vmesh_stop_all_actions = addr_as_ref<void(void* vmesh)>(0x004C07B0);
static auto& editor_vmesh_get_materials_array = addr_as_ref<void(void* vmesh, int* num_out, EditorMeshMaterial** materials_out)>(0x004C0A00);

// Bitmap load: loads a texture file, returns handle (or -1 on failure)
static auto& bm_load = addr_as_ref<int(const char* filename, int path_id, int generate_mipmaps)>(0x004BBBF0);

// character_mesh_load_action: __thiscall on mesh_data, loads .rfa file, returns action index
using EditorCharMeshLoadActionFn = int(__thiscall*)(void* mesh_data, const char* rfa_filename, char flag);
static const auto character_mesh_load_action = reinterpret_cast<EditorCharMeshLoadActionFn>(0x004C2150);

// vmesh_play_action_by_index: cdecl wrapper
static auto& vmesh_play_action_by_index = addr_as_ref<void(void* vmesh, int action_index, float transition_time, int hold_last_frame)>(0x004C0760);

// Drawing primitives
static auto& draw_3d_arrow = addr_as_ref<void(float, float, float, float, float, float, int, int, int)>(0x004CC2F0);
static auto& project_to_screen = addr_as_ref<uint32_t(void* screen_out, const void* world_pos)>(0x004C5E30);
static auto& set_draw_color = addr_as_ref<void(uint32_t r, uint32_t g, uint32_t b, uint32_t a)>(0x004B9700);
static auto& draw_line_2d = addr_as_ref<uint32_t(const void* pt1, const void* pt2, uint32_t mode)>(0x004CB150);
static auto& project_to_screen_2d = addr_as_ref<bool(const void* world_pos, float* out_x, float* out_y)>(0x004C6630);

// ─── Misc ────────────────────────────────────────────────────────────────────

static auto& file_add_path = addr_as_ref<int __cdecl(const char* path, const char* exts, bool cd)>(0x004C3950);
static auto& rf_alloc = addr_as_ref<void* __cdecl(size_t size)>(0x0052ee74);
static auto& log_dlg_append = addr_as_ref<int __cdecl(void*, const char*, ...)>(0x00444980);
static auto& log_dlg_clear = addr_as_ref<void __fastcall(void* self)>(0x00444940);
