#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>
#include <string>
#include <vector>
#include <xlog/xlog.h>
#include <patch_common/MemUtils.h>
#include <common/utils/list-utils.h>
#include <common/utils/string-utils.h>
#include <common/vehicle_meshes.h>
#include "vehicle.h"
#include "vehicle_internal.h"
#include "vehicle_spawn.h"
#include "../alpine_packets.h"
#include "../gametype.h"
#include "../../misc/level.h"
#include "../../object/alpine_obj_common.h"
#include "../../os/console.h"
#include "../../os/os.h"
#include "../../rf/entity.h"
#include "../../rf/file/file.h"
#include "../../rf/multi.h"
#include "../../rf/object.h"
#include "../../rf/os/console.h"
#include "../../rf/player/player.h"
#include "../../rf/vmesh.h"
#include "../../rf/weapon.h"

namespace
{
    constexpr const char* default_debug_vehicle_class = "Jeep01";
    constexpr float debug_spawn_distance = 5.0f;

    // Consecutive failed spawns before the slot logs once; it keeps retrying at the interval below.
    constexpr int vehicle_max_respawn_retries = 5;
    constexpr int vehicle_respawn_retry_interval_ms = 2000;
    // A slot that can never spawn retries forever; its PENDING re-broadcast is throttled to this.
    constexpr int64_t vehicle_retry_announce_interval_ms = 10000;
    constexpr int vehicle_max_respawn_delay_ms = 3600000;

    rf::Entity* vehicle_spawn_type(int entity_type, const rf::Vector3& pos, const rf::Matrix3& orient)
    {
        // Also the bounds check that makes indexing entity_types below safe.
        if (!vehicle_is_synced_entity_type(entity_type)) {
            return nullptr;
        }
        // A copy: entity_create uprights the matrix it is handed in place.
        rf::Matrix3 create_orient = orient;
        rf::Entity* ep = rf::entity_create(entity_type, rf::entity_types[entity_type].name.c_str(), -1,
                                           pos, create_orient, 0, -1);
        if (!ep) {
            return nullptr;
        }
        vehicle_init_synced_entity(ep);
        track_synced_entity(ep);
        rf::send_entity_create_packet_to_all(ep);
        return ep;
    }

    // The one derivation both the event announcements and the join sweep are built from.
    std::pair<uint8_t, uint16_t> vehicle_slot_wire_state(const VehicleSpawnSlot& slot)
    {
        if (slot.given_up) {
            return {AF_VEHICLE_FACTORY_GIVEN_UP, 0};
        }
        if (slot.handle != -1) {
            // A turret is free the moment its gunner leaves, so its line reads occupancy, not
            // entered_once: TAKEN is "IN USE" on the client and READY comes back on every exit.
            const bool taken = slot.is_turret
                ? vehicle_hull_occupied(vehicle_live_synced_entity(slot.handle))
                : slot.entered_once;
            return {taken ? AF_VEHICLE_FACTORY_ALIVE_TAKEN : AF_VEHICLE_FACTORY_ALIVE_READY, 0};
        }
        const int left_ms = slot.respawn_timer.valid() ? slot.respawn_timer.time_until() : 0;
        // Seconds, rounded up: a whole-second wire field must never finish ahead of the server.
        const int left_s = (std::max(left_ms, 0) + 999) / 1000;
        return {AF_VEHICLE_FACTORY_PENDING, static_cast<uint16_t>(std::min(left_s, 65535))};
    }

    // The factory record is the authority on affiliation; an entered hull takes its boarders' team.
    uint8_t vehicle_factory_wire_team(int factory_index)
    {
        const AlpineVehicleFactoryInfo* info = vehicle_factory(factory_index);
        if (!info || info->team < 0) {
            return af_vehicle_state_team_none;
        }
        return static_cast<uint8_t>(info->team);
    }

    void vehicle_slot_announce(const VehicleSpawnSlot& slot)
    {
        if (slot.factory_index < 0) {
            return;
        }
        const auto index = static_cast<uint16_t>(slot.factory_index);
        const auto [state, s_remaining] = vehicle_slot_wire_state(slot);
        const uint8_t team = vehicle_factory_wire_team(slot.factory_index);
        af_send_vehicle_factory_state_packet_to_all(index, state, s_remaining, team);
        // The broadcast skips the host's own player, so a listen server feeds its mirror here.
        if (!rf::is_dedicated_server) {
            vehicle_apply_factory_state_from_packet(index, state, s_remaining, team);
        }
    }

    bool vehicle_slot_try_spawn(VehicleSpawnSlot& slot, bool announce)
    {
        if (rf::Entity* ep = vehicle_spawn_type(slot.entity_type, slot.pos, slot.orient)) {
            slot.handle = ep->handle;
            slot.failures = 0;
            slot.respawn_timer.invalidate();
            slot.entered_once = false;
            const AlpineVehicleFactoryInfo* info = vehicle_factory(slot.factory_index);
            VehicleState& st = g_vehicle_state.hull_state[ep->handle];
            st.team = info ? info->team : -1;
            st.lock_to_team = info && info->lock_to_team;
            if (announce) {
                vehicle_slot_announce(slot);
                // Nothing else states a fresh hull's team and lock; the join sweep covers a
                // silent level-init spawn.
                vehicle_broadcast_seat_occupancy(ep->handle, ep, -1);
            }
            return true;
        }

        // Only a class that can NEVER spawn latches; a null from entity_create is transient.
        if (!vehicle_is_synced_entity_type(slot.entity_type)) {
            slot.given_up = true;
            slot.respawn_timer.invalidate();
            xlog::error("[vehicle] entity type {} is not a spawnable vehicle or turret class; "
                        "giving up for this level", slot.entity_type);
            if (announce) {
                vehicle_slot_announce(slot);
            }
            return false;
        }
        if (++slot.failures == vehicle_max_respawn_retries) {
            xlog::warn("[vehicle] entity type {} has failed to spawn {} times; still retrying",
                       slot.entity_type, vehicle_max_respawn_retries);
        }
        slot.respawn_timer.set(vehicle_respawn_retry_interval_ms);
        if (announce) {
            const int64_t now = timer::get_i64(1000);
            if (slot.failures == 1
                || now - slot.last_retry_announce_ms >= vehicle_retry_announce_interval_ms) {
                slot.last_retry_announce_ms = now;
                vehicle_slot_announce(slot);
            }
        }
        return false;
    }
} // namespace

void vehicle_slot_do_frame(VehicleSpawnSlot& slot)
{
    if (slot.handle != -1) {
        rf::Entity* ep = rf::entity_from_handle(slot.handle);
        if (ep && !rf::entity_is_dying(ep)) {
            return;
        }
        // Dying counts as gone: the wreck plays out while the timer runs.
        slot.handle = -1;
        slot.respawn_timer.set(slot.factory_delay_ms);
        vehicle_slot_announce(slot);
        return;
    }
    if (slot.given_up || !slot.respawn_timer.valid() || !slot.respawn_timer.elapsed()) {
        return;
    }
    vehicle_slot_try_spawn(slot, true);
}

VehicleSpawnSlot* vehicle_slot_for_hull(int vehicle_handle)
{
    if (vehicle_handle == -1) {
        return nullptr;
    }
    for (VehicleSpawnSlot& slot : g_vehicle_state.factories) {
        if (slot.handle == vehicle_handle) {
            return &slot;
        }
    }
    return nullptr;
}

void vehicle_slot_announce_for_hull(int vehicle_handle)
{
    if (!rf::is_server) {
        return;
    }
    const VehicleSpawnSlot* slot = vehicle_slot_for_hull(vehicle_handle);
    // Only a turret's wire state depends on occupancy; every other slot would re-send an unchanged one.
    if (slot && slot->is_turret) {
        vehicle_slot_announce(*slot);
    }
}

void vehicle_on_hull_boarded(rf::Entity* vehicle, const rf::Entity* rider)
{
    if (!rf::is_server || !vehicle) {
        return;
    }
    VehicleState* st = vehicle_hull_state(vehicle->handle);
    if (st) {
        st->entered_once = true;
        st->unoccupied_since_ms = 0;
        st->unoccupied_deadline_ms = 0;
        // The hull takes its boarder's team and keeps it once empty. A locked, teamed hull only ever
        // admits its own team; a team gained by boarding carries no lock, so a neutral hull stays
        // re-claimable.
        const rf::Player* pp = rider && multi_is_team_game_type()
            ? rf::player_from_entity_handle(rider->handle) : nullptr;
        if (pp) {
            const int team = pp->team == 1 ? 1 : 0;
            if (st->team != team) {
                st->team = team;
                st->lock_to_team = false;
            }
        }
    }
    VehicleSpawnSlot* slot = vehicle_slot_for_hull(vehicle->handle);
    if (!slot || slot->entered_once) {
        return;
    }
    // The factory's TAKEN announcement.
    slot->entered_once = true;
    vehicle_slot_announce(*slot);
}

int vehicle_factory_index_by_uid(int uid)
{
    for (std::size_t i = 0; i < g_vehicle_factories.size(); ++i) {
        if (g_vehicle_factories[i].uid == uid) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void vehicle_factory_set_team(int factory_index, int team, bool spawn_waiting)
{
    if (!rf::is_multi || !rf::is_server) {
        return;
    }
    if (factory_index < 0 || factory_index >= static_cast<int>(g_vehicle_factories.size())) {
        return;
    }
    if (team != rf::TEAM_RED && team != rf::TEAM_BLUE) {
        team = -1;
    }
    // Re-applying the same ownership must not re-spawn or re-announce anything.
    if (g_vehicle_factories[factory_index].team == team) {
        return;
    }
    g_vehicle_factories[factory_index].team = team;

    for (VehicleSpawnSlot& slot : g_vehicle_state.factories) {
        if (slot.factory_index != factory_index) {
            continue;
        }
        if (!slot.entered_once) {
            rf::Entity* ep = vehicle_live_synced_entity(slot.handle);
            VehicleState* st = ep ? vehicle_hull_state(ep->handle) : nullptr;
            if (st) {
                st->team = team;
                if (spawn_waiting) {
                    vehicle_broadcast_seat_occupancy(ep->handle, ep, -1);
                }
            }
        }
        // A capture that flips a waiting factory hands its new owners a vehicle at once.
        if (spawn_waiting && team >= 0 && slot.handle == -1 && !slot.given_up) {
            const rf::Timestamp pending = slot.respawn_timer;
            // Silent: the announce below is the one 0x69 this capture sends, and it must state the
            // timer AFTER the restore, not the short retry interval try_spawn sets for itself.
            const bool spawned = vehicle_slot_try_spawn(slot, false);
            if (!spawned && !slot.given_up && pending.valid()) {
                slot.respawn_timer = pending;
            }
            vehicle_slot_announce(slot);
            if (rf::Entity* ep = spawned ? vehicle_live_synced_entity(slot.handle) : nullptr) {
                // Nothing else states the fresh hull's team and lock.
                vehicle_broadcast_seat_occupancy(ep->handle, ep, -1);
            }
            continue;
        }
        // The team byte rides this packet, so a slot whose respawn state did not change still sends.
        if (spawn_waiting) {
            vehicle_slot_announce(slot);
        }
    }
}

// The reliable stream packs consecutive sends into one datagram (0x00479480 buffers to 0x200 bytes),
// so a per-factory sweep costs a handful of datagrams even at the 1000 factory cap.
void vehicle_send_factory_states_to(rf::Player* pp)
{
    if (!rf::is_server || !pp) {
        return;
    }
    for (const VehicleSpawnSlot& slot : g_vehicle_state.factories) {
        if (slot.factory_index < 0) {
            continue;
        }
        const auto [state, s_remaining] = vehicle_slot_wire_state(slot);
        af_send_vehicle_factory_state_packet(pp, static_cast<uint16_t>(slot.factory_index), state,
                                             s_remaining,
                                             vehicle_factory_wire_team(slot.factory_index));
    }
}

namespace
{
    rf::Entity* debug_spawn_anchor(std::optional<std::string> player_name)
    {
        if (player_name && !player_name->empty()) {
            rf::Player* pp = find_best_matching_player(player_name->c_str());
            return pp ? rf::entity_from_handle(pp->entity_handle) : nullptr;
        }
        if (rf::local_player) {
            if (rf::Entity* ep = rf::entity_from_handle(rf::local_player->entity_handle)) {
                return ep;
            }
        }
        for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
            if (&player == rf::local_player || player.is_browser) {
                continue;
            }
            if (rf::Entity* ep = rf::entity_from_handle(player.entity_handle)) {
                return ep;
            }
        }
        return nullptr;
    }

    ConsoleCommand2 dbg_vehicle_spawn_cmd{
        "dbg_vehicle_spawn",
        [](std::optional<std::string> class_name, std::optional<std::string> player_name) {
            if (!rf::is_multi || !rf::is_server) {
                rf::console::print("This command is only available on a multiplayer server.");
                return;
            }
            rf::Entity* anchor = debug_spawn_anchor(player_name);
            if (!anchor) {
                rf::console::print("No player entity to spawn a vehicle in front of.");
                return;
            }

            const std::string cls = class_name.value_or(default_debug_vehicle_class);
            const rf::Vector3 pos = anchor->pos + anchor->orient.fvec * debug_spawn_distance;
            rf::Entity* vehicle = vehicle_spawn(cls.c_str(), pos, anchor->orient);
            if (vehicle) {
                rf::console::print("Spawned {} (handle {}).", cls, vehicle->handle);
            }
            else {
                rf::console::print("Could not spawn {}.", cls);
            }
        },
        "Spawn a vehicle or turret entity in front of a player (server only)",
        "dbg_vehicle_spawn [class] [player]",
    };
} // namespace

rf::Entity* vehicle_spawn(const char* class_name, const rf::Vector3& pos, const rf::Matrix3& orient)
{
    if (!rf::is_multi || !rf::is_server || !class_name) {
        return nullptr;
    }

    const int entity_type = rf::entity_lookup_type(class_name);
    if (!vehicle_is_synced_entity_type(entity_type)) {
        return nullptr;
    }

    return vehicle_spawn_type(entity_type, pos, orient);
}

// Must stay in step with the editor's vehicle_factory_serialize_chunk.
void vehicle_factory_load_chunk(rf::File& file, std::size_t chunk_len)
{
    std::size_t remaining = chunk_len;
    rf::File::ChunkGuard chunk_guard{file, remaining};

    AlpineChunkReader reader{file, remaining};

    // Untrusted input reaching a server-side Bullet body: a non-finite value is UB and a
    // non-rotation basis shears every derived box.
    auto pose_is_sane = [](const AlpineVehicleFactoryInfo& info) {
        if (!vehicle_vector_is_finite(info.pos)) {
            return false;
        }
        if (!alpine_orient_is_sane(info.orient)) {
            return false;
        }
        return vehicle_orient_is_rotation(info.orient);
    };
    int rejected = 0;

    uint32_t count = 0;
    if (!reader.read_bytes(&count, sizeof(count))) {
        xlog::warn("[vehicle] failed to read factory count from chunk (len={})", chunk_len);
        return;
    }
    count = std::min(count, vehicle_factory_max_records);

    for (uint32_t i = 0; i < count; ++i) {
        AlpineVehicleFactoryInfo info;
        if (!reader.read_bytes(&info.uid, sizeof(info.uid))) return;
        if (!reader.read_bytes(&info.pos.x, sizeof(float))) return;
        if (!reader.read_bytes(&info.pos.y, sizeof(float))) return;
        if (!reader.read_bytes(&info.pos.z, sizeof(float))) return;
        if (!reader.read_bytes(&info.orient.rvec.x, sizeof(float))) return;
        if (!reader.read_bytes(&info.orient.rvec.y, sizeof(float))) return;
        if (!reader.read_bytes(&info.orient.rvec.z, sizeof(float))) return;
        if (!reader.read_bytes(&info.orient.uvec.x, sizeof(float))) return;
        if (!reader.read_bytes(&info.orient.uvec.y, sizeof(float))) return;
        if (!reader.read_bytes(&info.orient.uvec.z, sizeof(float))) return;
        if (!reader.read_bytes(&info.orient.fvec.x, sizeof(float))) return;
        if (!reader.read_bytes(&info.orient.fvec.y, sizeof(float))) return;
        if (!reader.read_bytes(&info.orient.fvec.z, sizeof(float))) return;
        if (!reader.read_string(info.script_name)) return;
        if (!reader.read_string(info.vehicle_class)) return;
        if (!reader.read_bytes(&info.respawn_delay_s, sizeof(float))) return;
        // Untrusted float: out of range it is UB in the later static_cast<int>(x*1000).
        if (!std::isfinite(info.respawn_delay_s) || info.respawn_delay_s < 0.0f) {
            info.respawn_delay_s = 0.0f;
        }
        info.respawn_delay_s =
            std::min(info.respawn_delay_s, static_cast<float>(vehicle_max_respawn_delay_ms) / 1000.0f);
        uint8_t team = 0xFF;
        if (!reader.read_bytes(&team, sizeof(team))) return;
        info.team = (team == 0 || team == 1) ? static_cast<int32_t>(team) : -1;
        uint8_t reserved[2] = {};
        if (!reader.read_bytes(reserved, sizeof(reserved))) return;
        uint8_t lock_to_team = 0;
        if (!reader.read_bytes(&lock_to_team, sizeof(lock_to_team))) return;
        // A team-none factory keeps its lock: a control point can hand it a team later, and
        // vehicle_locked_against ignores a lock while the hull's team is none. A team gained by
        // boarding clears the hull's copy.
        info.lock_to_team = lock_to_team != 0;
        uint8_t active = 1;
        if (!reader.read_bytes(&active, sizeof(active))) return;
        info.active_by_default = active != 0;

        // The whole record is read before either rejection so the stream stays in step.
        if (!pose_is_sane(info)) {
            ++rejected;
            continue;
        }
        // The clamp above bounds one chunk; a level carrying several would otherwise append past it.
        if (g_vehicle_factories.size() >= vehicle_factory_max_records) {
            ++rejected;
            continue;
        }

        g_vehicle_factories.push_back(std::move(info));
    }

    if (rejected > 0) {
        xlog::warn("[vehicle] rejected {} factory record(s) with an invalid pose or past the {} record limit",
                   rejected, vehicle_factory_max_records);
    }
}

// Factories are the sole source of vehicles and turrets. Parsed on client and server alike, before
// any level-init hook runs.
bool vehicle_level_has_factories()
{
    return !g_vehicle_factories.empty();
}

void vehicle_factory_clear_state()
{
    g_vehicle_factories.clear();
}

void vehicle_level_init_post()
{
    // Before the server gate: the client mirror exists on every machine that parsed the chunk.
    g_vehicle_state.factory_ui.assign(g_vehicle_factories.size(), VehicleFactoryUi{});

    if (!rf::is_multi || !rf::is_server) {
        return;
    }

    for (std::size_t i = 0; i < g_vehicle_factories.size(); ++i) {
        const AlpineVehicleFactoryInfo& info = g_vehicle_factories[i];
        const int entity_type = rf::entity_lookup_type(info.vehicle_class.c_str());
        if (!vehicle_is_synced_entity_type(entity_type)) {
            xlog::warn("[vehicle] factory (uid {}): '{}' is not a vehicle or turret entity class",
                       info.uid, info.vehicle_class);
            continue;
        }

        VehicleSpawnSlot slot;
        slot.entity_type = entity_type;
        slot.factory_index = static_cast<int>(i);
        slot.is_turret = rf::entity_types[entity_type].use_function == rf::ENTITY_USE_TURRET;
        slot.pos = info.pos;
        slot.orient = info.orient;
        slot.factory_delay_ms = std::clamp(static_cast<int>(info.respawn_delay_s * 1000.0f), 0,
                                           vehicle_max_respawn_delay_ms);
        g_vehicle_state.factories.push_back(slot);
    }

    // Silent: a client resets its own mirror when it finishes loading, which is after this runs, and
    // then gets every slot's state from the state-info sweep (vehicle_send_factory_states_to).
    for (VehicleSpawnSlot& slot : g_vehicle_state.factories) {
        if (g_vehicle_factories[slot.factory_index].active_by_default) {
            vehicle_slot_try_spawn(slot, false);
            continue;
        }
        slot.respawn_timer.set(slot.factory_delay_ms);
        if (!rf::is_dedicated_server) {
            const auto index = static_cast<uint16_t>(slot.factory_index);
            const auto [state, s_remaining] = vehicle_slot_wire_state(slot);
            vehicle_apply_factory_state_from_packet(index, state, s_remaining,
                                                    vehicle_factory_wire_team(slot.factory_index));
        }
    }
}

// tbl overrides for vehicle levels: applied to the LIVE parsed tbl structures for the duration of a
// vehicle level and written back verbatim afterwards.
namespace
{
    struct VehicleWeaponOverride
    {
        const char* name;
        std::optional<float> velocity;      // $Velocity
        std::optional<float> fire_wait;     // $Fire Wait, in SECONDS (0x004C8710 scales it x1000)
        std::optional<float> damage_multi;  // $Damage Multi
        std::optional<int> max_ammo_multi;  // 2nd field of $Max Ammo
        std::optional<float> damage_radius; // $Damage Radius
        std::optional<float> crater_radius; // +Crater Radius
        bool clear_homing;                  // $Homing: false
    };

    // Exactly the mods/gg diff. Anything not listed keeps its stock value.
    constexpr VehicleWeaponOverride vehicle_weapon_overrides[] = {
        {"Torpedo",         17.0f, 1.25f, 275.0f, 5,   12.0f, 8.5f,  true},
        {"Fighter Rocket",  {},    2.0f,  275.0f, 5,   {},    10.0f, true},
        {"APC Rocket",      {},    {},    275.0f, 5,   {},    {},    false},
        {"APC Minigun",     {},    {},    45.0f,  100, {},    {},    false},
        {"Jeep Gun",        {},    {},    45.0f,  150, {},    {},    false},
        {"Fighter Minigun", {},    {},    45.0f,  100, {},    {},    false},
        {"Vauss",           {},    {},    45.0f,  150, {},    {},    false},
    };

    struct VehicleEntityOverride
    {
        const char* name;
        float max_life; // $Life
    };

    constexpr VehicleEntityOverride vehicle_entity_overrides[] = {
        {"sub", 900.0f},
        {"APC", 6000.0f},
        {"Jeep01", 1250.0f},
        {"Fighter01", 1500.0f},
        {"masako_fighter", 2000.0f},
        {"Driller01", 7500.0f},
    };

    struct VehicleWeaponSaved
    {
        int weapon_type;
        float max_speed, max_speed_multi, max_speed_single, lifetime_mul_vel;
        float fire_wait, damage_multi;
        float damage_radius, damage_radius_single, damage_radius_multi;
        float crater_radius;
        int max_ammo, max_ammo_multi;
        int flags;
    };

    struct VehicleEntitySaved
    {
        int entity_type;
        float max_life;
    };

    struct VehicleEntityCockpitOverride
    {
        const char* class_name;           // entity.tbl class name, as in alpine_vehicle_class_meshes
        const char* cockpit_vfx_filename; // what $Cockpit VFX becomes for the level
    };

    // The hull meshes themselves come from alpine_vehicle_class_meshes, which the editor shares;
    // only these classes also get a new cockpit.
    constexpr VehicleEntityCockpitOverride vehicle_entity_cockpit_overrides[] = {
        {"APC", "af_APC.vfx"},
        {"Fighter01", "af_fighter01.vfx"},
    };

    const char* vehicle_cockpit_vfx_for(const char* class_name)
    {
        for (const VehicleEntityCockpitOverride& o : vehicle_entity_cockpit_overrides) {
            if (string_iequals(o.class_name, class_name)) {
                return o.cockpit_vfx_filename;
            }
        }
        return nullptr;
    }

    struct VehicleEntityMeshSaved
    {
        int entity_type;
        std::string vmesh_filename; // OUR copy of the bytes; the engine String itself is not held
        // A field we never overrode has nothing to restore, and restoring is an engine-heap write.
        bool cockpit_overridden = false;
        std::string cockpit_vfx_filename;
    };

    // The jeep's mounted gun has no tbl field at all: entity_create loads it from a filename baked
    // into the instruction stream, once per level on the first jeep created. The operand is
    // REPOINTED rather than the bytes overwritten - the stock literal has no room to grow into.
    constexpr uintptr_t jeep_gun_mesh_push_imm32 = 0x00423A45;
    constexpr uint32_t jeep_gun_mesh_stock_string = 0x00595AD8; // "jeep_gun.v3d" in .rdata
    constexpr char af_jeep_gun_mesh_filename[] = "af_jeep_gun.v3m";
    bool g_jeep_gun_mesh_repointed = false;

    // Seated clips appended to $EntityAnimType state lists as if entity.tbl listed them: entity_create
    // (0x00422360) resolves each state BY NAME from that list. miner1 has the jeep clips but not
    // on_turret; the other three MP rigs have none, so theirs are retargets of the miner1 clips.
    struct VehicleRiderAnim
    {
        const char* anim_type;
        rf::EntityState state;
        const char* state_name; // the engine's name for `state` (table at 0x0062F208)
        const char* filename;
    };

    constexpr VehicleRiderAnim vehicle_rider_anims[] = {
        {"miner1", rf::ENTITY_STATE_ON_TURRET, "on_turret", "ult2_on_turret.rfa"},
        {"multi_female", rf::ENTITY_STATE_JEEP_DRIVE, "jeep_drive", "af_female_jeep_driver.rfa"},
        {"multi_female", rf::ENTITY_STATE_JEEP_GUN, "jeep_gun", "af_female_jeep_gunner.rfa"},
        {"multi_female", rf::ENTITY_STATE_ON_TURRET, "on_turret", "af_female_on_turret.rfa"},
        {"multi_merc", rf::ENTITY_STATE_JEEP_DRIVE, "jeep_drive", "af_merc_jeep_driver.rfa"},
        {"multi_merc", rf::ENTITY_STATE_JEEP_GUN, "jeep_gun", "af_merc_jeep_gunner.rfa"},
        {"multi_merc", rf::ENTITY_STATE_ON_TURRET, "on_turret", "af_merc_on_turret.rfa"},
        {"multi_civilian", rf::ENTITY_STATE_JEEP_DRIVE, "jeep_drive", "af_civilian_jeep_driver.rfa"},
        {"multi_civilian", rf::ENTITY_STATE_JEEP_GUN, "jeep_gun", "af_civilian_jeep_gunner.rfa"},
        {"multi_civilian", rf::ENTITY_STATE_ON_TURRET, "on_turret", "af_civilian_on_turret.rfa"},
    };

    // The tbl parser (0x0041B910) allocates every state list at its fixed 23 entries.
    constexpr int entity_info_state_anims_capacity = 23;
    static_assert(entity_info_state_anims_capacity == rf::ENTITY_STATE_CUSTOM + 1);

    struct VehicleRiderAnimApplied
    {
        int entity_type;
        int info_index;
        const VehicleRiderAnim* anim;
    };

    std::vector<VehicleWeaponSaved> g_vehicle_tbl_weapon_saved;
    std::vector<VehicleEntitySaved> g_vehicle_tbl_entity_saved;
    std::vector<VehicleEntityMeshSaved> g_vehicle_tbl_mesh_saved;
    std::vector<VehicleRiderAnimApplied> g_vehicle_tbl_rider_anims;

    bool entity_info_has_state_anim(const rf::EntityInfo& ei, const char* state_name)
    {
        for (int i = 0; i < ei.num_state_anims; ++i) {
            if (string_iequals(ei.state_anims[i].name.c_str(), state_name)) {
                return true;
            }
        }
        return false;
    }

    void vehicle_rider_anims_apply()
    {
        for (const VehicleRiderAnim& a : vehicle_rider_anims) {
            const int et = rf::entity_lookup_type(a.anim_type);
            if (et < 0 || et >= rf::num_entity_types) {
                continue;
            }
            rf::EntityInfo& ei = rf::entity_types[et];
            if (!ei.state_anims || ei.num_state_anims < 0 || ei.num_state_anims >= entity_info_state_anims_capacity
                || entity_info_has_state_anim(ei, a.state_name)) {
                continue;
            }
            // A missing clip would map the seat to the mesh's clip 0, so this rig keeps standing instead.
            if (!rf::File{}.find(a.filename)) {
                xlog::warn("[vehicle] rider anim '{}' not found", a.filename);
                continue;
            }
            const int index = ei.num_state_anims;
            rf::EntityAnimInfo& info = ei.state_anims[index];
            info.name = a.state_name;
            info.anim_filename = a.filename;
            info.num_triggers = 0;
            ei.num_state_anims = index + 1;
            g_vehicle_tbl_rider_anims.push_back({et, index, &a});
        }
    }

    // LIFO, so each list's count steps back over exactly what was appended to it.
    void vehicle_rider_anims_revert()
    {
        for (auto it = g_vehicle_tbl_rider_anims.rbegin(); it != g_vehicle_tbl_rider_anims.rend(); ++it) {
            if (it->entity_type < 0 || it->entity_type >= rf::num_entity_types) {
                continue;
            }
            rf::EntityInfo& ei = rf::entity_types[it->entity_type];
            if (ei.num_state_anims != it->info_index + 1) {
                continue;
            }
            rf::EntityAnimInfo& info = ei.state_anims[it->info_index];
            info.name = rf::String{};
            info.anim_filename = rf::String{};
            ei.num_state_anims = it->info_index;
        }
        g_vehicle_tbl_rider_anims.clear();
    }

    // level_load creates a listen server's own entity (0x0045C807) before level_init_post, so it
    // resolved its state clips without the appended entries. Every other entity comes later.
    void vehicle_rider_anims_attach_local_entity()
    {
        rf::Player* pp = rf::local_player;
        rf::Entity* ep = pp ? rf::entity_from_handle(pp->entity_handle) : nullptr;
        if (!ep || !ep->vmesh || ep->vmesh->type != rf::MESH_TYPE_CHARACTER || !ep->vmesh->mesh) {
            return;
        }
        const int character = ep->mp_character_id;
        if (character < 0 || character >= rf::num_multi_characters) {
            return;
        }
        const int anim_type = rf::mp_characters[character].anim_entity_type;
        for (const VehicleRiderAnimApplied& a : g_vehicle_tbl_rider_anims) {
            if (a.entity_type != anim_type || ep->state_anims[a.anim->state].vmesh_anim_index != -1) {
                continue;
            }
            const int anim_index = rf::character_mesh_load_action(ep->vmesh->mesh, a.anim->filename, 1, 0);
            for (rf::EntityAnim* slot : {&ep->state_anims[a.anim->state], &ep->default_state_anims[a.anim->state]}) {
                slot->vmesh_anim_index = anim_index;
                slot->info_index = a.info_index;
            }
        }
    }
} // namespace

void vehicle_tbl_overrides_revert()
{
    for (const VehicleWeaponSaved& s : g_vehicle_tbl_weapon_saved) {
        if (s.weapon_type < 0 || s.weapon_type >= rf::num_weapon_types) {
            continue;
        }
        rf::WeaponInfo& wi = rf::weapon_types[s.weapon_type];
        wi.max_speed = s.max_speed;
        wi.max_speed_multi = s.max_speed_multi;
        wi.max_speed_single = s.max_speed_single;
        wi.lifetime_mul_vel = s.lifetime_mul_vel;
        wi.fire_wait = s.fire_wait;
        wi.damage_multi = s.damage_multi;
        wi.damage_radius = s.damage_radius;
        wi.damage_radius_single = s.damage_radius_single;
        wi.damage_radius_multi = s.damage_radius_multi;
        wi.crater_radius = s.crater_radius;
        wi.max_ammo = s.max_ammo;
        wi.max_ammo_multi = s.max_ammo_multi;
        wi.flags = s.flags;
    }
    for (const VehicleEntitySaved& s : g_vehicle_tbl_entity_saved) {
        if (s.entity_type < 0 || s.entity_type >= rf::num_entity_types) {
            continue;
        }
        rf::entity_types[s.entity_type].max_life = s.max_life;
    }
    for (const VehicleEntityMeshSaved& s : g_vehicle_tbl_mesh_saved) {
        if (s.entity_type < 0 || s.entity_type >= rf::num_entity_types) {
            continue;
        }
        // String::operator=(const char*) frees and reallocates out of the ENGINE heap, which is why
        // the saved copy is a std::string of the bytes rather than a held rf::String.
        rf::entity_types[s.entity_type].vmesh_filename = s.vmesh_filename.c_str();
        if (s.cockpit_overridden) {
            rf::entity_types[s.entity_type].cockpit_vfx_filename = s.cockpit_vfx_filename.c_str();
        }
    }
    if (g_jeep_gun_mesh_repointed) {
        write_mem<uint32_t>(jeep_gun_mesh_push_imm32, jeep_gun_mesh_stock_string);
        g_jeep_gun_mesh_repointed = false;
    }
    vehicle_rider_anims_revert();
    g_vehicle_tbl_weapon_saved.clear();
    g_vehicle_tbl_entity_saved.clear();
    g_vehicle_tbl_mesh_saved.clear();
}

// Revert-then-apply on every level init, so leaving a vehicle level for a vehicle-less one restores
// stock with no separate teardown path.
void vehicle_tbl_overrides_level_init_post()
{
    vehicle_tbl_overrides_revert();
    if (!rf::is_multi || !vehicle_level_has_factories()) {
        return;
    }

    for (const VehicleWeaponOverride& o : vehicle_weapon_overrides) {
        const int wt = rf::weapon_lookup_type(o.name);
        if (wt < 0 || wt >= rf::num_weapon_types) {
            xlog::warn("[vehicle] tbl override: unknown weapon class '{}'", o.name);
            continue;
        }
        rf::WeaponInfo& wi = rf::weapon_types[wt];
        g_vehicle_tbl_weapon_saved.push_back({wt, wi.max_speed, wi.max_speed_multi,
                                              wi.max_speed_single, wi.lifetime_mul_vel, wi.fire_wait,
                                              wi.damage_multi, wi.damage_radius,
                                              wi.damage_radius_single, wi.damage_radius_multi,
                                              wi.crater_radius, wi.max_ammo, wi.max_ammo_multi,
                                              wi.flags});
        // Every value the MP resolver (0x004C2AC0) re-derives is written to BOTH the _multi source
        // and the resolved slot, so a later resolver run recomputes the same number.
        if (o.velocity) {
            wi.max_speed = *o.velocity;
            wi.max_speed_multi = *o.velocity;
            wi.max_speed_single = *o.velocity;
            // Precomputed at startup and NOT re-derived by the resolver, so a velocity change must
            // carry it or the round keeps its old maximum range.
            wi.lifetime_mul_vel = wi.max_speed * wi.lifetime_seconds;
        }
        if (o.fire_wait) {
            wi.fire_wait = *o.fire_wait;
        }
        if (o.damage_multi) {
            // Only the _multi field: the MP getter reads it directly and the resolver never copies
            // damage, so $Damage stays stock.
            wi.damage_multi = *o.damage_multi;
        }
        if (o.max_ammo_multi) {
            wi.max_ammo_multi = *o.max_ammo_multi;
            wi.max_ammo = *o.max_ammo_multi;
        }
        if (o.damage_radius) {
            wi.damage_radius = *o.damage_radius;
            wi.damage_radius_single = *o.damage_radius;
            wi.damage_radius_multi = *o.damage_radius;
        }
        if (o.crater_radius) {
            wi.crater_radius = *o.crater_radius;
        }
        if (o.clear_homing) {
            // The fuse arm and the steering both gate on this flag (0x004C69CF / 0x004C6AB4), so
            // clearing it is the whole of "$Homing: false".
            wi.flags &= ~rf::WTF_HOMING;
        }
    }

    for (const VehicleEntityOverride& o : vehicle_entity_overrides) {
        const int et = rf::entity_lookup_type(o.name);
        if (et < 0 || et >= rf::num_entity_types) {
            xlog::warn("[vehicle] tbl override: unknown entity class '{}'", o.name);
            continue;
        }
        rf::EntityInfo& ei = rf::entity_types[et];
        g_vehicle_tbl_entity_saved.push_back({et, ei.max_life});
        ei.max_life = o.max_life;
    }

    // A hull already spawned keeps the VMesh it was built with, so this MUST land before the factory
    // spawns and before the first entity_create packet a client can process.
    // The swap must land on the SERVER too, because the seat array the server validates against is
    // derived from the mesh's own interface_N tags.
    for (const AlpineVehicleClassMesh& o : alpine_vehicle_class_meshes) {
        const int et = rf::entity_lookup_type(o.class_name);
        if (et < 0 || et >= rf::num_entity_types) {
            xlog::warn("[vehicle] tbl override: unknown entity class '{}'", o.class_name);
            continue;
        }
        rf::EntityInfo& ei = rf::entity_types[et];
        VehicleEntityMeshSaved saved{et, std::string{ei.vmesh_filename.c_str()}};
        ei.vmesh_filename = o.vmesh_filename;
        // Same window: entity_create reads both fields, so the cockpit swap lands with the hull's.
        if (const char* cockpit_vfx = vehicle_cockpit_vfx_for(o.class_name)) {
            saved.cockpit_overridden = true;
            saved.cockpit_vfx_filename = ei.cockpit_vfx_filename.c_str();
            ei.cockpit_vfx_filename = cockpit_vfx;
        }
        g_vehicle_tbl_mesh_saved.push_back(std::move(saved));
    }

    // Here rather than at patch-install time: the engine re-reads this operand on the first jeep of
    // every level, so the swap stays confined to vehicle levels.
    write_mem<uint32_t>(jeep_gun_mesh_push_imm32,
                        reinterpret_cast<uint32_t>(af_jeep_gun_mesh_filename));
    g_jeep_gun_mesh_repointed = true;

    vehicle_rider_anims_apply();
    vehicle_rider_anims_attach_local_entity();
}

void vehicle_spawn_install()
{
    dbg_vehicle_spawn_cmd.register_cmd();
}
