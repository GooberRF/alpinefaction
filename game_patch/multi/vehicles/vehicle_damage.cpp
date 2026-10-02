#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <type_traits>
#include <vector>
#include <xlog/xlog.h>
#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/FunHook.h>
#include <patch_common/MemUtils.h>
#include <common/utils/list-utils.h>
#include "vehicle.h"
#include "vehicle_physics.h"
#include "vehicle_internal.h"
#include "vehicle_sync.h"
#include "vehicle_damage.h"
#include "vehicle_crash_tuning.h"
#include "../alpine_packets.h"
#include "../gametype.h"
#include "../kill_attribution.h"
#include "../server_internal.h"
#include "../../hud/hud.h"
#include "../../hud/multi_spectate.h"
#include "../../misc/level.h"
#include "../../misc/player.h"
#include "../../os/console.h"
#include "../../os/os.h"
#include "../../rf/ai.h"
#include "../../rf/corpse.h"
#include "../../rf/entity.h"
#include "../../rf/item.h"
#include "../../rf/multi.h"
#include "../../rf/object.h"
#include "../../rf/os/frametime.h"
#include "../../rf/os/timer.h"
#include "../../rf/physics.h"
#include "../../rf/player/camera.h"
#include "../../rf/player/player.h"
#include "../../rf/sound/sound.h"
#include "../../rf/vmesh.h"
#include "../../rf/weapon.h"

namespace
{
    // Per-victim cooldown shared by every reporter, so one contact reported twice is one kill.
    constexpr float vehicle_crush_damage = 1000000.0f;
    constexpr int vehicle_crush_cooldown_ms = 500;

    // Per SENDER, whoever he names: bounds a client, not a contact.
    constexpr int vehicle_crush_report_cooldown_ms = 100;

    // How far outside its own hull box a vehicle may still be credited with a reported run-over.
    constexpr float vehicle_crush_report_slack = 10.0f;

    // A man seated in ANY synced vehicle is never run over, by anything.
    bool vehicle_crush_victim_is_exempt(const rf::Entity* victim)
    {
        return vehicle_ridden_hull(victim) != nullptr;
    }

    // How long a hull keeps killing for the driver who just stepped out. A ceiling: a boarding or a
    // settle closes the window early.
    constexpr int64_t vehicle_coast_memory_ms = 2500;

    // The figure entity_die pushes at 0x00418FFC (0x461C4000).
    constexpr float vehicle_occupant_death_damage = 10000.0f;

    constexpr float vehicle_regen_rate = 20.0f;         // life per second, all classes
    constexpr int64_t vehicle_regen_delay_ms = 10000;   // damage-free time before regen begins

    // Closing speed = relative velocity along the line between the two box centres.
    constexpr float vehicle_ram_min_speed = 5.0f; // world u/s of closing before a contact is a ram

    // Damage goes as the SQUARE of the overspeed.
    constexpr float vehicle_ram_damage_scale = 0.26f;

    // Ratio of the OTHER hull's mass to its own (rf::PhysicsData::mass), clamped.
    constexpr float vehicle_ram_mass_ratio_min = 0.25f;
    constexpr float vehicle_ram_mass_ratio_max = 4.0f;

    // Dwell before a hit is minted. Wall clock, not frames: the phantom it rejects is per sample.
    constexpr int64_t vehicle_ram_confirm_ms = 25;

    // How long a peak closing speed stays valid: the pre-step kills the live one before the dwell ends.
    constexpr int64_t vehicle_ram_peak_window_ms = 500;

    // Re-arm needs full separation AND this long; the time floor only covers boundary jitter.
    constexpr int64_t vehicle_ram_rearm_ms = 500;

    // Applied as mass * dv (vehicle_physics_server_shove), so the SPEED change is class-independent.
    constexpr float vehicle_ram_shove_frac = 0.6f;
    constexpr float vehicle_ram_max_shove = 10.0f; // world u/s of dv, the whole ceiling on a ram shove
    // The rammed hull's push is tilted this far toward straight up, which breaks the box overlap
    // instead of grinding along it; tilting keeps the magnitude at max_shove.
    constexpr float vehicle_ram_shove_up_bias = 0.15f;

    // A sleeping hull does not react late, it does not react at all: wake the pair before contact.
    constexpr float vehicle_ram_wake_closing = 0.5f;

    // Per second, scaled by frametime at the point of use; dealt as DT_CRUSH through the ram mint.
    constexpr float vehicle_drill_damage_per_sec = 200.0f;
    // Matches the driller's physics drill probe reach, so damage and carve describe the same bits.
    constexpr float vehicle_drill_reach = 9.0f;
    constexpr float vehicle_drill_radius = 2.5f;

    // One row per vehicle WEAPON; a weapon with no row never regenerates.
    struct VehicleWeaponAmmoRegen
    {
        const char* weapon_name;
        int interval_ms; // one round every this many milliseconds
    };

    constexpr VehicleWeaponAmmoRegen vehicle_weapon_ammo_regen[] = {
        {"Vauss", 100},
        {"Fighter Minigun", 100},
        {"APC Minigun", 100},
        {"Jeep Gun", 100},
        {"Torpedo", 5000},
        {"Fighter Rocket", 5000},
        {"APC Rocket", 5000},
    };

    // The authored table above is the readable config; this is the runtime one. Names are resolved
    // to weapon type ids once per level and compared as ints from then on; -1 disables the row.
    int g_vehicle_ammo_regen_type[std::size(vehicle_weapon_ammo_regen)]{};
    bool g_vehicle_ammo_regen_resolved = false;

    void vehicle_resolve_ammo_regen_types()
    {
        if (g_vehicle_ammo_regen_resolved) {
            return;
        }
        g_vehicle_ammo_regen_resolved = true;
        for (std::size_t i = 0; i < std::size(vehicle_weapon_ammo_regen); ++i) {
            const int wt = rf::weapon_lookup_type(vehicle_weapon_ammo_regen[i].weapon_name);
            g_vehicle_ammo_regen_type[i] = (wt >= 0 && wt < rf::num_weapon_types) ? wt : -1;
            if (g_vehicle_ammo_regen_type[i] < 0) {
                xlog::warn("[vehicle] ammo regen: unknown weapon class '{}'",
                           vehicle_weapon_ammo_regen[i].weapon_name);
            }
        }
    }

    // One row per vehicle class, one column per rf::DamageType; server only, applied BEFORE obj_damage's
    // own body. A NEGATIVE hull multiplier means REPAIR (bash -1.00: melee is the repair tool).
    struct VehicleDamageTuning
    {
        float occupant[rf::DT_COUNT]; // scale on damage dealt to a player seated in this class
        float hull[rf::DT_COUNT];     // scale on damage dealt to the vehicle entity itself
    };

    constexpr VehicleDamageTuning vehicle_damage_tuning[VDC_COUNT] = {
        // ------------------------------------------------------------------ VDC_JEEP (open seats)
        {
            //  bash  bullet    ap  explos   fire  energy  elec   acid  scald  crush   u10    u11
            {  0.75f, 0.75f, 0.75f, 0.75f, 0.75f, 0.75f, 0.75f, 0.75f, 0.75f, 0.75f, 0.75f, 0.75f },
            { -1.00f, 0.50f, 0.75f, 1.00f, 0.00f, 0.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f },
        },
        // ------------------------------------------------------------------------------- VDC_APC
        {
            //  bash  bullet    ap  explos   fire  energy  elec   acid  scald  crush   u10    u11
            {  1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f },
            { -1.00f, 0.50f, 0.75f, 1.00f, 0.00f, 0.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f },
        },
        // --------------------------------------------------------------------------- VDC_DRILLER
        {
            //  bash  bullet    ap  explos   fire  energy  elec   acid  scald  crush   u10    u11
            {  1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f },
            { -1.00f, 0.50f, 0.75f, 1.00f, 0.00f, 0.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f },
        },
        // --------------------------------------------------------------------------- VDC_FIGHTER
        // Fighter01's row; masako_fighter resolves here too but diverges in the tbl.
        {
            //  bash  bullet    ap  explos   fire  energy  elec   acid  scald  crush   u10    u11
            {  1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f },
            { -1.00f, 0.50f, 0.75f, 1.00f, 0.00f, 0.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f },
        },
        // ------------------------------------------------------------------------------- VDC_SUB
        {
            //  bash  bullet    ap  explos   fire  energy  elec   acid  scald  crush   u10    u11
            {  1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f },
            { -1.00f, 0.50f, 0.75f, 1.00f, 0.00f, 0.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f },
        },
        // ---------------------------------------------------- VDC_TURRET (gunner is stock-exposed)
        {
            //  bash  bullet    ap  explos   fire  energy  elec   acid  scald  crush   u10    u11
            {  0.75f, 0.75f, 0.75f, 0.75f, 0.75f, 0.75f, 0.75f, 0.75f, 0.75f, 0.75f, 0.75f, 0.75f },
            { -1.00f, 0.50f, 0.75f, 1.00f, 0.00f, 0.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f },
        },
    };

    // The synced vehicle currently running this victim over, or null. only_simulated keeps only a
    // crusher this machine simulates; pass false to recognise the contact on a watcher too.
    rf::Entity* vehicle_crusher_of(const rf::Entity* victim, bool only_simulated = true)
    {
        if (!victim) {
            return nullptr;
        }
        // The VICTIM's own record: physics_frame_init resets hit_time (+0x144) but never obj_handle
        // (+0x15C), so a hull nobody simulates here carries a stale handle and 0 is legal.
        if (victim->p_data.collide_out.hit_time >= 1.0f) {
            return nullptr;
        }
        rf::Entity* crusher = vehicle_synced_entity(victim->p_data.collide_out.obj_handle);
        if (!crusher) {
            return nullptr;
        }
        if (only_simulated && (crusher->p_data.flags & rf::PF_NET_PLAYER)) {
            return nullptr;
        }
        return crusher;
    }

    // The stock friendly-fire gate's questions, for the case it cannot see: a VEHICLE victim.
    bool vehicle_team_damage_blocked(const rf::Player* attacker, const rf::Player* victim)
    {
        if (!attacker || !victim || attacker == victim) {
            return false;
        }
        if (!multi_is_team_game_type() || rf::multi_is_team_damage_on()) {
            return false;
        }
        return attacker->team == victim->team;
    }

    // With friendly fire off, a player may not damage a vehicle a team-mate is sitting in:
    // entity_die's 10000 explosive to every occupant would carry into a team kill.
    bool vehicle_occupied_by_friendly(const rf::Entity* vehicle, int killer_handle)
    {
        if (!vehicle_is_synced_entity_type(vehicle) || killer_handle == -1) {
            return false;
        }
        rf::Player* attacker = rf::player_from_entity_handle(killer_handle);
        if (!attacker) {
            return false;
        }
        for (int i = 0; i < vehicle->interface_points.size(); ++i) {
            const int leech_handle = vehicle_seat_leech(vehicle, i);
            if (leech_handle == -1 || leech_handle == killer_handle) {
                continue;
            }
            if (vehicle_team_damage_blocked(attacker, rf::player_from_entity_handle(leech_handle))) {
                return true;
            }
        }
        return false;
    }

    float vehicle_hull_damage_multiplier(const rf::Entity* victim, int damage_type)
    {
        if (damage_type < 0 || damage_type >= rf::DT_COUNT) {
            return 1.0f;
        }
        const int hull_class = vehicle_damage_class(victim);
        return hull_class >= 0 ? vehicle_damage_tuning[hull_class].hull[damage_type] : 1.0f;
    }

    // Who may repair with a negative-multiplier blow: anybody if the hull is empty, otherwise an
    // occupant himself and - in a team gametype - anyone teamed with every occupant. NOT the
    // factory team lock, which says who may DRIVE a spawn.
    bool vehicle_repair_allowed(const rf::Entity* vehicle, int killer_handle)
    {
        if (killer_handle == -1) {
            return false; // player_from_entity_handle does not validate, and -1 matches a live row
        }
        const rf::Player* repairer = rf::player_from_entity_handle(killer_handle);
        if (!repairer) {
            return false;
        }
        const bool team_game = multi_is_team_game_type();
        for (int i = 0; i < vehicle->interface_points.size(); ++i) {
            const int leech_handle = vehicle_seat_leech(vehicle, i);
            if (leech_handle == -1 || leech_handle == killer_handle) {
                continue;
            }
            const rf::Player* occupant = vehicle_seat_occupant_player(vehicle, i);
            if (!occupant) {
                continue; // a stale seat handle or a non-player occupant
            }
            if (!team_game || occupant->team != repairer->team) {
                return false;
            }
        }
        return true;
    }

    // Turn a negative-multiplier blow into life, sharing the regen's ceiling and never-resurrect
    // rule. The regen clock is left alone: a repair is not damage.
    void vehicle_apply_repair(rf::Entity* vehicle, int killer_handle, float heal)
    {
        if (heal <= 0.0f || vehicle->life <= 0.0f || rf::entity_is_dying(vehicle)) {
            return;
        }
        auto it = g_vehicle_state.regen.find(vehicle->handle);
        if (it == g_vehicle_state.regen.end() || it->second.spawn_max_life <= 0.0f) {
            return; // no captured ceiling means no repair, rather than an unbounded one
        }
        if (!vehicle_repair_allowed(vehicle, killer_handle)) {
            return;
        }
        VehicleRegen& regen = it->second;
        const float before = vehicle->life;
        vehicle->life = std::min(vehicle->life + heal, regen.spawn_max_life);
        const float applied = vehicle->life - before;
        if (applied <= 0.0f) {
            return;
        }
        // Shift the watermark by exactly what was healed, not to the new life: damage that landed
        // earlier in this same frame must still read as a drop to the regen pass.
        regen.last_seen_life = std::min(regen.last_seen_life + applied, regen.spawn_max_life);
    }

    // pp's entity: the one entity_handle names while it is still his, otherwise his live one.
    rf::Entity* vehicle_player_credit_entity(const rf::Player* pp, int entity_handle)
    {
        rf::Entity* ep = rf::entity_from_handle(entity_handle);
        if (ep && rf::player_from_entity_handle(ep->handle) == pp) {
            return ep;
        }
        rf::Entity* live_ep = rf::entity_from_handle(pp->entity_handle);
        return (live_ep && rf::player_from_entity_handle(live_ep->handle) == pp) ? live_ep : nullptr;
    }

    // The entity the coast names as killer, or null. By net id, so credit follows the PLAYER if he
    // respawned inside the window. Expired entries are dropped on the way through.
    rf::Entity* vehicle_coast_memory_driver(int vehicle_handle)
    {
        auto it = g_vehicle_state.coast_memory.find(vehicle_handle);
        if (it == g_vehicle_state.coast_memory.end()) {
            return nullptr;
        }
        if (timer::get_i64(1000) >= it->second.expiry_ms) {
            g_vehicle_state.coast_memory.erase(it);
            return nullptr;
        }
        rf::Player* pp = rf::multi_find_player_by_id(it->second.player_id);
        if (!pp) {
            g_vehicle_state.coast_memory.erase(it); // disconnected: nobody left to credit
            return nullptr;
        }
        return vehicle_player_credit_entity(pp, it->second.exit_entity_handle);
    }

    // The player who destroyed the hull, resolved like vehicle_coast_memory_driver; null for a non-player.
    rf::Entity* vehicle_lethal_killer_entity(const VehicleLethalKiller& lethal)
    {
        if (lethal.entity_handle == -1 || lethal.player_id == 0xFF) {
            return nullptr;
        }
        rf::Player* pp = rf::multi_find_player_by_id(lethal.player_id);
        if (!pp) {
            return nullptr;
        }
        return vehicle_player_credit_entity(pp, lethal.entity_handle);
    }

    // What this module is minting through obj_damage right now, or an empty record. obj_damage
    // has no user tag and it recurses, so the mint is keyed by VICTIM handle: an inner blow on a
    // different object inherits nothing, and the RAII restore puts the outer mint back.
    struct VehicleCrushMint
    {
        int victim_handle = -1;
        int damage_class = -1; // the minting hull's VDC row, or -1 for an unattributed blow
        bool squash = false;   // a run-over or the drill, never a hull-on-hull ram
        float speed = 0.0f;    // the run-over hull's |v|; 0 for the drill and rams
    };
    VehicleCrushMint g_vehicle_crush_mint;

    class VehicleCrushMintScope
    {
    public:
        VehicleCrushMintScope(int victim_handle, const rf::Entity* vehicle, bool squash = false,
                              float speed = 0.0f)
            : saved_(g_vehicle_crush_mint)
        {
            g_vehicle_crush_mint =
                VehicleCrushMint{victim_handle, vehicle_damage_class(vehicle), squash, speed};
        }
        ~VehicleCrushMintScope()
        {
            g_vehicle_crush_mint = saved_;
        }
        VehicleCrushMintScope(const VehicleCrushMintScope&) = delete;
        VehicleCrushMintScope& operator=(const VehicleCrushMintScope&) = delete;

    private:
        VehicleCrushMint saved_;
    };

    bool vehicle_crush_damage_is_mint(int victim_handle)
    {
        return victim_handle != -1 && victim_handle == g_vehicle_crush_mint.victim_handle;
    }

    // Sibling of the crush mint, for entity_die's occupant blast: the readers must recognise that
    // blow from the scope the producer opens, never from the shape of its arguments.
    struct VehicleBlastMint
    {
        int occupant_handle = -1; // the rider this blast is being dealt to
        int vehicle_handle = -1;  // the hull whose destruction is dealing it
    };
    VehicleBlastMint g_vehicle_blast_mint;

    class VehicleBlastMintScope
    {
    public:
        VehicleBlastMintScope(int occupant_handle, int vehicle_handle)
            : saved_(g_vehicle_blast_mint)
        {
            g_vehicle_blast_mint = VehicleBlastMint{occupant_handle, vehicle_handle};
        }
        ~VehicleBlastMintScope()
        {
            g_vehicle_blast_mint = saved_;
        }
        VehicleBlastMintScope(const VehicleBlastMintScope&) = delete;
        VehicleBlastMintScope& operator=(const VehicleBlastMintScope&) = delete;

    private:
        VehicleBlastMint saved_;
    };

    bool vehicle_blast_damage_is_mint(int victim_handle)
    {
        return victim_handle != -1 && victim_handle == g_vehicle_blast_mint.occupant_handle;
    }

    // A hull the SERVER simulates carries its velocity only in Bullet - vehicle_server_body_do_frame
    // leaves p_data.vel at zero for one. Every other hull's p_data.vel is its own client's row,
    // clamped on receipt, so it is bounded but not trustworthy.
    rf::Vector3 vehicle_server_hull_velocity(const rf::Entity* ep)
    {
        rf::Vector3 vel{};
        if (vehicle_physics_server_velocity(ep->handle, &vel)) {
            return vel;
        }
        return ep->p_data.vel;
    }

    // The one place a server-side roadkill is minted: the report path and the coast sweep share the
    // per-victim cooldown, so neither can double-hit one contact.
    bool vehicle_server_mint_crush(rf::Entity* victim, rf::Entity* killer, const rf::Entity* vehicle)
    {
        // The last gate before the blow exists at all; every caller already asked.
        if (vehicle_crush_victim_is_exempt(victim) || victim->life <= 0.0f || rf::entity_is_dying(victim)) {
            return false;
        }
        rf::Timestamp& cooldown = g_vehicle_state.crush_cooldown[victim->handle];
        if (cooldown.valid() && !cooldown.elapsed()) {
            return false;
        }
        cooldown.set(vehicle_crush_cooldown_ms);
        // Named killer, so the friendly-fire gate, the obituary and the score treat a roadkill like
        // a shot he fired; the scope carries it past the MP nullification and names the hull for
        // the kill feed, which is otherwise left guessing from the arguments.
        const VehicleCrushMintScope scope{victim->handle, vehicle, true,
                                          vehicle_server_hull_velocity(vehicle).len()};
        rf::obj_damage(victim->handle, vehicle_crush_damage, killer->handle, -1, rf::DT_CRUSH,
                       nullptr, -1, 0);
        return true;
    }

    // The body of both server roadkill sweeps: only the killer and which hulls qualify differ.
    void vehicle_server_crush_sweep(rf::Entity* vehicle, const rf::Vector3& hull_pos,
                                    const rf::Vector3& half, const rf::Vector3& center,
                                    rf::Entity* killer)
    {
        const rf::Vector3 hull_vel = vehicle_server_hull_velocity(vehicle);
        // NaN-safe, and a hull at rest runs nobody over however fast he walks into it.
        if (!(hull_vel.len() > vehicle_crush_min_speed)) {
            return;
        }
        // The box sits about a local centre that is not the hull origin for any land vehicle.
        const rf::Vector3 box_center = hull_pos + vehicle->orient.transform_vector(center);
        const float hull_reject = vehicle->radius + half.len();

        for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
            rf::Entity* victim = rf::entity_from_handle(player.entity_handle);
            if (!victim || victim->life <= 0.0f || rf::entity_is_dying(victim)) {
                continue;
            }
            // Nobody seated in a synced vehicle is a victim, including a rider of THIS hull, who sits
            // permanently inside its collision box.
            if (vehicle_crush_victim_is_exempt(victim)) {
                continue;
            }
            if (victim->handle == killer->handle) {
                continue;
            }
            const float vr = std::max(victim->radius, 0.1f);
            const rf::Vector3 delta = victim->pos - box_center;
            const float dist = delta.len();
            if (!(dist <= hull_reject + vr)) {
                continue;
            }
            const rf::Vector3 local{
                delta.dot_prod(vehicle->orient.rvec),
                delta.dot_prod(vehicle->orient.uvec),
                delta.dot_prod(vehicle->orient.fvec),
            };
            if (std::fabs(local.x) > half.x + vr || std::fabs(local.y) > half.y + vr
                || std::fabs(local.z) > half.z + vr) {
                continue;
            }
            // Closing, not raw: a man riding the roof moves with the hull and is not run over.
            const float closing = dist > 0.0001f
                ? (hull_vel - victim->p_data.vel).dot_prod(delta / dist)
                : hull_vel.len();
            if (!(closing > vehicle_crush_min_speed)) {
                continue;
            }
            vehicle_server_mint_crush(victim, killer, vehicle);
        }
    }

    // Server-only: no client is authoritative over two hulls at once. Damage lands ON IMPACT, once -
    // a pair in contact takes nothing more until it has come fully apart.

    // One hull's contribution to the pair sweep, gathered once per frame.
    struct VehicleRamHull
    {
        rf::Entity* ep = nullptr;
        int handle = -1;      // the pointer is re-resolved from this at the top of every pair
        rf::Vector3 center{}; // WORLD centre of the hull box
        rf::Vector3 half{};   // hull-LOCAL half extents; the hull's own orient turns them into world
        rf::Vector3 vel{};
        float reach = 0.0f; // half.len(), the radius of the cheap centre-distance reject
        float mass = 0.1f;
    };

    // The shared overlap test, so this and the physics pair response cannot disagree about which hulls
    // are touching. Only the margin is the sweep's own.
    bool vehicle_ram_hulls_overlap(const VehicleRamHull& a, const VehicleRamHull& b)
    {
        const VehicleHullObb box_a{a.center, a.ep->orient, a.half};
        const VehicleHullObb box_b{b.center, b.ep->orient, b.half};
        return vehicle_hull_obb_overlap(box_a, box_b, vehicle_ram_contact_margin);
    }

    // The scope is what carries the blow past obj_damage's MP crush nullification. `vehicle` is the
    // hull dealing it, and null for a blow nothing is to be credited with.
    void vehicle_ram_mint_damage(int victim_handle, int killer_handle, float damage,
                                 const rf::Entity* vehicle, bool squash = false)
    {
        if (damage <= 0.0f) {
            return;
        }
        const VehicleCrushMintScope scope{victim_handle, vehicle, squash};
        rf::obj_damage(victim_handle, damage, killer_handle, -1, rf::DT_CRUSH, nullptr, -1, 0);
    }

    // Its seated driver, or the ex-driver it is still coasting for. Null for a turret or an expired
    // window, and the ram is then minted with killer -1, an unattributed environment blow.
    rf::Entity* vehicle_ram_credit(rf::Entity* rammer)
    {
        if (rf::Entity* driver = vehicle_driver_entity(rammer)) {
            return driver;
        }
        // The erase-on-expiry read is safe here: this walks the vehicle list, not the memory map.
        return vehicle_coast_memory_driver(rammer->handle);
    }

    // Direction runs from the attacker, or the hull he rides; a crash or a coasting hull's ram has none.
    // `stock_indicated`: stock weapon damage has already lit a local rider's indicator (0x004C6183, 0x00489195).
    void vehicle_note_hull_hit(rf::Entity* vehicle, int killer_handle, bool stock_indicated)
    {
        rf::Entity* source = killer_handle != -1 ? rf::entity_from_handle(killer_handle) : nullptr;
        if (rf::Entity* hull = vehicle_ridden_hull(source)) {
            source = hull;
        }
        else if (vehicle_crush_damage_is_mint(vehicle->handle)) {
            source = nullptr; // a coasting hull's credited ex-driver is on foot elsewhere
        }
        rf::Vector3 dir = source && source != vehicle ? vehicle->pos - source->pos : rf::Vector3{};
        const bool has_dir = dir.len_sq() > 1e-4f;
        if (has_dir) {
            dir.normalize();
            VehicleHealthSync& sync = g_vehicle_state.health_sync[vehicle->handle];
            sync.hit_dir = dir;
            sync.hit_ms = timer::get_i64(1000);
        }
        // A listen host is sent no 0x67 of his own.
        if (!rf::is_dedicated_server) {
            vehicle_rider_damage_feedback(vehicle->handle, has_dir && !stock_indicated ? &dir : nullptr);
        }
    }
} // namespace

// Opened by vehicle_server_handle_exit; closed by a boarding, a settle, the vehicle dying or going
// away, or expiry - whichever comes first.
void vehicle_coast_memory_set(rf::Entity* vehicle, rf::Player* pp, rf::Entity* rider)
{
    if (!rf::is_server || !pp || !pp->net_data || !rider) {
        return;
    }
    VehicleCoastMemory& mem = g_vehicle_state.coast_memory[vehicle->handle];
    mem.player_id = pp->net_data->player_id;
    mem.exit_entity_handle = rider->handle;
    mem.expiry_ms = timer::get_i64(1000) + vehicle_coast_memory_ms;
}

// Detection cannot live on a client: the coast's interp samples deliberately carry zero velocity, so
// a watcher's stock crush classification has nothing to key on. Only while the memory window is live.
void vehicle_server_coast_crush_sweep(rf::Entity* vehicle, const rf::Vector3& hull_pos)
{
    if (vehicle_hull_is_turret(vehicle)) {
        return;
    }
    rf::Entity* killer = vehicle_coast_memory_driver(vehicle->handle);
    if (!killer) {
        return;
    }
    // Ownership test only: no server body means this machine is not simulating the hull.
    if (!vehicle_physics_server_owns(vehicle->handle)) {
        return;
    }
    rf::Vector3 half{};
    rf::Vector3 center{};
    vehicle_physics_entity_hull_box(vehicle, &half, &center);
    vehicle_server_crush_sweep(vehicle, hull_pos, half, center, killer);
}

// The server sweeps driven hulls too: the stock crush trigger dies once a hull latches
// collide_out.is_liquid, which the engine never clears.
void vehicle_server_driven_crush_sweep(rf::Entity* vehicle)
{
    if (!rf::is_multi || !rf::is_server || !vehicle || vehicle_hull_is_turret(vehicle)
        || rf::entity_is_dying(vehicle)) {
        return;
    }
    // A driverless hull belongs to the coast sweep, which runs in the other branch of the same frame.
    rf::Entity* killer = vehicle_driver_entity(vehicle);
    if (!killer) {
        return;
    }
    rf::Vector3 half{};
    rf::Vector3 center{};
    vehicle_physics_entity_hull_box(vehicle, &half, &center);
    vehicle_server_crush_sweep(vehicle, vehicle->pos, half, center, killer);
}

// Per-frame rather than latched: a drill is a continuous contact and the engine bypasses the
// fire-wait (0x00425919), so there is no discrete shot to hang a cooldown on.
void vehicle_server_drill_damage_sweep(rf::Entity* vehicle)
{
    if (!rf::is_multi || !rf::is_server || !vehicle || rf::entity_is_dying(vehicle)) {
        return;
    }
    rf::Entity* driver = vehicle_driver_entity(vehicle);
    const int killer_handle = driver ? driver->handle : -1;
    const float damage = vehicle_drill_damage_per_sec * rf::frametime;
    if (damage <= 0.0f) {
        return;
    }
    const rf::Vector3 bit = vehicle->pos + vehicle->orient.fvec * vehicle_drill_reach;

    const auto hurt = [&](rf::Entity* victim) {
        if (!victim || victim == vehicle || victim->life <= 0.0f || rf::entity_is_dying(victim)) {
            return;
        }
        if (victim->host_handle == vehicle->handle) {
            return; // its own occupants ride inside the volume permanently
        }
        if (vehicle_crush_victim_is_exempt(victim)) {
            return; // seated in any synced vehicle: never run over, and this is a run-over
        }
        const float reach = vehicle_drill_radius + std::max(victim->radius, 0.1f);
        if ((victim->pos - bit).len_sq() > reach * reach) {
            return;
        }
        // The RAM mint, not a bare obj_damage: its scope carries the DT_CRUSH past the MP nullification
        // and names the hull to the kill feed.
        vehicle_ram_mint_damage(victim->handle, killer_handle, damage, driver ? vehicle : nullptr,
                                true);
    };

    for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
        hurt(rf::entity_from_handle(player.entity_handle));
    }
    for (int handle : g_vehicle_state.synced_handles) {
        hurt(rf::entity_from_handle(handle));
    }
}

void vehicle_server_ram_sweep()
{
    std::vector<VehicleRamHull> hulls;
    hulls.reserve(g_vehicle_state.synced_handles.size());
    for (int handle : g_vehicle_state.synced_handles) {
        rf::Entity* ep = vehicle_live_synced_entity(handle);
        if (!ep || ep->life <= 0.0f) {
            continue; // a wreck neither rams nor is rammed
        }
        VehicleRamHull hull;
        hull.ep = ep;
        hull.handle = ep->handle;
        vehicle_physics_entity_hull_box(ep, &hull.half, &hull.center);
        // The box sits about a local centre that is not the hull origin for any land vehicle.
        hull.center = ep->pos + ep->orient.transform_vector(hull.center);
        hull.vel = vehicle_server_hull_velocity(ep);
        hull.reach = hull.half.len();
        hull.mass = std::max(ep->p_data.mass, 0.1f);
        hulls.push_back(hull);
    }
    if (hulls.size() < 2) {
        g_vehicle_state.ram_pairs.clear();
        return;
    }

    const int64_t now = timer::get_i64(1000);
    for (size_t i = 0; i + 1 < hulls.size(); ++i) {
        for (size_t j = i + 1; j < hulls.size(); ++j) {
            VehicleRamHull& a = hulls[i];
            VehicleRamHull& b = hulls[j];
            // A mint earlier in this sweep can have killed either hull, so both pointers are
            // re-resolved; the pose, velocity and mass stay this frame's gathered ones.
            a.ep = rf::entity_from_handle(a.handle);
            b.ep = rf::entity_from_handle(b.handle);
            if (!a.ep || !b.ep || a.ep->life <= 0.0f || b.ep->life <= 0.0f
                || rf::entity_is_dying(a.ep) || rf::entity_is_dying(b.ep)) {
                continue;
            }
            if (a.ep->host_handle == b.ep->handle || b.ep->host_handle == a.ep->handle) {
                continue;
            }

            const rf::Vector3 delta = b.center - a.center; // a -> b
            const float dist = delta.len();
            const bool in_reach =
                dist <= a.reach + b.reach + 2.0f * vehicle_ram_contact_margin;

            // Derived ahead of the overlap test because the WAKE below needs it while the hulls are
            // still approaching; once they overlap a sleeping hull can no longer take part.
            const rf::Vector3 dir =
                dist > 0.0001f ? delta / dist : a.ep->orient.fvec; // concentric: any axis will do
            const float a_toward = a.vel.dot_prod(dir);
            const float b_toward = -b.vel.dot_prod(dir);
            const float closing = a_toward + b_toward;

            // Wake both server bodies for the whole approach, not just the touch.
            if (in_reach && closing > vehicle_ram_wake_closing) {
                vehicle_physics_server_wake(a.ep->handle);
                vehicle_physics_server_wake(b.ep->handle);
            }

            const bool overlap = in_reach && vehicle_ram_hulls_overlap(a, b);

            const VehicleRamKey key = a.ep->handle < b.ep->handle
                                        ? VehicleRamKey{a.ep->handle, b.ep->handle}
                                        : VehicleRamKey{b.ep->handle, a.ep->handle};

            if (!in_reach) {
                // Out of reach ENDS an episode and re-arms the pair, once the time floor has passed.
                auto it = g_vehicle_state.ram_pairs.find(key);
                if (it != g_vehicle_state.ram_pairs.end()
                    && (!it->second.latched
                        || now - it->second.latched_ms >= vehicle_ram_rearm_ms)) {
                    g_vehicle_state.ram_pairs.erase(it);
                }
                continue;
            }

            VehicleRamPair& pair = g_vehicle_state.ram_pairs[key];
            if (pair.latched) {
                continue; // same episode: nothing until the two go out of reach above
            }

            // A peak older than the window is replaced even by a slower sample. The two toward
            // components are stored with it, so rammer identity and mass split come off one sample.
            if (closing > pair.peak_closing || now - pair.peak_ms > vehicle_ram_peak_window_ms) {
                pair.peak_closing = closing;
                pair.peak_a_toward = a_toward;
                pair.peak_b_toward = b_toward;
                pair.peak_ms = now;
            }

            if (!overlap) {
                pair.overlap_since_ms = 0; // in reach and still approaching: keep the peak, wait
                continue;
            }
            if (pair.overlap_since_ms == 0) {
                pair.overlap_since_ms = now;
            }
            if (now - pair.overlap_since_ms < vehicle_ram_confirm_ms) {
                continue; // one sample of closing is a teleport correction until it holds
            }

            // The pre-step has usually zeroed the live sample by now, but it still wins for a hull a
            // client is driving.
            const bool use_peak = pair.peak_closing > closing;
            const float eff_closing = use_peak ? pair.peak_closing : closing;
            const float eff_a_toward = use_peak ? pair.peak_a_toward : a_toward;
            const float eff_b_toward = use_peak ? pair.peak_b_toward : b_toward;

            if (eff_closing < vehicle_ram_min_speed) {
                continue; // parking, nudging, grinding: free
            }

            const float excess = eff_closing - vehicle_ram_min_speed;
            const float base = vehicle_ram_damage_scale * excess * excess;
            const float dmg_a = base
                * std::clamp(b.mass / a.mass, vehicle_ram_mass_ratio_min,
                             vehicle_ram_mass_ratio_max);
            const float dmg_b = base
                * std::clamp(a.mass / b.mass, vehicle_ram_mass_ratio_min,
                             vehicle_ram_mass_ratio_max);

            // The RAMMER carries more of the closing speed; a stationary hull is never the rammer.
            const bool a_is_rammer = eff_a_toward >= eff_b_toward;
            rf::Entity* rammer = a_is_rammer ? a.ep : b.ep;
            rf::Entity* rammed = a_is_rammer ? b.ep : a.ep;
            const float rammer_damage = a_is_rammer ? dmg_a : dmg_b;
            const float rammed_damage = a_is_rammer ? dmg_b : dmg_a;
            rf::Entity* credit = vehicle_ram_credit(rammer);
            const int killer_handle = credit ? credit->handle : -1;

            // Latch BEFORE the damage: obj_damage can destroy either hull, and a hull's death runs
            // vehicle_drop_combat_state, which erases this entry. Nothing may touch `pair` after.
            pair.latched = true;
            pair.latched_ms = now;
            pair.overlap_since_ms = 0;
            pair.peak_closing = 0.0f;
            pair.peak_a_toward = 0.0f;
            pair.peak_b_toward = 0.0f;
            pair.peak_ms = 0;

            // BEFORE the damage, because obj_damage can wreck a hull and take its server body with
            // it. A hull a client is DRIVING has no server body here and the shove is dropped.
            const float shove_dv =
                std::min(eff_closing * vehicle_ram_shove_frac, vehicle_ram_max_shove);
            {
                const rf::Vector3 up{0.0f, vehicle_ram_shove_up_bias, 0.0f};
                const rf::Vector3 a_push = a_is_rammer ? -dir : (-dir + up);
                const rf::Vector3 b_push = a_is_rammer ? (dir + up) : dir;
                vehicle_physics_server_shove(a.ep->handle, a_push, shove_dv);
                vehicle_physics_server_shove(b.ep->handle, b_push, shove_dv);
            }

            // The rammed hull takes the named killer, so the tuning table scales it, the
            // last-damager record is written and its occupant kills are credited through entity_die.
            const int rammer_handle = rammer->handle;
            const int rammed_handle = rammed->handle;
            vehicle_ram_mint_damage(rammed_handle, killer_handle, rammed_damage, rammer);
            // The rammer's share is nobody's kill, so killer -1 and no minting hull - which also
            // exempts it from the tuning table, which never scales an unattributed blow.
            vehicle_ram_mint_damage(rammer_handle, -1, rammer_damage, nullptr);
        }
    }
}

namespace
{
    // Over each report bound: gravity and drive keep acting across the summing window.
    constexpr float vehicle_crash_report_slack = 3.0f;
    // Four buckets of this: the observed peak spans at least the last 375 ms of rows, at most 500.
    constexpr int64_t vehicle_observed_speed_bucket_ms = 125;
    // Net extremity speed a run-over needs: between a parked driven APC's measured creep (0.047 u/s on a
    // 20 deg slope, 0.075 u/s while a walker shoves it) and the 0.1 u/s that must count.
    constexpr float vehicle_crush_motion_min_speed = 0.085f;
    // An oldest pose younger than this is too close to rate a speed from.
    constexpr int64_t vehicle_crush_motion_min_span_ms = 250;
    // A step past it every <= 200 ms needs only 0.05 u/s, well under the speed floor.
    constexpr float vehicle_crush_change_min_extent = 0.01f;
    constexpr int64_t vehicle_crush_motion_window_ms = 500;
    // The victim's 2.2 x 25 ms interp delay, the server's relay tick and a ~120 ms report round trip.
    constexpr int64_t vehicle_crush_motion_tail_ms = 200;

    // The stock crusher speed: the pooled constant at 0x005893C0 that 0x0041A0AD/0x0041A0C5 compare against.
    constexpr float stock_crusher_min_speed = 0.5f;

    // A yaw rate times this is the fastest any box FACE moves along its own normal: the far end of a side.
    float vehicle_hull_yaw_reach(const rf::Entity* vehicle)
    {
        rf::Vector3 half{};
        rf::Vector3 center{};
        vehicle_physics_entity_hull_box(vehicle, &half, &center);
        return std::max(std::fabs(center.x) + half.x, std::fabs(center.z) + half.z);
    }

    // How far the hull's extremity moved: the origin's shift, or the heading change swung at the reach.
    float vehicle_pose_extent(const rf::Vector3& from_pos, float from_heading, const rf::Vector3& to_pos,
                              float to_heading, float reach)
    {
        const float turn = std::remainder(to_heading - from_heading, vehicle_two_pi);
        return std::max((to_pos - from_pos).len(), std::fabs(turn) * reach);
    }

    void vehicle_observed_speed_advance(VehicleObservedSpeed& s, int64_t now)
    {
        const int64_t shift = (now - s.bucket_start_ms) / vehicle_observed_speed_bucket_ms;
        if (shift <= 0) {
            return;
        }
        for (int i = VehicleObservedSpeed::bucket_count - 1; i >= 0; --i) {
            const bool kept = i >= shift;
            const int from = kept ? i - static_cast<int>(shift) : 0;
            s.bucket_peak[i] = kept ? s.bucket_peak[from] : 0.0f;
            s.bucket_pos[i] = kept ? s.bucket_pos[from] : rf::Vector3{};
            s.bucket_heading[i] = kept ? s.bucket_heading[from] : 0.0f;
            s.bucket_pose_ms[i] = kept ? s.bucket_pose_ms[from] : 0;
            s.bucket_posed[i] = kept && s.bucket_posed[from];
        }
        s.bucket_start_ms += shift * vehicle_observed_speed_bucket_ms;
    }

    struct VehicleObservedRates
    {
        float linear = 0.0f; // u/s of the origin
        float turn = 0.0f;   // rad/s of the heading
        float reach = 0.0f;
    };

    // Net from the oldest pose in the window to the newest, over their ARRIVAL span: a relay burst
    // changes when rows land, not what they carry. 0 once it stops stepping, and a turret never runs
    // anyone over.
    VehicleObservedRates vehicle_observed_linear_and_turn_rate(const rf::Entity* vehicle)
    {
        if (!vehicle || vehicle_hull_is_turret(vehicle)) {
            return {};
        }
        auto it = g_vehicle_state.observed_speed.find(vehicle->handle);
        if (it == g_vehicle_state.observed_speed.end()) {
            return {};
        }
        VehicleObservedSpeed& s = it->second;
        const int64_t now = timer::get_i64(1000);
        vehicle_observed_speed_advance(s, now);
        if (!s.changed || now - s.changed_ms > vehicle_crush_motion_tail_ms) {
            return {};
        }
        for (int i = VehicleObservedSpeed::bucket_count - 1; i >= 0; --i) {
            if (!s.bucket_posed[i] || now - s.bucket_pose_ms[i] > vehicle_crush_motion_window_ms) {
                continue;
            }
            const int64_t span_ms = s.newest_ms - s.bucket_pose_ms[i];
            if (span_ms < vehicle_crush_motion_min_span_ms) {
                return {};
            }
            const float span_s = static_cast<float>(span_ms) * 0.001f;
            const float turn = std::remainder(s.newest_heading - s.bucket_heading[i], vehicle_two_pi);
            return {(s.newest_pos - s.bucket_pos[i]).len() / span_s, std::fabs(turn) / span_s, s.reach};
        }
        return {};
    }

    // u/s of the hull's farthest point: the origin's speed, or the heading rate swung at the reach.
    float vehicle_observed_extremity_speed(const rf::Entity* vehicle)
    {
        const VehicleObservedRates r = vehicle_observed_linear_and_turn_rate(vehicle);
        return std::max(r.linear, r.turn * r.reach);
    }

    bool vehicle_observed_motion(const rf::Entity* vehicle)
    {
        return vehicle_observed_extremity_speed(vehicle) >= vehicle_crush_motion_min_speed;
    }

    float vehicle_observed_speed_peak(int vehicle_handle)
    {
        auto it = g_vehicle_state.observed_speed.find(vehicle_handle);
        if (it == g_vehicle_state.observed_speed.end()) {
            return 0.0f;
        }
        VehicleObservedSpeed& s = it->second;
        vehicle_observed_speed_advance(s, timer::get_i64(1000));
        float peak = 0.0f;
        for (float v : s.bucket_peak) {
            peak = std::max(peak, v);
        }
        return peak;
    }

    // The one derivation both the reporting client and the server use.
    float vehicle_crash_life_frac(int vdc, float impact_dv, bool ground)
    {
        if (vdc < 0 || vdc >= VDC_COUNT || !(impact_dv > 0.0f)) {
            return 0.0f;
        }
        const VehicleCrashTuning& t = vehicle_crash_tuning[vdc];
        const float threshold = ground ? t.ground_threshold : t.wall_threshold;
        const float over = impact_dv - threshold;
        const float ref_over = (ground ? t.land_ref_dv : t.wall_ref_dv) - threshold;
        if (over <= 0.0f || ref_over <= 0.0f) {
            return 0.0f;
        }
        const float x = std::min(over / ref_over, vehicle_crash_max_over_ref);
        const float frac = (ground ? t.land_frac : t.wall_frac) * x * x;
        return frac >= vehicle_crash_min_life_frac ? frac : 0.0f;
    }

    float vehicle_crash_max_impact_dv(int vdc)
    {
        return vehicle_physics_class_max_impact_speed(vdc) + vehicle_crash_report_slack;
    }

    // Killer -1 and no minting hull, as the rammer's own share: an environment blow on the hull alone.
    void vehicle_server_apply_crash(rf::Entity* vehicle, float impact_dv, bool ground)
    {
        if (!g_alpine_server_config_active_rules.vehicles.crash_damage || vehicle->life <= 0.0f
            || rf::entity_is_dying(vehicle) || vehicle_hull_is_turret(vehicle)) {
            return;
        }
        const float frac = vehicle_crash_life_frac(vehicle_damage_class(vehicle), impact_dv, ground);
        if (frac <= 0.0f) {
            return;
        }
        rf::Timestamp& cooldown = g_vehicle_state.crash_cooldown[vehicle->handle];
        if (cooldown.valid() && !cooldown.elapsed()) {
            return;
        }
        cooldown.set(vehicle_crash_server_cooldown_ms);
        vehicle_ram_mint_damage(vehicle->handle, -1, frac * vehicle_hud_max_life(vehicle), nullptr);
    }
} // namespace

void vehicle_crash_impact(rf::Entity* vehicle, float impact_dv, bool ground)
{
    if (!rf::is_multi || !vehicle || !std::isfinite(impact_dv)) {
        return;
    }
    if (rf::is_server) {
        vehicle_server_apply_crash(vehicle, impact_dv, ground);
        return;
    }
    // Only the hull this client drives is its to measure.
    if (vehicle_local_driven_vehicle() != vehicle || vehicle_hull_is_turret(vehicle)) {
        return;
    }
    const int vdc = vehicle_damage_class(vehicle);
    if (vehicle_crash_life_frac(vdc, impact_dv, ground) <= 0.0f) {
        return;
    }
    rf::Timestamp& cooldown = g_vehicle_state.crash_cooldown[vehicle->handle];
    if (cooldown.valid() && !cooldown.elapsed()) {
        return;
    }
    cooldown.set(vehicle_crash_client_cooldown_ms);
    af_send_vehicle_crash_report(vehicle->server_handle, ground,
                                 std::min(impact_dv, vehicle_crash_max_impact_dv(vdc)));
}

void vehicle_server_handle_crash_report(rf::Player* pp, int vehicle_handle, bool ground,
                                        float impact_dv)
{
    if (!rf::is_multi || !rf::is_server || !pp || !pp->net_data || pp->entity_handle == -1) {
        return;
    }
    rf::Timestamp& report_cooldown = g_vehicle_state.crash_report_cooldown[pp->net_data->player_id];
    if (report_cooldown.valid() && !report_cooldown.elapsed()) {
        return;
    }
    report_cooldown.set(vehicle_crash_report_cooldown_ms);

    if (!std::isfinite(impact_dv) || impact_dv <= 0.0f) {
        return;
    }
    rf::Entity* vehicle = vehicle_live_synced_entity(vehicle_handle);
    if (!vehicle || vehicle_hull_is_turret(vehicle)) {
        return;
    }
    // Only the seated driver simulates the hull, so only he measured the contact.
    if (vehicle_seat_leech(vehicle, 0) != pp->entity_handle) {
        return;
    }
    // Clamped, not rejected: a report may claim no more than the motion this server last saw allows.
    const int vdc = vehicle_damage_class(vehicle);
    const float observed_dv = vehicle_observed_speed_peak(vehicle->handle) * vehicle_physics_class_bounce_gain(vdc)
        + vehicle_crash_report_slack;
    vehicle_server_apply_crash(vehicle, std::min({impact_dv, vehicle_crash_max_impact_dv(vdc), observed_dv}), ground);
}

void vehicle_note_observed_speed(int vehicle_handle, float speed)
{
    if (!rf::is_server || !std::isfinite(speed)) {
        return;
    }
    VehicleObservedSpeed& s = g_vehicle_state.observed_speed[vehicle_handle];
    vehicle_observed_speed_advance(s, timer::get_i64(1000));
    s.bucket_peak[0] = std::max(s.bucket_peak[0], speed);
}

void vehicle_note_observed_pose(int vehicle_handle, const rf::Vector3& pos, float heading)
{
    if (!rf::is_multi || !vehicle_vector_is_finite(pos) || !std::isfinite(heading)) {
        return;
    }
    VehicleObservedSpeed& s = g_vehicle_state.observed_speed[vehicle_handle];
    const int64_t now = timer::get_i64(1000);
    vehicle_observed_speed_advance(s, now);
    if (!(s.reach > 0.0f)) {
        if (const rf::Entity* ep = rf::entity_from_handle(vehicle_handle)) {
            s.reach = vehicle_hull_yaw_reach(ep);
        }
    }
    if (!s.bucket_posed[0]) {
        s.bucket_pos[0] = pos;
        s.bucket_heading[0] = heading;
        s.bucket_pose_ms[0] = now;
        s.bucket_posed[0] = true;
    }
    s.newest_pos = pos;
    s.newest_heading = heading;
    s.newest_ms = now;
    // Against the last step's pose, not the previous note: a slow creep moves far less than the floor per row.
    if (!s.anchored
        || vehicle_pose_extent(s.anchor_pos, s.anchor_heading, pos, heading, s.reach)
               > vehicle_crush_change_min_extent) {
        s.changed = s.anchored;
        s.changed_ms = now;
        s.anchor_pos = pos;
        s.anchor_heading = heading;
        s.anchored = true;
    }
}

namespace
{
    // The three local axes of an OBB, in the order its half extents are stored.
    inline rf::Vector3 vehicle_obb_axis(const VehicleHullObb& box, int i)
    {
        return i == 0 ? box.orient.rvec : (i == 1 ? box.orient.uvec : box.orient.fvec);
    }

    inline float vehicle_obb_half_on(const VehicleHullObb& box, int i, float margin)
    {
        return (i == 0 ? box.half.x : (i == 1 ? box.half.y : box.half.z)) + margin;
    }

    // The projected half width of `box` (already inflated by `margin`) onto an arbitrary axis.
    float vehicle_obb_extent_on(const VehicleHullObb& box, const rf::Vector3& axis, float margin)
    {
        float e = 0.0f;
        for (int i = 0; i < 3; ++i) {
            e += vehicle_obb_half_on(box, i, margin) * std::fabs(vehicle_obb_axis(box, i).dot_prod(axis));
        }
        return e;
    }

    // A zero-length axis separates nothing: parallel edges, a case the six face axes already cover.
    bool vehicle_obb_axis_separates(const VehicleHullObb& a, const VehicleHullObb& b,
                                    const rf::Vector3& axis, float margin)
    {
        const float len_sq = axis.dot_prod(axis);
        if (len_sq < 1e-8f) {
            return false;
        }
        const rf::Vector3 n = axis / std::sqrt(len_sq);
        const float centre_gap = std::fabs((b.center - a.center).dot_prod(n));
        return centre_gap > vehicle_obb_extent_on(a, n, margin) + vehicle_obb_extent_on(b, n, margin);
    }

    // No client applies a vehicle crush locally. Every client suppresses its own; the DRIVER's
    // machine and the VICTIM's additionally report, each seeing one side of the contact first hand
    // while the server holds two interpolated bodies. A listen host mints through vehicle_server_crush.
    bool vehicle_client_report_crush(rf::Entity* victim, rf::Entity* vehicle)
    {
        if (!rf::is_multi || rf::is_server || !victim) {
            return false;
        }
        // Seated: suppress and report nothing, whatever is crushing. True rather than false, because
        // false hands him back to the stock crush and a seated man may not be crushed at all.
        if (vehicle_crush_victim_is_exempt(victim)) {
            return true;
        }
        if (!vehicle_is_synced_entity_type(vehicle)) {
            return false; // an SP-style crusher: none of this feature's business
        }
        // Suppress the local crush as for any hull, but send nothing: the server rejects it anyway.
        if (vehicle_hull_is_turret(vehicle)) {
            return true;
        }
        // A hull at rest reports nothing: 0x0041A000 crushes on a driven APC's contact alone.
        const bool can_report =
            (vehicle_local_driven_vehicle() == vehicle || victim == rf::local_player_entity)
            && vehicle_observed_motion(vehicle);
        if (can_report && rf::player_from_entity_handle(victim->handle)
            && !rf::entity_is_dying(victim)) {
            // The same window the server dedupes on, so a sliding contact is not sent per frame.
            rf::Timestamp& cooldown = g_vehicle_state.crush_cooldown[victim->handle];
            if (!cooldown.valid() || cooldown.elapsed()) {
                cooldown.set(vehicle_crush_cooldown_ms);
                af_send_vehicle_crush_report(vehicle->server_handle, victim->server_handle);
            }
        }
        return true; // suppress on every client, driver or watcher
    }

    // A player under a synced hull: the stock body minus its squash foley, which the kill record plays
    // unless he gibbed.
    bool vehicle_server_crush(rf::Entity* victim, rf::Entity* crusher)
    {
        if (!rf::is_multi || !rf::is_server || !victim || !rf::player_from_entity_handle(victim->handle)
            || !vehicle_is_synced_entity_type(crusher)) {
            return false;
        }
        if (vehicle_crush_victim_is_exempt(victim)) {
            return true;
        }
        if (vehicle_observed_motion(crusher)) {
            // The host's own contact counts as a victim report; a coasting hull credits its ex-driver.
            rf::Entity* driver = vehicle_ram_credit(crusher);
            if (driver == victim) {
                return true; // as in the sweeps, nobody runs himself over
            }
            if (driver) {
                vehicle_server_mint_crush(victim, driver, crusher);
            }
            else {
                rf::obj_damage(victim->handle, vehicle_crush_damage, -1, -1, rf::DT_CRUSH,
                               nullptr, -1, 0);
            }
        }
        return true;
    }

    // ESI is the crusher: both callers sit in 0x0041A000, which loads its param_1 into ESI and never
    // writes it again. The victim's own collide_out may already be the world's. 0x004297DD is the RET.
    CodeInjection entity_crush_damage_injection{
        0x00429790,
        [](auto& regs) {
            rf::Entity* crusher = regs.esi;
            rf::Entity* victim = addr_as_ref<rf::Entity*>(regs.esp + 4);
            if (vehicle_client_report_crush(victim, crusher) || vehicle_server_crush(victim, crusher)) {
                regs.eip = 0x004297DD;
            }
        },
    };

    // The crusher branch of 0x0041A000 compares |vel| (+0x144) and |rotvel| with 0.5, and a watched hull's
    // vel is its row's, truncated to whole u/s. Its observed rates, on the same two tests, can add the
    // crush (0x0041A0EE); stock otherwise. Replaces LEA ECX,[ESI+0x144] (6 bytes, no jump in).
    CodeInjection entity_crusher_observed_speed_injection{
        0x0041A0A2,
        [](auto& regs) {
            rf::Entity* hull = regs.esi;
            if (!rf::is_multi || !vehicle_level_has_factories() || !vehicle_is_synced_entity_type(hull)
                || !(hull->p_data.flags & rf::PF_NET_PLAYER) || vehicle_physics_drives(hull)) {
                return;
            }
            const VehicleObservedRates r = vehicle_observed_linear_and_turn_rate(hull);
            if (r.linear > stock_crusher_min_speed || r.turn > stock_crusher_min_speed) {
                regs.eip = 0x0041A0EE;
            }
        },
    };

    // The give ends in ai_select_weapon, whose fallback for an entity with no animation type - every
    // vehicle - is the hardcoded 12mm handgun, which then replaces the vehicle's own gun for good.
    FunHook<void __cdecl(rf::Item*, rf::Entity*, int, int)> item_pickup_hook{
        0x00459560,
        [](rf::Item* item, rf::Entity* ep, int do_los_check, int allow_in_multi) {
            if (rf::is_multi && vehicle_is_synced_entity_type(ep)) {
                return;
            }
            item_pickup_hook.call_target(item, ep, do_los_check, allow_in_multi);
        },
    };

    // The one DT_CRUSH not minted here: the engine's crusher damage (0x00429790). Its killer is already
    // rewritten to the SEATED driver; a coasting hull's stock crush never gets one, hence no coast lookup.
    rf::Entity* vehicle_crush_damage_roadkill_hull(int victim_handle, int killer_handle)
    {
        rf::Entity* killer = rf::entity_from_handle(killer_handle);
        if (!killer || !rf::player_from_entity_handle(killer->handle)) {
            return nullptr;
        }
        rf::Entity* vehicle = vehicle_ridden_hull(killer);
        if (!vehicle || vehicle_seat_leech(vehicle, 0) != killer->handle) {
            return nullptr;
        }
        rf::Entity* victim = rf::entity_from_handle(victim_handle);
        if (!victim || !rf::player_from_entity_handle(victim->handle)
            || vehicle_crush_victim_is_exempt(victim)) {
            return nullptr;
        }
        return vehicle;
    }

    // A fourth digit for the $Cockpit VFX hull readout; the engine's armor readout has exactly three
    // named materials. "digit4" is the fourth one ADDED: it sits LEFT of digit1, weight thousands.
    constexpr float cockpit_digit_weight_thousands = 0.001f; // 0x3A83126F, the primary strip's own
    constexpr const char* cockpit_armor_digit4_name = "cocpit_armor_digit4.tga";

    // Scoped to one call of the cockpit driver. It cannot be stateless: the clamp lives inside the
    // readout, which the driver calls before it walks any material, and which never sees the array.
    struct CockpitReadoutPass
    {
        bool has_digit4 = false;
        int armor_value = 0;
    };
    CockpitReadoutPass g_cockpit_pass;

    // The bar frame and the digits all come out of this one call, off Object::life - which on a
    // client is the obj_update mirror, clamped to 255.
    FunHook<void __cdecl(rf::Entity*, int, int*, int*)> cockpit_vfx_armor_readout_hook{
        0x004A7EF0,
        [](rf::Entity* vehicle, int num_frames, int* out_frame, int* out_value) {
            const bool sync = rf::is_multi && !rf::is_server
                              && vehicle_is_synced_entity_type(vehicle);
            const float mirrored_life = sync ? vehicle->life : 0.0f;
            if (sync) {
                vehicle->life = vehicle_hud_life(vehicle);
            }
            cockpit_vfx_armor_readout_hook.call_target(vehicle, num_frames, out_frame, out_value);
            if (sync) {
                vehicle->life = mirrored_life;
                // Digits are read modulo 10 per place, so a value with more places than the mesh has
                // materials shows the WRONG number: an APC at 5000 on a three-digit cockpit is "000".
                const int limit = g_cockpit_pass.has_digit4 ? 9999 : 999;
                if (out_value && *out_value > limit) {
                    *out_value = limit;
                }
            }
            // Captured on BOTH paths and after any clamp, so the fourth digit gets the number the
            // engine's own three are given a few instructions later.
            if (out_value) {
                g_cockpit_pass.armor_value = *out_value;
            }
        },
    };

    // Bounded compare: name[] is a fixed 33-byte array a malformed v3d could leave unterminated, and
    // the engine's own loop is a fixed-length byte match.
    bool cockpit_material_is(const rf::MeshMaterial& material, const char* name)
    {
        const char* map_name = material.texture_maps[0].name;
        return std::strncmp(map_name, name, sizeof(material.texture_maps[0].name) - 1) == 0;
    }

    // Scan, then call through, then write: the scan must precede call_target because the readout's
    // clamp runs inside it, and the write must follow it because nothing is settled before.
    FunHook<void __cdecl(rf::Entity*, int, rf::MeshMaterial*)> cockpit_vfx_update_materials_hook{
        0x004A7A80,
        [](rf::Entity* vehicle, int num_materials, rf::MeshMaterial* materials) {
            g_cockpit_pass = CockpitReadoutPass{};
            if (materials && num_materials > 0) {
                for (int i = 0; i < num_materials; ++i) {
                    if (cockpit_material_is(materials[i], cockpit_armor_digit4_name)) {
                        g_cockpit_pass.has_digit4 = true;
                        break;
                    }
                }
            }

            cockpit_vfx_update_materials_hook.call_target(vehicle, num_materials, materials);

            if (g_cockpit_pass.has_digit4) {
                const int strip_base = rf::entity_is_fighter(vehicle)
                    ? rf::cockpit_digit_strip_base_fighter
                    : rf::cockpit_digit_strip_base;
                const int frame = rf::cockpit_vfx_digit_frame(g_cockpit_pass.armor_value,
                                                              cockpit_digit_weight_thousands);
                for (int i = 0; i < num_materials; ++i) {
                    if (cockpit_material_is(materials[i], cockpit_armor_digit4_name)) {
                        materials[i].texture_maps[0].tex_handle = strip_base + frame;
                    }
                }
            }
            g_cockpit_pass = CockpitReadoutPass{};
        },
    };

    // obj_damage nullifies DT_CRUSH in MP unless the server carries a fall damage flag. The site
    // (5 bytes, absolutely addressed, no incoming jumps) is reached only once damage_type is known
    // to be 9. Stack: [esp+0x14] victim handle, [esp+0x1C] killer handle.
    CodeInjection obj_damage_vehicle_crush_injection{
        0x00489460,
        [](auto& regs) {
            const int victim_handle = addr_as_ref<int>(regs.esp + 0x14);
            const int killer_handle = addr_as_ref<int>(regs.esp + 0x1C);
            // Everything this module mints says so through its scope; the argument test is only
            // for the engine's own crusher damage, which nothing here wraps.
            if (vehicle_crush_damage_is_mint(victim_handle)
                || vehicle_crush_damage_roadkill_hull(victim_handle, killer_handle) != nullptr) {
                regs.eip = 0x004894DE;
            }
        },
    };

    // entity_die hands its 10000 explosive to occupants by walking the LOCAL player array, which
    // multi_player_create never files anyone into - so the scan reaches nobody on a dedicated server.
    // The site (5 bytes, absolute, no incoming jumps) sits past the entity_is_vehicle gate at
    // 0x00418FBA and ahead of the detach loop; 0x00419019 is the scan's own exit.
    CodeInjection entity_die_occupant_kill_injection{
        0x00418FBE,
        [](auto& regs) {
            rf::Entity* vehicle = regs.esi;
            if (!rf::is_multi || !rf::is_server || !vehicle) {
                return; // single player and clients keep the stock scan
            }
            for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
                rf::Entity* occupant = rf::entity_from_handle(player.entity_handle);
                if (occupant && occupant->host_handle == vehicle->handle) {
                    const VehicleBlastMintScope scope{occupant->handle, vehicle->handle};
                    rf::obj_damage(occupant->handle, vehicle_occupant_death_damage, -1, -1,
                                   rf::DT_EXPLOSIVE, nullptr, -1, 0);
                }
            }
            regs.eip = 0x00419019;
        },
    };

    // entity_damage sends a freshly ignited entity berserk unless its class has ignore_fire. Stock
    // exempts only use_function 1, so a flamed turret left CATATONIC and dropped its gun.
    CallHook<void(rf::AiInfo*, float, bool)> entity_damage_ignite_berserk_hook{
        0x0041A6E0,
        [](rf::AiInfo* ai, float berserk_time, bool force) {
            if (rf::is_multi && vehicle_is_synced_entity_type(ai->ep)) {
                return;
            }
            entity_damage_ignite_berserk_hook.call_target(ai, berserk_time, force);
        },
    };

    // A turret's $Corpse V3D is created on every machine unsynced, right where its factory respawns it.
    CallHook<rf::Corpse*(rf::Entity*, const char*, const rf::Vector3*, const rf::Matrix3*, int, int)>
        entity_die_corpse_create_hook{
            0x00419335,
            [](rf::Entity* ep, const char* mesh_name, const rf::Vector3* pos, const rf::Matrix3* orient,
               int unk1, int unk2) -> rf::Corpse* {
                if (rf::is_multi && vehicle_level_has_factories() && vehicle_is_synced_entity_type(ep)
                    && vehicle_hull_is_turret(ep)) {
                    return nullptr;
                }
                return entity_die_corpse_create_hook.call_target(ep, mesh_name, pos, orient, unk1, unk2);
            },
        };

    bool vehicle_damage_is_own_vehicle(const rf::Entity* victim, int killer_handle)
    {
        if (!rf::is_server || !victim || killer_handle == -1 || victim->host_handle == -1) {
            return false;
        }
        // entity_die's occupant blast names no killer, so only a shot parented to this vehicle lands here.
        if (killer_handle != victim->host_handle) {
            return false;
        }
        return vehicle_ridden_hull(victim) != nullptr;
    }

    int vehicle_resolve_damage_killer(rf::Entity* victim, int killer_handle, int damage_type)
    {
        if (!rf::is_server || !victim) {
            return killer_handle;
        }

        if (killer_handle != -1) {
            rf::Entity* killer = vehicle_synced_entity(killer_handle);
            if (killer) {
                // A vehicle handle is never a valid killer for the scoreboard.
                rf::Entity* occupant = vehicle_firing_seat_occupant(killer);
                if (!occupant) {
                    occupant = vehicle_driver_entity(killer);
                }
                if (occupant) {
                    return occupant->handle;
                }
            }
            return killer_handle;
        }

        // Crusher contact damage (0x00429790) names no killer at all; the crushing vehicle is the one
        // whose own collision result still points at this victim.
        if (damage_type == rf::DT_CRUSH) {
            if (rf::Entity* driver = vehicle_driver_entity(vehicle_crusher_of(victim))) {
                return driver->handle;
            }
            return killer_handle;
        }

        // entity_die's occupant blast (killer -1): credit whoever destroyed the hull. Only the mint scope
        // names this blow; a third-party explosive on a rider of a zero-life hull is ordinary damage.
        if (vehicle_blast_damage_is_mint(victim->handle)) {
            rf::Entity* vehicle = vehicle_synced_entity(g_vehicle_blast_mint.vehicle_handle);
            if (vehicle) {
                // An unattributed lethal blow (drowned, stranded, returned, void, own ram share) is the
                // rider's own death, whoever softened the hull first; they are paid in assists instead.
                auto lethal = g_vehicle_state.lethal_killer.find(vehicle->handle);
                if (lethal != g_vehicle_state.lethal_killer.end() && lethal->second.entity_handle == -1) {
                    return victim->handle;
                }
                // The player who destroyed it, not one who hit the wreck while it was dying.
                int credit = -1;
                rf::Entity* destroyer = lethal != g_vehicle_state.lethal_killer.end()
                    ? vehicle_lethal_killer_entity(lethal->second)
                    : nullptr;
                if (destroyer) {
                    credit = destroyer->handle;
                }
                else if (auto it = g_vehicle_state.last_damager.find(vehicle->handle);
                         it != g_vehicle_state.last_damager.end()) {
                    credit = it->second;
                }
                // The stock friendly-fire gate at 0x0048939F resolves its killer with
                // player_from_entity_handle, which does not validate -1 and so matches the first player
                // with no entity. Crediting the occupant makes the gate a no-op (killer == victim).
                rf::Player* occupant = rf::player_from_entity_handle(victim->handle);
                if (vehicle_team_damage_blocked(rf::player_from_entity_handle(credit), occupant)) {
                    return victim->handle;
                }
                // Nobody shot it down: a SUICIDE. -1 would print "killed mysteriously" and score nothing.
                return credit != -1 ? credit : victim->handle;
            }
        }
        return killer_handle;
    }

    void vehicle_note_damage(rf::Entity* victim, int killer_handle)
    {
        if (!rf::is_server || killer_handle == -1 || !vehicle_is_synced_entity_type(victim)) {
            return;
        }
        if (rf::player_from_entity_handle(killer_handle)) {
            g_vehicle_state.last_damager[victim->handle] = killer_handle;
        }
    }

    // entity_fire_weapon asks the same question the same way (WTF_MELEE at 0x00425B9E): a melee weapon
    // is never out of ammo, and with no pool configured ammo_type indexes nothing meaningful.
    bool vehicle_weapon_has_ammo_pool(int weapon_type)
    {
        if (weapon_type < 0 || weapon_type >= rf::max_weapon_types) {
            return false;
        }
        const rf::WeaponInfo& wi = rf::weapon_types[weapon_type];
        if (wi.flags & rf::WTF_MELEE) {
            return false;
        }
        if (wi.max_ammo <= 0) {
            return false;
        }
        return wi.ammo_type >= 0
            && wi.ammo_type < static_cast<int>(std::extent_v<decltype(rf::AiInfo::ammo)>);
    }

    // Slot 0 is the primary weapon, slot 1 the secondary; a turret's two triggers both drive its primary.
    bool vehicle_ammo_slot_trigger_held(const rf::Entity* ep, int slot_index)
    {
        auto it = g_vehicle_state.fire.find(ep->handle);
        if (it == g_vehicle_state.fire.end()) {
            return false;
        }
        const VehicleFireState& state = it->second;
        if (ep->info->use_function != rf::ENTITY_USE_VEHICLE) {
            return slot_index == 0 && state.any_held();
        }
        return slot_index == 0 ? state.primary_held : state.alt_held;
    }

    // Mirrors the life regen: any shot or a held trigger restarts the delay, then one round per
    // interval up to the spawn ammo. Runs after this frame's fire pass, so its shots read as a drop.
    void vehicle_ammo_regen_do_frame(rf::Entity* ep, VehicleRegen& regen, int64_t now)
    {
        for (int i = 0; i < 2; ++i) {
            VehicleAmmoRegen& slot = regen.ammo[i];
            if (slot.interval_ms <= 0 || slot.weapon_type < 0 || slot.spawn_ammo <= 0) {
                continue;
            }
            const int ammo = vehicle_weapon_ammo(ep, slot.weapon_type);
            if (ammo < slot.last_seen_ammo || vehicle_ammo_slot_trigger_held(ep, i)) {
                slot.last_fire_ms = now;
            }
            slot.last_seen_ammo = ammo;
            // The first round lands as the delay ends; a full weapon banks nothing.
            if (ammo >= slot.spawn_ammo) {
                slot.next_ms = now;
                continue;
            }
            if (now - slot.last_fire_ms < vehicle_ammo_regen_delay_ms) {
                slot.next_ms = slot.last_fire_ms + vehicle_ammo_regen_delay_ms;
                continue;
            }
            if (now < slot.next_ms) {
                continue;
            }
            slot.next_ms += slot.interval_ms;
            rf::entity_add_to_reserve_ammo(ep, slot.weapon_type, 1);
            slot.last_seen_ammo = vehicle_weapon_ammo(ep, slot.weapon_type);
        }
    }

    // Stock refills an empty fighter host by max_ammo here. On an MP client that refill is local only, so
    // the pilot kept predicting fire the server was no longer making.
    CallHook<void(rf::Entity*, int, int)> player_fire_fighter_ammo_refill_hook{
        0x004A549E,
        [](rf::Entity* host, int weapon_type, int count) {
            if (rf::is_multi && vehicle_is_synced_entity_type(host)) {
                return;
            }
            player_fire_fighter_ammo_refill_hook.call_target(host, weapon_type, count);
        },
    };

    // The primary-empty dry test ignores the alt flag, so an empty primary refuses a loaded secondary.
    // ESI is the shooter, [esp+0x6C] the alt flag; 0x004A554D is where a primary with ammo continues.
    CodeInjection player_fire_alt_skip_primary_empty_injection{
        0x004A550B,
        [](auto& regs) {
            rf::Entity* shooter = regs.esi;
            if (rf::is_multi && addr_as_ref<uint8_t>(regs.esp + 0x6C) != 0
                && vehicle_is_synced_entity_type(shooter)
                && shooter->info->use_function == rf::ENTITY_USE_VEHICLE
                && shooter->ai.current_secondary_weapon >= 0) {
                regs.eip = 0x004A554D;
            }
        },
    };
} // namespace

// The standard fifteen axes for two oriented boxes: three faces of A, three of B, nine edge-edge
// cross products. `margin` inflates both boxes on every face.
bool vehicle_hull_obb_overlap(const VehicleHullObb& a, const VehicleHullObb& b, float margin)
{
    for (int i = 0; i < 3; ++i) {
        if (vehicle_obb_axis_separates(a, b, vehicle_obb_axis(a, i), margin)) {
            return false;
        }
        if (vehicle_obb_axis_separates(a, b, vehicle_obb_axis(b, i), margin)) {
            return false;
        }
    }
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            const rf::Vector3 axis = vehicle_obb_axis(a, i).cross(vehicle_obb_axis(b, j));
            if (vehicle_obb_axis_separates(a, b, axis, margin)) {
                return false;
            }
        }
    }
    return true;
}

float vehicle_hud_life(const rf::Entity* vehicle)
{
    if (!vehicle) {
        return 0.0f;
    }
    auto it = g_vehicle_state.health.find(vehicle->handle);
    if (it != g_vehicle_state.health.end()) {
        return it->second.life;
    }
    // On a client Entity::life is the obj_update mirror, clamped to 255 and packed as one byte.
    if (rf::is_multi && !rf::is_server && vehicle->info) {
        return vehicle->info->max_life;
    }
    return vehicle->life;
}

// Order of authority: the health packet's max on a watcher, the captured spawn ceiling on the machine
// that owns the hull, then the class template - last because tbl overrides move it under a live level.
float vehicle_hud_max_life(const rf::Entity* vehicle)
{
    if (!vehicle) {
        return 0.0f;
    }
    auto health = g_vehicle_state.health.find(vehicle->handle);
    if (health != g_vehicle_state.health.end() && health->second.max_life > 0.0f) {
        return health->second.max_life;
    }
    auto regen = g_vehicle_state.regen.find(vehicle->handle);
    if (regen != g_vehicle_state.regen.end() && regen->second.spawn_max_life > 0.0f) {
        return regen->second.spawn_max_life;
    }
    return vehicle->info ? vehicle->info->max_life : 0.0f;
}

bool vehicle_is_occupant_death_blast(const rf::Entity* victim, int damage_type)
{
    // Scope, not argument shape: a rocket on a rider the frame his hull hits zero life is ordinary damage.
    return damage_type == rf::DT_EXPLOSIVE && victim && vehicle_blast_damage_is_mint(victim->handle);
}

int vehicle_occupant_death_blast_hull(int victim_handle, int damage_type)
{
    return damage_type == rf::DT_EXPLOSIVE && vehicle_blast_damage_is_mint(victim_handle)
        ? g_vehicle_blast_mint.vehicle_handle : -1;
}

bool vehicle_lethal_blow_attribution(int vehicle_handle, const rf::Player* killer_player, int& weapon_type,
                                     bool& splash, int& vehicle_class)
{
    auto it = g_vehicle_state.lethal_killer.find(vehicle_handle);
    if (it == g_vehicle_state.lethal_killer.end() || !killer_player || !killer_player->net_data
        || it->second.player_id != killer_player->net_data->player_id) {
        return false;
    }
    weapon_type = it->second.weapon_type;
    splash = it->second.splash;
    vehicle_class = it->second.vehicle_class;
    return true;
}

void vehicle_note_hull_damage(rf::Entity* vehicle, float life_before, int killer_handle,
                              float real_damage, int weapon_type, bool splash)
{
    if (!rf::is_multi || !rf::is_server || !vehicle_is_synced_entity_type(vehicle)
        || life_before <= 0.0f) {
        return; // a blow on a wreck neither kills it again nor softens it for anybody
    }
    if (vehicle->life <= 0.0f) {
        VehicleLethalKiller lethal;
        rf::Entity* killer = killer_handle != -1 ? rf::entity_from_handle(killer_handle) : nullptr;
        // A rider's own shot is his own death, as the own ram share is: suicides and assists.
        if (vehicle_ridden_hull(killer) != vehicle) {
            lethal.entity_handle = killer_handle;
            rf::Player* killer_player = killer_handle != -1 ? rf::player_from_entity_handle(killer_handle) : nullptr;
            if (killer_player && killer_player->net_data) {
                lethal.player_id = killer_player->net_data->player_id;
            }
            // A minted ram names its hull: a coasting hull's credited ex-driver is on foot.
            lethal.vehicle_class = vehicle_crush_damage_is_mint(vehicle->handle)
                ? g_vehicle_crush_mint.damage_class : -1;
            if (lethal.vehicle_class < 0) {
                lethal.vehicle_class = vehicle_occupied_damage_class(killer);
            }
            if (lethal.vehicle_class < 0 && kill_attribution_is_valid_weapon_type(weapon_type)) {
                lethal.weapon_type = weapon_type;
                lethal.splash = splash;
            }
        }
        g_vehicle_state.lethal_killer.try_emplace(vehicle->handle, lethal);
    }
    else if (real_damage > 0.0f) {
        const bool stock_indicated = splash || (kill_attribution_in_projectile_impact() && weapon_type >= 0);
        vehicle_note_hull_hit(vehicle, killer_handle, stock_indicated);
    }
    if (killer_handle == -1 || real_damage <= 0.0f) {
        return; // player_from_entity_handle does not validate -1
    }
    rf::Player* attacker = rf::player_from_entity_handle(killer_handle);
    if (!attacker || !attacker->net_data
        || vehicle_ridden_hull(rf::entity_from_handle(killer_handle)) == vehicle) {
        return;
    }
    // The riders entity_die_occupant_kill_injection will blast, found the same way.
    for (rf::Player& rider : SinglyLinkedList{rf::player_list}) {
        rf::Entity* occupant = rf::entity_from_handle(rider.entity_handle);
        if (!occupant || occupant->host_handle != vehicle->handle || &rider == attacker
            || !rider.net_data) {
            continue;
        }
        if (multi_is_team_game_type() && rider.team == attacker->team) {
            continue; // the combat chain's friendly-fire rule
        }
        kill_attribution_note_hull_damage(rider.net_data->player_id, attacker->net_data->player_id,
                                          vehicle->handle);
    }
}

float vehicle_scale_damage(int victim_handle, int killer_handle, int damage_type, float damage)
{
    // Only for a blow that NAMES a killer, which keeps the table out of the module's own lethal
    // blows and level hazards (all killer -1), so a 0.0 column cannot leave a drowning hull afloat.
    if (!rf::is_multi || !rf::is_server || killer_handle == -1) {
        return damage;
    }
    if (damage_type < 0 || damage_type >= rf::DT_COUNT) {
        return damage;
    }
    rf::Entity* victim = rf::entity_from_handle(victim_handle);
    if (!victim) {
        return damage;
    }
    // The victim is the hull itself...
    const int hull_class = vehicle_damage_class(victim);
    if (hull_class >= 0) {
        const float mult = vehicle_hull_damage_multiplier(victim, damage_type);
        if (mult < 0.0f) {
            // The blow is consumed here and ZERO continues down the call: obj_damage and
            // entity_damage subtract what they are handed with no sign test.
            vehicle_apply_repair(victim, killer_handle, damage * -mult);
            return 0.0f;
        }
        return damage * mult;
    }
    // ...or somebody riding one. host_handle -1 resolves to null and falls through untouched.
    const int seat_class = vehicle_damage_class(vehicle_ridden_hull(victim));
    if (seat_class >= 0) {
        return damage * vehicle_damage_tuning[seat_class].occupant[damage_type];
    }
    return damage;
}

bool vehicle_filter_obj_damage(int victim_handle, int* killer_handle, int damage_type)
{
    if (!rf::is_multi || !rf::is_server || !killer_handle) {
        return true;
    }
    rf::Entity* victim = rf::entity_from_handle(victim_handle);
    if (!victim) {
        return true; // clutter, movers, everything the feature has nothing to say about
    }

    // On a server with fall damage enabled the engine's own crush is not nullified, so a stock crush
    // can land on a seated player. Killer -1 is NOT excluded here: the engine's crush names nobody.
    if (damage_type == rf::DT_CRUSH && vehicle_crush_victim_is_exempt(victim)) {
        return false;
    }

    if (vehicle_damage_is_own_vehicle(victim, *killer_handle)) {
        return false;
    }
    // A negative hull multiplier already became a heal in vehicle_scale_damage, so drop the blow
    // before vehicle_note_damage names the repairer as the hull's last attacker. Killer -1 is
    // excluded because the table never scaled it.
    if (*killer_handle != -1 && vehicle_hull_damage_multiplier(victim, damage_type) < 0.0f) {
        return false;
    }
    // Ahead of the stock body, so the killer the friendly-fire gate resolves at 0x00489377 - and the
    // non-player damage scale at 0x004894B2 tests - is the responsible occupant.
    *killer_handle = vehicle_resolve_damage_killer(victim, *killer_handle, damage_type);

    if (vehicle_occupied_by_friendly(victim, *killer_handle)) {
        return false; // and no last-damager credit for a blow that never landed
    }
    vehicle_note_damage(victim, *killer_handle);
    return true;
}

void vehicle_server_handle_crush_report(rf::Player* pp, int vehicle_handle, int victim_handle)
{
    if (!rf::is_multi || !rf::is_server || !pp || !pp->net_data || pp->entity_handle == -1) {
        return;
    }
    // Per-sender cadence, taken before any lookup work: the report costs a kill mint and a broadcast.
    rf::Timestamp& report_cooldown = g_vehicle_state.crush_report_cooldown[pp->net_data->player_id];
    if (report_cooldown.valid() && !report_cooldown.elapsed()) {
        return;
    }
    report_cooldown.set(vehicle_crush_report_cooldown_ms);

    rf::Entity* vehicle = vehicle_live_synced_entity(vehicle_handle);
    if (!vehicle || vehicle_hull_is_turret(vehicle)) {
        return;
    }
    rf::Entity* victim = rf::entity_from_handle(victim_handle);
    if (!victim || victim->life <= 0.0f || rf::entity_is_dying(victim)) {
        return;
    }
    // Players only, and never anyone seated: a stale or hostile client can name a seated victim.
    if (!rf::player_from_entity_handle(victim->handle) || vehicle_crush_victim_is_exempt(victim)) {
        return;
    }
    // The report is a client CLAIM, so bound it to the hull's neighbourhood before it can mint a
    // kill. The slack is generous: the server holds only interpolated copies of both bodies.
    rf::Vector3 hull_half;
    rf::Vector3 hull_center;
    vehicle_physics_entity_hull_box(vehicle, &hull_half, &hull_center);
    const float reach =
        hull_half.len() + hull_center.len() + victim->radius + vehicle_crush_report_slack;
    if (victim->pos.distance_to(vehicle->pos) > reach) {
        return;
    }
    // Only the driver (who simulates the vehicle) and the victim (who owns his own position) see
    // this contact first hand.
    rf::Entity* driver = rf::entity_from_handle(vehicle_seat_leech(vehicle, 0));
    if (!driver) {
        // No seated driver, so a report names nobody; the coast sweep mints that case itself.
        return;
    }
    if (pp->entity_handle != driver->handle && pp->entity_handle != victim->handle) {
        return;
    }
    // The claim is only as good as the motion this server saw: rows carry |vel| in whole u/s, so poses.
    if (!vehicle_observed_motion(vehicle)) {
        return;
    }
    // The mint's own per-victim cooldown is the driver/victim dedupe.
    vehicle_server_mint_crush(victim, driver, vehicle);
}

int vehicle_roadkill_damage_class(int victim_handle, int killer_handle)
{
    if (!rf::is_multi || !rf::is_server) {
        return -1;
    }
    // The minting hull names itself; the argument derivation is left only for the stock crush.
    if (vehicle_crush_damage_is_mint(victim_handle)) {
        return g_vehicle_crush_mint.damage_class;
    }
    return vehicle_damage_class(vehicle_crush_damage_roadkill_hull(victim_handle, killer_handle));
}

bool vehicle_crush_squashes(int victim_handle, int killer_handle)
{
    if (!rf::is_multi || !rf::is_server) {
        return false;
    }
    if (vehicle_crush_damage_is_mint(victim_handle)) {
        return g_vehicle_crush_mint.squash;
    }
    return vehicle_crush_damage_roadkill_hull(victim_handle, killer_handle) != nullptr;
}

float vehicle_roadkill_speed(int victim_handle, int killer_handle)
{
    if (!rf::is_multi || !rf::is_server) {
        return 0.0f;
    }
    if (vehicle_crush_damage_is_mint(victim_handle)) {
        return g_vehicle_crush_mint.speed;
    }
    const rf::Entity* vehicle = vehicle_crush_damage_roadkill_hull(victim_handle, killer_handle);
    return vehicle ? vehicle_server_hull_velocity(vehicle).len() : 0.0f;
}

bool vehicle_roadkill_speed_gibs(int vdc_class, float speed)
{
    const float top_speed = vehicle_physics_class_top_speed(vdc_class);
    return top_speed > 0.0f && speed >= 0.75f * top_speed;
}

void vehicle_play_squash_sound(const rf::Entity* victim)
{
    if (!victim || !victim->info) {
        return;
    }
    int foley_id = victim->info->squash_sounds_id;
    if (foley_id < 0) {
        foley_id = rf::foley_lookup_by_name("Character Squash");
    }
    const int snd_handle = rf::foley_get_sound_handle(foley_id);
    if (snd_handle >= 0) {
        rf::snd_play_3d(snd_handle, victim->pos, 1.0f, rf::zero_vector, rf::SOUND_GROUP_EFFECTS);
    }
}

int vehicle_occupied_damage_class(const rf::Entity* rider)
{
    return vehicle_damage_class(vehicle_ridden_hull(rider));
}

void vehicle_before_entity_die(rf::Entity* ep)
{
    if (!rf::is_multi || !rf::is_server || !vehicle_is_synced_entity_type(ep)) {
        return;
    }
    // entity_die frees seats with entity_detach_leech, not entity_detach_from_host, so the detach
    // hook never runs on death: both the trigger drop and the seat statement must happen here.
    vehicle_server_stop_fire(ep);
    // The occupancy entity_die is about to leave behind - a null hull states "nobody is aboard it".
    vehicle_broadcast_seat_occupancy(ep->handle, nullptr, -1);
}

void vehicle_after_entity_die(rf::Entity* ep)
{
    // Runs for every entity death; every map below is keyed by a synced hull's handle.
    if (!rf::is_multi || !vehicle_is_synced_entity_type(ep)) {
        return;
    }
    vehicle_drop_combat_state(ep->handle);
    // On the EX-DRIVER's own machine nothing put PF_NET_PLAYER back on the wreck. Every machine,
    // because that one is not necessarily the server.
    vehicle_update_interp_ownership(ep);
}

// Total ammo in the terms the engine's fire gate uses (0x00428B70). -1 for a weapon with no pool -
// the status packet's own "not applicable" sentinel - so only an exact 0 means EMPTY.
int vehicle_weapon_ammo(const rf::Entity* vehicle, int weapon_type)
{
    if (!vehicle || !vehicle_weapon_has_ammo_pool(weapon_type)) {
        return -1;
    }
    const int ammo_type = rf::weapon_types[weapon_type].ammo_type;
    return vehicle->ai.clip_ammo[weapon_type] + vehicle->ai.ammo[ammo_type];
}

void vehicle_ammo_regen_runtime_reset()
{
    // -1, not 0: zero is a legal weapon type id, so an unresolved row must not match one.
    std::fill(std::begin(g_vehicle_ammo_regen_type), std::end(g_vehicle_ammo_regen_type), -1);
    g_vehicle_ammo_regen_resolved = false;
}

// Captured once at creation: entity_create has just granted the class's weapons, nothing has fired.
void vehicle_capture_spawn_ammo(rf::Entity* ep)
{
    vehicle_resolve_ammo_regen_types();
    VehicleRegen& regen = g_vehicle_state.regen[ep->handle];
    const int channels[2] = {ep->ai.current_primary_weapon, ep->ai.current_secondary_weapon};
    for (int i = 0; i < 2; ++i) {
        VehicleAmmoRegen& slot = regen.ammo[i];
        slot = VehicleAmmoRegen{};
        const int wt = channels[i];
        if (wt < 0 || wt >= rf::max_weapon_types) {
            continue;
        }
        for (std::size_t row = 0; row < std::size(vehicle_weapon_ammo_regen); ++row) {
            if (g_vehicle_ammo_regen_type[row] != wt) {
                continue;
            }
            slot.weapon_type = wt;
            slot.spawn_ammo = vehicle_weapon_ammo(ep, wt);
            slot.interval_ms = vehicle_weapon_ammo_regen[row].interval_ms;
            slot.last_seen_ammo = slot.spawn_ammo;
            break;
        }
    }
}

// Constant-rate hull regeneration back to the life this vehicle SPAWNED with. The damage clock is
// driven by an OBSERVED drop in life, not by a hook: every source that can hurt a hull writes that
// one field, so "any damage resets the clock" holds with no second mechanism.
void vehicle_regen_do_frame(rf::Entity* ep)
{
    auto it = g_vehicle_state.regen.find(ep->handle);
    if (it == g_vehicle_state.regen.end()) {
        return;
    }
    VehicleRegen& regen = it->second;
    const int64_t now = timer::get_i64(1000);

    // Ammo first: it keeps its own fire clock, not the life regen's, and runs whatever the hull's health.
    if (ep->life > 0.0f && !rf::entity_is_dying(ep)) {
        vehicle_ammo_regen_do_frame(ep, regen, now);
    }

    // Never resurrect: a hull at or below zero life, or already playing its death, is finished.
    if (ep->life <= 0.0f || rf::entity_is_dying(ep)) {
        regen.last_seen_life = ep->life;
        return;
    }

    if (ep->life < regen.last_seen_life - 0.01f) {
        regen.last_damage_ms = now;
    }
    regen.last_seen_life = ep->life;

    if (regen.spawn_max_life <= 0.0f || ep->life >= regen.spawn_max_life) {
        return;
    }
    if (now - regen.last_damage_ms < vehicle_regen_delay_ms) {
        return;
    }
    // A hull on the drown or strand clock is not on the repair clock.
    if (g_vehicle_state.hazard_since.count(ep->handle) != 0) {
        return;
    }

    ep->life = std::min(ep->life + vehicle_regen_rate * rf::frametime, regen.spawn_max_life);
    regen.last_seen_life = ep->life;
}

void vehicle_damage_apply_patch()
{
    entity_crush_damage_injection.install();
    entity_crusher_observed_speed_injection.install();
    obj_damage_vehicle_crush_injection.install();
    entity_die_occupant_kill_injection.install();
    entity_damage_ignite_berserk_hook.install();
    entity_die_corpse_create_hook.install();
    item_pickup_hook.install();
    cockpit_vfx_armor_readout_hook.install();
    cockpit_vfx_update_materials_hook.install();
    player_fire_fighter_ammo_refill_hook.install();
    player_fire_alt_skip_primary_empty_injection.install();
}
