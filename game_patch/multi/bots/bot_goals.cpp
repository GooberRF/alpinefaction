#include "bot_goals.h"

#include "bot_combat.h"
#include "bot_internal.h"
#include "bot_state.h"
#include "bot_utils.h"
#include "bot_weapon_profiles.h"
#include "bot_waypoint_route.h"
#include "../gametype.h"
#include "../../main/main.h"
#include "../../rf/multi.h"
#include "../../rf/player/player.h"
#include "../../rf/weapon.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <vector>

namespace
{
bool has_any_route_to_waypoint(const rf::Entity& local_entity, const int goal_waypoint)
{
    if (goal_waypoint <= 0) {
        return false;
    }

    const int start_waypoint = bot_find_closest_waypoint_with_fallback(local_entity.pos);
    if (start_waypoint <= 0) {
        return false;
    }

    return bot_waypoint_same_component(start_waypoint, goal_waypoint);
}

bool has_committed_pickup_route()
{
    if (!bot_goal_is_item_collection(g_client_bot_state.active_goal)) {
        return false;
    }
    if (g_client_bot_state.recovery_pending_reroute) {
        return false;
    }
    if (g_client_bot_state.goal_target_waypoint <= 0
        || g_client_bot_state.waypoint_goal != g_client_bot_state.goal_target_waypoint) {
        return false;
    }
    if (g_client_bot_state.waypoint_path.size() < 2) {
        return false;
    }
    if (g_client_bot_state.waypoint_next_index <= 0
        || g_client_bot_state.waypoint_next_index
            >= static_cast<int>(g_client_bot_state.waypoint_path.size())) {
        return false;
    }
    return g_client_bot_state.has_waypoint_target;
}

float compute_goal_score_tie_epsilon(const float score)
{
    return std::max(0.35f, std::abs(score) * 0.0025f);
}

bool goal_score_wins_with_tie_break(const float candidate_score, const float incumbent_score)
{
    if (!std::isfinite(candidate_score)) {
        return false;
    }
    if (!std::isfinite(incumbent_score)) {
        return true;
    }

    const float tie_epsilon = std::max(
        compute_goal_score_tie_epsilon(candidate_score),
        compute_goal_score_tie_epsilon(incumbent_score)
    );
    if (candidate_score > incumbent_score + tie_epsilon) {
        return true;
    }
    if (std::abs(candidate_score - incumbent_score) <= tie_epsilon) {
        // Deterministic tie handling: keep incumbent on ties/near-ties to avoid
        // oscillating between equally appealing alternatives.
        return false;
    }
    return false;
}

bool find_nearest_waypoint_of_type(
    const rf::Vector3& from_pos,
    const WaypointType waypoint_type,
    const int subtype_filter,
    int& out_waypoint,
    rf::Vector3& out_waypoint_pos)
{
    out_waypoint = 0;
    out_waypoint_pos = {};
    float best_dist_sq = std::numeric_limits<float>::infinity();
    bool found = false;

    const auto& type_waypoints = waypoints_get_by_type(waypoint_type);
    rf::Vector3 waypoint_pos{};
    for (const int waypoint_uid : type_waypoints) {
        int type_raw = 0;
        int subtype = 0;
        if (subtype_filter >= 0
            && (!waypoints_get_type_subtype(waypoint_uid, type_raw, subtype)
                || subtype != subtype_filter)) {
            continue;
        }
        if (!waypoints_get_pos(waypoint_uid, waypoint_pos)) {
            continue;
        }

        const float dist_sq = rf::vec_dist_squared(&from_pos, &waypoint_pos);
        if (dist_sq < best_dist_sq) {
            best_dist_sq = dist_sq;
            out_waypoint = waypoint_uid;
            out_waypoint_pos = waypoint_pos;
            found = true;
        }
    }

    return found;
}

bool find_nearest_team_spawn_waypoint(
    const rf::Vector3& from_pos,
    const bool team_is_red,
    int& out_waypoint,
    rf::Vector3& out_waypoint_pos)
{
    const int team_subtype = team_is_red
        ? static_cast<int>(WaypointRespawnSubtype::red_team)
        : static_cast<int>(WaypointRespawnSubtype::blue_team);
    if (find_nearest_waypoint_of_type(
            from_pos,
            WaypointType::respawn,
            team_subtype,
            out_waypoint,
            out_waypoint_pos)) {
        return true;
    }
    return find_nearest_waypoint_of_type(
        from_pos,
        WaypointType::respawn,
        static_cast<int>(WaypointRespawnSubtype::all_teams),
        out_waypoint,
        out_waypoint_pos
    );
}

bool is_ctf_mode()
{
    return rf::is_multi && rf::multi_get_game_type() == rf::NG_TYPE_CTF;
}

bool is_control_point_mode()
{
    return bot_internal_is_control_point_mode();
}

HillOwner team_to_hill_owner(const int team)
{
    if (team == rf::TEAM_RED) {
        return HillOwner::HO_Red;
    }
    if (team == rf::TEAM_BLUE) {
        return HillOwner::HO_Blue;
    }
    return HillOwner::HO_Neutral;
}

HillOwner local_hill_team_owner()
{
    if (!rf::local_player) {
        return HillOwner::HO_Neutral;
    }
    return team_to_hill_owner(rf::local_player->team);
}

bool control_point_team_can_capture_hill(const HillInfo& hill, const HillOwner local_team)
{
    if (local_team != HillOwner::HO_Red && local_team != HillOwner::HO_Blue) {
        return false;
    }
    if (hill.lock_status != HillLockStatus::HLS_Available) {
        return false;
    }
    if (hill.ownership == local_team) {
        return false;
    }
    if (gt_is_esc()) {
        return esc_team_can_attack_hill(hill, local_team);
    }
    if (gt_is_rev() && local_team != HillOwner::HO_Red) {
        return false;
    }
    return true;
}

bool control_point_hill_needs_defense(const HillInfo& hill, const HillOwner local_team)
{
    if (local_team != HillOwner::HO_Red && local_team != HillOwner::HO_Blue) {
        return false;
    }
    if (hill.lock_status != HillLockStatus::HLS_Available) {
        return false;
    }
    if (hill.ownership != local_team) {
        return false;
    }
    return hill.steal_dir == opposite(local_team) && hill.capture_milli > 0;
}

rf::Vector3 resolve_control_point_objective_pos(
    const HillInfo& hill,
    const rf::Vector3& fallback_pos)
{
    if (hill.trigger) {
        return hill.trigger->pos;
    }
    if (rf::Trigger* trigger = koth_resolve_trigger_from_uid(hill.trigger_uid)) {
        return trigger->pos;
    }
    return fallback_pos;
}

bool has_alive_enemy_players(const rf::Player& local_player)
{
    const bool team_mode = multi_is_team_game_type();
    for (const rf::Player& candidate : SinglyLinkedList{rf::player_list}) {
        if (&candidate == &local_player) {
            continue;
        }
        if (candidate.is_browser || candidate.is_spectator || candidate.is_spawn_disabled) {
            continue;
        }
        if (team_mode && candidate.team == local_player.team) {
            continue;
        }
        if (rf::player_is_dead(&candidate) || rf::player_is_dying(&candidate)) {
            continue;
        }

        rf::Entity* candidate_entity = rf::entity_from_handle(candidate.entity_handle);
        if (!candidate_entity || rf::entity_is_dying(candidate_entity)) {
            continue;
        }
        return true;
    }
    return false;
}

bool is_ctf_flag_in_base(const bool red_flag)
{
    return red_flag
        ? rf::multi_ctf_is_red_flag_in_base()
        : rf::multi_ctf_is_blue_flag_in_base();
}

rf::Player* get_ctf_flag_carrier(const bool red_flag)
{
    return red_flag
        ? rf::multi_ctf_get_red_flag_player()
        : rf::multi_ctf_get_blue_flag_player();
}

rf::Vector3 get_ctf_flag_pos(const bool red_flag)
{
    rf::Vector3 pos{};
    if (red_flag) {
        rf::multi_ctf_get_red_flag_pos(&pos);
    }
    else {
        rf::multi_ctf_get_blue_flag_pos(&pos);
    }
    return pos;
}

int get_ctf_flag_uid(const bool red_flag)
{
    rf::Object* flag_obj = red_flag ? rf::ctf_red_flag_item : rf::ctf_blue_flag_item;
    return flag_obj ? flag_obj->uid : -1;
}

struct CtfRuntimeState
{
    bool valid = false;
    bool local_is_red = true;
    bool own_flag_red = true;
    bool enemy_flag_red = false;
    bool own_flag_in_base = false;
    bool enemy_flag_in_base = false;
    bool own_flag_stolen = false;
    bool own_flag_dropped = false;
    bool enemy_flag_dropped = false;
    bool local_has_enemy_flag = false;
    int own_flag_uid = -1;
    int enemy_flag_uid = -1;
    rf::Vector3 own_flag_pos{};
    rf::Vector3 enemy_flag_pos{};
    rf::Player* own_flag_carrier = nullptr;
    rf::Player* enemy_flag_carrier = nullptr;
    rf::Entity* own_flag_carrier_entity = nullptr;
    int own_base_waypoint = 0;
    rf::Vector3 own_base_pos{};
    int enemy_base_waypoint = 0;
    rf::Vector3 enemy_base_pos{};
    int own_dropped_waypoint = 0;
    int enemy_dropped_waypoint = 0;
    int own_spawn_waypoint = 0;
    rf::Vector3 own_spawn_pos{};
};

struct CtfGoalCandidate
{
    BotGoalType goal = BotGoalType::none;
    int handle = -1;
    int identifier = -1;
    int waypoint = 0;
    rf::Vector3 pos{};
    float score = -std::numeric_limits<float>::infinity();
};

struct ControlPointGoalCandidate
{
    BotGoalType goal = BotGoalType::none;
    int identifier = -1;
    int waypoint = 0;
    rf::Vector3 pos{};
    float score = -std::numeric_limits<float>::infinity();
    bool defense = false;
};

bool build_ctf_runtime_state(
    const rf::Entity& local_entity,
    CtfRuntimeState& out_state)
{
    out_state = {};
    if (!is_ctf_mode() || !rf::local_player) {
        return false;
    }

    out_state.valid = true;
    out_state.local_is_red = rf::local_player->team == rf::TEAM_RED;
    out_state.own_flag_red = out_state.local_is_red;
    out_state.enemy_flag_red = !out_state.local_is_red;

    out_state.own_flag_in_base = is_ctf_flag_in_base(out_state.own_flag_red);
    out_state.enemy_flag_in_base = is_ctf_flag_in_base(out_state.enemy_flag_red);
    out_state.own_flag_carrier = get_ctf_flag_carrier(out_state.own_flag_red);
    out_state.enemy_flag_carrier = get_ctf_flag_carrier(out_state.enemy_flag_red);
    out_state.own_flag_pos = get_ctf_flag_pos(out_state.own_flag_red);
    out_state.enemy_flag_pos = get_ctf_flag_pos(out_state.enemy_flag_red);
    out_state.own_flag_uid = get_ctf_flag_uid(out_state.own_flag_red);
    out_state.enemy_flag_uid = get_ctf_flag_uid(out_state.enemy_flag_red);

    out_state.own_flag_stolen = !out_state.own_flag_in_base && out_state.own_flag_carrier != nullptr;
    out_state.own_flag_dropped = !out_state.own_flag_in_base && out_state.own_flag_carrier == nullptr;
    out_state.enemy_flag_dropped = !out_state.enemy_flag_in_base && out_state.enemy_flag_carrier == nullptr;
    out_state.local_has_enemy_flag = out_state.enemy_flag_carrier == rf::local_player;

    int dropped_waypoint = 0;
    rf::Vector3 dropped_pos{};
    if (waypoints_find_dropped_ctf_flag_waypoint(
            out_state.own_flag_red,
            dropped_waypoint,
            dropped_pos)) {
        out_state.own_flag_dropped = true;
        out_state.own_flag_in_base = false;
        out_state.own_flag_stolen = false;
        out_state.own_flag_carrier = nullptr;
        out_state.own_flag_carrier_entity = nullptr;
        out_state.own_flag_pos = dropped_pos;
        out_state.own_dropped_waypoint = dropped_waypoint;
    }
    if (!out_state.local_has_enemy_flag
        && out_state.enemy_flag_carrier == nullptr
        && waypoints_find_dropped_ctf_flag_waypoint(
            out_state.enemy_flag_red,
            dropped_waypoint,
            dropped_pos)) {
        out_state.enemy_flag_dropped = true;
        out_state.enemy_flag_in_base = false;
        out_state.enemy_flag_carrier = nullptr;
        out_state.enemy_flag_pos = dropped_pos;
        out_state.enemy_dropped_waypoint = dropped_waypoint;
    }

    if (out_state.own_flag_carrier) {
        out_state.own_flag_carrier_entity = rf::entity_from_handle(out_state.own_flag_carrier->entity_handle);
    }

    find_nearest_waypoint_of_type(
        local_entity.pos,
        WaypointType::ctf_flag,
        out_state.own_flag_red
            ? static_cast<int>(WaypointCtfFlagSubtype::red)
            : static_cast<int>(WaypointCtfFlagSubtype::blue),
        out_state.own_base_waypoint,
        out_state.own_base_pos
    );
    find_nearest_waypoint_of_type(
        local_entity.pos,
        WaypointType::ctf_flag,
        out_state.enemy_flag_red
            ? static_cast<int>(WaypointCtfFlagSubtype::red)
            : static_cast<int>(WaypointCtfFlagSubtype::blue),
        out_state.enemy_base_waypoint,
        out_state.enemy_base_pos
    );

    if (out_state.own_base_waypoint <= 0) {
        out_state.own_base_waypoint = bot_find_closest_waypoint_with_fallback(out_state.own_flag_pos);
        if (out_state.own_base_waypoint > 0) {
            waypoints_get_pos(out_state.own_base_waypoint, out_state.own_base_pos);
        }
        else {
            out_state.own_base_pos = out_state.own_flag_pos;
        }
    }
    if (out_state.enemy_base_waypoint <= 0) {
        out_state.enemy_base_waypoint = bot_find_closest_waypoint_with_fallback(out_state.enemy_flag_pos);
        if (out_state.enemy_base_waypoint > 0) {
            waypoints_get_pos(out_state.enemy_base_waypoint, out_state.enemy_base_pos);
        }
        else {
            out_state.enemy_base_pos = out_state.enemy_flag_pos;
        }
    }

    const rf::Vector3 spawn_search_origin =
        out_state.own_base_waypoint > 0 ? out_state.own_base_pos : local_entity.pos;
    find_nearest_team_spawn_waypoint(
        spawn_search_origin,
        out_state.local_is_red,
        out_state.own_spawn_waypoint,
        out_state.own_spawn_pos
    );

    return true;
}

bool find_reachable_bridge_zone_waypoint(
    const rf::Entity& local_entity,
    const int zone_uid,
    int& out_waypoint,
    rf::Vector3& out_pos,
    float* out_dist_sq = nullptr,
    const int preferred_waypoint = 0)
{
    out_waypoint = 0;
    out_pos = {};
    if (out_dist_sq) {
        *out_dist_sq = std::numeric_limits<float>::infinity();
    }

    const int start_waypoint = bot_find_closest_waypoint_with_fallback(local_entity.pos);
    if (start_waypoint <= 0) {
        return false;
    }

    struct Candidate
    {
        int waypoint = 0;
        rf::Vector3 pos{};
        float dist_sq = 0.0f;
    };

    const int waypoint_total = waypoints_count();
    std::vector<Candidate> candidates{};
    candidates.reserve(32);
    for (int waypoint = 1; waypoint < waypoint_total; ++waypoint) {
        if (!waypoints_waypoint_has_zone(waypoint, zone_uid)) {
            continue;
        }

        rf::Vector3 waypoint_pos{};
        if (!waypoints_get_pos(waypoint, waypoint_pos)) {
            continue;
        }

        candidates.push_back(Candidate{
            waypoint,
            waypoint_pos,
            rf::vec_dist_squared(&local_entity.pos, &waypoint_pos),
        });
    }

    if (candidates.empty()) {
        return false;
    }

    std::sort(
        candidates.begin(),
        candidates.end(),
        [&](const Candidate& lhs, const Candidate& rhs) {
            if (lhs.waypoint == preferred_waypoint) {
                return true;
            }
            if (rhs.waypoint == preferred_waypoint) {
                return false;
            }
            return lhs.dist_sq < rhs.dist_sq;
        });

    static const std::vector<int> kEmptyAvoidset{};
    std::vector<int> path{};
    path.reserve(64);
    constexpr int kRouteProbeLimit = 24;

    int route_checks = 0;
    for (const Candidate& candidate : candidates) {
        if (route_checks++ >= kRouteProbeLimit) {
            break;
        }

        const bool reachable = candidate.waypoint == start_waypoint
            || bot_waypoint_route(start_waypoint, candidate.waypoint, kEmptyAvoidset, path);
        if (!reachable) {
            continue;
        }

        out_waypoint = candidate.waypoint;
        out_pos = candidate.pos;
        if (out_dist_sq) {
            *out_dist_sq = candidate.dist_sq;
        }
        return true;
    }

    return false;
}

bool find_reachable_control_point_waypoint(
    const rf::Entity& local_entity,
    const int handler_uid,
    int& out_waypoint,
    rf::Vector3& out_pos,
    float* out_dist_sq = nullptr,
    const int preferred_waypoint = 0)
{
    int zone_uid = -1;
    if (!waypoints_get_control_point_zone_uid(handler_uid, zone_uid)) {
        out_waypoint = 0;
        out_pos = {};
        if (out_dist_sq) {
            *out_dist_sq = std::numeric_limits<float>::infinity();
        }
        return false;
    }

    return find_reachable_bridge_zone_waypoint(
        local_entity,
        zone_uid,
        out_waypoint,
        out_pos,
        out_dist_sq,
        preferred_waypoint);
}

bool find_reachable_explosion_target_waypoint(
    const rf::Entity& local_entity,
    const WaypointTargetDefinition& target,
    int& out_waypoint,
    rf::Vector3& out_waypoint_pos,
    float* out_dist_sq = nullptr)
{
    out_waypoint = 0;
    out_waypoint_pos = {};
    if (out_dist_sq) {
        *out_dist_sq = std::numeric_limits<float>::infinity();
    }

    const int start_waypoint = bot_find_closest_waypoint_with_fallback(local_entity.pos);
    if (start_waypoint <= 0) {
        return false;
    }

    struct Candidate
    {
        int waypoint = 0;
        rf::Vector3 pos{};
        float dist_sq = std::numeric_limits<float>::infinity();
    };

    std::vector<Candidate> candidates{};
    candidates.reserve(target.waypoint_uids.size() + 1);
    for (const int waypoint_uid : target.waypoint_uids) {
        rf::Vector3 waypoint_pos{};
        if (waypoint_uid <= 0 || !waypoints_get_pos(waypoint_uid, waypoint_pos)) {
            continue;
        }
        candidates.push_back(Candidate{
            waypoint_uid,
            waypoint_pos,
            rf::vec_dist_squared(&local_entity.pos, &waypoint_pos),
        });
    }

    if (candidates.empty()) {
        const int fallback_waypoint = bot_find_closest_waypoint_with_fallback(target.pos);
        rf::Vector3 fallback_waypoint_pos{};
        if (fallback_waypoint > 0 && waypoints_get_pos(fallback_waypoint, fallback_waypoint_pos)) {
            candidates.push_back(Candidate{
                fallback_waypoint,
                fallback_waypoint_pos,
                rf::vec_dist_squared(&local_entity.pos, &fallback_waypoint_pos),
            });
        }
    }

    if (candidates.empty()) {
        return false;
    }

    std::sort(
        candidates.begin(),
        candidates.end(),
        [](const Candidate& lhs, const Candidate& rhs) {
            return lhs.dist_sq < rhs.dist_sq;
        });

    static const std::vector<int> kEmptyAvoidset{};
    std::vector<int> path{};
    path.reserve(64);
    constexpr int kRouteProbeLimit = 16;

    int route_checks = 0;
    for (const Candidate& candidate : candidates) {
        if (route_checks++ >= kRouteProbeLimit) {
            break;
        }

        const bool reachable = candidate.waypoint == start_waypoint
            || bot_waypoint_route(start_waypoint, candidate.waypoint, kEmptyAvoidset, path);
        if (!reachable) {
            continue;
        }

        out_waypoint = candidate.waypoint;
        out_waypoint_pos = candidate.pos;
        if (out_dist_sq) {
            *out_dist_sq = candidate.dist_sq;
        }
        return true;
    }

    return false;
}

const rf::ItemInfo* resolve_item_info_for_goal(const rf::Item& item)
{
    if (item.info) {
        return item.info;
    }
    if (item.info_index >= 0 && item.info_index < rf::num_item_types) {
        return &rf::item_info[item.info_index];
    }
    return nullptr;
}

bool item_is_available_for_goal(const rf::Item& item)
{
    return (item.obj_flags & rf::OF_HIDDEN) == 0;
}

bool item_supports_crater_creation_goal(const rf::Entity& local_entity, const rf::ItemInfo& info)
{
    int crater_weapon_type = -1;
    if (info.gives_weapon_id >= 0) {
        crater_weapon_type = info.gives_weapon_id;
    }
    else if (info.ammo_for_weapon_id >= 0) {
        if (info.ammo_for_weapon_id < 0 || info.ammo_for_weapon_id >= rf::num_weapon_types) {
            return false;
        }
        if (!local_entity.ai.has_weapon[info.ammo_for_weapon_id]) {
            return false;
        }
        crater_weapon_type = info.ammo_for_weapon_id;
    }
    if (crater_weapon_type < 0) {
        return false;
    }

    const BotWeaponProfile* profile = bot_weapon_profile_for_weapon_type(crater_weapon_type);
    return profile
        && bot_weapon_profile_has_special(*profile, BotWeaponSpecialProperty::creates_craters);
}

bool find_best_crater_enabler_item_goal(
    const rf::Entity& local_entity,
    ItemGoalCandidate& out_candidate)
{
    out_candidate = {};
    float best_score = -std::numeric_limits<float>::infinity();

    for (rf::Object* obj = rf::object_list.next_obj; obj != &rf::object_list; obj = obj->next_obj) {
        if (!obj || obj->type != rf::OT_ITEM) {
            continue;
        }
        auto* item = static_cast<rf::Item*>(obj);
        if (!item || !item_is_available_for_goal(*item)) {
            continue;
        }
        const rf::ItemInfo* info = resolve_item_info_for_goal(*item);
        if (!info || (info->flags & rf::IIF_NO_PICKUP)) {
            continue;
        }
        if (!item_supports_crater_creation_goal(local_entity, *info)) {
            continue;
        }

        const int goal_waypoint = bot_find_closest_waypoint_with_fallback(item->pos);
        if (goal_waypoint <= 0) {
            continue;
        }

        const float dist = std::sqrt(std::max(
            rf::vec_dist_squared(&local_entity.pos, &item->pos),
            0.0f));
        const bool grants_weapon = info->gives_weapon_id >= 0;
        const float score = (grants_weapon ? 215.0f : 165.0f)
            - dist * 1.20f;
        if (!goal_score_wins_with_tie_break(score, best_score)) {
            continue;
        }

        best_score = score;
        out_candidate.item_handle = item->handle;
        out_candidate.item_uid = item->uid;
        out_candidate.goal_type = info->gives_weapon_id >= 0
            ? BotGoalType::collect_weapon
            : BotGoalType::collect_ammo;
        out_candidate.goal_waypoint = goal_waypoint;
        out_candidate.item_pos = item->pos;
        out_candidate.score = score;
    }

    return std::isfinite(best_score);
}
}

void bot_refresh_goal_state(
    const rf::Entity& local_entity,
    rf::Entity* enemy_target,
    const bool enemy_has_los)
{
    // Prevent multiple expensive goal refreshes within the same frame.
    if (g_client_bot_state.goal_refreshed_this_frame) {
        return;
    }
    g_client_bot_state.goal_refreshed_this_frame = true;

    const BotPersonality& personality = get_active_bot_personality();
    const float decision_skill = bot_get_decision_skill_factor();
    const float decision_efficiency = std::clamp(personality.decision_efficiency_bias, 0.35f, 2.25f);
    const float camping_bias = std::clamp(personality.camping_bias, 0.25f, 2.5f);
    const float camping_norm = std::clamp((camping_bias - 0.25f) / 2.25f, 0.0f, 1.0f);
    const float goal_commitment = std::clamp(
        personality.goal_commitment_bias * std::lerp(0.85f, 1.45f, camping_norm),
        0.35f,
        3.0f
    );
    const float eliminate_commitment = std::clamp(
        personality.eliminate_target_commitment_bias,
        0.25f,
        2.5f
    );
    const float eliminate_commitment_norm = std::clamp(
        (eliminate_commitment - 0.25f) / 2.25f,
        0.0f,
        1.0f
    );
    const float opportunism = std::clamp(personality.opportunism_bias, 0.25f, 2.5f);
    const float opportunism_norm = std::clamp((opportunism - 0.25f) / 2.25f, 0.0f, 1.0f);
    const float aggression = std::clamp(personality.decision_aggression_bias, 0.25f, 2.5f);
    const float aggression_norm = std::clamp((aggression - 0.25f) / 2.25f, 0.0f, 1.0f);
    const float raw_aggression = std::clamp(personality.raw_aggression_bias, 0.25f, 2.5f);
    const float seek_weapon_bias = std::clamp(personality.seek_weapon_bias, 0.25f, 2.5f);
    const float seek_weapon_norm = std::clamp((seek_weapon_bias - 0.25f) / 2.25f, 0.0f, 1.0f);
    const float replenish_bias = std::clamp(personality.replenish_bias, 0.25f, 2.5f);
    const float replenish_norm = std::clamp((replenish_bias - 0.25f) / 2.25f, 0.0f, 1.0f);
    const float retaliation_bias = std::clamp(personality.retaliation_bias, 0.25f, 2.5f);
    const float retaliation_norm = std::clamp((retaliation_bias - 0.25f) / 2.25f, 0.0f, 1.0f);
    const float maintenance_bias = std::clamp(
        get_active_bot_skill_profile().survivability_maintenance_bias,
        0.25f,
        2.5f
    );
    const float maintenance_norm = std::clamp((maintenance_bias - 0.25f) / 2.25f, 0.0f, 1.0f);
    const float health_ratio = std::clamp(local_entity.life / kBotNominalMaxHealth, 0.0f, 1.0f);
    const float armor_ratio = std::clamp(local_entity.armor / kBotNominalMaxArmor, 0.0f, 1.0f);
    const float health_need = std::clamp(1.0f - health_ratio, 0.0f, 1.0f);
    const float armor_need = std::clamp(1.0f - armor_ratio, 0.0f, 1.0f);
    const float survivability_need = std::clamp(
        std::max(health_need, armor_need * 0.95f),
        0.0f,
        1.0f
    );
    float maintenance_pressure = survivability_need * maintenance_norm;
    if (health_ratio > 0.75f && armor_ratio < 0.60f) {
        maintenance_pressure += 0.18f * maintenance_norm;
    }
    maintenance_pressure = std::clamp(maintenance_pressure, 0.0f, 1.0f);
    const bool deathmatch_mode = bot_internal_is_deathmatch_mode();
    const bool control_point_mode = is_control_point_mode();
    const float combat_readiness = bot_internal_compute_combat_readiness(local_entity, enemy_target);
    const float readiness_threshold = bot_internal_get_combat_readiness_threshold();
    const float readiness_delta = combat_readiness - readiness_threshold;
    const bool respawn_gearup_active =
        g_client_bot_state.respawn_gearup_timer.valid()
        && !g_client_bot_state.respawn_gearup_timer.elapsed();
    const float respawn_gearup_left_norm = respawn_gearup_active
        ? std::clamp(
            static_cast<float>(g_client_bot_state.respawn_gearup_timer.time_until())
                / static_cast<float>(kBotRespawnGearupDurationMs),
            0.0f,
            1.0f)
        : 0.0f;
    const float respawn_gearup_priority = respawn_gearup_active
        ? std::lerp(0.45f, 1.0f, respawn_gearup_left_norm)
        : 0.0f;
    const bool retaliation_active =
        g_client_bot_state.retaliation_target_handle >= 0
        && g_client_bot_state.retaliation_timer.valid()
        && !g_client_bot_state.retaliation_timer.elapsed();
    const bool retaliation_matches_enemy =
        retaliation_active
        && enemy_target
        && enemy_target->handle == g_client_bot_state.retaliation_target_handle;
    g_client_bot_state.ctf_threat_handle = enemy_target ? enemy_target->handle : -1;
    g_client_bot_state.ctf_threat_pos = enemy_target ? enemy_target->pos : rf::Vector3{};
    g_client_bot_state.ctf_threat_visible = enemy_target && enemy_has_los;
    const bool has_crater_weapon_now =
        rf::local_player
        && bot_has_usable_crater_weapon(
            *rf::local_player,
            const_cast<rf::Entity&>(local_entity));
    const bool has_shatter_weapon_now =
        rf::local_player
        && bot_has_usable_shatter_weapon(
            *rf::local_player,
            const_cast<rf::Entity&>(local_entity));
    const float kill_focus = std::clamp(personality.deathmatch_kill_focus_bias, 0.25f, 2.5f);
    const float kill_focus_norm = std::clamp((kill_focus - 0.25f) / 2.25f, 0.0f, 1.0f);
    const bool alive_enemy_present =
        rf::local_player && has_alive_enemy_players(*rf::local_player);
    const int goal_eval_ms = std::clamp(
        static_cast<int>(std::lround(
            std::lerp(1250.0f, 325.0f, decision_skill)
            / std::max(decision_efficiency, 0.2f)
            * std::lerp(1.20f, 0.90f, camping_norm))),
        320,
        2200
    );
    constexpr float kPickupGoalCommitDistance = 3.0f;
    const float pickup_goal_commit_distance_sq =
        kPickupGoalCommitDistance * kPickupGoalCommitDistance;

    bool reevaluate_goal =
        !g_client_bot_state.goal_eval_timer.valid()
        || g_client_bot_state.goal_eval_timer.elapsed();
    if (g_client_bot_state.active_goal == BotGoalType::none) {
        // Runtime gameplay should immediately recover from a temporary no-goal state.
        reevaluate_goal = true;
    }

    // Forced re-evaluation when the current goal's target is no longer valid.
    // This ensures the bot transitions immediately rather than waiting for timer expiry.
    bool goal_target_invalidated = false;
    if (g_client_bot_state.active_goal == BotGoalType::eliminate_target
        && g_client_bot_state.goal_target_handle >= 0) {
        rf::Entity* tracked = rf::entity_from_handle(g_client_bot_state.goal_target_handle);
        const bool entity_gone = !tracked || tracked->life <= 0.0f;
        // Also check player-level death state, since entity life may lag behind
        bool player_dead_or_dying = false;
        if (tracked) {
            rf::Player* target_player = rf::player_from_entity_handle(tracked->handle);
            if (target_player) {
                player_dead_or_dying = rf::player_is_dead(target_player)
                    || rf::player_is_dying(target_player);
            }
        }
        if (entity_gone || player_dead_or_dying) {
            goal_target_invalidated = true;
            reevaluate_goal = true;
        }
    }
    else if (bot_goal_is_item_collection(g_client_bot_state.active_goal)
        && g_client_bot_state.goal_target_handle >= 0) {
        rf::Object* goal_obj = rf::obj_from_handle(g_client_bot_state.goal_target_handle);
        if (!goal_obj || !bot_internal_is_collectible_goal_item(*static_cast<rf::Item*>(goal_obj))) {
            goal_target_invalidated = true;
            reevaluate_goal = true;
        }
    }
    if (goal_target_invalidated) {
        // Current goal's target is gone — clear everything so the bot immediately
        // picks the next best objective without any stale state interfering.
        g_client_bot_state.goal_switch_lock_timer.invalidate();
        g_client_bot_state.goal_eval_timer.invalidate();
        g_client_bot_state.recovery_roam_lock_timer.invalidate();
        g_client_bot_state.pursuit_recovery_timer.invalidate();
        g_client_bot_state.pursuit_route_failures = 0;
        g_client_bot_state.pursuit_target_handle = -1;
        g_client_bot_state.eliminate_target_reacquire_timer.invalidate();
        bot_state_clear_recovery_reroute();
        bot_state_clear_waypoint_route(true, true, false);
        // Clear the stale goal so downstream checks don't see the old goal type
        // and apply commitment/lock overrides for a goal that no longer exists.
        bot_state_clear_goal();
    }
    const bool recovering_navigation_now =
        g_client_bot_state.recovery_pending_reroute
        || g_client_bot_state.fsm_state == BotFsmState::recover_navigation;
    if (recovering_navigation_now
        && g_client_bot_state.active_goal == BotGoalType::roam
        && (g_client_bot_state.has_waypoint_target
            || !g_client_bot_state.waypoint_path.empty()
            || g_client_bot_state.waypoint_goal > 0)) {
        // Keep a recovery roam plan stable long enough to execute, but allow
        // reevaluation after the time limit so the bot can escape a stale recovery.
        if (!g_client_bot_state.recovery_roam_lock_timer.valid()) {
            g_client_bot_state.recovery_roam_lock_timer.set(kRecoveryRoamReevalLimitMs);
        }
        if (!g_client_bot_state.recovery_roam_lock_timer.elapsed()) {
            reevaluate_goal = false;
        }
    }
    else {
        g_client_bot_state.recovery_roam_lock_timer.invalidate();
    }
    bool item_pickup_commit_active = false;
    bool item_goal_satisfied_now = false;
    if (bot_goal_is_item_collection(g_client_bot_state.active_goal)
        && g_client_bot_state.goal_target_handle >= 0) {
        rf::Object* goal_obj = rf::obj_from_handle(g_client_bot_state.goal_target_handle);
        if (goal_obj
            && goal_obj->type == rf::OT_ITEM
            && bot_internal_is_collectible_goal_item(*static_cast<rf::Item*>(goal_obj))) {
            item_goal_satisfied_now = bot_internal_is_item_goal_satisfied(
                local_entity,
                g_client_bot_state.active_goal,
                *static_cast<rf::Item*>(goal_obj));
            g_client_bot_state.goal_target_pos = goal_obj->pos;
            const float dist_sq = rf::vec_dist_squared(
                &local_entity.pos,
                &g_client_bot_state.goal_target_pos
            );
            item_pickup_commit_active = dist_sq <= pickup_goal_commit_distance_sq;
            if (item_pickup_commit_active && !item_goal_satisfied_now) {
                // Prevent late retargeting when we are already on top of a valid pickup.
                reevaluate_goal = false;
            }
        }
    }
    const bool immediate_enemy_pressure =
        enemy_target
        && (enemy_has_los || retaliation_matches_enemy);
    if (!goal_target_invalidated
        && bot_goal_is_item_collection(g_client_bot_state.active_goal)
        && has_committed_pickup_route()
        && !immediate_enemy_pressure) {
        // Keep a committed pickup route stable unless there is immediate combat pressure.
        reevaluate_goal = false;
    }
    if (!goal_target_invalidated
        && g_client_bot_state.active_goal == BotGoalType::collect_super_item
        && g_client_bot_state.goal_target_handle >= 0
        && !immediate_enemy_pressure) {
        rf::Object* goal_obj = rf::obj_from_handle(g_client_bot_state.goal_target_handle);
        if (goal_obj
            && goal_obj->type == rf::OT_ITEM
            && bot_internal_is_collectible_goal_item(*static_cast<rf::Item*>(goal_obj))
            && !item_goal_satisfied_now) {
            // Super items are high-value objectives; stick to the current one until it is no longer valid.
            reevaluate_goal = false;
        }
    }
    if (item_goal_satisfied_now) {
        goal_target_invalidated = true;
        reevaluate_goal = true;
        g_client_bot_state.goal_switch_lock_timer.invalidate();
        g_client_bot_state.goal_eval_timer.invalidate();
        g_client_bot_state.recovery_roam_lock_timer.invalidate();
        g_client_bot_state.pursuit_recovery_timer.invalidate();
        bot_state_clear_recovery_reroute();
        bot_state_clear_waypoint_route(true, true, false);
        bot_state_clear_goal();
    }
    if (g_client_bot_state.active_goal == BotGoalType::eliminate_target) {
        if (!alive_enemy_present) {
            g_client_bot_state.eliminate_target_reacquire_timer.invalidate();
            g_client_bot_state.pursuit_recovery_timer.invalidate();
            g_client_bot_state.pursuit_route_failures = 0;
            g_client_bot_state.pursuit_target_handle = -1;
            bot_state_clear_waypoint_route(true, true, false);
            reevaluate_goal = true;
        }

        if (g_client_bot_state.goal_target_handle >= 0 && !goal_target_invalidated) {
            if (rf::Entity* tracked_entity = rf::entity_from_handle(
                    g_client_bot_state.goal_target_handle)) {
                g_client_bot_state.goal_target_pos = tracked_entity->pos;
            }

            if (enemy_target
                && enemy_target->handle == g_client_bot_state.goal_target_handle) {
                g_client_bot_state.eliminate_target_reacquire_timer.invalidate();
            }
            else if (alive_enemy_present) {
                // Only delay re-evaluation for reacquire if there are still live enemies
                // to potentially reacquire. When no enemies are alive, let the bot
                // immediately transition to a new goal.
                if (!g_client_bot_state.eliminate_target_reacquire_timer.valid()) {
                    const int reacquire_ms = static_cast<int>(std::lround(std::lerp(
                        650.0f,
                        5200.0f,
                        eliminate_commitment_norm
                    )));
                    g_client_bot_state.eliminate_target_reacquire_timer.set(reacquire_ms);
                }
                if (!g_client_bot_state.eliminate_target_reacquire_timer.elapsed()) {
                    reevaluate_goal = false;
                }
                else {
                    reevaluate_goal = true;
                }
            }
        }
        else if (enemy_target) {
            // We were hunting without a lock; now there is a target to bind.
            reevaluate_goal = true;
        }

        const bool stabilize_eliminate_recovery_decision =
            g_client_bot_state.goal_target_handle >= 0
            && (g_client_bot_state.fsm_state == BotFsmState::recover_navigation
                || g_client_bot_state.recovery_pending_reroute);
        if (stabilize_eliminate_recovery_decision) {
            rf::Entity* tracked_target =
                rf::entity_from_handle(g_client_bot_state.goal_target_handle);
            if (tracked_target
                && tracked_target != &local_entity
                && !rf::entity_is_dying(tracked_target)) {
                // While recovering navigation, stick to the current eliminate target
                // and route plan unless that target is no longer valid.
                reevaluate_goal = false;
            }
        }
    }
    else if (g_client_bot_state.active_goal == BotGoalType::activate_bridge) {
        if (g_client_bot_state.goal_target_identifier >= 0) {
            WaypointBridgeZoneState bridge_state{};
            int bridge_waypoint = 0;
            rf::Vector3 bridge_waypoint_pos{};
            if (waypoints_get_bridge_zone_state(
                    g_client_bot_state.goal_target_identifier,
                    bridge_state)
                && !bridge_state.on
                && bridge_state.available
                && find_reachable_bridge_zone_waypoint(
                    local_entity,
                    bridge_state.zone_uid,
                    bridge_waypoint,
                    bridge_waypoint_pos)) {
                g_client_bot_state.goal_target_pos = bridge_waypoint_pos;
                g_client_bot_state.goal_target_waypoint = bridge_waypoint;
                reevaluate_goal = false;
            }
            else {
                reevaluate_goal = true;
            }
        }
        else {
            reevaluate_goal = true;
        }
    }
    else if (g_client_bot_state.active_goal == BotGoalType::create_crater) {
        WaypointTargetDefinition current_target{};
        int target_waypoint = 0;
        rf::Vector3 target_waypoint_pos{};
        if (!has_crater_weapon_now
            || g_client_bot_state.goal_target_identifier < 0
            || !waypoints_get_target_by_uid(
                g_client_bot_state.goal_target_identifier,
                current_target)
            || current_target.type != WaypointTargetType::explosion) {
            reevaluate_goal = true;
        }
        else {
            g_client_bot_state.goal_target_pos = current_target.pos;
            if (find_reachable_explosion_target_waypoint(
                    local_entity,
                    current_target,
                    target_waypoint,
                    target_waypoint_pos)) {
                g_client_bot_state.goal_target_waypoint = target_waypoint;
                g_client_bot_state.crater_goal_abort_timer.invalidate();
                reevaluate_goal = false;
            }
            else {
                if (!g_client_bot_state.crater_goal_abort_timer.valid()) {
                    g_client_bot_state.crater_goal_abort_timer.set(2000);
                    reevaluate_goal = false;
                }
                else if (!g_client_bot_state.crater_goal_abort_timer.elapsed()) {
                    reevaluate_goal = false;
                }
                else {
                    reevaluate_goal = true;
                }

                if (!reevaluate_goal && g_client_bot_state.goal_target_waypoint <= 0) {
                    g_client_bot_state.goal_target_waypoint =
                        bot_find_closest_waypoint_with_fallback(current_target.pos);
                }
            }
        }
    }
    else if (g_client_bot_state.active_goal == BotGoalType::shatter_glass) {
        WaypointTargetDefinition current_target{};
        int target_waypoint = 0;
        rf::Vector3 target_waypoint_pos{};
        if (!has_shatter_weapon_now
            || g_client_bot_state.goal_target_identifier < 0
            || !waypoints_get_target_by_uid(
                g_client_bot_state.goal_target_identifier,
                current_target)
            || current_target.type != WaypointTargetType::shatter) {
            reevaluate_goal = true;
        }
        else {
            g_client_bot_state.goal_target_pos = current_target.pos;
            if (find_reachable_explosion_target_waypoint(
                    local_entity,
                    current_target,
                    target_waypoint,
                    target_waypoint_pos)) {
                g_client_bot_state.goal_target_waypoint = target_waypoint;
                g_client_bot_state.shatter_goal_abort_timer.invalidate();
                reevaluate_goal = false;
            }
            else {
                if (!g_client_bot_state.shatter_goal_abort_timer.valid()) {
                    g_client_bot_state.shatter_goal_abort_timer.set(2000);
                    reevaluate_goal = false;
                }
                else if (!g_client_bot_state.shatter_goal_abort_timer.elapsed()) {
                    reevaluate_goal = false;
                }
                else {
                    reevaluate_goal = true;
                }

                if (!reevaluate_goal && g_client_bot_state.goal_target_waypoint <= 0) {
                    g_client_bot_state.goal_target_waypoint =
                        bot_find_closest_waypoint_with_fallback(current_target.pos);
                }
            }
        }
    }
    else if (bot_goal_is_ctf_objective(g_client_bot_state.active_goal)) {
        CtfRuntimeState ctf_state{};
        if (!build_ctf_runtime_state(local_entity, ctf_state)) {
            reevaluate_goal = true;
        }
        else {
            bool goal_valid = false;
            switch (g_client_bot_state.active_goal) {
                case BotGoalType::ctf_capture_flag:
                    goal_valid = ctf_state.local_has_enemy_flag && ctf_state.own_flag_in_base;
                    if (goal_valid) {
                        g_client_bot_state.goal_target_identifier = ctf_state.own_flag_uid;
                        g_client_bot_state.goal_target_pos = ctf_state.own_base_pos;
                        g_client_bot_state.goal_target_waypoint = ctf_state.own_base_waypoint;
                    }
                    break;
                case BotGoalType::ctf_return_flag:
                    goal_valid = ctf_state.own_flag_dropped
                        || (ctf_state.own_flag_stolen && ctf_state.own_flag_carrier_entity);
                    if (goal_valid) {
                        if (ctf_state.own_flag_dropped) {
                            g_client_bot_state.goal_target_handle = -1;
                            g_client_bot_state.goal_target_identifier = ctf_state.own_flag_uid;
                            g_client_bot_state.goal_target_pos = ctf_state.own_flag_pos;
                            g_client_bot_state.goal_target_waypoint =
                                ctf_state.own_dropped_waypoint > 0
                                ? ctf_state.own_dropped_waypoint
                                : bot_find_closest_waypoint_with_fallback(ctf_state.own_flag_pos);
                        }
                        else {
                            g_client_bot_state.goal_target_handle =
                                ctf_state.own_flag_carrier_entity->handle;
                            g_client_bot_state.goal_target_identifier =
                                ctf_state.own_flag_carrier_entity->uid;
                            g_client_bot_state.goal_target_pos =
                                ctf_state.own_flag_carrier_entity->pos;
                            g_client_bot_state.goal_target_waypoint =
                                bot_find_closest_waypoint_with_fallback(
                                    ctf_state.own_flag_carrier_entity->pos);
                        }
                    }
                    break;
                case BotGoalType::ctf_steal_flag:
                    goal_valid = !ctf_state.local_has_enemy_flag
                        && (ctf_state.enemy_flag_in_base || ctf_state.enemy_flag_dropped);
                    if (goal_valid) {
                        g_client_bot_state.goal_target_identifier = ctf_state.enemy_flag_uid;
                        g_client_bot_state.goal_target_pos = ctf_state.enemy_flag_in_base
                            ? ctf_state.enemy_base_pos
                            : ctf_state.enemy_flag_pos;
                        g_client_bot_state.goal_target_waypoint = ctf_state.enemy_flag_in_base
                            ? ctf_state.enemy_base_waypoint
                            : (ctf_state.enemy_dropped_waypoint > 0
                                ? ctf_state.enemy_dropped_waypoint
                                : bot_find_closest_waypoint_with_fallback(ctf_state.enemy_flag_pos));
                    }
                    break;
                case BotGoalType::ctf_hold_enemy_flag:
                    goal_valid = ctf_state.local_has_enemy_flag && !ctf_state.own_flag_in_base;
                    if (goal_valid) {
                        g_client_bot_state.goal_target_identifier = ctf_state.own_flag_uid;
                        if (g_client_bot_state.goal_target_pos.len_sq() <= 0.0001f) {
                            g_client_bot_state.goal_target_pos =
                                (ctf_state.own_spawn_waypoint > 0)
                                    ? ctf_state.own_spawn_pos
                                    : ctf_state.own_base_pos;
                            g_client_bot_state.goal_target_waypoint =
                                (ctf_state.own_spawn_waypoint > 0)
                                    ? ctf_state.own_spawn_waypoint
                                    : ctf_state.own_base_waypoint;
                        }
                    }
                    break;
                default:
                    goal_valid = false;
                    break;
            }

            reevaluate_goal = !goal_valid;
        }
    }
    else if (bot_goal_is_control_point_objective(g_client_bot_state.active_goal)) {
        if (!is_control_point_mode() || !rf::local_player) {
            reevaluate_goal = true;
        }
        else {
            const HillOwner local_team = local_hill_team_owner();
            HillInfo* hill = koth_find_hill_by_handler_uid(g_client_bot_state.goal_target_identifier);
            if (!hill || local_team == HillOwner::HO_Neutral) {
                reevaluate_goal = true;
            }
            else {
                const bool capture_available =
                    control_point_team_can_capture_hill(*hill, local_team);
                const bool defense_needed =
                    control_point_hill_needs_defense(*hill, local_team);
                if (!capture_available && !defense_needed) {
                    reevaluate_goal = true;
                }
                else {
                    const int preferred_zone_waypoint = g_client_bot_state.goal_target_waypoint;
                    int zone_waypoint = 0;
                    rf::Vector3 zone_waypoint_pos{};
                    if (!find_reachable_control_point_waypoint(
                            local_entity,
                            g_client_bot_state.goal_target_identifier,
                            zone_waypoint,
                            zone_waypoint_pos,
                            nullptr,
                            preferred_zone_waypoint)) {
                        reevaluate_goal = true;
                    }
                    else {
                        g_client_bot_state.goal_target_waypoint = zone_waypoint;
                        g_client_bot_state.goal_target_pos =
                            resolve_control_point_objective_pos(*hill, zone_waypoint_pos);
                    }
                }
            }
        }
    }
    else if (!reevaluate_goal) {
        if (g_client_bot_state.active_goal == BotGoalType::roam) {
            // Roam is a valid steady-state goal; don't churn every frame unless
            // there is immediate combat pressure that should force reassessment.
            const bool urgent_enemy_pressure =
                enemy_target && (enemy_has_los || retaliation_matches_enemy);
            if (urgent_enemy_pressure) {
                reevaluate_goal = true;
            }
        }
        else if (bot_goal_is_item_collection(g_client_bot_state.active_goal)) {
            rf::Object* goal_obj = rf::obj_from_handle(g_client_bot_state.goal_target_handle);
            if (!goal_obj || goal_obj->type != rf::OT_ITEM
                || !bot_internal_is_collectible_goal_item(*static_cast<rf::Item*>(goal_obj))) {
                reevaluate_goal = true;
            }
            else {
                const bool lock_super_item_goal =
                    g_client_bot_state.active_goal == BotGoalType::collect_super_item
                    && !immediate_enemy_pressure;
                const auto* goal_item = static_cast<const rf::Item*>(goal_obj);
                const BotGoalType goal_item_type =
                    goal_item
                        ? bot_internal_classify_collect_goal_item(*goal_item)
                        : BotGoalType::none;
                if (goal_item_type == BotGoalType::none
                    || goal_item_type != g_client_bot_state.active_goal) {
                    reevaluate_goal = true;
                }
                else {
                    g_client_bot_state.goal_target_pos = goal_obj->pos;
                    if (!lock_super_item_goal) {
                        if (!bot_internal_find_item_goal_waypoint(
                                goal_obj->pos,
                                g_client_bot_state.goal_target_waypoint)) {
                            reevaluate_goal = true;
                        }
                        else {
                            const bool route_probe_due =
                                !g_client_bot_state.repath_timer.valid()
                                || g_client_bot_state.repath_timer.elapsed();
                            if (route_probe_due
                                && !has_any_route_to_waypoint(
                                    local_entity,
                                    g_client_bot_state.goal_target_waypoint)) {
                                reevaluate_goal = true;
                            }
                        }
                    }
                }
            }
        }
        else if (g_client_bot_state.active_goal == BotGoalType::none) {
            reevaluate_goal = true;
        }
        else {
            reevaluate_goal = true;
        }
    }

    if (!reevaluate_goal
        && deathmatch_mode
        && bot_goal_is_item_collection(g_client_bot_state.active_goal)
        && readiness_delta >= 0.0f
        && !respawn_gearup_active
        && !item_pickup_commit_active
        && enemy_target) {
        // Deathmatch: if we are now combat-ready while collecting, switch to target hunting faster.
        reevaluate_goal = true;
    }

    if (!reevaluate_goal) {
        return;
    }

    ItemGoalCandidate item_goal{};
    const bool has_item_goal = bot_internal_find_best_item_goal(
        local_entity,
        enemy_target,
        item_goal
    );

    WaypointBridgeZoneState bridge_goal{};
    rf::Vector3 bridge_goal_waypoint_pos{};
    int bridge_goal_waypoint = 0;
    bool has_bridge_goal = false;
    float bridge_goal_score = -std::numeric_limits<float>::infinity();
    const bool bridge_recovery_context =
        g_client_bot_state.recovery_pending_reroute
        || g_client_bot_state.pursuit_route_failures > 0
        || g_client_bot_state.fsm_state == BotFsmState::recover_navigation;
    if (bridge_recovery_context || g_client_bot_state.active_goal == BotGoalType::activate_bridge) {
        if (g_client_bot_state.active_goal == BotGoalType::activate_bridge
            && g_client_bot_state.goal_target_identifier >= 0
            && waypoints_get_bridge_zone_state(
                g_client_bot_state.goal_target_identifier,
                bridge_goal)
            && !bridge_goal.on) {
            has_bridge_goal = true;
        }
        else if (waypoints_find_nearest_inactive_bridge_zone(local_entity.pos, bridge_goal)) {
            has_bridge_goal = true;
        }

        if (has_bridge_goal) {
            float bridge_waypoint_dist_sq = std::numeric_limits<float>::infinity();
            const bool has_reachable_zone_waypoint = find_reachable_bridge_zone_waypoint(
                local_entity,
                bridge_goal.zone_uid,
                bridge_goal_waypoint,
                bridge_goal_waypoint_pos,
                &bridge_waypoint_dist_sq
            );
            if (!has_reachable_zone_waypoint) {
                has_bridge_goal = false;
            }
            else {
                const float bridge_dist = std::sqrt(std::max(bridge_waypoint_dist_sq, 0.0f));
                const float proximity_bonus = std::clamp(
                    160.0f - bridge_dist * 1.2f,
                    -55.0f,
                    160.0f
                );
                bridge_goal_score =
                    (bridge_recovery_context ? 195.0f : 95.0f)
                    + proximity_bonus
                    + static_cast<float>(g_client_bot_state.pursuit_route_failures) * 28.0f;
            }
        }
    }

    int crater_goal_target_uid = -1;
    rf::Vector3 crater_goal_target_pos{};
    int crater_goal_waypoint = 0;
    bool has_crater_goal = false;
    float crater_goal_score = -std::numeric_limits<float>::infinity();
    int shatter_goal_target_uid = -1;
    rf::Vector3 shatter_goal_target_pos{};
    int shatter_goal_waypoint = 0;
    bool has_shatter_goal = false;
    float shatter_goal_score = -std::numeric_limits<float>::infinity();
    const bool crater_unlock_affinity =
        bot_personality_has_quirk(BotPersonalityQuirk::crater_unlock_affinity);
    const bool route_unlock_context =
        (g_client_bot_state.active_goal == BotGoalType::create_crater
            || g_client_bot_state.active_goal == BotGoalType::shatter_glass
            || bridge_recovery_context
            || (g_client_bot_state.active_goal == BotGoalType::eliminate_target
                && enemy_target
                && !enemy_has_los));
    const bool crater_access_context =
        (g_client_bot_state.active_goal == BotGoalType::create_crater
            || bridge_recovery_context
            || (g_client_bot_state.active_goal == BotGoalType::eliminate_target
                && enemy_target
                && !enemy_has_los));
    const bool crater_context = has_crater_weapon_now && route_unlock_context;
    const bool shatter_context = has_shatter_weapon_now && route_unlock_context;
    if (crater_context) {
        const int target_count = waypoints_target_count();
        for (int target_index = 0; target_index < target_count; ++target_index) {
            WaypointTargetDefinition target{};
            if (!waypoints_get_target_by_index(target_index, target)
                || target.type != WaypointTargetType::explosion) {
                continue;
            }

            int target_waypoint = 0;
            rf::Vector3 target_waypoint_pos{};
            float target_waypoint_dist_sq = std::numeric_limits<float>::infinity();
            if (!find_reachable_explosion_target_waypoint(
                    local_entity,
                    target,
                    target_waypoint,
                    target_waypoint_pos,
                    &target_waypoint_dist_sq)) {
                continue;
            }

            const float target_waypoint_dist = std::sqrt(std::max(target_waypoint_dist_sq, 0.0f));
            const float proximity_bonus = std::clamp(
                170.0f - target_waypoint_dist * 1.25f,
                -60.0f,
                170.0f
            );
            float score =
                (bridge_recovery_context ? 205.0f : 115.0f)
                + proximity_bonus;
            if (enemy_target && !enemy_has_los) {
                score += 35.0f;
            }
            if (crater_unlock_affinity) {
                score += 24.0f;
            }
            if (target.uid == g_client_bot_state.goal_target_identifier) {
                score += 18.0f;
            }

            if (goal_score_wins_with_tie_break(score, crater_goal_score)) {
                crater_goal_score = score;
                crater_goal_target_uid = target.uid;
                crater_goal_target_pos = target.pos;
                crater_goal_waypoint = target_waypoint;
                has_crater_goal = true;
            }
        }
    }
    if (shatter_context) {
        const int target_count = waypoints_target_count();
        for (int target_index = 0; target_index < target_count; ++target_index) {
            WaypointTargetDefinition target{};
            if (!waypoints_get_target_by_index(target_index, target)
                || target.type != WaypointTargetType::shatter) {
                continue;
            }

            int target_waypoint = 0;
            rf::Vector3 target_waypoint_pos{};
            float target_waypoint_dist_sq = std::numeric_limits<float>::infinity();
            if (!find_reachable_explosion_target_waypoint(
                    local_entity,
                    target,
                    target_waypoint,
                    target_waypoint_pos,
                    &target_waypoint_dist_sq)) {
                continue;
            }

            const float target_waypoint_dist = std::sqrt(std::max(target_waypoint_dist_sq, 0.0f));
            const float proximity_bonus = std::clamp(
                165.0f - target_waypoint_dist * 1.20f,
                -55.0f,
                165.0f
            );
            float score =
                (bridge_recovery_context ? 190.0f : 105.0f)
                + proximity_bonus;
            if (enemy_target && !enemy_has_los) {
                score += 32.0f;
            }
            if (crater_unlock_affinity) {
                score += 8.0f;
            }
            if (target.uid == g_client_bot_state.goal_target_identifier) {
                score += 16.0f;
            }

            if (goal_score_wins_with_tie_break(score, shatter_goal_score)) {
                shatter_goal_score = score;
                shatter_goal_target_uid = target.uid;
                shatter_goal_target_pos = target.pos;
                shatter_goal_waypoint = target_waypoint;
                has_shatter_goal = true;
            }
        }
    }

    float post_respawn_item_bonus = 0.0f;
    if (respawn_gearup_active && has_item_goal) {
        switch (item_goal.goal_type) {
            case BotGoalType::collect_weapon:
                post_respawn_item_bonus = std::lerp(45.0f, 145.0f, seek_weapon_norm)
                    * respawn_gearup_priority;
                break;
            case BotGoalType::collect_armor:
                post_respawn_item_bonus = std::lerp(
                    40.0f,
                    125.0f,
                    std::max(replenish_norm, maintenance_norm))
                    * respawn_gearup_priority;
                break;
            case BotGoalType::collect_super_item:
                post_respawn_item_bonus = 35.0f * respawn_gearup_priority;
                break;
            default:
                break;
        }
    }
    const float post_respawn_enemy_penalty = respawn_gearup_active
        ? std::lerp(
            20.0f,
            90.0f,
            std::max(seek_weapon_norm, std::max(replenish_norm, maintenance_norm)))
            * respawn_gearup_priority
        : 0.0f;
    const float retaliation_enemy_bonus = retaliation_matches_enemy
        ? std::lerp(12.0f, 78.0f, retaliation_norm)
        : 0.0f;
    const float retaliation_item_penalty = retaliation_active
        ? std::lerp(4.0f, 32.0f, retaliation_norm)
        : 0.0f;
    const bool forage_recovery_active =
        g_client_bot_state.pursuit_recovery_timer.valid()
        && !g_client_bot_state.pursuit_recovery_timer.elapsed();
    const float forage_enemy_reacquire_penalty =
        (forage_recovery_active && !enemy_has_los)
            ? std::lerp(95.0f, 165.0f, 1.0f - eliminate_commitment_norm)
                * (retaliation_matches_enemy ? 0.30f : 1.0f)
            : 0.0f;

    const float enemy_goal_score = enemy_target
        ? (bot_internal_compute_enemy_goal_score(local_entity, *enemy_target, enemy_has_los)
            + (enemy_has_los ? 18.0f : 0.0f) * aggression
            + std::lerp(-8.0f, 20.0f, std::clamp((raw_aggression - 0.25f) / 2.25f, 0.0f, 1.0f))
            + std::lerp(-8.0f, 12.0f, decision_skill)
            - std::lerp(0.0f, 34.0f, maintenance_pressure)
            + readiness_delta * std::lerp(55.0f, 115.0f, decision_skill)
            + retaliation_enemy_bonus
            - post_respawn_enemy_penalty
            - forage_enemy_reacquire_penalty
            + (deathmatch_mode
                ? (std::lerp(0.0f, 42.0f, kill_focus_norm)
                    + readiness_delta * std::lerp(45.0f, 95.0f, kill_focus_norm))
                : 0.0f))
        : -std::numeric_limits<float>::infinity();
    float item_goal_score = has_item_goal
        ? item_goal.score
            + kGoalItemBaseBonus * opportunism
            - std::lerp(0.0f, 22.0f, std::clamp((raw_aggression - 0.25f) / 2.25f, 0.0f, 1.0f))
            + std::lerp(6.0f, 16.0f, decision_skill) * opportunism
            + std::lerp(0.0f, 42.0f, maintenance_pressure)
            + std::max(-readiness_delta, 0.0f) * std::lerp(16.0f, 38.0f, maintenance_norm)
            + post_respawn_item_bonus
            - retaliation_item_penalty
            - (deathmatch_mode
                ? std::max(readiness_delta, 0.0f) * std::lerp(15.0f, 55.0f, kill_focus_norm)
                : 0.0f)
        : -std::numeric_limits<float>::infinity();
    ItemGoalCandidate crater_enabler_item_goal{};
    const bool has_crater_enabler_item_goal =
        crater_access_context
        && !has_crater_weapon_now
        && find_best_crater_enabler_item_goal(local_entity, crater_enabler_item_goal);
    const float crater_enabler_item_goal_score = has_crater_enabler_item_goal
        ? (crater_enabler_item_goal.score
            + kGoalItemBaseBonus * 1.65f
            + std::lerp(6.0f, 22.0f, decision_skill)
            + (crater_unlock_affinity ? 28.0f : 12.0f))
        : -std::numeric_limits<float>::infinity();
    CtfRuntimeState ctf_state{};
    const bool ctf_mode = build_ctf_runtime_state(local_entity, ctf_state);
    const float ctf_capture_bias = std::clamp(personality.ctf_capture_priority_bias, 0.25f, 2.5f);
    const float ctf_recovery_bias = std::clamp(personality.ctf_flag_recovery_bias, 0.25f, 2.5f);
    const float ctf_hold_safety_bias = std::clamp(personality.ctf_hold_enemy_flag_safety_bias, 0.25f, 2.5f);
    const float ctf_hold_hunt_bias = std::clamp(personality.ctf_hold_carrier_hunt_bias, 0.25f, 2.5f);
    const float ctf_capture_norm = std::clamp((ctf_capture_bias - 0.25f) / 2.25f, 0.0f, 1.0f);
    const float ctf_recovery_norm = std::clamp((ctf_recovery_bias - 0.25f) / 2.25f, 0.0f, 1.0f);
    const float ctf_hold_safety_norm = std::clamp((ctf_hold_safety_bias - 0.25f) / 2.25f, 0.0f, 1.0f);
    const float ctf_hold_hunt_norm = std::clamp((ctf_hold_hunt_bias - 0.25f) / 2.25f, 0.0f, 1.0f);
    const bool hold_enemy_flag_state =
        ctf_mode
        && ctf_state.local_has_enemy_flag
        && !ctf_state.own_flag_in_base;
    const bool actively_holding_enemy_flag_goal =
        hold_enemy_flag_state
        && g_client_bot_state.active_goal == BotGoalType::ctf_hold_enemy_flag;
    if (actively_holding_enemy_flag_goal) {
        if (ctf_state.own_flag_dropped) {
            // If our flag is dropped, restart hold escalation window and stay defensive near base.
            g_client_bot_state.ctf_hold_goal_timer.set(15000);
        }
        else if (!g_client_bot_state.ctf_hold_goal_timer.valid()) {
            g_client_bot_state.ctf_hold_goal_timer.set(15000);
        }
    }
    else {
        g_client_bot_state.ctf_hold_goal_timer.invalidate();
    }
    const bool hold_escalation_window_open =
        actively_holding_enemy_flag_goal
        && g_client_bot_state.ctf_hold_goal_timer.elapsed();

    if (hold_enemy_flag_state && has_item_goal) {
        const bool survivability_pickup =
            item_goal.goal_type == BotGoalType::collect_health
            || item_goal.goal_type == BotGoalType::collect_armor
            || item_goal.goal_type == BotGoalType::collect_super_item;
        if (survivability_pickup) {
            const float survivability_bonus =
                std::lerp(30.0f, 185.0f, maintenance_pressure)
                * std::lerp(0.80f, 1.35f, ctf_hold_safety_norm);
            item_goal_score += survivability_bonus;
        }
    }

    CtfGoalCandidate ctf_goal{};
    bool ctf_force_flag_carrier_elimination = false;
    int ctf_priority_flag_carrier_handle = -1;
    if (ctf_mode) {
        const auto consider_ctf_candidate = [&](const CtfGoalCandidate& candidate) {
            if (!goal_score_wins_with_tie_break(candidate.score, ctf_goal.score)) {
                return;
            }
            ctf_goal = candidate;
        };

        if (ctf_state.local_has_enemy_flag && ctf_state.own_flag_in_base) {
            const float capture_dist = std::sqrt(std::max(
                rf::vec_dist_squared(&local_entity.pos, &ctf_state.own_base_pos),
                0.0f
            ));
            const float proximity_bonus = std::clamp(190.0f - capture_dist * 1.40f, -80.0f, 190.0f);
            consider_ctf_candidate(CtfGoalCandidate{
                BotGoalType::ctf_capture_flag,
                -1,
                ctf_state.own_flag_uid,
                ctf_state.own_base_waypoint,
                ctf_state.own_base_pos,
                std::lerp(230.0f, 420.0f, ctf_capture_norm) + proximity_bonus,
            });
        }
        else if (ctf_state.local_has_enemy_flag && !ctf_state.own_flag_in_base) {
            const bool has_spawn_anchor = ctf_state.own_spawn_waypoint > 0;
            const bool has_base_anchor = ctf_state.own_base_waypoint > 0;
            const bool prefer_spawn_anchor = has_spawn_anchor
                && std::lerp(0.20f, 1.0f, ctf_hold_safety_bias) > 0.55f;
            int hold_waypoint = prefer_spawn_anchor
                ? ctf_state.own_spawn_waypoint
                : ctf_state.own_base_waypoint;
            rf::Vector3 hold_pos = prefer_spawn_anchor
                ? ctf_state.own_spawn_pos
                : ctf_state.own_base_pos;
            const int alt_waypoint = prefer_spawn_anchor
                ? ctf_state.own_base_waypoint
                : ctf_state.own_spawn_waypoint;
            const rf::Vector3 alt_pos = prefer_spawn_anchor
                ? ctf_state.own_base_pos
                : ctf_state.own_spawn_pos;
            if (has_spawn_anchor && has_base_anchor && alt_waypoint > 0) {
                constexpr float kHoldAnchorSwapRadius = kWaypointLinkRadius * 1.8f;
                const float dist_to_primary_sq = rf::vec_dist_squared(&local_entity.pos, &hold_pos);
                if (dist_to_primary_sq <= kHoldAnchorSwapRadius * kHoldAnchorSwapRadius) {
                    // Keep moving between own base and own spawn anchors while holding enemy flag.
                    hold_waypoint = alt_waypoint;
                    hold_pos = alt_pos;
                }
            }
            const float hold_dist = std::sqrt(std::max(
                rf::vec_dist_squared(&local_entity.pos, &hold_pos),
                0.0f
            ));
            const float hold_proximity_bonus = std::clamp(160.0f - hold_dist * 1.15f, -55.0f, 160.0f);
            const float hold_maintenance_penalty =
                std::lerp(15.0f, 170.0f, maintenance_pressure)
                * std::lerp(0.75f, 1.20f, ctf_hold_safety_norm);
            consider_ctf_candidate(CtfGoalCandidate{
                BotGoalType::ctf_hold_enemy_flag,
                -1,
                ctf_state.own_flag_uid,
                hold_waypoint,
                hold_pos,
                std::lerp(200.0f, 350.0f, ctf_capture_norm) * ctf_hold_safety_bias
                    + hold_proximity_bonus
                    - hold_maintenance_penalty,
            });

            const bool own_flag_carrier_visible =
                hold_escalation_window_open
                && ctf_state.own_flag_stolen
                && ctf_state.own_flag_carrier_entity
                && enemy_target
                && enemy_has_los
                && enemy_target->handle == ctf_state.own_flag_carrier_entity->handle;
            if (own_flag_carrier_visible) {
                const float carrier_priority_score =
                    bot_internal_compute_enemy_goal_score(
                        local_entity,
                        *ctf_state.own_flag_carrier_entity,
                        true
                    )
                    + std::lerp(250.0f, 430.0f, ctf_recovery_norm) * ctf_hold_hunt_bias
                    + std::lerp(35.0f, 110.0f, ctf_capture_norm)
                    + std::lerp(0.0f, 165.0f, ctf_hold_hunt_norm);
                consider_ctf_candidate(CtfGoalCandidate{
                    BotGoalType::eliminate_target,
                    ctf_state.own_flag_carrier_entity->handle,
                    ctf_state.own_flag_carrier_entity->uid,
                    0,
                    ctf_state.own_flag_carrier_entity->pos,
                    carrier_priority_score,
                });
            }
        }
        else {
            if (ctf_state.own_flag_stolen && ctf_state.own_flag_carrier_entity) {
                const bool carrier_has_los =
                    enemy_target
                    && enemy_target->handle == ctf_state.own_flag_carrier_entity->handle
                    && enemy_has_los;
                float carrier_score = bot_internal_compute_enemy_goal_score(
                    local_entity,
                    *ctf_state.own_flag_carrier_entity,
                    carrier_has_los
                ) + std::lerp(420.0f, 760.0f, ctf_recovery_norm);
                if (ctf_state.local_has_enemy_flag) {
                    // If we are holding enemy flag while ours is stolen, carrier kill is critical.
                    carrier_score += 140.0f;
                }
                if (carrier_has_los) {
                    carrier_score += 45.0f;
                }
                consider_ctf_candidate(CtfGoalCandidate{
                    BotGoalType::ctf_return_flag,
                    ctf_state.own_flag_carrier_entity->handle,
                    ctf_state.own_flag_uid,
                    0,
                    ctf_state.own_flag_carrier_entity->pos,
                    carrier_score,
                });
                ctf_force_flag_carrier_elimination = true;
                ctf_priority_flag_carrier_handle = ctf_state.own_flag_carrier_entity->handle;
            }
            else if (ctf_state.own_flag_dropped) {
                const int return_waypoint = ctf_state.own_dropped_waypoint > 0
                    ? ctf_state.own_dropped_waypoint
                    : bot_find_closest_waypoint_with_fallback(ctf_state.own_flag_pos);
                const float return_dist = std::sqrt(std::max(
                    rf::vec_dist_squared(&local_entity.pos, &ctf_state.own_flag_pos),
                    0.0f
                ));
                const float return_proximity_bonus = std::clamp(180.0f - return_dist * 1.30f, -70.0f, 180.0f);
                consider_ctf_candidate(CtfGoalCandidate{
                    BotGoalType::ctf_return_flag,
                    -1,
                    ctf_state.own_flag_uid,
                    return_waypoint,
                    ctf_state.own_flag_pos,
                    std::lerp(205.0f, 390.0f, ctf_recovery_norm) + return_proximity_bonus,
                });
            }

            if (!ctf_force_flag_carrier_elimination
                && (ctf_state.enemy_flag_in_base || ctf_state.enemy_flag_dropped)) {
                const rf::Vector3 steal_target_pos = ctf_state.enemy_flag_in_base
                    ? ctf_state.enemy_base_pos
                    : ctf_state.enemy_flag_pos;
                const int steal_waypoint = ctf_state.enemy_flag_in_base
                    ? ctf_state.enemy_base_waypoint
                    : (ctf_state.enemy_dropped_waypoint > 0
                        ? ctf_state.enemy_dropped_waypoint
                        : bot_find_closest_waypoint_with_fallback(ctf_state.enemy_flag_pos));
                const float steal_dist = std::sqrt(std::max(
                    rf::vec_dist_squared(&local_entity.pos, &steal_target_pos),
                    0.0f
                ));
                float steal_score = std::lerp(190.0f, 355.0f, ctf_capture_norm)
                    + std::clamp(175.0f - steal_dist * 1.18f, -65.0f, 175.0f);
                if (ctf_state.own_flag_stolen || ctf_state.own_flag_dropped) {
                    steal_score -= std::lerp(95.0f, 22.0f, ctf_recovery_norm);
                }
                consider_ctf_candidate(CtfGoalCandidate{
                    BotGoalType::ctf_steal_flag,
                    -1,
                    ctf_state.enemy_flag_uid,
                    steal_waypoint,
                    steal_target_pos,
                    steal_score,
                });
            }
        }
    }

    ControlPointGoalCandidate control_point_goal{};
    float control_point_enemy_defense_bonus = 0.0f;
    float active_control_point_goal_score = -std::numeric_limits<float>::infinity();
    const bool active_control_point_objective =
        g_client_bot_state.active_goal == BotGoalType::control_point_objective
        && g_client_bot_state.goal_target_identifier >= 0;
    const int active_control_point_identifier =
        active_control_point_objective
            ? g_client_bot_state.goal_target_identifier
            : -1;
    const int active_control_point_waypoint =
        active_control_point_objective
            ? g_client_bot_state.goal_target_waypoint
            : 0;
    const float control_point_retention_bonus = std::clamp(
        36.0f * std::clamp(goal_commitment, 0.45f, 2.60f),
        20.0f,
        108.0f
    );
    if (is_control_point_mode() && rf::local_player) {
        const HillOwner local_team = local_hill_team_owner();
        if (local_team == HillOwner::HO_Red || local_team == HillOwner::HO_Blue) {
            const HillOwner enemy_team = opposite(local_team);
            const auto consider_control_point_candidate =
                [&](ControlPointGoalCandidate candidate) {
                    if (active_control_point_objective
                        && candidate.identifier == active_control_point_identifier) {
                        candidate.score += control_point_retention_bonus;
                        if (active_control_point_waypoint > 0
                            && candidate.waypoint == active_control_point_waypoint) {
                            candidate.score += 14.0f;
                        }
                        active_control_point_goal_score = std::max(
                            active_control_point_goal_score,
                            candidate.score
                        );
                    }
                    if (!goal_score_wins_with_tie_break(candidate.score, control_point_goal.score)) {
                        return;
                    }
                    control_point_goal = candidate;
                };

            for (const HillInfo& hill : g_koth_info.hills) {
                if (hill.lock_status != HillLockStatus::HLS_Available) {
                    continue;
                }
                const int handler_uid = koth_get_hill_handler_uid(hill);
                if (handler_uid < 0) {
                    continue;
                }

                int hill_waypoint = 0;
                rf::Vector3 hill_waypoint_pos{};
                const int preferred_hill_waypoint =
                    active_control_point_objective
                        && active_control_point_identifier == handler_uid
                        && active_control_point_waypoint > 0
                    ? active_control_point_waypoint
                    : 0;
                if (!find_reachable_control_point_waypoint(
                        local_entity,
                        handler_uid,
                        hill_waypoint,
                        hill_waypoint_pos,
                        nullptr,
                        preferred_hill_waypoint)) {
                    continue;
                }

                const rf::Vector3 objective_pos =
                    resolve_control_point_objective_pos(hill, hill_waypoint_pos);
                const float objective_dist = std::sqrt(std::max(
                    rf::vec_dist_squared(&local_entity.pos, &objective_pos),
                    0.0f
                ));

                if (control_point_hill_needs_defense(hill, local_team)) {
                    const float hostile_progress_norm = std::clamp(
                        static_cast<float>(hill.capture_milli) / 100000.0f,
                        0.0f,
                        1.0f
                    );
                    const float defense_score =
                        std::lerp(300.0f, 585.0f, hostile_progress_norm)
                        + std::clamp(225.0f - objective_dist * 1.35f, -65.0f, 225.0f);
                    consider_control_point_candidate(ControlPointGoalCandidate{
                        BotGoalType::control_point_objective,
                        handler_uid,
                        hill_waypoint,
                        objective_pos,
                        defense_score,
                        true,
                    });

                    if (enemy_target && enemy_team != HillOwner::HO_Neutral) {
                        const float enemy_to_hill_dist = std::sqrt(std::max(
                            rf::vec_dist_squared(&enemy_target->pos, &objective_pos),
                            0.0f
                        ));
                        constexpr float kControlPointDefenseEnemyRange = 16.0f;
                        if (enemy_to_hill_dist <= kControlPointDefenseEnemyRange) {
                            const float enemy_bonus =
                                std::clamp(245.0f - enemy_to_hill_dist * 11.0f, 45.0f, 245.0f)
                                + std::lerp(0.0f, 95.0f, hostile_progress_norm);
                            control_point_enemy_defense_bonus =
                                std::max(control_point_enemy_defense_bonus, enemy_bonus);
                        }
                    }
                    continue;
                }

                if (!control_point_team_can_capture_hill(hill, local_team)) {
                    continue;
                }

                const bool neutral_point = hill.ownership == HillOwner::HO_Neutral;
                const bool already_building_capture =
                    hill.steal_dir == local_team && hill.capture_milli > 0;
                const float progress_norm = std::clamp(
                    static_cast<float>(hill.capture_milli) / 100000.0f,
                    0.0f,
                    1.0f
                );
                float capture_score =
                    (neutral_point
                        ? std::lerp(180.0f, 325.0f, opportunism_norm)
                        : std::lerp(220.0f, 390.0f, aggression_norm))
                    + std::clamp(195.0f - objective_dist * 1.20f, -75.0f, 195.0f);
                if (already_building_capture) {
                    capture_score += std::lerp(22.0f, 160.0f, progress_norm);
                }

                if (enemy_target && enemy_has_los && enemy_team != HillOwner::HO_Neutral) {
                    const float enemy_to_hill_dist = std::sqrt(std::max(
                        rf::vec_dist_squared(&enemy_target->pos, &objective_pos),
                        0.0f
                    ));
                    constexpr float kControlPointContestEnemyRange = 15.0f;
                    if (enemy_to_hill_dist <= kControlPointContestEnemyRange) {
                        const float contest_bonus =
                            std::clamp(260.0f - enemy_to_hill_dist * 12.0f, 55.0f, 260.0f)
                            + std::lerp(0.0f, 120.0f, progress_norm);
                        control_point_enemy_defense_bonus =
                            std::max(control_point_enemy_defense_bonus, contest_bonus);
                    }
                }

                consider_control_point_candidate(ControlPointGoalCandidate{
                    BotGoalType::control_point_objective,
                    handler_uid,
                    hill_waypoint,
                    objective_pos,
                    capture_score,
                    false,
                });
            }
        }
    }

    // Capture the top-3 highest-scoring candidates for HUD debug display.
    // Snapshot taken before commitment/hysteresis so the display shows raw evaluation
    // results, making it clear why the final goal was (or wasn't) changed.
    {
        struct RankedEntry
        {
            BotGoalType goal = BotGoalType::none;
            float score = -std::numeric_limits<float>::infinity();
            int identifier = -1;
        };
        RankedEntry entries[9]{};
        int n = 0;
        const auto push_entry = [&](const BotGoalType g, const float s, const int id) {
            if (std::isfinite(s) && n < static_cast<int>(std::size(entries))) {
                entries[n++] = {g, s, id};
            }
        };
        if (enemy_target) push_entry(BotGoalType::eliminate_target, enemy_goal_score, enemy_target->uid);
        if (has_item_goal) push_entry(item_goal.goal_type, item_goal_score, item_goal.item_uid);
        if (has_crater_enabler_item_goal) push_entry(crater_enabler_item_goal.goal_type, crater_enabler_item_goal_score, crater_enabler_item_goal.item_uid);
        if (has_bridge_goal) push_entry(BotGoalType::activate_bridge, bridge_goal_score, bridge_goal.zone_uid);
        if (has_crater_goal) push_entry(BotGoalType::create_crater, crater_goal_score, crater_goal_target_uid);
        if (has_shatter_goal) push_entry(BotGoalType::shatter_glass, shatter_goal_score, shatter_goal_target_uid);
        if (ctf_goal.goal != BotGoalType::none) push_entry(ctf_goal.goal, ctf_goal.score, ctf_goal.identifier);
        if (control_point_goal.goal != BotGoalType::none) push_entry(control_point_goal.goal, control_point_goal.score, control_point_goal.identifier);
        std::sort(entries, entries + n, [](const RankedEntry& a, const RankedEntry& b) {
            return a.score > b.score;
        });
        for (int i = 0; i < 3; ++i) {
            if (i < n) {
                g_client_bot_state.last_eval_top3[i] = {entries[i].goal, entries[i].score, entries[i].identifier};
            }
            else {
                g_client_bot_state.last_eval_top3[i] = {};
            }
        }
    }

    // Tier 2 boost: when no tier 1 objective is achievable, boost item collection
    // scores so the bot actively pursues pickups instead of falling back to roam.
    {
        const bool has_tier1 =
            alive_enemy_present
            || (ctf_goal.goal != BotGoalType::none && bot_goal_is_tier1_objective(ctf_goal.goal))
            || (control_point_goal.goal != BotGoalType::none);
        if (!has_tier1 && has_item_goal) {
            const float tier2_boost = std::lerp(65.0f, 35.0f, aggression_norm);
            item_goal_score += tier2_boost;
        }
    }

    BotGoalType selected_goal = BotGoalType::none;
    int selected_handle = -1;
    int selected_identifier = -1;
    int selected_waypoint = 0;
    rf::Vector3 selected_pos{};
    float selected_score = -std::numeric_limits<float>::infinity();

    if (enemy_target && goal_score_wins_with_tie_break(enemy_goal_score, selected_score)) {
        selected_goal = BotGoalType::eliminate_target;
        selected_handle = enemy_target->handle;
        selected_identifier = enemy_target->uid;
        selected_pos = enemy_target->pos;
        selected_score = enemy_goal_score;
    }
    if (has_item_goal && goal_score_wins_with_tie_break(item_goal_score, selected_score)) {
        selected_goal = item_goal.goal_type;
        selected_handle = item_goal.item_handle;
        selected_identifier = item_goal.item_uid;
        selected_waypoint = item_goal.goal_waypoint;
        selected_pos = item_goal.item_pos;
        selected_score = item_goal_score;
    }
    if (has_crater_enabler_item_goal
        && goal_score_wins_with_tie_break(crater_enabler_item_goal_score, selected_score)) {
        selected_goal = crater_enabler_item_goal.goal_type;
        selected_handle = crater_enabler_item_goal.item_handle;
        selected_identifier = crater_enabler_item_goal.item_uid;
        selected_waypoint = crater_enabler_item_goal.goal_waypoint;
        selected_pos = crater_enabler_item_goal.item_pos;
        selected_score = crater_enabler_item_goal_score;
    }
    if (has_bridge_goal && goal_score_wins_with_tie_break(bridge_goal_score, selected_score)) {
        selected_goal = BotGoalType::activate_bridge;
        selected_handle = -1;
        selected_identifier = bridge_goal.zone_uid;
        selected_waypoint = bridge_goal_waypoint;
        selected_pos = bridge_goal_waypoint_pos;
        selected_score = bridge_goal_score;
    }
    if (has_crater_goal && goal_score_wins_with_tie_break(crater_goal_score, selected_score)) {
        selected_goal = BotGoalType::create_crater;
        selected_handle = -1;
        selected_identifier = crater_goal_target_uid;
        selected_waypoint = crater_goal_waypoint;
        selected_pos = crater_goal_target_pos;
        selected_score = crater_goal_score;
    }
    if (has_shatter_goal && goal_score_wins_with_tie_break(shatter_goal_score, selected_score)) {
        selected_goal = BotGoalType::shatter_glass;
        selected_handle = -1;
        selected_identifier = shatter_goal_target_uid;
        selected_waypoint = shatter_goal_waypoint;
        selected_pos = shatter_goal_target_pos;
        selected_score = shatter_goal_score;
    }
    if (ctf_goal.goal != BotGoalType::none
        && goal_score_wins_with_tie_break(ctf_goal.score, selected_score)) {
        selected_goal = ctf_goal.goal;
        selected_handle = ctf_goal.handle;
        selected_identifier = ctf_goal.identifier;
        selected_waypoint = ctf_goal.waypoint;
        selected_pos = ctf_goal.pos;
        selected_score = ctf_goal.score;
    }
    if (control_point_goal.goal != BotGoalType::none
        && goal_score_wins_with_tie_break(control_point_goal.score, selected_score)) {
        selected_goal = control_point_goal.goal;
        selected_handle = -1;
        selected_identifier = control_point_goal.identifier;
        selected_waypoint = control_point_goal.waypoint;
        selected_pos = control_point_goal.pos;
        selected_score = control_point_goal.score;
    }
    if (control_point_mode
        && control_point_goal.goal != BotGoalType::none
        && selected_goal != BotGoalType::none) {
        const bool secondary_goal =
            selected_goal == BotGoalType::roam
            || selected_goal == BotGoalType::activate_bridge
            || selected_goal == BotGoalType::create_crater
            || selected_goal == BotGoalType::shatter_glass
            || bot_goal_is_item_collection(selected_goal);
        if (secondary_goal) {
            const float control_point_priority_margin = std::clamp(
                std::lerp(110.0f, 48.0f, opportunism_norm) * std::clamp(goal_commitment, 0.55f, 2.20f),
                35.0f,
                185.0f
            );
            if (selected_score < control_point_goal.score + control_point_priority_margin) {
                selected_goal = control_point_goal.goal;
                selected_handle = -1;
                selected_identifier = control_point_goal.identifier;
                selected_waypoint = control_point_goal.waypoint;
                selected_pos = control_point_goal.pos;
                selected_score = control_point_goal.score;
            }
        }
    }
    else if (enemy_target && selected_goal == BotGoalType::none) {
        selected_goal = BotGoalType::eliminate_target;
        selected_handle = enemy_target->handle;
        selected_identifier = enemy_target->uid;
        selected_pos = enemy_target->pos;
        selected_score = enemy_goal_score;
    }

    if (ctf_mode
        && ctf_state.local_has_enemy_flag
        && !ctf_state.own_flag_in_base
        && enemy_target) {
        const bool retaliating_recently =
            g_client_bot_state.retaliation_target_handle >= 0
            && g_client_bot_state.retaliation_timer.valid()
            && !g_client_bot_state.retaliation_timer.elapsed();
        const bool retaliation_target =
            retaliating_recently
            && enemy_target->handle == g_client_bot_state.retaliation_target_handle;
        const float enemy_dist = std::sqrt(std::max(
            rf::vec_dist_squared(&local_entity.pos, &enemy_target->pos),
            0.0f
        ));
        const float threat_distance = std::max(
            6.0f,
            personality.preferred_engagement_near * 1.15f);
        const bool immediate_threat = enemy_has_los || retaliation_target || enemy_dist <= threat_distance;
        if (immediate_threat) {
            const float threat_bonus = std::clamp(220.0f - enemy_dist * 18.0f, 45.0f, 220.0f);
            const float hold_defense_score = enemy_goal_score + threat_bonus;
            if (goal_score_wins_with_tie_break(hold_defense_score, selected_score)) {
                selected_goal = BotGoalType::eliminate_target;
                selected_handle = enemy_target->handle;
                selected_identifier = enemy_target->uid;
                selected_waypoint = 0;
                selected_pos = enemy_target->pos;
                selected_score = hold_defense_score;
            }
        }
    }

    if (enemy_target
        && control_point_enemy_defense_bonus > 0.0f
        && std::isfinite(enemy_goal_score)) {
        const float defense_enemy_score = enemy_goal_score + control_point_enemy_defense_bonus;
        if (goal_score_wins_with_tie_break(defense_enemy_score, selected_score)) {
            selected_goal = BotGoalType::eliminate_target;
            selected_handle = enemy_target->handle;
            selected_identifier = enemy_target->uid;
            selected_waypoint = 0;
            selected_pos = enemy_target->pos;
            selected_score = defense_enemy_score;
        }
    }

    if (deathmatch_mode
        && readiness_delta >= 0.0f
        && !enemy_target
        && alive_enemy_present
        && !respawn_gearup_active) {
        // In DM, if combat-ready but no lock, actively hunt for opponents.
        selected_goal = BotGoalType::eliminate_target;
        selected_handle = -1;
        selected_identifier = -1;
        selected_waypoint = 0;
        selected_pos = {};
        selected_score = std::max(selected_score, 0.0f);
    }

    if (deathmatch_mode
        && enemy_target
        && has_item_goal
        && bot_goal_is_item_collection(selected_goal)
        && readiness_delta >= 0.0f
        && !respawn_gearup_active) {
        const float keep_enemy_margin = std::lerp(22.0f, -8.0f, kill_focus_norm);
        if (enemy_goal_score >= item_goal_score - keep_enemy_margin) {
            selected_goal = BotGoalType::eliminate_target;
            selected_handle = enemy_target->handle;
            selected_identifier = enemy_target->uid;
            selected_waypoint = 0;
            selected_pos = enemy_target->pos;
            selected_score = enemy_goal_score;
        }
    }

    if (selected_goal == BotGoalType::none) {
        // No concrete tactical objective is currently viable; keep traversing the map.
        selected_goal = BotGoalType::roam;
        selected_handle = -1;
        selected_identifier = -1;
        selected_waypoint = 0;
        selected_pos = {};
        selected_score = std::max(selected_score, -5.0f);
    }

    float current_goal_score = -std::numeric_limits<float>::infinity();
    if (g_client_bot_state.active_goal == BotGoalType::eliminate_target
        && g_client_bot_state.goal_target_handle >= 0) {
        if (enemy_target
            && enemy_target->handle == g_client_bot_state.goal_target_handle) {
            current_goal_score = enemy_goal_score;
        }
        else if (rf::Entity* tracked_goal_enemy =
                     rf::entity_from_handle(g_client_bot_state.goal_target_handle);
                 tracked_goal_enemy
                 && tracked_goal_enemy != &local_entity
                 && !rf::entity_is_dying(tracked_goal_enemy)) {
            // Preserve finite score for the current eliminate target even when enemy
            // selection probes another candidate. This keeps same-goal hysteresis active
            // and prevents rapid handle swapping under near-equal scores.
            current_goal_score = bot_internal_compute_enemy_goal_score(
                local_entity,
                *tracked_goal_enemy,
                false
            );
        }
    }
    else if (g_client_bot_state.active_goal == BotGoalType::eliminate_target
        && g_client_bot_state.goal_target_handle < 0
        && deathmatch_mode
        && readiness_delta >= 0.0f) {
        current_goal_score = std::max(enemy_goal_score, 0.0f);
    }
    else if (bot_goal_is_item_collection(g_client_bot_state.active_goal)) {
        rf::Object* current_goal_obj = rf::obj_from_handle(g_client_bot_state.goal_target_handle);
        if (current_goal_obj
            && current_goal_obj->type == rf::OT_ITEM
            && bot_internal_is_collectible_goal_item(*static_cast<rf::Item*>(current_goal_obj))) {
            current_goal_score = item_goal_score;
            if (!(has_item_goal
                && item_goal.item_handle == g_client_bot_state.goal_target_handle)) {
                // Keep a valid in-progress pickup target unless there is a clearly better alternative.
                const float retarget_penalty =
                    std::lerp(95.0f, 30.0f, decision_skill)
                    * std::clamp(goal_commitment, 0.50f, 3.0f);
                current_goal_score -= retarget_penalty;
            }
        }
    }
    else if (g_client_bot_state.active_goal == BotGoalType::activate_bridge
        && has_bridge_goal
        && g_client_bot_state.goal_target_identifier == bridge_goal.zone_uid) {
        current_goal_score = bridge_goal_score + 10.0f;
    }
    else if (g_client_bot_state.active_goal == BotGoalType::create_crater
        && has_crater_goal
        && g_client_bot_state.goal_target_identifier == crater_goal_target_uid) {
        current_goal_score = crater_goal_score + 12.0f;
    }
    else if (g_client_bot_state.active_goal == BotGoalType::shatter_glass
        && has_shatter_goal
        && g_client_bot_state.goal_target_identifier == shatter_goal_target_uid) {
        current_goal_score = shatter_goal_score + 12.0f;
    }
    else if (g_client_bot_state.active_goal == BotGoalType::eliminate_target
        && ctf_goal.goal == BotGoalType::eliminate_target
        && g_client_bot_state.goal_target_handle == ctf_goal.handle) {
        current_goal_score = ctf_goal.score + 12.0f;
    }
    else if (bot_goal_is_ctf_objective(g_client_bot_state.active_goal)
        && ctf_goal.goal == g_client_bot_state.active_goal
        && g_client_bot_state.goal_target_identifier == ctf_goal.identifier) {
        current_goal_score = ctf_goal.score + 12.0f;
    }
    else if (bot_goal_is_control_point_objective(g_client_bot_state.active_goal)
        && std::isfinite(active_control_point_goal_score)) {
        current_goal_score = active_control_point_goal_score + 12.0f;
    }
    else if (bot_goal_is_item_collection(g_client_bot_state.active_goal)
        && has_crater_enabler_item_goal
        && g_client_bot_state.goal_target_handle == crater_enabler_item_goal.item_handle) {
        current_goal_score = crater_enabler_item_goal_score + 10.0f;
    }

    const bool committed_pickup_route = has_committed_pickup_route();
    if (committed_pickup_route && std::isfinite(current_goal_score)) {
        // Reduce score churn from slight distance changes while we are already
        // successfully progressing toward a pickup target.
        current_goal_score += 28.0f;
    }

    if (std::isfinite(current_goal_score)
        && g_client_bot_state.active_goal != BotGoalType::none
        && selected_goal != BotGoalType::none
        && selected_goal != g_client_bot_state.active_goal
        && !(ctf_force_flag_carrier_elimination
            && (selected_goal == BotGoalType::eliminate_target
                || selected_goal == BotGoalType::ctf_return_flag)
            && selected_handle == ctf_priority_flag_carrier_handle)) {
        float switch_margin = std::lerp(52.0f, 14.0f, decision_skill) * goal_commitment;
        if (g_client_bot_state.active_goal == BotGoalType::eliminate_target
            && bot_goal_is_item_collection(selected_goal)) {
            switch_margin *= std::lerp(1.05f, 2.40f, eliminate_commitment_norm);
        }
        if (deathmatch_mode
            && selected_goal == BotGoalType::eliminate_target
            && bot_goal_is_item_collection(g_client_bot_state.active_goal)
            && readiness_delta >= 0.0f) {
            switch_margin *= std::lerp(0.55f, 0.20f, kill_focus_norm);
        }
        if (respawn_gearup_active
            && bot_goal_is_item_collection(g_client_bot_state.active_goal)
            && selected_goal == BotGoalType::eliminate_target) {
            switch_margin += std::lerp(20.0f, 85.0f, respawn_gearup_priority);
        }
        if (committed_pickup_route
            && bot_goal_is_item_collection(g_client_bot_state.active_goal)
            && bot_goal_is_item_collection(selected_goal)) {
            // Keep following an in-progress pickup route unless the alternative
            // is clearly better, to avoid health/ammo flip-flop behavior.
            switch_margin *= 3.10f;
            switch_margin += 30.0f;
        }
        if (selected_score < current_goal_score + switch_margin) {
            selected_goal = g_client_bot_state.active_goal;
            selected_handle = g_client_bot_state.goal_target_handle;
            selected_identifier = g_client_bot_state.goal_target_identifier;
            selected_waypoint = g_client_bot_state.goal_target_waypoint;
            selected_pos = g_client_bot_state.goal_target_pos;
        }
    }

    if (std::isfinite(current_goal_score)
        && g_client_bot_state.active_goal != BotGoalType::none
        && selected_goal == g_client_bot_state.active_goal) {
        const bool same_goal_target_changed =
            selected_handle != g_client_bot_state.goal_target_handle
            || selected_identifier != g_client_bot_state.goal_target_identifier
            || selected_waypoint != g_client_bot_state.goal_target_waypoint;
        if (same_goal_target_changed) {
            float same_goal_switch_margin =
                std::lerp(70.0f, 18.0f, decision_skill) * goal_commitment;
            if (bot_goal_is_item_collection(selected_goal)) {
                const float opportunism_norm =
                    std::clamp((opportunism - 0.25f) / 2.25f, 0.0f, 1.0f);
                same_goal_switch_margin *= std::lerp(1.30f, 0.85f, opportunism_norm);
            }
            if (bot_goal_is_ctf_objective(selected_goal)) {
                same_goal_switch_margin *= 1.40f;
            }
            if (bot_goal_is_control_point_objective(selected_goal)) {
                same_goal_switch_margin *= 1.90f;
                same_goal_switch_margin += 22.0f;
            }
            if (g_client_bot_state.recovery_pending_reroute
                || g_client_bot_state.fsm_state == BotFsmState::recover_navigation) {
                // Recovery mode should avoid churn. Keep stronger commitment unless there is a
                // clearly better alternative.
                if (bot_goal_is_item_collection(selected_goal)) {
                    same_goal_switch_margin *= 2.20f;
                }
                else if (selected_goal == BotGoalType::eliminate_target) {
                    same_goal_switch_margin *= std::lerp(1.20f, 2.80f, eliminate_commitment_norm);
                    same_goal_switch_margin += std::lerp(18.0f, 70.0f, eliminate_commitment_norm);
                }
                else {
                    same_goal_switch_margin *= 0.75f;
                }
            }
            if (committed_pickup_route && bot_goal_is_item_collection(selected_goal)) {
                same_goal_switch_margin *= 2.75f;
                same_goal_switch_margin += 24.0f;
            }

            if (selected_score < current_goal_score + same_goal_switch_margin) {
                selected_handle = g_client_bot_state.goal_target_handle;
                selected_identifier = g_client_bot_state.goal_target_identifier;
                selected_waypoint = g_client_bot_state.goal_target_waypoint;
                selected_pos = g_client_bot_state.goal_target_pos;
            }
        }
    }

    if (g_client_bot_state.goal_switch_lock_timer.valid()
        && g_client_bot_state.goal_switch_lock_timer.elapsed()) {
        g_client_bot_state.goal_switch_lock_timer.invalidate();
    }
    if (std::isfinite(current_goal_score)
        && g_client_bot_state.active_goal != BotGoalType::none
        && selected_goal != BotGoalType::none
        && selected_goal != g_client_bot_state.active_goal
        && g_client_bot_state.goal_switch_lock_timer.valid()) {
        const bool switch_to_critical_objective =
            bot_goal_is_ctf_objective(selected_goal)
            || bot_goal_is_control_point_objective(selected_goal);
        const bool switch_to_immediate_enemy_threat =
            selected_goal == BotGoalType::eliminate_target
            && enemy_target
            && (enemy_has_los || retaliation_matches_enemy);
        const float lock_margin = std::clamp(
            std::lerp(145.0f, 52.0f, decision_skill)
                * std::clamp(goal_commitment, 0.55f, 2.40f),
            35.0f,
            220.0f
        );
        if (!switch_to_critical_objective
            && !switch_to_immediate_enemy_threat
            && selected_score < current_goal_score + lock_margin) {
            selected_goal = g_client_bot_state.active_goal;
            selected_handle = g_client_bot_state.goal_target_handle;
            selected_identifier = g_client_bot_state.goal_target_identifier;
            selected_waypoint = g_client_bot_state.goal_target_waypoint;
            selected_pos = g_client_bot_state.goal_target_pos;
        }
    }

    const bool goal_identity_changed =
        selected_goal != g_client_bot_state.active_goal
        || selected_handle != g_client_bot_state.goal_target_handle
        || selected_identifier != g_client_bot_state.goal_target_identifier;
    const bool waypoint_anchor_changed =
        selected_waypoint != g_client_bot_state.goal_target_waypoint;
    const bool recovering_navigation =
        g_client_bot_state.recovery_pending_reroute
        || g_client_bot_state.fsm_state == BotFsmState::recover_navigation;
    const bool suppress_anchor_hard_change_for_item =
        bot_goal_is_item_collection(selected_goal)
        && selected_goal == g_client_bot_state.active_goal
        && selected_handle == g_client_bot_state.goal_target_handle
        && selected_identifier == g_client_bot_state.goal_target_identifier;
    const bool hard_goal_change =
        goal_identity_changed
        || (waypoint_anchor_changed
            && recovering_navigation
            && !suppress_anchor_hard_change_for_item);

    if (hard_goal_change) {
        bot_internal_set_last_heading_change_reason("goal_changed");
        bot_state_clear_waypoint_route(true, true, true);
        g_client_bot_state.pursuit_route_failures = 0;
        g_client_bot_state.eliminate_target_reacquire_timer.invalidate();
        g_client_bot_state.crater_goal_abort_timer.invalidate();
        g_client_bot_state.shatter_goal_abort_timer.invalidate();
        if (selected_goal != BotGoalType::create_crater) {
            g_client_bot_state.crater_goal_timeout_timer.invalidate();
        }
        if (selected_goal != BotGoalType::shatter_glass) {
            g_client_bot_state.shatter_goal_timeout_timer.invalidate();
        }
        if (selected_goal != BotGoalType::control_point_objective) {
            g_client_bot_state.control_point_route_fail_timer.invalidate();
            g_client_bot_state.control_point_patrol_waypoint = 0;
            g_client_bot_state.control_point_patrol_timer.invalidate();
        }
        else if (goal_identity_changed) {
            g_client_bot_state.control_point_patrol_waypoint = 0;
            g_client_bot_state.control_point_patrol_timer.invalidate();
        }
        const int goal_switch_lock_ms = std::clamp(
            static_cast<int>(std::lround(
                std::lerp(980.0f, 360.0f, decision_skill)
                / std::max(decision_efficiency, 0.25f)
                * std::clamp(goal_commitment, 0.65f, 1.85f)
            )),
            kGoalSwitchLockMinMs,
            kGoalSwitchLockMaxMs
        );
        g_client_bot_state.goal_switch_lock_timer.set(goal_switch_lock_ms);
    }

    const bool item_goal_selection_changed =
        bot_goal_is_item_collection(selected_goal)
        && selected_handle >= 0
        && (selected_goal != g_client_bot_state.active_goal
            || selected_handle != g_client_bot_state.goal_target_handle);
    if (item_goal_selection_changed) {
        bot_internal_note_item_goal_selection(selected_handle, selected_goal);
    }

    bot_state_set_goal(
        selected_goal,
        selected_handle,
        selected_identifier,
        selected_waypoint,
        selected_pos
    );
    int next_goal_eval_ms = goal_eval_ms;
    if (hard_goal_change) {
        // After committing to a new objective, hold it briefly to prevent
        // near-equal alternatives from causing rapid oscillation.
        constexpr int kPostCommitGoalEvalFloorMs = 700;
        next_goal_eval_ms = std::max(next_goal_eval_ms, kPostCommitGoalEvalFloorMs);
    }
    g_client_bot_state.goal_eval_timer.set(next_goal_eval_ms);
    if (selected_goal == BotGoalType::create_crater) {
        if (hard_goal_change || !g_client_bot_state.crater_goal_timeout_timer.valid()) {
            g_client_bot_state.crater_goal_timeout_timer.set(15000);
        }
    }
    else {
        g_client_bot_state.crater_goal_timeout_timer.invalidate();
    }
    if (selected_goal == BotGoalType::shatter_glass) {
        if (hard_goal_change || !g_client_bot_state.shatter_goal_timeout_timer.valid()) {
            g_client_bot_state.shatter_goal_timeout_timer.set(15000);
        }
    }
    else {
        g_client_bot_state.shatter_goal_timeout_timer.invalidate();
    }
}
