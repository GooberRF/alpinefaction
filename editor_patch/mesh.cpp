#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <xlog/xlog.h>
#include <patch_common/MemUtils.h>
#include "mesh.h"
#include "mfc_types.h"
#include "level.h"
#include "resources.h"
#include "vtypes.h"
#include "alpine_obj.h"
#include <common/utils/string-utils.h>

extern "C" IMAGE_DOS_HEADER __ImageBase;

// ─── Globals ─────────────────────────────────────────────────────────────────

// Forward declarations
static void mesh_apply_texture_overrides(DedMesh* mesh);

static const char* get_meshes_dir()
{
    static char path[MAX_PATH] = {};
    if (!path[0]) {
        DWORD len = GetModuleFileNameA(NULL, path, MAX_PATH);
        if (len == 0 || len >= MAX_PATH) { path[0] = '\0'; return path; }
        char* last_sep = strrchr(path, '\\');
        if (last_sep) *(last_sep + 1) = '\0';
        strcat_s(path, "user_maps\\meshes");
    }
    return path;
}

// DedObject::vmesh is void* (stock struct), this helper provides typed access
static EditorVMesh* get_vmesh(DedMesh* mesh) { return static_cast<EditorVMesh*>(mesh->vmesh); }

// Cache action indices per vmesh to avoid calling character_mesh_load_action during rendering
static std::unordered_map<EditorVMesh*, int> g_v3c_action_cache;
// Track whether simulate_in_editor was active when the action was last set up
static std::unordered_map<EditorVMesh*, bool> g_v3c_action_simulating;
// Track elapsed time for manually looping one-shot actions in simulate mode
static std::unordered_map<EditorVMesh*, float> g_v3c_action_elapsed;

// ─── VMesh Loading ──────────────────────────────────────────────────────────

static bool mesh_play_v3c_action(EditorVMesh* vmesh, const char* action_name,
                                 bool simulate = false)
{
    if (!vmesh || !action_name || action_name[0] == '\0') return false;
    if (vmesh->type != VMESH_TYPE_CHARACTER) return false;
    if (!vmesh->mesh) return false;

    // Verify the animation file exists before loading (character_mesh_load_action
    // returns 0 rather than -1 for missing files, which leads to garbage animation data)
    rf::File file;
    if (!file.open(action_name)) {
        xlog::warn("[Mesh] Animation file '{}' not found, skipping", action_name);
        return false;
    }

    // Always load as one-shot (is_state=0). vmesh_play_action_by_index silently
    // ignores state actions (is_state=1), so we must use one-shot and manually
    // handle looping by restarting the action when it expires.
    int action_index = character_mesh_load_action(vmesh->mesh, action_name, 0, 0);
    if (action_index < 0) {
        xlog::warn("[Mesh] Failed to load animation '{}' on vmesh {:p}", action_name, static_cast<void*>(vmesh));
        return false;
    }

    if (!vmesh->instance) {
        xlog::warn("[Mesh] No instance on vmesh {:p}, cannot play animation", static_cast<void*>(vmesh));
        return false;
    }

    g_v3c_action_cache[vmesh] = action_index;
    g_v3c_action_simulating[vmesh] = simulate;
    g_v3c_action_elapsed[vmesh] = 0.0f;

    vmesh_stop_all_actions(vmesh);
    vmesh_play_action_by_index(vmesh, action_index, 1.0f, 0);
    return true;
}

static void mesh_load_vmesh(DedMesh* mesh)
{
    if (!mesh) return;

    // Free existing vmesh if any
    if (auto* v = get_vmesh(mesh)) {
        g_v3c_action_cache.erase(v);
        g_v3c_action_simulating.erase(v);
        g_v3c_action_elapsed.erase(v);
        vmesh_free(v);
        mesh->vmesh = nullptr;
    }

    const char* filename = mesh->mesh_filename.c_str();
    if (!filename || filename[0] == '\0') return;

    auto ext = get_ext_from_filename(filename);

    EditorVMesh* vmesh = nullptr;
    if (string_iequals(ext, "v3m")) {
        vmesh = vmesh_load_v3m(filename, 1, -1);
    }
    else if (string_iequals(ext, "v3c") || string_iequals(ext, "vfx")) {
        // Both v3c and vfx loaders can trigger fatal errors if the file is missing.
        // Use RED's File class to check if the file exists (searches loose files + .vpp archives).
        rf::File file;
        if (file.open(filename)) {
            if (string_iequals(ext, "v3c")) {
                vmesh = vmesh_load_v3c(filename, 0, 0);
            }
            else {
                vmesh = vmesh_load_vfx(filename, 0x98967f);
            }
        }
        else {
            xlog::warn("[Mesh] File not found: '{}'", filename);
        }
    }

    mesh->vmesh = vmesh;
    mesh->vmesh_load_failed = (vmesh == nullptr);
    if (vmesh) {
        // Clear replacement material state to prevent stale data from memory reuse
        // (e.g. VFX vmesh freed then V3M allocated at same address with leftover flags)
        vmesh->replacement_materials = nullptr;
        vmesh->use_replacement_materials = false;

        auto vtype = vmesh_get_type(vmesh);
        // Initialize VFX animation state (matches stock item setup in FUN_004151c0)
        if (vtype == VMESH_TYPE_ANIM_FX) {
            vmesh_anim_init(vmesh, 0, 1.0f);
            vmesh_process(vmesh, 0.0f, 0, nullptr, nullptr, 1);
        }
        // Play state animation for v3c skeletal meshes
        if (vtype == VMESH_TYPE_CHARACTER) {
            const char* anim = mesh->state_anim.c_str();
            if (anim && anim[0] != '\0') {
                if (mesh_play_v3c_action(vmesh, anim, mesh->simulate_in_editor)) {
                    // Process a small dt to evaluate the initial pose (frame 0)
                    vmesh_process(vmesh, 0.01f, 0, &mesh->pos, &mesh->orient, 1);
                }
            }
        }
        // Apply texture overrides
        mesh_apply_texture_overrides(mesh);
        xlog::debug("[Mesh] Loaded vmesh for '{}' -> {:p}", filename, static_cast<void*>(vmesh));
    }
    else {
        xlog::warn("[Mesh] Failed to load vmesh for '{}'", filename);
    }
}

// Apply per-slot texture overrides to a mesh's vmesh
static void mesh_apply_texture_overrides(DedMesh* mesh)
{
    auto* ev = get_vmesh(mesh);
    if (!mesh || !ev) return;

    if (mesh->texture_overrides.empty()) return;

    int num_materials = 0;
    EditorMeshMaterial* materials = nullptr;
    editor_vmesh_get_materials_array(ev, &num_materials, &materials);
    if (!materials || num_materials <= 0) {
        // editor_vmesh_get_materials_array unconditionally sets use_replacement_materials=1
        // even when allocation fails (e.g. V3M with multiple LODs/sub-meshes).
        // Clear the flag to prevent the render code from dereferencing a null pointer.
        ev->use_replacement_materials = false;
        ev->replacement_materials = nullptr;

        // For V3M meshes with multiple LODs or sub-mesh groups, the engine's replacement
        // materials allocator (FUN_004c0ae0) bails early. However the render code applies
        // replacement materials to ALL LODs from a single set. Work around the limitation
        // by temporarily faking single-LOD/single-submesh counts during allocation.
        // V3M instance offsets: +0x50 = lod_count (int), +0x54 = submesh_list (int**)
        if (ev->type == VMESH_TYPE_STATIC) { // V3M
            auto* instance = reinterpret_cast<uint8_t*>(ev->instance);
            if (instance) {
                int* lod_count_ptr = reinterpret_cast<int*>(instance + 0x50);
                int** submesh_list_ptr = reinterpret_cast<int**>(instance + 0x54);
                int orig_lod_count = *lod_count_ptr;
                int orig_submesh_count = (submesh_list_ptr && *submesh_list_ptr) ? **submesh_list_ptr : 1;

                *lod_count_ptr = 1;
                if (submesh_list_ptr && *submesh_list_ptr)
                    **submesh_list_ptr = 1;

                editor_vmesh_get_materials_array(ev, &num_materials, &materials);

                *lod_count_ptr = orig_lod_count;
                if (submesh_list_ptr && *submesh_list_ptr)
                    **submesh_list_ptr = orig_submesh_count;

                if (!materials || num_materials <= 0) {
                    ev->use_replacement_materials = false;
                    xlog::warn("[Mesh] Failed to allocate replacement materials for multi-LOD V3M");
                    return;
                }
                xlog::debug("[Mesh] Allocated replacement materials for multi-LOD V3M ({} materials)", num_materials);
            }
            else {
                return;
            }
        }
        else {
            return;
        }
    }

    for (const auto& ovr : mesh->texture_overrides) {
        if (ovr.filename.empty()) continue;
        if (ovr.slot >= num_materials) {
            xlog::warn("[Mesh] Texture override slot {} exceeds material count {}", ovr.slot, num_materials);
            continue;
        }

        int bm_handle = bm_load(ovr.filename.c_str(), -1, 1);
        if (bm_handle < 0) {
            xlog::warn("[Mesh] Failed to load texture override '{}' for slot {}", ovr.filename, ovr.slot);
            continue;
        }
        materials[ovr.slot].texture_maps[0].tex_handle = bm_handle;
        xlog::debug("[Mesh] Applied texture override slot {}: '{}' (handle={})", ovr.slot, ovr.filename, bm_handle);
    }
}

// Free all VString members before deleting a DedMesh
void DestroyDedMesh(DedMesh* mesh)
{
    if (!mesh) return;
    // Free vmesh
    if (auto* v = get_vmesh(mesh)) {
        g_v3c_action_cache.erase(v);
        g_v3c_action_simulating.erase(v);
        g_v3c_action_elapsed.erase(v);
        vmesh_free(v);
        mesh->vmesh = nullptr;
    }
    // Free VString members from DedObject base
    mesh->field_4.free();
    mesh->script_name.free();
    mesh->class_name.free();
    // Free VString members from DedMesh
    mesh->mesh_filename.free();
    mesh->state_anim.free();
    // texture_overrides is std::vector, cleaned up automatically by delete
    delete mesh;
}

// ─── RFL Serialization ──────────────────────────────────────────────────────

// Stock RED DoLink (0x00415850) stores links at DedObject+0x7C for all types.
static VArray<int>& get_obj_links(DedObject* obj)
{
    return obj->links;  // +0x7C
}

void mesh_serialize_chunk(CDedLevel& level, rf::File& file)
{
    auto& meshes = level.GetAlpineLevelProperties().mesh_objects;
    if (meshes.empty()) return;

    auto start_pos = level.BeginRflSection(file, alpine_mesh_chunk_id);

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
        // texture overrides: count + (slot_id, filename) pairs
        uint8_t num_overrides = static_cast<uint8_t>(mesh->texture_overrides.size());
        file.write<uint8_t>(num_overrides);
        for (const auto& ovr : mesh->texture_overrides) {
            file.write<uint8_t>(ovr.slot);
            write_rfl_string(file, ovr.filename);
        }

        // material (independent of clutter)
        file.write<int32_t>(mesh->material);

        // clutter properties
        auto& cp = mesh->clutter_props;
        file.write<uint8_t>(cp.is_clutter ? 1 : 0);
        if (cp.is_clutter) {
            file.write<float>(cp.life);
            write_rfl_string(file, cp.debris_filename);
            write_rfl_string(file, cp.explosion_vclip);
            file.write<float>(cp.explosion_radius);
            file.write<float>(cp.debris_velocity);
            for (int di = 0; di < 11; di++) {
                file.write<float>(cp.damage_type_factors[di]);
            }
            write_rfl_string(file, cp.corpse_filename);
            write_rfl_string(file, cp.corpse_state_anim);
            file.write<uint8_t>(cp.corpse_collision);
            file.write<int8_t>(cp.corpse_material);
        }
    }

    level.EndRflSection(file, start_pos);
}

void mesh_deserialize_chunk(CDedLevel& level, rf::File& file, std::size_t chunk_len)
{
    auto& meshes = level.GetAlpineLevelProperties().mesh_objects;
    std::size_t remaining = chunk_len;

    rf::File::ChunkGuard chunk_guard{file, remaining};

    auto read_bytes = [&](void* dst, std::size_t n) -> bool {
        if (remaining < n) return false;
        int got = file.read(dst, n);
        if (got != static_cast<int>(n) || file.error()) {
            if (got > 0) remaining -= got;
            return false;
        }
        remaining -= n;
        return true;
    };

    uint32_t count = 0;
    if (!read_bytes(&count, sizeof(count))) return;
    if (count > 10000) count = 10000;

    for (uint32_t i = 0; i < count; i++) {
        auto* mesh = new DedMesh();
        memset(static_cast<DedObject*>(mesh), 0, sizeof(DedObject));
        mesh->vtbl = reinterpret_cast<void*>(ded_object_vtbl_addr); // base DedObject vtable
        mesh->type = DedObjectType::DED_MESH;
        mesh->collision_mode = 2; // All

        // uid
        if (!read_bytes(&mesh->uid, sizeof(mesh->uid))) { DestroyDedMesh(mesh); return; }
        // pos
        if (!read_bytes(&mesh->pos.x, sizeof(float))) { DestroyDedMesh(mesh); return; }
        if (!read_bytes(&mesh->pos.y, sizeof(float))) { DestroyDedMesh(mesh); return; }
        if (!read_bytes(&mesh->pos.z, sizeof(float))) { DestroyDedMesh(mesh); return; }
        // orient
        if (!read_bytes(&mesh->orient.rvec.x, sizeof(float))) { DestroyDedMesh(mesh); return; }
        if (!read_bytes(&mesh->orient.rvec.y, sizeof(float))) { DestroyDedMesh(mesh); return; }
        if (!read_bytes(&mesh->orient.rvec.z, sizeof(float))) { DestroyDedMesh(mesh); return; }
        if (!read_bytes(&mesh->orient.uvec.x, sizeof(float))) { DestroyDedMesh(mesh); return; }
        if (!read_bytes(&mesh->orient.uvec.y, sizeof(float))) { DestroyDedMesh(mesh); return; }
        if (!read_bytes(&mesh->orient.uvec.z, sizeof(float))) { DestroyDedMesh(mesh); return; }
        if (!read_bytes(&mesh->orient.fvec.x, sizeof(float))) { DestroyDedMesh(mesh); return; }
        if (!read_bytes(&mesh->orient.fvec.y, sizeof(float))) { DestroyDedMesh(mesh); return; }
        if (!read_bytes(&mesh->orient.fvec.z, sizeof(float))) { DestroyDedMesh(mesh); return; }
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
        if (!read_bytes(&collision_mode, sizeof(collision_mode))) { DestroyDedMesh(mesh); return; }
        mesh->collision_mode = (collision_mode <= 2) ? collision_mode : 2;
        // texture overrides: count + (slot_id, filename) pairs
        uint8_t num_overrides = 0;
        if (!read_bytes(&num_overrides, sizeof(num_overrides))) { DestroyDedMesh(mesh); return; }
        for (uint8_t oi = 0; oi < num_overrides; oi++) {
            uint8_t slot_id = 0;
            if (!read_bytes(&slot_id, sizeof(slot_id))) { DestroyDedMesh(mesh); return; }
            std::string tex = read_rfl_string(file, remaining);
            if (!tex.empty()) {
                mesh->texture_overrides.push_back({slot_id, std::move(tex)});
            }
        }

        int32_t mat = 0;

        // clutter properties
        if (remaining >= sizeof(int32_t) && read_bytes(&mat, sizeof(mat))) {
            mesh->material = (mat >= 0 && mat <= 9) ? mat : 0;

            uint8_t is_clutter_flag = 0;
            if (remaining >= 1 && read_bytes(&is_clutter_flag, sizeof(is_clutter_flag))) {
                mesh->clutter_props.is_clutter = (is_clutter_flag != 0);
                if (mesh->clutter_props.is_clutter) {
                    auto& cp = mesh->clutter_props;
                    if (!read_bytes(&cp.life, sizeof(float))) { DestroyDedMesh(mesh); return; }
                    cp.debris_filename = read_rfl_string(file, remaining);
                    cp.explosion_vclip = read_rfl_string(file, remaining);
                    if (!read_bytes(&cp.explosion_radius, sizeof(float))) { DestroyDedMesh(mesh); return; }
                    if (!read_bytes(&cp.debris_velocity, sizeof(float))) { DestroyDedMesh(mesh); return; }
                    for (int di = 0; di < 11; di++) {
                        if (!read_bytes(&cp.damage_type_factors[di], sizeof(float))) { DestroyDedMesh(mesh); return; }
                    }
                    // Corpse fields
                    if (remaining > 0) {
                        cp.corpse_filename = read_rfl_string(file, remaining);
                    }
                    if (remaining > 0) {
                        cp.corpse_state_anim = read_rfl_string(file, remaining);
                    }
                    if (remaining >= 1) {
                        read_bytes(&cp.corpse_collision, sizeof(uint8_t));
                    }
                    if (remaining >= 1) {
                        read_bytes(&cp.corpse_material, sizeof(int8_t));
                    }
                }
            }
        }

        // Don't load vmesh here - the RFL file is still open and loading a v3c
        // mesh during chunk parsing conflicts with the file I/O system.
        // The render hook's lazy-load will handle it on the first frame.
        mesh->vmesh = nullptr;
        mesh->vmesh_load_failed = false;

        meshes.push_back(mesh);
        // Add to master objects list so stock link validation (FUN_00483920) finds this mesh
        level.master_objects.add(static_cast<DedObject*>(mesh));
    }
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
static std::vector<EditorTextureOverride> g_init_overrides;
static bool g_init_overrides_multiple = false; // true if selected meshes have differing overrides
static int g_init_simulate = 0; // 0=unchecked, 1=checked, -1=indeterminate (mixed)
static int g_init_material = 0;
static bool g_init_material_multiple = false;
static int g_init_is_clutter = 0; // 0=unchecked, 1=checked, -1=indeterminate
static MeshClutterProps g_init_clutter_props;
static bool g_init_clutter_multiple = false; // true if selected meshes have differing clutter props

// Flags: set to true by IDOK if mesh filenames/anims were changed, so caller can reload
static bool g_mesh_filename_changed = false;
static bool g_mesh_anim_changed = false;

// Helper: populate the override ListView from a vector
static void mesh_dialog_populate_overrides(HWND hdlg, const std::vector<EditorTextureOverride>& overrides)
{
    HWND list = GetDlgItem(hdlg, IDC_MESH_OVERRIDE_LIST);
    ListView_DeleteAllItems(list);
    for (int i = 0; i < static_cast<int>(overrides.size()); i++) {
        char slot_str[8];
        snprintf(slot_str, sizeof(slot_str), "%d", overrides[i].slot);
        LVITEMA lvi = {};
        lvi.mask = LVIF_TEXT;
        lvi.iItem = i;
        lvi.iSubItem = 0;
        lvi.pszText = slot_str;
        ListView_InsertItem(list, &lvi);
        ListView_SetItemText(list, i, 1, const_cast<char*>(overrides[i].filename.c_str()));
    }
}

// Helper: read overrides from the ListView into a vector
static std::vector<EditorTextureOverride> mesh_dialog_read_overrides(HWND hdlg)
{
    std::vector<EditorTextureOverride> result;
    HWND list = GetDlgItem(hdlg, IDC_MESH_OVERRIDE_LIST);
    int count = ListView_GetItemCount(list);
    for (int i = 0; i < count; i++) {
        char slot_str[8] = {};
        char tex_str[MAX_PATH] = {};
        ListView_GetItemText(list, i, 0, slot_str, sizeof(slot_str));
        ListView_GetItemText(list, i, 1, tex_str, sizeof(tex_str));
        if (tex_str[0] != '\0') {
            int slot = std::clamp(atoi(slot_str), 0, 255);
            result.push_back({static_cast<uint8_t>(slot), tex_str});
        }
    }
    return result;
}

// Auto-correct legacy file extensions in an edit control.
static void mesh_dialog_fix_extension(HWND hdlg, int ctrl_id,
    const char* old_ext, const char* new_ext)
{
    char buf[MAX_PATH] = {};
    GetDlgItemTextA(hdlg, ctrl_id, buf, sizeof(buf));
    auto ext = get_ext_from_filename(buf);
    if (string_iequals(ext, old_ext)) {
        char* dot = strrchr(buf, '.');
        if (dot) {
            strcpy(dot + 1, new_ext);
            SetDlgItemTextA(hdlg, ctrl_id, buf);
            // Place caret at end so typing isn't disrupted
            SendDlgItemMessage(hdlg, ctrl_id, EM_SETSEL, strlen(buf), strlen(buf));
        }
    }
}

// Update dialog controls based on mesh type and material info
static void mesh_dialog_update_state(HWND hdlg)
{
    char fname_buf[MAX_PATH] = {};
    GetDlgItemTextA(hdlg, IDC_MESH_FILENAME, fname_buf, sizeof(fname_buf));
    bool has_filename = (fname_buf[0] != '\0');

    auto ext = get_ext_from_filename(fname_buf);

    // Set mesh type label
    const char* type_label = "";
    if (has_filename) {
        if (string_iequals(ext, "v3m"))
            type_label = "Specified mesh is static (.v3m)";
        else if (string_iequals(ext, "v3c"))
            type_label = "Specified mesh is skeletal (.v3c)";
        else if (string_iequals(ext, "vfx"))
            type_label = "Specified mesh is animated (.vfx)";
        else
            type_label = "Unknown mesh type";
    }
    SetDlgItemTextA(hdlg, IDC_MESH_TYPE_LABEL, type_label);

    // State anim and simulate: only applicable for V3C
    bool enable_state_anim = has_filename && string_iequals(ext, "v3c");
    EnableWindow(GetDlgItem(hdlg, IDC_MESH_STATE_ANIM), enable_state_anim);
    EnableWindow(GetDlgItem(hdlg, IDC_MESH_SIMULATE), enable_state_anim);

    // Collision: not applicable for VFX
    bool enable_collision = has_filename && !string_iequals(ext, "vfx");
    EnableWindow(GetDlgItem(hdlg, IDC_MESH_COLLISION_MODE), enable_collision);

    // Material overrides: enable/disable controls based on filename
    EnableWindow(GetDlgItem(hdlg, IDC_MESH_OVERRIDE_LIST), has_filename);
    EnableWindow(GetDlgItem(hdlg, IDC_MESH_OVERRIDE_SLOT), has_filename);
    EnableWindow(GetDlgItem(hdlg, IDC_MESH_OVERRIDE_FILENAME), has_filename);
    EnableWindow(GetDlgItem(hdlg, IDC_MESH_OVERRIDE_ADD), has_filename);
    EnableWindow(GetDlgItem(hdlg, IDC_MESH_OVERRIDE_REMOVE), has_filename);

    // Clutter properties: enable/disable based on "Is Clutter" checkbox
    bool is_clutter = (IsDlgButtonChecked(hdlg, IDC_MESH_IS_CLUTTER) == BST_CHECKED);
    static const int clutter_controls[] = {
        IDC_MESH_CLUTTER_LIFE,
        IDC_MESH_CLUTTER_DEBRIS, IDC_MESH_CLUTTER_DEBRIS_BROWSE,
        IDC_MESH_CLUTTER_VCLIP, IDC_MESH_CLUTTER_EXPLODE_RADIUS,
        IDC_MESH_CLUTTER_DEBRIS_VEL,
        IDC_MESH_CLUTTER_CORPSE, IDC_MESH_CLUTTER_CORPSE_BROWSE,
        IDC_MESH_CLUTTER_CORPSE_STATE_ANIM, IDC_MESH_CLUTTER_CORPSE_COLLISION,
        IDC_MESH_CLUTTER_CORPSE_MATERIAL,
        IDC_MESH_CLUTTER_DMG_BASH, IDC_MESH_CLUTTER_DMG_BULLET,
        IDC_MESH_CLUTTER_DMG_AP_BULLET, IDC_MESH_CLUTTER_DMG_EXPLOSIVE,
        IDC_MESH_CLUTTER_DMG_FIRE, IDC_MESH_CLUTTER_DMG_ENERGY,
        IDC_MESH_CLUTTER_DMG_ELECTRICAL, IDC_MESH_CLUTTER_DMG_ACID,
        IDC_MESH_CLUTTER_DMG_SCALDING, IDC_MESH_CLUTTER_DMG_CRUSH,
    };
    for (int id : clutter_controls) {
        EnableWindow(GetDlgItem(hdlg, id), is_clutter);
    }

    // Corpse mesh type label and conditional enable/disable of corpse state anim + collision
    if (is_clutter) {
        char corpse_buf[MAX_PATH] = {};
        GetDlgItemTextA(hdlg, IDC_MESH_CLUTTER_CORPSE, corpse_buf, sizeof(corpse_buf));
        bool has_corpse = (corpse_buf[0] != '\0');
        auto corpse_ext = get_ext_from_filename(corpse_buf);

        const char* corpse_type = "";
        if (has_corpse) {
            if (string_iequals(corpse_ext, "v3m"))
                corpse_type = "Corpse mesh is static (.v3m)";
            else if (string_iequals(corpse_ext, "v3c"))
                corpse_type = "Corpse mesh is skeletal (.v3c)";
            else if (string_iequals(corpse_ext, "vfx"))
                corpse_type = "Corpse mesh is animated (.vfx)";
            else
                corpse_type = "Unknown corpse mesh type";
        }
        SetDlgItemTextA(hdlg, IDC_MESH_CLUTTER_CORPSE_TYPE_LABEL, corpse_type);

        // State anim: only for v3c corpse meshes
        bool corpse_is_v3c = has_corpse && string_iequals(corpse_ext, "v3c");
        EnableWindow(GetDlgItem(hdlg, IDC_MESH_CLUTTER_CORPSE_STATE_ANIM), corpse_is_v3c);

        // Collision: not for vfx corpse meshes
        bool corpse_has_collision = has_corpse && !string_iequals(corpse_ext, "vfx");
        EnableWindow(GetDlgItem(hdlg, IDC_MESH_CLUTTER_CORPSE_COLLISION), corpse_has_collision);

        // Material: enabled whenever a corpse mesh is specified
        EnableWindow(GetDlgItem(hdlg, IDC_MESH_CLUTTER_CORPSE_MATERIAL), has_corpse);
    } else {
        SetDlgItemTextA(hdlg, IDC_MESH_CLUTTER_CORPSE_TYPE_LABEL, "");
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
        bool all_same_overrides = true;
        bool all_same_simulate = true;
        bool all_same_material = true;
        bool all_same_is_clutter = true;
        bool all_same_clutter = true;

        for (size_t i = 1; i < g_selected_meshes.size(); i++) {
            auto* m = g_selected_meshes[i];
            if (strcmp(m->script_name.c_str(), first->script_name.c_str()) != 0) all_same_script = false;
            if (strcmp(m->mesh_filename.c_str(), first->mesh_filename.c_str()) != 0) all_same_filename = false;
            if (strcmp(m->state_anim.c_str(), first->state_anim.c_str()) != 0) all_same_anim = false;
            if (m->collision_mode != first->collision_mode) all_same_collision = false;
            if (m->simulate_in_editor != first->simulate_in_editor) all_same_simulate = false;
            if (m->material != first->material) all_same_material = false;
            if (m->clutter_props.is_clutter != first->clutter_props.is_clutter) all_same_is_clutter = false;
            if (m->texture_overrides.size() != first->texture_overrides.size()) {
                all_same_overrides = false;
            } else {
                for (size_t ti = 0; ti < first->texture_overrides.size(); ti++) {
                    if (m->texture_overrides[ti].slot != first->texture_overrides[ti].slot ||
                        m->texture_overrides[ti].filename != first->texture_overrides[ti].filename) {
                        all_same_overrides = false;
                        break;
                    }
                }
            }
            // Check if clutter props differ
            if (all_same_clutter) {
                auto& a = first->clutter_props;
                auto& b = m->clutter_props;
                if (a.life != b.life ||
                    a.debris_filename != b.debris_filename || a.explosion_vclip != b.explosion_vclip ||
                    a.explosion_radius != b.explosion_radius || a.debris_velocity != b.debris_velocity ||
                    a.corpse_filename != b.corpse_filename ||
                    a.corpse_state_anim != b.corpse_state_anim ||
                    a.corpse_collision != b.corpse_collision ||
                    a.corpse_material != b.corpse_material) {
                    all_same_clutter = false;
                } else {
                    for (int di = 0; di < 11; di++) {
                        if (a.damage_type_factors[di] != b.damage_type_factors[di]) {
                            all_same_clutter = false;
                            break;
                        }
                    }
                }
            }
        }

        g_init_script_name = all_same_script ? first->script_name.c_str() : MULTIPLE_STR;
        g_init_filename = all_same_filename ? first->mesh_filename.c_str() : MULTIPLE_STR;
        g_init_state_anim = all_same_anim ? first->state_anim.c_str() : MULTIPLE_STR;
        g_init_collision_mode = all_same_collision ? first->collision_mode : MULTIPLE_COLLISION;
        g_init_overrides_multiple = !all_same_overrides;
        g_init_overrides = all_same_overrides ? first->texture_overrides : std::vector<EditorTextureOverride>{};
        g_init_simulate = all_same_simulate ? (first->simulate_in_editor ? 1 : 0) : -1;
        g_init_material = all_same_material ? first->material : -1;
        g_init_material_multiple = !all_same_material;
        g_init_is_clutter = all_same_is_clutter ? (first->clutter_props.is_clutter ? 1 : 0) : -1;
        g_init_clutter_multiple = !all_same_clutter;
        g_init_clutter_props = all_same_clutter ? first->clutter_props : MeshClutterProps{};

        SetDlgItemTextA(hdlg, IDC_MESH_SCRIPT_NAME, g_init_script_name.c_str());
        SetDlgItemTextA(hdlg, IDC_MESH_FILENAME, g_init_filename.c_str());
        SetDlgItemTextA(hdlg, IDC_MESH_STATE_ANIM, g_init_state_anim.c_str());
        if (g_init_simulate < 0) {
            SendDlgItemMessage(hdlg, IDC_MESH_SIMULATE, BM_SETCHECK, BST_INDETERMINATE, 0);
        } else {
            CheckDlgButton(hdlg, IDC_MESH_SIMULATE, g_init_simulate ? BST_CHECKED : BST_UNCHECKED);
        }

        // Material combo box (independent of clutter)
        {
            HWND mat_combo = GetDlgItem(hdlg, IDC_MESH_MATERIAL);
            static const char* material_names[] = {
                "Default", "Rock", "Metal", "Flesh", "Water",
                "Lava", "Solid", "Glass", "Sand", "Ice"
            };
            for (auto* name : material_names) {
                SendMessageA(mat_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
            }
            if (g_init_material_multiple) {
                SendMessageA(mat_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Undefined"));
                SendMessageA(mat_combo, CB_SETCURSEL, 10, 0); // "Undefined"
            } else if (g_init_material >= 0 && g_init_material < 10) {
                SendMessageA(mat_combo, CB_SETCURSEL, g_init_material, 0);
            } else {
                SendMessageA(mat_combo, CB_SETCURSEL, 0, 0);
            }
        }

        // Is Clutter checkbox
        if (g_init_is_clutter < 0) {
            SendDlgItemMessage(hdlg, IDC_MESH_IS_CLUTTER, BM_SETCHECK, BST_INDETERMINATE, 0);
        } else {
            CheckDlgButton(hdlg, IDC_MESH_IS_CLUTTER, g_init_is_clutter ? BST_CHECKED : BST_UNCHECKED);
        }

        // Clutter properties
        {
            auto& cp = g_init_clutter_props;
            char buf2[64];
            snprintf(buf2, sizeof(buf2), "%.1f", cp.life);
            SetDlgItemTextA(hdlg, IDC_MESH_CLUTTER_LIFE, buf2);

            SetDlgItemTextA(hdlg, IDC_MESH_CLUTTER_DEBRIS, cp.debris_filename.c_str());
            SetDlgItemTextA(hdlg, IDC_MESH_CLUTTER_VCLIP, cp.explosion_vclip.c_str());

            snprintf(buf2, sizeof(buf2), "%.1f", cp.explosion_radius);
            SetDlgItemTextA(hdlg, IDC_MESH_CLUTTER_EXPLODE_RADIUS, buf2);

            snprintf(buf2, sizeof(buf2), "%.1f", cp.debris_velocity);
            SetDlgItemTextA(hdlg, IDC_MESH_CLUTTER_DEBRIS_VEL, buf2);

            SetDlgItemTextA(hdlg, IDC_MESH_CLUTTER_CORPSE, cp.corpse_filename.c_str());
            SetDlgItemTextA(hdlg, IDC_MESH_CLUTTER_CORPSE_STATE_ANIM, cp.corpse_state_anim.c_str());

            // Corpse collision combo
            {
                HWND corpse_col = GetDlgItem(hdlg, IDC_MESH_CLUTTER_CORPSE_COLLISION);
                SendMessageA(corpse_col, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("None"));
                SendMessageA(corpse_col, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Only Weapons"));
                SendMessageA(corpse_col, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("All"));
                int sel = (cp.corpse_collision <= 2) ? cp.corpse_collision : 0;
                SendMessageA(corpse_col, CB_SETCURSEL, sel, 0);
            }

            // Corpse material combo — index 0 = Automatic, 1-10 = specific materials
            {
                HWND corpse_mat = GetDlgItem(hdlg, IDC_MESH_CLUTTER_CORPSE_MATERIAL);
                static const char* corpse_material_names[] = {
                    "Automatic", "Default", "Rock", "Metal", "Flesh", "Water",
                    "Lava", "Solid", "Glass", "Sand", "Ice"
                };
                for (auto* name : corpse_material_names) {
                    SendMessageA(corpse_mat, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
                }
                // -1 = Automatic (index 0), 0-9 = specific material (index 1-10)
                int mat_sel = (cp.corpse_material >= 0 && cp.corpse_material <= 9)
                    ? (cp.corpse_material + 1) : 0;
                SendMessageA(corpse_mat, CB_SETCURSEL, mat_sel, 0);
            }

            // Damage type factor edit fields
            static const int dmg_ids[] = {
                IDC_MESH_CLUTTER_DMG_BASH, IDC_MESH_CLUTTER_DMG_BULLET,
                IDC_MESH_CLUTTER_DMG_AP_BULLET, IDC_MESH_CLUTTER_DMG_EXPLOSIVE,
                IDC_MESH_CLUTTER_DMG_FIRE, IDC_MESH_CLUTTER_DMG_ENERGY,
                IDC_MESH_CLUTTER_DMG_ELECTRICAL, IDC_MESH_CLUTTER_DMG_ACID,
                IDC_MESH_CLUTTER_DMG_SCALDING, IDC_MESH_CLUTTER_DMG_CRUSH,
            };
            for (int di = 0; di < 10; di++) {
                snprintf(buf2, sizeof(buf2), "%.1f", cp.damage_type_factors[di]);
                SetDlgItemTextA(hdlg, dmg_ids[di], buf2);
            }
        }

        // Set up the material overrides ListView columns
        {
            HWND list = GetDlgItem(hdlg, IDC_MESH_OVERRIDE_LIST);
            ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
            LVCOLUMNA col = {};
            col.mask = LVCF_TEXT | LVCF_WIDTH;
            col.cx = 36;
            col.pszText = const_cast<char*>("Slot");
            ListView_InsertColumn(list, 0, &col);
            col.cx = 200;
            col.pszText = const_cast<char*>("Texture");
            ListView_InsertColumn(list, 1, &col);
            mesh_dialog_populate_overrides(hdlg, g_init_overrides);
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

        // Mesh objects are link targets only (from events), not link sources.
        // Hide the Links button entirely.
        ShowWindow(GetDlgItem(hdlg, ID_LINKS), SW_HIDE);

        mesh_dialog_update_state(hdlg);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IDC_MESH_FILENAME:
            if (HIWORD(wparam) == EN_CHANGE) {
                // Auto-correct legacy extensions
                mesh_dialog_fix_extension(hdlg, IDC_MESH_FILENAME, "v3d", "v3m");
                mesh_dialog_fix_extension(hdlg, IDC_MESH_FILENAME, "vcm", "v3c");
                mesh_dialog_update_state(hdlg);
            }
            return TRUE;

        case IDC_MESH_STATE_ANIM:
            if (HIWORD(wparam) == EN_CHANGE) {
                // Auto-correct legacy extension
                mesh_dialog_fix_extension(hdlg, IDC_MESH_STATE_ANIM, "mvf", "rfa");
            }
            return TRUE;

        case IDC_MESH_BROWSE:
        {
            char filename[MAX_PATH] = {};
            OPENFILENAMEA ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hdlg;
            ofn.lpstrFilter = "Mesh Files (*.v3m;*.v3c;*.vfx)\0*.v3m;*.v3c;*.vfx\0All Files (*.*)\0*.*\0";
            ofn.lpstrInitialDir = get_meshes_dir();
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

        case IDC_MESH_CLUTTER_CORPSE:
            if (HIWORD(wparam) == EN_CHANGE) {
                mesh_dialog_fix_extension(hdlg, IDC_MESH_CLUTTER_CORPSE, "v3d", "v3m");
                mesh_dialog_fix_extension(hdlg, IDC_MESH_CLUTTER_CORPSE, "vcm", "v3c");
                mesh_dialog_update_state(hdlg);
            }
            return TRUE;

        case IDC_MESH_CLUTTER_CORPSE_STATE_ANIM:
            if (HIWORD(wparam) == EN_CHANGE) {
                mesh_dialog_fix_extension(hdlg, IDC_MESH_CLUTTER_CORPSE_STATE_ANIM, "mvf", "rfa");
            }
            return TRUE;

        case IDC_MESH_CLUTTER_DEBRIS:
            if (HIWORD(wparam) == EN_CHANGE) {
                mesh_dialog_fix_extension(hdlg, IDC_MESH_CLUTTER_DEBRIS, "v3d", "v3m");
            }
            return TRUE;

        case IDC_MESH_IS_CLUTTER:
            mesh_dialog_update_state(hdlg);
            return TRUE;

        case IDC_MESH_CLUTTER_DEBRIS_BROWSE:
        {
            char filename[MAX_PATH] = {};
            OPENFILENAMEA ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hdlg;
            ofn.lpstrFilter = "Static Mesh (*.v3m)\0*.v3m\0All Files (*.*)\0*.*\0";
            ofn.lpstrInitialDir = get_meshes_dir();
            ofn.lpstrFile = filename;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameA(&ofn)) {
                const char* base = strrchr(filename, '\\');
                if (!base) base = strrchr(filename, '/');
                if (base) base++; else base = filename;
                SetDlgItemTextA(hdlg, IDC_MESH_CLUTTER_DEBRIS, base);
            }
            return TRUE;
        }

        case IDC_MESH_CLUTTER_CORPSE_BROWSE:
        {
            char filename[MAX_PATH] = {};
            OPENFILENAMEA ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hdlg;
            ofn.lpstrFilter = "Mesh Files (*.v3m;*.v3c;*.vfx)\0*.v3m;*.v3c;*.vfx\0All Files (*.*)\0*.*\0";
            ofn.lpstrInitialDir = get_meshes_dir();
            ofn.lpstrFile = filename;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameA(&ofn)) {
                const char* base = strrchr(filename, '\\');
                if (!base) base = strrchr(filename, '/');
                if (base) base++; else base = filename;
                SetDlgItemTextA(hdlg, IDC_MESH_CLUTTER_CORPSE, base);
                mesh_dialog_update_state(hdlg);
            }
            return TRUE;
        }

        case IDC_MESH_OVERRIDE_ADD:
        {
            char slot_str[8] = {};
            char tex_str[MAX_PATH] = {};
            GetDlgItemTextA(hdlg, IDC_MESH_OVERRIDE_SLOT, slot_str, sizeof(slot_str));
            GetDlgItemTextA(hdlg, IDC_MESH_OVERRIDE_FILENAME, tex_str, sizeof(tex_str));
            if (tex_str[0] == '\0') return TRUE;

            HWND list = GetDlgItem(hdlg, IDC_MESH_OVERRIDE_LIST);
            int slot_val = atoi(slot_str);

            // Check if this slot already exists and replace it
            int count = ListView_GetItemCount(list);
            int existing_idx = -1;
            for (int i = 0; i < count; i++) {
                char existing_slot[8] = {};
                ListView_GetItemText(list, i, 0, existing_slot, sizeof(existing_slot));
                if (atoi(existing_slot) == slot_val) {
                    existing_idx = i;
                    break;
                }
            }

            if (existing_idx >= 0) {
                // Update existing entry
                ListView_SetItemText(list, existing_idx, 1, tex_str);
            } else {
                // Add new entry
                LVITEMA lvi = {};
                lvi.mask = LVIF_TEXT;
                lvi.iItem = count;
                lvi.iSubItem = 0;
                lvi.pszText = slot_str;
                ListView_InsertItem(list, &lvi);
                ListView_SetItemText(list, count, 1, tex_str);
            }

            // Clear input fields
            SetDlgItemTextA(hdlg, IDC_MESH_OVERRIDE_SLOT, "");
            SetDlgItemTextA(hdlg, IDC_MESH_OVERRIDE_FILENAME, "");
            return TRUE;
        }

        case IDC_MESH_OVERRIDE_REMOVE:
        {
            HWND list = GetDlgItem(hdlg, IDC_MESH_OVERRIDE_LIST);
            int sel = ListView_GetNextItem(list, -1, LVNI_SELECTED);
            if (sel >= 0) {
                ListView_DeleteItem(list, sel);
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
            bool state_anim_enabled = IsWindowEnabled(GetDlgItem(hdlg, IDC_MESH_STATE_ANIM));
            // Only force-clear disabled fields if the user changed the filename
            // (all meshes now share a type). If filename is "<multiple>" and unchanged,
            // some meshes may legitimately use these values for their own type.
            bool force_clear_disabled = !state_anim_enabled && filename_changed;
            if (state_anim_enabled) {
                GetDlgItemTextA(hdlg, IDC_MESH_STATE_ANIM, anim_buf, sizeof(anim_buf));
            }
            bool anim_changed = state_anim_enabled
                ? (strcmp(anim_buf, g_init_state_anim.c_str()) != 0)
                : force_clear_disabled;

            HWND combo = GetDlgItem(hdlg, IDC_MESH_COLLISION_MODE);
            bool collision_enabled = IsWindowEnabled(combo);
            bool force_clear_collision = !collision_enabled && filename_changed;
            int collision_sel = collision_enabled
                ? static_cast<int>(SendMessageA(combo, CB_GETCURSEL, 0, 0))
                : 2; // default: All
            // "Undefined" is index 3 — only present in multi-select mode
            bool collision_changed = false;
            if (force_clear_collision) {
                collision_changed = true;
            } else if (!collision_enabled) {
                // Disabled but filename unchanged — don't touch
                collision_changed = false;
            } else if (g_init_collision_mode == MULTIPLE_COLLISION) {
                collision_changed = (collision_sel != 3); // changed from "Undefined"
            } else {
                collision_changed = (collision_sel != g_init_collision_mode);
            }

            // Check material override changes
            auto current_overrides = mesh_dialog_read_overrides(hdlg);
            bool overrides_changed = false;
            if (g_init_overrides_multiple) {
                // Overrides differed across selection — any non-empty list is a change
                overrides_changed = !current_overrides.empty();
            } else {
                // Compare with initial overrides
                if (current_overrides.size() != g_init_overrides.size()) {
                    overrides_changed = true;
                } else {
                    for (size_t i = 0; i < current_overrides.size(); i++) {
                        if (current_overrides[i].slot != g_init_overrides[i].slot ||
                            current_overrides[i].filename != g_init_overrides[i].filename) {
                            overrides_changed = true;
                            break;
                        }
                    }
                }
            }

            // Check simulate checkbox (only apply if not indeterminate)
            int simulate_check = static_cast<int>(IsDlgButtonChecked(hdlg, IDC_MESH_SIMULATE));
            bool simulate_changed = (simulate_check != BST_INDETERMINATE) &&
                ((g_init_simulate < 0) || (simulate_check != g_init_simulate));

            // Check material combo
            int material_sel = static_cast<int>(SendDlgItemMessage(hdlg, IDC_MESH_MATERIAL, CB_GETCURSEL, 0, 0));
            bool material_changed = false;
            if (g_init_material_multiple) {
                material_changed = (material_sel != 10); // changed from "Undefined"
            } else {
                material_changed = (material_sel != g_init_material);
            }

            // Check is_clutter checkbox
            int is_clutter_check = static_cast<int>(IsDlgButtonChecked(hdlg, IDC_MESH_IS_CLUTTER));
            bool is_clutter_changed = (is_clutter_check != BST_INDETERMINATE) &&
                ((g_init_is_clutter < 0) || (is_clutter_check != g_init_is_clutter));

            // Read clutter properties from dialog
            MeshClutterProps new_clutter;
            bool clutter_props_changed = false;
            if (is_clutter_check == BST_CHECKED) {
                new_clutter.is_clutter = true;
                char tmp[MAX_PATH];

                GetDlgItemTextA(hdlg, IDC_MESH_CLUTTER_LIFE, tmp, sizeof(tmp));
                new_clutter.life = static_cast<float>(atof(tmp));

                GetDlgItemTextA(hdlg, IDC_MESH_CLUTTER_DEBRIS, tmp, sizeof(tmp));
                new_clutter.debris_filename = tmp;

                GetDlgItemTextA(hdlg, IDC_MESH_CLUTTER_VCLIP, tmp, sizeof(tmp));
                new_clutter.explosion_vclip = tmp;

                GetDlgItemTextA(hdlg, IDC_MESH_CLUTTER_EXPLODE_RADIUS, tmp, sizeof(tmp));
                new_clutter.explosion_radius = static_cast<float>(atof(tmp));

                GetDlgItemTextA(hdlg, IDC_MESH_CLUTTER_DEBRIS_VEL, tmp, sizeof(tmp));
                new_clutter.debris_velocity = static_cast<float>(atof(tmp));

                GetDlgItemTextA(hdlg, IDC_MESH_CLUTTER_CORPSE, tmp, sizeof(tmp));
                new_clutter.corpse_filename = tmp;

                if (IsWindowEnabled(GetDlgItem(hdlg, IDC_MESH_CLUTTER_CORPSE_STATE_ANIM))) {
                    GetDlgItemTextA(hdlg, IDC_MESH_CLUTTER_CORPSE_STATE_ANIM, tmp, sizeof(tmp));
                    new_clutter.corpse_state_anim = tmp;
                }

                {
                    HWND corpse_col = GetDlgItem(hdlg, IDC_MESH_CLUTTER_CORPSE_COLLISION);
                    int sel = static_cast<int>(SendMessageA(corpse_col, CB_GETCURSEL, 0, 0));
                    new_clutter.corpse_collision = (sel >= 0 && sel <= 2) ? static_cast<uint8_t>(sel) : 0;
                }

                {
                    HWND corpse_mat = GetDlgItem(hdlg, IDC_MESH_CLUTTER_CORPSE_MATERIAL);
                    int sel = static_cast<int>(SendMessageA(corpse_mat, CB_GETCURSEL, 0, 0));
                    // Index 0 = Automatic (-1), Index 1-10 = material 0-9
                    new_clutter.corpse_material = (sel >= 1 && sel <= 10)
                        ? static_cast<int8_t>(sel - 1) : static_cast<int8_t>(-1);
                }

                static const int dmg_ids[] = {
                    IDC_MESH_CLUTTER_DMG_BASH, IDC_MESH_CLUTTER_DMG_BULLET,
                    IDC_MESH_CLUTTER_DMG_AP_BULLET, IDC_MESH_CLUTTER_DMG_EXPLOSIVE,
                    IDC_MESH_CLUTTER_DMG_FIRE, IDC_MESH_CLUTTER_DMG_ENERGY,
                    IDC_MESH_CLUTTER_DMG_ELECTRICAL, IDC_MESH_CLUTTER_DMG_ACID,
                    IDC_MESH_CLUTTER_DMG_SCALDING, IDC_MESH_CLUTTER_DMG_CRUSH,
                };
                for (int di = 0; di < 10; di++) {
                    GetDlgItemTextA(hdlg, dmg_ids[di], tmp, sizeof(tmp));
                    new_clutter.damage_type_factors[di] = static_cast<float>(atof(tmp));
                }

                clutter_props_changed = true; // always apply when checked
            } else if (is_clutter_changed && is_clutter_check == BST_UNCHECKED) {
                new_clutter.is_clutter = false;
                clutter_props_changed = true;
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
                if (overrides_changed) {
                    mesh->texture_overrides = current_overrides;
                }
                if (simulate_changed) {
                    mesh->simulate_in_editor = (simulate_check == BST_CHECKED);
                }
                if (material_changed && material_sel >= 0 && material_sel <= 9) {
                    mesh->material = material_sel;
                }
                if (clutter_props_changed) {
                    mesh->clutter_props = new_clutter;
                }
            }
            // Defer vmesh reload until after dialog closes to avoid message pump issues
            g_mesh_filename_changed = filename_changed || overrides_changed;
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
        if (auto* v = get_vmesh(mesh)) {
            g_v3c_action_cache.erase(v);
            g_v3c_action_simulating.erase(v);
            g_v3c_action_elapsed.erase(v);
            vmesh_free(v);
            mesh->vmesh = nullptr;
        }
        mesh->vmesh_load_failed = false;
    }
    // If only anim changed (not filename), fully reload vmesh to cleanly switch.
    // Freeing + clearing load flag lets the render loop lazy-load with the new anim.
    else if (g_mesh_anim_changed && mesh) {
        if (auto* v = get_vmesh(mesh)) {
            g_v3c_action_cache.erase(v);
            g_v3c_action_simulating.erase(v);
            g_v3c_action_elapsed.erase(v);
            vmesh_free(v);
            mesh->vmesh = nullptr;
        }
        mesh->vmesh_load_failed = false;
    }

    g_selected_meshes.clear();
    g_current_level = nullptr;
}

// ─── UID Generation ─────────────────────────────────────────────────────────

void mesh_ensure_uid(int& uid)
{
    auto* level = CDedLevel::Get();
    if (!level) return;
    for (auto* m : level->GetAlpineLevelProperties().mesh_objects) {
        if (m->uid >= uid) uid = m->uid + 1;
    }
}

// ─── Object Lifecycle ───────────────────────────────────────────────────────

void PlaceNewMeshObject()
{
    auto* level = CDedLevel::Get();
    if (!level) return;

    auto* mesh = new DedMesh();
    memset(static_cast<DedObject*>(mesh), 0, sizeof(DedObject));
    mesh->vtbl = reinterpret_cast<void*>(ded_object_vtbl_addr); // base DedObject vtable
    mesh->type = DedObjectType::DED_MESH;
    mesh->collision_mode = 2; // All

    // Default values
    mesh->script_name.assign_0("Mesh");
    mesh->mesh_filename.assign_0("barrel.v3m");

    // Get camera position and orientation from the active viewport
    auto* viewport = get_active_viewport();
    if (viewport && viewport->view_data) {
        mesh->pos = viewport->view_data->camera_pos;
        mesh->orient = viewport->view_data->camera_orient;
    }

    // Fallback if no viewport data
    if (mesh->pos.x == 0.0f && mesh->pos.y == 0.0f && mesh->pos.z == 0.0f) {
        mesh->orient.rvec = {1.0f, 0.0f, 0.0f};
        mesh->orient.uvec = {0.0f, 1.0f, 0.0f};
        mesh->orient.fvec = {0.0f, 0.0f, 1.0f};
    }

    mesh->uid = generate_uid();

    // Add to level (vmesh will be loaded lazily on first render)
    level->GetAlpineLevelProperties().mesh_objects.push_back(mesh);
    // Add to master objects list so stock link validation (FUN_00483920) finds this mesh
    level->master_objects.add(static_cast<DedObject*>(mesh));

    // Deselect all, then select the new mesh (matches stock FUN_004146b0 flow)
    level->clear_selection();
    level->add_to_selection(static_cast<DedObject*>(mesh));

    // Update console display to show selected object info
    level->update_console_display();
}

DedMesh* CloneMeshObject(DedMesh* source, bool add_to_level)
{
    if (!source) return nullptr;

    auto* mesh = new DedMesh();
    memset(static_cast<DedObject*>(mesh), 0, sizeof(DedObject));
    mesh->vtbl = reinterpret_cast<void*>(ded_object_vtbl_addr); // base DedObject vtable
    mesh->type = DedObjectType::DED_MESH;

    // Copy position and orientation
    mesh->pos = source->pos;
    mesh->orient = source->orient;

    // Copy properties
    mesh->script_name.assign_0(source->script_name.c_str());
    mesh->mesh_filename.assign_0(source->mesh_filename.c_str());
    mesh->state_anim.assign_0(source->state_anim.c_str());
    mesh->collision_mode = source->collision_mode;
    mesh->texture_overrides = source->texture_overrides;
    mesh->simulate_in_editor = source->simulate_in_editor;
    mesh->material = source->material;
    mesh->clutter_props = source->clutter_props;

    // Generate new UID
    mesh->uid = generate_uid();

    // Load vmesh for rendering
    mesh_load_vmesh(mesh);

    // Add to level if requested (false during copy/staging, true during direct placement)
    if (add_to_level) {
        auto* level = CDedLevel::Get();
        if (level) {
            level->GetAlpineLevelProperties().mesh_objects.push_back(mesh);
            level->master_objects.add(static_cast<DedObject*>(mesh));
        }
    }
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
    }
    // Remove from master objects list
    level->master_objects.remove_by_value(static_cast<DedObject*>(mesh));
    DestroyDedMesh(mesh);
}

// ─── Editor Hooks ───────────────────────────────────────────────────────────

void ShowMeshPropertiesForSelection(CDedLevel* level)
{
    auto& sel = level->selection;
    if (sel.get_size() < 1) return;

    // Collect all selected mesh objects
    g_selected_meshes.clear();
    g_current_level = level;
    for (int i = 0; i < sel.get_size(); i++) {
        DedObject* obj = sel[i];
        if (obj && obj->type == DedObjectType::DED_MESH) {
            g_selected_meshes.push_back(static_cast<DedMesh*>(obj));
        }
    }

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
        // hook's lazy-load path reloads them on the next frame.
        if (g_mesh_filename_changed) {
            for (auto* m : g_selected_meshes) {
                if (!m) continue;
                if (auto* v = get_vmesh(m)) {
                    g_v3c_action_cache.erase(v);
                    g_v3c_action_simulating.erase(v);
                    g_v3c_action_elapsed.erase(v);
                    vmesh_free(v);
                    m->vmesh = nullptr;
                }
                m->vmesh_load_failed = false;
            }
        }
        // If only anim changed, fully reload vmeshes to cleanly switch
        else if (g_mesh_anim_changed) {
            for (auto* m : g_selected_meshes) {
                if (!m) continue;
                if (auto* v = get_vmesh(m)) {
                    g_v3c_action_cache.erase(v);
                    g_v3c_action_simulating.erase(v);
                    g_v3c_action_elapsed.erase(v);
                    vmesh_free(v);
                    m->vmesh = nullptr;
                }
                m->vmesh_load_failed = false;
            }
        }
    }

    g_selected_meshes.clear();
    g_current_level = nullptr;
}


void mesh_render(CDedLevel* level)
{
    // Compute real frame delta time for animation playback
    static ULONGLONG last_tick = 0;
    ULONGLONG now = GetTickCount64();
    float frame_dt = 0.0f;
    if (last_tick != 0) {
        ULONGLONG delta_ms = now - last_tick;
        if (delta_ms > 200) delta_ms = 200; // clamp to avoid huge jumps
        frame_dt = static_cast<float>(delta_ms) / 1000.0f;
    }
    last_tick = now;

    auto& meshes = level->GetAlpineLevelProperties().mesh_objects;

    // Draw link lines from selected events/triggers to mesh objects
    if (!meshes.empty()) {
        std::unordered_map<int32_t, DedMesh*> mesh_uid_map;
        for (auto* m : meshes) {
            mesh_uid_map[m->uid] = m;
        }

        int sel_count = level->selection.get_size();
        for (int si = 0; si < sel_count; si++) {
            DedObject* sel_obj = level->selection[si];
            if (!sel_obj) continue;
            if (sel_obj->type != DedObjectType::DED_EVENT &&
                sel_obj->type != DedObjectType::DED_TRIGGER) continue;
            int lc = sel_obj->links.get_size();
            for (int li = 0; li < lc; li++) {
                auto it = mesh_uid_map.find(sel_obj->links[li]);
                if (it != mesh_uid_map.end()) {
                    auto* target = it->second;
                    draw_3d_line(sel_obj->pos.x, sel_obj->pos.y, sel_obj->pos.z,
                                 target->pos.x, target->pos.y, target->pos.z,
                                 0, 128, 255);
                }
            }
        }
    }

    constexpr float s = 0.5f;
    bool did_lazy_load = false;
    for (auto* mesh : meshes) {
        if (mesh->hidden_in_editor) continue;
        float x = mesh->pos.x, y = mesh->pos.y, z = mesh->pos.z;
        bool selected = is_object_selected(level, mesh);

        // Lazy-load vmesh on first render (one per frame to avoid VFX loader issues)
        bool just_loaded = false;
        if (!get_vmesh(mesh) && !mesh->vmesh_load_failed && mesh->mesh_filename.c_str()[0] != '\0') {
            if (!did_lazy_load) {
                mesh_load_vmesh(mesh);
                did_lazy_load = true;
                just_loaded = true;
            }
        }

        // Render the actual vmesh if loaded (skip on the frame it was just loaded
        // to avoid VFX render state conflicts from loading + rendering in same frame)
        auto* vm = get_vmesh(mesh);
        if (vm && !just_loaded) {
            set_draw_color(0xff, 0xff, 0xff, 0xff);

            EditorRenderParams render_params;

            // Check if textures are enabled (DAT_006c9aa8)
            if (*reinterpret_cast<int*>(0x006c9aa8) != 0) {
                render_params.flags |= ERF_TEXTURED;
                render_params.diffuse_color = {0xff, 0xff, 0xff, 0xff};
            }

            // Selection highlight
            if (selected) {
                render_params.flags |= ERF_SELECTION_HIGHLIGHT;
                render_params.selection_color = {0xff, 0x00, 0x00, 0xff};
            }

            // Advance animation each frame for animated mesh types
            auto vmesh_type = vmesh_get_type(vm);
            if (vmesh_type == VMESH_TYPE_ANIM_FX) {
                vmesh_process(vm, frame_dt, 0, &mesh->pos, &mesh->orient, 1);
                *reinterpret_cast<int*>(0x0059e21c) = 1;
            }
            else if (vmesh_type == VMESH_TYPE_CHARACTER) {
                if (vm->instance) {
                    auto it = g_v3c_action_cache.find(vm);
                    if (it != g_v3c_action_cache.end() && it->second >= 0) {
                        // Check if simulate mode changed; reload action if mismatched
                        auto sim_it = g_v3c_action_simulating.find(vm);
                        bool was_simulating = (sim_it != g_v3c_action_simulating.end()) && sim_it->second;
                        if (mesh->simulate_in_editor != was_simulating) {
                            const char* anim = mesh->state_anim.c_str();
                            if (anim && anim[0] != '\0') {
                                mesh_play_v3c_action(vm, anim, mesh->simulate_in_editor);
                                vmesh_process(vm, 0.01f, 0, &mesh->pos, &mesh->orient, 1);
                            }
                        }

                        if (mesh->simulate_in_editor) {
                            // Simulate mode: advance animation each frame
                            // Track elapsed time to detect when one-shot action expires
                            auto& elapsed = g_v3c_action_elapsed[vm];
                            elapsed += frame_dt;
                            float duration = vmesh_get_action_duration(vm, it->second);
                            if (duration > 0.0f && elapsed >= duration) {
                                // One-shot action expired — restart for looping
                                elapsed = 0.0f;
                                vmesh_stop_all_actions(vm);
                                vmesh_play_action_by_index(vm, it->second, 1.0f, 0);
                            }
                            vmesh_process(vm, frame_dt, 0, &mesh->pos, &mesh->orient, 1);
                        }
                        // Freeze mode: don't call vmesh_process, pose stays at frame 0
                    }
                }
            }

            // Get bounding sphere radius for room visibility setup
            float bound_center[3] = {};
            float bound_radius = 0.0f;
            vmesh_get_bound_sphere(vm, bound_center, &bound_radius);

            // Room visibility setup (required for mesh rendering)
            using RoomSetupFn = int(__cdecl*)(int, const void*, float, int, int);
            using RoomCleanupFn = void(__cdecl*)();
            auto room_setup = reinterpret_cast<RoomSetupFn>(0x004885d0);
            auto room_cleanup = reinterpret_cast<RoomCleanupFn>(0x00488bb0);

            room_setup(0, &mesh->pos, bound_radius, 1, 1);
            vmesh_render(vm, &mesh->pos, &mesh->orient, &render_params);
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

        // Draw inbound link arrows (blue lines) when selected
        if (selected && level) {
            // Links FROM other objects TO this mesh
            // Scan all per-type VArrays (all_objects is only populated during save)
            for (int va_off = 0x340; va_off <= 0x430; va_off += 0xC) {
                auto& va = struct_field_ref<VArray<DedObject*>>(level, va_off);
                int va_sz = va.get_size();
                for (int oi = 0; oi < va_sz; oi++) {
                    DedObject* obj = va[oi];
                    if (!obj) continue;
                    auto& stock_links = get_obj_links(obj);
                    int obj_links = stock_links.get_size();
                    for (int li = 0; li < obj_links; li++) {
                        if (stock_links[li] == mesh->uid) {
                            draw_3d_line(obj->pos.x, obj->pos.y, obj->pos.z, x, y, z, 0, 128, 255);
                        }
                    }
                }
            }
        }
    }
}

// ─── Handler Functions (called from alpine_obj.cpp shared hooks) ────────────

void mesh_tree_populate(EditorTreeCtrl* tree, int master_groups, CDedLevel* level)
{
    auto& meshes = level->GetAlpineLevelProperties().mesh_objects;

    char buf[64];
    snprintf(buf, sizeof(buf), "Meshes (%d)", static_cast<int>(meshes.size()));

    int parent = tree->insert_item(buf, master_groups, 0xffff0002);

    for (auto* mesh : meshes) {
        const char* name = mesh->script_name.c_str();
        if (!name || name[0] == '\0') {
            name = mesh->mesh_filename.c_str();
        }
        if (!name || name[0] == '\0') {
            name = "(unnamed)";
        }
        int child = tree->insert_item(name, parent, 0xffff0002);
        tree->set_item_data(child, mesh->uid);
    }
}

void mesh_tree_add_object_type(EditorTreeCtrl* tree)
{
    tree->insert_item("Mesh", 0xffff0000, 0xffff0002);
}

void mesh_pick(CDedLevel* level, int param1, int param2)
{
    auto& meshes = level->GetAlpineLevelProperties().mesh_objects;
    for (auto* mesh : meshes) {
        if (mesh->hidden_in_editor) continue;
        bool hit = level->hit_test_point(param1, param2, &mesh->pos);
        if (hit) {
            level->select_object(static_cast<DedObject*>(mesh));
            xlog::debug("[Mesh] Pick: selected mesh uid={} at ({},{},{})",
                mesh->uid, mesh->pos.x, mesh->pos.y, mesh->pos.z);
        }
    }
}

DedMesh* mesh_click_pick(CDedLevel* level, float click_x, float click_y, float* out_dist_sq)
{
    auto& meshes = level->GetAlpineLevelProperties().mesh_objects;
    float best_dist_sq = 1e30f;
    DedMesh* best_mesh = nullptr;

    for (auto* mesh : meshes) {
        if (mesh->hidden_in_editor) continue;

        // Get bounding sphere (world-space center + radius)
        float bound_center[3] = {}, bound_radius = 0.5f;
        if (auto* v = get_vmesh(mesh)) {
            vmesh_get_bound_sphere(v, bound_center, &bound_radius);
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

    if (out_dist_sq) *out_dist_sq = best_dist_sq;
    return best_mesh;
}

// ─── Clipboard ──────────────────────────────────────────────────────────────

// Mesh clipboard: stores staged clones (with add_to_level=false) for Ctrl+V paste.
static std::vector<DedMesh*> g_mesh_clipboard;

void mesh_clear_clipboard()
{
    for (auto* mesh : g_mesh_clipboard) {
        if (auto* v = get_vmesh(mesh)) {
            g_v3c_action_cache.erase(v);
            g_v3c_action_simulating.erase(v);
            g_v3c_action_elapsed.erase(v);
            vmesh_free(v);
        }
        mesh->field_4.free();
        mesh->script_name.free();
        mesh->class_name.free();
        mesh->mesh_filename.free();
        mesh->state_anim.free();
        delete mesh;
    }
    g_mesh_clipboard.clear();
}

bool mesh_copy_object(DedObject* source)
{
    if (!source || source->type != DedObjectType::DED_MESH) return false;
    auto* staged = CloneMeshObject(static_cast<DedMesh*>(source), false);
    if (staged) {
        g_mesh_clipboard.push_back(staged);
        return true;
    }
    return false;
}

void mesh_paste_objects(CDedLevel* level)
{
    for (auto* staged : g_mesh_clipboard) {
        auto* clone = CloneMeshObject(staged, true);
        if (clone) {
            level->add_to_selection(static_cast<DedObject*>(clone));
        }
    }
}

// ─── Delete / Cut ───────────────────────────────────────────────────────────

void mesh_handle_delete_or_cut(DedObject* obj)
{
    if (!obj || obj->type != DedObjectType::DED_MESH) return;
    auto* level = CDedLevel::Get();
    if (!level) return;

    auto* mesh = static_cast<DedMesh*>(obj);
    auto& mesh_objects = level->GetAlpineLevelProperties().mesh_objects;
    auto it = std::find(mesh_objects.begin(), mesh_objects.end(), mesh);
    if (it != mesh_objects.end()) {
        mesh_objects.erase(it);
    }
}

void mesh_handle_delete_selection(CDedLevel* level)
{
    auto& sel = level->selection;
    for (int i = sel.size - 1; i >= 0; i--) {
        DedObject* obj = sel.data_ptr[i];
        if (obj && obj->type == DedObjectType::DED_MESH) {
            // Remove from selection
            for (int j = i; j < sel.size - 1; j++) {
                sel.data_ptr[j] = sel.data_ptr[j + 1];
            }
            sel.size--;
            DeleteMeshObject(static_cast<DedMesh*>(obj));
        }
    }
}

