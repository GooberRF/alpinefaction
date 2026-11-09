#pragma once

// Payload_Handler and Payload_Checkpoint event fields and the level's Payload rules, shared by the game and RED. RED
// zero-fills a new event, so every event field reads 0, false or empty as its default.

#include <algorithm>
#include <array>
#include <cmath>

namespace payload
{

inline constexpr float default_push_radius = 3.0f;
inline constexpr float min_push_radius = 0.5f;
inline constexpr float max_push_radius = 20.0f;
inline constexpr int default_rollback_delay = 30;
inline constexpr int max_rollback_delay = 600;
inline constexpr const char* default_name = "Payload";
inline constexpr int heal_tick_ms = 1000;
// More pushers than this don't move the cart any faster
inline constexpr int max_useful_pushers = 3;
inline constexpr float max_checkpoint_hold = 60.0f;

// Level properties: defaults and clamps
inline constexpr float default_push_speed = 1.5f;
inline constexpr float min_push_speed = 0.1f;
inline constexpr float max_push_speed = 20.0f;
inline constexpr float default_rollback_speed = 0.5f;
inline constexpr float max_rollback_speed = 4.0f;
inline constexpr float default_cart_accel = 2.0f;
inline constexpr float min_cart_accel = 0.1f;
inline constexpr float max_cart_accel = 100.0f;
inline constexpr float default_checkpoint_time_bonus = 180.0f;
inline constexpr float max_checkpoint_time_bonus = 3600.0f;
inline constexpr float default_heal_per_second = 10.0f;
inline constexpr float max_heal_per_second = 500.0f;
inline constexpr float default_overtime_grace = 5.0f;
inline constexpr float max_overtime_grace = 60.0f;
inline constexpr float max_respawn_extra = 60.0f;

// A level file can carry anything, so a value that isn't finite takes the default
inline float clamp_rule(float value, float lo, float hi, float fallback)
{
    return std::isfinite(value) ? std::clamp(value, lo, hi) : fallback;
}

// Seconds added to a team's respawn delay, on top of the server's
struct RespawnExtras
{
    float base = 0.0f;
    float per_checkpoint = 0.0f; // for each checkpoint the cart has passed
    float final_stretch = 0.0f; // once the cart has reached the final stretch

    void sanitize(const RespawnExtras& fallback)
    {
        base = clamp_rule(base, -max_respawn_extra, max_respawn_extra, fallback.base);
        per_checkpoint = clamp_rule(per_checkpoint, -max_respawn_extra, max_respawn_extra, fallback.per_checkpoint);
        final_stretch = clamp_rule(final_stretch, -max_respawn_extra, max_respawn_extra, fallback.final_stretch);
    }
};

inline constexpr RespawnExtras default_attacker_respawn{};
inline constexpr RespawnExtras default_defender_respawn{1.0f, 0.5f, 1.0f};

// The Payload group of Level Properties
struct LevelRules
{
    float push_speed = default_push_speed; // m/s with one pusher
    float rollback_speed = default_rollback_speed; // fraction of push_speed
    float cart_accel = default_cart_accel; // m/s/s
    float checkpoint_time_bonus = default_checkpoint_time_bonus; // s
    float heal_per_second = default_heal_per_second;
    float overtime_grace = default_overtime_grace; // s
    RespawnExtras attacker_respawn = default_attacker_respawn;
    RespawnExtras defender_respawn = default_defender_respawn;

    void sanitize()
    {
        push_speed = clamp_rule(push_speed, min_push_speed, max_push_speed, default_push_speed);
        rollback_speed = clamp_rule(rollback_speed, 0.0f, max_rollback_speed, default_rollback_speed);
        cart_accel = clamp_rule(cart_accel, min_cart_accel, max_cart_accel, default_cart_accel);
        checkpoint_time_bonus =
            clamp_rule(checkpoint_time_bonus, 0.0f, max_checkpoint_time_bonus, default_checkpoint_time_bonus);
        heal_per_second = clamp_rule(heal_per_second, 0.0f, max_heal_per_second, default_heal_per_second);
        overtime_grace = clamp_rule(overtime_grace, 0.0f, max_overtime_grace, default_overtime_grace);
        attacker_respawn.sanitize(default_attacker_respawn);
        defender_respawn.sanitize(default_defender_respawn);
    }

    // In the level file's order; const rules give const pointers
    template<typename Rules>
    static auto file_fields(Rules& r)
    {
        return std::array{&r.push_speed, &r.rollback_speed, &r.cart_accel, &r.checkpoint_time_bonus, &r.heal_per_second,
                          &r.overtime_grace, &r.attacker_respawn.base, &r.attacker_respawn.per_checkpoint,
                          &r.attacker_respawn.final_stretch, &r.defender_respawn.base,
                          &r.defender_respawn.per_checkpoint, &r.defender_respawn.final_stretch};
    }
};

// int1 is the index of a Blue / Red dropdown; returns the RF team id (0 red, 1 blue)
inline int attacking_team(int int1)
{
    return int1 == 1 ? 0 : 1;
}

inline float push_radius(float float1)
{
    if (!(float1 > 0.0f)) {
        return default_push_radius;
    }
    return std::clamp(float1, min_push_radius, max_push_radius);
}

// Seconds before an unpushed cart rolls back, or a negative value if it never does
inline int rollback_delay(int int2)
{
    if (int2 < 0) {
        return -1;
    }
    return int2 == 0 ? default_rollback_delay : std::min(int2, max_rollback_delay);
}

inline bool heals(bool bool1)
{
    return !bool1;
}

inline bool has_finale(bool bool2)
{
    return !bool2;
}

// Speed multiplier for the number of attackers pushing, the same curve as capture points
inline float push_multiplier(int pushers)
{
    if (pushers <= 0) {
        return 0.0f;
    }
    return 1.0f + 0.5f * static_cast<float>(std::min(pushers, max_useful_pushers) - 1);
}

inline const char* name(const char* str1)
{
    return (str1 && *str1) ? str1 : default_name;
}

// Seconds a checkpoint holds the cart after its capture, 0 for none
inline float checkpoint_hold(float float1)
{
    if (!(float1 > 0.0f)) {
        return 0.0f;
    }
    return std::min(float1, max_checkpoint_hold);
}

// Seconds a checkpoint's capture adds to the clock: int1, or the level's value for 0, or none for a negative int1
inline float checkpoint_time_bonus(int int1, float level_bonus)
{
    if (int1 < 0) {
        return 0.0f;
    }
    return int1 == 0 ? level_bonus : std::min(static_cast<float>(int1), max_checkpoint_time_bonus);
}

} // namespace payload
