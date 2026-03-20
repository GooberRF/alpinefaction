#pragma once

#include "bot_personality.h"
#include "../../misc/waypoints.h"
#include "../../rf/entity.h"
#include "../../rf/item.h"
#include "../../rf/object.h"
#include "../../rf/os/timestamp.h"
#include <array>
#include <cassert>
#include <cstring>
#include <cstddef>
#include <limits>
#include <vector>

template<typename T, std::size_t N>
struct FixedRingBuffer
{
    std::array<T, N> data{};
    std::size_t head = 0;
    std::size_t count = 0;

    void clear() { head = 0; count = 0; }
    bool empty() const { return count == 0; }
    std::size_t size() const { return count; }

    void push_back(const T& value)
    {
        const std::size_t write_pos = (head + count) % N;
        data[write_pos] = value;
        if (count < N) {
            ++count;
        }
        else {
            head = (head + 1) % N;
        }
    }

    const T& back() const
    {
        assert(count > 0);
        return data[(head + count - 1) % N];
    }

    const T& operator[](std::size_t i) const
    {
        assert(i < count);
        return data[(head + i) % N];
    }
};

inline constexpr float kWaypointSearchRadius = 96.0f;
inline constexpr float kWaypointReachRadius = 4.0f;
inline constexpr float kWaypointLinkCorridorRadius = 3.5f;
inline constexpr float kBotNominalMaxHealth = 100.0f;
inline constexpr float kBotNominalMaxArmor = 100.0f;
inline constexpr float kWaypointStuckDistance = 1.0f;
inline constexpr float kWaypointProgressMinImprovement = 0.75f;
inline constexpr int kWaypointProgressTimeoutMs = 1000;
inline constexpr int kWaypointObjectiveProgressTimeoutMs = 500;
inline constexpr int kWaypointNoImprovementWindowLimit = 3;
inline constexpr int kWaypointPickAttempts = 72;
inline constexpr int kWaypointRepathMs = 6000;
inline constexpr int kWaypointStuckCheckMs = 1000;
inline constexpr int kWaypointRecoveryRepathMs = 125;
inline constexpr int kWaypointRecoveryDetourAttempts = 24;
inline constexpr int kPursuitRouteFailureLimit = 4;
inline constexpr int kCollectRouteFailureLimit = 4;
inline constexpr int kCollectRecoveryRepathMs = 650;
inline constexpr int kPursuitRecoveryMinMs = 1200;
inline constexpr int kPursuitRecoveryMaxMs = 2400;
inline constexpr float kBlindProgressDistanceThreshold = 10.0f;
inline constexpr int kBlindProgressWindowMs = 3000;
inline constexpr int kLongRouteEscapeMinMs = 2600;
inline constexpr int kLongRouteEscapeMaxMs = 4400;
inline constexpr int kLongRouteDetourAttempts = 40;
inline constexpr int kLosWaypointCandidateLimit = 20;
inline constexpr int kRouteSelectionPool = 4;
inline constexpr int kLosCheckIntervalVisibleMs = 220;
inline constexpr int kLosCheckIntervalBlockedMs = 90;
inline constexpr float kEnemyAimHeightBlend = 0.42f;
inline constexpr int kPursuitRepathIntervalMs = 2000;
inline constexpr int kCombatLosRepositionDecisionMs = 325;
inline constexpr int kCombatLosRepositionRepathMs = 450;
inline constexpr int kCombatLosSampleIntervalMs = 150;
inline constexpr int kCombatLosNeighborEvalIntervalMs = 750;
inline constexpr int kLosRouteSolveIntervalMs = 150;
inline constexpr int kObstacleProbeIntervalMs = 500;
inline constexpr float kObstacleProbeDistance = 2.25f;
inline constexpr float kObstacleForwardProbeDistance = 2.75f;
inline constexpr int kCornerSteerProbeIntervalMs = 50;
inline constexpr float kCornerSteerProbeDistance = 2.0f;
inline constexpr float kCornerSteerMaxStrafe = 0.75f;
inline constexpr int kForwardObstacleGuardIntervalMs = 100;
inline constexpr float kForwardObstacleProbeDistance = 2.75f;
inline constexpr int kNavigationBlockedProgressWindowMs = 800;
inline constexpr float kNavigationBlockedProgressDistance = 1.5f;
inline constexpr int kFailedEdgeCooldownMs = 3500;
inline constexpr int kLoopBreakerEdgeCooldownMs = 6500;
inline constexpr int kFailedEdgeCooldownCapacity = 24;
inline constexpr int kFailedWaypointBlacklistMs = 20000;
inline constexpr int kFailedWaypointBlacklistCapacity = 48;
inline constexpr int kFailedItemGoalCooldownMs = 12000;
inline constexpr int kFailedItemGoalCooldownCapacity = 48;
inline constexpr int kRecentItemGoalSelectionMs = 15000;
inline constexpr int kRecentItemGoalSelectionCapacity = 64;
inline constexpr int kSecondaryGoalTypeRepeatMs = 12000;
inline constexpr std::size_t kWaypointVisitHistoryCapacity = 24;
inline constexpr int kClosestLosWaypointCacheMs = 500;
inline constexpr float kClosestLosWaypointCacheMoveDist = 6.0f;
inline constexpr float kRouteDistanceWeight = 1.0f;
inline constexpr float kRouteItemValueWeight = 9.5f;
inline constexpr float kRouteTargetDistanceWeight = 1.2f;
inline constexpr float kRouteRecentVisitPenaltyWeight = 8.5f;
inline constexpr float kRouteCornerHugPenaltyWeight = 6.0f;
inline constexpr float kRouteEdgeExposurePenaltyWeight = 9.0f;
inline constexpr int kLoopEscapeMinMs = 3200;
inline constexpr int kLoopEscapeMaxMs = 5600;
inline constexpr int kGoalReevaluateMs = 800;
inline constexpr float kGoalEnemyBaseScore = 205.0f;
inline constexpr float kGoalEnemyDistancePenalty = 0.85f;
inline constexpr float kGoalEnemyVisibleBonus = 75.0f;
inline constexpr float kGoalItemBaseBonus = 45.0f;
inline constexpr int kItemRouteRepathMs = 1800;
inline constexpr int kBotRespawnGearupDurationMs = 9000;
inline constexpr int kBotRetaliationWindowMs = 2600;
inline constexpr int kBotRetaliationNearMissWindowMs = 1400;
inline constexpr float kNavigationAimVerticalThreshold = 1.6f;
inline constexpr int kGoalSwitchLockMinMs = 320;
inline constexpr int kGoalSwitchLockMaxMs = 1400;
inline constexpr int kRoamFallbackGoalLockMs = 900;
inline constexpr int kRecoveryRoamReevalLimitMs = 4000;
inline constexpr int kAimErrorUpdateSlowMs = 240;  // low-skill: longer interval between aim error updates
inline constexpr int kAimErrorUpdateFastMs = 55;   // high-skill: shorter interval between aim error updates
inline constexpr float kAimErrorMinUnits = 0.035f;
inline constexpr float kAimErrorMaxUnits = 2.65f;
inline constexpr float kAimErrorDistanceNorm = 22.0f;
inline constexpr float kSemiAutoClickIntervalScale = 3.0f;
inline constexpr int kSemiAutoClickMinMs = 90;
inline constexpr int kSemiAutoClickMaxMs = 900;
inline constexpr int kRemoteChargeDetonationWindowMs = 5000;
inline constexpr int kTargetAcquisitionReactionMs = 50;
inline constexpr int kBotServerConfigTimeoutMs = 10000;
inline constexpr int kBotConnectionWatchdogMs = 30000;

enum class BotGoalType
{
    none = 0,
    eliminate_target = 1,
    collect_weapon = 2,
    collect_ammo = 3,
    collect_health = 4,
    collect_armor = 5,
    collect_super_item = 6,
    activate_bridge = 7,
    create_crater = 8,
    shatter_glass = 9,
    ctf_steal_flag = 10,
    ctf_return_flag = 11,
    ctf_capture_flag = 12,
    ctf_hold_enemy_flag = 13,
    roam = 14,
    control_point_objective = 15,
};

inline constexpr bool bot_goal_is_item_collection(const BotGoalType goal)
{
    switch (goal) {
        case BotGoalType::collect_weapon:
        case BotGoalType::collect_ammo:
        case BotGoalType::collect_health:
        case BotGoalType::collect_armor:
        case BotGoalType::collect_super_item:
            return true;
        default:
            return false;
    }
}

inline constexpr bool bot_goal_is_ctf_objective(const BotGoalType goal)
{
    switch (goal) {
        case BotGoalType::ctf_steal_flag:
        case BotGoalType::ctf_return_flag:
        case BotGoalType::ctf_capture_flag:
        case BotGoalType::ctf_hold_enemy_flag:
            return true;
        default:
            return false;
    }
}

inline constexpr bool bot_goal_is_control_point_objective(const BotGoalType goal)
{
    return goal == BotGoalType::control_point_objective;
}

enum class BotGoalTier
{
    tier1_objective = 1,
    tier2_utility = 2,
    tier3_environmental = 3,
    fallback = 4,
};

inline constexpr BotGoalTier bot_goal_tier(const BotGoalType goal)
{
    switch (goal) {
        case BotGoalType::eliminate_target:
        case BotGoalType::ctf_steal_flag:
        case BotGoalType::ctf_return_flag:
        case BotGoalType::ctf_capture_flag:
        case BotGoalType::ctf_hold_enemy_flag:
        case BotGoalType::control_point_objective:
            return BotGoalTier::tier1_objective;
        case BotGoalType::collect_weapon:
        case BotGoalType::collect_ammo:
        case BotGoalType::collect_health:
        case BotGoalType::collect_armor:
        case BotGoalType::collect_super_item:
            return BotGoalTier::tier2_utility;
        case BotGoalType::activate_bridge:
        case BotGoalType::create_crater:
        case BotGoalType::shatter_glass:
            return BotGoalTier::tier3_environmental;
        default:
            return BotGoalTier::fallback;
    }
}

inline constexpr bool bot_goal_is_tier1_objective(const BotGoalType goal)
{
    return bot_goal_tier(goal) == BotGoalTier::tier1_objective;
}

inline constexpr bool bot_goal_is_tier2_utility(const BotGoalType goal)
{
    return bot_goal_tier(goal) == BotGoalTier::tier2_utility;
}

enum class BotFsmState
{
    inactive = 0,
    idle = 1,
    roam = 2,
    seek_enemy = 3,
    pursue_enemy = 4,
    engage_enemy = 5,
    collect_pickup = 6,
    reposition_los = 7,
    recover_navigation = 8,
    retreat = 9,
    seek_weapon = 10,
    replenish_health_armor = 11,
    find_power_position = 12,
    activate_bridge = 13,
    create_crater = 14,
    shatter_glass = 15,
    ctf_objective = 16,
    control_point_objective = 17,
};

struct FailedEdgeCooldown
{
    int from_waypoint = 0;
    int to_waypoint = 0;
    rf::Timestamp cooldown{};
};

struct FailedWaypointCooldown
{
    int waypoint = 0;
    rf::Timestamp cooldown{};
};

struct FailedItemGoalCooldown
{
    int item_handle = -1;
    rf::Timestamp cooldown{};
};

struct RecentItemGoalSelection
{
    int item_handle = -1;
    BotGoalType goal_type = BotGoalType::none;
    rf::Timestamp cooldown{};
};

enum class BotAlertType
{
    none = 0,
    damaged_by_enemy = 1,
    nearby_weapon_fire = 2,
    nearby_weapon_reload = 3,
};

struct BotAlertSourceSample
{
    int entity_handle = -1;
    int last_fired_timestamp_value = -1;
    int last_reload_done_timestamp_value = -1;
    bool was_reloading = false;
};

struct BotAlertContact
{
    int entity_handle = -1;
    rf::Vector3 last_known_pos{};
    float awareness_weight = 0.0f;
    BotAlertType type = BotAlertType::none;
    rf::Timestamp awareness_timer{};
};

struct BotCombatFiringState
{
    bool wants_fire = false;
    bool synthetic_primary_fire_down = false;
    bool synthetic_secondary_fire_down = false;
    bool synthetic_primary_fire_just_pressed = false;
    bool synthetic_secondary_fire_just_pressed = false;
    rf::Timestamp target_acquisition_timer{};
    int target_acquisition_handle = -1;
    rf::Timestamp aim_error_timer{};
    int aim_error_target_handle = -1;
    float aim_error_right = 0.0f;
    float aim_error_up = 0.0f;
    rf::Timestamp semi_auto_click_timer{};
    int semi_auto_click_weapon = -1;
    rf::Timestamp explosive_fire_delay_timer{};
    int explosive_fire_delay_weapon = -1;
    bool explosive_fire_delay_alt = false;
    rf::Timestamp explosive_release_hold_timer{};
    rf::Vector3 explosive_release_hold_target{};
    int explosive_release_hold_weapon = -1;
    bool explosive_release_hold_alt = false;
    bool remote_charge_pending_detonation = false;
    rf::Timestamp remote_charge_pending_detonation_timer{};

    void reset()
    {
        *this = BotCombatFiringState{};
    }
};

struct BotBridgeGoalState
{
    int zone_uid = -1;
    int trigger_uid = -1;
    rf::Vector3 trigger_pos{};
    float activation_radius = 0.0f;
    bool requires_use = false;
    rf::Timestamp use_press_timer{};
    rf::Timestamp activation_abort_timer{};
    float activation_best_dist_sq = std::numeric_limits<float>::infinity();
    int post_open_zone_uid = -1;
    int post_open_target_waypoint = 0;
    rf::Timestamp post_open_priority_timer{};

    void reset()
    {
        *this = BotBridgeGoalState{};
    }
};

struct BotGoalStuckWatchdog
{
    BotGoalType goal = BotGoalType::none;
    BotFsmState fsm = BotFsmState::inactive;
    int handle = -1;
    int identifier = -1;
    int waypoint = 0;
    int route_waypoint = 0;
    int route_next_index = 0;
    int recovery_anchor = 0;
    rf::Timestamp timer{};
    rf::Vector3 origin{};
    rf::Timestamp retry_window_timer{};
    int retry_count = 0;

    void reset()
    {
        *this = BotGoalStuckWatchdog{};
    }
};

struct BotPositionStallWatchdog
{
    rf::Timestamp timer{};
    rf::Vector3 origin{};
    int waypoint = 0;
    rf::Timestamp retry_window_timer{};
    BotGoalType goal = BotGoalType::none;
    int handle = -1;
    int identifier = -1;
    int retry_count = 0;

    void reset()
    {
        *this = BotPositionStallWatchdog{};
    }
};

struct BotGlobalFailsafeWatchdog
{
    rf::Timestamp timer{};
    rf::Timestamp retry_window_timer{};
    rf::Vector3 origin{};
    rf::Vector3 aim_dir{};
    BotGoalType goal = BotGoalType::none;
    BotFsmState fsm = BotFsmState::inactive;
    int handle = -1;
    int identifier = -1;
    int last_fired_timestamp = -1;
    int retry_count = 0;

    void reset()
    {
        *this = BotGlobalFailsafeWatchdog{};
    }
};

struct BotTopGoalEntry
{
    BotGoalType goal = BotGoalType::none;
    float score = -std::numeric_limits<float>::infinity();
    int target_identifier = -1;
};

struct ClientBotState
{
    std::vector<int> waypoint_path{};
    int waypoint_next_index = 0;
    int waypoint_goal = 0;
    rf::Timestamp route_choice_lock_timer{};
    BotGoalType route_choice_lock_goal_type = BotGoalType::none;
    int route_choice_lock_target_handle = -1;
    int route_choice_lock_target_identifier = -1;
    int route_choice_lock_goal_waypoint = 0;
    int route_choice_lock_next_waypoint = 0;
    rf::Vector3 waypoint_target_pos{};
    bool has_waypoint_target = false;
    bool fall_fast_forward_was_falling = false;
    bool fall_fast_forward_pending = false;
    float fall_fast_forward_start_y = 0.0f;
    rf::Timestamp fall_fast_forward_stuck_timer{};
    rf::Vector3 fall_fast_forward_stuck_origin{};

    rf::Timestamp repath_timer{};
    rf::Timestamp stuck_timer{};
    int geometry_stuck_sample_count = 0;
    rf::Vector3 last_pos{};
    rf::Timestamp waypoint_progress_timer{};
    int waypoint_progress_waypoint = 0;
    float waypoint_progress_best_dist_sq = 0.0f;
    int waypoint_progress_no_improvement_windows = 0;
    rf::Vector3 waypoint_progress_origin{};
    rf::Timestamp contextual_goal_eval_timer{};
    int contextual_goal_item_handle = -1;
    int contextual_goal_waypoint = 0;
    rf::Vector3 contextual_goal_pos{};
    rf::Timestamp item_goal_contact_timer{};
    int item_goal_contact_handle = -1;

    rf::Timestamp respawn_retry_timer{};
    rf::Timestamp respawn_gearup_timer{};
    rf::Timestamp respawn_uncrouch_retry_timer{};
    rf::Timestamp retaliation_timer{};
    int retaliation_target_handle = -1;
    float last_recorded_health = -1.0f;
    float last_recorded_armor = -1.0f;
    rf::Timestamp fire_decision_timer{};
    rf::Timestamp auto_fire_release_guard_timer{};
    int ctf_threat_handle = -1;
    rf::Vector3 ctf_threat_pos{};
    bool ctf_threat_visible = false;
    rf::Timestamp ctf_objective_route_fail_timer{};
    rf::Timestamp ctf_hold_goal_timer{};
    rf::Timestamp control_point_route_fail_timer{};
    int control_point_patrol_waypoint = 0;
    rf::Timestamp control_point_patrol_timer{};
    BotCombatFiringState firing{};

    rf::Timestamp jump_timer{};
    rf::Timestamp jump_variation_timer{};
    int jump_target_link_from_waypoint = 0;
    int jump_target_link_to_waypoint = 0;
    rf::Timestamp reload_timer{};
    rf::Timestamp los_check_timer{};
    rf::Timestamp los_route_solve_timer{};
    int los_target_handle = -1;
    bool los_to_enemy = false;
    int preferred_enemy_handle = -1;
    rf::Timestamp preferred_enemy_lock_timer{};
    int recovery_anchor_waypoint = 0;
    int recovery_avoid_waypoint = 0;
    bool recovery_pending_reroute = false;
    int pursuit_target_handle = -1;
    int pursuit_route_failures = 0;
    bool last_pursuit_route_was_fallback = false;
    int collect_route_failures = 0;
    rf::Timestamp pursuit_recovery_timer{};
    rf::Vector3 blind_progress_origin{};
    rf::Timestamp blind_progress_timer{};
    float blind_progress_max_dist_sq = 0.0f;
    rf::Timestamp long_route_escape_timer{};
    rf::Timestamp obstacle_probe_timer{};
    rf::Timestamp corner_steer_probe_timer{};
    rf::Timestamp forward_obstacle_guard_timer{};
    rf::Timestamp forward_obstacle_reroute_timer{};
    int forward_obstacle_blocked_samples = 0;
    rf::Timestamp forward_obstacle_progress_timer{};
    rf::Vector3 forward_obstacle_progress_origin{};
    rf::Timestamp corner_probe_progress_timer{};
    rf::Vector3 corner_probe_progress_origin{};
    rf::Timestamp closest_los_waypoint_cache_timer{};
    rf::Vector3 closest_los_waypoint_cache_pos{};
    int closest_los_waypoint_cache = 0;
    std::vector<FailedEdgeCooldown> failed_edge_cooldowns{};
    std::vector<FailedWaypointCooldown> failed_waypoint_cooldowns{};
    std::vector<FailedItemGoalCooldown> failed_item_goal_cooldowns{};
    std::vector<RecentItemGoalSelection> recent_item_goal_selections{};
    std::vector<BotAlertSourceSample> alert_source_samples{};
    std::vector<BotAlertContact> alert_contacts{};
    FixedRingBuffer<int, kWaypointVisitHistoryCapacity> recent_waypoint_visits{};
    int validated_link_from_waypoint = 0;
    int validated_link_to_waypoint = 0;
    bool validated_link_clear = false;
    int corridor_violation_from_waypoint = 0;
    int corridor_violation_to_waypoint = 0;
    int corridor_violation_count = 0;
    rf::Timestamp combat_los_reposition_timer{};
    int combat_los_anchor_waypoint = 0;
    int combat_los_target_waypoint = 0;
    rf::Timestamp combat_los_reposition_eval_timer{};
    rf::Timestamp combat_los_waypoint_guard_timer{};
    int combat_los_waypoint_guard_waypoint = 0;
    int combat_los_waypoint_guard_target_handle = -1;
    bool combat_los_waypoint_guard_visible = false;

    float move_input_x = 0.0f;
    float move_input_y = 0.0f;
    float move_input_z = 0.0f;
    float nav_look_phase = 0.0f;
    bool movement_override_active = false;

    BotGoalType active_goal = BotGoalType::none;
    int goal_target_handle = -1;
    int goal_target_identifier = -1;
    int goal_target_waypoint = 0;
    rf::Vector3 goal_target_pos{};
    BotGoalStuckWatchdog goal_stuck_wd{};
    rf::Timestamp eliminate_target_reacquire_timer{};
    rf::Timestamp goal_eval_timer{};
    rf::Timestamp goal_switch_lock_timer{};
    std::array<bool, 64> known_weapons{};
    bool known_weapons_initialized = false;
    rf::Timestamp weapon_switch_timer{};
    rf::Timestamp combat_weapon_eval_timer{};
    rf::Timestamp combat_maneuver_timer{};
    rf::Timestamp combat_crouch_timer{};
    bool traversal_crouch_active = false;
    bool traversal_crouch_toggled_on = false;
    float combat_maneuver_strafe = 0.0f;
    BotFsmState fsm_state = BotFsmState::inactive;
    rf::Timestamp fsm_state_timer{};
    rf::Timestamp fsm_transition_cooldown{};
    BotGoalType recent_secondary_goal_type = BotGoalType::none;
    rf::Timestamp recent_secondary_goal_timer{};
    std::array<BotTopGoalEntry, 3> last_eval_top3{};
    rf::Timestamp hud_status_timer{};
    rf::Timestamp console_status_timer{};
    bool server_config_received = false;
    bool server_deactivated = false;
    rf::Timestamp server_config_timeout_timer{};
    rf::Timestamp connection_watchdog_timer{};
    rf::Timestamp no_move_target_watchdog_timer{};
    int no_move_target_watchdog_retries = 0;
    BotPositionStallWatchdog position_stall_wd{};
    rf::Timestamp objective_progress_watchdog_timer{};
    rf::Timestamp objective_progress_retry_window_timer{};
    BotGoalType objective_progress_watchdog_goal = BotGoalType::none;
    BotFsmState objective_progress_watchdog_fsm = BotFsmState::inactive;
    int objective_progress_watchdog_handle = -1;
    int objective_progress_watchdog_identifier = -1;
    int objective_progress_watchdog_waypoint = 0;
    int objective_progress_watchdog_route_waypoint = 0;
    int objective_progress_watchdog_route_next_index = 0;
    float objective_progress_watchdog_best_dist_sq = std::numeric_limits<float>::infinity();
    int objective_progress_watchdog_retry_count = 0;
    BotGlobalFailsafeWatchdog global_failsafe_wd{};

    BotBridgeGoalState bridge{};
    rf::Timestamp crater_goal_abort_timer{};
    rf::Timestamp crater_goal_timeout_timer{};
    rf::Timestamp shatter_goal_abort_timer{};
    rf::Timestamp shatter_goal_timeout_timer{};
    rf::Timestamp confusion_abort_timer{};
    rf::Vector3 confusion_abort_pos{};
    rf::Vector3 confusion_abort_aim_dir{};
    int confusion_abort_last_fired_timestamp = -1;
    rf::Timestamp idle_escalation_timer{};
    rf::Timestamp combat_lock_watchdog_timer{};
    rf::Timestamp recovery_roam_lock_timer{};
    char last_heading_change_reason[96] = "none";

    // Per-frame dedup: prevent multiple expensive goal refreshes in a single frame.
    bool goal_refreshed_this_frame = false;

    // Per-frame cache for weapon readiness score (iterates all weapon types).
    bool weapon_readiness_cached = false;
    float weapon_readiness_cache_score = 0.0f;
    int weapon_readiness_cache_enemy_handle = -1;
};

struct ItemGoalCandidate
{
    int item_handle = -1;
    int item_uid = -1;
    BotGoalType goal_type = BotGoalType::none;
    int goal_waypoint = 0;
    rf::Vector3 item_pos{};
    float score = -std::numeric_limits<float>::infinity();
};

extern ClientBotState g_client_bot_state;

void bot_internal_clear_waypoint_route();
bool bot_internal_start_recovery_anchor_reroute(const rf::Entity& entity, int avoid_waypoint);
bool bot_internal_update_waypoint_target(const rf::Entity& entity);
bool bot_internal_update_waypoint_target_towards(
    const rf::Entity& entity,
    const rf::Vector3& destination,
    const rf::Vector3* los_target_eye_pos,
    const rf::Object* los_target_obj,
    int repath_ms);
bool bot_internal_update_waypoint_target_for_local_los_reposition(
    const rf::Entity& entity,
    const rf::Entity& enemy_target,
    bool enemy_has_los);
bool bot_internal_try_recover_from_corner_probe(
    const rf::Entity& entity,
    const rf::Vector3& move_target);
bool bot_internal_is_following_waypoint_link(const rf::Entity& entity);
bool bot_internal_is_collectible_goal_item(const rf::Item& item);
BotGoalType bot_internal_classify_collect_goal_item(const rf::Item& item);
bool bot_internal_is_item_goal_satisfied(
    const rf::Entity& local_entity,
    BotGoalType goal_type,
    const rf::Item& item);
bool bot_internal_find_item_goal_waypoint(const rf::Vector3& item_pos, int& out_waypoint);
bool bot_internal_find_best_item_goal(
    const rf::Entity& local_entity,
    const rf::Entity* enemy_target,
    ItemGoalCandidate& out_candidate);
void bot_internal_note_item_goal_selection(int item_handle, BotGoalType goal_type);
float bot_internal_compute_enemy_goal_score(
    const rf::Entity& local_entity,
    const rf::Entity& enemy_target,
    bool enemy_has_los);
float bot_internal_compute_combat_readiness(
    const rf::Entity& local_entity,
    const rf::Entity* enemy_target);
float bot_internal_get_combat_readiness_threshold();
bool bot_internal_is_deathmatch_mode();
bool bot_internal_is_control_point_mode();
void bot_internal_set_last_heading_change_reason(const char* reason);
const char* bot_internal_get_last_heading_change_reason();
