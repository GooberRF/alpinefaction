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

        // Opens a file with explicit mode (1=read, 2=write).
        // Returns 0 on success, negative on failure.
        int open_mode(const char* filename, int mode = 1, int path_id = 0x98967f)
        {
            return AddrCaller{0x004CFE50}.this_call<int>(this, filename, mode, path_id);
        }

        void close()
        {
            AddrCaller{0x004CFF60}.this_call(this);
        }

        // Returns the file size. Call after open.
        int get_size(int unk1 = 0, int unk2 = 0x98967f)
        {
            return AddrCaller{0x004D0030}.this_call<int>(this, unk1, unk2);
        }

        [[nodiscard]] int get_version() const
        {
            return AddrCaller{0x004CF680}.this_call<int>(this);
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
            ~ChunkGuard()
            {
                if (remaining == 0) return;
                if (remaining > 0x7fffffffu) file.seek(0, seek_end);
                else file.seek(static_cast<int>(remaining), seek_cur);
            }
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

// Editor v3d runtime geometry

struct EditorVifFace
{
    uint16_t vindex1;
    uint16_t vindex2;
    uint16_t vindex3;
    uint16_t flags;
};
static_assert(sizeof(EditorVifFace) == 0x8);

struct EditorVifChunk
{
    int mode;
    Vector3* vecs;
    Vector3* norms;
    void* uvs;
    Plane* face_planes;
    EditorVifFace* faces;
    int16_t* same_vertex_offsets;
    void* wi;
    int texture_idx;
    int16_t* orig_map;
    uint16_t num_vecs;
    uint16_t num_faces;
    uint16_t vecs_alloc;
    uint16_t faces_alloc;
    uint16_t uvs_alloc;
    uint16_t wi_alloc;
    uint16_t same_vertex_offsets_alloc;
};
static_assert(sizeof(EditorVifChunk) == 0x38);
static_assert(offsetof(EditorVifChunk, vecs) == 0x04);
static_assert(offsetof(EditorVifChunk, faces) == 0x14);
static_assert(offsetof(EditorVifChunk, texture_idx) == 0x20);
static_assert(offsetof(EditorVifChunk, num_vecs) == 0x28);
static_assert(offsetof(EditorVifChunk, num_faces) == 0x2A);

struct EditorVifMesh
{
    int data_block_size;
    void* data_block;
    EditorVifChunk* chunks;
    uint16_t num_chunks;
    void* prop_points;
    int num_prop_points;
    uint8_t tex_ids[7];
    int tex_handles[7];
    int num_texture_handles;
    int flags;
    int num_original_vecs;
    int unk_field_from_v3d_file;
};
static_assert(sizeof(EditorVifMesh) == 0x4C);
static_assert(offsetof(EditorVifMesh, chunks) == 0x08);
static_assert(offsetof(EditorVifMesh, num_chunks) == 0x0C);
static_assert(offsetof(EditorVifMesh, tex_handles) == 0x20);
static_assert(offsetof(EditorVifMesh, num_texture_handles) == 0x3C);
static_assert(offsetof(EditorVifMesh, flags) == 0x40);
static_assert(offsetof(EditorVifMesh, num_original_vecs) == 0x44);

// EditorVifMesh::flags bit that gives every chunk an orig_map (v3d V3D_LOD_MORPH_VERTICES_MAP).
constexpr int VIF_LOD_MORPH_VERTICES_MAP = 0x1;

struct EditorVifLodMesh
{
    int num_levels;
    EditorVifMesh* meshes[3];
    float distances[3];
    Vector3 center;
    float radius;
    Vector3 bbox_min;
    Vector3 bbox_max;
};
static_assert(sizeof(EditorVifLodMesh) == 0x44);
static_assert(offsetof(EditorVifLodMesh, meshes) == 0x04);

struct EditorV3dMesh
{
    char name[65];
    char parent_name[65];
    int num_materials;
    EditorMeshMaterial* materials;
    EditorVifLodMesh* lod_mesh;
};
static_assert(sizeof(EditorV3dMesh) == 0x90);
static_assert(offsetof(EditorV3dMesh, lod_mesh) == 0x8C);

struct EditorV3d
{
    char v3d_filename[65];
    int version;
    int num_meshes;
    EditorV3dMesh* meshes;
    int num_lod_meshes;
    void* lod_meshes;
    int num_navpoints;
    void* navpoints;
    int num_cspheres;
    void* cspheres;
    int total_vertices_count;
    int unk_vertices_array;
    int total_triangles_count;
    int unk_triangle_array;
    int num_mesh_materials;
    EditorMeshMaterial* mesh_materials;
    int hdr_field_14;
    int unk_array_hdr_field_14;
    int field_88;
    int flags;
};
static_assert(sizeof(EditorV3d) == 0x90);
static_assert(offsetof(EditorV3d, num_meshes) == 0x48);
static_assert(offsetof(EditorV3d, meshes) == 0x4C);

// EditorVifFace::flags bit marking a face the renderer draws from both sides.
constexpr int VIF_FACE_DOUBLE_SIDED = 0x20;

// Calls fn(vif_mesh, chunk, vertex) for every LOD 0 chunk that carries geometry. Shared by the
// lightmap mesh occluders and the mesh-to-brush conversion so both see the same set of chunks.
// vertex(i) yields chunk vertex i in render-space mesh-local coordinates: the engine draws a
// submesh at pos + orient * (lod_mesh->center + v), so the owning lod mesh's center is added here
// and every caller works in the space the editor renders.
template<typename Fn>
inline void vmesh_for_each_lod0_chunk(const EditorVifLodMesh* lod, Fn&& fn)
{
    if (!lod || lod->num_levels <= 0) {
        return;
    }
    const EditorVifMesh* vm = lod->meshes[0];
    if (!vm || !vm->chunks) {
        return;
    }
    const Vector3 center = lod->center;
    for (int c = 0; c < vm->num_chunks; c++) {
        const EditorVifChunk& chunk = vm->chunks[c];
        if (!chunk.vecs || !chunk.faces || chunk.num_vecs == 0 || chunk.num_faces == 0) {
            continue;
        }
        auto vertex = [&chunk, &center](int index) {
            const Vector3& v = chunk.vecs[index];
            return Vector3{v.x + center.x, v.y + center.y, v.z + center.z};
        };
        fn(*vm, chunk, vertex);
    }
}

// Whether a face's three indices are inside its chunk's vertex array.
inline bool vmesh_lod0_face_valid(const EditorVifChunk& chunk, const EditorVifFace& face)
{
    return face.vindex1 < chunk.num_vecs && face.vindex2 < chunk.num_vecs &&
           face.vindex3 < chunk.num_vecs;
}

struct EditorCharacterMesh
{
    EditorV3d v3d_file;
    EditorV3dMesh* mesh;
};
static_assert(sizeof(EditorCharacterMesh) == 0x94);

constexpr int editor_character_max_actions = (0x120C - 0xF5C) / 4;

// .v3c character; the size is the base character table's own stride.
struct EditorCharacter
{
    uint8_t pad_00[0x44];
    int flags;     // bit 0 marks a base character table slot in use
    int num_bones; // written from the BONE section by 0x004C1FC3
    uint8_t pad_4C[0xF58 - 0x4C];
    // character_mesh_load_action returns the index of an action already in this list and appends
    // otherwise, up to (0x120C - 0xF5C) / 4 entries with no bounds check; only a re-init clears it.
    int num_actions;
    void* actions[editor_character_max_actions]; // the skeleton the action's name resolved to
    uint8_t action_is_state[editor_character_max_actions];
    uint8_t pad_12B8[0x19BC - 0x12B8];
    int num_character_meshes; // 0x004C2960 loads at most one
    EditorCharacterMesh character_meshes[1];
    int field_1A54;
};
static_assert(sizeof(EditorCharacter) == 0x1A58);
static_assert(offsetof(EditorCharacter, flags) == 0x44);
static_assert(offsetof(EditorCharacter, num_bones) == 0x48);
static_assert(offsetof(EditorCharacter, num_actions) == 0xF58);
static_assert(offsetof(EditorCharacter, actions) == 0xF5C);
static_assert(offsetof(EditorCharacter, action_is_state) == 0x120C);
static_assert(offsetof(EditorCharacter, num_character_meshes) == 0x19BC);
static_assert(offsetof(EditorCharacter, character_meshes) == 0x19C0);

// Animation skeletons are pooled by name with the extension stripped.
struct EditorAnimSkeleton
{
    char name[0x40];
    uint8_t pad_40[0x7C - 0x40];
};
static_assert(sizeof(EditorAnimSkeleton) == 0x7C);
constexpr unsigned editor_max_anim_skeletons = 800;
static auto& editor_anim_skeletons =
    addr_as_ref<EditorAnimSkeleton[editor_max_anim_skeletons]>(0x01912278);

// .v3c instance; only the action hold state is mirrored.
struct EditorCharacterInstance
{
    uint8_t pad_0000[0x1D4C];
    uint8_t action_held;
    uint8_t pad_1D4D[0x1D5C - 0x1D4D];
};
static_assert(sizeof(EditorCharacterInstance) == 0x1D5C);
static_assert(offsetof(EditorCharacterInstance, action_held) == 0x1D4C);

// Base characters live in a fixed table of 64 entries.
constexpr unsigned editor_max_base_characters = 64;
constexpr int editor_character_in_use = 0x1;
static auto& editor_base_characters =
    addr_as_ref<EditorCharacter[editor_max_base_characters]>(0x014FFB10);
static auto& character_free = addr_as_ref<void __cdecl(void* character)>(0x004C2DD0);

// One bit per occupied slot, so a load that claimed one is identified by comparing before with
// after rather than by trusting the loader to report it.
inline uint64_t editor_base_characters_in_use()
{
    uint64_t mask = 0;
    for (unsigned i = 0; i < editor_max_base_characters; ++i) {
        if (editor_base_characters[i].flags & editor_character_in_use) {
            mask |= 1ull << i;
        }
    }
    return mask;
}

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
static auto& vmesh_find_tag_by_name = addr_as_ref<int(EditorVMesh* vmesh, const char* tag_name)>(0x004C05D0);
static auto& vmesh_get_tag_local_transform = addr_as_ref<void(EditorVMesh* vmesh, Vector3* out_pos, Matrix3* out_orient, int tag_index)>(0x004C05E0);
static auto& editor_vmesh_get_materials_array = addr_as_ref<void(EditorVMesh* vmesh, int* num_out, EditorMeshMaterial** materials_out)>(0x004C0A00);

// Bitmap load: loads a texture file, returns handle (or -1 on failure)
static auto& bm_load = addr_as_ref<int(const char* filename, int path_id, int generate_mipmaps)>(0x004BBBF0);
static auto& bm_get_filename = addr_as_ref<const char*(int bm_handle)>(0x004BDC60);
static auto& bm_get_mipmap_info = addr_as_ref<void(int bm_handle, int* width, int* height,
                                                  int* num_pixels_in_all_levels, int* mip_levels)>(0x004BCBD0);
// Nonzero when the bitmap's pixel format carries alpha (formats 4, 5 and 7).
static auto& bm_has_alpha = addr_as_ref<char __cdecl(int bm_handle)>(0x004BCC60);

// Primitives CBitmapPreviewDialog::OnPaint (0x0044C1B0) uses to draw a texture straight into a
// control's own window rather than through its device context.
static auto& gr_get_max_width = addr_as_ref<int()>(0x004B8DC0);
static auto& gr_get_max_height = addr_as_ref<int()>(0x004B8DD0);
static auto& gr_set_viewport_wnd = addr_as_ref<void(HWND wnd)>(0x004B8E10);
static auto& gr_set_clip = addr_as_ref<void(int x, int y, int w, int h)>(0x004B93E0);
static auto& gr_clear = addr_as_ref<void()>(0x004B9570);
static auto& gr_flip = addr_as_ref<void()>(0x004B95A0);
static auto& gr_bitmap_scaled = addr_as_ref<char(int bm_handle, int dst_x, int dst_y, int dst_w, int dst_h,
                                                 int src_x, int src_y, int src_w, int src_h,
                                                 float unused1, float unused2, uint32_t mode)>(0x004B99D0);
// Draw mode the stock bitmap preview passes to gr_bitmap_scaled.
static auto& gr_bitmap_preview_mode = addr_as_ref<uint32_t>(0x0147D6A0);

// Camera setup: orientation rows are right/up/forward, last argument perspective projection.
static auto& gr_setup_3d = addr_as_ref<void __cdecl(const Matrix3* orient, const Vector3* pos,
                                                    float h_fov, bool zbuffer, bool perspective)>(0x004C5980);

static auto& gr_set_far_clip = addr_as_ref<void __cdecl(float dist)>(0x004C5B30);
static auto& gr_far_clip_dist = addr_as_ref<float>(0x0158F3F8);

// Gathers the scene lights reaching a sphere into the render light list; paired with room_cleanup.
static auto& room_setup = addr_as_ref<int __cdecl(void* room, const Vector3* pos, float radius,
                                                  int include_static, int include_dynamic)>(0x004885D0);
static auto& room_cleanup = addr_as_ref<void __cdecl()>(0x00488BB0);

// The Preferences page holding the editor's user configurable colours; the viewport painter
// (0x0047DAE0) feeds the background one to set_draw_color before its clear.
struct EditorColorPrefs
{
    uint8_t pad_00[0x424];
    COLORREF background;
    uint8_t pad_428[0x464 - 0x428];
    uint8_t link_r;
    uint8_t link_g;
    uint8_t link_b;
};
static_assert(offsetof(EditorColorPrefs, background) == 0x424);
static_assert(offsetof(EditorColorPrefs, link_r) == 0x464);
static_assert(offsetof(EditorColorPrefs, link_g) == 0x465);
static_assert(offsetof(EditorColorPrefs, link_b) == 0x466);
static auto& editor_color_prefs = addr_as_ref<EditorColorPrefs* __cdecl()>(0x00483E10);

// character_mesh_load_action: __thiscall on mesh_data, loads .rfa file, returns action index
using EditorCharMeshLoadActionFn = int(__thiscall*)(void* mesh_data, const char* rfa_filename, char is_state, char unused);
static const auto character_mesh_load_action = reinterpret_cast<EditorCharMeshLoadActionFn>(0x004C2150);

// vmesh_play_action_by_index: cdecl wrapper
static auto& vmesh_play_action_by_index = addr_as_ref<void(EditorVMesh* vmesh, int action_index, float weight, int hold_last_frame)>(0x004C0760);
// vmesh_reset_actions: clears all active action slots
static auto& vmesh_reset_actions = addr_as_ref<void(EditorVMesh* vmesh)>(0x004C07A0);

// Drawing primitives
static auto& draw_3d_arrow = addr_as_ref<void(float, float, float, float, float, float, int, int, int)>(0x004CC2F0);
static auto& draw_link_line = addr_as_ref<void(float, float, float, float, float, float, int, int, int)>(0x004CC2B0);
static auto& project_to_screen = addr_as_ref<uint32_t(void* screen_out, const void* world_pos)>(0x004C5E30);
static auto& set_draw_color = addr_as_ref<void(uint32_t r, uint32_t g, uint32_t b, uint32_t a)>(0x004B9700);
static auto& gr_set_bitmap = addr_as_ref<void(int bm_handle, int unk)>(0x004B97E0);
// Nonzero while the viewports draw textured rather than wireframe.
static auto& editor_textures_enabled = addr_as_ref<int>(0x006C9AA8);
// Set around a .vfx draw so the renderer takes its transparency path.
static auto& vfx_render_transparent = addr_as_ref<int>(0x0059E21C);
static auto& gr_render_billboard = addr_as_ref<void(void* pos, int unk, float scale, float param)>(0x004CB360);
static auto& draw_line_2d = addr_as_ref<uint32_t(const void* pt1, const void* pt2, uint32_t mode)>(0x004CB150);
static auto& project_to_screen_2d = addr_as_ref<bool(const void* world_pos, float* out_x, float* out_y)>(0x004C6630);

// Stock wireframe shape drawing — the routines gas regions and triggers use. Their line
// segments go through the clipped 3D line drawer (0x004CB180), so edges survive near-plane
// crossings that a raw project-and-draw pass loses. Colour comes from set_draw_color.
// Box dimensions are full size; the routine halves them itself.
static auto& draw_wireframe_sphere_3d = addr_as_ref<bool(const Vector3* center, float radius, uint32_t mode)>(0x004CB4F0);
static auto& draw_wireframe_box_3d =
    addr_as_ref<bool(const Vector3* center, const Matrix3* orient, const Vector3* dims, uint32_t mode)>(0x004CB840);
// Draw mode the stock editor passes to the shape/line routines.
inline uint32_t editor_line_mode()
{
    return *reinterpret_cast<uint32_t*>(0x0147d260);
}

// ─── Render Params ───────────────────────────────────────────────────────────

enum EditorRenderFlag : uint32_t
{
    ERF_TEXTURED            = 0x2,
    ERF_SELECTION_HIGHLIGHT = 0x20,
    // If not set, renderer overwrites ambient_color with scene lighting sampled at draw position.
    ERF_CUSTOM_AMBIENT      = 0x80,
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
    Color ambient_color;
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

    void sort_children(int parent_handle)
    {
        SendMessage(_d.m_hWnd, 0x1113 /*TVM_SORTCHILDREN*/, FALSE,
            static_cast<LPARAM>(parent_handle));
    }
};
static_assert(sizeof(EditorTreeCtrl) == sizeof(CWnd));

// ─── Viewport ────────────────────────────────────────────────────────────────

// Editor view data — accessed from the active viewport at +0x54
struct EditorViewData
{
    uint8_t pad_00[0x04];               // +0x00
    Matrix3 camera_orient;              // +0x04
    Vector3 camera_pos;                 // +0x28
};
static_assert(offsetof(EditorViewData, camera_orient) == 0x04);
static_assert(offsetof(EditorViewData, camera_pos) == 0x28);

// Editor viewport — returned by get_active_viewport()
struct EditorViewport
{
    uint8_t pad_00[0x54];               // +0x00
    EditorViewData* view_data;          // +0x54
};
static_assert(offsetof(EditorViewport, view_data) == 0x54);

static auto& get_active_viewport = addr_as_ref<EditorViewport* __cdecl()>(0x004835B0);

// ─── Editor GrVertex ─────────────────────────────────────────────────────────
// Vertex structure (48 bytes) used by the editor's polygon renderer.
// Fields [0]-[2] are VIEW-SPACE coordinates (not world-space).

struct GrVertex {
    float vx, vy, vz;                  // +0x00 view-space position
    float screen_x, screen_y;          // +0x0C
    float rhw;                          // +0x14
    uint8_t clip_flags;                 // +0x18
    uint8_t proj_flags;                 // +0x19
    uint8_t pad1a[2];                   // +0x1A
    float u, v;                         // +0x1C
    uint8_t pad24[8];                   // +0x24
    uint8_t r, g, b, a;                // +0x2C
};
static_assert(sizeof(GrVertex) == 0x30);

// ─── Rendering pipeline ──────────────────────────────────────────────────────

namespace red
{
    struct GrScreen
    {
        int signature;
        int max_width;
        int max_height;
        int mode;
        int window_mode;
        int field_14;
        float aspect;
        int field_1c;
        int bits_per_pixel;
        int bytes_ber_pixel;
        int field_28;
        int offset_x;
        int offset_y;
        int clip_width;
        int clip_height;
        int max_tex_width;
        int max_tex_height;
        int clip_left;
        int clip_right;
        int clip_top;
        int clip_bottom;
        // Draw/clear colour bytes in the order set_draw_color takes them, so the low byte is red.
        int current_color;
        int current_bitmap;
        int current_bitmap2;
        int fog_mode;
        int fog_color;
        float fog_near;
        float fog_far;
        float fog_far_scaled;
        bool recolor_enabled;
        float recolor_red;
        float recolor_green;
        float recolor_blue;
        int field_84;
        int field_88;
        int zbuffer_mode;
    };
    static_assert(sizeof(GrScreen) == 0x90);
    static_assert(offsetof(GrScreen, current_color) == 0x54);

    static auto& gr_screen = addr_as_ref<GrScreen>(0x014CF748);
}

// D3D8 device pointer
static auto& d3d_device_ptr = addr_as_ref<void*>(0x0183b914);

// Batch management
static auto& gr_flush_batch = addr_as_ref<void()>(0x004e99d0);
static auto& gr_begin_batch = addr_as_ref<void(int, int)>(0x004e98e0);

static auto& gr_d3d_render_mode_cache = addr_as_ref<int>(0x01838dc0);

// Render mode and polygon submission
static auto& gr_set_mode = addr_as_ref<void(int)>(0x004BA730);
static auto& gr_poly_render = addr_as_ref<uint32_t(int, void**, int, float, int, float)>(0x004CB1C0);

// Computes clip flags from view-space coords in a GrVertex
static auto& gr_compute_clip_flags = addr_as_ref<uint32_t(void*)>(0x004c5df0);

// Camera position and 3x3 view matrix used by the editor's rendering pipeline
static auto& ed_cam_pos = addr_as_ref<float[3]>(0x0158ef20);
static auto& ed_view_mat = addr_as_ref<float[9]>(0x0158ef58); // row-major 3x3

// Billboard camera parameter (used as depth sort / z-bias)
static auto& gr_cam_param = addr_as_ref<float>(0x014cf7e0);

// ─── Lighting ────────────────────────────────────────────────────────────────

// Type 1 (directional) scene light; returns the light handle.
static auto& light_create_directional =
    addr_as_ref<int __cdecl(const Vector3* dir, float intensity, float r, float g, float b,
                            int is_dynamic, int shadow_condition, int atten_algo)>(0x00487950);
static auto& light_free = addr_as_ref<void __cdecl(int light_handle, int unk)>(0x00487D40);
// Stock per-lumel accumulator: adds every scene light's contribution to the already seeded r/g/b.
static auto& light_accum_at_texel =
    addr_as_ref<void __cdecl(float* r, float* g, float* b, const Vector3* pos, const Vector3* normal,
                             void* masks, int texel_index, const void* smooth_flag)>(0x004894C0);

// ─── Virtual file system ─────────────────────────────────────────────────────

// Search paths registered through file_add_path. Slot 0 is the game root and has no path
// string; file_add_path inserts the separator itself, so `path` carries none.
struct EditorVfsPath
{
    const char* path;
    const char* extensions;
    uint8_t cd_only;
    uint8_t pad_09[3];
};
static_assert(sizeof(EditorVfsPath) == 0xC);
constexpr int editor_vfs_path_count = 512;
static auto& vfs_paths = addr_as_ref<EditorVfsPath[editor_vfs_path_count]>(0x0156B110);

// Loose files found by file_scan_path, chained per name hash (0x004CF7B0 masks to 0x7FFF).
struct EditorVfsFile
{
    int path_index;
    const char* name;
    EditorVfsFile* next;
};
static_assert(sizeof(EditorVfsFile) == 0xC);
static auto& vfs_file_buckets = addr_as_ref<EditorVfsFile*[0x8000]>(0x01622004);

// Builds the path a search path slot resolves a name to: the root directory 0x0158CA10, which
// always carries its own trailing separator, then the slot's path, a separator and the name.
static auto& file_make_path =
    addr_as_ref<char* __cdecl(int path_index, const char* name, char* out)>(0x004C33D0);

// Adds a loose file to the hash unless a node for that name is already there.
// The name has to be lowercased.
static auto& file_add_loose_file =
    addr_as_ref<void __cdecl(const char* name, int path_index)>(0x004CF8D0);

// One .vpp directory entry. Names come from a 60 byte fixed record, so they are not
// guaranteed to be null terminated.
struct EditorPackfileEntry
{
    int name_hash;
    const char* name;
    int field_08;
    int size;
    void* packfile;
    int field_14;
};
static_assert(sizeof(EditorPackfileEntry) == 0x18);

struct EditorPackfile
{
    char name[0x20];
    char path[0x80];
    int field_a0;
    int num_entries;
    EditorPackfileEntry* entries;
    uint8_t pad_ac[4];
};
static_assert(sizeof(EditorPackfile) == 0xB0);
static_assert(offsetof(EditorPackfile, num_entries) == 0xA4);
constexpr int editor_packfile_max = 256;
static auto& packfiles = addr_as_ref<EditorPackfile[editor_packfile_max]>(0x015DE820);
static auto& num_packfiles = addr_as_ref<int>(0x01611F6C);
// Shared entry pool every mounted packfile carves its directory out of.
constexpr int editor_packfile_entry_max = 0x34BC;

// ─── Misc ────────────────────────────────────────────────────────────────────

static auto& generate_uid = addr_as_ref<int()>(0x00484230);
// True if uid is already taken. Scans master objects, brush list, undo/redo stacks.
static auto& is_uid_in_use = addr_as_ref<bool __cdecl(int uid)>(0x00484000);
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

// Length limits for names read from untrusted level files.
constexpr std::size_t rfl_name_max_len = 31;
constexpr std::size_t rfl_ext_max_len = 14;
constexpr std::size_t rfl_mesh_name_max_len = 64;
constexpr std::size_t rfl_anim_name_max_len = 59;

inline bool rfl_ext_over_long(const std::string& name)
{
    auto dot = name.rfind('.');
    return dot != std::string::npos && name.size() - dot > rfl_ext_max_len;
}

inline bool rfl_name_over_long(const std::string& name)
{
    return name.size() > rfl_name_max_len || rfl_ext_over_long(name);
}
