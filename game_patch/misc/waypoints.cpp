#include "waypoints.h"
#include "alpine_settings.h"
#include "../main/main.h"
#include "../os/console.h"
#include "../rf/collide.h"
#include "../rf/entity.h"
#include "../rf/file/file.h"
#include "../rf/geometry.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../rf/item.h"
#include "../rf/level.h"
#include "../rf/object.h"
#include "../rf/player/player.h"
#include "../rf/trigger.h"
#include "../object/object.h"
#include "../graphics/gr.h"
#include <algorithm>
#include <array>
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
#include <xlog/xlog.h>

std::vector<WaypointNode> g_waypoints;
std::vector<WpCacheNode> g_cache_nodes;
WpCacheNode* g_cache_root = nullptr;
bool g_cache_dirty = true;
bool g_has_loaded_wpt = false;
bool g_drop_waypoints = true;
int g_waypoint_revision = 0;
bool g_waypoints_compress = true;

std::unordered_map<int, int> g_last_drop_waypoint_by_entity{};

int g_debug_waypoints_mode = 0;
bool g_drop_waypoints_prev = true;

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

constexpr float kWaypointLinkRadiusEpsilon = 0.001f;

float sanitize_waypoint_link_radius(float link_radius)
{
    if (!std::isfinite(link_radius) || link_radius <= 0.0f) {
        return kWaypointLinkRadius;
    }
    return link_radius;
}

WaypointType waypoint_type_from_int(int raw_type)
{
    switch (raw_type) {
        case static_cast<int>(WaypointType::dropped_legacy):
            return WaypointType::dropped_legacy;
        case static_cast<int>(WaypointType::dropped):
            return WaypointType::dropped;
        case static_cast<int>(WaypointType::item):
            return WaypointType::item;
        case static_cast<int>(WaypointType::respawn):
            return WaypointType::respawn;
        case static_cast<int>(WaypointType::jump_pad):
            return WaypointType::jump_pad;
        case static_cast<int>(WaypointType::jump_pad_landing):
            return WaypointType::jump_pad_landing;
        case static_cast<int>(WaypointType::lift_entrance):
            return WaypointType::lift_entrance;
        case static_cast<int>(WaypointType::lift_exit):
            return WaypointType::lift_exit;
        case static_cast<int>(WaypointType::ladder_entrance):
            return WaypointType::ladder_entrance;
        case static_cast<int>(WaypointType::ctf_flag):
            return WaypointType::ctf_flag;
        case static_cast<int>(WaypointType::control_point):
            return WaypointType::control_point;
        default:
            return static_cast<WaypointType>(raw_type);
    }
}

int waypoint_type_to_save_value(WaypointType type)
{
    // Preserve compatibility with older files where dropped waypoints are encoded as 0.
    if (type == WaypointType::dropped) {
        return static_cast<int>(WaypointType::dropped_legacy);
    }
    return static_cast<int>(type);
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
    if (!entity.move_mode || !entity.move_mode->valid) {
        return false;
    }
    return entity.move_mode->mode == 1;
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

int find_jump_pad_waypoint_in_radius(const rf::Vector3& pos)
{
    constexpr float kJumpPadAutoLinkRangeScale = 0.5f;
    float best_dist_sq = std::numeric_limits<float>::max();
    int best_index = 0;
    for (int i = 1; i < static_cast<int>(g_waypoints.size()); ++i) {
        const auto& node = g_waypoints[i];
        if (!node.valid || node.type != WaypointType::jump_pad) {
            continue;
        }

        const float radius = sanitize_waypoint_link_radius(node.link_radius) * kJumpPadAutoLinkRangeScale;
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
                 float link_radius = kWaypointLinkRadius)
{
    WaypointNode node{};
    node.pos = pos;
    node.type = type;
    node.subtype = subtype;
    node.link_radius = sanitize_waypoint_link_radius(link_radius);
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
        return map_name.string() + ".toml";
    }
    return std::filesystem::path(waypoint_dir.value()) / (map_name.string() + ".toml");
}

void seed_waypoints_from_objects()
{
    if (g_waypoints.size() > 1) {
        return;
    }
    rf::Object* obj = rf::object_list.next_obj;
    while (obj != &rf::object_list) {
        if (obj->type == rf::OT_ITEM) {
            int subtype = static_cast<rf::Item*>(obj)->info_index;
            add_waypoint(obj->pos, WaypointType::item, subtype, false, true);
        }
        obj = obj->next_obj;
    }
    for (const auto& rp : get_alpine_respawn_points()) {
        if (rp.enabled) {
            int subtype = 3;
            if (rp.red_team && rp.blue_team) {
                subtype = 0;
            }
            else if (rp.red_team) {
                subtype = 1;
            }
            else if (rp.blue_team) {
                subtype = 2;
            }
            add_waypoint(rp.position, WaypointType::respawn, subtype, false, true);
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
        add_waypoint(push_region->pos, WaypointType::jump_pad, 0, false, true, link_radius);
    }
}

void clear_waypoints()
{
    g_waypoints.clear();
    g_waypoints.push_back({});
    invalidate_cache();
}

void reset_waypoints_to_default_grid()
{
    clear_waypoints();
    seed_waypoints_from_objects();
    g_has_loaded_wpt = false;
    g_waypoint_revision = 0;
    g_last_drop_waypoint_by_entity.clear();
}

bool save_waypoints()
{
    if (g_waypoints.size() <= 1) {
        return false;
    }
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
        {"version", kWptVersion},
        {"compressed", save_compressed},
        {"level", std::string{rf::level.filename.c_str()}},
        {"checksum", static_cast<int64_t>(rf::level.checksum)},
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
            entry.insert("r", node.link_radius);
        }
        nodes.push_back(entry);
    }
    toml::table root{
        {"header", header},
        {"w", nodes},
    };
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
        if (const auto* checksum_node = header->get("checksum"); checksum_node && checksum_node->is_number()) {
            header_checksum = static_cast<uint32_t>(checksum_node->value_or(0));
        }
    }
    if (header_checksum && *header_checksum != rf::level.checksum) {
        xlog::warn("Waypoint checksum mismatch for {}: file {}, level {}", filename.string(), *header_checksum,
                   rf::level.checksum);
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
        int raw_type = static_cast<int>(WaypointType::dropped_legacy);
        int subtype = 0;
        float link_radius = kWaypointLinkRadius;
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
                link_radius = static_cast<float>(radius_node->value_or<double>(link_radius));
            }
        }
        else if (const auto* radius_node = node_tbl->get("radius")) {
            if (radius_node->is_number()) {
                link_radius = static_cast<float>(radius_node->value_or<double>(link_radius));
            }
        }
        WaypointType type = waypoint_type_from_int(raw_type);
        int index = add_waypoint(wp_pos, type, subtype, false, true, link_radius);
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
}

void navigate_entity(int entity_handle, rf::Entity& entity, bool allow_drop)
{
    if (entity_handle < 0) {
        return;
    }

    rf::Vector3 pos = get_entity_feet_pos(entity);
    const int closest_standard = closest_waypoint(pos, kWaypointRadius);
    const int closest_jump_pad = find_jump_pad_waypoint_in_radius(pos);
    const int closest = closest_jump_pad > 0 ? closest_jump_pad : closest_standard;
    bool should_drop_new = allow_drop && should_drop();

    if (closest_standard > 0) {
        const float dist_sq = distance_sq(pos, g_waypoints[closest_standard].pos);
        if (dist_sq <= kWaypointRadius * kWaypointRadius) {
            should_drop_new = false;
        }
    }
    if (closest_jump_pad > 0) {
        should_drop_new = false;
    }

    int& last_drop_waypoint = g_last_drop_waypoint_by_entity[entity_handle];
    if (should_drop_new) {
        const bool grounded = is_player_grounded(entity);
        const bool falling = rf::entity_is_falling(&entity);
        const bool swimming = rf::entity_is_swimming(&entity);
        const bool crouching = rf::entity_is_crouching(&entity);
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
        const int new_index = add_waypoint(pos, WaypointType::dropped, subtype, grounded, grounded);
        if (last_drop_waypoint > 0) {
            link_waypoint_if_clear(last_drop_waypoint, new_index);
            if (grounded) {
                link_waypoint_if_clear(new_index, last_drop_waypoint);
            }
        }
        last_drop_waypoint = new_index;
        return;
    }

    if (closest > 0) {
        if (allow_drop && last_drop_waypoint > 0 && last_drop_waypoint != closest) {
            const auto& target_waypoint = g_waypoints[closest];
            if (target_waypoint.type == WaypointType::jump_pad) {
                // Entering a jump pad should create only an ingress link.
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
        case WaypointType::dropped_legacy:
            return {255, 255, 255, 150};
        case WaypointType::dropped:
            return {255, 255, 255, 75};
        case WaypointType::item:
            return {255, 220, 0, 150};
        case WaypointType::respawn:
            return {0, 220, 255, 150};
        case WaypointType::jump_pad:
            return {0, 255, 120, 150};
        case WaypointType::jump_pad_landing:
            return {180, 255, 120, 150};
        case WaypointType::lift_entrance:
            return {140, 180, 255, 150};
        case WaypointType::lift_exit:
            return {80, 120, 255, 150};
        case WaypointType::ladder_entrance:
            return {255, 170, 70, 150};
        case WaypointType::ctf_flag:
            return {255, 70, 70, 150};
        case WaypointType::control_point:
            return {200, 70, 255, 150};
        default:
            return {200, 200, 200, 150};
    }
}

float debug_waypoint_sphere_scale(WaypointType type)
{
    if (type == WaypointType::dropped_legacy || type == WaypointType::dropped) {
        return 0.125f;
    }
    return 0.25f;
}

void draw_debug_waypoints()
{
    if (g_debug_waypoints_mode == 0) {
        return;
    }
    const bool show_links = g_debug_waypoints_mode >= 1;
    const bool show_spheres = g_debug_waypoints_mode >= 2;
    const bool show_labels = g_debug_waypoints_mode >= 3;
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
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%d (%d : %d)", i, static_cast<int>(node.type), node.subtype);
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
    "Save current waypoint graph to .wpt",
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
    "Load waypoint graph from .wpt",
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
        rf::console::print("Waypoint debug mode {} (0=off, 1=links, 2=links+spheres, 3=links+spheres+labels)",
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

void waypoints_init()
{
    waypoint_save_cmd.register_cmd();
    waypoint_load_cmd.register_cmd();
    waypoint_drop_cmd.register_cmd();
    waypoint_debug_cmd.register_cmd();
    waypoint_compress_cmd.register_cmd();
    waypoint_clean_cmd.register_cmd();
    waypoint_reset_cmd.register_cmd();
}

void waypoints_level_init()
{
    g_has_loaded_wpt = load_waypoints();
    if (!g_has_loaded_wpt) {
        seed_waypoints_from_objects();
    }
    g_last_drop_waypoint_by_entity.clear();
    invalidate_cache();
}

void waypoints_level_reset()
{
    clear_waypoints();
    g_has_loaded_wpt = false;
    g_last_drop_waypoint_by_entity.clear();
}

void waypoints_do_frame()
{
    if (!(rf::level.flags & rf::LEVEL_LOADED)) {
        return;
    }
    if (!g_drop_waypoints && g_drop_waypoints_prev) {
        g_last_drop_waypoint_by_entity.clear();
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
