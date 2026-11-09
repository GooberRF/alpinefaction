#pragma once

#include "../../rf/math/vector.h"

namespace rf
{
    struct Entity;
    struct Player;
}

struct BotContext;
struct BotVehicleState;
enum class BotVehicleExitReason;

// A fighter or a sub: the hulls the flight law flies.
bool bot_vehicle_flight_hull(const rf::Entity& hull);

// A ride toward the objective in a fighter or sub: to the landing node of its layer near the objective that
// lets its riders out, over the vehicle graph. Fills the flight's length and the straight walk left from the exit.
bool bot_vehicle_flight_transport_estimate(
    const rf::Entity& hull, const rf::Vector3& objective, float& out_flight_dist, float& out_exit_walk);
// Anything for a fighter or sub to hunt: a fighter on a level whose vehicle layer has air nodes (patrol, air
// support), else only with a live enemy a clear straight line from the hull reaches now; a sub only with an enemy in
// the water.
bool bot_vehicle_flight_hunt_worthwhile(const rf::Player& pp, const rf::Entity& hull);

// A seated pilot of a fighter or sub, each think of the active bot: where to go, the route there over its layer,
// combat, avoidance, the progress monitor, and setting down to get out. Returns why the pilot wants out, or none.
BotVehicleExitReason bot_vehicle_flight_think(
    BotContext& ctx, const rf::Player& pp, const rf::Entity& rider, const rf::Entity& hull, const rf::Entity* enemy,
    bool enemy_in_sight);
// Whether the pilot may get out now: set down (fighter), surfaced (sub), or a hull that cannot wait for it.
bool bot_vehicle_flight_exit_ready(const BotVehicleState& vehicle);

// The flight law, every frame, from the last think's plan: writes the pilot's move, pitch and yaw intent. No traces.
void bot_vehicle_flight_step(BotVehicleState& vehicle, const rf::Entity& hull, float dt);
