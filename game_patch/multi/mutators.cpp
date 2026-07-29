#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <patch_common/FunHook.h>
#include <common/utils/string-utils.h>
#include "mutators.h"
#include "server_internal.h"
#include "multi.h"
#include "../rf/weapon.h"
#include "../rf/item.h"
#include "../rf/entity.h"
#include "../rf/ai.h"
#include "../rf/multi.h"
#include "../rf/player/player.h"
#include "../rf/os/console.h"

// Spawn reserve for a no-clip "infinite ammo" weapon. Firing draws from reserve,
// but we suppress the per-shot decrement, so this is purely the (constant) number
// the HUD shows and it never counts down.
static constexpr int NO_CLIP_RESERVE = 1;

// The weapon table class name for a given weapon type (accepted by
// weapon_lookup_type and the loadout/default-weapon setters).
static const char* mutator_weapon_name(int weapon_type)
{
    if (weapon_type < 0 || weapon_type >= rf::num_weapon_types)
        return "";
    return rf::weapon_types[weapon_type].name.c_str();
}

// Locate the level pickups that grant the featured weapon so we can
// redirect other pickups onto them.
static void resolve_featured_items(MutatorConfig& m)
{
    m.featured_weapon_item_index = -1;
    m.featured_ammo_item_index = -1;
    if (m.featured_weapon_index < 0)
        return;

    for (int i = 0; i < rf::num_item_types; ++i) {
        const rf::ItemInfo& info = rf::item_info[i];
        if (m.featured_weapon_item_index < 0 && info.gives_weapon_id == m.featured_weapon_index)
            m.featured_weapon_item_index = i;
        if (m.featured_ammo_item_index < 0 && info.ammo_for_weapon_id == m.featured_weapon_index)
            m.featured_ammo_item_index = i;
    }
}

// Replace the spawn loadout with a single weapon and spawn holding it.
static void set_single_weapon_loadout(AlpineServerConfigRules& r, int weapon_type, int reserve_ammo)
{
    r.spawn_loadout.red_weapons.clear();
    r.spawn_loadout.blue_weapons.clear();
    r.spawn_loadout.loadouts_active = true;
    r.spawn_loadout.add(mutator_weapon_name(weapon_type), reserve_ammo, false, true);
    r.default_player_weapon.set_weapon(mutator_weapon_name(weapon_type));
}

// ============================================================================
// Mutator definitions
// ============================================================================

// Instagib: one-shot rail, no pickups, infinite ammo, no reload, and no
// switching away from the rail.
static void apply_instagib(AlpineServerConfigRules& r, const toml::table& /*opts*/)
{
    const int rail = rf::rail_gun_weapon_type;

    // spawn delay
    r.spawn_delay.enabled = true;
    r.spawn_delay.set_base_value(0.5f);

    // Rail only (no baton), spawned holding it.
    // Reserve = NO_CLIP_RESERVE
    // The rail is made no-clip and its decrement is suppressed.
    set_single_weapon_loadout(r, rail, NO_CLIP_RESERVE);

    // Infinite reserves; rail never reloads; can't switch away from it.
    r.weapon_infinite_magazines = true;
    r.mutators.featured_weapon_index = rail;
    r.mutators.lock_to_featured_weapon = true;
    r.mutators.no_featured_reload = true;

    // No pickups; no weapon drops.
    r.mutators.pickup_policy = PickupPolicy::HideAll;
    r.drop_weapons = false;
}

// Rails: spawn with baton, no pickups except weapons/ammo, and every
// weapon/ammo pickup becomes the featured weapon's pickup (rail by default). The
// featured weapon otherwise behaves normally (reloads, finite ammo, free switch).
static void apply_rails(AlpineServerConfigRules& r, const toml::table& opts)
{
    int featured = rf::rail_gun_weapon_type;
    if (auto v = opts["featured_weapon"].value<std::string>()) {
        int idx = rf::weapon_lookup_type(v->c_str());
        if (idx >= 0)
            featured = idx;
        else
            rf::console::print("  [WARN] Rails mutator: unknown featured_weapon '{}', using rail gun\n", *v);
    }
    const bool exclude_thrown = opts["exclude_thrown"].value_or(true);

    // spawn delay
    r.spawn_delay.enabled = true;
    r.spawn_delay.set_base_value(0.5f);

    // Spawn with the baton only; the featured weapon comes from redirected pickups.
    const int baton = rf::riot_stick_weapon_type;
    set_single_weapon_loadout(r, baton, rf::weapon_types[baton].clip_size_multi);

    r.mutators.featured_weapon_index = featured;
    r.mutators.redirect_pickups_to_featured = true;
    r.mutators.redirect_exclude_thrown = exclude_thrown;
    resolve_featured_items(r.mutators);

    // Weapon stay by default.
    r.weapons_stay = true;

    // Keep weapon + ammo pickups (which get redirected), hide everything else.
    r.mutators.pickup_policy = PickupPolicy::WeaponsAndAmmoOnly;
}

// Arena: 100/100, back to full after each frag (+300 effective health), current
// weapon auto-reloads on each frag with no pause, infinite mags, no pickups
// except weapons. Spawns with Riot Stick + Assault Rifle.
static void apply_arena(AlpineServerConfigRules& r, const toml::table& /*opts*/)
{
    // 100 / 100 spawn.
    r.spawn_life.enabled = true;
    r.spawn_life.set_value(100.0f);
    r.spawn_armour.enabled = true;
    r.spawn_armour.set_value(100.0f);

    // spawn delay
    r.spawn_delay.enabled = true;
    r.spawn_delay.set_base_value(0.5f);

    // Spawn loadout: Riot Stick + Assault Rifle, holding the AR. The AR gets a large
    // reserve (clamped to its max) so infinite mags always has ammo to reload from.
    const int baton = rf::riot_stick_weapon_type;
    r.spawn_loadout.red_weapons.clear();
    r.spawn_loadout.blue_weapons.clear();
    r.spawn_loadout.loadouts_active = true;
    r.spawn_loadout.add("Riot Stick", rf::weapon_types[baton].clip_size_multi, false, true);
    r.spawn_loadout.add("Assault Rifle", 999, false, true);
    r.default_player_weapon.set_weapon("Assault Rifle");

    // Force respawn by default.
    r.force_respawn = true;

    // +300 effective health per frag restores health/armor to full.
    r.kill_rewards.kill_reward_effective_health = 300.0f;
    r.kill_rewards.kill_reward_health_super = false;
    r.kill_rewards.kill_reward_armor_super = false;

    // Weapon pickups grant full ammo and mags are infinite, so a reload always refills.
    r.weapon_items_give_full_ammo = true;
    r.weapon_infinite_magazines = true;
    r.mutators.reload_weapon_on_kill = true;

    // No pickups except weapons.
    r.mutators.pickup_policy = PickupPolicy::WeaponsOnly;
}

// ============================================================================
// Registry + application order
// ============================================================================

enum class MutatorId
{
    Arena,
    Rails,
    Instagib,
};

struct MutatorDef
{
    MutatorId id;
    const char* name;  // matched against the toml name
    const char* label; // shown in the printed rules
    void (*apply)(AlpineServerConfigRules&, const toml::table&);
};

static const MutatorDef MUTATORS[] = {
    {MutatorId::Instagib, "instagib", "Instagib", &apply_instagib},
    {MutatorId::Rails, "rails", "Rails", &apply_rails},
    {MutatorId::Arena, "arena", "Arena", &apply_arena},
};

// Hardcoded order in which simultaneously-active mutators are applied. Later
// entries win where they overlap.
static const MutatorId MUTATOR_APPLY_ORDER[] = {
    MutatorId::Arena,
    MutatorId::Rails,
    MutatorId::Instagib,
};

static const MutatorDef* find_mutator_by_name(std::string_view name)
{
    for (const auto& def : MUTATORS)
        if (string_iequals(name, def.name))
            return &def;
    return nullptr;
}

static const MutatorDef* find_mutator_by_id(MutatorId id)
{
    for (const auto& def : MUTATORS)
        if (def.id == id)
            return &def;
    return nullptr;
}

// Keys allowed inside a [[rules.mutators]] entry. Anything else is usually a
// base-rule key accidentally placed after the mutators array in the TOML (where
// it silently becomes a key of the mutator instead of the rules section).
static bool is_known_mutator_option(std::string_view key)
{
    return key == "name" || key == "featured_weapon" || key == "exclude_thrown";
}

void apply_mutators_from_toml(const toml::array& mutators_arr, AlpineServerConfigRules& rules)
{
    // Collect the mutators declared in this scope: id -> its options table.
    std::map<MutatorId, toml::table> declared;
    for (const auto& node : mutators_arr) {
        const toml::table* tbl = node.as_table();
        if (!tbl) {
            rf::console::print("  [WARN] each [[rules.mutators]] entry must be a table with a 'name'\n");
            continue;
        }
        auto name = (*tbl)["name"].value<std::string>();
        if (!name) {
            rf::console::print("  [WARN] a mutators entry is missing its 'name'\n");
            continue;
        }
        const MutatorDef* def = find_mutator_by_name(*name);
        if (!def) {
            rf::console::print("  [WARN] unknown mutator '{}'\n", *name);
            continue;
        }

        // Flag stray keys — almost always a base-rule key (e.g. game_type) that
        // ended up inside this mutator because it was written after the
        // [[rules.mutators]] header in the TOML.
        for (const auto& [k, v] : *tbl) {
            const std::string_view key = k.str();
            if (!is_known_mutator_option(key))
                rf::console::print("  [WARN] mutator '{}': unexpected key '{}'. In TOML, keys after [[rules.mutators]] belong to the mutator, not [rules] — move base-rule keys above the mutators array.\n",
                    *name, key);
        }

        declared[def->id] = *tbl; // a later declaration of the same mutator wins
    }

    // Apply in the fixed order.
    for (MutatorId id : MUTATOR_APPLY_ORDER) {
        auto it = declared.find(id);
        if (it == declared.end())
            continue;

        const MutatorDef* def = find_mutator_by_id(id);
        def->apply(rules, it->second);

        // Record for display; replace any inherited entry for the same mutator.
        auto& labels = rules.mutators.active_labels;
        labels.erase(std::remove(labels.begin(), labels.end(), def->label), labels.end());
        labels.push_back(def->label);

        // Record the raw declaration so this scope's mutators can be re-applied
        // after a runtime game_type change wipes MutatorConfig (see
        // apply_rules_for_current_level). Replace any inherited entry for the
        // same mutator, matching the label dedup above.
        MutatorDeclaration decl;
        decl.name = def->name;
        if (auto fw = it->second["featured_weapon"].value<std::string>())
            decl.featured_weapon = *fw;
        if (auto et = it->second["exclude_thrown"].value<bool>())
            decl.exclude_thrown = *et;
        auto& decls = rules.mutators.declarations;
        decls.erase(std::remove_if(decls.begin(), decls.end(),
            [&](const MutatorDeclaration& d) { return string_iequals(d.name, def->name); }), decls.end());
        decls.push_back(std::move(decl));
    }
}

std::optional<ManualRulesOverride> load_mutator_rules_override(std::string_view mutator_name)
{
    const MutatorDef* def = find_mutator_by_name(mutator_name);
    if (!def)
        return std::nullopt;

    // Apply the single mutator on top of the base rules with base mutators
    // stripped, so the voted mutator fully replaces (rather than stacks on) any
    // mutator the base rules declared.
    AlpineServerConfigRules rules = g_alpine_server_config.base_rules_no_mutators;

    toml::table entry;
    entry.insert_or_assign("name", std::string{mutator_name});
    toml::array arr;
    arr.push_back(std::move(entry));
    apply_mutators_from_toml(arr, rules);

    ManualRulesOverride result;
    result.rules = std::move(rules);
    result.preset_alias = std::string{def->label};
    return result;
}

// ============================================================================
// Runtime logic
// ============================================================================

void mutators_level_init_post()
{
    if (!rf::is_server)
        return;

    const auto& m = g_alpine_server_config_active_rules.mutators;
    if (m.pickup_policy == PickupPolicy::Normal)
        return;

    std::vector<int> allowed;
    if (m.pickup_policy != PickupPolicy::HideAll) {
        for (int i = 0; i < rf::num_item_types; ++i) {
            const rf::ItemInfo& info = rf::item_info[i];
            const bool is_weapon = info.gives_weapon_id >= 0;
            const bool is_ammo = info.ammo_for_weapon_id >= 0;
            if (is_weapon || (is_ammo && m.pickup_policy == PickupPolicy::WeaponsAndAmmoOnly))
                allowed.push_back(i);
        }
    }

    // Empty allowlist hides every pickup (HideAll).
    // Does not hide CTF gameplay items.
    multi_hide_level_items(allowed, true);
}

// Thrown explosives (Remote Charges, Grenade) the Rails mutator can exempt from
// pickup replacement.
static bool is_thrown_explosive_weapon(int weapon_type)
{
    return weapon_type == rf::remote_charge_weapon_type || weapon_type == rf::grenade_weapon_type;
}

int mutators_redirect_item_index(int item_type_index)
{
    const auto& m = g_alpine_server_config_active_rules.mutators;
    if (!m.redirect_pickups_to_featured || item_type_index < 0 || item_type_index >= rf::num_item_types)
        return item_type_index;

    const rf::ItemInfo& info = rf::item_info[item_type_index];

    // Weapon pickup -> featured weapon pickup.
    if (info.gives_weapon_id >= 0) {
        const int giver = info.gives_weapon_id;
        if (giver == m.featured_weapon_index)
            return item_type_index; // already the featured weapon
        if (m.redirect_exclude_thrown && is_thrown_explosive_weapon(giver))
            return item_type_index; // leave thrown explosives alone
        if (m.featured_weapon_item_index >= 0)
            return m.featured_weapon_item_index;
        return item_type_index;
    }

    // Ammo pickup -> featured weapon ammo pickup.
    if (info.ammo_for_weapon_id >= 0) {
        const int feeds = info.ammo_for_weapon_id;
        if (feeds == m.featured_weapon_index)
            return item_type_index; // already the featured ammo
        if (m.redirect_exclude_thrown && is_thrown_explosive_weapon(feeds))
            return item_type_index; // leave thrown-explosive ammo alone
        if (m.featured_ammo_item_index >= 0)
            return m.featured_ammo_item_index;
        return item_type_index;
    }

    return item_type_index;
}

bool mutators_should_deny_weapon_switch(int from_weapon, int to_weapon)
{
    const auto& m = g_alpine_server_config_active_rules.mutators;
    if (!m.lock_to_featured_weapon || m.featured_weapon_index < 0)
        return false;
    // Only the featured weapon may be selected.
    return to_weapon != m.featured_weapon_index;
}

void mutators_on_player_frag(rf::Player* killer)
{
    if (!killer)
        return;

    // Reload-on-kill state: the server reads its active rules; a client reads the
    // networked server info (join_accept / af_server_info flag).
    const bool active = rf::is_server
        ? g_alpine_server_config_active_rules.mutators.reload_weapon_on_kill
        : (get_af_server_info().has_value() && get_af_server_info()->reload_on_kill);
    if (!active)
        return;

    // A client only owns/predicts its own weapon, so only refill the local player.
    if (!rf::is_server && killer != rf::local_player)
        return;

    rf::Entity* ep = rf::entity_from_handle(killer->entity_handle);
    if (!ep)
        return;

    const int wt = ep->ai.current_primary_weapon;
    if (wt < 0 || wt >= 64 || !rf::weapon_uses_clip(wt))
        return;

    // Reset the clip to full directly, on both the server (authoritative) and the
    // killer's own client. No reload packet is sent, so there's no reload animation
    // or pause, you just keep firing.
    ep->ai.clip_ammo[wt] = rf::weapon_types[wt].clip_size;
}

// The weapon currently forced to no-clip.
static int g_no_clip_weapon = -1;

static void apply_no_clip_override(int wt)
{
    rf::weapon_types[wt].clip_size = 0; // no clip -> fires from reserve, never reloads
}

static void restore_weapon_from_multi(int wt)
{
    rf::weapon_types[wt].clip_size = rf::weapon_types[wt].clip_size_multi;
}

void mutators_set_no_clip_weapon(int weapon_type)
{
    // Restore the previous target's fields from their (untouched) multiplayer values.
    if (g_no_clip_weapon >= 0 && g_no_clip_weapon != weapon_type && g_no_clip_weapon < rf::num_weapon_types) {
        restore_weapon_from_multi(g_no_clip_weapon);
    }

    g_no_clip_weapon = weapon_type;

    if (weapon_type >= 0 && weapon_type < rf::num_weapon_types) {
        apply_no_clip_override(weapon_type);
    }
}

// The engine derives clip_size from clip_size_multi whenever it enters MP weapon
// mode (on join / mode transitions, sometimes after the server-info has already
// been processed). Re-apply our override immediately afterward.
static FunHook<void()> weapon_set_multiplayer_mode_hook{
    0x004C2AC0,
    []() {
        weapon_set_multiplayer_mode_hook.call_target();
        if (g_no_clip_weapon >= 0 && g_no_clip_weapon < rf::num_weapon_types)
            apply_no_clip_override(g_no_clip_weapon);
    },
};

// Per-shot ammo consumption (clip for clip weapons, reserve for no-clip weapons).
// Suppress it for the no-clip featured weapon so its ammo is truly infinite. Runs
// on both server and client (both predict fire), keeping them in sync.
FunHook<void(rf::Entity*, int)> entity_consume_ammo_on_fire_hook{
    0x004257C0,
    [](rf::Entity* ep, int weapon_type) {
        if (weapon_type == g_no_clip_weapon)
            return; // infinite ammo for the no-clip featured weapon
        entity_consume_ammo_on_fire_hook.call_target(ep, weapon_type);
    },
};

static bool entity_holds_no_clip_weapon(rf::Entity* ep)
{
    return ep && g_no_clip_weapon >= 0 && ep->ai.current_primary_weapon == g_no_clip_weapon;
}

// Rail third person fire anims include the motions for a reload. Suppress those
// fire animations if the rail is set to no-clip. Client side only.
static constexpr int ACTION_FIRE_STAND = 2;
static constexpr int ACTION_FIRE_CROUCH = 4;

FunHook<void(rf::Entity*, int, float, bool, bool)> entity_play_action_animation_hook{
    0x00428C90,
    [](rf::Entity* ep, int action, float transition, bool hold_last, bool with_sound) {
        if (!rf::is_server && (action == ACTION_FIRE_STAND || action == ACTION_FIRE_CROUCH) &&
            entity_holds_no_clip_weapon(ep)) {
            return; // suppress third-person fire body animation for the no-clip weapon
        }
        entity_play_action_animation_hook.call_target(ep, action, transition, hold_last, with_sound);
    },
};

void mutators_do_patch()
{
    weapon_set_multiplayer_mode_hook.install();
    entity_consume_ammo_on_fire_hook.install();
    entity_play_action_animation_hook.install();
}
