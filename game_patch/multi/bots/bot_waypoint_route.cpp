#include "bot_waypoint_route.h"

#include "bot_navigation_routes.h"
#include "../../misc/waypoints.h"
#include <algorithm>
#include <array>
#include <cmath>
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
int g_waypoint_components_total = 0;
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
}

bool bot_waypoint_route(
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

void bot_waypoint_invalidate_components()
{
    g_waypoint_components_dirty = true;
}

static void bot_waypoint_compute_components()
{
    const int waypoint_total = waypoints_count();
    g_waypoint_components.assign(static_cast<std::size_t>(waypoint_total), 0);
    g_waypoint_components_total = waypoint_total;
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

    if (g_waypoint_components_dirty || g_waypoint_components_total != waypoint_total) {
        bot_waypoint_compute_components();
    }

    return g_waypoint_components[a] != 0
        && g_waypoint_components[a] == g_waypoint_components[b];
}
