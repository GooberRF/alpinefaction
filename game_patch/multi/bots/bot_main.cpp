#include "bot_main.h"
#include "bot_chat_manager.h"
#include "bot_combat.h"
#include "bot_combat_manager.h"
#include "bot_decision_eval.h"
#include "bot_fsm.h"
#include "bot_fsm_manager.h"
#include "bot_goal_memory.h"
#include "bot_goal_manager.h"
#include "bot_goals.h"
#include "bot_internal.h"
#include "bot_math.h"
#include "bot_memory_manager.h"
#include "bot_movement.h"
#include "bot_navigation.h"
#include "bot_navigation_manager.h"
#include "bot_navigation_pathing.h"
#include "bot_navigation_routes.h"
#include "bot_perception_manager.h"
#include "bot_personality_manager.h"
#include "bot_state.h"
#include "bot_weapon_profiles.h"
#include "bot_utils.h"
#include "bot_waypoint_route.h"
#include "../gametype.h"
#include "../multi.h"
#include "../network.h"
#include <patch_common/FunHook.h>
#include "../../main/main.h"
#include "../../graphics/gr.h"
#include "../../misc/alpine_settings.h"
#include "../../misc/waypoints.h"
#include "../../rf/ai.h"
#include "../../rf/collide.h"
#include "../../rf/entity.h"
#include "../../rf/gameseq.h"
#include "../../rf/gr/gr_font.h"
#include "../../rf/hud.h"
#include "../../rf/item.h"
#include "../../rf/multi.h"
#include "../../rf/os/frametime.h"
#include "../../rf/player/player.h"
#include "../../rf/weapon.h"
#include <xlog/xlog.h>
#include <array>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <random>
#include <string_view>
#include <unordered_set>
#include <vector>

ClientBotState g_client_bot_state{};

namespace
{
bool g_bot_input_hook_installed = false;
constexpr int kBotRespawnRetryMs = 100;
constexpr int kBotNavProbeTraceFlags = rf::CF_ANY_HIT | rf::CF_PROCESS_INVISIBLE_FACES;
constexpr int kBotConfusionAbortMs = 2000;
constexpr float kBotConfusionStillDistance = 0.20f;
constexpr float kBotConfusionAimDotThreshold = 0.9985f;
constexpr int kBotGoalStuckWatchdogMs = 2000;
constexpr int kBotGoalStuckRetryWindowMs = 60000;
constexpr float kBotGoalStuckStillDistance = 1.0f;
constexpr int kPositionStallWatchdogMs = 2000;
constexpr int kPositionStallRetryWindowMs = 14000;
constexpr int kPositionStallRetryLimit = 3;
constexpr float kPositionStallWatchdogStillDistance = 0.9f;
constexpr float kPositionStallRetryResetDistance = 2.5f;
constexpr int kNoMoveTargetWatchdogMs = 900;
constexpr int kNoMoveTargetWatchdogRetryLimit = 3;
constexpr int kObjectiveProgressWatchdogMs = 2800;
constexpr int kObjectiveProgressRetryWindowMs = 45000;
constexpr int kObjectiveProgressRetryLimit = 3;
constexpr float kObjectiveProgressMinImprovement = 1.0f;
constexpr float kObjectiveProgressNearGoalDistance = 4.0f;
constexpr int kGlobalFailsafeWatchdogMs = 3000;
constexpr int kGlobalFailsafeRetryWindowMs = 20000;
constexpr int kGlobalFailsafeRetryLimit = 2;
constexpr float kGlobalFailsafeStillDistance = 0.5f;
constexpr float kGlobalFailsafeAimDotThreshold = 0.9992f;
constexpr int kIdleEscalationMs = 150;
constexpr int kCombatLockWatchdogOverrideMs = 6000;
constexpr int kPositionStallWatchdogRecoveryMs = 1200;

struct BotInfo
{
    bool spawn_allowed_by_server = true;
    bool can_spawn = false;
};

BotInfo g_bot_info{};

void reset_goal_stuck_watchdog(bool full_reset);
void reset_no_move_target_watchdog();
void reset_position_stall_watchdog();
void reset_objective_progress_watchdog(bool full_reset);
void reset_global_failsafe_watchdog(bool full_reset);

void update_bot_spawn_state(const rf::Player& player)
{
    const bool in_gameplay = rf::gameseq_in_gameplay();
    const bool is_dying = rf::player_is_dying(&player);
    const bool is_dead = rf::player_is_dead(&player);
    const bool is_alive = !player.is_spectator && !is_dead && !is_dying;

    // Server-side spawn permission will eventually drive this flag.
    g_bot_info.can_spawn =
        g_bot_info.spawn_allowed_by_server
        && !waypoints_missing_awp_from_level_init()
        && in_gameplay
        && !player.is_spawn_disabled
        && !is_alive
        && !is_dying;
}

void reset_confusion_abort_tracking()
{
    g_client_bot_state.confusion_abort_timer.invalidate();
    g_client_bot_state.confusion_abort_pos = {};
    g_client_bot_state.confusion_abort_aim_dir = {};
    g_client_bot_state.confusion_abort_last_fired_timestamp = -1;
}

void clear_bot_crouch_jump_link_state()
{
    g_client_bot_state.combat_crouch_timer.invalidate();
    g_client_bot_state.traversal_crouch_active = false;
    g_client_bot_state.traversal_crouch_toggled_on = false;
    g_client_bot_state.jump_target_link_from_waypoint = 0;
    g_client_bot_state.jump_target_link_to_waypoint = 0;
}

void force_uncrouch_after_respawn(rf::Player& local_player, rf::Entity& local_entity)
{
    clear_bot_crouch_jump_link_state();
    g_client_bot_state.respawn_uncrouch_retry_timer.invalidate();
    if (rf::entity_is_crouching(&local_entity)) {
        rf::player_execute_action(&local_player, rf::CC_ACTION_CROUCH, true);
        g_client_bot_state.respawn_uncrouch_retry_timer.set(180);
    }
}

void maintain_uncrouched_after_respawn(rf::Player& local_player, rf::Entity& local_entity)
{
    const bool within_respawn_window =
        g_client_bot_state.respawn_gearup_timer.valid()
        && !g_client_bot_state.respawn_gearup_timer.elapsed();
    if (!within_respawn_window) {
        g_client_bot_state.respawn_uncrouch_retry_timer.invalidate();
        return;
    }

    if (!rf::entity_is_crouching(&local_entity)) {
        return;
    }

    if (!g_client_bot_state.respawn_uncrouch_retry_timer.valid()
        || g_client_bot_state.respawn_uncrouch_retry_timer.elapsed()) {
        rf::player_execute_action(&local_player, rf::CC_ACTION_CROUCH, true);
        g_client_bot_state.respawn_uncrouch_retry_timer.set(180);
    }
}

bool has_confusion_abort_context()
{
    if (g_client_bot_state.recovery_pending_reroute
        || g_client_bot_state.fsm_state == BotFsmState::recover_navigation
        || g_client_bot_state.active_goal == BotGoalType::none
        || g_client_bot_state.active_goal == BotGoalType::roam) {
        return false;
    }

    return g_client_bot_state.active_goal != BotGoalType::none
        || g_client_bot_state.has_waypoint_target
        || !g_client_bot_state.waypoint_path.empty();
}

bool should_abort_for_confusion(const rf::Entity& entity, const rf::Vector3& aim_dir)
{
    if (!has_confusion_abort_context()) {
        reset_confusion_abort_tracking();
        return false;
    }

    const int fired_timestamp = entity.last_fired_timestamp.value;
    if (!g_client_bot_state.confusion_abort_timer.valid()) {
        g_client_bot_state.confusion_abort_pos = entity.pos;
        g_client_bot_state.confusion_abort_aim_dir = aim_dir;
        g_client_bot_state.confusion_abort_last_fired_timestamp = fired_timestamp;
        g_client_bot_state.confusion_abort_timer.set(kBotConfusionAbortMs);
        return false;
    }

    if (fired_timestamp != g_client_bot_state.confusion_abort_last_fired_timestamp) {
        g_client_bot_state.confusion_abort_pos = entity.pos;
        g_client_bot_state.confusion_abort_aim_dir = aim_dir;
        g_client_bot_state.confusion_abort_last_fired_timestamp = fired_timestamp;
        g_client_bot_state.confusion_abort_timer.set(kBotConfusionAbortMs);
        return false;
    }

    const rf::Vector3 delta_pos = entity.pos - g_client_bot_state.confusion_abort_pos;
    const bool stayed_still =
        delta_pos.len_sq() <= kBotConfusionStillDistance * kBotConfusionStillDistance;
    const float aim_dot = std::clamp(
        aim_dir.dot_prod(g_client_bot_state.confusion_abort_aim_dir),
        -1.0f,
        1.0f
    );
    const bool stayed_aimed = aim_dot >= kBotConfusionAimDotThreshold;

    if (!stayed_still || !stayed_aimed) {
        g_client_bot_state.confusion_abort_pos = entity.pos;
        g_client_bot_state.confusion_abort_aim_dir = aim_dir;
        g_client_bot_state.confusion_abort_timer.set(kBotConfusionAbortMs);
        return false;
    }

    if (!g_client_bot_state.confusion_abort_timer.elapsed()) {
        return false;
    }

    g_client_bot_state.confusion_abort_pos = entity.pos;
    g_client_bot_state.confusion_abort_aim_dir = aim_dir;
    g_client_bot_state.confusion_abort_timer.set(kBotConfusionAbortMs);
    return true;
}

void abort_goal_and_fsm_for_confusion(rf::Entity& entity)
{
    const BotGoalType aborted_goal = g_client_bot_state.active_goal;
    const int aborted_goal_handle = g_client_bot_state.goal_target_handle;

    if (g_alpine_game_config.dbg_bot) {
        xlog::warn(
            "Bot confusion failsafe: aborting goal={} fsm={} target_handle={} target_id={}",
            static_cast<int>(g_client_bot_state.active_goal),
            static_cast<int>(g_client_bot_state.fsm_state),
            g_client_bot_state.goal_target_handle,
            g_client_bot_state.goal_target_identifier
        );
    }

    bot_memory_manager_note_failed_goal_target(
        aborted_goal,
        aborted_goal_handle,
        kFailedItemGoalCooldownMs
    );

    g_client_bot_state.firing.reset();

    if (entity.ai.current_primary_weapon >= 0) {
        rf::entity_turn_weapon_off(entity.handle, entity.ai.current_primary_weapon);
    }

    bot_state_clear_waypoint_route(false, true, false);
    clear_synthetic_movement_controls();
    g_client_bot_state.pursuit_recovery_timer.invalidate();
    g_client_bot_state.pursuit_route_failures = 0;
    bot_state_clear_contextual_goal_tracking();

    bot_state_set_roam_fallback_goal(180);
    g_client_bot_state.eliminate_target_reacquire_timer.invalidate();
    g_client_bot_state.bridge.reset();
    g_client_bot_state.crater_goal_abort_timer.invalidate();
    g_client_bot_state.crater_goal_timeout_timer.invalidate();
    g_client_bot_state.shatter_goal_abort_timer.invalidate();
    g_client_bot_state.shatter_goal_timeout_timer.invalidate();

    bot_state_reset_fsm(BotFsmState::idle);
    bot_perception_manager_reset_tracking();

    reset_goal_stuck_watchdog(true);
    reset_no_move_target_watchdog();
    reset_position_stall_watchdog();
    reset_objective_progress_watchdog(true);
    reset_global_failsafe_watchdog(true);
    reset_confusion_abort_tracking();
}

enum class GoalStuckWatchdogAction
{
    none,
    reroute_same_goal,
    abandon_goal,
};

enum class PositionStallWatchdogAction
{
    none,
    reroute_same_goal,
    abandon_goal,
};

enum class ObjectiveProgressWatchdogAction
{
    none,
    reroute_same_goal,
    abandon_goal,
};

enum class GlobalFailsafeWatchdogAction
{
    none,
    recovery_triggered,
};

void reset_goal_stuck_watchdog(bool full_reset)
{
    if (full_reset) {
        g_client_bot_state.goal_stuck_wd.reset();
        return;
    }

    g_client_bot_state.goal_stuck_wd.timer.invalidate();
    g_client_bot_state.goal_stuck_wd.origin = {};
}

void reset_no_move_target_watchdog()
{
    g_client_bot_state.no_move_target_watchdog_timer.invalidate();
    g_client_bot_state.no_move_target_watchdog_retries = 0;
}

void reset_position_stall_watchdog()
{
    g_client_bot_state.position_stall_wd.reset();
}

void reset_objective_progress_watchdog(bool full_reset)
{
    g_client_bot_state.objective_progress_watchdog_timer.invalidate();
    if (!full_reset) {
        return;
    }

    g_client_bot_state.objective_progress_retry_window_timer.invalidate();
    g_client_bot_state.objective_progress_watchdog_goal = BotGoalType::none;
    g_client_bot_state.objective_progress_watchdog_fsm = BotFsmState::inactive;
    g_client_bot_state.objective_progress_watchdog_handle = -1;
    g_client_bot_state.objective_progress_watchdog_identifier = -1;
    g_client_bot_state.objective_progress_watchdog_waypoint = 0;
    g_client_bot_state.objective_progress_watchdog_route_waypoint = 0;
    g_client_bot_state.objective_progress_watchdog_route_next_index = 0;
    g_client_bot_state.objective_progress_watchdog_best_dist_sq = std::numeric_limits<float>::infinity();
    g_client_bot_state.objective_progress_watchdog_retry_count = 0;
}

void reset_global_failsafe_watchdog(bool full_reset)
{
    if (full_reset) {
        g_client_bot_state.global_failsafe_wd.reset();
        return;
    }

    g_client_bot_state.global_failsafe_wd.timer.invalidate();
    g_client_bot_state.global_failsafe_wd.origin = {};
    g_client_bot_state.global_failsafe_wd.aim_dir = {};
}

int get_route_current_waypoint()
{
    const int route_len = static_cast<int>(g_client_bot_state.waypoint_path.size());
    const int next_index = g_client_bot_state.waypoint_next_index;
    if (next_index > 0 && next_index - 1 < route_len) {
        return g_client_bot_state.waypoint_path[next_index - 1];
    }
    if (next_index >= 0 && next_index < route_len) {
        return g_client_bot_state.waypoint_path[next_index];
    }
    return 0;
}

int get_route_next_waypoint()
{
    const int route_len = static_cast<int>(g_client_bot_state.waypoint_path.size());
    const int next_index = g_client_bot_state.waypoint_next_index;
    if (next_index >= 0 && next_index < route_len) {
        return g_client_bot_state.waypoint_path[next_index];
    }
    return 0;
}

bool resolve_objective_progress_focus(
    const rf::Entity& entity,
    rf::Vector3& out_focus_pos,
    int& out_focus_waypoint,
    float& out_dist_sq)
{
    out_focus_waypoint = 0;

    if (g_client_bot_state.goal_target_waypoint > 0
        && waypoints_get_pos(g_client_bot_state.goal_target_waypoint, out_focus_pos)) {
        out_focus_waypoint = g_client_bot_state.goal_target_waypoint;
    }
    else if (g_client_bot_state.goal_target_pos.len_sq() > 0.0001f) {
        out_focus_pos = g_client_bot_state.goal_target_pos;
    }
    else if (g_client_bot_state.waypoint_goal > 0
             && waypoints_get_pos(g_client_bot_state.waypoint_goal, out_focus_pos)) {
        out_focus_waypoint = g_client_bot_state.waypoint_goal;
    }
    else if (g_client_bot_state.has_waypoint_target) {
        out_focus_pos = g_client_bot_state.waypoint_target_pos;
        out_focus_waypoint = get_route_next_waypoint();
    }
    else {
        return false;
    }

    out_dist_sq = rf::vec_dist_squared(&entity.pos, &out_focus_pos);
    return true;
}

void capture_objective_progress_watchdog_snapshot(
    const int focus_waypoint,
    const float dist_sq)
{
    g_client_bot_state.objective_progress_watchdog_goal = g_client_bot_state.active_goal;
    g_client_bot_state.objective_progress_watchdog_fsm = g_client_bot_state.fsm_state;
    g_client_bot_state.objective_progress_watchdog_handle = g_client_bot_state.goal_target_handle;
    g_client_bot_state.objective_progress_watchdog_identifier = g_client_bot_state.goal_target_identifier;
    g_client_bot_state.objective_progress_watchdog_waypoint = focus_waypoint;
    g_client_bot_state.objective_progress_watchdog_route_waypoint = get_route_next_waypoint();
    g_client_bot_state.objective_progress_watchdog_route_next_index =
        g_client_bot_state.waypoint_next_index;
    g_client_bot_state.objective_progress_watchdog_best_dist_sq = dist_sq;
    g_client_bot_state.objective_progress_watchdog_timer.set(kObjectiveProgressWatchdogMs);
}

ObjectiveProgressWatchdogAction update_objective_progress_watchdog(
    rf::Entity& entity,
    const rf::Entity* enemy_target,
    const bool enemy_has_los,
    const bool pursuing_enemy_goal,
    const bool combat_lock_expired)
{
    if (g_client_bot_state.active_goal == BotGoalType::none
        || g_client_bot_state.active_goal == BotGoalType::eliminate_target) {
        reset_objective_progress_watchdog(true);
        return ObjectiveProgressWatchdogAction::none;
    }

    const bool active_combat_lock =
        enemy_target
        && pursuing_enemy_goal
        && enemy_has_los
        && (g_client_bot_state.firing.wants_fire
            || g_client_bot_state.firing.synthetic_primary_fire_down
            || g_client_bot_state.firing.synthetic_secondary_fire_down);
    if (active_combat_lock && !combat_lock_expired) {
        reset_objective_progress_watchdog(true);
        return ObjectiveProgressWatchdogAction::none;
    }

    rf::Vector3 focus_pos{};
    int focus_waypoint = 0;
    float dist_sq = 0.0f;
    if (!resolve_objective_progress_focus(entity, focus_pos, focus_waypoint, dist_sq)) {
        reset_objective_progress_watchdog(true);
        return ObjectiveProgressWatchdogAction::none;
    }

    const float near_goal_dist_sq =
        kObjectiveProgressNearGoalDistance * kObjectiveProgressNearGoalDistance;
    if (dist_sq <= near_goal_dist_sq) {
        reset_objective_progress_watchdog(true);
        return ObjectiveProgressWatchdogAction::none;
    }

    const bool same_goal_key =
        g_client_bot_state.active_goal == g_client_bot_state.objective_progress_watchdog_goal
        && g_client_bot_state.fsm_state == g_client_bot_state.objective_progress_watchdog_fsm
        && g_client_bot_state.goal_target_handle == g_client_bot_state.objective_progress_watchdog_handle
        && g_client_bot_state.goal_target_identifier
            == g_client_bot_state.objective_progress_watchdog_identifier
        && focus_waypoint == g_client_bot_state.objective_progress_watchdog_waypoint;
    if (!same_goal_key || !g_client_bot_state.objective_progress_watchdog_timer.valid()) {
        g_client_bot_state.objective_progress_watchdog_retry_count = 0;
        g_client_bot_state.objective_progress_retry_window_timer.set(kObjectiveProgressRetryWindowMs);
        capture_objective_progress_watchdog_snapshot(focus_waypoint, dist_sq);
        return ObjectiveProgressWatchdogAction::none;
    }

    const int route_waypoint = get_route_next_waypoint();
    const int route_next_index = g_client_bot_state.waypoint_next_index;
    const bool route_progressed =
        route_waypoint != g_client_bot_state.objective_progress_watchdog_route_waypoint
        || route_next_index != g_client_bot_state.objective_progress_watchdog_route_next_index;
    g_client_bot_state.objective_progress_watchdog_route_waypoint = route_waypoint;
    g_client_bot_state.objective_progress_watchdog_route_next_index = route_next_index;

    if (!std::isfinite(g_client_bot_state.objective_progress_watchdog_best_dist_sq)) {
        g_client_bot_state.objective_progress_watchdog_best_dist_sq = dist_sq;
    }
    const float improvement =
        g_client_bot_state.objective_progress_watchdog_best_dist_sq - dist_sq;
    const float min_improvement_sq =
        kObjectiveProgressMinImprovement * kObjectiveProgressMinImprovement;
    const bool objective_progressed = improvement >= min_improvement_sq;

    if (objective_progressed) {
        g_client_bot_state.objective_progress_watchdog_best_dist_sq = dist_sq;
    }

    if (route_progressed || objective_progressed) {
        g_client_bot_state.objective_progress_watchdog_timer.set(kObjectiveProgressWatchdogMs);
        return ObjectiveProgressWatchdogAction::none;
    }

    if (!g_client_bot_state.objective_progress_watchdog_timer.elapsed()) {
        return ObjectiveProgressWatchdogAction::none;
    }

    if (!g_client_bot_state.objective_progress_retry_window_timer.valid()
        || g_client_bot_state.objective_progress_retry_window_timer.elapsed()) {
        g_client_bot_state.objective_progress_watchdog_retry_count = 0;
        g_client_bot_state.objective_progress_retry_window_timer.set(
            kObjectiveProgressRetryWindowMs
        );
    }

    ++g_client_bot_state.objective_progress_watchdog_retry_count;
    const int preferred_avoid_waypoint =
        route_waypoint > 0
            ? route_waypoint
            : (focus_waypoint > 0
                ? focus_waypoint
                : (g_client_bot_state.goal_target_waypoint > 0
                    ? g_client_bot_state.goal_target_waypoint
                    : g_client_bot_state.waypoint_goal));
    const int blacklist_waypoint =
        bot_nav_choose_blacklist_waypoint_for_failed_link(0, preferred_avoid_waypoint);
    if (blacklist_waypoint > 0) {
        bot_nav_blacklist_waypoint_temporarily(
            blacklist_waypoint,
            kFailedWaypointBlacklistMs
        );
    }
    const int avoid_waypoint =
        blacklist_waypoint > 0
            ? blacklist_waypoint
            : preferred_avoid_waypoint;

    if (g_client_bot_state.objective_progress_watchdog_retry_count >= kObjectiveProgressRetryLimit) {
        if (g_alpine_game_config.dbg_bot) {
            xlog::warn(
                "Bot objective-progress watchdog: abandoning goal={} fsm={} retries={}/{} handle={} id={} waypoint={} dist={:.2f}",
                static_cast<int>(g_client_bot_state.active_goal),
                static_cast<int>(g_client_bot_state.fsm_state),
                g_client_bot_state.objective_progress_watchdog_retry_count,
                kObjectiveProgressRetryLimit,
                g_client_bot_state.goal_target_handle,
                g_client_bot_state.goal_target_identifier,
                avoid_waypoint,
                std::sqrt(std::max(dist_sq, 0.0f))
            );
        }

        const BotGoalType stalled_goal = g_client_bot_state.active_goal;
        const int stalled_handle = g_client_bot_state.goal_target_handle;
        bot_memory_manager_note_failed_goal_target(
            stalled_goal,
            stalled_handle,
            kFailedItemGoalCooldownMs
        );

        bot_state_set_roam_fallback_goal(220);
        g_client_bot_state.eliminate_target_reacquire_timer.invalidate();
        bot_state_clear_waypoint_route(true, true, false);
        bot_internal_set_last_heading_change_reason("objective_progress_abandon");
        reset_objective_progress_watchdog(true);
        return ObjectiveProgressWatchdogAction::abandon_goal;
    }

    if (g_alpine_game_config.dbg_bot) {
        xlog::warn(
            "Bot objective-progress watchdog: rerouting goal={} fsm={} retry={}/{} handle={} id={} waypoint={} dist={:.2f}",
            static_cast<int>(g_client_bot_state.active_goal),
            static_cast<int>(g_client_bot_state.fsm_state),
            g_client_bot_state.objective_progress_watchdog_retry_count,
            kObjectiveProgressRetryLimit,
            g_client_bot_state.goal_target_handle,
            g_client_bot_state.goal_target_identifier,
            avoid_waypoint,
            std::sqrt(std::max(dist_sq, 0.0f))
        );
    }

    bot_internal_start_recovery_anchor_reroute(entity, avoid_waypoint);
    bot_internal_set_last_heading_change_reason("objective_progress_reroute");
    g_client_bot_state.objective_progress_watchdog_timer.set(kObjectiveProgressWatchdogMs);
    return ObjectiveProgressWatchdogAction::reroute_same_goal;
}

void capture_global_failsafe_watchdog_snapshot(const rf::Entity& entity)
{
    rf::Vector3 aim_dir = entity.eye_orient.fvec;
    if (aim_dir.len_sq() < 0.0001f) {
        aim_dir = entity.orient.fvec;
    }
    aim_dir.normalize_safe();

    g_client_bot_state.global_failsafe_wd.origin = entity.pos;
    g_client_bot_state.global_failsafe_wd.aim_dir = aim_dir;
    g_client_bot_state.global_failsafe_wd.goal = g_client_bot_state.active_goal;
    g_client_bot_state.global_failsafe_wd.fsm = g_client_bot_state.fsm_state;
    g_client_bot_state.global_failsafe_wd.handle = g_client_bot_state.goal_target_handle;
    g_client_bot_state.global_failsafe_wd.identifier = g_client_bot_state.goal_target_identifier;
    g_client_bot_state.global_failsafe_wd.last_fired_timestamp = entity.last_fired_timestamp.value;
    g_client_bot_state.global_failsafe_wd.timer.set(kGlobalFailsafeWatchdogMs);
}

bool has_global_failsafe_context(
    const bool has_move_target,
    const bool pursuing_enemy_goal,
    const bool enemy_has_los)
{
    if (g_client_bot_state.active_goal != BotGoalType::none
        || g_client_bot_state.fsm_state != BotFsmState::idle
        || g_client_bot_state.has_waypoint_target
        || !g_client_bot_state.waypoint_path.empty()
        || g_client_bot_state.recovery_pending_reroute
        || has_move_target) {
        return true;
    }

    return bot_fsm_state_should_have_move_target(
        g_client_bot_state.fsm_state,
        pursuing_enemy_goal,
        enemy_has_los
    );
}

GlobalFailsafeWatchdogAction update_global_failsafe_watchdog(
    rf::Entity& entity,
    const rf::Entity* enemy_target,
    const bool enemy_has_los,
    const bool pursuing_enemy_goal,
    const bool has_move_target,
    const rf::Vector3& aim_dir,
    const bool combat_lock_expired)
{
    if (!has_global_failsafe_context(has_move_target, pursuing_enemy_goal, enemy_has_los)) {
        reset_global_failsafe_watchdog(true);
        return GlobalFailsafeWatchdogAction::none;
    }

    if (!g_client_bot_state.global_failsafe_wd.timer.valid()) {
        capture_global_failsafe_watchdog_snapshot(entity);
        return GlobalFailsafeWatchdogAction::none;
    }

    const bool active_combat_lock =
        enemy_target
        && pursuing_enemy_goal
        && enemy_has_los
        && (g_client_bot_state.firing.wants_fire
            || g_client_bot_state.firing.synthetic_primary_fire_down
            || g_client_bot_state.firing.synthetic_secondary_fire_down);
    if (active_combat_lock && !combat_lock_expired) {
        capture_global_failsafe_watchdog_snapshot(entity);
        g_client_bot_state.global_failsafe_wd.retry_window_timer.invalidate();
        g_client_bot_state.global_failsafe_wd.retry_count = 0;
        return GlobalFailsafeWatchdogAction::none;
    }

    const rf::Vector3 delta_pos = entity.pos - g_client_bot_state.global_failsafe_wd.origin;
    const bool moved =
        delta_pos.len_sq() > kGlobalFailsafeStillDistance * kGlobalFailsafeStillDistance;
    const float aim_dot = std::clamp(
        aim_dir.dot_prod(g_client_bot_state.global_failsafe_wd.aim_dir),
        -1.0f,
        1.0f
    );
    const bool reaimed = aim_dot < kGlobalFailsafeAimDotThreshold;
    const bool goal_or_fsm_changed =
        g_client_bot_state.active_goal != g_client_bot_state.global_failsafe_wd.goal
        || g_client_bot_state.fsm_state != g_client_bot_state.global_failsafe_wd.fsm
        || g_client_bot_state.goal_target_handle != g_client_bot_state.global_failsafe_wd.handle
        || g_client_bot_state.goal_target_identifier != g_client_bot_state.global_failsafe_wd.identifier;
    const bool fired_weapon =
        entity.last_fired_timestamp.value
        != g_client_bot_state.global_failsafe_wd.last_fired_timestamp;

    if (moved || reaimed || goal_or_fsm_changed || fired_weapon) {
        capture_global_failsafe_watchdog_snapshot(entity);
        g_client_bot_state.global_failsafe_wd.retry_window_timer.invalidate();
        g_client_bot_state.global_failsafe_wd.retry_count = 0;
        return GlobalFailsafeWatchdogAction::none;
    }

    if (!g_client_bot_state.global_failsafe_wd.timer.elapsed()) {
        return GlobalFailsafeWatchdogAction::none;
    }

    if (!g_client_bot_state.global_failsafe_wd.retry_window_timer.valid()
        || g_client_bot_state.global_failsafe_wd.retry_window_timer.elapsed()) {
        g_client_bot_state.global_failsafe_wd.retry_count = 0;
        g_client_bot_state.global_failsafe_wd.retry_window_timer.set(kGlobalFailsafeRetryWindowMs);
    }
    ++g_client_bot_state.global_failsafe_wd.retry_count;

    const int route_waypoint = get_route_current_waypoint();
    const int preferred_avoid_waypoint =
        route_waypoint > 0
            ? route_waypoint
            : (g_client_bot_state.goal_target_waypoint > 0
                ? g_client_bot_state.goal_target_waypoint
                : g_client_bot_state.waypoint_goal);
    const int blacklist_waypoint =
        bot_nav_choose_blacklist_waypoint_for_failed_link(0, preferred_avoid_waypoint);
    if (blacklist_waypoint > 0) {
        bot_nav_blacklist_waypoint_temporarily(
            blacklist_waypoint,
            kFailedWaypointBlacklistMs
        );
    }
    const int avoid_waypoint =
        blacklist_waypoint > 0
            ? blacklist_waypoint
            : preferred_avoid_waypoint;

    if (g_client_bot_state.global_failsafe_wd.retry_count >= kGlobalFailsafeRetryLimit) {
        if (g_alpine_game_config.dbg_bot) {
            xlog::warn(
                "Bot global-failsafe watchdog: forcing goal reset goal={} fsm={} retries={}/{} handle={} id={} waypoint={}",
                static_cast<int>(g_client_bot_state.active_goal),
                static_cast<int>(g_client_bot_state.fsm_state),
                g_client_bot_state.global_failsafe_wd.retry_count,
                kGlobalFailsafeRetryLimit,
                g_client_bot_state.goal_target_handle,
                g_client_bot_state.goal_target_identifier,
                avoid_waypoint
            );
        }
        abort_goal_and_fsm_for_confusion(entity);
        bot_internal_set_last_heading_change_reason("global_failsafe_goal_reset");
        g_client_bot_state.global_failsafe_wd.retry_window_timer.invalidate();
        g_client_bot_state.global_failsafe_wd.retry_count = 0;
    }
    else {
        if (g_alpine_game_config.dbg_bot) {
            xlog::warn(
                "Bot global-failsafe watchdog: forcing reroute goal={} fsm={} retry={}/{} handle={} id={} waypoint={}",
                static_cast<int>(g_client_bot_state.active_goal),
                static_cast<int>(g_client_bot_state.fsm_state),
                g_client_bot_state.global_failsafe_wd.retry_count,
                kGlobalFailsafeRetryLimit,
                g_client_bot_state.goal_target_handle,
                g_client_bot_state.goal_target_identifier,
                avoid_waypoint
            );
        }
        bot_internal_start_recovery_anchor_reroute(entity, avoid_waypoint);
        bot_internal_set_last_heading_change_reason("global_failsafe_reroute");
    }

    capture_global_failsafe_watchdog_snapshot(entity);
    return GlobalFailsafeWatchdogAction::recovery_triggered;
}

PositionStallWatchdogAction update_position_stall_watchdog(
    const rf::Entity& entity,
    const bool has_move_target,
    const bool pursuing_enemy_goal,
    const bool enemy_has_los)
{
    const bool movement_expected =
        has_move_target
        && bot_fsm_state_should_have_move_target(
            g_client_bot_state.fsm_state,
            pursuing_enemy_goal,
            enemy_has_los
        );
    if (g_client_bot_state.active_goal == BotGoalType::none) {
        reset_position_stall_watchdog();
        return PositionStallWatchdogAction::none;
    }
    if (!movement_expected) {
        // Preserve retry budget across brief no-target/no-move frames so we do
        // not keep bouncing at retry=1 when the bot is jittering in recovery.
        g_client_bot_state.position_stall_wd.timer.invalidate();
        g_client_bot_state.position_stall_wd.origin = {};
        g_client_bot_state.position_stall_wd.waypoint = 0;
        return PositionStallWatchdogAction::none;
    }

    const BotGoalType goal = g_client_bot_state.active_goal;
    const BotFsmState fsm = g_client_bot_state.fsm_state;
    const int handle = g_client_bot_state.goal_target_handle;
    const int identifier = g_client_bot_state.goal_target_identifier;
    const bool same_goal_key =
        goal == g_client_bot_state.position_stall_wd.goal
        && handle == g_client_bot_state.position_stall_wd.handle
        && identifier == g_client_bot_state.position_stall_wd.identifier;
    if (!same_goal_key) {
        g_client_bot_state.position_stall_wd.goal = goal;
        g_client_bot_state.position_stall_wd.handle = handle;
        g_client_bot_state.position_stall_wd.identifier = identifier;
        g_client_bot_state.position_stall_wd.retry_window_timer.invalidate();
        g_client_bot_state.position_stall_wd.retry_count = 0;
    }

    const bool in_recovery =
        g_client_bot_state.fsm_state == BotFsmState::recover_navigation
        || g_client_bot_state.recovery_pending_reroute;
    const int stall_ms = in_recovery ? kPositionStallWatchdogRecoveryMs : kPositionStallWatchdogMs;

    const int current_waypoint = get_route_current_waypoint();
    if (!g_client_bot_state.position_stall_wd.timer.valid()) {
        g_client_bot_state.position_stall_wd.origin = entity.pos;
        g_client_bot_state.position_stall_wd.waypoint = current_waypoint;
        g_client_bot_state.position_stall_wd.timer.set(stall_ms);
        return PositionStallWatchdogAction::none;
    }

    const rf::Vector3 delta = entity.pos - g_client_bot_state.position_stall_wd.origin;
    const float moved_sq = delta.len_sq();
    if (moved_sq
        > kPositionStallWatchdogStillDistance * kPositionStallWatchdogStillDistance) {
        const bool route_waypoint_progressed =
            current_waypoint > 0
            && current_waypoint != g_client_bot_state.position_stall_wd.waypoint;
        const bool moved_far_enough_to_clear_retry_budget =
            moved_sq > kPositionStallRetryResetDistance * kPositionStallRetryResetDistance;
        g_client_bot_state.position_stall_wd.origin = entity.pos;
        g_client_bot_state.position_stall_wd.waypoint = current_waypoint;
        g_client_bot_state.position_stall_wd.timer.set(stall_ms);
        if (route_waypoint_progressed || moved_far_enough_to_clear_retry_budget) {
            g_client_bot_state.position_stall_wd.retry_window_timer.invalidate();
            g_client_bot_state.position_stall_wd.retry_count = 0;
        }
        return PositionStallWatchdogAction::none;
    }

    if (!g_client_bot_state.position_stall_wd.timer.elapsed()) {
        return PositionStallWatchdogAction::none;
    }

    int stalled_waypoint = g_client_bot_state.position_stall_wd.waypoint;
    if (stalled_waypoint <= 0) {
        stalled_waypoint = current_waypoint;
    }

    const int blacklist_waypoint =
        bot_nav_choose_blacklist_waypoint_for_failed_link(0, stalled_waypoint);
    if (blacklist_waypoint > 0) {
        bot_nav_blacklist_waypoint_temporarily(
            blacklist_waypoint,
            kFailedWaypointBlacklistMs
        );
    }

    const int avoid_waypoint =
        blacklist_waypoint > 0
            ? blacklist_waypoint
            : (stalled_waypoint > 0
                ? stalled_waypoint
                : (g_client_bot_state.goal_target_waypoint > 0
                    ? g_client_bot_state.goal_target_waypoint
                    : g_client_bot_state.waypoint_goal));

    if (!g_client_bot_state.position_stall_wd.retry_window_timer.valid()
        || g_client_bot_state.position_stall_wd.retry_window_timer.elapsed()) {
        g_client_bot_state.position_stall_wd.retry_count = 0;
        g_client_bot_state.position_stall_wd.retry_window_timer.set(kPositionStallRetryWindowMs);
    }
    ++g_client_bot_state.position_stall_wd.retry_count;

    int retry_limit = kPositionStallRetryLimit;
    if (goal == BotGoalType::eliminate_target
        && (fsm == BotFsmState::recover_navigation
            || g_client_bot_state.recovery_pending_reroute)) {
        retry_limit = 2;
    }
    retry_limit = std::max(retry_limit, 1);

    if (g_client_bot_state.position_stall_wd.retry_count >= retry_limit) {
        if (g_alpine_game_config.dbg_bot) {
            xlog::warn(
                "Bot position-stall watchdog: abandoning goal={} fsm={} retries={}/{} handle={} id={} waypoint={}",
                static_cast<int>(goal),
                static_cast<int>(fsm),
                g_client_bot_state.position_stall_wd.retry_count,
                retry_limit,
                handle,
                identifier,
                avoid_waypoint
            );
        }

        const BotGoalType stalled_goal = g_client_bot_state.active_goal;
        const int stalled_handle = g_client_bot_state.goal_target_handle;
        bot_memory_manager_note_failed_goal_target(
            stalled_goal,
            stalled_handle,
            kFailedItemGoalCooldownMs
        );

        bot_state_set_roam_fallback_goal(220);
        g_client_bot_state.eliminate_target_reacquire_timer.invalidate();
        bot_state_clear_waypoint_route(true, true, false);
        bot_internal_set_last_heading_change_reason("position_stall_abandon");
        reset_position_stall_watchdog();
        return PositionStallWatchdogAction::abandon_goal;
    }

    if (g_alpine_game_config.dbg_bot) {
        xlog::warn(
            "Bot position-stall watchdog: rerouting goal={} fsm={} retry={}/{} avoid_wp={} route_wp={} goal_wp={}",
            static_cast<int>(g_client_bot_state.active_goal),
            static_cast<int>(g_client_bot_state.fsm_state),
            g_client_bot_state.position_stall_wd.retry_count,
            retry_limit,
            avoid_waypoint,
            stalled_waypoint,
            g_client_bot_state.goal_target_waypoint
        );
    }

    bot_internal_start_recovery_anchor_reroute(entity, avoid_waypoint);
    bot_internal_set_last_heading_change_reason("position_stall_reroute");
    g_client_bot_state.position_stall_wd.origin = entity.pos;
    g_client_bot_state.position_stall_wd.waypoint = get_route_current_waypoint();
    g_client_bot_state.position_stall_wd.timer.set(stall_ms);
    return PositionStallWatchdogAction::reroute_same_goal;
}

GoalStuckWatchdogAction update_goal_stuck_watchdog(rf::Entity& entity)
{
    if (g_client_bot_state.active_goal == BotGoalType::none) {
        reset_goal_stuck_watchdog(true);
        reset_no_move_target_watchdog();
        reset_position_stall_watchdog();
        reset_objective_progress_watchdog(true);
        return GoalStuckWatchdogAction::none;
    }

    const BotGoalType goal = g_client_bot_state.active_goal;
    const BotFsmState fsm = g_client_bot_state.fsm_state;
    const int handle = g_client_bot_state.goal_target_handle;
    const int identifier = g_client_bot_state.goal_target_identifier;
    const int waypoint = g_client_bot_state.goal_target_waypoint;
    const int route_len = static_cast<int>(g_client_bot_state.waypoint_path.size());
    const int route_next_index = g_client_bot_state.waypoint_next_index;
    const int route_waypoint =
        (route_next_index >= 0 && route_next_index < route_len)
            ? g_client_bot_state.waypoint_path[route_next_index]
            : 0;

    int watchdog_ms = kBotGoalStuckWatchdogMs;
    if (bot_goal_is_control_point_objective(goal) || fsm == BotFsmState::control_point_objective) {
        watchdog_ms = std::max(watchdog_ms, 3200);
    }
    if (fsm == BotFsmState::recover_navigation || g_client_bot_state.recovery_pending_reroute) {
        watchdog_ms = std::max(watchdog_ms, 3500);
    }

    const bool same_goal_key =
        goal == g_client_bot_state.goal_stuck_wd.goal
        && handle == g_client_bot_state.goal_stuck_wd.handle
        && identifier == g_client_bot_state.goal_stuck_wd.identifier;
    if (!same_goal_key) {
        g_client_bot_state.goal_stuck_wd.goal = goal;
        g_client_bot_state.goal_stuck_wd.fsm = fsm;
        g_client_bot_state.goal_stuck_wd.handle = handle;
        g_client_bot_state.goal_stuck_wd.identifier = identifier;
        g_client_bot_state.goal_stuck_wd.waypoint = waypoint;
        g_client_bot_state.goal_stuck_wd.route_waypoint = route_waypoint;
        g_client_bot_state.goal_stuck_wd.route_next_index = route_next_index;
        g_client_bot_state.goal_stuck_wd.recovery_anchor =
            g_client_bot_state.recovery_anchor_waypoint;
        g_client_bot_state.goal_stuck_wd.retry_count = 0;
        g_client_bot_state.goal_stuck_wd.retry_window_timer.set(kBotGoalStuckRetryWindowMs);
        g_client_bot_state.goal_stuck_wd.origin = entity.pos;
        g_client_bot_state.goal_stuck_wd.timer.set(watchdog_ms);
        return GoalStuckWatchdogAction::none;
    }

    if (!g_client_bot_state.goal_stuck_wd.retry_window_timer.valid()
        || g_client_bot_state.goal_stuck_wd.retry_window_timer.elapsed()) {
        g_client_bot_state.goal_stuck_wd.retry_count = 0;
        g_client_bot_state.goal_stuck_wd.retry_window_timer.set(kBotGoalStuckRetryWindowMs);
    }
    g_client_bot_state.goal_stuck_wd.waypoint = waypoint;

    const bool route_progressed =
        (route_waypoint > 0
            && route_waypoint != g_client_bot_state.goal_stuck_wd.route_waypoint)
        || (route_next_index > g_client_bot_state.goal_stuck_wd.route_next_index)
        || (g_client_bot_state.recovery_anchor_waypoint > 0
            && g_client_bot_state.recovery_anchor_waypoint
                != g_client_bot_state.goal_stuck_wd.recovery_anchor);
    g_client_bot_state.goal_stuck_wd.route_waypoint = route_waypoint;
    g_client_bot_state.goal_stuck_wd.route_next_index = route_next_index;
    g_client_bot_state.goal_stuck_wd.recovery_anchor =
        g_client_bot_state.recovery_anchor_waypoint;
    if (route_progressed) {
        g_client_bot_state.goal_stuck_wd.origin = entity.pos;
        g_client_bot_state.goal_stuck_wd.timer.set(watchdog_ms);
        return GoalStuckWatchdogAction::none;
    }

    if (fsm != g_client_bot_state.goal_stuck_wd.fsm) {
        g_client_bot_state.goal_stuck_wd.fsm = fsm;
        g_client_bot_state.goal_stuck_wd.origin = entity.pos;
        g_client_bot_state.goal_stuck_wd.timer.set(watchdog_ms);
        return GoalStuckWatchdogAction::none;
    }

    if (!g_client_bot_state.goal_stuck_wd.timer.valid()) {
        g_client_bot_state.goal_stuck_wd.origin = entity.pos;
        g_client_bot_state.goal_stuck_wd.timer.set(watchdog_ms);
        return GoalStuckWatchdogAction::none;
    }
    if (!g_client_bot_state.goal_stuck_wd.timer.elapsed()) {
        return GoalStuckWatchdogAction::none;
    }

    rf::Vector3 moved = entity.pos - g_client_bot_state.goal_stuck_wd.origin;
    moved.y = 0.0f;
    if (moved.len_sq() > kBotGoalStuckStillDistance * kBotGoalStuckStillDistance) {
        g_client_bot_state.goal_stuck_wd.origin = entity.pos;
        g_client_bot_state.goal_stuck_wd.timer.set(watchdog_ms);
        return GoalStuckWatchdogAction::none;
    }

    const int retry_limit = std::clamp(
        get_active_bot_personality().stuck_goal_retry_limit,
        1,
        16
    );
    ++g_client_bot_state.goal_stuck_wd.retry_count;
    g_client_bot_state.goal_stuck_wd.origin = entity.pos;
    g_client_bot_state.goal_stuck_wd.timer.set(watchdog_ms);

    if (g_client_bot_state.goal_stuck_wd.retry_count >= retry_limit) {
        if (g_alpine_game_config.dbg_bot) {
            xlog::warn(
                "Bot stuck-goal watchdog: abandoning goal={} fsm={} retries={}/{} handle={} id={} waypoint={}",
                static_cast<int>(goal),
                static_cast<int>(fsm),
                g_client_bot_state.goal_stuck_wd.retry_count,
                retry_limit,
                handle,
                identifier,
                waypoint
            );
        }

        bot_memory_manager_note_failed_goal_target(
            goal,
            handle,
            kFailedItemGoalCooldownMs
        );

        bot_state_set_roam_fallback_goal(220);
        g_client_bot_state.eliminate_target_reacquire_timer.invalidate();
        bot_state_clear_waypoint_route(true, true, false);
        bot_internal_set_last_heading_change_reason("stuck_goal_abandon");
        reset_goal_stuck_watchdog(true);
        reset_no_move_target_watchdog();
        reset_position_stall_watchdog();
        return GoalStuckWatchdogAction::abandon_goal;
    }

    const int avoid_waypoint = g_client_bot_state.goal_target_waypoint > 0
        ? g_client_bot_state.goal_target_waypoint
        : g_client_bot_state.waypoint_goal;
    if (g_alpine_game_config.dbg_bot) {
        xlog::warn(
            "Bot stuck-goal watchdog: rerouting goal={} fsm={} retry={}/{} handle={} id={} waypoint={}",
            static_cast<int>(goal),
            static_cast<int>(fsm),
            g_client_bot_state.goal_stuck_wd.retry_count,
            retry_limit,
            handle,
            identifier,
            avoid_waypoint
        );
    }

    bot_internal_start_recovery_anchor_reroute(entity, avoid_waypoint);
    bot_internal_set_last_heading_change_reason("stuck_goal_reroute");
    return GoalStuckWatchdogAction::reroute_same_goal;
}

void update_retaliation_context(const rf::Player& local_player, const rf::Entity& local_entity)
{
    bot_memory_manager_update_context(local_player, local_entity);
}

bool find_nearest_other_waypoint_pos(
    const int origin_waypoint,
    const rf::Vector3& pos,
    rf::Vector3& out_pos)
{
    const int waypoint_total = waypoints_count();
    float best_dist_sq = std::numeric_limits<float>::max();
    bool found = false;
    rf::Vector3 candidate_pos{};

    for (int waypoint = 1; waypoint < waypoint_total; ++waypoint) {
        if (waypoint == origin_waypoint) {
            continue;
        }
        if (!waypoints_get_pos(waypoint, candidate_pos)) {
            continue;
        }

        const float dist_sq = rf::vec_dist_squared(&pos, &candidate_pos);
        if (dist_sq < best_dist_sq) {
            best_dist_sq = dist_sq;
            out_pos = candidate_pos;
            found = true;
        }
    }

    return found;
}

bool is_probe_direction_blocked(
    const rf::Entity& entity,
    const rf::Vector3& origin,
    const rf::Vector3& direction,
    const float distance)
{
    (void)entity;

    if (distance <= 0.0f || direction.len_sq() < 0.0001f) {
        return false;
    }

    rf::Vector3 dir = direction;
    dir.normalize_safe();
    const rf::Vector3 endpoint = origin + dir * distance;
    rf::Vector3 p0 = origin;
    rf::Vector3 p1 = endpoint;
    rf::GCollisionOutput collision{};
    return rf::collide_linesegment_level_solid(
        p0,
        p1,
        kBotNavProbeTraceFlags,
        &collision);
}

bool waypoint_can_directly_approach_item_pos(const int waypoint_uid, const rf::Vector3& item_pos)
{
    if (waypoint_uid <= 0) {
        return false;
    }

    rf::Vector3 waypoint_pos{};
    if (!waypoints_get_pos(waypoint_uid, waypoint_pos)) {
        return false;
    }

    constexpr float kItemApproachMaxDist = kWaypointLinkRadius * 1.35f;
    const float dist_sq = rf::vec_dist_squared(&waypoint_pos, &item_pos);
    if (dist_sq > kItemApproachMaxDist * kItemApproachMaxDist) {
        return false;
    }

    rf::Vector3 p0 = waypoint_pos;
    rf::Vector3 p1 = item_pos;
    rf::GCollisionOutput collision{};
    const bool blocked = rf::collide_linesegment_level_solid(
        p0,
        p1,
        kBotNavProbeTraceFlags,
        &collision
    );
    return !blocked;
}

bool find_item_goal_waypoint_internal(const rf::Vector3& item_pos, int& out_waypoint)
{
    out_waypoint = 0;

    const int nearest_waypoint = bot_find_closest_waypoint_with_fallback(item_pos);
    if (nearest_waypoint <= 0) {
        return false;
    }

    if (waypoint_can_directly_approach_item_pos(nearest_waypoint, item_pos)) {
        out_waypoint = nearest_waypoint;
        return true;
    }

    std::array<int, kMaxWaypointLinks> links{};
    const int link_count = waypoints_get_links(nearest_waypoint, links);
    int best_waypoint = 0;
    float best_dist_sq = std::numeric_limits<float>::max();
    for (int i = 0; i < link_count; ++i) {
        const int link_waypoint = links[i];
        if (link_waypoint <= 0 || link_waypoint == nearest_waypoint) {
            continue;
        }
        if (!waypoint_can_directly_approach_item_pos(link_waypoint, item_pos)) {
            continue;
        }

        rf::Vector3 link_pos{};
        if (!waypoints_get_pos(link_waypoint, link_pos)) {
            continue;
        }

        const float dist_sq = rf::vec_dist_squared(&link_pos, &item_pos);
        if (dist_sq < best_dist_sq) {
            best_dist_sq = dist_sq;
            best_waypoint = link_waypoint;
        }
    }

    if (best_waypoint > 0) {
        out_waypoint = best_waypoint;
        return true;
    }

    return false;
}

// find_closest_waypoint_with_los_fallback is defined in bot_utils.cpp

float get_entity_health_ratio(const rf::Entity& entity)
{
    return bot_decision_get_entity_health_ratio(entity);
}

float get_entity_armor_ratio(const rf::Entity& entity)
{
    return bot_decision_get_entity_armor_ratio(entity);
}

float apply_aggression_to_threshold(const float threshold, const float aggression_bias)
{
    return bot_decision_apply_aggression_to_threshold(threshold, aggression_bias);
}

BotWeaponRangeBand classify_combat_distance_band(const float distance)
{
    return bot_decision_classify_combat_distance_band(distance);
}

float compute_weapon_readiness_score_for_type(
    const rf::Entity& local_entity,
    const rf::Entity* enemy_target,
    const int weapon_type)
{
    return bot_decision_compute_weapon_readiness_score_for_type(
        local_entity,
        enemy_target,
        weapon_type);
}

float compute_weapon_readiness_score(
    const rf::Entity& local_entity,
    const rf::Entity* enemy_target)
{
    return bot_decision_compute_weapon_readiness_score(local_entity, enemy_target);
}

// resolve_weapon_type_cached and entity_has_weapon_type are defined in bot_utils.cpp

float compute_waypoint_path_length(const std::vector<int>& path)
{
    if (path.size() < 2) {
        return 0.0f;
    }

    rf::Vector3 prev_pos{};
    if (!waypoints_get_pos(path[0], prev_pos)) {
        return std::numeric_limits<float>::infinity();
    }

    float total_length = 0.0f;
    for (size_t index = 1; index < path.size(); ++index) {
        rf::Vector3 waypoint_pos{};
        if (!waypoints_get_pos(path[index], waypoint_pos)) {
            return std::numeric_limits<float>::infinity();
        }
        total_length += (waypoint_pos - prev_pos).len();
        prev_pos = waypoint_pos;
    }
    return total_length;
}

float get_waypoint_item_value(const int waypoint_index)
{
    int type = 0;
    int subtype = 0;
    if (!waypoints_get_type_subtype(waypoint_index, type, subtype) || type != 2) {
        return 0.0f;
    }

    if (subtype < 0 || subtype >= rf::num_item_types) {
        return 0.0f;
    }

    const rf::ItemInfo& item = rf::item_info[subtype];
    if (item.flags & rf::IIF_NO_PICKUP) {
        return 0.0f;
    }

    float value = bot_get_item_weight(subtype);
    if (item.gives_weapon_id >= 0) {
        value += 2.8f
            * bot_get_weapon_pickup_weight(item.gives_weapon_id)
            * bot_get_weapon_preference_weight(item.gives_weapon_id);
    }
    if (item.ammo_for_weapon_id >= 0) {
        value += 1.4f * bot_get_weapon_pickup_weight(item.ammo_for_weapon_id);
    }

    const int count = std::max(item.count_multi, item.count);
    value += std::clamp(static_cast<float>(count) * 0.06f, 0.0f, 2.2f);

    const float respawn_seconds =
        std::max(static_cast<float>(item.respawn_time_millis), 0.0f) / 1000.0f;
    value += std::clamp(respawn_seconds * 0.05f, 0.0f, 1.8f);
    return value;
}

float compute_waypoint_path_item_value(const std::vector<int>& path)
{
    if (path.size() < 2) {
        return 0.0f;
    }

    float total_item_value = 0.0f;
    std::unordered_set<int> visited;
    visited.reserve(path.size());
    for (size_t index = 1; index < path.size(); ++index) {
        const int waypoint = path[index];
        if (!visited.insert(waypoint).second) {
            continue;
        }
        total_item_value += get_waypoint_item_value(waypoint);
    }
    return total_item_value;
}

float compute_route_score(
    const std::vector<int>& path,
    const float target_distance = 0.0f)
{
    return bot_nav_compute_route_score(path, target_distance);
}

int choose_weighted_top_rank(const int candidate_count)
{
    if (candidate_count <= 1) {
        return 0;
    }

    float total_weight = 0.0f;
    for (int rank = 0; rank < candidate_count; ++rank) {
        total_weight += 1.0f / static_cast<float>(rank + 1);
    }

    std::uniform_real_distribution<float> roll_dist(0.0f, total_weight);
    const float roll = roll_dist(g_rng);
    float accumulated = 0.0f;
    for (int rank = 0; rank < candidate_count; ++rank) {
        accumulated += 1.0f / static_cast<float>(rank + 1);
        if (roll <= accumulated) {
            return rank;
        }
    }

    return candidate_count - 1;
}

void reset_blind_progress_tracking()
{
    g_client_bot_state.blind_progress_timer.invalidate();
    g_client_bot_state.blind_progress_origin = {};
    g_client_bot_state.blind_progress_max_dist_sq = 0.0f;
}

bool is_long_route_escape_active()
{
    if (g_client_bot_state.long_route_escape_timer.valid()
        && g_client_bot_state.long_route_escape_timer.elapsed()) {
        g_client_bot_state.long_route_escape_timer.invalidate();
    }
    return g_client_bot_state.long_route_escape_timer.valid();
}

void update_blind_escape_state(
    const rf::Entity& local_entity,
    const rf::Entity* enemy_target,
    const bool enemy_has_los)
{
    if (!enemy_target || enemy_has_los) {
        reset_blind_progress_tracking();
        g_client_bot_state.long_route_escape_timer.invalidate();
        return;
    }

    if (!g_client_bot_state.blind_progress_timer.valid()) {
        g_client_bot_state.blind_progress_timer.set(kBlindProgressWindowMs);
        g_client_bot_state.blind_progress_origin = local_entity.pos;
        g_client_bot_state.blind_progress_max_dist_sq = 0.0f;
    }

    rf::Vector3 displacement = local_entity.pos - g_client_bot_state.blind_progress_origin;
    displacement.y = 0.0f;
    g_client_bot_state.blind_progress_max_dist_sq = std::max(
        g_client_bot_state.blind_progress_max_dist_sq,
        displacement.len_sq()
    );

    if (!g_client_bot_state.blind_progress_timer.elapsed()) {
        return;
    }

    const float dist_threshold_sq =
        kBlindProgressDistanceThreshold * kBlindProgressDistanceThreshold;
    if (g_client_bot_state.blind_progress_max_dist_sq < dist_threshold_sq) {
        std::uniform_int_distribution<int> long_route_dist(
            kLongRouteEscapeMinMs,
            kLongRouteEscapeMaxMs
        );
        g_client_bot_state.long_route_escape_timer.set(long_route_dist(g_rng));
        clear_waypoint_route();
    }

    g_client_bot_state.blind_progress_timer.set(kBlindProgressWindowMs);
    g_client_bot_state.blind_progress_origin = local_entity.pos;
    g_client_bot_state.blind_progress_max_dist_sq = 0.0f;
}

void trigger_loop_escape_mode()
{
    std::uniform_int_distribution<int> long_route_dist(kLoopEscapeMinMs, kLoopEscapeMaxMs);
    g_client_bot_state.long_route_escape_timer.set(long_route_dist(g_rng));
    g_client_bot_state.repath_timer.invalidate();
}

bool path_contains_waypoint(const std::vector<int>& path, const int waypoint)
{
    return waypoint > 0 && std::find(path.begin(), path.end(), waypoint) != path.end();
}

void prune_failed_edge_cooldowns()
{
    bot_nav_prune_failed_edge_cooldowns();
}

void register_failed_edge_cooldown(
    const int from_waypoint,
    const int to_waypoint,
    const int cooldown_ms)
{
    bot_nav_register_failed_edge_cooldown(from_waypoint, to_waypoint, cooldown_ms);
}

void register_failed_edge_cooldown_bidirectional(
    const int waypoint_a,
    const int waypoint_b,
    const int cooldown_ms)
{
    bot_nav_register_failed_edge_cooldown_bidirectional(
        waypoint_a,
        waypoint_b,
        cooldown_ms
    );
}

bool is_failed_edge_cooldown_active_no_prune(const int from_waypoint, const int to_waypoint)
{
    return bot_nav_is_failed_edge_cooldown_active_no_prune(from_waypoint, to_waypoint);
}

bool path_contains_failed_edge_cooldown(const std::vector<int>& path)
{
    return bot_nav_path_contains_failed_edge_cooldown(path);
}

bool is_super_pickup_name(const char* name)
{
    if (!name || !name[0]) {
        return false;
    }

    return std::strcmp(name, "Multi Damage Amplifier") == 0
        || std::strcmp(name, "Multi Invulnerability") == 0
        || std::strcmp(name, "Multi Super Armor") == 0
        || std::strcmp(name, "Multi Super Health") == 0;
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

bool item_info_contains_token(const rf::ItemInfo& item_info, std::string_view token)
{
    return contains_case_insensitive(item_info.cls_name.c_str(), token)
        || (item_info.hud_msg_name
            && contains_case_insensitive(item_info.hud_msg_name, token));
}

bool item_info_is_health_pickup(const rf::ItemInfo& item_info)
{
    return item_info_contains_token(item_info, "health")
        || item_info_contains_token(item_info, "med")
        || item_info_contains_token(item_info, "kit");
}

bool item_info_is_armor_pickup(const rf::ItemInfo& item_info)
{
    return item_info_contains_token(item_info, "armor");
}

const rf::ItemInfo* resolve_item_info(const rf::Item& item)
{
    if (item.info) {
        return item.info;
    }
    if (item.info_index >= 0 && item.info_index < rf::num_item_types) {
        return &rf::item_info[item.info_index];
    }
    return nullptr;
}

int resolve_item_type_index(const rf::Item& item)
{
    if (item.info_index >= 0 && item.info_index < rf::num_item_types) {
        return item.info_index;
    }
    if (!item.info || rf::num_item_types <= 0) {
        return -1;
    }

    const rf::ItemInfo* first = &rf::item_info[0];
    const rf::ItemInfo* last = first + rf::num_item_types;
    if (item.info >= first && item.info < last) {
        return static_cast<int>(item.info - first);
    }
    return -1;
}

bool item_matches_super_pickup(const rf::Item& item, const rf::ItemInfo* item_info)
{
    if (is_super_pickup_name(item.name.c_str())) {
        return true;
    }
    if (item_info) {
        return is_super_pickup_name(item_info->hud_msg_name)
            || is_super_pickup_name(item_info->cls_name.c_str());
    }
    return false;
}

bool item_matches_super_item_hoarder_target(const rf::Item& item, const rf::ItemInfo* item_info)
{
    if (item_matches_super_pickup(item, item_info)) {
        return true;
    }

    const int railgun_type = bot_resolve_weapon_type_cached("rail_gun");
    const int shoulder_cannon_type = bot_resolve_weapon_type_cached("shoulder_cannon");
    if (item_info) {
        const bool gives_super_weapon =
            item_info->gives_weapon_id == railgun_type
            || item_info->gives_weapon_id == shoulder_cannon_type;
        if (gives_super_weapon) {
            return true;
        }
    }

    const char* item_name = item.name.c_str();
    return contains_case_insensitive(item_name ? item_name : "", "rail_gun")
        || contains_case_insensitive(item_name ? item_name : "", "shoulder_cannon");
}

bool item_is_currently_available(const rf::Item& item)
{
    return (item.obj_flags & rf::OF_HIDDEN) == 0;
}

float get_weapon_ammo_fill_ratio(const rf::Entity& local_entity, const int weapon_type)
{
    if (weapon_type < 0 || weapon_type >= rf::num_weapon_types) {
        return 1.0f;
    }

    const rf::WeaponInfo& weapon_info = rf::weapon_types[weapon_type];
    const int ammo_type = weapon_info.ammo_type;
    int reserve_ammo = 0;
    if (ammo_type >= 0 && ammo_type < 32) {
        reserve_ammo = std::max(local_entity.ai.ammo[ammo_type], 0);
    }
    const int clip_ammo = std::max(local_entity.ai.clip_ammo[weapon_type], 0);
    const int current_ammo = reserve_ammo + clip_ammo;
    int max_ammo = std::max(weapon_info.max_ammo_multi, weapon_info.max_ammo);
    if (max_ammo <= 0) {
        max_ammo = weapon_info.max_ammo_single;
    }
    if (max_ammo <= 0) {
        return 1.0f;
    }
    return std::clamp(static_cast<float>(current_ammo) / static_cast<float>(max_ammo), 0.0f, 1.0f);
}

BotGoalType classify_collect_goal_for_item(const rf::Item& item, const rf::ItemInfo& item_info)
{
    if (item_matches_super_pickup(item, &item_info)) {
        return BotGoalType::collect_super_item;
    }
    if (item_info.gives_weapon_id >= 0) {
        return BotGoalType::collect_weapon;
    }
    if (item_info.ammo_for_weapon_id >= 0) {
        return BotGoalType::collect_ammo;
    }
    if (item_info_is_health_pickup(item_info)) {
        return BotGoalType::collect_health;
    }
    if (item_info_is_armor_pickup(item_info)) {
        return BotGoalType::collect_armor;
    }
    return BotGoalType::none;
}

bool is_item_goal_satisfied(
    const rf::Entity& local_entity,
    const BotGoalType goal_type,
    const rf::Item& item)
{
    if (!bot_goal_is_item_collection(goal_type)) {
        return false;
    }

    const rf::ItemInfo* item_info = resolve_item_info(item);
    if (!item_info || (item_info->flags & rf::IIF_NO_PICKUP)) {
        // Invalid/disabled item data should not keep an item-collection goal alive.
        return true;
    }

    const int gives_weapon = item_info->gives_weapon_id;
    const bool has_given_weapon =
        gives_weapon >= 0
        && gives_weapon < rf::num_weapon_types
        && local_entity.ai.has_weapon[gives_weapon];

    switch (goal_type) {
        case BotGoalType::collect_weapon:
            // Weapon collection is complete once we own the weapon, even if
            // the pickup remains visible in weapon-stay modes.
            return has_given_weapon;

        case BotGoalType::collect_ammo:
            if (item_info->ammo_for_weapon_id < 0) {
                return false;
            }
            if (item_info->ammo_for_weapon_id >= rf::num_weapon_types) {
                return true;
            }
            if (item_info->ammo_for_weapon_id < rf::num_weapon_types
                && !local_entity.ai.has_weapon[item_info->ammo_for_weapon_id]) {
                return false;
            }
            return get_weapon_ammo_fill_ratio(local_entity, item_info->ammo_for_weapon_id) >= 0.96f;

        case BotGoalType::collect_health:
            return item_info_is_health_pickup(*item_info)
                && get_entity_health_ratio(local_entity) >= 0.98f;

        case BotGoalType::collect_armor:
            return item_info_is_armor_pickup(*item_info)
                && get_entity_armor_ratio(local_entity) >= 0.98f;

        case BotGoalType::collect_super_item:
            if (has_given_weapon) {
                return true;
            }
            if (item_info_is_health_pickup(*item_info)
                && get_entity_health_ratio(local_entity) >= 0.98f) {
                return true;
            }
            if (item_info_is_armor_pickup(*item_info)
                && get_entity_armor_ratio(local_entity) >= 0.98f) {
                return true;
            }
            return false;

        default:
            return false;
    }
}

struct ItemGoalWeaponContext
{
    int preferred_weapon_type = -1;
    float preferred_weapon_score = 0.0f;
    float preferred_weapon_ammo_fill = 1.0f;
    bool has_satisfactory_weapon = false;
    bool needs_preferred_weapon_ammo = false;
};

ItemGoalWeaponContext build_item_goal_weapon_context(
    const rf::Entity& local_entity,
    const rf::Entity* enemy_target)
{
    ItemGoalWeaponContext context{};
    const BotPersonality& personality = get_active_bot_personality();
    for (int weapon_type = 0; weapon_type < rf::num_weapon_types; ++weapon_type) {
        const float score = compute_weapon_readiness_score_for_type(
            local_entity,
            enemy_target,
            weapon_type
        );
        if (score <= context.preferred_weapon_score) {
            continue;
        }
        context.preferred_weapon_score = score;
        context.preferred_weapon_type = weapon_type;
    }

    const float satisfactory_weapon_threshold = std::clamp(
        personality.satisfactory_weapon_threshold,
        0.20f,
        0.98f
    );
    context.has_satisfactory_weapon =
        context.preferred_weapon_type >= 0
        && context.preferred_weapon_score >= satisfactory_weapon_threshold;

    if (context.preferred_weapon_type >= 0
        && !rf::weapon_is_melee(context.preferred_weapon_type)
        && !rf::weapon_is_detonator(context.preferred_weapon_type)) {
        context.preferred_weapon_ammo_fill = get_weapon_ammo_fill_ratio(
            local_entity,
            context.preferred_weapon_type
        );
    }

    const float ammo_fill_threshold = std::clamp(
        personality.preferred_weapon_ammo_fill_threshold,
        0.05f,
        0.95f
    );
    context.needs_preferred_weapon_ammo =
        context.has_satisfactory_weapon
        && context.preferred_weapon_type >= 0
        && !rf::weapon_is_melee(context.preferred_weapon_type)
        && !rf::weapon_is_detonator(context.preferred_weapon_type)
        && context.preferred_weapon_ammo_fill < ammo_fill_threshold;
    return context;
}

float compute_collectible_item_goal_value(
    const rf::Entity& local_entity,
    const rf::Entity* enemy_target,
    const rf::Item& item,
    const ItemGoalWeaponContext& weapon_context)
{
    const rf::ItemInfo* item_info = resolve_item_info(item);
    if (!item_info || (item_info->flags & rf::IIF_NO_PICKUP)) {
        return 0.0f;
    }

    const BotPersonality& personality = get_active_bot_personality();
    const float replenish_bias = std::clamp(personality.replenish_bias, 0.25f, 2.5f);
    const bool super_item_hoarder =
        bot_personality_has_quirk(BotPersonalityQuirk::super_item_hoarder);
    const bool is_super_pickup = item_matches_super_pickup(item, item_info);
    const bool is_super_item_hoarder_target =
        item_matches_super_item_hoarder_target(item, item_info);
    const bool is_weapon_pickup = item_info->gives_weapon_id >= 0;
    const bool is_ammo_pickup = item_info->ammo_for_weapon_id >= 0;

    if (weapon_context.has_satisfactory_weapon
        && !is_super_pickup
        && !is_super_item_hoarder_target) {
        if (is_weapon_pickup) {
            const int weapon_type = item_info->gives_weapon_id;
            const bool supports_preferred_ammo_need =
                weapon_context.needs_preferred_weapon_ammo
                && weapon_type == weapon_context.preferred_weapon_type;
            if (!supports_preferred_ammo_need) {
                return 0.0f;
            }
        }
        if (is_ammo_pickup) {
            const int weapon_type = item_info->ammo_for_weapon_id;
            const bool supports_preferred_ammo_need =
                weapon_context.needs_preferred_weapon_ammo
                && weapon_type == weapon_context.preferred_weapon_type;
            if (!supports_preferred_ammo_need) {
                return 0.0f;
            }
        }
    }

    const float health_ratio = get_entity_health_ratio(local_entity);
    const float armor_ratio = get_entity_armor_ratio(local_entity);
    const bool is_health_pickup = item_info_is_health_pickup(*item_info);
    const bool is_armor_pickup = item_info_is_armor_pickup(*item_info);
    if (!is_super_pickup) {
        if (is_health_pickup && health_ratio >= 1.0f) {
            return 0.0f;
        }
        if (is_armor_pickup && armor_ratio >= 1.0f) {
            return 0.0f;
        }
    }
    const int item_type = resolve_item_type_index(item);

    float value = is_super_pickup ? 32.0f : 0.0f;
    if (item_type >= 0) {
        value += bot_get_item_weight(item_type) * 14.0f;
    }
    else {
        value += 8.0f;
    }

    if (is_weapon_pickup) {
        const int weapon_type = item_info->gives_weapon_id;
        const bool has_weapon =
            weapon_type >= 0
            && weapon_type < rf::num_weapon_types
            && local_entity.ai.has_weapon[weapon_type];
        const float pickup_weight = bot_get_weapon_pickup_weight(weapon_type);
        const float preference_weight = bot_get_weapon_preference_weight(weapon_type);
        float contextual_range_bonus = 0.0f;
        if (const BotWeaponProfile* profile = bot_weapon_profile_for_weapon_type(weapon_type)) {
            float desired_distance = std::max(
                1.0f,
                0.5f * (
                    personality.preferred_engagement_near
                    + personality.preferred_engagement_far
                )
            );
            if (enemy_target) {
                desired_distance = std::sqrt(std::max(
                    rf::vec_dist_squared(&local_entity.pos, &enemy_target->pos),
                    0.0f
                ));
            }
            const BotWeaponRangeBand desired_band = classify_combat_distance_band(desired_distance);
            contextual_range_bonus = bot_weapon_profile_supports_range(*profile, desired_band)
                ? 8.0f
                : -4.0f;
        }

        if (has_weapon) {
            const float ammo_fill = get_weapon_ammo_fill_ratio(local_entity, weapon_type);
            const float ammo_need = std::clamp(1.0f - ammo_fill, 0.0f, 1.0f);
            const bool should_seek_repeat_weapon_pickup =
                is_super_item_hoarder_target || ammo_need > 0.35f;
            if (!should_seek_repeat_weapon_pickup) {
                return 0.0f;
            }

            value += std::lerp(2.5f, 11.0f, ammo_need)
                * pickup_weight
                * std::lerp(0.75f, 1.55f, std::clamp(preference_weight / 2.5f, 0.0f, 1.0f))
                + contextual_range_bonus;
        }
        else {
            value += 22.0f * pickup_weight * preference_weight + contextual_range_bonus;
        }
    }

    if (is_ammo_pickup) {
        const int weapon_type = item_info->ammo_for_weapon_id;
        const bool has_weapon =
            weapon_type >= 0
            && weapon_type < rf::num_weapon_types
            && local_entity.ai.has_weapon[weapon_type];
        if (!has_weapon && !is_super_item_hoarder_target) {
            return 0.0f;
        }
        const float pickup_weight = bot_get_weapon_pickup_weight(weapon_type);
        const float preference_weight = bot_get_weapon_preference_weight(weapon_type);
        if (has_weapon) {
            const float ammo_fill = get_weapon_ammo_fill_ratio(local_entity, weapon_type);
            const float ammo_need = std::clamp(1.0f - ammo_fill, 0.0f, 1.0f);
            value += std::lerp(2.0f, 24.0f, ammo_need)
                * pickup_weight
                * std::lerp(0.75f, 1.60f, std::clamp(preference_weight / 2.5f, 0.0f, 1.0f));
        }
        else {
            value += 2.0f * pickup_weight;
        }
    }

    if (super_item_hoarder && is_super_item_hoarder_target) {
        value += is_super_pickup ? 165.0f : 115.0f;
    }

    if (is_health_pickup) {
        const float need = std::clamp(1.0f - health_ratio, 0.0f, 1.0f);
        value += need * 36.0f * replenish_bias;
    }

    if (is_armor_pickup) {
        const float need = std::clamp(1.0f - armor_ratio, 0.0f, 1.0f);
        value += need * 30.0f * replenish_bias;
    }

    const int count = std::max(item_info->count_multi, item_info->count);
    value += std::clamp(static_cast<float>(count) * 0.08f, 0.0f, 2.0f);
    const float respawn_seconds =
        std::max(static_cast<float>(item_info->respawn_time_millis), 0.0f) / 1000.0f;
    value += std::clamp(respawn_seconds * 0.04f, 0.0f, 1.5f);
    return value;
}

bool is_collectible_goal_item(const rf::Item& item)
{
    if (!item_is_currently_available(item)) {
        return false;
    }

    const rf::ItemInfo* item_info = resolve_item_info(item);
    if (!item_info || (item_info->flags & rf::IIF_NO_PICKUP)) {
        return false;
    }

    const int item_type = resolve_item_type_index(item);
    if (item_type >= 0 && bot_get_item_weight(item_type) <= 0.05f) {
        return false;
    }

    return true;
}

bool find_best_item_goal(
    const rf::Entity& local_entity,
    const rf::Entity* enemy_target,
    ItemGoalCandidate& out_candidate)
{
    out_candidate = {};
    const BotPersonality& personality = get_active_bot_personality();
    const ItemGoalWeaponContext weapon_context = build_item_goal_weapon_context(
        local_entity,
        enemy_target
    );
    const int start_waypoint = bot_find_closest_waypoint_with_los_fallback(local_entity);
    if (start_waypoint <= 0) {
        return false;
    }

    bot_goal_memory_prune_failed_item_goal_cooldowns();

    std::vector<int> route;
    std::vector<int> avoidset;
    rf::Object* obj = rf::object_list.next_obj;
    while (obj != &rf::object_list) {
        if (obj->type == rf::OT_ITEM) {
            const rf::Item* item = static_cast<const rf::Item*>(obj);
            if (item
                && bot_goal_memory_is_failed_item_goal_cooldown_active_no_prune(item->handle)) {
                obj = obj->next_obj;
                continue;
            }
            if (item && is_collectible_goal_item(*item)) {
                const float item_value = compute_collectible_item_goal_value(
                    local_entity,
                    enemy_target,
                    *item,
                    weapon_context
                );
                if (item_value <= 0.0f) {
                    obj = obj->next_obj;
                    continue;
                }
                const rf::ItemInfo* item_info = resolve_item_info(*item);
                if (!item_info) {
                    obj = obj->next_obj;
                    continue;
                }
                const BotGoalType item_goal_type = classify_collect_goal_for_item(*item, *item_info);
                if (item_goal_type == BotGoalType::none) {
                    obj = obj->next_obj;
                    continue;
                }
                int goal_waypoint = 0;
                if (!find_item_goal_waypoint_internal(item->pos, goal_waypoint)) {
                    obj = obj->next_obj;
                    continue;
                }
                if (goal_waypoint > 0
                    && goal_waypoint != start_waypoint
                    && !bot_waypoint_same_component(start_waypoint, goal_waypoint)) {
                    obj = obj->next_obj;
                    continue;
                }
                if (goal_waypoint > 0) {
                    route.clear();
                    if (start_waypoint == goal_waypoint) {
                        route.push_back(start_waypoint);
                        route.push_back(goal_waypoint);
                    }
                    else if (!bot_waypoint_route(start_waypoint, goal_waypoint, avoidset, route)) {
                        obj = obj->next_obj;
                        continue;
                    }

                    if (route.size() < 2 || path_contains_failed_edge_cooldown(route)) {
                        obj = obj->next_obj;
                        continue;
                    }

                    float route_score = compute_route_score(route)
                        + item_value
                        + kGoalItemBaseBonus * personality.opportunism_bias;
                    route_score -=
                        bot_goal_memory_get_recent_item_goal_penalty(item->handle, item_goal_type);
                    route_score -=
                        bot_goal_memory_get_secondary_goal_repeat_penalty(item_goal_type);
                    if (enemy_target) {
                        const float enemy_dist = std::sqrt(
                            std::max(
                                rf::vec_dist_squared(&enemy_target->pos, &local_entity.pos),
                                0.0f
                            )
                        );
                        const float pickup_dist = std::sqrt(
                            std::max(
                                rf::vec_dist_squared(&item->pos, &local_entity.pos),
                                0.0f
                            )
                        );
                        route_score += std::clamp(
                            (enemy_dist - pickup_dist) * 0.35f,
                            -18.0f,
                            20.0f
                        );
                    }

                    if (route_score > out_candidate.score) {
                        out_candidate.item_handle = item->handle;
                        out_candidate.item_uid = item->uid;
                        out_candidate.goal_type = item_goal_type;
                        out_candidate.goal_waypoint = goal_waypoint;
                        out_candidate.item_pos = item->pos;
                        out_candidate.score = route_score;
                    }
                }
            }
        }
        obj = obj->next_obj;
    }

    return out_candidate.item_handle >= 0;
}

float compute_enemy_goal_score(const rf::Entity& local_entity, const rf::Entity& enemy_target, const bool enemy_has_los)
{
    return bot_decision_compute_enemy_goal_score(local_entity, enemy_target, enemy_has_los);
}

void record_waypoint_visit(const int waypoint)
{
    bot_nav_record_waypoint_visit(waypoint);
}

bool detect_recent_waypoint_ping_pong_loop(int& out_waypoint_a, int& out_waypoint_b)
{
    return bot_nav_detect_recent_waypoint_ping_pong_loop(
        out_waypoint_a,
        out_waypoint_b
    );
}

bool pick_waypoint_route_to_goal_randomized_after_stuck(
    const int start_waypoint,
    const int goal_waypoint,
    const int avoid_waypoint,
    const int repath_ms)
{
    return bot_nav_pick_waypoint_route_to_goal_randomized_after_stuck(
        start_waypoint,
        goal_waypoint,
        avoid_waypoint,
        repath_ms
    );
}

bool pick_waypoint_route_to_goal_long_detour(
    const int start_waypoint,
    const int goal_waypoint,
    const int repath_ms)
{
    return bot_nav_pick_waypoint_route_to_goal_long_detour(
        start_waypoint,
        goal_waypoint,
        repath_ms
    );
}

bool is_client_bot_active()
{
    return client_bot_launch_enabled()
        && rf::is_multi
        && !rf::is_dedicated_server
        && rf::gameseq_get_state() == rf::GS_GAMEPLAY;
}

const char* waypoint_type_to_string(const WaypointType type)
{
    switch (type) {
        case WaypointType::std:
            return "std";
        case WaypointType::std_new:
            return "std_new";
        case WaypointType::item:
            return "item";
        case WaypointType::respawn:
            return "respawn";
        case WaypointType::jump_pad:
            return "jump_pad";
        case WaypointType::lift_body:
            return "lift_body";
        case WaypointType::lift_entrance:
            return "lift_entrance";
        case WaypointType::lift_exit:
            return "lift_exit";
        case WaypointType::ladder:
            return "ladder";
        case WaypointType::ctf_flag:
            return "ctf_flag";
        case WaypointType::crater:
            return "crater";
        case WaypointType::tele_entrance:
            return "tele_entrance";
        case WaypointType::tele_exit:
            return "tele_exit";
        default:
            return "unknown";
    }
}

rf::Color waypoint_debug_color(const WaypointType type)
{
    switch (type) {
        case WaypointType::std:
            return {255, 255, 255, 150};
        case WaypointType::std_new:
            return {255, 255, 255, 75};
        case WaypointType::item:
            return {255, 220, 0, 150};
        case WaypointType::respawn:
            return {0, 220, 255, 150};
        case WaypointType::jump_pad:
            return {0, 255, 120, 150};
        case WaypointType::lift_body:
            return {110, 150, 255, 150};
        case WaypointType::lift_entrance:
            return {140, 180, 255, 150};
        case WaypointType::lift_exit:
            return {80, 120, 255, 150};
        case WaypointType::ladder:
            return {255, 170, 70, 150};
        case WaypointType::ctf_flag:
            return {255, 70, 70, 150};
        case WaypointType::crater:
            return {200, 70, 255, 150};
        case WaypointType::tele_entrance:
            return {255, 140, 60, 150};
        case WaypointType::tele_exit:
            return {255, 80, 220, 150};
        default:
            return {200, 200, 200, 150};
    }
}

float waypoint_debug_sphere_scale(const WaypointType type)
{
    if (type == WaypointType::std || type == WaypointType::std_new) {
        return 0.125f;
    }
    return 0.25f;
}

void draw_centered_debug_label(float sx, float sy, const char* text, const rf::Color& color)
{
    const auto [text_w, text_h] = rf::gr::get_string_size(text, -1);
    const int x = static_cast<int>(sx) - (text_w / 2);
    const int y = static_cast<int>(sy) - (text_h / 2);
    rf::gr::set_color(color.red, color.green, color.blue, 255);
    rf::gr::string(x, y, text, -1, no_overdraw_2d_text);
}

void update_bot_status_hud()
{
    if (g_client_bot_state.hud_status_timer.valid()
        && !g_client_bot_state.hud_status_timer.elapsed()) {
        return;
    }

    // Build a short description of the current goal's target.
    char target_label[40] = "-";
    if (g_client_bot_state.active_goal == BotGoalType::eliminate_target) {
        if (g_client_bot_state.goal_target_handle >= 0) {
            rf::Player* target_player =
                rf::player_from_entity_handle(g_client_bot_state.goal_target_handle);
            if (target_player && !target_player->name.empty()) {
                std::snprintf(target_label, sizeof(target_label), "%.22s", target_player->name.c_str());
            }
            else {
                std::snprintf(target_label, sizeof(target_label), "uid%d", g_client_bot_state.goal_target_identifier);
            }
        }
        else {
            std::strncpy(target_label, "hunt", sizeof(target_label) - 1);
        }
    }
    else if (g_client_bot_state.active_goal != BotGoalType::roam
        && g_client_bot_state.active_goal != BotGoalType::none) {
        std::snprintf(target_label, sizeof(target_label), "id%d", g_client_bot_state.goal_target_identifier);
    }

    // Format the top-3 candidates from the most recent goal evaluation pass.
    // These reflect raw scores before hysteresis; top3[0] may differ from active_goal
    // when commitment/switch-lock held the previous goal.
    char eval1[48] = "none", eval2[48] = "none", eval3[48] = "none";
    const auto fmt_eval_entry = [](const BotTopGoalEntry& e, char* buf, const int len) {
        if (e.goal != BotGoalType::none && std::isfinite(e.score)) {
            std::snprintf(buf, len, "%s(%.0f)", bot_goal_type_to_string(e.goal), e.score);
        }
    };
    fmt_eval_entry(g_client_bot_state.last_eval_top3[0], eval1, sizeof(eval1));
    fmt_eval_entry(g_client_bot_state.last_eval_top3[1], eval2, sizeof(eval2));
    fmt_eval_entry(g_client_bot_state.last_eval_top3[2], eval3, sizeof(eval3));

    char status_text[320]{};
    std::snprintf(
        status_text,
        sizeof(status_text),
        "FSM:%s Goal:%s->%s | Eval: %s, %s, %s",
        bot_fsm_state_to_string(g_client_bot_state.fsm_state),
        bot_goal_type_to_string(g_client_bot_state.active_goal),
        target_label,
        eval1,
        eval2,
        eval3
    );
    rf::hud_msg(status_text, 0, 650, nullptr);
    g_client_bot_state.hud_status_timer.set(500);
}

void update_bot_status_console(
    const rf::Entity& local_entity,
    const rf::Entity* enemy_target,
    const bool enemy_has_los,
    const bool pursuing_enemy_goal,
    const bool has_move_target,
    const rf::Vector3& move_target)
{
    if (g_client_bot_state.console_status_timer.valid()
        && !g_client_bot_state.console_status_timer.elapsed()) {
        return;
    }
    if (!g_alpine_game_config.dbg_bot) {
        return;
    }

    const float health_ratio = get_entity_health_ratio(local_entity);
    const float armor_ratio = get_entity_armor_ratio(local_entity);
    const float maintenance_bias = std::clamp(
        get_active_bot_skill_profile().survivability_maintenance_bias,
        0.25f,
        2.5f
    );
    const float maintenance_norm = std::clamp(
        (maintenance_bias - 0.25f) / 2.25f,
        0.0f,
        1.0f
    );
    const float health_need = std::clamp(1.0f - health_ratio, 0.0f, 1.0f);
    const float armor_need = std::clamp(1.0f - armor_ratio, 0.0f, 1.0f);
    const float survivability_need = std::clamp(
        std::max(health_need, armor_need * 0.95f),
        0.0f,
        1.0f
    );
    float maintenance_pressure = survivability_need * maintenance_norm;
    if (health_ratio > 0.75f && armor_ratio < 0.60f) {
        maintenance_pressure += 0.18f * maintenance_norm;
    }
    maintenance_pressure = std::clamp(maintenance_pressure, 0.0f, 1.0f);

    const rf::Entity* readiness_enemy = nullptr;
    if (g_client_bot_state.active_goal == BotGoalType::eliminate_target
        && g_client_bot_state.goal_target_handle >= 0) {
        readiness_enemy = rf::entity_from_handle(g_client_bot_state.goal_target_handle);
    }
    const float combat_readiness = bot_personality_manager_compute_combat_readiness(
        local_entity,
        readiness_enemy
    );
    const float combat_readiness_threshold =
        bot_personality_manager_compute_combat_readiness_threshold();

    const int route_len = static_cast<int>(g_client_bot_state.waypoint_path.size());
    const int next_idx = g_client_bot_state.waypoint_next_index;
    const int current_wp = (next_idx > 0 && next_idx - 1 < route_len)
        ? g_client_bot_state.waypoint_path[next_idx - 1]
        : 0;
    const int next_wp = (next_idx >= 0 && next_idx < route_len)
        ? g_client_bot_state.waypoint_path[next_idx]
        : 0;
    const int end_wp = route_len > 0 ? g_client_bot_state.waypoint_path.back() : 0;
    const int enemy_handle = enemy_target ? enemy_target->handle : -1;

    xlog::warn(
        "BotBehavior FSM={} Goal={} GoalID={} GoalWP={} GoalHandle={} EnemyHandle={} EnemyLOS={} Pursuit={} MoveTarget={} RouteLen={} NextIdx={}",
        bot_fsm_state_to_string(g_client_bot_state.fsm_state),
        bot_goal_type_to_string(g_client_bot_state.active_goal),
        g_client_bot_state.goal_target_identifier,
        g_client_bot_state.goal_target_waypoint,
        g_client_bot_state.goal_target_handle,
        enemy_handle,
        enemy_has_los,
        pursuing_enemy_goal,
        has_move_target,
        route_len,
        next_idx
    );

    xlog::warn(
        "BotConsider HP={:.1f} AP={:.1f} HR={:.2f} AR={:.2f} Maint={:.2f} Ready={:.2f}/{:.2f} Replenish={} DM={} CTF={} CP={} ThreatHandle={} ThreatVisible={}",
        local_entity.life,
        local_entity.armor,
        health_ratio,
        armor_ratio,
        maintenance_pressure,
        combat_readiness,
        combat_readiness_threshold,
        bot_personality_manager_should_replenish_state(local_entity),
        bot_personality_manager_is_deathmatch_mode(),
        bot_personality_manager_is_ctf_mode(),
        bot_personality_manager_is_control_point_mode(),
        g_client_bot_state.ctf_threat_handle,
        g_client_bot_state.ctf_threat_visible
    );

    xlog::warn(
        "BotEval CurrentWP={} NextWP={} EndWP={} WaypointGoal={} HasWaypointTarget={} RecoveryPending={} AvoidWP={} AnchorWP={} RepathTimer={} MoveTargetPos=({:.2f},{:.2f},{:.2f})",
        current_wp,
        next_wp,
        end_wp,
        g_client_bot_state.waypoint_goal,
        g_client_bot_state.has_waypoint_target,
        g_client_bot_state.recovery_pending_reroute,
        g_client_bot_state.recovery_avoid_waypoint,
        g_client_bot_state.recovery_anchor_waypoint,
        g_client_bot_state.repath_timer.valid(),
        move_target.x,
        move_target.y,
        move_target.z
    );

    g_client_bot_state.console_status_timer.set(5000);
}

void draw_bot_route_debug()
{
    if (!is_client_bot_active() || !rf::gameseq_in_gameplay()) {
        return;
    }
    if (g_client_bot_state.waypoint_path.empty()) {
        return;
    }

    const auto& path = g_client_bot_state.waypoint_path;
    const int path_size = static_cast<int>(path.size());
    if (path_size <= 0) {
        return;
    }

    const int draw_start = std::clamp(g_client_bot_state.waypoint_next_index - 1, 0, path_size - 1);
    rf::Vector3 prev_pos{};
    bool have_prev = false;
    for (int index = draw_start; index < path_size; ++index) {
        const int waypoint_uid = path[index];
        rf::Vector3 waypoint_pos{};
        int type_raw = 0;
        int subtype = 0;
        if (!waypoints_get_pos(waypoint_uid, waypoint_pos)
            || !waypoints_get_type_subtype(waypoint_uid, type_raw, subtype)) {
            continue;
        }

        (void)subtype;
        const auto waypoint_type = static_cast<WaypointType>(type_raw);
        const rf::Color waypoint_color = waypoint_debug_color(waypoint_type);
        rf::gr::set_color(waypoint_color);

        float link_radius = kWaypointLinkRadius;
        (void)waypoints_get_link_radius(waypoint_uid, link_radius);
        const float sphere_radius = std::max(
            0.05f,
            link_radius * waypoint_debug_sphere_scale(waypoint_type)
        );
        rf::gr::sphere(waypoint_pos, sphere_radius, no_overdraw_2d_line);

        if (have_prev) {
            rf::gr::line_arrow(
                prev_pos.x,
                prev_pos.y,
                prev_pos.z,
                waypoint_pos.x,
                waypoint_pos.y,
                waypoint_pos.z,
                0,
                255,
                0
            );
        }

        prev_pos = waypoint_pos;
        have_prev = true;
    }

    int destination_waypoint = g_client_bot_state.waypoint_goal;
    if (destination_waypoint <= 0 && path_size > 0) {
        destination_waypoint = path.back();
    }

    rf::Vector3 destination_pos{};
    int destination_type_raw = 0;
    int destination_subtype = 0;
    if (destination_waypoint <= 0
        || !waypoints_get_pos(destination_waypoint, destination_pos)
        || !waypoints_get_type_subtype(destination_waypoint, destination_type_raw, destination_subtype)) {
        return;
    }

    int destination_identifier = -1;
    (void)waypoints_get_identifier(destination_waypoint, destination_identifier);

    const auto destination_type = static_cast<WaypointType>(destination_type_raw);
    char label[96]{};
    if (destination_identifier >= 0) {
        std::snprintf(
            label,
            sizeof(label),
            "%s (%d : %d : %d)",
            waypoint_type_to_string(destination_type),
            destination_waypoint,
            destination_subtype,
            destination_identifier
        );
    }
    else {
        std::snprintf(
            label,
            sizeof(label),
            "%s (%d : %d)",
            waypoint_type_to_string(destination_type),
            destination_waypoint,
            destination_subtype
        );
    }

    rf::Vector3 label_pos = destination_pos;
    label_pos.y += 0.3f;
    rf::gr::Vertex dest{};
    if (!rf::gr::rotate_vertex(&dest, &label_pos)) {
        rf::gr::project_vertex(&dest);
        if (dest.flags & 1) {
            draw_centered_debug_label(dest.sx, dest.sy, label, rf::Color{255, 255, 255, 255});
        }
    }
}

FunHook<void(rf::Player*, rf::ControlConfig*, rf::ControlInfo*, int, int)> controls_read_internal2_hook{
    0x004307A0,
    [](rf::Player* pp, rf::ControlConfig* ccp, rf::ControlInfo* cip, int a4, int a5) {
        (void)ccp;
        (void)a4;
        (void)a5;
        controls_read_internal2_hook.call_target(pp, ccp, cip, a4, a5);
        if (!pp
            || !cip
            || pp != rf::local_player
            || !is_client_bot_active()
            || !g_client_bot_state.movement_override_active) {
            return;
        }

        cip->move.x = g_client_bot_state.move_input_x;
        cip->move.y = g_client_bot_state.move_input_y;
        cip->move.z = g_client_bot_state.move_input_z;
    },
};

FunHook<bool(rf::ControlConfig*, rf::ControlConfigAction, bool*)> control_config_check_pressed_hook{
    0x0043D4F0,
    [](rf::ControlConfig* ccp, rf::ControlConfigAction action, bool* just_pressed_out) {
        bool base_just_pressed = false;
        bool* base_just_pressed_out = just_pressed_out ? &base_just_pressed : nullptr;
        const bool base_pressed = control_config_check_pressed_hook.call_target(
            ccp,
            action,
            base_just_pressed_out
        );
        if (just_pressed_out) {
            *just_pressed_out = base_just_pressed;
        }

        if (!is_client_bot_active()
            || !rf::gameseq_in_gameplay()
            || !rf::local_player
            || ccp != &rf::local_player->settings.controls) {
            return base_pressed;
        }

        bool synthetic_pressed = false;
        bool synthetic_just_pressed = false;
        if (action == rf::CC_ACTION_PRIMARY_ATTACK) {
            synthetic_pressed = g_client_bot_state.firing.synthetic_primary_fire_down;
            synthetic_just_pressed = g_client_bot_state.firing.synthetic_primary_fire_just_pressed;
        }
        else if (action == rf::CC_ACTION_SECONDARY_ATTACK) {
            synthetic_pressed = g_client_bot_state.firing.synthetic_secondary_fire_down;
            synthetic_just_pressed = g_client_bot_state.firing.synthetic_secondary_fire_just_pressed;
        }
        else {
            return base_pressed;
        }

        if (!synthetic_pressed) {
            return base_pressed;
        }

        if (just_pressed_out) {
            *just_pressed_out = *just_pressed_out || synthetic_just_pressed;
        }
        return true;
    },
};

void ensure_bot_input_hook_installed()
{
    if (!client_bot_launch_enabled()) {
        return;
    }

    if (!g_bot_input_hook_installed) {
        controls_read_internal2_hook.install();
        control_config_check_pressed_hook.install();
        g_bot_input_hook_installed = true;
    }
}
}

void bot_internal_clear_waypoint_route()
{
    clear_waypoint_route();
}

bool bot_internal_start_recovery_anchor_reroute(
    const rf::Entity& entity,
    const int avoid_waypoint)
{
    return start_recovery_anchor_reroute(entity, avoid_waypoint);
}

bool bot_internal_update_waypoint_target(const rf::Entity& entity)
{
    return update_waypoint_target(entity);
}

bool bot_internal_update_waypoint_target_towards(
    const rf::Entity& entity,
    const rf::Vector3& destination,
    const rf::Vector3* los_target_eye_pos,
    const rf::Object* los_target_obj,
    const int repath_ms)
{
    return update_waypoint_target_towards(
        entity,
        destination,
        los_target_eye_pos,
        los_target_obj,
        repath_ms
    );
}

bool bot_internal_update_waypoint_target_for_local_los_reposition(
    const rf::Entity& entity,
    const rf::Entity& enemy_target,
    const bool enemy_has_los)
{
    return update_waypoint_target_for_local_los_reposition(
        entity,
        enemy_target,
        enemy_has_los
    );
}

bool bot_internal_try_recover_from_corner_probe(
    const rf::Entity& entity,
    const rf::Vector3& move_target)
{
    return try_recover_from_corner_probe(entity, move_target);
}

bool bot_internal_is_following_waypoint_link(const rf::Entity& entity)
{
    return is_following_waypoint_link(entity);
}

bool bot_internal_is_collectible_goal_item(const rf::Item& item)
{
    return is_collectible_goal_item(item);
}

BotGoalType bot_internal_classify_collect_goal_item(const rf::Item& item)
{
    const rf::ItemInfo* item_info = resolve_item_info(item);
    if (!item_info || (item_info->flags & rf::IIF_NO_PICKUP)) {
        return BotGoalType::none;
    }
    return classify_collect_goal_for_item(item, *item_info);
}

bool bot_internal_is_item_goal_satisfied(
    const rf::Entity& local_entity,
    const BotGoalType goal_type,
    const rf::Item& item)
{
    return is_item_goal_satisfied(local_entity, goal_type, item);
}

bool bot_internal_find_best_item_goal(
    const rf::Entity& local_entity,
    const rf::Entity* enemy_target,
    ItemGoalCandidate& out_candidate)
{
    return find_best_item_goal(local_entity, enemy_target, out_candidate);
}

bool bot_internal_find_item_goal_waypoint(const rf::Vector3& item_pos, int& out_waypoint)
{
    return find_item_goal_waypoint_internal(item_pos, out_waypoint);
}

void bot_internal_note_item_goal_selection(const int item_handle, const BotGoalType goal_type)
{
    if (!bot_goal_is_item_collection(goal_type) || item_handle < 0) {
        return;
    }
    bot_goal_memory_note_item_goal_selection(item_handle, goal_type);
}

float bot_internal_compute_enemy_goal_score(
    const rf::Entity& local_entity,
    const rf::Entity& enemy_target,
    const bool enemy_has_los)
{
    return compute_enemy_goal_score(local_entity, enemy_target, enemy_has_los);
}

float bot_internal_compute_combat_readiness(
    const rf::Entity& local_entity,
    const rf::Entity* enemy_target)
{
    return bot_personality_manager_compute_combat_readiness(local_entity, enemy_target);
}

float bot_internal_get_combat_readiness_threshold()
{
    return bot_personality_manager_compute_combat_readiness_threshold();
}

bool bot_internal_is_deathmatch_mode()
{
    return bot_personality_manager_is_deathmatch_mode();
}

bool bot_internal_is_control_point_mode()
{
    return bot_personality_manager_is_control_point_mode();
}

void client_bot_render_debug()
{
    draw_bot_route_debug();
}

void client_bot_do_frame()
{
    ensure_bot_input_hook_installed();
    clear_synthetic_movement_controls();
    g_bot_info.can_spawn = false;
    g_client_bot_state.goal_refreshed_this_frame = false;
    g_client_bot_state.weapon_readiness_cached = false;

    // Bot clients always run with toggle crouch enabled.
    if (client_bot_launch_enabled()
        && rf::local_player
        && !rf::local_player->settings.toggle_crouch) {
        rf::local_player->settings.toggle_crouch = true;
    }

    // Bot clients always run with weapon-specific control swapping disabled.
    if (client_bot_launch_enabled()) {
        g_alpine_game_config.swap_ar_controls = false;
        g_alpine_game_config.swap_gn_controls = false;
        g_alpine_game_config.swap_sg_controls = false;
    }

    // Bot config timeout - runs in all game states.
    // Catches cases where the bot never reaches GS_GAMEPLAY (connection timeout, server not found, etc.).
    // Join denials are handled separately via process_join_deny_packet_hook for faster response.
    if (client_bot_launch_enabled() && !g_client_bot_state.server_config_received) {
        if (!g_client_bot_state.server_config_timeout_timer.valid()) {
            g_client_bot_state.server_config_timeout_timer.set(kBotServerConfigTimeoutMs);
            xlog::info("Bot waiting for server config...");
        }
        else if (g_client_bot_state.server_config_timeout_timer.elapsed()) {
            if (g_alpine_game_config.bot_quit_when_disconnected) {
                WARN_ONCE("Bot did not receive config from server within timeout - auto-quitting");
                rf::gameseq_set_state(rf::GS_QUITING, false);
            }
            else {
                xlog::warn("Bot did not receive config from server within timeout - disconnecting");
                multi_disconnect_from_server();
            }
            return;
        }
    }

    if (!is_client_bot_active()) {
        // Connection watchdog: if config was received but we haven't reached (or returned to)
        // GS_GAMEPLAY within the timeout, the connection is likely dead.
        if (g_client_bot_state.server_config_received
            && g_client_bot_state.connection_watchdog_timer.valid()
            && g_client_bot_state.connection_watchdog_timer.elapsed()) {
            if (g_alpine_game_config.bot_quit_when_disconnected) {
                xlog::warn("Bot connection watchdog expired (no gameplay for {}s after config) - auto-quitting",
                    kBotConnectionWatchdogMs / 1000);
                rf::gameseq_set_state(rf::GS_QUITING, false);
            }
            else {
                xlog::warn("Bot connection watchdog expired - disconnecting");
                multi_disconnect_from_server();
            }
            return;
        }

        reset_client_bot_state();
        bot_chat_manager_reset();
        reset_goal_stuck_watchdog(true);
        reset_no_move_target_watchdog();
        reset_position_stall_watchdog();
        reset_objective_progress_watchdog(true);
        reset_global_failsafe_watchdog(true);
        bot_state_reset_fsm(BotFsmState::inactive);
        g_client_bot_state.hud_status_timer.invalidate();
        g_client_bot_state.console_status_timer.invalidate();
        // Preserve server_config_received across non-gameplay states (limbo, loading).
        // It is only reset on actual disconnect via multi_stop_hook.
        bot_perception_manager_reset_tracking();
        return;
    }

    // In active gameplay - reset connection watchdog since we're connected and playing.
    g_client_bot_state.connection_watchdog_timer.set(kBotConnectionWatchdogMs);

    // If server config hasn't been received yet, don't run bot logic.
    // The timeout is handled above (before the is_client_bot_active check).
    if (!g_client_bot_state.server_config_received) {
        return;
    }

    // Server deactivated the bot (go_inactive) - idle until reactivated.
    if (g_client_bot_state.server_deactivated) {
        return;
    }

    bot_weapon_profiles_init_for_active_bot();

    rf::Player* const local_player = rf::local_player;
    if (!local_player) {
        clear_bot_crouch_jump_link_state();
        g_client_bot_state.firing.wants_fire = false;
        g_client_bot_state.firing.synthetic_primary_fire_down = false;
        g_client_bot_state.firing.synthetic_secondary_fire_down = false;
        g_client_bot_state.firing.synthetic_primary_fire_just_pressed = false;
        g_client_bot_state.firing.synthetic_secondary_fire_just_pressed = false;
        g_client_bot_state.firing.explosive_fire_delay_timer.invalidate();
        g_client_bot_state.firing.explosive_fire_delay_weapon = -1;
        g_client_bot_state.firing.explosive_fire_delay_alt = false;
        g_client_bot_state.firing.explosive_release_hold_timer.invalidate();
        g_client_bot_state.firing.explosive_release_hold_target = {};
        g_client_bot_state.firing.explosive_release_hold_weapon = -1;
        g_client_bot_state.firing.explosive_release_hold_alt = false;
        g_client_bot_state.firing.remote_charge_pending_detonation = false;
        g_client_bot_state.firing.remote_charge_pending_detonation_timer.invalidate();
        g_client_bot_state.crater_goal_timeout_timer.invalidate();
        g_client_bot_state.shatter_goal_timeout_timer.invalidate();
        g_client_bot_state.respawn_gearup_timer.invalidate();
        g_client_bot_state.respawn_uncrouch_retry_timer.invalidate();
        g_client_bot_state.retaliation_timer.invalidate();
        g_client_bot_state.retaliation_target_handle = -1;
        g_client_bot_state.ctf_threat_handle = -1;
        g_client_bot_state.ctf_threat_pos = {};
        g_client_bot_state.ctf_threat_visible = false;
        g_client_bot_state.control_point_route_fail_timer.invalidate();
        g_client_bot_state.control_point_patrol_waypoint = 0;
        g_client_bot_state.control_point_patrol_timer.invalidate();
        g_client_bot_state.last_recorded_health = -1.0f;
        g_client_bot_state.last_recorded_armor = -1.0f;
        clear_semi_auto_click_state();
        bot_state_reset_fsm(BotFsmState::inactive);
        g_client_bot_state.hud_status_timer.invalidate();
        g_client_bot_state.console_status_timer.invalidate();
        bot_perception_manager_reset_tracking();
        bot_memory_manager_reset();
        reset_goal_stuck_watchdog(true);
        reset_no_move_target_watchdog();
        reset_position_stall_watchdog();
        reset_objective_progress_watchdog(true);
        reset_global_failsafe_watchdog(true);
        reset_confusion_abort_tracking();
        return;
    }

    const bool is_dead = rf::player_is_dead(local_player);
    const bool is_dying = rf::player_is_dying(local_player);
    bot_chat_manager_update_frame(*local_player);
    update_bot_spawn_state(*local_player);

    rf::Entity* local_entity = rf::local_player_entity
        ? rf::local_player_entity
        : rf::entity_from_handle(local_player->entity_handle);

    if (g_bot_info.can_spawn || is_dead || is_dying || local_player->is_spectator) {
        clear_bot_crouch_jump_link_state();
        g_client_bot_state.firing.wants_fire = false;
        g_client_bot_state.firing.synthetic_primary_fire_down = false;
        g_client_bot_state.firing.synthetic_secondary_fire_down = false;
        g_client_bot_state.firing.synthetic_primary_fire_just_pressed = false;
        g_client_bot_state.firing.synthetic_secondary_fire_just_pressed = false;
        bot_state_clear_goal_and_eval();
        g_client_bot_state.respawn_gearup_timer.invalidate();
        g_client_bot_state.respawn_uncrouch_retry_timer.invalidate();
        g_client_bot_state.retaliation_timer.invalidate();
        g_client_bot_state.retaliation_target_handle = -1;
        g_client_bot_state.ctf_threat_handle = -1;
        g_client_bot_state.ctf_threat_pos = {};
        g_client_bot_state.ctf_threat_visible = false;
        g_client_bot_state.control_point_route_fail_timer.invalidate();
        g_client_bot_state.control_point_patrol_waypoint = 0;
        g_client_bot_state.control_point_patrol_timer.invalidate();
        g_client_bot_state.last_recorded_health = -1.0f;
        g_client_bot_state.last_recorded_armor = -1.0f;
        g_client_bot_state.bridge.zone_uid = -1;
        g_client_bot_state.bridge.trigger_uid = -1;
        g_client_bot_state.bridge.trigger_pos = {};
        g_client_bot_state.bridge.activation_radius = 0.0f;
        g_client_bot_state.bridge.requires_use = false;
        g_client_bot_state.bridge.use_press_timer.invalidate();
        g_client_bot_state.bridge.activation_abort_timer.invalidate();
        g_client_bot_state.bridge.activation_best_dist_sq = std::numeric_limits<float>::infinity();
        g_client_bot_state.bridge.post_open_zone_uid = -1;
        g_client_bot_state.bridge.post_open_target_waypoint = 0;
        g_client_bot_state.bridge.post_open_priority_timer.invalidate();
        g_client_bot_state.crater_goal_abort_timer.invalidate();
        g_client_bot_state.crater_goal_timeout_timer.invalidate();
        g_client_bot_state.shatter_goal_abort_timer.invalidate();
        g_client_bot_state.shatter_goal_timeout_timer.invalidate();
        g_client_bot_state.firing.explosive_fire_delay_timer.invalidate();
        g_client_bot_state.firing.explosive_fire_delay_weapon = -1;
        g_client_bot_state.firing.explosive_fire_delay_alt = false;
        g_client_bot_state.firing.explosive_release_hold_timer.invalidate();
        g_client_bot_state.firing.explosive_release_hold_target = {};
        g_client_bot_state.firing.explosive_release_hold_weapon = -1;
        g_client_bot_state.firing.explosive_release_hold_alt = false;
        g_client_bot_state.firing.remote_charge_pending_detonation = false;
        g_client_bot_state.firing.remote_charge_pending_detonation_timer.invalidate();
        clear_enemy_aim_error_state();
        clear_semi_auto_click_state();

        if (local_entity && local_entity->ai.current_primary_weapon >= 0) {
            rf::entity_turn_weapon_off(
                local_entity->handle,
                local_entity->ai.current_primary_weapon
            );
        }

        if (g_bot_info.can_spawn
            && (!g_client_bot_state.respawn_retry_timer.valid()
                || g_client_bot_state.respawn_retry_timer.elapsed())) {
            rf::player_execute_action(local_player, rf::CC_ACTION_SECONDARY_ATTACK, true);
            g_client_bot_state.respawn_retry_timer.set(kBotRespawnRetryMs);
        }

        bot_state_reset_fsm(BotFsmState::inactive);
        g_client_bot_state.hud_status_timer.invalidate();
        g_client_bot_state.console_status_timer.invalidate();
        bot_perception_manager_reset_tracking();
        bot_memory_manager_reset();
        reset_goal_stuck_watchdog(true);
        reset_no_move_target_watchdog();
        reset_position_stall_watchdog();
        reset_objective_progress_watchdog(true);
        reset_global_failsafe_watchdog(true);
        reset_confusion_abort_tracking();

        return;
    }

    if (!local_entity) {
        clear_bot_crouch_jump_link_state();
        g_client_bot_state.firing.synthetic_primary_fire_down = false;
        g_client_bot_state.firing.synthetic_secondary_fire_down = false;
        g_client_bot_state.firing.synthetic_primary_fire_just_pressed = false;
        g_client_bot_state.firing.synthetic_secondary_fire_just_pressed = false;
        g_client_bot_state.firing.explosive_fire_delay_timer.invalidate();
        g_client_bot_state.firing.explosive_fire_delay_weapon = -1;
        g_client_bot_state.firing.explosive_fire_delay_alt = false;
        g_client_bot_state.firing.explosive_release_hold_timer.invalidate();
        g_client_bot_state.firing.explosive_release_hold_target = {};
        g_client_bot_state.firing.explosive_release_hold_weapon = -1;
        g_client_bot_state.firing.explosive_release_hold_alt = false;
        g_client_bot_state.firing.remote_charge_pending_detonation = false;
        g_client_bot_state.firing.remote_charge_pending_detonation_timer.invalidate();
        g_client_bot_state.crater_goal_timeout_timer.invalidate();
        g_client_bot_state.shatter_goal_timeout_timer.invalidate();
        g_client_bot_state.ctf_threat_handle = -1;
        g_client_bot_state.ctf_threat_pos = {};
        g_client_bot_state.ctf_threat_visible = false;
        g_client_bot_state.control_point_route_fail_timer.invalidate();
        g_client_bot_state.control_point_patrol_waypoint = 0;
        g_client_bot_state.control_point_patrol_timer.invalidate();
        g_client_bot_state.last_recorded_health = -1.0f;
        g_client_bot_state.last_recorded_armor = -1.0f;
        g_client_bot_state.console_status_timer.invalidate();
        bot_memory_manager_reset();
        reset_goal_stuck_watchdog(true);
        reset_no_move_target_watchdog();
        reset_position_stall_watchdog();
        reset_objective_progress_watchdog(true);
        reset_global_failsafe_watchdog(true);
        reset_confusion_abort_tracking();
        return;
    }

    if (!g_client_bot_state.respawn_gearup_timer.valid()) {
        g_client_bot_state.respawn_gearup_timer.set(kBotRespawnGearupDurationMs);
        force_uncrouch_after_respawn(*local_player, *local_entity);
    }
    maintain_uncrouched_after_respawn(*local_player, *local_entity);
    update_retaliation_context(*local_player, *local_entity);

    bot_combat_manager_prepare_frame(*local_player, *local_entity);

    rf::Entity* enemy_target = bot_perception_manager_select_enemy_target(*local_player, *local_entity);
    bool enemy_has_los = false;
    if (enemy_target) {
        enemy_has_los = bot_perception_manager_get_enemy_los_cached(*local_entity, *enemy_target);
    }
    else {
        // Keep pursuing a committed target handle when selector momentarily drops lock.
        if (g_client_bot_state.active_goal == BotGoalType::eliminate_target
            && g_client_bot_state.goal_target_handle >= 0) {
            rf::Entity* goal_enemy = rf::entity_from_handle(g_client_bot_state.goal_target_handle);
            if (goal_enemy
                && goal_enemy != local_entity
                && !rf::entity_is_dying(goal_enemy)) {
                enemy_target = goal_enemy;
                enemy_has_los = bot_perception_manager_get_enemy_los_cached(*local_entity, *enemy_target);
            }
        }

        if (!enemy_target) {
            bot_perception_manager_reset_tracking();
            clear_enemy_aim_error_state();
            clear_semi_auto_click_state();
        }
    }

    const bool any_combat_lock_active =
        enemy_target
        && enemy_has_los
        && (g_client_bot_state.firing.wants_fire
            || g_client_bot_state.firing.synthetic_primary_fire_down
            || g_client_bot_state.firing.synthetic_secondary_fire_down);
    if (any_combat_lock_active) {
        if (!g_client_bot_state.combat_lock_watchdog_timer.valid()) {
            g_client_bot_state.combat_lock_watchdog_timer.set(kCombatLockWatchdogOverrideMs);
        }
    }
    else {
        g_client_bot_state.combat_lock_watchdog_timer.invalidate();
    }
    const bool combat_lock_expired =
        g_client_bot_state.combat_lock_watchdog_timer.valid()
        && g_client_bot_state.combat_lock_watchdog_timer.elapsed();

    if (g_client_bot_state.active_goal == BotGoalType::none
        && g_client_bot_state.fsm_state == BotFsmState::idle) {
        reset_no_move_target_watchdog();
        reset_position_stall_watchdog();
        reset_objective_progress_watchdog(true);

        if (!g_client_bot_state.idle_escalation_timer.valid()) {
            g_client_bot_state.idle_escalation_timer.set(kIdleEscalationMs);
        }
        else if (g_client_bot_state.idle_escalation_timer.elapsed()) {
            g_client_bot_state.idle_escalation_timer.set(kIdleEscalationMs);
            g_client_bot_state.goal_eval_timer.invalidate();
            g_client_bot_state.goal_switch_lock_timer.invalidate();
            bot_state_clear_waypoint_route(true, true, false);
            if (g_alpine_game_config.dbg_bot) {
                xlog::warn("Bot idle escalation: forcing goal re-evaluation");
            }
        }
    }
    else {
        g_client_bot_state.idle_escalation_timer.invalidate();
    }

    rf::Vector3 bot_aim_dir = local_entity->eye_orient.fvec;
    if (bot_aim_dir.len_sq() < 0.0001f) {
        bot_aim_dir = local_entity->orient.fvec;
    }
    bot_aim_dir.normalize_safe();

    if (should_abort_for_confusion(*local_entity, bot_aim_dir)) {
        abort_goal_and_fsm_for_confusion(*local_entity);
    }

    bot_goal_manager_refresh_and_ensure(*local_entity, enemy_target, enemy_has_los, 180);

    bool pursuing_enemy_goal = bot_navigation_manager_sync_pursuit_target(enemy_target);
    bot_navigation_manager_update_pursuit_recovery_timer();

    // Set high-priority FSMs before move-target planning to avoid one-frame lag.
    const BotFsmState pre_move_state = bot_fsm_manager_select_state(
        *local_player,
        *local_entity,
        enemy_target,
        pursuing_enemy_goal,
        enemy_has_los,
        false
    );
    bot_fsm_manager_transition_state(pre_move_state);

    update_blind_escape_state(
        *local_entity,
        pursuing_enemy_goal ? enemy_target : nullptr,
        pursuing_enemy_goal && enemy_has_los
    );

    const float skill_factor = bot_get_skill_factor();

    rf::Vector3 move_target{};
    bool has_move_target = false;
    bot_navigation_manager_update_move_target(
        *local_entity,
        enemy_target,
        enemy_has_los,
        pursuing_enemy_goal,
        move_target,
        has_move_target
    );
    const BotFsmState resolved_state = bot_fsm_manager_select_state(
        *local_player,
        *local_entity,
        enemy_target,
        pursuing_enemy_goal,
        enemy_has_los,
        has_move_target
    );
    if (resolved_state != g_client_bot_state.fsm_state) {
        bot_fsm_manager_transition_state(resolved_state);
        bot_navigation_manager_update_move_target(
            *local_entity,
            enemy_target,
            enemy_has_los,
            pursuing_enemy_goal,
            move_target,
            has_move_target
        );
    }

    if (g_client_bot_state.active_goal == BotGoalType::none) {
        // Runtime invariant: movement/combat stages should never run with no active goal.
        bot_goal_manager_ensure_active(*local_entity, enemy_target, enemy_has_los, 180);

        pursuing_enemy_goal = bot_navigation_manager_sync_pursuit_target(enemy_target);
        const BotFsmState invariant_state = bot_fsm_manager_select_state(
            *local_player,
            *local_entity,
            enemy_target,
            pursuing_enemy_goal,
            enemy_has_los,
            false
        );
        bot_fsm_manager_transition_state(invariant_state);
        bot_navigation_manager_update_move_target(
            *local_entity,
            enemy_target,
            enemy_has_los,
            pursuing_enemy_goal,
            move_target,
            has_move_target
        );
    }

    const GoalStuckWatchdogAction stuck_watchdog_action =
        update_goal_stuck_watchdog(*local_entity);
    if (stuck_watchdog_action != GoalStuckWatchdogAction::none) {
        reset_position_stall_watchdog();
        reset_objective_progress_watchdog(true);
        if (stuck_watchdog_action == GoalStuckWatchdogAction::abandon_goal) {
            bot_goal_manager_force_refresh_and_ensure(*local_entity, enemy_target, enemy_has_los, 180);
        }

        pursuing_enemy_goal = bot_navigation_manager_sync_pursuit_target(enemy_target);
        const BotFsmState watchdog_state = bot_fsm_manager_select_state(
            *local_player,
            *local_entity,
            enemy_target,
            pursuing_enemy_goal,
            enemy_has_los,
            false
        );
        bot_fsm_manager_transition_state(watchdog_state);
        bot_navigation_manager_update_move_target(
            *local_entity,
            enemy_target,
            enemy_has_los,
            pursuing_enemy_goal,
            move_target,
            has_move_target
        );
    }

    const PositionStallWatchdogAction position_stall_action =
        update_position_stall_watchdog(
            *local_entity,
            has_move_target,
            pursuing_enemy_goal,
            enemy_has_los
        );
    if (position_stall_action != PositionStallWatchdogAction::none) {
        if (position_stall_action == PositionStallWatchdogAction::abandon_goal) {
            reset_goal_stuck_watchdog(true);
            reset_no_move_target_watchdog();
            reset_objective_progress_watchdog(true);
            bot_goal_manager_force_refresh_and_ensure(*local_entity, enemy_target, enemy_has_los, 180);
        }
        pursuing_enemy_goal = bot_navigation_manager_sync_pursuit_target(enemy_target);
        const BotFsmState stall_watchdog_state = bot_fsm_manager_select_state(
            *local_player,
            *local_entity,
            enemy_target,
            pursuing_enemy_goal,
            enemy_has_los,
            false
        );
        bot_fsm_manager_transition_state(stall_watchdog_state);
        bot_navigation_manager_update_move_target(
            *local_entity,
            enemy_target,
            enemy_has_los,
            pursuing_enemy_goal,
            move_target,
            has_move_target
        );
    }

    if (has_move_target) {
        reset_no_move_target_watchdog();
    }
    else if (bot_fsm_state_should_have_move_target(
                 g_client_bot_state.fsm_state,
                 pursuing_enemy_goal,
                 enemy_has_los)) {
        if (!g_client_bot_state.no_move_target_watchdog_timer.valid()) {
            g_client_bot_state.no_move_target_watchdog_timer.set(kNoMoveTargetWatchdogMs);
        }
        else if (g_client_bot_state.no_move_target_watchdog_timer.elapsed()) {
            ++g_client_bot_state.no_move_target_watchdog_retries;
            g_client_bot_state.no_move_target_watchdog_timer.set(kNoMoveTargetWatchdogMs);
            bot_internal_set_last_heading_change_reason("no_move_target_reroute");

            const int avoid_waypoint =
                g_client_bot_state.goal_target_waypoint > 0
                    ? g_client_bot_state.goal_target_waypoint
                    : g_client_bot_state.waypoint_goal;
            bot_internal_start_recovery_anchor_reroute(*local_entity, avoid_waypoint);
            bot_navigation_manager_update_move_target(
                *local_entity,
                enemy_target,
                enemy_has_los,
                pursuing_enemy_goal,
                move_target,
                has_move_target
            );

            if (!has_move_target
                && g_client_bot_state.no_move_target_watchdog_retries >= kNoMoveTargetWatchdogRetryLimit) {
                if (g_alpine_game_config.dbg_bot) {
                    xlog::warn(
                        "Bot no-move watchdog: clearing stalled goal={} fsm={} handle={} id={} waypoint={}",
                        static_cast<int>(g_client_bot_state.active_goal),
                        static_cast<int>(g_client_bot_state.fsm_state),
                        g_client_bot_state.goal_target_handle,
                        g_client_bot_state.goal_target_identifier,
                        g_client_bot_state.goal_target_waypoint
                    );
                }

                const BotGoalType stalled_goal = g_client_bot_state.active_goal;
                const int stalled_handle = g_client_bot_state.goal_target_handle;
                bot_memory_manager_note_failed_goal_target(
                    stalled_goal,
                    stalled_handle,
                    kFailedItemGoalCooldownMs
                );

                bot_state_set_roam_fallback_goal(220);
                g_client_bot_state.eliminate_target_reacquire_timer.invalidate();
                bot_state_clear_waypoint_route(true, true, false);
                bot_internal_set_last_heading_change_reason("no_move_target_goal_reset");
                reset_goal_stuck_watchdog(true);
                reset_no_move_target_watchdog();
                reset_position_stall_watchdog();
                reset_objective_progress_watchdog(true);

                bot_goal_manager_force_refresh_and_ensure(*local_entity, enemy_target, enemy_has_los, 180);
                pursuing_enemy_goal = bot_navigation_manager_sync_pursuit_target(enemy_target);
                const BotFsmState no_move_watchdog_state = bot_fsm_manager_select_state(
                    *local_player,
                    *local_entity,
                    enemy_target,
                    pursuing_enemy_goal,
                    enemy_has_los,
                    false
                );
                bot_fsm_manager_transition_state(no_move_watchdog_state);
                bot_navigation_manager_update_move_target(
                    *local_entity,
                    enemy_target,
                    enemy_has_los,
                    pursuing_enemy_goal,
                    move_target,
                    has_move_target
                );
            }

            if (has_move_target) {
                reset_no_move_target_watchdog();
            }
        }
    }
    else {
        reset_no_move_target_watchdog();
    }

    const ObjectiveProgressWatchdogAction objective_progress_action =
        update_objective_progress_watchdog(
            *local_entity,
            enemy_target,
            enemy_has_los,
            pursuing_enemy_goal,
            combat_lock_expired
        );
    if (objective_progress_action != ObjectiveProgressWatchdogAction::none) {
        reset_goal_stuck_watchdog(true);
        reset_no_move_target_watchdog();
        reset_position_stall_watchdog();
        if (objective_progress_action == ObjectiveProgressWatchdogAction::abandon_goal) {
            bot_goal_manager_force_refresh_and_ensure(*local_entity, enemy_target, enemy_has_los, 180);
        }

        pursuing_enemy_goal = bot_navigation_manager_sync_pursuit_target(enemy_target);
        const BotFsmState objective_watchdog_state = bot_fsm_manager_select_state(
            *local_player,
            *local_entity,
            enemy_target,
            pursuing_enemy_goal,
            enemy_has_los,
            false
        );
        bot_fsm_manager_transition_state(objective_watchdog_state);
        bot_navigation_manager_update_move_target(
            *local_entity,
            enemy_target,
            enemy_has_los,
            pursuing_enemy_goal,
            move_target,
            has_move_target
        );
    }

    const GlobalFailsafeWatchdogAction global_failsafe_action =
        update_global_failsafe_watchdog(
            *local_entity,
            enemy_target,
            enemy_has_los,
            pursuing_enemy_goal,
            has_move_target,
            bot_aim_dir,
            combat_lock_expired
        );
    if (global_failsafe_action != GlobalFailsafeWatchdogAction::none) {
        reset_goal_stuck_watchdog(true);
        reset_no_move_target_watchdog();
        reset_position_stall_watchdog();
        reset_objective_progress_watchdog(true);

        bot_goal_manager_force_refresh_and_ensure(*local_entity, enemy_target, enemy_has_los, 180);
        pursuing_enemy_goal = bot_navigation_manager_sync_pursuit_target(enemy_target);
        const BotFsmState global_failsafe_state = bot_fsm_manager_select_state(
            *local_player,
            *local_entity,
            enemy_target,
            pursuing_enemy_goal,
            enemy_has_los,
            false
        );
        bot_fsm_manager_transition_state(global_failsafe_state);
        bot_navigation_manager_update_move_target(
            *local_entity,
            enemy_target,
            enemy_has_los,
            pursuing_enemy_goal,
            move_target,
            has_move_target
        );
    }

    update_bot_status_hud();
    update_bot_status_console(
        *local_entity,
        enemy_target,
        enemy_has_los,
        pursuing_enemy_goal,
        has_move_target,
        move_target
    );

    bot_combat_manager_process_frame(
        *local_player,
        *local_entity,
        enemy_target,
        enemy_has_los,
        skill_factor,
        has_move_target,
        move_target
    );

    if (!has_move_target) {
        return;
    }

    bot_process_movement(
        *local_player,
        *local_entity,
        move_target,
        pursuing_enemy_goal,
        enemy_has_los,
        skill_factor
    );
}
