#include "waypoints.h"
#include "alpine_settings.h"
#include "../main/main.h"
#include "../multi/gametype.h"
#include "../os/console.h"
#include <xlog/xlog.h>
#include "../rf/collide.h"
#include "../rf/entity.h"
#include "../rf/event.h"
#include "../rf/file/file.h"
#include "../rf/geometry.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../rf/item.h"
#include "../rf/level.h"
#include "../rf/object.h"
#include "../rf/player/player.h"
#include "../rf/player/camera.h"
#include "../rf/trigger.h"
#include "../object/object.h"
#include "../object/mover.h"
#include "../graphics/gr.h"
#include <common/utils/string-utils.h>
#include <patch_common/FunHook.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <queue>
#include <random>
#include <string>
#include <unordered_map>
#include <toml++/toml.hpp>
#include <windows.h>

std::vector<WaypointNode> g_waypoints;
std::vector<WaypointZoneDefinition> g_waypoint_zones;
std::vector<WaypointTargetDefinition> g_waypoint_targets;
std::vector<WpCacheNode> g_cache_nodes;
WpCacheNode* g_cache_root = nullptr;
bool g_cache_dirty = true;
bool g_has_loaded_wpt = false;
bool g_drop_waypoints = true;
int g_waypoint_revision = 0;
bool g_waypoints_compress = true;
bool g_waypoints_seed_bridges = true;
int g_next_waypoint_target_uid = 1;

std::unordered_map<int, int> g_last_drop_waypoint_by_entity{};
std::unordered_map<int, int> g_last_lift_uid_by_entity{};

int g_debug_waypoints_mode = 0;
bool g_drop_waypoints_prev = true;

FunHook<void(rf::Vector3*, float)> glass_remove_floating_shards_hook{
    0x00492400,
    [](rf::Vector3* pos, float radius) {
        if (pos) {
            on_geomod_crater_created(*pos, radius);
        }
        glass_remove_floating_shards_hook.call_target(pos, radius);
    },
};

rf::Vector3 get_entity_feet_pos(const rf::Entity& entity)
{
    return entity.pos;
}

void invalidate_cache()
{
    g_cache_root = nullptr;
    g_cache_nodes.clear();
    g_cache_dirty = true;
}

float sanitize_waypoint_link_radius(float link_radius)
{
    if (!std::isfinite(link_radius) || link_radius <= 0.0f) {
        return kWaypointLinkRadius;
    }
    return link_radius;
}

std::optional<int16_t> compress_waypoint_radius(float link_radius)
{
    const float sanitized_radius = sanitize_waypoint_link_radius(link_radius);
    const float packed_radius = std::round(sanitized_radius * kWaypointRadiusCompressionScale);
    if (!std::isfinite(packed_radius)
        || packed_radius < static_cast<float>(std::numeric_limits<int16_t>::min())
        || packed_radius > static_cast<float>(std::numeric_limits<int16_t>::max())) {
        return std::nullopt;
    }
    return static_cast<int16_t>(packed_radius);
}

float decompress_waypoint_radius(int16_t packed_radius)
{
    return sanitize_waypoint_link_radius(static_cast<float>(packed_radius) / kWaypointRadiusCompressionScale);
}

WaypointType waypoint_type_from_int(int raw_type)
{
    switch (raw_type) {
        case static_cast<int>(WaypointType::std):
            return WaypointType::std;
        case static_cast<int>(WaypointType::std_new):
            return WaypointType::std_new;
        case static_cast<int>(WaypointType::item):
            return WaypointType::item;
        case static_cast<int>(WaypointType::respawn):
            return WaypointType::respawn;
        case static_cast<int>(WaypointType::jump_pad):
            return WaypointType::jump_pad;
        case static_cast<int>(WaypointType::lift_body):
            return WaypointType::lift_body;
        case static_cast<int>(WaypointType::lift_entrance):
            return WaypointType::lift_entrance;
        case static_cast<int>(WaypointType::lift_exit):
            return WaypointType::lift_exit;
        case static_cast<int>(WaypointType::ladder):
            return WaypointType::ladder;
        case static_cast<int>(WaypointType::ctf_flag):
            return WaypointType::ctf_flag;
        case static_cast<int>(WaypointType::crater):
            return WaypointType::crater;
        case static_cast<int>(WaypointType::tele_entrance):
            return WaypointType::tele_entrance;
        case static_cast<int>(WaypointType::tele_exit):
            return WaypointType::tele_exit;
        default:
            return static_cast<WaypointType>(raw_type);
    }
}

int waypoint_type_to_save_value(WaypointType type)
{
    // Preserve compatibility with older files where dropped waypoints are encoded as 0.
    if (type == WaypointType::std_new) {
        return static_cast<int>(WaypointType::std);
    }
    return static_cast<int>(type);
}

std::string_view waypoint_type_name(WaypointType type)
{
    switch (type) {
        case WaypointType::std:
            return "std";
        case WaypointType::std_new:
            return "std_new";
        case WaypointType::item:
            return "item";
        case WaypointType::respawn:
            return "respawn";
        case WaypointType::jump_pad:
            return "jump_pad";
        case WaypointType::lift_body:
            return "lift_body";
        case WaypointType::lift_entrance:
            return "lift_entrance";
        case WaypointType::lift_exit:
            return "lift_exit";
        case WaypointType::ladder:
            return "ladder";
        case WaypointType::ctf_flag:
            return "ctf_flag";
        case WaypointType::crater:
            return "crater";
        case WaypointType::tele_entrance:
            return "tele_entrance";
        case WaypointType::tele_exit:
            return "tele_exit";
        default:
            return "unknown";
    }
}

WaypointTargetType waypoint_target_type_from_int(int raw_type)
{
    switch (raw_type) {
        case static_cast<int>(WaypointTargetType::explosion):
            return WaypointTargetType::explosion;
        default:
            return static_cast<WaypointTargetType>(raw_type);
    }
}

std::string_view waypoint_target_type_name(WaypointTargetType type)
{
    switch (type) {
        case WaypointTargetType::explosion:
            return "explosion";
        default:
            return "unknown";
    }
}

WaypointZoneType waypoint_zone_type_from_int(int raw_type)
{
    switch (raw_type) {
        case static_cast<int>(WaypointZoneType::control_point):
            return WaypointZoneType::control_point;
        case static_cast<int>(WaypointZoneType::damaging_liquid_room):
            return WaypointZoneType::damaging_liquid_room;
        case static_cast<int>(WaypointZoneType::damage_zone):
            return WaypointZoneType::damage_zone;
        case static_cast<int>(WaypointZoneType::instant_death_zone):
            return WaypointZoneType::instant_death_zone;
        default:
            return static_cast<WaypointZoneType>(raw_type);
    }
}

WaypointZoneSource waypoint_zone_source_from_int(int raw_source)
{
    switch (raw_source) {
        case static_cast<int>(WaypointZoneSource::trigger_uid):
            return WaypointZoneSource::trigger_uid;
        case static_cast<int>(WaypointZoneSource::room_uid):
            return WaypointZoneSource::room_uid;
        case static_cast<int>(WaypointZoneSource::box_extents):
            return WaypointZoneSource::box_extents;
        default:
            return static_cast<WaypointZoneSource>(raw_source);
    }
}

WaypointZoneSource resolve_waypoint_zone_source(const WaypointZoneDefinition& zone)
{
    if (zone.trigger_uid >= 0) {
        return WaypointZoneSource::trigger_uid;
    }
    if (zone.room_uid >= 0) {
        return WaypointZoneSource::room_uid;
    }
    return WaypointZoneSource::box_extents;
}

std::string_view waypoint_zone_type_name(WaypointZoneType type)
{
    switch (type) {
        case WaypointZoneType::control_point:
            return "control_point";
        case WaypointZoneType::damaging_liquid_room:
            return "damaging_liquid_room";
        case WaypointZoneType::damage_zone:
            return "damage_zone";
        case WaypointZoneType::instant_death_zone:
            return "instant_death_zone";
        default:
            return "unknown";
    }
}

std::string_view waypoint_zone_source_name(WaypointZoneSource source)
{
    switch (source) {
        case WaypointZoneSource::trigger_uid:
            return "trigger_uid";
        case WaypointZoneSource::room_uid:
            return "room_uid";
        case WaypointZoneSource::box_extents:
            return "box_extents";
        default:
            return "unknown";
    }
}

std::optional<WaypointZoneType> parse_waypoint_zone_type_token(std::string_view token)
{
    if (token.empty()) {
        return std::nullopt;
    }

    int numeric_type = 0;
    const char* begin = token.data();
    const char* end = begin + token.size();
    if (auto [ptr, ec] = std::from_chars(begin, end, numeric_type); ec == std::errc{} && ptr == end) {
        return waypoint_zone_type_from_int(numeric_type);
    }

    if (string_iequals(token, "control_point") || string_iequals(token, "cp")) {
        return WaypointZoneType::control_point;
    }
    if (string_iequals(token, "liquid") || string_iequals(token, "liquid_area")
        || string_iequals(token, "damaging_liquid_room")) {
        return WaypointZoneType::damaging_liquid_room;
    }
    if (string_iequals(token, "damage") || string_iequals(token, "damage_zone")) {
        return WaypointZoneType::damage_zone;
    }
    if (string_iequals(token, "instant_death") || string_iequals(token, "instant_death_zone")
        || string_iequals(token, "death")) {
        return WaypointZoneType::instant_death_zone;
    }

    return std::nullopt;
}

std::optional<WaypointZoneSource> parse_waypoint_zone_source_token(std::string_view token)
{
    if (token.empty()) {
        return std::nullopt;
    }

    int numeric_source = 0;
    const char* begin = token.data();
    const char* end = begin + token.size();
    if (auto [ptr, ec] = std::from_chars(begin, end, numeric_source); ec == std::errc{} && ptr == end) {
        return waypoint_zone_source_from_int(numeric_source);
    }

    if (string_iequals(token, "trigger") || string_iequals(token, "trigger_uid")) {
        return WaypointZoneSource::trigger_uid;
    }
    if (string_iequals(token, "room") || string_iequals(token, "room_uid")) {
        return WaypointZoneSource::room_uid;
    }
    if (string_iequals(token, "box") || string_iequals(token, "box_extents")) {
        return WaypointZoneSource::box_extents;
    }

    return std::nullopt;
}

std::vector<std::string_view> tokenize_console_command_line(std::string_view command_line)
{
    std::vector<std::string_view> tokens{};
    size_t index = 0;
    while (index < command_line.size()) {
        while (index < command_line.size()
               && std::isspace(static_cast<unsigned char>(command_line[index]))) {
            ++index;
        }
        if (index >= command_line.size()) {
            break;
        }
        const size_t start = index;
        while (index < command_line.size()
               && !std::isspace(static_cast<unsigned char>(command_line[index]))) {
            ++index;
        }
        tokens.push_back(command_line.substr(start, index - start));
    }
    return tokens;
}

std::optional<int> parse_int_token(std::string_view token)
{
    if (token.empty()) {
        return std::nullopt;
    }

    int value = 0;
    const char* begin = token.data();
    const char* end = begin + token.size();
    if (auto [ptr, ec] = std::from_chars(begin, end, value); ec == std::errc{} && ptr == end) {
        return value;
    }
    return std::nullopt;
}

std::optional<float> parse_float_token(std::string_view token)
{
    if (token.empty()) {
        return std::nullopt;
    }

    float value = 0.0f;
    const char* begin = token.data();
    const char* end = begin + token.size();
    if (auto [ptr, ec] = std::from_chars(begin, end, value); ec == std::errc{} && ptr == end) {
        return value;
    }
    return std::nullopt;
}

std::optional<WaypointTargetType> parse_waypoint_target_type_token(std::string_view token)
{
    if (token.empty()) {
        return std::nullopt;
    }

    int numeric_type = 0;
    const char* begin = token.data();
    const char* end = begin + token.size();
    if (auto [ptr, ec] = std::from_chars(begin, end, numeric_type); ec == std::errc{} && ptr == end) {
        if (numeric_type == static_cast<int>(WaypointTargetType::explosion)) {
            return WaypointTargetType::explosion;
        }
        return std::nullopt;
    }

    if (string_iequals(token, "explosion")) {
        return WaypointTargetType::explosion;
    }

    return std::nullopt;
}

float waypoint_link_radius_from_push_region(const rf::PushRegion& push_region)
{
    if (push_region.shape == 0) { // sphere
        return sanitize_waypoint_link_radius(push_region.radius_pow2);
    }
    if (push_region.shape == 1 || push_region.shape == 2) { // box
        // vExtents appears to represent box diameter in this context; convert to radius.
        return sanitize_waypoint_link_radius(push_region.vExtents.len() * 0.5f);
    }
    return kWaypointLinkRadius;
}

float waypoint_link_radius_from_trigger(const rf::Trigger& trigger)
{
    if (trigger.type == 0) { // sphere
        return sanitize_waypoint_link_radius(std::fabs(trigger.radius));
    }
    if (trigger.type == 1) { // box
        return sanitize_waypoint_link_radius(trigger.box_size.len() * 0.5f);
    }
    return kWaypointLinkRadius;
}

bool waypoint_link_exists(const WaypointNode& node, int link)
{
    for (int i = 0; i < node.num_links; ++i) {
        if (node.links[i] == link) {
            return true;
        }
    }
    return false;
}

void link_waypoint(int from, int to)
{
    if (from <= 0 || to <= 0 || from >= static_cast<int>(g_waypoints.size()) ||
        to >= static_cast<int>(g_waypoints.size())) {
        return;
    }
    auto& node = g_waypoints[from];
    if (waypoint_link_exists(node, to)) {
        return;
    }
    if (node.num_links < kMaxWaypointLinks) {
        node.links[node.num_links++] = to;
        return;
    }
    std::uniform_int_distribution<int> dist(0, kMaxWaypointLinks - 1);
    node.links[dist(g_rng)] = to;
}

bool is_player_grounded(const rf::Entity& entity)
{
    return rf::entity_is_running(const_cast<rf::Entity*>(&entity));
}

rf::Vector3 point_min(const rf::Vector3& a, const rf::Vector3& b)
{
    return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
}

rf::Vector3 point_max(const rf::Vector3& a, const rf::Vector3& b)
{
    return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}

void update_bounds(WpCacheNode& node, const rf::Vector3& pos)
{
    node.min = point_min(node.min, pos);
    node.max = point_max(node.max, pos);
}

float distance_sq(const rf::Vector3& a, const rf::Vector3& b)
{
    rf::Vector3 d = a - b;
    return d.dot_prod(d);
}

rf::ShortVector compress_waypoint_pos(const rf::Vector3& pos)
{
    rf::ShortVector compressed = rf::ShortVector::from(pos);
    if (rf::level.geometry) {
        rf::ShortVector packed{};
        rf::Vector3 in_pos = pos;
        if (rf::compress_vector3(rf::level.geometry, &in_pos, &packed)) {
            compressed = packed;
        }
    }
    return compressed;
}

rf::Vector3 decompress_waypoint_pos(const rf::ShortVector& pos)
{
    rf::Vector3 out{static_cast<float>(pos.x), static_cast<float>(pos.y), static_cast<float>(pos.z)};
    if (rf::level.geometry) {
        rf::decompress_vector3(rf::level.geometry, &pos, &out);
    }
    return out;
}

std::optional<rf::Vector3> parse_waypoint_pos(const toml::array& pos, bool header_compressed)
{
    if (pos.size() != 3) {
        return std::nullopt;
    }
    bool has_float = false;
    bool all_int = true;
    for (const auto& entry : pos) {
        has_float |= entry.is_floating_point();
        all_int &= entry.is_integer();
    }
    bool should_use_short = header_compressed;
    if (!header_compressed) {
        if (!has_float && all_int) {
            should_use_short = true;
            for (const auto& entry : pos) {
                int value = entry.value_or<int>(0);
                if (value < std::numeric_limits<int16_t>::min() || value > std::numeric_limits<int16_t>::max()) {
                    should_use_short = false;
                    break;
                }
            }
        }
    }
    if (should_use_short) {
        rf::ShortVector compressed{
            static_cast<int16_t>(pos[0].value_or<int>(0)),
            static_cast<int16_t>(pos[1].value_or<int>(0)),
            static_cast<int16_t>(pos[2].value_or<int>(0)),
        };
        return decompress_waypoint_pos(compressed);
    }
    rf::Vector3 wp_pos{
        static_cast<float>(pos[0].value_or<double>(0.0)),
        static_cast<float>(pos[1].value_or<double>(0.0)),
        static_cast<float>(pos[2].value_or<double>(0.0)),
    };
    return wp_pos;
}

std::optional<rf::Vector3> parse_vec3_floats(const toml::array& values)
{
    if (values.size() != 3) {
        return std::nullopt;
    }

    return rf::Vector3{
        static_cast<float>(values[0].value_or<double>(0.0)),
        static_cast<float>(values[1].value_or<double>(0.0)),
        static_cast<float>(values[2].value_or<double>(0.0)),
    };
}

void normalize_zone_box_bounds(WaypointZoneDefinition& zone)
{
    const rf::Vector3 min_bound = point_min(zone.box_min, zone.box_max);
    const rf::Vector3 max_bound = point_max(zone.box_min, zone.box_max);
    zone.box_min = min_bound;
    zone.box_max = max_bound;
}

bool parse_waypoint_zone_bounds(const toml::table& zone_tbl, rf::Vector3& out_min, rf::Vector3& out_max)
{
    const auto* min_node = zone_tbl.get_as<toml::array>("mn");
    const auto* max_node = zone_tbl.get_as<toml::array>("mx");

    if (!min_node || !max_node) {
        return false;
    }

    auto min_opt = parse_vec3_floats(*min_node);
    auto max_opt = parse_vec3_floats(*max_node);
    if (!min_opt || !max_opt) {
        return false;
    }

    out_min = min_opt.value();
    out_max = max_opt.value();
    return true;
}

bool parse_waypoint_zone_definition(const toml::table& zone_tbl, WaypointZoneDefinition& out_zone)
{
    WaypointZoneDefinition zone{};

    if (const auto* type_node = zone_tbl.get("t"); type_node && type_node->is_number()) {
        zone.type = waypoint_zone_type_from_int(static_cast<int>(type_node->value_or(0)));
    }
    if (const auto* identifier_node = zone_tbl.get("i"); identifier_node && identifier_node->is_number()) {
        zone.identifier = static_cast<int>(identifier_node->value_or(-1));
    }
    else if (const auto* identifier_node = zone_tbl.get("id"); identifier_node && identifier_node->is_number()) {
        zone.identifier = static_cast<int>(identifier_node->value_or(-1));
    }
    else if (const auto* identifier_node = zone_tbl.get("identifier"); identifier_node && identifier_node->is_number()) {
        zone.identifier = static_cast<int>(identifier_node->value_or(-1));
    }

    bool has_trigger_uid = false;
    bool has_room_uid = false;
    bool has_box_extents = false;

    if (const auto* trigger_uid_node = zone_tbl.get("t_uid"); trigger_uid_node && trigger_uid_node->is_number()) {
        zone.trigger_uid = static_cast<int>(trigger_uid_node->value_or(-1));
        has_trigger_uid = zone.trigger_uid >= 0;
    }
    if (const auto* room_uid_node = zone_tbl.get("r_uid"); room_uid_node && room_uid_node->is_number()) {
        zone.room_uid = static_cast<int>(room_uid_node->value_or(-1));
        has_room_uid = zone.room_uid >= 0;
    }
    if (parse_waypoint_zone_bounds(zone_tbl, zone.box_min, zone.box_max)) {
        has_box_extents = true;
    }

    if (!has_trigger_uid && !has_room_uid && !has_box_extents) {
        return false;
    }

    if (has_box_extents) {
        normalize_zone_box_bounds(zone);
    }

    out_zone = zone;
    return true;
}

bool point_inside_axis_aligned_box(const rf::Vector3& point, const rf::Vector3& min_bound, const rf::Vector3& max_bound)
{
    return point.x >= min_bound.x && point.x <= max_bound.x
        && point.y >= min_bound.y && point.y <= max_bound.y
        && point.z >= min_bound.z && point.z <= max_bound.z;
}

bool point_inside_trigger_zone(const rf::Trigger& trigger, const rf::Vector3& point)
{
    if (trigger.type == 1) {
        const rf::Vector3 half_extents{
            std::fabs(trigger.box_size.x) * 0.5f,
            std::fabs(trigger.box_size.y) * 0.5f,
            std::fabs(trigger.box_size.z) * 0.5f,
        };
        const rf::Vector3 delta = point - trigger.pos;
        const float local_x = std::fabs(delta.dot_prod(trigger.orient.rvec));
        const float local_y = std::fabs(delta.dot_prod(trigger.orient.uvec));
        const float local_z = std::fabs(delta.dot_prod(trigger.orient.fvec));
        return local_x <= half_extents.x
            && local_y <= half_extents.y
            && local_z <= half_extents.z;
    }

    const float radius = std::fabs(trigger.radius);
    if (radius <= 0.0f) {
        return false;
    }
    return distance_sq(point, trigger.pos) <= radius * radius;
}

bool point_inside_room_zone(int room_uid, const rf::Vector3& point, const rf::Object* source_object)
{
    if (source_object && source_object->room && source_object->room->uid == room_uid) {
        return true;
    }

    const rf::GRoom* room = rf::level_room_from_uid(room_uid);
    if (!room) {
        return false;
    }

    return point_inside_axis_aligned_box(point, room->bbox_min, room->bbox_max);
}

bool waypoint_zone_contains_point(const WaypointZoneDefinition& zone, const rf::Vector3& point, const rf::Object* source_object)
{
    switch (resolve_waypoint_zone_source(zone)) {
        case WaypointZoneSource::trigger_uid: {
            rf::Object* trigger_obj = rf::obj_lookup_from_uid(zone.trigger_uid);
            if (!trigger_obj || trigger_obj->type != rf::OT_TRIGGER) {
                return false;
            }
            const auto* trigger = static_cast<rf::Trigger*>(trigger_obj);
            return point_inside_trigger_zone(*trigger, point);
        }
        case WaypointZoneSource::room_uid:
            // Room zones intentionally never associate directly with waypoints.
            return false;
        case WaypointZoneSource::box_extents:
            return point_inside_axis_aligned_box(point, zone.box_min, zone.box_max);
        default:
            return false;
    }
}

void normalize_waypoint_zone_refs(std::vector<int>& zone_refs)
{
    zone_refs.erase(
        std::remove_if(zone_refs.begin(), zone_refs.end(), [](int zone_index) {
            if (zone_index < 0 || zone_index >= static_cast<int>(g_waypoint_zones.size())) {
                return true;
            }
            const auto& zone = g_waypoint_zones[zone_index];
            return resolve_waypoint_zone_source(zone) == WaypointZoneSource::room_uid;
        }),
        zone_refs.end());
    std::sort(zone_refs.begin(), zone_refs.end());
    zone_refs.erase(std::unique(zone_refs.begin(), zone_refs.end()), zone_refs.end());
}

std::vector<int> collect_waypoint_zone_refs(const rf::Vector3& point, const rf::Object* source_object)
{
    std::vector<int> zone_refs{};
    zone_refs.reserve(g_waypoint_zones.size());
    for (int zone_index = 0; zone_index < static_cast<int>(g_waypoint_zones.size()); ++zone_index) {
        if (waypoint_zone_contains_point(g_waypoint_zones[zone_index], point, source_object)) {
            zone_refs.push_back(zone_index);
        }
    }
    return zone_refs;
}

void refresh_all_waypoint_zone_refs()
{
    for (int i = 1; i < static_cast<int>(g_waypoints.size()); ++i) {
        auto& node = g_waypoints[i];
        if (!node.valid) {
            node.zones.clear();
            continue;
        }
        node.zones = collect_waypoint_zone_refs(node.pos, nullptr);
    }
}

int add_waypoint_zone_definition(WaypointZoneDefinition zone)
{
    if (resolve_waypoint_zone_source(zone) == WaypointZoneSource::box_extents) {
        normalize_zone_box_bounds(zone);
    }

    g_waypoint_zones.push_back(zone);
    refresh_all_waypoint_zone_refs();
    return static_cast<int>(g_waypoint_zones.size()) - 1;
}

bool remove_waypoint_zone_definition(int zone_uid)
{
    if (zone_uid < 0 || zone_uid >= static_cast<int>(g_waypoint_zones.size())) {
        return false;
    }

    g_waypoint_zones.erase(g_waypoint_zones.begin() + zone_uid);
    refresh_all_waypoint_zone_refs();
    return true;
}

WaypointTargetDefinition* find_waypoint_target_by_uid(int target_uid)
{
    auto it = std::find_if(
        g_waypoint_targets.begin(),
        g_waypoint_targets.end(),
        [target_uid](const WaypointTargetDefinition& target) { return target.uid == target_uid; });
    if (it == g_waypoint_targets.end()) {
        return nullptr;
    }
    return &(*it);
}

bool waypoint_target_uid_exists(int target_uid)
{
    return find_waypoint_target_by_uid(target_uid) != nullptr;
}

void normalize_target_waypoint_uids(std::vector<int>& waypoint_uids)
{
    waypoint_uids.erase(
        std::remove_if(waypoint_uids.begin(), waypoint_uids.end(), [](int waypoint_uid) {
            if (waypoint_uid <= 0 || waypoint_uid >= static_cast<int>(g_waypoints.size())) {
                return true;
            }
            return !g_waypoints[waypoint_uid].valid;
        }),
        waypoint_uids.end());
    std::sort(waypoint_uids.begin(), waypoint_uids.end());
    waypoint_uids.erase(std::unique(waypoint_uids.begin(), waypoint_uids.end()), waypoint_uids.end());
}

std::vector<int> collect_target_waypoint_uids(const rf::Vector3& pos)
{
    std::vector<int> waypoint_uids{};
    waypoint_uids.reserve(g_waypoints.size());
    const float link_radius_sq = kWaypointLinkRadius * kWaypointLinkRadius;
    for (int i = 1; i < static_cast<int>(g_waypoints.size()); ++i) {
        const auto& node = g_waypoints[i];
        if (!node.valid) {
            continue;
        }
        if (distance_sq(pos, node.pos) <= link_radius_sq) {
            waypoint_uids.push_back(i);
        }
    }
    normalize_target_waypoint_uids(waypoint_uids);
    return waypoint_uids;
}

int allocate_waypoint_target_uid()
{
    if (g_next_waypoint_target_uid < 1) {
        g_next_waypoint_target_uid = 1;
    }
    while (waypoint_target_uid_exists(g_next_waypoint_target_uid)) {
        ++g_next_waypoint_target_uid;
    }
    return g_next_waypoint_target_uid++;
}

int resolve_waypoint_target_uid(std::optional<int> preferred_uid = std::nullopt)
{
    if (preferred_uid && preferred_uid.value() > 0 && !waypoint_target_uid_exists(preferred_uid.value())) {
        g_next_waypoint_target_uid = std::max(g_next_waypoint_target_uid, preferred_uid.value() + 1);
        return preferred_uid.value();
    }
    return allocate_waypoint_target_uid();
}

int add_waypoint_target(const rf::Vector3& pos, WaypointTargetType type, std::optional<int> preferred_uid = std::nullopt)
{
    WaypointTargetDefinition target{};
    target.uid = resolve_waypoint_target_uid(preferred_uid);
    target.pos = pos;
    target.type = type;
    target.waypoint_uids = collect_target_waypoint_uids(pos);
    g_waypoint_targets.push_back(std::move(target));
    return g_waypoint_targets.back().uid;
}

bool remove_waypoint_target_by_uid(int target_uid)
{
    auto it = std::find_if(
        g_waypoint_targets.begin(),
        g_waypoint_targets.end(),
        [target_uid](const WaypointTargetDefinition& target) { return target.uid == target_uid; });
    if (it == g_waypoint_targets.end()) {
        return false;
    }
    g_waypoint_targets.erase(it);
    return true;
}

void remove_realized_waypoint_targets(const rf::Vector3& crater_pos, float crater_radius)
{
    if (g_waypoint_targets.empty() || !std::isfinite(crater_radius)) {
        return;
    }

    const float radius = std::fabs(crater_radius);
    const float radius_sq = radius * radius;
    g_waypoint_targets.erase(
        std::remove_if(
            g_waypoint_targets.begin(),
            g_waypoint_targets.end(),
            [&crater_pos, radius_sq](const WaypointTargetDefinition& target) {
                if (target.type != WaypointTargetType::explosion) {
                    return false;
                }
                return distance_sq(crater_pos, target.pos) <= radius_sq;
            }),
        g_waypoint_targets.end());
}

std::optional<rf::Vector3> get_looked_at_target_point()
{
    rf::Player* player = rf::local_player;
    if (!player || !player->cam) {
        return std::nullopt;
    }

    rf::Vector3 p0 = rf::camera_get_pos(player->cam);
    rf::Matrix3 orient = rf::camera_get_orient(player->cam);
    rf::Vector3 p1 = p0 + orient.fvec * 10000.0f;
    rf::Entity* entity = rf::entity_from_handle(player->entity_handle);

    rf::LevelCollisionOut col_info{};
    col_info.face = nullptr;
    col_info.obj_handle = -1;
    const bool hit = rf::collide_linesegment_level_for_multi(p0, p1, entity, nullptr, &col_info, 0.1f, false, 1.0f);
    if (!hit) {
        return std::nullopt;
    }
    return col_info.hit_point;
}

int find_nearest_waypoint(const rf::Vector3& pos, float radius, int exclude)
{
    float best_dist_sq = radius * radius;
    int best_index = 0;
    for (int i = 1; i < static_cast<int>(g_waypoints.size()); ++i) {
        if (i == exclude || !g_waypoints[i].valid) {
            continue;
        }
        float dist_sq = distance_sq(pos, g_waypoints[i].pos);
        if (dist_sq < best_dist_sq) {
            best_dist_sq = dist_sq;
            best_index = i;
        }
    }
    return best_index;
}

float waypoint_auto_link_detection_radius(const WaypointNode& node)
{
    switch (node.type) {
        case WaypointType::jump_pad:
            return sanitize_waypoint_link_radius(node.link_radius) * kJumpPadAutoLinkRangeScale;
        case WaypointType::tele_entrance:
            return sanitize_waypoint_link_radius(node.link_radius) * kTeleEntranceAutoLinkRangeScale;
        default:
            return kWaypointRadius;
    }
}

int find_jump_pad_waypoint_in_radius(const rf::Vector3& pos)
{
    float best_dist_sq = std::numeric_limits<float>::max();
    int best_index = 0;
    for (int i = 1; i < static_cast<int>(g_waypoints.size()); ++i) {
        const auto& node = g_waypoints[i];
        if (!node.valid || node.type != WaypointType::jump_pad) {
            continue;
        }

        const float radius = waypoint_auto_link_detection_radius(node);
        const float dist_sq = distance_sq(pos, node.pos);
        if (dist_sq > radius * radius) {
            continue;
        }

        if (dist_sq < best_dist_sq) {
            best_dist_sq = dist_sq;
            best_index = i;
        }
    }
    return best_index;
}

int find_tele_entrance_waypoint_in_radius(const rf::Vector3& pos)
{
    float best_dist_sq = std::numeric_limits<float>::max();
    int best_index = 0;
    for (int i = 1; i < static_cast<int>(g_waypoints.size()); ++i) {
        const auto& node = g_waypoints[i];
        if (!node.valid || node.type != WaypointType::tele_entrance) {
            continue;
        }

        const float radius = waypoint_auto_link_detection_radius(node);
        const float dist_sq = distance_sq(pos, node.pos);
        if (dist_sq > radius * radius) {
            continue;
        }

        if (dist_sq < best_dist_sq) {
            best_dist_sq = dist_sq;
            best_index = i;
        }
    }
    return best_index;
}

int find_lift_uid_below_waypoint(const rf::Vector3& pos)
{
    constexpr float kLiftTraceDistance = 4.0f;
    rf::Vector3 p0 = pos;
    rf::Vector3 p1 = pos - rf::Vector3{0.0f, kLiftTraceDistance, 0.0f};
    rf::PCollisionOut collision{};
    collision.obj_handle = -1;
    const bool hit = rf::collide_linesegment_world(p0, p1, 0, &collision);
    if (!hit || collision.obj_handle < 0) {
        return -1;
    }
    rf::Object* hit_obj = rf::obj_from_handle(collision.obj_handle);
    if (!hit_obj || hit_obj->type != rf::OT_MOVER_BRUSH) {
        return -1;
    }

    auto* mover_brush = static_cast<rf::MoverBrush*>(hit_obj);
    rf::Mover* mover = mover_find_by_mover_brush(mover_brush);
    if (!mover) {
        return -1;
    }
    return mover->uid;
}

bool should_skip_default_item_waypoint(std::string_view item_name)
{
    static constexpr const char* kSkippedItemWaypointNames[] = {
        "Brainstem",
        "keycard",
        "Demo_K000",
        "Doctor Uniform",
        "flag_red",
        "flag_blue",
        "base_red",
        "base_blue",
        "CTF Banner Red",
        "CTF Banner Blue",
    };

    return std::any_of(
        std::begin(kSkippedItemWaypointNames),
        std::end(kSkippedItemWaypointNames),
        [item_name](const char* skipped_name) { return string_iequals(item_name, skipped_name); });
}

std::optional<WaypointCtfFlagSubtype> get_default_grid_ctf_flag_subtype(std::string_view item_name)
{
    if (string_iequals(item_name, "flag_red")) {
        return WaypointCtfFlagSubtype::red;
    }
    if (string_iequals(item_name, "flag_blue")) {
        return WaypointCtfFlagSubtype::blue;
    }
    return std::nullopt;
}

void seed_waypoint_zones_from_trigger_damage_events()
{
    rf::Object* obj = rf::object_list.next_obj;
    while (obj != &rf::object_list) {
        if (obj->type != rf::OT_TRIGGER) {
            obj = obj->next_obj;
            continue;
        }

        auto* trigger = static_cast<rf::Trigger*>(obj);
        if (trigger->uid < 0 || trigger->links.size() != 1) {
            obj = obj->next_obj;
            continue;
        }

        const int linked_handle = trigger->links[0];
        rf::Object* linked_obj = rf::obj_from_handle(linked_handle);
        if (!linked_obj || linked_obj->type != rf::OT_EVENT) {
            obj = obj->next_obj;
            continue;
        }

        auto* event = static_cast<rf::Event*>(linked_obj);
        if (event->event_type != rf::event_type_to_int(rf::EventType::Continuous_Damage)) {
            obj = obj->next_obj;
            continue;
        }

        auto* continuous_damage_event = static_cast<rf::ContinuousDamageEvent*>(event);
        if (continuous_damage_event->damage_per_second < 0) {
            obj = obj->next_obj;
            continue;
        }

        WaypointZoneDefinition zone{};
        zone.type = (continuous_damage_event->damage_per_second == 0)
            ? WaypointZoneType::instant_death_zone
            : WaypointZoneType::damage_zone;
        zone.trigger_uid = trigger->uid;
        add_waypoint_zone_definition(zone);

        obj = obj->next_obj;
    }
}

bool has_control_point_zone_for_hill(int trigger_uid, int handler_uid)
{
    return std::any_of(g_waypoint_zones.begin(), g_waypoint_zones.end(), [trigger_uid, handler_uid](const auto& zone) {
        return zone.type == WaypointZoneType::control_point
            && resolve_waypoint_zone_source(zone) == WaypointZoneSource::trigger_uid
            && zone.trigger_uid == trigger_uid
            && zone.identifier == handler_uid;
    });
}

void seed_waypoint_zones_from_control_points()
{
    if (!multi_is_game_type_with_hills()) {
        return;
    }

    for (const auto& hill : g_koth_info.hills) {
        if (hill.trigger_uid < 0) {
            continue;
        }

        rf::Object* trigger_obj = rf::obj_lookup_from_uid(hill.trigger_uid);
        if (!trigger_obj || trigger_obj->type != rf::OT_TRIGGER) {
            continue;
        }

        int handler_uid = -1;
        if (hill.handler) {
            auto* handler_event = reinterpret_cast<rf::Event*>(hill.handler);
            if (handler_event->event_type == rf::event_type_to_int(rf::EventType::Capture_Point_Handler)) {
                handler_uid = handler_event->uid;
            }
        }

        if (handler_uid >= 0) {
            rf::Event* handler_event = rf::event_lookup_from_uid(handler_uid);
            if (!handler_event
                || handler_event->event_type != rf::event_type_to_int(rf::EventType::Capture_Point_Handler)) {
                continue;
            }
        }

        if (has_control_point_zone_for_hill(hill.trigger_uid, handler_uid)) {
            continue;
        }

        WaypointZoneDefinition zone{};
        zone.type = WaypointZoneType::control_point;
        zone.trigger_uid = hill.trigger_uid;
        zone.identifier = handler_uid;
        add_waypoint_zone_definition(zone);
    }
}

void seed_waypoint_zones_from_damaging_liquid_rooms()
{
    if (!rf::level.geometry) {
        return;
    }

    for (int i = 0; i < rf::level.geometry->all_rooms.size(); ++i) {
        const auto* room = rf::level.geometry->all_rooms[i];
        if (!room || room->uid < 0 || !room->contains_liquid) {
            continue;
        }
        if (room->liquid_type != 2 && room->liquid_type != 3) {
            continue;
        }

        WaypointZoneDefinition zone{};
        zone.type = WaypointZoneType::damaging_liquid_room;
        zone.room_uid = room->uid;
        add_waypoint_zone_definition(zone);
    }
}

void seed_waypoints_from_teleport_events(
    std::vector<int>* out_seeded_indices = nullptr, std::vector<int>* out_auto_link_source_indices = nullptr)
{
    const auto teleport_events = rf::find_all_events_by_type(rf::EventType::AF_Teleport_Player);
    if (teleport_events.empty()) {
        return;
    }

    std::unordered_map<int, int> tele_exit_by_event_uid{};
    tele_exit_by_event_uid.reserve(teleport_events.size());

    for (auto* event : teleport_events) {
        if (!event) {
            continue;
        }
        const int event_uid = event->uid;
        const int tele_exit_index = add_waypoint(
            event->pos, WaypointType::tele_exit, 0, false, true, kWaypointLinkRadius, event_uid, event, true);
        tele_exit_by_event_uid[event_uid] = tele_exit_index;
        if (out_seeded_indices) {
            out_seeded_indices->push_back(tele_exit_index);
        }
        if (out_auto_link_source_indices) {
            out_auto_link_source_indices->push_back(tele_exit_index);
        }
    }

    if (tele_exit_by_event_uid.empty()) {
        return;
    }

    std::unordered_set<uint64_t> seeded_entrance_pairs{};
    rf::Object* obj = rf::object_list.next_obj;
    while (obj != &rf::object_list) {
        if (obj->type != rf::OT_TRIGGER) {
            obj = obj->next_obj;
            continue;
        }

        auto* trigger = static_cast<rf::Trigger*>(obj);
        for (int i = 0; i < trigger->links.size(); ++i) {
            const int linked_id = trigger->links[i];
            int linked_teleport_uid = -1;

            if (rf::Object* linked_obj = rf::obj_from_handle(linked_id);
                linked_obj && linked_obj->type == rf::OT_EVENT) {
                auto* linked_event = static_cast<rf::Event*>(linked_obj);
                if (linked_event->event_type == rf::event_type_to_int(rf::EventType::AF_Teleport_Player)) {
                    linked_teleport_uid = linked_event->uid;
                }
            }

            if (linked_teleport_uid < 0) {
                if (rf::Event* linked_event = rf::event_lookup_from_uid(linked_id); linked_event) {
                    if (linked_event->event_type == rf::event_type_to_int(rf::EventType::AF_Teleport_Player)) {
                        linked_teleport_uid = linked_event->uid;
                    }
                }
            }

            if (linked_teleport_uid < 0) {
                continue;
            }

            auto exit_it = tele_exit_by_event_uid.find(linked_teleport_uid);
            if (exit_it == tele_exit_by_event_uid.end()) {
                continue;
            }

            const uint64_t pair_key =
                (static_cast<uint64_t>(static_cast<uint32_t>(trigger->uid)) << 32)
                | static_cast<uint32_t>(linked_teleport_uid);
            if (!seeded_entrance_pairs.insert(pair_key).second) {
                continue;
            }

            const float link_radius = waypoint_link_radius_from_trigger(*trigger) + 1.0f;
            const int tele_entrance_index = add_waypoint(
                trigger->pos, WaypointType::tele_entrance, 0, false, true, link_radius, linked_teleport_uid,
                trigger, true);
            if (out_seeded_indices) {
                out_seeded_indices->push_back(tele_entrance_index);
            }
            if (out_auto_link_source_indices) {
                out_auto_link_source_indices->push_back(tele_entrance_index);
            }
            link_waypoint(tele_entrance_index, exit_it->second);
        }

        obj = obj->next_obj;
    }
}

uint64_t make_waypoint_pair_key(int a, int b)
{
    const int min_index = std::min(a, b);
    const int max_index = std::max(a, b);
    return (static_cast<uint64_t>(static_cast<uint32_t>(min_index)) << 32)
        | static_cast<uint32_t>(max_index);
}

bool waypoint_has_link_to(int from, int to)
{
    if (from <= 0 || to <= 0
        || from >= static_cast<int>(g_waypoints.size())
        || to >= static_cast<int>(g_waypoints.size())) {
        return false;
    }

    const auto& node = g_waypoints[from];
    if (!node.valid || !g_waypoints[to].valid) {
        return false;
    }

    return waypoint_link_exists(node, to);
}

bool waypoints_are_bidirectionally_linked(int a, int b)
{
    return waypoint_has_link_to(a, b) && waypoint_has_link_to(b, a);
}

int compute_bridge_intermediate_count(const rf::Vector3& from, const rf::Vector3& to)
{
    const float dist = std::sqrt(distance_sq(from, to));
    const float drop_spacing = std::max(kWaypointRadius, kWaypointLinkRadiusEpsilon);
    const float normalized = (dist - kWaypointLinkRadiusEpsilon) / drop_spacing;
    const int drop_count = static_cast<int>(std::floor(normalized)) - 1;
    return std::max(0, drop_count);
}

bool has_player_drop_spacing(const rf::Vector3& pos)
{
    return closest_waypoint(pos, kWaypointRadius) <= 0;
}

bool bridge_waypoint_is_near_ground(const rf::Vector3& pos)
{
    rf::Vector3 p0 = pos;
    rf::Vector3 p1 = pos - rf::Vector3{0.0f, kBridgeWaypointMaxGroundDistance, 0.0f};
    rf::PCollisionOut collision{};
    collision.obj_handle = -1;
    return rf::collide_linesegment_world(p0, p1, 0, &collision);
}

std::vector<int> create_seed_bridge_waypoints(int from, int to, int intermediate_count)
{
    std::vector<int> bridge_indices{};
    if (intermediate_count <= 0) {
        return bridge_indices;
    }
    if (from <= 0 || to <= 0
        || from >= static_cast<int>(g_waypoints.size())
        || to >= static_cast<int>(g_waypoints.size())) {
        return bridge_indices;
    }
    const auto& from_node = g_waypoints[from];
    const auto& to_node = g_waypoints[to];
    if (!from_node.valid || !to_node.valid) {
        return bridge_indices;
    }

    const rf::Vector3 from_pos = from_node.pos;
    const rf::Vector3 to_pos = to_node.pos;
    std::vector<rf::Vector3> bridge_positions{};
    bridge_positions.reserve(intermediate_count);

    const float min_spacing_sq = kWaypointRadius * kWaypointRadius;
    for (int step = 1; step <= intermediate_count; ++step) {
        const float t = static_cast<float>(step) / static_cast<float>(intermediate_count + 1);
        const rf::Vector3 bridge_pos{
            from_pos.x + (to_pos.x - from_pos.x) * t,
            from_pos.y + (to_pos.y - from_pos.y) * t,
            from_pos.z + (to_pos.z - from_pos.z) * t,
        };

        if (!has_player_drop_spacing(bridge_pos)) {
            return {};
        }
        if (!bridge_waypoint_is_near_ground(bridge_pos)) {
            return {};
        }
        for (const auto& existing_pos : bridge_positions) {
            if (distance_sq(existing_pos, bridge_pos) <= min_spacing_sq) {
                return {};
            }
        }
        bridge_positions.push_back(bridge_pos);
    }

    bridge_indices.reserve(intermediate_count);
    int prev_index = from;

    for (const auto& bridge_pos : bridge_positions) {
        const int bridge_index = add_waypoint(
            bridge_pos, WaypointType::std_new, static_cast<int>(WaypointDroppedSubtype::normal),
            false, true, kWaypointLinkRadius, -1, nullptr, true);
        bridge_indices.push_back(bridge_index);

        link_waypoint_if_clear(prev_index, bridge_index);
        link_waypoint_if_clear(bridge_index, prev_index);
        prev_index = bridge_index;
    }

    link_waypoint_if_clear(prev_index, to);
    link_waypoint_if_clear(to, prev_index);
    return bridge_indices;
}

void auto_link_default_seeded_waypoints(std::vector<int>& seeded_indices, std::vector<int>& source_indices)
{
    if (seeded_indices.empty() || source_indices.empty()) {
        return;
    }

    std::vector<int> all_target_indices = seeded_indices;
    std::unordered_set<uint64_t> bridged_pairs{};
    int bridge_waypoints_created = 0;

    size_t source_cursor = 0;
    while (source_cursor < source_indices.size()) {
        const int source_index = source_indices[source_cursor++];
        if (source_index <= 0 || source_index >= static_cast<int>(g_waypoints.size())) {
            continue;
        }

        const auto& source_node = g_waypoints[source_index];
        if (!source_node.valid) {
            continue;
        }

        const rf::Vector3 source_pos = source_node.pos;
        const float auto_link_radius = waypoint_auto_link_detection_radius(source_node);
        const float auto_link_radius_sq = auto_link_radius * auto_link_radius;

        for (int target_index : all_target_indices) {
            if (target_index <= 0
                || target_index >= static_cast<int>(g_waypoints.size())
                || target_index == source_index) {
                continue;
            }

            const auto& target_node = g_waypoints[target_index];
            if (!target_node.valid) {
                continue;
            }

            const rf::Vector3 target_pos = target_node.pos;
            const float endpoint_dist_sq = distance_sq(source_pos, target_pos);
            if (endpoint_dist_sq <= auto_link_radius_sq) {
                link_waypoint_if_clear(source_index, target_index);
                link_waypoint_if_clear(target_index, source_index);
            }

            if (!g_waypoints_seed_bridges) {
                continue;
            }
            if (bridge_waypoints_created >= kMaxAutoSeedBridgeWaypoints) {
                continue;
            }
            if (waypoints_are_bidirectionally_linked(source_index, target_index)) {
                continue;
            }

            const uint64_t pair_key = make_waypoint_pair_key(source_index, target_index);
            if (!bridged_pairs.insert(pair_key).second) {
                continue;
            }

            if (!can_link_waypoints(source_pos, target_pos)) {
                continue;
            }

            const int intermediate_count = compute_bridge_intermediate_count(source_pos, target_pos);
            if (intermediate_count <= 0 || intermediate_count > kMaxAutoSeedBridgeDrops) {
                continue;
            }
            if (bridge_waypoints_created + intermediate_count > kMaxAutoSeedBridgeWaypoints) {
                continue;
            }

            auto bridge_indices = create_seed_bridge_waypoints(source_index, target_index, intermediate_count);
            if (bridge_indices.empty()) {
                continue;
            }

            bridge_waypoints_created += static_cast<int>(bridge_indices.size());
            for (int bridge_index : bridge_indices) {
                all_target_indices.push_back(bridge_index);
                source_indices.push_back(bridge_index);
            }
        }
    }

    if (bridge_waypoints_created >= kMaxAutoSeedBridgeWaypoints) {
        xlog::warn("Default grid auto-link bridge generation hit {} waypoint cap", kMaxAutoSeedBridgeWaypoints);
    }
}

int allocate_new_ladder_identifier()
{
    int max_identifier = -1;
    for (int i = 1; i < static_cast<int>(g_waypoints.size()); ++i) {
        const auto& node = g_waypoints[i];
        if (!node.valid || node.type != WaypointType::ladder || node.identifier < 0) {
            continue;
        }
        max_identifier = std::max(max_identifier, node.identifier);
    }
    return max_identifier + 1;
}

void assign_ladder_identifier(int new_index, int previous_index)
{
    if (new_index <= 0 || new_index >= static_cast<int>(g_waypoints.size())) {
        return;
    }

    auto& new_node = g_waypoints[new_index];
    if (!new_node.valid || new_node.type != WaypointType::ladder) {
        return;
    }

    int ladder_identifier = -1;
    std::vector<int> linked_ladders_without_identifier{};
    auto consider_ladder_neighbor = [&](int neighbor_index) {
        if (neighbor_index <= 0 || neighbor_index >= static_cast<int>(g_waypoints.size()) || neighbor_index == new_index) {
            return;
        }

        auto& neighbor = g_waypoints[neighbor_index];
        if (!neighbor.valid || neighbor.type != WaypointType::ladder) {
            return;
        }

        if (neighbor.identifier >= 0) {
            if (ladder_identifier < 0) {
                ladder_identifier = neighbor.identifier;
            }
            return;
        }

        if (std::find(linked_ladders_without_identifier.begin(), linked_ladders_without_identifier.end(), neighbor_index)
            == linked_ladders_without_identifier.end()) {
            linked_ladders_without_identifier.push_back(neighbor_index);
        }
    };

    // Prefer inheriting from the immediate predecessor when available.
    consider_ladder_neighbor(previous_index);

    for (int link_idx = 0; link_idx < new_node.num_links; ++link_idx) {
        consider_ladder_neighbor(new_node.links[link_idx]);
    }

    for (int i = 1; i < static_cast<int>(g_waypoints.size()); ++i) {
        if (i == new_index) {
            continue;
        }
        const auto& candidate = g_waypoints[i];
        if (!candidate.valid || candidate.type != WaypointType::ladder) {
            continue;
        }
        for (int link_idx = 0; link_idx < candidate.num_links; ++link_idx) {
            if (candidate.links[link_idx] == new_index) {
                consider_ladder_neighbor(i);
                break;
            }
        }
    }

    if (ladder_identifier < 0) {
        ladder_identifier = allocate_new_ladder_identifier();
    }

    new_node.identifier = ladder_identifier;
    for (int ladder_index : linked_ladders_without_identifier) {
        g_waypoints[ladder_index].identifier = ladder_identifier;
    }
}

void link_waypoints_bidirectional(int a, int b)
{
    link_waypoint(a, b);
    link_waypoint(b, a);
}

bool can_link_waypoints(const rf::Vector3& a, const rf::Vector3& b)
{
    rf::GCollisionOutput collision{};
    rf::Vector3 p0 = a;
    rf::Vector3 p1 = b;
    return !rf::collide_linesegment_level_solid(p0, p1, 0, &collision);
}

bool can_link_waypoint_indices(int from, int to)
{
    if (from <= 0 || to <= 0
        || from == to
        || from >= static_cast<int>(g_waypoints.size())
        || to >= static_cast<int>(g_waypoints.size())) {
        return false;
    }

    const auto& from_node = g_waypoints[from];
    const auto& to_node = g_waypoints[to];
    if (!from_node.valid || !to_node.valid) {
        return false;
    }

    return can_link_waypoints(from_node.pos, to_node.pos);
}

bool link_waypoint_if_clear(int from, int to)
{
    if (!can_link_waypoint_indices(from, to)) {
        return false;
    }

    link_waypoint(from, to);
    return true;
}

void on_geomod_crater_created(const rf::Vector3& crater_pos, float crater_radius)
{
    if (!(rf::level.flags & rf::LEVEL_LOADED) || g_waypoints.empty()) {
        return;
    }

    remove_realized_waypoint_targets(crater_pos, crater_radius);

    const int crater_index = add_waypoint(
        crater_pos, WaypointType::crater, 0, false, true, kWaypointLinkRadius, -1, nullptr, true);

    const float link_radius_sq = kWaypointLinkRadius * kWaypointLinkRadius;
    for (int i = 1; i < crater_index; ++i) {
        const auto& node = g_waypoints[i];
        if (!node.valid) {
            continue;
        }
        if (distance_sq(crater_pos, node.pos) > link_radius_sq) {
            continue;
        }

        link_waypoint_if_clear(crater_index, i);
        link_waypoint_if_clear(i, crater_index);
    }
}

void sanitize_waypoint_links_against_geometry()
{
    int removed_links = 0;
    const int waypoint_total = static_cast<int>(g_waypoints.size());
    for (int index = 1; index < waypoint_total; ++index) {
        auto& node = g_waypoints[index];
        if (!node.valid) {
            node.num_links = 0;
            continue;
        }

        int write_index = 0;
        for (int read_index = 0; read_index < node.num_links; ++read_index) {
            const int link = node.links[read_index];
            if (!can_link_waypoint_indices(index, link)) {
                ++removed_links;
                continue;
            }

            bool duplicate = false;
            for (int i = 0; i < write_index; ++i) {
                if (node.links[i] == link) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                ++removed_links;
                continue;
            }

            node.links[write_index++] = link;
        }
        node.num_links = write_index;
    }

    if (removed_links > 0) {
        xlog::info("Pruned {} blocked or invalid waypoint links", removed_links);
    }
}

void link_waypoint_to_nearest(int index, bool bidirectional)
{
    if (index <= 0 || index >= static_cast<int>(g_waypoints.size())) {
        return;
    }
    const auto& node = g_waypoints[index];
    const float link_radius = sanitize_waypoint_link_radius(node.link_radius);
    int nearest = find_nearest_waypoint(node.pos, link_radius, index);
    if (nearest > 0) {
        if (bidirectional) {
            link_waypoint_if_clear(index, nearest);
            link_waypoint_if_clear(nearest, index);
        }
        else {
            link_waypoint_if_clear(index, nearest);
        }
    }
}

int add_waypoint(const rf::Vector3& pos, WaypointType type, int subtype, bool link_to_nearest, bool bidirectional_link,
                 float link_radius, int identifier, const rf::Object* source_object, bool auto_assign_zones)
{
    WaypointNode node{};
    node.pos = pos;
    node.type = type;
    node.subtype = subtype;
    node.identifier = identifier;
    node.link_radius = sanitize_waypoint_link_radius(link_radius);
    if (auto_assign_zones) {
        node.zones = collect_waypoint_zone_refs(pos, source_object);
    }
    g_waypoints.push_back(node);
    invalidate_cache();
    int index = static_cast<int>(g_waypoints.size()) - 1;
    if (link_to_nearest) {
        link_waypoint_to_nearest(index, bidirectional_link);
    }
    return index;
}

std::vector<int> gather_indices()
{
    std::vector<int> indices;
    indices.reserve(g_waypoints.size());
    for (int i = 1; i < static_cast<int>(g_waypoints.size()); ++i) {
        if (g_waypoints[i].valid) {
            indices.push_back(i);
        }
    }
    return indices;
}

WpCacheNode* build_cache(std::vector<int>& indices, int depth)
{
    if (indices.empty()) {
        return nullptr;
    }
    int axis = depth % 3;
    std::sort(indices.begin(), indices.end(), [axis](int a, int b) {
        const auto& pa = g_waypoints[a].pos;
        const auto& pb = g_waypoints[b].pos;
        if (axis == 0)
            return pa.x < pb.x;
        if (axis == 1)
            return pa.y < pb.y;
        return pa.z < pb.z;
    });
    int mid = static_cast<int>(indices.size()) / 2;
    int index = indices[mid];
    auto node_idx = static_cast<int>(g_cache_nodes.size());
    g_cache_nodes.push_back({});
    auto& node = g_cache_nodes.back();
    node.index = index;
    node.axis = axis;
    const auto& pos = g_waypoints[index].pos;
    node.min = pos;
    node.max = pos;
    std::vector<int> left(indices.begin(), indices.begin() + mid);
    std::vector<int> right(indices.begin() + mid + 1, indices.end());
    node.left = build_cache(left, depth + 1);
    node.right = build_cache(right, depth + 1);
    if (node.left) {
        update_bounds(node, node.left->min);
        update_bounds(node, node.left->max);
    }
    if (node.right) {
        update_bounds(node, node.right->min);
        update_bounds(node, node.right->max);
    }
    return &g_cache_nodes[node_idx];
}

void ensure_cache()
{
    if (!g_cache_dirty) {
        return;
    }
    g_cache_nodes.clear();
    std::vector<int> indices = gather_indices();
    g_cache_nodes.reserve(indices.size());
    g_cache_root = build_cache(indices, 0);
    g_cache_dirty = false;
}

float bbox_distance_sq(const rf::Vector3& p, const rf::Vector3& min, const rf::Vector3& max)
{
    float dx = 0.0f;
    if (p.x < min.x)
        dx = min.x - p.x;
    else if (p.x > max.x)
        dx = p.x - max.x;
    float dy = 0.0f;
    if (p.y < min.y)
        dy = min.y - p.y;
    else if (p.y > max.y)
        dy = p.y - max.y;
    float dz = 0.0f;
    if (p.z < min.z)
        dz = min.z - p.z;
    else if (p.z > max.z)
        dz = p.z - max.z;
    return dx * dx + dy * dy + dz * dz;
}

void closest_recursive(WpCacheNode* node, const rf::Vector3& pos, float radius_sq, int& best_index, float& best_dist_sq)
{
    if (!node) {
        return;
    }
    if (bbox_distance_sq(pos, node->min, node->max) > best_dist_sq) {
        return;
    }
    const auto& wp = g_waypoints[node->index];
    if (wp.valid) {
        float dist_sq = distance_sq(pos, wp.pos);
        if (dist_sq <= radius_sq && dist_sq < best_dist_sq) {
            best_dist_sq = dist_sq;
            best_index = node->index;
        }
    }
    WpCacheNode* first = node->left;
    WpCacheNode* second = node->right;
    float delta = 0.0f;
    if (node->axis == 0)
        delta = pos.x - wp.pos.x;
    else if (node->axis == 1)
        delta = pos.y - wp.pos.y;
    else
        delta = pos.z - wp.pos.z;
    if (delta > 0.0f) {
        std::swap(first, second);
    }
    closest_recursive(first, pos, radius_sq, best_index, best_dist_sq);
    if (delta * delta < best_dist_sq) {
        closest_recursive(second, pos, radius_sq, best_index, best_dist_sq);
    }
}

int closest_waypoint(const rf::Vector3& pos, float radius)
{
    if (g_waypoints.size() <= 1) {
        return 0;
    }
    ensure_cache();
    float radius_sq = radius * radius;
    int best_index = 0;
    float best_dist_sq = radius_sq;
    closest_recursive(g_cache_root, pos, radius_sq, best_index, best_dist_sq);
    return best_index;
}

std::optional<std::string> get_waypoint_dir()
{
    auto base_path = std::string{rf::root_path};
    if (base_path.empty()) {
        return std::nullopt;
    }
    auto user_maps_path = base_path + "\\user_maps";
    auto waypoints_path = user_maps_path + "\\waypoints";
    if (!CreateDirectoryA(user_maps_path.c_str(), nullptr)) {
        if (GetLastError() != ERROR_ALREADY_EXISTS) {
            xlog::error("Failed to create user_maps directory {}", GetLastError());
            return std::nullopt;
        }
    }
    if (!CreateDirectoryA(waypoints_path.c_str(), nullptr)) {
        if (GetLastError() != ERROR_ALREADY_EXISTS) {
            xlog::error("Failed to create waypoint directory {}", GetLastError());
            return std::nullopt;
        }
    }
    return waypoints_path;
}

std::filesystem::path get_waypoint_filename()
{
    std::filesystem::path map_name = std::string{get_filename_without_ext(rf::level.filename.c_str())};
    auto waypoint_dir = get_waypoint_dir();
    if (!waypoint_dir) {
        return map_name.string() + ".awp";
    }
    return std::filesystem::path(waypoint_dir.value()) / (map_name.string() + ".awp");
}

void seed_waypoints_from_objects()
{
    if (g_waypoints.size() > 1) {
        return;
    }

    seed_waypoint_zones_from_control_points();
    seed_waypoint_zones_from_trigger_damage_events();
    seed_waypoint_zones_from_damaging_liquid_rooms();
    std::vector<int> seeded_indices{};
    std::vector<int> auto_link_source_indices{};

    rf::Object* obj = rf::object_list.next_obj;
    while (obj != &rf::object_list) {
        if (obj->type == rf::OT_ITEM) {
            auto* item = static_cast<rf::Item*>(obj);
            const std::string_view item_name = item->name.c_str();

            if (auto ctf_subtype = get_default_grid_ctf_flag_subtype(item_name); ctf_subtype) {
                const int waypoint_index = add_waypoint(
                    obj->pos, WaypointType::ctf_flag, static_cast<int>(ctf_subtype.value()), false, true,
                    kWaypointLinkRadius, -1, obj);
                seeded_indices.push_back(waypoint_index);
            }
            else if (!should_skip_default_item_waypoint(item_name)) {
                const int waypoint_index = add_waypoint(
                    obj->pos, WaypointType::item, item->info_index, false, true, kWaypointLinkRadius, obj->uid, obj);
                seeded_indices.push_back(waypoint_index);
                auto_link_source_indices.push_back(waypoint_index);
            }
        }
        obj = obj->next_obj;
    }
    for (const auto& rp : get_alpine_respawn_points()) {
        if (rp.enabled) {
            WaypointRespawnSubtype subtype = WaypointRespawnSubtype::neutral;
            if (rp.red_team && rp.blue_team) {
                subtype = WaypointRespawnSubtype::all_teams;
            }
            else if (rp.red_team) {
                subtype = WaypointRespawnSubtype::red_team;
            }
            else if (rp.blue_team) {
                subtype = WaypointRespawnSubtype::blue_team;
            }
            const int waypoint_index = add_waypoint(
                rp.position, WaypointType::respawn, static_cast<int>(subtype), false, true, kWaypointLinkRadius, rp.uid);
            seeded_indices.push_back(waypoint_index);
            auto_link_source_indices.push_back(waypoint_index);
        }
    }
    for (int i = 0; i < rf::level.pushers.size(); ++i) {
        auto* push_region = rf::level.pushers[i];
        if (!push_region) {
            continue;
        }
        if ((push_region->flags_and_turbulence & rf::PushRegionFlags::PRF_JUMP_PAD) == 0) {
            continue;
        }
        const float link_radius = waypoint_link_radius_from_push_region(*push_region) + 1.0f;
        const int waypoint_index = add_waypoint(
            push_region->pos, WaypointType::jump_pad, static_cast<int>(WaypointJumpPadSubtype::default_pad), false,
            true, link_radius, push_region->uid);
        seeded_indices.push_back(waypoint_index);
        auto_link_source_indices.push_back(waypoint_index);
    }

    seed_waypoints_from_teleport_events(&seeded_indices, &auto_link_source_indices);
    auto_link_default_seeded_waypoints(seeded_indices, auto_link_source_indices);
}

void clear_waypoints()
{
    g_waypoints.clear();
    g_waypoints.push_back({});
    g_waypoint_zones.clear();
    g_waypoint_targets.clear();
    g_next_waypoint_target_uid = 1;
    invalidate_cache();
}

void reset_waypoints_to_default_grid()
{
    clear_waypoints();
    seed_waypoints_from_objects();
    g_has_loaded_wpt = false;
    g_waypoint_revision = 0;
    g_last_drop_waypoint_by_entity.clear();
    g_last_lift_uid_by_entity.clear();
}

bool save_waypoints()
{
    if (g_waypoints.size() <= 1) {
        return false;
    }
    refresh_all_waypoint_zone_refs();
    auto filename = get_waypoint_filename();
    auto now = std::time(nullptr);
    std::tm time_info{};
#if defined(_WIN32)
    localtime_s(&time_info, &now);
#else
    time_info = *std::localtime(&now);
#endif
    char time_buf[32];
    std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &time_info);
    const char* author =
        (rf::local_player && rf::local_player->name.c_str()) ? rf::local_player->name.c_str() : "unknown";
    const int revision = g_waypoint_revision + 1;
    const bool save_compressed = g_waypoints_compress;
    toml::table header{
        {"revision", revision},
        {"awp_ver", kWptVersion},
        {"compressed", save_compressed},
        {"level", std::string{rf::level.filename.c_str()}},
        {"level_checksum", static_cast<int64_t>(rf::level.checksum)},
        {"saved_at", std::string{time_buf}},
        {"author", std::string{author}},
    };
    toml::array nodes;
    for (int i = 1; i < static_cast<int>(g_waypoints.size()); ++i) {
        const auto& node = g_waypoints[i];
        toml::array pos;
        if (save_compressed) {
            auto compressed = compress_waypoint_pos(node.pos);
            pos = toml::array{compressed.x, compressed.y, compressed.z};
        }
        else {
            pos = toml::array{node.pos.x, node.pos.y, node.pos.z};
        }
        toml::array links;
        for (int link = 0; link < node.num_links; ++link) {
            links.push_back(node.links[link]);
        }
        int saved_type = waypoint_type_to_save_value(node.type);
        toml::table entry{
            {"p", pos},
            {"t", saved_type},
            {"s", node.subtype},
            {"l", links},
        };
        if (std::fabs(node.link_radius - kWaypointLinkRadius) > kWaypointLinkRadiusEpsilon) {
            if (save_compressed) {
                if (auto compressed_radius = compress_waypoint_radius(node.link_radius); compressed_radius) {
                    entry.insert("r", static_cast<int64_t>(compressed_radius.value()));
                }
                else {
                    entry.insert("r", node.link_radius);
                }
            }
            else {
                entry.insert("r", node.link_radius);
            }
        }
        if (node.identifier != -1) {
            entry.insert("i", node.identifier);
        }
        if (!node.zones.empty()) {
            toml::array zone_refs{};
            for (int zone_index : node.zones) {
                if (zone_index >= 0 && zone_index < static_cast<int>(g_waypoint_zones.size())) {
                    zone_refs.push_back(zone_index);
                }
            }
            if (!zone_refs.empty()) {
                entry.insert("z", zone_refs);
            }
        }
        nodes.push_back(entry);
    }
    toml::table root{
        {"header", header},
        {"w", nodes},
    };
    if (!g_waypoint_zones.empty()) {
        toml::array zones{};
        for (const auto& zone : g_waypoint_zones) {
            toml::table zone_entry{
                {"t", static_cast<int>(zone.type)},
            };

            switch (resolve_waypoint_zone_source(zone)) {
                case WaypointZoneSource::trigger_uid:
                    zone_entry.insert("t_uid", zone.trigger_uid);
                    break;
                case WaypointZoneSource::room_uid:
                    zone_entry.insert("r_uid", zone.room_uid);
                    break;
                case WaypointZoneSource::box_extents:
                    zone_entry.insert("mn", toml::array{zone.box_min.x, zone.box_min.y, zone.box_min.z});
                    zone_entry.insert("mx", toml::array{zone.box_max.x, zone.box_max.y, zone.box_max.z});
                    break;
                default:
                    break;
            }
            if (zone.identifier != -1) {
                zone_entry.insert("i", zone.identifier);
            }

            zones.push_back(zone_entry);
        }
        root.insert("z", zones);
    }
    if (!g_waypoint_targets.empty()) {
        toml::array targets{};
        for (const auto& target : g_waypoint_targets) {
            toml::array pos{};
            if (save_compressed) {
                auto compressed = compress_waypoint_pos(target.pos);
                pos = toml::array{compressed.x, compressed.y, compressed.z};
            }
            else {
                pos = toml::array{target.pos.x, target.pos.y, target.pos.z};
            }

            toml::array waypoint_uids{};
            for (int waypoint_uid : target.waypoint_uids) {
                if (waypoint_uid <= 0 || waypoint_uid >= static_cast<int>(g_waypoints.size())) {
                    continue;
                }
                if (!g_waypoints[waypoint_uid].valid) {
                    continue;
                }
                waypoint_uids.push_back(waypoint_uid);
            }

            toml::table target_entry{
                {"p", pos},
                {"t", static_cast<int>(target.type)},
                {"w", waypoint_uids},
            };
            targets.push_back(target_entry);
        }
        root.insert("t", targets);
    }
    std::ofstream file(filename);
    if (!file.is_open()) {
        xlog::error("Failed to open waypoint file for write {}", filename.string());
        return false;
    }
    file << root;
    g_waypoint_revision = revision;
    return true;
}

bool load_waypoints()
{
    clear_waypoints();
    auto filename = get_waypoint_filename();
    toml::table root;
    try {
        root = toml::parse_file(filename.string());
    }
    catch (const toml::parse_error& err) {
        xlog::error("Failed to parse waypoint file {}: {}", filename.string(), err.description());
        return false;
    }
    g_waypoint_revision = 0;
    bool header_compressed = false;
    std::optional<uint32_t> header_checksum;
    if (const auto* header = root["header"].as_table()) {
        if (const auto* revision_node = header->get("revision"); revision_node && revision_node->is_number()) {
            g_waypoint_revision = static_cast<int>(revision_node->value_or(g_waypoint_revision));
        }
        if (const auto* compressed_node = header->get("compressed"); compressed_node && compressed_node->is_boolean()) {
            header_compressed = compressed_node->value_or(false);
        }
        const auto* checksum_node = header->get("level_checksum");
        if ((!checksum_node || !checksum_node->is_number()) && header->get("checksum")) {
            checksum_node = header->get("checksum");
        }
        if (checksum_node && checksum_node->is_number()) {
            header_checksum = static_cast<uint32_t>(checksum_node->value_or(0));
        }
    }
    if (header_checksum && *header_checksum != rf::level.checksum) {
        xlog::warn("Waypoint checksum mismatch for {}: file {}, level {}", filename.string(), *header_checksum,
                   rf::level.checksum);
    }
    auto load_zone_entries = [](const toml::array& zone_entries) {
        for (const auto& zone_entry_node : zone_entries) {
            const auto* zone_tbl = zone_entry_node.as_table();
            if (!zone_tbl) {
                continue;
            }

            WaypointZoneDefinition zone{};
            if (parse_waypoint_zone_definition(*zone_tbl, zone)) {
                g_waypoint_zones.push_back(zone);
            }
        }
    };

    if (const auto* zone_entries = root["z"].as_array()) {
        load_zone_entries(*zone_entries);
    }
    const auto* target_entries = root["t"].as_array();
    if (!target_entries) {
        target_entries = root["targets"].as_array();
    }
    const auto* nodes = root["w"].as_array();
    if (!nodes) {
        nodes = root["waypoints"].as_array();
    }
    if (!nodes) {
        return false;
    }
    for (const auto& node_entry : *nodes) {
        const auto* node_tbl = node_entry.as_table();
        if (!node_tbl) {
            continue;
        }
        const auto* pos = node_tbl->get_as<toml::array>("p");
        if (!pos) {
            pos = node_tbl->get_as<toml::array>("pos");
        }
        if (!pos) {
            continue;
        }
        auto wp_pos_opt = parse_waypoint_pos(*pos, header_compressed);
        if (!wp_pos_opt) {
            continue;
        }
        rf::Vector3 wp_pos = wp_pos_opt.value();
        int raw_type = static_cast<int>(WaypointType::std);
        int subtype = 0;
        float link_radius = kWaypointLinkRadius;
        int identifier = -1;
        if (const auto* type_node = node_tbl->get("t")) {
            if (type_node->is_number()) {
                raw_type = static_cast<int>(type_node->value_or(raw_type));
            }
        }
        else if (const auto* type_node = node_tbl->get("type")) {
            if (type_node->is_number()) {
                raw_type = static_cast<int>(type_node->value_or(raw_type));
            }
        }
        if (const auto* subtype_node = node_tbl->get("s")) {
            if (subtype_node->is_number()) {
                subtype = static_cast<int>(subtype_node->value_or(subtype));
            }
        }
        else if (const auto* subtype_node = node_tbl->get("subtype")) {
            if (subtype_node->is_number()) {
                subtype = static_cast<int>(subtype_node->value_or(subtype));
            }
        }
        if (const auto* radius_node = node_tbl->get("r")) {
            if (radius_node->is_number()) {
                if (header_compressed && radius_node->is_integer()) {
                    const int packed_radius = static_cast<int>(radius_node->value_or(0));
                    if (packed_radius >= std::numeric_limits<int16_t>::min()
                        && packed_radius <= std::numeric_limits<int16_t>::max()) {
                        link_radius = decompress_waypoint_radius(static_cast<int16_t>(packed_radius));
                    }
                }
                else {
                    link_radius = static_cast<float>(radius_node->value_or<double>(link_radius));
                }
            }
        }
        else if (const auto* radius_node = node_tbl->get("radius")) {
            if (radius_node->is_number()) {
                link_radius = static_cast<float>(radius_node->value_or<double>(link_radius));
            }
        }
        if (const auto* identifier_node = node_tbl->get("i")) {
            if (identifier_node->is_number()) {
                identifier = static_cast<int>(identifier_node->value_or(identifier));
            }
        }
        else if (const auto* identifier_node = node_tbl->get("id")) {
            if (identifier_node->is_number()) {
                identifier = static_cast<int>(identifier_node->value_or(identifier));
            }
        }
        else if (const auto* identifier_node = node_tbl->get("identifier")) {
            if (identifier_node->is_number()) {
                identifier = static_cast<int>(identifier_node->value_or(identifier));
            }
        }
        std::vector<int> zone_refs{};
        bool has_zone_refs = false;
        if (const auto* zones_node = node_tbl->get_as<toml::array>("z")) {
            has_zone_refs = true;
            for (const auto& zone_ref_node : *zones_node) {
                if (zone_ref_node.is_number()) {
                    zone_refs.push_back(static_cast<int>(zone_ref_node.value_or(0)));
                }
            }
        }
        WaypointType type = waypoint_type_from_int(raw_type);
        int index = add_waypoint(wp_pos, type, subtype, false, true, link_radius, identifier, nullptr, false);
        auto& node = g_waypoints[index];
        const auto* links = node_tbl->get_as<toml::array>("l");
        if (!links) {
            links = node_tbl->get_as<toml::array>("links");
        }
        if (links) {
            int link_count = std::min(static_cast<int>(links->size()), kMaxWaypointLinks);
            node.num_links = link_count;
            for (int link = 0; link < link_count; ++link) {
                node.links[link] = static_cast<int>(links->at(link).value_or(0));
            }
        }
        if (has_zone_refs) {
            normalize_waypoint_zone_refs(zone_refs);
            node.zones = std::move(zone_refs);
        }
        else if (!g_waypoint_zones.empty()) {
            node.zones = collect_waypoint_zone_refs(node.pos, nullptr);
        }
    }
    if (target_entries) {
        for (const auto& target_entry_node : *target_entries) {
            const auto* target_tbl = target_entry_node.as_table();
            if (!target_tbl) {
                continue;
            }

            const auto* pos_node = target_tbl->get_as<toml::array>("p");
            if (!pos_node) {
                pos_node = target_tbl->get_as<toml::array>("pos");
            }
            if (!pos_node) {
                continue;
            }

            auto target_pos_opt = parse_waypoint_pos(*pos_node, header_compressed);
            if (!target_pos_opt) {
                continue;
            }

            int raw_type = static_cast<int>(WaypointTargetType::explosion);
            if (const auto* type_node = target_tbl->get("t"); type_node && type_node->is_number()) {
                raw_type = static_cast<int>(type_node->value_or(raw_type));
            }
            else if (const auto* type_node = target_tbl->get("type"); type_node && type_node->is_number()) {
                raw_type = static_cast<int>(type_node->value_or(raw_type));
            }

            std::vector<int> waypoint_uids{};
            bool has_waypoint_uids = false;
            if (const auto* waypoint_uids_node = target_tbl->get_as<toml::array>("w")) {
                has_waypoint_uids = true;
                for (const auto& waypoint_uid_node : *waypoint_uids_node) {
                    if (waypoint_uid_node.is_number()) {
                        waypoint_uids.push_back(static_cast<int>(waypoint_uid_node.value_or(0)));
                    }
                }
            }
            else if (const auto* waypoint_uids_node = target_tbl->get_as<toml::array>("waypoint_uids")) {
                has_waypoint_uids = true;
                for (const auto& waypoint_uid_node : *waypoint_uids_node) {
                    if (waypoint_uid_node.is_number()) {
                        waypoint_uids.push_back(static_cast<int>(waypoint_uid_node.value_or(0)));
                    }
                }
            }
            else if (const auto* waypoint_uids_node = target_tbl->get_as<toml::array>("waypoints")) {
                has_waypoint_uids = true;
                for (const auto& waypoint_uid_node : *waypoint_uids_node) {
                    if (waypoint_uid_node.is_number()) {
                        waypoint_uids.push_back(static_cast<int>(waypoint_uid_node.value_or(0)));
                    }
                }
            }

            WaypointTargetDefinition target{};
            target.uid = resolve_waypoint_target_uid();
            target.pos = target_pos_opt.value();
            target.type = waypoint_target_type_from_int(raw_type);
            if (has_waypoint_uids) {
                normalize_target_waypoint_uids(waypoint_uids);
                target.waypoint_uids = std::move(waypoint_uids);
            }
            else {
                target.waypoint_uids = collect_target_waypoint_uids(target.pos);
            }
            g_waypoint_targets.push_back(std::move(target));
        }
    }
    sanitize_waypoint_links_against_geometry();
    g_has_loaded_wpt = true;
    return true;
}

void mark_invalid_waypoints()
{
    for (int i = 1; i < static_cast<int>(g_waypoints.size()); ++i) {
        auto& node = g_waypoints[i];
        if (!node.valid) {
            continue;
        }
        rf::Vector3 p0 = node.pos + rf::Vector3{0.0f, 0.0f, 2.0f};
        rf::Vector3 p1 = node.pos - rf::Vector3{0.0f, 0.0f, 1.0f};
        rf::LevelCollisionOut col{};
        col.obj_handle = -1;
        col.face = nullptr;
        bool hit = rf::collide_linesegment_level_for_multi(p0, p1, nullptr, nullptr, &col, 0.0f, false, 1.0f);
        if (!hit) {
            continue;
        }
        if (col.face) {
            auto* face = static_cast<rf::GFace*>(col.face);
            if (face->attributes.is_liquid()) {
                node.valid = false;
            }
        }
    }
}

void remap_waypoints()
{
    std::vector<int> remap(g_waypoints.size(), 0);
    std::vector<WaypointNode> new_nodes;
    new_nodes.reserve(g_waypoints.size());
    new_nodes.push_back(g_waypoints[0]);
    int next_index = 1;
    for (int i = 1; i < static_cast<int>(g_waypoints.size()); ++i) {
        if (g_waypoints[i].valid) {
            remap[i] = next_index++;
            new_nodes.push_back(g_waypoints[i]);
        }
    }
    for (int i = 1; i < static_cast<int>(new_nodes.size()); ++i) {
        auto& node = new_nodes[i];
        int write_idx = 0;
        for (int j = 0; j < node.num_links; ++j) {
            int link = node.links[j];
            if (link <= 0 || link >= static_cast<int>(remap.size())) {
                continue;
            }
            int remapped = remap[link];
            if (remapped > 0) {
                node.links[write_idx++] = remapped;
            }
        }
        node.num_links = write_idx;
    }
    g_waypoints = std::move(new_nodes);
    for (auto& target : g_waypoint_targets) {
        std::vector<int> remapped_waypoint_uids{};
        remapped_waypoint_uids.reserve(target.waypoint_uids.size());
        for (int waypoint_uid : target.waypoint_uids) {
            if (waypoint_uid <= 0 || waypoint_uid >= static_cast<int>(remap.size())) {
                continue;
            }
            const int remapped_waypoint_uid = remap[waypoint_uid];
            if (remapped_waypoint_uid > 0) {
                remapped_waypoint_uids.push_back(remapped_waypoint_uid);
            }
        }
        normalize_target_waypoint_uids(remapped_waypoint_uids);
        target.waypoint_uids = std::move(remapped_waypoint_uids);
    }
    invalidate_cache();
}

void clean_waypoints()
{
    mark_invalid_waypoints();
    remap_waypoints();
    sanitize_waypoint_links_against_geometry();
}

bool should_navigate()
{
    return g_drop_waypoints || g_has_loaded_wpt;
}

bool should_drop()
{
    return g_drop_waypoints || !g_has_loaded_wpt;
}

bool should_skip_local_bot_waypoint_drop()
{
    if (!rf::local_player) {
        return false;
    }

    return rf::local_player->is_bot || g_alpine_game_config.client_bot_mode;
}

void prune_drop_trackers(const std::unordered_set<int>& active_entity_handles)
{
    for (auto it = g_last_drop_waypoint_by_entity.begin();
         it != g_last_drop_waypoint_by_entity.end();) {
        if (active_entity_handles.find(it->first) == active_entity_handles.end()) {
            it = g_last_drop_waypoint_by_entity.erase(it);
        }
        else {
            ++it;
        }
    }
    for (auto it = g_last_lift_uid_by_entity.begin();
         it != g_last_lift_uid_by_entity.end();) {
        if (active_entity_handles.find(it->first) == active_entity_handles.end()) {
            it = g_last_lift_uid_by_entity.erase(it);
        }
        else {
            ++it;
        }
    }
}

void navigate_entity(int entity_handle, rf::Entity& entity, bool allow_drop)
{
    if (entity_handle < 0) {
        return;
    }

    rf::Vector3 pos = get_entity_feet_pos(entity);
    const int closest_standard = closest_waypoint(pos, kWaypointRadius);
    const int closest_jump_pad = find_jump_pad_waypoint_in_radius(pos);
    const int closest_tele_entrance = find_tele_entrance_waypoint_in_radius(pos);
    int closest_special = 0;
    if (closest_jump_pad > 0) {
        closest_special = closest_jump_pad;
    }
    if (closest_tele_entrance > 0) {
        if (closest_special <= 0
            || distance_sq(pos, g_waypoints[closest_tele_entrance].pos) < distance_sq(pos, g_waypoints[closest_special].pos)) {
            closest_special = closest_tele_entrance;
        }
    }
    const int closest = closest_special > 0 ? closest_special : closest_standard;
    bool should_drop_new = allow_drop && should_drop();

    if (closest_standard > 0) {
        const float dist_sq = distance_sq(pos, g_waypoints[closest_standard].pos);
        if (dist_sq <= kWaypointRadius * kWaypointRadius) {
            should_drop_new = false;
        }
    }
    if (closest_special > 0) {
        should_drop_new = false;
    }

    int& last_drop_waypoint = g_last_drop_waypoint_by_entity[entity_handle];
    if (should_drop_new) {
        const bool grounded = is_player_grounded(entity);
        const bool falling = rf::entity_is_falling(&entity);
        const bool swimming = rf::entity_is_swimming(&entity);
        const bool crouching = rf::entity_is_crouching(&entity);
        const bool climbing = rf::entity_is_climbing(&entity);
        auto lift_it = g_last_lift_uid_by_entity.find(entity_handle);
        if (lift_it == g_last_lift_uid_by_entity.end()) {
            lift_it = g_last_lift_uid_by_entity.emplace(entity_handle, -1).first;
        }
        int& last_lift_uid = lift_it->second;
        int subtype = static_cast<int>(WaypointDroppedSubtype::normal);
        if (falling) {
            subtype = static_cast<int>(WaypointDroppedSubtype::falling);
        }
        else if (swimming) {
            subtype = static_cast<int>(WaypointDroppedSubtype::swimming);
        }
        else if (crouching) {
            subtype = static_cast<int>(WaypointDroppedSubtype::crouch_needed);
        }
        WaypointType drop_type = WaypointType::std_new;
        int identifier = -1;
        if (climbing) {
            drop_type = WaypointType::ladder;
            last_lift_uid = -1;
        }
        else {
            const int lift_uid_below = find_lift_uid_below_waypoint(pos);
            if (lift_uid_below >= 0) {
                drop_type = (last_lift_uid == lift_uid_below) ? WaypointType::lift_body : WaypointType::lift_entrance;
                identifier = lift_uid_below;
                last_lift_uid = lift_uid_below;
            }
            else if (last_lift_uid >= 0) {
                drop_type = WaypointType::lift_exit;
                identifier = last_lift_uid;
                last_lift_uid = -1;
            }
        }
        const int new_index = add_waypoint(
            pos, drop_type, subtype, grounded, grounded, kWaypointLinkRadius, identifier, &entity);
        if (last_drop_waypoint > 0) {
            link_waypoint_if_clear(last_drop_waypoint, new_index);
            if (grounded) {
                link_waypoint_if_clear(new_index, last_drop_waypoint);
            }
        }
        if (drop_type == WaypointType::ladder) {
            assign_ladder_identifier(new_index, last_drop_waypoint);
        }
        last_drop_waypoint = new_index;
        return;
    }

    if (closest > 0) {
        if (allow_drop && last_drop_waypoint > 0 && last_drop_waypoint != closest) {
            const auto& target_waypoint = g_waypoints[closest];
            if (target_waypoint.type == WaypointType::jump_pad || target_waypoint.type == WaypointType::tele_entrance) {
                // Entering a jump pad or teleporter entrance should create only an ingress link.
                link_waypoint_if_clear(last_drop_waypoint, closest);
            }
            else {
                const bool grounded = is_player_grounded(entity);
                link_waypoint_if_clear(last_drop_waypoint, closest);
                if (grounded) {
                    link_waypoint_if_clear(closest, last_drop_waypoint);
                }
            }
        }
        last_drop_waypoint = closest;
    }
}

void navigate()
{
    if (!rf::local_player) {
        return;
    }
    if (!should_navigate()) {
        return;
    }

    std::unordered_set<int> active_entity_handles{};

    if (!should_skip_local_bot_waypoint_drop()) {
        auto* entity = rf::entity_from_handle(rf::local_player->entity_handle);
        if (!entity) {
            return;
        }
        active_entity_handles.insert(rf::local_player->entity_handle);
        navigate_entity(rf::local_player->entity_handle, *entity, true);
        prune_drop_trackers(active_entity_handles);
        return;
    }

    for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
        if (&player == rf::local_player
            || player.entity_handle < 0
            || player.is_bot
            || player.is_browser
            || player.is_spectator
            || player.is_spawn_disabled
            || rf::player_is_dead(&player)
            || rf::player_is_dying(&player)) {
            continue;
        }

        auto* entity = rf::entity_from_handle(player.entity_handle);
        if (!entity) {
            continue;
        }

        active_entity_handles.insert(player.entity_handle);
        navigate_entity(player.entity_handle, *entity, true);
    }

    prune_drop_trackers(active_entity_handles);
}

rf::Color debug_waypoint_color(WaypointType type)
{
    switch (type) {
        case WaypointType::std:
            return {255, 255, 255, 150};
        case WaypointType::std_new:
            return {255, 255, 255, 75};
        case WaypointType::item:
            return {255, 220, 0, 150};
        case WaypointType::respawn:
            return {0, 220, 255, 150};
        case WaypointType::jump_pad:
            return {0, 255, 120, 150};
        case WaypointType::lift_body:
            return {110, 150, 255, 150};
        case WaypointType::lift_entrance:
            return {140, 180, 255, 150};
        case WaypointType::lift_exit:
            return {80, 120, 255, 150};
        case WaypointType::ladder:
            return {255, 170, 70, 150};
        case WaypointType::ctf_flag:
            return {255, 70, 70, 150};
        case WaypointType::crater:
            return {200, 70, 255, 150};
        case WaypointType::tele_entrance:
            return {255, 140, 60, 150};
        case WaypointType::tele_exit:
            return {255, 80, 220, 150};
        default:
            return {200, 200, 200, 150};
    }
}

float debug_waypoint_sphere_scale(WaypointType type)
{
    if (type == WaypointType::std || type == WaypointType::std_new) {
        return 0.125f;
    }
    return 0.25f;
}

rf::Color debug_waypoint_zone_color(WaypointZoneType type)
{
    switch (type) {
        case WaypointZoneType::control_point:
            return {200, 70, 255, 150};
        case WaypointZoneType::damaging_liquid_room:
            return {70, 160, 255, 150};
        case WaypointZoneType::damage_zone:
            return {255, 150, 70, 150};
        case WaypointZoneType::instant_death_zone:
            return {255, 60, 60, 150};
        default:
            return {200, 200, 200, 150};
    }
}

rf::Color debug_waypoint_target_color(WaypointTargetType type)
{
    switch (type) {
        case WaypointTargetType::explosion:
            return {255, 120, 40, 150};
        default:
            return {200, 200, 200, 150};
    }
}

void draw_debug_wire_box(const std::array<rf::Vector3, 8>& corners)
{
    static constexpr std::array<std::array<int, 2>, 12> kBoxEdges{{
        {0, 1},
        {0, 2},
        {0, 4},
        {1, 3},
        {1, 5},
        {2, 3},
        {2, 6},
        {3, 7},
        {4, 5},
        {4, 6},
        {5, 7},
        {6, 7},
    }};

    for (const auto& edge : kBoxEdges) {
        rf::gr::line_vec(corners[edge[0]], corners[edge[1]], no_overdraw_2d_line);
    }
}

bool draw_debug_trigger_zone_bounds(const rf::Trigger& trigger, const rf::Color& color, rf::Vector3& out_center)
{
    out_center = trigger.pos;
    rf::gr::set_color(color);

    if (trigger.type == 1) {
        const rf::Vector3 half_extents{
            std::fabs(trigger.box_size.x) * 0.5f,
            std::fabs(trigger.box_size.y) * 0.5f,
            std::fabs(trigger.box_size.z) * 0.5f,
        };
        const auto& orient = trigger.orient;
        const rf::Vector3 center = trigger.pos;
        const rf::Vector3 r = orient.rvec * half_extents.x;
        const rf::Vector3 u = orient.uvec * half_extents.y;
        const rf::Vector3 f = orient.fvec * half_extents.z;

        const std::array<rf::Vector3, 8> corners{
            center - r - u - f,
            center - r - u + f,
            center - r + u - f,
            center - r + u + f,
            center + r - u - f,
            center + r - u + f,
            center + r + u - f,
            center + r + u + f,
        };
        draw_debug_wire_box(corners);
        return true;
    }

    const float radius = std::fabs(trigger.radius);
    if (radius <= 0.0f) {
        return false;
    }
    rf::gr::sphere(trigger.pos, radius, no_overdraw_2d_line);
    return true;
}

bool draw_debug_extent_zone_bounds(const WaypointZoneDefinition& zone, const rf::Color& color, rf::Vector3& out_center)
{
    const rf::Vector3 min_bound = point_min(zone.box_min, zone.box_max);
    const rf::Vector3 max_bound = point_max(zone.box_min, zone.box_max);
    out_center = (min_bound + max_bound) * 0.5f;

    const std::array<rf::Vector3, 8> corners{
        rf::Vector3{min_bound.x, min_bound.y, min_bound.z},
        rf::Vector3{min_bound.x, min_bound.y, max_bound.z},
        rf::Vector3{min_bound.x, max_bound.y, min_bound.z},
        rf::Vector3{min_bound.x, max_bound.y, max_bound.z},
        rf::Vector3{max_bound.x, min_bound.y, min_bound.z},
        rf::Vector3{max_bound.x, min_bound.y, max_bound.z},
        rf::Vector3{max_bound.x, max_bound.y, min_bound.z},
        rf::Vector3{max_bound.x, max_bound.y, max_bound.z},
    };

    rf::gr::set_color(color);
    draw_debug_wire_box(corners);
    return true;
}

bool draw_debug_target_bounds(const WaypointTargetDefinition& target, const rf::Color& color, rf::Vector3& out_center)
{
    constexpr float kHalfExtent = 0.5f; // 1x1x1 debug box
    out_center = target.pos;

    const std::array<rf::Vector3, 8> corners{
        rf::Vector3{target.pos.x - kHalfExtent, target.pos.y - kHalfExtent, target.pos.z - kHalfExtent},
        rf::Vector3{target.pos.x - kHalfExtent, target.pos.y - kHalfExtent, target.pos.z + kHalfExtent},
        rf::Vector3{target.pos.x - kHalfExtent, target.pos.y + kHalfExtent, target.pos.z - kHalfExtent},
        rf::Vector3{target.pos.x - kHalfExtent, target.pos.y + kHalfExtent, target.pos.z + kHalfExtent},
        rf::Vector3{target.pos.x + kHalfExtent, target.pos.y - kHalfExtent, target.pos.z - kHalfExtent},
        rf::Vector3{target.pos.x + kHalfExtent, target.pos.y - kHalfExtent, target.pos.z + kHalfExtent},
        rf::Vector3{target.pos.x + kHalfExtent, target.pos.y + kHalfExtent, target.pos.z - kHalfExtent},
        rf::Vector3{target.pos.x + kHalfExtent, target.pos.y + kHalfExtent, target.pos.z + kHalfExtent},
    };

    rf::gr::set_color(color);
    draw_debug_wire_box(corners);
    return true;
}

struct WaypointZoneDebugRenderInfo
{
    bool renderable = false;
    rf::Vector3 center{};
    WaypointZoneType type = WaypointZoneType::control_point;
    rf::Color color{};
};

struct WaypointTargetDebugRenderInfo
{
    bool renderable = false;
    rf::Vector3 center{};
    WaypointTargetType type = WaypointTargetType::explosion;
    rf::Color color{};
    std::vector<int> waypoint_uids{};
};

void draw_debug_waypoint_zones(bool show_membership_arrows, bool show_labels)
{
    if (g_waypoint_zones.empty()) {
        return;
    }

    std::vector<WaypointZoneDebugRenderInfo> zone_infos(g_waypoint_zones.size());

    for (int zone_index = 0; zone_index < static_cast<int>(g_waypoint_zones.size()); ++zone_index) {
        const auto& zone = g_waypoint_zones[zone_index];
        auto& zone_info = zone_infos[zone_index];
        zone_info.type = zone.type;
        zone_info.color = debug_waypoint_zone_color(zone.type);

        switch (resolve_waypoint_zone_source(zone)) {
            case WaypointZoneSource::trigger_uid: {
                rf::Object* trigger_obj = rf::obj_lookup_from_uid(zone.trigger_uid);
                if (!trigger_obj || trigger_obj->type != rf::OT_TRIGGER) {
                    break;
                }

                const auto* trigger = static_cast<rf::Trigger*>(trigger_obj);
                zone_info.renderable = draw_debug_trigger_zone_bounds(*trigger, zone_info.color, zone_info.center);
                break;
            }
            case WaypointZoneSource::box_extents:
                zone_info.renderable = draw_debug_extent_zone_bounds(zone, zone_info.color, zone_info.center);
                break;
            case WaypointZoneSource::room_uid:
            default:
                // Room zones are intentionally skipped for debug rendering.
                break;
        }
    }

    if (show_membership_arrows) {
        for (int wp_index = 1; wp_index < static_cast<int>(g_waypoints.size()); ++wp_index) {
            const auto& node = g_waypoints[wp_index];
            if (!node.valid) {
                continue;
            }

            for (int zone_index : node.zones) {
                if (zone_index < 0 || zone_index >= static_cast<int>(zone_infos.size())) {
                    continue;
                }
                const auto& zone_info = zone_infos[zone_index];
                if (!zone_info.renderable) {
                    continue;
                }

                rf::gr::line_arrow(
                    node.pos.x, node.pos.y, node.pos.z,
                    zone_info.center.x, zone_info.center.y, zone_info.center.z,
                    zone_info.color.red, zone_info.color.green, zone_info.color.blue);
            }
        }
    }

    if (show_labels) {
        for (const auto& zone_info : zone_infos) {
            if (!zone_info.renderable) {
                continue;
            }

            rf::Vector3 label_pos = zone_info.center;
            label_pos.y += 0.3f;
            rf::gr::Vertex dest{};
            if (!rf::gr::rotate_vertex(&dest, &label_pos)) {
                rf::gr::project_vertex(&dest);
                if (dest.flags & 1) {
                    const auto label_sv = waypoint_zone_type_name(zone_info.type);
                    char label[64]{};
                    std::snprintf(label, sizeof(label), "%.*s", static_cast<int>(label_sv.size()), label_sv.data());
                    rf::gr::set_color(zone_info.color.red, zone_info.color.green, zone_info.color.blue, 255);
                    rf::gr::string(static_cast<int>(dest.sx), static_cast<int>(dest.sy), label, -1, no_overdraw_2d_text);
                }
            }
        }
    }
}

void draw_debug_waypoint_targets(bool show_waypoint_arrows, bool show_labels)
{
    if (g_waypoint_targets.empty()) {
        return;
    }

    std::vector<WaypointTargetDebugRenderInfo> target_infos{};
    target_infos.reserve(g_waypoint_targets.size());
    for (const auto& target : g_waypoint_targets) {
        WaypointTargetDebugRenderInfo info{};
        info.type = target.type;
        info.color = debug_waypoint_target_color(target.type);
        info.waypoint_uids = target.waypoint_uids;
        info.renderable = draw_debug_target_bounds(target, info.color, info.center);
        target_infos.push_back(std::move(info));
    }

    if (show_waypoint_arrows) {
        for (const auto& target_info : target_infos) {
            if (!target_info.renderable) {
                continue;
            }
            for (int waypoint_uid : target_info.waypoint_uids) {
                if (waypoint_uid <= 0 || waypoint_uid >= static_cast<int>(g_waypoints.size())) {
                    continue;
                }
                const auto& waypoint = g_waypoints[waypoint_uid];
                if (!waypoint.valid) {
                    continue;
                }

                rf::gr::line_arrow(
                    target_info.center.x, target_info.center.y, target_info.center.z,
                    waypoint.pos.x, waypoint.pos.y, waypoint.pos.z,
                    target_info.color.red, target_info.color.green, target_info.color.blue);
            }
        }
    }

    if (show_labels) {
        for (const auto& target_info : target_infos) {
            if (!target_info.renderable) {
                continue;
            }

            rf::Vector3 label_pos = target_info.center;
            label_pos.y += 0.3f;
            rf::gr::Vertex dest{};
            if (!rf::gr::rotate_vertex(&dest, &label_pos)) {
                rf::gr::project_vertex(&dest);
                if (dest.flags & 1) {
                    const auto label_sv = waypoint_target_type_name(target_info.type);
                    char label[64]{};
                    std::snprintf(label, sizeof(label), "%.*s", static_cast<int>(label_sv.size()), label_sv.data());
                    rf::gr::set_color(target_info.color.red, target_info.color.green, target_info.color.blue, 255);
                    rf::gr::string(static_cast<int>(dest.sx), static_cast<int>(dest.sy), label, -1, no_overdraw_2d_text);
                }
            }
        }
    }
}

void draw_debug_waypoints()
{
    if (g_debug_waypoints_mode == 0) {
        return;
    }
    const bool show_links = g_debug_waypoints_mode >= 1;
    const bool show_spheres = g_debug_waypoints_mode >= 2;
    const bool show_labels = g_debug_waypoints_mode >= 3;
    const bool show_zone_membership_arrows = g_debug_waypoints_mode >= 2;
    const bool show_zone_labels = g_debug_waypoints_mode >= 3;
    const bool show_target_waypoint_arrows = g_debug_waypoints_mode >= 2;
    const bool show_target_labels = g_debug_waypoints_mode >= 3;
    draw_debug_waypoint_zones(show_zone_membership_arrows, show_zone_labels);
    draw_debug_waypoint_targets(show_target_waypoint_arrows, show_target_labels);
    for (int i = 1; i < static_cast<int>(g_waypoints.size()); ++i) {
        const auto& node = g_waypoints[i];
        if (!node.valid) {
            continue;
        }
        if (show_spheres) {
            rf::gr::set_color(debug_waypoint_color(node.type));
            const float debug_radius = sanitize_waypoint_link_radius(node.link_radius) * debug_waypoint_sphere_scale(node.type);
            rf::gr::sphere(node.pos, debug_radius, no_overdraw_2d_line);
        }
        if (show_labels) {
            rf::Vector3 label_pos = node.pos;
            label_pos.y += 0.3f;
            rf::gr::Vertex dest;
            if (!rf::gr::rotate_vertex(&dest, &label_pos)) {
                rf::gr::project_vertex(&dest);
                if (dest.flags & 1) {
                    const auto type_name = waypoint_type_name(node.type);
                    char buf[64];
                    if (node.identifier >= 0) {
                        std::snprintf(
                            buf, sizeof(buf), "%.*s (%d : %d : %d)",
                            static_cast<int>(type_name.size()), type_name.data(),
                            i, node.subtype, node.identifier);
                    }
                    else {
                        std::snprintf(
                            buf, sizeof(buf), "%.*s (%d : %d)",
                            static_cast<int>(type_name.size()), type_name.data(),
                            i, node.subtype);
                    }
                    rf::gr::set_color(255, 255, 255, 255);
                    rf::gr::string(static_cast<int>(dest.sx), static_cast<int>(dest.sy), buf, -1, no_overdraw_2d_text);
                }
            }
        }
        if (show_links) {
            for (int j = 0; j < node.num_links; ++j) {
                int link = node.links[j];
                if (link <= 0 || link >= static_cast<int>(g_waypoints.size())) {
                    continue;
                }
                const auto& dest = g_waypoints[link];
                rf::gr::line_arrow(node.pos.x, node.pos.y, node.pos.z, dest.pos.x, dest.pos.y, dest.pos.z, 0, 255, 0);
            }
        }
    }
}

ConsoleCommand2 waypoint_save_cmd{
    "waypoints_save",
    []() {
        if (save_waypoints()) {
            rf::console::print("Waypoints saved");
        }
        else {
            rf::console::print("No waypoints to save");
        }
    },
    "Save current waypoint graph to .awp",
    "waypoints_save",
};

ConsoleCommand2 waypoint_load_cmd{
    "waypoints_load",
    []() {
        if (load_waypoints()) {
            rf::console::print("Waypoints loaded");
        }
        else {
            rf::console::print("No waypoint file found");
        }
    },
    "Load waypoint graph from .awp",
    "waypoints_load",
};

ConsoleCommand2 waypoint_drop_cmd{
    "waypoints_drop",
    [](std::optional<bool> enabled) {
        if (enabled) {
            g_drop_waypoints = enabled.value();
        }
        else {
            g_drop_waypoints = !g_drop_waypoints;
        }
        rf::console::print("Waypoint auto-drop {}", g_drop_waypoints ? "enabled" : "disabled");
    },
    "Toggle waypoint auto-drop",
    "waypoints_drop [true|false]",
};

ConsoleCommand2 waypoint_debug_cmd{
    "waypoints_debug",
    [](std::optional<int> mode) {
        if (mode) {
            if (mode.value() < 0 || mode.value() > 3) {
                rf::console::print("Waypoint debug mode must be 0, 1, 2, or 3");
                return;
            }
            g_debug_waypoints_mode = mode.value();
        }
        else {
            g_debug_waypoints_mode = (g_debug_waypoints_mode + 1) % 4;
        }
        rf::console::print(
            "Waypoint debug mode {} (0=off, 1=links+zone_bounds+target_boxes, "
            "2=links+spheres+zone_bounds+zone_arrows+target_boxes+target_arrows, "
            "3=links+spheres+labels+zone_bounds+zone_arrows+zone_labels+target_boxes+target_arrows+target_labels)",
            g_debug_waypoints_mode);
    },
    "Set waypoint debug drawing mode",
    "waypoints_debug [0|1|2|3]",
};

ConsoleCommand2 waypoint_compress_cmd{
    "waypoints_compress",
    [](std::optional<bool> enabled) {
        if (enabled) {
            g_waypoints_compress = enabled.value();
        }
        else {
            g_waypoints_compress = !g_waypoints_compress;
        }
        rf::console::print("Waypoint compression {}", g_waypoints_compress ? "enabled" : "disabled");
    },
    "Toggle waypoint position compression on save",
    "waypoints_compress [true|false]",
};

ConsoleCommand2 waypoint_seed_bridges_cmd{
    "waypoints_seed_bridges",
    [](std::optional<bool> enabled) {
        if (enabled) {
            g_waypoints_seed_bridges = enabled.value();
        }
        else {
            g_waypoints_seed_bridges = !g_waypoints_seed_bridges;
        }
        rf::console::print(
            "Waypoint default-grid bridge seeding {}",
            g_waypoints_seed_bridges ? "enabled" : "disabled");
    },
    "Toggle bridge waypoint generation for default-grid seeded waypoints",
    "waypoints_seed_bridges [true|false]",
};

ConsoleCommand2 waypoint_clean_cmd{
    "waypoints_clean",
    []() {
        clean_waypoints();
        rf::console::print("Waypoints cleaned");
    },
    "Remove invalid waypoints",
    "waypoints_clean",
};

ConsoleCommand2 waypoint_reset_cmd{
    "waypoints_reset",
    []() {
        if (!(rf::level.flags & rf::LEVEL_LOADED)) {
            rf::console::print("No level loaded");
            return;
        }
        reset_waypoints_to_default_grid();
        rf::console::print("Waypoints reset to default map grid");
    },
    "Reset waypoints to default map grid",
    "waypoints_reset",
};

ConsoleCommand2 waypoint_zone_add_cmd{
    "waypoints_zone_add",
    []() {
        const std::string_view command_line = rf::console::cmd_line;
        const auto tokens = tokenize_console_command_line(command_line);
        if (tokens.size() < 3) {
            rf::console::print("Usage:");
            rf::console::print("  waypoints_zone_add <zone_type> trigger <trigger_uid>");
            rf::console::print("  waypoints_zone_add <zone_type> room <room_uid>");
            rf::console::print("  waypoints_zone_add <zone_type> box <min_x> <min_y> <min_z> <max_x> <max_y> <max_z>");
            return;
        }

        const std::string_view zone_type_token = tokens[1];
        const std::string_view source_token = tokens[2];

        auto zone_type = parse_waypoint_zone_type_token(zone_type_token);
        if (!zone_type) {
            rf::console::print("Invalid zone type '{}'", zone_type_token);
            return;
        }

        auto source = parse_waypoint_zone_source_token(source_token);
        if (!source) {
            rf::console::print("Invalid zone source '{}'", source_token);
            return;
        }

        WaypointZoneDefinition zone{};
        zone.type = zone_type.value();

        switch (source.value()) {
            case WaypointZoneSource::trigger_uid: {
                if (tokens.size() != 4) {
                    rf::console::print("Usage: waypoints_zone_add <zone_type> trigger <trigger_uid>");
                    return;
                }

                auto trigger_uid = parse_int_token(tokens[3]);
                if (!trigger_uid) {
                    rf::console::print("Invalid trigger UID '{}'", tokens[3]);
                    return;
                }

                rf::Object* trigger_obj = rf::obj_lookup_from_uid(trigger_uid.value());
                if (!trigger_obj || trigger_obj->type != rf::OT_TRIGGER) {
                    rf::console::print("UID {} is not a trigger", trigger_uid.value());
                    return;
                }

                zone.trigger_uid = trigger_uid.value();
                const int zone_index = add_waypoint_zone_definition(zone);
                rf::console::print("Added zone {} as index {} (trigger uid {})",
                                   waypoint_zone_type_name(zone.type), zone_index, zone.trigger_uid);
                return;
            }
            case WaypointZoneSource::room_uid: {
                if (tokens.size() != 4) {
                    rf::console::print("Usage: waypoints_zone_add <zone_type> room <room_uid>");
                    return;
                }

                auto room_uid = parse_int_token(tokens[3]);
                if (!room_uid) {
                    rf::console::print("Invalid room UID '{}'", tokens[3]);
                    return;
                }

                if (!rf::level_room_from_uid(room_uid.value())) {
                    rf::console::print("Room UID {} was not found", room_uid.value());
                    return;
                }

                zone.room_uid = room_uid.value();
                const int zone_index = add_waypoint_zone_definition(zone);
                rf::console::print("Added zone {} as index {} (room uid {})",
                                   waypoint_zone_type_name(zone.type), zone_index, zone.room_uid);
                return;
            }
            case WaypointZoneSource::box_extents: {
                if (tokens.size() != 9) {
                    rf::console::print(
                        "Usage: waypoints_zone_add <zone_type> box <min_x> <min_y> <min_z> <max_x> <max_y> <max_z>");
                    return;
                }

                std::array<float, 6> bounds{};
                for (size_t i = 0; i < bounds.size(); ++i) {
                    auto value = parse_float_token(tokens[3 + i]);
                    if (!value) {
                        rf::console::print("Invalid box value '{}'", tokens[3 + i]);
                        return;
                    }
                    bounds[i] = value.value();
                }

                zone.box_min = {bounds[0], bounds[1], bounds[2]};
                zone.box_max = {bounds[3], bounds[4], bounds[5]};
                const int zone_index = add_waypoint_zone_definition(zone);
                const auto& stored_zone = g_waypoint_zones[zone_index];
                rf::console::print("Added zone {} as index {} (box min {:.2f},{:.2f},{:.2f} max {:.2f},{:.2f},{:.2f})",
                                   waypoint_zone_type_name(stored_zone.type), zone_index,
                                   stored_zone.box_min.x, stored_zone.box_min.y, stored_zone.box_min.z,
                                   stored_zone.box_max.x, stored_zone.box_max.y, stored_zone.box_max.z);
                return;
            }
            default:
                rf::console::print("Invalid zone source '{}'", source_token);
                return;
        }
    },
    "Add a waypoint zone definition",
    "waypoints_zone_add <zone_type> <trigger|room|box> ...",
    true,
};

ConsoleCommand2 waypoint_zone_list_cmd{
    "waypoints_zone_list",
    []() {
        if (g_waypoint_zones.empty()) {
            rf::console::print("No waypoint zones defined");
            return;
        }

        rf::console::print("Waypoint zones ({})", static_cast<int>(g_waypoint_zones.size()));
        for (int i = 0; i < static_cast<int>(g_waypoint_zones.size()); ++i) {
            const auto& zone = g_waypoint_zones[i];
            switch (resolve_waypoint_zone_source(zone)) {
                case WaypointZoneSource::trigger_uid:
                    rf::console::print("  [{}] {} via {} uid {} (i {})",
                                       i, waypoint_zone_type_name(zone.type),
                                       waypoint_zone_source_name(WaypointZoneSource::trigger_uid),
                                       zone.trigger_uid, zone.identifier);
                    break;
                case WaypointZoneSource::room_uid:
                    rf::console::print("  [{}] {} via {} uid {} (i {})",
                                       i, waypoint_zone_type_name(zone.type),
                                       waypoint_zone_source_name(WaypointZoneSource::room_uid),
                                       zone.room_uid, zone.identifier);
                    break;
                case WaypointZoneSource::box_extents:
                    rf::console::print("  [{}] {} via {} min {:.2f},{:.2f},{:.2f} max {:.2f},{:.2f},{:.2f} (i {})",
                                       i, waypoint_zone_type_name(zone.type),
                                       waypoint_zone_source_name(WaypointZoneSource::box_extents),
                                       zone.box_min.x, zone.box_min.y, zone.box_min.z,
                                       zone.box_max.x, zone.box_max.y, zone.box_max.z, zone.identifier);
                    break;
                default:
                    break;
            }
        }
    },
    "List waypoint zones",
    "waypoints_zone_list",
};

ConsoleCommand2 waypoint_zone_delete_cmd{
    "waypoints_zone_delete",
    []() {
        const std::string_view command_line = rf::console::cmd_line;
        const auto tokens = tokenize_console_command_line(command_line);
        if (tokens.size() != 2) {
            rf::console::print("Usage: waypoints_zone_delete <zone_uid>");
            return;
        }

        auto zone_uid = parse_int_token(tokens[1]);
        if (!zone_uid) {
            rf::console::print("Invalid zone UID '{}'", tokens[1]);
            return;
        }

        if (!remove_waypoint_zone_definition(zone_uid.value())) {
            rf::console::print("No waypoint zone found with UID {}", zone_uid.value());
            return;
        }

        rf::console::print("Deleted waypoint zone {}", zone_uid.value());
    },
    "Delete a waypoint zone by UID/index",
    "waypoints_zone_delete <zone_uid>",
    true,
};

ConsoleCommand2 waypoint_zone_clear_cmd{
    "waypoints_zone_clear",
    []() {
        g_waypoint_zones.clear();
        for (int i = 1; i < static_cast<int>(g_waypoints.size()); ++i) {
            g_waypoints[i].zones.clear();
        }
        rf::console::print("Cleared waypoint zones");
    },
    "Remove all waypoint zones",
    "waypoints_zone_clear",
};

ConsoleCommand2 waypoint_target_add_cmd{
    "waypoints_target_add",
    []() {
        if (!(rf::level.flags & rf::LEVEL_LOADED)) {
            rf::console::print("No level loaded");
            return;
        }

        const std::string_view command_line = rf::console::cmd_line;
        const auto tokens = tokenize_console_command_line(command_line);
        if (tokens.size() != 2) {
            rf::console::print("Usage: waypoints_target_add <type>");
            rf::console::print("Valid target types: explosion");
            return;
        }

        auto target_type = parse_waypoint_target_type_token(tokens[1]);
        if (!target_type) {
            rf::console::print("Invalid target type '{}'", tokens[1]);
            rf::console::print("Valid target types: explosion");
            return;
        }

        auto target_pos = get_looked_at_target_point();
        if (!target_pos) {
            rf::console::print("Could not place target: no valid looked-at world position");
            return;
        }

        const int target_uid = add_waypoint_target(target_pos.value(), target_type.value());
        const auto* target = find_waypoint_target_by_uid(target_uid);
        const int waypoint_ref_count = target ? static_cast<int>(target->waypoint_uids.size()) : 0;
        rf::console::print(
            "Added target {} uid {} at {:.2f},{:.2f},{:.2f} ({} waypoint refs)",
            waypoint_target_type_name(target_type.value()),
            target_uid,
            target_pos->x, target_pos->y, target_pos->z,
            waypoint_ref_count);
    },
    "Add a waypoint target at the looked-at world position",
    "waypoints_target_add <type>",
    true,
};

ConsoleCommand2 waypoint_target_list_cmd{
    "waypoints_target_list",
    []() {
        if (g_waypoint_targets.empty()) {
            rf::console::print("No waypoint targets defined");
            return;
        }

        rf::console::print("Waypoint targets ({})", static_cast<int>(g_waypoint_targets.size()));
        for (const auto& target : g_waypoint_targets) {
            std::string waypoint_uid_list{};
            for (size_t i = 0; i < target.waypoint_uids.size(); ++i) {
                if (!waypoint_uid_list.empty()) {
                    waypoint_uid_list += ",";
                }
                waypoint_uid_list += std::to_string(target.waypoint_uids[i]);
            }
            if (waypoint_uid_list.empty()) {
                waypoint_uid_list = "-";
            }

            rf::console::print(
                "  [{}] {} p {:.2f},{:.2f},{:.2f} w {}",
                target.uid,
                waypoint_target_type_name(target.type),
                target.pos.x, target.pos.y, target.pos.z,
                waypoint_uid_list);
        }
    },
    "List waypoint targets",
    "waypoints_target_list",
};

ConsoleCommand2 waypoint_target_delete_cmd{
    "waypoints_target_delete",
    []() {
        const std::string_view command_line = rf::console::cmd_line;
        const auto tokens = tokenize_console_command_line(command_line);
        if (tokens.size() != 2) {
            rf::console::print("Usage: waypoints_target_delete <target_uid>");
            return;
        }

        auto target_uid = parse_int_token(tokens[1]);
        if (!target_uid) {
            rf::console::print("Invalid target UID '{}'", tokens[1]);
            return;
        }

        if (!remove_waypoint_target_by_uid(target_uid.value())) {
            rf::console::print("No waypoint target found with UID {}", target_uid.value());
            return;
        }

        rf::console::print("Deleted waypoint target {}", target_uid.value());
    },
    "Delete a waypoint target by UID",
    "waypoints_target_delete <target_uid>",
    true,
};

ConsoleCommand2 waypoint_target_clear_cmd{
    "waypoints_target_clear",
    []() {
        g_waypoint_targets.clear();
        g_next_waypoint_target_uid = 1;
        rf::console::print("Cleared waypoint targets");
    },
    "Remove all waypoint targets",
    "waypoints_target_clear",
};

void waypoints_init()
{
    glass_remove_floating_shards_hook.install();
    waypoint_save_cmd.register_cmd();
    waypoint_load_cmd.register_cmd();
    waypoint_drop_cmd.register_cmd();
    waypoint_debug_cmd.register_cmd();
    waypoint_compress_cmd.register_cmd();
    waypoint_seed_bridges_cmd.register_cmd();
    waypoint_clean_cmd.register_cmd();
    waypoint_reset_cmd.register_cmd();
    waypoint_zone_add_cmd.register_cmd();
    waypoint_zone_list_cmd.register_cmd();
    waypoint_zone_delete_cmd.register_cmd();
    waypoint_zone_clear_cmd.register_cmd();
    waypoint_target_add_cmd.register_cmd();
    waypoint_target_list_cmd.register_cmd();
    waypoint_target_delete_cmd.register_cmd();
    waypoint_target_clear_cmd.register_cmd();
}

void waypoints_level_init()
{
    g_has_loaded_wpt = load_waypoints();
    if (!g_has_loaded_wpt) {
        seed_waypoints_from_objects();
    }
    g_last_drop_waypoint_by_entity.clear();
    g_last_lift_uid_by_entity.clear();
    invalidate_cache();
}

void waypoints_level_reset()
{
    clear_waypoints();
    g_has_loaded_wpt = false;
    g_last_drop_waypoint_by_entity.clear();
    g_last_lift_uid_by_entity.clear();
}

void waypoints_do_frame()
{
    if (!(rf::level.flags & rf::LEVEL_LOADED)) {
        return;
    }
    if (!g_has_loaded_wpt) {
        seed_waypoint_zones_from_control_points();
    }
    if (!g_drop_waypoints && g_drop_waypoints_prev) {
        g_last_drop_waypoint_by_entity.clear();
        g_last_lift_uid_by_entity.clear();
    }
    g_drop_waypoints_prev = g_drop_waypoints;
    navigate();
}

void waypoints_render_debug()
{
    if (!(rf::level.flags & rf::LEVEL_LOADED)) {
        return;
    }
    draw_debug_waypoints();
}

int waypoints_closest(const rf::Vector3& pos, float radius)
{
    return closest_waypoint(pos, radius);
}

int waypoints_count()
{
    return static_cast<int>(g_waypoints.size());
}

bool waypoints_get_pos(int index, rf::Vector3& out_pos)
{
    if (index <= 0 || index >= static_cast<int>(g_waypoints.size())) {
        return false;
    }
    const auto& node = g_waypoints[index];
    if (!node.valid) {
        return false;
    }
    out_pos = node.pos;
    return true;
}

bool waypoints_get_type_subtype(int index, int& out_type, int& out_subtype)
{
    if (index <= 0 || index >= static_cast<int>(g_waypoints.size())) {
        return false;
    }

    const auto& node = g_waypoints[index];
    if (!node.valid) {
        return false;
    }

    out_type = static_cast<int>(node.type);
    out_subtype = node.subtype;
    return true;
}

int waypoints_get_links(int index, std::array<int, kMaxWaypointLinks>& out_links)
{
    out_links.fill(0);
    if (index <= 0 || index >= static_cast<int>(g_waypoints.size())) {
        return 0;
    }

    const auto& node = g_waypoints[index];
    if (!node.valid) {
        return 0;
    }

    int count = 0;
    for (int i = 0; i < node.num_links; ++i) {
        const int link = node.links[i];
        if (link <= 0
            || link >= static_cast<int>(g_waypoints.size())
            || !g_waypoints[link].valid) {
            continue;
        }
        out_links[count++] = link;
    }
    return count;
}

bool waypoints_has_direct_link(int from, int to)
{
    if (from <= 0 || to <= 0
        || from >= static_cast<int>(g_waypoints.size())
        || to >= static_cast<int>(g_waypoints.size())) {
        return false;
    }

    const auto& node = g_waypoints[from];
    if (!node.valid || !g_waypoints[to].valid) {
        return false;
    }

    for (int i = 0; i < node.num_links; ++i) {
        if (node.links[i] == to) {
            return true;
        }
    }

    return false;
}

bool waypoints_link_is_clear(int from, int to)
{
    if (!waypoints_has_direct_link(from, to)) {
        return false;
    }

    const auto& from_node = g_waypoints[from];
    const auto& to_node = g_waypoints[to];
    return can_link_waypoints(from_node.pos, to_node.pos);
}

bool waypoints_route(int from, int to, const std::unordered_set<int>& avoidset, std::vector<int>& out_path)
{
    out_path.clear();
    if (from <= 0 || to <= 0 || from >= static_cast<int>(g_waypoints.size()) ||
        to >= static_cast<int>(g_waypoints.size())) {
        return false;
    }
    if (from == to) {
        out_path.push_back(from);
        return true;
    }
    for (auto& node : g_waypoints) {
        node.route = -1;
        node.prev = -1;
        node.cur_score = std::numeric_limits<float>::infinity();
        node.est_score = std::numeric_limits<float>::infinity();
    }
    auto heuristic = [&](int a, int b) { return (g_waypoints[a].pos - g_waypoints[b].pos).len(); };
    struct NodeEntry
    {
        int index;
        float score;
        bool operator<(const NodeEntry& other) const
        {
            return score > other.score;
        }
    };
    std::priority_queue<NodeEntry> open;
    g_waypoints[from].cur_score = 0.0f;
    g_waypoints[from].est_score = heuristic(from, to);
    open.push({from, g_waypoints[from].est_score});
    while (!open.empty()) {
        int current = open.top().index;
        if (current == to) {
            break;
        }
        open.pop();
        auto& node = g_waypoints[current];
        if (!node.valid) {
            continue;
        }
        for (int i = 0; i < node.num_links; ++i) {
            int neighbor = node.links[i];
            if (neighbor <= 0 || neighbor >= static_cast<int>(g_waypoints.size())) {
                continue;
            }
            if (!g_waypoints[neighbor].valid) {
                continue;
            }
            if (avoidset.find(neighbor) != avoidset.end()) {
                continue;
            }
            float tentative = node.cur_score + (g_waypoints[neighbor].pos - node.pos).len();
            if (tentative < g_waypoints[neighbor].cur_score) {
                g_waypoints[neighbor].prev = current;
                g_waypoints[neighbor].cur_score = tentative;
                g_waypoints[neighbor].est_score = tentative + heuristic(neighbor, to);
                open.push({neighbor, g_waypoints[neighbor].est_score});
            }
        }
    }
    if (g_waypoints[to].prev == -1) {
        return false;
    }
    std::deque<int> reverse_path;
    int current = to;
    while (current != -1) {
        reverse_path.push_front(current);
        current = g_waypoints[current].prev;
    }
    out_path.assign(reverse_path.begin(), reverse_path.end());
    return !out_path.empty();
}
