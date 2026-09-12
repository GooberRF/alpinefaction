#include <patch_common/CallHook.h>
#include <patch_common/FunHook.h>
#include <common/utils/list-utils.h>
#include <xlog/xlog.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <unordered_map>
#include <vector>
#include "projectile_lag_comp.h"
#include "obj_interp_history.h"
#include "network.h"
#include "server_internal.h"
#include "multi.h"
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

// Hard cap on the client-side advance; the server's configured max is not sent to clients
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
    if (info->max_speed > 0.0f && info->lifetime_seconds > 0.0f)
        return true;
    // Continuous fire (flamethrower)
    if (info->flags & rf::WTF_CONTINUOUS_FIRE)
        return true;
    return false;
}

// --- Entity position history ---

struct PosRecord
{
    rf::Vector3 pos;
    int64_t timestamp_ms;
};

// Records closer than min_record_interval_ms are skipped so the ring spans >= 640 ms at any frame rate
constexpr int HISTORY_SIZE = 160;
constexpr int64_t min_record_interval_ms = 4;

struct PosHistory
{
    PosRecord records[HISTORY_SIZE];
    int write_idx = 0;
    int count = 0;

    void push(const rf::Vector3& p, int64_t time_ms)
    {
        records[write_idx] = {p, time_ms};
        write_idx = (write_idx + 1) % HISTORY_SIZE;
        if (count < HISTORY_SIZE)
            ++count;
    }

    // 0 = oldest
    const PosRecord& at(int i) const
    {
        int start = (write_idx - count + HISTORY_SIZE) % HISTORY_SIZE;
        return records[(start + i) % HISTORY_SIZE];
    }
};

static std::unordered_map<int, PosHistory> g_entity_pos_history; // entity handle -> history

static rf::Vector3 interpolate_position(const PosHistory& history, int64_t target_time_ms)
{
    if (history.count == 0)
        return {};

    if (target_time_ms <= history.at(0).timestamp_ms)
        return history.at(0).pos;

    if (target_time_ms >= history.at(history.count - 1).timestamp_ms)
        return history.at(history.count - 1).pos;

    for (int i = 0; i < history.count - 1; ++i) {
        const auto& a = history.at(i);
        const auto& b = history.at(i + 1);
        if (target_time_ms >= a.timestamp_ms && target_time_ms <= b.timestamp_ms) {
            int64_t dt = b.timestamp_ms - a.timestamp_ms;
            if (dt <= 0)
                return a.pos;
            float t = static_cast<float>(target_time_ms - a.timestamp_ms) / static_cast<float>(dt);
            return a.pos + (b.pos - a.pos) * t;
        }
    }

    return history.at(history.count - 1).pos;
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

static std::vector<SavedEntityPos> g_rewound_entities;

// --- Public API ---

bool projectile_lag_comp_enabled()
{
    return g_alpine_server_config.projectile_lag_comp;
}

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
    if (!pp || !pp->net_data)
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
        // The server's copy is shooter-half-ping old and the relay took our own half ping
        const auto& info = get_af_server_info();
        if (!info || !info->projectile_lag_comp || shooter == rf::local_player_entity)
            return 0.0f;
        if (!rf::local_player || !rf::local_player->net_data)
            return 0.0f;
        ms = std::min(static_cast<float>(pp->net_data->ping + rf::local_player->net_data->ping) / 2.0f,
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

    // Sweep the skipped path so the projectile cannot start inside a wall or past a victim. The shooter
    // must be an ignore or the sweep stops at zero inside its own box.
    rf::LevelCollisionOut col_out{};
    bool hit = rf::collide_linesegment_level_for_multi(
        wp->pos, new_pos, shooter, wp, &col_out, wp->info->collision_radius, false, 1.0f);

    const float speed = std::sqrt(vel.x * vel.x + vel.y * vel.y + vel.z * vel.z);
    if (hit) {
        // Back off slightly from the hit point
        new_pos = col_out.hit_point;
        if (speed > 0.001f) {
            new_pos -= vel * (0.01f / speed);
        }
    }

    wp->pos = new_pos;
    wp->p_data.pos = new_pos;
    wp->last_pos = new_pos;
    wp->p_data.vel = vel;

    wp->lifeleft_seconds = std::max(wp->lifeleft_seconds - t, 0.0f);

    xlog::trace("Projectile lag comp: advanced weapon {} by {:.1f}ms (speed {:.1f}, hit {})",
        wp->handle, t * 1000.0f, speed, hit);
}

// How far behind the server the killer's client was rendering its victims, in ms
static float rewind_ms_for_killer(int killer_handle, bool full_ping)
{
    rf::Player* pp = rf::player_from_entity_handle(killer_handle);
    if (!pp || !pp->net_data)
        return 0.0f;

    // Victims' state took half a ping to reach the shooter plus an interp delay. The client -> server
    // leg was absorbed by the advance at creation unless the packet was un-advanced (full_ping).
    float ping = static_cast<float>(pp->net_data->ping);
    float ms = full_ping ? ping : ping / 2.0f;
    // Pre-1.5 and non-Alpine clients run the stock clock (2.2 x interval behind); newer clients'
    // jitter is unknown here, assume the minimum headroom
    const float interval_ms = 1000.0f / static_cast<float>(server_player_netfps(pp));
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

static void rewind_entities_for_projectile(int killer_handle, int keep_handle, bool full_ping)
{
    float rewind_ms = rewind_ms_for_killer(killer_handle, full_ping);
    if (rewind_ms <= 0.0f)
        return;

    int64_t now = timer::get_i64(1000);
    int64_t target_time = now - static_cast<int64_t>(rewind_ms);

    for (auto& entity : DoublyLinkedList{rf::entity_list}) {
        // Skip the shooter and an entity already hit where it stands
        if (entity.handle == killer_handle || entity.handle == keep_handle)
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

bool projectile_lag_comp_rewind_for_killer(int killer_handle, int keep_handle, bool full_ping)
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

    rewind_entities_for_projectile(killer_handle, keep_handle, full_ping);
    return !g_rewound_entities.empty();
}

// --- Hooks: apply_radius_damage call sites ---
// apply_radius_damage (0x00488DC0) cannot be FunHooked (prologue FLD [ESP+8] breaks in a trampoline), so
// its call sites are CallHooked. Of the 4 sites: two below, the wall-hit site (0x004C53A8) is wrapped in
// object/weapon.cpp, and game_do_explosion (0x0043660D) passes killer -1 so a rewind would never fire.

static void radius_damage_with_rewind(
    CallHook<void(rf::Vector3&, float, float, int, int)>& hook,
    rf::Vector3& epicenter, float damage, float radius, int killer_handle, int damage_type,
    int keep_handle, bool full_ping)
{
    bool rewound = projectile_lag_comp_rewind_for_killer(killer_handle, keep_handle, full_ping);
    hook.call_target(epicenter, damage, radius, killer_handle, damage_type);
    if (rewound) {
        restore_entities_after_projectile();
    }
}

// weapon_hit_obj (0x004C62F5): epicenter is wp->p_data.collide_out.hit_point. The directly hit object
// already took damage at its live position, so it stays there for the splash.
static_assert(offsetof(rf::PCollisionOut, hit_point) == 0);
static_assert(offsetof(rf::PCollisionOut, obj_handle) == 0x30);
CallHook<void(rf::Vector3&, float, float, int, int)> weapon_hit_entity_radius_damage_hook{
    0x004C62F5,
    [](rf::Vector3& epicenter, float damage, float radius, int killer_handle, int damage_type) {
        const auto& collide_out = *reinterpret_cast<rf::PCollisionOut*>(&epicenter);
        radius_damage_with_rewind(weapon_hit_entity_radius_damage_hook,
            epicenter, damage, radius, killer_handle, damage_type, collide_out.obj_handle, false);
    },
};

// weapon_process_pre lifetime explosion (0x004C6C94): epicenter is wp->pos. A remote charge detonates
// on an un-advanced detonator packet, so its shooter is a full round trip behind.
static_assert(offsetof(rf::Object, pos) == 0x3C);
CallHook<void(rf::Vector3&, float, float, int, int)> weapon_explode_radius_damage_hook{
    0x004C6C94,
    [](rf::Vector3& epicenter, float damage, float radius, int killer_handle, int damage_type) {
        const auto* wp = reinterpret_cast<const rf::Weapon*>(
            reinterpret_cast<const std::byte*>(&epicenter) - offsetof(rf::Object, pos));
        const bool detonated = wp->info_index == rf::remote_charge_weapon_type;
        radius_damage_with_rewind(weapon_explode_radius_damage_hook,
            epicenter, damage, radius, killer_handle, damage_type, -1, detonated);
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
            float rewind_ms = rewind_ms_for_killer(particle->parent_handle, false);
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
    weapon_hit_entity_radius_damage_hook.install();
    weapon_explode_radius_damage_hook.install();
    particle_can_damage_entity_hook.install();
}

void projectile_lag_comp_on_level_init()
{
    g_entity_pos_history.clear();
    g_rewound_entities.clear();
}
