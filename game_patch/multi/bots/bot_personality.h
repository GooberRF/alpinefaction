#pragma once

#include <cstdint>
#include <vector>

struct BotContext;

enum class BotAttackStyle
{
    balanced = 0,
    aggressive = 1,
    evasive = 2,
};

enum class BotPersonalityQuirk : uint64_t
{
    none = 0,
    railgun_no_los_hunter = 1ull << 0,
    melee_finisher = 1ull << 1,
    shotgun_low_health_finisher = 1ull << 2,
    jump_pad_pathing_affinity = 1ull << 3,
    spawn_hunter = 1ull << 4,
    crater_unlock_affinity = 1ull << 5,
    super_item_hoarder = 1ull << 6,
    sneaky_capper = 1ull << 7,
};

struct BotPersonality
{
    const char* name = "average";
    BotAttackStyle attack_style = BotAttackStyle::balanced;
    float preferred_engagement_near = 6.5f;
    float preferred_engagement_far = 13.5f;
    float super_pickup_bias = 1.0f;
    float revisit_avoidance_bias = 1.0f;
    float retrace_avoidance_bias = 1.0f;
    int retrace_lookback_waypoints = 10;
    float decision_aggression_bias = 1.0f;
    float decision_efficiency_bias = 1.0f;
    float decision_risk_tolerance = 1.0f;
    float path_smoothing_bias = 1.0f;
    float corner_centering_bias = 1.0f;
    float roam_intensity_bias = 1.0f;
    float navigation_strafe_bias = 0.8f;
    float crouch_route_avoidance_bias = 1.0f;
    int stuck_goal_retry_limit = 4;
    float goal_commitment_bias = 1.0f;
    float eliminate_target_commitment_bias = 1.0f;
    float opportunism_bias = 1.0f;
    float retreat_health_threshold = 0.32f;
    float retreat_armor_threshold = 0.22f;
    float replenish_health_threshold = 0.74f;
    float replenish_armor_threshold = 0.46f;
    float seek_weapon_bias = 1.0f;
    float satisfactory_weapon_threshold = 0.60f;
    float preferred_weapon_ammo_fill_threshold = 0.40f;
    float replenish_bias = 1.0f;
    float power_position_bias = 1.0f;
    float weapon_switch_bias = 1.0f;
    int min_weapon_switch_cooldown_ms = 3000;
    float crouch_combat_bias = 1.0f;
    float jump_combat_bias = 0.25f;
    float dodge_combat_bias = 1.0f;
    float raw_aggression_bias = 1.0f;
    float camping_bias = 1.0f;
    float easy_frag_bias = 1.0f;
    float retaliation_bias = 1.0f;
    float combat_readiness_threshold = 0.52f;
    float deathmatch_kill_focus_bias = 1.0f;
    float ctf_capture_priority_bias = 1.0f;
    float ctf_flag_recovery_bias = 1.0f;
    float ctf_hold_enemy_flag_safety_bias = 1.0f;
    float ctf_hold_carrier_hunt_bias = 1.0f;
    uint64_t quirk_mask = static_cast<uint64_t>(BotPersonalityQuirk::none);
    std::vector<int> preferred_weapon_types{};
    float pickup_switch_chance_without_preferences = 0.50f;
    float taunt_on_kill_chance = 0.05f;
    float gg_on_map_end_chance = 0.50f;
    float hello_on_join_chance = 0.50f;
    float red_faction_response_chance = 0.50f;
    std::vector<float> item_weights{};
    std::vector<float> weapon_pickup_weights{};
    std::vector<float> weapon_preference_weights{};
};

struct BotSkillProfile
{
    const char* name = "average";
    float base_skill = 0.75f;
    float aim_profile_scale = 1.0f;
    float decision_profile_scale = 1.0f;
    float survivability_maintenance_bias = 1.0f;
    float alertness = 1.0f;
    float fov_degrees = 120.0f;
    float target_focus_bias = 1.0f;
    float weapon_switch_likelihood = 0.50f;
    float crouch_likelihood = 0.14f;
    float jump_likelihood = 0.14f;
    float dodge_likelihood = 0.14f;
    float reaction_time_ms = 50.0f;
    // Aim error bound at a 22 unit target distance, from no skill (max) to full skill (min).
    float aim_error_min = 0.035f;
    float aim_error_max = 2.65f;
    // Turn speed (radians per second) and minimum aim alignment (dot product) to fire, at full skill.
    float yaw_speed_cap = 10.0f;
    float pitch_speed_cap = 7.0f;
    float fire_alignment_threshold = 0.80f;
    // Semi-automatic trigger presses are this many fire waits apart (before skill scaling).
    float semi_auto_click_interval_scale = 3.0f;
    std::vector<float> weapon_skill_weights{};
};

struct BotConfigOverride
{
    uint8_t field_id;
    float value;
};

// Weapon preference entry for preset definitions.
// weapon_name is resolved to a weapon type index at runtime via rf::weapon_lookup_type().
// The weight values are applied as multipliers on auto-calculated base weights.
struct BotWeaponPreferenceEntry
{
    const char* weapon_name;
    float pickup_weight;
    float preference_weight;
    float skill_weight;
};

const BotPersonality& bot_personality_get(BotContext& ctx);
const BotSkillProfile& bot_skill_profile_get(BotContext& ctx);
const BotPersonality& get_active_bot_personality();
const BotSkillProfile& get_active_bot_skill_profile();
bool bot_personality_has_quirk(BotPersonalityQuirk quirk);

float bot_get_item_weight(int item_type);
float bot_get_weapon_pickup_weight(int weapon_type);
float bot_get_weapon_preference_weight(int weapon_type);
float bot_get_weapon_skill_weight(int weapon_type);

float bot_get_skill_factor();
float bot_get_decision_skill_factor();

// Configure presets by name and immediately reinitialize. The field overrides are applied on top of the presets
// on every reinitialization. Passing nullptr for a parameter leaves that preset or override list unchanged.
void bot_personality_set_presets(
    BotContext& ctx,
    const char* personality_name,
    const char* skill_name,
    const std::vector<BotConfigOverride>* personality_overrides = nullptr,
    const std::vector<BotConfigOverride>* skill_overrides = nullptr);

// Name-to-field-ID lookup for TOML config parsing.
// Returns -1 if the name is not recognized.
int bot_personality_field_id_from_name(const char* name);
int bot_skill_field_id_from_name(const char* name);

// Quirk name-to-bit lookup for TOML config parsing.
// Returns the bit position (0-63) or -1 if unrecognized.
int bot_quirk_bit_from_name(const char* name);
