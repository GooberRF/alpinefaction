#include "waypoints.h"
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

int g_last_drop_waypoint = 0;

bool g_debug_waypoints = false;
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

void link_waypoint_to_nearest(int index, bool bidirectional)
{
    if (index <= 0 || index >= static_cast<int>(g_waypoints.size())) {
        return;
    }
    int nearest = find_nearest_waypoint(g_waypoints[index].pos, kWaypointLinkRadius, index);
    if (nearest > 0) {
        if (!can_link_waypoints(g_waypoints[index].pos, g_waypoints[nearest].pos)) {
            return;
        }
        if (bidirectional) {
            link_waypoints_bidirectional(index, nearest);
        }
        else {
            link_waypoint(index, nearest);
        }
    }
}

int add_waypoint(const rf::Vector3& pos, int type, int subtype, bool link_to_nearest, bool bidirectional_link)
{
    WaypointNode node{};
    node.pos = pos;
    node.type = type;
    node.subtype = subtype;
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
    add_waypoint({}, 0, 0, false, true);
    rf::Object* obj = rf::object_list.next_obj;
    while (obj != &rf::object_list) {
        bool should_add = false;
        int type = 0;
        int subtype = 0;
        if (obj->type == rf::OT_ITEM) {
            should_add = true;
            type = 2;
            subtype = static_cast<rf::Item*>(obj)->info_index;
        }
        if (should_add) {
            add_waypoint(obj->pos, type, subtype, false, true);
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
            add_waypoint(rp.position, 3, subtype, false, true);
        }
    }
}

void clear_waypoints()
{
    g_waypoints.clear();
    g_waypoints.push_back({});
    invalidate_cache();
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
        int saved_type = node.type == 1 ? 0 : node.type;
        toml::table entry{
            {"p", pos},
            {"t", saved_type},
            {"s", node.subtype},
            {"l", links},
        };
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
        int type = 0;
        int subtype = 0;
        if (const auto* type_node = node_tbl->get("t")) {
            if (type_node->is_number()) {
                type = static_cast<int>(type_node->value_or(type));
            }
        }
        else if (const auto* type_node = node_tbl->get("type")) {
            if (type_node->is_number()) {
                type = static_cast<int>(type_node->value_or(type));
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
        int index = add_waypoint(wp_pos, type, subtype, false, true);
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
}

bool should_navigate()
{
    return g_drop_waypoints || g_has_loaded_wpt;
}

bool should_drop()
{
    return g_drop_waypoints || !g_has_loaded_wpt;
}

void navigate()
{
    if (!rf::local_player) {
        return;
    }
    auto* entity = rf::entity_from_handle(rf::local_player->entity_handle);
    if (!entity) {
        return;
    }
    if (!should_navigate()) {
        return;
    }
    rf::Vector3 pos = get_entity_feet_pos(*entity);
    int closest = closest_waypoint(pos, kWaypointRadius);
    bool should_drop_new = should_drop();
    if (closest > 0) {
        float dist_sq = distance_sq(pos, g_waypoints[closest].pos);
        if (dist_sq <= kWaypointRadius * kWaypointRadius) {
            should_drop_new = false;
        }
    }
    if (should_drop_new) {
        bool grounded = is_player_grounded(*entity);
        int subtype = grounded ? 0 : 1;
        int new_index = add_waypoint(pos, 1, subtype, grounded, grounded);
        if (g_last_drop_waypoint > 0) {
            link_waypoint(g_last_drop_waypoint, new_index);
            if (grounded) {
                link_waypoint(new_index, g_last_drop_waypoint);
            }
        }
        g_last_drop_waypoint = new_index;
        return;
    }
    if (closest > 0) {
        if (g_last_drop_waypoint > 0 && g_last_drop_waypoint != closest) {
            bool grounded = is_player_grounded(*entity);
            link_waypoint(g_last_drop_waypoint, closest);
            if (grounded) {
                link_waypoint(closest, g_last_drop_waypoint);
            }
        }
        g_last_drop_waypoint = closest;
    }
}

void draw_debug_waypoints()
{
    if (!g_debug_waypoints) {
        return;
    }
    for (int i = 1; i < static_cast<int>(g_waypoints.size()); ++i) {
        const auto& node = g_waypoints[i];
        if (!node.valid) {
            continue;
        }
        {
            rf::Vector3 label_pos = node.pos;
            label_pos.y += 0.3f;
            rf::gr::Vertex dest;
            if (!rf::gr::rotate_vertex(&dest, &label_pos)) {
                rf::gr::project_vertex(&dest);
                if (dest.flags & 1) {
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%d (%d : %d)", i, node.type, node.subtype);
                    rf::gr::set_color(255, 255, 255, 255);
                    rf::gr::string(static_cast<int>(dest.sx), static_cast<int>(dest.sy), buf, -1);
                }
            }
        }
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
    [](std::optional<bool> enabled) {
        if (enabled) {
            g_debug_waypoints = enabled.value();
        }
        else {
            g_debug_waypoints = !g_debug_waypoints;
        }
        rf::console::print("Waypoint debug {}", g_debug_waypoints ? "enabled" : "disabled");
    },
    "Toggle waypoint debug drawing",
    "waypoints_debug [true|false]",
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

void waypoints_init()
{
    waypoint_save_cmd.register_cmd();
    waypoint_load_cmd.register_cmd();
    waypoint_drop_cmd.register_cmd();
    waypoint_debug_cmd.register_cmd();
    waypoint_compress_cmd.register_cmd();
    waypoint_clean_cmd.register_cmd();
}

void waypoints_level_init()
{
    g_has_loaded_wpt = load_waypoints();
    if (!g_has_loaded_wpt) {
        seed_waypoints_from_objects();
    }
    g_last_drop_waypoint = 0;
    invalidate_cache();
}

void waypoints_level_reset()
{
    clear_waypoints();
    g_has_loaded_wpt = false;
    g_last_drop_waypoint = 0;
}

void waypoints_do_frame()
{
    if (!(rf::level.flags & rf::LEVEL_LOADED)) {
        return;
    }
    if (!g_drop_waypoints && g_drop_waypoints_prev) {
        g_last_drop_waypoint = 0;
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
        for (int i = 0; i < node.num_links; ++i) {
            int neighbor = node.links[i];
            if (neighbor <= 0 || neighbor >= static_cast<int>(g_waypoints.size())) {
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
