#include <algorithm>
#include <chrono>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>
#include <patch_common/FunHook.h>
#include "../rf/entity.h"
#include "../rf/item.h"
#include "../rf/math/vector.h"
#include "../rf/multi.h"
#include "../rf/player/player.h"
#include "../rf/weapon.h"
#include "alpine_packets.h"
#include "kill_attribution.h"

// Server-side capture of what actually landed the killing blow. The stock obj_kill packet
// carries no weapon at all, so every client used to guess it from replicated held-weapon
// state; the data collected here is shipped alongside instead.

// Valid only inside an obj_damage call tree.
static DamageWeaponContext g_damage_ctx;
// Weapon of the explosion whose radius-damage tree is currently running.
static std::optional<int> g_splash_weapon_ctx;

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
    prev_ = g_splash_weapon_ctx;
    g_splash_weapon_ctx = wp->info_index;
}

SplashWeaponScope::~SplashWeaponScope()
{
    if (active_) {
        // Restore instead of clearing: detonations chain (a rocket setting off a
        // remote charge) and each explosion must keep its own weapon.
        g_splash_weapon_ctx = prev_;
    }
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

        // obj_damage recurses (chained deaths, explosions setting off explosions), so the
        // outer call's context is saved and restored rather than blindly cleared.
        const DamageWeaponContext prev_ctx = g_damage_ctx;
        if (weapon_type >= 0) {
            g_damage_ctx = {weapon_type, false};
        }
        else if (g_splash_weapon_ctx) {
            g_damage_ctx = {*g_splash_weapon_ctx, true};
        }
        else {
            g_damage_ctx = {};
        }

        const float real_damage = obj_damage_hook.call_target(victim_handle, damage, killer_handle,
                                                              weapon_type, damage_type, pos,
                                                              killer_uid, flags);
        g_damage_ctx = prev_ctx;
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
        return weapon_hit_obj_hook.call_target(wp);
    },
};

// Projectile hit level geometry.
FunHook<bool(rf::Weapon*)> weapon_hit_level_hook{
    0x004C4EC0,
    [](rf::Weapon* wp) {
        SplashWeaponScope splash_scope{wp};
        return weapon_hit_level_hook.call_target(wp);
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
    return g_damage_ctx;
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
    g_splash_weapon_ctx.reset();
    g_damage_ctx = {};
    g_hit_region_ctx = {};
    g_riot_shield_weapon_type = -2;
}

void kill_attribution_do_patch()
{
    obj_damage_hook.install();
    get_hit_region_multiplier_hook.install();
    weapon_hit_obj_hook.install();
    weapon_hit_level_hook.install();
    send_obj_kill_packet_hook.install();
}
