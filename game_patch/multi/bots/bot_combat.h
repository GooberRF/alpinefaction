#pragma once

#include "../../rf/entity.h"
#include "../../rf/player/player.h"

void clear_enemy_aim_error_state();
void clear_semi_auto_click_state();
// Turns towards the aim goal of the last think; server bots run it on the frames they do not think.
void bot_track_aim_goal(rf::Entity& local_entity);
void ensure_weapon_ready(rf::Player& player, rf::Entity& entity);
bool bot_has_usable_crater_weapon(rf::Player& player, rf::Entity& entity);
bool bot_has_usable_shatter_weapon(rf::Player& player, rf::Entity& entity);
// Expected damage per second of the best usable weapon at the distance, with the active bot's aim.
float bot_best_combat_weapon_dps(rf::Player& player, rf::Entity& entity, float target_distance);
// Distance the held weapon works best at (-1 if none) and the distance its own blast needs.
float bot_combat_engagement_distance(const rf::Entity& entity, float& out_min_safe_distance);
float bot_combat_splash_keep_distance(const rf::Entity& entity);
int bot_weapon_create_delay_ms(int weapon_type, bool alt_fire);
float bot_crater_standoff_distance(rf::Player& player, rf::Entity& entity, float& out_max_distance);
void bot_process_combat(
    rf::Player& local_player,
    rf::Entity& local_entity,
    rf::Entity* enemy_target,
    bool enemy_has_los,
    float skill_factor,
    bool has_move_target,
    const rf::Vector3& move_target);
