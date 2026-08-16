#include <algorithm>
#include <chrono>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>
#include <patch_common/CallHook.h>
#include <patch_common/FunHook.h>
#include <xlog/xlog.h>
#include "../rf/clutter.h"
#include "../rf/entity.h"
#include "../rf/item.h"
#include "../rf/math/vector.h"
#include "../rf/multi.h"
#include "../rf/object.h"
#include "../rf/player/player.h"
#include "../rf/weapon.h"
#include "../fflink/afstats_client.h" // AFSTATS_VERIFICATION_LOGGING
#include "../fflink/afstats_events.h"
#include "alpine_packets.h"
#include "kill_attribution.h"

// Server-side capture of what actually landed the killing blow. The stock obj_kill packet
// carries no weapon at all, so every client used to guess it from replicated held-weapon
// state; the data collected here is shipped alongside instead.

// Everything that describes what is currently being resolved inside an obj_damage or impact call
// tree. Grouped so the invariants live in one place: every field here is scope-owned -- written
// only by one of the RAII guards below, read only through the accessors -- and none of it means
// anything outside such a tree.
//
// The guards restore PER CONCERN, never by snapshotting the whole struct: splash_hit_counted has
// to survive the nested obj_damage calls inside one detonation, or every victim of a blast gets a
// fresh unspent hit.
struct DamageResolutionContext
{
    // Weapon and splash flag of the obj_damage call being processed.
    DamageWeaponContext damage{};
    // Weapon of the explosion whose radius-damage tree is currently running.
    std::optional<int> splash_weapon{};
    // Inside weapon_hit_obj: a real projectile delivered this, as opposed to a per-frame processor
    // that merely names a weapon (the burn spread does exactly that).
    bool projectile_impact = false;
    // Inside a SplashWeaponScope, and whether that detonation's single accuracy hit is spent.
    bool splash_scope = false;
    bool splash_hit_counted = false;
    // A damaging particle is applying its contact damage (the flamethrower stream).
    bool particle_damage = false;
};
static DamageResolutionContext g_ctx;

// Restores one field on the way out, so an early return inside a hooked call cannot leak scope
// state.
template<typename T>
class ScopedRestore
{
public:
    explicit ScopedRestore(T& slot) : slot_(slot), saved_(slot) {}
    ~ScopedRestore() { slot_ = saved_; }
    ScopedRestore(const ScopedRestore&) = delete;
    ScopedRestore& operator=(const ScopedRestore&) = delete;
    ScopedRestore(ScopedRestore&&) = delete;
    ScopedRestore& operator=(ScopedRestore&&) = delete;

private:
    T& slot_;
    T saved_;
};

// Marks the obj_damage call being processed with its weapon, or with the enclosing explosion's
// weapon when the call itself names none.
class DamageWeaponScope
{
public:
    explicit DamageWeaponScope(int weapon_type) : restore_(g_ctx.damage)
    {
        if (weapon_type >= 0) {
            g_ctx.damage = {weapon_type, false};
        }
        else if (g_ctx.splash_weapon) {
            g_ctx.damage = {*g_ctx.splash_weapon, true};
        }
        else {
            g_ctx.damage = {};
        }
    }

    DamageWeaponScope(const DamageWeaponScope&) = delete;
    DamageWeaponScope& operator=(const DamageWeaponScope&) = delete;

private:
    ScopedRestore<DamageWeaponContext> restore_;
};

// Marks damage as delivered by a projectile impact rather than by a per-frame processor.
class ProjectileImpactScope
{
public:
    ProjectileImpactScope() : restore_(g_ctx.projectile_impact) { g_ctx.projectile_impact = true; }

    ProjectileImpactScope(const ProjectileImpactScope&) = delete;
    ProjectileImpactScope& operator=(const ProjectileImpactScope&) = delete;

private:
    ScopedRestore<bool> restore_;
};

// Damage delivered by a damaging particle. Scoped to one call inside the particle update, so
// nothing else in that system and nothing in the burn processors is covered.
class ParticleDamageScope
{
public:
    ParticleDamageScope() : restore_(g_ctx.particle_damage) { g_ctx.particle_damage = true; }

    ParticleDamageScope(const ParticleDamageScope&) = delete;
    ParticleDamageScope& operator=(const ParticleDamageScope&) = delete;

private:
    ScopedRestore<bool> restore_;
};

// Body region of the most recent projectile hit test, tagged with the entity it was for so a
// hit on someone else cannot be mistaken for a hit on the victim.
struct HitRegionContext
{
    int entity_handle = 0;
    int region = -1;
};
static HitRegionContext g_hit_region_ctx;

struct KillAttributionRecord
{
    KillAttribution attr;
    std::chrono::steady_clock::time_point recorded_at;
    uint32_t sequence = 0;
};

// Long enough to survive the send + print sequence, short enough that a record can
// never be picked up by an unrelated later death.
static constexpr std::chrono::seconds kill_attribution_lifetime{5};

static std::unordered_map<uint8_t, KillAttributionRecord> g_kill_attributions;

// Identifies a recorded death. The send path is deliberately non-consuming (the kill
// message is printed from the record afterwards), so without an identity a victim who dies
// twice inside the record lifetime through a path that records nothing - a silent kill via
// rounds_kill_entity_silent or rf::entity_maybe_die, which never reach entity_damage - would
// have their second death announced with the first death's weapon, flags and assists.
static uint32_t g_kill_attribution_next_sequence = 1;
// Last sequence already announced for each victim, so a record is only ever sent once.
static std::unordered_map<uint8_t, uint32_t> g_kill_attribution_sent_sequence;

// How long after a hit an attacker still counts as part of the fight. Mirrors the client
// damage indicator, which hud_world.cpp arms with timestamp.set_ms(1000) and re-arms on
// every hit, so "still trading with this player" means the same thing on both sides.
static constexpr std::chrono::milliseconds combat_chain_window{1000};

struct CombatChain
{
    std::chrono::steady_clock::time_point expires_at;
    std::unordered_map<uint8_t, std::chrono::steady_clock::time_point> contributors;
};

static std::unordered_map<uint8_t, CombatChain> g_combat_chains;

// -2 = not resolved yet, -1 = no such weapon in the loaded tables.
static int g_riot_shield_weapon_type = -2;

// Multiplayer server only: single player and clients must not pay for any of the hooks below.
static bool kill_attribution_is_active()
{
    return rf::is_multi && rf::is_server;
}

SplashWeaponScope::SplashWeaponScope(rf::Weapon* wp)
{
    if (!kill_attribution_is_active() || !wp) {
        return;
    }
    active_ = true;
    prev_ = g_ctx.splash_weapon;
    prev_hit_counted_ = g_ctx.splash_hit_counted;
    g_ctx.splash_weapon = wp->info_index;
    // A fresh detonation starts with its hit unspent, whatever the enclosing one did. Keep this
    // ctor free of consuming or erasing: weapon_move_one constructs it every frame per live
    // weapon, so anything consumed here fires on a projectile's first frame, not at its impact.
    g_ctx.splash_scope = true;
    g_ctx.splash_hit_counted = false;
}

SplashWeaponScope::~SplashWeaponScope()
{
    if (active_) {
        // Restore instead of clearing: detonations chain (a rocket setting off a
        // remote charge) and each explosion must keep its own weapon, and its own
        // unspent accuracy hit.
        g_ctx.splash_weapon = prev_;
        g_ctx.splash_hit_counted = prev_hit_counted_;
        g_ctx.splash_scope = prev_.has_value();
    }
}

bool kill_attribution_in_splash_scope()
{
    return g_ctx.splash_scope;
}

bool kill_attribution_splash_hit_consume()
{
    if (!g_ctx.splash_scope) {
        // Fail closed. Every stock splash application arrives inside a scope, so this costs
        // nothing there; it stops a modded lag-comp weapon with a blast radius scoring per victim.
        return false;
    }
    if (g_ctx.splash_hit_counted) {
        return false;
    }
    g_ctx.splash_hit_counted = true;
    return true;
}

// Universal damage dispatcher and the only place a weapon type is passed in.
// entity_damage, one level down, never sees it.
FunHook<float(int, float, int, int, int, rf::Vector3*, int, char)> obj_damage_hook{
    0x004892C0,
    [](int victim_handle, float damage, int killer_handle, int weapon_type, int damage_type,
       rf::Vector3* pos, int killer_uid, char flags) {
        if (!kill_attribution_is_active()) {
            return obj_damage_hook.call_target(victim_handle, damage, killer_handle, weapon_type, damage_type, pos, killer_uid, flags);
        }

        // Stats stream: snapshot whether this blow is about to kill a live
        // clutter prop, so the alive->dead transition can be detected after the damage
        // lands. obj_damage is the lethal-blow context that carries killer/weapon/damage
        // type. Already-dead / delayed-delete clutter is excluded so a repeat blow on a
        // corpse cannot emit a second time.
        const rf::Object* const victim_before = rf::obj_from_handle(victim_handle);
        // Local, not a static: obj_damage recurses (chained deaths), and a shared flag
        // would be clobbered by the inner call before the outer one reads it.
        const bool clutter_alive_before = victim_before && victim_before->type == rf::OT_CLUTTER
            && !(victim_before->obj_flags & rf::OF_DELAYED_DELETE) && victim_before->life > 0.0f;

        // obj_damage recurses (chained deaths, explosions setting off explosions), so the outer
        // call's context is restored on the way out rather than blindly cleared. The guard is
        // scoped to the call alone: everything after it runs with the enclosing context back in
        // place, exactly as the manual restore this replaced did.
        float real_damage;
        {
            const DamageWeaponScope weapon_scope{weapon_type};
            real_damage = obj_damage_hook.call_target(victim_handle, damage, killer_handle,
                                                      weapon_type, damage_type, pos,
                                                      killer_uid, flags);
        }

        // Re-resolve rather than reusing the pre-call pointer: RF handles carry a
        // generation, so a victim freed inside the damage call yields null here instead of
        // a dangling read. That removes the whole lifetime assumption -- whether clutter
        // death defers deletion or not, this can only ever touch a live object.
        if (clutter_alive_before) {
            rf::Object* const after = rf::obj_from_handle(victim_handle);
            if (after && after->type == rf::OT_CLUTTER && after->life <= 0.0f) {
                afstats::on_clutter_destroyed(static_cast<rf::Clutter*>(after), killer_handle,
                                              weapon_type, damage_type);
            }
        }
        return real_damage;
    },
};

// Picks the collision sphere a projectile hit and returns its damage multiplier. Runs just
// before the obj_damage call for the same hit, which is what makes the region usable as the
// location of the lethal blow.
FunHook<float(rf::Entity*, rf::Weapon*, rf::Vector3*, int*)> get_hit_region_multiplier_hook{
    0x0042CE00,
    [](rf::Entity* victim, rf::Weapon* wp, rf::Vector3* hit_pos, int* out_region) {
        const float multiplier =
            get_hit_region_multiplier_hook.call_target(victim, wp, hit_pos, out_region);
        if (kill_attribution_is_active() && victim && out_region) {
            g_hit_region_ctx = {victim->handle, *out_region};
        }
        return multiplier;
    },
};

// Projectile hit an object: direct hit passes the weapon type explicitly, the impact
// splash that follows does not.
FunHook<bool(rf::Weapon*)> weapon_hit_obj_hook{
    0x004C59F0,
    [](rf::Weapon* wp) {
        SplashWeaponScope splash_scope{wp};
        // The direct-hit obj_damage lives inside this function, so the flag marks damage a real
        // impact produced.
        const ProjectileImpactScope impact_scope;
        const bool result = weapon_hit_obj_hook.call_target(wp);
        return result;
    },
};

// The flamethrower stream damages through the particle system, not weapon objects: the particle
// update applies contact damage here with weapon_type = -1, which is why it arrives naming no
// weapon and inside no projectile scope. Hooked at the CALL SITE, not the function, so the burn
// spread and the Flaming Enemies DoT stay outside it.
CallHook<float(int, float, int, int, int, rf::Vector3*, int, char)> particle_damage_hook{
    0x00495520,
    [](int victim_handle, float damage, int killer_handle, int weapon_type, int damage_type,
       rf::Vector3* pos, int killer_uid, char flags) {
        const ParticleDamageScope particle_scope;
        return particle_damage_hook.call_target(victim_handle, damage, killer_handle, weapon_type,
                                                damage_type, pos, killer_uid, flags);
    },
};

// Projectile hit level geometry.
FunHook<bool(rf::Weapon*)> weapon_hit_level_hook{
    0x004C4EC0,
    [](rf::Weapon* wp) {
        SplashWeaponScope splash_scope{wp};
        const bool result = weapon_hit_level_hook.call_target(wp);
        return result;
    },
};

FunHook<void(rf::Entity*, rf::Item*, int*)> send_obj_kill_packet_hook{
    0x0047E8C0,
    [](rf::Entity* killed_entity, rf::Item* item, int* a3) {
        if (kill_attribution_is_active() && killed_entity) {
            rf::Player* killed_player = rf::player_from_entity_handle(killed_entity->handle);
            if (killed_player && killed_player->net_data) {
                // MUST be sent before call_target so it is queued ahead of obj_kill on the
                // reliable stream: obj_kill is what makes the client print the kill message,
                // so a later send would always lose the race and fall back.
                af_send_kill_info(killed_player);
            }
        }
        send_obj_kill_packet_hook.call_target(killed_entity, item, a3);
    },
};

DamageWeaponContext kill_attribution_get_damage_context()
{
    return g_ctx.damage;
}

bool kill_attribution_in_projectile_impact()
{
    return g_ctx.projectile_impact;
}

bool kill_attribution_in_particle_damage()
{
    return g_ctx.particle_damage;
}

int kill_attribution_get_hit_region(int entity_handle)
{
    if (g_hit_region_ctx.entity_handle != entity_handle) {
        return -1;
    }
    return g_hit_region_ctx.region;
}

bool kill_attribution_is_valid_weapon_type(int weapon_type)
{
    constexpr int weapon_types_capacity = 64;
    return weapon_type >= 0 && weapon_type < std::min(rf::num_weapon_types, weapon_types_capacity);
}

int kill_attribution_riot_shield_type()
{
    if (g_riot_shield_weapon_type == -2) {
        // weapons.tbl is parsed at game init, so this resolves from any level.
        g_riot_shield_weapon_type = rf::weapon_lookup_type("Riot Shield");
    }
    return g_riot_shield_weapon_type;
}

bool kill_attribution_is_melee_weapon(int weapon_type)
{
    if (!kill_attribution_is_valid_weapon_type(weapon_type)) {
        return false;
    }
    if (rf::weapon_is_melee(weapon_type) || weapon_type == rf::riot_stick_weapon_type) {
        return true;
    }
    const int shield = kill_attribution_riot_shield_type();
    return shield >= 0 && weapon_type == shield;
}

void kill_attribution_note_pvp_damage(uint8_t victim_player_id, uint8_t attacker_player_id)
{
    const auto now = std::chrono::steady_clock::now();
    CombatChain& chain = g_combat_chains[victim_player_id];
    if (now > chain.expires_at) {
        // The previous fight lapsed; this hit starts a fresh one.
        chain.contributors.clear();
    }
    chain.contributors[attacker_player_id] = now;
    chain.expires_at = now + combat_chain_window;
}

std::vector<uint8_t> kill_attribution_take_assists(uint8_t victim_player_id, uint8_t killer_player_id)
{
    std::vector<uint8_t> assists;

    auto it = g_combat_chains.find(victim_player_id);
    if (it == g_combat_chains.end()) {
        return assists;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now > it->second.expires_at) {
        g_combat_chains.erase(it);
        return assists;
    }

    std::vector<std::pair<uint8_t, std::chrono::steady_clock::time_point>> ranked;
    for (const auto& [attacker_id, last_hit] : it->second.contributors) {
        if (attacker_id == killer_player_id || attacker_id == victim_player_id) {
            continue;
        }
        ranked.emplace_back(attacker_id, last_hit);
    }
    g_combat_chains.erase(it);

    // Most recent contributor first, so truncation drops the stalest assists.
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    if (ranked.size() > af_kill_info_max_assists) {
        ranked.resize(af_kill_info_max_assists);
    }
    for (const auto& entry : ranked) {
        assists.push_back(entry.first);
    }
    return assists;
}

void kill_attribution_record(uint8_t killed_player_id, uint8_t killer_player_id, int weapon_type,
                             uint8_t flags, int damage_type, std::vector<uint8_t> assist_player_ids)
{
    KillAttributionRecord record{};
    record.attr.killer_player_id = killer_player_id;
    if (kill_attribution_is_valid_weapon_type(weapon_type)) {
        record.attr.weapon_type = static_cast<uint8_t>(weapon_type);
    }
    record.attr.flags = flags;
    if (damage_type >= 0 && damage_type < 0xFF) {
        record.attr.damage_type = static_cast<uint8_t>(damage_type);
    }
    record.attr.assist_player_ids = std::move(assist_player_ids);
    record.recorded_at = std::chrono::steady_clock::now();
    record.sequence = g_kill_attribution_next_sequence++;

    g_kill_attributions[killed_player_id] = std::move(record);
}

std::optional<KillAttribution> kill_attribution_lookup_for_send(uint8_t killed_player_id)
{
    auto it = g_kill_attributions.find(killed_player_id);
    if (it == g_kill_attributions.end()) {
        return {};
    }
    if (std::chrono::steady_clock::now() - it->second.recorded_at > kill_attribution_lifetime) {
        g_kill_attributions.erase(it);
        return {};
    }

    // Read without erasing - print_kill_message consumes it a moment later - but never hand
    // the same recorded death to a second kill-info packet.
    uint32_t& last_sent = g_kill_attribution_sent_sequence[killed_player_id];
    if (last_sent == it->second.sequence) {
        return {};
    }
    last_sent = it->second.sequence;
    return it->second.attr;
}

std::optional<KillAttribution> kill_attribution_consume(uint8_t killed_player_id)
{
    auto it = g_kill_attributions.find(killed_player_id);
    if (it == g_kill_attributions.end()) {
        return {};
    }

    std::optional<KillAttribution> attr;
    if (std::chrono::steady_clock::now() - it->second.recorded_at <= kill_attribution_lifetime) {
        attr = it->second.attr;
    }
    g_kill_attributions.erase(it);
    return attr;
}

void kill_attribution_level_init()
{
    g_kill_attributions.clear();
    g_kill_attribution_sent_sequence.clear();
    g_combat_chains.clear();
    g_ctx = DamageResolutionContext{};
    g_hit_region_ctx = {};
    g_riot_shield_weapon_type = -2;
}

void kill_attribution_do_patch()
{
    obj_damage_hook.install();
    get_hit_region_multiplier_hook.install();
    weapon_hit_obj_hook.install();
    weapon_hit_level_hook.install();
    particle_damage_hook.install();
    send_obj_kill_packet_hook.install();
}
