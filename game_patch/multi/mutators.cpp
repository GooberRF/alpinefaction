#include <algorithm>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>
#include <patch_common/FunHook.h>
#include <common/utils/list-utils.h>
#include <common/utils/string-utils.h>
#include "mutators.h"
#include "server_internal.h"
#include "multi.h"
#include "gametype.h"
#include "kill.h"
#include "../rf/weapon.h"
#include "../rf/item.h"
#include "../rf/entity.h"
#include "../rf/ai.h"
#include "../rf/multi.h"
#include "../rf/gameseq.h"
#include "../rf/player/player.h"
#include "../rf/os/console.h"
#include "../rf/os/timestamp.h"

// Spawn reserve for a no-clip "infinite ammo" weapon. Firing draws from reserve,
// but we suppress the per-shot decrement, so this is purely the (constant) number
// the HUD shows and it never counts down.
static constexpr int NO_CLIP_RESERVE = 1;

// Config-supplied strings are clamped before being interpolated into a console
// line: the engine's console output (0x00509EC0) copies a line into its ring
// buffer without bounding it to the slot size, so an over-long line corrupts
// adjacent entries.
static std::string_view clamp_for_console(std::string_view value)
{
    constexpr size_t max_len = 32;
    return value.substr(0, std::min(value.size(), max_len));
}

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
static constexpr bool RAILS_DEFAULT_EXCLUDE_THROWN = true;

static void apply_rails(AlpineServerConfigRules& r, const toml::table& opts)
{
    int featured = rf::rail_gun_weapon_type;
    if (auto v = opts["featured_weapon"].value<std::string>()) {
        int idx = rf::weapon_lookup_type(v->c_str());
        if (idx >= 0)
            featured = idx;
        else
            rf::console::print("  [WARN] Rails mutator: unknown featured_weapon '{}', using rail gun\n",
                               clamp_for_console(*v));
    }
    const bool exclude_thrown = opts["exclude_thrown"].value_or(RAILS_DEFAULT_EXCLUDE_THROWN);

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

// Vampire: landing damage on another player heals the attacker for a fixed
// share of the damage that was actually applied — 1:2, i.e. 100 damage dealt
// gives 50 effective health back.
static constexpr float VAMPIRE_HEAL_RATIO = 0.5f;
static constexpr bool VAMPIRE_DEFAULT_HIDE_HEALTH_ARMOR = true;

static void apply_vampire(AlpineServerConfigRules& r, const toml::table& opts)
{
    r.mutators.vampire_enabled = true;

    // Composed with (not replacing) whatever pickup policy is in force.
    r.mutators.hide_health_armor_pickups =
        opts["hide_health_armor_pickups"].value_or(VAMPIRE_DEFAULT_HIDE_HEALTH_ARMOR);
}

// Super Drain: health and armor above the entity's normal max rot back down.
static void apply_super_drain(AlpineServerConfigRules& r, const toml::table& /*opts*/)
{
    r.mutators.super_drain_enabled = true;
}

// ============================================================================
// Registry + application order
// ============================================================================

// Static half of the option schema. Option ids are frozen per mutator (they are
// wire values in the vote-options schema); choice lists and defaults that depend
// on the loaded weapon table are filled in by mutators_get_registry().
struct MutatorOptionDef
{
    uint8_t id;
    const char* name;  // toml key
    const char* label; // shown in the client UI
    MutatorOptionType type;
};

static const MutatorOptionDef RAILS_OPTIONS[] = {
    {0, "featured_weapon", "Weapon", MutatorOptionType::Choice},
    {1, "exclude_thrown", "Keep thrown explosives", MutatorOptionType::Bool},
};

// Label kept short enough to clear the option row's checkbox label space at 640x480.
static const MutatorOptionDef VAMPIRE_OPTIONS[] = {
    {0, "hide_health_armor_pickups", "No health/armor pickups", MutatorOptionType::Bool},
};

struct MutatorDef
{
    MutatorId id;
    const char* name;  // matched against the toml name
    const char* label; // shown in the printed rules
    // Minimum AF client MINOR version (major implicitly 1) needed to play with
    // this mutator active, or MUTATOR_NO_CLIENT_REQUIREMENT.
    int min_client_minor_version;
    void (*apply)(AlpineServerConfigRules&, const toml::table&);
    const MutatorOptionDef* options;
    size_t num_options;
};

// Per-mutator client requirement — what the mutator actually depends on, not
// necessarily what release it shipped in.
static const MutatorDef MUTATORS[] = {
    {MutatorId::Instagib, "instagib", "Instagib", 4, &apply_instagib, nullptr, 0},
    {MutatorId::Rails, "rails", "Rails", 4, &apply_rails, RAILS_OPTIONS, std::size(RAILS_OPTIONS)},
    {MutatorId::Arena, "arena", "Arena", 4, &apply_arena, nullptr, 0},
    {MutatorId::Vampire, "vampire", "Vampire", MUTATOR_NO_CLIENT_REQUIREMENT, &apply_vampire, VAMPIRE_OPTIONS, std::size(VAMPIRE_OPTIONS)},
    {MutatorId::SuperDrain, "superdrain", "Super Drain", 4, &apply_super_drain, nullptr, 0},
};

// Hardcoded order in which simultaneously-active mutators are applied. Later
// entries win where they overlap.
static const MutatorId MUTATOR_APPLY_ORDER[] = {
    MutatorId::SuperDrain,
    MutatorId::Vampire,
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
static bool is_known_mutator_option(const MutatorDef& def, std::string_view key)
{
    if (key == "name")
        return true;
    for (size_t i = 0; i < def.num_options; ++i) {
        if (key == def.options[i].name)
            return true;
    }
    return false;
}

// Short labels for the featured-weapon selector in the client's vote panel.
struct WeaponShortLabel
{
    const char* match; // stock display name or weapons.tbl class name
    const char* label;
};

// Only display names are needed for Vampire, but map includes class names too
// so it can be used in the future for other short name translations.
static const WeaponShortLabel WEAPON_SHORT_LABELS[] = {
    {"Remote Charge", "Charges"},           // class + display
    {"Control Baton", "Baton"},             // display
    {"Riot Stick", "Baton"},                // class
    {"12mm pistol", "Pistol"},              // display
    {"12mm handgun", "Pistol"},             // class
    {"Automatic Shotgun", "Shotgun"},       // display
    {"Shotgun", "Shotgun"},                 // class
    {"Sniper Rifle", "Sniper"},             // class + display
    {"Rocket Launcher", "Rocket"},          // class + display
    {"Assault Rifle", "AR"},                // class + display
    {"Submachine Gun", "SMG"},              // display
    {"Machine Pistol", "SMG"},              // class
    {"Grenade", "Grenades"},                // class + display
    {"Grenades", "Grenades"},               // a table that already pluralises
    {"Flamethrower", "Flame"},              // class + display
    {"Riot Shield", "Shield"},              // class + display
    {"Rail Driver", "Rail"},                // display
    {"rail_gun", "Rail"},                   // class
    {"Heavy Machine Gun", "HMG"},           // display
    {"heavy_machine_gun", "HMG"},           // class
    {"Precision Rifle", "PR"},              // display
    {"scope_assault_rifle", "PR"},          // class
    {"Fusion Rocket Launcher", "Fusion"},   // display
    {"shoulder_cannon", "Fusion"},          // class
};

static const char* weapon_short_label(std::string_view display_name, std::string_view class_name)
{
    for (const auto& entry : WEAPON_SHORT_LABELS) {
        if (string_iequals(display_name, entry.match) || string_iequals(class_name, entry.match))
            return entry.label;
    }
    return nullptr;
}

// Weapons the Rails mutator can feature: anything a level pickup can grant.
// Rails redirects every weapon/ammo pickup onto the featured weapon's own
// pickup, so a weapon without one would leave players stuck with the baton.
// Not relevant in the stock game but could be in TC mods.
static std::vector<MutatorOptionChoice> build_featured_weapon_choices()
{
    std::vector<MutatorOptionChoice> choices;
    for (int wt = 0; wt < rf::num_weapon_types; ++wt) {
        if (rf::weapon_is_detonator(wt))
            continue;

        bool has_pickup = false;
        for (int i = 0; i < rf::num_item_types && !has_pickup; ++i)
            has_pickup = rf::item_info[i].gives_weapon_id == wt;
        if (!has_pickup)
            continue;

        std::string value = rf::weapon_types[wt].name.c_str();
        if (value.empty())
            continue;
        std::string label = rf::weapon_types[wt].display_name.c_str();
        if (label.empty())
            label = value;
        if (const char* short_label = weapon_short_label(label, value))
            label = short_label;
        choices.push_back(MutatorOptionChoice{std::move(label), std::move(value)});
    }
    return choices;
}

static std::vector<MutatorInfo> g_mutator_registry;
// Whether the cached build saw the weapon/item tables.
static bool g_mutator_registry_built_with_tables = false;

const std::vector<MutatorInfo>& mutators_get_registry()
{
    const bool tables_ready = rf::num_weapon_types > 0 && rf::num_item_types > 0;
    if (!g_mutator_registry.empty() && (g_mutator_registry_built_with_tables || !tables_ready))
        return g_mutator_registry;

    std::vector<MutatorInfo> registry;
    for (const auto& def : MUTATORS) {
        MutatorInfo info;
        info.id = def.id;
        info.name = def.name;
        info.label = def.label;
        info.min_client_minor_version = def.min_client_minor_version;

        for (size_t i = 0; i < def.num_options; ++i) {
            const MutatorOptionDef& opt_def = def.options[i];
            MutatorOptionInfo opt;
            opt.id = opt_def.id;
            opt.name = opt_def.name;
            opt.label = opt_def.label;
            opt.type = opt_def.type;

            if (def.id == MutatorId::Rails && opt.name == "featured_weapon") {
                opt.choices = build_featured_weapon_choices();
                const std::string rail_name =
                    (rf::rail_gun_weapon_type >= 0 && rf::rail_gun_weapon_type < rf::num_weapon_types)
                        ? rf::weapon_types[rf::rail_gun_weapon_type].name.c_str()
                        : "";
                for (size_t c = 0; c < opt.choices.size(); ++c) {
                    if (string_iequals(opt.choices[c].value, rail_name)) {
                        opt.default_choice = static_cast<uint8_t>(c);
                        break;
                    }
                }
            }
            else if (def.id == MutatorId::Rails && opt.name == "exclude_thrown") {
                opt.default_bool = RAILS_DEFAULT_EXCLUDE_THROWN;
            }
            else if (def.id == MutatorId::Vampire && opt.name == "hide_health_armor_pickups") {
                opt.default_bool = VAMPIRE_DEFAULT_HIDE_HEALTH_ARMOR;
            }

            info.options.push_back(std::move(opt));
        }

        registry.push_back(std::move(info));
    }

    g_mutator_registry = std::move(registry);
    g_mutator_registry_built_with_tables = tables_ready;
    return g_mutator_registry;
}

const MutatorInfo* mutators_find_by_id(MutatorId id)
{
    for (const auto& info : mutators_get_registry())
        if (info.id == id)
            return &info;
    return nullptr;
}

const MutatorInfo* mutators_find_by_name(std::string_view name)
{
    for (const auto& info : mutators_get_registry())
        if (string_iequals(name, info.name))
            return &info;
    return nullptr;
}

int mutators_min_client_minor_version(const std::vector<MutatorDeclaration>& declarations)
{
    int required = MUTATOR_NO_CLIENT_REQUIREMENT;
    for (const auto& decl : declarations) {
        // A name with no registry entry cannot be in force — apply_mutators_from_toml
        // rejects unknown names before a declaration is ever recorded — so a miss
        // here means "no requirement", not a defaulted one.
        if (const MutatorInfo* info = mutators_find_by_name(decl.name))
            required = std::max(required, info->min_client_minor_version);
    }
    return required;
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
        // Config-supplied names are clamped before being interpolated: the engine
        // console ring buffer copies a line without bounding it to its slot, so a
        // long line corrupts adjacent entries.
        const std::string_view name_for_msg = clamp_for_console(*name);

        const MutatorDef* def = find_mutator_by_name(*name);
        if (!def) {
            rf::console::print("  [WARN] unknown mutator '{}'\n", name_for_msg);
            continue;
        }

        // Flag stray keys — almost always a base-rule key (e.g. game_type) that
        // ended up inside this mutator because it was written after the
        // [[rules.mutators]] header in the TOML.
        for ([[maybe_unused]] const auto& [k, v] : *tbl) {
            const std::string_view key = k.str();
            if (!is_known_mutator_option(*def, key))
                rf::console::print("  [WARN] mutator '{}': unexpected key '{}'. In TOML, keys after [[rules.mutators]] belong to the mutator, not [rules] — move base-rule keys above the mutators array.\n",
                    name_for_msg, clamp_for_console(key));
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
        for (size_t i = 0; i < def->num_options; ++i) {
            const MutatorOptionDef& opt = def->options[i];
            const auto node = it->second[opt.name];
            switch (opt.type) {
                case MutatorOptionType::Bool:
                    if (auto v = node.value<bool>())
                        decl.options[opt.name] = *v;
                    break;
                case MutatorOptionType::Int:
                    if (auto v = node.value<int64_t>())
                        decl.options[opt.name] = static_cast<int32_t>(*v);
                    break;
                case MutatorOptionType::Float:
                    if (auto v = node.value<double>())
                        decl.options[opt.name] = static_cast<float>(*v);
                    break;
                case MutatorOptionType::Choice:
                case MutatorOptionType::String:
                    if (auto v = node.value<std::string>())
                        decl.options[opt.name] = *v;
                    break;
            }
        }
        auto& decls = rules.mutators.declarations;
        decls.erase(std::remove_if(decls.begin(), decls.end(),
            [&](const MutatorDeclaration& d) { return string_iequals(d.name, def->name); }), decls.end());
        decls.push_back(std::move(decl));
    }
}

toml::array mutator_declarations_to_toml_array(const std::vector<MutatorDeclaration>& declarations)
{
    toml::array arr;
    for (const auto& decl : declarations) {
        toml::table tbl;
        tbl.insert_or_assign("name", decl.name);
        for (const auto& [key, value] : decl.options) {
            if (const auto* v = std::get_if<bool>(&value))
                tbl.insert_or_assign(key, *v);
            else if (const auto* v = std::get_if<int32_t>(&value))
                tbl.insert_or_assign(key, static_cast<int64_t>(*v));
            else if (const auto* v = std::get_if<float>(&value))
                tbl.insert_or_assign(key, static_cast<double>(*v));
            else if (const auto* v = std::get_if<std::string>(&value))
                tbl.insert_or_assign(key, *v);
        }
        arr.push_back(std::move(tbl));
    }
    return arr;
}

std::optional<std::string> mutators_build_declarations_from_vote(
    const std::vector<VoteMutatorInput>& input, std::vector<MutatorDeclaration>& out)
{
    out.clear();

    std::set<uint8_t> seen_ids;
    for (const auto& entry : input) {
        const MutatorInfo* info = mutators_find_by_id(static_cast<MutatorId>(entry.mutator_id));
        if (!info) {
            return std::format("this server does not know mutator id {}", entry.mutator_id);
        }
        if (!seen_ids.insert(entry.mutator_id).second) {
            return std::format("mutator '{}' was selected more than once", info->label);
        }

        MutatorDeclaration decl;
        decl.name = info->name;

        for (const auto& opt_in : entry.options) {
            const MutatorOptionInfo* opt = nullptr;
            for (const auto& candidate : info->options) {
                if (candidate.id == opt_in.option_id) {
                    opt = &candidate;
                    break;
                }
            }
            if (!opt) {
                return std::format("mutator '{}' has no option id {}", info->label, opt_in.option_id);
            }
            if (opt->type != opt_in.type) {
                return std::format("option '{}' of mutator '{}' was sent with the wrong value type",
                                   opt->name, info->label);
            }

            switch (opt->type) {
                case MutatorOptionType::Bool:
                    decl.options[opt->name] = opt_in.bool_value;
                    break;
                case MutatorOptionType::Choice:
                    if (opt_in.choice_index >= opt->choices.size()) {
                        return std::format("option '{}' of mutator '{}' has an out of range selection",
                                           opt->name, info->label);
                    }
                    decl.options[opt->name] = opt->choices[opt_in.choice_index].value;
                    break;
                case MutatorOptionType::Int:
                    decl.options[opt->name] = opt_in.int_value;
                    break;
                case MutatorOptionType::Float:
                    decl.options[opt->name] = opt_in.float_value;
                    break;
                case MutatorOptionType::String:
                    decl.options[opt->name] = opt_in.string_value;
                    break;
            }
        }

        out.push_back(std::move(decl));
    }

    return std::nullopt;
}

std::string mutators_join_labels(const std::vector<MutatorDeclaration>& declarations)
{
    std::string joined;
    for (const auto& decl : declarations) {
        const MutatorInfo* info = mutators_find_by_name(decl.name);
        if (!joined.empty())
            joined += ", ";
        joined += info ? info->label : decl.name;
    }
    return joined;
}

const AlpineServerConfigRules& vote_natural_rules_for_level(std::string_view level_filename)
{
    for (const auto& entry : g_alpine_server_config.levels) {
        if (string_iequals(entry.level_filename, level_filename))
            return entry.rule_overrides_no_mutators;
    }
    // Not in the rotation: a manually named level runs on the base rules.
    return g_alpine_server_config.base_rules_no_mutators;
}

std::optional<ManualRulesOverride> load_vote_rules_override(
    std::string_view level_filename, const std::vector<MutatorDeclaration>& mutators,
    std::optional<rf::NetGameType> gametype)
{
    if (mutators.empty() && !gametype)
        return std::nullopt;

    // Inheritance rule for a vote override, in layering order:
    //   1. the rules the voted level would run with on its own — its rotation
    //      entry's rules if it is in the rotation, otherwise the base rules —
    //      with config-declared mutators stripped (voted mutators REPLACE
    //      configured ones rather than stacking on them),
    //   2. the voted game type and its gametype defaults, if one was voted,
    //   3. the voted mutator declarations.
    // Starting from the base rules instead (as this used to) silently dragged the
    // base game type onto a level whose rotation entry overrides it, so adding a
    // single mutator to a level vote could flip the whole game type.
    AlpineServerConfigRules rules = vote_natural_rules_for_level(level_filename);

    if (gametype && rules.game_type != *gametype) {
        // Only re-derive the gametype defaults when the type actually CHANGES.
        // apply_defaults_for_game_type() overwrites operator-configured rules
        // (spawn loadout, pvp_damage_modifier, spawn_delay, ...), so explicitly
        // voting the type a level already runs must behave the same as voting
        // "Server default" rather than silently wiping the config. It also
        // rebuilds the loadout and clears MutatorConfig, so mutators come after.
        rules.game_type = *gametype;
        apply_defaults_for_game_type(*gametype, rules);
    }

    if (!mutators.empty()) {
        const toml::array arr = mutator_declarations_to_toml_array(mutators);
        apply_mutators_from_toml(arr, rules);
    }

    ManualRulesOverride result;
    result.rules = std::move(rules);
    std::string labels = mutators_join_labels(mutators);
    if (!labels.empty())
        result.preset_alias = std::move(labels);
    return result;
}

// ============================================================================
// Runtime logic
// ============================================================================

// Touch callbacks the engine binds by class name. Stock bindings:
//   0x0045A2E0  medical kit      -> "Medical Kit", "First Aid Kit"
//   0x0045A1F0  suit repair      -> "Suit Repair"
//   0x0045A050  miner envirosuit -> "Miner Envirosuit"
static constexpr uintptr_t ITEM_TOUCH_MEDICAL_KIT = 0x0045A2E0;
static constexpr uintptr_t ITEM_TOUCH_SUIT_REPAIR = 0x0045A1F0;
static constexpr uintptr_t ITEM_TOUCH_MINER_ENVIROSUIT = 0x0045A050;

static bool is_standard_health_or_armor_item(const rf::ItemInfo& info)
{
    const auto callback = reinterpret_cast<uintptr_t>(info.touch_callback);
    return callback == ITEM_TOUCH_MEDICAL_KIT
        || callback == ITEM_TOUCH_SUIT_REPAIR
        || callback == ITEM_TOUCH_MINER_ENVIROSUIT;
}

void mutators_level_init_post()
{
    if (!rf::is_server)
        return;

    const auto& m = g_alpine_server_config_active_rules.mutators;
    if (m.pickup_policy == PickupPolicy::Normal && !m.hide_health_armor_pickups)
        return;

    std::vector<int> allowed;
    if (m.pickup_policy != PickupPolicy::HideAll) {
        for (int i = 0; i < rf::num_item_types; ++i) {
            const rf::ItemInfo& info = rf::item_info[i];

            // Composed on top of the policy, not instead of it: Vampire removes
            // the standard health/armour pickups from whatever set the policy
            // would otherwise keep.
            if (m.hide_health_armor_pickups && is_standard_health_or_armor_item(info))
                continue;

            if (m.pickup_policy == PickupPolicy::Normal) {
                allowed.push_back(i);
                continue;
            }

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
    if (wt < 0 || wt >= rf::num_weapon_types || !rf::weapon_uses_clip(wt))
        return;

    // Reset the clip to full directly, on both the server (authoritative) and the
    // killer's own client. No reload packet is sent, so there's no reload animation
    // or pause, you just keep firing.
    ep->ai.clip_ammo[wt] = rf::weapon_types[wt].clip_size;
}

void mutators_on_pvp_damage(rf::Player* attacker, rf::Player* victim, float effective_damage)
{
    if (!rf::is_server || !rf::is_multi)
        return;

    const auto& m = g_alpine_server_config_active_rules.mutators;
    if (!m.vampire_enabled)
        return;

    // Prevent Vampire healing from self damage.
    if (!attacker || !victim || attacker == victim)
        return;

    // Prevent Vampire healing from team damage.
    if (multi_game_type_is_team_type(rf::multi_get_game_type()) && attacker->team == victim->team)
        return;

    if (!(effective_damage > 0.0f))
        return;

    rf::Entity* ep = rf::entity_from_handle(attacker->entity_handle);
    if (!ep || !ep->info || rf::entity_is_dying(ep))
        return;

    const float max_life_limit = std::max(ep->life, ep->info->max_life);
    const float max_armor_limit = std::max(ep->armor, ep->info->max_armor);

    // Same logic effective health kill reward uses.
    distribute_effective_health(ep, effective_damage * VAMPIRE_HEAL_RATIO, max_life_limit, max_armor_limit);
}

// Super Drain tick: one global timer drains every alive player's health and
// armor by one point per second while either sits above the entity's normal max.
// The fields are written directly, so no damage is dealt and no kill can result;
// clients suppress the stock damage feedback for these decreases.
static constexpr int SUPER_DRAIN_TICK_MS = 1000;
static constexpr float SUPER_DRAIN_PER_TICK = 1.0f;

static rf::Timestamp g_super_drain_tick;

void mutators_do_frame()
{
    if (!rf::is_server)
        return;

    // Dropped rather than left running while the mutator is off or the server is
    // between levels: a deadline that expired during a level change would make
    // the first gameplay frame tick immediately instead of a second in.
    if (!g_alpine_server_config_active_rules.mutators.super_drain_enabled
        || rf::gameseq_get_state() != rf::GameState::GS_GAMEPLAY) {
        if (g_super_drain_tick.valid())
            g_super_drain_tick.invalidate();
        return;
    }

    if (!g_super_drain_tick.valid()) {
        g_super_drain_tick.set(SUPER_DRAIN_TICK_MS);
        return;
    }
    if (!g_super_drain_tick.elapsed())
        return;
    g_super_drain_tick.set(SUPER_DRAIN_TICK_MS);

    for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
        if (rf::player_is_dead(&player) || rf::player_is_dying(&player))
            continue;
        rf::Entity* ep = rf::entity_from_handle(player.entity_handle);
        if (!ep || ep->life <= 0.0f)
            continue;

        const float max_life = ep->info ? ep->info->max_life : 100.0f;
        const float max_armor = ep->info ? ep->info->max_armor : 100.0f;

        // The final tick clamps exactly to the max.
        if (ep->life > max_life)
            ep->life = std::max(ep->life - SUPER_DRAIN_PER_TICK, max_life);
        if (ep->armor > max_armor)
            ep->armor = std::max(ep->armor - SUPER_DRAIN_PER_TICK, max_armor);
    }
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
