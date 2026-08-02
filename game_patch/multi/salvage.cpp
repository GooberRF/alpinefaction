#include <algorithm>
#include <cmath>
#include <format>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <xlog/xlog.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/FunHook.h>
#include <patch_common/MemUtils.h>
#include <common/utils/string-utils.h>
#include "salvage.h"
#include "gametype.h"
#include "server_internal.h"
#include "alpine_packets.h"
#include "../hud/multi_spectate.h"
#include "../misc/waypoints.h"
#include "../rf/entity.h"
#include "../rf/file/file.h"
#include "../rf/gr/gr_light.h"
#include "../rf/item.h"
#include "../rf/level.h"
#include "../rf/multi.h"
#include "../rf/object.h"
#include "../rf/os/frametime.h"
#include "../rf/os/timestamp.h"
#include "../rf/player/camera.h"
#include "../rf/player/player.h"
#include "../rf/sound/sound.h"
#include "../rf/vmesh.h"
#include "../sound/sound.h"

SalvageInfo g_salvage_info;

constexpr const char* kSalFlagItemClass = "flag_red";
// Dedicated Salvage flag mesh, swapped in over whatever items.tbl gives flag_red
// (stock: CTFflag-red.vfx). Optional - a package without it just keeps the CTF
// flag, so the gametype still works, it only looks like CTF's.
constexpr const char* kSalFlagMeshFilename = "AFSALflag1.vfx";
constexpr const char* kSalRedBaseItemClass = "base_red";
constexpr const char* kSalBlueBaseItemClass = "base_blue";
// Prop point both the player character meshes and the flag mesh carry; stock CTF
// aligns the two to hang a flag off a carrier's back.
constexpr const char* kSalPropPointName = "$prop_flag";
constexpr int kSalPickupUnlockDelayMs = 500;
constexpr int kSalStateRefreshIntervalMs = 1000;
constexpr int kSalRespawnRetryIntervalMs = 2000;
// Consecutive failed flag recreations before the server stops trying for the rest
// of the level. Guards against a mod with no flag_red item class turning the
// recovery path into a warn + broadcast every 2 seconds, forever.
constexpr int kSalMaxRespawnRetries = 5;
constexpr int kSalClientBaseLookupRetryMs = 1000;
// A candidate central item is accepted only if its distance to the two bases is
// within this relative imbalance, i.e. |d_r - d_b| / mean(d_r, d_b).
constexpr float kSalMaxCenterImbalance = 0.30f;
constexpr float kSalMidpointGroundTraceDist = 32.0f;
constexpr int kSalSoundFlagSteal = 0x40;
constexpr int kSalSoundFlagCapture = 0x3F;
// Objective glow. Green, like Bagman's — the neutral flag belongs to nobody, so
// a team colour would be misleading. Radii match what the stock game uses for
// the equivalent lights: 4.0 for a CTF flag carrier, 3.0 (0x0059479C) for a
// powerup/pickup sitting on the ground.
constexpr float kSalCarrierLightRadius = 4.0f;
// Used only if a $prop_flag lookup fails, so a carried flag is at worst floating
// above its carrier rather than stranded where it was picked up.
constexpr float kSalCarrierFallbackUpOffset = 1.5f;

namespace
{

// Hardcoded per-RFL flag spawn positions. Highest priority source; populate for
// maps where the automatic center resolution picks a poor spot.
struct SalFlagSpawnEntry { const char* rfl; float x, y, z; };
constexpr SalFlagSpawnEntry kSalFlagSpawnPositions[] = {
    // { "ctf01.rfl", 0.0f, 0.0f, 0.0f },
    { nullptr, 0.0f, 0.0f, 0.0f },
};

// Priority-ordered item classes considered as the map center.
constexpr const char* const kSalCenterCandidateClasses[] = {
    "Multi Damage Amplifier",
    "Multi Invulnerability",
    "Multi Super Armor",
    "shoulder cannon",
    "Multi Super Health",
};

int g_salvage_light_handle = -1;
float g_salvage_light_pulse_phase = 0.0f;

// Throttles the client-side base item lookup while the item list is still
// filling in after a level load.
rf::Timestamp g_client_base_lookup_retry_timer;

// Cached handle of the one flag item, for the client-facing lookup. That lookup
// sits on every visual path (world HUD, dynamic light, both outline queries, the
// carried attachment), so it runs several times per frame; stock CTF keeps a raw
// pointer for the same reason (ctf_red_flag_item), but a handle also survives the
// item dying under us — it simply stops resolving.
int g_client_flag_item_handle = -1;

// Resolved index of the $prop_flag prop point, cached per role: one slot for the
// carrier's character mesh, one for the flag's own mesh. The lookup is a string
// compare across every prop point on the mesh and the carried-flag solve needs it
// for both meshes every frame, so caching it is worth it.
//
// Keying on the VMesh pointer alone is NOT safe. A freed VMesh can be replaced by
// a later allocation at the same address, and the engine's prop-point getter takes
// the index on trust — no bounds check — so a stale index into a smaller prop table
// reads out of bounds. Each slot therefore also remembers the object handle it was
// resolved for, and both slots are dropped outright whenever the carrier changes
// (set_carrier) or the level does (reset_salvage_local_state).
struct SalPropPointCache
{
    rf::VMesh* vmesh = nullptr;
    int owner_handle = -1;
    int index = -1;
};
SalPropPointCache g_carrier_prop_point_cache;
SalPropPointCache g_flag_prop_point_cache;

void clear_prop_point_caches()
{
    g_carrier_prop_point_cache = SalPropPointCache{};
    g_flag_prop_point_cache = SalPropPointCache{};
}

// The single place g_salvage_info.carrier is assigned. Both prop-point caches are
// keyed to specific objects, and a carrier change is exactly the moment the
// outgoing carrier's character mesh can be freed and something smaller allocated
// over it, so every steal/drop/capture/disconnect path funnels through here.
void set_carrier(rf::Player* player)
{
    if (g_salvage_info.carrier == player) return;
    g_salvage_info.carrier = player;
    clear_prop_point_caches();
}

// Server-side recovery latch for the AtSpawn path (see salvage_do_frame).
int g_respawn_retry_failures = 0;
bool g_respawn_recovery_given_up = false;

// Every file-scope piece of Salvage state, cleared in one place. Two sites have to
// do this — salvage_level_init, and the "game type is no longer Salvage" branch of
// salvage_do_frame — and they must not drift apart, because both leave the module
// otherwise dormant with whatever was cached still in it.
//
// delete_light is the one genuine difference between them: mid-level the dynamic
// light object is still alive and has to be handed back, while at level init the
// previous level took it down already and the handle is nothing but a stale number.
void reset_salvage_local_state(bool delete_light)
{
    g_client_base_lookup_retry_timer.invalidate();

    // Every cached object/mesh pointer and handle is stale, and a fresh allocation
    // can land on the same address, so none of them may survive.
    g_client_flag_item_handle = -1;
    clear_prop_point_caches();

    g_respawn_retry_failures = 0;
    g_respawn_recovery_given_up = false;

    if (delete_light && g_salvage_light_handle >= 0) {
        rf::gr::light_delete(g_salvage_light_handle, 0);
    }
    g_salvage_light_handle = -1;
    g_salvage_light_pulse_phase = 0.0f;
}

std::optional<rf::Vector3> lookup_hardcoded_flag_spawn(std::string_view filename)
{
    for (const SalFlagSpawnEntry& e : kSalFlagSpawnPositions) {
        if (e.rfl && string_iequals(filename, e.rfl)) {
            return rf::Vector3{e.x, e.y, e.z};
        }
    }
    return std::nullopt;
}

rf::Item* item_from_handle_or_null(int handle)
{
    if (handle < 0) return nullptr;
    rf::Object* obj = rf::obj_from_handle(handle);
    if (!obj || obj->type != rf::OT_ITEM) return nullptr;
    return static_cast<rf::Item*>(obj);
}

rf::Entity* alive_entity_for(rf::Player* player)
{
    if (!player) return nullptr;
    if (rf::player_is_dead(player) || rf::player_is_dying(player)) return nullptr;
    rf::Entity* ep = rf::entity_from_handle(player->entity_handle);
    if (!ep) return nullptr;
    if (ep->life <= 0.0f) return nullptr;
    return ep;
}

void announce(std::string_view msg)
{
    af_broadcast_automated_chat_msg(msg);
}

// Listen-server hosts never receive their own state packets, so server-side
// transitions play the transition sound locally for them.
void play_local_transition_sound(int sound_id)
{
    if (rf::is_dedicated_server) return;
    rf::snd_play(sound_id, 0, 0.0f, 1.0f);
}

void set_flag_item_class_spin(bool enable)
{
    const int type_idx = rf::item_lookup_type(kSalFlagItemClass);
    if (type_idx < 0) return;
    if (enable) {
        rf::item_info[type_idx].flags |= rf::IIF_SPINS_IN_MULTI;
    }
    else {
        rf::item_info[type_idx].flags &= ~rf::IIF_SPINS_IN_MULTI;
    }
}

void kill_current_flag_item()
{
    rf::Item* item = item_from_handle_or_null(g_salvage_info.flag_item_handle);
    if (item) {
        // entity_handle = 0 tells clients the item just goes away rather than
        // being consumed by a player. obj_flag_dead alone is server-only.
        rf::send_item_apply_packet(nullptr, item->handle, 0, -1, -1, -1);
        rf::obj_flag_dead(item);
        // item_create re-pointed the engine's red flag global at our neutral flag;
        // don't leave it dangling at a dead object.
        if (rf::ctf_red_flag_item == static_cast<rf::Object*>(item)) {
            rf::ctf_red_flag_item = nullptr;
        }
    }
    g_salvage_info.flag_item_handle = -1;
    // Listen server hosts share this object list with the client-side lookup.
    g_client_flag_item_handle = -1;
}

// Presence of kSalFlagMeshFilename, probed once and cached: the swap sits on a
// level-init path, and a missing file is a permanent condition, not something
// worth re-testing every level.
bool g_sal_flag_mesh_exists = false;
bool g_sal_flag_mesh_checked = false;
// items.tbl value for the flag class, held while the Salvage mesh is swapped in.
// Empty means "not currently swapped".
std::string g_saved_flag_class_mesh;

void ensure_sal_flag_mesh_checked()
{
    if (g_sal_flag_mesh_checked) return;
    g_sal_flag_mesh_checked = true;
    auto file = std::make_unique<rf::File>();
    if (file->open(kSalFlagMeshFilename) == 0) {
        g_sal_flag_mesh_exists = true;
        file->close();
    }
    else {
        xlog::info("salvage: '{}' is not installed, keeping the flag_red mesh from items.tbl",
                   kSalFlagMeshFilename);
    }
}

// The mesh cannot be swapped onto an item after the fact. rf::item_restore_mesh
// hardcodes MESH_TYPE_STATIC (0x00459BE1 and 0x00459C1A both push 1) and the
// static loader rewrites the extension to .v3m before opening anything
// (0x0053AB40), so handing it a .vfx loads nothing: the item ends up with a null
// vmesh - invisible, but with its radius-based collision intact - and the object
// is flagged dead on top of that. Only the anim-fx path (MESH_TYPE_ANIM_FX ->
// vmesh_create_anim_fx) can load a .vfx.
//
// So swap the item CLASS instead and let the engine do the loading. item_create
// reads item_info[type].v3d_filename and .v3d_type (0x0045917E / 0x00459194)
// straight into obj_create, and items.tbl gives flag_red $V3D Type "anim", so the
// mesh comes in through the correct path. Both sides run their own item_create -
// the server in spawn_flag_item, clients from the engine's item-create packet
// handler - so one class swap covers both with no per-item work.
void revert_flag_class_mesh_if_swapped()
{
    if (g_saved_flag_class_mesh.empty()) return;
    const int type_idx = rf::item_lookup_type(kSalFlagItemClass);
    if (type_idx < 0) {
        // Nothing to put the original back into. Hold on to it: dropping it here
        // would lose the items.tbl value for good, so a later revert (once the class
        // resolves again) would have nothing to restore.
        return;
    }
    // String::operator=(const char*) reallocates through the engine's own
    // allocator, so neither half of the swap ever crosses an allocator.
    rf::item_info[type_idx].v3d_filename = g_saved_flag_class_mesh.c_str();
    g_saved_flag_class_mesh.clear();
}

void apply_flag_class_mesh_if_available()
{
    if (!rf::is_multi || !gt_is_salvage()) return;
    if (!g_saved_flag_class_mesh.empty()) return; // already swapped in
    ensure_sal_flag_mesh_checked();
    if (!g_sal_flag_mesh_exists) return;
    const int type_idx = rf::item_lookup_type(kSalFlagItemClass);
    if (type_idx < 0) return;
    // Only the anim-fx loader can open a .vfx (see the block comment above). If a mod
    // has retyped flag_red, swapping the filename in would hand a .vfx to the static
    // loader and leave the item with no mesh at all; keeping the mod's own flag is
    // the right degradation.
    if (rf::item_info[type_idx].v3d_type != rf::MESH_TYPE_ANIM_FX) {
        xlog::warn("salvage: flag_red is not an anim-fx item class (v3d_type {}); "
            "keeping its items.tbl mesh", rf::item_info[type_idx].v3d_type);
        return;
    }
    const char* current = rf::item_info[type_idx].v3d_filename.c_str();
    // An empty original would make the saved value indistinguishable from "not
    // swapped", and there would be nothing to put back anyway.
    if (!current || !*current) return;
    g_saved_flag_class_mesh = current;
    rf::item_info[type_idx].v3d_filename = kSalFlagMeshFilename;
}

// Arm a brief grace period after the flag is (re)positioned, so the player who
// just dropped it — or who is standing on the home spawn when it returns — can't
// immediately walk straight back into it. salvage_do_frame lifts the gate again
// once the timer elapses.
void arm_pickup_unlock(rf::Item* item)
{
    item->item_flags |= rf::IF_NO_PICKUP;
    g_salvage_info.pickup_unlock_timer.set(kSalPickupUnlockDelayMs);
}

// Reposition the existing flag item instead of destroying and recreating it.
// Object::update_room re-derives the room from the new position (and clears the
// teleported flag), which the render and collision paths both need.
// A null orient means "position only", leaving the item's current orientation
// alone; clients use that so they never invent an orientation of their own.
void move_flag_item_to(rf::Item* item, const rf::Vector3& pos, const rf::Matrix3* orient)
{
    if (!item) return;
    rf::Vector3 new_pos = pos;
    if (orient) {
        item->orient = *orient;
    }
    item->move(&new_pos);
    item->update_room();
}

void spawn_flag_item(const rf::Vector3& pos, rf::Matrix3& orient)
{
    g_salvage_info.flag_item_handle = -1;
    if (g_salvage_info.flag_item_type < 0) return;

    rf::Item* item = rf::item_create(
        g_salvage_info.flag_item_type,
        kSalFlagItemClass,
        1,
        -1,
        &pos,
        &orient,
        -1,
        0,
        0
    );

    if (!item) {
        xlog::warn("salvage: item_create failed for flag at ({},{},{})", pos.x, pos.y, pos.z);
        return;
    }

    // IF_PERMANENT exempts the flag from item culling.
    item->item_flags |= rf::IF_DROPPED | rf::IF_PERMANENT;
    g_salvage_info.flag_item_handle = item->handle;
    arm_pickup_unlock(item);

    // -1 for level_item_index: this is not a level-placed item.
    rf::send_item_create_packet(item, 0, -1);
}

// Remove the level's colored CTF flags so the neutral flag is the only one.
void remove_level_ctf_flags()
{
    // Server-side only, and deliberately not replicated: multiplayer clients never
    // create level-placed items themselves (item_create bails unless the create came
    // from a packet), so no client ever has these flags to remove. A listen server's
    // host shares the server's object list, so obj_flag_dead covers it there too.
    for (const char* cls : {"flag_red", "flag_blue"}) {
        const int type_idx = rf::item_lookup_type(cls);
        if (type_idx < 0) continue;
        rf::Item* it = rf::item_list.next;
        while (it && it != &rf::item_list) {
            rf::Item* next = it->next;
            if (it->info_index == type_idx) {
                rf::obj_flag_dead(it);
            }
            it = next;
        }
    }

    // The engine keeps raw pointers to the flag items; both are dead now.
    rf::ctf_red_flag_item = nullptr;
    rf::ctf_blue_flag_item = nullptr;
}

bool find_first_item_pos_of_class(const char* cls, rf::Vector3* out_pos)
{
    const int type_idx = rf::item_lookup_type(cls);
    if (type_idx < 0) return false;
    for (rf::Item* it = rf::item_list.next; it && it != &rf::item_list; it = it->next) {
        if (it->info_index == type_idx) {
            *out_pos = it->pos;
            return true;
        }
    }
    return false;
}

void resolve_base_positions()
{
    const bool red_found = find_first_item_pos_of_class(kSalRedBaseItemClass, &g_salvage_info.base_red_pos);
    const bool blue_found = find_first_item_pos_of_class(kSalBlueBaseItemClass, &g_salvage_info.base_blue_pos);
    g_salvage_info.bases_known = red_found && blue_found;

    if (!g_salvage_info.bases_known) {
        xlog::warn("salvage: level is missing a team base (red found: {}, blue found: {}); "
            "captures cannot be scored", red_found, blue_found);
    }
}

// The state packet never carries base positions, but the base items are
// level-placed so they exist on clients too. Resolve them locally on first use
// and cache in g_salvage_info; retry on a timer because the item list is still
// filling in right after a level load. Bots are headless clients and need this
// to route a carried flag home.
// Servers use this too: salvage_level_init_post is not the earliest consumer.
// process_queued_spawn_points_from_items runs before it in level_init_post and
// classifies dynamic respawn points via is_closer_to_red_flag, which needs the
// bases already resolved.
void resolve_base_positions_lazy()
{
    if (g_salvage_info.bases_known) return;
    if (g_client_base_lookup_retry_timer.valid() && !g_client_base_lookup_retry_timer.elapsed()) {
        return;
    }
    g_client_base_lookup_retry_timer.set(kSalClientBaseLookupRetryMs);

    const bool red_found = find_first_item_pos_of_class(kSalRedBaseItemClass, &g_salvage_info.base_red_pos);
    const bool blue_found = find_first_item_pos_of_class(kSalBlueBaseItemClass, &g_salvage_info.base_blue_pos);
    g_salvage_info.bases_known = red_found && blue_found;
}

struct CenterCandidate
{
    rf::Item* item = nullptr;
    int priority = 0;
    float imbalance = 0.0f;
};

// Walk the item list once per candidate class and score each instance by how
// evenly it sits between the two bases.
std::vector<CenterCandidate> collect_center_candidates()
{
    std::vector<CenterCandidate> out;
    if (!g_salvage_info.bases_known) return out;

    for (int priority = 0; priority < static_cast<int>(std::size(kSalCenterCandidateClasses)); ++priority) {
        const int type_idx = rf::item_lookup_type(kSalCenterCandidateClasses[priority]);
        if (type_idx < 0) continue;
        for (rf::Item* it = rf::item_list.next; it && it != &rf::item_list; it = it->next) {
            if (it->info_index != type_idx) continue;
            const float d_r = (it->pos - g_salvage_info.base_red_pos).len();
            const float d_b = (it->pos - g_salvage_info.base_blue_pos).len();
            const float mean = std::max(1.0f, (d_r + d_b) * 0.5f);
            out.push_back(CenterCandidate{it, priority, std::fabs(d_r - d_b) / mean});
        }
    }
    return out;
}

const CenterCandidate* pick_best_candidate(const std::vector<CenterCandidate>& candidates, bool require_balanced)
{
    const CenterCandidate* best = nullptr;
    for (const CenterCandidate& c : candidates) {
        if (require_balanced && c.imbalance > kSalMaxCenterImbalance) continue;
        if (!best) {
            best = &c;
            continue;
        }
        if (c.priority != best->priority) {
            if (c.priority < best->priority) best = &c;
            continue;
        }
        if (c.imbalance < best->imbalance) best = &c;
    }
    return best;
}

// Resolve the neutral flag's home. Bases must already be resolved.
void resolve_flag_spawn()
{
    g_salvage_info.spawn_orient = rf::identity_matrix;

    if (auto override_pos = lookup_hardcoded_flag_spawn(rf::level.filename.c_str())) {
        g_salvage_info.spawn_pos = *override_pos;
        g_salvage_info.spawn_known = true;
        xlog::info("salvage: flag spawn from hardcoded table at ({},{},{})",
            g_salvage_info.spawn_pos.x, g_salvage_info.spawn_pos.y, g_salvage_info.spawn_pos.z);
        return;
    }

    const std::vector<CenterCandidate> candidates = collect_center_candidates();
    const CenterCandidate* best = pick_best_candidate(candidates, true);
    const char* source = "balanced central item";
    if (!best) {
        best = pick_best_candidate(candidates, false);
        source = "least-imbalanced central item";
    }

    if (best) {
        g_salvage_info.spawn_pos = best->item->pos;
        g_salvage_info.spawn_orient = best->item->orient;
        g_salvage_info.spawn_known = true;
        xlog::info("salvage: flag spawn from {} '{}' (imbalance {:.2f}) at ({},{},{})",
            source, kSalCenterCandidateClasses[best->priority], best->imbalance,
            g_salvage_info.spawn_pos.x, g_salvage_info.spawn_pos.y, g_salvage_info.spawn_pos.z);
        // The chosen powerup would sit inside the flag. Level-placed, so server-side
        // only — see remove_level_ctf_flags.
        rf::obj_flag_dead(best->item);
        return;
    }

    if (g_salvage_info.bases_known) {
        const rf::Vector3 midpoint = (g_salvage_info.base_red_pos + g_salvage_info.base_blue_pos) * 0.5f;
        rf::Vector3 floor_hit{};
        if (trace_ground_below_point(midpoint, kSalMidpointGroundTraceDist, &floor_hit)) {
            g_salvage_info.spawn_pos = floor_hit + rf::Vector3{0.0f, kWaypointGenerateGroundOffset, 0.0f};
            g_salvage_info.spawn_known = true;
            xlog::info("salvage: flag spawn from snapped base midpoint at ({},{},{})",
                g_salvage_info.spawn_pos.x, g_salvage_info.spawn_pos.y, g_salvage_info.spawn_pos.z);
            return;
        }
    }

    g_salvage_info.spawn_pos = rf::level.player_start_pos;
    g_salvage_info.spawn_orient = rf::level.player_start_orient;
    g_salvage_info.spawn_known = true;
    xlog::warn("salvage: could not resolve a map center; flag spawns at the player start position");
}

void enter_delayed_state(int delay_ms)
{
    g_salvage_info.state = SalFlagState::Delayed;
    set_carrier(nullptr);
    g_salvage_info.return_timer.invalidate();
    g_salvage_info.spawn_delay_timer.set(std::max(0, delay_ms));
    g_salvage_info.refresh_timer.set(kSalStateRefreshIntervalMs);
    // No item exists until the delay elapses, but the class flag is global: put it
    // back so the flag spins again the moment it respawns.
    set_flag_item_class_spin(true);
}

void spawn_flag_at_home()
{
    g_salvage_info.state = SalFlagState::AtSpawn;
    set_carrier(nullptr);
    g_salvage_info.return_timer.invalidate();
    g_salvage_info.spawn_delay_timer.invalidate();
    g_salvage_info.refresh_timer.invalidate();
    g_salvage_info.respawn_retry_timer.invalidate();
    set_flag_item_class_spin(true);

    // Coming back from Carried or Dropped the item is still alive: move it home
    // rather than churning through a kill/create pair, the way the stock game's
    // multi_ctf_flag_return_to_base does.
    if (rf::Item* item = item_from_handle_or_null(g_salvage_info.flag_item_handle)) {
        move_flag_item_to(item, g_salvage_info.spawn_pos, &g_salvage_info.spawn_orient);
        arm_pickup_unlock(item);
    }
    else {
        spawn_flag_item(g_salvage_info.spawn_pos, g_salvage_info.spawn_orient);
    }
}

void drop_flag_at(rf::Player* prev_carrier, const rf::Vector3& drop_pos)
{
    set_carrier(nullptr);
    g_salvage_info.state = SalFlagState::Dropped;
    g_salvage_info.spawn_delay_timer.invalidate();
    g_salvage_info.return_timer.set(g_alpine_server_config_active_rules.salvage.flag_return_time_ms);
    g_salvage_info.refresh_timer.set(kSalStateRefreshIntervalMs);
    set_flag_item_class_spin(true);

    rf::Matrix3 drop_orient = rf::identity_matrix;
    if (rf::Item* item = item_from_handle_or_null(g_salvage_info.flag_item_handle)) {
        move_flag_item_to(item, drop_pos, &drop_orient);
        arm_pickup_unlock(item);
    }
    else {
        // The item went missing under us (failed create, level churn). Recreate it
        // so the round can continue.
        spawn_flag_item(drop_pos, drop_orient);
    }

    if (prev_carrier) {
        announce(std::format("{} dropped the flag!", prev_carrier->name.c_str()));
    }
    else {
        announce("The flag has been dropped!");
    }

    salvage_broadcast_state();
}

// Find the neutral flag from a client's point of view. flag_item_handle is
// server-only state, so clients identify the item by class — cached by handle,
// because this is called several times a frame. The handle is re-resolved (and
// re-validated against the flag class) on every call, so a dead or recycled object
// just falls through to a fresh walk.
// flag_item_type is resolved once, in salvage_level_init: the game type is settled
// before level init on every path (including join-in-progress, where the join
// accept carries it), so there is nothing to re-resolve later.
rf::Item* find_client_side_flag_item()
{
    if (g_salvage_info.flag_item_type < 0) return nullptr;

    if (rf::Item* cached = item_from_handle_or_null(g_client_flag_item_handle)) {
        if (cached->info_index == g_salvage_info.flag_item_type) {
            return cached;
        }
    }
    g_client_flag_item_handle = -1;

    for (rf::Item* it = rf::item_list.next; it && it != &rf::item_list; it = it->next) {
        if (it->info_index == g_salvage_info.flag_item_type) {
            g_client_flag_item_handle = it->handle;
            return it;
        }
    }
    return nullptr;
}

// The one flag item, from whichever side is asking.
rf::Item* current_flag_item()
{
    if (rf::is_server) {
        return item_from_handle_or_null(g_salvage_info.flag_item_handle);
    }
    return find_client_side_flag_item();
}

// $prop_flag index for a mesh, remembered per (owning object, VMesh) pair. A
// negative result is cached too: a mesh without the prop point stays negative until
// it is replaced. Either half changing re-resolves, which is what keeps a recycled
// VMesh address from serving an index into a different — possibly shorter — prop
// table.
int lookup_prop_flag_index(rf::VMesh* vmesh, int owner_handle, SalPropPointCache& cache)
{
    if (cache.vmesh != vmesh || cache.owner_handle != owner_handle) {
        cache.vmesh = vmesh;
        cache.owner_handle = owner_handle;
        cache.index = rf::vmesh_lookup_prop_point(vmesh, kSalPropPointName);
    }
    return cache.index;
}

// Solve the flag's world transform so its $prop_flag lands exactly on the
// carrier's, the same alignment stock CTF performs in multi_ctf_move_flags.
bool solve_carrier_flag_transform(rf::Entity* ep, rf::Item* item,
                                  rf::Vector3* out_pos, rf::Matrix3* out_orient)
{
    if (!ep->vmesh || !item->vmesh) return false;

    // Carrier's $prop_flag in WORLD space.
    const int carrier_prop_idx =
        lookup_prop_flag_index(ep->vmesh, ep->handle, g_carrier_prop_point_cache);
    if (carrier_prop_idx < 0) return false;

    rf::Vector3 carrier_prop_pos{};
    rf::Matrix3 carrier_prop_orient{};
    rf::vmesh_get_prop_point_transform(
        ep->vmesh, carrier_prop_idx, &ep->orient, &ep->pos,
        &carrier_prop_orient, &carrier_prop_pos);

    // Flag mesh's $prop_flag in FLAG-LOCAL space. Keyed on the item handle as well:
    // a capture-respawn builds a brand new item with a brand new mesh.
    const int flag_prop_idx =
        lookup_prop_flag_index(item->vmesh, item->handle, g_flag_prop_point_cache);
    if (flag_prop_idx < 0) return false;

    rf::Vector3 flag_prop_local_pos{};
    rf::Matrix3 flag_prop_local_orient{};
    rf::Vector3 zero_pos{0.0f, 0.0f, 0.0f};
    rf::Matrix3 ident = rf::identity_matrix;
    rf::vmesh_get_prop_point_transform(
        item->vmesh, flag_prop_idx, &ident, &zero_pos,
        &flag_prop_local_orient, &flag_prop_local_pos);

    // For an orthonormal rotation, inverse == transpose.
    rf::Matrix3 flag_prop_local_inv = flag_prop_local_orient;
    flag_prop_local_inv.transpose();
    rf::Matrix3 flag_orient_world = carrier_prop_orient;
    flag_orient_world.mul(flag_prop_local_inv);

    *out_pos = carrier_prop_pos - flag_orient_world.transform_vector(flag_prop_local_pos);
    *out_orient = flag_orient_world;
    return true;
}

// True when the local view looks out of the carrier's own eyes, in which case the
// flag hanging off their back would only ever be drawn inside the camera. Third
// person and spectators keep seeing it, exactly like a stock CTF flag.
bool flag_hidden_for_local_view()
{
    if (g_salvage_info.state != SalFlagState::Carried || !g_salvage_info.carrier) return false;

    if (multi_spectate_is_spectating()) {
        return multi_spectate_is_first_person()
            && multi_spectate_get_target_player() == g_salvage_info.carrier;
    }
    if (rf::local_player != g_salvage_info.carrier) return false;
    return rf::local_player->cam
        && rf::camera_get_mode(*rf::local_player->cam) == rf::CAMERA_FIRST_PERSON;
}

} // namespace

void salvage_apply_flag_class_overrides()
{
    // Called for every game type from the engine's CTF level init, which the
    // engine reaches from multi_level_init - before level_load, so before any
    // item in the level exists. Both the spin bit and the mesh are class-wide
    // state, so both are reverted unconditionally first: the flag class must
    // never carry Salvage state into the next CTF level.
    set_flag_item_class_spin(rf::is_multi && gt_is_salvage());
    revert_flag_class_mesh_if_swapped();
    apply_flag_class_mesh_if_available(); // no-ops outside Salvage
}

void salvage_on_multi_shutdown()
{
    // The engine's own teardown only calls the CTF cleanup hook when the *outgoing*
    // game type is CTF, so leaving a Salvage server never reaches
    // salvage_apply_flag_class_overrides. Both overrides are class-wide global state
    // on flag_red, so without this the single-player campaign (and the next CTF
    // level, until its own init runs) inherits the Salvage flag mesh and a spinning
    // flag. Mirrors what the level-init path does, which stays as belt and braces.
    revert_flag_class_mesh_if_swapped();
    set_flag_item_class_spin(false);

    // The session is over, so the module's local state has to end with it. Nothing
    // else will do that: salvage_level_init is the only other reset and it runs from
    // multi_level_init, a path single player never takes and the *next* multiplayer
    // session does not reach until after its level has loaded.
    //
    // g_salvage_light_handle is the dangerous survivor. The engine frees the flag
    // glow along with the level it belonged to, so once the session ends the handle
    // is only a number naming a released pool entry. multi_start sets is_server and
    // netgame.type several frames before the new level loads, and server_do_frame
    // ticks salvage_do_frame in that gap - with active still set from the previous
    // session and the game type no longer Salvage, it takes the "no longer Salvage"
    // branch and calls reset_salvage_local_state(true). That is a second
    // gr::light_delete on an already unlinked pool entry, and the unlink writes
    // through its null prev pointer.
    //
    // delete_light is false for the same reason it is false at level init: the light
    // is the outgoing level's to reclaim, not ours.
    g_salvage_info = SalvageInfo{};
    reset_salvage_local_state(false);
}

int salvage_get_red_team_score()
{
    return g_salvage_info.red_caps;
}

int salvage_get_blue_team_score()
{
    return g_salvage_info.blue_caps;
}

void salvage_set_red_team_score(int v)
{
    if (rf::is_server) return;
    g_salvage_info.red_caps = v;
}

void salvage_set_blue_team_score(int v)
{
    if (rf::is_server) return;
    g_salvage_info.blue_caps = v;
}

SalFlagState salvage_get_state()
{
    return g_salvage_info.state;
}

rf::Player* salvage_get_carrier()
{
    return g_salvage_info.carrier;
}

bool salvage_player_is_carrier(const rf::Player* player)
{
    return gt_is_salvage() && player && g_salvage_info.carrier == player;
}

const rf::Vector3& salvage_get_spawn_pos()
{
    return g_salvage_info.spawn_pos;
}

bool salvage_spawn_is_known()
{
    return g_salvage_info.spawn_known;
}

bool salvage_get_base_positions(rf::Vector3* out_red, rf::Vector3* out_blue)
{
    if (!g_salvage_info.bases_known && gt_is_salvage()) {
        resolve_base_positions_lazy();
    }
    if (!g_salvage_info.bases_known) return false;
    if (out_red) *out_red = g_salvage_info.base_red_pos;
    if (out_blue) *out_blue = g_salvage_info.base_blue_pos;
    return true;
}

bool salvage_get_local_team_base_pos(rf::Vector3* out_pos)
{
    if (!out_pos || !rf::local_player) return false;
    rf::Vector3 base_red{};
    rf::Vector3 base_blue{};
    if (!salvage_get_base_positions(&base_red, &base_blue)) return false;
    *out_pos = (rf::local_player->team == rf::TEAM_RED) ? base_red : base_blue;
    return true;
}

int salvage_get_time_left_ms()
{
    if (g_salvage_info.state == SalFlagState::Dropped && g_salvage_info.return_timer.valid()) {
        return std::max(0, g_salvage_info.return_timer.time_until());
    }
    if (g_salvage_info.state == SalFlagState::Delayed && g_salvage_info.spawn_delay_timer.valid()) {
        return std::max(0, g_salvage_info.spawn_delay_timer.time_until());
    }
    return 0;
}

bool salvage_get_client_flag_pos(rf::Vector3* out_pos)
{
    // "Where the flag is lying", not "where the flag object is". The item stays
    // alive while carried now, so the state gate is what keeps every consumer
    // (world HUD, bots, waypoints, the dropped-flag light) from treating a flag on
    // somebody's back as a flag on the floor.
    if (g_salvage_info.state != SalFlagState::AtSpawn
        && g_salvage_info.state != SalFlagState::Dropped) return false;
    rf::Item* item = find_client_side_flag_item();
    if (!item) return false;
    *out_pos = item->pos;
    return true;
}

bool salvage_get_flag_pos(rf::Vector3* out_pos)
{
    rf::Item* item = current_flag_item();
    if (!item) return false;
    *out_pos = item->pos;
    return true;
}

bool salvage_is_flag_item(const rf::Item* item)
{
    if (!gt_is_salvage() || !item || g_salvage_info.flag_item_type < 0) return false;
    return item->info_index == g_salvage_info.flag_item_type;
}

bool salvage_viewer_is_carrier_first_person()
{
    if (!gt_is_salvage() || !g_salvage_info.carrier) return false;
    if (rf::local_player == g_salvage_info.carrier) return true;
    if (multi_spectate_is_first_person()
        && multi_spectate_get_target_player() == g_salvage_info.carrier) {
        return true;
    }
    return false;
}

bool salvage_query_flag_outline(rf::VMesh** out_vmesh, rf::Vector3* out_pos, rf::Matrix3* out_orient)
{
    if (!gt_is_salvage()) return false;

    const SalFlagState state = g_salvage_info.state;
    if (state != SalFlagState::AtSpawn && state != SalFlagState::Dropped
        && state != SalFlagState::Carried) return false;

    // The carried flag is already suppressed for the carrier's own first-person view
    // by item_render_hide_carried_flag_patch; an outline of a mesh sitting inside the
    // camera would be the one thing still visible, so drop it here too.
    if (state == SalFlagState::Carried && salvage_viewer_is_carrier_first_person()) return false;

    rf::Item* item = current_flag_item();
    if (!item || !item->vmesh) return false;
    if (item->obj_flags & (rf::OF_DELAYED_DELETE | rf::OF_HIDDEN)) return false;

    *out_vmesh = item->vmesh;
    *out_pos = item->pos;
    if (state == SalFlagState::Carried) {
        // salvage_move_carried_flag has already placed the item on the carrier's back.
        *out_orient = item->orient;
    }
    else if (item->info && (item->info->flags & rf::IIF_SPINS_IN_MULTI)) {
        // A spinning item is drawn from spin_angle, not from item->orient. Mirror the
        // stock renderer so the outline turns with the mesh.
        out_orient->set_from_angles(0.0f, 0.0f, -item->spin_angle);
    }
    else {
        *out_orient = item->orient;
    }
    return true;
}

void salvage_move_carried_flag()
{
    if (!rf::is_multi || !gt_is_salvage()) return;
    if (g_salvage_info.state != SalFlagState::Carried || !g_salvage_info.carrier) return;

    rf::Item* item = current_flag_item();
    if (!item) return;
    rf::Entity* ep = rf::entity_from_handle(g_salvage_info.carrier->entity_handle);
    if (!ep) return;

    rf::Vector3 pos{};
    rf::Matrix3 orient{};
    if (!solve_carrier_flag_transform(ep, item, &pos, &orient)) {
        // Missing prop point or mesh: park the flag over the carrier so it is at
        // least visible and at least in the right place.
        pos = ep->pos;
        pos.y += kSalCarrierFallbackUpOffset;
        orient = ep->orient;
    }

    item->orient = orient;
    item->move(&pos);
    // The flag is following its carrier, not teleporting. Stock CTF clears the same
    // bit right after moving a carried flag.
    item->obj_flags = static_cast<rf::ObjectFlags>(item->obj_flags & ~rf::OF_WAS_TELEPORTED);
    item->set_room(ep->room);
}

void salvage_client_do_frame()
{
    // Servers drive the attachment from salvage_do_frame; this is the client half.
    if (rf::is_server) return;
    salvage_move_carried_flag();
}

void salvage_play_return_sound()
{
    rf::snd_play(stock_sound_id::flag_respawn, 0, 0.0f, 1.0f);
}

void salvage_update_dynamic_light()
{
    if (g_salvage_light_handle >= 0) {
        rf::gr::light_delete(g_salvage_light_handle, 0);
        g_salvage_light_handle = -1;
    }

    if (!gt_is_salvage()) return;

    // Where the glow goes. The engine lights CTF carriers from
    // entity_process_post by asking multi_ctf_get_*_flag_player(), which Salvage
    // never populates, so the carrier glow has to be built here too. The flag is
    // lit wherever it physically is — at home, dropped, or on a carrier's back.
    // Only the Delayed respawn window, where no flag exists, stays dark.
    rf::Vector3 pos{};
    float radius = 0.0f;
    if (g_salvage_info.state == SalFlagState::Carried) {
        if (!g_salvage_info.carrier) return;
        rf::Entity* ep = rf::entity_from_handle(g_salvage_info.carrier->entity_handle);
        if (!ep) return;
        pos = ep->pos; // same anchor the engine uses for the CTF carrier light
        radius = kSalCarrierLightRadius;
    }
    else if (g_salvage_info.state == SalFlagState::AtSpawn
        || g_salvage_info.state == SalFlagState::Dropped) {
        if (!salvage_get_client_flag_pos(&pos)) return;
        radius = addr_as_ref<float>(0x0059479C);
    }
    else {
        return;
    }

    // Match stock game dynamic light pulse for amps/flags.
    const float pulse_period   = addr_as_ref<float>(0x005947A0);
    const float sine_amplitude = addr_as_ref<float>(0x005947A4);
    const float sine_base      = addr_as_ref<float>(0x00594798);
    if (pulse_period <= 0.0f) return;

    g_salvage_light_pulse_phase = std::fmod(g_salvage_light_pulse_phase + rf::frametime, pulse_period);

    constexpr float kTwoPi = 6.28318530718f;
    const float intensity =
        sine_base + sine_amplitude * std::sin(kTwoPi * g_salvage_light_pulse_phase / pulse_period);

    g_salvage_light_handle = rf::gr::light_create_point(
        &pos,
        radius,
        intensity,
        0.0f, 1.0f, 0.0f,
        true,
        rf::gr::LightShadowcastCondition::SHADOWCAST_EDITOR,
        0);
}

void salvage_force_state_sync_to(rf::Player* player)
{
    if (!gt_is_salvage()) return;

    af_send_salvage_state_packet(player);
}

void salvage_broadcast_state()
{
    if (!gt_is_salvage() || !rf::is_server) return;
    af_send_salvage_state_packet_to_all();
}

void salvage_apply_state_from_packet(uint8_t state, uint8_t carrier_player_id, uint16_t time_left_ms,
                                     uint16_t red_caps, uint16_t blue_caps, const rf::Vector3& spawn_pos,
                                     const rf::Vector3& flag_pos)
{
    // Untrusted: a server could send information in a state this build has no
    // meaning for. Drop the packet rather than parking g_salvage_info in a value
    // no branch below (or in the HUD/bot code) recognises.
    if (state > static_cast<uint8_t>(SalFlagState::Delayed)) {
        xlog::warn("salvage: ignoring state packet with unknown flag state {}", state);
        return;
    }

    const SalFlagState prev_state = g_salvage_info.state;
    // The first packet after level init seeds the local mirror rather than
    // reporting a transition. Without this, a player joining mid-round hears the
    // capture cue for a capture that happened before they connected, or the steal
    // cue for a flag that was already on somebody's back.
    const bool seeding = !g_salvage_info.first_state_packet_applied;
    g_salvage_info.first_state_packet_applied = true;

    g_salvage_info.state = static_cast<SalFlagState>(state);
    set_carrier(carrier_player_id == 0xFF ? nullptr
                                          : rf::multi_find_player_by_id(carrier_player_id));
    g_salvage_info.red_caps = red_caps;
    g_salvage_info.blue_caps = blue_caps;
    g_salvage_info.spawn_pos = spawn_pos;
    g_salvage_info.spawn_known = true;

    // Re-arm the local timers so the world HUD countdown ticks smoothly between
    // broadcasts.
    if (g_salvage_info.state == SalFlagState::Dropped) {
        g_salvage_info.return_timer.set(time_left_ms);
        g_salvage_info.spawn_delay_timer.invalidate();
    }
    else if (g_salvage_info.state == SalFlagState::Delayed) {
        g_salvage_info.spawn_delay_timer.set(time_left_ms);
        g_salvage_info.return_timer.invalidate();
    }
    else {
        g_salvage_info.return_timer.invalidate();
        g_salvage_info.spawn_delay_timer.invalidate();
    }

    // The flag item is no longer destroyed and recreated on every transition, so
    // the server's authoritative position has to be applied to the local object.
    // While Carried the per-frame attachment owns it instead. The spin bit is
    // client-local state, so it gets the same treatment as on the server.
    if (g_salvage_info.state == SalFlagState::Carried) {
        set_flag_item_class_spin(false);
        salvage_move_carried_flag();
    }
    else {
        set_flag_item_class_spin(true);
        // Position only: the resting orientation belongs to the server (spawn_orient,
        // replicated with the item create packet), so overwriting it with identity
        // here would silently diverge from it.
        if (rf::Item* item = find_client_side_flag_item()) {
            if (g_salvage_info.state == SalFlagState::AtSpawn) {
                move_flag_item_to(item, g_salvage_info.spawn_pos, nullptr);
            }
            else if (g_salvage_info.state == SalFlagState::Dropped) {
                // Repeated every refresh broadcast, which doubles as drift correction.
                move_flag_item_to(item, flag_pos, nullptr);
            }
        }
    }

    // Entering Delayed means the server just killed the flag item, and the create
    // packet that made it had re-pointed the engine's red flag global at it. The
    // engine never clears these globals on item death, so leaving them is a landmine
    // for any future reader that is not CTF-gated. Servers (including a listen
    // server's host) clear their own copy in kill_current_flag_item.
    if (!rf::is_server && prev_state != SalFlagState::Delayed
        && g_salvage_info.state == SalFlagState::Delayed) {
        rf::ctf_red_flag_item = nullptr;
        rf::ctf_blue_flag_item = nullptr;
        // Same story for our own cached handle: the item it names is on its way out.
        g_client_flag_item_handle = -1;
    }

    if (!seeding) {
        // Capture is inferred from the Carried -> Delayed edge, not from a caps delta:
        // caps also arrive via the unreliable stock team_scores packet, which can beat
        // this reliable one, and Carried -> Delayed only ever happens on a capture.
        if (prev_state == SalFlagState::Carried && g_salvage_info.state == SalFlagState::Delayed) {
            rf::snd_play(kSalSoundFlagCapture, 0, 0.0f, 1.0f);
        }
        else if (prev_state != SalFlagState::Carried && g_salvage_info.state == SalFlagState::Carried) {
            rf::snd_play(kSalSoundFlagSteal, 0, 0.0f, 1.0f);
        }
        else if (prev_state != SalFlagState::AtSpawn && g_salvage_info.state == SalFlagState::AtSpawn) {
            salvage_play_return_sound();
        }
    }
}

void salvage_level_init()
{
    g_salvage_info = SalvageInfo{};
    // The old level took its lights with it, so the handle is only a stale number.
    reset_salvage_local_state(false);

    // The flag_red class overrides (spin bit, Salvage mesh) are reverted and
    // (re)applied from salvage_apply_flag_class_overrides, which the engine's CTF
    // level init calls after this for every game type.
    g_salvage_info.active = gt_is_salvage();
    if (!g_salvage_info.active) return;

    // Resolved on both sides: clients need it to find the replicated flag item.
    g_salvage_info.flag_item_type = rf::item_lookup_type(kSalFlagItemClass);
    if (g_salvage_info.flag_item_type < 0) {
        xlog::warn("salvage: item class '{}' is not registered", kSalFlagItemClass);
    }

    g_salvage_info.state = SalFlagState::Delayed;
}

void salvage_level_init_post()
{
    if (!g_salvage_info.active || !rf::is_server) return;

    resolve_base_positions();
    remove_level_ctf_flags();
    resolve_flag_spawn();

    enter_delayed_state(g_alpine_server_config_active_rules.salvage.flag_spawn_delay_ms);
    salvage_broadcast_state();
}

void salvage_on_flag_touch(rf::Player* player, rf::Item* item)
{
    if (!rf::is_server || !player || !item) return;
    if (item->handle != g_salvage_info.flag_item_handle) return;
    if (g_salvage_info.state != SalFlagState::AtSpawn
        && g_salvage_info.state != SalFlagState::Dropped) return;
    if (item->item_flags & rf::IF_NO_PICKUP) return;
    // Same rejection the carrier validation in salvage_do_frame applies: a spectator
    // must never end up holding the flag.
    if (player->is_spectator) return;

    rf::Entity* ep = alive_entity_for(player);
    if (!ep || rf::entity_is_dying(ep)) return;

    // The item stays alive and becomes the flag on the carrier's back — the same
    // trick stock CTF plays. The touch hook above is state-gated, but flagging the
    // item unpickable keeps the engine's generic item paths out of it too.
    item->item_flags |= rf::IF_NO_PICKUP;
    g_salvage_info.pickup_unlock_timer.invalidate();
    // A carried flag must not spin: item_render would override the orientation the
    // attachment just computed. CTF clears the same bit when a flag is taken.
    set_flag_item_class_spin(false);

    set_carrier(player);
    g_salvage_info.state = SalFlagState::Carried;
    g_salvage_info.last_carrier_pos = ep->pos;
    g_salvage_info.return_timer.invalidate();
    g_salvage_info.spawn_delay_timer.invalidate();
    g_salvage_info.refresh_timer.invalidate();
    salvage_move_carried_flag(); // attach on the same frame it is taken

    announce(std::format("{} has the flag!", player->name.c_str()));
    play_local_transition_sound(kSalSoundFlagSteal);
    salvage_broadcast_state();
}

void salvage_on_base_touch(rf::Player* player, rf::Item* item)
{
    if (!rf::is_server || !player || !item) return;
    if (g_salvage_info.state != SalFlagState::Carried) return;
    if (g_salvage_info.carrier != player) return;

    const bool red_base = (item->item_flags & rf::IF_RED_BASE) != 0;
    const bool blue_base = (item->item_flags & rf::IF_BLUE_BASE) != 0;
    if (!red_base && !blue_base) return;
    // Capture only at your OWN base.
    if (red_base && player->team != rf::TEAM_RED) return;
    if (blue_base && player->team != rf::TEAM_BLUE) return;

    if (red_base) {
        ++g_salvage_info.red_caps;
    }
    else {
        ++g_salvage_info.blue_caps;
    }

    rf::player_add_score(player, 4);
    if (player->stats) {
        ++player->stats->caps;
        player->stats->took_part_in_flag_capture = true;
    }

    kill_current_flag_item();
    enter_delayed_state(g_alpine_server_config_active_rules.salvage.flag_capture_respawn_delay_ms);

    announce(std::format("{} captured the flag for the {} team!", player->name.c_str(),
        red_base ? "RED" : "BLUE"));
    play_local_transition_sound(kSalSoundFlagCapture);
    salvage_broadcast_state();
}

void salvage_on_entity_will_die(rf::Entity* ep)
{
    if (!rf::is_server || !gt_is_salvage() || !ep) return;
    rf::Player* player = rf::player_from_entity_handle(ep->handle);
    if (!player || player != g_salvage_info.carrier) return;
    drop_flag_at(player, ep->pos);
}

void salvage_on_player_disconnect(rf::Player* player)
{
    if (!gt_is_salvage()) return;
    if (g_salvage_info.carrier != player) return;

    if (rf::is_server) {
        rf::Entity* ep = rf::entity_from_handle(player->entity_handle);
        drop_flag_at(player, ep ? ep->pos : g_salvage_info.last_carrier_pos);
    }
    else {
        // The carrier Player struct is about to be freed; drop our reference so
        // render/HUD paths don't touch it before the next state packet arrives.
        set_carrier(nullptr);
    }
}

void salvage_handle_drop_flag_request(rf::Player* player)
{
    if (!rf::is_server || !gt_is_salvage()) return;
    if (g_salvage_info.state != SalFlagState::Carried) return;
    if (g_salvage_info.carrier != player) return;

    rf::Entity* ep = alive_entity_for(player);
    drop_flag_at(player, ep ? ep->pos : g_salvage_info.last_carrier_pos);
}

void salvage_do_frame()
{
    if (!rf::is_server) return;
    if (!gt_is_salvage()) {
        if (g_salvage_info.active) {
            g_salvage_info = SalvageInfo{};
            // Mid-level: the light object is still alive, so hand it back rather
            // than just forgetting the handle.
            reset_salvage_local_state(true);
        }
        return;
    }
    if (!g_salvage_info.spawn_known) return;

    if (g_salvage_info.state == SalFlagState::Carried) {
        rf::Entity* ep = alive_entity_for(g_salvage_info.carrier);
        if (!ep || rf::entity_is_dying(ep) || g_salvage_info.carrier->is_spectator) {
            drop_flag_at(g_salvage_info.carrier,
                ep ? ep->pos : g_salvage_info.last_carrier_pos);
        }
        else {
            g_salvage_info.last_carrier_pos = ep->pos;
            salvage_move_carried_flag();
        }
        return;
    }

    // The grace window armed by arm_pickup_unlock has run out: make the flag
    // pickable again.
    if (g_salvage_info.pickup_unlock_timer.valid() && g_salvage_info.pickup_unlock_timer.elapsed()) {
        g_salvage_info.pickup_unlock_timer.invalidate();
        if (rf::Item* flag = item_from_handle_or_null(g_salvage_info.flag_item_handle)) {
            flag->item_flags &= ~rf::IF_NO_PICKUP;
        }
    }

    if (g_salvage_info.state == SalFlagState::Delayed) {
        if (g_salvage_info.spawn_delay_timer.valid() && g_salvage_info.spawn_delay_timer.elapsed()) {
            spawn_flag_at_home();
            announce("The flag is now available!");
            play_local_transition_sound(stock_sound_id::flag_respawn);
            salvage_broadcast_state();
            return;
        }
    }
    else if (g_salvage_info.state == SalFlagState::Dropped) {
        if (g_salvage_info.return_timer.valid() && g_salvage_info.return_timer.elapsed()) {
            spawn_flag_at_home();
            announce("The flag has returned.");
            play_local_transition_sound(stock_sound_id::flag_respawn);
            salvage_broadcast_state();
            return;
        }
    }
    else if (g_salvage_info.state == SalFlagState::AtSpawn) {
        // Recovery: the state says the flag is on the ground but the item is gone.
        // Bounded: without a flag_red item class (or with a create that keeps
        // failing) this would otherwise warn and re-broadcast every 2 seconds for
        // the rest of the level.
        const bool retry_ready = !g_salvage_info.respawn_retry_timer.valid()
            || g_salvage_info.respawn_retry_timer.elapsed();
        if (!g_respawn_recovery_given_up && g_salvage_info.flag_item_type >= 0 && retry_ready
            && item_from_handle_or_null(g_salvage_info.flag_item_handle) == nullptr) {
            xlog::warn("salvage: flag item missing at spawn — recreating");
            spawn_flag_at_home();
            if (g_salvage_info.flag_item_handle >= 0) {
                g_salvage_info.respawn_retry_timer.invalidate();
                g_respawn_retry_failures = 0;
            }
            else if (++g_respawn_retry_failures >= kSalMaxRespawnRetries) {
                g_respawn_recovery_given_up = true;
                g_salvage_info.respawn_retry_timer.invalidate();
                xlog::error("salvage: could not recreate the flag item after {} attempts; "
                    "giving up for this level", kSalMaxRespawnRetries);
            }
            else {
                g_salvage_info.respawn_retry_timer.set(kSalRespawnRetryIntervalMs);
            }
            salvage_broadcast_state();
            return;
        }
    }

    // Correct client drift on the visible countdowns.
    if (g_salvage_info.refresh_timer.valid() && g_salvage_info.refresh_timer.elapsed()) {
        g_salvage_info.refresh_timer.set(kSalStateRefreshIntervalMs);
        salvage_broadcast_state();
    }
}

// Steal. The engine dispatches here for any item carrying IF_RED_FLAG/IF_BLUE_FLAG
// once it has confirmed the toucher is a player on a server.
FunHook<void(rf::Player*, rf::Item*)> multi_ctf_apply_flag_hook{
    0x004738D0,
    [](rf::Player* pp, rf::Item* item) {
        if (!gt_is_salvage()) {
            multi_ctf_apply_flag_hook.call_target(pp, item);
            return;
        }
        salvage_on_flag_touch(pp, item);
    },
};

// Capture. Same dispatch, for IF_RED_BASE/IF_BLUE_BASE items.
FunHook<void(rf::Player*, rf::Item*)> multi_ctf_apply_item_hook{
    0x00473BA0,
    [](rf::Player* pp, rf::Item* item) {
        if (!gt_is_salvage()) {
            multi_ctf_apply_item_hook.call_target(pp, item);
            return;
        }
        salvage_on_base_touch(pp, item);
    },
};

// item_render skips a CTF flag whose carrier is the local player in first person,
// because the flag rides on their back and would only ever be drawn inside the
// camera. That test asks multi_ctf_get_*_flag_player(), which Salvage never
// populates, so the neutral flag needs the same early-out made here.
// 0x00458F99 is the first instruction after the obj_is_hidden test, clear of the
// D3D11 renderer's FunHook on the function entry; ESI holds the item and
// 0x004590F6 is the function's own epilogue.
CodeInjection item_render_hide_carried_flag_patch{
    0x00458F99,
    [](auto& regs) {
        if (!gt_is_salvage()) return;
        auto* item = reinterpret_cast<rf::Item*>(regs.esi.value);
        if (!salvage_is_flag_item(item)) return;
        if (!flag_hidden_for_local_view()) return;
        regs.eip = 0x004590F6;
    },
};

void salvage_do_patch()
{
    multi_ctf_apply_flag_hook.install();
    multi_ctf_apply_item_hook.install();
    item_render_hide_carried_flag_patch.install();
}
