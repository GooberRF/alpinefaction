#include <algorithm>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>
#include <patch_common/CodeInjection.h>
#include "vehicle.h"
#include "vehicle_debris.h"
#include "vehicle_internal.h"
#include "vehicle_render.h"
#include "../multi.h"
#include "../demo/demo.h"
#include "../../main/main.h"
#include "../../misc/alpine_settings.h"
#include "../../misc/settings_registry.h"
#include "../../object/object.h"
#include "../../os/console.h"
#include "../../rf/entity.h"
#include "../../rf/geometry.h"
#include "../../rf/multi.h"
#include "../../rf/object.h"
#include "../../rf/vmesh.h"
#include "../../rf/os/console.h"
#include "../../sound/sound_foley.h"

namespace
{
    struct DebrisClass
    {
        const char* class_name;
        const char* debris_filename; // one piece per submesh
        // Out of liquid the hull throws debris and dies with this class's explosion; in liquid it keeps its stock death.
        const char* dry_explode_class = nullptr;
    };

    constexpr DebrisClass debris_classes[] = {
        {"Jeep01", "af_Jeep01_debris.v3m"},
        {"APC", "af_APC_debris.v3m"},
        {"Driller01", "af_Driller01_debris.v3m"},
        {"Fighter01", "af_Fighter01_debris.v3m"},
        {"Stationary Turret_Plain", "af_turret_plain_debris.v3m"},
        {"sub", "af_sub_debris.v3m", "Jeep01"},
    };

    constexpr int debris_min_lifetime_ms = 8000;
    constexpr int debris_max_lifetime_ms = 15000;
    constexpr float debris_inherit_vel = 0.7f;
    constexpr float debris_max_inherited_speed = 25.0f;
    constexpr float debris_min_burst = 2.0f; // u/s out from the hull's centre
    constexpr float debris_max_burst = 6.0f;
    constexpr float debris_min_lift = 2.0f; // u/s up
    constexpr float debris_max_lift = 4.0f;
    constexpr float debris_min_spin = 1.0f; // rad/s
    constexpr float debris_max_spin = 4.0f;
    // A piece rests on one sphere: a little inside its bounds, so it settles onto its faces rather than floats.
    constexpr float debris_collision_radius_scale = 0.6f;
    constexpr float debris_mass = 100.0f;
    constexpr int debris_flame_burn_ms = 3000;

    // Debris handle -> the team its hull was drawn with.
    std::unordered_map<int, int> g_debris_team;

    const DebrisClass* debris_class(const rf::Entity* hull)
    {
        for (const DebrisClass& cls : debris_classes) {
            if (hull->info->name == cls.class_name) {
                return &cls;
            }
        }
        return nullptr;
    }

    // The class a dying hull breaks up as, or null when it dies its stock death.
    const DebrisClass* debris_class_for_death(const rf::Entity* hull)
    {
        const DebrisClass* cls = debris_class(hull);
        return cls && cls->dry_explode_class && vehicle_hull_in_liquid(hull) ? nullptr : cls;
    }

    // Throws every submesh of `filename`, placed with `orient` at `pos`, as a piece of `hull` drawn with `team`'s art
    // (-1 for stock art).
    void throw_pieces(const rf::Entity* hull, const char* filename, const rf::Vector3& pos, const rf::Matrix3& orient,
                      const rf::Vector3& base_vel, int team, std::vector<rf::Debris*>& out)
    {
        rf::VMesh* vmesh = rf::vmesh_load(filename, rf::MESH_TYPE_STATIC, -1);
        if (!vmesh) {
            return;
        }
        const int num_pieces = rf::vmesh_get_num_submeshes(vmesh);
        for (int i = 0; i < num_pieces; ++i) {
            rf::Vector3 offset{};
            float radius = 0.0f;
            rf::vmesh_get_submesh_bound(vmesh, i, &offset, &radius);

            rf::DebrisCreateStruct dcs{};
            dcs.pos = pos + orient.transform_vector(offset);
            dcs.orient = orient;
            rf::Vector3 out_dir = dcs.pos - hull->pos;
            out_dir.y = 0.0f;
            if (out_dir.len_sq() > 1e-4f) {
                out_dir.normalize();
            }
            else {
                out_dir.rand_quick();
            }
            dcs.vel = base_vel + out_dir * fx_rand(debris_min_burst, debris_max_burst)
                + rf::Vector3{0.0f, fx_rand(debris_min_lift, debris_max_lift), 0.0f};
            rf::Vector3 spin;
            spin.rand_quick();
            dcs.spin = spin * fx_rand(debris_min_spin, debris_max_spin);
            dcs.lifetime_ms =
                std::uniform_int_distribution<int>{debris_min_lifetime_ms, debris_max_lifetime_ms}(g_rng);
            dcs.material = hull->material;
            dcs.debris_flags = rf::DF_BOUNCE;
            dcs.obj_flags = rf::OF_NO_COLLIDE_REGISTER; // weapons pass through, as they do gibs
            dcs.room = hull->room;
            dcs.iss = g_metal_debris_iss;
            if (rf::Debris* piece = rf::debris_create(hull->handle, filename, debris_mass, &dcs, i,
                                                      radius * debris_collision_radius_scale)) {
                piece->obj_flags |= rf::OF_INVULNERABLE;
                if (team >= 0) {
                    g_debris_team[piece->handle] = team;
                }
                out.push_back(piece);
            }
        }
        rf::vmesh_free(vmesh);
    }

    // entity_blow_up, loading the explode vclip for its 0x00436490 call; the radius is already pushed. Every machine,
    // dedicated server included: the vclip and radius also set the death's radius damage (0x00436490 ->
    // apply_radius_damage).
    CodeInjection entity_blow_up_vclip_injection{
        0x00419482,
        [](auto& regs) {
            rf::Entity* ep = regs.esi;
            const DebrisClass* cls =
                rf::is_multi && vehicle_is_synced_entity_type(ep) ? debris_class_for_death(ep) : nullptr;
            const int dry_type = cls && cls->dry_explode_class ? rf::entity_lookup_type(cls->dry_explode_class) : -1;
            if (dry_type < 0 || dry_type >= rf::num_entity_types) {
                return;
            }
            const rf::EntityInfo& dry = rf::entity_types[dry_type];
            regs.ecx = dry.explode_vclip_index;
            addr_as_ref<float>(regs.esp + 0xC) = dry.explode_vclip_radius;
            regs.eip = 0x00419488; // past the original load of ecx
        },
    };

    ConsoleCommand2 vehicledebris_cmd{
        "cl_vehicledebris",
        [](std::optional<bool> enabled) {
            setting_console_set_bool("vehicle_debris", enabled);
            rf::console::print("Vehicle debris is {}", g_alpine_game_config.vehicle_debris ? "enabled" : "disabled");
        },
        "Break destroyed vehicles into pieces of wreckage in multiplayer",
        "cl_vehicledebris [bool]",
    };

    ConsoleCommand2 vehicledebrisflames_cmd{
        "cl_vehicledebrisflames",
        [](std::optional<bool> enabled) {
            setting_console_set_bool("vehicle_debris_flames", enabled);
            rf::console::print("Vehicle debris flames are {}",
                               g_alpine_game_config.vehicle_debris_flames ? "enabled" : "disabled");
        },
        "Set some of the wreckage of destroyed vehicles on fire in multiplayer",
        "cl_vehicledebrisflames [bool]",
    };
} // namespace

void vehicle_debris_spawn(rf::Entity* hull)
{
    if (!rf::is_multi || rf::is_dedicated_server || is_headless_mode() || !g_alpine_game_config.rendering_enabled
        || !g_alpine_game_config.vehicle_debris || demo_playback_is_seeking() || !hull->info) {
        return;
    }
    const DebrisClass* cls = debris_class_for_death(hull);
    if (!cls) {
        return;
    }
    rf::Vector3 base_vel = vehicle_live_hull_velocity(hull) * debris_inherit_vel;
    if (base_vel.len() > debris_max_inherited_speed) {
        base_vel.normalize();
        base_vel *= debris_max_inherited_speed;
    }
    const int team = vehicle_render_team(hull);
    std::vector<rf::Debris*> pieces;
    throw_pieces(hull, cls->debris_filename, hull->pos, hull->orient, base_vel, team, pieces);
    if (hull->vmesh) {
        for (const VehicleRestAttachment& part : vehicle_rest_attachments(hull->info_index, hull->vmesh)) {
            rf::Matrix3 part_orient = hull->orient;
            part_orient.mul(part.orient);
            // Tires and screws keep stock art on the hull too.
            const int part_team = vehicle_is_spinner_mesh_filename(part.mesh_filename.c_str()) ? -1 : team;
            throw_pieces(hull, part.mesh_filename.c_str(), hull->pos + hull->orient.transform_vector(part.pos),
                         part_orient, base_vel, part_team, pieces);
        }
    }
    if (g_alpine_game_config.vehicle_debris_flames) {
        std::shuffle(pieces.begin(), pieces.end(), g_rng);
        const size_t burning = (pieces.size() + 1) / 2;
        for (size_t i = 0; i < burning; ++i) {
            gib_flame_attach(pieces[i], debris_flame_burn_ms);
        }
    }
}

void vehicle_debris_do_frame()
{
    std::erase_if(g_debris_team, [](const auto& entry) {
        const rf::Object* obj = rf::obj_from_handle(entry.first);
        return !obj || obj->type != rf::OT_DEBRIS;
    });
}

void vehicle_debris_clear()
{
    g_debris_team.clear();
}

int vehicle_debris_team(int debris_handle)
{
    if (g_debris_team.empty()) {
        return -1;
    }
    const auto it = g_debris_team.find(debris_handle);
    return it != g_debris_team.end() ? it->second : -1;
}

void vehicle_debris_install()
{
    entity_blow_up_vclip_injection.install();
    vehicledebris_cmd.register_cmd();
    vehicledebrisflames_cmd.register_cmd();
}
