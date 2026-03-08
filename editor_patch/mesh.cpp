#include <windows.h>
#include <commdlg.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <xlog/xlog.h>
#include <patch_common/MemUtils.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/FunHook.h>
#include <patch_common/AsmWriter.h>
#include "mesh.h"
#include "mfc_types.h"
#include "level.h"
#include "resources.h"
#include "vtypes.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

// ─── Helpers ────────────────────────────────────────────────────────────────

// Free a VString buffer using the stock allocator
static void vstring_free(VString& s)
{
    AddrCaller{0x004b6710}.this_call(&s);
}

// ─── VMesh Loading ──────────────────────────────────────────────────────────

// VMesh factory functions (all __cdecl, return vmesh pointer or 0 on failure)
using VmeshLoadV3mFn = void*(__cdecl*)(const char* filename, int param2, int param3);
using VmeshLoadV3cFn = void*(__cdecl*)(const char* filename, int param2, int param3);
using VmeshLoadVfxFn = void*(__cdecl*)(const char* filename, int param2);
using VmeshFreeFn = void(__cdecl*)(void* vmesh_ptr);
using VmeshRenderFn = void(__cdecl*)(void* vmesh_ptr, const void* pos, const void* orient, const void* flags);
using VmeshGetBoundSphereFn = void(__cdecl*)(void* vmesh_ptr, void* center_out, void* radius_out);
using VmeshProcessFn = void(__cdecl*)(void* vmesh_ptr, float time, int param3, const void* pos, const void* orient, int param6);

static const auto vmesh_load_v3m = reinterpret_cast<VmeshLoadV3mFn>(0x004bfc30);
static const auto vmesh_load_v3c = reinterpret_cast<VmeshLoadV3cFn>(0x004bfd70);
static const auto vmesh_load_vfx = reinterpret_cast<VmeshLoadVfxFn>(0x004bfe10);
static const auto vmesh_free = reinterpret_cast<VmeshFreeFn>(0x004bfec0);
static const auto vmesh_render = reinterpret_cast<VmeshRenderFn>(0x004c04b0);
static const auto vmesh_get_bound_sphere = reinterpret_cast<VmeshGetBoundSphereFn>(0x004c0680);
static const auto vmesh_process = reinterpret_cast<VmeshProcessFn>(0x004c0710);
// VFX animation init: FUN_004c0740(vmesh, start_frame, speed) - sets initial VFX state
using VmeshAnimInitFn = void(__cdecl*)(void* vmesh_ptr, int start_frame, float speed);
static const auto vmesh_anim_init = reinterpret_cast<VmeshAnimInitFn>(0x004c0740);
// VMesh type query: returns 1=v3m, 2=v3c, 3=vfx
using VmeshGetTypeFn = int(__cdecl*)(void* vmesh_ptr);
static const auto vmesh_get_type = reinterpret_cast<VmeshGetTypeFn>(0x004bfeb0);

// Bitmap load: loads a texture file, returns handle (or -1 on failure)
using BmLoadFn = int(__cdecl*)(const char* filename, int path_id, int generate_mipmaps);
static const auto bm_load = reinterpret_cast<BmLoadFn>(0x004BBBF0);

// VMesh material replacement: gets (or creates) per-instance replacement materials array
// MeshMaterial is 0xC8 bytes. Returns count and pointer to replacement array.
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
static_assert(sizeof(EditorMeshMaterial) == 0xC8);
using VmeshGetMaterialsArrayFn = void(__cdecl*)(void* vmesh, int* num_out, EditorMeshMaterial** materials_out);
static const auto editor_vmesh_get_materials_array = reinterpret_cast<VmeshGetMaterialsArrayFn>(0x004c0a00);

// Forward declarations
static void mesh_apply_texture_overrides(DedMesh* mesh);
// vmesh_stop_all_actions: editor equivalent of game's 0x00503400
using VmeshStopAllActionsFn = void(__cdecl*)(void* vmesh_ptr);
static const auto vmesh_stop_all_actions = reinterpret_cast<VmeshStopAllActionsFn>(0x004c07b0);

// Load an .rfa/.mvf animation file onto a v3c character mesh and play it.
// VMesh layout: [+0x00] type (int), [+0x04] instance (void*), [+0x08] mesh_data (void*)
//
// character_mesh_load_action (editor 0x004c2150, game 0x0051cc10):
//   __thiscall on mesh_data, loads .rfa file, returns action index
// vmesh_play_action_by_index (editor 0x004c0760, game 0x005033b0):
//   cdecl(vmesh, action_index, transition_time, hold_last_frame)

// character_mesh_load_action: __thiscall, this=mesh_data, returns action index
using CharMeshLoadActionFn = int(__thiscall*)(void* mesh_data, const char* rfa_filename, char flag);
static const auto character_mesh_load_action = reinterpret_cast<CharMeshLoadActionFn>(0x004c2150);

// vmesh_play_action_by_index: cdecl wrapper
using VmeshPlayActionByIndexFn = void(__cdecl*)(void* vmesh, int action_index, float transition_time, int hold_last_frame);
static const auto vmesh_play_action_by_index = reinterpret_cast<VmeshPlayActionByIndexFn>(0x004c0760);

// Preview animation state
static DedMesh* g_preview_mesh = nullptr;
static float g_preview_timer = 0.0f;
static constexpr float PREVIEW_DURATION = 10.0f; // max preview time in seconds

// Cache action indices per vmesh to avoid calling character_mesh_load_action during rendering
static std::unordered_map<void*, int> g_v3c_action_cache;

static bool mesh_play_v3c_action(void* vmesh, const char* action_name, float transition_time = 1.0f)
{
    if (!vmesh || !action_name || action_name[0] == '\0') return false;

    auto* vmesh_bytes = reinterpret_cast<uint8_t*>(vmesh);
    int type = *reinterpret_cast<int*>(vmesh_bytes + 0x00);
    if (type != 2) return false; // only for character meshes

    auto* mesh_data = *reinterpret_cast<void**>(vmesh_bytes + 0x08);
    if (!mesh_data) return false;

    // Verify the animation file exists before loading (character_mesh_load_action
    // returns 0 rather than -1 for missing files, which leads to garbage animation data)
    uint8_t file_obj[0x114] = {};
    AddrCaller{0x004cf600}.this_call(file_obj);
    bool file_found = AddrCaller{0x004cf9a0}.this_call<bool>(file_obj, action_name);
    if (!file_found) {
        xlog::warn("[Mesh] Animation file '{}' not found, skipping", action_name);
        return false;
    }

    // Load the .rfa/.mvf animation file onto the character mesh_data
    int action_index = character_mesh_load_action(mesh_data, action_name, 0);
    if (action_index < 0) {
        xlog::warn("[Mesh] Failed to load animation '{}' on vmesh {:p}", action_name, vmesh);
        return false;
    }

    // Check instance exists
    auto* instance = *reinterpret_cast<void**>(vmesh_bytes + 0x04);
    if (!instance) {
        xlog::warn("[Mesh] No instance on vmesh {:p}, cannot play animation", vmesh);
        return false;
    }

    auto* inst_bytes = reinterpret_cast<uint8_t*>(instance);
    int slots_before = *reinterpret_cast<int*>(inst_bytes + 0x12D0);

    // Cache the action index for later use in the render loop
    g_v3c_action_cache[vmesh] = action_index;

    // Play the loaded action (transition_time must be > 0 or play_action is a no-op)
    vmesh_play_action_by_index(vmesh, action_index, transition_time, 0);

    int slots_after = *reinterpret_cast<int*>(inst_bytes + 0x12D0);
    char freeze_flag = *reinterpret_cast<char*>(inst_bytes + 0x1D4C);
    xlog::info("[Mesh] Playing animation '{}' (action_index={}, transition={:.3f}) on vmesh {:p} "
        "slots: {}→{} freeze={}", action_name, action_index, transition_time, vmesh,
        slots_before, slots_after, (int)freeze_flag);
    return true;
}

static const char* get_file_extension(const char* filename)
{
    const char* dot = strrchr(filename, '.');
    return dot ? dot : "";
}

static void mesh_load_vmesh(DedMesh* mesh)
{
    if (!mesh) return;

    xlog::info("[Mesh] mesh_load_vmesh: mesh={:p} old_vmesh={:p}", static_cast<void*>(mesh), mesh->vmesh);

    // Free existing vmesh if any
    if (mesh->vmesh) {
        xlog::info("[Mesh] mesh_load_vmesh: freeing old vmesh {:p}", mesh->vmesh);
        g_v3c_action_cache.erase(mesh->vmesh);
        vmesh_free(mesh->vmesh);
        mesh->vmesh = 0;
        xlog::info("[Mesh] mesh_load_vmesh: old vmesh freed");
    }

    const char* filename = mesh->mesh_filename.c_str();
    if (!filename || filename[0] == '\0') return;

    const char* ext = get_file_extension(filename);
    xlog::info("[Mesh] mesh_load_vmesh: loading '{}' ext='{}'", filename, ext);

    void* vmesh = nullptr;
    if (_stricmp(ext, ".v3m") == 0) {
        vmesh = vmesh_load_v3m(filename, 1, -1);
    }
    else if (_stricmp(ext, ".v3c") == 0) {
        vmesh = vmesh_load_v3c(filename, 0, 0);
    }
    else if (_stricmp(ext, ".vfx") == 0) {
        // The VFX loader triggers a fatal error dialog if the file is missing,
        // unlike v3m/v3c which return null gracefully.
        // Use RED's File class to check if the file exists (searches loose files + .vpp archives).
        uint8_t file_obj[0x114] = {};
        AddrCaller{0x004cf600}.this_call(file_obj); // File constructor (__thiscall)
        xlog::info("[Mesh] mesh_load_vmesh: checking VFX file existence...");
        bool found = AddrCaller{0x004cf9a0}.this_call<bool>(file_obj, filename);
        xlog::info("[Mesh] mesh_load_vmesh: VFX file found={}", found);
        if (found) {
            xlog::info("[Mesh] mesh_load_vmesh: calling vmesh_load_vfx...");
            vmesh = vmesh_load_vfx(filename, 0x98967f);
            xlog::info("[Mesh] mesh_load_vmesh: vmesh_load_vfx returned {:p}", vmesh);
        }
        else {
            xlog::warn("[Mesh] VFX file not found: '{}'", filename);
        }
    }

    mesh->vmesh = vmesh;
    mesh->vmesh_load_failed = (vmesh == nullptr);
    if (vmesh) {
        // Clear replacement material state to prevent stale data from memory reuse
        // (e.g. VFX vmesh freed then V3M allocated at same address with leftover flags)
        auto* vbytes = reinterpret_cast<uint8_t*>(vmesh);
        *reinterpret_cast<void**>(vbytes + 0x50) = nullptr;  // replacement_materials
        *(vbytes + 0x54) = 0;                                // use_replacement_materials

        int vtype = vmesh_get_type(vmesh);
        // Initialize VFX animation state (matches stock item setup in FUN_004151c0)
        if (vtype == 3) {
            vmesh_anim_init(vmesh, 0, 1.0f);
            vmesh_process(vmesh, 0.0f, 0, nullptr, nullptr, 1);
        }
        // Play state animation for v3c skeletal meshes — jump to first frame
        if (vtype == 2) {
            const char* anim = mesh->state_anim.c_str();
            if (anim && anim[0] != '\0') {
                // Use tiny transition time so the blend completes almost instantly
                if (mesh_play_v3c_action(vmesh, anim, 0.001f)) {
                    // Process enough dt to fully complete the blend transition
                    vmesh_process(vmesh, 0.01f, 0, &mesh->pos, &mesh->orient, 1);
                }
            }
        }
        // Apply texture overrides
        mesh_apply_texture_overrides(mesh);
        xlog::info("[Mesh] Loaded vmesh for '{}' -> {:p}", filename, vmesh);
    }
    else {
        xlog::warn("[Mesh] Failed to load vmesh for '{}'", filename);
    }
}

// Apply per-slot texture overrides to a mesh's vmesh
static void mesh_apply_texture_overrides(DedMesh* mesh)
{
    if (!mesh || !mesh->vmesh) return;

    bool has_any = false;
    for (int ti = 0; ti < MAX_MESH_TEXTURES; ti++) {
        const char* tex = mesh->texture_overrides[ti].c_str();
        if (tex && tex[0] != '\0') { has_any = true; break; }
    }
    if (!has_any) return;

    int num_materials = 0;
    EditorMeshMaterial* materials = nullptr;
    editor_vmesh_get_materials_array(mesh->vmesh, &num_materials, &materials);
    if (!materials || num_materials <= 0) {
        // editor_vmesh_get_materials_array unconditionally sets use_replacement_materials=1
        // even when allocation fails (e.g. V3M with multiple LODs/sub-meshes).
        // Clear the flag to prevent the render code from dereferencing a null pointer.
        auto* vbytes = reinterpret_cast<uint8_t*>(mesh->vmesh);
        *(vbytes + 0x54) = 0;
        *reinterpret_cast<void**>(vbytes + 0x50) = nullptr;

        // For V3M meshes with multiple LODs or sub-mesh groups, the engine's replacement
        // materials allocator (FUN_004c0ae0) bails early. However the render code applies
        // replacement materials to ALL LODs from a single set. Work around the limitation
        // by temporarily faking single-LOD/single-submesh counts during allocation.
        int vtype = vmesh_get_type(mesh->vmesh);
        if (vtype == 1) { // V3M
            auto* instance = *reinterpret_cast<uint8_t**>(vbytes + 0x04);
            if (instance) {
                int* lod_count_ptr = reinterpret_cast<int*>(instance + 0x50);
                int** submesh_list_ptr = reinterpret_cast<int**>(instance + 0x54);
                int orig_lod_count = *lod_count_ptr;
                int orig_submesh_count = (submesh_list_ptr && *submesh_list_ptr) ? **submesh_list_ptr : 1;

                // Temporarily set counts to 1 so the allocator proceeds
                *lod_count_ptr = 1;
                if (submesh_list_ptr && *submesh_list_ptr)
                    **submesh_list_ptr = 1;

                editor_vmesh_get_materials_array(mesh->vmesh, &num_materials, &materials);

                // Restore original counts
                *lod_count_ptr = orig_lod_count;
                if (submesh_list_ptr && *submesh_list_ptr)
                    **submesh_list_ptr = orig_submesh_count;

                if (!materials || num_materials <= 0) {
                    *(vbytes + 0x54) = 0;
                    xlog::warn("[Mesh] Failed to allocate replacement materials for multi-LOD V3M");
                    return;
                }
                xlog::info("[Mesh] Allocated replacement materials for multi-LOD V3M ({} materials)", num_materials);
            }
            else {
                return;
            }
        }
        else {
            return;
        }
    }

    for (int ti = 0; ti < MAX_MESH_TEXTURES; ti++) {
        const char* tex = mesh->texture_overrides[ti].c_str();
        if (!tex || tex[0] == '\0') continue;
        if (ti >= num_materials) {
            xlog::warn("[Mesh] Texture override slot {} exceeds material count {}", ti, num_materials);
            break;
        }

        int bm_handle = bm_load(tex, -1, 1);
        if (bm_handle < 0) {
            xlog::warn("[Mesh] Failed to load texture override '{}' for slot {}", tex, ti);
            continue;
        }
        materials[ti].texture_maps[0].tex_handle = bm_handle;
        xlog::info("[Mesh] Applied texture override slot {}: '{}' (handle={})", ti, tex, bm_handle);
    }
}

// Free all VString members before deleting a DedMesh
static void destroy_ded_mesh(DedMesh* mesh)
{
    if (!mesh) return;
    // Free vmesh
    if (mesh->vmesh) {
        g_v3c_action_cache.erase(mesh->vmesh);
        vmesh_free(mesh->vmesh);
        mesh->vmesh = 0;
    }
    // Free VString members from DedObject base
    vstring_free(mesh->field_4);
    vstring_free(mesh->script_name);
    vstring_free(mesh->class_name);
    // Free VString members from DedMesh
    vstring_free(mesh->mesh_filename);
    vstring_free(mesh->state_anim);
    for (int ti = 0; ti < MAX_MESH_TEXTURES; ti++) {
        vstring_free(mesh->texture_overrides[ti]);
    }
    delete mesh;
}

// ─── RFL Serialization ──────────────────────────────────────────────────────

static void write_rfl_string(rf::File& file, const VString& str)
{
    const char* s = str.c_str();
    uint16_t len = static_cast<uint16_t>(strlen(s));
    file.write<uint16_t>(len);
    if (len > 0) {
        file.write(s, len);
    }
}

static std::string read_rfl_string(rf::File& file, std::size_t& remaining)
{
    if (remaining < 2) return "";
    uint16_t len = file.read<uint16_t>();
    remaining -= 2;
    if (len == 0 || remaining < len) {
        if (len > 0 && remaining < len) {
            // skip remaining
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

// Mesh chunk version: 1 = original, 2 = adds texture overrides
static constexpr uint32_t MESH_CHUNK_VERSION_MARKER = 0xAF000002;

void mesh_serialize_chunk(CDedLevel& level, rf::File& file)
{
    auto& meshes = level.GetAlpineLevelProperties().mesh_objects;
    if (meshes.empty()) return;

    auto start_pos = level.BeginRflSection(file, alpine_mesh_chunk_id);

    // Write version marker (distinguishes from old format where first uint32 was count)
    file.write<uint32_t>(MESH_CHUNK_VERSION_MARKER);

    uint32_t count = static_cast<uint32_t>(meshes.size());
    file.write<uint32_t>(count);

    for (auto* mesh : meshes) {
        // uid
        file.write<int32_t>(mesh->uid);
        // pos
        file.write<float>(mesh->pos.x);
        file.write<float>(mesh->pos.y);
        file.write<float>(mesh->pos.z);
        // orient (3x3 matrix, row-major)
        file.write<float>(mesh->orient.rvec.x);
        file.write<float>(mesh->orient.rvec.y);
        file.write<float>(mesh->orient.rvec.z);
        file.write<float>(mesh->orient.uvec.x);
        file.write<float>(mesh->orient.uvec.y);
        file.write<float>(mesh->orient.uvec.z);
        file.write<float>(mesh->orient.fvec.x);
        file.write<float>(mesh->orient.fvec.y);
        file.write<float>(mesh->orient.fvec.z);
        // script_name
        write_rfl_string(file, mesh->script_name);
        // mesh_filename
        write_rfl_string(file, mesh->mesh_filename);
        // state_anim
        write_rfl_string(file, mesh->state_anim);
        // collision mode
        file.write<uint8_t>(mesh->collision_mode);
        // links
        int32_t link_count = mesh->links.get_size();
        file.write<int32_t>(link_count);
        for (int i = 0; i < link_count; i++) {
            file.write<int32_t>(mesh->links[i]);
        }
        // v2: texture overrides
        for (int ti = 0; ti < MAX_MESH_TEXTURES; ti++) {
            write_rfl_string(file, mesh->texture_overrides[ti]);
        }
    }

    level.EndRflSection(file, start_pos);
    xlog::debug("[Mesh] Serialized {} mesh objects (v2)", count);
}

void mesh_deserialize_chunk(CDedLevel& level, rf::File& file, std::size_t chunk_len)
{
    auto& meshes = level.GetAlpineLevelProperties().mesh_objects;
    std::size_t remaining = chunk_len;

    auto read_bytes = [&](void* dst, std::size_t n) -> bool {
        if (remaining < n) return false;
        int got = file.read(dst, n);
        if (got != static_cast<int>(n)) return false;
        remaining -= n;
        return true;
    };

    // Check for version marker vs old format (first uint32 was count directly)
    uint32_t first_word = 0;
    if (!read_bytes(&first_word, sizeof(first_word))) return;

    bool has_texture_overrides = false;
    uint32_t count = 0;
    if (first_word == MESH_CHUNK_VERSION_MARKER) {
        has_texture_overrides = true;
        if (!read_bytes(&count, sizeof(count))) return;
    } else {
        count = first_word; // old format: first word is count
    }
    if (count > 10000) count = 10000;

    for (uint32_t i = 0; i < count; i++) {
        auto* mesh = new DedMesh();
        memset(static_cast<DedObject*>(mesh), 0, sizeof(DedObject));
        // Zero-init new fields (VStrings after DedObject base)
        memset(&mesh->texture_overrides, 0, sizeof(mesh->texture_overrides));
        mesh->vtbl = reinterpret_cast<void*>(0x55712c); // base DedObject vtable
        mesh->type = DedObjectType::DED_MESH;
        mesh->collision_mode = 2; // All

        // uid
        if (!read_bytes(&mesh->uid, sizeof(mesh->uid))) { destroy_ded_mesh(mesh); return; }
        // pos
        if (!read_bytes(&mesh->pos.x, sizeof(float))) { destroy_ded_mesh(mesh); return; }
        if (!read_bytes(&mesh->pos.y, sizeof(float))) { destroy_ded_mesh(mesh); return; }
        if (!read_bytes(&mesh->pos.z, sizeof(float))) { destroy_ded_mesh(mesh); return; }
        // orient
        if (!read_bytes(&mesh->orient.rvec.x, sizeof(float))) { destroy_ded_mesh(mesh); return; }
        if (!read_bytes(&mesh->orient.rvec.y, sizeof(float))) { destroy_ded_mesh(mesh); return; }
        if (!read_bytes(&mesh->orient.rvec.z, sizeof(float))) { destroy_ded_mesh(mesh); return; }
        if (!read_bytes(&mesh->orient.uvec.x, sizeof(float))) { destroy_ded_mesh(mesh); return; }
        if (!read_bytes(&mesh->orient.uvec.y, sizeof(float))) { destroy_ded_mesh(mesh); return; }
        if (!read_bytes(&mesh->orient.uvec.z, sizeof(float))) { destroy_ded_mesh(mesh); return; }
        if (!read_bytes(&mesh->orient.fvec.x, sizeof(float))) { destroy_ded_mesh(mesh); return; }
        if (!read_bytes(&mesh->orient.fvec.y, sizeof(float))) { destroy_ded_mesh(mesh); return; }
        if (!read_bytes(&mesh->orient.fvec.z, sizeof(float))) { destroy_ded_mesh(mesh); return; }
        // script_name
        std::string sname = read_rfl_string(file, remaining);
        mesh->script_name.assign_0(sname.c_str());
        // mesh_filename
        std::string mfname = read_rfl_string(file, remaining);
        mesh->mesh_filename.assign_0(mfname.c_str());
        // state_anim
        std::string sanim = read_rfl_string(file, remaining);
        mesh->state_anim.assign_0(sanim.c_str());
        // collision mode
        uint8_t collision_mode = 2;
        if (!read_bytes(&collision_mode, sizeof(collision_mode))) { destroy_ded_mesh(mesh); return; }
        mesh->collision_mode = (collision_mode <= 2) ? collision_mode : 2;
        // links
        int32_t link_count = 0;
        if (!read_bytes(&link_count, sizeof(link_count))) { destroy_ded_mesh(mesh); return; }
        if (link_count > 1000) link_count = 1000;
        for (int32_t j = 0; j < link_count; j++) {
            int32_t link_uid = 0;
            if (!read_bytes(&link_uid, sizeof(link_uid))) { destroy_ded_mesh(mesh); return; }
            mesh->links.add_if_not_exists_int(link_uid);
        }
        // v2: texture overrides
        if (has_texture_overrides) {
            for (int ti = 0; ti < MAX_MESH_TEXTURES; ti++) {
                std::string tex = read_rfl_string(file, remaining);
                mesh->texture_overrides[ti].assign_0(tex.c_str());
            }
        }

        // Don't load vmesh here - the RFL file is still open and loading a v3c
        // mesh during chunk parsing conflicts with the file I/O system.
        // The render hook's lazy-load will handle it on the first frame.
        mesh->vmesh = nullptr;
        mesh->vmesh_load_failed = false;

        meshes.push_back(mesh);
        xlog::debug("[Mesh] Loaded mesh uid={} file='{}' pos=({},{},{})",
            mesh->uid, mfname, mesh->pos.x, mesh->pos.y, mesh->pos.z);
    }

    // skip any remaining data
    if (remaining > 0) {
        file.seek(static_cast<int>(remaining), rf::File::seek_cur);
    }

    xlog::debug("[Mesh] Deserialized {} mesh objects (v{})", count, has_texture_overrides ? 2 : 1);
}

// ─── Property Dialog ────────────────────────────────────────────────────────

static std::vector<DedMesh*> g_selected_meshes;
static CDedLevel* g_current_level = nullptr;

// Sentinel strings for multi-selection fields with differing values
static const char* const MULTIPLE_STR = "<multiple>";
static const int MULTIPLE_COLLISION = -1;

// Track initial dialog values to detect user changes
static std::string g_init_script_name;
static std::string g_init_filename;
static std::string g_init_state_anim;
static int g_init_collision_mode;
static std::string g_init_textures[MAX_MESH_TEXTURES];

static constexpr int TEXTURE_CTRL_IDS[MAX_MESH_TEXTURES] = {
    IDC_MESH_TEX0, IDC_MESH_TEX1, IDC_MESH_TEX2, IDC_MESH_TEX3,
    IDC_MESH_TEX4, IDC_MESH_TEX5, IDC_MESH_TEX6
};

// Flags: set to true by IDOK if mesh filenames/anims were changed, so caller can reload
static bool g_mesh_filename_changed = false;
static bool g_mesh_anim_changed = false;

// Returns the file extension (lowercase, with dot) from a filename, or empty string
static std::string get_mesh_extension(const char* filename)
{
    if (!filename || !filename[0]) return "";
    const char* dot = strrchr(filename, '.');
    if (!dot) return "";
    std::string ext = dot;
    for (auto& c : ext) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return ext;
}

// Update dialog controls based on mesh type and material info
static void mesh_dialog_update_state(HWND hdlg)
{
    char fname_buf[MAX_PATH] = {};
    GetDlgItemTextA(hdlg, IDC_MESH_FILENAME, fname_buf, sizeof(fname_buf));
    bool has_filename = (fname_buf[0] != '\0');

    std::string ext = get_mesh_extension(fname_buf);

    // Set mesh type label
    const char* type_label = "";
    if (has_filename) {
        if (ext == ".v3m")
            type_label = "Specified mesh is static (.v3m)";
        else if (ext == ".v3c")
            type_label = "Specified mesh is skeletal (.v3c)";
        else if (ext == ".vfx")
            type_label = "Specified mesh is animated (.vfx)";
        else
            type_label = "Unknown mesh type";
    }
    SetDlgItemTextA(hdlg, IDC_MESH_TYPE_LABEL, type_label);

    // State anim: only applicable for V3C
    bool enable_state_anim = has_filename && (ext == ".v3c");
    EnableWindow(GetDlgItem(hdlg, IDC_MESH_STATE_ANIM), enable_state_anim);
    EnableWindow(GetDlgItem(hdlg, IDC_MESH_PREVIEW), enable_state_anim && g_selected_meshes.size() == 1);

    // Collision: not applicable for VFX
    bool enable_collision = has_filename && (ext != ".vfx");
    EnableWindow(GetDlgItem(hdlg, IDC_MESH_COLLISION_MODE), enable_collision);

    // Texture overrides: grey out all slots if no filename specified
    for (int ti = 0; ti < MAX_MESH_TEXTURES; ti++) {
        EnableWindow(GetDlgItem(hdlg, TEXTURE_CTRL_IDS[ti]), has_filename);
    }
}

static INT_PTR CALLBACK MeshDialogProc(HWND hdlg, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_INITDIALOG:
    {
        if (g_selected_meshes.empty()) return FALSE;

        auto* first = g_selected_meshes[0];
        bool all_same_script = true, all_same_filename = true;
        bool all_same_anim = true, all_same_collision = true;

        bool all_same_tex[MAX_MESH_TEXTURES];
        for (int ti = 0; ti < MAX_MESH_TEXTURES; ti++) all_same_tex[ti] = true;

        for (size_t i = 1; i < g_selected_meshes.size(); i++) {
            auto* m = g_selected_meshes[i];
            if (strcmp(m->script_name.c_str(), first->script_name.c_str()) != 0) all_same_script = false;
            if (strcmp(m->mesh_filename.c_str(), first->mesh_filename.c_str()) != 0) all_same_filename = false;
            if (strcmp(m->state_anim.c_str(), first->state_anim.c_str()) != 0) all_same_anim = false;
            if (m->collision_mode != first->collision_mode) all_same_collision = false;
            for (int ti = 0; ti < MAX_MESH_TEXTURES; ti++) {
                if (strcmp(m->texture_overrides[ti].c_str(), first->texture_overrides[ti].c_str()) != 0)
                    all_same_tex[ti] = false;
            }
        }

        g_init_script_name = all_same_script ? first->script_name.c_str() : MULTIPLE_STR;
        g_init_filename = all_same_filename ? first->mesh_filename.c_str() : MULTIPLE_STR;
        g_init_state_anim = all_same_anim ? first->state_anim.c_str() : MULTIPLE_STR;
        g_init_collision_mode = all_same_collision ? first->collision_mode : MULTIPLE_COLLISION;
        for (int ti = 0; ti < MAX_MESH_TEXTURES; ti++) {
            g_init_textures[ti] = all_same_tex[ti] ? first->texture_overrides[ti].c_str() : MULTIPLE_STR;
        }

        SetDlgItemTextA(hdlg, IDC_MESH_SCRIPT_NAME, g_init_script_name.c_str());
        SetDlgItemTextA(hdlg, IDC_MESH_FILENAME, g_init_filename.c_str());
        SetDlgItemTextA(hdlg, IDC_MESH_STATE_ANIM, g_init_state_anim.c_str());
        for (int ti = 0; ti < MAX_MESH_TEXTURES; ti++) {
            SetDlgItemTextA(hdlg, TEXTURE_CTRL_IDS[ti], g_init_textures[ti].c_str());
        }

        {
            HWND combo = GetDlgItem(hdlg, IDC_MESH_COLLISION_MODE);
            SendMessageA(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("None"));
            SendMessageA(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Only Weapons"));
            SendMessageA(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("All"));
            if (g_init_collision_mode == MULTIPLE_COLLISION) {
                SendMessageA(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Undefined"));
                SendMessageA(combo, CB_SETCURSEL, 3, 0); // "Undefined"
            } else {
                SendMessageA(combo, CB_SETCURSEL, g_init_collision_mode, 0);
            }
        }

        // Disable links and preview buttons for multi-selection
        if (g_selected_meshes.size() > 1) {
            EnableWindow(GetDlgItem(hdlg, ID_LINKS), FALSE);
        }

        mesh_dialog_update_state(hdlg);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IDC_MESH_FILENAME:
            if (HIWORD(wparam) == EN_CHANGE) {
                mesh_dialog_update_state(hdlg);
            }
            return TRUE;

        case IDC_MESH_BROWSE:
        {
            char filename[MAX_PATH] = {};
            OPENFILENAMEA ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hdlg;
            ofn.lpstrFilter = "Mesh Files (*.v3m;*.v3c;*.vfx)\0*.v3m;*.v3c;*.vfx\0All Files (*.*)\0*.*\0";
            ofn.lpstrFile = filename;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameA(&ofn)) {
                const char* base = strrchr(filename, '\\');
                if (!base) base = strrchr(filename, '/');
                if (base) base++; else base = filename;
                SetDlgItemTextA(hdlg, IDC_MESH_FILENAME, base);
                mesh_dialog_update_state(hdlg);
            }
            return TRUE;
        }

        case IDC_MESH_PREVIEW:
        {
            if (g_selected_meshes.size() != 1) return TRUE;
            auto* mesh = g_selected_meshes[0];
            if (!mesh) return TRUE;

            // Get current values from dialog fields
            char fname_buf[MAX_PATH] = {};
            GetDlgItemTextA(hdlg, IDC_MESH_FILENAME, fname_buf, sizeof(fname_buf));
            char anim_buf[MAX_PATH] = {};
            GetDlgItemTextA(hdlg, IDC_MESH_STATE_ANIM, anim_buf, sizeof(anim_buf));

            // Reload vmesh if filename changed or not loaded yet
            const char* cur_filename = mesh->mesh_filename.c_str();
            if (!mesh->vmesh || _stricmp(cur_filename, fname_buf) != 0) {
                // Apply filename to mesh so load uses it
                mesh->mesh_filename.assign_0(fname_buf);
                if (mesh->vmesh) {
                    g_v3c_action_cache.erase(mesh->vmesh);
                    vmesh_free(mesh->vmesh);
                    mesh->vmesh = nullptr;
                }
                mesh->vmesh_load_failed = false;
                mesh_load_vmesh(mesh);
            }

            if (mesh->vmesh && anim_buf[0] != '\0') {
                // Play the animation and start preview timer
                mesh_play_v3c_action(mesh->vmesh, anim_buf);
                g_preview_mesh = mesh;
                g_preview_timer = PREVIEW_DURATION;
            }
            return TRUE;
        }

        case ID_LINKS:
        {
            if (g_current_level && g_selected_meshes.size() == 1) {
                static auto& open_links_dialog = addr_as_ref<void(CDedLevel* level, DedObject* obj)>(0x00469780);
                open_links_dialog(g_current_level, static_cast<DedObject*>(g_selected_meshes[0]));
            }
            return TRUE;
        }

        case IDOK:
        {
            char buf[MAX_PATH] = {};

            // Only apply fields that the user actually changed from the initial value
            GetDlgItemTextA(hdlg, IDC_MESH_SCRIPT_NAME, buf, sizeof(buf));
            bool script_changed = (strcmp(buf, g_init_script_name.c_str()) != 0);

            char fname_buf[MAX_PATH] = {};
            GetDlgItemTextA(hdlg, IDC_MESH_FILENAME, fname_buf, sizeof(fname_buf));
            bool filename_changed = (strcmp(fname_buf, g_init_filename.c_str()) != 0);

            char anim_buf[MAX_PATH] = {};
            GetDlgItemTextA(hdlg, IDC_MESH_STATE_ANIM, anim_buf, sizeof(anim_buf));
            bool anim_changed = (strcmp(anim_buf, g_init_state_anim.c_str()) != 0);

            HWND combo = GetDlgItem(hdlg, IDC_MESH_COLLISION_MODE);
            int collision_sel = static_cast<int>(SendMessageA(combo, CB_GETCURSEL, 0, 0));
            // "Undefined" is index 3 — only present in multi-select mode
            bool collision_changed = false;
            if (g_init_collision_mode == MULTIPLE_COLLISION) {
                collision_changed = (collision_sel != 3); // changed from "Undefined"
            } else {
                collision_changed = (collision_sel != g_init_collision_mode);
            }

            // Check texture override changes
            char tex_bufs[MAX_MESH_TEXTURES][MAX_PATH] = {};
            bool tex_changed[MAX_MESH_TEXTURES] = {};
            bool any_tex_changed = false;
            for (int ti = 0; ti < MAX_MESH_TEXTURES; ti++) {
                GetDlgItemTextA(hdlg, TEXTURE_CTRL_IDS[ti], tex_bufs[ti], MAX_PATH);
                tex_changed[ti] = (strcmp(tex_bufs[ti], g_init_textures[ti].c_str()) != 0);
                if (tex_changed[ti]) any_tex_changed = true;
            }

            for (size_t idx = 0; idx < g_selected_meshes.size(); idx++) {
                auto* mesh = g_selected_meshes[idx];
                if (!mesh) continue;
                if (script_changed) mesh->script_name.assign_0(buf);
                if (filename_changed) mesh->mesh_filename.assign_0(fname_buf);
                if (anim_changed) mesh->state_anim.assign_0(anim_buf);
                if (collision_changed && collision_sel >= 0 && collision_sel <= 2) {
                    mesh->collision_mode = static_cast<uint8_t>(collision_sel);
                }
                for (int ti = 0; ti < MAX_MESH_TEXTURES; ti++) {
                    if (tex_changed[ti]) mesh->texture_overrides[ti].assign_0(tex_bufs[ti]);
                }
            }
            // Defer vmesh reload until after dialog closes to avoid message pump issues
            g_mesh_filename_changed = filename_changed || any_tex_changed;
            g_mesh_anim_changed = anim_changed;

            EndDialog(hdlg, IDOK);
            return TRUE;
        }

        case IDCANCEL:
            EndDialog(hdlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

void ShowMeshPropertiesDialog(DedMesh* mesh)
{
    // This overload exists for single-mesh callers
    g_selected_meshes.clear();
    g_selected_meshes.push_back(mesh);
    g_current_level = CDedLevel::Get();
    g_mesh_filename_changed = false;
    g_mesh_anim_changed = false;

    DialogBoxParam(
        reinterpret_cast<HINSTANCE>(&__ImageBase),
        MAKEINTRESOURCE(IDD_ALPINE_MESH_PROPERTIES),
        GetActiveWindow(),
        MeshDialogProc,
        0
    );

    // Free old vmesh and let render hook lazy-load the new one
    if (g_mesh_filename_changed && mesh) {
        if (mesh->vmesh) {
            g_v3c_action_cache.erase(mesh->vmesh);
            vmesh_free(mesh->vmesh);
            mesh->vmesh = nullptr;
        }
        mesh->vmesh_load_failed = false;
    }
    // If only anim changed (not filename), fully reload vmesh to cleanly switch.
    // Freeing + clearing load flag lets the render loop lazy-load with the new anim.
    else if (g_mesh_anim_changed && mesh) {
        if (mesh->vmesh) {
            g_v3c_action_cache.erase(mesh->vmesh);
            vmesh_free(mesh->vmesh);
            mesh->vmesh = nullptr;
        }
        mesh->vmesh_load_failed = false;
    }

    g_selected_meshes.clear();
    g_current_level = nullptr;
}

// ─── UID Generation ─────────────────────────────────────────────────────────

// Stock UID generator scans all brushes and objects for max UID, returns max+1
static auto& generate_uid = addr_as_ref<int __cdecl()>(0x00484230);

static int generate_mesh_uid()
{
    int uid = generate_uid();
    // Also check mesh objects to avoid collisions
    auto* level = CDedLevel::Get();
    if (level) {
        for (auto* m : level->GetAlpineLevelProperties().mesh_objects) {
            if (m->uid >= uid) uid = m->uid + 1;
        }
    }
    return uid;
}

// ─── Object Lifecycle ───────────────────────────────────────────────────────

// FUN_004835b0: get active 3D viewport
static auto& get_active_viewport = addr_as_ref<void* __cdecl()>(0x004835b0);

void PlaceNewMeshObject()
{
    auto* level = CDedLevel::Get();
    if (!level) return;

    auto* mesh = new DedMesh();
    memset(mesh, 0, sizeof(DedMesh));
    mesh->vtbl = reinterpret_cast<void*>(0x55712c); // base DedObject vtable
    mesh->type = DedObjectType::DED_MESH;
    mesh->collision_mode = 2; // All

    // Default values
    mesh->script_name.assign_0("Mesh");
    mesh->mesh_filename.assign_0("barrel.v3m");

    // Get camera position and orientation from the active viewport
    void* viewport = get_active_viewport();
    if (viewport) {
        void* view_data = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(viewport) + 0x54);
        if (view_data) {
            // Position at view_data + 0x28 (Vector3)
            auto* cam_pos = reinterpret_cast<Vector3*>(reinterpret_cast<uintptr_t>(view_data) + 0x28);
            mesh->pos = *cam_pos;
            // Orientation at view_data + 0x4 (Matrix3)
            auto* cam_orient = reinterpret_cast<Matrix3*>(reinterpret_cast<uintptr_t>(view_data) + 0x4);
            mesh->orient = *cam_orient;
        }
    }

    // Fallback if no viewport data
    if (mesh->pos.x == 0.0f && mesh->pos.y == 0.0f && mesh->pos.z == 0.0f) {
        mesh->orient.rvec = {1.0f, 0.0f, 0.0f};
        mesh->orient.uvec = {0.0f, 1.0f, 0.0f};
        mesh->orient.fvec = {0.0f, 0.0f, 1.0f};
    }

    mesh->uid = generate_mesh_uid();

    // Add to level (vmesh will be loaded lazily on first render)
    level->GetAlpineLevelProperties().mesh_objects.push_back(mesh);

    // Deselect all, then select the new mesh
    AddrCaller{0x00419eb0}.this_call(level);
    auto* sel = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(level) + 0x298);
    AddrCaller{0x00416BF0}.this_call(sel, static_cast<DedObject*>(mesh));

    // Update console display to show selected object info
    AddrCaller{0x00423460}.this_call(level);

    xlog::info("[Mesh] Placed new mesh uid={} at ({},{},{})", mesh->uid, mesh->pos.x, mesh->pos.y, mesh->pos.z);
}

DedMesh* CloneMeshObject(DedMesh* source, bool add_to_level)
{
    if (!source) return nullptr;

    auto* mesh = new DedMesh();
    memset(mesh, 0, sizeof(DedMesh));
    mesh->vtbl = reinterpret_cast<void*>(0x55712c); // base DedObject vtable
    mesh->type = DedObjectType::DED_MESH;

    // Copy position and orientation
    mesh->pos = source->pos;
    mesh->orient = source->orient;

    // Copy properties
    mesh->script_name.assign_0(source->script_name.c_str());
    mesh->mesh_filename.assign_0(source->mesh_filename.c_str());
    mesh->state_anim.assign_0(source->state_anim.c_str());
    mesh->collision_mode = source->collision_mode;
    for (int ti = 0; ti < MAX_MESH_TEXTURES; ti++) {
        mesh->texture_overrides[ti].assign_0(source->texture_overrides[ti].c_str());
    }

    // Generate new UID
    mesh->uid = generate_mesh_uid();

    // Load vmesh for rendering
    mesh_load_vmesh(mesh);

    // Add to level if requested (false during copy/staging, true during direct placement)
    if (add_to_level) {
        auto* level = CDedLevel::Get();
        if (level) {
            level->GetAlpineLevelProperties().mesh_objects.push_back(mesh);
        }
    }

    xlog::info("[Mesh] Cloned mesh uid={} from uid={} add_to_level={}",
        mesh->uid, source->uid, add_to_level);
    return mesh;
}

void DeleteMeshObject(DedMesh* mesh)
{
    if (!mesh) return;
    auto* level = CDedLevel::Get();
    if (!level) return;

    auto& meshes = level->GetAlpineLevelProperties().mesh_objects;
    auto it = std::find(meshes.begin(), meshes.end(), mesh);
    if (it != meshes.end()) {
        meshes.erase(it);
        xlog::info("[Mesh] Deleted mesh uid={} (mesh_objects count={})", mesh->uid, meshes.size());
    }
    else {
        xlog::warn("[Mesh] DeleteMeshObject: mesh uid={} not found in mesh_objects!", mesh->uid);
    }
    destroy_ded_mesh(mesh);
}

// ─── Editor Hooks ───────────────────────────────────────────────────────────

// Hook into object properties dialog dispatcher at the type switch point.
// At 0x0040200e: EAX = object type, EBX = first selected DedObject*,
// ESI = CDedLevel*. Stack has saved EBX/EBP/ESI/EDI from function prologue.
// For DED_MESH (0x17), show our dialog and jump to common return at 0x00402293.
CodeInjection open_mesh_properties_patch{
    0x0040200e,
    [](auto& regs) {
        if (regs.eax != static_cast<int>(DedObjectType::DED_MESH)) return;

        auto* level = reinterpret_cast<CDedLevel*>(static_cast<uintptr_t>(regs.esi));
        auto& sel = level->selection;
        if (sel.get_size() < 1) { regs.eip = 0x00402293; return; }

        // Collect all selected mesh objects
        g_selected_meshes.clear();
        g_current_level = level;
        xlog::info("[Mesh] Properties: selection has {} items", sel.get_size());
        for (int i = 0; i < sel.get_size(); i++) {
            DedObject* obj = sel[i];
            xlog::info("[Mesh] Properties: sel[{}] ptr={:p} type=0x{:x} uid={}",
                i, static_cast<void*>(obj),
                obj ? static_cast<int>(obj->type) : -1,
                obj ? obj->uid : -1);
            if (obj && obj->type == DedObjectType::DED_MESH) {
                g_selected_meshes.push_back(static_cast<DedMesh*>(obj));
            }
        }
        xlog::info("[Mesh] Properties: collected {} mesh objects, vector data={:p}",
            g_selected_meshes.size(), static_cast<void*>(g_selected_meshes.data()));

        if (!g_selected_meshes.empty()) {
            g_mesh_filename_changed = false;
            g_mesh_anim_changed = false;
            DialogBoxParam(
                reinterpret_cast<HINSTANCE>(&__ImageBase),
                MAKEINTRESOURCE(IDD_ALPINE_MESH_PROPERTIES),
                GetActiveWindow(),
                MeshDialogProc,
                0
            );
            // If filename changed, free old vmeshes and reset flags so the render
            // hook's lazy-load path reloads them on the next frame. We avoid calling
            // mesh_load_vmesh here because VFX loading can trigger exceptions that
            // get caught by CodeInjection's SEH handler, silently aborting this lambda.
            if (g_mesh_filename_changed) {
                for (auto* mesh : g_selected_meshes) {
                    if (!mesh) continue;
                    if (mesh->vmesh) {
                        g_v3c_action_cache.erase(mesh->vmesh);
                        vmesh_free(mesh->vmesh);
                        mesh->vmesh = nullptr;
                    }
                    mesh->vmesh_load_failed = false;
                }
            }
            // If only anim changed, fully reload vmeshes to cleanly switch
            else if (g_mesh_anim_changed) {
                for (auto* mesh : g_selected_meshes) {
                    if (!mesh) continue;
                    if (mesh->vmesh) {
                        g_v3c_action_cache.erase(mesh->vmesh);
                        vmesh_free(mesh->vmesh);
                        mesh->vmesh = nullptr;
                    }
                    mesh->vmesh_load_failed = false;
                }
            }
        }

        g_selected_meshes.clear();
        g_current_level = nullptr;
        regs.eip = 0x00402293;
    }
};

// Note: mesh save/load hooks are integrated into CDedLevel_SaveLevel_patch and
// CDedLevel_LoadLevel_patch2 in level.cpp to avoid CodeInjection address conflicts.

// Hook into the editor's 3D render function to also render mesh objects.
// Inject after the main object render loop in FUN_0041f6d0, before the icon pass.

// FUN_004cc2f0 draws a 3D arrow (line with arrowhead) - used for orientation axes
using DrawArrowFn = void(__cdecl*)(float, float, float, float, float, float, int, int, int);
static const auto draw_3d_arrow = reinterpret_cast<DrawArrowFn>(0x004cc2f0);

// Low-level rendering primitives for drawing plain lines without arrowheads:
// FUN_004c5e30: project 3D point to screen (48-byte output buffer, Vector3* input)
// FUN_004b9700: set drawing color (r, g, b, a)
// FUN_004cb150: draw 2D line between two projected points
using ProjectFn = uint32_t(__cdecl*)(void* screen_out, const void* world_pos);
using SetColorFn = void(__cdecl*)(uint32_t r, uint32_t g, uint32_t b, uint32_t a);
using DrawLine2DFn = uint32_t(__cdecl*)(const void* pt1, const void* pt2, uint32_t mode);
static const auto project_to_screen = reinterpret_cast<ProjectFn>(0x004c5e30);
static const auto set_draw_color = reinterpret_cast<SetColorFn>(0x004b9700);
static const auto draw_line_2d = reinterpret_cast<DrawLine2DFn>(0x004cb150);

// Draw a plain 3D line (no arrowhead) by projecting endpoints and drawing a 2D line
static void draw_3d_line(float x1, float y1, float z1, float x2, float y2, float z2, int r, int g, int b)
{
    uint8_t screen1[48] = {};
    uint8_t screen2[48] = {};
    float p1[3] = {x1, y1, z1};
    float p2[3] = {x2, y2, z2};

    project_to_screen(screen1, p1);
    project_to_screen(screen2, p2);

    // Check that projected points differ (not zero-length or behind camera)
    float sx1 = *reinterpret_cast<float*>(screen1);
    float sy1 = *reinterpret_cast<float*>(screen1 + 4);
    float sx2 = *reinterpret_cast<float*>(screen2);
    float sy2 = *reinterpret_cast<float*>(screen2 + 4);
    if (sx1 == sx2 && sy1 == sy2) return;

    set_draw_color(r, g, b, 0xff);
    draw_line_2d(screen1, screen2, *reinterpret_cast<uint32_t*>(0x0147d260));
}

static bool is_mesh_selected(CDedLevel* level, DedMesh* mesh)
{
    auto& sel = level->selection;
    for (int i = 0; i < sel.size; i++) {
        if (sel.data_ptr[i] == static_cast<DedObject*>(mesh)) return true;
    }
    return false;
}

static void draw_wireframe_sphere(float cx, float cy, float cz, float radius, int r, int g, int b)
{
    // Draw 3 circles (XY, XZ, YZ planes) using line segments
    constexpr int segments = 24;
    constexpr float pi2 = 6.2831853f;
    for (int i = 0; i < segments; i++) {
        float a0 = pi2 * i / segments;
        float a1 = pi2 * (i + 1) / segments;
        float c0 = cosf(a0) * radius, s0 = sinf(a0) * radius;
        float c1 = cosf(a1) * radius, s1 = sinf(a1) * radius;
        // XY circle
        draw_3d_line(cx + c0, cy + s0, cz, cx + c1, cy + s1, cz, r, g, b);
        // XZ circle
        draw_3d_line(cx + c0, cy, cz + s0, cx + c1, cy, cz + s1, r, g, b);
        // YZ circle
        draw_3d_line(cx, cy + c0, cz + s0, cx, cy + c1, cz + s1, r, g, b);
    }
}

CodeInjection mesh_render_patch{
    0x0041f9b2, // after main render loop, before icon/sprite pass
    [](auto& regs) {
        auto* level = CDedLevel::Get();
        if (!level) return;

        auto& meshes = level->GetAlpineLevelProperties().mesh_objects;
        constexpr float s = 0.5f;
        bool did_lazy_load = false;
        for (auto* mesh : meshes) {
            if (mesh->hidden_in_editor) continue;
            float x = mesh->pos.x, y = mesh->pos.y, z = mesh->pos.z;
            bool selected = is_mesh_selected(level, mesh);

            // Lazy-load vmesh on first render (one per frame to avoid VFX loader issues)
            bool just_loaded = false;
            if (!mesh->vmesh && !mesh->vmesh_load_failed && mesh->mesh_filename.c_str()[0] != '\0') {
                if (!did_lazy_load) {
                    mesh_load_vmesh(mesh);
                    did_lazy_load = true;
                    just_loaded = true;
                }
            }

            // Render the actual vmesh if loaded (skip on the frame it was just loaded
            // to avoid VFX render state conflicts from loading + rendering in same frame)
            if (mesh->vmesh && !just_loaded) {
                set_draw_color(0xff, 0xff, 0xff, 0xff);

                // Render params struct (~80 bytes) initialized by FUN_004be330.
                // Layout (from FUN_004be360 + FUN_004b1fc0):
                //   [+0x00] flags: bit 0x2 = textured, bit 0x20 = selection highlight
                //   [+0x04] = 0xFFFFFFFF
                //   [+0x08] = 0xFF
                //   [+0x0C] diffuse color (4 bytes RGBA)
                //   [+0x10] = 0
                //   [+0x14] = -1.0f
                //   [+0x18] selection color (4 bytes RGBA)
                //   [+0x1C] = 0
                //   [+0x20] = 0xFFFFFFFF
                //   [+0x24] = 0xFFFFFFFF
                //   [+0x2C] 3x3 identity matrix (36 bytes)
                uint8_t render_params[96] = {};
                // Initialize with stock defaults via FUN_004be330
                AddrCaller{0x004be330}.this_call(render_params);

                uint32_t& flags = *reinterpret_cast<uint32_t*>(&render_params[0]);

                // Check if textures are enabled (DAT_006c9aa8)
                if (*reinterpret_cast<int*>(0x006c9aa8) != 0) {
                    flags |= 0x2; // texture bit
                    // Set diffuse color to white (RGBA) at offset 0x0C
                    render_params[0x0C] = 0xff;
                    render_params[0x0D] = 0xff;
                    render_params[0x0E] = 0xff;
                    render_params[0x0F] = 0xff;
                }

                // Selection highlight
                if (selected) {
                    flags |= 0x20; // selection bit
                    // Set selection color (red) at offset 0x18
                    render_params[0x18] = 0xff;
                    render_params[0x19] = 0x00;
                    render_params[0x1A] = 0x00;
                    render_params[0x1B] = 0xff;
                }

                // Advance animation each frame for animated mesh types
                int vmesh_type = vmesh_get_type(mesh->vmesh);
                if (vmesh_type == 3) { // VFX
                    vmesh_process(mesh->vmesh, 0.03f, 0, &mesh->pos, &mesh->orient, 1);
                    *reinterpret_cast<int*>(0x0059e21c) = 1;
                }
                else if (vmesh_type == 2) { // V3C character
                    auto* instance = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(mesh->vmesh) + 0x04);
                    // One-shot debug: log slot state on first render
                    {
                        static int v3c_log_count = 0;
                        if (v3c_log_count < 5) {
                            auto* ib = instance ? reinterpret_cast<uint8_t*>(instance) : nullptr;
                            int sc = ib ? *reinterpret_cast<int*>(ib + 0x12D0) : -1;
                            char ff = ib ? *reinterpret_cast<char*>(ib + 0x1D4C) : -1;
                            xlog::info("[Mesh] Render v3c: instance={:p} slots={} freeze={} has_anim='{}'",
                                instance, sc, (int)ff, mesh->state_anim.c_str());
                            v3c_log_count++;
                        }
                    }
                    if (instance) {
                        if (g_preview_mesh == mesh && g_preview_timer > 0.0f) {
                            // Preview is active - advance animation in real time
                            vmesh_process(mesh->vmesh, 0.03f, 0, &mesh->pos, &mesh->orient, 1);
                            g_preview_timer -= 0.03f;
                            if (g_preview_timer <= 0.0f) {
                                g_preview_mesh = nullptr;
                                g_preview_timer = 0.0f;
                            }
                        }
                        else {
                            // Non-preview: re-trigger play_action every frame to reset
                            // the slot's start timestamp to "now". The animation system
                            // uses real system time to evaluate pose, so this keeps the
                            // pose perpetually at frame 0.
                            auto it = g_v3c_action_cache.find(mesh->vmesh);
                            if (it != g_v3c_action_cache.end() && it->second >= 0) {
                                vmesh_play_action_by_index(mesh->vmesh, it->second, 0.001f, 0);
                                vmesh_process(mesh->vmesh, 0.03f, 0, &mesh->pos, &mesh->orient, 1);
                            }
                        }
                    }
                }

                // Get bounding sphere radius for room visibility setup
                float bound_center[3] = {};
                float bound_radius = 0.0f;
                vmesh_get_bound_sphere(mesh->vmesh, bound_center, &bound_radius);

                // Room visibility setup (required for mesh rendering)
                using RoomSetupFn = int(__cdecl*)(int, const void*, float, int, int);
                using RoomCleanupFn = void(__cdecl*)();
                auto room_setup = reinterpret_cast<RoomSetupFn>(0x004885d0);
                auto room_cleanup = reinterpret_cast<RoomCleanupFn>(0x00488bb0);

                room_setup(0, &mesh->pos, bound_radius, 1, 1);
                vmesh_render(mesh->vmesh, &mesh->pos, &mesh->orient, render_params);
                room_cleanup();

                // Reset VFX transparency flag
                *reinterpret_cast<int*>(0x0059e21c) = 0;
            }

            // Draw 3D cross at mesh position (cyan normal, red if selected)
            int r = selected ? 255 : 0;
            int g = selected ? 0 : 255;
            int b = selected ? 0 : 255;
            draw_3d_line(x - s, y, z, x + s, y, z, r, g, b);
            draw_3d_line(x, y - s, z, x, y + s, z, r, g, b);
            draw_3d_line(x, y, z - s, x, y, z + s, r, g, b);

            // Draw orientation axes (RGB = XYZ) - arrows make sense here
            float al = selected ? 1.0f : 0.5f;
            draw_3d_arrow(x, y, z, x + mesh->orient.rvec.x * al, y + mesh->orient.rvec.y * al, z + mesh->orient.rvec.z * al, 255, 0, 0);
            draw_3d_arrow(x, y, z, x + mesh->orient.uvec.x * al, y + mesh->orient.uvec.y * al, z + mesh->orient.uvec.z * al, 0, 255, 0);
            draw_3d_arrow(x, y, z, x + mesh->orient.fvec.x * al, y + mesh->orient.fvec.y * al, z + mesh->orient.fvec.z * al, 0, 0, 255);

            // Draw wireframe sphere when selected (bounding indicator)
            if (selected) {
                draw_wireframe_sphere(x, y, z, 0.75f, 255, 255, 0);
            }
        }
    },
};

// Hook the tree view population function (FUN_00440590) to add "Meshes (%d)" section.
// Inject just before the finalization call FUN_00442250.
// At this point: ESI = tree control (this+0x5c), EDI = panel object.
// FUN_004422b0(__thiscall): insert tree item (label, parent_handle, sort_flags) -> handle
// FUN_00442320(__thiscall): set item data (item_handle, uid)
CodeInjection mesh_tree_patch{
    0x00441c89, // just before PUSH 1 / CALL FUN_00442250
    [](auto& regs) {
        auto* level = CDedLevel::Get();
        if (!level) return;
        auto& meshes = level->GetAlpineLevelProperties().mesh_objects;

        void* tree_ctrl = reinterpret_cast<void*>(static_cast<uintptr_t>(regs.esi));
        int master_groups = *reinterpret_cast<int*>(regs.edi + 0x98);

        char buf[64];
        snprintf(buf, sizeof(buf), "Meshes (%d)", static_cast<int>(meshes.size()));

        // Create "Meshes (%d)" parent node under Master_Groups
        int parent = AddrCaller{0x004422b0}.this_call<int>(tree_ctrl, buf, master_groups, 0xffff0002);

        for (auto* mesh : meshes) {
            const char* name = mesh->script_name.c_str();
            if (!name || name[0] == '\0') {
                name = mesh->mesh_filename.c_str();
            }
            if (!name || name[0] == '\0') {
                name = "(unnamed)";
            }
            int child = AddrCaller{0x004422b0}.this_call<int>(tree_ctrl, name, parent, 0xffff0002);
            AddrCaller{0x00442320}.this_call(tree_ctrl, child, mesh->uid);
        }
    },
};

// Hook the object picking function FUN_0042ae80, just before it calls FUN_00423460
// (console display). This ensures mesh objects are in the selection when the console
// display runs. At 0x0042aeea: EBX = CDedLevel*, stack has pick params.
// Stack layout (3 saved regs): [saved_EDI] [saved_EBP] [saved_EBX] [ret] [p1] [p2] [p3]
//                                ESP+0x00    +0x04       +0x08      +0x0C +0x10 +0x14 +0x18
CodeInjection mesh_pick_patch{
    0x0042aeea,
    [](auto& regs) {
        auto* level = reinterpret_cast<CDedLevel*>(static_cast<uintptr_t>(regs.ebx));
        if (!level) return;

        int param1 = *reinterpret_cast<int*>(regs.esp + 0x10);
        int param2 = *reinterpret_cast<int*>(regs.esp + 0x14);

        auto& meshes = level->GetAlpineLevelProperties().mesh_objects;
        for (auto* mesh : meshes) {
            if (mesh->hidden_in_editor) continue;
            bool hit = AddrCaller{0x0042AC00}.this_call<bool>(level, param1, param2, &mesh->pos);
            if (hit) {
                auto* sel = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(level) + 0x298);
                AddrCaller{0x00416BF0}.this_call(sel, static_cast<DedObject*>(mesh));
                xlog::info("[Mesh] Pick: selected mesh uid={} at ({},{},{})",
                    mesh->uid, mesh->pos.x, mesh->pos.y, mesh->pos.z);
            }
        }
    },
};

// Project a 3D position to screen coordinates.
// Returns true if visible, outputs screen x,y as floats.
using ProjectToScreen2DFn = bool(__cdecl*)(const void* world_pos, float* out_x, float* out_y);
static const auto project_to_screen_2d = reinterpret_cast<ProjectToScreen2DFn>(0x004c6630);

// Remove a specific object from a VArray selection
static void remove_from_selection(VArray<DedObject*>& sel, DedObject* obj)
{
    for (int i = 0; i < sel.size; i++) {
        if (sel.data_ptr[i] == obj) {
            for (int j = i; j < sel.size - 1; j++) {
                sel.data_ptr[j] = sel.data_ptr[j + 1];
            }
            sel.size--;
            return;
        }
    }
}

// Hook the click-pick handler FUN_0042bd10 at the CALL to FUN_0042b880 (ray-pick).
// At 0x0042bd1f: 5-byte CALL instruction, safe for CodeInjection.
// ECX = ESI = CDedLevel*. Stack: [pushed_arg1] [pushed_arg2] [saved_ESI] [ret] [arg1] [arg2] [arg3]
// We call stock ray-pick ourselves, then check mesh objects if nothing found.
// For mesh hits we handle selection directly to avoid stock code calling virtual
// methods on DedMesh objects.
CodeInjection mesh_click_pick_patch{
    0x0042bd1f, // CALL FUN_0042b880 (5 bytes)
    [](auto& regs) {
        auto* level = reinterpret_cast<CDedLevel*>(static_cast<uintptr_t>(regs.esi));
        if (!level) return;

        // Read the two params pushed for FUN_0042b880
        uintptr_t esp_val = static_cast<uintptr_t>(regs.esp);
        uintptr_t p1 = *reinterpret_cast<uintptr_t*>(esp_val);       // arg1 (click coords ptr)
        uintptr_t p2 = *reinterpret_cast<uintptr_t*>(esp_val + 0x04); // arg2

        // Call stock ray-pick ourselves
        void* picked = AddrCaller{0x0042b880}.this_call<void*>(level, p1, p2);

        if (!picked) {
            // Stock found nothing - check mesh objects using bounding sphere
            auto* click_ptr = reinterpret_cast<float*>(p1);
            float click_x = click_ptr[0];
            float click_y = click_ptr[1];

            auto& meshes = level->GetAlpineLevelProperties().mesh_objects;
            float best_dist_sq = 1e30f;
            DedMesh* best_mesh = nullptr;

            for (auto* mesh : meshes) {
                if (mesh->hidden_in_editor) continue;

                // Get bounding sphere (world-space center + radius)
                float bound_center[3] = {}, bound_radius = 0.5f;
                if (mesh->vmesh) {
                    vmesh_get_bound_sphere(mesh->vmesh, bound_center, &bound_radius);
                    if (bound_radius < 0.25f) bound_radius = 0.25f;
                }

                // Transform bound center from local to world space
                float world_cx = mesh->pos.x
                    + mesh->orient.rvec.x * bound_center[0]
                    + mesh->orient.uvec.x * bound_center[1]
                    + mesh->orient.fvec.x * bound_center[2];
                float world_cy = mesh->pos.y
                    + mesh->orient.rvec.y * bound_center[0]
                    + mesh->orient.uvec.y * bound_center[1]
                    + mesh->orient.fvec.y * bound_center[2];
                float world_cz = mesh->pos.z
                    + mesh->orient.rvec.z * bound_center[0]
                    + mesh->orient.uvec.z * bound_center[1]
                    + mesh->orient.fvec.z * bound_center[2];

                // Project world center to screen
                float center_pos[3] = {world_cx, world_cy, world_cz};
                float screen_cx = 0.0f, screen_cy = 0.0f;
                if (!project_to_screen_2d(center_pos, &screen_cx, &screen_cy))
                    continue;

                // Project an offset point to estimate screen-space radius
                float edge_pos[3] = {world_cx, world_cy + bound_radius, world_cz};
                float screen_ex = 0.0f, screen_ey = 0.0f;
                float screen_radius_sq;
                if (project_to_screen_2d(edge_pos, &screen_ex, &screen_ey)) {
                    float rdx = screen_ex - screen_cx;
                    float rdy = screen_ey - screen_cy;
                    screen_radius_sq = rdx * rdx + rdy * rdy;
                } else {
                    screen_radius_sq = 400.0f; // fallback 20px
                }
                // Minimum 10px screen radius for very small/distant meshes
                if (screen_radius_sq < 100.0f) screen_radius_sq = 100.0f;

                float dx = screen_cx - click_x;
                float dy = screen_cy - click_y;
                float dist_sq = dx * dx + dy * dy;
                if (dist_sq <= screen_radius_sq && dist_sq < best_dist_sq) {
                    best_dist_sq = dist_sq;
                    best_mesh = mesh;
                }
            }

            if (best_mesh) {
                // Handle selection ourselves to avoid stock virtual method calls on DedMesh
                uint8_t shift = *reinterpret_cast<uint8_t*>(esp_val + 0x18);
                if (!shift) {
                    AddrCaller{0x0042c7a0}.this_call(level); // deselect all
                }
                if (shift && is_mesh_selected(level, best_mesh)) {
                    remove_from_selection(level->selection, static_cast<DedObject*>(best_mesh));
                } else {
                    auto* sel = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(level) + 0x298);
                    AddrCaller{0x00416BF0}.this_call(sel, static_cast<DedObject*>(best_mesh));
                }
                xlog::info("[Mesh] Click-pick: selected mesh uid={}", best_mesh->uid);

                // Clean up the 2 pushed params (callee-cleaned by FUN_0042b880's RET 8)
                // and skip to console update + return
                regs.esp = static_cast<int32_t>(esp_val + 8);
                regs.eip = 0x0042bd4f;
                return;
            }
        }

        // Stock object found, or nothing found at all - let stock code handle it
        regs.eax = reinterpret_cast<uintptr_t>(picked);
        regs.esp = static_cast<int32_t>(esp_val + 8); // clean the 2 pushed params
        regs.eip = 0x0042bd24; // TEST EAX,EAX
    },
};

// Mesh clipboard: stores staged clones (with add_to_level=false) for Ctrl+V paste.
// Populated during Ctrl+C (copy), consumed during Ctrl+V (paste).
static std::vector<DedMesh*> g_mesh_clipboard;

// Clear the mesh clipboard, freeing any staged clones.
static void clear_mesh_clipboard()
{
    for (auto* mesh : g_mesh_clipboard) {
        if (mesh->vmesh) {
            vmesh_free(mesh->vmesh);
        }
        vstring_free(mesh->script_name);
        vstring_free(mesh->mesh_filename);
        vstring_free(mesh->state_anim);
        for (int ti = 0; ti < MAX_MESH_TEXTURES; ti++) {
            vstring_free(mesh->texture_overrides[ti]);
        }
        delete mesh;
    }
    g_mesh_clipboard.clear();
}

// Hook the start of FUN_00412e20 to clear clipboard for new copy operation.
// If mesh objects are in the selection, mesh_copy_hook will re-populate it.
// If not, it stays empty so paste won't create stale mesh clones.
CodeInjection mesh_copy_begin_hook{
    0x00412e20,
    [](auto& /*regs*/) {
        clear_mesh_clipboard();
    },
};

// Hook the copy function FUN_00412e20 at 0x00412ea1 (MOV EBX,[EAX] + CMP = 6 bytes).
// For DED_MESH objects, stage a clone to our clipboard instead of the stock clones VArray
// (CDedLevel+0x2EC). The clone is NOT added to the level yet — that happens on paste.
CodeInjection mesh_copy_hook{
    0x00412ea1,
    [](auto& regs) {
        // Peek at the source object (equivalent of MOV EBX,[EAX])
        auto* source = reinterpret_cast<DedObject*>(
            *reinterpret_cast<uintptr_t*>(static_cast<uintptr_t>(regs.eax)));
        if (source && source->type == DedObjectType::DED_MESH) {
            regs.ebx = reinterpret_cast<uintptr_t>(source);
            // Stage a clone to clipboard (NOT added to level)
            auto* staged = CloneMeshObject(static_cast<DedMesh*>(source), false);
            if (staged) {
                g_mesh_clipboard.push_back(staged);
                xlog::info("[Mesh] Copy: staged mesh uid={} to clipboard (clipboard size={})",
                    staged->uid, g_mesh_clipboard.size());
            }
            // Skip stock CloneObject call and UID=0 assignment, jump to loop increment
            regs.eip = 0x00412edb;
        }
        // Non-mesh: trampoline runs MOV EBX,[EAX] + CMP, returns to JZ at 0x00412ea7
    },
};

// Wrapper for the paste function (FUN_00413050, called from Ctrl+V via thunk at 0x00448650).
// After the stock paste function processes clones from CDedLevel+0x2EC, we create
// mesh clones from our clipboard and add them to the selection.
// Uses __fastcall to receive ECX (CDedLevel*) from the thunk's JMP.
static void __fastcall mesh_paste_wrapper(void* ecx_level, void* /*edx_unused*/)
{
    // Call original paste function (handles stock object types from 0x2EC)
    AddrCaller{0x00413050}.this_call(ecx_level);

    // Create mesh clones from clipboard and add to selection
    if (!g_mesh_clipboard.empty()) {
        auto* level = CDedLevel::Get();
        if (level) {
            auto* sel_ptr = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(level) + 0x298);
            for (auto* staged : g_mesh_clipboard) {
                auto* clone = CloneMeshObject(staged, true);
                if (clone) {
                    // Add to selection (same function the stock paste loop uses: FUN_00491020)
                    AddrCaller{0x00491020}.this_call(sel_ptr, static_cast<DedObject*>(clone));
                    xlog::info("[Mesh] Paste: created mesh uid={} from clipboard, selected", clone->uid);
                }
            }
            // Don't clear clipboard — allow multiple pastes from same copy
        }
    }
}

// Flags to detect delete and cut operations in FUN_0041be70.
static bool g_mesh_delete_mode = false;
static bool g_mesh_cut_mode = false;

// Hook FUN_0041bd00 to detect delete/cut mode before FUN_0041bbb0 processes selection items.
// FUN_0041bd00 signature: __thiscall(CDedLevel* this, int param_2)
// param_2=0: cut finalization, param_2=1: delete (keyboard Delete key)
CodeInjection mesh_delete_mode_patch{
    0x0041bd00,
    [](auto& regs) {
        auto esp_val = static_cast<uintptr_t>(regs.esp);
        auto param_2 = *reinterpret_cast<int*>(esp_val + 4);
        auto* level = reinterpret_cast<CDedLevel*>(static_cast<uintptr_t>(regs.ecx));
        auto mode = *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(level) + 0xf8);
        g_mesh_delete_mode = (mode == 4 && param_2 == 1);
        g_mesh_cut_mode = (mode == 4 && param_2 == 0);
    },
};

// Hook FUN_0041be70 to handle mesh objects during cut finalization and delete.
// For cut: remove mesh from mesh_objects (it's already staged in clipboard).
// For delete: remove mesh from mesh_objects.
// Stock code removes from 0x2E0 (no-op for mesh) and selection (0x298).
CodeInjection mesh_paste_finalize_patch{
    0x0041be70,
    [](auto& regs) {
        auto esp_val = static_cast<uintptr_t>(regs.esp);
        auto* obj = reinterpret_cast<DedObject*>(
            *reinterpret_cast<uintptr_t*>(esp_val + 4));
        if (obj && obj->type == DedObjectType::DED_MESH) {
            if (g_mesh_delete_mode || g_mesh_cut_mode) {
                auto* level = CDedLevel::Get();
                if (level) {
                    auto* mesh = static_cast<DedMesh*>(obj);
                    auto& mesh_objects = level->GetAlpineLevelProperties().mesh_objects;
                    auto it = std::find(mesh_objects.begin(), mesh_objects.end(), mesh);
                    if (it != mesh_objects.end()) {
                        mesh_objects.erase(it);
                        xlog::info("[Mesh] {}: removed mesh uid={} from mesh_objects (count={})",
                            g_mesh_cut_mode ? "Cut" : "Delete", mesh->uid, mesh_objects.size());
                    }
                }
            }
            // Let stock FUN_0041be70 continue - for mesh type (0x17 > 0x16), the switch
            // falls through and the post-switch code removes from 0x2E0 (no-op since mesh
            // isn't there) and from selection (0x298). This is harmless and handles cleanup.
        }
    },
};

// Hook the delete command handler (command ID 0x8018) at 0x00448690.
// Before stock code runs, we remove any DedMesh objects from the selection,
// call DeleteMeshObject for each, then let stock code handle remaining objects.
CodeInjection mesh_delete_patch{
    0x00448690,
    [](auto& regs) {
        auto* level = CDedLevel::Get();
        if (!level) return;

        auto& sel = level->selection;
        xlog::info("[Mesh] Delete handler: selection has {} items", sel.size);
        for (int i = 0; i < sel.size; i++) {
            DedObject* obj = sel.data_ptr[i];
            xlog::info("[Mesh]   sel[{}]: ptr={:p} type=0x{:x} uid={}",
                i, static_cast<void*>(obj),
                obj ? static_cast<int>(obj->type) : -1,
                obj ? obj->uid : -1);
        }

        // Iterate backwards to safely remove while iterating
        for (int i = sel.size - 1; i >= 0; i--) {
            DedObject* obj = sel.data_ptr[i];
            if (obj && obj->type == DedObjectType::DED_MESH) {
                remove_from_selection(sel, obj);
                DeleteMeshObject(static_cast<DedMesh*>(obj));
            }
        }
        // Stock code will handle any remaining non-mesh objects in selection
    },
};

// Hook the Object mode tree view (FUN_00443610) to add "Mesh" type entry.
// Inject at 0x004442b7, right after "Trigger Door" is added and before FUN_00444550 (finalize).
// ESI = tree control at this point.
CodeInjection mesh_object_tree_patch{
    0x004442b7,
    [](auto& regs) {
        void* tree_ctrl = reinterpret_cast<void*>(static_cast<uintptr_t>(regs.esi));
        AddrCaller{0x004422b0}.this_call<int>(tree_ctrl, "Mesh", 0xffff0000, 0xffff0002);
    },
};

// Hook the object creation call site in FUN_004431c0. The factory FUN_00442a40 returns
// NULL for unknown types (including our "Mesh"). The caller at 004432f5 does
// MOV EBP,EAX and then dereferences EBP+0x50, crashing on NULL.
// Hook at 004432f5 (right after the CALL returns). If EAX is NULL, the type was
// unrecognized - this only happens for "Mesh" since we control the tree entries.
// Call PlaceNewMeshObject() and skip to cleanup.
CodeInjection mesh_create_object_patch{
    0x004432f5,
    [](auto& regs) {
        if (regs.eax == 0) {
            PlaceNewMeshObject();
            // Skip the stock object setup code (which would deref NULL).
            // Jump to cleanup path at 0x004435be (mark dirty + epilogue).
            regs.eip = 0x004435be;
        }
    },
};

void ApplyMeshPatches()
{
    open_mesh_properties_patch.install();
    mesh_render_patch.install();
    mesh_pick_patch.install();
    mesh_click_pick_patch.install();
    mesh_copy_begin_hook.install();
    mesh_copy_hook.install();
    // Redirect paste thunk's JMP to our wrapper that handles mesh clipboard paste
    AsmWriter(0x00448659).jmp(mesh_paste_wrapper);
    mesh_delete_mode_patch.install();
    mesh_paste_finalize_patch.install();
    mesh_delete_patch.install();
    mesh_tree_patch.install();
    mesh_object_tree_patch.install();
    mesh_create_object_patch.install();

    xlog::info("[Mesh] Mesh object patches applied");
}
