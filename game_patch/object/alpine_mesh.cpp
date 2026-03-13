#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
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

// Dummy ClutterInfo as raw bytes - avoids calling rf::String/VArray constructors.
// All zeros is safe: String{0,nullptr} = empty, VArray{0,nullptr} = empty.
// Only scalar sentinel fields are written; String members are never used.
alignas(rf::ClutterInfo) static uint8_t g_dummy_clutter_info_buf[sizeof(rf::ClutterInfo)];

// Clutter linked list tail pointer (sentinel.prev)
static auto& clutter_list_tail = addr_as_ref<rf::Clutter*>(0x005C95F0);
// Clutter count
static auto& clutter_count = addr_as_ref<int>(0x005C9358);

static bool g_dummy_clutter_info_initialized = false;

// Per-mesh ClutterInfo objects allocated for "is clutter" meshes (need cleanup)
static std::vector<rf::ClutterInfo*> g_mesh_clutter_infos;

// Per-object corpse data (handle -> info). When an alpine mesh with a
// corpse filename is killed, the mesh swaps to the corpse model instead of being removed.
struct CorpseData {
    std::string filename;
    std::string state_anim;
    uint8_t collision = 2;
    int8_t material = -1; // -1=automatic (inherit from base), 0-9=specific material
};
static std::unordered_map<int, CorpseData> g_alpine_corpse_data;

// Set of object handles that have had corpse mesh applied. Used to prevent subsequent
// obj_flag_dead calls in the same death processing function from killing the corpse object.
static std::unordered_set<int> g_alpine_corpse_applied;

// Original texture handles saved before overrides are applied.
// Key: (obj_handle << 32 | slot), Value: original tex_handle.
// Used by alpine_mesh_clear_texture to restore the base mesh texture.
static std::unordered_map<uint64_t, int> g_original_tex_handles;

// Build a tex_handles map key from handle + slot, avoiding sign-extension
static uint64_t tex_key(int handle, int slot) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(handle)) << 32)
         | static_cast<uint32_t>(slot);
}
static uint64_t tex_key_handle(int handle) {
    return static_cast<uint64_t>(static_cast<uint32_t>(handle)) << 32;
}

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

    rf::File::ChunkGuard chunk_guard{file, remaining};

    bool read_error = false;

    auto read_bytes = [&](void* dst, std::size_t n) -> bool {
        if (remaining < n) { read_error = true; return false; }
        int got = file.read(dst, n);
        if (got != static_cast<int>(n) || file.error()) {
            if (got > 0) remaining -= got;
            read_error = true;
            return false;
        }
        remaining -= n;
        return true;
    };

    auto read_string = [&]() -> std::string {
        uint16_t len = 0;
        if (!read_bytes(&len, sizeof(len))) return "";
        if (len == 0) return "";
        std::string result(len, '\0');
        if (!read_bytes(result.data(), len)) return "";
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
        if (read_error) return;
        info.mesh_filename = read_string();
        if (read_error) return;
        info.state_anim = read_string();
        if (read_error) return;
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
            if (read_error) return;
            if (!tex.empty()) {
                info.texture_overrides.push_back({slot_id, std::move(tex)});
            }
        }

        int32_t mat = 0;

        // clutter properties
        if (remaining >= sizeof(int32_t) && read_bytes(&mat, sizeof(mat))) {
            info.material = (mat >= 0 && mat <= 9) ? mat : 0;

            uint8_t is_clutter_flag = 0;
            if (remaining >= 1 && read_bytes(&is_clutter_flag, sizeof(is_clutter_flag))) {
                info.clutter.is_clutter = (is_clutter_flag != 0);
                if (info.clutter.is_clutter) {
                    auto& cp = info.clutter;
                    if (!read_bytes(&cp.life, sizeof(float))) return;
                    cp.debris_filename = read_string();
                    if (read_error) return;
                    cp.explosion_vclip = read_string();
                    if (read_error) return;
                    if (!read_bytes(&cp.explosion_radius, sizeof(float))) return;
                    if (!read_bytes(&cp.debris_velocity, sizeof(float))) return;
                    for (int di = 0; di < 11; di++) {
                        if (!read_bytes(&cp.damage_type_factors[di], sizeof(float))) return;
                    }
                    // Corpse fields
                    if (remaining > 0 && !read_error) {
                        cp.corpse_filename = read_string();
                    }
                    if (remaining > 0 && !read_error) {
                        cp.corpse_state_anim = read_string();
                    }
                    if (remaining >= 1) {
                        uint8_t col = 0;
                        if (read_bytes(&col, sizeof(uint8_t))) {
                            cp.corpse_collision = col;
                        }
                    }
                    if (remaining >= 1) {
                        int8_t mat = -1;
                        if (read_bytes(&mat, sizeof(int8_t))) {
                            cp.corpse_material = mat;
                        }
                    }
                }
            }
        }

        // Create the game object immediately so it exists before the stock link
        // resolver runs. This lets the stock resolver convert event→mesh link UIDs
        // to handles automatically, just like any other object type.
        alpine_mesh_create_object(info);
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

    // Initialize dummy ClutterInfo once (memset + sentinel writes only —
    // String members stay as {0,nullptr} which is valid empty state)
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
    oci.material = info.material;
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

    auto* clutter = reinterpret_cast<rf::Clutter*>(obj);

    // Set up ClutterInfo — allocate per-mesh if clutter or non-default material,
    // otherwise use the shared dummy
    bool needs_own_info = info.clutter.is_clutter || info.material != 0;
    if (needs_own_info) {
        // Allocate a dedicated ClutterInfo — default constructor handles all members
        auto* ci = new rf::ClutterInfo{};

        // The stock clutter death function checks explode_anim_timer.elapsed()
        // before playing the explosion vclip. Timestamp's default value is -1
        // (invalid), which makes elapsed() return false — preventing vclip/debris
        // from ever firing. Set to 0 so elapsed() returns true on the first death frame.
        ci->explode_anim_timer.value = 0;

        ci->material = info.material;
        ci->sound = -1;
        ci->use_sound = -1;
        ci->glare = -1;
        ci->rod_glare = -1;
        ci->light_prop = -1;

        if (info.clutter.is_clutter) {
            ci->life = info.clutter.life;
            if (!info.clutter.debris_filename.empty()) {
                ci->debris_filename = info.clutter.debris_filename.c_str();
            }
            if (!info.clutter.explosion_vclip.empty()) {
                ci->explode_anim_vclip = rf::vclip_lookup(info.clutter.explosion_vclip.c_str());
            } else {
                ci->explode_anim_vclip = -1;
            }
            ci->explode_anim_radius = info.clutter.explosion_radius;
            ci->debris_velocity = info.clutter.debris_velocity;
            for (int di = 0; di < 11; di++) {
                ci->damage_type_factors[di] = info.clutter.damage_type_factors[di];
            }
        } else {
            ci->life = -1.0f;
            ci->explode_anim_vclip = -1;
        }

        clutter->info = ci;
        g_mesh_clutter_infos.push_back(ci);
    } else {
        auto* dummy_info = reinterpret_cast<rf::ClutterInfo*>(g_dummy_clutter_info_buf);
        clutter->info = dummy_info;
    }

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

    if (info.clutter.is_clutter) {
        // Destructible mesh: use life from clutter properties, not invulnerable
        obj->life = info.clutter.life;
        if (info.clutter.life < 0.0f) {
            // Negative life = invulnerable (matches stock clutter behavior)
            obj->obj_flags = static_cast<rf::ObjectFlags>(
                static_cast<int>(obj->obj_flags) | static_cast<int>(rf::OF_INVULNERABLE)
            );
        }
    } else {
        // Non-clutter mesh: invulnerable with default life
        obj->life = 100.0f;
        obj->obj_flags = static_cast<rf::ObjectFlags>(
            static_cast<int>(obj->obj_flags) | static_cast<int>(rf::OF_INVULNERABLE)
        );
    }

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
                auto key = tex_key(obj->handle, ovr.slot);
                if (g_original_tex_handles.find(key) == g_original_tex_handles.end()) {
                    g_original_tex_handles[key] = materials[ovr.slot].texture_maps[0].tex_handle;
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

    // Store corpse data if specified (for mesh swap on death)
    if (!info.clutter.corpse_filename.empty()) {
        g_alpine_corpse_data[obj->handle] = {
            info.clutter.corpse_filename,
            info.clutter.corpse_state_anim,
            info.clutter.corpse_collision,
            info.clutter.corpse_material
        };
    }

    g_alpine_mesh_handles.push_back(obj->handle);
    xlog::debug("[AlpineMesh] Created object: uid={} handle={} file='{}' pos=({:.2f},{:.2f},{:.2f})",
        info.uid, obj->handle, info.mesh_filename, obj->pos.x, obj->pos.y, obj->pos.z);
}

// ─── Per-Frame Animation Processing ─────────────────────────────────────────

void alpine_mesh_do_frame()
{
    for (auto it = g_mesh_anim_states.begin(); it != g_mesh_anim_states.end(); ) {
        rf::Object* obj = rf::obj_from_handle(it->obj_handle);
        if (!obj || !obj->vmesh || obj->vmesh->type != rf::MESH_TYPE_CHARACTER
            || !obj->vmesh->mesh || !obj->vmesh->instance) {
            it = g_mesh_anim_states.erase(it);
            continue;
        }

        // Wait a few frames after level load before starting animations
        // This ensures all subsystems are fully initialized
        if (it->startup_delay > 0) {
            it->startup_delay--;
            ++it;
            continue;
        }

        // Load the animation action with flag=1 (looping).
        // Looping actions use modular time (fmod) — the playback position wraps
        // automatically and the slot is never removed.
        if (it->action_index < 0) {
            it->action_index = rf::character_mesh_load_action(obj->vmesh->mesh, it->state_anim.c_str(), 1, 0);
            if (it->action_index < 0) {
                xlog::warn("[AlpineMesh] Failed to load animation '{}' for handle {}",
                    it->state_anim, it->obj_handle);
                it = g_mesh_anim_states.erase(it);
                continue;
            }
            xlog::debug("[AlpineMesh] Loaded animation '{}' action_index={} for handle {}",
                it->state_anim, it->action_index, it->obj_handle);
        }

        // Entity-style looping: each frame, zero all looping action weights then
        // set the desired action back to weight 1.0. This never resets the playback
        // position, so the animation loops seamlessly with no base pose flash.
        // NOTE: The stock clutter process (FUN_0040fe10) does NOT call vmesh_process,
        // so we must call it ourselves.
        rf::vmesh_reset_actions(obj->vmesh);
        rf::vmesh_set_action_weight(obj->vmesh, it->action_index, 1.0f);

        if (!it->anim_started) {
            it->anim_started = true;
            xlog::debug("[AlpineMesh] Started animation '{}' (action_index={}) on handle {}",
                it->state_anim, it->action_index, it->obj_handle);
        }

        // Advance animation — stock clutter process does NOT call vmesh_process
        rf::vmesh_process(obj->vmesh, rf::frametime, 0, &obj->pos, &obj->orient, 1);
        ++it;
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
    g_alpine_corpse_data.clear();
    g_alpine_corpse_applied.clear();
    g_original_tex_handles.clear();
    g_dummy_clutter_info_initialized = false;
    // Free per-mesh ClutterInfo objects
    for (auto* ci : g_mesh_clutter_infos) {
        delete ci;
    }
    g_mesh_clutter_infos.clear();
}

// ─── Corpse Mesh Support ─────────────────────────────────────────────────────

const CorpseData* alpine_mesh_get_corpse_data(int handle)
{
    auto it = g_alpine_corpse_data.find(handle);
    if (it != g_alpine_corpse_data.end()) {
        return &it->second;
    }
    return nullptr;
}

const std::string* alpine_mesh_get_corpse_filename(int handle)
{
    auto* data = alpine_mesh_get_corpse_data(handle);
    return data ? &data->filename : nullptr;
}

bool alpine_mesh_is_corpse(int handle)
{
    return g_alpine_corpse_applied.count(handle) > 0;
}

void alpine_mesh_apply_corpse(rf::Object* obj, const std::string& corpse_filename)
{
    // Copy corpse data before erasing
    CorpseData corpse_data;
    auto* data = alpine_mesh_get_corpse_data(obj->handle);
    if (data) corpse_data = *data;

    g_alpine_corpse_data.erase(obj->handle);

    // Remove from animation state lists
    g_mesh_anim_states.erase(
        std::remove_if(g_mesh_anim_states.begin(), g_mesh_anim_states.end(),
            [&](const AlpineMeshAnimState& a) { return a.obj_handle == obj->handle; }),
        g_mesh_anim_states.end());
    g_event_animated_meshes.erase(
        std::remove_if(g_event_animated_meshes.begin(), g_event_animated_meshes.end(),
            [&](const EventAnimatedMesh& e) { return e.obj_handle == obj->handle; }),
        g_event_animated_meshes.end());

    // Clear stale original-texture entries for this object before destroying the old vmesh
    for (auto it = g_original_tex_handles.begin(); it != g_original_tex_handles.end(); ) {
        if ((it->first >> 32) == static_cast<uint64_t>(static_cast<uint32_t>(obj->handle)))
            it = g_original_tex_handles.erase(it);
        else
            ++it;
    }

    // Delete old vmesh
    rf::obj_delete_mesh(obj);

    // Load corpse mesh with type determined by extension
    auto vtype = determine_vmesh_type(corpse_filename);
    rf::VMesh* new_mesh = rf::obj_create_mesh(obj, corpse_filename.c_str(), vtype);

    if (!new_mesh) {
        xlog::warn("[AlpineMesh] Corpse mesh '{}' failed to load for handle {}", corpse_filename, obj->handle);
        return;
    }

    // Mark as corpsed — prevents subsequent obj_flag_dead calls in the same
    // death processing function from killing this object
    g_alpine_corpse_applied.insert(obj->handle);

    // Make invulnerable with positive life so the stock clutter process doesn't
    // re-enter the death handler each frame (it checks life <= 0)
    obj->life = 1.0f;
    obj->obj_flags = static_cast<rf::ObjectFlags>(
        static_cast<int>(obj->obj_flags) | static_cast<int>(rf::OF_INVULNERABLE)
    );

    // Clear replacement materials (texture overrides) — corpse uses its own textures
    if (obj->vmesh) {
        obj->vmesh->use_replacement_materials = 0;
        obj->vmesh->replacement_materials = nullptr;
    }

    // Apply corpse collision mode
    alpine_mesh_set_collision(obj, corpse_data.collision);

    // Apply corpse material if not automatic (-1 means inherit from base mesh)
    if (corpse_data.material >= 0 && corpse_data.material <= 9) {
        obj->material = corpse_data.material;
    }

    // Start corpse state anim if specified (v3c only)
    if (!corpse_data.state_anim.empty() && vtype == rf::MESH_TYPE_CHARACTER) {
        // Use state anim type (2) with full weight
        alpine_mesh_animate(obj, 2, corpse_data.state_anim, 1.0f);
    }

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

    auto key = tex_key(obj->handle, slot);
    if (g_original_tex_handles.find(key) == g_original_tex_handles.end()) {
        g_original_tex_handles[key] = materials[slot].texture_maps[0].tex_handle;
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

    auto key = tex_key(obj->handle, slot);
    auto it = g_original_tex_handles.find(key);
    if (it == g_original_tex_handles.end()) {
        // No override was applied to this slot — nothing to restore
        return;
    }

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

    materials[slot].texture_maps[0].tex_handle = it->second;
    g_original_tex_handles.erase(it);
    xlog::debug("[AlpineMesh] Restored original texture on slot {} for obj handle {}", slot, obj->handle);
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

    // Deregister existing collision pairs and clear flags
    rf::obj_collision_deregister(obj);
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
