#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>
#include <xlog/xlog.h>
#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/FunHook.h>
#include <common/utils/list-utils.h>
#include <common/utils/string-utils.h>
#include "vehicle.h"
#include "vehicle_physics.h"
#include "vehicle_internal.h"
#include "vehicle_render.h"
#include "../alpine_packets.h"
#include "../bagman.h"
#include "../gametype.h"
#include "../../rf/file/file.h"
#include "../../hud/multi_spectate.h"
#include "../../misc/level.h"
#include "../../rf/ai.h"
#include "../../rf/bmpman.h"
#include "../../rf/entity.h"
#include "../../rf/geometry.h"
#include "../../rf/gr/gr.h"
#include "../../rf/item.h"
#include "../../rf/level.h"
#include "../../rf/multi.h"
#include "../../rf/object.h"
#include "../../rf/os/frametime.h"
#include "../../rf/physics.h"
#include "../../rf/player/camera.h"
#include "../../rf/player/player.h"
#include "../../rf/v3d.h"
#include "../../rf/vmesh.h"
#include "../../rf/weapon.h"

namespace
{
    // The room walk only ENQUEUES, so OF_WAS_RENDERED dedupes across passes and never within one.
    int g_vehicle_render_pass = 0;

    FunHook<void()> obj_clear_render_flags_hook{
        0x00488200,
        []() {
            obj_clear_render_flags_hook.call_target();
            ++g_vehicle_render_pass;
        },
    };

    // Is `detail` a detail room the renderer draws as part of `rendered`? Guarded as gr_d3d11_solid
    // guards it: detail_rooms may be corrupt on a detail room.
    bool vehicle_room_renders_detail_room(rf::GRoom* rendered, rf::GRoom* detail)
    {
        if (!rendered || !detail || !detail->is_detail || rendered->is_detail) {
            return false;
        }
        if (detail->room_to_render_with == rendered) {
            return true;
        }
        for (rf::GRoom* child : rendered->detail_rooms) {
            if (child == detail) {
                return true;
            }
        }
        return false;
    }

    // No room walk ever submits a detail room, so a hull whose room is one is otherwise permanently
    // invisible. The ledger is this override's own within-pass dedupe.
    FunHook<char(rf::Object*, rf::GRoom*, char)> obj_render_room_gate_hook{
        0x00488D10,
        [](rf::Object* objp, rf::GRoom* room, char allow_rider_override) -> char {
            const char stock = obj_render_room_gate_hook.call_target(objp, room, allow_rider_override);
            if (!rf::is_multi || !objp || objp->type != rf::OT_ENTITY || !room) {
                return stock;
            }
            auto* ep = static_cast<rf::Entity*>(objp);
            if (!vehicle_is_synced_entity_type(ep)) {
                return stock;
            }
            static int ledger_pass = -1;
            static std::vector<int> submitted_this_pass;
            if (ledger_pass != g_vehicle_render_pass) {
                ledger_pass = g_vehicle_render_pass;
                submitted_this_pass.clear();
            }
            const bool already = std::find(submitted_this_pass.begin(), submitted_this_pass.end(),
                                           ep->handle)
                != submitted_this_pass.end();
            if (stock) {
                if (!already) {
                    submitted_this_pass.push_back(ep->handle);
                }
                return stock;
            }
            if (already || (ep->obj_flags & (rf::OF_DELAYED_DELETE | rf::OF_HIDDEN))) {
                return stock;
            }
            const bool room_match = ep->room == room;
            if (!room_match && !vehicle_room_renders_detail_room(room, ep->room)) {
                return stock;
            }
            submitted_this_pass.push_back(ep->handle);
            return 1;
        },
    };

    // Route every synced vehicle through the gate above, which stock reaches for drillers only.
    // Per-site, so entity_is_driller's real answer (the drilling logic) is untouched.
    CallHook<bool(rf::Entity*)> obj_mark_room_driller_route_hook{
        0x0048851F,
        [](rf::Entity* ep) -> bool {
            if (rf::is_multi && vehicle_is_synced_entity_type(ep)) {
                return true;
            }
            return obj_mark_room_driller_route_hook.call_target(ep);
        },
    };

    // The mesh draw path is handed mesh + pose only, so the owner of a hull's belts is recorded
    // here and read back by vehicle_tread_scroll_for_draw. Saved/restored, not just cleared: the
    // attachment draws inside this call may render further entities.
    rf::Entity* g_rendering_entity = nullptr;

    FunHook<void(rf::Entity*)> entity_render_hook{
        0x00421850,
        [](rf::Entity* ep) {
            rf::Entity* prev = g_rendering_entity;
            g_rendering_entity = ep;
            entity_render_hook.call_target(ep);
            g_rendering_entity = prev;
        },
    };
} // namespace

rf::Entity* vehicle_rendering_entity()
{
    return g_rendering_entity;
}

rf::Entity* vehicle_outline_occupant(rf::Entity* vehicle)
{
    if (!vehicle_is_synced_entity_type(vehicle)) {
        return nullptr;
    }
    const rf::Player* bag_carrier = gt_is_bagman_any() ? g_bagman_info.carrier : nullptr;
    rf::Entity* first = nullptr;
    for (int i = 0; i < vehicle->interface_points.size(); ++i) {
        rf::Entity* occupant = rf::entity_from_handle(vehicle_seat_leech(vehicle, i));
        if (!occupant) {
            continue;
        }
        if (!bag_carrier) {
            return occupant;
        }
        if (occupant->handle == bag_carrier->entity_handle) {
            return occupant;
        }
        if (!first) {
            first = occupant;
        }
    }
    return first;
}

namespace
{
    constexpr int vehicle_team_tex_slots = std::extent_v<decltype(rf::VifMesh::tex_ids)>;

    struct VehicleTeamTexSet
    {
        std::vector<int> handles; // indexed by V3d MATERIAL index, which is what alt_tex is
        bool any_variant = false;
    };

    // Keyed per V3dMesh so each hull sub-mesh and the jeep gun get their own array. Holds bm
    // handles, so it dies with the level in vehicle_drop_jeep_tire_mesh.
    std::map<std::pair<const rf::V3dMesh*, int>, VehicleTeamTexSet> g_team_tex_cache;

    // No refcount, and the only bitmap sweep (0x0050ED60) is bm_init's atexit, not level unload.
    std::vector<int> g_team_tex_loaded;

    // The engine's own rule (pc_multi.tbl skin cloning, 0x00475B00): drop the extension, then add
    // "r"/"b" when the base carries a mip marker and "_red"/"_blue" otherwise.
    std::string vehicle_team_variant_name(std::string_view filename, int team)
    {
        std::string base{get_filename_without_ext(filename)};
        const bool mip = base.find("-mip") != std::string::npos || base.find("_mip") != std::string::npos;
        if (mip) {
            base += team == 0 ? "r" : "b";
        }
        else {
            base += team == 0 ? "_red" : "_blue";
        }
        base += ".tga";
        return base;
    }

    // Every index the engine will read out of the array: 0x0052FA40 fetches alt_tex[tex_ids[i]] for
    // each LOD, so a malformed tex_id must still land inside it.
    int vehicle_team_tex_array_size(const rf::V3dMesh* mesh)
    {
        int size = std::clamp(mesh->num_materials, 0, 255);
        if (const rf::VifLodMesh* lod = mesh->vu) {
            for (int l = 0; l < lod->num_levels && l < 3; ++l) {
                const rf::VifMesh* m = lod->meshes[l];
                if (!m) {
                    continue;
                }
                const int n = std::clamp(m->num_textures_handles, 0, vehicle_team_tex_slots);
                for (int i = 0; i < n; ++i) {
                    size = std::max(size, static_cast<int>(m->tex_ids[i]) + 1);
                }
            }
        }
        return size;
    }

    // Defined with the tread rows below.
    bool vehicle_bitmap_is_tread_belt(int bm_handle);

    // Null when nothing resolved: every probe result is cached, so a mesh with no team art costs
    // one File::find sweep for the level and a map lookup per draw after that.
    VehicleTeamTexSet* vehicle_team_tex_set(const rf::V3dMesh* mesh, int team)
    {
        const auto [it, inserted] = g_team_tex_cache.try_emplace({mesh, team});
        VehicleTeamTexSet& set = it->second;
        if (!inserted) {
            return set.any_variant ? &set : nullptr;
        }
        const int size = vehicle_team_tex_array_size(mesh);
        if (size <= 0 || !mesh->materials) {
            return nullptr;
        }
        set.handles.assign(static_cast<std::size_t>(size), -1);
        const auto* materials = reinterpret_cast<const rf::MeshMaterial*>(mesh->materials);
        const int num_materials = std::min(mesh->num_materials, size);
        for (int j = 0; j < num_materials; ++j) {
            const int base = materials[j].texture_maps[0].tex_handle;
            set.handles[j] = base;
            // A belt is shared art with no team variant, and the scroll latches its stock handle.
            if (vehicle_bitmap_is_tread_belt(base)) {
                continue;
            }
            const char* filename = base >= 0 ? rf::bm::get_filename(base) : nullptr;
            if (!filename) {
                continue;
            }
            const std::string variant = vehicle_team_variant_name(filename, team);
            if (!rf::File{}.find(variant.c_str())) {
                continue;
            }
            // Before the load: an already-resident name is the level's or a skin's, never ours to free.
            const bool was_resident = rf::bm::find_by_filename(variant.c_str()) >= 0;
            const int handle = rf::bm::load(variant.c_str(), -1, true);
            if (handle < 0) {
                continue;
            }
            set.handles[j] = handle;
            set.any_variant = true;
            // One entry per HANDLE, not per set: two sets can name one bitmap.
            if (!was_resident
                && std::find(g_team_tex_loaded.begin(), g_team_tex_loaded.end(), handle)
                       == g_team_tex_loaded.end()) {
                g_team_tex_loaded.push_back(handle);
            }
        }
        if (!set.any_variant) {
            set.handles.clear();
            set.handles.shrink_to_fit();
            return nullptr;
        }
        return &set;
    }

    void vehicle_team_tex_cache_clear()
    {
        for (const int handle : g_team_tex_loaded) {
            // bm::release does not evict the D3D11 slot-keyed texture cache, so the GPU texture
            // would leak and a reused cache slot would inherit it.
            rf::gr::mark_texture_dirty(handle);
            rf::bm::release(handle);
        }
        g_team_tex_loaded.clear();
        g_team_tex_cache.clear();
    }

    // Who the hull reads as, client-side: its seat-0 rider, else the stored affiliation (which boarding
    // sets to the boarder's team). -1 keeps stock art.
    int vehicle_effective_team(rf::Entity* vehicle)
    {
        if (!multi_is_team_game_type()) {
            return -1;
        }
        if (const rf::Player* pp = vehicle_seat_occupant_player(vehicle, 0)) {
            return pp->team == 1 ? 1 : 0;
        }
        const int team = vehicle_hull_team(vehicle->handle);
        return team == 0 || team == 1 ? team : -1;
    }

    // Defined with the spinner table below; the team-texture patch has to ask about it first.
    bool vehicle_is_spinner_mesh(const rf::VMesh* vmesh);

    // Per SUB-MESH, inside the static-mesh branch of vmesh_render's body (0x00502B20): EAX holds
    // &V3d::meshes[i] and [ESP] the MeshRenderParams the draw is about to take. Both renderers read
    // alt_tex (+0x10) off that struct, so this one site covers D3D9 and D3D11, and it covers the
    // hull and the jeep's gun alike - both draw inside the hull's entity_render.
    CodeInjection vmesh_render_static_team_tex_patch{
        0x00502FA2,
        [](auto& regs) {
            rf::Entity* vehicle = g_rendering_entity;
            if (!rf::is_multi || !vehicle_is_synced_entity_type(vehicle)) {
                return;
            }
            auto* params = *static_cast<rf::MeshRenderParams**>(regs.esp);
            auto* vmesh = reinterpret_cast<const rf::VMesh*>(regs.ebx.value);
            if (!params || !vmesh) {
                return;
            }
            // 0x0052FA40 writes its own remapped array BACK into this params, so from the second
            // sub-mesh on, alt_tex is the previous one's - the caller's own value survives only at
            // ESI 0, the first iteration of the loop this site sits in. Captured there so a
            // sub-mesh with no team art can be handed the CALLER's array back rather than null.
            static rf::MeshRenderParams* caller_params = nullptr;
            static int* caller_alt_tex = nullptr;
            if (regs.esi.value == 0 || params != caller_params) {
                caller_params = params;
                caller_alt_tex = params->alt_tex;
            }
            // The engine's own skin array wins, and it re-asserts it per sub-mesh two instructions
            // above. Read the flag, never params->alt_tex: see the assignment below.
            if (vmesh->use_replacement_materials) {
                return;
            }
            // Tires and propellers have no team art; they still must be ASSIGNED, see below.
            if (vehicle_is_spinner_mesh(vmesh)) {
                params->alt_tex = caller_alt_tex;
                return;
            }
            const int team = vehicle_effective_team(vehicle);
            const auto* mesh = reinterpret_cast<const rf::V3dMesh*>(regs.eax.value);
            VehicleTeamTexSet* set = team >= 0 && mesh ? vehicle_team_tex_set(mesh, team) : nullptr;
            // Every sub-mesh must be assigned, not just coloured ones.
            params->alt_tex = set ? set->handles.data() : caller_alt_tex;
        },
    };

    struct VehicleSpinnerPropName
    {
        const char* name;
        bool is_front;
        bool is_right;
    };
    constexpr VehicleSpinnerPropName jeep_wheel_prop_names[4] = {
        {"wheel_fl", true, false},
        {"wheel_fr", true, true},
        {"wheel_rl", false, false},
        {"wheel_rr", false, true},
    };
    constexpr VehicleSpinnerPropName sub_screw_prop_names[2] = {
        {"wheel_l", false, false},
        {"wheel_r", false, true},
    };

    // A hull class whose named prop points each carry one instance of a shared mesh, spun from the
    // hull's forward travel.
    struct VehicleSpinnerConfig
    {
        int vehicle_class;
        const char* mesh_filename;
        const VehicleSpinnerPropName* props;
        int num_props;
        // The TIRE MESH's own rolling radius (the mesh is drawn unscaled), not the suspension spring
        // sphere's 0.750. Half the vertex span across the axle, if the mesh is ever re-authored.
        float rolling_radius;    // >0: radians per world unit travelled forward is 1/radius
        float revs_at_max_speed; // >0 instead: revolutions per second at the class speed cap
        bool spin_about_shaft;   // the screw's disc is normal to the tag's +Z, the tire's to its +X
        bool use_prop_orient;    // false ignores the tag's own rotation: see the jeep note below
        bool steer_front;
        bool yaw_right_180;      // the tire mesh is a LEFT wheel, faced outboard on the right side
        bool negate_right;       // right-side instances spin the other way, as twin screws do
    };

    constexpr VehicleSpinnerConfig vehicle_spinner_configs[] = {
        {VDC_JEEP, "af_Jeep01T.v3m", jeep_wheel_prop_names, 4, 0.52735f, 0.0f, false, false, true,
         true, true},
        {VDC_SUB, "af_Sub_Mini01T.v3m", sub_screw_prop_names, 2, 0.0f, 2.0f, true, true, false,
         false, true},
    };

    constexpr int vehicle_spinner_config_count =
        static_cast<int>(std::size(vehicle_spinner_configs));
    constexpr int vehicle_spinner_max_props = 4;

    // Dropped on every level init: the engine frees its static meshes with the level.
    struct VehicleSpinnerMesh
    {
        rf::VMesh* mesh = nullptr;
        bool load_attempted = false;
    };
    VehicleSpinnerMesh g_spinner_mesh[vehicle_spinner_config_count];

    // One instance of a row's mesh, in HULL-LOCAL space.
    struct VehicleSpinnerPlacement
    {
        rf::Vector3 center;
        rf::Matrix3 orient;
        bool is_front;
        bool is_right;
    };

    // A cheap same-level key only - VMesh::instance is the V3d pointer, not a generation counter -
    // so correctness rests on vehicle_drop_jeep_tire_mesh dropping the entry with the meshes.
    struct VehicleSpinnerPropCache
    {
        const rf::VMesh* vmesh = nullptr;
        const void* instance = nullptr;
        int index[vehicle_spinner_max_props] = {-1, -1, -1, -1};
        bool complete = false;
    };
    VehicleSpinnerPropCache g_spinner_prop_cache[vehicle_spinner_config_count];

    bool vehicle_is_spinner_mesh(const rf::VMesh* vmesh)
    {
        if (!vmesh) {
            return false;
        }
        for (const VehicleSpinnerMesh& m : g_spinner_mesh) {
            if (m.mesh == vmesh) {
                return true;
            }
        }
        return false;
    }

    int vehicle_spinner_config_for_class(int vdc)
    {
        for (int cfg = 0; cfg < vehicle_spinner_config_count; ++cfg) {
            if (vehicle_spinner_configs[cfg].vehicle_class == vdc) {
                return cfg;
            }
        }
        return -1;
    }

    // Which spinner row this hull is, or -1 for a class with none. The same gate the hull mesh
    // override is on: a hull on a level the override skipped has its wheels baked in.
    int vehicle_spinner_config_for_entity(const rf::Entity* ep)
    {
        if (!ep || !rf::is_multi || !vehicle_level_has_factories()) {
            return -1;
        }
        const int vdc = vehicle_damage_class(ep);
        return vdc < 0 ? -1 : vehicle_spinner_config_for_class(vdc);
    }

    const VehicleSpinnerPropCache& vehicle_spinner_prop_cache(int cfg, rf::VMesh* vmesh)
    {
        VehicleSpinnerPropCache& cache = g_spinner_prop_cache[cfg];
        if (cache.vmesh == vmesh && cache.instance == vmesh->instance) {
            return cache;
        }
        const VehicleSpinnerConfig& c = vehicle_spinner_configs[cfg];
        cache.vmesh = vmesh;
        cache.instance = vmesh->instance;
        cache.complete = true;
        for (int i = 0; i < c.num_props; ++i) {
            cache.index[i] = rf::vmesh_lookup_prop_point(vmesh, c.props[i].name);
            if (cache.index[i] < 0) {
                cache.complete = false;
            }
        }
        return cache;
    }

    // The row's named prop points are the ONLY source: a mesh without them has nothing to draw.
    // af_Jeep01's wheel helpers carry a 90-degree-about-Z quaternion that would lay every tire on
    // its side, so that row takes the position alone; af_Sub_Mini01's screw tags are identity.
    int vehicle_spinner_placements(int cfg, const rf::Entity* ep, VehicleSpinnerPlacement* out)
    {
        if (!ep || !ep->vmesh) {
            return 0;
        }
        const VehicleSpinnerPropCache& cache = vehicle_spinner_prop_cache(cfg, ep->vmesh);
        if (!cache.complete) {
            return 0;
        }
        const VehicleSpinnerConfig& c = vehicle_spinner_configs[cfg];
        // Identity frame in, so the transform comes back in HULL-LOCAL space. The position is used
        // RAW: a conforming mesh authors prop points in the space vmesh_render lands geometry in.
        for (int i = 0; i < c.num_props; ++i) {
            rf::Vector3 pos{};
            rf::Matrix3 orient{};
            rf::Vector3 origin{};
            rf::vmesh_get_prop_point_transform(ep->vmesh, cache.index[i], &rf::identity_matrix,
                                               &origin, &orient, &pos);
            out[i] = {pos, orient, c.props[i].is_front, c.props[i].is_right};
        }
        return c.num_props;
    }

    float vehicle_spinner_radians_per_unit(int cfg)
    {
        const VehicleSpinnerConfig& c = vehicle_spinner_configs[cfg];
        if (c.rolling_radius > 0.0f) {
            return 1.0f / c.rolling_radius;
        }
        const float max_speed = vehicle_physics_class_max_speed(c.vehicle_class);
        return max_speed > 0.0f ? vehicle_two_pi * c.revs_at_max_speed / max_speed : 0.0f;
    }

    // About the frame's own +X (the axle). Matrix3's rvec/uvec/fvec are the images of local X/Y/Z.
    rf::Matrix3 vehicle_rotation_about_x(float angle)
    {
        const float c = std::cos(angle);
        const float s = std::sin(angle);
        return rf::Matrix3{{1.0f, 0.0f, 0.0f}, {0.0f, c, s}, {0.0f, -s, c}};
    }

    // About the frame's own +Y (up). Positive turns forward (+Z) toward right (+X), the sense
    // btRaycastVehicle applies steer_current in.
    rf::Matrix3 vehicle_rotation_about_y(float angle)
    {
        const float c = std::cos(angle);
        const float s = std::sin(angle);
        return rf::Matrix3{{c, 0.0f, -s}, {0.0f, 1.0f, 0.0f}, {s, 0.0f, c}};
    }

    // About the frame's own +Z (the screw's shaft).
    rf::Matrix3 vehicle_rotation_about_z(float angle)
    {
        const float c = std::cos(angle);
        const float s = std::sin(angle);
        return rf::Matrix3{{c, s, 0.0f}, {-s, c, 0.0f}, {0.0f, 0.0f, 1.0f}};
    }

} // namespace

// Both teams, because a hull can change hands without re-spawning.
void vehicle_team_textures_prime(rf::Entity* ep)
{
    if (rf::is_dedicated_server || !ep || !multi_is_team_game_type()) {
        return;
    }
    rf::VMesh* vmesh = ep->vmesh;
    if (!vmesh || rf::vmesh_get_type(vmesh) != rf::MESH_TYPE_STATIC) {
        return;
    }
    const auto* v3d = static_cast<const rf::V3d*>(vmesh->instance);
    if (!v3d || !v3d->meshes) {
        return;
    }
    for (int team = 0; team <= 1; ++team) {
        for (int i = 0; i < v3d->num_meshes; ++i) {
            vehicle_team_tex_set(&v3d->meshes[i], team);
        }
    }
}

// Dropped, not vmesh_free'd: that would decrement a reference the level teardown already owns.
void vehicle_drop_jeep_tire_mesh()
{
    for (int cfg = 0; cfg < vehicle_spinner_config_count; ++cfg) {
        g_spinner_mesh[cfg] = VehicleSpinnerMesh{};
        // The hull meshes go with the level too, so the memoized tag indices must not outlive them.
        g_spinner_prop_cache[cfg] = VehicleSpinnerPropCache{};
    }
    // Same for the team texture arrays: both the mesh keys and the bm handles die with the level.
    vehicle_team_tex_cache_clear();
}

namespace
{
    void vehicle_ensure_spinner_mesh(int cfg)
    {
        VehicleSpinnerMesh& m = g_spinner_mesh[cfg];
        if (m.load_attempted) {
            return;
        }
        m.load_attempted = true;
        const char* filename = vehicle_spinner_configs[cfg].mesh_filename;
        m.mesh = rf::vmesh_load(filename, rf::MESH_TYPE_STATIC, -1);
        if (!m.mesh) {
            xlog::warn("[vehicle] failed to load mesh '{}', that hull renders without its wheels",
                       filename);
        }
    }
} // namespace

// Spin is integrated from the hull's position DELTA, not a velocity: a driverless coasting hull
// deliberately reports zero velocity on the wire. On the frame tick and not in the render hook,
// which does not fire for an off-screen hull and fires twice for one in the rail-scanner overlay.
void vehicle_update_jeep_wheels()
{
    // Nothing can be in the map when this is false: the same gate the row lookup opens on.
    if (!rf::is_multi || !vehicle_level_has_factories()) {
        return;
    }
    std::erase_if(g_vehicle_state.wheel_spin, [](const auto& kv) {
        return vehicle_spinner_config_for_entity(rf::entity_from_handle(kv.first)) < 0;
    });

    for (rf::Entity& entity : DoublyLinkedList{rf::entity_list}) {
        const int cfg = vehicle_spinner_config_for_entity(&entity);
        if (cfg < 0) {
            continue;
        }
        const VehicleSpinnerConfig& c = vehicle_spinner_configs[cfg];
        vehicle_ensure_spinner_mesh(cfg);
        VehicleWheelSpin& spin = g_vehicle_state.wheel_spin[entity.handle];
        const rf::Vector3 delta = entity.pos - spin.last_pos;
        const bool had_last = spin.has_last;
        spin.last_pos = entity.pos;
        spin.has_last = true;

        if (c.steer_front) {
            // The driving machine reads its Bullet body's live angle; everyone else reads the
            // driver's quantized one at obj_update cadence, which has to be eased. The first frame
            // snaps, so a jeep entering view mid-turn does not sweep in from zero.
            float steer_target = 0.0f;
            const bool steer_is_local =
                vehicle_physics_driven_steer_angle(entity.handle, &steer_target);
            if (!steer_is_local) {
                if (const VehicleOrientSupplement* supp = vehicle_orient_supplement(entity.handle)) {
                    steer_target = supp->steer;
                }
            }
            if (steer_is_local || !had_last) {
                spin.steer = steer_target;
            }
            else {
                constexpr float steer_ease_rate = 10.0f;
                // Frame-rate independent: a clamped rate*dt converges faster the finer the frame is.
                const float t = 1.0f - std::exp(-steer_ease_rate * rf::frametime);
                spin.steer += (steer_target - spin.steer) * t;
            }
        }

        if (!had_last) {
            continue;
        }
        // A respawn, a level-start snap or an interp catch-up moves further in one frame than any
        // hull can drive; spinning that would burst the wheels, so the angle holds instead.
        constexpr float max_step = 8.0f;
        if (delta.len_sq() > max_step * max_step) {
            continue;
        }
        // Only whether this hull has drawable spinners; the transforms are the render path's job.
        if (!entity.vmesh || !vehicle_spinner_prop_cache(cfg, entity.vmesh).complete) {
            continue;
        }
        const float forward = delta.dot_prod(entity.orient.fvec);
        spin.angle = std::fmod(spin.angle + forward * vehicle_spinner_radians_per_unit(cfg),
                               vehicle_two_pi);
    }
}

namespace
{
    // Tracked hulls carry their belts as geometry, so they are animated by scrolling the texture.
    struct VehicleTreadConfig
    {
        int vehicle_class;                 // VehicleDamageClass row this belt belongs to
        std::string_view texture_basename; // matched WITHOUT extension: an ATX name carries none
        bool tile_on_u;                    // per-mesh fact
        float scale;                       // UV units per world unit
        float sign;                        // -1 sends the belt's TOP run forward with the hull
    };

    constexpr VehicleTreadConfig vehicle_tread_configs[] = {
        {VDC_APC, "af_apctread1", true, 0.3473f, -1.0f},       // APC
        {VDC_DRILLER, "af_drilltread1", true, 0.2546f, -1.0f}, // Driller01
    };

    constexpr int vehicle_tread_config_count = static_cast<int>(std::size(vehicle_tread_configs));

    // The bm cache keys on the REQUESTED filename, extension included, so one .atx reached through
    // two names is two handles sharing one controller: a row resolves to a small SET, not a handle.
    constexpr int vehicle_tread_max_handles = 4;
    // Hull LODs plus the attachment meshes drawn between them, so alternating draws never rescan.
    constexpr int vehicle_tread_max_scanned = 8;

    struct VehicleTreadRuntime
    {
        // Keyed on the VifMesh, not on the handle array: under alt_tex that array is a stack local
        // the engine rebuilds at one fixed address for every sub-mesh it draws (0x0052FB9A).
        const void* scanned[vehicle_tread_max_scanned] = {};
        int scanned_cursor = 0;
        int handles[vehicle_tread_max_handles] = {-1, -1, -1, -1};
        int num_handles = 0;
    };
    VehicleTreadRuntime g_tread_runtime[vehicle_tread_config_count];

    // Which tread row this hull is, or -1 for anything that has no belts.
    int vehicle_tread_config_for_entity(const rf::Entity* ep)
    {
        if (!ep || !rf::is_multi || !vehicle_level_has_factories()) {
            return -1;
        }
        // Carries the synced-type gate, so a renamed class or a second EIF_DRILLER hull still lands
        // on the row every other per-class rule puts it on.
        const int vdc = vehicle_damage_class(ep);
        if (vdc < 0) {
            return -1;
        }
        for (int cfg = 0; cfg < vehicle_tread_config_count; ++cfg) {
            if (vehicle_tread_configs[cfg].vehicle_class == vdc) {
                return cfg;
            }
        }
        return -1;
    }

    // Any row's belt, matched on the resolved handle or on the name the resolve matches on.
    bool vehicle_bitmap_is_tread_belt(int bm_handle)
    {
        if (bm_handle < 0) {
            return false;
        }
        const char* filename = rf::bm::get_filename(bm_handle);
        const std::string_view base =
            filename ? get_filename_without_ext(filename) : std::string_view{};
        for (int cfg = 0; cfg < vehicle_tread_config_count; ++cfg) {
            if (vehicle_is_tread_bitmap(bm_handle, cfg)) {
                return true;
            }
            if (!base.empty() && string_iequals(base, vehicle_tread_configs[cfg].texture_basename)) {
                return true;
            }
        }
        return false;
    }
} // namespace

// Advanced from the position delta on the frame tick, for the same reasons as the jeep's roll.
void vehicle_update_treads()
{
    // Same gate vehicle_tread_config_for_entity opens on, so the map is empty when it is shut.
    if (!rf::is_multi || !vehicle_level_has_factories()) {
        return;
    }
    std::erase_if(g_vehicle_state.tread_scroll, [](const auto& kv) {
        return vehicle_tread_config_for_entity(rf::entity_from_handle(kv.first)) < 0;
    });

    for (rf::Entity& entity : DoublyLinkedList{rf::entity_list}) {
        const int cfg = vehicle_tread_config_for_entity(&entity);
        if (cfg < 0) {
            continue;
        }
        VehicleTreadScroll& scroll = g_vehicle_state.tread_scroll[entity.handle];
        const rf::Vector3 delta = entity.pos - scroll.last_pos;
        const bool had_last = scroll.has_last;
        scroll.last_pos = entity.pos;
        scroll.has_last = true;
        if (!had_last) {
            continue;
        }
        // Same guard as the jeep's roll.
        constexpr float max_step = 8.0f;
        if (delta.len_sq() > max_step * max_step) {
            continue;
        }
        const VehicleTreadConfig& c = vehicle_tread_configs[cfg];
        scroll.config = cfg;
        const float forward = delta.dot_prod(entity.orient.fvec);
        // Wrapped so the accumulator never loses float precision on a long drive.
        scroll.offset = std::fmod(scroll.offset + c.sign * c.scale * forward, 1.0f);
    }
}

rf::VMesh* vehicle_jeep_tire_mesh()
{
    const int cfg = vehicle_spinner_config_for_class(VDC_JEEP);
    return cfg < 0 ? nullptr : g_spinner_mesh[cfg].mesh;
}

bool vehicle_is_tread_bitmap(int bm_handle, int config_index)
{
    if (bm_handle < 0 || config_index < 0 || config_index >= vehicle_tread_config_count) {
        return false;
    }
    const VehicleTreadRuntime& rt = g_tread_runtime[config_index];
    for (int i = 0; i < rt.num_handles; ++i) {
        if (rt.handles[i] == bm_handle) {
            return true;
        }
    }
    return false;
}

void vehicle_tread_resolve_mesh_bitmaps(int config_index, const void* mesh_key, const int* tex_handles,
                                        int num_tex_handles)
{
    if (config_index < 0 || config_index >= vehicle_tread_config_count || !tex_handles || !mesh_key) {
        return;
    }
    VehicleTreadRuntime& rt = g_tread_runtime[config_index];
    for (const void* seen : rt.scanned) {
        if (seen == mesh_key) {
            return;
        }
    }
    rt.scanned[rt.scanned_cursor] = mesh_key;
    rt.scanned_cursor = (rt.scanned_cursor + 1) % vehicle_tread_max_scanned;
    const std::string_view basename = vehicle_tread_configs[config_index].texture_basename;
    for (int i = 0; i < num_tex_handles && rt.num_handles < vehicle_tread_max_handles; ++i) {
        const int h = tex_handles[i];
        if (h < 0 || vehicle_is_tread_bitmap(h, config_index)) {
            continue;
        }
        const char* filename = rf::bm::get_filename(h);
        if (filename && string_iequals(get_filename_without_ext(filename), basename)) {
            rt.handles[rt.num_handles++] = h;
        }
    }
}

bool vehicle_tread_scroll_for_draw(int& config_out, float& u_out, float& v_out)
{
    config_out = -1;
    u_out = 0.0f;
    v_out = 0.0f;
    const rf::Entity* ep = vehicle_rendering_entity();
    if (!ep) {
        return false;
    }
    const auto it = g_vehicle_state.tread_scroll.find(ep->handle);
    if (it == g_vehicle_state.tread_scroll.end() || it->second.config < 0) {
        return false;
    }
    config_out = it->second.config;
    if (vehicle_tread_configs[config_out].tile_on_u) {
        u_out = it->second.offset;
    }
    else {
        v_out = it->second.offset;
    }
    return true;
}

void vehicle_render_jeep_tires(rf::Entity* ep)
{
    // No visibility test needed: this runs from the attachment point inside entity_render
    // (0x00421C0B), right after the hull's own vmesh_render and before every reason it is skipped.
    const int cfg = vehicle_spinner_config_for_entity(ep);
    if (cfg < 0 || !g_spinner_mesh[cfg].mesh) {
        return;
    }
    VehicleSpinnerPlacement placements[vehicle_spinner_max_props];
    const int n = vehicle_spinner_placements(cfg, ep, placements);
    if (n == 0) {
        return;
    }

    const VehicleSpinnerConfig& c = vehicle_spinner_configs[cfg];
    const auto it = g_vehicle_state.wheel_spin.find(ep->handle);
    const float spin = it != g_vehicle_state.wheel_spin.end() ? it->second.angle : 0.0f;
    // Radians, positive toward the hull's right.
    const float steer = it != g_vehicle_state.wheel_spin.end() ? it->second.steer : 0.0f;

    rf::MeshRenderParams params{};
    params.init_defaults();
    for (int i = 0; i < n; ++i) {
        const VehicleSpinnerPlacement& w = placements[i];
        rf::Vector3 pos = ep->pos + ep->orient.transform_vector(w.center);

        rf::Matrix3 orient = ep->orient;
        if (c.use_prop_orient) {
            orient.mul(w.orient);
        }
        if (c.steer_front && w.is_front && steer != 0.0f) {
            orient.mul(vehicle_rotation_about_y(steer));
        }
        // The tire mesh is authored as a LEFT wheel, and facing it outboard maps its axle from +X
        // to -X, so the same world roll needs the opposite local angle on that side.
        if (c.yaw_right_180 && w.is_right) {
            orient.mul(vehicle_rotation_about_y(std::numbers::pi_v<float>));
        }
        const float angle = c.negate_right && w.is_right ? -spin : spin;
        orient.mul(c.spin_about_shaft ? vehicle_rotation_about_z(angle)
                                      : vehicle_rotation_about_x(angle));

        params.orient = orient;
        rf::vmesh_render(g_spinner_mesh[cfg].mesh, &pos, &orient, &params);
    }
}

void vehicle_tread_runtime_reset()
{
    for (VehicleTreadRuntime& rt : g_tread_runtime) {
        rt = VehicleTreadRuntime{};
    }
}

void vehicle_render_install()
{
    obj_clear_render_flags_hook.install();
    obj_render_room_gate_hook.install();
    obj_mark_room_driller_route_hook.install();
    entity_render_hook.install();
    vmesh_render_static_team_tex_patch.install();
}
