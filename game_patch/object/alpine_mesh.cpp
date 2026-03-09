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
#include <common/utils/string-utils.h>

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

static bool g_dummy_clutter_info_initialized = false;

// ─── VMesh Type Detection ───────────────────────────────────────────────────

static rf::VMeshType determine_vmesh_type(const std::string& filename)
{
    auto ext = get_ext_from_filename(filename);
    if (string_iequals(ext, "v3c")) return rf::MESH_TYPE_CHARACTER;
    if (string_iequals(ext, "vfx")) return rf::MESH_TYPE_ANIM_FX;
    return rf::MESH_TYPE_STATIC;
}

// Forward declaration
static void alpine_mesh_create_object(const AlpineMeshInfo& info);

// ─── Chunk Loading ──────────────────────────────────────────────────────────

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

    uint32_t count = 0;
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
        // texture overrides: count + (slot_id, filename) pairs
        uint8_t num_overrides = 0;
        if (!read_bytes(&num_overrides, sizeof(num_overrides))) return;
        for (uint8_t oi = 0; oi < num_overrides; oi++) {
            uint8_t slot_id = 0;
            if (!read_bytes(&slot_id, sizeof(slot_id))) return;
            std::string tex = read_string();
            if (!tex.empty()) {
                info.texture_overrides.push_back({slot_id, std::move(tex)});
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

}

// ─── Material Helpers ────────────────────────────────────────────────────────

// Helper to get replacement materials array with multi-LOD V3M workaround.
// V3M meshes with multiple LODs/sub-mesh groups cause the engine's replacement
// materials allocator to bail early. The render code applies replacement materials
// to ALL LODs from a single set, so we temporarily fake single-LOD/single-submesh
// counts during allocation.
// V3M instance offsets: +0x50 = lod_count (int), +0x54 = submesh_list (int**)
static bool get_replacement_materials(rf::VMesh* vmesh, int& num_materials, rf::MeshMaterial*& materials)
{
    num_materials = 0;
    materials = nullptr;
    rf::vmesh_get_materials_array(vmesh, &num_materials, &materials);

    if ((!materials || num_materials <= 0) && vmesh->type == rf::MESH_TYPE_STATIC) {
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

// ─── Object Creation ────────────────────────────────────────────────────────

static bool vmesh_play_v3c_action_by_name(rf::VMesh* vmesh, const char* action_name)
{
    if (!vmesh || !action_name || action_name[0] == '\0') return false;
    if (vmesh->type != rf::MESH_TYPE_CHARACTER) return false;
    if (!vmesh->mesh || !vmesh->instance) {
        xlog::warn("[AlpineMesh] Cannot play animation '{}': mesh={:p} instance={:p}",
            action_name, vmesh->mesh, vmesh->instance);
        return false;
    }

    // Load the .rfa/.mvf animation file onto the character mesh_data
    int action_index = rf::character_mesh_load_action(vmesh->mesh, action_name, 0, 0);
    if (action_index < 0) {
        xlog::warn("[AlpineMesh] Failed to load animation '{}' on vmesh", action_name);
        return false;
    }

    // Play the loaded action (transition_time must be > 0 or play_action is a no-op)
    rf::vmesh_play_action_by_index(vmesh, action_index, 0.001f, 0);
    return true;
}

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

    rf::Object* obj = rf::obj_create(rf::OT_CLUTTER, -1, 0, &oci, 0, nullptr);
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

        rf::obj_collision_register(obj);
    }

    // Apply texture overrides
    if (obj->vmesh && !info.texture_overrides.empty()) {
        int num_materials = 0;
        rf::MeshMaterial* materials = nullptr;
        if (get_replacement_materials(obj->vmesh, num_materials, materials)) {
            for (const auto& ovr : info.texture_overrides) {
                if (ovr.slot >= num_materials) {
                    xlog::warn("[AlpineMesh] Texture override slot {} exceeds material count {}", ovr.slot, num_materials);
                    continue;
                }
                int bm_handle = rf::bm::load(ovr.filename.c_str(), -1, true);
                if (bm_handle < 0) {
                    xlog::warn("[AlpineMesh] Failed to load texture '{}' for slot {}",
                        ovr.filename, ovr.slot);
                    continue;
                }
                materials[ovr.slot].texture_maps[0].tex_handle = bm_handle;
                xlog::debug("[AlpineMesh] Applied texture override slot {}: '{}' (handle={})",
                    ovr.slot, ovr.filename, bm_handle);
            }
        } else {
            xlog::warn("[AlpineMesh] Failed to allocate replacement materials for uid={}", info.uid);
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
    xlog::debug("[AlpineMesh] Created object: uid={} handle={} file='{}' pos=({:.2f},{:.2f},{:.2f})",
        info.uid, obj->handle, info.mesh_filename, obj->pos.x, obj->pos.y, obj->pos.z);
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
            anim.action_index = rf::character_mesh_load_action(obj->vmesh->mesh, anim.state_anim.c_str(), 1, 0);
            if (anim.action_index < 0) {
                xlog::warn("[AlpineMesh] Failed to load animation '{}' for handle {}",
                    anim.state_anim, anim.obj_handle);
                anim.anim_started = true;
                continue;
            }
            xlog::debug("[AlpineMesh] Loaded animation '{}' action_index={} for handle {}",
                anim.state_anim, anim.action_index, anim.obj_handle);
        }

        // Entity-style looping: each frame, zero all looping action weights then
        // set the desired action back to weight 1.0. This never resets the playback
        // position, so the animation loops seamlessly with no base pose flash.
        // NOTE: The stock clutter process (FUN_0040fe10) does NOT call vmesh_process,
        // so we must call it ourselves.
        rf::vmesh_reset_actions(obj->vmesh);
        rf::vmesh_set_action_weight(obj->vmesh, anim.action_index, 1.0f);

        if (!anim.anim_started) {
            anim.anim_started = true;
            xlog::debug("[AlpineMesh] Started animation '{}' (action_index={}) on handle {}",
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
            rf::vmesh_reset_actions(obj->vmesh);
            rf::vmesh_set_action_weight(obj->vmesh, it->action_index, it->blend_weight);
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
    g_alpine_mesh_handles.clear();
    g_mesh_anim_states.clear();
    g_event_animated_meshes.clear();
    g_dummy_clutter_info_initialized = false;
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

    // Clear all existing action slots (looping AND one-shot/hold-last) so previous
    // animations don't interfere. Without this, a held action's weight persists and
    // blocks new animations from being visible.
    rf::vmesh_stop_all_actions(obj->vmesh);

    // type 0 = Action: play once, then return to state_anim
    // type 1 = Action Hold Last: play once, freeze on last frame
    // type 2 = State: loop, override state_anim

    if (type == 0) {
        // Action: one-shot that returns to the state_anim after completion.
        // Load as one-shot (flag=0), play with hold_last_frame=0 so the engine
        // auto-removes the action when it finishes. The state_anim continues
        // running underneath via g_mesh_anim_states, so vmesh_process is already
        // being called each frame — no need to add to g_event_animated_meshes.
        int action_index = rf::character_mesh_load_action(obj->vmesh->mesh, anim_filename.c_str(), 0, 0);
        if (action_index < 0) {
            xlog::warn("[AlpineMesh] Failed to load animation '{}' on handle {}", anim_filename, obj->handle);
            return;
        }

        rf::vmesh_set_action_weight(obj->vmesh, action_index, blend_weight);
        rf::vmesh_play_action_by_index(obj->vmesh, action_index, 0.001f, 0);

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

        xlog::debug("[AlpineMesh] Playing animation '{}' (type=Action, action_index={}, weight={:.2f}) on handle {}",
            anim_filename, action_index, blend_weight, obj->handle);
    }
    else if (type == 1) {
        // Action Hold Last: one-shot that freezes on the last frame permanently.
        // Load as one-shot (flag=0), play with hold_last_frame=1.
        int action_index = rf::character_mesh_load_action(obj->vmesh->mesh, anim_filename.c_str(), 0, 0);
        if (action_index < 0) {
            xlog::warn("[AlpineMesh] Failed to load animation '{}' on handle {}", anim_filename, obj->handle);
            return;
        }

        rf::vmesh_set_action_weight(obj->vmesh, action_index, blend_weight);
        rf::vmesh_play_action_by_index(obj->vmesh, action_index, 0.001f, 1);

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

        xlog::debug("[AlpineMesh] Playing animation '{}' (type=Action Hold Last, action_index={}, weight={:.2f}) on handle {}",
            anim_filename, action_index, blend_weight, obj->handle);
    }
    else if (type == 2) {
        // State: looping animation that overrides the state_anim.
        // Load as looping (flag=1), manage weight per-frame.
        int action_index = rf::character_mesh_load_action(obj->vmesh->mesh, anim_filename.c_str(), 1, 0);
        if (action_index < 0) {
            xlog::warn("[AlpineMesh] Failed to load animation '{}' on handle {}", anim_filename, obj->handle);
            return;
        }

        rf::vmesh_reset_actions(obj->vmesh);
        rf::vmesh_set_action_weight(obj->vmesh, action_index, blend_weight);

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

        xlog::debug("[AlpineMesh] Playing animation '{}' (type=State, action_index={}, weight={:.2f}) on handle {}",
            anim_filename, action_index, blend_weight, obj->handle);
    }
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
    xlog::debug("[AlpineMesh] Set texture slot {} to '{}' (handle={}) on obj handle {}",
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
    xlog::debug("[AlpineMesh] Cleared texture slot {} on obj handle {}", slot, obj->handle);
}

void alpine_mesh_set_collision(rf::Object* obj, int collision_type)
{
    if (!obj || obj->type != rf::OT_CLUTTER) {
        return;
    }

    // Clamp to valid range
    if (collision_type < 0 || collision_type > 2) {
        xlog::warn("[AlpineMesh] set_collision: invalid type {} (expected 0-2)", collision_type);
        return;
    }

    // Clear existing collision flags
    obj->obj_flags = static_cast<rf::ObjectFlags>(
        static_cast<int>(obj->obj_flags) & ~static_cast<int>(rf::OF_WEAPON_ONLY_COLLIDE)
    );
    obj->p_data.flags &= ~rf::PF_COLLIDE_OBJECTS;

    if (collision_type > 0) {
        // Enable collision
        obj->p_data.flags |= rf::PF_COLLIDE_OBJECTS;

        // Set up collision sphere if not already present
        if (obj->p_data.cspheres.size() == 0) {
            float r = obj->radius;
            obj->p_data.radius = r;
            obj->p_data.mass = 10000.0f;
            rf::PCollisionSphere sphere{};
            sphere.center = {0.0f, 0.0f, 0.0f};
            sphere.radius = r;
            obj->p_data.cspheres.add(sphere);
            obj->p_data.bbox_min = {obj->pos.x - r, obj->pos.y - r, obj->pos.z - r};
            obj->p_data.bbox_max = {obj->pos.x + r, obj->pos.y + r, obj->pos.z + r};
        }

        if (collision_type == 1) {
            obj->obj_flags = static_cast<rf::ObjectFlags>(
                static_cast<int>(obj->obj_flags) | static_cast<int>(rf::OF_WEAPON_ONLY_COLLIDE)
            );
        }

        rf::obj_collision_register(obj);
    }

    xlog::debug("[AlpineMesh] Set collision type {} on obj handle {}", collision_type, obj->handle);
}
