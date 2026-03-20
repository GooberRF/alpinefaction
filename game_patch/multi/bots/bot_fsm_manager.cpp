#include "bot_fsm_manager.h"

#include "bot_fsm.h"
#include "bot_personality_manager.h"
#include "bot_state.h"
#include <algorithm>
#include <cmath>

BotFsmState bot_fsm_manager_select_state(
    const rf::Player& local_player,
    const rf::Entity& local_entity,
    const rf::Entity* enemy_target,
    const bool pursuing_enemy_goal,
    const bool enemy_has_los,
    const bool has_move_target)
{
    if (pursuing_enemy_goal && enemy_has_los) {
        // Active target with LOS should not remain in stale recovery mode.
        g_client_bot_state.recovery_pending_reroute = false;
        g_client_bot_state.pursuit_recovery_timer.invalidate();
    }

    if (g_client_bot_state.recovery_pending_reroute
        && g_client_bot_state.recovery_anchor_waypoint <= 0) {
        g_client_bot_state.recovery_pending_reroute = false;
    }

    // Recovery navigation is only forced for goals that need precise pathing (combat pursuit,
    // item collection, objectives). For roam goals, skip recovery — the bot can just pick any
    // nearby waypoint and walk there without the full recovery anchor system.
    const bool recovery_applicable =
        g_client_bot_state.active_goal != BotGoalType::roam
        && g_client_bot_state.active_goal != BotGoalType::none;
    if (recovery_applicable
        && (g_client_bot_state.recovery_pending_reroute
            || (g_client_bot_state.pursuit_recovery_timer.valid()
                && !g_client_bot_state.pursuit_recovery_timer.elapsed()))) {
        return BotFsmState::recover_navigation;
    }
    if (!recovery_applicable && g_client_bot_state.recovery_pending_reroute) {
        bot_state_clear_recovery_reroute();
    }

    const bool retreat_now =
        enemy_target
        && bot_personality_manager_should_retreat_state(local_entity, enemy_has_los);
    const bool needs_weapon =
        bot_personality_manager_should_seek_weapon_state(local_player, local_entity);
    const bool needs_replenish =
        bot_personality_manager_should_replenish_state(local_entity);
    const bool deathmatch_mode = bot_personality_manager_is_deathmatch_mode();
    const bool combat_ready_now =
        enemy_target
        && bot_personality_manager_is_combat_ready(local_entity, enemy_target);
    const bool keep_pressure_on_enemy =
        pursuing_enemy_goal
        && enemy_target
        && (enemy_has_los || (deathmatch_mode && combat_ready_now));

    if (retreat_now) {
        return BotFsmState::retreat;
    }

    if (g_client_bot_state.active_goal == BotGoalType::activate_bridge) {
        return BotFsmState::activate_bridge;
    }
    if (g_client_bot_state.active_goal == BotGoalType::create_crater) {
        return BotFsmState::create_crater;
    }
    if (g_client_bot_state.active_goal == BotGoalType::shatter_glass) {
        return BotFsmState::shatter_glass;
    }
    if (bot_goal_is_ctf_objective(g_client_bot_state.active_goal)) {
        if (g_client_bot_state.active_goal == BotGoalType::ctf_hold_enemy_flag
            && needs_replenish
            && !keep_pressure_on_enemy) {
            return BotFsmState::replenish_health_armor;
        }
        return BotFsmState::ctf_objective;
    }
    if (bot_goal_is_control_point_objective(g_client_bot_state.active_goal)) {
        return BotFsmState::control_point_objective;
    }

    if (bot_goal_is_item_collection(g_client_bot_state.active_goal)) {
        return BotFsmState::collect_pickup;
    }
    if (g_client_bot_state.active_goal == BotGoalType::roam) {
        return BotFsmState::roam;
    }

    if (needs_replenish && !keep_pressure_on_enemy) {
        return BotFsmState::replenish_health_armor;
    }

    if (needs_weapon && !keep_pressure_on_enemy) {
        return BotFsmState::seek_weapon;
    }

    if (g_client_bot_state.active_goal == BotGoalType::eliminate_target && !pursuing_enemy_goal) {
        return BotFsmState::seek_enemy;
    }

    if (pursuing_enemy_goal) {
        if (!enemy_has_los && enemy_target) {
            const float enemy_dist = std::sqrt(std::max(
                rf::vec_dist_squared(&local_entity.pos, &enemy_target->pos),
                0.0f
            ));
            const float power_position_bias = std::clamp(
                bot_personality_manager_get_active_personality().power_position_bias,
                0.25f,
                2.5f
            );
            const float trigger_distance = std::lerp(
                16.0f,
                7.0f,
                std::clamp(power_position_bias * 0.4f, 0.0f, 1.0f)
            );
            if (enemy_dist > trigger_distance) {
                return BotFsmState::find_power_position;
            }
        }
        return enemy_has_los ? BotFsmState::engage_enemy : BotFsmState::pursue_enemy;
    }

    if (needs_replenish) {
        return BotFsmState::replenish_health_armor;
    }

    if (needs_weapon) {
        return BotFsmState::seek_weapon;
    }

    if (g_client_bot_state.combat_los_target_waypoint > 0) {
        return BotFsmState::reposition_los;
    }

    return BotFsmState::roam;
}

void bot_fsm_manager_transition_state(
    const BotFsmState new_state,
    const int transition_timer_ms)
{
    bot_fsm_update_state(new_state, transition_timer_ms);
}
