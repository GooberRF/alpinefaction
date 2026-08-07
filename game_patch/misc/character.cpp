#include <unordered_map>
#include <memory>
#include <cstring>
#include <patch_common/FunHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/AsmWriter.h>
#include <common/utils/string-utils.h>
#include <common/utils/mem-pool.h>
#include <xlog/xlog.h>
#include "../rf/character.h"
#include "../rf/crt.h"
#include "../rf/entity.h"
#include "../rf/os/os.h"
#include "../rf/v3d.h"
#include "../rf/vmesh.h"

using SkeletonPool = MemPool<rf::Skeleton, 800>;
static SkeletonPool skeleton_pool;
static std::unordered_map<std::string, SkeletonPool::Pointer> skeletons;

static void skeleton_cleanup_one(rf::Skeleton* sp)
{
    if (sp->internally_allocated && sp->animation_data) {
        rf::rf_free(sp->animation_data);
        sp->animation_data = nullptr;
    }
}

static FunHook<rf::Skeleton*(const char*)> skeleton_find_hook{
    0x00539BE0,
    [](const char *filename) {
        std::string key = string_to_lower(get_filename_without_ext(filename));
        auto it = skeletons.find(key);
        if (it == skeletons.end()) {
            auto p = skeletons.insert({key, skeleton_pool.alloc()});
            it = p.first;
            auto& skeleton = it->second;
            std::strncpy(skeleton->mvf_filename, filename, std::size(skeleton->mvf_filename) - 1);
            skeleton->mvf_filename[std::size(skeleton->mvf_filename) - 1] = '\0';
            xlog::trace("Allocated skeleton: {} (total {})", filename, skeletons.size());
        }
        return it->second.get();
    },
};

static FunHook<void(rf::Skeleton*, bool)> skeleton_unlink_base_hook{
    0x00539D20,
    [](rf::Skeleton* sp, bool force_unload) {
        sp->base_usage_count = std::max(sp->base_usage_count - 1, 0);
        if (sp->base_usage_count == 0 || force_unload) {
            if (sp->base_usage_count != 0) {
                xlog::warn("Expected 0 base usages for skeleton '{}' but got {}", sp->mvf_filename, sp->base_usage_count);
            }
            xlog::trace("Unloading skeleton: {} (total {})", sp->mvf_filename, skeletons.size());
            skeleton_cleanup_one(sp);
            std::string key = string_to_lower(get_filename_without_ext(sp->mvf_filename));
            skeletons.erase(key);
            // Note: skeleton is deallocated here
        }
    },
};

static FunHook<void()> skeleton_init_hook{
    0x00539D90,
    []() {
        skeletons.reserve(800);
    },
};

static FunHook<void()> skeleton_close_hook{
    0x00539DB0,
    []() {
        for (auto& p : skeletons) {
            auto& skeleton = p.second;
            if (skeleton->base_usage_count > 0) {
                xlog::warn("Expected 0 base usages for skeleton '{}' but got {}",
                    skeleton->mvf_filename, skeleton->base_usage_count);
            }
            if (skeleton->instance_usage_count > 0) {
                xlog::warn("Expected 0 instance usages for skeleton '{}' but got {}",
                    skeleton->mvf_filename, skeleton->instance_usage_count);
            }
            skeleton_cleanup_one(skeleton.get());
        }
        skeletons.clear();
    },
};

static void skeleton_flush()
{
    auto it = skeletons.begin();
    while (it != skeletons.end()) {
        rf::Skeleton* sp = it->second.get();
        if (sp->base_usage_count == 0) {
            xlog::trace("Unloading unused skeleton '{}'", sp->mvf_filename);
            skeleton_cleanup_one(sp);
            it = skeletons.erase(it);
        }
        else {
            ++it;
        }
    }
};

static int __fastcall character_load_animation(rf::Character *this_, int, const char *anim_filename, bool is_state, [[maybe_unused]] bool unused)
{
    rf::Skeleton* sp = rf::skeleton_link_base(anim_filename);
    if (!sp) {
        xlog::error("Couldn't load skeleton {}!", anim_filename);
        return (this_->num_anims > 0 ? 0 : -1);
    }
    for (int i = 0; i < this_->num_anims; ++i) {
        if (this_->animations[i] == sp && this_->anim_is_state[i] == is_state) {
            // Unlink call is missing in original code (RF bug)
            rf::skeleton_unlink_base(sp, false);
            xlog::trace("Animation '{}' already used by character '{}' ({} base usages)", anim_filename, this_->name, sp->base_usage_count);
            return i;
        }
    }
    // Animation not found - check if we can add it
    if (this_->num_anims >= static_cast<int>(std::size(this_->animations))) {
        // Protect from buffer overflow
        xlog::error("Cannot add animation '{}' to character '{}' because there is no free slot!", anim_filename, this_->name);
        rf::skeleton_unlink_base(sp, false);
        return 0;
    }
    // tech_gren_attack is missing from base game files — skip silently
    if (std::string_view{anim_filename}.find("tech_gren_attack") != std::string_view::npos) {
        rf::skeleton_unlink_base(sp, false);
        return (this_->num_anims > 0 ? 0 : -1);
    }
    rf::skeleton_page_in(anim_filename, nullptr);
    if (!sp->animation_data) {
        xlog::warn("Cannot add animation '{}' to character '{}' because skeleton data failed to load", anim_filename, this_->name);
        rf::skeleton_unlink_base(sp, false);
        return (this_->num_anims > 0 ? 0 : -1);
    }
    // Add animation skeleton
    int anim_index = this_->num_anims++;
    this_->animations[anim_index] = sp;
    this_->anim_is_state[anim_index] = is_state;
    xlog::trace("Animation '{}' loaded for character '{}' ({} base usages) this {} sp {} anim_index {} is_state {}",
        anim_filename, this_->name, sp->base_usage_count, this_, sp, anim_index, is_state);
    return anim_index;
}

static FunHook character_load_animation_hook{0x0051CC10, character_load_animation};

// eos.v3c ships with its $prop_flag prop point missing the 180 degree yaw that every
// other stock character bakes in, so anything hung off it — the CTF flag, the Bagman
// bag, the jetpack — faces forward and clips through the character's back.
// Gated on both the filename and the geometry: the name keeps any other mesh out of
// reach, and the orientation test means a modified eos.v3c is left alone as long as
// it doesn't have the same bug.
constexpr const char* eos_prop_fix_mesh = "eos";
constexpr const char* eos_prop_fix_point = "$prop_flag";

// Prop point's local forward (+Z rotated by its quaternion). Stock characters land near
// -1 on Z, eos at +1, so the sign carries the whole decision with a wide margin.
static float prop_point_forward_z(const rf::Quaternion& q)
{
    return 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
}

// Pre-multiply by a 180 degree yaw about Y, i.e. (0,1,0,0) * q. Maps eos.v3c's
// quaternion exactly onto the value every other stock character carries.
static void prop_point_yaw_180(rf::Quaternion& q)
{
    const rf::Quaternion in = q;
    q.x = in.z;
    q.y = in.w;
    q.z = -in.x;
    q.w = -in.y;
}

// Correct every LOD of one lod mesh.
static void fix_prop_flag_in_lod_mesh(rf::VifLodMesh* lod_mesh, const char* mesh_name)
{
    if (!lod_mesh) return;
    // Note: Only LOD 0 is consulted by the engine's character prop point lookup,
    // but we correct every level so the data stays consistent.
    for (int lod = 0; lod < lod_mesh->num_levels && lod < 3; ++lod) {
        rf::VifMesh* mesh = lod_mesh->meshes[lod];
        if (!mesh || !mesh->prop_points) continue;
        for (int i = 0; i < mesh->num_prop_points; ++i) {
            rf::VifPropPoint& pp = mesh->prop_points[i];
            if (std::strcmp(pp.name, eos_prop_fix_point) != 0) continue;
            if (prop_point_forward_z(pp.orient) <= 0.0f) continue;
            prop_point_yaw_180(pp.orient);
            xlog::debug("Corrected reversed {} on '{}' (lod {})", eos_prop_fix_point, mesh_name, lod);
        }
    }
}

static void fix_eos_prop_flag(rf::VMesh* vmesh)
{
    if (!vmesh || rf::vmesh_get_type(vmesh) != rf::MESH_TYPE_CHARACTER) return;
    if (!string_iequals(get_filename_without_ext(vmesh->filename), eos_prop_fix_mesh)) return;

    auto* ci = static_cast<rf::CharacterInstance*>(vmesh->instance);
    if (!ci || !ci->base_character) return;
    rf::Character* cp = ci->base_character;
    if (cp->num_character_meshes < 1) return;

    rf::CharacterMesh& cm = cp->character_meshes[0];
    if (cm.mesh) {
        fix_prop_flag_in_lod_mesh(cm.mesh->vu, vmesh->filename);
    }
}

static FunHook<rf::Entity*(int, const char*, int, const rf::Vector3&, const rf::Matrix3&, int, int)>
entity_create_prop_fix_hook{
    0x00422360,
    [](int entity_type, const char* name, int parent_handle, const rf::Vector3& pos,
       const rf::Matrix3& orient, int create_flags, int mp_character) {
        rf::Entity* ep = entity_create_prop_fix_hook.call_target(
            entity_type, name, parent_handle, pos, orient, create_flags, mp_character);
        if (ep) {
            fix_eos_prop_flag(ep->vmesh);
        }
        return ep;
    },
};

static CodeInjection character_delete_character_injection{
    0x0051C981,
    [](auto& regs) {
        rf::Character* cp = regs.ebx;
        for (int i = 0; i < cp->num_anims; ++i) {
            rf::skeleton_unlink_base(cp->animations[i], false);
        }
        regs.eip = 0x0051CA28;
    },
};

static FunHook<void()> character_level_init_hook{
    0x0051D980,
    []() {
        character_level_init_hook.call_target();
        // Destroy unused skeletons (they may have been loaded by page in rfl chunk)
        skeleton_flush();
    },
};

void character_apply_patch()
{
    // do not load fast_anims value from registry
    AsmWriter{0x0050C24B}.nop(6);

    skeleton_find_hook.install();
    skeleton_unlink_base_hook.install();
    skeleton_init_hook.install();
    skeleton_close_hook.install();
    character_load_animation_hook.install();
    entity_create_prop_fix_hook.install();
    character_delete_character_injection.install();
    character_level_init_hook.install();
}
