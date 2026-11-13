#pragma once

// Stock weapons.tbl / entity.tbl values, generated from the authoritative tbls in
// research/rf_decomp/tables/ by applying the parser defaulting rules of 0x004C2C80
// and 0x0041B910. Only multiplayer-effective values are stored.
namespace afstats::tbl {

struct StockWeapon
{
    const char* name;
    float collision_radius;
    int max_ammo;
    int clip_size;
    int num_projectiles;
    float damage;
    float alt_damage;
    float spread;
    float alt_spread;
    float damage_radius;
    float crater_radius;
    float bbox_factor;
    bool burst;
    bool burst_alt;
    int burst_count;
    float burst_delay;
    bool piercing;
    float piercing_power;
    float ricochet_deg;
    float ricochet_cos;
};

struct StockSphere
{
    const char* name;
    float damage_factor_multi;
};

struct StockEntity
{
    const char* name;
    const StockSphere* spheres;
    int num_spheres;
};

extern const StockWeapon g_stock_weapons[];
extern const int g_num_stock_weapons;
extern const StockEntity g_stock_entities[];
extern const int g_num_stock_entities;

} // namespace afstats::tbl
