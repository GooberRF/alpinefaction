#include "bot_combat.h"

#include "bot_internal.h"
#include "bot_math.h"
#include "bot_utils.h"
#include "bot_weapon_profiles.h"
#include "../../main/main.h"
#include "../../rf/collide.h"
#include "../../rf/item.h"
#include "../../rf/multi.h"
#include "../../rf/os/frametime.h"
#include "../../rf/player/player_fpgun.h"
#include "../../rf/weapon.h"
#include <xlog/xlog.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <random>
#include <unordered_map>
#include <vector>

namespace
{
constexpr int kMaxWeaponLookupAliases = 6;
using WeaponLookupAliases = std::array<const char*, kMaxWeaponLookupAliases>;

struct RawBotWeaponProfile
{
    const char* name = nullptr;
    WeaponLookupAliases lookup_aliases{};
    int lookup_alias_count = 0;
    BotWeaponDamageClass damage_class = BotWeaponDamageClass::bullet;
    bool is_semi_auto = false;
    BotWeaponFireMode primary_fire = BotWeaponFireMode::shoot;
    BotWeaponFireMode alt_fire = BotWeaponFireMode::none;
    int value_tier = 0;
    uint8_t range_band_mask = 0;
    uint32_t special_property_mask = 0;
};

constexpr uint8_t range_band_bit(const BotWeaponRangeBand range)
{
    return static_cast<uint8_t>(1u << static_cast<uint8_t>(range));
}

constexpr uint8_t make_range_band_mask(
    const bool melee,
    const bool close,
    const bool medium,
    const bool long_range,
    const bool very_long)
{
    return (melee ? range_band_bit(BotWeaponRangeBand::melee) : 0u)
        | (close ? range_band_bit(BotWeaponRangeBand::close) : 0u)
        | (medium ? range_band_bit(BotWeaponRangeBand::medium) : 0u)
        | (long_range ? range_band_bit(BotWeaponRangeBand::long_range) : 0u)
        | (very_long ? range_band_bit(BotWeaponRangeBand::very_long) : 0u);
}

constexpr uint32_t special_property_bit(const BotWeaponSpecialProperty property)
{
    return static_cast<uint32_t>(property);
}

const std::array<RawBotWeaponProfile, 15> kRawProfiles{{
    {
        "Remote Charge",
        {"Remote Charge", "remote_charge", "remote charge"},
        3,
        BotWeaponDamageClass::thrown_explosive_projectile,
        false,
        BotWeaponFireMode::throw_projectile,
        BotWeaponFireMode::none,
        2,
        make_range_band_mask(false, true, false, false, false),
        special_property_bit(BotWeaponSpecialProperty::creates_craters),
    },
    {
        "Riot Stick",
        {"Riot Stick", "riot_stick", "riot stick"},
        3,
        BotWeaponDamageClass::melee,
        false,
        BotWeaponFireMode::swing,
        BotWeaponFireMode::use_taser,
        0,
        make_range_band_mask(true, false, false, false, false),
        special_property_bit(BotWeaponSpecialProperty::taser_finisher),
    },
    {
        "12mm handgun",
        {"12mm handgun", "12mm Handgun", "12mm_handgun", "12mm pistol", "Pistol"},
        5,
        BotWeaponDamageClass::bullet,
        true,
        BotWeaponFireMode::shoot,
        BotWeaponFireMode::none,
        1,
        make_range_band_mask(true, true, true, false, false),
        special_property_bit(BotWeaponSpecialProperty::none),
    },
    {
        "Shotgun",
        {"Shotgun", "shotgun"},
        2,
        BotWeaponDamageClass::bullet,
        false,
        BotWeaponFireMode::high_impact_shoot,
        BotWeaponFireMode::shoot,
        3,
        make_range_band_mask(true, true, true, false, false),
        special_property_bit(BotWeaponSpecialProperty::multi_projectile_shot),
    },
    {
        "Sniper Rifle",
        {"Sniper Rifle", "sniper_rifle", "sniper rifle"},
        3,
        BotWeaponDamageClass::bullet,
        false,
        BotWeaponFireMode::shoot,
        BotWeaponFireMode::engage_scope,
        4,
        make_range_band_mask(false, false, true, true, true),
        special_property_bit(BotWeaponSpecialProperty::use_scope_for_long_range),
    },
    {
        "Rocket Launcher",
        {"Rocket Launcher", "rocket_launcher", "rocket launcher"},
        3,
        BotWeaponDamageClass::explosive_projectile,
        false,
        BotWeaponFireMode::shoot,
        BotWeaponFireMode::none,
        3,
        make_range_band_mask(false, false, true, false, false),
        special_property_bit(BotWeaponSpecialProperty::creates_craters),
    },
    {
        "Assault Rifle",
        {"Assault Rifle", "assault_rifle", "assault rifle"},
        3,
        BotWeaponDamageClass::bullet,
        false,
        BotWeaponFireMode::shoot,
        BotWeaponFireMode::shoot,
        3,
        make_range_band_mask(false, true, true, true, false),
        special_property_bit(BotWeaponSpecialProperty::always_use_alt_fire),
    },
    {
        "Machine Pistol",
        {"Machine Pistol", "machine_pistol", "machine pistol"},
        3,
        BotWeaponDamageClass::bullet,
        false,
        BotWeaponFireMode::shoot,
        BotWeaponFireMode::none,
        2,
        make_range_band_mask(false, true, true, false, false),
        special_property_bit(BotWeaponSpecialProperty::none),
    },
    {
        "Grenade",
        {"Grenade", "grenade"},
        2,
        BotWeaponDamageClass::thrown_explosive_projectile,
        false,
        BotWeaponFireMode::throw_timed_detonation,
        BotWeaponFireMode::throw_impact_detonation,
        2,
        make_range_band_mask(false, true, false, false, false),
        special_property_bit(BotWeaponSpecialProperty::creates_craters),
    },
    {
        "Flamethrower",
        {"Flamethrower", "flamethrower", "flame_thrower"},
        3,
        BotWeaponDamageClass::spray_flames,
        false,
        BotWeaponFireMode::spray_flame_stream,
        BotWeaponFireMode::throw_impact_detonation,
        1,
        make_range_band_mask(true, true, false, false, false),
        special_property_bit(BotWeaponSpecialProperty::avoid_alt_fire),
    },
    {
        "riot shield",
        {"riot shield", "riot_shield", "Riot Shield"},
        3,
        BotWeaponDamageClass::melee,
        false,
        BotWeaponFireMode::swing,
        BotWeaponFireMode::none,
        0,
        make_range_band_mask(true, false, false, false, false),
        special_property_bit(BotWeaponSpecialProperty::avoid_alt_fire),
    },
    {
        "rail_gun",
        {"rail_gun", "Rail Gun", "rail gun", "railgun"},
        4,
        BotWeaponDamageClass::piercing_bullet,
        false,
        BotWeaponFireMode::shoot,
        BotWeaponFireMode::none,
        5,
        make_range_band_mask(false, false, true, true, false),
        special_property_bit(BotWeaponSpecialProperty::pierces_world),
    },
    {
        "heavy_machine_gun",
        {"heavy_machine_gun", "Heavy Machine Gun", "heavy machine gun", "HMG"},
        4,
        BotWeaponDamageClass::bullet,
        false,
        BotWeaponFireMode::shoot,
        BotWeaponFireMode::precision_alt_shoot,
        4,
        make_range_band_mask(false, true, true, true, false),
        special_property_bit(BotWeaponSpecialProperty::alt_fire_for_long_range),
    },
    {
        "scope_assault_rifle",
        {"scope_assault_rifle", "Scope Assault Rifle", "scope assault rifle"},
        3,
        BotWeaponDamageClass::bullet,
        true,
        BotWeaponFireMode::shoot,
        BotWeaponFireMode::engage_scope,
        4,
        make_range_band_mask(false, true, true, true, true),
        special_property_bit(BotWeaponSpecialProperty::use_scope_for_long_range),
    },
    {
        "shoulder_cannon",
        {"shoulder_cannon", "Shoulder Cannon", "shoulder cannon"},
        3,
        BotWeaponDamageClass::explosive_projectile,
        false,
        BotWeaponFireMode::shoot,
        BotWeaponFireMode::none,
        5,
        make_range_band_mask(false, false, true, true, false),
        special_property_bit(BotWeaponSpecialProperty::creates_craters),
    },
}};

std::vector<BotWeaponProfile> g_bot_weapon_profiles{};
std::unordered_map<int, size_t> g_profile_index_by_weapon_type{};
bool g_bot_weapon_profiles_initialized = false;

int resolve_weapon_type(const RawBotWeaponProfile& raw_profile)
{
    const int alias_count = std::clamp(raw_profile.lookup_alias_count, 0, kMaxWeaponLookupAliases);
    for (int i = 0; i < alias_count; ++i) {
        const char* alias = raw_profile.lookup_aliases[i];
        if (!alias || alias[0] == '\0') {
            continue;
        }
        const int weapon_type = rf::weapon_lookup_type(alias);
        if (weapon_type >= 0) {
            return weapon_type;
        }
    }
    return -1;
}

void collect_item_ids_for_weapon(BotWeaponProfile& profile)
{
    if (profile.weapon_type < 0) {
        return;
    }

    for (int item_id = 0; item_id < rf::num_item_types; ++item_id) {
        const auto& item = rf::item_info[item_id];
        if (item.gives_weapon_id == profile.weapon_type) {
            profile.weapon_item_ids.push_back(item_id);
        }
        if (item.ammo_for_weapon_id == profile.weapon_type) {
            profile.ammo_item_ids.push_back(item_id);
        }
    }
}
}

void bot_weapon_profiles_init_for_active_bot()
{
    if (g_bot_weapon_profiles_initialized) {
        return;
    }
    g_bot_weapon_profiles_initialized = true;
    g_bot_weapon_profiles.clear();
    g_profile_index_by_weapon_type.clear();
    g_bot_weapon_profiles.reserve(kRawProfiles.size());
    g_profile_index_by_weapon_type.reserve(kRawProfiles.size());

    for (const auto& raw_profile : kRawProfiles) {
        BotWeaponProfile profile{};
        profile.name = raw_profile.name ? raw_profile.name : "";
        profile.damage_class = raw_profile.damage_class;
        profile.is_semi_auto = raw_profile.is_semi_auto;
        profile.primary_fire = raw_profile.primary_fire;
        profile.alt_fire = raw_profile.alt_fire;
        profile.value_tier = raw_profile.value_tier;
        profile.range_band_mask = raw_profile.range_band_mask;
        profile.special_property_mask = raw_profile.special_property_mask;
        profile.weapon_type = resolve_weapon_type(raw_profile);
        collect_item_ids_for_weapon(profile);

        const size_t profile_index = g_bot_weapon_profiles.size();
        if (profile.weapon_type >= 0) {
            g_profile_index_by_weapon_type[profile.weapon_type] = profile_index;
        }
        else {
            xlog::warn("Bot weapon profile '{}' could not resolve weapon type", profile.name);
        }

        g_bot_weapon_profiles.push_back(std::move(profile));
    }

    xlog::info("Initialized {} bot weapon profiles", g_bot_weapon_profiles.size());
}

bool bot_weapon_profiles_initialized()
{
    return g_bot_weapon_profiles_initialized;
}

const std::vector<BotWeaponProfile>& bot_weapon_profiles()
{
    return g_bot_weapon_profiles;
}

const BotWeaponProfile* bot_weapon_profile_for_weapon_type(const int weapon_type)
{
    if (!g_bot_weapon_profiles_initialized || weapon_type < 0) {
        return nullptr;
    }
    const auto it = g_profile_index_by_weapon_type.find(weapon_type);
    if (it == g_profile_index_by_weapon_type.end()) {
        return nullptr;
    }
    const size_t profile_index = it->second;
    if (profile_index >= g_bot_weapon_profiles.size()) {
        return nullptr;
    }
    return &g_bot_weapon_profiles[profile_index];
}

bool bot_weapon_profile_supports_range(const BotWeaponProfile& profile, const BotWeaponRangeBand range)
{
    return (profile.range_band_mask & range_band_bit(range)) != 0;
}

bool bot_weapon_profile_has_special(const BotWeaponProfile& profile, const BotWeaponSpecialProperty property)
{
    return (profile.special_property_mask & special_property_bit(property)) != 0;
}

int bot_weapon_profile_range_band_count(const BotWeaponProfile& profile)
{
    int count = 0;
    uint8_t mask = profile.range_band_mask;
    while (mask != 0) {
        count += static_cast<int>(mask & 1u);
        mask >>= 1u;
    }
    return count;
}

namespace
{
float bot_profile_weapon_selection_bonus(const int weapon_type)
{
    const BotWeaponProfile* profile = bot_weapon_profile_for_weapon_type(weapon_type);
    if (!profile) {
        return 0.0f;
    }

    float bonus = static_cast<float>(profile->value_tier) * 6.0f;
    bonus += static_cast<float>(bot_weapon_profile_range_band_count(*profile));
    if (bot_weapon_profile_has_special(*profile, BotWeaponSpecialProperty::pierces_world)) {
        bonus += 4.0f;
    }
    if (bot_weapon_profile_has_special(*profile, BotWeaponSpecialProperty::creates_craters)) {
        bonus += 2.0f;
    }
    bonus += (bot_get_weapon_preference_weight(weapon_type) - 1.0f) * 14.0f;
    bonus += (bot_get_weapon_skill_weight(weapon_type) - 1.0f) * 10.0f;
    return bonus;
}

BotWeaponRangeBand classify_distance_band(const float distance)
{
    if (distance <= 2.5f) {
        return BotWeaponRangeBand::melee;
    }
    if (distance <= 10.0f) {
        return BotWeaponRangeBand::close;
    }
    if (distance <= 28.0f) {
        return BotWeaponRangeBand::medium;
    }
    if (distance <= 55.0f) {
        return BotWeaponRangeBand::long_range;
    }
    return BotWeaponRangeBand::very_long;
}

float bot_profile_combat_range_bonus(const BotWeaponProfile& profile, const float distance)
{
    const BotWeaponRangeBand band = classify_distance_band(distance);
    float bonus = bot_weapon_profile_supports_range(profile, band) ? 18.0f : -12.0f;
    if (band == BotWeaponRangeBand::long_range || band == BotWeaponRangeBand::very_long) {
        if (bot_weapon_profile_has_special(profile, BotWeaponSpecialProperty::use_scope_for_long_range)) {
            bonus += 8.0f;
        }
        if (bot_weapon_profile_has_special(profile, BotWeaponSpecialProperty::alt_fire_for_long_range)) {
            bonus += 6.0f;
        }
    }
    return bonus;
}

bool bot_weapon_is_switch_blocked(const int weapon_type)
{
    if (weapon_type < 0 || weapon_type >= rf::num_weapon_types) {
        return false;
    }

    if (weapon_type == rf::machine_pistol_special_weapon_type) {
        return true;
    }

    struct BlockedWeaponEntry
    {
        const char* name = nullptr;
        int weapon_type = -2;
    };
    static std::array<BlockedWeaponEntry, 2> blocked_weapons{{
        {"Undercover 12mm handgun", -2},
        {"Machine Pistol Special", -2},
    }};

    for (BlockedWeaponEntry& blocked_weapon : blocked_weapons) {
        if (blocked_weapon.weapon_type == -2) {
            blocked_weapon.weapon_type = rf::weapon_lookup_type(blocked_weapon.name);
        }
        if (weapon_type == blocked_weapon.weapon_type) {
            return true;
        }
    }

    return false;
}

int select_best_weapon(rf::Player& player, rf::Entity& entity, const float desired_distance = -1.0f)
{
    int best_weapon = -1;
    float best_score = -1.0f;

    for (int weapon_type = 0; weapon_type < rf::num_weapon_types; ++weapon_type) {
        if (!entity.ai.has_weapon[weapon_type]) {
            continue;
        }
        if (bot_weapon_is_switch_blocked(weapon_type)) {
            continue;
        }

        if (rf::weapon_is_detonator(weapon_type)) {
            continue;
        }

        const bool is_melee = rf::weapon_is_melee(weapon_type);
        const int total_ammo = rf::player_get_weapon_total_ammo(&player, weapon_type);
        if (!is_melee && total_ammo <= 0) {
            continue;
        }

        const float damage_score = rf::weapon_types[weapon_type].damage_multi;
        const float ammo_score = is_melee ? 1.0f : static_cast<float>(total_ammo);
        float weapon_score =
            ammo_score + damage_score * 0.05f + bot_profile_weapon_selection_bonus(weapon_type);
        if (desired_distance >= 0.0f) {
            if (const BotWeaponProfile* profile = bot_weapon_profile_for_weapon_type(weapon_type)) {
                weapon_score += bot_profile_combat_range_bonus(*profile, desired_distance);
            }
        }

        if (weapon_score > best_score) {
            best_score = weapon_score;
            best_weapon = weapon_type;
        }
    }

    return best_weapon;
}

bool weapon_has_usable_ammo(rf::Player& player, rf::Entity& entity, const int weapon_type)
{
    if (weapon_type < 0 || weapon_type >= rf::num_weapon_types) {
        return false;
    }
    if (bot_weapon_is_switch_blocked(weapon_type)) {
        return false;
    }
    if (!entity.ai.has_weapon[weapon_type]) {
        return false;
    }
    if (rf::weapon_is_melee(weapon_type)) {
        return true;
    }
    if (entity.ai.clip_ammo[weapon_type] > 0) {
        return true;
    }
    return rf::player_get_weapon_total_ammo(&player, weapon_type) > 0;
}

bool personality_has_preferred_weapons(const BotPersonality& personality)
{
    return !personality.preferred_weapon_types.empty();
}

int select_personality_preferred_weapon(rf::Player& player, rf::Entity& entity)
{
    const BotPersonality& personality = get_active_bot_personality();
    if (!personality_has_preferred_weapons(personality)) {
        return -1;
    }

    for (const int preferred_weapon : personality.preferred_weapon_types) {
        if (weapon_has_usable_ammo(player, entity, preferred_weapon)) {
            return preferred_weapon;
        }
    }

    return -1;
}

void initialize_known_weapons_snapshot(const rf::Entity& entity)
{
    g_client_bot_state.known_weapons.fill(false);
    const int max_weapon = std::min(
        rf::num_weapon_types,
        static_cast<int>(g_client_bot_state.known_weapons.size())
    );
    for (int weapon_type = 0; weapon_type < max_weapon; ++weapon_type) {
        g_client_bot_state.known_weapons[weapon_type] = entity.ai.has_weapon[weapon_type];
    }
    g_client_bot_state.known_weapons_initialized = true;
}

std::vector<int> collect_newly_acquired_weapons(const rf::Entity& entity)
{
    std::vector<int> newly_acquired;
    const int max_weapon = std::min(
        rf::num_weapon_types,
        static_cast<int>(g_client_bot_state.known_weapons.size())
    );
    if (!g_client_bot_state.known_weapons_initialized) {
        initialize_known_weapons_snapshot(entity);
        return newly_acquired;
    }

    for (int weapon_type = 0; weapon_type < max_weapon; ++weapon_type) {
        const bool has_weapon_now = entity.ai.has_weapon[weapon_type];
        const bool had_weapon_before = g_client_bot_state.known_weapons[weapon_type];
        if (has_weapon_now && !had_weapon_before) {
            newly_acquired.push_back(weapon_type);
        }
        g_client_bot_state.known_weapons[weapon_type] = has_weapon_now;
    }
    return newly_acquired;
}

bool bot_weapon_action_anim_is_blocking_fire_or_switch(rf::Player& player)
{
    return rf::player_fpgun_action_anim_is_playing(&player, rf::WeaponAction::WA_RELOAD)
        || rf::player_fpgun_action_anim_is_playing(&player, rf::WeaponAction::WA_HOLSTER)
        || rf::player_fpgun_action_anim_is_playing(&player, rf::WeaponAction::WA_DRAW);
}

bool bot_weapon_fire_wait_is_blocking_switch(const rf::Entity& entity)
{
    const bool primary_wait_active =
        entity.ai.next_fire_primary.valid() && !entity.ai.next_fire_primary.elapsed();
    const bool secondary_wait_active =
        entity.ai.next_fire_secondary.valid() && !entity.ai.next_fire_secondary.elapsed();
    return primary_wait_active || secondary_wait_active;
}

int get_weapon_switch_cooldown_ms()
{
    const int configured_ms = get_active_bot_personality().min_weapon_switch_cooldown_ms;
    return std::clamp(configured_ms, 250, 15000);
}

void switch_to_weapon(
    rf::Player& player,
    rf::Entity& entity,
    const int weapon_type)
{
    if (bot_weapon_action_anim_is_blocking_fire_or_switch(player)) {
        return;
    }
    if (bot_weapon_fire_wait_is_blocking_switch(entity)) {
        return;
    }

    if (weapon_type < 0
        || weapon_type >= rf::num_weapon_types
        || weapon_type == entity.ai.current_primary_weapon) {
        return;
    }
    if (bot_weapon_is_switch_blocked(weapon_type)) {
        return;
    }

    if (rf::is_multi && !rf::is_server) {
        // Match multiplayer direct-weapon selection behavior: request selection from server.
        rf::multi_set_next_weapon(weapon_type);
    }
    else {
        rf::player_make_weapon_current_selection(&player, weapon_type);
        entity.ai.current_primary_weapon = weapon_type;
    }
    g_client_bot_state.firing.wants_fire = false;
    g_client_bot_state.fire_decision_timer.invalidate();
    g_client_bot_state.firing.explosive_fire_delay_timer.invalidate();
    g_client_bot_state.firing.explosive_fire_delay_weapon = -1;
    g_client_bot_state.firing.explosive_fire_delay_alt = false;
    g_client_bot_state.firing.explosive_release_hold_timer.invalidate();
    g_client_bot_state.firing.explosive_release_hold_target = {};
    g_client_bot_state.firing.explosive_release_hold_weapon = -1;
    g_client_bot_state.firing.explosive_release_hold_alt = false;
    clear_semi_auto_click_state();
    g_client_bot_state.weapon_switch_timer.set(get_weapon_switch_cooldown_ms());
}

void maybe_switch_weapon_for_personality(rf::Player& player, rf::Entity& entity)
{
    if (g_client_bot_state.active_goal == BotGoalType::create_crater
        || g_client_bot_state.active_goal == BotGoalType::shatter_glass
        || g_client_bot_state.fsm_state == BotFsmState::create_crater
        || g_client_bot_state.fsm_state == BotFsmState::shatter_glass) {
        return;
    }

    if (bot_weapon_action_anim_is_blocking_fire_or_switch(player)) {
        collect_newly_acquired_weapons(entity);
        return;
    }
    if (bot_weapon_fire_wait_is_blocking_switch(entity)) {
        collect_newly_acquired_weapons(entity);
        return;
    }

    if (g_client_bot_state.weapon_switch_timer.valid()
        && !g_client_bot_state.weapon_switch_timer.elapsed()) {
        collect_newly_acquired_weapons(entity);
        return;
    }

    const BotPersonality& personality = get_active_bot_personality();
    const std::vector<int> newly_acquired = collect_newly_acquired_weapons(entity);

    if (personality_has_preferred_weapons(personality)) {
        const int preferred_weapon = select_personality_preferred_weapon(player, entity);
        if (preferred_weapon >= 0 && preferred_weapon != entity.ai.current_primary_weapon) {
            switch_to_weapon(player, entity, preferred_weapon);
        }
        return;
    }

    if (newly_acquired.empty()) {
        return;
    }

    std::vector<int> switch_candidates;
    switch_candidates.reserve(newly_acquired.size());
    for (const int weapon_type : newly_acquired) {
        if (weapon_type == entity.ai.current_primary_weapon) {
            continue;
        }
        if (weapon_has_usable_ammo(player, entity, weapon_type)) {
            switch_candidates.push_back(weapon_type);
        }
    }

    if (switch_candidates.empty()) {
        return;
    }

    const BotSkillProfile& skill_profile = get_active_bot_skill_profile();
    const float switch_chance = std::clamp(
        personality.pickup_switch_chance_without_preferences
            * std::clamp(personality.weapon_switch_bias, 0.25f, 2.5f)
            * std::clamp(skill_profile.weapon_switch_likelihood, 0.05f, 1.0f),
        0.0f,
        1.0f
    );
    std::uniform_real_distribution<float> roll_dist(0.0f, 1.0f);
    if (roll_dist(g_rng) > switch_chance) {
        return;
    }

    std::uniform_int_distribution<size_t> pick_dist(0, switch_candidates.size() - 1);
    const int selected_weapon = switch_candidates[pick_dist(g_rng)];
    switch_to_weapon(player, entity, selected_weapon);
}

bool should_press_semi_auto_trigger(const int weapon_type, const float skill_factor)
{
    if (!rf::weapon_is_semi_automatic(weapon_type)) {
        return true;
    }

    if (weapon_type != g_client_bot_state.firing.semi_auto_click_weapon) {
        g_client_bot_state.firing.semi_auto_click_weapon = weapon_type;
        g_client_bot_state.firing.semi_auto_click_timer.invalidate();
    }

    if (g_client_bot_state.firing.semi_auto_click_timer.valid()
        && !g_client_bot_state.firing.semi_auto_click_timer.elapsed()) {
        return false;
    }

    int fire_wait_ms = rf::weapon_get_fire_wait_ms(weapon_type, false);
    if (fire_wait_ms <= 0) {
        fire_wait_ms = 120;
    }

    const float skill_scale = std::lerp(1.18f, 0.92f, skill_factor);
    const float base_interval = static_cast<float>(fire_wait_ms)
        * kSemiAutoClickIntervalScale
        * skill_scale;
    const float variance = std::lerp(0.24f, 0.10f, skill_factor);
    std::uniform_real_distribution<float> jitter_dist(-variance, variance);
    const float jittered_interval = base_interval * (1.0f + jitter_dist(g_rng));
    const int click_interval_ms = std::clamp(
        static_cast<int>(std::lround(jittered_interval)),
        kSemiAutoClickMinMs,
        kSemiAutoClickMaxMs
    );
    g_client_bot_state.firing.semi_auto_click_timer.set(click_interval_ms);
    return true;
}

rf::Vector3 compute_skill_adjusted_enemy_aim_target(
    const rf::Entity& local_entity,
    const rf::Entity& enemy_target,
    const float skill_factor)
{
    rf::Vector3 aim_target = enemy_target.eye_pos;
    aim_target.y = std::lerp(
        enemy_target.pos.y,
        enemy_target.eye_pos.y,
        kEnemyAimHeightBlend
    );

    rf::Vector3 to_target = aim_target - local_entity.eye_pos;
    const float dist = to_target.len();
    if (dist <= 0.0001f) {
        return aim_target;
    }

    // Projectile leading for explosive/thrown weapons
    const int weapon_type = local_entity.ai.current_primary_weapon;
    if (weapon_type >= 0 && weapon_type < rf::num_weapon_types) {
        const BotWeaponProfile* profile = bot_weapon_profile_for_weapon_type(weapon_type);
        if (profile
            && (profile->damage_class == BotWeaponDamageClass::explosive_projectile
                || profile->damage_class == BotWeaponDamageClass::thrown_explosive_projectile)) {
            const float projectile_speed = rf::weapon_types[weapon_type].max_speed;
            if (projectile_speed > 1.0f) {
                const float lead_time = std::min(dist / projectile_speed, 1.5f)
                    * std::clamp(skill_factor, 0.15f, 1.0f);
                aim_target += enemy_target.p_data.vel * lead_time;
            }
        }
    }

    if (g_client_bot_state.firing.aim_error_target_handle != enemy_target.handle) {
        g_client_bot_state.firing.aim_error_target_handle = enemy_target.handle;
        g_client_bot_state.firing.aim_error_timer.invalidate();
    }

    if (!g_client_bot_state.firing.aim_error_timer.valid()
        || g_client_bot_state.firing.aim_error_timer.elapsed()) {
        const float distance_scale = std::clamp(
            dist / kAimErrorDistanceNorm,
            0.65f,
            2.6f
        );
        const float max_error = std::clamp(
            std::lerp(kAimErrorMaxUnits, kAimErrorMinUnits, skill_factor) * distance_scale,
            kAimErrorMinUnits,
            5.0f
        );
        const float vertical_max_error = max_error * 0.82f;

        std::normal_distribution<float> lateral_dist(0.0f, max_error * 0.42f);
        std::normal_distribution<float> vertical_dist(0.0f, vertical_max_error * 0.44f);
        g_client_bot_state.firing.aim_error_right = std::clamp(
            lateral_dist(g_rng),
            -max_error,
            max_error
        );
        g_client_bot_state.firing.aim_error_up = std::clamp(
            vertical_dist(g_rng),
            -vertical_max_error,
            vertical_max_error
        );

        const float low_skill_factor = 1.0f - skill_factor;
        std::uniform_real_distribution<float> roll_dist(0.0f, 1.0f);
        if (roll_dist(g_rng) < low_skill_factor * 0.20f) {
            std::uniform_real_distribution<float> extra_dist(-max_error, max_error);
            g_client_bot_state.firing.aim_error_right = std::clamp(
                g_client_bot_state.firing.aim_error_right + extra_dist(g_rng) * 0.45f,
                -max_error,
                max_error
            );
            g_client_bot_state.firing.aim_error_up = std::clamp(
                g_client_bot_state.firing.aim_error_up + extra_dist(g_rng) * 0.35f,
                -vertical_max_error,
                vertical_max_error
            );
        }

        const int update_ms = static_cast<int>(
            std::lerp(
                static_cast<float>(kAimErrorUpdateSlowMs),
                static_cast<float>(kAimErrorUpdateFastMs),
                skill_factor
            )
        );
        g_client_bot_state.firing.aim_error_timer.set(update_ms);
    }

    rf::Vector3 forward = to_target;
    forward.normalize_safe();
    const rf::Vector3 world_up{0.0f, 1.0f, 0.0f};
    rf::Vector3 right = world_up.cross_prod(forward);
    if (right.len_sq() < 0.0001f) {
        right = rf::Vector3{1.0f, 0.0f, 0.0f};
    }
    right.normalize_safe();
    rf::Vector3 up = forward.cross_prod(right);
    up.normalize_safe();

    aim_target += right * g_client_bot_state.firing.aim_error_right;
    aim_target += up * g_client_bot_state.firing.aim_error_up;
    return aim_target;
}

bool update_aim_towards(
    rf::Entity& entity,
    const rf::Vector3& target_pos,
    const float skill_factor,
    float& out_alignment)
{
    rf::Vector3 look_dir = target_pos - entity.eye_pos;
    if (look_dir.len_sq() < 0.0001f) {
        look_dir = target_pos - entity.pos;
    }
    if (look_dir.len_sq() < 0.0001f) {
        out_alignment = 1.0f;
        return true;
    }
    look_dir.normalize_safe();

    const rf::Vector3 current_forward = forward_from_non_linear_yaw_pitch(
        entity.control_data.phb.y,
        entity.control_data.eye_phb.x
    );

    const bool combat_tracking =
        g_client_bot_state.active_goal == BotGoalType::eliminate_target
        || g_client_bot_state.fsm_state == BotFsmState::engage_enemy
        || g_client_bot_state.fsm_state == BotFsmState::pursue_enemy
        || g_client_bot_state.fsm_state == BotFsmState::seek_enemy;
    const float interp_alpha = std::clamp(
        combat_tracking
            ? std::lerp(0.14f, 0.32f, skill_factor)
            : std::lerp(0.10f, 0.24f, skill_factor),
        0.05f,
        0.40f
    );
    rf::Vector3 smoothed_look_dir =
        current_forward * (1.0f - interp_alpha) + look_dir * interp_alpha;
    if (smoothed_look_dir.len_sq() < 0.0001f) {
        smoothed_look_dir = look_dir;
    }
    smoothed_look_dir.normalize_safe();

    const float desired_yaw = std::atan2(smoothed_look_dir.x, smoothed_look_dir.z);
    const float desired_pitch = std::clamp(
        non_linear_pitch_from_forward_vector(smoothed_look_dir),
        -1.45f,
        1.45f
    );

    const float frame_dt = std::clamp(rf::frametime, 0.001f, 0.05f);
    const float yaw_speed = std::lerp(1.8f, 10.0f, skill_factor);
    const float pitch_speed = std::lerp(1.3f, 7.0f, skill_factor);

    entity.control_data.phb.y = approach_angle(
        entity.control_data.phb.y,
        desired_yaw,
        yaw_speed * frame_dt
    );
    entity.control_data.eye_phb.x = std::clamp(
        approach_angle(
            entity.control_data.eye_phb.x,
            desired_pitch,
            pitch_speed * frame_dt
        ),
        -1.45f,
        1.45f
    );

    const rf::Vector3 forward = forward_from_non_linear_yaw_pitch(
        entity.control_data.phb.y,
        entity.control_data.eye_phb.x
    );

    out_alignment = forward.dot_prod(look_dir);
    const float required_alignment = std::lerp(0.985f, 0.80f, skill_factor);
    return out_alignment >= required_alignment;
}

int get_grenade_weapon_type()
{
    static int grenade_weapon_type = -2;
    if (grenade_weapon_type == -2) {
        grenade_weapon_type = rf::weapon_lookup_type("Grenade");
    }
    return grenade_weapon_type;
}

int get_remote_charge_weapon_type()
{
    static int remote_charge_weapon_type = -2;
    if (remote_charge_weapon_type == -2) {
        remote_charge_weapon_type = rf::weapon_lookup_type("Remote Charge");
    }
    return remote_charge_weapon_type;
}

bool weapon_uses_projectile_release_hold(const int weapon_type)
{
    return weapon_type == get_grenade_weapon_type()
        || weapon_type == get_remote_charge_weapon_type();
}

int get_weapon_projectile_release_hold_ms(const int weapon_type, const bool use_alt_fire)
{
    if (weapon_type < 0 || weapon_type >= rf::num_weapon_types) {
        return 0;
    }

    const rf::WeaponInfo& info = rf::weapon_types[weapon_type];
    const int mode_index = rf::is_multi ? 1 : 0;
    const int fallback_index = mode_index == 0 ? 1 : 0;

    auto pick_delay_seconds = [&](const bool alt_fire) {
        float delay = alt_fire
            ? info.alt_create_weapon_delay_seconds[mode_index]
            : info.create_weapon_delay_seconds[mode_index];
        if (delay <= 0.0f) {
            delay = alt_fire
                ? info.alt_create_weapon_delay_seconds[fallback_index]
                : info.create_weapon_delay_seconds[fallback_index];
        }
        return std::max(delay, 0.0f);
    };

    float delay_seconds = pick_delay_seconds(use_alt_fire);
    if (delay_seconds <= 0.0f) {
        delay_seconds = pick_delay_seconds(false);
    }
    if (delay_seconds <= 0.0f) {
        return 0;
    }

    return static_cast<int>(std::lround(delay_seconds * 1000.0f));
}

bool decide_fire(
    const float skill_factor,
    const bool has_aim_solution,
    const float dist_sq,
    const float range)
{
    if (!has_aim_solution || range <= 0.0f) {
        g_client_bot_state.firing.wants_fire = false;
        g_client_bot_state.fire_decision_timer.invalidate();
        return false;
    }

    if (!g_client_bot_state.fire_decision_timer.valid()
        || g_client_bot_state.fire_decision_timer.elapsed()) {
        const float distance = std::sqrt(std::max(dist_sq, 0.0f));
        const float distance_ratio = distance / std::max(range, 1.0f);
        const float distance_factor = 1.0f - std::clamp(distance_ratio, 0.0f, 1.0f);
        const float over_range_ratio = std::max(distance_ratio - 1.0f, 0.0f);
        const float over_range_penalty = 1.0f / (1.0f + over_range_ratio * 1.5f);

        float fire_chance = (0.08f + skill_factor * 0.82f + distance_factor * 0.25f)
            * over_range_penalty;
        fire_chance = std::clamp(fire_chance, 0.01f, 1.0f);

        std::uniform_real_distribution<float> roll_dist(0.0f, 1.0f);
        g_client_bot_state.firing.wants_fire = roll_dist(g_rng) <= fire_chance;

        const int fire_eval_interval_ms = static_cast<int>(
            std::lerp(260.0f, 45.0f, skill_factor)
        );
        g_client_bot_state.fire_decision_timer.set(fire_eval_interval_ms);
    }

    return g_client_bot_state.firing.wants_fire;
}

struct CombatFireMode
{
    bool use_alt_fire = false;
    bool is_semi_auto = true;
};

bool weapon_fire_mode_is_throw(const BotWeaponFireMode fire_mode)
{
    switch (fire_mode) {
        case BotWeaponFireMode::throw_projectile:
        case BotWeaponFireMode::throw_timed_detonation:
        case BotWeaponFireMode::throw_impact_detonation:
            return true;
        default:
            return false;
    }
}

CombatFireMode select_combat_fire_mode(const int weapon_type, const float distance)
{
    CombatFireMode mode{};
    mode.is_semi_auto = rf::weapon_is_semi_automatic(weapon_type);

    const BotWeaponProfile* profile = bot_weapon_profile_for_weapon_type(weapon_type);
    if (!profile) {
        if (rf::weapon_is_detonator(weapon_type)) {
            mode.is_semi_auto = true;
        }
        return mode;
    }

    // Treat profile metadata as authoritative for bot behavior.
    mode.is_semi_auto = profile->is_semi_auto;

    bool alt_mode_locked = false;
    if (bot_weapon_profile_has_special(
            *profile,
            BotWeaponSpecialProperty::always_use_alt_fire)) {
        mode.use_alt_fire = true;
        // "always_use_alt_fire" profile entries are currently intended for continuous fire.
        mode.is_semi_auto = false;
        alt_mode_locked = true;
    }

    if (!alt_mode_locked
        && bot_weapon_profile_has_special(
            *profile,
            BotWeaponSpecialProperty::avoid_alt_fire)) {
        mode.use_alt_fire = false;
        alt_mode_locked = true;
    }

    if (!alt_mode_locked) {
        const BotWeaponRangeBand range_band = classify_distance_band(distance);
        const bool is_long_range =
            range_band == BotWeaponRangeBand::long_range
            || range_band == BotWeaponRangeBand::very_long;
        if (is_long_range
            && bot_weapon_profile_has_special(
                *profile,
                BotWeaponSpecialProperty::alt_fire_for_long_range)) {
            mode.use_alt_fire = true;
        }
    }

    const BotWeaponFireMode selected_fire_mode =
        mode.use_alt_fire ? profile->alt_fire : profile->primary_fire;
    if (weapon_fire_mode_is_throw(selected_fire_mode)
        || rf::weapon_is_detonator(weapon_type)) {
        // Thrown explosives and detonators should be actuated as discrete trigger pulls.
        mode.is_semi_auto = true;
    }

    return mode;
}

bool should_hold_automatic_fire(const bool engage_now)
{
    if (engage_now) {
        // Automatic fire should behave like held input while we are in kill mode.
        g_client_bot_state.auto_fire_release_guard_timer.set(350);
        g_client_bot_state.firing.wants_fire = true;
        g_client_bot_state.fire_decision_timer.invalidate();
        return true;
    }

    if (g_client_bot_state.auto_fire_release_guard_timer.valid()
        && !g_client_bot_state.auto_fire_release_guard_timer.elapsed()) {
        g_client_bot_state.firing.wants_fire = true;
        g_client_bot_state.fire_decision_timer.invalidate();
        return true;
    }

    g_client_bot_state.auto_fire_release_guard_timer.invalidate();
    g_client_bot_state.firing.wants_fire = false;
    g_client_bot_state.fire_decision_timer.invalidate();
    return false;
}

float get_weapon_range(const int weapon_type)
{
    if (weapon_type < 0 || weapon_type >= rf::num_weapon_types) {
        return 0.0f;
    }

    const rf::WeaponInfo& wi = rf::weapon_types[weapon_type];
    float range = std::max(wi.ai_max_range_multi, wi.ai_max_range);
    if (range <= 0.0f) {
        range = 80.0f;
    }
    return range;
}

float compute_weapon_skill_adjusted_factor(const float base_skill, const int weapon_type)
{
    const float weapon_weight = std::clamp(bot_get_weapon_skill_weight(weapon_type), 0.35f, 1.65f);
    return std::clamp(base_skill * weapon_weight, 0.0f, 1.0f);
}

float get_entity_health_ratio(const rf::Entity& entity)
{
    return std::clamp(entity.life / kBotNominalMaxHealth, 0.0f, 1.0f);
}

float get_entity_armor_ratio(const rf::Entity& entity)
{
    return std::clamp(entity.armor / kBotNominalMaxArmor, 0.0f, 1.0f);
}

// resolve_weapon_type_cached is defined in bot_utils.cpp

int find_best_melee_weapon(rf::Player& local_player, rf::Entity& local_entity)
{
    int best_weapon = -1;
    float best_score = -std::numeric_limits<float>::infinity();
    for (int weapon_type = 0; weapon_type < rf::num_weapon_types; ++weapon_type) {
        if (!rf::weapon_is_melee(weapon_type)
            || !weapon_has_usable_ammo(local_player, local_entity, weapon_type)) {
            continue;
        }

        float score = 1.0f + bot_profile_weapon_selection_bonus(weapon_type);
        if (const BotWeaponProfile* profile = bot_weapon_profile_for_weapon_type(weapon_type)) {
            score += static_cast<float>(profile->value_tier) * 2.0f;
        }
        if (score > best_score) {
            best_score = score;
            best_weapon = weapon_type;
        }
    }
    return best_weapon;
}

int find_best_crater_weapon(rf::Player& local_player, rf::Entity& local_entity)
{
    int best_weapon = -1;
    float best_score = -std::numeric_limits<float>::infinity();
    for (int weapon_type = 0; weapon_type < rf::num_weapon_types; ++weapon_type) {
        if (!weapon_has_usable_ammo(local_player, local_entity, weapon_type)) {
            continue;
        }
        const BotWeaponProfile* profile = bot_weapon_profile_for_weapon_type(weapon_type);
        if (!profile
            || !bot_weapon_profile_has_special(*profile, BotWeaponSpecialProperty::creates_craters)) {
            continue;
        }

        const float score =
            static_cast<float>(profile->value_tier) * 4.5f
            + bot_profile_weapon_selection_bonus(weapon_type);
        if (score > best_score) {
            best_score = score;
            best_weapon = weapon_type;
        }
    }
    return best_weapon;
}

bool weapon_can_shatter_glass(const int weapon_type)
{
    if (weapon_type < 0 || weapon_type >= rf::num_weapon_types) {
        return false;
    }
    if (bot_weapon_is_switch_blocked(weapon_type)
        || rf::weapon_is_detonator(weapon_type)
        || rf::weapon_is_melee(weapon_type)) {
        return false;
    }

    const BotWeaponProfile* profile = bot_weapon_profile_for_weapon_type(weapon_type);
    if (!profile) {
        return true;
    }

    return profile->damage_class != BotWeaponDamageClass::melee
        && profile->damage_class != BotWeaponDamageClass::thrown_explosive_projectile;
}

int find_best_shatter_weapon(rf::Player& local_player, rf::Entity& local_entity)
{
    int best_weapon = -1;
    float best_score = -std::numeric_limits<float>::infinity();
    for (int weapon_type = 0; weapon_type < rf::num_weapon_types; ++weapon_type) {
        if (!weapon_has_usable_ammo(local_player, local_entity, weapon_type)
            || !weapon_can_shatter_glass(weapon_type)) {
            continue;
        }

        float score = bot_profile_weapon_selection_bonus(weapon_type);
        if (const BotWeaponProfile* profile = bot_weapon_profile_for_weapon_type(weapon_type)) {
            score += static_cast<float>(profile->value_tier) * 3.6f;
            if (profile->damage_class == BotWeaponDamageClass::bullet
                || profile->damage_class == BotWeaponDamageClass::piercing_bullet) {
                score += 3.0f;
            }
        }
        if (score > best_score) {
            best_score = score;
            best_weapon = weapon_type;
        }
    }
    return best_weapon;
}

bool weapon_can_create_craters(const int weapon_type)
{
    if (weapon_type < 0 || weapon_type >= rf::num_weapon_types) {
        return false;
    }
    if (weapon_type == rf::remote_charge_det_weapon_type
        || rf::weapon_is_detonator(weapon_type)) {
        return true;
    }

    const BotWeaponProfile* profile = bot_weapon_profile_for_weapon_type(weapon_type);
    return profile
        && bot_weapon_profile_has_special(*profile, BotWeaponSpecialProperty::creates_craters);
}

bool apply_explosive_pre_fire_delay(
    const int weapon_type,
    const bool use_alt_fire,
    const bool wants_attack)
{
    if (rf::weapon_is_detonator(weapon_type)) {
        g_client_bot_state.firing.explosive_fire_delay_timer.invalidate();
        g_client_bot_state.firing.explosive_fire_delay_weapon = -1;
        g_client_bot_state.firing.explosive_fire_delay_alt = false;
        return wants_attack;
    }

    if (!wants_attack || !weapon_can_create_craters(weapon_type)) {
        g_client_bot_state.firing.explosive_fire_delay_timer.invalidate();
        g_client_bot_state.firing.explosive_fire_delay_weapon = -1;
        g_client_bot_state.firing.explosive_fire_delay_alt = false;
        return wants_attack;
    }

    const bool fire_already_down = use_alt_fire
        ? g_client_bot_state.firing.synthetic_secondary_fire_down
        : g_client_bot_state.firing.synthetic_primary_fire_down;
    if (fire_already_down) {
        return true;
    }

    const bool same_request =
        g_client_bot_state.firing.explosive_fire_delay_weapon == weapon_type
        && g_client_bot_state.firing.explosive_fire_delay_alt == use_alt_fire;
    if (!same_request || !g_client_bot_state.firing.explosive_fire_delay_timer.valid()) {
        g_client_bot_state.firing.explosive_fire_delay_weapon = weapon_type;
        g_client_bot_state.firing.explosive_fire_delay_alt = use_alt_fire;
        g_client_bot_state.firing.explosive_fire_delay_timer.set(25);
        return false;
    }
    if (!g_client_bot_state.firing.explosive_fire_delay_timer.elapsed()) {
        return false;
    }
    return true;
}

int get_active_impact_delay_remaining_ms(const rf::Entity& entity, const bool use_alt_fire)
{
    const int slot = use_alt_fire ? 1 : 0;
    if (slot < 0 || slot >= 2) {
        return 0;
    }

    const rf::Timestamp& delay = entity.ai.create_weapon_delay_timestamps[slot];
    if (!delay.valid() || delay.elapsed()) {
        return 0;
    }
    return std::max(delay.time_until(), 0);
}

bool target_has_linked_waypoint_los(
    const rf::Entity& local_entity,
    const WaypointTargetDefinition& target)
{
    if (target.waypoint_uids.empty()) {
        return false;
    }

    const rf::Object* local_obj = static_cast<const rf::Object*>(&local_entity);
    for (const int waypoint_uid : target.waypoint_uids) {
        rf::Vector3 waypoint_pos{};
        if (waypoint_uid <= 0 || !waypoints_get_pos(waypoint_uid, waypoint_pos)) {
            continue;
        }

        const rf::Vector3 waypoint_los_pos = waypoint_pos + rf::Vector3{0.0f, 1.0f, 0.0f};
        if (bot_has_unobstructed_level_los(
                local_entity.eye_pos,
                waypoint_los_pos,
                local_obj,
                nullptr)) {
            return true;
        }
    }

    return false;
}

bool crater_target_has_linked_waypoint_los(const rf::Entity& local_entity, const int target_uid)
{
    WaypointTargetDefinition target{};
    if (target_uid < 0
        || !waypoints_get_target_by_uid(target_uid, target)
        || target.type != WaypointTargetType::explosion) {
        return false;
    }

    return target_has_linked_waypoint_los(local_entity, target);
}

bool shatter_target_has_linked_waypoint_los(const rf::Entity& local_entity, const int target_uid)
{
    WaypointTargetDefinition target{};
    if (target_uid < 0
        || !waypoints_get_target_by_uid(target_uid, target)
        || target.type != WaypointTargetType::shatter) {
        return false;
    }

    return target_has_linked_waypoint_los(local_entity, target);
}

bool shatter_target_has_direct_los(const rf::Entity& local_entity, const int target_uid)
{
    WaypointTargetDefinition target{};
    if (target_uid < 0
        || !waypoints_get_target_by_uid(target_uid, target)
        || target.type != WaypointTargetType::shatter) {
        return false;
    }

    const rf::Object* local_obj = static_cast<const rf::Object*>(&local_entity);
    return bot_has_unobstructed_level_los(
        local_entity.eye_pos,
        target.pos,
        local_obj,
        nullptr);
}

void maybe_switch_weapon_for_crater_goal(
    rf::Player& local_player,
    rf::Entity& local_entity)
{
    if (g_client_bot_state.active_goal != BotGoalType::create_crater) {
        return;
    }
    if (bot_weapon_action_anim_is_blocking_fire_or_switch(local_player)) {
        return;
    }
    if (bot_weapon_fire_wait_is_blocking_switch(local_entity)) {
        return;
    }
    if (g_client_bot_state.weapon_switch_timer.valid()
        && !g_client_bot_state.weapon_switch_timer.elapsed()) {
        return;
    }
    if (g_client_bot_state.firing.remote_charge_pending_detonation
        && rf::remote_charge_det_weapon_type >= 0
        && rf::remote_charge_det_weapon_type < rf::num_weapon_types
        && local_entity.ai.has_weapon[rf::remote_charge_det_weapon_type]
        && local_entity.ai.current_primary_weapon != rf::remote_charge_det_weapon_type) {
        switch_to_weapon(local_player, local_entity, rf::remote_charge_det_weapon_type);
        return;
    }
    if (rf::weapon_is_detonator(local_entity.ai.current_primary_weapon)
        && g_client_bot_state.firing.remote_charge_pending_detonation) {
        // Do not preempt the stock remote-charge -> detonator flow.
        return;
    }

    const int crater_weapon = find_best_crater_weapon(local_player, local_entity);
    if (crater_weapon >= 0 && crater_weapon != local_entity.ai.current_primary_weapon) {
        switch_to_weapon(local_player, local_entity, crater_weapon);
    }
}

void maybe_switch_weapon_for_shatter_goal(
    rf::Player& local_player,
    rf::Entity& local_entity)
{
    if (g_client_bot_state.active_goal != BotGoalType::shatter_glass) {
        return;
    }
    if (bot_weapon_action_anim_is_blocking_fire_or_switch(local_player)) {
        return;
    }
    if (bot_weapon_fire_wait_is_blocking_switch(local_entity)) {
        return;
    }
    if (g_client_bot_state.weapon_switch_timer.valid()
        && !g_client_bot_state.weapon_switch_timer.elapsed()) {
        return;
    }

    const int shatter_weapon = find_best_shatter_weapon(local_player, local_entity);
    if (shatter_weapon >= 0 && shatter_weapon != local_entity.ai.current_primary_weapon) {
        switch_to_weapon(local_player, local_entity, shatter_weapon);
    }
}

void maybe_switch_weapon_for_combat_context(
    rf::Player& local_player,
    rf::Entity& local_entity,
    const rf::Entity& enemy_target,
    const bool enemy_has_los)
{
    if (bot_weapon_action_anim_is_blocking_fire_or_switch(local_player)) {
        return;
    }
    if (bot_weapon_fire_wait_is_blocking_switch(local_entity)) {
        return;
    }

    if (g_client_bot_state.weapon_switch_timer.valid()
        && !g_client_bot_state.weapon_switch_timer.elapsed()) {
        return;
    }

    if (g_client_bot_state.combat_weapon_eval_timer.valid()
        && !g_client_bot_state.combat_weapon_eval_timer.elapsed()) {
        return;
    }

    const BotPersonality& personality = get_active_bot_personality();
    const BotSkillProfile& skill_profile = get_active_bot_skill_profile();
    const float switch_chance = std::clamp(
        std::clamp(skill_profile.weapon_switch_likelihood, 0.05f, 1.0f)
            * std::clamp(personality.weapon_switch_bias, 0.25f, 2.5f),
        0.0f,
        1.0f
    );
    std::uniform_real_distribution<float> roll_dist(0.0f, 1.0f);
    if (roll_dist(g_rng) > switch_chance) {
        const int next_eval_ms = static_cast<int>(std::lround(std::lerp(760.0f, 260.0f, switch_chance)));
        g_client_bot_state.combat_weapon_eval_timer.set(next_eval_ms);
        return;
    }

    const float enemy_dist = std::sqrt(std::max(
        rf::vec_dist_squared(&local_entity.pos, &enemy_target.pos),
        0.0f
    ));
    const float enemy_health_ratio = get_entity_health_ratio(enemy_target);
    const float enemy_armor_ratio = get_entity_armor_ratio(enemy_target);
    const float enemy_survivability = enemy_health_ratio * 0.72f + enemy_armor_ratio * 0.28f;

    int quirk_weapon = -1;
    if (bot_personality_has_quirk(BotPersonalityQuirk::shotgun_low_health_finisher)
        && enemy_survivability <= 0.42f
        && enemy_dist <= 20.0f) {
        const int shotgun_type = bot_resolve_weapon_type_cached("Shotgun");
        if (weapon_has_usable_ammo(local_player, local_entity, shotgun_type)) {
            quirk_weapon = shotgun_type;
        }
    }
    if (quirk_weapon < 0
        && bot_personality_has_quirk(BotPersonalityQuirk::melee_finisher)
        && enemy_survivability <= 0.30f
        && enemy_dist <= 3.2f) {
        quirk_weapon = find_best_melee_weapon(local_player, local_entity);
    }
    if (quirk_weapon < 0
        && !enemy_has_los
        && bot_personality_has_quirk(BotPersonalityQuirk::railgun_no_los_hunter)) {
        const int railgun_type = bot_resolve_weapon_type_cached("rail_gun");
        if (weapon_has_usable_ammo(local_player, local_entity, railgun_type)) {
            quirk_weapon = railgun_type;
        }
    }
    if (quirk_weapon >= 0 && quirk_weapon != local_entity.ai.current_primary_weapon) {
        switch_to_weapon(local_player, local_entity, quirk_weapon);
        const int next_eval_ms = static_cast<int>(std::lround(std::lerp(700.0f, 220.0f, switch_chance)));
        g_client_bot_state.combat_weapon_eval_timer.set(next_eval_ms);
        return;
    }

    if (!enemy_has_los) {
        const int next_eval_ms = static_cast<int>(std::lround(std::lerp(760.0f, 260.0f, switch_chance)));
        g_client_bot_state.combat_weapon_eval_timer.set(next_eval_ms);
        return;
    }

    const int best_weapon = select_best_weapon(local_player, local_entity, enemy_dist);
    if (best_weapon >= 0 && best_weapon != local_entity.ai.current_primary_weapon) {
        switch_to_weapon(local_player, local_entity, best_weapon);
    }

    const int next_eval_ms = static_cast<int>(std::lround(std::lerp(700.0f, 220.0f, switch_chance)));
    g_client_bot_state.combat_weapon_eval_timer.set(next_eval_ms);
}
}

bool bot_has_usable_crater_weapon(rf::Player& player, rf::Entity& entity)
{
    return find_best_crater_weapon(player, entity) >= 0;
}

bool bot_has_usable_shatter_weapon(rf::Player& player, rf::Entity& entity)
{
    return find_best_shatter_weapon(player, entity) >= 0;
}

void clear_enemy_aim_error_state()
{
    g_client_bot_state.firing.aim_error_timer.invalidate();
    g_client_bot_state.firing.aim_error_target_handle = -1;
    g_client_bot_state.firing.aim_error_right = 0.0f;
    g_client_bot_state.firing.aim_error_up = 0.0f;
}

void clear_semi_auto_click_state()
{
    g_client_bot_state.firing.semi_auto_click_timer.invalidate();
    g_client_bot_state.firing.semi_auto_click_weapon = -1;
}

void ensure_weapon_ready(rf::Player& player, rf::Entity& entity)
{
    maybe_switch_weapon_for_personality(player, entity);

    int current_weapon = entity.ai.current_primary_weapon;
    const bool has_current_weapon =
        current_weapon >= 0
        && current_weapon < rf::num_weapon_types
        && entity.ai.has_weapon[current_weapon];

    bool should_reselect_weapon = !has_current_weapon;
    if (!should_reselect_weapon && !rf::weapon_is_melee(current_weapon)) {
        should_reselect_weapon =
            rf::player_get_weapon_total_ammo(&player, current_weapon) <= 0;
    }

    if (should_reselect_weapon) {
        const int best_weapon = select_best_weapon(player, entity);
        if (best_weapon >= 0 && best_weapon != current_weapon) {
            switch_to_weapon(player, entity, best_weapon);
        }
        current_weapon = entity.ai.current_primary_weapon;
    }

    if (current_weapon < 0 || current_weapon >= rf::num_weapon_types) {
        return;
    }

    if (!rf::weapon_uses_clip(current_weapon)) {
        return;
    }

    const int total_ammo = rf::player_get_weapon_total_ammo(&player, current_weapon);
    if (entity.ai.clip_ammo[current_weapon] > 0 || total_ammo <= 0) {
        return;
    }

    if (!g_client_bot_state.reload_timer.valid() || g_client_bot_state.reload_timer.elapsed()) {
        rf::player_execute_action(&player, rf::CC_ACTION_RELOAD, true);
        g_client_bot_state.reload_timer.set(800);
    }
}

void bot_process_combat(
    rf::Player& local_player,
    rf::Entity& local_entity,
    rf::Entity* enemy_target,
    const bool enemy_has_los,
    const float skill_factor,
    const bool has_move_target,
    const rf::Vector3& move_target)
{
    float aim_alignment = -1.0f;
    bool has_aim_solution = false;
    float active_skill_factor = skill_factor;
    const rf::Vector3 current_forward = forward_from_non_linear_yaw_pitch(
        local_entity.control_data.phb.y,
        local_entity.control_data.eye_phb.x
    );
    rf::Vector3 resolved_aim_target = local_entity.eye_pos + current_forward * 8.0f;
    int attack_weapon_type = -1;
    bool attack_use_alt_fire = false;
    const bool crater_goal_active =
        g_client_bot_state.active_goal == BotGoalType::create_crater
        && g_client_bot_state.fsm_state == BotFsmState::create_crater
        && g_client_bot_state.goal_target_identifier >= 0;
    const bool shatter_goal_active =
        g_client_bot_state.active_goal == BotGoalType::shatter_glass
        && g_client_bot_state.fsm_state == BotFsmState::shatter_glass
        && g_client_bot_state.goal_target_identifier >= 0;
    const bool crater_has_waypoint_los = crater_goal_active
        && crater_target_has_linked_waypoint_los(
            local_entity,
            g_client_bot_state.goal_target_identifier);
    const bool shatter_has_waypoint_los = shatter_goal_active
        && shatter_target_has_linked_waypoint_los(
            local_entity,
            g_client_bot_state.goal_target_identifier);
    bool explosive_release_hold_active =
        g_client_bot_state.firing.explosive_release_hold_timer.valid()
        && !g_client_bot_state.firing.explosive_release_hold_timer.elapsed();
    if (!explosive_release_hold_active) {
        g_client_bot_state.firing.explosive_release_hold_timer.invalidate();
    }
    if (g_client_bot_state.firing.remote_charge_pending_detonation_timer.valid()
        && g_client_bot_state.firing.remote_charge_pending_detonation_timer.elapsed()) {
        g_client_bot_state.firing.remote_charge_pending_detonation = false;
        g_client_bot_state.firing.remote_charge_pending_detonation_timer.invalidate();
    }
    if (!crater_goal_active) {
        g_client_bot_state.firing.remote_charge_pending_detonation = false;
        g_client_bot_state.firing.remote_charge_pending_detonation_timer.invalidate();
    }

    if (crater_goal_active) {
        maybe_switch_weapon_for_crater_goal(local_player, local_entity);
    }
    else if (shatter_goal_active) {
        maybe_switch_weapon_for_shatter_goal(local_player, local_entity);
    }
    else if (enemy_target) {
        maybe_switch_weapon_for_combat_context(
            local_player,
            local_entity,
            *enemy_target,
            enemy_has_los
        );
    }

    if (local_entity.ai.current_primary_weapon >= 0) {
        active_skill_factor = compute_weapon_skill_adjusted_factor(
            skill_factor,
            local_entity.ai.current_primary_weapon
        );
    }

    if (explosive_release_hold_active) {
        resolved_aim_target = g_client_bot_state.firing.explosive_release_hold_target;
        has_aim_solution = update_aim_towards(
            local_entity,
            resolved_aim_target,
            active_skill_factor,
            aim_alignment
        );
    }
    else if ((crater_goal_active && crater_has_waypoint_los)
        || (shatter_goal_active && shatter_has_waypoint_los)) {
        if (g_client_bot_state.goal_target_pos.len_sq() > 0.0001f) {
            resolved_aim_target = g_client_bot_state.goal_target_pos;
            has_aim_solution = update_aim_towards(
                local_entity,
                resolved_aim_target,
                active_skill_factor,
                aim_alignment
            );
        }
    }
    else if (enemy_target && enemy_has_los) {
        const rf::Vector3 enemy_aim_target = compute_skill_adjusted_enemy_aim_target(
            local_entity,
            *enemy_target,
            active_skill_factor
        );
        resolved_aim_target = enemy_aim_target;
        has_aim_solution = update_aim_towards(
            local_entity,
            enemy_aim_target,
            active_skill_factor,
            aim_alignment
        );
    }
    else if (has_move_target) {
        rf::Vector3 aim_target = move_target;
        const float vertical_delta = std::abs(move_target.y - local_entity.eye_pos.y);
        if (vertical_delta <= kNavigationAimVerticalThreshold) {
            aim_target.y = local_entity.eye_pos.y;
        }

        const bool precision_navigation_goal =
            bot_goal_is_item_collection(g_client_bot_state.active_goal)
            || bot_goal_is_ctf_objective(g_client_bot_state.active_goal)
            || g_client_bot_state.active_goal == BotGoalType::activate_bridge
            || g_client_bot_state.active_goal == BotGoalType::create_crater
            || g_client_bot_state.active_goal == BotGoalType::shatter_glass;
        if (!enemy_target && !precision_navigation_goal) {
            const BotPersonality& personality = get_active_bot_personality();
            const float nav_strafe_ratio = std::clamp(
                std::clamp(personality.navigation_strafe_bias, 0.0f, 2.0f) * 0.5f,
                0.0f,
                1.0f
            );
            if (nav_strafe_ratio > 0.001f) {
                constexpr float kTwoPi = 6.28318530718f;
                const float scan_speed = std::lerp(0.8f, 1.7f, nav_strafe_ratio);
                g_client_bot_state.nav_look_phase += rf::frametime * scan_speed;
                if (g_client_bot_state.nav_look_phase > kTwoPi) {
                    g_client_bot_state.nav_look_phase -= kTwoPi;
                }

                rf::Vector3 to_move = move_target - local_entity.eye_pos;
                to_move.y = 0.0f;
                if (to_move.len_sq() > 0.0001f) {
                    to_move.normalize_safe();
                    rf::Vector3 right{to_move.z, 0.0f, -to_move.x};
                    right.normalize_safe();
                    const float dist = std::sqrt(std::max(
                        rf::vec_dist_squared(&local_entity.pos, &move_target),
                        0.0f
                    ));
                    const float dist_scale = std::clamp(dist / 8.0f, 0.0f, 1.0f);
                    const float lookahead = std::lerp(1.2f, 3.6f, dist_scale);
                    const float scan_amplitude = std::lerp(0.0f, 1.6f, nav_strafe_ratio);
                    const float scan_offset =
                        std::sin(g_client_bot_state.nav_look_phase) * scan_amplitude;
                    const rf::Vector3 curiosity_target =
                        local_entity.eye_pos + to_move * lookahead + right * scan_offset;

                    const float blend = std::clamp(0.10f + 0.35f * nav_strafe_ratio, 0.0f, 0.55f);
                    aim_target = aim_target * (1.0f - blend) + curiosity_target * blend;
                    aim_target.y = local_entity.eye_pos.y;
                }
            }
        }

        resolved_aim_target = aim_target;
        has_aim_solution = update_aim_towards(
            local_entity,
            aim_target,
            active_skill_factor,
            aim_alignment
        );
    }

    bool next_primary_fire_down = false;
    bool next_secondary_fire_down = false;

    // Track target acquisition for reaction delay — the bot should not fire
    // the instant a new enemy becomes visible; a brief delay lets the aim
    // system turn towards the target first.
    bool within_acquisition_reaction = false;
    if (enemy_target && enemy_has_los) {
        if (g_client_bot_state.firing.target_acquisition_handle != enemy_target->handle) {
            g_client_bot_state.firing.target_acquisition_handle = enemy_target->handle;
            g_client_bot_state.firing.target_acquisition_timer.set(kTargetAcquisitionReactionMs);
        }
        within_acquisition_reaction =
            g_client_bot_state.firing.target_acquisition_timer.valid()
            && !g_client_bot_state.firing.target_acquisition_timer.elapsed();
    }
    else if (!enemy_target) {
        g_client_bot_state.firing.target_acquisition_handle = -1;
        g_client_bot_state.firing.target_acquisition_timer.invalidate();
    }

    if ((enemy_target || crater_goal_active || shatter_goal_active)
        && local_entity.ai.current_primary_weapon >= 0) {
        const int weapon_type = local_entity.ai.current_primary_weapon;
        const rf::Vector3 fire_target_pos = (crater_goal_active || shatter_goal_active)
            ? g_client_bot_state.goal_target_pos
            : enemy_target->pos;
        const float weapon_range = get_weapon_range(weapon_type);
        const float dist_sq = rf::vec_dist_squared(&local_entity.pos, &fire_target_pos);
        const float distance = std::sqrt(std::max(dist_sq, 0.0f));
        CombatFireMode fire_mode = select_combat_fire_mode(weapon_type, distance);
        if (crater_goal_active && weapon_type == get_grenade_weapon_type()) {
            fire_mode.use_alt_fire = true;
        }
        attack_weapon_type = weapon_type;
        attack_use_alt_fire = fire_mode.use_alt_fire;
        const bool is_semi_auto = fire_mode.is_semi_auto;
        const bool can_fire_now = !bot_weapon_action_anim_is_blocking_fire_or_switch(local_player);
        if (!can_fire_now) {
            g_client_bot_state.firing.wants_fire = false;
            g_client_bot_state.fire_decision_timer.invalidate();
            g_client_bot_state.auto_fire_release_guard_timer.invalidate();
        }
        bool wants_attack = false;
        if (crater_goal_active) {
            g_client_bot_state.auto_fire_release_guard_timer.invalidate();
            const int remote_charge_weapon_type = get_remote_charge_weapon_type();
            const bool using_remote_charge =
                weapon_type >= 0 && weapon_type == remote_charge_weapon_type;
            const bool using_remote_detonator = rf::weapon_is_detonator(weapon_type);
            wants_attack = can_fire_now
                && has_aim_solution
                && weapon_can_create_craters(weapon_type)
                && crater_has_waypoint_los;
            if (using_remote_charge && g_client_bot_state.firing.remote_charge_pending_detonation) {
                // Wait for detonator phase before throwing another charge.
                wants_attack = false;
            }
            if (using_remote_detonator && !g_client_bot_state.firing.remote_charge_pending_detonation) {
                // Avoid pointless detonator spam when there is no pending charge.
                wants_attack = false;
            }
        }
        else if (shatter_goal_active) {
            g_client_bot_state.auto_fire_release_guard_timer.invalidate();
            wants_attack = can_fire_now
                && has_aim_solution
                && weapon_can_shatter_glass(weapon_type)
                && shatter_has_waypoint_los
                && shatter_target_has_direct_los(
                    local_entity,
                    g_client_bot_state.goal_target_identifier);
        }
        else if (within_acquisition_reaction) {
            // Suppress firing during the brief reaction window after acquiring a new target.
            // This prevents the bot from shooting before the aim system has turned to face the enemy.
            wants_attack = false;
        }
        else {
            const bool automatic_engage_now =
                has_aim_solution && enemy_has_los && enemy_target != nullptr;
            wants_attack = can_fire_now && (is_semi_auto
                ? decide_fire(
                    active_skill_factor,
                    has_aim_solution && enemy_has_los,
                    dist_sq,
                    weapon_range
                )
                : should_hold_automatic_fire(automatic_engage_now));
        }

        wants_attack = apply_explosive_pre_fire_delay(
            weapon_type,
            fire_mode.use_alt_fire,
            wants_attack
        );
        if (wants_attack) {
            if (!is_semi_auto) {
                clear_semi_auto_click_state();
                if (fire_mode.use_alt_fire) {
                    next_secondary_fire_down = true;
                }
                else {
                    next_primary_fire_down = true;
                }
            }
            else if (should_press_semi_auto_trigger(weapon_type, active_skill_factor)) {
                // Semi-auto should behave as discrete trigger pulls.
                if (fire_mode.use_alt_fire) {
                    next_secondary_fire_down = true;
                }
                else {
                    next_primary_fire_down = true;
                }
            }
        }
        else {
            clear_semi_auto_click_state();
        }
    }
    else {
        g_client_bot_state.firing.wants_fire = false;
        g_client_bot_state.auto_fire_release_guard_timer.invalidate();
        g_client_bot_state.firing.explosive_fire_delay_timer.invalidate();
        g_client_bot_state.firing.explosive_fire_delay_weapon = -1;
        g_client_bot_state.firing.explosive_fire_delay_alt = false;
        clear_semi_auto_click_state();
    }

    const bool previous_primary_fire_down = g_client_bot_state.firing.synthetic_primary_fire_down;
    const bool previous_secondary_fire_down = g_client_bot_state.firing.synthetic_secondary_fire_down;
    const bool fired_primary_this_frame =
        next_primary_fire_down && !previous_primary_fire_down;
    const bool fired_secondary_this_frame =
        next_secondary_fire_down && !previous_secondary_fire_down;
    const bool fired_this_frame = fired_primary_this_frame || fired_secondary_this_frame;
    const int remote_charge_weapon_type = get_remote_charge_weapon_type();
    if (fired_this_frame && attack_weapon_type == remote_charge_weapon_type) {
        g_client_bot_state.firing.remote_charge_pending_detonation = true;
        g_client_bot_state.firing.remote_charge_pending_detonation_timer.set(
            kRemoteChargeDetonationWindowMs
        );
    }
    else if (fired_this_frame && rf::weapon_is_detonator(attack_weapon_type)) {
        g_client_bot_state.firing.remote_charge_pending_detonation = false;
        g_client_bot_state.firing.remote_charge_pending_detonation_timer.invalidate();
    }
    if (fired_this_frame
        && attack_weapon_type >= 0
        && weapon_uses_projectile_release_hold(attack_weapon_type)) {
        int hold_ms = get_weapon_projectile_release_hold_ms(
            attack_weapon_type,
            attack_use_alt_fire
        );
        hold_ms = std::max(
            hold_ms,
            get_active_impact_delay_remaining_ms(local_entity, attack_use_alt_fire)
        );
        if (hold_ms > 0) {
            g_client_bot_state.firing.explosive_release_hold_target = resolved_aim_target;
            g_client_bot_state.firing.explosive_release_hold_timer.set(hold_ms);
            g_client_bot_state.firing.explosive_release_hold_weapon = attack_weapon_type;
            g_client_bot_state.firing.explosive_release_hold_alt = attack_use_alt_fire;
        }
    }

    g_client_bot_state.firing.synthetic_primary_fire_down = next_primary_fire_down;
    g_client_bot_state.firing.synthetic_secondary_fire_down = next_secondary_fire_down;
    g_client_bot_state.firing.synthetic_primary_fire_just_pressed =
        fired_primary_this_frame;
    g_client_bot_state.firing.synthetic_secondary_fire_just_pressed =
        fired_secondary_this_frame;

    if (!next_primary_fire_down && !next_secondary_fire_down) {
        g_client_bot_state.firing.synthetic_primary_fire_just_pressed = false;
        g_client_bot_state.firing.synthetic_secondary_fire_just_pressed = false;
    }
}
