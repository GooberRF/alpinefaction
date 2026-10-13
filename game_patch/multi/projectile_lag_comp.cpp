#include <patch_common/CallHook.h>
#include <patch_common/FunHook.h>
#include <common/utils/list-utils.h>
#include <xlog/xlog.h>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>
#include "projectile_lag_comp.h"
#include "obj_interp_history.h"
#include "network.h"
#include "server_internal.h"
#include "multi.h"
#include "vehicles/vehicle.h"
#include "../misc/alpine_settings.h"
#include "../rf/entity.h"
#include "../rf/weapon.h"
#include "../rf/object.h"
#include "../rf/physics.h"
#include "../rf/collide.h"
#include "../rf/multi.h"
#include "../rf/player/player.h"
#include "../rf/particle_emitter.h"
#include "../os/os.h"

// Hard cap on the client-side advance
constexpr float client_max_advance_ms = 500.0f;

// --- Weapon classification ---

static bool is_projectile_weapon(const rf::Weapon* wp)
{
    if (!wp || !wp->info)
        return false;
    const auto* info = wp->info;
    // Melee and detonators are not projectiles
    if (info->flags & (rf::WTF_MELEE | rf::WTF_DETONATOR))
        return false;
    // Stock hitscan lag comp already compensates these; advancing too would double-compensate
    if (info->flags2 & rf::WTF2_STOCK_LAG_COMP)
        return false;
    // Includes the flamethrower ($Velocity 10, $Lifetime 10)
    return info->max_speed > 0.0f && info->lifetime_seconds > 0.0f;
}

namespace
{

// --- Entity position history ---

struct PosRecord
{
    rf::Vector3 pos;
    int64_t timestamp_ms;
};

// Records closer than min_record_interval_ms are skipped so the ring spans >= 640 ms at any frame rate
constexpr int history_size = 160;
constexpr int64_t min_record_interval_ms = 4;

struct PosHistory
{
    PosRecord records[history_size];
    int write_idx = 0;
    int count = 0;

    void push(const rf::Vector3& p, int64_t time_ms)
    {
        records[write_idx] = {p, time_ms};
        write_idx = (write_idx + 1) % history_size;
        if (count < history_size)
            ++count;
    }

    // 0 = oldest
    const PosRecord& at(int i) const
    {
        int start = (write_idx - count + history_size) % history_size;
        return records[(start + i) % history_size];
    }
};

std::unordered_map<int, PosHistory> g_entity_pos_history; // entity handle -> history

rf::Vector3 interpolate_position(const PosHistory& history, int64_t target_time_ms)
{
    if (history.count == 0)
        return {};

    if (target_time_ms <= history.at(0).timestamp_ms)
        return history.at(0).pos;

    if (target_time_ms >= history.at(history.count - 1).timestamp_ms)
        return history.at(history.count - 1).pos;

    // Binary search for the bracketing pair: records are oldest first with strictly increasing timestamps, and
    // the checks above leave at(lo) < target < at(hi). Runs per particle per entity for the flamethrower.
    int lo = 0;
    int hi = history.count - 1;
    while (hi - lo > 1) {
        const int mid = (lo + hi) / 2;
        if (history.at(mid).timestamp_ms <= target_time_ms)
            lo = mid;
        else
            hi = mid;
    }
    const auto& a = history.at(lo);
    const auto& b = history.at(hi);
    const int64_t dt = b.timestamp_ms - a.timestamp_ms;
    const int64_t into = target_time_ms - a.timestamp_ms;
    // A jump faster than anything moves (teleporter, respawn) is not interpolated across: the victim was
    // at one end or the other, never in between
    constexpr float teleport_speed = 100.0f; // m/s
    constexpr float slack_sq = 1.0f;         // m^2
    if ((b.pos - a.pos).len_sq() > (teleport_speed * dt / 1000.0f) * (teleport_speed * dt / 1000.0f) + slack_sq)
        return 2 * into < dt ? a.pos : b.pos;
    return a.pos + (b.pos - a.pos) * (static_cast<float>(into) / static_cast<float>(dt));
}

// --- Entity rewind/restore ---

struct SavedEntityPos
{
    int handle;
    rf::Vector3 original_pos;
    rf::Vector3 original_p_data_pos;
    rf::Vector3 original_bbox_min;
    rf::Vector3 original_bbox_max;
};

std::vector<SavedEntityPos> g_rewound_entities;

bool projectile_lag_comp_enabled()
{
    return g_alpine_server_config.projectile_lag_comp;
}
} // namespace

// --- Public API ---

void projectile_lag_comp_record_positions()
{
    if (!rf::is_server || !projectile_lag_comp_enabled())
        return;

    int64_t now = timer::get_i64(1000);

    for (auto& entity : DoublyLinkedList{rf::entity_list}) {
        auto& history = g_entity_pos_history[entity.handle];
        if (history.count == 0 || now - history.at(history.count - 1).timestamp_ms >= min_record_interval_ms) {
            history.push(entity.pos, now);
        }
    }

    // Entities respawn with fresh handles; drop histories of entities that no longer exist
    static int frames_since_sweep = 0;
    if (++frames_since_sweep >= 1024) {
        frames_since_sweep = 0;
        std::erase_if(g_entity_pos_history,
                      [](const auto& kv) { return rf::entity_from_handle(kv.first) == nullptr; });
    }
}

// How far a just-created projectile is behind the shooter's own copy of it, in seconds
static float advance_seconds_for_shooter(rf::Entity* shooter)
{
    rf::Player* pp = rf::player_from_entity_handle(shooter->handle);
    if (!pp || !pp->net_data || (rf::is_server && pp == rf::local_player))
        return 0.0f;

    float ms;
    if (rf::is_server) {
        if (!projectile_lag_comp_enabled())
            return 0.0f;
        // The fire packet took half a ping to reach us
        ms = std::min(static_cast<float>(pp->net_data->ping) / 2.0f,
                      static_cast<float>(g_alpine_server_config.projectile_lag_comp_max_ms));
    }
    else {
        // The server advanced its copy by the shooter's half ping (capped at its max) and the relay took
        // our own half ping
        const auto& info = get_af_server_info();
        if (!info || !info->projectile_lag_comp || shooter == rf::local_player_entity)
            return 0.0f;
        if (!rf::local_player || !rf::local_player->net_data)
            return 0.0f;
        float server_advance_ms = static_cast<float>(pp->net_data->ping) / 2.0f;
        if (info->projectile_lag_comp_max_ms > 0)
            server_advance_ms = std::min(server_advance_ms, static_cast<float>(info->projectile_lag_comp_max_ms));
        ms = std::min(server_advance_ms + static_cast<float>(rf::local_player->net_data->ping) / 2.0f,
                      client_max_advance_ms);
    }
    return ms / 1000.0f;
}

void projectile_lag_comp_advance_weapon(rf::Entity* shooter, rf::Weapon* wp)
{
    if (!rf::is_multi || !shooter || !wp || !is_projectile_weapon(wp))
        return;

    const float t = advance_seconds_for_shooter(shooter);
    if (t <= 0.0f)
        return;

    // weapon_create set p_data.vel and PF_GRAVITY; neither creation site touches them afterwards
    rf::Vector3 vel = wp->p_data.vel;
    rf::Vector3 new_pos = wp->pos + vel * t;
    if (wp->p_data.flags & rf::PF_GRAVITY) {
        new_pos.y -= 0.5f * rf::gravity * t * t;
        vel.y -= rf::gravity * t;
    }

    // The fuse runs on the shooter's timeline wherever the projectile ends up
    wp->lifeleft_seconds = std::max(wp->lifeleft_seconds - t, 0.0f);

    // Sweep the skipped path so the projectile cannot start inside a wall or past a victim. The shooter
    // must be an ignore or the sweep stops at zero inside its own box. On the server the victims are where
    // the shooter saw them, as for the direct-hit test that takes over from here.
    rf::LevelCollisionOut col_out{};
    const bool rewound = rf::is_server && projectile_lag_comp_rewind_for_killer(shooter->handle);
    const float radius = wp->info->collision_radius;
    bool hit = rf::collide_linesegment_level_for_multi(wp->pos, new_pos, shooter, wp, &col_out, radius, false, 1.0f);
    // That test sees vehicle hulls as human-sized pillars; it left new_pos clipped to any world hit
    hit |= vehicle_collide_segment_hulls(wp->pos, new_pos, shooter, vehicle_ridden_hull(shooter), radius, col_out);
    if (rewound)
        restore_entities_after_projectile();

    if (hit) {
        // The sweep is a thin line: back off a collision radius so the sphere does not start inside the
        // surface, or stay at the muzzle when the hit is closer than that
        rf::Vector3 dir = col_out.hit_point - wp->pos;
        const float dist = dir.len();
        const float back_off = radius + 0.01f;
        if (dist <= back_off) {
            return;
        }
        new_pos = col_out.hit_point - dir * (back_off / dist);
    }

    wp->pos = new_pos;
    wp->p_data.pos = new_pos;
    wp->last_pos = new_pos;
    wp->p_data.vel = vel;

    xlog::trace("Projectile lag comp: advanced weapon {} by {:.1f}ms (hit {})", wp->handle, t * 1000.0f, hit);
}

// How far behind the server the killer's client was rendering its victims, in ms
static float rewind_ms_for_killer(int killer_handle, bool full_ping)
{
    rf::Player* pp = rf::player_from_entity_handle(killer_handle);
    // The listen host sees the server's live state
    if (!pp || !pp->net_data || pp == rf::local_player)
        return 0.0f;

    // Victims' state took half a ping to reach the shooter plus an interp delay. The client -> server
    // leg was absorbed by the advance at creation unless the packet was un-advanced (full_ping).
    float ping = static_cast<float>(pp->net_data->ping);
    float ms = full_ping ? ping : ping / 2.0f;
    // Pre-1.5 and non-Alpine clients run the stock clock (2.2 x interval behind); newer clients'
    // jitter is unknown here, assume the minimum headroom
    const float interval_ms = 1000.0f / static_cast<float>(g_alpine_game_config.server_netfps);
    const bool stock_interp = pp->version_info.software != ClientSoftware::AlpineFaction
        || version_is_older(pp->version_info.major, pp->version_info.minor, 1, 5);
    ms += stock_interp ? 2.2f * interval_ms : obj_interp_target_delay_ms(interval_ms, 0.0f);
    return std::min(ms, static_cast<float>(g_alpine_server_config.projectile_lag_comp_max_ms));
}

static void rewind_entity(rf::Entity& entity, int64_t target_time)
{
    auto it = g_entity_pos_history.find(entity.handle);
    if (it == g_entity_pos_history.end())
        return;

    rf::Vector3 rewound_pos = interpolate_position(it->second, target_time);
    rf::Vector3 delta = rewound_pos - entity.pos;

    g_rewound_entities.push_back(
        {entity.handle, entity.pos, entity.p_data.pos, entity.p_data.bbox_min, entity.p_data.bbox_max});

    // obj_apply_radius_damage selects victims by physics bbox and does LOS against p_data.pos: move both
    entity.pos = rewound_pos;
    entity.p_data.pos = rewound_pos;
    entity.p_data.bbox_min += delta;
    entity.p_data.bbox_max += delta;
}

static void rewind_entities_for_projectile(int killer_handle, bool full_ping)
{
    float rewind_ms = rewind_ms_for_killer(killer_handle, full_ping);
    if (rewind_ms <= 0.0f)
        return;

    int64_t now = timer::get_i64(1000);
    int64_t target_time = now - static_cast<int64_t>(rewind_ms);

    for (auto& entity : DoublyLinkedList{rf::entity_list}) {
        if (entity.handle == killer_handle)
            continue;
        rewind_entity(entity, target_time);
    }

    xlog::trace("Projectile lag comp: rewound {} entities by {:.1f}ms for killer {}",
        g_rewound_entities.size(), rewind_ms, killer_handle);
}

void restore_entities_after_projectile()
{
    for (auto& saved : g_rewound_entities) {
        rf::Object* obj = rf::obj_from_handle(saved.handle);
        if (obj) {
            obj->pos = saved.original_pos;
            obj->p_data.pos = saved.original_p_data_pos;
            obj->p_data.bbox_min = saved.original_bbox_min;
            obj->p_data.bbox_max = saved.original_bbox_max;
        }
    }
    g_rewound_entities.clear();
}

bool projectile_lag_comp_rewind_for_killer(int killer_handle, bool full_ping)
{
    if (!rf::is_server || !projectile_lag_comp_enabled() || killer_handle <= 0)
        return false;

    // A nested rewind would share (and clear) the restore list of the outer one
    if (!g_rewound_entities.empty())
        return false;

    rf::Object* killer_obj = rf::obj_from_handle(killer_handle);
    if (!killer_obj || killer_obj->type != rf::OT_ENTITY)
        return false;

    if (!rf::player_from_entity_handle(killer_handle))
        return false;

    rewind_entities_for_projectile(killer_handle, full_ping);
    return !g_rewound_entities.empty();
}

rf::Vector3 projectile_lag_comp_rewound_offset(int handle)
{
    for (const auto& saved : g_rewound_entities) {
        if (saved.handle == handle) {
            if (rf::Object* obj = rf::obj_from_handle(handle))
                return obj->pos - saved.original_pos;
        }
    }
    return {};
}

// --- Hook: direct projectile hits ---
// (The three blast call sites that rewind splash victims are hooked in object/weapon.cpp.)
// The physics pair pass (FUN_0048CA60) tests a player's projectile against each entity one pair at a time, in
// collide_object_object_mesh (0x0049AFE0): the weapon's collision spheres against the entity's animated vmesh.
// For the entity it reads p_data.pos/next_pos and the bbox (the pair's overlap gate); orientation and pose stay
// live, and it writes only the two collide_out records. A lag-compensated projectile is tested against the
// entity where its shooter saw it, for that one call only. The hit point comes out in that frame and is moved
// back onto the live entity, so body part, damage and effects see the same spot on the body; the splash
// epicenter then follows the victim's rewind (weapon.cpp).

static bool direct_hit_offset(const rf::Weapon* wp, const rf::Entity* ep, rf::Vector3& delta)
{
    if (!rf::is_server || !projectile_lag_comp_enabled() || ep->handle == wp->parent_handle
        || !is_projectile_weapon(wp) || !g_rewound_entities.empty())
        return false;
    const float rewind_ms = rewind_ms_for_killer(wp->parent_handle, false);
    if (rewind_ms <= 0.0f)
        return false;
    auto it = g_entity_pos_history.find(ep->handle);
    if (it == g_entity_pos_history.end() || it->second.count == 0)
        return false;
    delta = interpolate_position(it->second, timer::get_i64(1000) - static_cast<int64_t>(rewind_ms)) - ep->pos;
    return true;
}

static bool collide_weapon_entity_rewound(CallHook<bool(rf::Object*, rf::Object*)>& hook, rf::Object* objp,
                                          rf::Object* mesh_objp)
{
    // Only the verified order: projectile first, entity (the vmesh side) second
    rf::Vector3 delta;
    if (objp->type != rf::OT_WEAPON || mesh_objp->type != rf::OT_ENTITY
        || !direct_hit_offset(static_cast<rf::Weapon*>(objp), static_cast<rf::Entity*>(mesh_objp), delta))
        return hook.call_target(objp, mesh_objp);

    rf::PhysicsData& victim = mesh_objp->p_data;
    const rf::Vector3 saved_pos = victim.pos;
    const rf::Vector3 saved_next_pos = victim.next_pos;
    const rf::Vector3 saved_bbox_min = victim.bbox_min;
    const rf::Vector3 saved_bbox_max = victim.bbox_max;
    victim.pos += delta;
    victim.next_pos += delta;
    victim.bbox_min += delta;
    victim.bbox_max += delta;

    rf::PCollisionOut& weapon_out = objp->p_data.collide_out;
    const int handle_before = weapon_out.obj_handle;
    const float time_before = weapon_out.hit_time;
    const bool hit = hook.call_target(objp, mesh_objp);

    victim.pos = saved_pos;
    victim.next_pos = saved_next_pos;
    victim.bbox_min = saved_bbox_min;
    victim.bbox_max = saved_bbox_max;

    // This call recorded the hit: move the point back onto the live entity. The entity's own record gets a
    // copy of the same point when the engine fills it (heavy projectiles). The engine's fast path for
    // PF_UNK_400 records a hit without computing a point, so there is nothing to move.
    const bool fast_path = ((objp->p_data.flags | victim.flags) & rf::PF_UNK_400) != 0;
    if (!fast_path && weapon_out.obj_handle == mesh_objp->handle
        && (handle_before != mesh_objp->handle || weapon_out.hit_time != time_before)) {
        const rf::Vector3 rewound_point = weapon_out.hit_point;
        weapon_out.hit_point -= delta;
        rf::PCollisionOut& victim_out = victim.collide_out;
        if (victim_out.obj_handle == objp->handle && victim_out.hit_point == rewound_point)
            victim_out.hit_point = weapon_out.hit_point;
    }
    return hit;
}

// Both calls of the pair pass (not weapon_create's instant sweep at 0x004C7FC8, which runs before the advance)
CallHook<bool(rf::Object*, rf::Object*)> pair_collide_mesh_hook{
    0x0048CB51,
    [](rf::Object* objp, rf::Object* mesh_objp) {
        return collide_weapon_entity_rewound(pair_collide_mesh_hook, objp, mesh_objp);
    },
};

CallHook<bool(rf::Object*, rf::Object*)> pair_collide_mesh_reversed_hook{
    0x0048CB6E,
    [](rf::Object* objp, rf::Object* mesh_objp) {
        return collide_weapon_entity_rewound(pair_collide_mesh_reversed_hook, objp, mesh_objp);
    },
};

// --- Hook: flamethrower particle hit test ---
// particle_move_all_in_list (0x00495120) calls particle_can_damage_entity (0x00494D40) per entity with the
// wielder as killer. Rewind only the entity under test; the damage itself is not position dependent.
CallHook<bool(rf::Particle*, float, rf::Entity*)> particle_can_damage_entity_hook{
    0x004954D8,
    [](rf::Particle* particle, float damage, rf::Entity* entity) {
        bool rewound = false;
        if (rf::is_server && projectile_lag_comp_enabled() && g_rewound_entities.empty()
            && entity->handle != particle->parent_handle
            && rf::player_from_entity_handle(particle->parent_handle)) {
            // Flame particles are never advanced, so as for an un-advanced detonation the wielder saw its
            // victims a full round trip before the server applies the flame
            float rewind_ms = rewind_ms_for_killer(particle->parent_handle, true);
            if (rewind_ms > 0.0f) {
                rewind_entity(*entity, timer::get_i64(1000) - static_cast<int64_t>(rewind_ms));
                rewound = !g_rewound_entities.empty();
            }
        }
        bool hit = particle_can_damage_entity_hook.call_target(particle, damage, entity);
        if (rewound) {
            restore_entities_after_projectile();
        }
        return hit;
    },
};

// --- Init / cleanup ---

void projectile_lag_comp_init()
{
    pair_collide_mesh_hook.install();
    pair_collide_mesh_reversed_hook.install();
    particle_can_damage_entity_hook.install();
}

void projectile_lag_comp_on_level_init()
{
    g_entity_pos_history.clear();
    g_rewound_entities.clear();
}
