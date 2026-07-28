#include <algorithm>
#include <format>
#include <map>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <xlog/xlog.h>
#include <common/utils/list-utils.h>
#include "gungame.h"
#include "gametype.h"
#include "multi.h"
#include "server.h"
#include "server_internal.h"
#include "alpine_packets.h"
#include "../main/main.h"
#include "../sound/sound.h"
#include "../hud/hud.h"
#include "../rf/multi.h"
#include "../rf/entity.h"
#include "../rf/weapon.h"
#include "../rf/player/player.h"
#include "../rf/gameseq.h"

namespace
{

// Built-in default progression, used unless specified with `gg_tiers` dedi cfg param.
// Each tier is shuffled per player at order-build time.
const std::vector<std::vector<const char*>> g_tier_names = {
    {"rail_gun", "heavy_machine_gun", "Assault Rifle", "scope_assault_rifle"},
    {"Rocket Launcher", "Grenade", "shoulder_cannon"},
    {"Sniper Rifle", "Remote Charge", "Machine Pistol"},
    {"12mm handgun", "Flamethrower", "Shotgun"},
};

// Resolved tier weapon indices (config-driven when gungame_tiers is set, else
// the built-in table) and the resolved final-level weapon.
std::vector<std::vector<int>> g_resolved_tiers;
int g_final_weapon_index = -1;

// Per-player state (server-side): score-threshold -> weapon-index map, and the
// score each player's current life began at (drives the per-weapon rampage
// reward: newly reaching AND passing a weapon on one life).
std::unordered_map<rf::Player*, std::map<int, int>> g_player_orders;
std::unordered_map<rf::Player*, int> g_life_start_score;

// Players we've sent the order to while they were LOADED. The send must happen
// after the client has finished loading (past its own reset_local_gungame_order
// in multi_hud_level_init), otherwise the client wipes the order right after
// receiving it and never gets it again. Reset each level.
std::unordered_set<rf::Player*> g_order_sent;

// The score limit the current orders were built against. If the config limit
// changes mid-map (sv_loadconfig), do_frame rebuilds + resends every order.
int g_orders_built_limit = -1;

// The active-rules generation the current orders/tiers were built against. A
// mid-map sv_loadconfig bumps get_active_rules_generation(), so do_frame can
// detect changes to gg_tiers / gg_final_weapon (which don't touch the limit)
// and rebuild — not just gg_score_limit changes.
int g_orders_built_generation = -1;

// One-frag-to-win announce fires once per map.
bool g_final_reached_announced = false;

// A player has finished loading the level when the engine set
// NPF_CLIENT_IS_LOADED. The listen-server host and bots are always loaded.
bool player_is_loaded(rf::Player* p)
{
    if (!p) return false;
    if (p == rf::local_player) return true;
    if (p->is_bot) return true;
    if (!p->net_data) return false;
    return (p->net_data->flags & rf::NPF_CLIENT_IS_LOADED) != 0;
}

// Effective score limit = the configured Gun Game frag limit, which is routed
// into netgame.max_kills (see dedi_cfg apply rules). We read netgame.max_kills
// rather than rf::multi_kill_limit because it's set at rules-application time,
// before level_init_post builds the initial orders. The standard FFA win fires
// off rf::multi_kill_limit, which the engine syncs from netgame.max_kills.
int effective_limit()
{
    return std::max(1, rf::netgame.max_kills);
}

void resolve_tier_weapons()
{
    g_resolved_tiers.clear();

    const auto resolve_one = [](const char* name) {
        const int idx = rf::weapon_lookup_type(name);
        if (idx < 0) {
            xlog::warn("GunGame: weapon '{}' did not resolve; skipping", name);
        }
        return idx;
    };

    // Config-driven layout (gungame_tiers): outer array = tiers in order, inner
    // = weapons.tbl names. Failed names are skipped, empty tiers dropped. If the
    // whole config resolves to nothing, fall back to the built-in table.
    const auto& cfg_tiers = g_alpine_server_config_active_rules.gungame_tiers;
    if (!cfg_tiers.empty()) {
        for (const auto& tier_names : cfg_tiers) {
            std::vector<int> resolved;
            for (const std::string& name : tier_names) {
                const int idx = resolve_one(name.c_str());
                if (idx >= 0) resolved.push_back(idx);
            }
            if (!resolved.empty()) {
                g_resolved_tiers.push_back(std::move(resolved));
            }
        }
        if (g_resolved_tiers.empty()) {
            xlog::warn("GunGame: configured gungame_tiers resolved to no weapons; using built-in tiers");
        }
    }

    if (g_resolved_tiers.empty()) {
        for (const auto& tier_names : g_tier_names) {
            std::vector<int> resolved;
            for (const char* name : tier_names) {
                const int idx = resolve_one(name);
                if (idx >= 0) resolved.push_back(idx);
            }
            if (!resolved.empty()) {
                g_resolved_tiers.push_back(std::move(resolved));
            }
        }
    }

    // Final level weapon (gungame_final_weapon config; Riot Stick by default).
    const std::string& final_name = g_alpine_server_config_active_rules.gungame_final_weapon;
    int final_idx = final_name.empty() ? -1 : rf::weapon_lookup_type(final_name.c_str());
    if (final_idx < 0) {
        if (!final_name.empty() && final_name != "Riot Stick") {
            xlog::warn("GunGame: final weapon '{}' did not resolve; using Riot Stick", final_name);
        }
        final_idx = rf::weapon_lookup_type("Riot Stick");
    }
    g_final_weapon_index = (final_idx >= 0) ? final_idx : rf::riot_stick_weapon_type;
}

// Build a per-player score-threshold -> weapon map. The tier weapons are spread
// proportionally across [0, top), but per-player and with each tier shuffled per player.
// The Riot Stick is the final level at `top` (= limit - 1) so the limit-th kill is
// made with the Riot Stick and wins.
std::map<int, int> build_order_for_player()
{
    std::map<int, int> map;

    // Lazy resolution: on a listen server the host can spawn (and need an
    // order) BEFORE gungame_level_init_post runs on the first map of the
    // session. weapons.tbl is parsed at game init, so weapon_lookup_type is
    // valid by any spawn.
    if (g_resolved_tiers.empty() && g_final_weapon_index < 0) {
        resolve_tier_weapons();
    }

    const int limit = effective_limit();
    const int top = std::max(1, limit - 1);

    int total_weapons = 0;
    for (const auto& tier : g_resolved_tiers) {
        total_weapons += static_cast<int>(tier.size());
    }
    if (total_weapons == 0) {
        map[top] = g_final_weapon_index; // no tiers resolved: just the final weapon
        return map;
    }

    int accumulated = 0;
    int remaining = std::max(0, top);
    int total_left = total_weapons;

    for (auto tier : g_resolved_tiers) { // copy so we can shuffle per player
        std::shuffle(tier.begin(), tier.end(), g_rng);
        const int tier_size = static_cast<int>(tier.size());

        const int tier_kills = (total_left > 0) ? (remaining * tier_size) / total_left : 0;
        const int weapon_interval = (tier_kills > 0 && tier_size > 0)
                                        ? std::max(1, tier_kills / tier_size)
                                        : 1;

        for (int widx : tier) {
            map[accumulated] = widx;
            accumulated = std::min(accumulated + weapon_interval, top);
            if (accumulated >= top) break;
        }

        remaining = std::max(0, top - accumulated);
        total_left -= tier_size;
        if (accumulated >= top || total_left <= 0) break;
    }

    // Final level (Riot Stick unless configured otherwise), granted at (limit - 1).
    map[top] = g_final_weapon_index;
    return map;
}

// Nearest-or-lower threshold weapon for a score (lower_bound).
int weapon_for_score(const std::map<int, int>& m, int score)
{
    if (m.empty()) return -1;
    if (score <= 0) return m.begin()->second;
    auto it = m.lower_bound(score);
    if (it != m.end() && it->first == score) return it->second;
    if (it == m.begin()) return m.begin()->second;
    return std::prev(it)->second;
}

void send_order(rf::Player* p)
{
    if (!p) return;
    const auto it = g_player_orders.find(p);
    if (it == g_player_orders.end()) return;

    std::vector<af_gungame_order_entry> entries;
    entries.reserve(it->second.size());
    for (const auto& [threshold, weapon] : it->second) {
        entries.push_back(af_gungame_order_entry{
            static_cast<uint16_t>(std::clamp(threshold, 0, 65535)),
            static_cast<uint8_t>(std::clamp(weapon, 0, 255))});
    }
    af_send_gungame_order(p, entries);
}

// Build a player's server-side order if they don't have one yet, and return it.
// Does NOT send it — the client send is owned by the loaded-gated do_frame path
// so it lands after the client's level-init reset.
std::map<int, int>& ensure_order(rf::Player* p)
{
    auto it = g_player_orders.find(p);
    if (it == g_player_orders.end()) {
        it = g_player_orders.emplace(p, build_order_for_player()).first;
    }
    return it->second;
}

void grant_and_switch(rf::Player* player, int weapon_type)
{
    rf::Entity* entity = rf::entity_from_handle(player->entity_handle);
    if (!entity) return;
    server_add_player_weapon(player, weapon_type, true);

    // ai_add_weapon is a complete no-op for a weapon the entity already holds
    // (it early-returns on has_weapon), so the grant above doesn't top up ammo
    // on a re-grant (e.g. the engine already granted the spawn weapon
    // natively). Explicitly set a full clip + the REAL max reserve — idempotent
    // for freshly-granted weapons. The af_obj_update stream replicates the
    // authoritative ammo to clients.
    rf::WeaponInfo& winfo = rf::weapon_types[weapon_type];
    entity->ai.clip_ammo[weapon_type] = winfo.clip_size_multi;
    if (winfo.ammo_type >= 0 && winfo.ammo_type < 32) {
        entity->ai.ammo[winfo.ammo_type] = winfo.max_ammo;
    }

    server_set_player_weapon(player, entity, weapon_type);
}

// Random powerup + reward sound for newly reaching and passing a weapon level
// on a single life (rampage).
// Announced via the big center-screen HUD notification.
void grant_rampage_reward(rf::Player* player)
{
    std::uniform_int_distribution<int> powerup_dist(0, 2); // invuln, amp, or super armour
    const int random_powerup = powerup_dist(g_rng);
    // 0 = invuln; 1 = amp; 2 = super armour; 3 = super health

    if (random_powerup <= 1) {
        const int max_duration = (random_powerup == 0) ? 5 : 10; // invuln max 5, amp max 10
        std::uniform_int_distribution<int> duration_dist(3, max_duration);
        const int duration = duration_dist(g_rng);
        const int amp_time = rf::multi_powerup_get_time_until(player, random_powerup) + duration * 1000;
        rf::multi_powerup_add(player, random_powerup, amp_time);
    }
    else {
        rf::multi_powerup_add(player, random_powerup, 10000);
    }

    af_send_hud_notification("YOU'RE ON A RAMPAGE", 3, static_cast<int>(HudNotificationType::Rampage), true, player);
    send_sound_packet_throwaway(player, stock_sound_id::jolt_01);
}

// True level-up: the weapon changed AND it's not just the Remote Charge <->
// Remote Charge Detonator toggle (the engine treats those as one weapon slot).
bool is_weapon_level_up(int new_weapon, int current_weapon)
{
    if (new_weapon == current_weapon) return false;
    return !is_remote_charge_pair(new_weapon, current_weapon);
}

} // namespace

void gungame_level_init()
{
    // Real level boundary: drop all per-player state and the announce latch. Tier
    // weapons are (re)resolved in level_init_post once weapons.tbl is loaded.
    g_player_orders.clear();
    g_life_start_score.clear();
    g_order_sent.clear();
    g_orders_built_limit = -1;
    g_orders_built_generation = -1;
    g_final_reached_announced = false;
}

void gungame_level_init_post()
{
    if (!rf::is_server) return;
    if (!gt_is_gungame()) return;

    resolve_tier_weapons();
    g_player_orders.clear();
    g_life_start_score.clear();
    g_order_sent.clear();
    g_orders_built_limit = effective_limit();
    g_orders_built_generation = get_active_rules_generation();
    g_final_reached_announced = false;

    // Build orders for all connected players (the server needs them for weapon
    // grants), but do NOT send here — the send would race the client's
    // reset_local_gungame_order on level load and be wiped. gungame_do_frame
    // sends once the client is LOADED (past its reset).
    for (rf::Player& p : SinglyLinkedList{rf::player_list}) {
        if (p.is_browser) continue;
        auto& order = (g_player_orders[&p] = build_order_for_player());

        // Anyone who already has a live entity spawned BEFORE this init ran
        // (listen host on the session's first map, pre-init spawners) — put
        // them on their authoritative order now and seed the rampage tracking
        // their spawn hook would have set (life "starts" here for the new order).
        rf::Entity* ep = rf::entity_from_handle(p.entity_handle);
        if (ep && !rf::entity_is_dying(ep)) {
            const int score = p.stats ? p.stats->score : 0;
            const int weapon = weapon_for_score(order, score);
            if (weapon >= 0) {
                grant_and_switch(&p, weapon);
            }
            g_life_start_score[&p] = score;
        }
    }

    // GunGame has NO in-level item pickups at all — weapons come only from the
    // progression. Empty allowlist hides everything (weapons, ammo, health,
    // armor, powerups); respawn timers are invalidated so nothing comes back.
    multi_hide_level_items({});

    xlog::debug("GunGame: level init, score limit {}, {} tiers resolved", effective_limit(), static_cast<int>(g_resolved_tiers.size()));
}

void gungame_do_frame()
{
    if (!rf::is_server) return;
    if (!gt_is_gungame()) return;
    // GS_GAMEPLAY only starts after level init (thus after the client's reset),
    // and NPF_CLIENT_IS_LOADED is set only once the client finishes loading, so
    // orders sent here land after the client's reset and stick. This also covers
    // late joiners uniformly (host is always loaded; remotes once they load).
    if (rf::gameseq_get_state() != rf::GameState::GS_GAMEPLAY) return;

    // Mid-map config reload (sv_loadconfig): a bumped rules generation and/or a
    // changed score limit means the resolved tiers, final weapon, or threshold
    // spread may no longer match. This is where ALL mid-map config changes are
    // handled — gg_tiers, gg_final_weapon and gg_score_limit alike. Re-resolve
    // tier weapons first (so the rebuilt orders reflect any changed
    // gg_tiers/gg_final_weapon before build_order_for_player reads them), rebuild
    // every order, re-grant alive players, and let the send gate below resend
    // everything (g_order_sent cleared).
    const int rules_generation = get_active_rules_generation();
    if (rules_generation != g_orders_built_generation
        || effective_limit() != g_orders_built_limit) {
        resolve_tier_weapons();
        g_orders_built_generation = rules_generation;
        g_orders_built_limit = effective_limit();
        g_player_orders.clear();
        g_order_sent.clear();

        bool any_at_final = false;
        for (rf::Player& p : SinglyLinkedList{rf::player_list}) {
            if (p.is_browser) continue;
            auto& order = ensure_order(&p);
            const int score = p.stats ? p.stats->score : 0;
            if (score >= g_orders_built_limit - 1) any_at_final = true;

            rf::Entity* ep = rf::entity_from_handle(p.entity_handle);
            if (ep && !rf::entity_is_dying(ep)) {
                const int weapon = weapon_for_score(order, score);
                if (weapon >= 0) grant_and_switch(&p, weapon);
                // The weapon spans changed — restart rampage tracking so no
                // cross-order span counts as completed.
                g_life_start_score[&p] = score;
            }
        }
        // Un-latch the one-frag-to-win announce if nobody has reached the new
        // final threshold (a raised limit re-arms the cue).
        if (!any_at_final) {
            g_final_reached_announced = false;
        }
    }

    for (rf::Player& p : SinglyLinkedList{rf::player_list}) {
        if (p.is_browser) continue;
        ensure_order(&p); // build if missing (server-side)
        if (player_is_loaded(&p) && !g_order_sent.contains(&p)) {
            // Bots are real clients but wipe/ignore the order anyway — mark as
            // sent without spending a reliable packet on them.
            if (!p.is_bot) {
                send_order(&p);
            }
            g_order_sent.insert(&p);
        }

        // "Effectively infinite" ammo for thrown (non-clip) weapons WITHOUT
        // advertising a bogus 9999 count: keep the reserve topped at the REAL
        // max_ammo. Clip weapons get the same feel from the reload-refund patch.
        //
        // Writing the server entity alone does NOT reach the owner: their ammo
        // is client-simulated and af_obj_update excludes the owner from its own
        // entity's updates (both send- and receive-side). So on each actual
        // refill (reserve was below max) we send the owner a one-shot
        // RF_GPT_RELOAD to resync — this fires at throw cadence, not per frame.
        // af_obj_update still carries the topped value to OTHER clients' views.
        rf::Entity* ep = rf::entity_from_handle(p.entity_handle);
        if (ep && !rf::entity_is_dying(ep)) {
            int w = ep->ai.current_primary_weapon;
            if (w == rf::remote_charge_det_weapon_type) {
                w = rf::remote_charge_weapon_type; // the pair shares the charge ammo
            }
            if (w >= 0 && w < 64 && !rf::weapon_uses_clip(w)) {
                const rf::WeaponInfo& winfo = rf::weapon_types[w];
                if (winfo.ammo_type >= 0 && winfo.ammo_type < 32
                    && ep->ai.ammo[winfo.ammo_type] < winfo.max_ammo) {
                    ep->ai.ammo[winfo.ammo_type] = winfo.max_ammo;
                    if (&p != rf::local_player && !p.is_bot) {
                        send_nonclip_ammo_sync(&p, ep, w);
                    }
                }
            }
        }
    }
}

void gungame_on_player_spawn(rf::Player* player)
{
    if (!rf::is_server || !gt_is_gungame() || !player) return;

    auto& order = ensure_order(player);
    rf::Entity* ep = rf::entity_from_handle(player->entity_handle);
    if (!ep) return;

    const int score = player->stats ? player->stats->score : 0;
    const int weapon = weapon_for_score(order, score);
    if (weapon < 0) return;

    // Rampage tracking: remember where this life began. A weapon level only
    // rewards if it was newly reached AND passed on this life (see
    // on_player_kill for the exact rule).
    g_life_start_score[player] = score;

    grant_and_switch(player, weapon);
}

void gungame_on_player_kill(rf::Player* killer, rf::Player* killed)
{
    if (!rf::is_server || !gt_is_gungame()) return;
    if (!killer || killer == killed) return;

    auto& order = ensure_order(killer);
    rf::Entity* ep = rf::entity_from_handle(killer->entity_handle);
    if (!ep) return;

    // The engine already incremented the killer's frag score before this hook.
    const int score = killer->stats ? killer->stats->score : 0;
    const int weapon = weapon_for_score(order, score);

    // On a trade/mutual kill the killing blow lands while the killer's own
    // entity is dying — require a live killer for the level-up branch so
    // rewards/notifications aren't pushed into their next life (the spawn hook
    // re-grants the right weapon by score anyway).
    const bool killer_alive = !rf::entity_is_dying(ep) && !rf::player_is_dead(killer);

    if (weapon >= 0 && killer_alive && is_weapon_level_up(weapon, ep->ai.current_primary_weapon)) {
        // Rampage: reward requires NEWLY reaching AND passing the weapon on one
        // life. The just-completed weapon's span [T_start, T_next) contained the
        // pre-kill score; strict `life_start < T_start` means the level-up INTO
        // that weapon also happened this life.
        if (g_alpine_server_config_active_rules.gungame_rampage_rewards) {
            const int prev_score = score - 1; // the engine adds exactly 1 per kill
            int completed_start = order.empty() ? 0 : order.begin()->first;
            auto span_it = order.upper_bound(prev_score);
            if (span_it != order.begin()) {
                completed_start = std::prev(span_it)->first;
            }
            const auto life_it = g_life_start_score.find(killer);
            const int life_start = (life_it != g_life_start_score.end()) ? life_it->second : score;
            if (life_start == 0 || life_start < completed_start) {
                grant_rampage_reward(killer);
            }
        }
        grant_and_switch(killer, weapon);
    }

    // First player to reach the final weapon (final level) is one frag from
    // winning. Skipped for degenerate limits (< 2: the same frag ends the game).
    if (!g_final_reached_announced && effective_limit() >= 2 && score >= (effective_limit() - 1)) {
        g_final_reached_announced = true;
        af_broadcast_play_custom_sound(custom_sound_id::ann_one_kill_left);
        // Resolve the configured final weapon's display name (gg_final_weapon),
        // falling back to a generic literal if it's out of range or unnamed.
        const char* final_name = "final weapon";
        if (g_final_weapon_index >= 0 && g_final_weapon_index < 64
            && rf::weapon_types[g_final_weapon_index].display_name) {
            final_name = rf::weapon_types[g_final_weapon_index].display_name;
        }
        // Everyone EXCEPT the reacher gets the broadcast text — the reacher's
        // own client shows the "Final weapon: <weapon>" notification and the
        // two would clobber each other in the single notification slot.
        const std::string msg =
            std::format("{} reached the {} - one frag from winning!", killer->name.c_str(), final_name);
        for (rf::Player& p : SinglyLinkedList{rf::player_list}) {
            if (p.is_browser) continue;
            if (&p == killer) continue;
            af_send_hud_notification(msg, 4, static_cast<int>(HudNotificationType::GunGame), true, &p);
        }
    }
}

void gungame_on_player_disconnect(rf::Player* player)
{
    g_player_orders.erase(player);
    g_life_start_score.erase(player);
    g_order_sent.erase(player); // avoid a stale pointer lingering in the sent-set
}

int gungame_spawn_weapon_for(rf::Player* player)
{
    if (!rf::is_server || !gt_is_gungame() || !player) return -1;
    auto& order = ensure_order(player);
    const int score = player->stats ? player->stats->score : 0;
    return weapon_for_score(order, score);
}
