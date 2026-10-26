#include "bot_combat.h"

#include "bot_context.h"
#include "bot_decision_eval.h"
#include "bot_internal.h"
#include "bot_math.h"
#include "bot_utils.h"
#include "bot_weapon_profiles.h"
#include "../gametype.h"
#include "../multi.h"
#include "../../main/main.h"
#include "../../misc/waypoints.h"
#include "../../rf/collide.h"
#include "../../rf/item.h"
#include "../../rf/misc.h"
#include "../../rf/multi.h"
#include "../../rf/entity.h"
#include "../../rf/object.h"
#include "../../rf/physics.h"
#include "../../rf/os/frametime.h"
#include "../../rf/weapon.h"
#include <common/utils/list-utils.h>
#include <xlog/xlog.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <random>
#include <unordered_map>
#include <utility>
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
    uint32_t special_property_mask = 0;
};

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

bool bot_weapon_profile_has_special(const BotWeaponProfile& profile, const BotWeaponSpecialProperty property)
{
    return (profile.special_property_mask & special_property_bit(property)) != 0;
}

namespace
{
constexpr float kDegreesToRadians = 0.017453292519943295769f;
constexpr float kTargetHitRadius = 0.7f;
constexpr float kSplashHitRadiusScale = 0.5f;
constexpr float kAimErrorMissRadiusScale = 0.63f;
constexpr float kTargetEvadeSpeed = 2.5f;
constexpr float kThrownFlightTimeScale = 1.3f;
constexpr float kRangeFalloffStart = 0.85f;
constexpr float kMeleeMinReach = 1.5f;
constexpr float kMeleeBodyReach = 0.6f;
constexpr float kUnknownWeaponRange = 80.0f;
constexpr float kStatsReferenceDistance = 15.0f;
constexpr float kRemoteChargeDetonationDelaySeconds = 2.5f;
constexpr float kRemoteChargeMinThrowScale = 0.6f;
constexpr float kSplashSelfDamageBudget = 20.0f;
// Effective health a combat splash shot or detonation must leave the bot.
constexpr float kSplashSelfDamageMargin = 25.0f;
// A level hit this close to the aim point is the target's blast.
constexpr float kSplashTargetBurstSlack = 2.0f;
// Level geometry closer than this to a blast is what the blast lies on, not cover from it.
constexpr float kSplashCoverSlack = 1.0f;
// A target running at the bot faster than this share of the bot's run speed cannot be backed away from.
constexpr float kSplashTradeOutpaceScale = 1.2f;
// A bot with no way out trades only with the target this close; farther out it holds fire.
constexpr float kSplashTradeCorneredDistance = 5.0f;
// The weapons.tbl damage is the alt canister's, not the stream's.
constexpr float kFlameStreamDamagePerSecond = 120.0f;
constexpr float kFlameStreamReach = 9.0f;
constexpr float kMinShotIntervalMs = 50.0f;
constexpr float kMeleeCandidateReachShare = 0.75f;
constexpr float kSplashCandidateMaxRangeShare = 0.9f;
constexpr float kThrownStandoffMaxRangeShare = 0.95f;
constexpr float kThrownStandoffSafeScale = 1.1f;
constexpr int kWeaponRequestPendingMs = 250;
constexpr int kRangeSampleCount = 24;
constexpr float kRangeSampleMaxDistance = 80.0f;
// Shares of a weapon's best expected damage: where it is still worth firing, where it is at its best.
constexpr float kEffectiveRangeDpsShare = 0.15f;
constexpr float kIdealRangeDpsShare = 0.8f;
constexpr float kRangeBandDpsShare = 0.35f;
constexpr float kCraterStandoffScale = 1.5f;
constexpr float kLeadProjectileMaxSpeed = 100.0f;
constexpr float kFloorAimProbeDepth = 3.0f;
constexpr float kFloorAimHeight = 0.1f;
constexpr int kSplashClearanceTraceFlags = rf::CF_SKIP_SHOOT_THRU | rf::CF_PROCESS_INVISIBLE_FACES;
constexpr float kPreferredWeaponRankBonus = 0.4f;
constexpr float kAmmoFillUtilityExponent = 0.25f;
constexpr float kEmptyClipCloseFightUtilityScale = 0.35f;
constexpr float kEmptyClipUtilityScale = 0.85f;
constexpr float kQuirkWeaponUtilityScale = 3.0f;
constexpr float kSplashCandidateDistanceScale = 1.25f;
constexpr float kWeaponSwitchUtilityMargin = 1.25f;
constexpr float kCloseFightDistance = 15.0f;
constexpr int kWeaponSelectCadenceMs = 1000;
constexpr int kWeaponSelectDwellSlowMs = 900;
constexpr int kWeaponSelectDwellFastMs = 150;
constexpr int kTacticalReloadLosGapMs = 700;
constexpr float kTacticalReloadClipShare = 0.3f;
constexpr int kRemoteChargeThrowSettleMs = 400;
constexpr int kThrownClearanceChords = 3;
constexpr float kRemoteChargeEnemyRelevanceScale = 2.0f;
constexpr float kRemoteChargeEnemyConsideredScale = 4.0f;
constexpr float kDetonatorReturnDistanceScale = 1.25f;
constexpr float kThreatLookMinTravelDot = -0.17f;
constexpr float kCornerLookAheadDistance = 4.0f;
constexpr float kCornerLookMaxTurnDot = 0.82f;

struct BotWeaponStats
{
    int weapon_type = -1;
    bool melee = false;
    bool thrown = false; // gravity projectile, aimed on a ballistic arc
    bool remote_charge = false;
    float shot_damage = 0.0f; // all projectiles of one shot
    float shot_interval = 0.0f; // seconds between the bot's shots
    float sustain = 1.0f; // share of time spent firing rather than reloading
    float spread = 0.0f; // cone angle, radians
    float speed = 0.0f;
    float max_range = 0.0f;
    float splash_radius = 0.0f; // explosion only, flames excluded
    float min_safe_distance = 0.0f;
};

BotWeaponStats weapon_stats_for_mode(int weapon_type, bool use_alt_fire);
float bot_weapon_expected_dps(const BotWeaponStats& stats, float distance);

float splash_self_safe_distance(const float radius, const float damage)
{
    return damage > 0.0f ? radius * std::max(1.0f - kSplashSelfDamageBudget / damage, 0.0f) : 0.0f;
}

float bot_profile_weapon_selection_bonus(const int weapon_type)
{
    const BotWeaponProfile* profile = bot_weapon_profile_for_weapon_type(weapon_type);
    if (!profile) {
        return 0.0f;
    }

    float bonus = static_cast<float>(profile->value_tier) * 6.0f;
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

// Only certain weapons work underwater: handgun/pistol, remote charges, grenades,
// and riot stick (primary fire). Bots should not try to fire anything else.
bool weapon_works_underwater(int weapon_type)
{
    if (weapon_type < 0 || weapon_type >= rf::num_weapon_types) {
        return false;
    }

    struct UnderwaterWeaponEntry
    {
        const char* name = nullptr;
        int weapon_type = -2;
    };
    static std::array<UnderwaterWeaponEntry, 4> underwater_weapons{{
        {"12mm Handgun", -2},
        {"Remote Charge", -2},
        {"Grenade", -2},
        {"Riot Stick", -2},
    }};

    for (auto& entry : underwater_weapons) {
        if (entry.weapon_type == -2) {
            entry.weapon_type = rf::weapon_lookup_type(entry.name);
        }
        if (weapon_type == entry.weapon_type) {
            return true;
        }
    }
    return false;
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

void initialize_known_weapons_snapshot(const rf::Entity& entity)
{
    bot_state().known_weapons.fill(false);
    const int max_weapon = std::min(
        rf::num_weapon_types,
        static_cast<int>(bot_state().known_weapons.size())
    );
    for (int weapon_type = 0; weapon_type < max_weapon; ++weapon_type) {
        bot_state().known_weapons[weapon_type] = entity.ai.has_weapon[weapon_type];
    }
    bot_state().known_weapons_initialized = true;
}

std::vector<int> collect_newly_acquired_weapons(const rf::Entity& entity)
{
    std::vector<int> newly_acquired;
    const int max_weapon = std::min(
        rf::num_weapon_types,
        static_cast<int>(bot_state().known_weapons.size())
    );
    if (!bot_state().known_weapons_initialized) {
        initialize_known_weapons_snapshot(entity);
        return newly_acquired;
    }

    for (int weapon_type = 0; weapon_type < max_weapon; ++weapon_type) {
        const bool has_weapon_now = entity.ai.has_weapon[weapon_type];
        const bool had_weapon_before = bot_state().known_weapons[weapon_type];
        if (has_weapon_now && !had_weapon_before) {
            newly_acquired.push_back(weapon_type);
        }
        bot_state().known_weapons[weapon_type] = has_weapon_now;
    }
    return newly_acquired;
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

bool weapon_request_pending(const int weapon_type)
{
    const BotCombatFiringState& firing = bot_state().firing;
    return firing.requested_weapon == weapon_type
        && firing.weapon_request_timer.valid()
        && !firing.weapon_request_timer.elapsed();
}

bool switch_to_weapon(
    rf::Player& player,
    rf::Entity& entity,
    const int weapon_type)
{
    if (weapon_type != entity.ai.current_primary_weapon && weapon_request_pending(weapon_type)) {
        return true;
    }
    if (bot_backend_weapon_busy(player, entity)) {
        return false;
    }
    const int current_weapon = entity.ai.current_primary_weapon;
    const bool throw_leaving_hand = std::any_of(
        std::begin(entity.ai.create_weapon_delay_timestamps),
        std::end(entity.ai.create_weapon_delay_timestamps),
        [](const rf::Timestamp& delay) { return delay.valid() && !delay.elapsed(); });
    const bool current_clip_empty =
        current_weapon >= 0
        && current_weapon < rf::num_weapon_types
        && rf::weapon_uses_clip(current_weapon)
        && entity.ai.clip_ammo[current_weapon] <= 0
        && !throw_leaving_hand;
    if (bot_weapon_fire_wait_is_blocking_switch(entity) && !current_clip_empty) {
        return false;
    }

    if (weapon_type < 0
        || weapon_type >= rf::num_weapon_types
        || weapon_type == current_weapon) {
        return false;
    }
    if (bot_weapon_is_switch_blocked(weapon_type) || multi_is_weapon_switch_locked(current_weapon, weapon_type)) {
        return false;
    }

    bot_backend_switch_weapon(player, entity, weapon_type);
    bot_state().firing.wants_fire = false;
    bot_state().fire_decision_timer.invalidate();
    bot_state().firing.explosive_fire_delay_timer.invalidate();
    bot_state().firing.explosive_fire_delay_weapon = -1;
    bot_state().firing.explosive_fire_delay_alt = false;
    bot_state().firing.explosive_release_hold_timer.invalidate();
    bot_state().firing.explosive_release_hold_target = {};
    bot_state().firing.explosive_release_hold_weapon = -1;
    bot_state().firing.explosive_release_hold_alt = false;
    clear_semi_auto_click_state();
    bot_state().firing.weapon_select_candidate = -1;
    bot_state().firing.requested_weapon = weapon_type;
    bot_state().firing.weapon_request_timer.set(kWeaponRequestPendingMs);
    bot_state().weapon_switch_timer.set(get_weapon_switch_cooldown_ms());
    return true;
}

float semi_auto_click_base_interval_ms(const int weapon_type, const float skill_factor)
{
    int fire_wait_ms = rf::weapon_get_fire_wait_ms(weapon_type, false);
    if (fire_wait_ms <= 0) {
        fire_wait_ms = 120;
    }
    const float skill_scale = std::lerp(1.18f, 0.92f, skill_factor);
    return static_cast<float>(fire_wait_ms)
        * std::max(get_active_bot_skill_profile().semi_auto_click_interval_scale, 0.0f)
        * skill_scale;
}

bool should_press_semi_auto_trigger(const int weapon_type, const float skill_factor)
{
    if (!rf::weapon_is_semi_automatic(weapon_type)) {
        return true;
    }

    if (weapon_type != bot_state().firing.semi_auto_click_weapon) {
        bot_state().firing.semi_auto_click_weapon = weapon_type;
        bot_state().firing.semi_auto_click_timer.invalidate();
    }

    if (bot_state().firing.semi_auto_click_timer.valid()
        && !bot_state().firing.semi_auto_click_timer.elapsed()) {
        return false;
    }

    const float base_interval = semi_auto_click_base_interval_ms(weapon_type, skill_factor);
    const float variance = std::lerp(0.24f, 0.10f, skill_factor);
    std::uniform_real_distribution<float> jitter_dist(-variance, variance);
    const float jittered_interval = base_interval * (1.0f + jitter_dist(g_rng));
    const int click_interval_ms = std::clamp(
        static_cast<int>(std::lround(jittered_interval)),
        kSemiAutoClickMinMs,
        kSemiAutoClickMaxMs
    );
    bot_state().firing.semi_auto_click_timer.set(click_interval_ms);
    return true;
}

float compute_weapon_skill_adjusted_factor(const float base_skill, const int weapon_type)
{
    const float weapon_weight = std::clamp(bot_get_weapon_skill_weight(weapon_type), 0.35f, 1.65f);
    return std::clamp(base_skill * weapon_weight, 0.0f, 1.0f);
}

float aim_error_bound(const float distance, const float skill_factor)
{
    const float distance_scale = std::clamp(distance / kAimErrorDistanceNorm, 0.65f, 2.6f);
    const BotSkillProfile& skill_profile = get_active_bot_skill_profile();
    const float min_error = std::clamp(skill_profile.aim_error_min, 0.0f, 5.0f);
    return std::clamp(
        std::lerp(std::max(skill_profile.aim_error_max, min_error), min_error, skill_factor) * distance_scale,
        min_error,
        5.0f
    );
}

rf::Vector3 ballistic_aim_point(const rf::Vector3& from, const rf::Vector3& target, const float speed)
{
    rf::Vector3 horizontal = target - from;
    const float rise = horizontal.y;
    horizontal.y = 0.0f;
    const float range = horizontal.len();
    if (range < 0.01f || speed <= 1.0f || rf::gravity <= 0.0f) {
        return target;
    }
    horizontal.normalize_safe();
    const float speed_sq = speed * speed;
    const float discriminant =
        speed_sq * speed_sq - rf::gravity * (rf::gravity * range * range + 2.0f * rise * speed_sq);
    const float tan_pitch = discriminant < 0.0f
        ? 1.0f
        : (speed_sq - std::sqrt(discriminant)) / (rf::gravity * range);
    return from + horizontal * range + rf::Vector3{0.0f, range * tan_pitch, 0.0f};
}

// Kept apart from BotWeaponStats because the aim reads it every frame.
struct ProjectileTraits
{
    bool melee = false;
    bool flame = false;
    bool thrown = false;
    float speed = 0.0f;
    float splash_radius = 0.0f; // explosion only, flames excluded
};

ProjectileTraits projectile_traits(const int weapon_type)
{
    ProjectileTraits traits{};
    if (weapon_type < 0 || weapon_type >= rf::num_weapon_types || rf::weapon_is_detonator(weapon_type)) {
        return traits;
    }
    const rf::WeaponInfo& info = rf::weapon_types[weapon_type];
    traits.melee = rf::weapon_is_melee(weapon_type);
    traits.flame = rf::weapon_is_flamethrower(weapon_type) || (info.flags2 & rf::WTF2_FLAME) != 0;
    traits.thrown = !traits.melee && !traits.flame && (info.flags & rf::WTF_GRAVITY) != 0;
    traits.speed = traits.flame ? 0.0f : std::max(info.max_speed, 0.0f);
    if (info.damage_radius > 0.0f && !traits.flame) {
        traits.splash_radius = info.damage_radius;
    }
    return traits;
}

rf::Vector3 compute_skill_adjusted_enemy_aim_target(
    const rf::Entity& local_entity,
    const rf::Entity& enemy_target,
    const float skill_factor)
{
    const ProjectileTraits projectile = projectile_traits(local_entity.ai.current_primary_weapon);
    rf::Vector3 aim_target = enemy_target.eye_pos;
    aim_target.y = std::lerp(
        enemy_target.pos.y,
        enemy_target.eye_pos.y,
        kEnemyAimHeightBlend
    );
    if (projectile.splash_radius > 0.0f
        && !projectile.thrown
        && bot_state().firing.aim_at_floor
        && rf::entity_on_ground(const_cast<rf::Entity*>(&enemy_target))) {
        aim_target = enemy_target.pos;
        aim_target.y -= bot_state().firing.aim_floor_drop;
    }

    rf::Vector3 to_target = aim_target - local_entity.eye_pos;
    const float dist = to_target.len();
    if (dist <= 0.0001f) {
        return aim_target;
    }

    if (!projectile.melee && projectile.speed > 1.0f && projectile.speed < kLeadProjectileMaxSpeed) {
        const float flight_time = dist / projectile.speed * (projectile.thrown ? kThrownFlightTimeScale : 1.0f);
        const float lead_time = std::min(flight_time, 1.5f) * std::clamp(skill_factor, 0.15f, 1.0f);
        aim_target += enemy_target.p_data.vel * lead_time;
    }

    if (bot_state().firing.aim_error_target_handle != enemy_target.handle) {
        bot_state().firing.aim_error_target_handle = enemy_target.handle;
        bot_state().firing.aim_error_timer.invalidate();
    }

    if (!bot_state().firing.aim_error_timer.valid()
        || bot_state().firing.aim_error_timer.elapsed()) {
        const float max_error = aim_error_bound(dist, skill_factor);
        const float vertical_max_error = max_error * 0.82f;
        bot_state().firing.aim_error_right = 0.0f;
        bot_state().firing.aim_error_up = 0.0f;

        // A normal distribution needs a positive spread.
        if (max_error > 0.0f) {
            std::normal_distribution<float> lateral_dist(0.0f, max_error * 0.42f);
            std::normal_distribution<float> vertical_dist(0.0f, vertical_max_error * 0.44f);
            bot_state().firing.aim_error_right = std::clamp(
                lateral_dist(g_rng),
                -max_error,
                max_error
            );
            bot_state().firing.aim_error_up = std::clamp(
                vertical_dist(g_rng),
                -vertical_max_error,
                vertical_max_error
            );
        }

        const float low_skill_factor = 1.0f - skill_factor;
        std::uniform_real_distribution<float> roll_dist(0.0f, 1.0f);
        if (max_error > 0.0f && roll_dist(g_rng) < low_skill_factor * 0.20f) {
            std::uniform_real_distribution<float> extra_dist(-max_error, max_error);
            bot_state().firing.aim_error_right = std::clamp(
                bot_state().firing.aim_error_right + extra_dist(g_rng) * 0.45f,
                -max_error,
                max_error
            );
            bot_state().firing.aim_error_up = std::clamp(
                bot_state().firing.aim_error_up + extra_dist(g_rng) * 0.35f,
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
        bot_state().firing.aim_error_timer.set(update_ms);
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

    aim_target += right * bot_state().firing.aim_error_right;
    aim_target += up * bot_state().firing.aim_error_up;
    if (projectile.thrown) {
        aim_target = ballistic_aim_point(local_entity.eye_pos, aim_target, projectile.speed);
    }
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
        bot_state().active_goal == BotGoalType::eliminate_target
        || bot_state().fsm_state == BotFsmState::engage_enemy
        || bot_state().fsm_state == BotFsmState::pursue_enemy
        || bot_state().fsm_state == BotFsmState::seek_enemy;
    const float tuned_alpha = std::clamp(
        combat_tracking
            ? std::lerp(0.14f, 0.32f, skill_factor)
            : std::lerp(0.10f, 0.24f, skill_factor),
        0.05f,
        0.40f
    );
    // The blend constants are per 30 fps frame; rescaling the exponent keeps convergence time independent of the host
    // frame rate.
    constexpr float kAimBlendTunedRate = 30.0f;
    const float blend_dt = std::clamp(rf::frametime, 0.0f, 0.05f);
    const float interp_alpha = 1.0f - std::pow(1.0f - tuned_alpha, blend_dt * kAimBlendTunedRate);
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

    // One step per frame: server bots also step on the frames between their thinks (bot_track_aim_goal).
    const float frame_dt = std::clamp(rf::frametime, 0.001f, 0.05f);
    const BotSkillProfile& skill_profile = get_active_bot_skill_profile();
    const float yaw_speed = std::lerp(1.8f, std::max(skill_profile.yaw_speed_cap, 1.8f), skill_factor);
    const float pitch_speed = std::lerp(1.3f, std::max(skill_profile.pitch_speed_cap, 1.3f), skill_factor);

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
    const float required_alignment =
        std::lerp(0.985f, std::clamp(skill_profile.fire_alignment_threshold, -1.0f, 0.999f), skill_factor);
    return out_alignment >= required_alignment;
}

// Holding the charges is enough: GunGame grants no detonator.
bool owns_remote_charge_detonator(const rf::Entity& entity)
{
    return bot_entity_has_weapon_type(entity, rf::remote_charge_det_weapon_type)
        || (rf::remote_charge_det_weapon_type >= 0
            && bot_entity_has_weapon_type(entity, rf::remote_charge_weapon_type));
}

bool weapon_uses_projectile_release_hold(const int weapon_type, const bool alt_fire)
{
    return multi_weapon_fire_is_thrown(weapon_type, alt_fire);
}

bool decide_fire(
    const float skill_factor,
    const bool has_aim_solution,
    const float dist_sq,
    const float range)
{
    if (!has_aim_solution || range <= 0.0f) {
        bot_state().firing.wants_fire = false;
        bot_state().fire_decision_timer.invalidate();
        return false;
    }

    if (!bot_state().fire_decision_timer.valid()
        || bot_state().fire_decision_timer.elapsed()) {
        const float distance = std::sqrt(std::max(dist_sq, 0.0f));
        const float distance_ratio = distance / std::max(range, 1.0f);
        const float distance_factor = 1.0f - std::clamp(distance_ratio, 0.0f, 1.0f);
        const float over_range_ratio = std::max(distance_ratio - 1.0f, 0.0f);
        const float over_range_penalty = 1.0f / (1.0f + over_range_ratio * 1.5f);

        float fire_chance = (0.08f + skill_factor * 0.82f + distance_factor * 0.25f)
            * over_range_penalty;
        fire_chance = std::clamp(fire_chance, 0.01f, 1.0f);

        std::uniform_real_distribution<float> roll_dist(0.0f, 1.0f);
        bot_state().firing.wants_fire = roll_dist(g_rng) <= fire_chance;

        const int fire_eval_interval_ms = static_cast<int>(
            std::lerp(260.0f, 45.0f, skill_factor)
        );
        bot_state().fire_decision_timer.set(fire_eval_interval_ms);
    }

    return bot_state().firing.wants_fire;
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

    if (!alt_mode_locked
        && bot_weapon_profile_has_special(*profile, BotWeaponSpecialProperty::alt_fire_for_long_range)) {
        mode.use_alt_fire = bot_weapon_expected_dps(weapon_stats_for_mode(weapon_type, true), distance)
            > bot_weapon_expected_dps(weapon_stats_for_mode(weapon_type, false), distance);
    }

    if (!alt_mode_locked
        && profile->primary_fire == BotWeaponFireMode::throw_timed_detonation
        && profile->alt_fire == BotWeaponFireMode::throw_impact_detonation) {
        // A fuse gives a moving target time to leave; impact detonation lands where it was aimed.
        mode.use_alt_fire = true;
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
        bot_state().auto_fire_release_guard_timer.set(350);
        bot_state().firing.wants_fire = true;
        bot_state().fire_decision_timer.invalidate();
        return true;
    }

    if (bot_state().auto_fire_release_guard_timer.valid()
        && !bot_state().auto_fire_release_guard_timer.elapsed()) {
        bot_state().firing.wants_fire = true;
        bot_state().fire_decision_timer.invalidate();
        return true;
    }

    bot_state().auto_fire_release_guard_timer.invalidate();
    bot_state().firing.wants_fire = false;
    bot_state().fire_decision_timer.invalidate();
    return false;
}

BotWeaponStats weapon_stats_for_mode(const int weapon_type, const bool use_alt_fire)
{
    BotWeaponStats stats{};
    if (weapon_type < 0 || weapon_type >= rf::num_weapon_types || rf::weapon_is_detonator(weapon_type)) {
        return stats;
    }
    const rf::WeaponInfo& info = rf::weapon_types[weapon_type];
    const ProjectileTraits projectile = projectile_traits(weapon_type);
    stats.weapon_type = weapon_type;
    stats.melee = projectile.melee;
    stats.remote_charge = (info.flags & rf::WTF_REMOTE_CHARGE) != 0;
    stats.thrown = projectile.thrown;

    const float damage = use_alt_fire && info.alt_damage_multi > 0.0f ? info.alt_damage_multi : info.damage_multi;
    stats.shot_damage = std::max(damage, 0.0f) * static_cast<float>(std::max(info.num_projectiles, 1));
    float interval_ms = std::max(
        static_cast<float>(rf::weapon_get_fire_wait_ms(weapon_type, use_alt_fire)),
        kMinShotIntervalMs);
    if (rf::weapon_is_semi_automatic(weapon_type)) {
        const float skill = compute_weapon_skill_adjusted_factor(bot_get_skill_factor(), weapon_type);
        interval_ms = std::max(interval_ms, std::clamp(
            semi_auto_click_base_interval_ms(weapon_type, skill),
            static_cast<float>(kSemiAutoClickMinMs),
            static_cast<float>(kSemiAutoClickMaxMs)));
    }
    stats.shot_interval = interval_ms / 1000.0f;
    if (projectile.flame) {
        stats.shot_damage = kFlameStreamDamagePerSecond * stats.shot_interval;
    }
    if (stats.remote_charge) {
        stats.shot_interval += kRemoteChargeDetonationDelaySeconds;
    }
    if (rf::weapon_uses_clip(weapon_type) && info.clip_size > 0 && info.clip_reload_time_secs > 0.0f) {
        const float clip_fire_time = static_cast<float>(info.clip_size) * stats.shot_interval;
        stats.sustain = clip_fire_time / (clip_fire_time + info.clip_reload_time_secs);
    }

    const float spread_degrees = use_alt_fire ? info.alt_spread_degrees : info.spread_degrees;
    stats.spread = std::max(spread_degrees, 0.0f) * kDegreesToRadians;
    stats.speed = projectile.speed;
    if (stats.melee) {
        stats.max_range = std::max(stats.speed * info.lifetime_seconds, kMeleeMinReach) + kMeleeBodyReach;
    }
    else if (projectile.flame) {
        stats.max_range = kFlameStreamReach;
    }
    else {
        stats.max_range = stats.speed * info.lifetime_seconds;
        if (stats.max_range <= 0.0f) {
            stats.max_range = kUnknownWeaponRange;
        }
        if (stats.thrown && rf::gravity > 0.0f) {
            stats.max_range = std::min(stats.max_range, stats.speed * stats.speed / rf::gravity);
        }
    }
    if (projectile.splash_radius > 0.0f) {
        stats.splash_radius = projectile.splash_radius;
        // A remote charge's blast is checked when it is set off; it is only thrown far enough to back off from.
        stats.min_safe_distance = stats.remote_charge
            ? stats.splash_radius * kRemoteChargeMinThrowScale
            : splash_self_safe_distance(stats.splash_radius, stats.shot_damage);
    }
    return stats;
}

BotWeaponStats bot_weapon_stats(const int weapon_type, const float distance)
{
    return weapon_stats_for_mode(weapon_type, select_combat_fire_mode(weapon_type, distance).use_alt_fire);
}

float bot_weapon_expected_dps(const BotWeaponStats& stats, const float distance)
{
    if (stats.weapon_type < 0
        || stats.shot_interval <= 0.0f
        || stats.shot_damage <= 0.0f
        || distance > stats.max_range
        || distance < stats.min_safe_distance) {
        return 0.0f;
    }
    float hit_chance = 1.0f;
    if (!stats.melee) {
        const float skill = compute_weapon_skill_adjusted_factor(bot_get_skill_factor(), stats.weapon_type);
        const float spread_miss = distance * std::tan(stats.spread * 0.5f);
        const float aim_miss = aim_error_bound(distance, skill) * kAimErrorMissRadiusScale;
        const float flight_time = stats.speed > 1.0f
            ? distance / stats.speed * (stats.thrown ? kThrownFlightTimeScale : 1.0f)
            : 0.0f;
        const float evade_miss = flight_time * kTargetEvadeSpeed;
        const float miss_sq = spread_miss * spread_miss + aim_miss * aim_miss + evade_miss * evade_miss;
        const float hit_radius = kTargetHitRadius + stats.splash_radius * kSplashHitRadiusScale;
        if (miss_sq > hit_radius * hit_radius) {
            hit_chance = hit_radius * hit_radius / miss_sq;
        }
    }
    const float falloff_start = stats.max_range * kRangeFalloffStart;
    if (distance > falloff_start) {
        hit_chance *= (stats.max_range - distance) / std::max(stats.max_range - falloff_start, 0.01f);
    }
    return stats.shot_damage * hit_chance * stats.sustain / stats.shot_interval;
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

// The band between a throw's blast distance and its reach can be empty.
bool crater_standoff_reachable(const BotWeaponStats& stats)
{
    return !stats.thrown
        || stats.remote_charge
        || stats.min_safe_distance * kThrownStandoffSafeScale < stats.max_range * kThrownStandoffMaxRangeShare;
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
            || !bot_weapon_profile_has_special(*profile, BotWeaponSpecialProperty::creates_craters)
            || !crater_standoff_reachable(weapon_stats_for_mode(weapon_type, false))) {
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
        bot_state().firing.explosive_fire_delay_timer.invalidate();
        bot_state().firing.explosive_fire_delay_weapon = -1;
        bot_state().firing.explosive_fire_delay_alt = false;
        return wants_attack;
    }

    if (!wants_attack || !weapon_can_create_craters(weapon_type)) {
        bot_state().firing.explosive_fire_delay_timer.invalidate();
        bot_state().firing.explosive_fire_delay_weapon = -1;
        bot_state().firing.explosive_fire_delay_alt = false;
        return wants_attack;
    }

    const bool fire_already_down = use_alt_fire
        ? bot_state().firing.synthetic_secondary_fire_down
        : bot_state().firing.synthetic_primary_fire_down;
    if (fire_already_down) {
        return true;
    }

    const bool same_request =
        bot_state().firing.explosive_fire_delay_weapon == weapon_type
        && bot_state().firing.explosive_fire_delay_alt == use_alt_fire;
    if (!same_request || !bot_state().firing.explosive_fire_delay_timer.valid()) {
        bot_state().firing.explosive_fire_delay_weapon = weapon_type;
        bot_state().firing.explosive_fire_delay_alt = use_alt_fire;
        bot_state().firing.explosive_fire_delay_timer.set(25);
        return false;
    }
    if (!bot_state().firing.explosive_fire_delay_timer.elapsed()) {
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
    if (bot_state().active_goal != BotGoalType::create_crater) {
        return;
    }
    if (bot_backend_weapon_busy(local_player, local_entity)) {
        return;
    }
    if (bot_weapon_fire_wait_is_blocking_switch(local_entity)) {
        return;
    }
    if (bot_state().weapon_switch_timer.valid()
        && !bot_state().weapon_switch_timer.elapsed()) {
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
    if (bot_state().active_goal != BotGoalType::shatter_glass) {
        return;
    }
    if (bot_backend_weapon_busy(local_player, local_entity)) {
        return;
    }
    if (bot_weapon_fire_wait_is_blocking_switch(local_entity)) {
        return;
    }
    if (bot_state().weapon_switch_timer.valid()
        && !bot_state().weapon_switch_timer.elapsed()) {
        return;
    }

    const int shatter_weapon = find_best_shatter_weapon(local_player, local_entity);
    if (shatter_weapon >= 0 && shatter_weapon != local_entity.ai.current_primary_weapon) {
        switch_to_weapon(local_player, local_entity, shatter_weapon);
    }
}

bool crater_target_has_direct_los(const rf::Entity& local_entity, const rf::Vector3& target_pos)
{
    // The target sits on a surface, so test a point just in front of it.
    rf::Vector3 to_eye = local_entity.eye_pos - target_pos;
    to_eye.normalize_safe();
    return bot_has_unobstructed_level_los(
        local_entity.eye_pos,
        target_pos + to_eye * 0.6f,
        &local_entity,
        nullptr);
}

float nearest_player_entity_distance(
    const rf::Entity& local_entity,
    const rf::Vector3& point,
    const float radius,
    const bool teammates,
    const bool require_los)
{
    float nearest = std::numeric_limits<float>::infinity();
    const rf::Player* const local_player = bot_player();
    const bool team_mode = multi_is_team_game_type();
    if (!local_player || (teammates && !team_mode)) {
        return nearest;
    }
    float nearest_sq = radius * radius;
    for (const rf::Player& player : SinglyLinkedList{rf::player_list}) {
        if (&player == local_player || player.is_spectator) {
            continue;
        }
        if ((team_mode && player.team == local_player->team) != teammates) {
            continue;
        }
        rf::Entity* const entity = rf::entity_from_handle(player.entity_handle);
        if (!entity || entity == &local_entity || rf::entity_is_dying(entity)) {
            continue;
        }
        const float dist_sq = rf::vec_dist_squared(&entity->pos, &point);
        if (dist_sq <= nearest_sq
            && (!require_los
                || bot_has_unobstructed_level_los(local_entity.eye_pos, entity->eye_pos, &local_entity, entity))) {
            nearest_sq = dist_sq;
            nearest = std::sqrt(dist_sq);
        }
    }
    return nearest;
}

bool player_entity_within_radius(
    const rf::Entity& local_entity,
    const rf::Vector3& point,
    const float radius,
    const bool teammates,
    const bool require_los = false)
{
    return nearest_player_entity_distance(local_entity, point, radius, teammates, require_los) <= radius;
}

// Mirrors the engine's area damage, cover included.
float splash_damage_to_entity(
    const rf::Entity& entity,
    const float damage,
    const float radius,
    const rf::Vector3& blast_pos,
    const bool check_los = true)
{
    const float distance = std::sqrt(rf::vec_dist_squared(&entity.pos, &blast_pos));
    if (damage <= 0.0f || radius <= 0.0f || distance >= radius) {
        return 0.0f;
    }
    if (check_los) {
        rf::Vector3 p0 = entity.p_data.pos;
        rf::Vector3 p1 = blast_pos;
        rf::PCollisionOut collision{};
        collision.obj_handle = -1;
        if (rf::collide_linesegment_world(p0, p1, rf::CF_SKIP_SHOOT_THRU, &collision)
            && rf::vec_dist_squared(&collision.hit_point, &blast_pos) > kSplashCoverSlack * kSplashCoverSlack) {
            return 0.0f;
        }
    }
    return damage * (1.0f - distance / radius);
}

// Mirrors the engine's Damage Amplifier scaling of area damage, which Bagman leaves out.
float splash_self_damage_scale(const rf::Entity& entity)
{
    return (entity.entity_flags2 & rf::EF2_POWERUP_DAMAGE_AMP) && !gt_is_bagman_any()
        ? rf::g_multi_damage_modifier
        : 1.0f;
}

float entity_effective_health(const rf::Entity& entity)
{
    return std::max(entity.life, 0.0f) + 2.0f * std::max(entity.armor, 0.0f);
}

bool splash_self_damage_acceptable(const rf::Entity& entity, const float self_damage, const bool combat)
{
    return combat
        ? self_damage < entity_effective_health(entity) - kSplashSelfDamageMargin
        : self_damage <= kSplashSelfDamageBudget;
}

// Muzzle and current view, not the eye-to-target LOS test: that skips glass, and the bot may still be turning.
bool splash_level_burst(
    const rf::Entity& local_entity,
    const BotWeaponStats& stats,
    const rf::Vector3& impact_pos,
    rf::Vector3& out_burst_pos)
{
    rf::Vector3 p0{};
    rf::Matrix3 muzzle_orient{};
    rf::entity_get_weapon_fire_pos_orient(
        const_cast<rf::Entity*>(&local_entity),
        stats.weapon_type,
        &p0,
        &muzzle_orient,
        local_entity.ai.next_fire_primary_slot,
        true);
    const rf::Vector3 dir = forward_from_non_linear_yaw_pitch(
        local_entity.control_data.phb.y,
        local_entity.control_data.eye_phb.x);
    rf::GCollisionOutput collision{};
    bool hit = false;
    const float horizontal_speed = stats.speed * std::sqrt(dir.x * dir.x + dir.z * dir.z);
    if (!stats.thrown || horizontal_speed < 1.0f || rf::gravity <= 0.0f) {
        const float reach = std::min(
            stats.splash_radius,
            std::sqrt(rf::vec_dist_squared(&p0, &impact_pos)) + kSplashTargetBurstSlack);
        rf::Vector3 p1 = p0 + dir * reach;
        hit = rf::collide_linesegment_level_solid(p0, p1, kSplashClearanceTraceFlags, &collision);
    }
    else {
        const float reach = std::min(stats.splash_radius, std::sqrt(bot_horizontal_dist_sq(p0, impact_pos)));
        const float end_time = reach / horizontal_speed;
        rf::Vector3 chord_start = p0;
        for (int chord = 1; chord <= kThrownClearanceChords && !hit; ++chord) {
            const float time = end_time * static_cast<float>(chord) / static_cast<float>(kThrownClearanceChords);
            rf::Vector3 chord_end = p0 + dir * (stats.speed * time);
            chord_end.y -= 0.5f * rf::gravity * time * time;
            rf::Vector3 trace_start = chord_start;
            hit = rf::collide_linesegment_level_solid(trace_start, chord_end, kSplashClearanceTraceFlags, &collision);
            chord_start = chord_end;
        }
    }
    if (hit) {
        out_burst_pos = collision.hit_point;
    }
    return hit;
}

bool weapon_is_combat_candidate(rf::Player& player, rf::Entity& entity, int weapon_type);

// The last resort of a bot that cannot open the distance to fire its only weapon safely.
bool splash_trade_allowed(
    rf::Player& local_player,
    rf::Entity& local_entity,
    const BotWeaponStats& stats,
    const rf::Entity& target)
{
    if (stats.thrown) {
        return false;
    }
    rf::Vector3 away = local_entity.pos - target.pos;
    away.y = 0.0f;
    const float distance = away.len();
    away.normalize_safe();
    const rf::Vector3 target_vel{target.p_data.vel.x, 0.0f, target.p_data.vel.z};
    const bool outpaced = rf::entity_on_ground(const_cast<rf::Entity*>(&target))
        && target_vel.dot_prod(away) > local_entity.max_vel * kSplashTradeOutpaceScale;
    const rf::Timestamp& blocked = bot_state().splash_retreat_blocked_timer;
    const bool cornered = blocked.valid() && !blocked.elapsed() && distance <= kSplashTradeCorneredDistance;
    if (!cornered && !outpaced) {
        return false;
    }
    const float target_damage =
        stats.shot_damage * splash_self_damage_scale(local_entity) * (1.0f - kTargetHitRadius / stats.splash_radius);
    if (target_damage < entity_effective_health(target)) {
        return false;
    }
    for (int weapon_type = 0; weapon_type < rf::num_weapon_types; ++weapon_type) {
        if (weapon_type != stats.weapon_type && weapon_is_combat_candidate(local_player, local_entity, weapon_type)) {
            return false;
        }
    }
    return true;
}

// The bot sees the target, so no cover from the blast is assumed.
bool splash_fire_is_safe(
    rf::Player& local_player,
    rf::Entity& local_entity,
    const BotWeaponStats& stats,
    const rf::Vector3& impact_pos,
    const rf::Entity* target)
{
    if (stats.splash_radius <= 0.0f || stats.remote_charge) {
        return true;
    }
    const float radius = stats.splash_radius;
    if (player_entity_within_radius(local_entity, impact_pos, radius, true)) {
        return false;
    }
    const float damage = stats.shot_damage * splash_self_damage_scale(local_entity);
    float self_damage = 0.0f;
    bool blast_at_target = target != nullptr;
    rf::Vector3 burst_pos{};
    if (splash_level_burst(local_entity, stats, impact_pos, burst_pos)) {
        if (player_entity_within_radius(local_entity, burst_pos, radius, true)) {
            return false;
        }
        self_damage = splash_damage_to_entity(local_entity, damage, radius, burst_pos, false);
        blast_at_target = blast_at_target
            && rf::vec_dist_squared(&burst_pos, &impact_pos) <= kSplashTargetBurstSlack * kSplashTargetBurstSlack;
    }
    else if (!target) {
        self_damage = splash_damage_to_entity(local_entity, damage, radius, impact_pos, false);
    }
    const float target_distance = target
        ? std::sqrt(rf::vec_dist_squared(&local_entity.pos, &target->pos))
        : std::numeric_limits<float>::infinity();
    if (target) {
        const rf::Vector3 to_target = target->pos - local_entity.pos;
        const float closing_speed = (local_entity.p_data.vel - target->p_data.vel).dot_prod(to_target)
            / std::max(target_distance, 0.01f);
        const float flight_time = stats.speed > 1.0f
            ? target_distance / stats.speed * (stats.thrown ? kThrownFlightTimeScale : 1.0f)
            : 0.0f;
        const float closed_distance =
            std::max(target_distance - target->radius - std::max(closing_speed, 0.0f) * flight_time, 0.0f);
        const rf::Vector3 closed_pos =
            local_entity.pos + to_target * (closed_distance / std::max(target_distance, 0.01f));
        if (player_entity_within_radius(local_entity, closed_pos, radius, true)) {
            return false;
        }
        self_damage = std::max(self_damage, damage * (1.0f - std::min(closed_distance / radius, 1.0f)));
    }
    const float nearest_enemy = nearest_player_entity_distance(local_entity, local_entity.pos, radius, false, true);
    if (nearest_enemy < target_distance) {
        self_damage = std::max(self_damage, damage * (1.0f - nearest_enemy / radius));
        blast_at_target = false;
    }
    return splash_self_damage_acceptable(local_entity, self_damage, target != nullptr)
        || (blast_at_target && splash_trade_allowed(local_player, local_entity, stats, *target));
}

bool remote_charge_throw_in_progress()
{
    const BotCombatFiringState& firing = bot_state().firing;
    return firing.explosive_release_hold_weapon == rf::remote_charge_weapon_type
        && firing.explosive_release_hold_timer.valid()
        && !firing.explosive_release_hold_timer.elapsed();
}

constexpr int kMaxTrackedRemoteCharges = 8;

struct LiveRemoteCharges
{
    std::array<const rf::Weapon*, kMaxTrackedRemoteCharges> charges{};
    int count = 0;
    int total = 0; // also the charges past the array
};

LiveRemoteCharges find_live_remote_charges(const rf::Entity& local_entity);

bool remote_charge_settling()
{
    const rf::Timestamp& settle = bot_state().firing.remote_charge_settle_timer;
    return remote_charge_throw_in_progress() || (settle.valid() && !settle.elapsed());
}

bool remote_charges_given_up()
{
    const rf::Timestamp& give_up = bot_state().firing.remote_charge_give_up_timer;
    return give_up.valid() && give_up.elapsed();
}

bool remote_charge_already_out()
{
    // More charges than a detonation can judge are never added to, given up or not.
    const int live = bot_state().firing.remote_charges_live;
    return bot_state().firing.remote_charge_pending_detonation
        || live > kMaxTrackedRemoteCharges
        || (!remote_charges_given_up() && live > 0);
}

LiveRemoteCharges find_live_remote_charges(const rf::Entity& local_entity)
{
    LiveRemoteCharges live{};
    for (rf::Object* obj = rf::object_list.next_obj; obj != &rf::object_list; obj = obj->next_obj) {
        if (obj->type != rf::OT_WEAPON || obj->parent_handle != local_entity.handle) {
            continue;
        }
        const rf::Weapon* const weapon = static_cast<const rf::Weapon*>(obj);
        if (weapon->info && (weapon->info->flags & rf::WTF_REMOTE_CHARGE)) {
            if (live.count < static_cast<int>(live.charges.size())) {
                live.charges[live.count++] = weapon;
            }
            ++live.total;
        }
    }
    return live;
}

bool enemy_takes_splash(
    const rf::Entity& local_entity,
    const float damage,
    const float radius,
    const rf::Vector3& blast_pos)
{
    const rf::Player* const local_player = bot_player();
    if (!local_player) {
        return false;
    }
    const bool team_mode = multi_is_team_game_type();
    for (const rf::Player& player : SinglyLinkedList{rf::player_list}) {
        if (&player == local_player || player.is_spectator || (team_mode && player.team == local_player->team)) {
            continue;
        }
        rf::Entity* const entity = rf::entity_from_handle(player.entity_handle);
        if (entity
            && entity != &local_entity
            && !rf::entity_is_dying(entity)
            && splash_damage_to_entity(*entity, damage, radius, blast_pos) > 0.0f) {
            return true;
        }
    }
    return false;
}

bool should_detonate_remote_charges(
    const rf::Entity& local_entity,
    const LiveRemoteCharges& live,
    const bool crater_goal,
    const bool enemies_leaving)
{
    // Charges past the tracked ones cannot be judged.
    if (live.count == 0 || live.total > live.count) {
        return false;
    }
    bool enemy_in_blast = false;
    float self_damage = 0.0f;
    const float damage_scale = splash_self_damage_scale(local_entity);
    for (int i = 0; i < live.count; ++i) {
        const rf::Vector3& charge_pos = live.charges[i]->pos;
        const float radius = live.charges[i]->info->damage_radius;
        if (player_entity_within_radius(local_entity, charge_pos, radius, true)) {
            return false;
        }
        const float damage = live.charges[i]->info->damage_multi * damage_scale;
        self_damage += splash_damage_to_entity(local_entity, damage, radius, charge_pos);
        enemy_in_blast = enemy_in_blast || enemy_takes_splash(local_entity, damage, radius, charge_pos);
    }
    if (!splash_self_damage_acceptable(local_entity, self_damage, !crater_goal)) {
        return false;
    }
    const rf::Timestamp& window = bot_state().firing.remote_charge_pending_detonation_timer;
    return crater_goal || enemy_in_blast || enemies_leaving || !window.valid() || window.elapsed();
}

bool enemies_leaving_charges(const rf::Entity& local_entity, const LiveRemoteCharges& live)
{
    const rf::Player* const local_player = bot_player();
    const bool team_mode = multi_is_team_game_type();
    for (const rf::Player& player : SinglyLinkedList{rf::player_list}) {
        const bool teammate = team_mode && local_player && player.team == local_player->team;
        if (&player == local_player || player.is_spectator || teammate) {
            continue;
        }
        rf::Entity* const entity = rf::entity_from_handle(player.entity_handle);
        if (!entity || rf::entity_is_dying(entity)) {
            continue;
        }
        bool near_charges = false;
        bool considered = false;
        for (int i = 0; i < live.count; ++i) {
            const float radius = live.charges[i]->info->damage_radius;
            const float dist_sq = rf::vec_dist_squared(&entity->pos, &live.charges[i]->pos);
            near_charges = near_charges || dist_sq <= std::pow(radius * kRemoteChargeEnemyRelevanceScale, 2.0f);
            considered = considered || dist_sq <= std::pow(radius * kRemoteChargeEnemyConsideredScale, 2.0f);
        }
        if (!considered
            || (!near_charges
                && !bot_has_unobstructed_level_los(local_entity.eye_pos, entity->eye_pos, &local_entity, entity))) {
            continue;
        }
        for (int i = 0; i < live.count; ++i) {
            const rf::Vector3 away = entity->pos - live.charges[i]->pos;
            const float radius = live.charges[i]->info->damage_radius;
            if (away.len_sq() <= radius * radius || away.dot_prod(entity->p_data.vel) <= 0.0f) {
                return false;
            }
        }
    }
    return true;
}

void update_remote_charge_state(const LiveRemoteCharges& live, const bool crater_goal)
{
    BotCombatFiringState& firing = bot_state().firing;
    const bool settling = remote_charge_settling();
    if (live.count == 0 || remote_charges_given_up()) {
        if (live.count == 0 && !settling) {
            firing.remote_charge_give_up_timer.invalidate();
        }
        if (!settling) {
            firing.remote_charge_pending_detonation = false;
            firing.remote_charge_pending_detonation_timer.invalidate();
        }
        return;
    }
    if (!firing.remote_charge_pending_detonation) {
        const int window_ms = crater_goal ? kRemoteChargeCraterWindowMs : kRemoteChargeDetonationWindowMs;
        firing.remote_charge_pending_detonation = true;
        firing.remote_charge_pending_detonation_timer.set(window_ms);
        if (!firing.remote_charge_give_up_timer.valid()) {
            firing.remote_charge_give_up_timer.set(window_ms + kRemoteChargeGiveUpMs);
        }
    }
}

// The bot returns to the detonator farther out than it leaves it, so it does not flip between weapons at one distance.
bool enemy_too_close_for_detonator(const rf::Entity& local_entity, const rf::Entity& enemy)
{
    const float leave_distance =
        bot_weapon_stats(rf::remote_charge_weapon_type, kStatsReferenceDistance).min_safe_distance;
    const float close_distance = local_entity.ai.current_primary_weapon == rf::remote_charge_det_weapon_type
        ? leave_distance
        : leave_distance * kDetonatorReturnDistanceScale;
    return rf::vec_dist_squared(&local_entity.pos, &enemy.pos) < close_distance * close_distance;
}

void hold_remote_charge_detonator(rf::Player& local_player, rf::Entity& local_entity)
{
    const int detonator = rf::remote_charge_det_weapon_type;
    if (!remote_charge_throw_in_progress()
        && owns_remote_charge_detonator(local_entity)
        && local_entity.ai.current_primary_weapon != detonator) {
        switch_to_weapon(local_player, local_entity, detonator);
    }
}

const BotWeaponRangeCache& weapon_ranges(const int weapon_type)
{
    static const BotWeaponRangeCache no_ranges{};
    auto& cache = bot_state().firing.weapon_ranges;
    if (weapon_type < 0 || weapon_type >= std::min(rf::num_weapon_types, static_cast<int>(cache.size()))) {
        return no_ranges;
    }
    BotWeaponRangeCache& ranges = cache[weapon_type];
    const float skill_factor = bot_get_skill_factor();
    if (ranges.valid && ranges.skill_factor == skill_factor) {
        return ranges;
    }
    ranges = BotWeaponRangeCache{true, skill_factor};
    const BotWeaponStats reference = bot_weapon_stats(weapon_type, kStatsReferenceDistance);
    if (reference.weapon_type < 0) {
        return ranges;
    }
    const float near_limit = std::max(reference.min_safe_distance, 1.0f);
    const float far_limit = std::max(std::min(reference.max_range, kRangeSampleMaxDistance), near_limit);
    std::array<float, kRangeSampleCount> distances{};
    std::array<float, kRangeSampleCount> dps{};
    for (int i = 0; i < kRangeSampleCount; ++i) {
        distances[i] =
            std::lerp(near_limit, far_limit, static_cast<float>(i) / static_cast<float>(kRangeSampleCount - 1));
        dps[i] = bot_weapon_expected_dps(bot_weapon_stats(weapon_type, distances[i]), distances[i]);
        ranges.peak_dps = std::max(ranges.peak_dps, dps[i]);
    }
    for (int i = kRangeSampleCount - 1; i >= 0 && ranges.peak_dps > 0.0f; --i) {
        if (ranges.ideal_range <= 0.0f && dps[i] >= ranges.peak_dps * kIdealRangeDpsShare) {
            ranges.ideal_range = distances[i];
        }
        if (ranges.effective_range <= 0.0f && dps[i] >= ranges.peak_dps * kEffectiveRangeDpsShare) {
            ranges.effective_range = distances[i];
        }
    }
    return ranges;
}

bool weapon_in_effective_range(const BotWeaponStats& stats, const float distance)
{
    const float dps = bot_weapon_expected_dps(stats, distance);
    return dps > 0.0f && dps >= weapon_ranges(stats.weapon_type).peak_dps * kEffectiveRangeDpsShare;
}

float personality_weapon_weight(const int weapon_type)
{
    float weight = std::sqrt(std::max(bot_get_weapon_preference_weight(weapon_type), 0.05f));
    const std::vector<int>& preferred = get_active_bot_personality().preferred_weapon_types;
    const auto it = std::find(preferred.begin(), preferred.end(), weapon_type);
    if (it != preferred.end()) {
        const float count = static_cast<float>(preferred.size());
        weight *= 1.0f + kPreferredWeaponRankBonus * (count - static_cast<float>(it - preferred.begin())) / count;
    }
    return weight;
}

float quirk_weapon_weight(
    const int weapon_type,
    const float distance,
    const rf::Entity* enemy_target,
    const bool enemy_has_los)
{
    if (!enemy_target) {
        return 1.0f;
    }
    const float enemy_survivability =
        get_entity_health_ratio(*enemy_target) * 0.72f + get_entity_armor_ratio(*enemy_target) * 0.28f;
    const bool finisher_shotgun =
        bot_personality_has_quirk(BotPersonalityQuirk::shotgun_low_health_finisher)
        && enemy_survivability <= 0.42f
        && distance <= 20.0f
        && weapon_type == bot_resolve_weapon_type_cached("Shotgun");
    const bool finisher_melee =
        bot_personality_has_quirk(BotPersonalityQuirk::melee_finisher)
        && enemy_survivability <= 0.30f
        && rf::weapon_is_melee(weapon_type);
    const rf::Timestamp& los_recent = bot_state().firing.enemy_los_recent_timer;
    const bool no_los_railgun =
        bot_personality_has_quirk(BotPersonalityQuirk::railgun_no_los_hunter)
        && !enemy_has_los
        && (!los_recent.valid() || los_recent.elapsed())
        && weapon_type == bot_resolve_weapon_type_cached("rail_gun");
    return finisher_shotgun || finisher_melee || no_los_railgun ? kQuirkWeaponUtilityScale : 1.0f;
}

bool weapon_is_combat_candidate(rf::Player& player, rf::Entity& entity, const int weapon_type)
{
    if (!weapon_has_usable_ammo(player, entity, weapon_type)
        || rf::weapon_is_detonator(weapon_type)
        || (weapon_type != entity.ai.current_primary_weapon
            && multi_is_weapon_switch_locked(entity.ai.current_primary_weapon, weapon_type))) {
        return false;
    }
    if (rf::entity_is_swimming(&entity) && !weapon_works_underwater(weapon_type)) {
        return false;
    }
    return !(weapon_type == rf::remote_charge_weapon_type && remote_charge_already_out());
}

// Shared by every weapon of one evaluation, so the LOS traces run once.
float nearest_visible_enemy_in_blast_range(rf::Player& player, rf::Entity& entity, const float distance)
{
    float max_safe_distance = 0.0f;
    for (int weapon_type = 0; weapon_type < rf::num_weapon_types; ++weapon_type) {
        if (!weapon_is_combat_candidate(player, entity, weapon_type)) {
            continue;
        }
        const BotWeaponStats stats = bot_weapon_stats(weapon_type, distance);
        if (!stats.remote_charge) {
            max_safe_distance = std::max(max_safe_distance, stats.min_safe_distance);
        }
    }
    if (max_safe_distance <= 0.0f) {
        return std::numeric_limits<float>::infinity();
    }
    return nearest_player_entity_distance(entity, entity.pos, max_safe_distance, false, true);
}

bool clip_is_empty(const rf::Entity& entity, const int weapon_type)
{
    return rf::weapon_uses_clip(weapon_type) && entity.ai.clip_ammo[weapon_type] <= 0;
}

bool is_close_fight(const rf::Entity* enemy_target, const bool enemy_has_los, const float distance)
{
    return enemy_target && enemy_has_los && distance <= kCloseFightDistance;
}

float weapon_combat_utility(
    rf::Player& player,
    rf::Entity& entity,
    const int weapon_type,
    const float distance,
    const rf::Entity* enemy_target,
    const bool enemy_has_los,
    const float nearest_visible_enemy)
{
    if (!weapon_is_combat_candidate(player, entity, weapon_type)) {
        return 0.0f;
    }
    const BotWeaponStats stats = bot_weapon_stats(weapon_type, distance);
    const bool current = weapon_type == entity.ai.current_primary_weapon;
    // Taken up only clear of its blast distance, so it is not dropped again at once.
    const float splash_take_up_distance = std::min(
        stats.min_safe_distance * kSplashCandidateDistanceScale,
        stats.max_range * kSplashCandidateMaxRangeShare);
    if (stats.min_safe_distance > 0.0f
        && !stats.remote_charge
        && ((!current && distance < splash_take_up_distance)
            || nearest_visible_enemy <= stats.min_safe_distance)) {
        return 0.0f;
    }
    if (stats.melee && !current && distance > stats.max_range * kMeleeCandidateReachShare) {
        return 0.0f;
    }
    float utility = bot_weapon_expected_dps(stats, distance);
    if (utility <= 0.0f) {
        return 0.0f;
    }
    if (!rf::weapon_is_melee(weapon_type)) {
        const float fill = std::max(bot_decision_get_weapon_ammo_fill_ratio(entity, weapon_type), 0.02f);
        utility *= std::pow(fill, kAmmoFillUtilityExponent);
        if (clip_is_empty(entity, weapon_type)) {
            const bool close_fight = is_close_fight(enemy_target, enemy_has_los, distance);
            // Swapping to another empty weapon in a close fight still means a reload.
            if (close_fight && !current) {
                return 0.0f;
            }
            utility *= close_fight ? kEmptyClipCloseFightUtilityScale : kEmptyClipUtilityScale;
        }
    }
    return utility
        * personality_weapon_weight(weapon_type)
        * quirk_weapon_weight(weapon_type, distance, enemy_target, enemy_has_los);
}

struct CombatWeaponChoice
{
    int weapon_type = -1;
    float utility = 0.0f;
};

CombatWeaponChoice bot_select_combat_weapon(
    rf::Player& player,
    rf::Entity& entity,
    const float target_distance,
    const rf::Entity* enemy_target,
    const bool enemy_has_los,
    const float nearest_visible_enemy)
{
    CombatWeaponChoice best{};
    for (int weapon_type = 0; weapon_type < rf::num_weapon_types; ++weapon_type) {
        const float utility = weapon_combat_utility(
            player,
            entity,
            weapon_type,
            target_distance,
            enemy_target,
            enemy_has_los,
            nearest_visible_enemy);
        if (utility > best.utility) {
            best = {weapon_type, utility};
        }
    }
    return best;
}

bool owns_combat_weapon(rf::Player& player, rf::Entity& entity)
{
    for (int weapon_type = 0; weapon_type < rf::num_weapon_types; ++weapon_type) {
        if (weapon_is_combat_candidate(player, entity, weapon_type)) {
            return true;
        }
    }
    return false;
}

int strongest_usable_weapon(rf::Player& player, rf::Entity& entity)
{
    int best_weapon = -1;
    float best_peak = 0.0f;
    for (int weapon_type = 0; weapon_type < rf::num_weapon_types; ++weapon_type) {
        if (!weapon_is_combat_candidate(player, entity, weapon_type)) {
            continue;
        }
        const float peak = weapon_ranges(weapon_type).peak_dps;
        if (peak > best_peak) {
            best_peak = peak;
            best_weapon = weapon_type;
        }
    }
    return best_weapon;
}

float combat_reference_distance(const rf::Entity& entity, const rf::Entity* enemy_target)
{
    if (enemy_target) {
        return std::sqrt(std::max(rf::vec_dist_squared(&entity.pos, &enemy_target->pos), 0.0f));
    }
    const BotPersonality& personality = get_active_bot_personality();
    return 0.5f * (personality.preferred_engagement_near + personality.preferred_engagement_far);
}

void update_combat_weapon_choice(
    rf::Player& player,
    rf::Entity& entity,
    const rf::Entity* enemy_target,
    const bool enemy_has_los)
{
    BotCombatFiringState& firing = bot_state().firing;
    const int current_weapon = entity.ai.current_primary_weapon;
    const bool acquired =
        enemy_target && enemy_has_los && enemy_target->handle != firing.weapon_select_target_handle;
    if (enemy_target && enemy_has_los) {
        firing.weapon_select_target_handle = enemy_target->handle;
    }
    else if (!enemy_target) {
        firing.weapon_select_target_handle = -1;
    }

    const float distance = combat_reference_distance(entity, enemy_target);
    const float nearest_visible_enemy = nearest_visible_enemy_in_blast_range(player, entity, distance);
    const float current_utility = weapon_combat_utility(
        player,
        entity,
        current_weapon,
        distance,
        enemy_target,
        enemy_has_los,
        nearest_visible_enemy);
    const bool dry_in_close_fight =
        current_weapon >= 0
        && current_weapon < rf::num_weapon_types
        && clip_is_empty(entity, current_weapon)
        && is_close_fight(enemy_target, enemy_has_los, distance);
    const bool cadence_due =
        !bot_state().combat_weapon_eval_timer.valid() || bot_state().combat_weapon_eval_timer.elapsed();
    if (!acquired
        && !firing.weapon_pickup_pending
        && !dry_in_close_fight
        && current_utility > 0.0f
        && !cadence_due
        && firing.weapon_select_candidate < 0) {
        return;
    }
    if (cadence_due) {
        bot_state().combat_weapon_eval_timer.set(kWeaponSelectCadenceMs);
    }

    const CombatWeaponChoice best =
        bot_select_combat_weapon(player, entity, distance, enemy_target, enemy_has_los, nearest_visible_enemy);
    if (best.weapon_type < 0 && !weapon_is_combat_candidate(player, entity, current_weapon)) {
        switch_to_weapon(player, entity, strongest_usable_weapon(player, entity));
        return;
    }
    if (best.weapon_type < 0
        || best.weapon_type == current_weapon
        || best.utility < current_utility * kWeaponSwitchUtilityMargin) {
        firing.weapon_pickup_pending = false;
        firing.weapon_select_candidate = -1;
        return;
    }

    const BotPersonality& personality = get_active_bot_personality();
    if (current_utility <= 0.0f || dry_in_close_fight) {
        switch_to_weapon(player, entity, best.weapon_type);
        return;
    }
    if (firing.requested_weapon >= 0 && weapon_request_pending(firing.requested_weapon)) {
        return;
    }
    // A pickup made during the cooldown stays pending until it is over.
    if (bot_state().weapon_switch_timer.valid() && !bot_state().weapon_switch_timer.elapsed()) {
        return;
    }
    const bool pickup = std::exchange(firing.weapon_pickup_pending, false);
    const float switch_norm = std::clamp(
        std::clamp(get_active_bot_skill_profile().weapon_switch_likelihood, 0.05f, 1.0f)
            * std::clamp(personality.weapon_switch_bias, 0.0f, 2.5f),
        0.0f,
        1.0f
    );
    if (switch_norm <= 0.0f) {
        return;
    }
    std::uniform_real_distribution<float> roll_dist(0.0f, 1.0f);
    const bool immediate_pickup_switch =
        pickup
        && (!personality.preferred_weapon_types.empty()
            || roll_dist(g_rng) <= std::clamp(personality.pickup_switch_chance_without_preferences, 0.0f, 1.0f));
    if (acquired || immediate_pickup_switch) {
        if (!switch_to_weapon(player, entity, best.weapon_type) && acquired) {
            firing.weapon_select_target_handle = -1;
        }
        return;
    }
    if (firing.weapon_select_candidate != best.weapon_type) {
        firing.weapon_select_candidate = best.weapon_type;
        firing.weapon_select_candidate_timer.set(static_cast<int>(std::lround(std::lerp(
            static_cast<float>(kWeaponSelectDwellSlowMs),
            static_cast<float>(kWeaponSelectDwellFastMs),
            switch_norm))));
        return;
    }
    if (firing.weapon_select_candidate_timer.elapsed()) {
        switch_to_weapon(player, entity, best.weapon_type);
    }
}

void update_floor_aim(const rf::Entity& local_entity, const rf::Entity& enemy_target)
{
    BotCombatFiringState& firing = bot_state().firing;
    firing.aim_at_floor = false;
    const ProjectileTraits projectile = projectile_traits(local_entity.ai.current_primary_weapon);
    if (projectile.splash_radius <= 0.0f
        || projectile.thrown
        || !rf::entity_on_ground(const_cast<rf::Entity*>(&enemy_target))) {
        return;
    }
    rf::Vector3 floor_pos{};
    if (!trace_ground_below_point(enemy_target.pos, kFloorAimProbeDepth, &floor_pos)) {
        return;
    }
    floor_pos.y += kFloorAimHeight;
    if (!bot_has_unobstructed_level_los(local_entity.eye_pos, floor_pos, &local_entity, &enemy_target)) {
        return;
    }
    firing.aim_at_floor = true;
    firing.aim_floor_drop = enemy_target.pos.y - floor_pos.y;
}

void update_reload(rf::Player& player, rf::Entity& entity)
{
    const int weapon_type = entity.ai.current_primary_weapon;
    if (weapon_type < 0 || weapon_type >= rf::num_weapon_types || !rf::weapon_uses_clip(weapon_type)) {
        return;
    }
    const int requested_weapon = bot_state().firing.requested_weapon;
    if ((bot_state().reload_timer.valid() && !bot_state().reload_timer.elapsed())
        || bot_backend_weapon_busy(player, entity)
        || (requested_weapon != weapon_type && weapon_request_pending(requested_weapon))) {
        return;
    }
    const rf::WeaponInfo& info = rf::weapon_types[weapon_type];
    const int clip = entity.ai.clip_ammo[weapon_type];
    const int reserve = info.ammo_type >= 0 && info.ammo_type < static_cast<int>(std::size(entity.ai.ammo))
        ? entity.ai.ammo[info.ammo_type]
        : 0;
    if (reserve <= 0 || clip >= info.clip_size) {
        return;
    }
    const rf::Timestamp& los_recent = bot_state().firing.enemy_los_recent_timer;
    const bool enemy_out_of_sight = !los_recent.valid() || los_recent.elapsed();
    const bool tactical = enemy_out_of_sight
        && static_cast<float>(clip) <= static_cast<float>(info.clip_size) * kTacticalReloadClipShare;
    if (clip <= 0 || tactical) {
        bot_backend_reload(player);
        bot_state().reload_timer.set(800);
    }
}

rf::Vector3 navigation_look_point(const rf::Entity& local_entity, const rf::Vector3& move_target)
{
    rf::Vector3 travel_dir = move_target - local_entity.pos;
    travel_dir.y = 0.0f;
    if (travel_dir.len_sq() < 0.0001f) {
        return move_target;
    }
    travel_dir.normalize_safe();

    const BotAlertContact* threat = nullptr;
    for (const BotAlertContact& contact : bot_state().alert_contacts) {
        if (!contact.awareness_timer.valid() || contact.awareness_timer.elapsed()) {
            continue;
        }
        rf::Vector3 to_contact = contact.last_known_pos - local_entity.pos;
        to_contact.y = 0.0f;
        if (to_contact.len_sq() < 1.0f) {
            continue;
        }
        to_contact.normalize_safe();
        if (to_contact.dot_prod(travel_dir) < kThreatLookMinTravelDot) {
            continue;
        }
        if (!threat || contact.awareness_weight > threat->awareness_weight) {
            threat = &contact;
        }
    }
    if (threat) {
        return threat->last_known_pos;
    }

    const std::vector<int>& path = bot_state().waypoint_path;
    const int after_next_index = bot_state().waypoint_next_index + 1;
    rf::Vector3 after_next_pos{};
    if (after_next_index <= 0
        || after_next_index >= static_cast<int>(path.size())
        || rf::vec_dist_squared(&local_entity.pos, &move_target) >= kCornerLookAheadDistance * kCornerLookAheadDistance
        || !waypoints_get_pos(path[after_next_index], after_next_pos)) {
        return move_target;
    }
    rf::Vector3 next_leg_dir = after_next_pos - move_target;
    next_leg_dir.y = 0.0f;
    if (next_leg_dir.len_sq() < 0.0001f) {
        return move_target;
    }
    next_leg_dir.normalize_safe();
    return next_leg_dir.dot_prod(travel_dir) < kCornerLookMaxTurnDot ? after_next_pos : move_target;
}
}

bool bot_weapon_profile_supports_range(const BotWeaponProfile& profile, const BotWeaponRangeBand range)
{
    const float peak = weapon_ranges(profile.weapon_type).peak_dps;
    if (peak <= 0.0f) {
        return false;
    }
    const float near_limit = range == BotWeaponRangeBand::melee
        ? 0.5f
        : bot_decision_combat_distance_band_far_limit(static_cast<BotWeaponRangeBand>(static_cast<int>(range) - 1));
    const float far_limit = bot_decision_combat_distance_band_far_limit(range);
    constexpr int kBandSamples = 5;
    for (int i = 0; i < kBandSamples; ++i) {
        const float distance =
            std::lerp(near_limit, far_limit, static_cast<float>(i) / static_cast<float>(kBandSamples - 1));
        if (bot_weapon_expected_dps(bot_weapon_stats(profile.weapon_type, distance), distance)
            >= peak * kRangeBandDpsShare) {
            return true;
        }
    }
    return false;
}

int bot_weapon_create_delay_ms(const int weapon_type, const bool alt_fire)
{
    if (weapon_type < 0 || weapon_type >= rf::num_weapon_types) {
        return 0;
    }
    const rf::WeaponInfo& info = rf::weapon_types[weapon_type];
    const float delay = alt_fire ? info.alt_create_weapon_delay_seconds[0] : info.create_weapon_delay_seconds[0];
    return delay > 0.0f ? static_cast<int>(std::lround(delay * 1000.0f)) : 0;
}

float bot_combat_engagement_distance(const rf::Entity& entity, float& out_min_safe_distance)
{
    out_min_safe_distance = 0.0f;
    const int weapon_type = entity.ai.current_primary_weapon;
    if (rf::weapon_is_detonator(weapon_type)) {
        if (!bot_state().firing.remote_charge_pending_detonation) {
            return -1.0f;
        }
        const BotWeaponStats charge = bot_weapon_stats(rf::remote_charge_weapon_type, kStatsReferenceDistance);
        out_min_safe_distance = splash_self_safe_distance(charge.splash_radius, charge.shot_damage);
        return out_min_safe_distance * kCraterStandoffScale;
    }
    const BotWeaponStats stats = bot_weapon_stats(weapon_type, kStatsReferenceDistance);
    if (stats.weapon_type < 0
        || (!stats.melee && bot_decision_get_weapon_ammo_fill_ratio(entity, weapon_type) <= 0.0f)) {
        return -1.0f;
    }
    out_min_safe_distance = stats.min_safe_distance;
    const float ideal = weapon_ranges(weapon_type).ideal_range;
    return ideal > 0.0f ? ideal : -1.0f;
}

float bot_combat_splash_keep_distance(const rf::Entity& entity)
{
    const int weapon_type = entity.ai.current_primary_weapon;
    if (rf::weapon_is_detonator(weapon_type)) {
        return 0.0f;
    }
    const BotWeaponStats stats = bot_weapon_stats(weapon_type, kStatsReferenceDistance);
    return stats.remote_charge || stats.thrown ? 0.0f : stats.min_safe_distance;
}

float bot_best_combat_weapon_dps(rf::Player& player, rf::Entity& entity, const float target_distance)
{
    float best_dps = 0.0f;
    for (int weapon_type = 0; weapon_type < rf::num_weapon_types; ++weapon_type) {
        if (weapon_is_combat_candidate(player, entity, weapon_type)) {
            best_dps = std::max(
                best_dps,
                bot_weapon_expected_dps(bot_weapon_stats(weapon_type, target_distance), target_distance));
        }
    }
    return best_dps;
}

float bot_crater_standoff_distance(rf::Player& player, rf::Entity& entity, float& out_max_distance)
{
    out_max_distance = std::numeric_limits<float>::max();
    const bool remote_charge_live = bot_state().firing.remote_charge_pending_detonation;
    const BotWeaponStats stats = bot_weapon_stats(
        remote_charge_live ? rf::remote_charge_weapon_type : find_best_crater_weapon(player, entity),
        kStatsReferenceDistance);
    if (stats.splash_radius <= 0.0f || (stats.remote_charge && !remote_charge_live)) {
        return 0.0f;
    }
    float standoff = stats.splash_radius * kCraterStandoffScale;
    if (stats.thrown && !stats.remote_charge) {
        out_max_distance = stats.max_range * kThrownStandoffMaxRangeShare;
        standoff = std::min(standoff, stats.min_safe_distance * kThrownStandoffSafeScale);
    }
    return standoff;
}

bool bot_has_usable_crater_weapon(rf::Player& player, rf::Entity& entity)
{
    const bool charge_to_set_off =
        bot_state().firing.remote_charge_pending_detonation && owns_remote_charge_detonator(entity);
    return charge_to_set_off || find_best_crater_weapon(player, entity) >= 0;
}

bool bot_has_usable_shatter_weapon(rf::Player& player, rf::Entity& entity)
{
    return find_best_shatter_weapon(player, entity) >= 0;
}

void clear_enemy_aim_error_state()
{
    bot_state().firing.aim_error_timer.invalidate();
    bot_state().firing.aim_error_target_handle = -1;
    bot_state().firing.aim_error_right = 0.0f;
    bot_state().firing.aim_error_up = 0.0f;
}

void clear_semi_auto_click_state()
{
    bot_state().firing.semi_auto_click_timer.invalidate();
    bot_state().firing.semi_auto_click_weapon = -1;
}

void bot_track_aim_goal(rf::Entity& local_entity)
{
    const BotCombatFiringState& firing = bot_state().firing;
    if (!firing.aim_goal_active) {
        return;
    }
    rf::Vector3 aim_target = firing.aim_goal_point;
    if (firing.aim_goal_enemy_handle >= 0) {
        rf::Entity* const enemy = rf::entity_from_handle(firing.aim_goal_enemy_handle);
        if (!enemy || rf::entity_is_dying(enemy)) {
            return;
        }
        aim_target = compute_skill_adjusted_enemy_aim_target(local_entity, *enemy, firing.aim_goal_skill_factor);
    }
    float alignment = 0.0f;
    update_aim_towards(local_entity, aim_target, firing.aim_goal_skill_factor, alignment);
}

void ensure_weapon_ready(rf::Player& player, rf::Entity& entity)
{
    if (!collect_newly_acquired_weapons(entity).empty()) {
        bot_state().firing.weapon_pickup_pending = true;
    }
    const int current_weapon = entity.ai.current_primary_weapon;
    if (bot_state().firing.remote_charge_pending_detonation || weapon_has_usable_ammo(player, entity, current_weapon)) {
        return;
    }
    const float distance = combat_reference_distance(entity, nullptr);
    const CombatWeaponChoice best = bot_select_combat_weapon(
        player,
        entity,
        distance,
        nullptr,
        false,
        nearest_visible_enemy_in_blast_range(player, entity, distance));
    switch_to_weapon(
        player,
        entity,
        best.weapon_type >= 0 ? best.weapon_type : strongest_usable_weapon(player, entity));
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
        bot_state().active_goal == BotGoalType::create_crater
        && bot_state().fsm_state == BotFsmState::create_crater
        && bot_state().goal_target_identifier >= 0;
    const bool shatter_goal_active =
        bot_state().active_goal == BotGoalType::shatter_glass
        && bot_state().fsm_state == BotFsmState::shatter_glass
        && bot_state().goal_target_identifier >= 0;
    const bool crater_has_los = crater_goal_active
        && (crater_target_has_linked_waypoint_los(local_entity, bot_state().goal_target_identifier)
            || crater_target_has_direct_los(local_entity, bot_state().goal_target_pos));
    const bool shatter_has_waypoint_los = shatter_goal_active
        && shatter_target_has_linked_waypoint_los(
            local_entity,
            bot_state().goal_target_identifier);
    bool explosive_release_hold_active =
        bot_state().firing.explosive_release_hold_timer.valid()
        && !bot_state().firing.explosive_release_hold_timer.elapsed();
    if (!explosive_release_hold_active) {
        bot_state().firing.explosive_release_hold_timer.invalidate();
    }
    if (enemy_target && enemy_has_los) {
        bot_state().firing.enemy_los_recent_timer.set(kTacticalReloadLosGapMs);
    }
    const LiveRemoteCharges live_remote_charges = owns_remote_charge_detonator(local_entity)
        ? find_live_remote_charges(local_entity)
        : LiveRemoteCharges{};
    bot_state().firing.remote_charges_live = live_remote_charges.total;
    update_remote_charge_state(live_remote_charges, crater_goal_active);
    const bool remote_charge_live = bot_state().firing.remote_charge_pending_detonation;
    bool detonation_due = false;
    bool fight_before_detonating = false;
    if (remote_charge_live) {
        const bool enemies_leaving = !crater_goal_active
            && !remote_charge_settling()
            && enemies_leaving_charges(local_entity, live_remote_charges);
        detonation_due =
            should_detonate_remote_charges(local_entity, live_remote_charges, crater_goal_active, enemies_leaving);
        fight_before_detonating = !detonation_due
            && !crater_goal_active
            && enemy_target
            && enemy_has_los
            && enemy_too_close_for_detonator(local_entity, *enemy_target)
            && owns_combat_weapon(local_player, local_entity);
    }

    if (remote_charge_live || crater_goal_active || shatter_goal_active) {
        bot_state().firing.weapon_pickup_pending = false;
    }
    if (remote_charge_live && !fight_before_detonating) {
        hold_remote_charge_detonator(local_player, local_entity);
    }
    else if (crater_goal_active) {
        maybe_switch_weapon_for_crater_goal(local_player, local_entity);
    }
    else if (shatter_goal_active) {
        maybe_switch_weapon_for_shatter_goal(local_player, local_entity);
    }
    else {
        update_combat_weapon_choice(local_player, local_entity, enemy_target, enemy_has_los);
    }
    update_reload(local_player, local_entity);
    if (enemy_target && enemy_has_los) {
        update_floor_aim(local_entity, *enemy_target);
    }

    if (local_entity.ai.current_primary_weapon >= 0) {
        active_skill_factor = compute_weapon_skill_adjusted_factor(
            skill_factor,
            local_entity.ai.current_primary_weapon
        );
    }

    const auto set_aim_goal = [&](const rf::Vector3& point, const int enemy_handle) {
        BotCombatFiringState& firing = bot_state().firing;
        firing.aim_goal_active = true;
        firing.aim_goal_point = point;
        firing.aim_goal_enemy_handle = enemy_handle;
        firing.aim_goal_skill_factor = active_skill_factor;
    };
    bot_state().firing.aim_goal_active = false;
    if (explosive_release_hold_active
        && enemy_target
        && enemy_has_los
        && !crater_goal_active
        && !shatter_goal_active) {
        BotCombatFiringState& firing = bot_state().firing;
        firing.explosive_release_hold_target =
            compute_skill_adjusted_enemy_aim_target(local_entity, *enemy_target, active_skill_factor);
        const BotWeaponStats hold_stats =
            weapon_stats_for_mode(firing.explosive_release_hold_weapon, firing.explosive_release_hold_alt);
        if (!splash_fire_is_safe(local_player, local_entity, hold_stats, enemy_target->pos, enemy_target)) {
            firing.explosive_release_aborted = true;
        }
    }
    if (explosive_release_hold_active) {
        resolved_aim_target = bot_state().firing.explosive_release_hold_target;
        set_aim_goal(resolved_aim_target, -1);
        has_aim_solution = update_aim_towards(
            local_entity,
            resolved_aim_target,
            active_skill_factor,
            aim_alignment
        );
    }
    else if ((crater_goal_active && crater_has_los)
        || (shatter_goal_active && shatter_has_waypoint_los)) {
        if (bot_state().goal_target_pos.len_sq() > 0.0001f) {
            resolved_aim_target = bot_state().goal_target_pos;
            const ProjectileTraits projectile = projectile_traits(local_entity.ai.current_primary_weapon);
            if (crater_goal_active && projectile.thrown) {
                resolved_aim_target = ballistic_aim_point(local_entity.eye_pos, resolved_aim_target, projectile.speed);
            }
            set_aim_goal(resolved_aim_target, -1);
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
        set_aim_goal(resolved_aim_target, enemy_target->handle);
        has_aim_solution = update_aim_towards(
            local_entity,
            enemy_aim_target,
            active_skill_factor,
            aim_alignment
        );
    }
    else if (has_move_target
             && !bot_state().freefall_suppress_aim) {
        rf::Vector3 aim_target = move_target;
        const float vertical_delta = std::abs(move_target.y - local_entity.eye_pos.y);
        if (vertical_delta <= kNavigationAimVerticalThreshold) {
            const bool precision_navigation_goal =
                bot_goal_is_item_collection(bot_state().active_goal)
                || bot_goal_is_ctf_objective(bot_state().active_goal)
                || bot_state().active_goal == BotGoalType::activate_bridge
                || bot_state().active_goal == BotGoalType::create_crater
                || bot_state().active_goal == BotGoalType::shatter_glass;
            // navigation_strafe_bias 0 keeps the bot looking where it goes.
            if (!enemy_target
                && !precision_navigation_goal
                && get_active_bot_personality().navigation_strafe_bias > 0.0f) {
                aim_target = navigation_look_point(local_entity, move_target);
                if (std::abs(aim_target.y - local_entity.eye_pos.y) > kNavigationAimVerticalThreshold) {
                    aim_target = move_target;
                }
            }
            aim_target.y = local_entity.eye_pos.y;
        }

        resolved_aim_target = aim_target;
        set_aim_goal(resolved_aim_target, -1);
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
        if (bot_state().firing.target_acquisition_handle != enemy_target->handle) {
            bot_state().firing.target_acquisition_handle = enemy_target->handle;
            bot_state().firing.target_acquisition_timer.set(
                std::max(static_cast<int>(get_active_bot_skill_profile().reaction_time_ms), 0));
        }
        within_acquisition_reaction =
            bot_state().firing.target_acquisition_timer.valid()
            && !bot_state().firing.target_acquisition_timer.elapsed();
    }
    else if (!enemy_target) {
        bot_state().firing.target_acquisition_handle = -1;
        bot_state().firing.target_acquisition_timer.invalidate();
    }

    const bool bot_is_underwater = rf::entity_is_swimming(&local_entity);
    if ((enemy_target || crater_goal_active || shatter_goal_active || remote_charge_live)
        && local_entity.ai.current_primary_weapon >= 0
        && (!bot_is_underwater || weapon_works_underwater(local_entity.ai.current_primary_weapon))) {
        const int weapon_type = local_entity.ai.current_primary_weapon;
        const rf::Vector3 fire_target_pos = (crater_goal_active || shatter_goal_active)
            ? bot_state().goal_target_pos
            : (enemy_target ? enemy_target->pos : local_entity.pos);
        const float dist_sq = rf::vec_dist_squared(&local_entity.pos, &fire_target_pos);
        const float distance = std::sqrt(std::max(dist_sq, 0.0f));
        const BotWeaponStats weapon_stats = bot_weapon_stats(weapon_type, distance);
        const CombatFireMode fire_mode = select_combat_fire_mode(weapon_type, distance);
        attack_weapon_type = weapon_type;
        attack_use_alt_fire = fire_mode.use_alt_fire;
        const bool is_semi_auto = fire_mode.is_semi_auto;
        const bool can_fire_now = !bot_backend_weapon_busy(local_player, local_entity);
        if (!can_fire_now) {
            bot_state().firing.wants_fire = false;
            bot_state().fire_decision_timer.invalidate();
            bot_state().auto_fire_release_guard_timer.invalidate();
        }
        bool wants_attack = false;
        if (rf::weapon_is_detonator(weapon_type)) {
            bot_state().auto_fire_release_guard_timer.invalidate();
            wants_attack = can_fire_now
                && remote_charge_live
                && detonation_due;
        }
        else if (crater_goal_active) {
            bot_state().auto_fire_release_guard_timer.invalidate();
            wants_attack = can_fire_now
                && has_aim_solution
                && weapon_can_create_craters(weapon_type)
                && crater_has_los
                && distance <= weapon_stats.max_range
                && splash_fire_is_safe(local_player, local_entity, weapon_stats, fire_target_pos, nullptr)
                && !(weapon_stats.remote_charge && remote_charge_already_out());
        }
        else if (shatter_goal_active) {
            bot_state().auto_fire_release_guard_timer.invalidate();
            wants_attack = can_fire_now
                && has_aim_solution
                && weapon_can_shatter_glass(weapon_type)
                && shatter_has_waypoint_los
                && shatter_target_has_direct_los(
                    local_entity,
                    bot_state().goal_target_identifier);
        }
        else if (within_acquisition_reaction) {
            // Suppress firing during the brief reaction window after acquiring a new target.
            // This prevents the bot from shooting before the aim system has turned to face the enemy.
            wants_attack = false;
        }
        else {
            const float lateral_miss =
                distance * std::sqrt(std::max(1.0f - aim_alignment * aim_alignment, 0.0f));
            const float lateral_tolerance = kTargetHitRadius + weapon_stats.splash_radius * kSplashHitRadiusScale;
            const bool engage_now =
                has_aim_solution
                && enemy_has_los
                && enemy_target != nullptr
                && aim_alignment > 0.0f
                && lateral_miss <= lateral_tolerance
                && weapon_in_effective_range(
                    weapon_stats,
                    weapon_stats.remote_charge ? distance : std::max(distance, weapon_stats.min_safe_distance))
                && !(weapon_stats.remote_charge && remote_charge_already_out())
                && splash_fire_is_safe(local_player, local_entity, weapon_stats, fire_target_pos, enemy_target);
            if (!engage_now && weapon_stats.splash_radius > 0.0f) {
                // The release guard must not carry a held trigger into a splash shot that is not cleared.
                bot_state().auto_fire_release_guard_timer.invalidate();
            }
            wants_attack = can_fire_now && (is_semi_auto
                ? decide_fire(
                    active_skill_factor,
                    engage_now,
                    dist_sq,
                    weapon_ranges(weapon_type).effective_range
                )
                : should_hold_automatic_fire(engage_now));
        }

        wants_attack = apply_explosive_pre_fire_delay(
            weapon_type,
            fire_mode.use_alt_fire,
            wants_attack
        );
        // A server bot throws only on a fresh press, so each throw arms its own release hold.
        const bool server_throw_held_off = bot_ctx().is_server_bot
            && weapon_uses_projectile_release_hold(weapon_type, fire_mode.use_alt_fire)
            && ((fire_mode.use_alt_fire
                    ? bot_state().firing.synthetic_secondary_fire_down
                    : bot_state().firing.synthetic_primary_fire_down)
                || explosive_release_hold_active
                || (local_entity.ai.next_fire_primary.valid() && !local_entity.ai.next_fire_primary.elapsed()));
        if (wants_attack && !server_throw_held_off) {
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
        bot_state().firing.wants_fire = false;
        bot_state().auto_fire_release_guard_timer.invalidate();
        bot_state().firing.explosive_fire_delay_timer.invalidate();
        bot_state().firing.explosive_fire_delay_weapon = -1;
        bot_state().firing.explosive_fire_delay_alt = false;
        clear_semi_auto_click_state();
    }

    const bool previous_primary_fire_down = bot_state().firing.synthetic_primary_fire_down;
    const bool previous_secondary_fire_down = bot_state().firing.synthetic_secondary_fire_down;
    const bool fired_primary_this_frame =
        next_primary_fire_down && !previous_primary_fire_down;
    const bool fired_secondary_this_frame =
        next_secondary_fire_down && !previous_secondary_fire_down;
    const bool fired_this_frame = fired_primary_this_frame || fired_secondary_this_frame;
    if (fired_this_frame && attack_weapon_type == rf::remote_charge_weapon_type) {
        bot_state().firing.remote_charge_pending_detonation = true;
        bot_state().firing.remote_charge_pending_detonation_timer.set(
            crater_goal_active ? kRemoteChargeCraterWindowMs : kRemoteChargeDetonationWindowMs
        );
        bot_state().firing.remote_charge_settle_timer.set(kRemoteChargeThrowSettleMs);
        bot_state().firing.remote_charge_give_up_timer.set(
            (crater_goal_active ? kRemoteChargeCraterWindowMs : kRemoteChargeDetonationWindowMs) + kRemoteChargeGiveUpMs
        );
    }
    if (fired_this_frame) {
        bot_state().firing.explosive_release_aborted = false;
    }
    if (fired_this_frame
        && attack_weapon_type >= 0
        && weapon_uses_projectile_release_hold(attack_weapon_type, attack_use_alt_fire)) {
        int hold_ms = bot_weapon_create_delay_ms(attack_weapon_type, attack_use_alt_fire);
        hold_ms = std::max(
            hold_ms,
            get_active_impact_delay_remaining_ms(local_entity, attack_use_alt_fire)
        );
        if (hold_ms > 0) {
            bot_state().firing.explosive_release_hold_target = resolved_aim_target;
            bot_state().firing.explosive_release_hold_timer.set(hold_ms);
            bot_state().firing.explosive_release_hold_weapon = attack_weapon_type;
            bot_state().firing.explosive_release_hold_alt = attack_use_alt_fire;
        }
    }

    bot_state().firing.synthetic_primary_fire_down = next_primary_fire_down;
    bot_state().firing.synthetic_secondary_fire_down = next_secondary_fire_down;
    bot_state().firing.synthetic_primary_fire_just_pressed =
        fired_primary_this_frame;
    bot_state().firing.synthetic_secondary_fire_just_pressed =
        fired_secondary_this_frame;

    if (!next_primary_fire_down && !next_secondary_fire_down) {
        bot_state().firing.synthetic_primary_fire_just_pressed = false;
        bot_state().firing.synthetic_secondary_fire_just_pressed = false;
    }
}
