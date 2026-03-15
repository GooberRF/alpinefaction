#pragma once

#include <patch_common/MemUtils.h>
#include "mfc_types.h"

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
        int dir_id;
        char exists;
        char filename[259];
        void* packfile_entry;
        int cd_file;
        int open_file_index;

        enum SeekOrigin {
            seek_set = 0,
            seek_cur = 1,
            seek_end = 2,
        };

        File()
        {
            AddrCaller{0x004CF600}.this_call(this);
        }

        // Opens a file by name, searching loose files and .vpp archives.
        // path_id: search path identifier (0x98967f = default mesh/anim paths)
        // Returns true if the file was found and opened.
        bool open(const char* filename, int path_id = 0x98967f)
        {
            return AddrCaller{0x004CF9A0}.this_call<bool>(this, filename, path_id);
        }

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

        // RAII guard that seeks past unread chunk data on scope exit.
        // Use at the top of chunk deserialize functions to ensure the file
        // position advances past the full chunk even on early return.
        struct ChunkGuard {
            File& file;
            std::size_t& remaining;
            ~ChunkGuard() { if (remaining > 0) file.seek(static_cast<int>(remaining), seek_cur); }
        };
    };
    static_assert(sizeof(File) == 0x114, "File size mismatch");
}

// ─── Editor VMesh ────────────────────────────────────────────────────────────

enum EditorVMeshType : int
{
    VMESH_TYPE_UNINITIALIZED = 0,
    VMESH_TYPE_STATIC        = 1, // .v3m
    VMESH_TYPE_CHARACTER     = 2, // .v3c
    VMESH_TYPE_ANIM_FX       = 3, // .vfx
};

// Editor-side VMesh struct (same layout as game rf::VMesh at 0x58 bytes)
struct EditorVMesh
{
    EditorVMeshType type;
    void* instance;
    void* mesh; // mesh_data for v3c
    char filename[65];
    // 3 bytes padding
    void* replacement_materials;
    bool use_replacement_materials;
    // 3 bytes padding to 0x58
};
static_assert(sizeof(EditorVMesh) == 0x58, "EditorVMesh size mismatch");

struct EditorTextureMap {
    int tex_handle;
    char name[33];
    int start_frame;
    float playback_rate;
    int anim_type;
};

// Editor MeshMaterial (0xC8 bytes, same layout as game rf::MeshMaterial)
struct EditorMeshMaterial {
    int material_type;
    int flags;
    bool use_additive_blending;
    char _pad[3];
    Color diffuse_color;
    EditorTextureMap texture_maps[2];
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

struct EditorRenderParams; // forward declaration for vmesh_render

// VMesh factory functions
static auto& vmesh_load_v3m = addr_as_ref<EditorVMesh*(const char* filename, int param2, int param3)>(0x004BFC30);
static auto& vmesh_load_v3c = addr_as_ref<EditorVMesh*(const char* filename, int param2, int param3)>(0x004BFD70);
static auto& vmesh_load_vfx = addr_as_ref<EditorVMesh*(const char* filename, int param2)>(0x004BFE10);
static auto& vmesh_free = addr_as_ref<void(EditorVMesh* vmesh)>(0x004BFEC0);
static auto& vmesh_render = addr_as_ref<void(EditorVMesh* vmesh, const void* pos, const void* orient, const EditorRenderParams* params)>(0x004C04B0);
static auto& vmesh_get_bound_sphere = addr_as_ref<void(EditorVMesh* vmesh, void* center_out, void* radius_out)>(0x004C0680);
static auto& vmesh_process = addr_as_ref<void(EditorVMesh* vmesh, float time, int param3, const void* pos, const void* orient, int param6)>(0x004C0710);
static auto& vmesh_anim_init = addr_as_ref<void(EditorVMesh* vmesh, int start_frame, float speed)>(0x004C0740);
static auto& vmesh_get_type = addr_as_ref<EditorVMeshType(EditorVMesh* vmesh)>(0x004BFEB0);
static auto& vmesh_stop_all_actions = addr_as_ref<void(EditorVMesh* vmesh)>(0x004C07B0);
static auto& editor_vmesh_get_materials_array = addr_as_ref<void(EditorVMesh* vmesh, int* num_out, EditorMeshMaterial** materials_out)>(0x004C0A00);

// Bitmap load: loads a texture file, returns handle (or -1 on failure)
static auto& bm_load = addr_as_ref<int(const char* filename, int path_id, int generate_mipmaps)>(0x004BBBF0);

// character_mesh_load_action: __thiscall on mesh_data, loads .rfa file, returns action index
using EditorCharMeshLoadActionFn = int(__thiscall*)(void* mesh_data, const char* rfa_filename, char is_state, char unused);
static const auto character_mesh_load_action = reinterpret_cast<EditorCharMeshLoadActionFn>(0x004C2150);

// vmesh_play_action_by_index: cdecl wrapper
static auto& vmesh_play_action_by_index = addr_as_ref<void(EditorVMesh* vmesh, int action_index, float transition_time, int hold_last_frame)>(0x004C0760);
// vmesh_get_action_duration: returns duration in seconds for given action index
static auto& vmesh_get_action_duration = addr_as_ref<float(EditorVMesh* vmesh, int action_index)>(0x004C0790);
// vmesh_reset_actions: clears all active action slots
static auto& vmesh_reset_actions = addr_as_ref<void(EditorVMesh* vmesh)>(0x004C07A0);

// Drawing primitives
static auto& draw_3d_arrow = addr_as_ref<void(float, float, float, float, float, float, int, int, int)>(0x004CC2F0);
static auto& project_to_screen = addr_as_ref<uint32_t(void* screen_out, const void* world_pos)>(0x004C5E30);
static auto& set_draw_color = addr_as_ref<void(uint32_t r, uint32_t g, uint32_t b, uint32_t a)>(0x004B9700);
static auto& gr_set_bitmap = addr_as_ref<void(int bm_handle, int unk)>(0x004B97E0);
static auto& gr_render_billboard = addr_as_ref<void(void* pos, int unk, float scale, float param)>(0x004CB360);
static auto& draw_line_2d = addr_as_ref<uint32_t(const void* pt1, const void* pt2, uint32_t mode)>(0x004CB150);
static auto& project_to_screen_2d = addr_as_ref<bool(const void* world_pos, float* out_x, float* out_y)>(0x004C6630);

// ─── Render Params ───────────────────────────────────────────────────────────

enum EditorRenderFlag : uint32_t
{
    ERF_TEXTURED            = 0x2,
    ERF_SELECTION_HIGHLIGHT = 0x20,
};

// VMesh render parameters
struct EditorRenderParams
{
    uint32_t flags;
    uint32_t field_04;
    uint32_t field_08;
    Color diffuse_color;
    uint32_t field_10;
    float field_14;
    Color selection_color;
    uint32_t field_1C;
    uint32_t field_20;
    uint32_t field_24;
    Color field_28;
    Matrix3 orient;

    EditorRenderParams()
    {
        AddrCaller{0x004BE330}.this_call(this);
    }
};
static_assert(sizeof(EditorRenderParams) == 0x50);

// ─── Tree Control ────────────────────────────────────────────────────────────

// Editor tree control — CTreeCtrl inherits from CWnd (same layout, no additional data)
struct EditorTreeCtrl : CWnd
{
    int insert_item(const char* label, int parent_handle, int sort_flags)
    {
        return AddrCaller{0x004422B0}.this_call<int>(this, label, parent_handle, sort_flags);
    }

    void set_item_data(int item_handle, int data)
    {
        AddrCaller{0x00442320}.this_call(this, item_handle, data);
    }
};
static_assert(sizeof(EditorTreeCtrl) == sizeof(CWnd));

// ─── Viewport ────────────────────────────────────────────────────────────────

static auto& get_active_viewport = addr_as_ref<void* __cdecl()>(0x004835B0);

// ─── Misc ────────────────────────────────────────────────────────────────────

static auto& generate_uid = addr_as_ref<int()>(0x00484230);
static auto& file_add_path = addr_as_ref<int __cdecl(const char* path, const char* exts, bool cd)>(0x004C3950);
static auto& file_scan_path = addr_as_ref<void(int slot_index)>(0x004CF800);
static auto& rf_alloc = addr_as_ref<void* __cdecl(size_t size)>(0x0052ee74);
static auto& log_dlg_append = addr_as_ref<int __cdecl(void*, const char*, ...)>(0x00444980);
static auto& log_dlg_clear = addr_as_ref<void __fastcall(void* self)>(0x00444940);

// ─── RFL String I/O ──────────────────────────────────────────────────────────

inline void write_rfl_string(rf::File& file, const VString& str)
{
    const char* s = str.c_str();
    uint16_t len = static_cast<uint16_t>(strlen(s));
    file.write<uint16_t>(len);
    if (len > 0) {
        file.write(s, len);
    }
}

inline void write_rfl_string(rf::File& file, const std::string& str)
{
    uint16_t len = static_cast<uint16_t>(str.size());
    file.write<uint16_t>(len);
    if (len > 0) {
        file.write(str.c_str(), len);
    }
}

inline std::string read_rfl_string(rf::File& file, std::size_t& remaining)
{
    if (remaining < 2) return "";
    uint16_t len = file.read<uint16_t>();
    remaining -= 2;
    if (len == 0 || remaining < len) {
        if (len > 0 && remaining < len) {
            file.seek(static_cast<int>(remaining), rf::File::seek_cur);
            remaining = 0;
        }
        return "";
    }
    std::string result(len, '\0');
    file.read(result.data(), len);
    remaining -= len;
    return result;
}
