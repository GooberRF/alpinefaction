#pragma once

#include "bot_vehicle_nav.h"
#include "../../rf/math/vector.h"
#include <vector>

namespace rf
{
    struct Entity;
    struct Player;
}

struct BotContext;
struct BotVehicleDriveState;
struct BotVehicleState;
enum class BotVehicleDriveMode;
enum class BotVehicleExitReason;

// A seated driver of a ground hull, each think of the active bot: where to go, the route there, avoidance and the
// progress monitor. Returns why the driver wants out, or none.
BotVehicleExitReason bot_vehicle_drive_think(
    BotContext& ctx, const rf::Player& pp, const rf::Entity& rider, const rf::Entity& hull, const rf::Entity* enemy,
    bool enemy_in_sight);

// The car law, every frame, from the last think's plan: writes the driver's move and yaw intent. No traces.
void bot_vehicle_drive_step(BotVehicleState& vehicle, const rf::Entity& hull, float dt);

// Shared with the flight think. A new mode drops the route; the same one keeps it, to repath when dest moved enough.
void bot_vehicle_drive_set_destination(
    BotVehicleDriveState& drive, BotVehicleDriveMode mode, const rf::Vector3& pos, bool stop_at_dest);
std::vector<BotVehicleNavLink> bot_vehicle_drive_excluded_links(const BotVehicleDriveState& drive);
const char* bot_vehicle_drive_mode_name(BotVehicleDriveMode mode);
// Whether a driver of this ground hull would find a transport route to the objective (the one its think plans, and
// worth taking). Answers are shared between bots by hull, objective and hull position for a few seconds.
bool bot_vehicle_drive_transport_route_exists(const rf::Entity& hull, const rf::Vector3& objective);

// Route following shared by the car and flight laws: a ground route is measured flat and skipped ahead along a
// hull-wide drive corridor, a flight route in 3D along a flight corridor.
struct BotVehicleRouteSample
{
    rf::Vector3 lookahead{};
    // Route length left from pos, and the turn at the first vertex past the lookahead point (0 when none is near).
    float remaining = 0.0f;
    float turn_ahead = 0.0f;
};
BotVehicleRouteSample bot_vehicle_drive_sample_route(
    const BotVehicleDriveState& drive, const rf::Vector3& pos, float lookahead, bool flat);
void bot_vehicle_drive_advance_route(
    BotVehicleDriveState& drive, const rf::Vector3& pos, const BotVehicleNavClass& cls, float pass_radius);
// Once a second, or at once without a destination; restarts the plan timer when due.
bool bot_vehicle_drive_plan_due(BotVehicleDriveState& drive);
// The repath timer ran out (5 s after a plan, or after a failed one with a route still in hand; 1 s after a failed one
// without), or the destination moved enough while a route is in hand and the last plan is min_interval_ms old.
bool bot_vehicle_drive_repath_due(
    const BotVehicleDriveState& drive, const rf::Vector3& pos, bool flat, int min_interval_ms);
// After a plan found no route: true once the plans failed often enough to give the destination up.
bool bot_vehicle_drive_plan_failed(BotVehicleDriveState& drive);
// A hunt waits and a patrol picks another point; any other destination leaves no way on.
BotVehicleExitReason bot_vehicle_drive_give_up_route(BotVehicleDriveState& drive);
void bot_vehicle_drive_commit_route(BotVehicleDriveState& drive, BotVehicleRoute&& route);
// The hunt gives up its target for a while and the hull patrols.
void bot_vehicle_drive_block_hunt(BotVehicleDriveState& drive);

enum class BotVehicleProgress
{
    opened,
    open,
    progressing,
    stalled,
};
// The progress monitor's window: stalled when a whole window gained less than min_distance, net, on the route left or
// along the way the route led from where the window opened (toward: a point ahead on the route then).
BotVehicleProgress bot_vehicle_drive_progress_window(
    BotVehicleDriveState& drive, const rf::Vector3& pos, float remaining, const rf::Vector3& toward,
    float min_distance);
// A stalled hull its law could not free: one repath without the link it is on, then stuck.
BotVehicleExitReason bot_vehicle_drive_repath_or_stuck(BotVehicleDriveState& drive);
