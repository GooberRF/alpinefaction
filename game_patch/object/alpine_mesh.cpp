#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>
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
#include "../rf/event.h"
#include "../misc/level.h"

// ─── Globals ────────────────────────────────────────────────────────────────

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

// Meshes that had animations triggered by events and need vmesh_process each frame.
// Without vmesh_process, the animation blending state is never advanced and the
// renderer reads uninitialized data, causing crashes.
struct EventAnimatedMesh {
    int obj_handle;
    int animate_type;    // 0=Action (unused here, driven by state_anim), 1=Action Hold Last, 2=State
    int action_index;
    float blend_weight;
    int startup_delay = 0;  // frames to wait before first vmesh_process
};
static std::vector<EventAnimatedMesh> g_event_animated_meshes;

// Dummy ClutterInfo as raw bytes - avoids calling rf::String/VArray constructors during DLL init
// All zeros is safe: String{0,nullptr} = empty, VArray{0,nullptr} = empty
alignas(rf::ClutterInfo) static uint8_t g_dummy_clutter_info_buf[sizeof(rf::ClutterInfo)];

// Clutter linked list tail pointer (sentinel.prev)
static auto& clutter_list_tail = addr_as_ref<rf::Clutter*>(0x005C95F0);
// Clutter count
static auto& clutter_count = addr_as_ref<int>(0x005C9358);

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

// Forward declaration
static void alpine_mesh_create_object(const AlpineMeshInfo& info);

// ─── Chunk Loading ──────────────────────────────────────────────────────────

// Mesh chunk version marker
static constexpr uint32_t MESH_CHUNK_VERSION_MARKER = 0xAF000003;

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

    uint32_t count = 0;
    if (first_word != MESH_CHUNK_VERSION_MARKER) {
        xlog::warn("[AlpineMesh] Unknown mesh chunk version marker: 0x{:08X}", first_word);
        return;
    }
    if (!read_bytes(&count, sizeof(count))) return;
    if (count > 10000) count = 10000;

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
        // links (mesh→object links, stored but not used for event resolution)
        int32_t link_count = 0;
        if (!read_bytes(&link_count, sizeof(link_count))) return;
        if (link_count > 1000) link_count = 1000;
        info.link_uids.resize(link_count);
        for (int32_t j = 0; j < link_count; j++) {
            if (!read_bytes(&info.link_uids[j], sizeof(int32_t))) return;
        }
        // texture overrides
        for (int ti = 0; ti < MAX_MESH_TEXTURES; ti++) {
            info.texture_overrides[ti] = read_string();
        }

        xlog::info("[AlpineMesh] Read mesh info[{}]: uid={} file='{}' pos=({:.2f},{:.2f},{:.2f}) "
            "links={} collision={} state_anim='{}'",
            i, info.uid, info.mesh_filename, info.pos.x, info.pos.y, info.pos.z,
            static_cast<int>(info.link_uids.size()), info.collision_mode, info.state_anim);
        for (size_t j = 0; j < info.link_uids.size(); j++) {
            xlog::info("[AlpineMesh]   mesh uid={} link_uid[{}] = {}", info.uid, j, info.link_uids[j]);
        }
        for (int ti = 0; ti < MAX_MESH_TEXTURES; ti++) {
            if (!info.texture_overrides[ti].empty()) {
                xlog::info("[AlpineMesh]   mesh uid={} texture_override[{}] = '{}'",
                    info.uid, ti, info.texture_overrides[ti]);
            }
        }

        // Create the game object immediately so it exists before the stock link
        // resolver runs. This lets the stock resolver convert event→mesh link UIDs
        // to handles automatically, just like any other object type.
        alpine_mesh_create_object(info);
    }

    // skip remaining
    if (remaining > 0) {
        file.seek(static_cast<int>(remaining), rf::File::seek_cur);
    }

    xlog::info("[AlpineMesh] Chunk complete: created {} mesh objects, g_alpine_mesh_handles has {} entries",
        count, g_alpine_mesh_handles.size());
    // Dump all created handles for cross-reference with link resolution
    for (size_t i = 0; i < g_alpine_mesh_handles.size(); i++) {
        rf::Object* obj = rf::obj_from_handle(g_alpine_mesh_handles[i]);
        xlog::info("[AlpineMesh]   handle[{}] = {} -> obj={:p} uid={} name='{}'",
            i, g_alpine_mesh_handles[i],
            static_cast<void*>(obj),
            obj ? obj->uid : -1,
            obj ? obj->name.c_str() : "<null>");
    }
}

// ─── Object Creation ────────────────────────────────────────────────────────

// obj_create: creates an object in the game world
// Signature: Object*(int type, int sub_type, int parent, ObjectCreateInfo*, int flags, GRoom*)
static auto& obj_create = addr_as_ref<rf::Object*(int, int, int, rf::ObjectCreateInfo*, int, rf::GRoom*)>(0x00486DA0);


// Load an .rfa/.mvf animation file onto a v3c character mesh and play it.
// character_mesh_load_action (game 0x0051cc10): __thiscall on mesh_data, loads .rfa, returns action index
// vmesh_play_action_by_index (game 0x005033b0): cdecl(vmesh, action_index, transition_time, hold_last_frame)

// 3 stack args: filename, is_state flag, unused — original function does RET 0xC (12 bytes)
using CharMeshLoadActionFn = int(__thiscall*)(void* mesh_data, const char* rfa_filename, char is_state, char unused);
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
    int action_index = character_mesh_load_action(vmesh->mesh, action_name, 0, 0);
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

static bool g_dummy_clutter_info_initialized = false;

// Create a single mesh object from loaded info. Called during chunk reading so mesh
// objects exist before the stock link resolver runs (just like stock clutter/entities).
static void alpine_mesh_create_object(const AlpineMeshInfo& info)
{
    if (info.mesh_filename.empty()) {
        xlog::warn("[AlpineMesh] Skipping mesh uid={} with empty filename", info.uid);
        return;
    }

    // Initialize dummy ClutterInfo once (deferred to first use rather than DLL init)
    if (!g_dummy_clutter_info_initialized) {
        auto* dummy_info = reinterpret_cast<rf::ClutterInfo*>(g_dummy_clutter_info_buf);
        std::memset(dummy_info, 0, sizeof(rf::ClutterInfo));
        dummy_info->life = -1.0f;
        dummy_info->sound = -1;
        dummy_info->use_sound = -1;
        dummy_info->explode_anim_vclip = -1;
        dummy_info->glare = -1;
        dummy_info->rod_glare = -1;
        dummy_info->light_prop = -1;
        g_dummy_clutter_info_initialized = true;
    }

    rf::VMeshType vtype = determine_vmesh_type(info.mesh_filename);

    rf::ObjectCreateInfo oci{};
    oci.v3d_filename = info.mesh_filename.c_str();
    oci.v3d_type = vtype;
    oci.pos = info.pos;
    oci.orient = info.orient;
    if (info.collision_mode > 0) {
        oci.physics_flags = rf::PF_COLLIDE_OBJECTS;
    }

    rf::Object* obj = obj_create(rf::OT_CLUTTER, -1, 0, &oci, 0, nullptr);
    if (!obj) {
        xlog::warn("[AlpineMesh] Failed to create object for mesh uid={} file='{}'", info.uid, info.mesh_filename);
        return;
    }

    auto* dummy_info = reinterpret_cast<rf::ClutterInfo*>(g_dummy_clutter_info_buf);
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
    *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(clutter) + 0x2D0) = -1;

    clutter->prev = clutter_list_tail;
    clutter->next = reinterpret_cast<rf::Clutter*>(&rf::clutter_list);
    clutter_list_tail->next = clutter;
    clutter_list_tail = clutter;
    clutter_count++;

    obj->uid = info.uid;
    if (!info.script_name.empty()) {
        obj->name = info.script_name.c_str();
    }

    obj->life = 100.0f;
    obj->obj_flags = static_cast<rf::ObjectFlags>(
        static_cast<int>(obj->obj_flags) | static_cast<int>(rf::OF_INVULNERABLE)
    );

    if (info.collision_mode > 0) {
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
            obj->obj_flags = static_cast<rf::ObjectFlags>(
                static_cast<int>(obj->obj_flags) | static_cast<int>(rf::OF_WEAPON_ONLY_COLLIDE)
            );
        }

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
    xlog::info("[AlpineMesh] Created object: uid={} handle={} obj={:p} type={} file='{}' "
        "vmesh={:p} vmesh_type={} room={:p} name='{}' "
        "obj_radius={:.3f} p_radius={:.3f} p_flags=0x{:x} cspheres={} "
        "pos=({:.2f},{:.2f},{:.2f}) bbox=({:.1f},{:.1f},{:.1f})-({:.1f},{:.1f},{:.1f}) "
        "obj_flags=0x{:x} life={:.1f}",
        info.uid, obj->handle, static_cast<void*>(obj), static_cast<int>(obj->type),
        info.mesh_filename,
        static_cast<void*>(obj->vmesh),
        obj->vmesh ? static_cast<int>(obj->vmesh->type) : -1,
        static_cast<void*>(obj->room),
        obj->name.c_str(),
        obj->radius, obj->p_data.radius, obj->p_data.flags,
        obj->p_data.cspheres.size(),
        obj->pos.x, obj->pos.y, obj->pos.z,
        obj->p_data.bbox_min.x, obj->p_data.bbox_min.y, obj->p_data.bbox_min.z,
        obj->p_data.bbox_max.x, obj->p_data.bbox_max.y, obj->p_data.bbox_max.z,
        static_cast<int>(obj->obj_flags), obj->life);

    // Verify the clutter linked list integrity
    auto* clutter_obj = reinterpret_cast<rf::Clutter*>(obj);
    xlog::info("[AlpineMesh]   clutter: info={:p} info_index={} prev={:p} next={:p} "
        "clutter_count={} killable_index={}",
        static_cast<void*>(clutter_obj->info), clutter_obj->info_index,
        static_cast<void*>(clutter_obj->prev), static_cast<void*>(clutter_obj->next),
        clutter_count, clutter_obj->killable_index);
}

// ─── Per-Frame Animation Processing ─────────────────────────────────────────

static int g_do_frame_log_count = 0;

void alpine_mesh_do_frame()
{
    bool should_log = (g_do_frame_log_count < 5);
    if (should_log) {
        g_do_frame_log_count++;
        xlog::info("[AlpineMesh] do_frame #{}: {} anim_states, {} event_animated, {} handles",
            g_do_frame_log_count, g_mesh_anim_states.size(),
            g_event_animated_meshes.size(), g_alpine_mesh_handles.size());
    }

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
            anim.action_index = character_mesh_load_action(obj->vmesh->mesh, anim.state_anim.c_str(), 1, 0);
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

    // Process event-animated meshes: call vmesh_process so animations actually advance
    // and the blending state stays valid for the renderer
    for (auto it = g_event_animated_meshes.begin(); it != g_event_animated_meshes.end(); ) {
        rf::Object* obj = rf::obj_from_handle(it->obj_handle);
        if (!obj || !obj->vmesh || !obj->vmesh->mesh || !obj->vmesh->instance) {
            it = g_event_animated_meshes.erase(it);
            continue;
        }

        // Safety: skip non-clutter objects (should never happen, but guard against it)
        if (obj->type != rf::OT_CLUTTER) {
            xlog::warn("[AlpineMesh] Removing non-clutter handle {} (type={}) from event-animated list",
                it->obj_handle, static_cast<int>(obj->type));
            it = g_event_animated_meshes.erase(it);
            continue;
        }

        if (it->startup_delay > 0) {
            it->startup_delay--;
            ++it;
            continue;
        }

        if (it->animate_type == 2) {
            // State (looping): reset all weights then set our action, like state_anim
            vmesh_reset_actions(obj->vmesh);
            vmesh_set_action_weight(obj->vmesh, it->action_index, it->blend_weight);
        }

        // Advance animation
        rf::vmesh_process(obj->vmesh, rf::frametime, 0, &obj->pos, &obj->orient, 1);
        ++it;
    }
}

std::vector<int>& get_alpine_mesh_handles()
{
    return g_alpine_mesh_handles;
}

void alpine_mesh_clear_state()
{
    xlog::info("[AlpineMesh] Clearing state: {} handles, {} anim_states, {} event_animated",
        g_alpine_mesh_handles.size(), g_mesh_anim_states.size(), g_event_animated_meshes.size());
    // Verify all handles still resolve before clearing
    for (size_t i = 0; i < g_alpine_mesh_handles.size(); i++) {
        rf::Object* obj = rf::obj_from_handle(g_alpine_mesh_handles[i]);
        xlog::info("[AlpineMesh]   clearing handle[{}]={} -> obj={:p}{}",
            i, g_alpine_mesh_handles[i], static_cast<void*>(obj),
            obj ? "" : " (STALE!)");
    }
    g_alpine_mesh_handles.clear();
    g_mesh_anim_states.clear();
    g_event_animated_meshes.clear();
    g_dummy_clutter_info_initialized = false;
    g_do_frame_log_count = 0;
}

// ─── Event Helper Functions ─────────────────────────────────────────────────

void alpine_mesh_animate(rf::Object* obj, int type, const std::string& anim_filename, float blend_weight)
{
    if (!obj) {
        return;
    }
    // Only animate clutter objects (our mesh objects are created as OT_CLUTTER).
    // Skip events, triggers, entities, etc. that might be in the event's link list.
    if (obj->type != rf::OT_CLUTTER) {
        return;
    }
    if (!obj->vmesh) {
        xlog::warn("[AlpineMesh] animate: null vmesh on clutter handle {}", obj->handle);
        return;
    }
    if (obj->vmesh->type != rf::MESH_TYPE_CHARACTER) {
        xlog::warn("[AlpineMesh] animate: object is not a skeletal mesh (v3c)");
        return;
    }
    if (!obj->vmesh->mesh || !obj->vmesh->instance) {
        xlog::warn("[AlpineMesh] animate: mesh data not loaded");
        return;
    }

    if (anim_filename.empty()) {
        xlog::warn("[AlpineMesh] animate: no animation filename specified");
        return;
    }

    // Default blend_weight to 1.0 if not set (editor default for float fields is 0)
    if (blend_weight <= 0.0f) {
        blend_weight = 1.0f;
    }

    // type 0 = Action: play once, then return to state_anim
    // type 1 = Action Hold Last: play once, freeze on last frame
    // type 2 = State: loop, override state_anim

    if (type == 0) {
        // Action: one-shot that returns to the state_anim after completion.
        // Load as one-shot (flag=0), play with hold_last_frame=0 so the engine
        // auto-removes the action when it finishes. The state_anim continues
        // running underneath via g_mesh_anim_states, so vmesh_process is already
        // being called each frame — no need to add to g_event_animated_meshes.
        int action_index = character_mesh_load_action(obj->vmesh->mesh, anim_filename.c_str(), 0, 0);
        if (action_index < 0) {
            xlog::warn("[AlpineMesh] Failed to load animation '{}' on handle {}", anim_filename, obj->handle);
            return;
        }

        vmesh_set_action_weight(obj->vmesh, action_index, blend_weight);
        vmesh_play_action_by_index(obj->vmesh, action_index, 1.0f, 0);

        // If the mesh has no state_anim driving vmesh_process, we need to ensure
        // vmesh_process is called each frame so the one-shot actually advances.
        bool has_state_anim = std::any_of(g_mesh_anim_states.begin(), g_mesh_anim_states.end(),
            [&](const AlpineMeshAnimState& a) { return a.obj_handle == obj->handle; });

        if (!has_state_anim) {
            g_event_animated_meshes.erase(
                std::remove_if(g_event_animated_meshes.begin(), g_event_animated_meshes.end(),
                    [&](const EventAnimatedMesh& e) { return e.obj_handle == obj->handle; }),
                g_event_animated_meshes.end());
            g_event_animated_meshes.push_back({obj->handle, 0, action_index, blend_weight});
        }

        rf::vmesh_process(obj->vmesh, 0.0f, 0, &obj->pos, &obj->orient, 1);

        xlog::info("[AlpineMesh] Playing animation '{}' (type=Action, action_index={}, weight={:.2f}) on handle {}",
            anim_filename, action_index, blend_weight, obj->handle);
    }
    else if (type == 1) {
        // Action Hold Last: one-shot that freezes on the last frame permanently.
        // Load as one-shot (flag=0), play with hold_last_frame=1.
        int action_index = character_mesh_load_action(obj->vmesh->mesh, anim_filename.c_str(), 0, 0);
        if (action_index < 0) {
            xlog::warn("[AlpineMesh] Failed to load animation '{}' on handle {}", anim_filename, obj->handle);
            return;
        }

        vmesh_set_action_weight(obj->vmesh, action_index, blend_weight);
        vmesh_play_action_by_index(obj->vmesh, action_index, 1.0f, 1);

        // Remove from state_anim processing — hold-last overrides permanently.
        g_mesh_anim_states.erase(
            std::remove_if(g_mesh_anim_states.begin(), g_mesh_anim_states.end(),
                [&](const AlpineMeshAnimState& a) { return a.obj_handle == obj->handle; }),
            g_mesh_anim_states.end());

        // Register for per-frame vmesh_process.
        g_event_animated_meshes.erase(
            std::remove_if(g_event_animated_meshes.begin(), g_event_animated_meshes.end(),
                [&](const EventAnimatedMesh& e) { return e.obj_handle == obj->handle; }),
            g_event_animated_meshes.end());
        g_event_animated_meshes.push_back({obj->handle, 1, action_index, blend_weight});

        rf::vmesh_process(obj->vmesh, 0.0f, 0, &obj->pos, &obj->orient, 1);

        xlog::info("[AlpineMesh] Playing animation '{}' (type=Action Hold Last, action_index={}, weight={:.2f}) on handle {}",
            anim_filename, action_index, blend_weight, obj->handle);
    }
    else if (type == 2) {
        // State: looping animation that overrides the state_anim.
        // Load as looping (flag=1), manage weight per-frame.
        int action_index = character_mesh_load_action(obj->vmesh->mesh, anim_filename.c_str(), 1, 0);
        if (action_index < 0) {
            xlog::warn("[AlpineMesh] Failed to load animation '{}' on handle {}", anim_filename, obj->handle);
            return;
        }

        vmesh_reset_actions(obj->vmesh);
        vmesh_set_action_weight(obj->vmesh, action_index, blend_weight);

        // Remove from state_anim processing — this overrides it.
        g_mesh_anim_states.erase(
            std::remove_if(g_mesh_anim_states.begin(), g_mesh_anim_states.end(),
                [&](const AlpineMeshAnimState& a) { return a.obj_handle == obj->handle; }),
            g_mesh_anim_states.end());

        // Register for per-frame vmesh_process with weight management.
        g_event_animated_meshes.erase(
            std::remove_if(g_event_animated_meshes.begin(), g_event_animated_meshes.end(),
                [&](const EventAnimatedMesh& e) { return e.obj_handle == obj->handle; }),
            g_event_animated_meshes.end());
        g_event_animated_meshes.push_back({obj->handle, 2, action_index, blend_weight});

        rf::vmesh_process(obj->vmesh, 0.0f, 0, &obj->pos, &obj->orient, 1);

        xlog::info("[AlpineMesh] Playing animation '{}' (type=State, action_index={}, weight={:.2f}) on handle {}",
            anim_filename, action_index, blend_weight, obj->handle);
    }
}

// Helper to get replacement materials array with multi-LOD V3M workaround
static bool get_replacement_materials(rf::VMesh* vmesh, int& num_materials, rf::MeshMaterial*& materials)
{
    num_materials = 0;
    materials = nullptr;
    rf::vmesh_get_materials_array(vmesh, &num_materials, &materials);

    if ((!materials || num_materials <= 0) && vmesh->type == rf::MESH_TYPE_STATIC) {
        // Multi-LOD V3M workaround: temporarily fake single-LOD/single-submesh
        vmesh->use_replacement_materials = 0;
        vmesh->replacement_materials = nullptr;
        auto* instance = reinterpret_cast<uint8_t*>(vmesh->instance);
        if (instance) {
            int* lod_count_ptr = reinterpret_cast<int*>(instance + 0x50);
            int** submesh_list_ptr = reinterpret_cast<int**>(instance + 0x54);
            int orig_lod = *lod_count_ptr;
            int orig_sub = (submesh_list_ptr && *submesh_list_ptr) ? **submesh_list_ptr : 1;
            *lod_count_ptr = 1;
            if (submesh_list_ptr && *submesh_list_ptr) **submesh_list_ptr = 1;
            rf::vmesh_get_materials_array(vmesh, &num_materials, &materials);
            *lod_count_ptr = orig_lod;
            if (submesh_list_ptr && *submesh_list_ptr) **submesh_list_ptr = orig_sub;
            if (!materials || num_materials <= 0) {
                vmesh->use_replacement_materials = 0;
                return false;
            }
        }
    }
    return materials && num_materials > 0;
}

void alpine_mesh_set_texture(rf::Object* obj, int slot, const std::string& texture_filename)
{
    if (!obj || obj->type != rf::OT_CLUTTER || !obj->vmesh) {
        return;
    }

    int num_materials = 0;
    rf::MeshMaterial* materials = nullptr;
    if (!get_replacement_materials(obj->vmesh, num_materials, materials)) {
        xlog::warn("[AlpineMesh] set_texture: failed to get replacement materials");
        return;
    }

    if (slot < 0 || slot >= num_materials) {
        xlog::warn("[AlpineMesh] set_texture: slot {} out of range (0-{})", slot, num_materials - 1);
        return;
    }

    int bm_handle = rf::bm::load(texture_filename.c_str(), -1, true);
    if (bm_handle < 0) {
        xlog::warn("[AlpineMesh] set_texture: failed to load texture '{}'", texture_filename);
        return;
    }

    materials[slot].texture_maps[0].tex_handle = bm_handle;
    xlog::info("[AlpineMesh] Set texture slot {} to '{}' (handle={}) on obj handle {}",
        slot, texture_filename, bm_handle, obj->handle);
}

void alpine_mesh_clear_texture(rf::Object* obj, int slot)
{
    if (!obj || obj->type != rf::OT_CLUTTER || !obj->vmesh) {
        return;
    }

    // To clear a texture override, we need the original material.
    // The replacement materials array is a copy of the originals when first allocated,
    // so we need to reload from the base mesh. For now, set handle to -1 which
    // will cause the renderer to use the default texture.
    int num_materials = 0;
    rf::MeshMaterial* materials = nullptr;
    if (!get_replacement_materials(obj->vmesh, num_materials, materials)) {
        xlog::warn("[AlpineMesh] clear_texture: failed to get replacement materials");
        return;
    }

    if (slot < 0 || slot >= num_materials) {
        xlog::warn("[AlpineMesh] clear_texture: slot {} out of range (0-{})", slot, num_materials - 1);
        return;
    }

    // Reload original texture from the base mesh data
    // The base mesh materials are accessible via vmesh->mesh (V3M/V3C mesh data)
    // For simplicity, setting to -1 effectively clears the override
    materials[slot].texture_maps[0].tex_handle = -1;
    xlog::info("[AlpineMesh] Cleared texture slot {} on obj handle {}", slot, obj->handle);
}
