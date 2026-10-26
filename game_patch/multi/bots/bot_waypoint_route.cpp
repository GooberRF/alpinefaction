#include "bot_waypoint_route.h"

#include "bot_context.h"
#include "bot_navigation_routes.h"
#include "../../misc/waypoints.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <queue>
#include <vector>

namespace
{
std::vector<int> g_route_search_generation{};
std::vector<int> g_route_closed_generation{};
std::vector<int> g_route_prev{};
std::vector<float> g_route_cur_score{};
int g_route_generation_counter = 1;

std::vector<int> g_waypoint_components{};
int g_waypoint_components_graph_version = 0;
bool g_waypoint_components_dirty = true;

void ensure_route_search_buffers(const std::size_t waypoint_count)
{
    if (g_route_search_generation.size() != waypoint_count) {
        g_route_search_generation.assign(waypoint_count, 0);
        g_route_closed_generation.assign(waypoint_count, 0);
        g_route_prev.assign(waypoint_count, -1);
        g_route_cur_score.assign(waypoint_count, std::numeric_limits<float>::infinity());
        g_route_generation_counter = 1;
    }
}

void reset_route_search_generations()
{
    std::fill(g_route_search_generation.begin(), g_route_search_generation.end(), 0);
    std::fill(g_route_closed_generation.begin(), g_route_closed_generation.end(), 0);
    g_route_generation_counter = 1;
}

bool search_waypoint_route(
    int from,
    int to,
    const std::vector<int>& avoidset,
    std::vector<int>& out_path)
{
    out_path.clear();

    const int waypoint_total = waypoints_count();
    if (from <= 0 || to <= 0 || from >= waypoint_total || to >= waypoint_total) {
        return false;
    }
    if (std::binary_search(avoidset.begin(), avoidset.end(), from)
        || std::binary_search(avoidset.begin(), avoidset.end(), to)) {
        return false;
    }
    if (from == to) {
        out_path.push_back(from);
        return true;
    }

    rf::Vector3 from_pos{};
    rf::Vector3 to_pos{};
    if (!waypoints_get_pos(from, from_pos) || !waypoints_get_pos(to, to_pos)) {
        return false;
    }

    ensure_route_search_buffers(static_cast<std::size_t>(waypoint_total));
    if (g_route_generation_counter >= std::numeric_limits<int>::max() - 2) {
        reset_route_search_generations();
    }
    ++g_route_generation_counter;
    const int generation = g_route_generation_counter;

    auto ensure_node_initialized = [&](int index) {
        if (g_route_search_generation[index] == generation) {
            return;
        }
        g_route_search_generation[index] = generation;
        g_route_closed_generation[index] = 0;
        g_route_prev[index] = -1;
        g_route_cur_score[index] = std::numeric_limits<float>::infinity();
    };

    struct NodeEntry
    {
        int index;
        float score;
        bool operator<(const NodeEntry& other) const
        {
            return score > other.score;
        }
    };

    std::priority_queue<NodeEntry> open{};
    ensure_node_initialized(from);
    g_route_cur_score[from] = 0.0f;
    open.push({from, (from_pos - to_pos).len()});

    std::array<int, kMaxWaypointLinks> links{};
    while (!open.empty()) {
        const int current = open.top().index;
        open.pop();

        if (g_route_closed_generation[current] == generation) {
            continue;
        }
        g_route_closed_generation[current] = generation;

        if (current == to) {
            break;
        }

        const float current_score = g_route_cur_score[current];
        if (!std::isfinite(current_score)) {
            continue;
        }

        rf::Vector3 current_pos{};
        if (!waypoints_get_pos(current, current_pos)) {
            continue;
        }

        const int num_links = waypoints_get_links(current, links);
        for (int i = 0; i < num_links; ++i) {
            const int neighbor = links[i];
            if (neighbor <= 0 || neighbor >= waypoint_total) {
                continue;
            }
            if (!bot_nav_is_link_traversable_for_route(current, neighbor)) {
                continue;
            }
            if (std::binary_search(avoidset.begin(), avoidset.end(), neighbor)) {
                continue;
            }
            if (g_route_closed_generation[neighbor] == generation) {
                continue;
            }

            rf::Vector3 neighbor_pos{};
            if (!waypoints_get_pos(neighbor, neighbor_pos)) {
                continue;
            }

            ensure_node_initialized(neighbor);
            const float tentative = current_score + (neighbor_pos - current_pos).len();
            if (tentative < g_route_cur_score[neighbor]) {
                g_route_prev[neighbor] = current;
                g_route_cur_score[neighbor] = tentative;
                open.push({neighbor, tentative + (neighbor_pos - to_pos).len()});
            }
        }
    }

    if (g_route_search_generation[to] != generation || g_route_prev[to] == -1) {
        return false;
    }

    int current = to;
    int safety_counter = waypoint_total + 1;
    while (current != -1 && safety_counter-- > 0) {
        out_path.push_back(current);
        current = g_route_prev[current];
    }
    if (safety_counter <= 0) {
        out_path.clear();
        return false;
    }

    std::reverse(out_path.begin(), out_path.end());
    return !out_path.empty();
}

uint64_t hash_avoidset(const std::vector<int>& avoidset)
{
    uint64_t hash = 14695981039346656037ull;
    const auto mix = [&hash](const uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            hash ^= (value >> shift) & 0xFFu;
            hash *= 1099511628211ull;
        }
    };
    mix(static_cast<uint32_t>(avoidset.size()));
    for (const int waypoint : avoidset) {
        mix(static_cast<uint32_t>(waypoint));
    }
    return hash;
}

// Each avoid set is tracked apart, so a stricter search cannot hold back its relaxed fallbacks.
std::vector<FailedRouteSearch>& failed_route_searches()
{
    BotState& state = bot_state();
    if (state.failed_route_searches_graph_version != waypoints_graph_version()) {
        state.failed_route_searches.clear();
        state.failed_route_searches_graph_version = waypoints_graph_version();
    }
    return state.failed_route_searches;
}

FailedRouteSearch* find_failed_route_search(const int from, const int to, const uint64_t avoidset_hash)
{
    for (FailedRouteSearch& entry : failed_route_searches()) {
        if (entry.from_waypoint == from && entry.to_waypoint == to && entry.avoidset_hash == avoidset_hash) {
            return &entry;
        }
    }
    return nullptr;
}

bool is_route_search_backed_off(const int from, const int to, const uint64_t avoidset_hash)
{
    const FailedRouteSearch* entry = find_failed_route_search(from, to, avoidset_hash);
    return entry && entry->cooldown.valid() && !entry->cooldown.elapsed();
}

void note_route_search_result(const int from, const int to, const uint64_t avoidset_hash, const bool found)
{
    auto& entries = failed_route_searches();
    FailedRouteSearch* entry = find_failed_route_search(from, to, avoidset_hash);
    if (found) {
        if (entry) {
            entries.erase(entries.begin() + (entry - entries.data()));
        }
        return;
    }
    if (!entry) {
        if (entries.size() >= kFailedRouteSearchCapacity) {
            auto victim = std::find_if(entries.begin(), entries.end(), [](const FailedRouteSearch& e) {
                return !e.cooldown.valid() || e.cooldown.elapsed();
            });
            if (victim == entries.end()) {
                victim = std::min_element(entries.begin(), entries.end(),
                    [](const FailedRouteSearch& a, const FailedRouteSearch& b) {
                        return a.cooldown.time_until() < b.cooldown.time_until();
                    });
            }
            entries.erase(victim);
        }
        entry = &entries.emplace_back();
        entry->from_waypoint = from;
        entry->to_waypoint = to;
        entry->avoidset_hash = avoidset_hash;
    }
    if (++entry->consecutive_failures >= kRouteFailureBackoffThreshold) {
        entry->consecutive_failures = 0;
        entry->cooldown.set(kRouteFailureBackoffMs);
    }
}
}

bool bot_waypoint_route(
    const int from,
    const int to,
    const std::vector<int>& avoidset,
    std::vector<int>& out_path)
{
    const int waypoint_total = waypoints_count();
    if (from <= 0 || to <= 0 || from >= waypoint_total || to >= waypoint_total || from == to) {
        return search_waypoint_route(from, to, avoidset, out_path);
    }
    const uint64_t avoidset_hash = hash_avoidset(avoidset);
    if (is_route_search_backed_off(from, to, avoidset_hash)) {
        out_path.clear();
        return false;
    }
    const bool found = search_waypoint_route(from, to, avoidset, out_path);
    note_route_search_result(from, to, avoidset_hash, found);
    return found;
}

void bot_waypoint_compute_distance_field(
    const int from,
    const std::vector<int>& avoidset,
    BotWaypointDistanceField& out)
{
    const int waypoint_total = std::max(waypoints_count(), 0);
    out.source = from;
    out.avoidset = avoidset;
    out.dist.assign(static_cast<std::size_t>(waypoint_total), std::numeric_limits<float>::infinity());
    out.prev.assign(static_cast<std::size_t>(waypoint_total), -1);

    rf::Vector3 from_pos{};
    if (from <= 0 || from >= waypoint_total
        || std::binary_search(avoidset.begin(), avoidset.end(), from)
        || !waypoints_get_pos(from, from_pos)) {
        return;
    }

    struct NodeEntry
    {
        int index;
        float dist;
        bool operator<(const NodeEntry& other) const
        {
            return dist > other.dist;
        }
    };

    std::priority_queue<NodeEntry> open{};
    out.dist[from] = 0.0f;
    open.push({from, 0.0f});

    std::array<int, kMaxWaypointLinks> links{};
    while (!open.empty()) {
        const NodeEntry entry = open.top();
        open.pop();
        const int current = entry.index;
        if (entry.dist > out.dist[current]) {
            continue;
        }

        rf::Vector3 current_pos{};
        if (!waypoints_get_pos(current, current_pos)) {
            continue;
        }

        const int num_links = waypoints_get_links(current, links);
        for (int i = 0; i < num_links; ++i) {
            const int neighbor = links[i];
            if (neighbor <= 0 || neighbor >= waypoint_total) {
                continue;
            }
            if (!bot_nav_is_link_traversable_for_route(current, neighbor)) {
                continue;
            }
            if (std::binary_search(avoidset.begin(), avoidset.end(), neighbor)) {
                continue;
            }

            rf::Vector3 neighbor_pos{};
            if (!waypoints_get_pos(neighbor, neighbor_pos)) {
                continue;
            }

            const float tentative = entry.dist + (neighbor_pos - current_pos).len();
            if (tentative < out.dist[neighbor]) {
                out.dist[neighbor] = tentative;
                out.prev[neighbor] = current;
                open.push({neighbor, tentative});
            }
        }
    }
}

bool bot_waypoint_distance_field_path(
    const BotWaypointDistanceField& field,
    const int to,
    std::vector<int>& out_path)
{
    out_path.clear();
    if (to <= 0 || to >= static_cast<int>(field.dist.size()) || !std::isfinite(field.dist[to])) {
        return false;
    }

    for (int current = to; current != -1; current = field.prev[current]) {
        if (out_path.size() >= field.dist.size()) {
            out_path.clear();
            return false;
        }
        out_path.push_back(current);
    }
    std::reverse(out_path.begin(), out_path.end());
    return true;
}

void bot_waypoint_invalidate_components()
{
    g_waypoint_components_dirty = true;
}

static void bot_waypoint_compute_components()
{
    const int waypoint_total = waypoints_count();
    g_waypoint_components.assign(static_cast<std::size_t>(waypoint_total), 0);
    g_waypoint_components_graph_version = waypoints_graph_version();
    g_waypoint_components_dirty = false;

    if (waypoint_total <= 1) {
        return;
    }

    int next_component = 1;
    std::queue<int> bfs_queue{};
    std::array<int, kMaxWaypointLinks> links{};

    for (int start = 1; start < waypoint_total; ++start) {
        if (g_waypoint_components[start] != 0) {
            continue;
        }

        const int component_id = next_component++;
        g_waypoint_components[start] = component_id;
        bfs_queue.push(start);

        while (!bfs_queue.empty()) {
            const int current = bfs_queue.front();
            bfs_queue.pop();

            const int num_links = waypoints_get_links(current, links);
            for (int i = 0; i < num_links; ++i) {
                const int neighbor = links[i];
                if (neighbor <= 0 || neighbor >= waypoint_total) {
                    continue;
                }
                if (g_waypoint_components[neighbor] != 0) {
                    continue;
                }
                if (!bot_nav_is_link_traversable_for_route(current, neighbor)) {
                    continue;
                }
                g_waypoint_components[neighbor] = component_id;
                bfs_queue.push(neighbor);
            }
        }
    }
}

bool bot_waypoint_same_component(int a, int b)
{
    const int waypoint_total = waypoints_count();
    if (a <= 0 || b <= 0 || a >= waypoint_total || b >= waypoint_total) {
        return false;
    }
    if (a == b) {
        return true;
    }

    if (g_waypoint_components_dirty
        || g_waypoint_components_graph_version != waypoints_graph_version()
        || g_waypoint_components.size() != static_cast<std::size_t>(waypoint_total)) {
        bot_waypoint_compute_components();
    }

    return g_waypoint_components[a] != 0
        && g_waypoint_components[a] == g_waypoint_components[b];
}
