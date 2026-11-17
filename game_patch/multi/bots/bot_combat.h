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
// One frame's turn of a (yaw, non-linear pitch) aim toward the unit dir, with the active bot's smoothing and turn
// rates. True when the new aim is aligned enough to fire; out_alignment gets its dot product with dir.
bool bot_aim_step(float& yaw, float& pitch, const rf::Vector3& dir, float dt, float skill_factor, float& out_alignment);
// The least dot product of aim and target direction the active bot fires at.
float bot_aim_fire_alignment(float skill_factor);
// Where a shot of weapon_type from origin must aim to hit the enemy, with the active bot's lead and aim error.
rf::Vector3 bot_combat_enemy_aim_point(
    const rf::Vector3& origin, int weapon_type, const rf::Entity& enemy, float skill_factor);
float bot_combat_weapon_effective_range(int weapon_type);
// A blast at impact_pos spares a rider sitting out in the open, and his team-mates.
bool bot_combat_mounted_splash_is_safe(const rf::Entity& rider, int weapon_type, const rf::Vector3& impact_pos);
