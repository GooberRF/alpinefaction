#include "bot_navigation.h"

#include "bot_combat.h"
#include "bot_decision_eval.h"
#include "bot_fsm.h"
#include "bot_goal_runtime.h"
#include "bot_internal.h"
#include "bot_math.h"
#include "bot_memory_manager.h"
#include "bot_navigation_pathing.h"
#include "bot_state.h"
#include "bot_utils.h"
#include "bot_waypoint_route.h"
#include "../gametype.h"
#include "../../rf/multi.h"
#include "../../rf/player/player.h"
#include "../../rf/trigger.h"
#include "../../rf/weapon.h"
#include <array>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace
{
int scale_repath_ms(const int base_ms, const bool urgent);
constexpr int kItemGoalContactTimeoutMs = 1200;
constexpr float kItemGoalContactRadius = 1.20f;
constexpr float kRecoveryAnchorFallbackHorizontalDeadlockRadius = 0.85f;
constexpr float kRecoveryAnchorFallbackVerticalDeadlockHeight = 1.6f;

void abandon_eliminate_goal_for_forage(const char* heading_change_reason)
{
    if (g_client_bot_state.active_goal != BotGoalType::eliminate_target) {
        return;
    }

    // Forage transition should never leave the bot with no objective.
    // Use centralized fallback plumbing to keep transition behavior consistent.
    // For forage fallback, immediately refresh goals next frame so we don't
    // stall in a temporary roam placeholder before committing to the next
    // best objective.
    bot_state_set_roam_fallback_goal(0);
    g_client_bot_state.eliminate_target_reacquire_timer.invalidate();

    g_client_bot_state.pursuit_target_handle = -1;
    g_client_bot_state.pursuit_route_failures = 0;
    g_client_bot_state.pursuit_recovery_timer.set(std::clamp(
        scale_repath_ms(kPursuitRecoveryMinMs, true),
        kPursuitRecoveryMinMs,
        kPursuitRecoveryMaxMs
    ));

    bot_state_clear_waypoint_route(true, true, false);
    bot_internal_set_last_heading_change_reason(heading_change_reason);
}

int scale_repath_ms(const int base_ms, const bool urgent = false)
{
    const float decision_skill = bot_get_decision_skill_factor();
    const BotPersonality& personality = get_active_bot_personality();
    const float efficiency = std::clamp(personality.decision_efficiency_bias, 0.35f, 2.25f);

    float scale = std::lerp(
        1.30f,
        0.72f,
        std::clamp(decision_skill * std::sqrt(efficiency), 0.0f, 1.0f)
    );

    switch (g_client_bot_state.fsm_state) {
        case BotFsmState::recover_navigation:
            scale *= 0.55f;
            break;
        case BotFsmState::engage_enemy:
        case BotFsmState::pursue_enemy:
        case BotFsmState::seek_enemy:
            scale *= 0.82f;
            break;
        case BotFsmState::collect_pickup:
            scale *= 0.90f;
            break;
        case BotFsmState::retreat:
            scale *= 0.68f;
            break;
        case BotFsmState::seek_weapon:
        case BotFsmState::replenish_health_armor:
            scale *= 0.78f;
            break;
        case BotFsmState::find_power_position:
            scale *= 0.86f;
            break;
        case BotFsmState::activate_bridge:
            scale *= 0.60f;
            break;
        case BotFsmState::create_crater:
            scale *= 0.65f;
            break;
        case BotFsmState::shatter_glass:
            scale *= 0.65f;
            break;
        case BotFsmState::ctf_objective:
            scale *= 0.70f;
            break;
        case BotFsmState::control_point_objective:
            scale *= 0.72f;
            break;
        case BotFsmState::roam:
            scale *= 1.05f;
            break;
        default:
            break;
    }

    if (urgent) {
        scale *= 0.8f;
    }

    const int scaled_ms = static_cast<int>(std::lround(static_cast<float>(base_ms) * scale));
    return std::clamp(scaled_ms, 120, std::max(120, base_ms * 2));
}

float get_entity_health_ratio(const rf::Entity& entity)
{
    return bot_decision_get_entity_health_ratio(entity);
}

float get_entity_armor_ratio(const rf::Entity& entity)
{
    return bot_decision_get_entity_armor_ratio(entity);
}

int bot_find_closest_waypoint_with_fallback(const rf::Vector3& pos)
{
    float radius = kWaypointSearchRadius;
    for (int pass = 0; pass < 6; ++pass) {
        if (const int waypoint = waypoints_closest(pos, radius); waypoint > 0) {
            return waypoint;
        }
        radius *= 2.0f;
    }
    return 0;
}

bool find_reachable_bridge_zone_waypoint(
    const rf::Entity& local_entity,
    const int zone_uid,
    const int preferred_waypoint,
    int& out_waypoint,
    rf::Vector3& out_pos)
{
    out_waypoint = 0;
    out_pos = {};
    if (zone_uid < 0) {
        return false;
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

    std::vector<Candidate> candidates{};
    candidates.reserve(32);
    const int waypoint_total = waypoints_count();
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
        return true;
    }

    return false;
}

bool find_reachable_crater_target_waypoint(
    const rf::Entity& local_entity,
    const WaypointTargetDefinition& target,
    const int preferred_waypoint,
    int& out_waypoint,
    rf::Vector3& out_pos)
{
    out_waypoint = 0;
    out_pos = {};

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
        rf::Vector3 fallback_pos{};
        if (fallback_waypoint > 0 && waypoints_get_pos(fallback_waypoint, fallback_pos)) {
            candidates.push_back(Candidate{
                fallback_waypoint,
                fallback_pos,
                rf::vec_dist_squared(&local_entity.pos, &fallback_pos),
            });
        }
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
        out_pos = candidate.pos;
        return true;
    }

    return false;
}

bool find_reachable_waypoint_near_position(
    const rf::Entity& local_entity,
    const rf::Vector3& target_pos,
    const int preferred_waypoint,
    int& out_waypoint,
    rf::Vector3& out_pos,
    const bool exclude_non_temporary_ctf_flag = false,
    const int excluded_ctf_flag_subtype = -1)
{
    out_waypoint = 0;
    out_pos = {};

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
    candidates.reserve(128);
    const int waypoint_total = waypoints_count();
    for (int waypoint = 1; waypoint < waypoint_total; ++waypoint) {
        if (exclude_non_temporary_ctf_flag) {
            int type_raw = 0;
            int subtype = 0;
            if (waypoints_get_type_subtype(waypoint, type_raw, subtype)
                && static_cast<WaypointType>(type_raw) == WaypointType::ctf_flag
                && (excluded_ctf_flag_subtype < 0 || subtype == excluded_ctf_flag_subtype)) {
                bool temporary = false;
                if (!waypoints_get_temporary(waypoint, temporary) || !temporary) {
                    continue;
                }
            }
        }

        rf::Vector3 waypoint_pos{};
        if (!waypoints_get_pos(waypoint, waypoint_pos)) {
            continue;
        }
        candidates.push_back(Candidate{
            waypoint,
            waypoint_pos,
            rf::vec_dist_squared(&waypoint_pos, &target_pos),
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
    constexpr int kRouteProbeLimit = 48;
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
        return true;
    }

    return false;
}

bool find_nearest_ctf_flag_waypoint(
    const rf::Vector3& from_pos,
    const bool red_flag,
    int& out_waypoint,
    rf::Vector3& out_waypoint_pos)
{
    out_waypoint = 0;
    out_waypoint_pos = {};
    const int desired_subtype = red_flag
        ? static_cast<int>(WaypointCtfFlagSubtype::red)
        : static_cast<int>(WaypointCtfFlagSubtype::blue);
    float best_dist_sq = std::numeric_limits<float>::infinity();
    bool found = false;

    const int waypoint_total = waypoints_count();
    for (int waypoint_uid = 1; waypoint_uid < waypoint_total; ++waypoint_uid) {
        int type_raw = 0;
        int subtype = 0;
        rf::Vector3 waypoint_pos{};
        if (!waypoints_get_type_subtype(waypoint_uid, type_raw, subtype)
            || static_cast<WaypointType>(type_raw) != WaypointType::ctf_flag
            || subtype != desired_subtype
            || !waypoints_get_pos(waypoint_uid, waypoint_pos)) {
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
    out_waypoint = 0;
    out_waypoint_pos = {};
    float best_dist_sq = std::numeric_limits<float>::infinity();
    bool found = false;

    const int team_subtype = team_is_red
        ? static_cast<int>(WaypointRespawnSubtype::red_team)
        : static_cast<int>(WaypointRespawnSubtype::blue_team);

    const auto try_respawn_subtype = [&](const int subtype) {
        const int waypoint_total = waypoints_count();
        for (int waypoint_uid = 1; waypoint_uid < waypoint_total; ++waypoint_uid) {
            int type_raw = 0;
            int waypoint_subtype = 0;
            rf::Vector3 waypoint_pos{};
            if (!waypoints_get_type_subtype(waypoint_uid, type_raw, waypoint_subtype)
                || static_cast<WaypointType>(type_raw) != WaypointType::respawn
                || waypoint_subtype != subtype
                || !waypoints_get_pos(waypoint_uid, waypoint_pos)) {
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
    };

    try_respawn_subtype(team_subtype);
    if (!found) {
        try_respawn_subtype(static_cast<int>(WaypointRespawnSubtype::all_teams));
    }
    return found;
}

bool point_inside_bridge_trigger(const int trigger_uid, const rf::Vector3& point)
{
    rf::Object* trigger_obj = rf::obj_lookup_from_uid(trigger_uid);
    if (!trigger_obj || trigger_obj->type != rf::OT_TRIGGER) {
        return false;
    }

    const auto* trigger = static_cast<rf::Trigger*>(trigger_obj);
    if (trigger->type == 1) {
        const rf::Vector3 half_extents{
            std::fabs(trigger->box_size.x) * 0.5f,
            std::fabs(trigger->box_size.y) * 0.5f,
            std::fabs(trigger->box_size.z) * 0.5f,
        };
        const rf::Vector3 delta = point - trigger->pos;
        const float local_x = std::fabs(delta.dot_prod(trigger->orient.rvec));
        const float local_y = std::fabs(delta.dot_prod(trigger->orient.uvec));
        const float local_z = std::fabs(delta.dot_prod(trigger->orient.fvec));
        return local_x <= half_extents.x
            && local_y <= half_extents.y
            && local_z <= half_extents.z;
    }

    const float radius = std::fabs(trigger->radius);
    if (radius <= 0.0f) {
        return false;
    }
    return rf::vec_dist_squared(&point, &trigger->pos) <= radius * radius;
}

bool is_ctf_mode()
{
    return rf::is_multi && rf::multi_get_game_type() == rf::NG_TYPE_CTF;
}

bool is_control_point_mode()
{
    return rf::is_multi && multi_is_game_type_with_hills();
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

bool find_reachable_control_point_zone_waypoint(
    const rf::Entity& local_entity,
    const int handler_uid,
    const int preferred_waypoint,
    int& out_waypoint,
    rf::Vector3& out_pos)
{
    int zone_uid = -1;
    if (!waypoints_get_control_point_zone_uid(handler_uid, zone_uid)) {
        out_waypoint = 0;
        out_pos = {};
        return false;
    }

    return find_reachable_bridge_zone_waypoint(
        local_entity,
        zone_uid,
        preferred_waypoint,
        out_waypoint,
        out_pos);
}

bool find_control_point_patrol_waypoint(
    const rf::Entity& local_entity,
    const int handler_uid,
    const int avoid_waypoint,
    int& out_waypoint,
    rf::Vector3& out_pos)
{
    out_waypoint = 0;
    out_pos = {};

    int zone_uid = -1;
    if (!waypoints_get_control_point_zone_uid(handler_uid, zone_uid)) {
        return false;
    }

    struct Candidate
    {
        int waypoint = 0;
        rf::Vector3 pos{};
        float dist_sq = 0.0f;
    };

    std::vector<Candidate> candidates{};
    candidates.reserve(16);
    const int waypoint_total = waypoints_count();
    for (int waypoint = 1; waypoint < waypoint_total; ++waypoint) {
        if (!waypoints_waypoint_has_zone(waypoint, zone_uid)) {
            continue;
        }
        if (waypoint == avoid_waypoint) {
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
        [](const Candidate& lhs, const Candidate& rhs) {
            return lhs.dist_sq > rhs.dist_sq;
        });

    const int start_waypoint = bot_find_closest_waypoint_with_fallback(local_entity.pos);
    static const std::vector<int> kEmptyAvoidset{};
    std::vector<int> path{};
    path.reserve(64);
    constexpr int kRouteProbeLimit = 12;
    int route_checks = 0;

    for (const Candidate& candidate : candidates) {
        if (route_checks++ >= kRouteProbeLimit) {
            break;
        }

        const bool reachable = start_waypoint <= 0
            || candidate.waypoint == start_waypoint
            || bot_waypoint_route(start_waypoint, candidate.waypoint, kEmptyAvoidset, path);
        if (!reachable) {
            continue;
        }

        out_waypoint = candidate.waypoint;
        out_pos = candidate.pos;
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

bool contains_case_insensitive(std::string_view haystack, std::string_view needle)
{
    if (needle.empty() || haystack.empty() || needle.size() > haystack.size()) {
        return false;
    }
    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(haystack[i + j])));
            const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(needle[j])));
            if (a != b) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

bool equals_case_insensitive(std::string_view lhs, std::string_view rhs)
{
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (size_t index = 0; index < lhs.size(); ++index) {
        const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(lhs[index])));
        const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(rhs[index])));
        if (a != b) {
            return false;
        }
    }
    return true;
}

const rf::ItemInfo* resolve_item_info(const rf::Item& item)
{
    if (item.info) {
        return item.info;
    }
    if (item.info_index >= 0 && item.info_index < rf::num_item_types) {
        return &rf::item_info[item.info_index];
    }
    return nullptr;
}

int resolve_item_type_index(const rf::Item& item, const rf::ItemInfo* item_info)
{
    if (item.info_index >= 0 && item.info_index < rf::num_item_types) {
        return item.info_index;
    }
    if (!item_info || rf::num_item_types <= 0) {
        return -1;
    }
    const rf::ItemInfo* first = &rf::item_info[0];
    const rf::ItemInfo* last = first + rf::num_item_types;
    if (item_info >= first && item_info < last) {
        return static_cast<int>(item_info - first);
    }
    return -1;
}

bool item_is_currently_available(const rf::Item& item)
{
    return (item.obj_flags & rf::OF_HIDDEN) == 0;
}

bool item_info_contains_token(const rf::ItemInfo& item_info, std::string_view token)
{
    return contains_case_insensitive(item_info.cls_name.c_str(), token)
        || (item_info.hud_msg_name
            && contains_case_insensitive(item_info.hud_msg_name, token));
}

bool item_is_health_pickup(const rf::ItemInfo& item_info)
{
    return item_info_contains_token(item_info, "health")
        || item_info_contains_token(item_info, "med")
        || item_info_contains_token(item_info, "kit");
}

bool item_is_armor_pickup(const rf::ItemInfo& item_info)
{
    return item_info_contains_token(item_info, "armor")
        || item_info_contains_token(item_info, "envirosuit")
        || item_info_contains_token(item_info, "suit repair");
}

enum class ReplenishItemKind
{
    none = 0,
    health = 1,
    armor = 2,
};

struct ReplenishItemProfile
{
    ReplenishItemKind kind = ReplenishItemKind::none;
    int amount = 0;
    int value_tier = 0;
    bool is_super = false;
};

bool try_apply_named_replenish_profile(
    std::string_view name,
    ReplenishItemProfile& out_profile)
{
    struct NamedProfile
    {
        const char* name;
        ReplenishItemKind kind;
        int amount;
        int value_tier;
        bool is_super;
    };

    static constexpr std::array<NamedProfile, 6> kProfiles{{
        {"Multi Super Health", ReplenishItemKind::health, 200, 4, true},
        {"Multi Super Armor", ReplenishItemKind::armor, 200, 5, true},
        {"Suit Repair", ReplenishItemKind::armor, 50, 2, false},
        {"Medical Kit", ReplenishItemKind::health, 50, 1, false},
        {"First Aid Kit", ReplenishItemKind::health, 25, 0, false},
        {"Miner Envirosuit", ReplenishItemKind::armor, 100, 3, false},
    }};

    for (const NamedProfile& profile : kProfiles) {
        if (equals_case_insensitive(name, profile.name)) {
            out_profile.kind = profile.kind;
            out_profile.amount = profile.amount;
            out_profile.value_tier = profile.value_tier;
            out_profile.is_super = profile.is_super;
            return true;
        }
    }

    if (contains_case_insensitive(name, "super")
        && contains_case_insensitive(name, "health")) {
        out_profile = {ReplenishItemKind::health, 200, 4, true};
        return true;
    }
    if (contains_case_insensitive(name, "super")
        && contains_case_insensitive(name, "armor")) {
        out_profile = {ReplenishItemKind::armor, 200, 5, true};
        return true;
    }
    if (contains_case_insensitive(name, "suit")
        && contains_case_insensitive(name, "repair")) {
        out_profile = {ReplenishItemKind::armor, 50, 2, false};
        return true;
    }
    if (contains_case_insensitive(name, "medical")
        && contains_case_insensitive(name, "kit")) {
        out_profile = {ReplenishItemKind::health, 50, 1, false};
        return true;
    }
    if (contains_case_insensitive(name, "first")
        && contains_case_insensitive(name, "aid")) {
        out_profile = {ReplenishItemKind::health, 25, 0, false};
        return true;
    }
    if (contains_case_insensitive(name, "envirosuit")) {
        out_profile = {ReplenishItemKind::armor, 100, 3, false};
        return true;
    }
    return false;
}

bool resolve_replenish_item_profile(
    const rf::Item& item,
    const rf::ItemInfo& item_info,
    ReplenishItemProfile& out_profile)
{
    out_profile = {};
    const auto try_name = [&](const char* raw_name) {
        return raw_name && raw_name[0]
            && try_apply_named_replenish_profile(raw_name, out_profile);
    };

    if (try_name(item.name.c_str())
        || try_name(item_info.hud_msg_name)
        || try_name(item_info.cls_name.c_str())) {
        return true;
    }

    if (item_is_health_pickup(item_info) || item_is_armor_pickup(item_info)) {
        const bool is_health = item_is_health_pickup(item_info);
        const int amount = std::max(
            25,
            std::max(item_info.count_multi, item_info.count)
        );
        out_profile.kind = is_health
            ? ReplenishItemKind::health
            : ReplenishItemKind::armor;
        out_profile.amount = amount;
        out_profile.value_tier = std::clamp(amount / 50, 0, 4);
        out_profile.is_super = amount >= 150;
        return true;
    }

    return false;
}

enum class ContextualGoalMode
{
    seek_weapon = 0,
    replenish_survivability = 1,
};

bool find_best_contextual_item_goal(
    const rf::Entity& local_entity,
    const ContextualGoalMode mode,
    int& out_item_handle,
    int& out_waypoint,
    rf::Vector3& out_pos)
{
    out_item_handle = -1;
    out_waypoint = 0;
    out_pos = {};

    const BotPersonality& personality = get_active_bot_personality();
    const BotSkillProfile& skill_profile = get_active_bot_skill_profile();
    const float health_ratio = get_entity_health_ratio(local_entity);
    const float armor_ratio = get_entity_armor_ratio(local_entity);
    const float decision_skill = bot_get_decision_skill_factor();
    const float seek_weapon_bias = std::clamp(personality.seek_weapon_bias, 0.25f, 2.5f);
    const float replenish_bias = std::clamp(personality.replenish_bias, 0.25f, 2.5f);
    const float maintenance_bias = std::clamp(
        skill_profile.survivability_maintenance_bias,
        0.25f,
        2.5f
    );

    float best_score = -std::numeric_limits<float>::infinity();
    for (rf::Object* obj = rf::object_list.next_obj; obj != &rf::object_list; obj = obj->next_obj) {
        if (obj->type != rf::OT_ITEM) {
            continue;
        }
        const auto* item = static_cast<const rf::Item*>(obj);
        if (!item || !item_is_currently_available(*item)) {
            continue;
        }
        if (!bot_internal_is_collectible_goal_item(*item)) {
            continue;
        }

        const rf::ItemInfo* item_info = resolve_item_info(*item);
        if (!item_info || (item_info->flags & rf::IIF_NO_PICKUP)) {
            continue;
        }
        const int item_type = resolve_item_type_index(*item, item_info);
        int goal_waypoint = 0;
        if (!bot_internal_find_item_goal_waypoint(item->pos, goal_waypoint)) {
            continue;
        }

        const bool is_weapon_pickup = item_info->gives_weapon_id >= 0;
        const bool is_ammo_pickup = item_info->ammo_for_weapon_id >= 0;

        float desirability = (item_type >= 0) ? bot_get_item_weight(item_type) : 1.0f;
        if (mode == ContextualGoalMode::seek_weapon) {
            if (!is_weapon_pickup && !is_ammo_pickup) {
                continue;
            }

            if (is_weapon_pickup) {
                const int weapon_type = item_info->gives_weapon_id;
                const float pickup_weight = bot_get_weapon_pickup_weight(weapon_type);
                const float preference_weight = bot_get_weapon_preference_weight(weapon_type);
                const bool already_has_weapon =
                    weapon_type >= 0
                    && weapon_type < rf::num_weapon_types
                    && local_entity.ai.has_weapon[weapon_type];
                const float novelty_bonus = already_has_weapon ? 0.85f : 2.45f;
                desirability += novelty_bonus * pickup_weight * preference_weight * seek_weapon_bias;
            }
            if (is_ammo_pickup) {
                const int weapon_type = item_info->ammo_for_weapon_id;
                const bool already_has_weapon =
                    weapon_type >= 0
                    && weapon_type < rf::num_weapon_types
                    && local_entity.ai.has_weapon[weapon_type];
                const float ammo_weight = bot_get_weapon_pickup_weight(weapon_type);
                desirability += (already_has_weapon ? 0.95f : 0.20f) * ammo_weight * seek_weapon_bias;
            }
        }
        else {
            ReplenishItemProfile profile{};
            if (!resolve_replenish_item_profile(*item, *item_info, profile)) {
                continue;
            }
            if (!profile.is_super) {
                if (profile.kind == ReplenishItemKind::health && health_ratio >= 1.0f) {
                    continue;
                }
                if (profile.kind == ReplenishItemKind::armor && armor_ratio >= 1.0f) {
                    continue;
                }
            }

            const float health_need = std::clamp(1.0f - health_ratio, 0.0f, 1.0f);
            const float armor_need = std::clamp(1.0f - armor_ratio, 0.0f, 1.0f);
            const float low_health_pressure = std::clamp((0.55f - health_ratio) / 0.55f, 0.0f, 1.0f);
            const float amount_units = std::clamp(static_cast<float>(profile.amount) / 25.0f, 0.75f, 8.0f);

            float replenish_score = amount_units * 4.0f + static_cast<float>(profile.value_tier) * 2.8f;
            if (profile.kind == ReplenishItemKind::health) {
                const float health_priority = (0.25f + health_need * 2.6f)
                    * std::lerp(1.0f, 2.7f, low_health_pressure);
                replenish_score *= health_priority;
                if (profile.is_super && health_ratio <= 1.0f) {
                    replenish_score += 8.0f;
                }
            }
            else {
                const float armor_priority = (0.25f + armor_need * 2.15f)
                    * std::lerp(1.3f, 0.60f, low_health_pressure);
                replenish_score *= armor_priority;
                if (profile.is_super && armor_ratio <= 1.0f) {
                    replenish_score += 6.0f;
                }
            }

            if (health_ratio < 0.35f && profile.kind == ReplenishItemKind::armor) {
                replenish_score *= 0.55f;
            }
            if (health_ratio < 0.35f && profile.kind == ReplenishItemKind::health) {
                replenish_score += 10.0f;
            }

            replenish_score *= std::lerp(
                0.80f,
                1.35f,
                std::clamp((maintenance_bias - 0.25f) / 2.25f, 0.0f, 1.0f)
            );
            desirability += replenish_score * replenish_bias;
        }

        const float distance = std::sqrt(
            std::max(rf::vec_dist_squared(&local_entity.pos, &item->pos), 0.0f)
        );
        const float distance_penalty = (mode == ContextualGoalMode::replenish_survivability)
            ? std::lerp(2.2f, 1.4f, decision_skill)
            : 1.0f;
        const float desirability_scale = (mode == ContextualGoalMode::replenish_survivability)
            ? 20.0f
            : 46.0f;
        const float score = desirability * desirability_scale - distance * distance_penalty;
        if (score > best_score) {
            best_score = score;
            out_item_handle = item->handle;
            out_waypoint = goal_waypoint;
            out_pos = item->pos;
        }
    }

    return out_item_handle >= 0 && out_waypoint > 0;
}

bool update_contextual_item_goal(
    const rf::Entity& local_entity,
    const ContextualGoalMode mode,
    rf::Vector3& move_target,
    bool& has_move_target)
{
    bool need_refresh =
        g_client_bot_state.contextual_goal_item_handle < 0
        || g_client_bot_state.contextual_goal_waypoint <= 0
        || !g_client_bot_state.contextual_goal_eval_timer.valid()
        || g_client_bot_state.contextual_goal_eval_timer.elapsed();

    if (!need_refresh) {
        rf::Object* goal_obj = rf::obj_from_handle(g_client_bot_state.contextual_goal_item_handle);
        if (!goal_obj || goal_obj->type != rf::OT_ITEM) {
            need_refresh = true;
        }
        else {
            const auto* goal_item = static_cast<const rf::Item*>(goal_obj);
            const rf::ItemInfo* goal_info = goal_item ? resolve_item_info(*goal_item) : nullptr;
            if (!goal_item
                || !goal_info
                || !item_is_currently_available(*goal_item)
                || !bot_internal_is_collectible_goal_item(*goal_item)) {
                need_refresh = true;
            }
            else if (mode == ContextualGoalMode::seek_weapon) {
                if (goal_info->gives_weapon_id < 0 && goal_info->ammo_for_weapon_id < 0) {
                    need_refresh = true;
                }
            }
            else {
                ReplenishItemProfile profile{};
                if (!resolve_replenish_item_profile(*goal_item, *goal_info, profile)) {
                    need_refresh = true;
                }
            }

            if (!need_refresh) {
                g_client_bot_state.contextual_goal_pos = goal_item->pos;
                if (!bot_internal_find_item_goal_waypoint(
                        goal_item->pos,
                        g_client_bot_state.contextual_goal_waypoint)) {
                    need_refresh = true;
                }
            }
        }
    }

    if (need_refresh) {
        if (!find_best_contextual_item_goal(
                local_entity,
                mode,
                g_client_bot_state.contextual_goal_item_handle,
                g_client_bot_state.contextual_goal_waypoint,
                g_client_bot_state.contextual_goal_pos)) {
            g_client_bot_state.contextual_goal_item_handle = -1;
            g_client_bot_state.contextual_goal_waypoint = 0;
            g_client_bot_state.contextual_goal_pos = {};
            g_client_bot_state.contextual_goal_eval_timer.invalidate();
            return false;
        }

        const int eval_ms = (mode == ContextualGoalMode::seek_weapon)
            ? scale_repath_ms(650)
            : scale_repath_ms(520);
        g_client_bot_state.contextual_goal_eval_timer.set(eval_ms);
    }

    if (g_client_bot_state.contextual_goal_waypoint <= 0) {
        return false;
    }

    const float item_dist_sq = rf::vec_dist_squared(
        &local_entity.pos,
        &g_client_bot_state.contextual_goal_pos
    );
    if (item_dist_sq <= kWaypointLinkRadius * kWaypointLinkRadius) {
        move_target = g_client_bot_state.contextual_goal_pos;
        has_move_target = true;
        return true;
    }

    const int repath_ms = (mode == ContextualGoalMode::seek_weapon)
        ? scale_repath_ms(kItemRouteRepathMs)
        : scale_repath_ms(kWaypointRecoveryRepathMs, true);
    const bool routed = bot_internal_update_waypoint_target_towards(
        local_entity,
        g_client_bot_state.contextual_goal_pos,
        nullptr,
        nullptr,
        repath_ms
    );
    if (!routed) {
        g_client_bot_state.contextual_goal_item_handle = -1;
        g_client_bot_state.contextual_goal_waypoint = 0;
        g_client_bot_state.contextual_goal_pos = {};
        return false;
    }

    move_target = g_client_bot_state.waypoint_target_pos;
    has_move_target = true;
    return true;
}

bool update_retreat_goal(
    const rf::Entity& local_entity,
    const rf::Entity* enemy_target,
    rf::Vector3& move_target,
    bool& has_move_target)
{
    if (!enemy_target) {
        return false;
    }

    rf::Vector3 away = local_entity.pos - enemy_target->pos;
    away.y = 0.0f;
    if (away.len_sq() < 0.001f) {
        away = rf::Vector3{0.0f, 0.0f, 1.0f};
    }
    away.normalize_safe();

    const float risk_tolerance = std::clamp(
        get_active_bot_personality().decision_risk_tolerance,
        0.25f,
        2.5f
    );
    const float retreat_distance = std::lerp(
        26.0f,
        12.0f,
        std::clamp(risk_tolerance * 0.5f, 0.0f, 1.0f)
    );
    const rf::Vector3 retreat_destination = local_entity.pos + away * retreat_distance;
    const bool routed = bot_internal_update_waypoint_target_towards(
        local_entity,
        retreat_destination,
        nullptr,
        enemy_target,
        scale_repath_ms(kWaypointRecoveryRepathMs, true)
    );
    if (!routed) {
        return false;
    }

    move_target = g_client_bot_state.waypoint_target_pos;
    has_move_target = true;
    return true;
}

bool update_power_position_goal(
    const rf::Entity& local_entity,
    const rf::Entity* enemy_target,
    const bool enemy_has_los,
    rf::Vector3& move_target,
    bool& has_move_target)
{
    if (!enemy_target) {
        return false;
    }

    bool routed = bot_internal_update_waypoint_target_for_local_los_reposition(
        local_entity,
        *enemy_target,
        enemy_has_los
    );
    if (!routed) {
        routed = bot_internal_update_waypoint_target_towards(
            local_entity,
            enemy_target->pos,
            &enemy_target->eye_pos,
            enemy_target,
            scale_repath_ms(kPursuitRepathIntervalMs)
        );
    }
    if (!routed) {
        return false;
    }

    move_target = g_client_bot_state.waypoint_target_pos;
    has_move_target = true;
    return true;
}

void clear_bridge_goal_runtime_state()
{
    bot_goal_runtime_clear_bridge_goal_state();
}

void clear_bridge_post_open_priority_state()
{
    bot_goal_runtime_clear_bridge_post_open_priority_state();
}

void prime_bridge_post_open_priority(const int zone_uid)
{
    bot_goal_runtime_prime_bridge_post_open_priority(
        zone_uid,
        scale_repath_ms(6000, true));
}

bool choose_bridge_cross_target_waypoint(
    const rf::Entity& local_entity,
    const int zone_uid,
    int& out_waypoint,
    rf::Vector3& out_pos)
{
    out_waypoint = 0;
    out_pos = {};

    const int waypoint_total = waypoints_count();
    float best_score = -std::numeric_limits<float>::infinity();
    for (int waypoint = 1; waypoint < waypoint_total; ++waypoint) {
        if (!waypoints_waypoint_has_zone(waypoint, zone_uid)) {
            continue;
        }

        rf::Vector3 waypoint_pos{};
        if (!waypoints_get_pos(waypoint, waypoint_pos)) {
            continue;
        }

        const float dist_sq = rf::vec_dist_squared(&local_entity.pos, &waypoint_pos);
        if (dist_sq > best_score) {
            best_score = dist_sq;
            out_waypoint = waypoint;
            out_pos = waypoint_pos;
        }
    }

    return out_waypoint > 0;
}

bool update_bridge_post_open_priority(
    const rf::Entity& local_entity,
    rf::Vector3& move_target,
    bool& has_move_target)
{
    if (g_client_bot_state.bridge.post_open_zone_uid < 0
        || !g_client_bot_state.bridge.post_open_priority_timer.valid()) {
        return false;
    }
    if (g_client_bot_state.bridge.post_open_priority_timer.elapsed()) {
        clear_bridge_post_open_priority_state();
        return false;
    }

    WaypointBridgeZoneState zone_state{};
    if (!waypoints_get_bridge_zone_state(g_client_bot_state.bridge.post_open_zone_uid, zone_state)
        || !zone_state.on) {
        clear_bridge_post_open_priority_state();
        return false;
    }

    rf::Vector3 bridge_target_pos{};
    if (g_client_bot_state.bridge.post_open_target_waypoint <= 0
        || !waypoints_get_pos(g_client_bot_state.bridge.post_open_target_waypoint, bridge_target_pos)
        || !waypoints_waypoint_has_zone(
            g_client_bot_state.bridge.post_open_target_waypoint,
            g_client_bot_state.bridge.post_open_zone_uid)) {
        if (!choose_bridge_cross_target_waypoint(
                local_entity,
                g_client_bot_state.bridge.post_open_zone_uid,
                g_client_bot_state.bridge.post_open_target_waypoint,
                bridge_target_pos)) {
            clear_bridge_post_open_priority_state();
            return false;
        }
    }

    const float reach_radius = kWaypointReachRadius * 1.6f;
    if (rf::vec_dist_squared(&local_entity.pos, &bridge_target_pos) <= reach_radius * reach_radius) {
        clear_bridge_post_open_priority_state();
        return false;
    }

    const bool routed = bot_internal_update_waypoint_target_towards(
        local_entity,
        bridge_target_pos,
        nullptr,
        nullptr,
        scale_repath_ms(kWaypointRecoveryRepathMs, true)
    );
    if (!routed) {
        clear_bridge_post_open_priority_state();
        return false;
    }

    move_target = g_client_bot_state.waypoint_target_pos;
    has_move_target = true;
    return true;
}

bool update_bridge_activation_goal(
    const rf::Entity& local_entity,
    rf::Vector3& move_target,
    bool& has_move_target)
{
    if (g_client_bot_state.goal_target_identifier < 0) {
        return bot_goal_runtime_abort_bridge_goal();
    }

    WaypointBridgeZoneState zone_state{};
    if (!waypoints_get_bridge_zone_state(g_client_bot_state.goal_target_identifier, zone_state)) {
        return bot_goal_runtime_abort_bridge_goal();
    }
    if (!zone_state.available) {
        return bot_goal_runtime_abort_bridge_goal();
    }

    g_client_bot_state.bridge.zone_uid = zone_state.zone_uid;
    g_client_bot_state.bridge.trigger_uid = zone_state.trigger_uid;
    g_client_bot_state.bridge.trigger_pos = zone_state.trigger_pos;
    g_client_bot_state.bridge.activation_radius = zone_state.activation_radius;
    g_client_bot_state.bridge.requires_use = zone_state.requires_use;

    if (zone_state.on) {
        prime_bridge_post_open_priority(zone_state.zone_uid);
        return bot_goal_runtime_abort_bridge_goal();
    }

    int bridge_zone_waypoint = 0;
    rf::Vector3 bridge_zone_waypoint_pos{};
    if (!find_reachable_bridge_zone_waypoint(
            local_entity,
            zone_state.zone_uid,
            g_client_bot_state.goal_target_waypoint,
            bridge_zone_waypoint,
            bridge_zone_waypoint_pos)) {
        return bot_goal_runtime_abort_bridge_goal();
    }

    g_client_bot_state.goal_target_waypoint = bridge_zone_waypoint;
    g_client_bot_state.goal_target_pos = zone_state.trigger_pos;

    const float activation_radius =
        std::max(zone_state.activation_radius, kWaypointReachRadius * 1.5f);
    const float trigger_dist_sq = rf::vec_dist_squared(
        &local_entity.pos,
        &zone_state.trigger_pos
    );
    constexpr float kBridgeActivationProgressEpsilonSq = 0.04f; // 20cm progress
    if (!g_client_bot_state.bridge.activation_abort_timer.valid()) {
        g_client_bot_state.bridge.activation_abort_timer.set(2000);
        g_client_bot_state.bridge.activation_best_dist_sq = trigger_dist_sq;
    }
    else if (trigger_dist_sq + kBridgeActivationProgressEpsilonSq
             < g_client_bot_state.bridge.activation_best_dist_sq) {
        g_client_bot_state.bridge.activation_best_dist_sq = trigger_dist_sq;
        g_client_bot_state.bridge.activation_abort_timer.set(2000);
    }
    else if (g_client_bot_state.bridge.activation_abort_timer.elapsed()) {
        return bot_goal_runtime_abort_bridge_goal();
    }

    const bool inside_trigger = point_inside_bridge_trigger(zone_state.trigger_uid, local_entity.pos);
    if (inside_trigger) {
        if (zone_state.requires_use
            && (!g_client_bot_state.bridge.use_press_timer.valid()
                || g_client_bot_state.bridge.use_press_timer.elapsed())
            && rf::local_player) {
            rf::player_execute_action(rf::local_player, rf::CC_ACTION_USE, true);
            g_client_bot_state.bridge.use_press_timer.set(220);
        }
        move_target = zone_state.trigger_pos;
        has_move_target = true;
        return true;
    }

    bool routed = bot_internal_update_waypoint_target_towards(
        local_entity,
        zone_state.trigger_pos,
        nullptr,
        nullptr,
        scale_repath_ms(kWaypointRecoveryRepathMs, true)
    );
    if (!routed) {
        bot_internal_start_recovery_anchor_reroute(local_entity, g_client_bot_state.goal_target_waypoint);
        routed = bot_internal_update_waypoint_target_towards(
            local_entity,
            bridge_zone_waypoint_pos,
            nullptr,
            nullptr,
            scale_repath_ms(kWaypointRecoveryRepathMs, true)
        );
    }
    if (!routed) {
        return bot_goal_runtime_abort_bridge_goal();
    }

    move_target = g_client_bot_state.waypoint_target_pos;
    const float center_approach_radius = std::max(activation_radius + 0.5f, 1.0f);
    const bool can_move_direct_to_trigger_center = can_link_waypoints(
        local_entity.pos,
        zone_state.trigger_pos
    );
    if (trigger_dist_sq <= center_approach_radius * center_approach_radius
        && can_move_direct_to_trigger_center) {
        move_target = zone_state.trigger_pos;
    }
    has_move_target = true;
    return true;
}

bool update_crater_goal(
    const rf::Entity& local_entity,
    rf::Vector3& move_target,
    bool& has_move_target)
{
    if (g_client_bot_state.goal_target_identifier < 0) {
        return bot_goal_runtime_abort_crater_goal();
    }

    WaypointTargetDefinition target{};
    if (!waypoints_get_target_by_uid(g_client_bot_state.goal_target_identifier, target)
        || target.type != WaypointTargetType::explosion) {
        return bot_goal_runtime_abort_crater_goal();
    }
    if (!rf::local_player
        || !bot_has_usable_crater_weapon(
            *rf::local_player,
            const_cast<rf::Entity&>(local_entity))) {
        return bot_goal_runtime_abort_crater_goal();
    }
    if (!g_client_bot_state.crater_goal_timeout_timer.valid()) {
        g_client_bot_state.crater_goal_timeout_timer.set(15000);
    }
    else if (g_client_bot_state.crater_goal_timeout_timer.elapsed()) {
        return bot_goal_runtime_abort_crater_goal();
    }

    int target_waypoint = 0;
    rf::Vector3 target_waypoint_pos{};
    if (!find_reachable_crater_target_waypoint(
            local_entity,
            target,
            g_client_bot_state.goal_target_waypoint,
            target_waypoint,
            target_waypoint_pos)) {
        if (!g_client_bot_state.crater_goal_abort_timer.valid()) {
            g_client_bot_state.crater_goal_abort_timer.set(2000);
        }
        else if (g_client_bot_state.crater_goal_abort_timer.elapsed()) {
            return bot_goal_runtime_abort_crater_goal();
        }
        return false;
    }
    g_client_bot_state.crater_goal_abort_timer.invalidate();

    g_client_bot_state.goal_target_waypoint = target_waypoint;
    g_client_bot_state.goal_target_pos = target.pos;

    const bool routed = bot_goal_runtime_route_to_waypoint_target_with_recovery(
        local_entity,
        target_waypoint_pos,
        g_client_bot_state.goal_target_waypoint,
        scale_repath_ms(kCollectRecoveryRepathMs),
        scale_repath_ms(kCollectRecoveryRepathMs));
    if (!routed) {
        return bot_goal_runtime_abort_crater_goal();
    }

    move_target = g_client_bot_state.waypoint_target_pos;
    has_move_target = true;
    return true;
}

bool update_shatter_goal(
    const rf::Entity& local_entity,
    rf::Vector3& move_target,
    bool& has_move_target)
{
    if (g_client_bot_state.goal_target_identifier < 0) {
        return bot_goal_runtime_abort_shatter_goal();
    }

    WaypointTargetDefinition target{};
    if (!waypoints_get_target_by_uid(g_client_bot_state.goal_target_identifier, target)
        || target.type != WaypointTargetType::shatter) {
        return bot_goal_runtime_abort_shatter_goal();
    }
    if (!rf::local_player
        || !bot_has_usable_shatter_weapon(
            *rf::local_player,
            const_cast<rf::Entity&>(local_entity))) {
        return bot_goal_runtime_abort_shatter_goal();
    }
    if (!g_client_bot_state.shatter_goal_timeout_timer.valid()) {
        g_client_bot_state.shatter_goal_timeout_timer.set(15000);
    }
    else if (g_client_bot_state.shatter_goal_timeout_timer.elapsed()) {
        return bot_goal_runtime_abort_shatter_goal();
    }

    int target_waypoint = 0;
    rf::Vector3 target_waypoint_pos{};
    if (!find_reachable_crater_target_waypoint(
            local_entity,
            target,
            g_client_bot_state.goal_target_waypoint,
            target_waypoint,
            target_waypoint_pos)) {
        if (!g_client_bot_state.shatter_goal_abort_timer.valid()) {
            g_client_bot_state.shatter_goal_abort_timer.set(2000);
        }
        else if (g_client_bot_state.shatter_goal_abort_timer.elapsed()) {
            return bot_goal_runtime_abort_shatter_goal();
        }
        return false;
    }
    g_client_bot_state.shatter_goal_abort_timer.invalidate();

    g_client_bot_state.goal_target_waypoint = target_waypoint;
    g_client_bot_state.goal_target_pos = target.pos;

    const bool routed = bot_goal_runtime_route_to_waypoint_target_with_recovery(
        local_entity,
        target_waypoint_pos,
        g_client_bot_state.goal_target_waypoint,
        scale_repath_ms(kCollectRecoveryRepathMs),
        scale_repath_ms(kCollectRecoveryRepathMs));
    if (!routed) {
        return bot_goal_runtime_abort_shatter_goal();
    }

    move_target = g_client_bot_state.waypoint_target_pos;
    has_move_target = true;
    return true;
}

bool update_ctf_objective_goal(
    const rf::Entity& local_entity,
    rf::Vector3& move_target,
    bool& has_move_target)
{
    if (!is_ctf_mode() || !rf::local_player) {
        return bot_goal_runtime_abort_ctf_goal();
    }

    const bool local_is_red = rf::local_player->team == rf::TEAM_RED;
    const bool own_flag_red = local_is_red;
    const bool enemy_flag_red = !local_is_red;
    bool own_flag_in_base = is_ctf_flag_in_base(own_flag_red);
    bool enemy_flag_in_base = is_ctf_flag_in_base(enemy_flag_red);
    rf::Player* own_flag_carrier = get_ctf_flag_carrier(own_flag_red);
    rf::Player* enemy_flag_carrier = get_ctf_flag_carrier(enemy_flag_red);
    rf::Entity* own_flag_carrier_entity = own_flag_carrier
        ? rf::entity_from_handle(own_flag_carrier->entity_handle)
        : nullptr;
    int own_dropped_waypoint = 0;
    rf::Vector3 own_dropped_pos{};
    const bool own_has_dropped_waypoint = waypoints_find_dropped_ctf_flag_waypoint(
        own_flag_red,
        own_dropped_waypoint,
        own_dropped_pos
    );
    int enemy_dropped_waypoint = 0;
    rf::Vector3 enemy_dropped_pos{};
    const bool enemy_has_dropped_waypoint = waypoints_find_dropped_ctf_flag_waypoint(
        enemy_flag_red,
        enemy_dropped_waypoint,
        enemy_dropped_pos
    );
    if (own_has_dropped_waypoint) {
        own_flag_in_base = false;
        own_flag_carrier = nullptr;
    }
    const bool local_carrying_enemy_flag = enemy_flag_carrier == rf::local_player;

    if (enemy_has_dropped_waypoint && !local_carrying_enemy_flag) {
        enemy_flag_in_base = false;
        enemy_flag_carrier = nullptr;
    }
    const bool local_has_enemy_flag = local_carrying_enemy_flag;
    rf::Vector3 target_pos{};
    bool has_target = false;

    switch (g_client_bot_state.active_goal) {
        case BotGoalType::ctf_capture_flag:
            if (!local_has_enemy_flag || !own_flag_in_base) {
                return bot_goal_runtime_abort_ctf_goal();
            }
            target_pos = get_ctf_flag_pos(own_flag_red);
            has_target = true;
            break;
        case BotGoalType::ctf_return_flag:
            if (own_flag_in_base) {
                return bot_goal_runtime_abort_ctf_goal();
            }
            if (own_flag_carrier_entity && !rf::entity_is_dying(own_flag_carrier_entity)) {
                target_pos = own_flag_carrier_entity->pos;
                g_client_bot_state.goal_target_handle = own_flag_carrier_entity->handle;
                g_client_bot_state.goal_target_identifier = own_flag_carrier_entity->uid;
            }
            else {
                target_pos = own_has_dropped_waypoint ? own_dropped_pos : get_ctf_flag_pos(own_flag_red);
                g_client_bot_state.goal_target_handle = -1;
            }
            has_target = true;
            break;
        case BotGoalType::ctf_steal_flag:
            if (local_has_enemy_flag || (!enemy_flag_in_base && enemy_flag_carrier)) {
                return bot_goal_runtime_abort_ctf_goal();
            }
            target_pos = enemy_has_dropped_waypoint ? enemy_dropped_pos : get_ctf_flag_pos(enemy_flag_red);
            has_target = true;
            break;
        case BotGoalType::ctf_hold_enemy_flag:
            if (!local_has_enemy_flag || own_flag_in_base) {
                return bot_goal_runtime_abort_ctf_goal();
            }
            {
                int own_base_waypoint = 0;
                rf::Vector3 own_base_pos{};
                find_nearest_ctf_flag_waypoint(
                    local_entity.pos,
                    own_flag_red,
                    own_base_waypoint,
                    own_base_pos
                );

                int own_spawn_waypoint = 0;
                rf::Vector3 own_spawn_pos{};
                find_nearest_team_spawn_waypoint(
                    local_entity.pos,
                    local_is_red,
                    own_spawn_waypoint,
                    own_spawn_pos
                );

                const float hold_safety_bias = std::clamp(
                    get_active_bot_personality().ctf_hold_enemy_flag_safety_bias,
                    0.25f,
                    2.5f
                );
                const bool prefer_spawn_anchor = own_spawn_waypoint > 0
                    && std::lerp(0.20f, 1.0f, hold_safety_bias) > 0.55f;

                int primary_waypoint = prefer_spawn_anchor ? own_spawn_waypoint : own_base_waypoint;
                rf::Vector3 primary_pos = prefer_spawn_anchor ? own_spawn_pos : own_base_pos;
                int secondary_waypoint = prefer_spawn_anchor ? own_base_waypoint : own_spawn_waypoint;
                rf::Vector3 secondary_pos = prefer_spawn_anchor ? own_base_pos : own_spawn_pos;

                if (primary_waypoint <= 0) {
                    primary_waypoint = secondary_waypoint;
                    primary_pos = secondary_pos;
                    secondary_waypoint = 0;
                }

                int patrol_waypoint = primary_waypoint;
                rf::Vector3 patrol_pos = primary_pos;
                if (secondary_waypoint > 0 && primary_waypoint > 0) {
                    if (g_client_bot_state.goal_target_waypoint == primary_waypoint) {
                        patrol_waypoint = secondary_waypoint;
                        patrol_pos = secondary_pos;
                    }
                    else if (g_client_bot_state.goal_target_waypoint == secondary_waypoint) {
                        patrol_waypoint = primary_waypoint;
                        patrol_pos = primary_pos;
                    }
                    else {
                        const float dist_to_primary_sq = rf::vec_dist_squared(&local_entity.pos, &primary_pos);
                        constexpr float kHoldAnchorSwapRadius = kWaypointLinkRadius * 1.8f;
                        if (dist_to_primary_sq <= kHoldAnchorSwapRadius * kHoldAnchorSwapRadius) {
                            patrol_waypoint = secondary_waypoint;
                            patrol_pos = secondary_pos;
                        }
                    }
                }

                if (patrol_waypoint > 0) {
                    g_client_bot_state.goal_target_waypoint = patrol_waypoint;
                    g_client_bot_state.goal_target_pos = patrol_pos;
                    target_pos = patrol_pos;
                }
                else if (g_client_bot_state.goal_target_pos.len_sq() > 0.0001f) {
                    target_pos = g_client_bot_state.goal_target_pos;
                }
                else {
                    target_pos = get_ctf_flag_pos(own_flag_red);
                }
            }
            has_target = true;
            break;
        default:
            return false;
    }

    if (!has_target || target_pos.len_sq() <= 0.0001f) {
        return bot_goal_runtime_abort_ctf_goal();
    }

    const bool base_anchor_goal =
        (g_client_bot_state.active_goal == BotGoalType::ctf_steal_flag && enemy_flag_in_base)
        || (g_client_bot_state.active_goal == BotGoalType::ctf_capture_flag && own_flag_in_base);
    const bool dropped_flag_goal =
        (g_client_bot_state.active_goal == BotGoalType::ctf_return_flag
            && !own_flag_in_base
            && own_flag_carrier == nullptr)
        || (g_client_bot_state.active_goal == BotGoalType::ctf_steal_flag
            && !enemy_flag_in_base
            && enemy_flag_carrier == nullptr);
    const bool stolen_return_goal =
        g_client_bot_state.active_goal == BotGoalType::ctf_return_flag
        && own_flag_carrier_entity
        && !rf::entity_is_dying(own_flag_carrier_entity);

    // Keep CTF objective target pinned to the live flag/objective location.
    // Waypoint positions are only used as routing anchors.
    g_client_bot_state.goal_target_pos = target_pos;
    rf::Vector3 waypoint_pos{};
    bool waypoint_valid =
        g_client_bot_state.goal_target_waypoint > 0
        && waypoints_get_pos(g_client_bot_state.goal_target_waypoint, waypoint_pos);
    bool dropped_flag_goal_red = own_flag_red;
    if (base_anchor_goal) {
        const bool objective_flag_red =
            (g_client_bot_state.active_goal == BotGoalType::ctf_steal_flag)
            ? enemy_flag_red
            : own_flag_red;
        int ctf_flag_waypoint = 0;
        rf::Vector3 ctf_flag_waypoint_pos{};
        if (find_nearest_ctf_flag_waypoint(
                target_pos,
                objective_flag_red,
                ctf_flag_waypoint,
                ctf_flag_waypoint_pos)) {
            g_client_bot_state.goal_target_waypoint = ctf_flag_waypoint;
            waypoint_pos = ctf_flag_waypoint_pos;
            waypoint_valid = true;
        }
    }
    else if (dropped_flag_goal) {
        int preferred_waypoint = g_client_bot_state.goal_target_waypoint;
        const bool dropped_flag_red =
            (g_client_bot_state.active_goal == BotGoalType::ctf_return_flag)
            ? own_flag_red
            : enemy_flag_red;
        dropped_flag_goal_red = dropped_flag_red;
        const int dropped_flag_subtype = dropped_flag_red
            ? static_cast<int>(WaypointCtfFlagSubtype::red)
            : static_cast<int>(WaypointCtfFlagSubtype::blue);
        int dropped_flag_anchor_waypoint = 0;
        rf::Vector3 dropped_flag_anchor_pos{};
        if ((dropped_flag_red && own_has_dropped_waypoint)
            || (!dropped_flag_red && enemy_has_dropped_waypoint)) {
            dropped_flag_anchor_waypoint = dropped_flag_red ? own_dropped_waypoint : enemy_dropped_waypoint;
            dropped_flag_anchor_pos = dropped_flag_red ? own_dropped_pos : enemy_dropped_pos;
            g_client_bot_state.goal_target_waypoint = dropped_flag_anchor_waypoint;
            waypoint_pos = dropped_flag_anchor_pos;
            waypoint_valid = true;
            preferred_waypoint = dropped_flag_anchor_waypoint;
        }
        else {
            // Don't keep preferring a stale base anchor while flag is dropped.
            preferred_waypoint = 0;
        }

        int dropped_flag_waypoint = 0;
        rf::Vector3 dropped_flag_waypoint_pos{};
        if (find_reachable_waypoint_near_position(
                local_entity,
                g_client_bot_state.goal_target_pos,
                preferred_waypoint,
                dropped_flag_waypoint,
                dropped_flag_waypoint_pos,
                true,
                dropped_flag_subtype)) {
            g_client_bot_state.goal_target_waypoint = dropped_flag_waypoint;
            waypoint_pos = dropped_flag_waypoint_pos;
            waypoint_valid = true;
        }
    }
    if (!waypoint_valid
        || rf::vec_dist_squared(&waypoint_pos, &g_client_bot_state.goal_target_pos)
            > kWaypointLinkRadius * kWaypointLinkRadius) {
        if (!base_anchor_goal) {
            if (dropped_flag_goal) {
                const int dropped_flag_subtype = dropped_flag_goal_red
                    ? static_cast<int>(WaypointCtfFlagSubtype::red)
                    : static_cast<int>(WaypointCtfFlagSubtype::blue);
                int dropped_goal_waypoint = 0;
                rf::Vector3 dropped_goal_waypoint_pos{};
                if (find_reachable_waypoint_near_position(
                        local_entity,
                        g_client_bot_state.goal_target_pos,
                        0,
                        dropped_goal_waypoint,
                        dropped_goal_waypoint_pos,
                        true,
                        dropped_flag_subtype)) {
                    g_client_bot_state.goal_target_waypoint = dropped_goal_waypoint;
                    waypoint_pos = dropped_goal_waypoint_pos;
                    waypoint_valid = true;
                }
            }
            else if (stolen_return_goal) {
                g_client_bot_state.goal_target_waypoint = bot_find_closest_waypoint_with_fallback(
                    own_flag_carrier_entity->pos
                );
                waypoint_valid =
                    g_client_bot_state.goal_target_waypoint > 0
                    && waypoints_get_pos(g_client_bot_state.goal_target_waypoint, waypoint_pos);
            }
            else {
                g_client_bot_state.goal_target_waypoint = bot_find_closest_waypoint_with_fallback(
                    g_client_bot_state.goal_target_pos
                );
                waypoint_valid =
                    g_client_bot_state.goal_target_waypoint > 0
                    && waypoints_get_pos(g_client_bot_state.goal_target_waypoint, waypoint_pos);
            }
        }
        else if (!waypoint_valid) {
            g_client_bot_state.goal_target_waypoint = bot_find_closest_waypoint_with_fallback(
                g_client_bot_state.goal_target_pos
            );
            waypoint_valid =
                g_client_bot_state.goal_target_waypoint > 0
                && waypoints_get_pos(g_client_bot_state.goal_target_waypoint, waypoint_pos);
        }
    }

    const float goal_dist_sq = rf::vec_dist_squared(
        &local_entity.pos,
        &g_client_bot_state.goal_target_pos
    );
    if (goal_dist_sq <= kWaypointLinkRadius * kWaypointLinkRadius) {
        bot_internal_clear_waypoint_route();
        move_target = g_client_bot_state.goal_target_pos;
        has_move_target = true;
        return true;
    }

    if (waypoint_valid) {
        constexpr float kCtfFinalApproachWaypointProximity = kWaypointReachRadius * 1.2f;
        const float dist_to_goal_waypoint_sq = rf::vec_dist_squared(
            &local_entity.pos,
            &waypoint_pos
        );
        if (dist_to_goal_waypoint_sq
            <= kCtfFinalApproachWaypointProximity * kCtfFinalApproachWaypointProximity) {
            // Once we reach the anchor waypoint near the flag objective,
            // commit to running directly over the live flag position.
            bot_internal_clear_waypoint_route();
            move_target = g_client_bot_state.goal_target_pos;
            has_move_target = true;
            return true;
        }
    }

    const rf::Vector3 routing_destination =
        waypoint_valid ? waypoint_pos : g_client_bot_state.goal_target_pos;

    bool routed = bot_internal_update_waypoint_target_towards(
        local_entity,
        routing_destination,
        nullptr,
        nullptr,
        scale_repath_ms(kItemRouteRepathMs)
    );
    const bool allow_reachable_fallback =
        !base_anchor_goal
        && g_client_bot_state.active_goal != BotGoalType::ctf_capture_flag;
    if (!routed && allow_reachable_fallback) {
        int reachable_waypoint = 0;
        rf::Vector3 reachable_waypoint_pos{};
        const int dropped_flag_subtype = dropped_flag_goal_red
            ? static_cast<int>(WaypointCtfFlagSubtype::red)
            : static_cast<int>(WaypointCtfFlagSubtype::blue);
        if (find_reachable_waypoint_near_position(
                local_entity,
                g_client_bot_state.goal_target_pos,
                g_client_bot_state.goal_target_waypoint,
                reachable_waypoint,
                reachable_waypoint_pos,
                dropped_flag_goal,
                dropped_flag_subtype)) {
            const float current_goal_dist_sq = rf::vec_dist_squared(
                &local_entity.pos,
                &g_client_bot_state.goal_target_pos
            );
            const float fallback_goal_dist_sq = rf::vec_dist_squared(
                &reachable_waypoint_pos,
                &g_client_bot_state.goal_target_pos
            );
            constexpr float kFallbackProgressMargin = 1.0f; // meters
            const bool accept_fallback =
                dropped_flag_goal
                || (fallback_goal_dist_sq + (kFallbackProgressMargin * kFallbackProgressMargin)
                    < current_goal_dist_sq);
            if (accept_fallback) {
                routed = bot_internal_update_waypoint_target_towards(
                    local_entity,
                    reachable_waypoint_pos,
                    nullptr,
                    nullptr,
                    scale_repath_ms(kItemRouteRepathMs)
                );
                if (routed) {
                    g_client_bot_state.goal_target_waypoint = reachable_waypoint;
                }
            }
        }
    }
    if (!routed && waypoint_valid) {
        // If the flag waypoint itself is isolated, fall back to pathing toward the live flag position.
        routed = bot_internal_update_waypoint_target_towards(
            local_entity,
            g_client_bot_state.goal_target_pos,
            nullptr,
            nullptr,
            scale_repath_ms(kItemRouteRepathMs)
        );
    }
    if (!routed) {
        bot_internal_start_recovery_anchor_reroute(local_entity, g_client_bot_state.goal_target_waypoint);
        routed = bot_internal_update_waypoint_target_towards(
            local_entity,
            routing_destination,
            nullptr,
            nullptr,
            scale_repath_ms(kWaypointRecoveryRepathMs, true)
        );
    }
    if (!routed) {
        // Keep the steal/capture objective live and keep trying for a short window.
        // Abort only after sustained route failure, which indicates likely impossibility.
        if (!g_client_bot_state.ctf_objective_route_fail_timer.valid()) {
            g_client_bot_state.ctf_objective_route_fail_timer.set(5000);
        }
        else if (g_client_bot_state.ctf_objective_route_fail_timer.elapsed()) {
            return bot_goal_runtime_abort_ctf_goal();
        }

        move_target = g_client_bot_state.goal_target_pos;
        has_move_target = true;
        return true;
    }

    g_client_bot_state.ctf_objective_route_fail_timer.invalidate();
    move_target = g_client_bot_state.waypoint_target_pos;
    has_move_target = true;
    return true;
}

bool update_control_point_objective_goal(
    const rf::Entity& local_entity,
    rf::Vector3& move_target,
    bool& has_move_target)
{
    if (!is_control_point_mode() || !rf::local_player) {
        return bot_goal_runtime_abort_control_point_goal();
    }
    if (!bot_goal_is_control_point_objective(g_client_bot_state.active_goal)
        || g_client_bot_state.goal_target_identifier < 0) {
        return bot_goal_runtime_abort_control_point_goal();
    }

    const HillOwner local_team = local_hill_team_owner();
    if (local_team != HillOwner::HO_Red && local_team != HillOwner::HO_Blue) {
        return bot_goal_runtime_abort_control_point_goal();
    }

    HillInfo* hill = koth_find_hill_by_handler_uid(g_client_bot_state.goal_target_identifier);
    if (!hill) {
        return bot_goal_runtime_abort_control_point_goal();
    }

    const bool capture_available =
        control_point_team_can_capture_hill(*hill, local_team);
    const bool defense_needed =
        control_point_hill_needs_defense(*hill, local_team);
    if (!capture_available && !defense_needed) {
        return bot_goal_runtime_abort_control_point_goal();
    }

    int objective_waypoint = 0;
    rf::Vector3 objective_waypoint_pos{};
    if (!find_reachable_control_point_zone_waypoint(
            local_entity,
            g_client_bot_state.goal_target_identifier,
            g_client_bot_state.goal_target_waypoint,
            objective_waypoint,
            objective_waypoint_pos)) {
        if (!g_client_bot_state.control_point_route_fail_timer.valid()) {
            g_client_bot_state.control_point_route_fail_timer.set(5000);
        }
        else if (g_client_bot_state.control_point_route_fail_timer.elapsed()) {
            return bot_goal_runtime_abort_control_point_goal();
        }

        g_client_bot_state.goal_target_pos =
            resolve_control_point_objective_pos(*hill, g_client_bot_state.goal_target_pos);
        move_target = g_client_bot_state.goal_target_pos;
        has_move_target = true;
        return true;
    }

    g_client_bot_state.goal_target_waypoint = objective_waypoint;
    g_client_bot_state.goal_target_pos =
        resolve_control_point_objective_pos(*hill, objective_waypoint_pos);

    const float goal_dist_sq = rf::vec_dist_squared(
        &local_entity.pos,
        &g_client_bot_state.goal_target_pos
    );
    if (goal_dist_sq <= kWaypointLinkRadius * kWaypointLinkRadius) {
        bool switch_patrol_waypoint =
            g_client_bot_state.control_point_patrol_waypoint <= 0
            || !g_client_bot_state.control_point_patrol_timer.valid()
            || g_client_bot_state.control_point_patrol_timer.elapsed();
        rf::Vector3 patrol_pos{};
        if (!switch_patrol_waypoint) {
            if (!waypoints_get_pos(g_client_bot_state.control_point_patrol_waypoint, patrol_pos)) {
                switch_patrol_waypoint = true;
            }
            else {
                const float patrol_dist_sq = rf::vec_dist_squared(&local_entity.pos, &patrol_pos);
                if (patrol_dist_sq <= kWaypointReachRadius * kWaypointReachRadius) {
                    switch_patrol_waypoint = true;
                }
            }
        }
        if (switch_patrol_waypoint) {
            int patrol_waypoint = 0;
            if (find_control_point_patrol_waypoint(
                    local_entity,
                    g_client_bot_state.goal_target_identifier,
                    g_client_bot_state.control_point_patrol_waypoint > 0
                        ? g_client_bot_state.control_point_patrol_waypoint
                        : g_client_bot_state.goal_target_waypoint,
                    patrol_waypoint,
                    patrol_pos)) {
                g_client_bot_state.control_point_patrol_waypoint = patrol_waypoint;
                g_client_bot_state.control_point_patrol_timer.set(1200);
            }
            else {
                // If only one waypoint anchors this point, keep moving around center.
                const bool use_left_orbit =
                    g_client_bot_state.control_point_patrol_waypoint != -1;
                g_client_bot_state.control_point_patrol_waypoint =
                    use_left_orbit ? -1 : -2;
                g_client_bot_state.control_point_patrol_timer.set(900);
                patrol_pos = g_client_bot_state.goal_target_pos;
                const rf::Vector3 lateral =
                    forward_from_non_linear_yaw_pitch(
                        local_entity.control_data.phb.y
                            + (use_left_orbit ? 1.57079632679f : -1.57079632679f),
                        0.0f);
                patrol_pos += lateral * (kWaypointReachRadius * 0.75f);
            }
        }

        bot_internal_clear_waypoint_route();
        g_client_bot_state.control_point_route_fail_timer.invalidate();
        move_target = patrol_pos.len_sq() > 0.0001f
            ? patrol_pos
            : g_client_bot_state.goal_target_pos;
        has_move_target = true;
        return true;
    }

    constexpr float kControlPointFinalApproachWaypointProximity = kWaypointReachRadius * 1.25f;
    const float dist_to_waypoint_sq = rf::vec_dist_squared(
        &local_entity.pos,
        &objective_waypoint_pos
    );
    if (dist_to_waypoint_sq
        <= kControlPointFinalApproachWaypointProximity * kControlPointFinalApproachWaypointProximity) {
        bot_internal_clear_waypoint_route();
        g_client_bot_state.control_point_route_fail_timer.invalidate();
        move_target = g_client_bot_state.goal_target_pos;
        has_move_target = true;
        return true;
    }

    const bool routed = bot_goal_runtime_route_to_waypoint_target_with_recovery(
        local_entity,
        objective_waypoint_pos,
        g_client_bot_state.goal_target_waypoint,
        scale_repath_ms(kItemRouteRepathMs),
        scale_repath_ms(kWaypointRecoveryRepathMs, true));
    if (!routed) {
        if (!g_client_bot_state.control_point_route_fail_timer.valid()) {
            g_client_bot_state.control_point_route_fail_timer.set(5000);
        }
        else if (g_client_bot_state.control_point_route_fail_timer.elapsed()) {
            return bot_goal_runtime_abort_control_point_goal();
        }

        move_target = g_client_bot_state.goal_target_pos;
        has_move_target = true;
        return true;
    }

    g_client_bot_state.control_point_route_fail_timer.invalidate();
    move_target = g_client_bot_state.waypoint_target_pos;
    has_move_target = true;
    return true;
}
}

bool bot_sync_pursuit_target(rf::Entity* enemy_target)
{
    const bool pursuing_enemy_goal =
        g_client_bot_state.active_goal == BotGoalType::eliminate_target
        && enemy_target
        && enemy_target->handle == g_client_bot_state.goal_target_handle;
    const int desired_pursuit_handle = pursuing_enemy_goal ? enemy_target->handle : -1;
    if (desired_pursuit_handle != g_client_bot_state.pursuit_target_handle) {
        bot_state_clear_waypoint_route(true, true, false);
        g_client_bot_state.pursuit_target_handle = desired_pursuit_handle;
        g_client_bot_state.pursuit_route_failures = 0;
        g_client_bot_state.recent_waypoint_visits.clear();
        clear_enemy_aim_error_state();
    }
    return pursuing_enemy_goal;
}

void bot_update_pursuit_recovery_timer()
{
    if (g_client_bot_state.pursuit_recovery_timer.valid()
        && g_client_bot_state.pursuit_recovery_timer.elapsed()) {
        g_client_bot_state.pursuit_recovery_timer.invalidate();
    }
}

void bot_update_move_target(
    const rf::Entity& local_entity,
    rf::Entity* enemy_target,
    const bool enemy_has_los,
    const bool pursuing_enemy_goal,
    rf::Vector3& move_target,
    bool& has_move_target)
{
    move_target = {};
    has_move_target = false;
    bool force_direct_pickup_approach = false;

    if (g_client_bot_state.active_goal == BotGoalType::none) {
        bot_state_set_roam_fallback_goal(0);
    }

    const auto clear_unavailable_item_goal = [&]() {
        if (bot_goal_is_item_collection(g_client_bot_state.active_goal)
            && g_client_bot_state.goal_target_handle >= 0) {
            // Prevent immediate re-selection churn on an item that just became unavailable
            // (picked up/despawned) while standing on its position.
            bot_memory_manager_note_failed_goal_target(
                g_client_bot_state.active_goal,
                g_client_bot_state.goal_target_handle,
                2200
            );
        }
        g_client_bot_state.collect_route_failures = 0;
        bot_state_clear_item_goal_contact_tracking();
        // Item pickup completed/invalidated: transition through the centralized
        // roam fallback path to keep decision plumbing consistent.
        // Item became unavailable (picked/despawned): force immediate
        // reevaluation so we do not pause in placeholder roam state.
        bot_state_set_roam_fallback_goal(0);
        bot_state_clear_waypoint_route(true, true, false);
        bot_internal_set_last_heading_change_reason("item_unavailable");
    };

    const auto update_item_goal_contact_watchdog =
        [&](rf::Object* pickup_obj, const float item_dist_sq) {
            if (!pickup_obj || pickup_obj->handle < 0) {
                bot_state_clear_item_goal_contact_tracking();
                return false;
            }

            const float contact_radius_sq = kItemGoalContactRadius * kItemGoalContactRadius;
            if (item_dist_sq > contact_radius_sq) {
                if (g_client_bot_state.item_goal_contact_handle == pickup_obj->handle) {
                    bot_state_clear_item_goal_contact_tracking();
                }
                return false;
            }

            if (g_client_bot_state.item_goal_contact_handle != pickup_obj->handle
                || !g_client_bot_state.item_goal_contact_timer.valid()) {
                g_client_bot_state.item_goal_contact_handle = pickup_obj->handle;
                g_client_bot_state.item_goal_contact_timer.set(kItemGoalContactTimeoutMs);
                return false;
            }

            if (!g_client_bot_state.item_goal_contact_timer.elapsed()) {
                return false;
            }

            bot_memory_manager_note_failed_goal_target(
                g_client_bot_state.active_goal,
                pickup_obj->handle,
                kFailedItemGoalCooldownMs
            );
            bot_internal_set_last_heading_change_reason("item_contact_timeout");
            clear_unavailable_item_goal();
            return true;
        };

    const auto pickup_direct_approach_radius = [&]() -> float {
        float radius = kWaypointLinkRadius * 1.35f;
        if (g_client_bot_state.active_goal == BotGoalType::collect_super_item) {
            radius = std::max(radius, kWaypointLinkRadius * 2.0f);
        }
        return radius;
    };

    bot_goal_runtime_clear_non_active_goal_state(g_client_bot_state.active_goal);

    const bool is_contextual_state =
        bot_fsm_is_contextual_item_state(g_client_bot_state.fsm_state);
    if (!is_contextual_state) {
        bot_state_clear_contextual_goal_tracking();
    }
    if (!bot_goal_is_item_collection(g_client_bot_state.active_goal)) {
        g_client_bot_state.collect_route_failures = 0;
        bot_state_clear_item_goal_contact_tracking();
    }
    else if (g_client_bot_state.goal_target_handle >= 0) {
        rf::Object* pickup_obj = rf::obj_from_handle(g_client_bot_state.goal_target_handle);
        if (!pickup_obj || pickup_obj->type != rf::OT_ITEM
            || !bot_internal_is_collectible_goal_item(*static_cast<rf::Item*>(pickup_obj))) {
            clear_unavailable_item_goal();
        }
    }

    if (update_bridge_post_open_priority(local_entity, move_target, has_move_target)) {
        return;
    }

    switch (g_client_bot_state.fsm_state) {
        case BotFsmState::activate_bridge:
            update_bridge_activation_goal(local_entity, move_target, has_move_target);
            break;
        case BotFsmState::create_crater:
            update_crater_goal(local_entity, move_target, has_move_target);
            break;
        case BotFsmState::shatter_glass:
            update_shatter_goal(local_entity, move_target, has_move_target);
            break;
        case BotFsmState::ctf_objective:
            update_ctf_objective_goal(local_entity, move_target, has_move_target);
            break;
        case BotFsmState::control_point_objective:
            update_control_point_objective_goal(local_entity, move_target, has_move_target);
            break;
        case BotFsmState::retreat:
            if (!update_retreat_goal(local_entity, enemy_target, move_target, has_move_target)) {
                update_contextual_item_goal(
                    local_entity,
                    ContextualGoalMode::replenish_survivability,
                    move_target,
                    has_move_target
                );
            }
            break;
        case BotFsmState::seek_weapon:
            update_contextual_item_goal(
                local_entity,
                ContextualGoalMode::seek_weapon,
                move_target,
                has_move_target
            );
            break;
        case BotFsmState::replenish_health_armor:
            update_contextual_item_goal(
                local_entity,
                ContextualGoalMode::replenish_survivability,
                move_target,
                has_move_target
            );
            break;
        case BotFsmState::find_power_position:
            update_power_position_goal(
                local_entity,
                enemy_target,
                enemy_has_los,
                move_target,
                has_move_target
            );
            break;
        default:
            break;
    }

    if (bot_goal_is_item_collection(g_client_bot_state.active_goal)
        && g_client_bot_state.goal_target_handle >= 0) {
        rf::Object* pickup_obj = rf::obj_from_handle(g_client_bot_state.goal_target_handle);
        if (!pickup_obj || pickup_obj->type != rf::OT_ITEM
            || !bot_internal_is_collectible_goal_item(*static_cast<rf::Item*>(pickup_obj))) {
            clear_unavailable_item_goal();
        }
        else {
            g_client_bot_state.goal_target_pos = pickup_obj->pos;
            const float item_dist_sq = rf::vec_dist_squared(
                &local_entity.pos,
                &g_client_bot_state.goal_target_pos
            );
            const auto* pickup_item = static_cast<rf::Item*>(pickup_obj);
            const float direct_pickup_radius = pickup_direct_approach_radius();
            if (item_dist_sq <= direct_pickup_radius * direct_pickup_radius
                && bot_internal_is_item_goal_satisfied(
                    local_entity,
                    g_client_bot_state.active_goal,
                    *pickup_item)) {
                bot_internal_set_last_heading_change_reason("item_goal_satisfied");
                clear_unavailable_item_goal();
                if (bot_internal_update_waypoint_target(local_entity)) {
                    move_target = g_client_bot_state.waypoint_target_pos;
                    has_move_target = true;
                }
                return;
            }
            if (update_item_goal_contact_watchdog(pickup_obj, item_dist_sq)) {
                if (bot_internal_update_waypoint_target(local_entity)) {
                    move_target = g_client_bot_state.waypoint_target_pos;
                    has_move_target = true;
                }
                return;
            }
            if (item_dist_sq <= direct_pickup_radius * direct_pickup_radius) {
                move_target = g_client_bot_state.goal_target_pos;
                g_client_bot_state.waypoint_target_pos = g_client_bot_state.goal_target_pos;
                // Final pickup approach must run through the item, not stop at waypoint tolerance.
                g_client_bot_state.has_waypoint_target = false;
                has_move_target = true;
                force_direct_pickup_approach = true;
            }
        }
    }

    const bool allow_pursuit_navigation =
        pursuing_enemy_goal && g_client_bot_state.fsm_state != BotFsmState::recover_navigation;
    if (!has_move_target && allow_pursuit_navigation) {
        // Prioritize line-of-sight routes over preferred engagement distance.
        bool routed_towards_enemy = false;
        if (enemy_has_los) {
            routed_towards_enemy = bot_internal_update_waypoint_target_for_local_los_reposition(
                local_entity,
                *enemy_target,
                true
            );
            if (!routed_towards_enemy) {
                // Already visible: avoid expensive LOS-target route solving.
                routed_towards_enemy = bot_internal_update_waypoint_target_towards(
                    local_entity,
                    enemy_target->pos,
                    nullptr,
                    enemy_target,
                    scale_repath_ms(kPursuitRepathIntervalMs)
                );
            }
        }
        else {
            // Near-corner peeking: try local LOS-aware reposition even before long route solving.
            routed_towards_enemy = bot_internal_update_waypoint_target_for_local_los_reposition(
                local_entity,
                *enemy_target,
                false
            );
            if (routed_towards_enemy) {
                move_target = g_client_bot_state.waypoint_target_pos;
                has_move_target = true;
            }
        }
        if (!routed_towards_enemy && !enemy_has_los) {
            routed_towards_enemy = bot_internal_update_waypoint_target_towards(
                local_entity,
                enemy_target->pos,
                &enemy_target->eye_pos,
                enemy_target,
                scale_repath_ms(kPursuitRepathIntervalMs)
            );
        }
        if (!routed_towards_enemy) {
            routed_towards_enemy = bot_internal_update_waypoint_target_towards(
                local_entity,
                enemy_target->pos,
                nullptr,
                enemy_target,
                scale_repath_ms(kWaypointRecoveryRepathMs, true)
            );
        }
        if (!routed_towards_enemy) {
            bot_internal_start_recovery_anchor_reroute(local_entity, g_client_bot_state.waypoint_goal);
            routed_towards_enemy = bot_internal_update_waypoint_target_towards(
                local_entity,
                enemy_target->pos,
                nullptr,
                enemy_target,
                scale_repath_ms(kWaypointRecoveryRepathMs, true)
            );
        }

        bool abandoned_pursuit_for_forage = false;
        if (routed_towards_enemy) {
            // A single-hop fallback route is usually unstable. Count it as partial failure
            // so we can break out of reroute thrash instead of looping forever.
            if (g_client_bot_state.last_pursuit_route_was_fallback) {
                ++g_client_bot_state.pursuit_route_failures;
            }
            else {
                g_client_bot_state.pursuit_route_failures = 0;
            }
        }
        else {
            ++g_client_bot_state.pursuit_route_failures;
        }
        if (g_client_bot_state.pursuit_route_failures >= kPursuitRouteFailureLimit) {
            bot_state_clear_waypoint_route(false, true, false);
            g_client_bot_state.pursuit_route_failures = 0;
            if (!enemy_has_los) {
                abandon_eliminate_goal_for_forage("pursuit_abort_to_forage");
                abandoned_pursuit_for_forage = true;
            }
        }

        if (routed_towards_enemy) {
            move_target = g_client_bot_state.waypoint_target_pos;
            has_move_target = true;
        }
        else if (enemy_target && !abandoned_pursuit_for_forage) {
            // Last-resort near-corner fallback: keep pressure instead of freezing.
            const float close_press_dist_sq = 12.0f * 12.0f;
            if (rf::vec_dist_squared(&local_entity.pos, &enemy_target->pos) <= close_press_dist_sq) {
                move_target = enemy_target->pos;
                has_move_target = true;
            }
        }
    }
    else if (!has_move_target && bot_goal_is_item_collection(g_client_bot_state.active_goal)) {
        rf::Object* pickup_obj = rf::obj_from_handle(g_client_bot_state.goal_target_handle);
        bool routed_towards_pickup = false;
        bool collectible_pickup_valid = false;
        if (pickup_obj && pickup_obj->type == rf::OT_ITEM
            && bot_internal_is_collectible_goal_item(*static_cast<rf::Item*>(pickup_obj))) {
            collectible_pickup_valid = true;
            g_client_bot_state.goal_target_pos = pickup_obj->pos;

            const float item_dist_sq = rf::vec_dist_squared(
                &local_entity.pos,
                &g_client_bot_state.goal_target_pos
            );
            const auto* pickup_item = static_cast<rf::Item*>(pickup_obj);
            const float direct_pickup_radius = pickup_direct_approach_radius();
            if (item_dist_sq <= direct_pickup_radius * direct_pickup_radius
                && bot_internal_is_item_goal_satisfied(
                    local_entity,
                    g_client_bot_state.active_goal,
                    *pickup_item)) {
                bot_internal_set_last_heading_change_reason("item_goal_satisfied");
                clear_unavailable_item_goal();
                if (bot_internal_update_waypoint_target(local_entity)) {
                    move_target = g_client_bot_state.waypoint_target_pos;
                    has_move_target = true;
                }
                return;
            }
            if (update_item_goal_contact_watchdog(pickup_obj, item_dist_sq)) {
                if (bot_internal_update_waypoint_target(local_entity)) {
                    move_target = g_client_bot_state.waypoint_target_pos;
                    has_move_target = true;
                }
                return;
            }
            if (item_dist_sq <= direct_pickup_radius * direct_pickup_radius) {
                move_target = g_client_bot_state.goal_target_pos;
                g_client_bot_state.waypoint_target_pos = g_client_bot_state.goal_target_pos;
                // Final pickup approach must run through the item, not stop at waypoint tolerance.
                g_client_bot_state.has_waypoint_target = false;
                has_move_target = true;
                routed_towards_pickup = true;
            }

            if (!routed_towards_pickup) {
                routed_towards_pickup = bot_internal_update_waypoint_target_towards(
                    local_entity,
                    g_client_bot_state.goal_target_pos,
                    nullptr,
                    pickup_obj,
                    scale_repath_ms(kItemRouteRepathMs)
                );
            }
            if (!routed_towards_pickup) {
                bot_internal_start_recovery_anchor_reroute(local_entity, g_client_bot_state.goal_target_waypoint);
                routed_towards_pickup = bot_internal_update_waypoint_target_towards(
                    local_entity,
                    g_client_bot_state.goal_target_pos,
                    nullptr,
                    pickup_obj,
                    scale_repath_ms(kCollectRecoveryRepathMs, true)
                );
            }
        }

        if (routed_towards_pickup && !has_move_target) {
            if (g_client_bot_state.last_pursuit_route_was_fallback
                || g_client_bot_state.recovery_pending_reroute) {
                ++g_client_bot_state.collect_route_failures;
            }
            else {
                g_client_bot_state.collect_route_failures = 0;
            }
            move_target = g_client_bot_state.waypoint_target_pos;
            has_move_target = true;
        }
        else if (!collectible_pickup_valid) {
            clear_unavailable_item_goal();
        }
        else {
            ++g_client_bot_state.collect_route_failures;
            if (g_client_bot_state.collect_route_failures >= kCollectRouteFailureLimit) {
                g_client_bot_state.collect_route_failures = 0;
                if (g_client_bot_state.goal_target_handle >= 0) {
                    bot_memory_manager_note_failed_goal_target(
                        g_client_bot_state.active_goal,
                        g_client_bot_state.goal_target_handle,
                        kFailedItemGoalCooldownMs
                    );
                }
                bot_state_clear_item_goal_contact_tracking();
                // Route-abort fallback should not wait before selecting
                // a replacement objective.
                bot_state_set_roam_fallback_goal(0);
                bot_internal_set_last_heading_change_reason("collect_route_abort");
                bot_state_clear_waypoint_route(true, true, false);
            }
            // Keep the goal stable when pathing momentarily fails; hard-abort is handled by watchdogs.
            else {
                bot_internal_start_recovery_anchor_reroute(local_entity, g_client_bot_state.goal_target_waypoint);
            }
        }
    }
    else if (!has_move_target && g_client_bot_state.active_goal == BotGoalType::eliminate_target) {
        bool routed_towards_hunt = false;
        rf::Vector3 hunt_pos = g_client_bot_state.goal_target_pos;
        if (g_client_bot_state.goal_target_handle >= 0) {
            if (rf::Entity* tracked_enemy =
                    rf::entity_from_handle(g_client_bot_state.goal_target_handle)) {
                hunt_pos = tracked_enemy->pos;
                g_client_bot_state.goal_target_pos = tracked_enemy->pos;
            }
        }

        if (hunt_pos.len_sq() > 0.0001f) {
            routed_towards_hunt = bot_internal_update_waypoint_target_towards(
                local_entity,
                hunt_pos,
                nullptr,
                nullptr,
                scale_repath_ms(kPursuitRepathIntervalMs)
            );
            if (!routed_towards_hunt) {
                bot_internal_start_recovery_anchor_reroute(
                    local_entity,
                    g_client_bot_state.waypoint_goal
                );
                routed_towards_hunt = bot_internal_update_waypoint_target_towards(
                    local_entity,
                    hunt_pos,
                    nullptr,
                    nullptr,
                    scale_repath_ms(kWaypointRecoveryRepathMs, true)
                );
            }
        }

        if (routed_towards_hunt) {
            if (g_client_bot_state.last_pursuit_route_was_fallback) {
                ++g_client_bot_state.pursuit_route_failures;
            }
            else {
                g_client_bot_state.pursuit_route_failures = 0;
            }
            move_target = g_client_bot_state.waypoint_target_pos;
            has_move_target = true;
        }
        else {
            ++g_client_bot_state.pursuit_route_failures;
            if (g_client_bot_state.pursuit_route_failures >= kPursuitRouteFailureLimit) {
                abandon_eliminate_goal_for_forage("seek_enemy_abort_to_forage");
            }
        }
    }

    if (!has_move_target) {
        has_move_target = bot_internal_update_waypoint_target(local_entity);
        if (has_move_target) {
            move_target = g_client_bot_state.waypoint_target_pos;
        }
    }

    if (!has_move_target && g_client_bot_state.fsm_state == BotFsmState::recover_navigation) {
        rf::Vector3 anchor_pos{};
        if (g_client_bot_state.recovery_anchor_waypoint > 0
            && waypoints_get_pos(g_client_bot_state.recovery_anchor_waypoint, anchor_pos)) {
            const float horizontal_sq = bot_horizontal_dist_sq(local_entity.pos, anchor_pos);
            const float vertical_delta = std::abs(anchor_pos.y - local_entity.pos.y);
            const bool vertical_deadlock =
                horizontal_sq
                    <= kRecoveryAnchorFallbackHorizontalDeadlockRadius
                        * kRecoveryAnchorFallbackHorizontalDeadlockRadius
                && vertical_delta >= kRecoveryAnchorFallbackVerticalDeadlockHeight;
            if (!vertical_deadlock) {
                g_client_bot_state.waypoint_target_pos = anchor_pos;
                g_client_bot_state.has_waypoint_target = true;
                move_target = anchor_pos;
                has_move_target = true;
            }
            else {
                bot_state_clear_recovery_reroute();
                g_client_bot_state.has_waypoint_target = false;
                has_move_target = bot_internal_update_waypoint_target(local_entity);
                if (has_move_target) {
                    move_target = g_client_bot_state.waypoint_target_pos;
                }
            }
        }
        else {
            // Avoid perma-sticking in recover mode when no valid recovery anchor exists.
            bot_state_clear_recovery_reroute();
            has_move_target = bot_internal_update_waypoint_target(local_entity);
            if (has_move_target) {
                move_target = g_client_bot_state.waypoint_target_pos;
            }
        }
    }

    if (!has_move_target
        && (g_client_bot_state.active_goal == BotGoalType::roam
            || g_client_bot_state.active_goal == BotGoalType::none
            || (g_client_bot_state.goal_target_waypoint <= 0
                && g_client_bot_state.waypoint_goal <= 0))) {
        if (bot_nav_try_emergency_direct_move_target(local_entity, move_target)) {
            has_move_target = true;
            g_client_bot_state.has_waypoint_target = false;
            bot_internal_set_last_heading_change_reason("direct_move_failsafe");
        }
    }

    if (!has_move_target
        && (g_client_bot_state.active_goal == BotGoalType::roam
            || g_client_bot_state.active_goal == BotGoalType::none)) {
        // Roam has no move target — clear stale route so the next frame builds
        // a fresh one. Do NOT invalidate goal_eval_timer here as that causes
        // constant re-evaluation churn where the bot cycles targets every frame
        // without ever committing to walking toward any of them.
        g_client_bot_state.repath_timer.invalidate();
        bot_state_clear_waypoint_route(false, false, false);
    }

    if (has_move_target
        && !force_direct_pickup_approach
        && bot_internal_try_recover_from_corner_probe(local_entity, move_target)) {
        move_target = g_client_bot_state.waypoint_target_pos;
        has_move_target = g_client_bot_state.has_waypoint_target;
    }
}
