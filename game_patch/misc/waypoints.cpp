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
#include "../rf/input.h"
#include "../rf/item.h"
#include "../rf/level.h"
#include "../rf/multi.h"
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
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
std::vector<std::string> g_waypoint_authors{};
bool g_waypoints_compress = true;
int g_next_waypoint_target_uid = 1;

std::unordered_map<int, int> g_last_drop_waypoint_by_entity{};
std::unordered_map<int, int> g_last_lift_uid_by_entity{};

void remap_waypoints();
bool link_waypoint_if_clear_autogen(int from, int to);
rf::Mover* find_mover_by_uid(int mover_uid);
void update_ctf_dropped_flag_temporary_waypoints();

int g_debug_waypoints_mode = 0;
bool g_drop_waypoints_prev = true;

enum class WaypointEditorSelectionKind : int
{
    none = 0,
    waypoint = 1,
    zone = 2,
    target = 3,
};

struct WaypointEditorSelectionState
{
    WaypointEditorSelectionKind kind = WaypointEditorSelectionKind::none;
    int uid = -1;
};

WaypointEditorSelectionState g_waypoint_editor_selection{};

constexpr int kWaypointSolidTraceFlags = rf::CF_ANY_HIT | rf::CF_PROCESS_INVISIBLE_FACES;
constexpr int kWaypointWorldTraceFlags = rf::CF_ANY_HIT | rf::CF_PROCESS_INVISIBLE_FACES;

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

bool has_waypoint_author(std::string_view author_name)
{
    for (const auto& existing_author : g_waypoint_authors) {
        if (string_iequals(existing_author, author_name)) {
            return true;
        }
    }
    return false;
}

void add_waypoint_author_if_missing(std::string_view author_name)
{
    std::string sanitized_author{trim(author_name)};
    if (sanitized_author.empty()) {
        return;
    }
    if (has_waypoint_author(sanitized_author)) {
        return;
    }
    g_waypoint_authors.push_back(std::move(sanitized_author));
}

bool is_waypoint_bot_mode_active()
{
    return g_alpine_game_config.client_bot_mode;
}

bool are_waypoint_commands_enabled_for_local_client()
{
    return is_waypoint_bot_mode_active() || g_alpine_game_config.waypoints_edit_mode;
}

bool ensure_waypoint_command_enabled(std::string_view command_name)
{
    if (are_waypoint_commands_enabled_for_local_client()) {
        return true;
    }
    rf::console::print(
        "{} ignored: waypoint editing is disabled. Run waypoints_edit first "
        "(not available while connected as multiplayer client).",
        command_name);
    return false;
}

bool is_multiplayer_client()
{
    return rf::is_multi && !rf::is_server;
}

bool is_ctf_mode_for_waypoints()
{
    return rf::is_multi && rf::multi_get_game_type() == rf::NG_TYPE_CTF;
}

rf::Vector3 get_ctf_flag_pos_world(bool red_flag)
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

bool is_ctf_flag_dropped(bool red_flag)
{
    const bool in_base = red_flag
        ? rf::multi_ctf_is_red_flag_in_base()
        : rf::multi_ctf_is_blue_flag_in_base();
    rf::Player* carrier = red_flag
        ? rf::multi_ctf_get_red_flag_player()
        : rf::multi_ctf_get_blue_flag_player();
    return !in_base && carrier == nullptr;
}

int get_ctf_flag_object_uid(bool red_flag)
{
    rf::Object* flag_obj = red_flag ? rf::ctf_red_flag_item : rf::ctf_blue_flag_item;
    return flag_obj ? flag_obj->uid : -1;
}

int ctf_flag_subtype(bool red_flag)
{
    return red_flag
        ? static_cast<int>(WaypointCtfFlagSubtype::red)
        : static_cast<int>(WaypointCtfFlagSubtype::blue);
}

int find_temporary_ctf_flag_waypoint(bool red_flag)
{
    const int subtype = ctf_flag_subtype(red_flag);
    for (int waypoint_uid = 1; waypoint_uid < static_cast<int>(g_waypoints.size()); ++waypoint_uid) {
        const auto& node = g_waypoints[waypoint_uid];
        if (!node.valid || !node.temporary) {
            continue;
        }
        if (node.type != WaypointType::ctf_flag || node.subtype != subtype) {
            continue;
        }
        return waypoint_uid;
    }
    return 0;
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

int waypoint_type_to_save_value(WaypointType type, bool preserve_std_new_types = false)
{
    // Preserve compatibility with older files where dropped waypoints are encoded as 0.
    if (!preserve_std_new_types && type == WaypointType::std_new) {
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

bool waypoint_type_is_standard(WaypointType type)
{
    return type == WaypointType::std || type == WaypointType::std_new;
}

WaypointDroppedSubtype waypoint_dropped_subtype_from_int(int raw_subtype)
{
    switch (raw_subtype) {
        case static_cast<int>(WaypointDroppedSubtype::normal):
            return WaypointDroppedSubtype::normal;
        case static_cast<int>(WaypointDroppedSubtype::crouch_needed):
            return WaypointDroppedSubtype::crouch_needed;
        case static_cast<int>(WaypointDroppedSubtype::swimming):
            return WaypointDroppedSubtype::swimming;
        case static_cast<int>(WaypointDroppedSubtype::falling):
            return WaypointDroppedSubtype::falling;
        case static_cast<int>(WaypointDroppedSubtype::unsafe_terrain):
            return WaypointDroppedSubtype::unsafe_terrain;
        default:
            return static_cast<WaypointDroppedSubtype>(raw_subtype);
    }
}

std::string_view waypoint_dropped_subtype_name(int raw_subtype)
{
    switch (waypoint_dropped_subtype_from_int(raw_subtype)) {
        case WaypointDroppedSubtype::normal:
            return "normal";
        case WaypointDroppedSubtype::crouch_needed:
            return "crouch_needed";
        case WaypointDroppedSubtype::swimming:
            return "swimming";
        case WaypointDroppedSubtype::falling:
            return "falling";
        case WaypointDroppedSubtype::unsafe_terrain:
            return "unsafe_terrain";
        default:
            return "unknown";
    }
}

int normalize_waypoint_dropped_subtype(int raw_subtype)
{
    switch (waypoint_dropped_subtype_from_int(raw_subtype)) {
        case WaypointDroppedSubtype::normal:
        case WaypointDroppedSubtype::crouch_needed:
        case WaypointDroppedSubtype::swimming:
        case WaypointDroppedSubtype::falling:
        case WaypointDroppedSubtype::unsafe_terrain:
            return raw_subtype;
        default:
            return static_cast<int>(WaypointDroppedSubtype::normal);
    }
}

int cycle_waypoint_dropped_subtype(int raw_subtype, int direction)
{
    static constexpr std::array<WaypointDroppedSubtype, 5> kCycleOrder{
        WaypointDroppedSubtype::normal,
        WaypointDroppedSubtype::crouch_needed,
        WaypointDroppedSubtype::swimming,
        WaypointDroppedSubtype::falling,
        WaypointDroppedSubtype::unsafe_terrain,
    };

    const int normalized = normalize_waypoint_dropped_subtype(raw_subtype);
    int index = 0;
    for (int i = 0; i < static_cast<int>(kCycleOrder.size()); ++i) {
        if (normalized == static_cast<int>(kCycleOrder[i])) {
            index = i;
            break;
        }
    }

    const int delta = (direction >= 0) ? 1 : -1;
    index = (index + delta + static_cast<int>(kCycleOrder.size())) % static_cast<int>(kCycleOrder.size());
    return static_cast<int>(kCycleOrder[index]);
}

WaypointTargetType waypoint_target_type_from_int(int raw_type)
{
    switch (raw_type) {
        case static_cast<int>(WaypointTargetType::explosion):
            return WaypointTargetType::explosion;
        case static_cast<int>(WaypointTargetType::shatter):
            return WaypointTargetType::shatter;
        default:
            return static_cast<WaypointTargetType>(raw_type);
    }
}

std::string_view waypoint_target_type_name(WaypointTargetType type)
{
    switch (type) {
        case WaypointTargetType::explosion:
            return "explosion";
        case WaypointTargetType::shatter:
            return "shatter";
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
        case static_cast<int>(WaypointZoneType::bridge_use):
            return WaypointZoneType::bridge_use;
        case static_cast<int>(WaypointZoneType::bridge_prox):
            return WaypointZoneType::bridge_prox;
        default:
            return static_cast<WaypointZoneType>(raw_type);
    }
}

bool waypoint_zone_type_is_bridge(WaypointZoneType type)
{
    return type == WaypointZoneType::bridge_use
        || type == WaypointZoneType::bridge_prox;
}

bool waypoint_zone_type_allows_auto_waypoint_membership(WaypointZoneType type)
{
    (void)type;
    return true;
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
        case WaypointZoneType::bridge_use:
            return "bridge_use";
        case WaypointZoneType::bridge_prox:
            return "bridge_prox";
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
    if (string_iequals(token, "mover_bridge_use") || string_iequals(token, "bridge_use")) {
        return WaypointZoneType::bridge_use;
    }
    if (string_iequals(token, "mover_bridge_prox") || string_iequals(token, "bridge_prox")
        || string_iequals(token, "mover_bridge_proximity")) {
        return WaypointZoneType::bridge_prox;
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
        if (numeric_type == static_cast<int>(WaypointTargetType::shatter)) {
            return WaypointTargetType::shatter;
        }
        return std::nullopt;
    }

    if (string_iequals(token, "explosion")) {
        return WaypointTargetType::explosion;
    }
    if (string_iequals(token, "shatter")) {
        return WaypointTargetType::shatter;
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

bool try_build_bridge_zone_state(int zone_uid, WaypointBridgeZoneState& out_state)
{
    if (zone_uid < 0 || zone_uid >= static_cast<int>(g_waypoint_zones.size())) {
        return false;
    }

    const auto& zone = g_waypoint_zones[zone_uid];
    if (!waypoint_zone_type_is_bridge(zone.type)) {
        return false;
    }
    if (resolve_waypoint_zone_source(zone) != WaypointZoneSource::trigger_uid) {
        return false;
    }

    rf::Object* trigger_obj = rf::obj_lookup_from_uid(zone.trigger_uid);
    if (!trigger_obj || trigger_obj->type != rf::OT_TRIGGER) {
        return false;
    }

    const auto* trigger = static_cast<rf::Trigger*>(trigger_obj);
    const bool trigger_available =
        !(trigger->obj_flags & rf::OF_DELAYED_DELETE)
        && (trigger->max_count < 0 || trigger->count < trigger->max_count)
        && (!trigger->next_check.valid() || trigger->next_check.elapsed());
    out_state.zone_uid = zone_uid;
    out_state.trigger_uid = zone.trigger_uid;
    out_state.trigger_pos = trigger->pos;
    out_state.activation_radius = waypoint_link_radius_from_trigger(*trigger);
    out_state.requires_use = (zone.type == WaypointZoneType::bridge_use);
    out_state.on = zone.on;
    out_state.available = trigger_available;
    return true;
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

void normalize_zone_bridge_waypoint_refs(std::vector<int>& waypoint_uids)
{
    waypoint_uids.erase(
        std::remove_if(
            waypoint_uids.begin(),
            waypoint_uids.end(),
            [](const int waypoint_uid) {
                return waypoint_uid <= 0;
            }),
        waypoint_uids.end());
    std::sort(waypoint_uids.begin(), waypoint_uids.end());
    waypoint_uids.erase(
        std::unique(waypoint_uids.begin(), waypoint_uids.end()),
        waypoint_uids.end());
}

bool is_waypoint_uid_valid(int waypoint_uid)
{
    return waypoint_uid > 0
        && waypoint_uid < static_cast<int>(g_waypoints.size())
        && g_waypoints[waypoint_uid].valid;
}

int remove_waypoint_links_from_to(int from, int to)
{
    if (!is_waypoint_uid_valid(from)) {
        return 0;
    }

    auto& node = g_waypoints[from];
    int write_index = 0;
    int removed = 0;
    for (int read_index = 0; read_index < node.num_links; ++read_index) {
        const int link = node.links[read_index];
        if (link == to) {
            ++removed;
            continue;
        }
        node.links[write_index++] = link;
    }
    node.num_links = write_index;
    for (int i = write_index; i < kMaxWaypointLinks; ++i) {
        node.links[i] = 0;
    }
    return removed;
}

int remove_waypoint_links_from_all(int from)
{
    if (!is_waypoint_uid_valid(from)) {
        return 0;
    }

    auto& node = g_waypoints[from];
    const int removed = node.num_links;
    node.num_links = 0;
    node.links.fill(0);
    return removed;
}

int remove_waypoint_links_to_from_all(int to)
{
    int removed = 0;
    for (int from = 1; from < static_cast<int>(g_waypoints.size()); ++from) {
        removed += remove_waypoint_links_from_to(from, to);
    }
    return removed;
}

bool tele_entrance_outbound_link_allowed(const WaypointNode& from_node, const WaypointNode& to_node)
{
    if (from_node.type != WaypointType::tele_entrance) {
        return true;
    }
    if (to_node.type != WaypointType::tele_exit) {
        return false;
    }
    if (from_node.identifier >= 0 && to_node.identifier >= 0) {
        return from_node.identifier == to_node.identifier;
    }
    return true;
}

bool lift_entrance_outbound_link_allowed(const WaypointNode& from_node, const WaypointNode& to_node)
{
    if (from_node.type != WaypointType::lift_entrance) {
        return true;
    }
    return to_node.type == WaypointType::lift_body;
}

bool lift_exit_inbound_link_allowed(const WaypointNode& from_node, const WaypointNode& to_node)
{
    if (to_node.type != WaypointType::lift_exit) {
        return true;
    }
    if (from_node.type != WaypointType::lift_body) {
        return false;
    }
    if (from_node.identifier >= 0 && to_node.identifier >= 0) {
        return from_node.identifier == to_node.identifier;
    }
    return true;
}

bool waypoint_link_types_allowed(const WaypointNode& from_node, const WaypointNode& to_node)
{
    return tele_entrance_outbound_link_allowed(from_node, to_node)
        && lift_entrance_outbound_link_allowed(from_node, to_node)
        && lift_exit_inbound_link_allowed(from_node, to_node);
}

bool waypoint_bridge_zone_is_active(const WaypointZoneDefinition& zone)
{
    if (!waypoint_zone_type_is_bridge(zone.type)) {
        return false;
    }
    return zone.on;
}

bool waypoint_zone_has_bridge_waypoint(const WaypointZoneDefinition& zone, const int waypoint_uid)
{
    if (!waypoint_zone_type_is_bridge(zone.type)) {
        return false;
    }
    return std::find(
               zone.bridge_waypoint_uids.begin(),
               zone.bridge_waypoint_uids.end(),
               waypoint_uid)
        != zone.bridge_waypoint_uids.end();
}

bool waypoint_uid_enabled_by_bridge_zone_state(const int waypoint_uid)
{
    if (waypoint_uid <= 0 || waypoint_uid >= static_cast<int>(g_waypoints.size())) {
        return false;
    }
    if (!g_waypoints[waypoint_uid].valid) {
        return false;
    }

    if (g_waypoint_zones.empty()) {
        return true;
    }

    bool has_bridge_zone = false;
    for (const auto& zone : g_waypoint_zones) {
        if (!waypoint_zone_has_bridge_waypoint(zone, waypoint_uid)) {
            continue;
        }

        has_bridge_zone = true;
        if (waypoint_bridge_zone_is_active(zone)) {
            return true;
        }
    }

    return !has_bridge_zone;
}

bool waypoint_link_blocked_by_bridge_zone_state(const int from_waypoint_uid, const int to_waypoint_uid)
{
    return !waypoint_uid_enabled_by_bridge_zone_state(from_waypoint_uid)
        || !waypoint_uid_enabled_by_bridge_zone_state(to_waypoint_uid);
}

void link_waypoint(int from, int to)
{
    if (from <= 0 || to <= 0 || from >= static_cast<int>(g_waypoints.size()) ||
        to >= static_cast<int>(g_waypoints.size())) {
        return;
    }
    auto& node = g_waypoints[from];
    if (!node.valid || !g_waypoints[to].valid) {
        return;
    }
    if (!waypoint_link_types_allowed(node, g_waypoints[to])) {
        return;
    }
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

struct WaypointCellCoord
{
    int x = 0;
    int y = 0;
    int z = 0;

    bool operator==(const WaypointCellCoord& other) const = default;
};

struct WaypointCellCoordHash
{
    std::size_t operator()(const WaypointCellCoord& coord) const
    {
        const std::size_t hx = std::hash<int>{}(coord.x);
        const std::size_t hy = std::hash<int>{}(coord.y);
        const std::size_t hz = std::hash<int>{}(coord.z);
        return hx ^ (hy << 1) ^ (hz << 2);
    }
};

using WaypointCellMap = std::unordered_map<WaypointCellCoord, std::vector<int>, WaypointCellCoordHash>;

WaypointCellCoord waypoint_cell_coord_from_pos(const rf::Vector3& pos, float cell_size)
{
    const float inv_cell = (cell_size > kWaypointLinkRadiusEpsilon) ? (1.0f / cell_size) : 1.0f;
    return WaypointCellCoord{
        static_cast<int>(std::floor(pos.x * inv_cell)),
        static_cast<int>(std::floor(pos.y * inv_cell)),
        static_cast<int>(std::floor(pos.z * inv_cell)),
    };
}

void build_waypoint_cell_map(WaypointCellMap& out_map, float cell_size)
{
    out_map.clear();
    out_map.reserve(g_waypoints.size());
    for (int i = 1; i < static_cast<int>(g_waypoints.size()); ++i) {
        const auto& node = g_waypoints[i];
        if (!node.valid) {
            continue;
        }
        out_map[waypoint_cell_coord_from_pos(node.pos, cell_size)].push_back(i);
    }
}

bool trace_ground_below_point(const rf::Vector3& pos, float max_downward_dist, rf::Vector3* out_hit_point = nullptr)
{
    if (max_downward_dist <= 0.0f) {
        return false;
    }

    rf::Vector3 p0 = pos;
    rf::Vector3 p1 = pos - rf::Vector3{0.0f, max_downward_dist, 0.0f};
    rf::PCollisionOut collision{};
    collision.obj_handle = -1;
    if (!rf::collide_linesegment_world(
            p0,
            p1,
            kWaypointWorldTraceFlags,
            &collision)) {
        return false;
    }

    if (out_hit_point) {
        *out_hit_point = collision.hit_point;
    }
    return true;
}

float trace_upward_clearance_from_floor_hit(const rf::Vector3& floor_hit_pos, float max_upward_dist)
{
    if (max_upward_dist <= 0.0f) {
        return 0.0f;
    }

    // Keep a tiny start offset so the upward trace does not immediately re-hit the floor.
    constexpr float kTraceStartOffset = 0.01f;
    rf::Vector3 p0 = floor_hit_pos + rf::Vector3{0.0f, kTraceStartOffset, 0.0f};
    rf::Vector3 p1 = floor_hit_pos + rf::Vector3{0.0f, max_upward_dist, 0.0f};
    rf::PCollisionOut collision{};
    collision.obj_handle = -1;
    if (!rf::collide_linesegment_world(
            p0,
            p1,
            kWaypointWorldTraceFlags,
            &collision)) {
        return max_upward_dist;
    }

    return std::clamp(collision.hit_point.y - floor_hit_pos.y, 0.0f, max_upward_dist);
}

float waypoint_incline_degrees(const rf::Vector3& from, const rf::Vector3& to)
{
    const float horizontal = std::sqrt(
        (to.x - from.x) * (to.x - from.x) +
        (to.z - from.z) * (to.z - from.z));
    const float vertical = std::fabs(to.y - from.y);
    if (horizontal <= kWaypointLinkRadiusEpsilon) {
        return (vertical <= kWaypointLinkRadiusEpsilon) ? 0.0f : 90.0f;
    }
    constexpr float kRadToDeg = 57.295779513f;
    return std::atan2(vertical, horizontal) * kRadToDeg;
}

bool waypoint_link_within_incline(const rf::Vector3& from, const rf::Vector3& to, float max_incline_deg)
{
    return waypoint_incline_degrees(from, to) <= max_incline_deg;
}

bool waypoint_upward_link_allowed(
    const rf::Vector3& from,
    const rf::Vector3& to,
    float max_upward_incline_deg)
{
    if (to.y <= from.y + kWaypointLinkRadiusEpsilon) {
        return true;
    }
    return waypoint_link_within_incline(from, to, max_upward_incline_deg);
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

    if (const auto* duration_node = zone_tbl.get("d"); duration_node && duration_node->is_number()) {
        const float parsed_duration = static_cast<float>(duration_node->value_or<double>(zone.duration));
        if (std::isfinite(parsed_duration) && parsed_duration >= 0.0f) {
            zone.duration = parsed_duration;
        }
    }
    else if (const auto* duration_node = zone_tbl.get("duration"); duration_node && duration_node->is_number()) {
        const float parsed_duration = static_cast<float>(duration_node->value_or<double>(zone.duration));
        if (std::isfinite(parsed_duration) && parsed_duration >= 0.0f) {
            zone.duration = parsed_duration;
        }
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

    if (waypoint_zone_type_is_bridge(zone.type) && !has_trigger_uid) {
        return false;
    }

    if (waypoint_zone_type_is_bridge(zone.type)) {
        const toml::array* bridge_waypoint_uids_node = zone_tbl.get_as<toml::array>("w");
        if (!bridge_waypoint_uids_node) {
            bridge_waypoint_uids_node = zone_tbl.get_as<toml::array>("bridge_waypoint_uids");
        }
        if (!bridge_waypoint_uids_node) {
            bridge_waypoint_uids_node = zone_tbl.get_as<toml::array>("bridge_waypoints");
        }
        if (bridge_waypoint_uids_node) {
            zone.bridge_waypoint_uids.reserve(bridge_waypoint_uids_node->size());
            for (const auto& waypoint_uid_node : *bridge_waypoint_uids_node) {
                if (waypoint_uid_node.is_number()) {
                    zone.bridge_waypoint_uids.push_back(
                        static_cast<int>(waypoint_uid_node.value_or(0)));
                }
            }
            normalize_zone_bridge_waypoint_refs(zone.bridge_waypoint_uids);
        }
    }
    else {
        zone.bridge_waypoint_uids.clear();
    }

    if (has_box_extents) {
        normalize_zone_box_bounds(zone);
    }

    // Bridge zones are runtime-gated from trigger activation state and always
    // start inactive when loaded/added.
    zone.on = false;
    zone.timer.invalidate();

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
    if (!waypoint_zone_type_allows_auto_waypoint_membership(zone.type)) {
        return false;
    }

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

int bridge_zone_duration_ms(const WaypointZoneDefinition& zone)
{
    float duration_seconds = zone.duration;
    if (!std::isfinite(duration_seconds) || duration_seconds < 0.0f) {
        duration_seconds = 5.0f;
    }
    const float duration_ms = duration_seconds * 1000.0f;
    if (!std::isfinite(duration_ms) || duration_ms <= 0.0f) {
        return 0;
    }
    if (duration_ms >= static_cast<float>(std::numeric_limits<int>::max())) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(std::lround(duration_ms));
}

void update_bridge_zone_states()
{
    for (auto& zone : g_waypoint_zones) {
        if (!waypoint_zone_type_is_bridge(zone.type) || !zone.on) {
            continue;
        }
        if (!zone.timer.valid() || zone.timer.elapsed()) {
            zone.on = false;
            zone.timer.invalidate();
        }
    }
}

void activate_bridge_zones_for_trigger_uid(int trigger_uid)
{
    if (trigger_uid < 0) {
        return;
    }

    for (auto& zone : g_waypoint_zones) {
        if (!waypoint_zone_type_is_bridge(zone.type)) {
            continue;
        }
        if (resolve_waypoint_zone_source(zone) != WaypointZoneSource::trigger_uid
            || zone.trigger_uid != trigger_uid) {
            continue;
        }

        zone.on = true;
        const int duration_ms = bridge_zone_duration_ms(zone);
        if (duration_ms > 0) {
            zone.timer.set(duration_ms);
        }
        else {
            zone.timer.invalidate();
        }
    }
}

void refresh_all_waypoint_zone_refs()
{
    for (int i = 1; i < static_cast<int>(g_waypoints.size()); ++i) {
        auto& node = g_waypoints[i];
        if (!node.valid) {
            node.zones.clear();
            continue;
        }

        std::vector<int> preserved_manual_refs{};
        preserved_manual_refs.reserve(node.zones.size());
        for (int zone_index : node.zones) {
            if (zone_index < 0 || zone_index >= static_cast<int>(g_waypoint_zones.size())) {
                continue;
            }
            const auto& zone = g_waypoint_zones[zone_index];
            if (!waypoint_zone_type_allows_auto_waypoint_membership(zone.type)) {
                preserved_manual_refs.push_back(zone_index);
            }
        }

        node.zones = collect_waypoint_zone_refs(node.pos, nullptr);
        node.zones.insert(
            node.zones.end(),
            preserved_manual_refs.begin(),
            preserved_manual_refs.end());
        normalize_waypoint_zone_refs(node.zones);
    }
}

int add_waypoint_zone_definition(WaypointZoneDefinition zone)
{
    if (resolve_waypoint_zone_source(zone) == WaypointZoneSource::box_extents) {
        normalize_zone_box_bounds(zone);
    }
    if (waypoint_zone_type_is_bridge(zone.type)) {
        normalize_zone_bridge_waypoint_refs(zone.bridge_waypoint_uids);
        if (!std::isfinite(zone.duration) || zone.duration < 0.0f) {
            zone.duration = 5.0f;
        }
        zone.on = false;
        zone.timer.invalidate();
    }
    else {
        zone.bridge_waypoint_uids.clear();
    }

    g_waypoint_zones.push_back(zone);
    refresh_all_waypoint_zone_refs();
    return static_cast<int>(g_waypoint_zones.size()) - 1;
}

bool remove_waypoint_by_uid(int waypoint_uid)
{
    if (!is_waypoint_uid_valid(waypoint_uid)) {
        return false;
    }

    auto& node = g_waypoints[waypoint_uid];
    node.valid = false;
    node.num_links = 0;
    node.links.fill(0);
    node.zones.clear();

    remap_waypoints();
    sanitize_waypoint_links_against_geometry();
    return true;
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

int clear_waypoint_zone_definitions()
{
    const int removed = static_cast<int>(g_waypoint_zones.size());
    g_waypoint_zones.clear();
    for (int i = 1; i < static_cast<int>(g_waypoints.size()); ++i) {
        g_waypoints[i].zones.clear();
    }
    return removed;
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

void normalize_all_zone_bridge_waypoint_refs()
{
    for (auto& zone : g_waypoint_zones) {
        if (!waypoint_zone_type_is_bridge(zone.type)) {
            zone.bridge_waypoint_uids.clear();
            continue;
        }

        zone.bridge_waypoint_uids.erase(
            std::remove_if(
                zone.bridge_waypoint_uids.begin(),
                zone.bridge_waypoint_uids.end(),
                [](const int waypoint_uid) {
                    if (waypoint_uid <= 0 || waypoint_uid >= static_cast<int>(g_waypoints.size())) {
                        return true;
                    }
                    return !g_waypoints[waypoint_uid].valid;
                }),
            zone.bridge_waypoint_uids.end());
        normalize_zone_bridge_waypoint_refs(zone.bridge_waypoint_uids);
    }
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
    target.identifier = -1;
    target.waypoint_uids = collect_target_waypoint_uids(pos);
    g_waypoint_targets.push_back(std::move(target));
    return g_waypoint_targets.back().uid;
}

int add_waypoint_target_with_waypoint_uids(
    const rf::Vector3& pos,
    WaypointTargetType type,
    std::vector<int> waypoint_uids,
    std::optional<int> preferred_uid = std::nullopt)
{
    WaypointTargetDefinition target{};
    target.uid = resolve_waypoint_target_uid(preferred_uid);
    target.pos = pos;
    target.type = type;
    target.identifier = -1;
    normalize_target_waypoint_uids(waypoint_uids);
    if (waypoint_uids.empty()) {
        waypoint_uids = collect_target_waypoint_uids(pos);
    }
    target.waypoint_uids = std::move(waypoint_uids);
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

int clear_waypoint_targets()
{
    const int removed = static_cast<int>(g_waypoint_targets.size());
    g_waypoint_targets.clear();
    g_next_waypoint_target_uid = 1;
    return removed;
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
                if (target.type != WaypointTargetType::explosion
                    && target.type != WaypointTargetType::shatter) {
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

bool waypoint_editor_selection_mode_active()
{
    if (is_waypoint_bot_mode_active()
        || !g_alpine_game_config.waypoints_edit_mode
        || g_debug_waypoints_mode < 1) {
        return false;
    }

    rf::Player* local_player = rf::local_player;
    if (!local_player || !local_player->cam) {
        return false;
    }

    return rf::camera_get_mode(*local_player->cam) == rf::CAMERA_FREELOOK;
}

void clear_waypoint_editor_selection()
{
    g_waypoint_editor_selection = {};
}

bool get_waypoint_editor_camera_ray(rf::Vector3& out_origin, rf::Vector3& out_dir, float& out_max_dist)
{
    rf::Player* local_player = rf::local_player;
    if (!local_player || !local_player->cam) {
        return false;
    }

    out_origin = rf::camera_get_pos(local_player->cam);
    const rf::Matrix3 orient = rf::camera_get_orient(local_player->cam);
    out_dir = orient.fvec;
    if (out_dir.len_sq() <= 0.0001f) {
        return false;
    }
    out_dir.normalize_safe();
    out_max_dist = 10000.0f;

    rf::Entity* entity = rf::entity_from_handle(local_player->entity_handle);
    rf::LevelCollisionOut col_info{};
    col_info.face = nullptr;
    col_info.obj_handle = -1;
    rf::Vector3 p0 = out_origin;
    rf::Vector3 p1 = out_origin + out_dir * out_max_dist;
    if (rf::collide_linesegment_level_for_multi(
            p0,
            p1,
            entity,
            nullptr,
            &col_info,
            0.1f,
            false,
            1.0f)) {
        const rf::Vector3 to_hit = col_info.hit_point - out_origin;
        out_max_dist = std::sqrt(to_hit.dot_prod(to_hit)) + 0.1f;
    }
    return true;
}

std::optional<float> ray_sphere_intersection_t(
    const rf::Vector3& ray_origin,
    const rf::Vector3& ray_dir,
    const rf::Vector3& sphere_center,
    float sphere_radius)
{
    if (!std::isfinite(sphere_radius) || sphere_radius <= 0.0f) {
        return std::nullopt;
    }

    const rf::Vector3 oc = ray_origin - sphere_center;
    const float b = oc.dot_prod(ray_dir);
    const float c = oc.dot_prod(oc) - sphere_radius * sphere_radius;
    const float discriminant = b * b - c;
    if (discriminant < 0.0f) {
        return std::nullopt;
    }

    const float sqrt_disc = std::sqrt(discriminant);
    float t = -b - sqrt_disc;
    if (t < 0.0f) {
        t = -b + sqrt_disc;
    }
    if (t < 0.0f) {
        return std::nullopt;
    }
    return t;
}

bool get_zone_selection_center_and_radius(
    const WaypointZoneDefinition& zone,
    rf::Vector3& out_center,
    float& out_radius)
{
    switch (resolve_waypoint_zone_source(zone)) {
        case WaypointZoneSource::trigger_uid: {
            rf::Object* trigger_obj = rf::obj_lookup_from_uid(zone.trigger_uid);
            if (!trigger_obj || trigger_obj->type != rf::OT_TRIGGER) {
                return false;
            }

            const auto* trigger = static_cast<rf::Trigger*>(trigger_obj);
            out_center = trigger->pos;
            if (trigger->type == 1) {
                const rf::Vector3 half_extents{
                    std::fabs(trigger->box_size.x) * 0.5f,
                    std::fabs(trigger->box_size.y) * 0.5f,
                    std::fabs(trigger->box_size.z) * 0.5f,
                };
                out_radius = std::sqrt(half_extents.dot_prod(half_extents));
                return out_radius > kWaypointLinkRadiusEpsilon;
            }

            out_radius = std::fabs(trigger->radius);
            return out_radius > kWaypointLinkRadiusEpsilon;
        }
        case WaypointZoneSource::box_extents: {
            const rf::Vector3 min_bound = point_min(zone.box_min, zone.box_max);
            const rf::Vector3 max_bound = point_max(zone.box_min, zone.box_max);
            out_center = (min_bound + max_bound) * 0.5f;
            const rf::Vector3 half_extents = (max_bound - min_bound) * 0.5f;
            out_radius = std::sqrt(half_extents.dot_prod(half_extents));
            return out_radius > kWaypointLinkRadiusEpsilon;
        }
        case WaypointZoneSource::room_uid:
        default:
            return false;
    }
}

bool is_waypoint_editor_selected_waypoint(int waypoint_uid)
{
    return g_waypoint_editor_selection.kind == WaypointEditorSelectionKind::waypoint
        && g_waypoint_editor_selection.uid == waypoint_uid;
}

bool is_waypoint_editor_selected_zone(int zone_uid)
{
    return g_waypoint_editor_selection.kind == WaypointEditorSelectionKind::zone
        && g_waypoint_editor_selection.uid == zone_uid;
}

bool is_waypoint_editor_selected_target(int target_uid)
{
    return g_waypoint_editor_selection.kind == WaypointEditorSelectionKind::target
        && g_waypoint_editor_selection.uid == target_uid;
}

void update_waypoint_editor_selection()
{
    if (!waypoint_editor_selection_mode_active()) {
        clear_waypoint_editor_selection();
        return;
    }

    rf::Vector3 ray_origin{};
    rf::Vector3 ray_dir{};
    float ray_max_dist = 0.0f;
    if (!get_waypoint_editor_camera_ray(ray_origin, ray_dir, ray_max_dist)) {
        clear_waypoint_editor_selection();
        return;
    }

    WaypointEditorSelectionState best_selection{};
    float best_t = ray_max_dist;

    for (int waypoint_uid = 1; waypoint_uid < static_cast<int>(g_waypoints.size()); ++waypoint_uid) {
        const auto& node = g_waypoints[waypoint_uid];
        if (!node.valid) {
            continue;
        }

        const float selection_radius = std::max(
            0.5f,
            sanitize_waypoint_link_radius(node.link_radius) * 0.25f);
        auto hit_t = ray_sphere_intersection_t(
            ray_origin,
            ray_dir,
            node.pos,
            selection_radius);
        if (!hit_t || hit_t.value() > best_t) {
            continue;
        }
        best_t = hit_t.value();
        best_selection.kind = WaypointEditorSelectionKind::waypoint;
        best_selection.uid = waypoint_uid;
    }

    for (int zone_uid = 0; zone_uid < static_cast<int>(g_waypoint_zones.size()); ++zone_uid) {
        rf::Vector3 zone_center{};
        float zone_radius = 0.0f;
        if (!get_zone_selection_center_and_radius(g_waypoint_zones[zone_uid], zone_center, zone_radius)) {
            continue;
        }
        auto hit_t = ray_sphere_intersection_t(
            ray_origin,
            ray_dir,
            zone_center,
            std::max(zone_radius, 0.5f));
        if (!hit_t || hit_t.value() > best_t) {
            continue;
        }
        best_t = hit_t.value();
        best_selection.kind = WaypointEditorSelectionKind::zone;
        best_selection.uid = zone_uid;
    }

    constexpr float kTargetSelectionRadius = 0.9f;
    for (const auto& target : g_waypoint_targets) {
        auto hit_t = ray_sphere_intersection_t(
            ray_origin,
            ray_dir,
            target.pos,
            kTargetSelectionRadius);
        if (!hit_t || hit_t.value() > best_t) {
            continue;
        }
        best_t = hit_t.value();
        best_selection.kind = WaypointEditorSelectionKind::target;
        best_selection.uid = target.uid;
    }

    g_waypoint_editor_selection = best_selection;
}

bool cycle_selected_waypoint_movement_subtype(int direction)
{
    if (g_waypoint_editor_selection.kind != WaypointEditorSelectionKind::waypoint
        || g_waypoint_editor_selection.uid <= 0
        || g_waypoint_editor_selection.uid >= static_cast<int>(g_waypoints.size())) {
        return false;
    }

    auto& node = g_waypoints[g_waypoint_editor_selection.uid];
    if (!node.valid) {
        return false;
    }

    node.movement_subtype = cycle_waypoint_dropped_subtype(node.movement_subtype, direction);
    rf::console::print(
        "Waypoint {} movement subtype set to {}",
        g_waypoint_editor_selection.uid,
        waypoint_dropped_subtype_name(node.movement_subtype));
    return true;
}

void delete_selected_waypoint_editor_object()
{
    if (g_waypoint_editor_selection.kind == WaypointEditorSelectionKind::waypoint) {
        const int waypoint_uid = g_waypoint_editor_selection.uid;
        if (remove_waypoint_by_uid(waypoint_uid)) {
            rf::console::print("Deleted waypoint {}", waypoint_uid);
        }
    }
    else if (g_waypoint_editor_selection.kind == WaypointEditorSelectionKind::zone) {
        const int zone_uid = g_waypoint_editor_selection.uid;
        if (remove_waypoint_zone_definition(zone_uid)) {
            rf::console::print("Deleted zone {}", zone_uid);
        }
    }
    else if (g_waypoint_editor_selection.kind == WaypointEditorSelectionKind::target) {
        const int target_uid = g_waypoint_editor_selection.uid;
        if (remove_waypoint_target_by_uid(target_uid)) {
            rf::console::print("Deleted target {}", target_uid);
        }
    }

    clear_waypoint_editor_selection();
}

void handle_waypoint_editor_input()
{
    if (!waypoint_editor_selection_mode_active()
        || g_waypoint_editor_selection.kind == WaypointEditorSelectionKind::none) {
        return;
    }

    if (rf::key_get_and_reset_down_counter(rf::KEY_DELETE) > 0) {
        delete_selected_waypoint_editor_object();
        return;
    }

    const int up_count = rf::key_get_and_reset_down_counter(rf::KEY_UP);
    const int down_count = rf::key_get_and_reset_down_counter(rf::KEY_DOWN);
    const int net_steps = up_count - down_count;
    if (net_steps == 0) {
        return;
    }

    const int direction = net_steps > 0 ? 1 : -1;
    const int steps = std::abs(net_steps);
    for (int i = 0; i < steps; ++i) {
        if (!cycle_selected_waypoint_movement_subtype(direction)) {
            break;
        }
    }
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

int find_special_waypoint_in_radius(const rf::Vector3& pos)
{
    const int closest_jump_pad = find_jump_pad_waypoint_in_radius(pos);
    const int closest_tele_entrance = find_tele_entrance_waypoint_in_radius(pos);
    if (closest_jump_pad <= 0) {
        return closest_tele_entrance;
    }
    if (closest_tele_entrance <= 0) {
        return closest_jump_pad;
    }
    return distance_sq(pos, g_waypoints[closest_tele_entrance].pos)
            < distance_sq(pos, g_waypoints[closest_jump_pad].pos)
        ? closest_tele_entrance
        : closest_jump_pad;
}

int find_lift_uid_below_waypoint(const rf::Vector3& pos)
{
    constexpr float kLiftTraceDistance = 4.0f;
    rf::Vector3 p0 = pos;
    rf::Vector3 p1 = pos - rf::Vector3{0.0f, kLiftTraceDistance, 0.0f};
    rf::PCollisionOut collision{};
    collision.obj_handle = -1;
    const bool hit = rf::collide_linesegment_world(
        p0,
        p1,
        kWaypointWorldTraceFlags,
        &collision);
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

rf::Mover* find_mover_by_uid(int mover_uid)
{
    if (mover_uid < 0) {
        return nullptr;
    }

    rf::Object* mover_obj = rf::obj_lookup_from_uid(mover_uid);
    if (!mover_obj || mover_obj->type != rf::OT_MOVER) {
        return nullptr;
    }

    return static_cast<rf::Mover*>(mover_obj);
}

bool get_mover_lift_path_delta(const rf::Mover& mover, rf::Vector3& out_delta)
{
    const int keyframe_count = mover.keyframes.size();
    if (keyframe_count < 2) {
        return false;
    }

    const rf::MoverKeyframe* first_keyframe = mover.keyframes[0];
    const rf::MoverKeyframe* last_keyframe = mover.keyframes[keyframe_count - 1];
    if (!first_keyframe || !last_keyframe) {
        return false;
    }

    out_delta = last_keyframe->pos - first_keyframe->pos;
    return out_delta.len_sq() > (kWaypointLinkRadiusEpsilon * kWaypointLinkRadiusEpsilon);
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

void auto_link_default_seeded_waypoints(std::vector<int>& seeded_indices, std::vector<int>& source_indices)
{
    if (seeded_indices.empty() || source_indices.empty()) {
        return;
    }

    for (int source_index : source_indices) {
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

        for (int target_index : seeded_indices) {
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
                link_waypoint_if_clear_autogen(source_index, target_index);
                link_waypoint_if_clear_autogen(target_index, source_index);
            }
        }
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
    return !rf::collide_linesegment_level_solid(
        p0,
        p1,
        kWaypointSolidTraceFlags,
        &collision);
}

bool waypoint_has_horizontal_geometry_clearance(const rf::Vector3& pos, float clearance)
{
    if (clearance <= 0.0f) {
        return true;
    }

    constexpr float kInvSqrt2 = 0.70710678f;
    static const std::array<rf::Vector3, 8> kTraceDirs{
        rf::Vector3{1.0f, 0.0f, 0.0f},
        rf::Vector3{kInvSqrt2, 0.0f, kInvSqrt2},
        rf::Vector3{0.0f, 0.0f, 1.0f},
        rf::Vector3{-kInvSqrt2, 0.0f, kInvSqrt2},
        rf::Vector3{-1.0f, 0.0f, 0.0f},
        rf::Vector3{-kInvSqrt2, 0.0f, -kInvSqrt2},
        rf::Vector3{0.0f, 0.0f, -1.0f},
        rf::Vector3{kInvSqrt2, 0.0f, -kInvSqrt2},
    };

    for (const auto& dir : kTraceDirs) {
        rf::Vector3 p0 = pos;
        rf::Vector3 p1 = pos + dir * clearance;
        rf::GCollisionOutput collision{};
        if (rf::collide_linesegment_level_solid(
                p0,
                p1,
                kWaypointSolidTraceFlags,
                &collision)) {
            return false;
        }
    }
    return true;
}

bool waypoint_has_ground_edge_clearance(
    const rf::Vector3& pos,
    float clearance_radius,
    int probe_count = kWaypointGenerateEdgeProbeCount,
    int unsupported_threshold = kWaypointGenerateEdgeUnsupportedThreshold)
{
    if (clearance_radius <= 0.0f || probe_count <= 0 || unsupported_threshold <= 0) {
        return true;
    }

    constexpr float kTwoPi = 6.283185307f;
    int unsupported_probes = 0;
    for (int probe_index = 0; probe_index < probe_count; ++probe_index) {
        const float angle =
            (kTwoPi * static_cast<float>(probe_index)) / static_cast<float>(probe_count);
        const rf::Vector3 probe_pos{
            pos.x + std::cos(angle) * clearance_radius,
            pos.y,
            pos.z + std::sin(angle) * clearance_radius,
        };
        if (!trace_ground_below_point(probe_pos, kBridgeWaypointMaxGroundDistance)) {
            ++unsupported_probes;
            if (unsupported_probes >= unsupported_threshold) {
                return false;
            }
        }
    }
    return true;
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
    if (!waypoint_link_types_allowed(from_node, to_node)) {
        return false;
    }
    if (from_node.type == WaypointType::tele_entrance) {
        return true;
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

bool lift_body_autogen_outbound_link_allowed(const WaypointNode& from_node, const WaypointNode& to_node)
{
    if (from_node.type != WaypointType::lift_body) {
        return true;
    }
    if (to_node.type != WaypointType::lift_body && to_node.type != WaypointType::lift_exit) {
        return false;
    }
    if (from_node.identifier < 0 || to_node.identifier < 0 || from_node.identifier != to_node.identifier) {
        return false;
    }

    rf::Mover* mover = find_mover_by_uid(from_node.identifier);
    if (!mover) {
        return false;
    }
    rf::Vector3 lift_delta{};
    if (!get_mover_lift_path_delta(*mover, lift_delta)) {
        return false;
    }

    const rf::Vector3 link_delta = to_node.pos - from_node.pos;
    return link_delta.dot_prod(lift_delta) > kWaypointLinkRadiusEpsilon;
}

bool can_link_waypoint_indices_autogen(int from, int to)
{
    if (!can_link_waypoint_indices(from, to)) {
        return false;
    }
    if (from <= 0 || from >= static_cast<int>(g_waypoints.size())) {
        return false;
    }
    const auto& from_node = g_waypoints[from];
    const auto& to_node = g_waypoints[to];
    return from_node.valid
        && from_node.type != WaypointType::jump_pad
        && lift_body_autogen_outbound_link_allowed(from_node, to_node);
}

bool waypoint_type_requires_floor_supported_midpoint_for_autogen_link(WaypointType type)
{
    switch (type) {
        case WaypointType::std:
        case WaypointType::std_new:
        case WaypointType::item:
        case WaypointType::respawn:
        case WaypointType::jump_pad:
        case WaypointType::ctf_flag:
        case WaypointType::tele_entrance:
        case WaypointType::tele_exit:
            return true;
        default:
            return false;
    }
}

bool autogen_link_midpoint_has_floor_support(int from, int to)
{
    if (from <= 0 || to <= 0
        || from >= static_cast<int>(g_waypoints.size())
        || to >= static_cast<int>(g_waypoints.size())) {
        return false;
    }

    const auto& from_node = g_waypoints[from];
    const auto& to_node = g_waypoints[to];
    if (!from_node.valid || !to_node.valid) {
        return false;
    }

    if (!waypoint_type_requires_floor_supported_midpoint_for_autogen_link(from_node.type)
        || !waypoint_type_requires_floor_supported_midpoint_for_autogen_link(to_node.type)) {
        return true;
    }

    const rf::Vector3 midpoint = (from_node.pos + to_node.pos) * 0.5f;
    return trace_ground_below_point(midpoint, kBridgeWaypointMaxGroundDistance);
}

bool autogen_directional_link_allowed(int from, int to)
{
    if (from <= 0 || to <= 0
        || from >= static_cast<int>(g_waypoints.size())
        || to >= static_cast<int>(g_waypoints.size())) {
        return false;
    }

    const auto& from_node = g_waypoints[from];
    const auto& to_node = g_waypoints[to];
    if (!from_node.valid || !to_node.valid) {
        return false;
    }

    if (!waypoint_type_requires_floor_supported_midpoint_for_autogen_link(from_node.type)
        || !waypoint_type_requires_floor_supported_midpoint_for_autogen_link(to_node.type)) {
        return true;
    }

    rf::Vector3 from_floor_hit{};
    if (!trace_ground_below_point(from_node.pos, kBridgeWaypointMaxGroundDistance, &from_floor_hit)) {
        return false;
    }

    rf::Vector3 to_floor_hit{};
    if (!trace_ground_below_point(to_node.pos, kBridgeWaypointMaxGroundDistance, &to_floor_hit)) {
        return false;
    }

    if (!autogen_link_midpoint_has_floor_support(from, to)) {
        return false;
    }

    constexpr float kAutogenLevelLinkHeightThreshold = 0.25f;
    constexpr float kAutogenLevelLinkFeetTraceDrop = 0.75f;
    if (std::fabs(to_node.pos.y - from_node.pos.y) <= kAutogenLevelLinkHeightThreshold) {
        rf::Vector3 p0 = from_node.pos - rf::Vector3{0.0f, kAutogenLevelLinkFeetTraceDrop, 0.0f};
        rf::Vector3 p1 = to_node.pos - rf::Vector3{0.0f, kAutogenLevelLinkFeetTraceDrop, 0.0f};
        rf::GCollisionOutput feet_collision{};
        if (rf::collide_linesegment_level_solid(
                p0,
                p1,
                kWaypointSolidTraceFlags,
                &feet_collision)) {
            return false;
        }
    }

    if (to_node.pos.y <= from_node.pos.y + kWaypointLinkRadiusEpsilon) {
        return true;
    }

    if (!waypoint_upward_link_allowed(from_node.pos, to_node.pos, kWaypointGenerateMaxInclineDeg)) {
        return false;
    }

    constexpr float kAutogenUpwardLipProbeHeight = 0.125f;
    const rf::Vector3* lower_floor = &from_floor_hit;
    const rf::Vector3* higher_floor = &to_floor_hit;
    if (from_floor_hit.y > to_floor_hit.y) {
        std::swap(lower_floor, higher_floor);
    }

    rf::Vector3 p0 = *lower_floor + rf::Vector3{0.0f, kAutogenUpwardLipProbeHeight, 0.0f};
    rf::Vector3 p1 = *higher_floor + rf::Vector3{0.0f, kAutogenUpwardLipProbeHeight, 0.0f};
    rf::GCollisionOutput collision{};
    if (rf::collide_linesegment_level_solid(
            p0,
            p1,
            kWaypointSolidTraceFlags,
            &collision)) {
        return false;
    }

    return true;
}

bool link_waypoint_if_clear_autogen(int from, int to)
{
    if (!can_link_waypoint_indices_autogen(from, to)) {
        return false;
    }
    if (!autogen_directional_link_allowed(from, to)) {
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
        crater_pos,
        WaypointType::crater,
        0,
        false,
        true,
        kWaypointLinkRadius,
        -1,
        nullptr,
        true,
        static_cast<int>(WaypointDroppedSubtype::normal),
        true);

    const float link_radius_sq = kWaypointLinkRadius * kWaypointLinkRadius;
    for (int i = 1; i < crater_index; ++i) {
        const auto& node = g_waypoints[i];
        if (!node.valid) {
            continue;
        }
        if (distance_sq(crater_pos, node.pos) > link_radius_sq) {
            continue;
        }

        if (waypoint_upward_link_allowed(crater_pos, node.pos, kWaypointGenerateMaxInclineDeg)) {
            link_waypoint_if_clear(crater_index, i);
        }
        if (waypoint_upward_link_allowed(node.pos, crater_pos, kWaypointGenerateMaxInclineDeg)) {
            link_waypoint_if_clear(i, crater_index);
        }
    }
}

void link_temporary_waypoint_like_crater(int waypoint_uid)
{
    if (!is_waypoint_uid_valid(waypoint_uid)) {
        return;
    }

    const auto& temp_waypoint = g_waypoints[waypoint_uid];
    const rf::Vector3 waypoint_pos = temp_waypoint.pos;
    const float link_radius_sq =
        sanitize_waypoint_link_radius(temp_waypoint.link_radius)
        * sanitize_waypoint_link_radius(temp_waypoint.link_radius);
    for (int i = 1; i < static_cast<int>(g_waypoints.size()); ++i) {
        if (i == waypoint_uid) {
            continue;
        }

        const auto& node = g_waypoints[i];
        if (!node.valid) {
            continue;
        }
        if (distance_sq(waypoint_pos, node.pos) > link_radius_sq) {
            continue;
        }

        if (waypoint_upward_link_allowed(waypoint_pos, node.pos, kWaypointGenerateMaxInclineDeg)) {
            link_waypoint_if_clear(waypoint_uid, i);
        }
        if (waypoint_upward_link_allowed(node.pos, waypoint_pos, kWaypointGenerateMaxInclineDeg)) {
            link_waypoint_if_clear(i, waypoint_uid);
        }
    }
}

int create_temporary_dropped_flag_waypoint(bool red_flag, const rf::Vector3& flag_pos, int flag_uid)
{
    const int waypoint_uid = add_waypoint(
        flag_pos,
        WaypointType::ctf_flag,
        ctf_flag_subtype(red_flag),
        false,
        true,
        kWaypointLinkRadius,
        flag_uid,
        nullptr,
        false,
        static_cast<int>(WaypointDroppedSubtype::normal),
        true);
    link_temporary_waypoint_like_crater(waypoint_uid);
    return waypoint_uid;
}

void remove_temporary_ctf_flag_waypoints(bool red_flag)
{
    while (true) {
        const int waypoint_uid = find_temporary_ctf_flag_waypoint(red_flag);
        if (waypoint_uid <= 0) {
            break;
        }
        remove_waypoint_by_uid(waypoint_uid);
    }
}

void update_ctf_dropped_flag_temporary_waypoint_for_team(bool red_flag)
{
    const bool dropped = is_ctf_flag_dropped(red_flag);
    const int existing_waypoint_uid = find_temporary_ctf_flag_waypoint(red_flag);
    if (!dropped) {
        if (existing_waypoint_uid > 0) {
            remove_waypoint_by_uid(existing_waypoint_uid);
        }
        return;
    }

    const rf::Vector3 flag_pos = get_ctf_flag_pos_world(red_flag);
    const int flag_uid = get_ctf_flag_object_uid(red_flag);
    if (existing_waypoint_uid <= 0) {
        create_temporary_dropped_flag_waypoint(red_flag, flag_pos, flag_uid);
        return;
    }

    constexpr float kDroppedFlagWaypointMoveThreshold = 0.75f;
    auto& waypoint = g_waypoints[existing_waypoint_uid];
    waypoint.identifier = flag_uid;
    if (distance_sq(waypoint.pos, flag_pos)
        <= kDroppedFlagWaypointMoveThreshold * kDroppedFlagWaypointMoveThreshold) {
        return;
    }

    remove_waypoint_by_uid(existing_waypoint_uid);
    create_temporary_dropped_flag_waypoint(red_flag, flag_pos, flag_uid);
}

void update_ctf_dropped_flag_temporary_waypoints()
{
    if (!is_waypoint_bot_mode_active()) {
        remove_temporary_ctf_flag_waypoints(true);
        remove_temporary_ctf_flag_waypoints(false);
        return;
    }

    if (!is_ctf_mode_for_waypoints()) {
        remove_temporary_ctf_flag_waypoints(true);
        remove_temporary_ctf_flag_waypoints(false);
        return;
    }

    update_ctf_dropped_flag_temporary_waypoint_for_team(true);
    update_ctf_dropped_flag_temporary_waypoint_for_team(false);
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

int add_waypoint(
    const rf::Vector3& pos, WaypointType type, int subtype, bool link_to_nearest, bool bidirectional_link,
    float link_radius, int identifier, const rf::Object* source_object, bool auto_assign_zones,
    int movement_subtype, bool temporary)
{
    WaypointNode node{};
    node.pos = pos;
    node.type = type;
    node.subtype = waypoint_type_is_standard(type) ? 0 : subtype;
    node.movement_subtype = normalize_waypoint_dropped_subtype(movement_subtype);
    node.identifier = identifier;
    node.link_radius = sanitize_waypoint_link_radius(link_radius);
    node.temporary = temporary;
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
                    kWaypointLinkRadius, obj->uid, obj);
                seeded_indices.push_back(waypoint_index);
                auto_link_source_indices.push_back(waypoint_index);
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
    g_waypoint_authors.clear();
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

std::vector<int> collect_generation_seed_waypoint_indices()
{
    std::vector<int> seed_indices{};
    seed_indices.reserve(g_waypoints.size());

    static constexpr std::array<WaypointType, 3> kSeedTypeOrder{
        WaypointType::ctf_flag,
        WaypointType::item,
        WaypointType::respawn,
    };

    for (const auto seed_type : kSeedTypeOrder) {
        for (int i = 1; i < static_cast<int>(g_waypoints.size()); ++i) {
            const auto& node = g_waypoints[i];
            if (!node.valid || node.type != seed_type) {
                continue;
            }
            seed_indices.push_back(i);
        }
    }
    return seed_indices;
}

void add_lift_entrance_generation_candidate(
    std::unordered_map<int, std::vector<int>>& lift_entrances_by_uid,
    int lift_uid,
    int waypoint_index)
{
    if (lift_uid < 0 || waypoint_index <= 0) {
        return;
    }

    auto& indices = lift_entrances_by_uid[lift_uid];
    if (std::find(indices.begin(), indices.end(), waypoint_index) == indices.end()) {
        indices.push_back(waypoint_index);
    }
}

int ladder_axis_sample_count(float extent, float step)
{
    const float abs_extent = std::fabs(extent);
    if (abs_extent <= kWaypointLinkRadiusEpsilon) {
        return 1;
    }
    int sample_count = std::max(2, static_cast<int>(std::ceil((abs_extent * 2.0f) / step)) + 1);
    if ((sample_count % 2) == 0) {
        ++sample_count; // Keep center sample at zero so growth starts at region center.
    }
    return sample_count;
}

float ladder_axis_sample_coordinate(float extent, int index, int sample_count)
{
    const float abs_extent = std::fabs(extent);
    if (sample_count <= 1 || abs_extent <= kWaypointLinkRadiusEpsilon) {
        return 0.0f;
    }
    const float t = static_cast<float>(index) / static_cast<float>(sample_count - 1);
    return -abs_extent + (abs_extent * 2.0f) * t;
}

std::vector<rf::Vector3> build_ladder_seed_offsets_center_out(const rf::ClimbRegion& climb_region, float step)
{
    const int x_samples = ladder_axis_sample_count(climb_region.extents.x, step);
    const int y_samples = ladder_axis_sample_count(climb_region.extents.y, step);
    const int z_samples = ladder_axis_sample_count(climb_region.extents.z, step);

    std::vector<rf::Vector3> offsets{};
    offsets.reserve(static_cast<size_t>(x_samples) * static_cast<size_t>(y_samples) * static_cast<size_t>(z_samples));

    for (int x_idx = 0; x_idx < x_samples; ++x_idx) {
        const float local_x = ladder_axis_sample_coordinate(climb_region.extents.x, x_idx, x_samples);
        for (int y_idx = 0; y_idx < y_samples; ++y_idx) {
            const float local_y = ladder_axis_sample_coordinate(climb_region.extents.y, y_idx, y_samples);
            for (int z_idx = 0; z_idx < z_samples; ++z_idx) {
                const float local_z = ladder_axis_sample_coordinate(climb_region.extents.z, z_idx, z_samples);
                offsets.push_back(rf::Vector3{local_x, local_y, local_z});
            }
        }
    }

    std::stable_sort(offsets.begin(), offsets.end(), [](const rf::Vector3& a, const rf::Vector3& b) {
        return a.len_sq() < b.len_sq();
    });
    return offsets;
}

bool climb_region_point_has_edge_clearance(const rf::ClimbRegion& climb_region, const rf::Vector3& point, float margin)
{
    if (margin <= kWaypointLinkRadiusEpsilon) {
        return true;
    }

    rf::Vector3 point_query = point;
    if (rf::level_point_in_climb_region(&point_query) != &climb_region) {
        return false;
    }

    std::array<rf::Vector3, 3> axes{
        climb_region.orient.rvec,
        climb_region.orient.uvec,
        climb_region.orient.fvec,
    };

    for (auto axis : axes) {
        if (axis.len_sq() <= kWaypointLinkRadiusEpsilon * kWaypointLinkRadiusEpsilon) {
            continue;
        }
        axis.normalize_safe();

        rf::Vector3 forward_probe = point + axis * margin;
        if (rf::level_point_in_climb_region(&forward_probe) != &climb_region) {
            return false;
        }

        rf::Vector3 backward_probe = point - axis * margin;
        if (rf::level_point_in_climb_region(&backward_probe) != &climb_region) {
            return false;
        }
    }

    return true;
}

int seed_climb_region_waypoints_for_autogen(
    const rf::ClimbRegion& climb_region,
    int ladder_identifier,
    int max_new_waypoints)
{
    if (max_new_waypoints <= 0) {
        return 0;
    }

    const float step = std::max(kWaypointGenerateProbeStepDistance, kWaypointLinkRadiusEpsilon);
    const auto offsets = build_ladder_seed_offsets_center_out(climb_region, step);
    int created_waypoints = 0;

    // Always seed the center of the climb region first, then expand outward.
    if (created_waypoints < max_new_waypoints
        && climb_region_point_has_edge_clearance(
            climb_region, climb_region.pos, kWaypointGenerateLadderEdgeClearance)) {
        rf::Vector3 center_query = climb_region.pos;
        if (rf::level_point_in_climb_region(&center_query) == &climb_region
            && find_nearest_waypoint(climb_region.pos, kWaypointLinkRadiusEpsilon, 0) <= 0) {
            add_waypoint(
                climb_region.pos,
                WaypointType::ladder,
                static_cast<int>(WaypointDroppedSubtype::normal),
                false,
                true,
                kWaypointLinkRadius,
                ladder_identifier,
                nullptr,
                true);
            ++created_waypoints;
        }
    }

    for (const auto& local_offset : offsets) {
        if (created_waypoints >= max_new_waypoints) {
            break;
        }

        if (local_offset.len_sq() <= (kWaypointLinkRadiusEpsilon * kWaypointLinkRadiusEpsilon)) {
            continue;
        }

        const rf::Vector3 candidate_pos = climb_region.pos + climb_region.orient.transform_vector(local_offset);
        if (!climb_region_point_has_edge_clearance(
                climb_region, candidate_pos, kWaypointGenerateLadderEdgeClearance)) {
            continue;
        }

        rf::Vector3 candidate_query = candidate_pos;
        if (rf::level_point_in_climb_region(&candidate_query) != &climb_region) {
            continue;
        }

        if (find_nearest_waypoint(candidate_pos, step, 0) > 0) {
            continue;
        }

        add_waypoint(
            candidate_pos,
            WaypointType::ladder,
            static_cast<int>(WaypointDroppedSubtype::normal),
            false,
            true,
            kWaypointLinkRadius,
            ladder_identifier,
            nullptr,
            true);
        ++created_waypoints;
    }

    return created_waypoints;
}

int seed_ladder_waypoints_for_autogen(int max_new_waypoints)
{
    if (max_new_waypoints <= 0 || rf::level.ladders.empty()) {
        return 0;
    }

    int created_waypoints = 0;
    std::unordered_set<const rf::ClimbRegion*> seen_regions{};
    seen_regions.reserve(rf::level.ladders.size());

    for (int i = 0; i < rf::level.ladders.size() && created_waypoints < max_new_waypoints; ++i) {
        const rf::ClimbRegion* climb_region = rf::level.ladders[i];
        if (!climb_region || !seen_regions.insert(climb_region).second) {
            continue;
        }

        const int ladder_identifier = allocate_new_ladder_identifier();
        created_waypoints += seed_climb_region_waypoints_for_autogen(
            *climb_region,
            ladder_identifier,
            max_new_waypoints - created_waypoints);
    }

    return created_waypoints;
}

int find_most_central_waypoint_index(const std::vector<int>& waypoint_indices)
{
    float center_x = 0.0f;
    float center_y = 0.0f;
    float center_z = 0.0f;
    int valid_count = 0;

    for (int waypoint_index : waypoint_indices) {
        if (waypoint_index <= 0 || waypoint_index >= static_cast<int>(g_waypoints.size())) {
            continue;
        }
        const auto& node = g_waypoints[waypoint_index];
        if (!node.valid) {
            continue;
        }
        center_x += node.pos.x;
        center_y += node.pos.y;
        center_z += node.pos.z;
        ++valid_count;
    }

    if (valid_count <= 0) {
        return 0;
    }

    const rf::Vector3 center{
        center_x / static_cast<float>(valid_count),
        center_y / static_cast<float>(valid_count),
        center_z / static_cast<float>(valid_count),
    };

    int best_index = 0;
    float best_dist_sq = std::numeric_limits<float>::max();
    for (int waypoint_index : waypoint_indices) {
        if (waypoint_index <= 0 || waypoint_index >= static_cast<int>(g_waypoints.size())) {
            continue;
        }
        const auto& node = g_waypoints[waypoint_index];
        if (!node.valid) {
            continue;
        }
        const float dist_sq = distance_sq(node.pos, center);
        if (dist_sq < best_dist_sq) {
            best_dist_sq = dist_sq;
            best_index = waypoint_index;
        }
    }

    return best_index;
}

int find_matching_lift_waypoint_near(
    const rf::Vector3& pos,
    WaypointType type,
    int lift_uid,
    float radius)
{
    const float radius_sq = radius * radius;
    for (int i = 1; i < static_cast<int>(g_waypoints.size()); ++i) {
        const auto& node = g_waypoints[i];
        if (!node.valid || node.type != type || node.identifier != lift_uid) {
            continue;
        }
        if (distance_sq(node.pos, pos) <= radius_sq) {
            return i;
        }
    }
    return 0;
}

void link_lift_exit_outbound_to_nearby_waypoints(int lift_exit_index)
{
    if (lift_exit_index <= 0 || lift_exit_index >= static_cast<int>(g_waypoints.size())) {
        return;
    }
    const auto& lift_exit = g_waypoints[lift_exit_index];
    if (!lift_exit.valid || lift_exit.type != WaypointType::lift_exit) {
        return;
    }

    const float link_radius = sanitize_waypoint_link_radius(lift_exit.link_radius);
    const float link_radius_sq = link_radius * link_radius;
    for (int i = 1; i < static_cast<int>(g_waypoints.size()); ++i) {
        if (i == lift_exit_index) {
            continue;
        }

        const auto& node = g_waypoints[i];
        if (!node.valid) {
            continue;
        }
        if (distance_sq(lift_exit.pos, node.pos) > link_radius_sq) {
            continue;
        }

        link_waypoint_if_clear_autogen(lift_exit_index, i);
    }
}

int generate_lift_path_waypoints_for_autogen(
    const std::unordered_map<int, std::vector<int>>& lift_entrances_by_uid,
    int max_new_waypoints)
{
    if (max_new_waypoints <= 0 || lift_entrances_by_uid.empty()) {
        return 0;
    }

    int created_waypoints = 0;
    const float step_distance = std::max(kWaypointRadius, kWaypointLinkRadiusEpsilon);
    const float duplicate_radius = kWaypointRadius * 0.5f;

    for (const auto& [lift_uid, entrance_indices] : lift_entrances_by_uid) {
        if (created_waypoints >= max_new_waypoints) {
            break;
        }
        if (entrance_indices.empty()) {
            continue;
        }

        const int central_entrance = find_most_central_waypoint_index(entrance_indices);
        if (central_entrance <= 0 || central_entrance >= static_cast<int>(g_waypoints.size())) {
            continue;
        }

        const auto& entrance_node = g_waypoints[central_entrance];
        if (!entrance_node.valid || entrance_node.type != WaypointType::lift_entrance) {
            continue;
        }

        rf::Mover* mover = find_mover_by_uid(lift_uid);
        if (!mover) {
            continue;
        }

        rf::Vector3 lift_delta{};
        if (!get_mover_lift_path_delta(*mover, lift_delta)) {
            continue;
        }

        const rf::Vector3 start_pos = entrance_node.pos;
        const rf::Vector3 end_pos = start_pos + lift_delta;
        const rf::Vector3 travel_delta = end_pos - start_pos;
        const float travel_dist = travel_delta.len();
        if (travel_dist <= kWaypointLinkRadiusEpsilon) {
            continue;
        }

        int step_count = std::max(1, static_cast<int>(std::ceil(travel_dist / step_distance)));
        step_count = std::min(step_count, max_new_waypoints - created_waypoints);
        if (step_count <= 0) {
            continue;
        }

        int previous_index = central_entrance;
        for (int step = 1; step <= step_count; ++step) {
            const bool is_exit_step = (step == step_count);
            const float t = is_exit_step
                ? 1.0f
                : std::clamp((static_cast<float>(step) * step_distance) / travel_dist, 0.0f, 1.0f);
            const rf::Vector3 waypoint_pos = start_pos + travel_delta * t;
            const WaypointType waypoint_type = is_exit_step ? WaypointType::lift_exit : WaypointType::lift_body;

            int waypoint_index = find_matching_lift_waypoint_near(
                waypoint_pos, waypoint_type, lift_uid, duplicate_radius);
            if (waypoint_index <= 0) {
                waypoint_index = add_waypoint(
                    waypoint_pos,
                    waypoint_type,
                    static_cast<int>(WaypointDroppedSubtype::normal),
                    false,
                    true,
                    kWaypointLinkRadius,
                    lift_uid,
                    nullptr,
                    true);
                ++created_waypoints;
            }

            if (previous_index > 0 && previous_index != waypoint_index) {
                link_waypoint_if_clear_autogen(previous_index, waypoint_index);
                link_waypoint_if_clear_autogen(waypoint_index, previous_index);
            }

            if (is_exit_step) {
                link_lift_exit_outbound_to_nearby_waypoints(waypoint_index);
            }

            previous_index = waypoint_index;
        }
    }

    return created_waypoints;
}

int generate_waypoints_from_seed_probes(const std::vector<int>& seed_indices)
{
    if (seed_indices.empty()) {
        return 0;
    }

    std::deque<int> probe_frontier{};
    probe_frontier.insert(probe_frontier.end(), seed_indices.begin(), seed_indices.end());
    std::unordered_set<int> expanded_indices{};
    expanded_indices.reserve(seed_indices.size() * 2);
    std::unordered_map<int, std::vector<int>> lift_entrances_by_uid{};
    int created_waypoints = seed_ladder_waypoints_for_autogen(kWaypointGenerateMaxCreatedWaypoints);
    constexpr float kDegToRad = 0.01745329252f;
    const int direction_count = std::max(
        1,
        static_cast<int>(std::round(360.0f / std::max(kWaypointGenerateProbeAngleStepDeg, 1.0f))));

    while (!probe_frontier.empty() && created_waypoints < kWaypointGenerateMaxCreatedWaypoints) {
        const int source_index = probe_frontier.front();
        probe_frontier.pop_front();

        if (source_index <= 0 || source_index >= static_cast<int>(g_waypoints.size())) {
            continue;
        }
        if (!expanded_indices.insert(source_index).second) {
            continue;
        }

        const auto& source_node = g_waypoints[source_index];
        if (!source_node.valid) {
            continue;
        }
        if (source_node.type == WaypointType::lift_entrance && source_node.identifier >= 0) {
            add_lift_entrance_generation_candidate(
                lift_entrances_by_uid, source_node.identifier, source_index);
        }
        const rf::Vector3 source_pos = source_node.pos;

        for (int direction_idx = 0; direction_idx < direction_count; ++direction_idx) {
            const float angle_deg = static_cast<float>(direction_idx) * kWaypointGenerateProbeAngleStepDeg;
            const float angle_rad = angle_deg * kDegToRad;
            const rf::Vector3 dir{
                std::cos(angle_rad),
                0.0f,
                std::sin(angle_rad),
            };
            const rf::Vector3 probe_pos = source_pos + dir * kWaypointGenerateProbeStepDistance;

            rf::Vector3 floor_pos{};
            if (!trace_ground_below_point(probe_pos, kBridgeWaypointMaxGroundDistance, &floor_pos)) {
                continue;
            }
            const float upward_clearance = trace_upward_clearance_from_floor_hit(
                floor_pos,
                kWaypointGenerateStandingHeadroom
            );
            if (upward_clearance <= kWaypointGenerateMinHeadroom) {
                continue;
            }

            rf::Vector3 candidate_pos = floor_pos + rf::Vector3{0.0f, kWaypointGenerateGroundOffset, 0.0f};
            if (!waypoint_link_within_incline(source_pos, candidate_pos, kWaypointGenerateMaxInclineDeg)) {
                continue;
            }
            if (!can_link_waypoints(source_pos, candidate_pos)) {
                continue;
            }
            if (!waypoint_has_horizontal_geometry_clearance(candidate_pos, kWaypointGenerateWallClearance)) {
                continue;
            }
            if (!waypoint_has_ground_edge_clearance(candidate_pos, kWaypointGenerateEdgeClearance)) {
                continue;
            }

            if (const int closest_special = find_special_waypoint_in_radius(candidate_pos);
                closest_special > 0 && closest_special != source_index) {
                link_waypoint_if_clear_autogen(source_index, closest_special);
                continue;
            }

            if (closest_waypoint(candidate_pos, kWaypointRadius) > 0) {
                continue;
            }

            WaypointType generated_type = WaypointType::std_new;
            int generated_subtype = 0;
            int generated_movement_subtype = static_cast<int>(WaypointDroppedSubtype::normal);
            int generated_identifier = -1;
            rf::Vector3 candidate_climb_query = candidate_pos;
            if (rf::level_point_in_climb_region(&candidate_climb_query)) {
                continue;
            }
            if (upward_clearance < kWaypointGenerateStandingHeadroom) {
                generated_movement_subtype = static_cast<int>(WaypointDroppedSubtype::crouch_needed);
            }
            if (const int lift_uid_below = find_lift_uid_below_waypoint(candidate_pos); lift_uid_below >= 0) {
                generated_type = WaypointType::lift_entrance;
                generated_identifier = lift_uid_below;
                generated_movement_subtype = static_cast<int>(WaypointDroppedSubtype::normal);
            }

            const int waypoint_index = add_waypoint(
                candidate_pos,
                generated_type,
                generated_subtype,
                false,
                true,
                kWaypointLinkRadius,
                generated_identifier,
                nullptr,
                true,
                generated_movement_subtype);
            if (generated_type == WaypointType::lift_entrance && generated_identifier >= 0) {
                add_lift_entrance_generation_candidate(
                    lift_entrances_by_uid, generated_identifier, waypoint_index);
            }
            probe_frontier.push_back(waypoint_index);
            ++created_waypoints;

            if (created_waypoints >= kWaypointGenerateMaxCreatedWaypoints) {
                break;
            }
        }
    }

    if (created_waypoints < kWaypointGenerateMaxCreatedWaypoints) {
        created_waypoints += generate_lift_path_waypoints_for_autogen(
            lift_entrances_by_uid,
            kWaypointGenerateMaxCreatedWaypoints - created_waypoints);
    }

    return created_waypoints;
}

bool link_waypoint_if_clear_no_replace(int from, int to)
{
    if (!can_link_waypoint_indices_autogen(from, to)) {
        return false;
    }
    if (!autogen_directional_link_allowed(from, to)) {
        return false;
    }

    auto& node = g_waypoints[from];
    if (waypoint_link_exists(node, to)) {
        return false;
    }
    for (int link_idx = 0; link_idx < node.num_links; ++link_idx) {
        const int intermediate = node.links[link_idx];
        if (intermediate <= 0 || intermediate >= static_cast<int>(g_waypoints.size()) || intermediate == to) {
            continue;
        }
        const auto& intermediate_node = g_waypoints[intermediate];
        if (!intermediate_node.valid) {
            continue;
        }
        if (waypoint_link_exists(intermediate_node, to)) {
            return false;
        }
    }
    if (node.num_links >= kMaxWaypointLinks) {
        return false;
    }

    node.links[node.num_links++] = to;
    return true;
}

bool add_waypoint_link_no_replace(int from, int to)
{
    if (!can_link_waypoint_indices_autogen(from, to)) {
        return false;
    }
    if (!autogen_directional_link_allowed(from, to)) {
        return false;
    }

    auto& node = g_waypoints[from];
    if (waypoint_link_exists(node, to)) {
        return false;
    }
    if (node.num_links >= kMaxWaypointLinks) {
        return false;
    }

    node.links[node.num_links++] = to;
    return true;
}

std::optional<int> find_waypoint_intersecting_link_segment(
    int from, int to, const WaypointCellMap& cell_map, float cell_size)
{
    if (from <= 0 || to <= 0 || from == to
        || from >= static_cast<int>(g_waypoints.size())
        || to >= static_cast<int>(g_waypoints.size())) {
        return std::nullopt;
    }

    const auto& from_node = g_waypoints[from];
    const auto& to_node = g_waypoints[to];
    if (!from_node.valid || !to_node.valid) {
        return std::nullopt;
    }

    const rf::Vector3 a = from_node.pos;
    const rf::Vector3 c = to_node.pos;
    const rf::Vector3 segment = c - a;
    const float segment_len_sq = segment.dot_prod(segment);
    if (segment_len_sq <= kWaypointLinkRadiusEpsilon * kWaypointLinkRadiusEpsilon) {
        return std::nullopt;
    }
    const float segment_len = std::sqrt(segment_len_sq);
    if (segment_len <= (2.0f * kWaypointGenerateLinkPassThroughRadius + kWaypointLinkRadiusEpsilon)) {
        return std::nullopt;
    }

    const float pass_radius_sq = kWaypointGenerateLinkPassThroughRadius * kWaypointGenerateLinkPassThroughRadius;
    const rf::Vector3 pass_padding{
        kWaypointGenerateLinkPassThroughRadius,
        kWaypointGenerateLinkPassThroughRadius,
        kWaypointGenerateLinkPassThroughRadius,
    };
    const rf::Vector3 bounds_min = point_min(a, c) - pass_padding;
    const rf::Vector3 bounds_max = point_max(a, c) + pass_padding;
    int best_index = -1;
    float best_dist_sq = std::numeric_limits<float>::max();

    const auto min_cell = waypoint_cell_coord_from_pos(bounds_min, cell_size);
    const auto max_cell = waypoint_cell_coord_from_pos(bounds_max, cell_size);

    for (int x = min_cell.x; x <= max_cell.x; ++x) {
        for (int y = min_cell.y; y <= max_cell.y; ++y) {
            for (int z = min_cell.z; z <= max_cell.z; ++z) {
                const WaypointCellCoord coord{x, y, z};
                auto it = cell_map.find(coord);
                if (it == cell_map.end()) {
                    continue;
                }
                for (int i : it->second) {
                    if (i == from || i == to) {
                        continue;
                    }

                    const auto& candidate = g_waypoints[i];
                    if (!candidate.valid) {
                        continue;
                    }
                    if (candidate.pos.x < bounds_min.x || candidate.pos.x > bounds_max.x
                        || candidate.pos.y < bounds_min.y || candidate.pos.y > bounds_max.y
                        || candidate.pos.z < bounds_min.z || candidate.pos.z > bounds_max.z) {
                        continue;
                    }

                    const rf::Vector3 ac_to_candidate = candidate.pos - a;
                    const float t = ac_to_candidate.dot_prod(segment) / segment_len_sq;
                    if (t <= kWaypointGeneratePassThroughEndpointEpsilon
                        || t >= (1.0f - kWaypointGeneratePassThroughEndpointEpsilon)) {
                        continue;
                    }

                    const rf::Vector3 closest_on_segment = a + segment * t;
                    const float dist_sq_to_segment = distance_sq(candidate.pos, closest_on_segment);
                    if (dist_sq_to_segment > pass_radius_sq) {
                        continue;
                    }

                    if (dist_sq_to_segment < best_dist_sq) {
                        best_dist_sq = dist_sq_to_segment;
                        best_index = i;
                    }
                }
            }
        }
    }

    if (best_index <= 0) {
        return std::nullopt;
    }

    return best_index;
}

bool remove_waypoint_link_no_replace(int from, int to)
{
    if (from <= 0 || from >= static_cast<int>(g_waypoints.size())) {
        return false;
    }
    auto& node = g_waypoints[from];
    if (!node.valid) {
        return false;
    }

    for (int i = 0; i < node.num_links; ++i) {
        if (node.links[i] != to) {
            continue;
        }
        for (int j = i + 1; j < node.num_links; ++j) {
            node.links[j - 1] = node.links[j];
        }
        --node.num_links;
        if (node.num_links >= 0 && node.num_links < kMaxWaypointLinks) {
            node.links[node.num_links] = 0;
        }
        return true;
    }
    return false;
}

int prune_redundant_generated_links()
{
    int removed_links = 0;
    const int waypoint_count = static_cast<int>(g_waypoints.size());

    for (int from = 1; from < waypoint_count; ++from) {
        auto& from_node = g_waypoints[from];
        if (!from_node.valid) {
            continue;
        }

        bool changed = true;
        while (changed) {
            changed = false;
            for (int i = 0; i < from_node.num_links; ++i) {
                const int to = from_node.links[i];
                if (to <= 0 || to >= waypoint_count || !g_waypoints[to].valid) {
                    continue;
                }

                bool redundant = false;
                for (int j = 0; j < from_node.num_links; ++j) {
                    const int intermediate = from_node.links[j];
                    if (intermediate == to
                        || intermediate <= 0
                        || intermediate >= waypoint_count
                        || !g_waypoints[intermediate].valid) {
                        continue;
                    }
                    if (waypoint_has_link_to(intermediate, to)) {
                        redundant = true;
                        break;
                    }
                }

                if (!redundant) {
                    continue;
                }

                if (remove_waypoint_link_no_replace(from, to)) {
                    ++removed_links;
                    changed = true;
                }
                break;
            }
        }
    }

    return removed_links;
}

int reroute_links_through_intermediate_waypoints(const WaypointCellMap& cell_map, float cell_size)
{
    int rerouted_links = 0;
    const int waypoint_count = static_cast<int>(g_waypoints.size());

    for (int from = 1; from < waypoint_count; ++from) {
        auto& from_node = g_waypoints[from];
        if (!from_node.valid) {
            continue;
        }

        const int original_link_count = from_node.num_links;
        std::array<int, kMaxWaypointLinks> original_links{};
        for (int idx = 0; idx < original_link_count; ++idx) {
            original_links[idx] = from_node.links[idx];
        }

        for (int link_idx = 0; link_idx < original_link_count; ++link_idx) {
            const int to = original_links[link_idx];
            if (to <= 0 || to >= waypoint_count || !g_waypoints[to].valid) {
                continue;
            }
            if (!waypoint_has_link_to(from, to)) {
                continue;
            }

            auto through_opt = find_waypoint_intersecting_link_segment(
                from,
                to,
                cell_map,
                cell_size
            );
            if (!through_opt) {
                continue;
            }

            const int through = through_opt.value();
            const bool need_from_to_through = !waypoint_has_link_to(from, through);
            const bool need_through_to_to = !waypoint_has_link_to(through, to);

            // Reroute only if it will truly break the direct pass-through edge.
            if (!need_from_to_through && !need_through_to_to) {
                if (remove_waypoint_link_no_replace(from, to)) {
                    ++rerouted_links;
                }
                continue;
            }

            if (!remove_waypoint_link_no_replace(from, to)) {
                continue;
            }

            bool reroute_ok = true;
            if (need_from_to_through && !add_waypoint_link_no_replace(from, through)) {
                reroute_ok = false;
            }
            if (reroute_ok && need_through_to_to && !add_waypoint_link_no_replace(through, to)) {
                reroute_ok = false;
            }

            if (!reroute_ok) {
                // Keep direct pass-through edges removed even if full reroute is not possible.
                continue;
            }

            ++rerouted_links;
        }
    }

    return rerouted_links;
}

struct GeneratedWaypointLinkStats
{
    int bidirectional_links = 0;
    int downward_links = 0;
    int pass_through_links_rerouted = 0;
    int redundant_links_pruned = 0;
};

GeneratedWaypointLinkStats link_generated_waypoint_grid()
{
    GeneratedWaypointLinkStats stats{};
    const float link_radius_sq = kWaypointLinkRadius * kWaypointLinkRadius;
    const int waypoint_count = static_cast<int>(g_waypoints.size());
    WaypointCellMap cell_map{};
    build_waypoint_cell_map(cell_map, kWaypointLinkRadius);

    for (int i = 1; i < waypoint_count; ++i) {
        const auto& node_a = g_waypoints[i];
        if (!node_a.valid) {
            continue;
        }

        const WaypointCellCoord cell = waypoint_cell_coord_from_pos(node_a.pos, kWaypointLinkRadius);
        for (int dx = -1; dx <= 1; ++dx) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dz = -1; dz <= 1; ++dz) {
                    const WaypointCellCoord neighbor_cell{cell.x + dx, cell.y + dy, cell.z + dz};
                    auto it = cell_map.find(neighbor_cell);
                    if (it == cell_map.end()) {
                        continue;
                    }

                    for (int j : it->second) {
                        if (j <= i) {
                            continue;
                        }
                        const auto& node_b = g_waypoints[j];
                        if (!node_b.valid) {
                            continue;
                        }

                        if (distance_sq(node_a.pos, node_b.pos) > link_radius_sq) {
                            continue;
                        }
                        if (!can_link_waypoints(node_a.pos, node_b.pos)) {
                            continue;
                        }

                        const auto through_opt =
                            find_waypoint_intersecting_link_segment(i, j, cell_map, kWaypointLinkRadius);
                        if (through_opt) {
                            const int through = through_opt.value();
                            auto link_split = [&](const int from, const int to, int& stat_counter) {
                                bool split_added = false;
                                if (link_waypoint_if_clear_no_replace(from, through)) {
                                    split_added = true;
                                }
                                if (link_waypoint_if_clear_no_replace(through, to)) {
                                    split_added = true;
                                }
                                if (split_added) {
                                    ++stat_counter;
                                }
                            };

                            if (waypoint_link_within_incline(node_a.pos, node_b.pos, kWaypointGenerateMaxInclineDeg)) {
                                link_split(i, j, stats.bidirectional_links);
                                link_split(j, i, stats.bidirectional_links);
                            }
                            else if (node_a.pos.y > node_b.pos.y) {
                                link_split(i, j, stats.downward_links);
                            }
                            else if (node_b.pos.y > node_a.pos.y) {
                                link_split(j, i, stats.downward_links);
                            }
                            continue;
                        }

                        const bool ladder_pair =
                            node_a.type == WaypointType::ladder && node_b.type == WaypointType::ladder;
                        if (ladder_pair) {
                            if (link_waypoint_if_clear_no_replace(i, j)) {
                                ++stats.bidirectional_links;
                            }
                            if (link_waypoint_if_clear_no_replace(j, i)) {
                                ++stats.bidirectional_links;
                            }
                            continue;
                        }

                        if (waypoint_link_within_incline(node_a.pos, node_b.pos, kWaypointGenerateMaxInclineDeg)) {
                            if (link_waypoint_if_clear_no_replace(i, j)) {
                                ++stats.bidirectional_links;
                            }
                            if (link_waypoint_if_clear_no_replace(j, i)) {
                                ++stats.bidirectional_links;
                            }
                        }
                        else if (node_a.pos.y > node_b.pos.y) {
                            if (link_waypoint_if_clear_no_replace(i, j)) {
                                ++stats.downward_links;
                            }
                        }
                        else if (node_b.pos.y > node_a.pos.y) {
                            if (link_waypoint_if_clear_no_replace(j, i)) {
                                ++stats.downward_links;
                            }
                        }
                    }
                }
            }
        }
    }

    stats.pass_through_links_rerouted = reroute_links_through_intermediate_waypoints(cell_map, kWaypointLinkRadius);
    stats.redundant_links_pruned = prune_redundant_generated_links();
    return stats;
}

bool trace_segment_hits_detail_brush(const rf::Vector3& from, const rf::Vector3& to)
{
    rf::Vector3 segment = to - from;
    const float segment_length = segment.len();
    if (segment_length <= kWaypointLinkRadiusEpsilon) {
        return false;
    }
    segment.normalize_safe();

    constexpr int kMaxTraceIterations = 64;
    constexpr float kTraceAdvanceEpsilon = 0.05f;
    constexpr float kMinAdvanceDelta = 0.001f;

    rf::Vector3 trace_start = from;
    float advanced_distance = 0.0f;
    for (int iteration = 0;
         iteration < kMaxTraceIterations
         && advanced_distance + kTraceAdvanceEpsilon < segment_length;
         ++iteration) {
        rf::Vector3 p0 = trace_start;
        rf::Vector3 p1 = to;
        rf::GCollisionOutput collision{};
        if (!rf::collide_linesegment_level_solid(
                p0,
                p1,
                kWaypointSolidTraceFlags,
                &collision)) {
            return false;
        }

        if (collision.face && collision.face->which_room && collision.face->which_room->is_detail) {
            return true;
        }

        const float hit_distance_from_start = (collision.hit_point - from).len();
        const float next_advanced_distance = std::clamp(
            hit_distance_from_start + kTraceAdvanceEpsilon,
            0.0f,
            segment_length);
        if (next_advanced_distance <= advanced_distance + kMinAdvanceDelta) {
            break;
        }

        advanced_distance = next_advanced_distance;
        trace_start = from + segment * advanced_distance;
    }

    return false;
}

bool compute_blocked_link_wall_midpoint(
    const rf::Vector3& from,
    const rf::Vector3& to,
    rf::Vector3& out_midpoint)
{
    rf::Vector3 p0 = from;
    rf::Vector3 p1 = to;
    rf::GCollisionOutput forward_collision{};
    if (!rf::collide_linesegment_level_solid(
            p0,
            p1,
            kWaypointSolidTraceFlags,
            &forward_collision)) {
        return false;
    }

    p0 = to;
    p1 = from;
    rf::GCollisionOutput backward_collision{};
    if (!rf::collide_linesegment_level_solid(
            p0,
            p1,
            kWaypointSolidTraceFlags,
            &backward_collision)) {
        return false;
    }

    out_midpoint = (forward_collision.hit_point + backward_collision.hit_point) * 0.5f;
    return true;
}

rf::GRoom* collision_breakable_glass_room(const rf::GCollisionOutput& collision)
{
    if (!collision.face || !collision.face->which_room) {
        return nullptr;
    }

    auto* room = collision.face->which_room;
    if (!room->is_breakable_glass()) {
        return nullptr;
    }
    return room;
}

int breakable_glass_room_key(const rf::GRoom& room)
{
    if (room.uid >= 0) {
        return room.uid;
    }
    return -(room.room_index + 1);
}

void waypoints_on_glass_shattered(const rf::GFace* face)
{
    if (!face || !face->which_room || g_waypoint_targets.empty()) {
        return;
    }

    auto* room = face->which_room;
    if (!room->is_breakable_glass()) {
        return;
    }

    const int room_key = breakable_glass_room_key(*room);
    const auto try_link_shatter_target_waypoints = [](const WaypointTargetDefinition& target) {
        const auto force_link_direction = [](int from_uid, int to_uid) {
            if (from_uid <= 0 || to_uid <= 0
                || from_uid >= static_cast<int>(g_waypoints.size())
                || to_uid >= static_cast<int>(g_waypoints.size())) {
                return;
            }

            auto& from_node = g_waypoints[from_uid];
            const auto& to_node = g_waypoints[to_uid];
            if (!from_node.valid || !to_node.valid || from_uid == to_uid) {
                return;
            }
            if (waypoint_link_exists(from_node, to_uid)) {
                return;
            }

            if (from_node.num_links < kMaxWaypointLinks) {
                from_node.links[from_node.num_links++] = to_uid;
                return;
            }

            std::uniform_int_distribution<int> dist(0, kMaxWaypointLinks - 1);
            from_node.links[dist(g_rng)] = to_uid;
        };

        const auto try_link_direction = [&force_link_direction](int from_uid, int to_uid) {
            if (from_uid <= 0 || to_uid <= 0
                || from_uid >= static_cast<int>(g_waypoints.size())
                || to_uid >= static_cast<int>(g_waypoints.size())) {
                return;
            }
            const auto& from_node = g_waypoints[from_uid];
            const auto& to_node = g_waypoints[to_uid];
            if (!from_node.valid || !to_node.valid) {
                return;
            }
            if (!waypoint_upward_link_allowed(from_node.pos, to_node.pos, kWaypointGenerateMaxInclineDeg)) {
                return;
            }
            // Do not require LOS here: target creation already validated this bridge candidate.
            force_link_direction(from_uid, to_uid);
        };

        const auto& waypoint_uids = target.waypoint_uids;
        if (waypoint_uids.size() < 2) {
            return;
        }

        for (size_t i = 0; i < waypoint_uids.size(); ++i) {
            for (size_t j = i + 1; j < waypoint_uids.size(); ++j) {
                const int uid_a = waypoint_uids[i];
                const int uid_b = waypoint_uids[j];
                try_link_direction(uid_a, uid_b);
                try_link_direction(uid_b, uid_a);
            }
        }
    };

    for (const auto& target : g_waypoint_targets) {
        if (target.type != WaypointTargetType::shatter || target.identifier != room_key) {
            continue;
        }
        try_link_shatter_target_waypoints(target);
    }

    g_waypoint_targets.erase(
        std::remove_if(
            g_waypoint_targets.begin(),
            g_waypoint_targets.end(),
            [room_key](const WaypointTargetDefinition& target) {
                return target.type == WaypointTargetType::shatter
                    && target.identifier == room_key;
            }),
        g_waypoint_targets.end());
}

bool compute_blocked_link_breakable_glass_midpoint(
    const rf::Vector3& from,
    const rf::Vector3& to,
    rf::Vector3& out_midpoint,
    int& out_room_key)
{
    rf::Vector3 p0 = from;
    rf::Vector3 p1 = to;
    rf::GCollisionOutput forward_collision{};
    if (!rf::collide_linesegment_level_solid(
            p0,
            p1,
            kWaypointSolidTraceFlags,
            &forward_collision)) {
        return false;
    }
    const auto* forward_room = collision_breakable_glass_room(forward_collision);
    if (!forward_room) {
        return false;
    }

    p0 = to;
    p1 = from;
    rf::GCollisionOutput backward_collision{};
    if (!rf::collide_linesegment_level_solid(
            p0,
            p1,
            kWaypointSolidTraceFlags,
            &backward_collision)) {
        return false;
    }
    const auto* backward_room = collision_breakable_glass_room(backward_collision);
    if (!backward_room) {
        return false;
    }

    const int forward_room_key = breakable_glass_room_key(*forward_room);
    const int backward_room_key = breakable_glass_room_key(*backward_room);
    if (forward_room_key != backward_room_key) {
        return false;
    }

    out_midpoint = (forward_collision.hit_point + backward_collision.hit_point) * 0.5f;
    out_room_key = forward_room_key;
    return true;
}

bool point_still_inside_breakable_glass_brush(
    const rf::Vector3& point,
    const rf::Vector3& from,
    const rf::Vector3& to,
    int room_key)
{
    const auto hits_same_brush = [room_key, &point](const rf::Vector3& endpoint) {
        rf::Vector3 p0 = point;
        rf::Vector3 p1 = endpoint;
        rf::GCollisionOutput collision{};
        if (!rf::collide_linesegment_level_solid(
                p0,
                p1,
                kWaypointSolidTraceFlags,
                &collision)) {
            return false;
        }
        const auto* room = collision_breakable_glass_room(collision);
        if (!room) {
            return false;
        }
        return room_key == breakable_glass_room_key(*room);
    };

    return hits_same_brush(from) && hits_same_brush(to);
}

bool point_matches_autogen_explosion_target_hardness_rules(const rf::Vector3& point)
{
    const int level_hardness = std::clamp(rf::level.default_rock_hardness, 0, 100);
    bool has_region_hardness_gte_75 = false;
    bool has_region_hardness_lte_50 = false;

    for (int region_index = 0; region_index < rf::level.regions.size(); ++region_index) {
        auto* region = rf::level.regions[region_index];
        if (!region) {
            continue;
        }

        rf::Vector3 query_point = point;
        if (!rf::level_geo_region_contains(&query_point, region)) {
            continue;
        }

        has_region_hardness_gte_75 = has_region_hardness_gte_75 || (region->hardness >= 75);
        has_region_hardness_lte_50 = has_region_hardness_lte_50 || (region->hardness <= 50);
        if (has_region_hardness_gte_75 && has_region_hardness_lte_50) {
            break;
        }
    }

    bool allowed = false;
    if (level_hardness <= 50) {
        allowed = allowed || !has_region_hardness_gte_75;
    }
    if (level_hardness >= 50) {
        allowed = allowed || has_region_hardness_lte_50;
    }
    return allowed;
}

bool point_inside_shallow_geo_region(const rf::Vector3& point)
{
    for (int region_index = 0; region_index < rf::level.regions.size(); ++region_index) {
        auto* region = rf::level.regions[region_index];
        if (!region || !region->use_shallow_geomods) {
            continue;
        }

        rf::Vector3 query_point = point;
        if (rf::level_geo_region_contains(&query_point, region)) {
            return true;
        }
    }

    return false;
}

bool waypoint_pair_can_link_if_geometry_removed(int from, int to)
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

    const bool forward_type_allowed = waypoint_link_types_allowed(from_node, to_node);
    const bool backward_type_allowed = waypoint_link_types_allowed(to_node, from_node);
    if (!forward_type_allowed && !backward_type_allowed) {
        return false;
    }

    const bool forward_incline_allowed = waypoint_upward_link_allowed(
        from_node.pos,
        to_node.pos,
        kWaypointGenerateMaxInclineDeg);
    const bool backward_incline_allowed = waypoint_upward_link_allowed(
        to_node.pos,
        from_node.pos,
        kWaypointGenerateMaxInclineDeg);
    if (!(forward_type_allowed && forward_incline_allowed)
        && !(backward_type_allowed && backward_incline_allowed)) {
        return false;
    }

    if (waypoint_has_link_to(from, to) || waypoint_has_link_to(to, from)) {
        return false;
    }

    return !can_link_waypoints(from_node.pos, to_node.pos);
}

int generate_explosion_targets_for_autogen()
{
    constexpr float kMaxPairDistance = kWaypointLinkRadius * 2.0f;
    constexpr float kMaxPairDistanceSq = kMaxPairDistance * kMaxPairDistance;
    constexpr float kTargetSpacing = kWaypointLinkRadius * 2.0f;
    constexpr float kTargetSpacingSq = kTargetSpacing * kTargetSpacing;
    constexpr float kTargetYOffset = 1.0f;

    const int waypoint_count = static_cast<int>(g_waypoints.size());
    if (waypoint_count <= 2) {
        return 0;
    }

    WaypointCellMap cell_map{};
    build_waypoint_cell_map(cell_map, kMaxPairDistance);

    std::vector<rf::Vector3> existing_target_positions{};
    existing_target_positions.reserve(g_waypoint_targets.size() + 64);
    for (const auto& target : g_waypoint_targets) {
        if (target.type == WaypointTargetType::explosion) {
            existing_target_positions.push_back(target.pos);
        }
    }

    struct AutogenExplosionTargetCandidate
    {
        int from_waypoint = 0;
        int to_waypoint = 0;
        float pair_distance_sq = std::numeric_limits<float>::max();
        rf::Vector3 target_pos{};
    };

    std::vector<AutogenExplosionTargetCandidate> candidates{};
    candidates.reserve(1024);

    for (int from = 1; from < waypoint_count; ++from) {
        const auto& from_node = g_waypoints[from];
        if (!from_node.valid) {
            continue;
        }

        const WaypointCellCoord cell = waypoint_cell_coord_from_pos(from_node.pos, kMaxPairDistance);
        for (int dx = -1; dx <= 1; ++dx) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dz = -1; dz <= 1; ++dz) {
                    const WaypointCellCoord neighbor_cell{cell.x + dx, cell.y + dy, cell.z + dz};
                    auto it = cell_map.find(neighbor_cell);
                    if (it == cell_map.end()) {
                        continue;
                    }

                    for (int to : it->second) {
                        if (to <= from) {
                            continue;
                        }

                        const auto& to_node = g_waypoints[to];
                        if (!to_node.valid) {
                            continue;
                        }
                        if (distance_sq(from_node.pos, to_node.pos) > kMaxPairDistanceSq) {
                            continue;
                        }
                        if (!waypoint_pair_can_link_if_geometry_removed(from, to)) {
                            continue;
                        }
                        if (trace_segment_hits_detail_brush(from_node.pos, to_node.pos)) {
                            continue;
                        }

                        rf::Vector3 target_pos{};
                        if (!compute_blocked_link_wall_midpoint(from_node.pos, to_node.pos, target_pos)) {
                            continue;
                        }

                        const rf::Vector3 base_target_pos = target_pos;
                        const bool in_shallow_geo_region = point_inside_shallow_geo_region(base_target_pos);
                        const bool base_target_valid =
                            rf::find_room(rf::level.geometry, &base_target_pos) == nullptr;
                        if (!in_shallow_geo_region) {
                            const rf::Vector3 raised_target_pos =
                                base_target_pos + rf::Vector3{0.0f, kTargetYOffset, 0.0f};
                            const bool raised_target_valid =
                                rf::find_room(rf::level.geometry, &raised_target_pos) == nullptr;
                            if (raised_target_valid) {
                                target_pos = raised_target_pos;
                            }
                            else if (base_target_valid) {
                                target_pos = base_target_pos;
                            }
                            else {
                                continue;
                            }
                        }
                        else if (base_target_valid) {
                            // Shallow geomod regions should use midpoint placement directly.
                            target_pos = base_target_pos;
                        }
                        else {
                            continue;
                        }

                        const rf::Vector3 hardness_query_pos =
                            ((from_node.pos + to_node.pos) * 0.5f) + rf::Vector3{0.0f, kTargetYOffset, 0.0f};
                        if (!point_matches_autogen_explosion_target_hardness_rules(hardness_query_pos)) {
                            continue;
                        }

                        candidates.push_back(
                            {from, to, distance_sq(from_node.pos, to_node.pos), target_pos}
                        );
                    }
                }
            }
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        return a.pair_distance_sq < b.pair_distance_sq;
    });

    int created_targets = 0;
    for (const auto& candidate : candidates) {
        bool too_close_to_existing_target = false;
        for (const auto& existing_pos : existing_target_positions) {
            if (distance_sq(existing_pos, candidate.target_pos) <= kTargetSpacingSq) {
                too_close_to_existing_target = true;
                break;
            }
        }
        if (too_close_to_existing_target) {
            continue;
        }

        add_waypoint_target_with_waypoint_uids(
            candidate.target_pos,
            WaypointTargetType::explosion,
            {candidate.from_waypoint, candidate.to_waypoint});
        existing_target_positions.push_back(candidate.target_pos);
        ++created_targets;
    }

    return created_targets;
}

int generate_shatter_targets_for_autogen()
{
    constexpr float kMaxPairDistance = kWaypointLinkRadius * 2.0f;
    constexpr float kMaxPairDistanceSq = kMaxPairDistance * kMaxPairDistance;
    constexpr float kTargetYOffset = 1.0f;

    const int waypoint_count = static_cast<int>(g_waypoints.size());
    if (waypoint_count <= 2) {
        return 0;
    }

    WaypointCellMap cell_map{};
    build_waypoint_cell_map(cell_map, kMaxPairDistance);

    std::unordered_set<int> existing_glass_room_keys{};
    existing_glass_room_keys.reserve(g_waypoint_targets.size());
    for (const auto& target : g_waypoint_targets) {
        if (target.type == WaypointTargetType::shatter && target.identifier != -1) {
            existing_glass_room_keys.insert(target.identifier);
        }
    }

    struct AutogenShatterTargetCandidate
    {
        int from_waypoint = 0;
        int to_waypoint = 0;
        float pair_distance_sq = std::numeric_limits<float>::max();
        rf::Vector3 target_pos{};
        int glass_room_key = -1;
    };

    std::unordered_map<int, AutogenShatterTargetCandidate> candidate_by_glass_room{};
    candidate_by_glass_room.reserve(128);

    for (int from = 1; from < waypoint_count; ++from) {
        const auto& from_node = g_waypoints[from];
        if (!from_node.valid) {
            continue;
        }

        const WaypointCellCoord cell = waypoint_cell_coord_from_pos(from_node.pos, kMaxPairDistance);
        for (int dx = -1; dx <= 1; ++dx) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dz = -1; dz <= 1; ++dz) {
                    const WaypointCellCoord neighbor_cell{cell.x + dx, cell.y + dy, cell.z + dz};
                    auto it = cell_map.find(neighbor_cell);
                    if (it == cell_map.end()) {
                        continue;
                    }

                    for (int to : it->second) {
                        if (to <= from) {
                            continue;
                        }

                        const auto& to_node = g_waypoints[to];
                        if (!to_node.valid) {
                            continue;
                        }
                        const float pair_distance_sq = distance_sq(from_node.pos, to_node.pos);
                        if (pair_distance_sq > kMaxPairDistanceSq) {
                            continue;
                        }
                        if (!waypoint_pair_can_link_if_geometry_removed(from, to)) {
                            continue;
                        }

                        rf::Vector3 target_pos{};
                        int glass_room_key = -1;
                        if (!compute_blocked_link_breakable_glass_midpoint(
                                from_node.pos,
                                to_node.pos,
                                target_pos,
                                glass_room_key)) {
                            continue;
                        }

                        const rf::Vector3 raised_target_pos =
                            target_pos + rf::Vector3{0.0f, kTargetYOffset, 0.0f};
                        if (point_still_inside_breakable_glass_brush(
                                raised_target_pos,
                                from_node.pos,
                                to_node.pos,
                                glass_room_key)) {
                            target_pos = raised_target_pos;
                        }

                        auto candidate_it = candidate_by_glass_room.find(glass_room_key);
                        if (candidate_it == candidate_by_glass_room.end()
                            || pair_distance_sq < candidate_it->second.pair_distance_sq) {
                            candidate_by_glass_room[glass_room_key] = AutogenShatterTargetCandidate{
                                from,
                                to,
                                pair_distance_sq,
                                target_pos,
                                glass_room_key,
                            };
                        }
                    }
                }
            }
        }
    }

    int created_targets = 0;
    for (const auto& [glass_room_key, candidate] : candidate_by_glass_room) {
        if (existing_glass_room_keys.find(glass_room_key) != existing_glass_room_keys.end()) {
            continue;
        }

        const int target_uid = add_waypoint_target_with_waypoint_uids(
            candidate.target_pos,
            WaypointTargetType::shatter,
            {candidate.from_waypoint, candidate.to_waypoint});
        if (auto* target = find_waypoint_target_by_uid(target_uid)) {
            target->identifier = glass_room_key;
        }
        existing_glass_room_keys.insert(glass_room_key);
        ++created_targets;
    }

    return created_targets;
}

bool save_waypoints()
{
    if (g_waypoints.size() <= 1) {
        return false;
    }

    int persistent_waypoint_count = 0;
    for (int i = 1; i < static_cast<int>(g_waypoints.size()); ++i) {
        const auto& node = g_waypoints[i];
        if (node.valid && !node.temporary) {
            ++persistent_waypoint_count;
        }
    }
    if (persistent_waypoint_count <= 0) {
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
    if (rf::local_player) {
        add_waypoint_author_if_missing(rf::local_player->name.c_str());
    }
    if (g_waypoint_authors.empty()) {
        add_waypoint_author_if_missing("unknown");
    }

    toml::array authors{};
    for (const auto& author_name : g_waypoint_authors) {
        authors.push_back(author_name);
    }

    const int revision = g_waypoint_revision + 1;
    const bool save_compressed = g_waypoints_compress;
    const bool preserve_std_new_types = is_waypoint_bot_mode_active();
    toml::table header{
        {"revision", revision},
        {"awp_ver", kWptVersion},
        {"compressed", save_compressed},
        {"level", std::string{rf::level.filename.c_str()}},
        {"level_checksum", static_cast<int64_t>(rf::level.checksum)},
        {"saved_at", std::string{time_buf}},
        {"author", authors},
    };
    toml::array nodes;
    for (int i = 1; i < static_cast<int>(g_waypoints.size()); ++i) {
        const auto& node = g_waypoints[i];
        if (!node.valid || node.temporary) {
            continue;
        }
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
        int saved_type = waypoint_type_to_save_value(node.type, preserve_std_new_types);
        toml::table entry{
            {"p", pos},
            {"t", saved_type},
            {"s", node.subtype},
            {"l", links},
        };
        if (normalize_waypoint_dropped_subtype(node.movement_subtype)
            != static_cast<int>(WaypointDroppedSubtype::normal)) {
            entry.insert("m", normalize_waypoint_dropped_subtype(node.movement_subtype));
        }
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
            if (waypoint_zone_type_is_bridge(zone.type)
                && std::fabs(zone.duration - 5.0f) > kWaypointLinkRadiusEpsilon) {
                zone_entry.insert("d", zone.duration);
            }
            if (waypoint_zone_type_is_bridge(zone.type)
                && !zone.bridge_waypoint_uids.empty()) {
                toml::array bridge_waypoint_uids{};
                for (const int waypoint_uid : zone.bridge_waypoint_uids) {
                    if (waypoint_uid <= 0 || waypoint_uid >= static_cast<int>(g_waypoints.size())) {
                        continue;
                    }
                    if (!g_waypoints[waypoint_uid].valid) {
                        continue;
                    }
                    bridge_waypoint_uids.push_back(waypoint_uid);
                }
                if (!bridge_waypoint_uids.empty()) {
                    zone_entry.insert("w", bridge_waypoint_uids);
                }
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
            if (target.identifier != -1) {
                target_entry.insert("i", target.identifier);
            }
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

bool load_waypoints(bool include_std_new_waypoints = true)
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

        if (const auto* author_node = header->get("author")) {
            if (const auto author_value = author_node->value<std::string>(); author_value) {
                add_waypoint_author_if_missing(author_value.value());
            }
            else if (const auto* author_array = author_node->as_array()) {
                for (const auto& author_entry : *author_array) {
                    if (const auto author_value = author_entry.value<std::string>(); author_value) {
                        add_waypoint_author_if_missing(author_value.value());
                    }
                }
            }
        }
        else if (const auto* author_array = header->get_as<toml::array>("authors")) {
            for (const auto& author_entry : *author_array) {
                if (const auto author_value = author_entry.value<std::string>(); author_value) {
                    add_waypoint_author_if_missing(author_value.value());
                }
            }
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
    bool skipped_std_new_waypoints = false;
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
        int movement_subtype = static_cast<int>(WaypointDroppedSubtype::normal);
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
        if (const auto* movement_node = node_tbl->get("m")) {
            if (movement_node->is_number()) {
                movement_subtype = static_cast<int>(movement_node->value_or(movement_subtype));
            }
        }
        else if (const auto* movement_node = node_tbl->get("movement")) {
            if (movement_node->is_number()) {
                movement_subtype = static_cast<int>(movement_node->value_or(movement_subtype));
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
        movement_subtype = normalize_waypoint_dropped_subtype(movement_subtype);
        if (waypoint_type_is_standard(type)) {
            subtype = 0;
        }
        int index = add_waypoint(
            wp_pos,
            type,
            subtype,
            false,
            true,
            link_radius,
            identifier,
            nullptr,
            false,
            movement_subtype);
        auto& node = g_waypoints[index];
        if (!include_std_new_waypoints && type == WaypointType::std_new) {
            node.valid = false;
            node.num_links = 0;
            skipped_std_new_waypoints = true;
            continue;
        }
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
            int identifier = -1;
            if (const auto* type_node = target_tbl->get("t"); type_node && type_node->is_number()) {
                raw_type = static_cast<int>(type_node->value_or(raw_type));
            }
            else if (const auto* type_node = target_tbl->get("type"); type_node && type_node->is_number()) {
                raw_type = static_cast<int>(type_node->value_or(raw_type));
            }
            if (const auto* identifier_node = target_tbl->get("i"); identifier_node && identifier_node->is_number()) {
                identifier = static_cast<int>(identifier_node->value_or(identifier));
            }
            else if (const auto* identifier_node = target_tbl->get("id");
                     identifier_node && identifier_node->is_number()) {
                identifier = static_cast<int>(identifier_node->value_or(identifier));
            }
            else if (const auto* identifier_node = target_tbl->get("identifier");
                     identifier_node && identifier_node->is_number()) {
                identifier = static_cast<int>(identifier_node->value_or(identifier));
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
            target.identifier = identifier;
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
    if (skipped_std_new_waypoints) {
        remap_waypoints();
    }
    normalize_all_zone_bridge_waypoint_refs();
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
    for (auto& zone : g_waypoint_zones) {
        if (!waypoint_zone_type_is_bridge(zone.type)) {
            zone.bridge_waypoint_uids.clear();
            continue;
        }

        std::vector<int> remapped_bridge_waypoint_uids{};
        remapped_bridge_waypoint_uids.reserve(zone.bridge_waypoint_uids.size());
        for (int waypoint_uid : zone.bridge_waypoint_uids) {
            if (waypoint_uid <= 0 || waypoint_uid >= static_cast<int>(remap.size())) {
                continue;
            }
            const int remapped_waypoint_uid = remap[waypoint_uid];
            if (remapped_waypoint_uid > 0) {
                remapped_bridge_waypoint_uids.push_back(remapped_waypoint_uid);
            }
        }
        normalize_zone_bridge_waypoint_refs(remapped_bridge_waypoint_uids);
        zone.bridge_waypoint_uids = std::move(remapped_bridge_waypoint_uids);
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
    if (!are_waypoint_commands_enabled_for_local_client()) {
        return false;
    }
    return g_drop_waypoints || g_has_loaded_wpt;
}

bool should_drop()
{
    if (!are_waypoint_commands_enabled_for_local_client()) {
        return false;
    }
    return g_drop_waypoints || !g_has_loaded_wpt;
}

bool should_skip_local_bot_waypoint_drop()
{
    if (!rf::local_player) {
        return false;
    }

    return rf::local_player->is_bot || is_waypoint_bot_mode_active();
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

void clear_drop_trackers_for_entity(int entity_handle)
{
    g_last_drop_waypoint_by_entity.erase(entity_handle);
    g_last_lift_uid_by_entity.erase(entity_handle);
}

void navigate_entity(int entity_handle, rf::Entity& entity, bool allow_drop)
{
    if (entity_handle < 0) {
        return;
    }

    rf::Vector3 pos = get_entity_feet_pos(entity);
    const int closest_standard = closest_waypoint(pos, kWaypointRadius);
    const int closest_special = find_special_waypoint_in_radius(pos);
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
        int movement_subtype = static_cast<int>(WaypointDroppedSubtype::normal);
        if (falling) {
            movement_subtype = static_cast<int>(WaypointDroppedSubtype::falling);
        }
        else if (swimming) {
            movement_subtype = static_cast<int>(WaypointDroppedSubtype::swimming);
        }
        else if (crouching) {
            movement_subtype = static_cast<int>(WaypointDroppedSubtype::crouch_needed);
        }
        int waypoint_subtype = 0;
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
            pos,
            drop_type,
            waypoint_subtype,
            grounded,
            grounded,
            kWaypointLinkRadius,
            identifier,
            &entity,
            true,
            movement_subtype);
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
        const int local_entity_handle = rf::local_player->entity_handle;
        if (local_entity_handle < 0) {
            return;
        }
        if (rf::player_is_dead(rf::local_player) || rf::player_is_dying(rf::local_player)) {
            clear_drop_trackers_for_entity(local_entity_handle);
            prune_drop_trackers(active_entity_handles);
            return;
        }

        auto* entity = rf::entity_from_handle(local_entity_handle);
        if (!entity) {
            clear_drop_trackers_for_entity(local_entity_handle);
            return;
        }
        active_entity_handles.insert(local_entity_handle);
        navigate_entity(local_entity_handle, *entity, true);
        prune_drop_trackers(active_entity_handles);
        return;
    }

    for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
        if (&player == rf::local_player
            || player.entity_handle < 0
            || player.is_bot
            || player.is_browser
            || player.is_spectator
            || player.is_spawn_disabled) {
            continue;
        }

        if (rf::player_is_dead(&player) || rf::player_is_dying(&player)) {
            clear_drop_trackers_for_entity(player.entity_handle);
            continue;
        }

        auto* entity = rf::entity_from_handle(player.entity_handle);
        if (!entity) {
            clear_drop_trackers_for_entity(player.entity_handle);
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
        case WaypointZoneType::bridge_use:
            return {120, 255, 120, 150};
        case WaypointZoneType::bridge_prox:
            return {80, 220, 255, 150};
        default:
            return {200, 200, 200, 150};
    }
}

rf::Color debug_waypoint_target_color(WaypointTargetType type)
{
    switch (type) {
        case WaypointTargetType::explosion:
            return {255, 120, 40, 150};
        case WaypointTargetType::shatter:
            return {120, 220, 255, 150};
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
    bool selected = false;
    rf::Vector3 center{};
    WaypointZoneType type = WaypointZoneType::control_point;
    int uid = -1;
    int identifier = -1;
    rf::Color color{};
};

struct WaypointTargetDebugRenderInfo
{
    bool renderable = false;
    bool selected = false;
    rf::Vector3 center{};
    WaypointTargetType type = WaypointTargetType::explosion;
    int uid = -1;
    int identifier = -1;
    rf::Color color{};
    std::vector<int> waypoint_uids{};
};

void draw_centered_debug_label(float sx, float sy, const char* text, const rf::Color& color)
{
    const auto [text_w, text_h] = rf::gr::get_string_size(text, -1);
    const int x = static_cast<int>(sx) - (text_w / 2);
    const int y = static_cast<int>(sy) - (text_h / 2);
    rf::gr::set_color(color.red, color.green, color.blue, 255);
    rf::gr::string(x, y, text, -1, no_overdraw_2d_text);
}

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
        zone_info.uid = zone_index;
        zone_info.identifier = zone.identifier;
        zone_info.selected = is_waypoint_editor_selected_zone(zone_index);
        zone_info.color = debug_waypoint_zone_color(zone.type);
        if (zone_info.selected) {
            zone_info.color.alpha = 255;
        }

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

        for (int zone_index = 0; zone_index < static_cast<int>(g_waypoint_zones.size()); ++zone_index) {
            const auto& zone = g_waypoint_zones[zone_index];
            if (!waypoint_zone_type_is_bridge(zone.type)) {
                continue;
            }

            const auto& zone_info = zone_infos[zone_index];
            if (!zone_info.renderable) {
                continue;
            }

            for (const int waypoint_uid : zone.bridge_waypoint_uids) {
                if (waypoint_uid <= 0 || waypoint_uid >= static_cast<int>(g_waypoints.size())) {
                    continue;
                }
                const auto& waypoint = g_waypoints[waypoint_uid];
                if (!waypoint.valid) {
                    continue;
                }

                rf::gr::line_arrow(
                    zone_info.center.x, zone_info.center.y, zone_info.center.z,
                    waypoint.pos.x, waypoint.pos.y, waypoint.pos.z,
                    255, 32, 32);
            }
        }
    }

    for (const auto& zone_info : zone_infos) {
        if ((!show_labels && !zone_info.selected) || !zone_info.renderable) {
            continue;
        }

        rf::Vector3 label_pos = zone_info.center;
        label_pos.y += 0.3f;
        rf::gr::Vertex dest{};
        if (!rf::gr::rotate_vertex(&dest, &label_pos)) {
            rf::gr::project_vertex(&dest);
            if (dest.flags & 1) {
                const auto label_sv = waypoint_zone_type_name(zone_info.type);
                char label[96]{};
                std::snprintf(
                    label, sizeof(label), "%.*s (%d : %d)",
                    static_cast<int>(label_sv.size()), label_sv.data(),
                    zone_info.uid, zone_info.identifier);
                draw_centered_debug_label(dest.sx, dest.sy, label, zone_info.color);
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
        info.uid = target.uid;
        info.identifier = target.identifier;
        info.selected = is_waypoint_editor_selected_target(target.uid);
        info.color = debug_waypoint_target_color(target.type);
        if (info.selected) {
            info.color.alpha = 255;
        }
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

    for (const auto& target_info : target_infos) {
        if ((!show_labels && !target_info.selected) || !target_info.renderable) {
            continue;
        }

        rf::Vector3 label_pos = target_info.center;
        label_pos.y += 0.3f;
        rf::gr::Vertex dest{};
        if (!rf::gr::rotate_vertex(&dest, &label_pos)) {
            rf::gr::project_vertex(&dest);
            if (dest.flags & 1) {
                const auto label_sv = waypoint_target_type_name(target_info.type);
                char label[96]{};
                std::snprintf(
                    label, sizeof(label), "%.*s (%d : %d)",
                    static_cast<int>(label_sv.size()), label_sv.data(),
                    target_info.uid, target_info.identifier);
                draw_centered_debug_label(dest.sx, dest.sy, label, target_info.color);
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
        const bool selected = is_waypoint_editor_selected_waypoint(i);
        if (show_spheres || selected) {
            auto color = debug_waypoint_color(node.type);
            if (selected) {
                color.alpha = 255;
            }
            rf::gr::set_color(color);
            const float debug_radius = sanitize_waypoint_link_radius(node.link_radius) * debug_waypoint_sphere_scale(node.type);
            rf::gr::sphere(node.pos, debug_radius, no_overdraw_2d_line);
        }
        if (show_labels || selected) {
            rf::Vector3 label_pos = node.pos;
            label_pos.y += 0.3f;
            rf::gr::Vertex dest;
            if (!rf::gr::rotate_vertex(&dest, &label_pos)) {
                rf::gr::project_vertex(&dest);
                if (dest.flags & 1) {
                    const auto type_name = waypoint_type_name(node.type);
                    const auto movement_subtype_name = waypoint_dropped_subtype_name(node.movement_subtype);
                    char buf[128];
                    if (waypoint_type_is_standard(node.type)) {
                        if (node.identifier >= 0) {
                            std::snprintf(
                                buf, sizeof(buf), "%.*s (%d : %.*s : %d)",
                                static_cast<int>(type_name.size()), type_name.data(),
                                i,
                                static_cast<int>(movement_subtype_name.size()), movement_subtype_name.data(),
                                node.identifier);
                        }
                        else {
                            std::snprintf(
                                buf, sizeof(buf), "%.*s (%d : %.*s)",
                                static_cast<int>(type_name.size()), type_name.data(),
                                i,
                                static_cast<int>(movement_subtype_name.size()), movement_subtype_name.data());
                        }
                    }
                    else if (node.identifier >= 0) {
                        std::snprintf(
                            buf, sizeof(buf), "%.*s (%d : %d : %.*s : %d)",
                            static_cast<int>(type_name.size()), type_name.data(),
                            i,
                            node.subtype,
                            static_cast<int>(movement_subtype_name.size()), movement_subtype_name.data(),
                            node.identifier);
                    }
                    else {
                        std::snprintf(
                            buf, sizeof(buf), "%.*s (%d : %d : %.*s)",
                            static_cast<int>(type_name.size()), type_name.data(),
                            i,
                            node.subtype,
                            static_cast<int>(movement_subtype_name.size()), movement_subtype_name.data());
                    }
                    draw_centered_debug_label(dest.sx, dest.sy, buf, rf::Color{255, 255, 255, 255});
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
        if (!ensure_waypoint_command_enabled("waypoints_save")) {
            return;
        }
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
        if (!ensure_waypoint_command_enabled("waypoints_load")) {
            return;
        }
        const bool include_std_new_waypoints = is_waypoint_bot_mode_active();
        if (load_waypoints(include_std_new_waypoints)) {
            rf::console::print("Waypoints loaded");
        }
        else {
            rf::console::print("No waypoint file found");
        }
    },
    "Load waypoint graph from .awp",
    "waypoints_load",
};

ConsoleCommand2 waypoint_load_all_cmd{
    "waypoints_load_all",
    []() {
        if (!ensure_waypoint_command_enabled("waypoints_load_all")) {
            return;
        }
        if (load_waypoints(true)) {
            rf::console::print("Waypoints loaded");
        }
        else {
            rf::console::print("No waypoint file found");
        }
    },
    "Load waypoint graph from .awp (including std_new waypoints)",
    "waypoints_load_all",
};

ConsoleCommand2 waypoint_edit_cmd{
    "waypoints_edit",
    [](std::optional<bool> enabled) {
        if (is_waypoint_bot_mode_active()) {
            rf::console::print("waypoints_edit is not used in -bot mode.");
            return;
        }

        const bool request_enable = !enabled || enabled.value();
        if (!request_enable) {
            rf::console::print("waypoints_edit can only enable waypoint editing.");
            return;
        }

        if (is_multiplayer_client()) {
            rf::console::print("waypoints_edit cannot be enabled while connected as a multiplayer client.");
            return;
        }

        if (!g_alpine_game_config.waypoints_edit_mode) {
            g_alpine_game_config.waypoints_edit_mode = true;
            rf::console::print("Waypoint editing enabled.");
        }
        else {
            rf::console::print("Waypoint editing is already enabled.");
        }
    },
    "Enable waypoint editing mode for non-bot clients",
    "waypoints_edit [true]",
};

ConsoleCommand2 waypoint_drop_cmd{
    "waypoints_drop",
    [](std::optional<bool> enabled) {
        if (!ensure_waypoint_command_enabled("waypoints_drop")) {
            return;
        }
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
        if (!ensure_waypoint_command_enabled("waypoints_debug")) {
            return;
        }
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
        if (!ensure_waypoint_command_enabled("waypoints_compress")) {
            return;
        }
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

ConsoleCommand2 waypoint_clean_cmd{
    "waypoints_clean",
    []() {
        if (!ensure_waypoint_command_enabled("waypoints_clean")) {
            return;
        }
        clean_waypoints();
        rf::console::print("Waypoints cleaned");
    },
    "Remove invalid waypoints",
    "waypoints_clean",
};

ConsoleCommand2 waypoint_reset_cmd{
    "waypoints_reset",
    []() {
        if (!ensure_waypoint_command_enabled("waypoints_reset")) {
            return;
        }
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

ConsoleCommand2 waypoint_generate_cmd{
    "waypoints_generate",
    []() {
        if (!ensure_waypoint_command_enabled("waypoints_generate")) {
            return;
        }
        if (!(rf::level.flags & rf::LEVEL_LOADED)) {
            rf::console::print("No level loaded");
            return;
        }

        if (g_waypoints.size() <= 1) {
            seed_waypoints_from_objects();
        }

        const auto seed_indices = collect_generation_seed_waypoint_indices();
        if (seed_indices.empty()) {
            rf::console::print("No respawn/item waypoints found to seed generation");
            return;
        }

        const int generated_count = generate_waypoints_from_seed_probes(seed_indices);
        const auto link_stats = link_generated_waypoint_grid();
        const int generated_explosion_target_count = generate_explosion_targets_for_autogen();
        const int generated_shatter_target_count = generate_shatter_targets_for_autogen();

        if (generated_count >= kWaypointGenerateMaxCreatedWaypoints) {
            rf::console::print(
                "Waypoint generation hit creation cap of {} nodes",
                kWaypointGenerateMaxCreatedWaypoints);
        }
        rf::console::print(
            "Generated {} waypoints from {} ctf_flag/item/respawn seeds",
            generated_count,
            static_cast<int>(seed_indices.size()));
        rf::console::print(
            "Link pass added {} bidirectional links and {} downward links",
            link_stats.bidirectional_links,
            link_stats.downward_links);
        if (link_stats.pass_through_links_rerouted > 0) {
            rf::console::print(
                "Link pass rerouted {} links through intermediate waypoints",
                link_stats.pass_through_links_rerouted);
        }
        if (link_stats.redundant_links_pruned > 0) {
            rf::console::print(
                "Link pass pruned {} redundant direct links",
                link_stats.redundant_links_pruned);
        }
        if (generated_explosion_target_count > 0) {
            rf::console::print(
                "Generated {} explosion targets from blocked waypoint pairs",
                generated_explosion_target_count);
        }
        if (generated_shatter_target_count > 0) {
            rf::console::print(
                "Generated {} shatter targets from breakable-glass blocked waypoint pairs",
                generated_shatter_target_count);
        }
    },
    "Generate a walk-probe waypoint grid from respawn/item waypoints",
    "waypoints_generate",
};

ConsoleCommand2 waypoint_zone_add_cmd{
    "waypoints_zone_add",
    []() {
        if (!ensure_waypoint_command_enabled("waypoints_zone_add")) {
            return;
        }
        const std::string_view command_line = rf::console::cmd_line;
        const auto tokens = tokenize_console_command_line(command_line);
        if (tokens.size() < 3) {
            rf::console::print("Usage:");
            rf::console::print("  waypoints_zone_add <zone_type> trigger <trigger_uid>");
            rf::console::print(
                "  waypoints_zone_add <bridge_zone_type> trigger <trigger_uid> [bridge_waypoint_uid ...]");
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

        if (waypoint_zone_type_is_bridge(zone.type)
            && source.value() != WaypointZoneSource::trigger_uid) {
            rf::console::print(
                "Zone type {} only supports trigger source",
                waypoint_zone_type_name(zone.type));
            return;
        }

        switch (source.value()) {
            case WaypointZoneSource::trigger_uid: {
                if (tokens.size() < 4) {
                    rf::console::print("Usage: waypoints_zone_add <zone_type> trigger <trigger_uid> [bridge_waypoint_uid ...]");
                    return;
                }
                if (!waypoint_zone_type_is_bridge(zone.type) && tokens.size() != 4) {
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
                if (waypoint_zone_type_is_bridge(zone.type) && tokens.size() > 4) {
                    for (size_t token_index = 4; token_index < tokens.size(); ++token_index) {
                        auto bridge_waypoint_uid = parse_int_token(tokens[token_index]);
                        if (!bridge_waypoint_uid) {
                            rf::console::print(
                                "Invalid bridge waypoint UID '{}'",
                                tokens[token_index]);
                            return;
                        }
                        zone.bridge_waypoint_uids.push_back(bridge_waypoint_uid.value());
                    }
                    normalize_zone_bridge_waypoint_refs(zone.bridge_waypoint_uids);
                }
                zone.on = false;
                zone.timer.invalidate();
                const int zone_index = add_waypoint_zone_definition(zone);
                if (waypoint_zone_type_is_bridge(zone.type)) {
                    rf::console::print(
                        "Added zone {} as index {} (trigger uid {}, duration {:.2f}s, on false, gated waypoints {})",
                                       waypoint_zone_type_name(zone.type), zone_index,
                                       zone.trigger_uid, zone.duration,
                                       static_cast<int>(zone.bridge_waypoint_uids.size()));
                }
                else {
                    rf::console::print("Added zone {} as index {} (trigger uid {})",
                                       waypoint_zone_type_name(zone.type), zone_index,
                                       zone.trigger_uid);
                }
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
        if (!ensure_waypoint_command_enabled("waypoints_zone_list")) {
            return;
        }
        if (g_waypoint_zones.empty()) {
            rf::console::print("No waypoint zones defined");
            return;
        }

        rf::console::print("Waypoint zones ({})", static_cast<int>(g_waypoint_zones.size()));
        for (int i = 0; i < static_cast<int>(g_waypoint_zones.size()); ++i) {
            const auto& zone = g_waypoint_zones[i];
            const bool bridge_zone = waypoint_zone_type_is_bridge(zone.type);
            std::string bridge_waypoint_uid_list{};
            if (bridge_zone) {
                for (size_t waypoint_index = 0; waypoint_index < zone.bridge_waypoint_uids.size(); ++waypoint_index) {
                    if (!bridge_waypoint_uid_list.empty()) {
                        bridge_waypoint_uid_list += ",";
                    }
                    bridge_waypoint_uid_list += std::to_string(zone.bridge_waypoint_uids[waypoint_index]);
                }
                if (bridge_waypoint_uid_list.empty()) {
                    bridge_waypoint_uid_list = "-";
                }
            }
            const int timer_remaining_ms = (zone.on && zone.timer.valid())
                ? std::max(zone.timer.time_until(), 0)
                : -1;
            switch (resolve_waypoint_zone_source(zone)) {
                case WaypointZoneSource::trigger_uid:
                    if (bridge_zone) {
                        rf::console::print("  [{}] {} via {} uid {} (i {}, on {}, d {:.2f}s, t {}ms, w {})",
                                           i, waypoint_zone_type_name(zone.type),
                                           waypoint_zone_source_name(WaypointZoneSource::trigger_uid),
                                           zone.trigger_uid, zone.identifier,
                                           zone.on ? "true" : "false",
                                           zone.duration, timer_remaining_ms, bridge_waypoint_uid_list);
                    }
                    else {
                        rf::console::print("  [{}] {} via {} uid {} (i {})",
                                           i, waypoint_zone_type_name(zone.type),
                                           waypoint_zone_source_name(WaypointZoneSource::trigger_uid),
                                           zone.trigger_uid, zone.identifier);
                    }
                    break;
                case WaypointZoneSource::room_uid:
                    if (bridge_zone) {
                        rf::console::print("  [{}] {} via {} uid {} (i {}, on {}, d {:.2f}s, t {}ms, w {})",
                                           i, waypoint_zone_type_name(zone.type),
                                           waypoint_zone_source_name(WaypointZoneSource::room_uid),
                                           zone.room_uid, zone.identifier,
                                           zone.on ? "true" : "false",
                                           zone.duration, timer_remaining_ms, bridge_waypoint_uid_list);
                    }
                    else {
                        rf::console::print("  [{}] {} via {} uid {} (i {})",
                                           i, waypoint_zone_type_name(zone.type),
                                           waypoint_zone_source_name(WaypointZoneSource::room_uid),
                                           zone.room_uid, zone.identifier);
                    }
                    break;
                case WaypointZoneSource::box_extents:
                    if (bridge_zone) {
                        rf::console::print(
                            "  [{}] {} via {} min {:.2f},{:.2f},{:.2f} max {:.2f},{:.2f},{:.2f} (i {}, on {}, d {:.2f}s, t {}ms, w {})",
                            i, waypoint_zone_type_name(zone.type),
                            waypoint_zone_source_name(WaypointZoneSource::box_extents),
                            zone.box_min.x, zone.box_min.y, zone.box_min.z,
                            zone.box_max.x, zone.box_max.y, zone.box_max.z, zone.identifier,
                            zone.on ? "true" : "false",
                            zone.duration, timer_remaining_ms, bridge_waypoint_uid_list);
                    }
                    else {
                        rf::console::print("  [{}] {} via {} min {:.2f},{:.2f},{:.2f} max {:.2f},{:.2f},{:.2f} (i {})",
                                           i, waypoint_zone_type_name(zone.type),
                                           waypoint_zone_source_name(WaypointZoneSource::box_extents),
                                           zone.box_min.x, zone.box_min.y, zone.box_min.z,
                                           zone.box_max.x, zone.box_max.y, zone.box_max.z, zone.identifier);
                    }
                    break;
                default:
                    break;
            }
        }
    },
    "List waypoint zones",
    "waypoints_zone_list",
};

ConsoleCommand2 waypoint_target_add_cmd{
    "waypoints_target_add",
    []() {
        if (!ensure_waypoint_command_enabled("waypoints_target_add")) {
            return;
        }
        if (!(rf::level.flags & rf::LEVEL_LOADED)) {
            rf::console::print("No level loaded");
            return;
        }

        const std::string_view command_line = rf::console::cmd_line;
        const auto tokens = tokenize_console_command_line(command_line);
        if (tokens.size() != 2) {
            rf::console::print("Usage: waypoints_target_add <type>");
            rf::console::print("Valid target types: explosion, shatter");
            return;
        }

        auto target_type = parse_waypoint_target_type_token(tokens[1]);
        if (!target_type) {
            rf::console::print("Invalid target type '{}'", tokens[1]);
            rf::console::print("Valid target types: explosion, shatter");
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
        if (!ensure_waypoint_command_enabled("waypoints_target_list")) {
            return;
        }
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

ConsoleCommand2 waypoint_delete_cmd{
    "waypoints_delete",
    []() {
        if (!ensure_waypoint_command_enabled("waypoints_delete")) {
            return;
        }

        const auto print_usage = []() {
            rf::console::print("Usage:");
            rf::console::print("  waypoints_delete w <waypoint_uid>");
            rf::console::print("  waypoints_delete z <zone_uid|all>");
            rf::console::print("  waypoints_delete t <target_uid|all>");
            rf::console::print("  waypoints_delete l <from_waypoint_uid> <to_waypoint_uid|all> [both]");
        };

        const std::string_view command_line = rf::console::cmd_line;
        const auto tokens = tokenize_console_command_line(command_line);
        if (tokens.size() < 3) {
            print_usage();
            return;
        }

        const std::string_view delete_type = tokens[1];
        if (string_iequals(delete_type, "w") || string_iequals(delete_type, "waypoint")) {
            if (tokens.size() != 3) {
                print_usage();
                return;
            }

            auto waypoint_uid = parse_int_token(tokens[2]);
            if (!waypoint_uid) {
                rf::console::print("Invalid waypoint UID '{}'", tokens[2]);
                return;
            }
            if (!remove_waypoint_by_uid(waypoint_uid.value())) {
                rf::console::print("No waypoint found with UID {}", waypoint_uid.value());
                return;
            }
            rf::console::print("Deleted waypoint {}", waypoint_uid.value());
            return;
        }

        if (string_iequals(delete_type, "z") || string_iequals(delete_type, "zone")) {
            if (tokens.size() != 3) {
                print_usage();
                return;
            }

            if (string_iequals(tokens[2], "all")) {
                const int removed = clear_waypoint_zone_definitions();
                rf::console::print("Cleared {} zone(s).", removed);
                return;
            }

            auto zone_uid = parse_int_token(tokens[2]);
            if (!zone_uid) {
                rf::console::print("Invalid zone UID '{}'", tokens[2]);
                return;
            }
            if (!remove_waypoint_zone_definition(zone_uid.value())) {
                rf::console::print("No waypoint zone found with UID {}", zone_uid.value());
                return;
            }
            rf::console::print("Deleted zone {}", zone_uid.value());
            return;
        }

        if (string_iequals(delete_type, "t") || string_iequals(delete_type, "target")) {
            if (tokens.size() != 3) {
                print_usage();
                return;
            }

            if (string_iequals(tokens[2], "all")) {
                const int removed = clear_waypoint_targets();
                rf::console::print("Cleared {} target(s).", removed);
                return;
            }

            auto target_uid = parse_int_token(tokens[2]);
            if (!target_uid) {
                rf::console::print("Invalid target UID '{}'", tokens[2]);
                return;
            }
            if (!remove_waypoint_target_by_uid(target_uid.value())) {
                rf::console::print("No waypoint target found with UID {}", target_uid.value());
                return;
            }
            rf::console::print("Deleted target {}", target_uid.value());
            return;
        }

        if (string_iequals(delete_type, "l") || string_iequals(delete_type, "link")) {
            if (tokens.size() < 4 || tokens.size() > 5) {
                print_usage();
                return;
            }

            auto from_waypoint_uid = parse_int_token(tokens[2]);
            if (!from_waypoint_uid) {
                rf::console::print("Invalid source waypoint UID '{}'", tokens[2]);
                return;
            }
            const int from_uid = from_waypoint_uid.value();
            if (!is_waypoint_uid_valid(from_uid)) {
                rf::console::print("No waypoint found with UID {}", from_uid);
                return;
            }

            bool remove_both_directions = false;
            if (tokens.size() == 5) {
                if (!string_iequals(tokens[4], "both")) {
                    print_usage();
                    return;
                }
                remove_both_directions = true;
            }

            int removed_links = 0;
            if (string_iequals(tokens[3], "all")) {
                removed_links += remove_waypoint_links_from_all(from_uid);
                if (remove_both_directions) {
                    removed_links += remove_waypoint_links_to_from_all(from_uid);
                }
            }
            else {
                auto to_waypoint_uid = parse_int_token(tokens[3]);
                if (!to_waypoint_uid) {
                    rf::console::print("Invalid destination waypoint UID '{}'", tokens[3]);
                    return;
                }
                const int to_uid = to_waypoint_uid.value();
                removed_links += remove_waypoint_links_from_to(from_uid, to_uid);
                if (remove_both_directions) {
                    removed_links += remove_waypoint_links_from_to(to_uid, from_uid);
                }
            }

            if (removed_links <= 0) {
                rf::console::print("No matching links were deleted.");
                return;
            }
            rf::console::print("Deleted {} link(s).", removed_links);
            return;
        }

        print_usage();
    },
    "Delete waypoints, zones, targets, or links",
    "waypoints_delete <w|z|l|t> ...",
    true,
};

void waypoints_init()
{
    glass_remove_floating_shards_hook.install();
    waypoint_edit_cmd.register_cmd();
    waypoint_save_cmd.register_cmd();
    waypoint_load_cmd.register_cmd();
    waypoint_load_all_cmd.register_cmd();
    waypoint_drop_cmd.register_cmd();
    waypoint_debug_cmd.register_cmd();
    waypoint_compress_cmd.register_cmd();
    waypoint_clean_cmd.register_cmd();
    waypoint_reset_cmd.register_cmd();
    waypoint_generate_cmd.register_cmd();
    waypoint_zone_add_cmd.register_cmd();
    waypoint_zone_list_cmd.register_cmd();
    waypoint_target_add_cmd.register_cmd();
    waypoint_target_list_cmd.register_cmd();
    waypoint_delete_cmd.register_cmd();
}

void waypoints_level_init()
{
    if (is_waypoint_bot_mode_active()) {
        g_has_loaded_wpt = load_waypoints(true);
        if (!g_has_loaded_wpt) {
            seed_waypoints_from_objects();
        }
    }
    else {
        clear_waypoints();
        g_has_loaded_wpt = false;
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

void waypoints_on_limbo_enter()
{
    if (!is_waypoint_bot_mode_active()) {
        return;
    }
    if (!(rf::level.flags & rf::LEVEL_LOADED)) {
        return;
    }

    if (save_waypoints()) {
        xlog::info("Bot mode: saved waypoints on limbo entry");
    }
}

void waypoints_on_trigger_activated(int trigger_uid)
{
    if (trigger_uid < 0 || g_waypoint_zones.empty()) {
        return;
    }
    activate_bridge_zones_for_trigger_uid(trigger_uid);
}

void waypoints_do_frame()
{
    if (!is_waypoint_bot_mode_active()
        && g_alpine_game_config.waypoints_edit_mode
        && is_multiplayer_client()) {
        g_alpine_game_config.waypoints_edit_mode = false;
        g_last_drop_waypoint_by_entity.clear();
        g_last_lift_uid_by_entity.clear();
        rf::console::print("Waypoint editing disabled while connected as multiplayer client.");
    }

    if (!(rf::level.flags & rf::LEVEL_LOADED)) {
        clear_waypoint_editor_selection();
        return;
    }

    update_bridge_zone_states();
    update_ctf_dropped_flag_temporary_waypoints();
    update_waypoint_editor_selection();
    handle_waypoint_editor_input();

    if (!are_waypoint_commands_enabled_for_local_client()) {
        g_drop_waypoints_prev = g_drop_waypoints;
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

bool waypoints_get_bridge_zone_state(int zone_uid, WaypointBridgeZoneState& out_state)
{
    return try_build_bridge_zone_state(zone_uid, out_state);
}

bool waypoints_find_nearest_inactive_bridge_zone(const rf::Vector3& from_pos, WaypointBridgeZoneState& out_state)
{
    float best_dist_sq = std::numeric_limits<float>::max();
    bool found = false;
    WaypointBridgeZoneState best_state{};

    for (int zone_uid = 0; zone_uid < static_cast<int>(g_waypoint_zones.size()); ++zone_uid) {
        WaypointBridgeZoneState state{};
        if (!try_build_bridge_zone_state(zone_uid, state) || state.on || !state.available) {
            continue;
        }

        const float dist_sq = distance_sq(from_pos, state.trigger_pos);
        if (dist_sq < best_dist_sq) {
            best_dist_sq = dist_sq;
            best_state = state;
            found = true;
        }
    }

    if (!found) {
        return false;
    }

    out_state = best_state;
    return true;
}

bool waypoints_waypoint_has_zone(int waypoint_uid, int zone_uid)
{
    if (waypoint_uid <= 0 || waypoint_uid >= static_cast<int>(g_waypoints.size())
        || zone_uid < 0 || zone_uid >= static_cast<int>(g_waypoint_zones.size())) {
        return false;
    }
    const auto& node = g_waypoints[waypoint_uid];
    if (!node.valid) {
        return false;
    }
    return std::find(node.zones.begin(), node.zones.end(), zone_uid) != node.zones.end();
}

int waypoints_target_count()
{
    return static_cast<int>(g_waypoint_targets.size());
}

bool waypoints_get_target_by_index(int index, WaypointTargetDefinition& out_target)
{
    if (index < 0 || index >= static_cast<int>(g_waypoint_targets.size())) {
        return false;
    }
    out_target = g_waypoint_targets[index];
    return true;
}

bool waypoints_get_target_by_uid(int target_uid, WaypointTargetDefinition& out_target)
{
    const WaypointTargetDefinition* target = find_waypoint_target_by_uid(target_uid);
    if (!target) {
        return false;
    }
    out_target = *target;
    return true;
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

bool waypoints_get_movement_subtype(int index, int& out_movement_subtype)
{
    if (index <= 0 || index >= static_cast<int>(g_waypoints.size())) {
        return false;
    }

    const auto& node = g_waypoints[index];
    if (!node.valid) {
        return false;
    }

    out_movement_subtype = normalize_waypoint_dropped_subtype(node.movement_subtype);
    return true;
}

bool waypoints_get_identifier(int index, int& out_identifier)
{
    if (index <= 0 || index >= static_cast<int>(g_waypoints.size())) {
        return false;
    }

    const auto& node = g_waypoints[index];
    if (!node.valid) {
        return false;
    }

    out_identifier = node.identifier;
    return true;
}

bool waypoints_get_link_radius(int index, float& out_link_radius)
{
    if (index <= 0 || index >= static_cast<int>(g_waypoints.size())) {
        return false;
    }

    const auto& node = g_waypoints[index];
    if (!node.valid) {
        return false;
    }

    out_link_radius = sanitize_waypoint_link_radius(node.link_radius);
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
        if (waypoint_link_blocked_by_bridge_zone_state(index, link)) {
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
            return !waypoint_link_blocked_by_bridge_zone_state(from, to);
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
