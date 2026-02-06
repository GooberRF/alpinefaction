#include "bots.h"
#include "gametype.h"
#include "../main/main.h"
#include "../rf/ai.h"
#include "../rf/entity.h"
#include "../rf/gameseq.h"
#include "../rf/multi.h"
#include "../rf/player/player.h"
#include "multi.h"
#include <algorithm>
#include <limits>
#include <random>
#include <unordered_set>
#include <unordered_map>
#include <vector>

static std::unordered_map<rf::Player*, BotNavigationState> g_bot_navigation_state{};

static BotNavigationState& bot_navigation_for(rf::Player& player)
{
    return g_bot_navigation_state[&player];
}

static void bot_ai_control_info_reset(rf::AiInfo& ai)
{
    ai.ci.rot = {};
    ai.ci.move = {};
    ai.ci.field_18 = false;
    ai.ci.mouse_dh = 0.0f;
    ai.ci.mouse_dp = 0.0f;
}

static void bot_ai_execute_behavior(rf::AiInfo& ai)
{
    //xlog::warn("executing behaviour for mode {}", static_cast<int>(ai.mode));
    switch (ai.mode) {
        case rf::AI_MODE_WAITING:
            rf::ai_waiting_do_frame(&ai);
            break;
        case rf::AI_MODE_CHASE:
            rf::ai_chase_do_frame(&ai);
            break;
        case rf::AI_MODE_WAYPOINTS:
            rf::ai_waypoints_do_frame(&ai);
            break;
        default:
            rf::ai_set_mode(&ai, rf::AI_MODE_CHASE, -1, -1);
            rf::ai_chase_do_frame(&ai);
            break;
    }
}

static void bot_ai_do_frame(rf::AiInfo& ai)
{
    if (!ai.ep) {
        return;
    }

    if ((ai.ep->obj_flags & rf::OF_DELAYED_DELETE) != rf::ObjectFlags{}) {
        return;
    }

    bot_ai_control_info_reset(ai);
    ai.steering_vector = {};
    bot_ai_execute_behavior(ai);

    ai.ci.move.x = std::clamp(ai.ci.move.x, -1.0f, 1.0f);
    ai.ci.move.y = std::clamp(ai.ci.move.y, -1.0f, 1.0f);
    ai.ci.move.z = std::clamp(ai.ci.move.z, -1.0f, 1.0f);
}

static rf::Entity* bot_ai_select_target(const rf::Player& bot_player, const rf::Entity& bot_entity)
{
    const bool team_mode = multi_is_team_game_type();
    rf::Entity* best_target = nullptr;
    float best_dist_sq = std::numeric_limits<float>::max();

    for (const rf::Player& candidate : SinglyLinkedList{rf::player_list}) {
        if (&candidate == &bot_player) {
            continue;
        }

        if (candidate.is_spawn_disabled || candidate.is_spectator || candidate.is_browser) {
            continue;
        }

        if (team_mode && candidate.team == bot_player.team) {
            continue;
        }

        if (rf::player_is_dead(&candidate) || rf::player_is_dying(&candidate)) {
            continue;
        }

        rf::Entity* target_entity = rf::entity_from_handle(candidate.entity_handle);
        if (!target_entity) {
            continue;
        }

        float dist_sq = rf::vec_dist_squared(&bot_entity.pos, &target_entity->pos);
        if (dist_sq < best_dist_sq) {
            best_dist_sq = dist_sq;
            best_target = target_entity;
        }
    }

    return best_target;
}

static void bot_waypoint_nav_clear(rf::Player& player)
{
    auto& nav = bot_navigation_for(player);
    nav.path.clear();
    nav.next_index = 0;
    nav.goal_waypoint = 0;
}

static bool bot_waypoint_nav_pick_route(rf::Player& player, int start_waypoint)
{
    const int waypoint_total = waypoints_count();
    if (waypoint_total <= 1) {
        return false;
    }
    std::uniform_int_distribution<int> dist(1, waypoint_total - 1);
    std::unordered_set<int> avoidset;
    std::vector<int> new_path;
    for (int attempt = 0; attempt < kBotWaypointPickAttempts; ++attempt) {
        int candidate = dist(g_rng);
        if (candidate == start_waypoint) {
            continue;
        }
        if (!waypoints_route(start_waypoint, candidate, avoidset, new_path)) {
            continue;
        }
        auto& nav = bot_navigation_for(player);
        nav.path = std::move(new_path);
        nav.next_index = (nav.path.size() > 1) ? 1 : 0;
        nav.goal_waypoint = candidate;
        nav.repath_timer.set(kBotWaypointRepathMs);
        return true;
    }
    return false;
}

static bool bot_waypoint_nav_update(rf::Player& player, rf::Entity& entity)
{
    auto& nav = bot_navigation_for(player);
    if (!nav.stuck_timer.valid()) {
        nav.stuck_timer.set(kBotWaypointStuckCheckMs);
        nav.last_pos = entity.pos;
    } else if (nav.stuck_timer.elapsed()) {
        float moved_sq = rf::vec_dist_squared(&nav.last_pos, &entity.pos);
        if (moved_sq < kBotWaypointStuckDistance * kBotWaypointStuckDistance) {
            bot_waypoint_nav_clear(player);
        }
        nav.last_pos = entity.pos;
        nav.stuck_timer.set(kBotWaypointStuckCheckMs);
    }

    int closest = waypoints_closest(entity.pos, kBotWaypointSearchRadius);
    if (closest <= 0) {
        bot_waypoint_nav_clear(player);
        return false;
    }

    const bool need_route = nav.path.empty()
        || nav.next_index < 0
        || nav.next_index >= static_cast<int>(nav.path.size())
        || (nav.repath_timer.valid() && nav.repath_timer.elapsed());
    if (need_route) {
        if (!bot_waypoint_nav_pick_route(player, closest)) {
            bot_waypoint_nav_clear(player);
            return false;
        }
    }

    if (nav.path.empty()) {
        return false;
    }

    int current_waypoint = nav.path[nav.next_index];
    rf::Vector3 target_pos{};
    if (!waypoints_get_pos(current_waypoint, target_pos)) {
        bot_waypoint_nav_clear(player);
        return false;
    }

    float dist_sq = rf::vec_dist_squared(&entity.pos, &target_pos);
    if (dist_sq <= kBotWaypointReachRadius * kBotWaypointReachRadius) {
        if (nav.next_index + 1 < static_cast<int>(nav.path.size())) {
            ++nav.next_index;
            current_waypoint = nav.path[nav.next_index];
            if (!waypoints_get_pos(current_waypoint, target_pos)) {
                bot_waypoint_nav_clear(player);
                return false;
            }
        } else {
            bot_waypoint_nav_clear(player);
            return false;
        }
    }

    entity.ai.current_path.goal_pos = target_pos;
    entity.ai.current_path.current_goal_pos = target_pos;
    entity.ai.current_path.has_current_goal_pos = true;
    entity.ai.current_path.goal_waypoint_index = current_waypoint;
    entity.ai.current_path.waypoint_list_index = -1;
    entity.ai.current_path.num_nodes = 0;
    entity.ai.current_path.start_pos = entity.pos;
    return true;
}

void bot_ai_do_frame()
{
    if (!rf::is_server || rf::gameseq_get_state() != rf::GS_GAMEPLAY) {
        return;
    }

    const bool team_mode = multi_is_team_game_type();

    for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
        if (!player.is_bot || player.is_spawn_disabled) {
            continue;
        }

        if (rf::player_is_dead(&player) || rf::player_is_dying(&player)) {
            continue;
        }

        rf::Entity* entity = rf::entity_from_handle(player.entity_handle);
        if (!entity) {
            continue;
        }

        bool needs_target = entity->ai.target_handle < 0;
        if (!needs_target) {
            rf::Entity* current_target = rf::entity_from_handle(entity->ai.target_handle);
            if (!current_target) {
                needs_target = true;
            } else if (team_mode && current_target->team == entity->team) {
                needs_target = true;
            }
        }

        if (needs_target) {
            rf::ai_acquire_new_target(&entity->ai);
            if (entity->ai.target_handle < 0) {
                if (rf::Entity* target = bot_ai_select_target(player, *entity)) {
                    rf::ai_set_target(&entity->ai, target->handle);
                }
            }
            if (entity->ai.target_handle >= 0) {
                rf::ai_enter_attack_mode(&entity->ai);
                rf::ai_set_mode(&entity->ai, rf::AI_MODE_CHASE, -1, -1);
                rf::ai_set_submode(&entity->ai, rf::AI_SUBMODE_ATTACK);
            }
        }

        if (entity->ai.target_handle < 0) {
            const bool has_waypoint_goal = bot_waypoint_nav_update(player, *entity);
            if (has_waypoint_goal) {
                rf::ai_set_mode(&entity->ai, rf::AI_MODE_WAYPOINTS, -1, -1);
                rf::ai_set_submode(&entity->ai, rf::AI_SUBMODE_ON_PATH);
            } else {
                rf::ai_set_mode(&entity->ai, rf::AI_MODE_CHASE, -1, -1);
                rf::ai_set_submode(&entity->ai, rf::AI_SUBMODE_NONE);
            }
        }

        bot_ai_do_frame(entity->ai);

        if (entity->ai.target_handle >= 0 && entity->ai.current_primary_weapon >= 0) {
            if (rf::Entity* target = rf::entity_from_handle(entity->ai.target_handle)) {
                float range = rf::ai_get_attack_range(entity->ai);
                float dist_sq = rf::vec_dist_squared(&entity->pos, &target->pos);
                if (range > 0.0f && dist_sq <= range * range) {
                    const bool alt_fire = (entity->ai.ai_flags & rf::AIF_ALT_FIRE) != 0;
                    rf::entity_turn_weapon_on(entity->handle, entity->ai.current_primary_weapon, alt_fire);
                } else {
                    rf::entity_turn_weapon_off(entity->handle, entity->ai.current_primary_weapon);
                }
            }
        } else if (entity->ai.current_primary_weapon >= 0) {
            rf::entity_turn_weapon_off(entity->handle, entity->ai.current_primary_weapon);
        }
    }
}
