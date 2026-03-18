#include "bot_personality.h"
#include "../../main/main.h"
#include "../../misc/alpine_settings.h"
#include "../../rf/item.h"
#include "../../rf/weapon.h"
#include "../alpine_packets.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <random>
#include <string_view>
#include <xlog/xlog.h>

const BotPersonality kDefaultBotPersonality{};
const BotSkillProfile kDefaultBotSkillProfile{};

namespace
{

// ---------------------------------------------------------------------------
// Internal types
// ---------------------------------------------------------------------------

static constexpr int kMaxPreferredWeapons = 6;
static constexpr int kMaxWeaponOverrides = 10;

struct PersonalityPresetDef
{
    BotPersonality personality;
    const char* preferred_weapon_names[kMaxPreferredWeapons]{};
    int preferred_weapon_count = 0;
    BotWeaponPreferenceEntry weapon_overrides[kMaxWeaponOverrides]{};
    int weapon_override_count = 0;
};

struct SkillPresetDef
{
    BotSkillProfile profile;
};

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

BotPersonality g_active_bot_personality = kDefaultBotPersonality;
BotSkillProfile g_active_bot_skill_profile = kDefaultBotSkillProfile;
bool g_profiles_initialized = false;

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------

float read_weight_or_default(const std::vector<float>& weights, const int index)
{
    if (index < 0 || index >= static_cast<int>(weights.size())) {
        return 1.0f;
    }
    return std::clamp(weights[static_cast<size_t>(index)], 0.05f, 8.0f);
}

bool contains_case_insensitive(std::string_view haystack, std::string_view needle)
{
    if (needle.empty() || haystack.empty() || needle.size() > haystack.size()) {
        return false;
    }

    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(haystack[i + j])));
            const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(needle[j])));
            if (a != b) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

bool strings_equal_case_insensitive(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i]))
            != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Personality preset factory functions
// ---------------------------------------------------------------------------

PersonalityPresetDef make_balanced_preset()
{
    PersonalityPresetDef def{};
    def.personality.name = "balanced";
    return def;
}

PersonalityPresetDef make_aggressive_preset()
{
    PersonalityPresetDef def{};
    auto& p = def.personality;
    p.name = "aggressive";
    p.attack_style = BotAttackStyle::aggressive;
    p.preferred_engagement_near = 4.5f;
    p.preferred_engagement_far = 10.0f;
    p.decision_aggression_bias = 1.55f;
    p.decision_risk_tolerance = 1.40f;
    p.raw_aggression_bias = 1.50f;
    p.retreat_health_threshold = 0.18f;
    p.retreat_armor_threshold = 0.12f;
    p.replenish_health_threshold = 0.55f;
    p.replenish_armor_threshold = 0.30f;
    p.goal_commitment_bias = 0.80f;
    p.eliminate_target_commitment_bias = 1.35f;
    p.opportunism_bias = 1.40f;
    p.easy_frag_bias = 1.40f;
    p.retaliation_bias = 1.50f;
    p.crouch_combat_bias = 0.40f;
    p.jump_combat_bias = 0.40f;
    p.dodge_combat_bias = 1.35f;
    p.camping_bias = 0.30f;
    p.deathmatch_kill_focus_bias = 1.40f;
    p.weapon_switch_bias = 1.30f;
    p.min_weapon_switch_cooldown_ms = 2200;
    p.ctf_hold_carrier_hunt_bias = 1.40f;
    p.taunt_on_kill_chance = 0.18f;
    p.gg_on_map_end_chance = 0.35f;

    def.preferred_weapon_names[0] = "Shotgun";
    def.preferred_weapon_names[1] = "Rocket Launcher";
    def.preferred_weapon_names[2] = "Flamethrower";
    def.preferred_weapon_count = 3;

    def.weapon_overrides[0] = {"Shotgun", 1.50f, 1.50f, 1.0f};
    def.weapon_overrides[1] = {"Rocket Launcher", 1.40f, 1.35f, 1.0f};
    def.weapon_overrides[2] = {"Flamethrower", 1.20f, 1.15f, 1.0f};
    def.weapon_overrides[3] = {"rail_gun", 0.80f, 0.75f, 1.0f};
    def.weapon_overrides[4] = {"Sniper Rifle", 0.70f, 0.65f, 1.0f};
    def.weapon_override_count = 5;

    return def;
}

PersonalityPresetDef make_defensive_preset()
{
    PersonalityPresetDef def{};
    auto& p = def.personality;
    p.name = "defensive";
    p.attack_style = BotAttackStyle::evasive;
    p.preferred_engagement_near = 9.0f;
    p.preferred_engagement_far = 18.0f;
    p.decision_aggression_bias = 0.65f;
    p.decision_risk_tolerance = 0.55f;
    p.decision_efficiency_bias = 1.30f;
    p.raw_aggression_bias = 0.55f;
    p.retreat_health_threshold = 0.48f;
    p.retreat_armor_threshold = 0.35f;
    p.replenish_health_threshold = 0.88f;
    p.replenish_armor_threshold = 0.65f;
    p.replenish_bias = 1.45f;
    p.goal_commitment_bias = 1.30f;
    p.eliminate_target_commitment_bias = 0.70f;
    p.opportunism_bias = 0.60f;
    p.easy_frag_bias = 0.70f;
    p.retaliation_bias = 0.70f;
    p.crouch_combat_bias = 1.40f;
    p.dodge_combat_bias = 1.20f;
    p.camping_bias = 1.60f;
    p.power_position_bias = 1.50f;
    p.combat_readiness_threshold = 0.65f;
    p.weapon_switch_bias = 0.80f;
    p.min_weapon_switch_cooldown_ms = 3800;
    p.taunt_on_kill_chance = 0.02f;

    def.preferred_weapon_names[0] = "Sniper Rifle";
    def.preferred_weapon_names[1] = "rail_gun";
    def.preferred_weapon_names[2] = "Assault Rifle";
    def.preferred_weapon_count = 3;

    def.weapon_overrides[0] = {"Sniper Rifle", 1.45f, 1.50f, 1.0f};
    def.weapon_overrides[1] = {"rail_gun", 1.40f, 1.40f, 1.0f};
    def.weapon_overrides[2] = {"Assault Rifle", 1.20f, 1.15f, 1.0f};
    def.weapon_overrides[3] = {"Shotgun", 0.75f, 0.70f, 1.0f};
    def.weapon_overrides[4] = {"Flamethrower", 0.65f, 0.55f, 1.0f};
    def.weapon_override_count = 5;

    return def;
}

PersonalityPresetDef make_tactician_preset()
{
    PersonalityPresetDef def{};
    auto& p = def.personality;
    p.name = "tactician";
    p.preferred_engagement_near = 7.0f;
    p.preferred_engagement_far = 15.0f;
    p.decision_aggression_bias = 0.90f;
    p.decision_efficiency_bias = 1.50f;
    p.decision_risk_tolerance = 0.85f;
    p.raw_aggression_bias = 0.85f;
    p.super_pickup_bias = 1.65f;
    p.revisit_avoidance_bias = 0.70f;
    p.path_smoothing_bias = 1.30f;
    p.power_position_bias = 1.70f;
    p.goal_commitment_bias = 1.25f;
    p.retreat_health_threshold = 0.38f;
    p.retreat_armor_threshold = 0.28f;
    p.replenish_health_threshold = 0.80f;
    p.replenish_armor_threshold = 0.55f;
    p.seek_weapon_bias = 1.25f;
    p.satisfactory_weapon_threshold = 0.75f;
    p.weapon_switch_bias = 1.20f;
    p.min_weapon_switch_cooldown_ms = 2600;
    p.crouch_combat_bias = 1.15f;
    p.dodge_combat_bias = 1.15f;
    p.camping_bias = 1.25f;
    p.deathmatch_kill_focus_bias = 0.90f;
    p.ctf_capture_priority_bias = 1.20f;
    p.taunt_on_kill_chance = 0.04f;
    p.quirk_mask = static_cast<uint64_t>(BotPersonalityQuirk::super_item_hoarder);

    def.preferred_weapon_names[0] = "rail_gun";
    def.preferred_weapon_names[1] = "Rocket Launcher";
    def.preferred_weapon_names[2] = "Assault Rifle";
    def.preferred_weapon_count = 3;

    def.weapon_overrides[0] = {"rail_gun", 1.35f, 1.40f, 1.0f};
    def.weapon_overrides[1] = {"Rocket Launcher", 1.25f, 1.20f, 1.0f};
    def.weapon_overrides[2] = {"shoulder_cannon", 1.30f, 1.25f, 1.0f};
    def.weapon_overrides[3] = {"heavy_machine_gun", 1.15f, 1.10f, 1.0f};
    def.weapon_override_count = 4;

    return def;
}

PersonalityPresetDef make_berserker_preset()
{
    PersonalityPresetDef def{};
    auto& p = def.personality;
    p.name = "berserker";
    p.attack_style = BotAttackStyle::aggressive;
    p.preferred_engagement_near = 2.5f;
    p.preferred_engagement_far = 7.0f;
    p.decision_aggression_bias = 2.00f;
    p.decision_risk_tolerance = 2.00f;
    p.decision_efficiency_bias = 0.60f;
    p.raw_aggression_bias = 2.20f;
    p.retreat_health_threshold = 0.10f;
    p.retreat_armor_threshold = 0.05f;
    p.replenish_health_threshold = 0.40f;
    p.replenish_armor_threshold = 0.20f;
    p.goal_commitment_bias = 0.55f;
    p.eliminate_target_commitment_bias = 1.60f;
    p.opportunism_bias = 1.70f;
    p.easy_frag_bias = 1.80f;
    p.retaliation_bias = 2.00f;
    p.crouch_combat_bias = 0.20f;
    p.jump_combat_bias = 0.70f;
    p.dodge_combat_bias = 0.70f;
    p.camping_bias = 0.10f;
    p.power_position_bias = 0.40f;
    p.combat_readiness_threshold = 0.25f;
    p.weapon_switch_bias = 1.50f;
    p.min_weapon_switch_cooldown_ms = 1800;
    p.taunt_on_kill_chance = 0.35f;
    p.gg_on_map_end_chance = 0.20f;
    p.red_faction_response_chance = 0.90f;
    p.quirk_mask =
        static_cast<uint64_t>(BotPersonalityQuirk::melee_finisher)
        | static_cast<uint64_t>(BotPersonalityQuirk::shotgun_low_health_finisher)
        | static_cast<uint64_t>(BotPersonalityQuirk::spawn_hunter);

    def.preferred_weapon_names[0] = "Shotgun";
    def.preferred_weapon_names[1] = "Flamethrower";
    def.preferred_weapon_names[2] = "Rocket Launcher";
    def.preferred_weapon_count = 3;

    def.weapon_overrides[0] = {"Shotgun", 1.70f, 1.80f, 1.0f};
    def.weapon_overrides[1] = {"Flamethrower", 1.50f, 1.45f, 1.0f};
    def.weapon_overrides[2] = {"Rocket Launcher", 1.30f, 1.20f, 1.0f};
    def.weapon_overrides[3] = {"Sniper Rifle", 0.50f, 0.40f, 1.0f};
    def.weapon_overrides[4] = {"rail_gun", 0.60f, 0.50f, 1.0f};
    def.weapon_override_count = 5;

    return def;
}

PersonalityPresetDef make_objective_preset()
{
    PersonalityPresetDef def{};
    auto& p = def.personality;
    p.name = "objective";
    p.attack_style = BotAttackStyle::evasive;
    p.preferred_engagement_near = 6.0f;
    p.preferred_engagement_far = 12.0f;
    p.decision_aggression_bias = 0.80f;
    p.decision_efficiency_bias = 1.25f;
    p.decision_risk_tolerance = 0.75f;
    p.raw_aggression_bias = 0.70f;
    p.retreat_health_threshold = 0.40f;
    p.retreat_armor_threshold = 0.30f;
    p.replenish_health_threshold = 0.82f;
    p.replenish_armor_threshold = 0.55f;
    p.goal_commitment_bias = 1.50f;
    p.eliminate_target_commitment_bias = 0.60f;
    p.opportunism_bias = 0.50f;
    p.super_pickup_bias = 1.30f;
    p.deathmatch_kill_focus_bias = 0.60f;
    p.ctf_capture_priority_bias = 1.70f;
    p.ctf_flag_recovery_bias = 1.55f;
    p.ctf_hold_enemy_flag_safety_bias = 1.60f;
    p.ctf_hold_carrier_hunt_bias = 0.70f;
    p.navigation_strafe_bias = 1.10f;
    p.roam_intensity_bias = 1.30f;
    p.camping_bias = 0.50f;
    p.taunt_on_kill_chance = 0.02f;
    p.quirk_mask =
        static_cast<uint64_t>(BotPersonalityQuirk::sneaky_capper)
        | static_cast<uint64_t>(BotPersonalityQuirk::jump_pad_pathing_affinity);

    def.preferred_weapon_names[0] = "Assault Rifle";
    def.preferred_weapon_names[1] = "Rocket Launcher";
    def.preferred_weapon_count = 2;

    return def;
}

PersonalityPresetDef make_sniper_preset()
{
    PersonalityPresetDef def{};
    auto& p = def.personality;
    p.name = "sniper";
    p.attack_style = BotAttackStyle::evasive;
    p.preferred_engagement_near = 12.0f;
    p.preferred_engagement_far = 25.0f;
    p.decision_aggression_bias = 0.70f;
    p.decision_efficiency_bias = 1.40f;
    p.decision_risk_tolerance = 0.60f;
    p.raw_aggression_bias = 0.50f;
    p.retreat_health_threshold = 0.45f;
    p.retreat_armor_threshold = 0.32f;
    p.replenish_health_threshold = 0.85f;
    p.replenish_armor_threshold = 0.58f;
    p.goal_commitment_bias = 1.15f;
    p.eliminate_target_commitment_bias = 0.85f;
    p.crouch_combat_bias = 1.60f;
    p.dodge_combat_bias = 0.80f;
    p.camping_bias = 1.80f;
    p.power_position_bias = 2.00f;
    p.seek_weapon_bias = 1.40f;
    p.satisfactory_weapon_threshold = 0.80f;
    p.weapon_switch_bias = 0.65f;
    p.min_weapon_switch_cooldown_ms = 4200;
    p.combat_readiness_threshold = 0.60f;
    p.taunt_on_kill_chance = 0.08f;
    p.quirk_mask = static_cast<uint64_t>(BotPersonalityQuirk::railgun_no_los_hunter);

    def.preferred_weapon_names[0] = "rail_gun";
    def.preferred_weapon_names[1] = "Sniper Rifle";
    def.preferred_weapon_names[2] = "scope_assault_rifle";
    def.preferred_weapon_count = 3;

    def.weapon_overrides[0] = {"rail_gun", 1.70f, 1.80f, 1.15f};
    def.weapon_overrides[1] = {"Sniper Rifle", 1.60f, 1.65f, 1.10f};
    def.weapon_overrides[2] = {"scope_assault_rifle", 1.40f, 1.40f, 1.05f};
    def.weapon_overrides[3] = {"Shotgun", 0.55f, 0.45f, 0.85f};
    def.weapon_overrides[4] = {"Flamethrower", 0.45f, 0.35f, 0.80f};
    def.weapon_overrides[5] = {"Rocket Launcher", 0.80f, 0.75f, 0.90f};
    def.weapon_override_count = 6;

    return def;
}

PersonalityPresetDef make_guerrilla_preset()
{
    PersonalityPresetDef def{};
    auto& p = def.personality;
    p.name = "guerrilla";
    p.attack_style = BotAttackStyle::evasive;
    p.preferred_engagement_near = 5.5f;
    p.preferred_engagement_far = 14.0f;
    p.decision_aggression_bias = 1.10f;
    p.decision_risk_tolerance = 0.75f;
    p.decision_efficiency_bias = 1.15f;
    p.raw_aggression_bias = 0.90f;
    p.retreat_health_threshold = 0.45f;
    p.retreat_armor_threshold = 0.35f;
    p.replenish_health_threshold = 0.80f;
    p.replenish_armor_threshold = 0.50f;
    p.goal_commitment_bias = 0.65f;
    p.eliminate_target_commitment_bias = 0.55f;
    p.opportunism_bias = 1.50f;
    p.easy_frag_bias = 1.45f;
    p.retaliation_bias = 0.60f;
    p.crouch_combat_bias = 0.60f;
    p.jump_combat_bias = 0.30f;
    p.dodge_combat_bias = 1.50f;
    p.camping_bias = 0.20f;
    p.power_position_bias = 0.60f;
    p.roam_intensity_bias = 1.50f;
    p.navigation_strafe_bias = 1.20f;
    p.retrace_avoidance_bias = 1.40f;
    p.revisit_avoidance_bias = 1.30f;
    p.combat_readiness_threshold = 0.58f;
    p.weapon_switch_bias = 1.15f;
    p.min_weapon_switch_cooldown_ms = 2000;
    p.taunt_on_kill_chance = 0.10f;

    def.preferred_weapon_names[0] = "Rocket Launcher";
    def.preferred_weapon_names[1] = "Shotgun";
    def.preferred_weapon_names[2] = "Grenade";
    def.preferred_weapon_count = 3;

    def.weapon_overrides[0] = {"Rocket Launcher", 1.35f, 1.30f, 1.0f};
    def.weapon_overrides[1] = {"Shotgun", 1.25f, 1.30f, 1.0f};
    def.weapon_overrides[2] = {"Grenade", 1.40f, 1.35f, 1.0f};
    def.weapon_override_count = 3;

    return def;
}

PersonalityPresetDef make_collector_preset()
{
    PersonalityPresetDef def{};
    auto& p = def.personality;
    p.name = "collector";
    p.attack_style = BotAttackStyle::evasive;
    p.preferred_engagement_near = 7.0f;
    p.preferred_engagement_far = 14.0f;
    p.decision_aggression_bias = 0.55f;
    p.decision_efficiency_bias = 1.40f;
    p.decision_risk_tolerance = 0.50f;
    p.raw_aggression_bias = 0.45f;
    p.retreat_health_threshold = 0.50f;
    p.retreat_armor_threshold = 0.40f;
    p.replenish_health_threshold = 0.92f;
    p.replenish_armor_threshold = 0.70f;
    p.replenish_bias = 1.70f;
    p.super_pickup_bias = 1.80f;
    p.revisit_avoidance_bias = 0.55f;
    p.seek_weapon_bias = 1.60f;
    p.satisfactory_weapon_threshold = 0.85f;
    p.goal_commitment_bias = 1.10f;
    p.eliminate_target_commitment_bias = 0.50f;
    p.opportunism_bias = 0.40f;
    p.easy_frag_bias = 0.50f;
    p.retaliation_bias = 0.50f;
    p.camping_bias = 0.40f;
    p.power_position_bias = 0.80f;
    p.combat_readiness_threshold = 0.72f;
    p.roam_intensity_bias = 1.40f;
    p.navigation_strafe_bias = 0.90f;
    p.weapon_switch_bias = 1.40f;
    p.min_weapon_switch_cooldown_ms = 2400;
    p.taunt_on_kill_chance = 0.02f;
    p.quirk_mask = static_cast<uint64_t>(BotPersonalityQuirk::super_item_hoarder);

    def.preferred_weapon_names[0] = "heavy_machine_gun";
    def.preferred_weapon_names[1] = "Assault Rifle";
    def.preferred_weapon_names[2] = "Rocket Launcher";
    def.preferred_weapon_count = 3;

    def.weapon_overrides[0] = {"heavy_machine_gun", 1.30f, 1.25f, 1.0f};
    def.weapon_overrides[1] = {"scope_assault_rifle", 1.20f, 1.20f, 1.0f};
    def.weapon_override_count = 2;

    return def;
}

PersonalityPresetDef make_brawler_preset()
{
    PersonalityPresetDef def{};
    auto& p = def.personality;
    p.name = "brawler";
    p.preferred_engagement_near = 5.0f;
    p.preferred_engagement_far = 12.0f;
    p.decision_aggression_bias = 1.20f;
    p.decision_risk_tolerance = 1.15f;
    p.decision_efficiency_bias = 1.05f;
    p.raw_aggression_bias = 1.15f;
    p.retreat_health_threshold = 0.28f;
    p.retreat_armor_threshold = 0.18f;
    p.replenish_health_threshold = 0.68f;
    p.replenish_armor_threshold = 0.40f;
    p.goal_commitment_bias = 1.05f;
    p.eliminate_target_commitment_bias = 1.25f;
    p.opportunism_bias = 1.10f;
    p.retaliation_bias = 1.30f;
    p.crouch_combat_bias = 0.90f;
    p.jump_combat_bias = 0.20f;
    p.dodge_combat_bias = 1.30f;
    p.camping_bias = 0.50f;
    p.weapon_switch_bias = 1.35f;
    p.min_weapon_switch_cooldown_ms = 2000;
    p.combat_readiness_threshold = 0.42f;
    p.deathmatch_kill_focus_bias = 1.20f;
    p.taunt_on_kill_chance = 0.12f;

    def.preferred_weapon_names[0] = "Assault Rifle";
    def.preferred_weapon_names[1] = "heavy_machine_gun";
    def.preferred_weapon_names[2] = "Shotgun";
    def.preferred_weapon_count = 3;

    def.weapon_overrides[0] = {"Assault Rifle", 1.20f, 1.25f, 1.0f};
    def.weapon_overrides[1] = {"heavy_machine_gun", 1.25f, 1.20f, 1.0f};
    def.weapon_overrides[2] = {"Shotgun", 1.15f, 1.20f, 1.0f};
    def.weapon_overrides[3] = {"Rocket Launcher", 1.10f, 1.10f, 1.0f};
    def.weapon_override_count = 4;

    return def;
}

PersonalityPresetDef make_destroyer_preset()
{
    PersonalityPresetDef def{};
    auto& p = def.personality;
    p.name = "destroyer";
    p.attack_style = BotAttackStyle::aggressive;
    p.preferred_engagement_near = 5.0f;
    p.preferred_engagement_far = 11.0f;
    p.decision_aggression_bias = 1.30f;
    p.decision_risk_tolerance = 1.25f;
    p.decision_efficiency_bias = 0.80f;
    p.raw_aggression_bias = 1.35f;
    p.retreat_health_threshold = 0.22f;
    p.retreat_armor_threshold = 0.15f;
    p.replenish_health_threshold = 0.60f;
    p.replenish_armor_threshold = 0.35f;
    p.goal_commitment_bias = 1.40f;
    p.eliminate_target_commitment_bias = 0.85f;
    p.retaliation_bias = 1.20f;
    p.crouch_combat_bias = 0.50f;
    p.jump_combat_bias = 0.30f;
    p.camping_bias = 0.30f;
    p.power_position_bias = 0.50f;
    p.weapon_switch_bias = 1.10f;
    p.min_weapon_switch_cooldown_ms = 2600;
    p.combat_readiness_threshold = 0.40f;
    p.taunt_on_kill_chance = 0.15f;
    p.red_faction_response_chance = 0.80f;
    p.quirk_mask = static_cast<uint64_t>(BotPersonalityQuirk::crater_unlock_affinity);

    def.preferred_weapon_names[0] = "Rocket Launcher";
    def.preferred_weapon_names[1] = "shoulder_cannon";
    def.preferred_weapon_names[2] = "Grenade";
    def.preferred_weapon_names[3] = "Remote Charge";
    def.preferred_weapon_count = 4;

    def.weapon_overrides[0] = {"Rocket Launcher", 1.60f, 1.55f, 1.0f};
    def.weapon_overrides[1] = {"shoulder_cannon", 1.55f, 1.50f, 1.0f};
    def.weapon_overrides[2] = {"Grenade", 1.45f, 1.40f, 1.0f};
    def.weapon_overrides[3] = {"Remote Charge", 1.50f, 1.45f, 1.0f};
    def.weapon_overrides[4] = {"Flamethrower", 1.20f, 1.15f, 1.0f};
    def.weapon_overrides[5] = {"Sniper Rifle", 0.60f, 0.55f, 1.0f};
    def.weapon_overrides[6] = {"rail_gun", 0.80f, 0.75f, 1.0f};
    def.weapon_override_count = 7;

    return def;
}

// ---------------------------------------------------------------------------
// Skill preset factory functions
// ---------------------------------------------------------------------------

SkillPresetDef make_beginner_skill()
{
    SkillPresetDef def{};
    auto& s = def.profile;
    s.name = "beginner";
    s.base_skill = 0.15f;
    s.aim_profile_scale = 0.70f;
    s.decision_profile_scale = 0.65f;
    s.survivability_maintenance_bias = 0.55f;
    s.alertness = 0.40f;
    s.fov_degrees = 85.0f;
    s.target_focus_bias = 0.55f;
    s.weapon_switch_likelihood = 0.15f;
    s.crouch_likelihood = 0.04f;
    s.jump_likelihood = 0.03f;
    s.dodge_likelihood = 0.04f;
    return def;
}

SkillPresetDef make_easy_skill()
{
    SkillPresetDef def{};
    auto& s = def.profile;
    s.name = "easy";
    s.base_skill = 0.35f;
    s.aim_profile_scale = 0.85f;
    s.decision_profile_scale = 0.80f;
    s.survivability_maintenance_bias = 0.75f;
    s.alertness = 0.60f;
    s.fov_degrees = 100.0f;
    s.target_focus_bias = 0.70f;
    s.weapon_switch_likelihood = 0.28f;
    s.crouch_likelihood = 0.07f;
    s.jump_likelihood = 0.06f;
    s.dodge_likelihood = 0.07f;
    return def;
}

SkillPresetDef make_average_skill()
{
    SkillPresetDef def{};
    def.profile.name = "average";
    // All other fields match kDefaultBotSkillProfile
    return def;
}

SkillPresetDef make_hard_skill()
{
    SkillPresetDef def{};
    auto& s = def.profile;
    s.name = "hard";
    s.base_skill = 0.82f;
    s.aim_profile_scale = 1.10f;
    s.decision_profile_scale = 1.15f;
    s.survivability_maintenance_bias = 1.25f;
    s.alertness = 1.10f;
    s.fov_degrees = 130.0f;
    s.target_focus_bias = 1.10f;
    s.weapon_switch_likelihood = 0.60f;
    s.crouch_likelihood = 0.18f;
    s.jump_likelihood = 0.18f;
    s.dodge_likelihood = 0.20f;
    return def;
}

SkillPresetDef make_expert_skill()
{
    SkillPresetDef def{};
    auto& s = def.profile;
    s.name = "expert";
    s.base_skill = 0.92f;
    s.aim_profile_scale = 1.20f;
    s.decision_profile_scale = 1.25f;
    s.survivability_maintenance_bias = 1.50f;
    s.alertness = 1.20f;
    s.fov_degrees = 140.0f;
    s.target_focus_bias = 1.25f;
    s.weapon_switch_likelihood = 0.72f;
    s.crouch_likelihood = 0.22f;
    s.jump_likelihood = 0.22f;
    s.dodge_likelihood = 0.26f;
    return def;
}

SkillPresetDef make_nightmare_skill()
{
    SkillPresetDef def{};
    auto& s = def.profile;
    s.name = "nightmare";
    s.base_skill = 1.0f;
    s.aim_profile_scale = 1.35f;
    s.decision_profile_scale = 1.40f;
    s.survivability_maintenance_bias = 1.75f;
    s.alertness = 1.35f;
    s.fov_degrees = 150.0f;
    s.target_focus_bias = 1.40f;
    s.weapon_switch_likelihood = 0.85f;
    s.crouch_likelihood = 0.25f;
    s.jump_likelihood = 0.25f;
    s.dodge_likelihood = 0.30f;
    return def;
}

// ---------------------------------------------------------------------------
// Preset registry
// ---------------------------------------------------------------------------

struct PresetRegistry
{
    PersonalityPresetDef personalities[11];
    int personality_count;
    SkillPresetDef skills[6];
    int skill_count;
};

const PresetRegistry& get_preset_registry()
{
    static const PresetRegistry registry = [] {
        PresetRegistry r{};
        r.personalities[0] = make_balanced_preset();
        r.personalities[1] = make_aggressive_preset();
        r.personalities[2] = make_defensive_preset();
        r.personalities[3] = make_tactician_preset();
        r.personalities[4] = make_berserker_preset();
        r.personalities[5] = make_objective_preset();
        r.personalities[6] = make_sniper_preset();
        r.personalities[7] = make_guerrilla_preset();
        r.personalities[8] = make_collector_preset();
        r.personalities[9] = make_brawler_preset();
        r.personalities[10] = make_destroyer_preset();
        r.personality_count = 11;
        r.skills[0] = make_average_skill();
        r.skills[1] = make_beginner_skill();
        r.skills[2] = make_easy_skill();
        r.skills[3] = make_hard_skill();
        r.skills[4] = make_expert_skill();
        r.skills[5] = make_nightmare_skill();
        r.skill_count = 6;
        return r;
    }();
    return registry;
}

const PersonalityPresetDef* find_personality_preset(std::string_view name)
{
    const auto& reg = get_preset_registry();
    for (int i = 0; i < reg.personality_count; ++i) {
        if (strings_equal_case_insensitive(name, reg.personalities[i].personality.name)) {
            return &reg.personalities[i];
        }
    }
    return nullptr;
}

const SkillPresetDef* find_skill_preset(std::string_view name)
{
    const auto& reg = get_preset_registry();
    for (int i = 0; i < reg.skill_count; ++i) {
        if (strings_equal_case_insensitive(name, reg.skills[i].profile.name)) {
            return &reg.skills[i];
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Weight initialization from game data
// ---------------------------------------------------------------------------

void initialize_weights_from_game_data()
{
    const int item_count = std::max(rf::num_item_types, 0);
    const int weapon_count = std::max(rf::num_weapon_types, 0);
    g_active_bot_personality.item_weights.assign(static_cast<size_t>(item_count), 1.0f);
    g_active_bot_personality.weapon_pickup_weights.assign(static_cast<size_t>(weapon_count), 1.0f);
    g_active_bot_personality.weapon_preference_weights.assign(static_cast<size_t>(weapon_count), 1.0f);
    g_active_bot_skill_profile.weapon_skill_weights.assign(static_cast<size_t>(weapon_count), 1.0f);

    for (int item_type = 0; item_type < item_count; ++item_type) {
        const rf::ItemInfo& item = rf::item_info[item_type];
        float weight = 1.0f;
        if (item.gives_weapon_id >= 0) {
            weight += 0.35f;
        }
        if (item.ammo_for_weapon_id >= 0) {
            weight += 0.15f;
        }
        if (contains_case_insensitive(item.cls_name.c_str(), "health")
            || contains_case_insensitive(item.cls_name.c_str(), "med")
            || contains_case_insensitive(item.cls_name.c_str(), "armor")
            || (item.hud_msg_name
                && (contains_case_insensitive(item.hud_msg_name, "health")
                    || contains_case_insensitive(item.hud_msg_name, "med")
                    || contains_case_insensitive(item.hud_msg_name, "armor")))) {
            weight += 0.45f;
        }
        g_active_bot_personality.item_weights[static_cast<size_t>(item_type)] = weight;
    }

    for (int weapon_type = 0; weapon_type < weapon_count; ++weapon_type) {
        const rf::WeaponInfo& weapon = rf::weapon_types[weapon_type];
        const float damage = std::max(weapon.damage, weapon.damage_multi);
        const float normalized_damage = std::clamp(damage / 70.0f, 0.0f, 1.5f);
        g_active_bot_personality.weapon_pickup_weights[static_cast<size_t>(weapon_type)] =
            std::clamp(0.85f + normalized_damage * 0.35f, 0.5f, 2.5f);
        g_active_bot_personality.weapon_preference_weights[static_cast<size_t>(weapon_type)] =
            std::clamp(0.90f + normalized_damage * 0.30f, 0.5f, 2.5f);
        g_active_bot_skill_profile.weapon_skill_weights[static_cast<size_t>(weapon_type)] = 1.0f;
    }
}

void resolve_preferred_weapons(const PersonalityPresetDef& preset)
{
    g_active_bot_personality.preferred_weapon_types.clear();
    for (int i = 0; i < preset.preferred_weapon_count; ++i) {
        const char* name = preset.preferred_weapon_names[i];
        if (!name) break;
        const int weapon_type = rf::weapon_lookup_type(name);
        if (weapon_type >= 0) {
            g_active_bot_personality.preferred_weapon_types.push_back(weapon_type);
        }
    }
}

void apply_weapon_overrides(const PersonalityPresetDef& preset)
{
    const int weapon_count = static_cast<int>(g_active_bot_personality.weapon_pickup_weights.size());
    for (int i = 0; i < preset.weapon_override_count; ++i) {
        const auto& ovr = preset.weapon_overrides[i];
        if (!ovr.weapon_name) break;
        const int weapon_type = rf::weapon_lookup_type(ovr.weapon_name);
        if (weapon_type < 0 || weapon_type >= weapon_count) continue;
        const auto idx = static_cast<size_t>(weapon_type);
        g_active_bot_personality.weapon_pickup_weights[idx] =
            std::clamp(g_active_bot_personality.weapon_pickup_weights[idx] * ovr.pickup_weight, 0.05f, 8.0f);
        g_active_bot_personality.weapon_preference_weights[idx] =
            std::clamp(g_active_bot_personality.weapon_preference_weights[idx] * ovr.preference_weight, 0.05f, 8.0f);
        g_active_bot_skill_profile.weapon_skill_weights[idx] =
            std::clamp(g_active_bot_skill_profile.weapon_skill_weights[idx] * ovr.skill_weight, 0.05f, 8.0f);
    }
}

// ---------------------------------------------------------------------------
// Main initialization
// ---------------------------------------------------------------------------

void ensure_profile_weights_initialized()
{
    if (g_profiles_initialized) {
        return;
    }
    g_profiles_initialized = true;

    const auto& reg = get_preset_registry();

    // Select personality preset
    const PersonalityPresetDef* personality_preset = nullptr;
    const std::string& personality_name = g_alpine_game_config.bot_personality_preset;
    if (strings_equal_case_insensitive(personality_name, "random")) {
        std::uniform_int_distribution<int> dist(0, reg.personality_count - 1);
        personality_preset = &reg.personalities[dist(g_rng)];
    }
    else {
        personality_preset = find_personality_preset(personality_name);
        if (!personality_preset) {
            xlog::warn("Bot personality preset '{}' not found, falling back to 'balanced'", personality_name);
            personality_preset = &reg.personalities[0];
        }
    }

    // Select skill preset
    const SkillPresetDef* skill_preset = nullptr;
    const std::string& skill_name = g_alpine_game_config.bot_skill_preset;
    if (strings_equal_case_insensitive(skill_name, "random")) {
        std::uniform_int_distribution<int> dist(0, reg.skill_count - 1);
        skill_preset = &reg.skills[dist(g_rng)];
    }
    else {
        skill_preset = find_skill_preset(skill_name);
        if (!skill_preset) {
            xlog::warn("Bot skill preset '{}' not found, falling back to 'average'", skill_name);
            skill_preset = &reg.skills[0];
        }
    }

    // Apply selected presets (scalar fields only; vectors populated below)
    g_active_bot_personality = personality_preset->personality;
    g_active_bot_skill_profile = skill_preset->profile;

    // Initialize dynamic weight vectors from game data
    initialize_weights_from_game_data();

    // Resolve weapon names to type indices and apply preset overrides
    resolve_preferred_weapons(*personality_preset);
    apply_weapon_overrides(*personality_preset);

    xlog::info("Bot profile: personality={}, skill={}",
        g_active_bot_personality.name, g_active_bot_skill_profile.name);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

const BotPersonality& get_active_bot_personality()
{
    ensure_profile_weights_initialized();
    return g_active_bot_personality;
}

const BotSkillProfile& get_active_bot_skill_profile()
{
    ensure_profile_weights_initialized();
    return g_active_bot_skill_profile;
}

bool bot_personality_has_quirk(const BotPersonalityQuirk quirk)
{
    ensure_profile_weights_initialized();
    const uint64_t bit = static_cast<uint64_t>(quirk);
    if (bit == 0) {
        return false;
    }
    return (g_active_bot_personality.quirk_mask & bit) != 0;
}

float bot_get_item_weight(const int item_type)
{
    ensure_profile_weights_initialized();
    return read_weight_or_default(g_active_bot_personality.item_weights, item_type);
}

float bot_get_weapon_pickup_weight(const int weapon_type)
{
    ensure_profile_weights_initialized();
    return read_weight_or_default(g_active_bot_personality.weapon_pickup_weights, weapon_type);
}

float bot_get_weapon_preference_weight(const int weapon_type)
{
    ensure_profile_weights_initialized();
    return read_weight_or_default(g_active_bot_personality.weapon_preference_weights, weapon_type);
}

float bot_get_weapon_skill_weight(const int weapon_type)
{
    ensure_profile_weights_initialized();
    return read_weight_or_default(g_active_bot_skill_profile.weapon_skill_weights, weapon_type);
}

float bot_get_skill_factor()
{
    const float base_skill = std::clamp(get_active_bot_skill_profile().base_skill, 0.0f, 1.0f);
    const float profile_scale = std::clamp(
        get_active_bot_skill_profile().aim_profile_scale,
        0.25f,
        2.0f
    );
    return std::clamp(base_skill * profile_scale, 0.0f, 1.0f);
}

float bot_get_decision_skill_factor()
{
    // Decision quality rises faster at lower levels than pure aim precision.
    const float skill = bot_get_skill_factor();
    const float base_decision = std::clamp(std::pow(skill, 0.75f), 0.0f, 1.0f);
    const float profile_scale = std::clamp(
        get_active_bot_skill_profile().decision_profile_scale,
        0.25f,
        2.0f
    );
    return std::clamp(base_decision * profile_scale, 0.0f, 1.0f);
}

void bot_personality_set_presets(const char* personality_name, const char* skill_name)
{
    if (personality_name) {
        g_alpine_game_config.bot_personality_preset = personality_name;
    }
    if (skill_name) {
        g_alpine_game_config.bot_skill_preset = skill_name;
    }
    g_profiles_initialized = false;
    ensure_profile_weights_initialized();
}

BotPersonality& bot_personality_get_mutable()
{
    ensure_profile_weights_initialized();
    return g_active_bot_personality;
}

BotSkillProfile& bot_skill_profile_get_mutable()
{
    ensure_profile_weights_initialized();
    return g_active_bot_skill_profile;
}

// ---------------------------------------------------------------------------
// Field override by ID
// ---------------------------------------------------------------------------

bool bot_personality_apply_field_override(uint8_t field_id, float value)
{
    auto& p = bot_personality_get_mutable();
    switch (static_cast<af_personality_field>(field_id)) {
        case af_personality_field::attack_style:
            p.attack_style = static_cast<BotAttackStyle>(static_cast<int>(value));
            return true;
        case af_personality_field::preferred_engagement_near:  p.preferred_engagement_near = value; return true;
        case af_personality_field::preferred_engagement_far:   p.preferred_engagement_far = value; return true;
        case af_personality_field::super_pickup_bias:          p.super_pickup_bias = value; return true;
        case af_personality_field::revisit_avoidance_bias:     p.revisit_avoidance_bias = value; return true;
        case af_personality_field::retrace_avoidance_bias:     p.retrace_avoidance_bias = value; return true;
        case af_personality_field::retrace_lookback_waypoints: p.retrace_lookback_waypoints = static_cast<int>(value); return true;
        case af_personality_field::decision_aggression_bias:   p.decision_aggression_bias = value; return true;
        case af_personality_field::decision_efficiency_bias:   p.decision_efficiency_bias = value; return true;
        case af_personality_field::decision_risk_tolerance:    p.decision_risk_tolerance = value; return true;
        case af_personality_field::path_smoothing_bias:        p.path_smoothing_bias = value; return true;
        case af_personality_field::corner_centering_bias:      p.corner_centering_bias = value; return true;
        case af_personality_field::roam_intensity_bias:        p.roam_intensity_bias = value; return true;
        case af_personality_field::navigation_strafe_bias:     p.navigation_strafe_bias = value; return true;
        case af_personality_field::crouch_route_avoidance_bias: p.crouch_route_avoidance_bias = value; return true;
        case af_personality_field::stuck_goal_retry_limit:     p.stuck_goal_retry_limit = static_cast<int>(value); return true;
        case af_personality_field::goal_commitment_bias:       p.goal_commitment_bias = value; return true;
        case af_personality_field::eliminate_target_commitment_bias: p.eliminate_target_commitment_bias = value; return true;
        case af_personality_field::opportunism_bias:           p.opportunism_bias = value; return true;
        case af_personality_field::retreat_health_threshold:   p.retreat_health_threshold = value; return true;
        case af_personality_field::retreat_armor_threshold:    p.retreat_armor_threshold = value; return true;
        case af_personality_field::replenish_health_threshold: p.replenish_health_threshold = value; return true;
        case af_personality_field::replenish_armor_threshold:  p.replenish_armor_threshold = value; return true;
        case af_personality_field::seek_weapon_bias:           p.seek_weapon_bias = value; return true;
        case af_personality_field::satisfactory_weapon_threshold: p.satisfactory_weapon_threshold = value; return true;
        case af_personality_field::preferred_weapon_ammo_fill_threshold: p.preferred_weapon_ammo_fill_threshold = value; return true;
        case af_personality_field::replenish_bias:             p.replenish_bias = value; return true;
        case af_personality_field::power_position_bias:        p.power_position_bias = value; return true;
        case af_personality_field::weapon_switch_bias:         p.weapon_switch_bias = value; return true;
        case af_personality_field::min_weapon_switch_cooldown_ms: p.min_weapon_switch_cooldown_ms = static_cast<int>(value); return true;
        case af_personality_field::crouch_combat_bias:         p.crouch_combat_bias = value; return true;
        case af_personality_field::jump_combat_bias:           p.jump_combat_bias = value; return true;
        case af_personality_field::dodge_combat_bias:          p.dodge_combat_bias = value; return true;
        case af_personality_field::raw_aggression_bias:        p.raw_aggression_bias = value; return true;
        case af_personality_field::camping_bias:               p.camping_bias = value; return true;
        case af_personality_field::easy_frag_bias:             p.easy_frag_bias = value; return true;
        case af_personality_field::retaliation_bias:           p.retaliation_bias = value; return true;
        case af_personality_field::combat_readiness_threshold: p.combat_readiness_threshold = value; return true;
        case af_personality_field::deathmatch_kill_focus_bias: p.deathmatch_kill_focus_bias = value; return true;
        case af_personality_field::ctf_capture_priority_bias:  p.ctf_capture_priority_bias = value; return true;
        case af_personality_field::ctf_flag_recovery_bias:     p.ctf_flag_recovery_bias = value; return true;
        case af_personality_field::ctf_hold_enemy_flag_safety_bias: p.ctf_hold_enemy_flag_safety_bias = value; return true;
        case af_personality_field::ctf_hold_carrier_hunt_bias: p.ctf_hold_carrier_hunt_bias = value; return true;
        case af_personality_field::quirk_mask_low: {
            uint32_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            p.quirk_mask = (p.quirk_mask & 0xFFFFFFFF00000000ull) | bits;
            return true;
        }
        case af_personality_field::quirk_mask_high: {
            uint32_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            p.quirk_mask = (p.quirk_mask & 0x00000000FFFFFFFFull) | (static_cast<uint64_t>(bits) << 32);
            return true;
        }
        case af_personality_field::pickup_switch_chance_without_preferences: p.pickup_switch_chance_without_preferences = value; return true;
        case af_personality_field::taunt_on_kill_chance:       p.taunt_on_kill_chance = value; return true;
        case af_personality_field::gg_on_map_end_chance:       p.gg_on_map_end_chance = value; return true;
        case af_personality_field::hello_on_join_chance:       p.hello_on_join_chance = value; return true;
        case af_personality_field::red_faction_response_chance: p.red_faction_response_chance = value; return true;
        default:
            return false;
    }
}

bool bot_skill_apply_field_override(uint8_t field_id, float value)
{
    auto& s = bot_skill_profile_get_mutable();
    switch (static_cast<af_skill_field>(field_id)) {
        case af_skill_field::base_skill:                    s.base_skill = value; return true;
        case af_skill_field::aim_profile_scale:             s.aim_profile_scale = value; return true;
        case af_skill_field::decision_profile_scale:        s.decision_profile_scale = value; return true;
        case af_skill_field::survivability_maintenance_bias: s.survivability_maintenance_bias = value; return true;
        case af_skill_field::alertness:                     s.alertness = value; return true;
        case af_skill_field::fov_degrees:                   s.fov_degrees = value; return true;
        case af_skill_field::target_focus_bias:             s.target_focus_bias = value; return true;
        case af_skill_field::weapon_switch_likelihood:      s.weapon_switch_likelihood = value; return true;
        case af_skill_field::crouch_likelihood:             s.crouch_likelihood = value; return true;
        case af_skill_field::jump_likelihood:               s.jump_likelihood = value; return true;
        case af_skill_field::dodge_likelihood:              s.dodge_likelihood = value; return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// Name-to-field-ID mapping
// ---------------------------------------------------------------------------

struct FieldNameMapping {
    const char* name;
    uint8_t field_id;
};

static const FieldNameMapping g_personality_field_map[] = {
    {"attack_style",                          static_cast<uint8_t>(af_personality_field::attack_style)},
    {"preferred_engagement_near",             static_cast<uint8_t>(af_personality_field::preferred_engagement_near)},
    {"preferred_engagement_far",              static_cast<uint8_t>(af_personality_field::preferred_engagement_far)},
    {"super_pickup_bias",                     static_cast<uint8_t>(af_personality_field::super_pickup_bias)},
    {"revisit_avoidance_bias",                static_cast<uint8_t>(af_personality_field::revisit_avoidance_bias)},
    {"retrace_avoidance_bias",                static_cast<uint8_t>(af_personality_field::retrace_avoidance_bias)},
    {"retrace_lookback_waypoints",            static_cast<uint8_t>(af_personality_field::retrace_lookback_waypoints)},
    {"decision_aggression_bias",              static_cast<uint8_t>(af_personality_field::decision_aggression_bias)},
    {"decision_efficiency_bias",              static_cast<uint8_t>(af_personality_field::decision_efficiency_bias)},
    {"decision_risk_tolerance",               static_cast<uint8_t>(af_personality_field::decision_risk_tolerance)},
    {"path_smoothing_bias",                   static_cast<uint8_t>(af_personality_field::path_smoothing_bias)},
    {"corner_centering_bias",                 static_cast<uint8_t>(af_personality_field::corner_centering_bias)},
    {"roam_intensity_bias",                   static_cast<uint8_t>(af_personality_field::roam_intensity_bias)},
    {"navigation_strafe_bias",                static_cast<uint8_t>(af_personality_field::navigation_strafe_bias)},
    {"crouch_route_avoidance_bias",           static_cast<uint8_t>(af_personality_field::crouch_route_avoidance_bias)},
    {"stuck_goal_retry_limit",                static_cast<uint8_t>(af_personality_field::stuck_goal_retry_limit)},
    {"goal_commitment_bias",                  static_cast<uint8_t>(af_personality_field::goal_commitment_bias)},
    {"eliminate_target_commitment_bias",       static_cast<uint8_t>(af_personality_field::eliminate_target_commitment_bias)},
    {"opportunism_bias",                      static_cast<uint8_t>(af_personality_field::opportunism_bias)},
    {"retreat_health_threshold",              static_cast<uint8_t>(af_personality_field::retreat_health_threshold)},
    {"retreat_armor_threshold",               static_cast<uint8_t>(af_personality_field::retreat_armor_threshold)},
    {"replenish_health_threshold",            static_cast<uint8_t>(af_personality_field::replenish_health_threshold)},
    {"replenish_armor_threshold",             static_cast<uint8_t>(af_personality_field::replenish_armor_threshold)},
    {"seek_weapon_bias",                      static_cast<uint8_t>(af_personality_field::seek_weapon_bias)},
    {"satisfactory_weapon_threshold",         static_cast<uint8_t>(af_personality_field::satisfactory_weapon_threshold)},
    {"preferred_weapon_ammo_fill_threshold",  static_cast<uint8_t>(af_personality_field::preferred_weapon_ammo_fill_threshold)},
    {"replenish_bias",                        static_cast<uint8_t>(af_personality_field::replenish_bias)},
    {"power_position_bias",                   static_cast<uint8_t>(af_personality_field::power_position_bias)},
    {"weapon_switch_bias",                    static_cast<uint8_t>(af_personality_field::weapon_switch_bias)},
    {"min_weapon_switch_cooldown_ms",         static_cast<uint8_t>(af_personality_field::min_weapon_switch_cooldown_ms)},
    {"crouch_combat_bias",                    static_cast<uint8_t>(af_personality_field::crouch_combat_bias)},
    {"jump_combat_bias",                      static_cast<uint8_t>(af_personality_field::jump_combat_bias)},
    {"dodge_combat_bias",                     static_cast<uint8_t>(af_personality_field::dodge_combat_bias)},
    {"raw_aggression_bias",                   static_cast<uint8_t>(af_personality_field::raw_aggression_bias)},
    {"camping_bias",                          static_cast<uint8_t>(af_personality_field::camping_bias)},
    {"easy_frag_bias",                        static_cast<uint8_t>(af_personality_field::easy_frag_bias)},
    {"retaliation_bias",                      static_cast<uint8_t>(af_personality_field::retaliation_bias)},
    {"combat_readiness_threshold",            static_cast<uint8_t>(af_personality_field::combat_readiness_threshold)},
    {"deathmatch_kill_focus_bias",            static_cast<uint8_t>(af_personality_field::deathmatch_kill_focus_bias)},
    {"ctf_capture_priority_bias",             static_cast<uint8_t>(af_personality_field::ctf_capture_priority_bias)},
    {"ctf_flag_recovery_bias",                static_cast<uint8_t>(af_personality_field::ctf_flag_recovery_bias)},
    {"ctf_hold_enemy_flag_safety_bias",       static_cast<uint8_t>(af_personality_field::ctf_hold_enemy_flag_safety_bias)},
    {"ctf_hold_carrier_hunt_bias",            static_cast<uint8_t>(af_personality_field::ctf_hold_carrier_hunt_bias)},
    {"quirk_mask_low",                        static_cast<uint8_t>(af_personality_field::quirk_mask_low)},
    {"quirk_mask_high",                       static_cast<uint8_t>(af_personality_field::quirk_mask_high)},
    {"pickup_switch_chance_without_preferences", static_cast<uint8_t>(af_personality_field::pickup_switch_chance_without_preferences)},
    {"taunt_on_kill_chance",                  static_cast<uint8_t>(af_personality_field::taunt_on_kill_chance)},
    {"gg_on_map_end_chance",                  static_cast<uint8_t>(af_personality_field::gg_on_map_end_chance)},
    {"hello_on_join_chance",                  static_cast<uint8_t>(af_personality_field::hello_on_join_chance)},
    {"red_faction_response_chance",           static_cast<uint8_t>(af_personality_field::red_faction_response_chance)},
};

static const FieldNameMapping g_skill_field_map[] = {
    {"base_skill",                    static_cast<uint8_t>(af_skill_field::base_skill)},
    {"aim_profile_scale",             static_cast<uint8_t>(af_skill_field::aim_profile_scale)},
    {"decision_profile_scale",        static_cast<uint8_t>(af_skill_field::decision_profile_scale)},
    {"survivability_maintenance_bias", static_cast<uint8_t>(af_skill_field::survivability_maintenance_bias)},
    {"alertness",                     static_cast<uint8_t>(af_skill_field::alertness)},
    {"fov_degrees",                   static_cast<uint8_t>(af_skill_field::fov_degrees)},
    {"target_focus_bias",             static_cast<uint8_t>(af_skill_field::target_focus_bias)},
    {"weapon_switch_likelihood",      static_cast<uint8_t>(af_skill_field::weapon_switch_likelihood)},
    {"crouch_likelihood",             static_cast<uint8_t>(af_skill_field::crouch_likelihood)},
    {"jump_likelihood",               static_cast<uint8_t>(af_skill_field::jump_likelihood)},
    {"dodge_likelihood",              static_cast<uint8_t>(af_skill_field::dodge_likelihood)},
};

int bot_personality_field_id_from_name(const char* name)
{
    for (const auto& entry : g_personality_field_map) {
        if (strings_equal_case_insensitive(name, entry.name)) {
            return entry.field_id;
        }
    }
    return -1;
}

int bot_skill_field_id_from_name(const char* name)
{
    for (const auto& entry : g_skill_field_map) {
        if (strings_equal_case_insensitive(name, entry.name)) {
            return entry.field_id;
        }
    }
    return -1;
}

struct QuirkNameMapping {
    const char* name;
    int bit;
};

static const QuirkNameMapping g_quirk_name_map[] = {
    {"railgun_no_los_hunter",       0},
    {"melee_finisher",              1},
    {"shotgun_low_health_finisher", 2},
    {"jump_pad_pathing_affinity",   3},
    {"spawn_hunter",                4},
    {"crater_unlock_affinity",      5},
    {"super_item_hoarder",          6},
    {"sneaky_capper",               7},
};

int bot_quirk_bit_from_name(const char* name)
{
    for (const auto& entry : g_quirk_name_map) {
        if (strings_equal_case_insensitive(name, entry.name)) {
            return entry.bit;
        }
    }
    return -1;
}
