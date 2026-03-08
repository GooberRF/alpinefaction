#include <string>
#include <vector>
#include <cstring>
#include <xlog/xlog.h>
#include <patch_common/MemUtils.h>
#include "../rf/object.h"
#include "../rf/clutter.h"
#include "../rf/vmesh.h"
#include "../rf/level.h"
#include "../rf/file/file.h"
#include "../rf/geometry.h"
#include "../rf/os/frametime.h"
#include "../rf/bmpman.h"
#include "../misc/level.h"

// ─── Globals ────────────────────────────────────────────────────────────────

static std::vector<AlpineMeshInfo> g_alpine_mesh_infos;
static std::vector<int> g_alpine_mesh_handles; // object handles for cleanup

// Per-mesh animation state for deferred startup and looping
struct AlpineMeshAnimState {
    int obj_handle;
    std::string state_anim;
    int action_index = -1;     // resolved action index, -1 = not yet loaded
    bool anim_started = false;  // true once we've started playing
    int startup_delay = 2;      // frames to wait before starting animation
};
static std::vector<AlpineMeshAnimState> g_mesh_anim_states;

// Dummy ClutterInfo as raw bytes - avoids calling rf::String/VArray constructors during DLL init
// All zeros is safe: String{0,nullptr} = empty, VArray{0,nullptr} = empty
alignas(rf::ClutterInfo) static uint8_t g_dummy_clutter_info_buf[sizeof(rf::ClutterInfo)];

// Clutter linked list tail pointer (sentinel.prev)
static auto& clutter_list_tail = addr_as_ref<rf::Clutter*>(0x005C95F0);
// Clutter count
static auto& clutter_count = addr_as_ref<int>(0x005C9358);

std::vector<AlpineMeshInfo>& get_alpine_mesh_infos()
{
    return g_alpine_mesh_infos;
}

// ─── VMesh Type Detection ───────────────────────────────────────────────────

static rf::VMeshType determine_vmesh_type(const std::string& filename)
{
    auto dot = filename.rfind('.');
    if (dot == std::string::npos) return rf::MESH_TYPE_STATIC;

    std::string ext = filename.substr(dot);
    // lowercase compare
    for (auto& c : ext) c = static_cast<char>(tolower(c));

    if (ext == ".v3c") return rf::MESH_TYPE_CHARACTER;
    if (ext == ".vfx") return rf::MESH_TYPE_ANIM_FX;
    return rf::MESH_TYPE_STATIC;
}

// ─── Chunk Loading ──────────────────────────────────────────────────────────

// Mesh chunk version marker (distinguishes v2+ from old format where first uint32 was count)
static constexpr uint32_t MESH_CHUNK_VERSION_MARKER = 0xAF000002;

void alpine_mesh_load_chunk(rf::File& file, std::size_t chunk_len)
{
    std::size_t remaining = chunk_len;

    auto read_bytes = [&](void* dst, std::size_t n) -> bool {
        if (remaining < n) return false;
        int got = file.read(dst, n);
        if (got != static_cast<int>(n)) return false;
        remaining -= n;
        return true;
    };

    auto read_string = [&]() -> std::string {
        uint16_t len = 0;
        if (!read_bytes(&len, sizeof(len))) return "";
        if (len == 0 || remaining < len) {
            if (len > 0 && remaining >= len) {
                file.seek(len, rf::File::seek_cur);
                remaining -= len;
            }
            return "";
        }
        std::string result(len, '\0');
        file.read(result.data(), len);
        remaining -= len;
        return result;
    };

    // Check for version marker vs old format
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

    g_alpine_mesh_infos.reserve(count);

    for (uint32_t i = 0; i < count; i++) {
        AlpineMeshInfo info;

        if (!read_bytes(&info.uid, sizeof(info.uid))) return;
        // pos
        if (!read_bytes(&info.pos.x, sizeof(float))) return;
        if (!read_bytes(&info.pos.y, sizeof(float))) return;
        if (!read_bytes(&info.pos.z, sizeof(float))) return;
        // orient (3x3 row-major)
        if (!read_bytes(&info.orient.rvec.x, sizeof(float))) return;
        if (!read_bytes(&info.orient.rvec.y, sizeof(float))) return;
        if (!read_bytes(&info.orient.rvec.z, sizeof(float))) return;
        if (!read_bytes(&info.orient.uvec.x, sizeof(float))) return;
        if (!read_bytes(&info.orient.uvec.y, sizeof(float))) return;
        if (!read_bytes(&info.orient.uvec.z, sizeof(float))) return;
        if (!read_bytes(&info.orient.fvec.x, sizeof(float))) return;
        if (!read_bytes(&info.orient.fvec.y, sizeof(float))) return;
        if (!read_bytes(&info.orient.fvec.z, sizeof(float))) return;
        // strings
        info.script_name = read_string();
        info.mesh_filename = read_string();
        info.state_anim = read_string();
        // collision mode
        uint8_t collision_mode = 2;
        if (!read_bytes(&collision_mode, sizeof(collision_mode))) return;
        info.collision_mode = (collision_mode <= 2) ? collision_mode : 2;
        // links
        int32_t link_count = 0;
        if (!read_bytes(&link_count, sizeof(link_count))) return;
        if (link_count > 1000) link_count = 1000;
        info.link_uids.resize(link_count);
        for (int32_t j = 0; j < link_count; j++) {
            if (!read_bytes(&info.link_uids[j], sizeof(int32_t))) return;
        }
        // v2: texture overrides
        if (has_texture_overrides) {
            for (int ti = 0; ti < MAX_MESH_TEXTURES; ti++) {
                info.texture_overrides[ti] = read_string();
            }
        }

        xlog::debug("[AlpineMesh] Loaded mesh info uid={} file='{}' pos=({},{},{})",
            info.uid, info.mesh_filename, info.pos.x, info.pos.y, info.pos.z);

        g_alpine_mesh_infos.push_back(std::move(info));
    }

    // skip remaining
    if (remaining > 0) {
        file.seek(static_cast<int>(remaining), rf::File::seek_cur);
    }

    xlog::info("[AlpineMesh] Loaded {} mesh infos from RFL (v{})", count, has_texture_overrides ? 2 : 1);
}

// ─── Object Creation ────────────────────────────────────────────────────────

// obj_create: creates an object in the game world
// Signature: Object*(int type, int sub_type, int parent, ObjectCreateInfo*, int flags, GRoom*)
static auto& obj_create = addr_as_ref<rf::Object*(int, int, int, rf::ObjectCreateInfo*, int, rf::GRoom*)>(0x00486DA0);


// Load an .rfa/.mvf animation file onto a v3c character mesh and play it.
// character_mesh_load_action (game 0x0051cc10): __thiscall on mesh_data, loads .rfa, returns action index
// vmesh_play_action_by_index (game 0x005033b0): cdecl(vmesh, action_index, transition_time, hold_last_frame)

using CharMeshLoadActionFn = int(__thiscall*)(void* mesh_data, const char* rfa_filename, char flag);
static const auto character_mesh_load_action = reinterpret_cast<CharMeshLoadActionFn>(0x0051cc10);

using VmeshPlayActionByIndexFn = void(__cdecl*)(rf::VMesh* vmesh, int action_index, float transition_time, int hold_last_frame);
static const auto vmesh_play_action_by_index = reinterpret_cast<VmeshPlayActionByIndexFn>(0x005033b0);

// vmesh_reset_actions: zeros weights of all looping (flag=1) action slots
using VmeshResetActionsFn = void(__cdecl*)(rf::VMesh* vmesh);
static const auto vmesh_reset_actions = reinterpret_cast<VmeshResetActionsFn>(0x005033f0);

// vmesh_set_action_weight: sets blend weight for an action slot (does NOT reset playback position)
using VmeshSetActionWeightFn = void(__cdecl*)(rf::VMesh* vmesh, int action_index, float weight);
static const auto vmesh_set_action_weight = reinterpret_cast<VmeshSetActionWeightFn>(0x00503390);

static bool vmesh_play_v3c_action_by_name(rf::VMesh* vmesh, const char* action_name)
{
    if (!vmesh || !action_name || action_name[0] == '\0') return false;
    if (vmesh->type != rf::MESH_TYPE_CHARACTER) return false;
    if (!vmesh->mesh || !vmesh->instance) {
        xlog::warn("[AlpineMesh] Cannot play animation '{}': mesh={:p} instance={:p}",
            action_name, vmesh->mesh, vmesh->instance);
        return false;
    }

    auto* mesh_data = reinterpret_cast<uint8_t*>(vmesh->mesh);
    int existing_count = *reinterpret_cast<int*>(mesh_data + 0xF58);
    xlog::info("[AlpineMesh] mesh_data={:p} existing_action_count={}", vmesh->mesh, existing_count);

    // Load the .rfa/.mvf animation file onto the character mesh_data
    int action_index = character_mesh_load_action(vmesh->mesh, action_name, 0);
    if (action_index < 0) {
        xlog::warn("[AlpineMesh] Failed to load animation '{}' on vmesh", action_name);
        return false;
    }

    xlog::info("[AlpineMesh] Playing animation '{}' (action_index={}) on vmesh", action_name, action_index);

    // Play the loaded action (transition_time must be > 0 or play_action is a no-op)
    vmesh_play_action_by_index(vmesh, action_index, 1.0f, 0);
    return true;
}

// obj_collision_register: registers an object for collision detection (creates collision pairs)
static auto& obj_collision_register = addr_as_ref<void(rf::Object* obj)>(0x0048C9A0);

void alpine_mesh_create_all()
{
    g_alpine_mesh_handles.clear();

    // Initialize dummy ClutterInfo with safe defaults (raw bytes, no constructors)
    auto* dummy_info = reinterpret_cast<rf::ClutterInfo*>(g_dummy_clutter_info_buf);
    std::memset(dummy_info, 0, sizeof(rf::ClutterInfo));
    dummy_info->life = -1.0f; // invulnerable
    dummy_info->sound = -1;
    dummy_info->use_sound = -1;
    dummy_info->explode_anim_vclip = -1;
    dummy_info->glare = -1;
    dummy_info->rod_glare = -1;
    dummy_info->light_prop = -1;

    for (auto& info : g_alpine_mesh_infos) {
        if (info.mesh_filename.empty()) {
            xlog::warn("[AlpineMesh] Skipping mesh uid={} with empty filename", info.uid);
            continue;
        }

        rf::VMeshType vtype = determine_vmesh_type(info.mesh_filename);

        rf::ObjectCreateInfo oci{};
        oci.v3d_filename = info.mesh_filename.c_str();
        oci.v3d_type = vtype;
        oci.pos = info.pos;
        oci.orient = info.orient;
        // Match stock clutter: only set PF_COLLIDE_OBJECTS when collision is enabled
        if (info.collision_mode > 0) {
            oci.physics_flags = rf::PF_COLLIDE_OBJECTS;
        }

        // Create as clutter object (type 4 = OT_CLUTTER)
        // Pass nullptr for room - obj_create handles room assignment from position
        rf::Object* obj = obj_create(rf::OT_CLUTTER, -1, 0, &oci, 0, nullptr);
        if (!obj) {
            xlog::warn("[AlpineMesh] Failed to create object for mesh uid={} file='{}'", info.uid, info.mesh_filename);
            continue;
        }

        // Cast to Clutter and set fields needed for safe cleanup
        // Don't memset — the Clutter constructor initialized VArrays and internal state we must preserve
        auto* clutter = reinterpret_cast<rf::Clutter*>(obj);
        clutter->info = dummy_info;
        clutter->info_index = -1;
        clutter->corpse_index = -1;
        clutter->sound_handle = -1;
        clutter->delayed_kill_sound = -1;
        clutter->dmg_type_that_killed_me = 0;
        clutter->corpse_vmesh_handle = nullptr;
        clutter->current_skin_index = 0;
        clutter->already_spawned_glass = false;
        clutter->use_sound = -1;
        clutter->killable_index = 0xFFFF;
        // Field at +0x2D0 is freed as sound handle in cleanup if != -1
        *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(clutter) + 0x2D0) = -1;
        // Note: Do NOT set corpse_vmesh_handle (+0x2B8) to -1 — cleanup dereferences it as a VMesh*.
        // It's set to nullptr above which is safe.

        // Register in clutter linked list (required for rendering and proper cleanup)
        clutter->prev = clutter_list_tail;
        clutter->next = reinterpret_cast<rf::Clutter*>(&rf::clutter_list);
        clutter_list_tail->next = clutter;
        clutter_list_tail = clutter;
        clutter_count++;

        obj->uid = info.uid;
        if (!info.script_name.empty()) {
            obj->name = info.script_name.c_str();
        }

        // Make invulnerable by default and set life to positive value.
        // Life MUST be >= 0 or the stock clutter process sets the dead flag (obj_flags |= 2).
        obj->life = 100.0f;
        obj->obj_flags = static_cast<rf::ObjectFlags>(
            static_cast<int>(obj->obj_flags) | static_cast<int>(rf::OF_INVULNERABLE)
        );

        // Match stock clutter collision setup (FUN_004104a0):
        // - None: no collision registration, no physics fixup needed
        // - Only Weapons: OF_WEAPON_ONLY_COLLIDE + collision registration
        // - All: collision registration only
        if (info.collision_mode > 0) {
            // Fix up physics data — physics_create_object generates bad auto-cspheres
            // with radius=0, causing mass=0 and zero-size bbox
            float r = obj->radius;
            obj->p_data.radius = r;
            obj->p_data.mass = 10000.0f;
            obj->p_data.cspheres.clear();
            rf::PCollisionSphere sphere{};
            sphere.center = {0.0f, 0.0f, 0.0f};
            sphere.radius = r;
            obj->p_data.cspheres.add(sphere);
            obj->p_data.bbox_min = {obj->pos.x - r, obj->pos.y - r, obj->pos.z - r};
            obj->p_data.bbox_max = {obj->pos.x + r, obj->pos.y + r, obj->pos.z + r};

            if (info.collision_mode == 1) {
                // Only Weapons: collides with weapons but not players/entities
                obj->obj_flags = static_cast<rf::ObjectFlags>(
                    static_cast<int>(obj->obj_flags) | static_cast<int>(rf::OF_WEAPON_ONLY_COLLIDE)
                );
            }

            // Register for collision (stock code does this only when PF_COLLIDE_OBJECTS is set)
            obj_collision_register(obj);
        }

        // Apply texture overrides
        if (obj->vmesh) {
            bool has_tex_override = false;
            for (int ti = 0; ti < MAX_MESH_TEXTURES; ti++) {
                if (!info.texture_overrides[ti].empty()) { has_tex_override = true; break; }
            }
            if (has_tex_override) {
                int num_materials = 0;
                rf::MeshMaterial* materials = nullptr;
                rf::vmesh_get_materials_array(obj->vmesh, &num_materials, &materials);
                if ((!materials || num_materials <= 0) && obj->vmesh->type == rf::MESH_TYPE_STATIC) {
                    // V3M with multiple LODs/sub-meshes: the engine's allocator bails early.
                    // Temporarily fake single-LOD/single-submesh counts to force allocation.
                    obj->vmesh->use_replacement_materials = 0;
                    obj->vmesh->replacement_materials = nullptr;
                    auto* instance = reinterpret_cast<uint8_t*>(obj->vmesh->instance);
                    if (instance) {
                        int* lod_count_ptr = reinterpret_cast<int*>(instance + 0x50);
                        int** submesh_list_ptr = reinterpret_cast<int**>(instance + 0x54);
                        int orig_lod = *lod_count_ptr;
                        int orig_sub = (submesh_list_ptr && *submesh_list_ptr) ? **submesh_list_ptr : 1;
                        *lod_count_ptr = 1;
                        if (submesh_list_ptr && *submesh_list_ptr) **submesh_list_ptr = 1;
                        rf::vmesh_get_materials_array(obj->vmesh, &num_materials, &materials);
                        *lod_count_ptr = orig_lod;
                        if (submesh_list_ptr && *submesh_list_ptr) **submesh_list_ptr = orig_sub;
                        if (!materials || num_materials <= 0) {
                            obj->vmesh->use_replacement_materials = 0;
                            xlog::warn("[AlpineMesh] Failed to allocate replacement materials for multi-LOD V3M");
                        } else {
                            xlog::info("[AlpineMesh] Allocated replacement materials for multi-LOD V3M ({} mats)", num_materials);
                        }
                    }
                }
                if (materials && num_materials > 0) {
                    for (int ti = 0; ti < MAX_MESH_TEXTURES; ti++) {
                        if (info.texture_overrides[ti].empty()) continue;
                        if (ti >= num_materials) {
                            xlog::warn("[AlpineMesh] Texture override slot {} exceeds material count {}", ti, num_materials);
                            break;
                        }
                        int bm_handle = rf::bm::load(info.texture_overrides[ti].c_str(), -1, true);
                        if (bm_handle < 0) {
                            xlog::warn("[AlpineMesh] Failed to load texture '{}' for slot {}",
                                info.texture_overrides[ti], ti);
                            continue;
                        }
                        materials[ti].texture_maps[0].tex_handle = bm_handle;
                        xlog::info("[AlpineMesh] Applied texture override slot {}: '{}' (handle={})",
                            ti, info.texture_overrides[ti], bm_handle);
                    }
                }
            }
        }

        // Defer state animation for skeletal meshes to first game frame
        if (vtype == rf::MESH_TYPE_CHARACTER && !info.state_anim.empty() && obj->vmesh) {
            AlpineMeshAnimState anim_state;
            anim_state.obj_handle = obj->handle;
            anim_state.state_anim = info.state_anim;
            g_mesh_anim_states.push_back(std::move(anim_state));
        }

        g_alpine_mesh_handles.push_back(obj->handle);
        xlog::info("[AlpineMesh] Created mesh object uid={} handle={} file='{}' vmesh={} room={} "
            "obj_radius={:.3f} p_radius={:.3f} p_flags=0x{:x} cspheres={} bbox=({:.1f},{:.1f},{:.1f})-({:.1f},{:.1f},{:.1f})",
            info.uid, obj->handle, info.mesh_filename,
            static_cast<void*>(obj->vmesh), static_cast<void*>(obj->room),
            obj->radius, obj->p_data.radius, obj->p_data.flags,
            obj->p_data.cspheres.size(),
            obj->p_data.bbox_min.x, obj->p_data.bbox_min.y, obj->p_data.bbox_min.z,
            obj->p_data.bbox_max.x, obj->p_data.bbox_max.y, obj->p_data.bbox_max.z);
    }

    xlog::info("[AlpineMesh] Created {} mesh objects in game", g_alpine_mesh_handles.size());
}

// ─── Per-Frame Animation Processing ─────────────────────────────────────────

void alpine_mesh_do_frame()
{
    for (auto& anim : g_mesh_anim_states) {
        rf::Object* obj = rf::obj_from_handle(anim.obj_handle);
        if (!obj || !obj->vmesh) continue;
        if (obj->vmesh->type != rf::MESH_TYPE_CHARACTER) continue;
        if (!obj->vmesh->mesh || !obj->vmesh->instance) continue;

        // Wait a few frames after level load before starting animations
        // This ensures all subsystems are fully initialized
        if (anim.startup_delay > 0) {
            anim.startup_delay--;
            continue;
        }

        // Load the animation action with flag=1 (looping).
        // Looping actions use modular time (fmod) — the playback position wraps
        // automatically and the slot is never removed.
        if (anim.action_index < 0) {
            anim.action_index = character_mesh_load_action(obj->vmesh->mesh, anim.state_anim.c_str(), 1);
            if (anim.action_index < 0) {
                xlog::warn("[AlpineMesh] Failed to load animation '{}' for handle {}",
                    anim.state_anim, anim.obj_handle);
                anim.anim_started = true;
                continue;
            }
            xlog::info("[AlpineMesh] Loaded animation '{}' action_index={} for handle {}",
                anim.state_anim, anim.action_index, anim.obj_handle);
        }

        // Entity-style looping: each frame, zero all looping action weights then
        // set the desired action back to weight 1.0. This never resets the playback
        // position, so the animation loops seamlessly with no base pose flash.
        // NOTE: The stock clutter process (FUN_0040fe10) does NOT call vmesh_process,
        // so we must call it ourselves.
        vmesh_reset_actions(obj->vmesh);
        vmesh_set_action_weight(obj->vmesh, anim.action_index, 1.0f);

        if (!anim.anim_started) {
            anim.anim_started = true;
            xlog::info("[AlpineMesh] Started animation '{}' (action_index={}) on handle {}",
                anim.state_anim, anim.action_index, anim.obj_handle);
        }

        // Advance animation — stock clutter process does NOT call vmesh_process
        rf::vmesh_process(obj->vmesh, rf::frametime, 0, &obj->pos, &obj->orient, 1);
    }
}

void alpine_mesh_clear_state()
{
    g_alpine_mesh_infos.clear();
    g_alpine_mesh_handles.clear();
    g_mesh_anim_states.clear();
}
