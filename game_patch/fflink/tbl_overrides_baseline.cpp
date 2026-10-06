#include <iterator>
#include "tbl_overrides_baseline.h"

namespace afstats::tbl {

const StockWeapon g_stock_weapons[] = {
    {"Remote Charge", 0.051000003f, 21, 0, 1, 350.0f, 350.0f, 0.0f, 0.0f, 8.0f, 5.0f, 1.0f, false, false, 0, 0.0f, false, 0.0f, 180.0f, 0.0f},
    {"Remote Charge Detonator", 0.051000003f, 21, 0, 1, 200.0f, 200.0f, 0.0f, 0.0f, 5.0f, 5.0f, 1.0f, false, false, 0, 0.0f, false, 0.0f, 180.0f, 0.0f},
    {"Riot Stick", 0.25f, 900, 100, 1, 60.0f, 80.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, false, false, 0, 0.0f, false, 0.0f, 180.0f, 0.0f},
    {"12mm handgun", 0.020000001f, 210, 16, 1, 25.0f, 25.0f, 0.5f, 0.0f, 0.0f, 0.0f, 1.0f, false, false, 0, 0.0f, false, 0.0f, 180.0f, 0.0f},
    {"Undercover 12mm handgun", 0.020000001f, 210, 16, 1, 20.0f, 20.0f, 0.5f, 0.0f, 0.0f, 0.0f, 1.0f, false, false, 0, 0.0f, false, 0.0f, 180.0f, 0.0f},
    {"Shotgun", 0.020000001f, 48, 8, 4, 35.0f, 35.0f, 2.75f, 4.0f, 0.0f, 0.0f, 1.0f, false, false, 0, 0.0f, false, 0.0f, 180.0f, 0.0f},
    {"Sniper Rifle", 0.020000001f, 36, 6, 1, 150.0f, 150.0f, 0.25f, 0.0f, 0.0f, 0.0f, 1.0f, false, false, 0, 0.0f, true, 1.0f, 45.0f, 0.70710677f},
    {"Rocket Launcher", 0.051000003f, 18, 6, 1, 275.0f, 275.0f, 0.0f, 0.0f, 5.0f, 5.0f, 1.0f, false, false, 0, 0.0f, false, 0.0f, 180.0f, 0.0f},
    {"Assault Rifle", 0.020000001f, 210, 42, 1, 40.0f, 40.0f, 0.25f, 0.75f, 0.0f, 0.0f, 1.0f, true, false, 3, 0.1f, true, 0.25f, 45.0f, 0.70710677f},
    {"Machine Pistol", 0.020000001f, 192, 64, 1, 30.0f, 30.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, false, false, 0, 0.0f, false, 0.0f, 180.0f, 0.0f},
    {"Machine Pistol Special", 0.020000001f, 210, 21, 1, 50.0f, 50.0f, 3.0f, 0.0f, 0.0f, 0.0f, 1.0f, false, false, 0, 0.0f, true, 0.25f, 45.0f, 0.70710677f},
    {"Grenade", 0.15f, 9, 0, 1, 250.0f, 250.0f, 0.0f, 0.0f, 12.0f, 5.0f, 1.0f, false, false, 0, 0.0f, false, 0.0f, 180.0f, 0.0f},
    {"Flamethrower", 0.051000003f, 1000, 200, 1, 60.0f, 150.0f, 0.0f, 0.0f, 7.0f, 0.0f, 1.0f, false, false, 0, 0.0f, false, 0.0f, 180.0f, 0.0f},
    {"riot shield", 0.25f, 0, 32, 1, 60.0f, 60.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, false, false, 0, 0.0f, false, 0.0f, 180.0f, 0.0f},
    {"rail_gun", 0.020000001f, 6, 1, 1, 587.0f, 587.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.75f, false, false, 0, 0.0f, true, 100.0f, 180.0f, -1.0f},
    {"heavy_machine_gun", 0.020000001f, 297, 99, 1, 45.0f, 45.0f, 1.25f, 0.25f, 0.0f, 0.0f, 1.0f, false, false, 0, 0.0f, true, 0.25f, 45.0f, 0.70710677f},
    {"scope_assault_rifle", 0.020000001f, 80, 20, 1, 60.0f, 60.0f, 0.25f, 0.0f, 0.0f, 0.0f, 1.0f, false, false, 0, 0.0f, true, 0.25f, 45.0f, 0.70710677f},
    {"shoulder_cannon", 0.051000003f, 1, 1, 1, 1500.0f, 1500.0f, 0.0f, 0.0f, 30.0f, 7.0f, 1.0f, false, false, 0, 0.0f, false, 0.0f, 180.0f, 0.0f},
    {"Vauss", 0.020000001f, 170, 0, 1, 100.0f, 100.0f, 1.5f, 0.0f, 0.0f, 0.0f, 1.0f, false, false, 0, 0.0f, true, 0.1f, 45.0f, 0.70710677f},
    {"Tankbot Chaingun", 0.020000001f, 170, 0, 1, 100.0f, 100.0f, 1.5f, 0.0f, 0.0f, 0.0f, 1.0f, true, false, 16, 0.1f, true, 0.1f, 45.0f, 0.70710677f},
    {"TriBeam Laser", 0.2f, 200, 0, 1, 225.0f, 225.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, false, false, 0, 0.0f, false, 0.0f, 180.0f, 0.0f},
    {"Laser", 0.2f, 200, 0, 1, 80.0f, 80.0f, 2.5f, 0.0f, 0.0f, 0.0f, 1.0f, false, false, 0, 0.0f, false, 0.0f, 180.0f, 0.0f},
    {"Capek Cane", 0.5f, 200, 0, 1, 225.0f, 225.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, false, false, 0, 0.0f, false, 0.0f, 180.0f, 0.0f},
    {"Reeper Claw", 0.5f, 200, 0, 1, 40.0f, 40.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, false, false, 0, 0.0f, false, 0.0f, 180.0f, 0.0f},
    {"Baby Reeper Claw", 0.5f, 100, 0, 1, 10.0f, 10.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, false, false, 0, 0.0f, false, 0.0f, 180.0f, 0.0f},
    {"Rock Snake Smash", 1.0f, 200, 0, 1, 1600.0f, 1600.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, false, false, 0, 0.0f, false, 0.0f, 180.0f, 0.0f},
    {"Rock Snake Spit", 0.5f, 200, 0, 1, 80.0f, 80.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, false, false, 0, 0.0f, false, 0.0f, 180.0f, 0.0f},
    {"Big Rock Snake Smash", 2.0f, 200, 0, 1, 1600.0f, 1600.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, false, false, 0, 0.0f, false, 0.0f, 180.0f, 0.0f},
    {"Big Rock Snake Spit", 0.5f, 200, 0, 1, 80.0f, 80.0f, 0.0f, 0.0f, 3.0f, 0.0f, 1.0f, false, false, 0, 0.0f, false, 0.0f, 180.0f, 0.0f},
    {"Sea Creature Sonar Attack", 0.5f, 200, 0, 1, 80.0f, 80.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, false, false, 0, 0.0f, false, 0.0f, 180.0f, 0.0f},
    {"Drone Smash", 0.5f, 200, 0, 1, 1600.0f, 1600.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, false, false, 0, 0.0f, false, 0.0f, 180.0f, 0.0f},
    {"Tankbot Smash", 0.5f, 200, 0, 1, 1200.0f, 1200.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, false, false, 0, 0.0f, false, 0.0f, 180.0f, 0.0f},
    {"Mutant Attack 1", 0.5f, 200, 0, 1, 40.0f, 40.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, false, false, 0, 0.0f, false, 0.0f, 180.0f, 0.0f},
    {"Mutant Attack 2", 0.5f, 200, 0, 1, 40.0f, 40.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, false, false, 0, 0.0f, false, 0.0f, 180.0f, 0.0f},
    {"HEAP", 0.020000001f, 20, 0, 1, 250.0f, 250.0f, 0.0f, 0.0f, 5.0f, 4.0f, 1.0f, false, false, 0, 0.0f, false, 0.0f, 180.0f, 0.0f},
    {"Torpedo", 0.15f, 20, 0, 1, 200.0f, 200.0f, 0.0f, 0.0f, 5.0f, 5.0f, 1.0f, false, false, 0, 0.0f, false, 0.0f, 180.0f, 0.0f},
    {"APC Minigun", 0.020000001f, 999, 0, 1, 100.0f, 100.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, false, false, 0, 0.0f, false, 0.0f, 180.0f, 0.0f},
    {"Jeep Gun", 0.020000001f, 999, 0, 1, 100.0f, 100.0f, 0.5f, 0.0f, 0.0f, 0.0f, 1.0f, false, false, 0, 0.0f, false, 0.0f, 180.0f, 0.0f},
    {"Fighter Minigun", 0.020000001f, 900, 0, 1, 100.0f, 100.0f, 1.5f, 0.0f, 0.0f, 0.0f, 1.0f, false, false, 0, 0.0f, false, 0.0f, 180.0f, 0.0f},
    {"Drill", 0.2f, 0, 0, 1, 30.0f, 50.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, false, false, 0, 0.0f, false, 0.0f, 180.0f, 0.0f},
    {"Drone Missile", 0.15f, 12, 0, 1, 15.0f, 15.0f, 0.0f, 0.0f, 2.0f, 5.0f, 1.0f, false, false, 0, 0.0f, false, 0.0f, 180.0f, 0.0f},
    {"Tankbot Missile", 0.15f, 36, 0, 1, 25.0f, 25.0f, 0.0f, 0.0f, 3.0f, 5.0f, 1.0f, false, false, 0, 0.0f, false, 0.0f, 180.0f, 0.0f},
    {"Fighter Rocket", 0.15f, 20, 0, 1, 200.0f, 200.0f, 0.0f, 0.0f, 15.0f, 8.0f, 1.0f, false, false, 0, 0.0f, false, 0.0f, 180.0f, 0.0f},
    {"APC Rocket", 0.15f, 15, 0, 1, 300.0f, 300.0f, 0.0f, 0.0f, 10.0f, 8.0f, 1.0f, false, false, 0, 0.0f, false, 0.0f, 180.0f, 0.0f},
};
const int g_num_stock_weapons = static_cast<int>(std::size(g_stock_weapons));

static const StockSphere g_spheres_comp_tech[] = {{"csphere_0", 0.5f}, {"csphere_1", 1.0f}, {"csphere_2", 2.0f},};
static const StockSphere g_spheres_eos[] = {{"csphere_0", 0.5f}, {"csphere_1", 1.0f}, {"csphere_2", 2.0f},};
static const StockSphere g_spheres_admin_male[] = {{"csphere_0", 0.5f}, {"csphere_1", 1.0f}, {"csphere_2", 2.0f},};
static const StockSphere g_spheres_admin_male2[] = {{"csphere_0", 0.5f}, {"csphere_1", 1.0f}, {"csphere_2", 2.0f},};
static const StockSphere g_spheres_miner1[] = {{"csphere_0", 0.5f}, {"csphere_1", 1.0f}, {"csphere_2", 2.0f},};
static const StockSphere g_spheres_medic1[] = {{"csphere_0", 0.5f}, {"csphere_1", 1.0f}, {"csphere_2", 2.0f},};
static const StockSphere g_spheres_tech1[] = {{"csphere_0", 0.5f}, {"csphere_1", 1.0f}, {"csphere_2", 2.0f},};
static const StockSphere g_spheres_admin_fem[] = {{"csphere_0", 0.5f}, {"csphere_1", 1.0f}, {"csphere_2", 2.0f},};
static const StockSphere g_spheres_env_scientist[] = {{"csphere_0", 0.5f}, {"csphere_1", 1.0f}, {"csphere_2", 2.0f},};
static const StockSphere g_spheres_ult_scientist[] = {{"csphere_0", 0.5f}, {"csphere_1", 1.0f}, {"csphere_2", 2.0f},};
static const StockSphere g_spheres_miner3[] = {{"csphere_0", 0.5f}, {"csphere_1", 1.0f}, {"csphere_2", 2.0f},};
static const StockSphere g_spheres_nurse1[] = {{"csphere_0", 0.5f}, {"csphere_1", 1.0f}, {"csphere_2", 2.0f},};
static const StockSphere g_spheres_parker_suit[] = {{"csphere_0", 0.5f}, {"csphere_1", 1.0f}, {"csphere_2", 2.0f},};
static const StockSphere g_spheres_parker_sci[] = {{"csphere_0", 0.5f}, {"csphere_1", 1.0f}, {"csphere_2", 2.0f},};
static const StockSphere g_spheres_multi_guard2[] = {{"csphere_0", 0.5f}, {"csphere_1", 1.0f}, {"csphere_2", 2.0f},};
static const StockSphere g_spheres_elite[] = {{"csphere_0", 0.5f}, {"csphere_1", 1.0f}, {"csphere_2", 2.0f},};
static const StockSphere g_spheres_env_guard[] = {{"csphere_0", 0.5f}, {"csphere_1", 1.0f}, {"csphere_2", 2.0f},};
static const StockSphere g_spheres_riot_guard[] = {{"csphere_0", 0.5f}, {"csphere_1", 1.0f}, {"csphere_2", 2.0f},};
static const StockSphere g_spheres_merc_grunt[] = {{"csphere_0", 0.5f}, {"csphere_1", 1.0f}, {"csphere_2", 2.0f},};
static const StockSphere g_spheres_masako[] = {{"csphere_0", 0.5f}, {"csphere_1", 1.0f}, {"csphere_2", 2.0f},};

const StockEntity g_stock_entities[] = {
    {"comp_tech", g_spheres_comp_tech, 3},
    {"eos", g_spheres_eos, 3},
    {"admin_male", g_spheres_admin_male, 3},
    {"admin_male2", g_spheres_admin_male2, 3},
    {"miner1", g_spheres_miner1, 3},
    {"medic1", g_spheres_medic1, 3},
    {"tech1", g_spheres_tech1, 3},
    {"admin_fem", g_spheres_admin_fem, 3},
    {"env_scientist", g_spheres_env_scientist, 3},
    {"ult_scientist", g_spheres_ult_scientist, 3},
    {"miner3", g_spheres_miner3, 3},
    {"nurse1", g_spheres_nurse1, 3},
    {"parker_suit", g_spheres_parker_suit, 3},
    {"parker_sci", g_spheres_parker_sci, 3},
    {"multi_guard2", g_spheres_multi_guard2, 3},
    {"elite", g_spheres_elite, 3},
    {"env_guard", g_spheres_env_guard, 3},
    {"riot_guard", g_spheres_riot_guard, 3},
    {"merc_grunt", g_spheres_merc_grunt, 3},
    {"masako", g_spheres_masako, 3},
};
const int g_num_stock_entities = static_cast<int>(std::size(g_stock_entities));

} // namespace afstats::tbl
