#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

enum class BotWeaponDamageClass : uint8_t
{
    bullet = 0,
    piercing_bullet = 1,
    explosive_projectile = 2,
    thrown_explosive_projectile = 3,
    spray_flames = 4,
    melee = 5,
};

enum class BotWeaponFireMode : uint8_t
{
    none = 0,
    shoot = 1,
    swing = 2,
    throw_projectile = 3,
    throw_timed_detonation = 4,
    throw_impact_detonation = 5,
    high_impact_shoot = 6,
    engage_scope = 7,
    use_taser = 8,
    spray_flame_stream = 9,
    precision_alt_shoot = 10,
};

enum class BotWeaponRangeBand : uint8_t
{
    melee = 0,
    close = 1,
    medium = 2,
    long_range = 3,
    very_long = 4,
};

enum class BotWeaponSpecialProperty : uint32_t
{
    none = 0,
    creates_craters = 1u << 0,
    taser_finisher = 1u << 1,
    always_use_alt_fire = 1u << 2,
    use_scope_for_long_range = 1u << 3,
    multi_projectile_shot = 1u << 4,
    avoid_alt_fire = 1u << 5,
    alt_fire_for_long_range = 1u << 6,
    pierces_world = 1u << 7,
};

struct BotWeaponProfile
{
    std::string_view name{};
    BotWeaponDamageClass damage_class = BotWeaponDamageClass::bullet;
    bool is_semi_auto = false;
    BotWeaponFireMode primary_fire = BotWeaponFireMode::shoot;
    BotWeaponFireMode alt_fire = BotWeaponFireMode::none;
    int value_tier = 0;
    uint8_t range_band_mask = 0;
    uint32_t special_property_mask = 0;
    int weapon_type = -1;
    std::vector<int> weapon_item_ids{};
    std::vector<int> ammo_item_ids{};
};

void bot_weapon_profiles_init_for_active_bot();
bool bot_weapon_profiles_initialized();
const std::vector<BotWeaponProfile>& bot_weapon_profiles();
const BotWeaponProfile* bot_weapon_profile_for_weapon_type(int weapon_type);
bool bot_weapon_profile_supports_range(const BotWeaponProfile& profile, BotWeaponRangeBand range);
bool bot_weapon_profile_has_special(const BotWeaponProfile& profile, BotWeaponSpecialProperty property);
int bot_weapon_profile_range_band_count(const BotWeaponProfile& profile);

