#include "bot_fsm.h"

#include "bot_state.h"

bool bot_fsm_state_should_have_move_target(
    const BotFsmState fsm_state,
    const bool pursuing_enemy_goal,
    const bool enemy_has_los)
{
    switch (fsm_state) {
        case BotFsmState::seek_enemy:
        case BotFsmState::pursue_enemy:
        case BotFsmState::collect_pickup:
        case BotFsmState::reposition_los:
        case BotFsmState::recover_navigation:
        case BotFsmState::retreat:
        case BotFsmState::seek_weapon:
        case BotFsmState::replenish_health_armor:
        case BotFsmState::find_power_position:
        case BotFsmState::activate_bridge:
        case BotFsmState::create_crater:
        case BotFsmState::shatter_glass:
        case BotFsmState::ctf_objective:
        case BotFsmState::control_point_objective:
        case BotFsmState::roam:
            return true;
        case BotFsmState::engage_enemy:
            return pursuing_enemy_goal && !enemy_has_los;
        default:
            return false;
    }
}

bool bot_fsm_is_contextual_item_state(const BotFsmState fsm_state)
{
    return fsm_state == BotFsmState::seek_weapon
        || fsm_state == BotFsmState::replenish_health_armor;
}

void bot_fsm_update_state(const BotFsmState new_state, const int transition_timer_ms)
{
    bot_state_transition_fsm(new_state, transition_timer_ms);
}

const char* bot_fsm_state_to_string(const BotFsmState state)
{
    switch (state) {
        case BotFsmState::inactive:
            return "inactive";
        case BotFsmState::idle:
            return "idle";
        case BotFsmState::roam:
            return "roam";
        case BotFsmState::seek_enemy:
            return "seek_enemy";
        case BotFsmState::pursue_enemy:
            return "pursue_enemy";
        case BotFsmState::engage_enemy:
            return "engage_enemy";
        case BotFsmState::collect_pickup:
            return "collect_pickup";
        case BotFsmState::reposition_los:
            return "reposition_los";
        case BotFsmState::recover_navigation:
            return "recover_navigation";
        case BotFsmState::retreat:
            return "retreat";
        case BotFsmState::seek_weapon:
            return "seek_weapon";
        case BotFsmState::replenish_health_armor:
            return "replenish_health_armor";
        case BotFsmState::find_power_position:
            return "find_power_position";
        case BotFsmState::activate_bridge:
            return "activate_bridge";
        case BotFsmState::create_crater:
            return "create_crater";
        case BotFsmState::shatter_glass:
            return "shatter_glass";
        case BotFsmState::ctf_objective:
            return "ctf_objective";
        case BotFsmState::control_point_objective:
            return "control_point_objective";
        default:
            return "unknown";
    }
}

const char* bot_goal_type_to_string(const BotGoalType goal)
{
    switch (goal) {
        case BotGoalType::none:
            return "none";
        case BotGoalType::eliminate_target:
            return "eliminate_target";
        case BotGoalType::collect_weapon:
            return "collect_weapon";
        case BotGoalType::collect_ammo:
            return "collect_ammo";
        case BotGoalType::collect_health:
            return "collect_health";
        case BotGoalType::collect_armor:
            return "collect_armor";
        case BotGoalType::collect_super_item:
            return "collect_super_item";
        case BotGoalType::activate_bridge:
            return "activate_bridge";
        case BotGoalType::create_crater:
            return "create_crater";
        case BotGoalType::shatter_glass:
            return "shatter_glass";
        case BotGoalType::ctf_steal_flag:
            return "ctf_steal_flag";
        case BotGoalType::ctf_return_flag:
            return "ctf_return_flag";
        case BotGoalType::ctf_capture_flag:
            return "ctf_capture_flag";
        case BotGoalType::ctf_hold_enemy_flag:
            return "ctf_hold_enemy_flag";
        case BotGoalType::control_point_objective:
            return "control_point_objective";
        case BotGoalType::roam:
            return "roam";
        default:
            return "unknown";
    }
}
