#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../rf/math/vector.h"
#include "../rf/player/control_config.h"

namespace rf
{
    struct Entity;
    struct Player;
}

// Riding Rope Emitters flagged as zip lines.

void zipline_level_init();
// From rf_do_frame, ahead of the stock frame: ride bookkeeping, hint, effects, server relay.
void zipline_do_frame();

// Air move for a rider this machine simulates. True when it set the velocity and stock air move must
// not run.
bool zipline_air_move(rf::Entity* ep);
// True while `ep` is the local player riding a line.
bool zipline_local_is_riding(const rf::Entity* ep);
// True while this machine simulates `ep` riding a line: the local player, or a server bot on the server.
bool zipline_entity_simulates_ride(const rf::Entity* ep);
// Riding, or just let go: slam and impact damage is not taken.
bool zipline_slam_immune(const rf::Entity* ep);
// Use grabs a line; Jump and Crouch let go of one. True when the action was consumed.
bool zipline_execute_action(rf::Player* player, rf::ControlConfigAction action);

// True for any entity riding a line (local or remote, and on a dedicated server a claimed rider still
// on the line); riders are posed in freefall.
bool zipline_entity_is_riding(const rf::Entity* ep);
void zipline_render_attachment(rf::Entity* ep);
void zipline_render_first_person();
// The grab or ride hint for the line under the reticle, or empty when none is up.
const std::string& zipline_hint_text();

void zipline_server_on_state_request(rf::Player* player, int32_t rope_uid, bool riding, int8_t dir);
void zipline_on_remote_state(uint8_t player_id, int32_t rope_uid, bool riding, int8_t dir);
// Catches a joining client up on everyone currently riding.
void zipline_send_state_to(rf::Player* player);

// A ride a bot can plan: one rideable direction of a line, switched on or not, whose whole ride is clear.
struct ZiplineBotRoute
{
    int32_t rope_uid = -1;
    int dir = 1; // +1 toward the target end
    // Points on the line a ride can start from, nearest the start anchor first.
    std::vector<rf::Vector3> grab_points;
    // Where a rider let go at the far end comes down.
    rf::Vector3 landing_pos{};
};

void zipline_collect_bot_routes(std::vector<ZiplineBotRoute>& out);
// Whether a bot standing at stand_pos (its entity position) can take hold of line_point, if need be at
// the top of a jump.
bool zipline_bot_reaches_line(const rf::Vector3& stand_pos, const rf::Vector3& line_point, bool jump);
// Whether a bot standing at stand_pos can take line rope_uid riding dir right now: switched on, and still
// within its reach. ride_seconds is how long the ride from there lasts.
bool zipline_bot_ride_open(int32_t rope_uid, int dir, const rf::Vector3& stand_pos, float* ride_seconds);
// For a bot client's own entity (-bot or -debugbot), or a server bot on the server: takes line rope_uid
// riding dir when it is within reach. False when it is not.
bool zipline_bot_grab(rf::Entity* ep, int32_t rope_uid, int dir);
// Whether the ride `ep` was last let go from carried it to the far end, rather than ending early.
bool zipline_last_ride_reached_end(const rf::Entity* ep);
