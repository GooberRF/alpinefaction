#pragma once

#include "../../rf/math/vector.h"
#include <limits>

namespace rf
{
    struct Entity;
    struct Object;
    struct Player;
}

struct BotContext;
enum class BotGoalType;
enum class BotVehicleExitReason;

// The frame of a bot seated in a synced hull (a dying one included), run in place of the whole foot frame.
void bot_vehicle_run_seated_frame(BotContext& ctx, rf::Player& player, rf::Entity& rider, rf::Entity& hull);
// Every on-foot frame: the first one after an exit drops the seat and re-arms the foot watchdogs.
void bot_vehicle_note_on_foot(BotContext& ctx);

// Server bots, every frame while seated: seat aim, a driver's hull controls and aim, and the hull trigger edges.
void bot_vehicle_server_apply_outputs(BotContext& ctx, rf::Player& pp, rf::Entity& rider, rf::Entity& hull);
// Server bots that stop authoring (dying, idle): triggers released, and the hull they drive gets zero input and no aim.
void bot_vehicle_server_stop_outputs(BotContext& ctx, rf::Player& pp);

struct BotVehicleGoalContext
{
    bool enemy_contact = false;
    // Boarding would drop it, or take it out of the other team's reach; only the jeep carries a CTF flag.
    bool carrying_objective = false;
    bool carrying_ctf_flag = false;
    // Boarding keeps the bag, but a fighter or sub takes it where its holder cannot hold a spot.
    bool carrying_bag = false;
    // The best tier-1 objective this evaluation found, and its score.
    bool objective_valid = false;
    rf::Vector3 objective_pos{};
    float objective_score = -std::numeric_limits<float>::infinity();
};

struct BotVehicleGoalCandidate
{
    BotGoalType goal{};
    int hull_handle = -1;
    int seat = -1;
    int waypoint = 0;
    rf::Vector3 pos{};
    float score = -std::numeric_limits<float>::infinity();
};

// The best seat for the active bot to walk to: a turret near an objective, a team-mate's jeep gun, or a ride.
bool bot_vehicle_find_goal_candidate(
    const rf::Player& pp, const rf::Entity& local_entity, const BotVehicleGoalContext& context,
    BotVehicleGoalCandidate& out);
// The active vehicle goal's seat is still there to be taken.
bool bot_vehicle_goal_still_valid(const rf::Player& pp);
// The active vehicle goal is a jeep's wheel, the one seat a CTF flag carrier boards.
bool bot_vehicle_active_goal_carries_flag();
// Enemy contact now or within the last 20 s, as turret candidates need it.
bool bot_vehicle_enemy_contact(bool contact_now);
// dbg_bot: one line when the active bot picks or drops a vehicle goal; called before the new goal is set.
void bot_vehicle_dbg_goal_change(BotGoalType goal, int hull_handle, int seat, float score);

enum class BotVehicleAcquireStatus
{
    abort,
    approach,
    boarding,
};

struct BotVehicleApproach
{
    int board_waypoint = 0;
    rf::Vector3 board_pos{};
    rf::Vector3 hull_pos{};
    const rf::Object* hull = nullptr;
    const char* abort_reason = "unknown";
};

// On foot, each think of a vehicle goal: asks for the seat once in range, retrying, and puts it on cooldown when it
// is refused; otherwise says where to walk.
BotVehicleAcquireStatus bot_vehicle_update_acquire(const rf::Entity& local_entity, BotVehicleApproach& out);
// Puts the active vehicle goal's seat on cooldown and falls back to roaming; the reason is logged under dbg_bot.
// Always false.
bool bot_vehicle_abort_acquire(const char* reason);
// Any other path that gives up the active goal (the foot watchdogs): when it is a vehicle goal, its seat goes on
// cooldown like an aborted one, so the next evaluation does not pick it straight back.
void bot_vehicle_note_goal_abandoned(const char* reason);
// The active vehicle goal's hull stands parked, so its seat waits where it was scored.
bool bot_vehicle_goal_hull_parked();
// Standing in boarding range of the active vehicle goal's hull, waiting to be seated.
bool bot_vehicle_is_boarding(const rf::Entity& local_entity);

// That player's live entity while he stands on foot (neither seated nor dying), else null.
const rf::Entity* bot_vehicle_on_foot_entity(const rf::Player& player);
// What to shoot at for this entity: the hull of a rider in an enclosed hull, else the entity itself.
rf::Entity* bot_vehicle_effective_target(rf::Entity* entity);
// A synced hull with no enemy of pp aboard, which is no target.
bool bot_vehicle_hull_without_enemy(const rf::Entity& entity, const rf::Player& pp);
// A hull's, or a rider's hull's, velocity; the entity's own otherwise.
rf::Vector3 bot_vehicle_target_velocity(const rf::Entity& entity);
// Synced hulls only.
float bot_vehicle_health_ratio(const rf::Entity& hull);
// Low enough that its riders should get out while they can.
bool bot_vehicle_hull_failing(const rf::Entity& hull);
bool bot_vehicle_is_enemy(const rf::Player& pp, const rf::Player& other);
float bot_vehicle_hull_hit_radius(const rf::Entity& hull);
// How far the hull's mounted gun is worth firing at the target; farther at a hull.
float bot_vehicle_mounted_weapon_range(const rf::Entity& hull, const rf::Entity& target);
// A team-mate of pp (a human one, when humans_only) stands on foot within radius of pos. Never in a non-team game.
bool bot_vehicle_team_mate_on_foot_within(const rf::Player& pp, const rf::Vector3& pos, float radius, bool humans_only);
const char* bot_vehicle_exit_reason_name(BotVehicleExitReason reason);

// An enemy-driven hull closing on a bot on foot: what to be aware of, and, when the bot stands in its path, which
// way to step aside.
struct BotVehicleHullThreat
{
    rf::Entity* target = nullptr;
    bool in_path = false;
    rf::Vector3 dodge_dir{};
};
bool bot_vehicle_find_hull_threat(const rf::Player& pp, const rf::Entity& local_entity, BotVehicleHullThreat& out);
// A foot node there is next to a vehicle road, which routes keep off while an enemy hull was recently closing on the
// bot. Depends only on the ground links of the AWP's vehicle layer.
bool bot_vehicle_foot_node_near_road(const rf::Vector3& pos);
