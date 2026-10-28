#include "bot_waypoint_route.h"

#include "bot_context.h"
#include "bot_navigation_routes.h"
#include "bot_vehicle.h"
#include "bot_vehicle_nav.h"
#include "../../misc/waypoints.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <queue>
#include <span>
#include <utility>
#include <vector>

namespace
{
constexpr float kInfinity = std::numeric_limits<float>::infinity();
// A goal asked for again within this many goal lookups (routes and destination distances) gets a destination field.
constexpr int kRecentRouteDestinations = 32;
// This many searches in a row from one start waypoint (goals in sight, detours) share a field from it.
constexpr int kRouteSourceRunForField = 3;
constexpr std::size_t kDestinationFieldBudgetBytes = 16u << 20;
constexpr int kMaxDestinationFields = 64;
// Past this many logged overlay changes, fields start over from the base, or afresh, rather than walk through each.
constexpr std::size_t kMaxOverlayChanges = 4096;
// Extra route length for entering a foot node beside a vehicle road while an enemy hull is near.
constexpr float kVehicleRoadNodeRouteCost = 4.0f;

struct RouteEdge
{
    int node = 0;
    float cost = 0.0f;

    bool operator==(const RouteEdge&) const = default;
};

struct RouteAdjacency
{
    std::vector<int> begin{};
    std::vector<RouteEdge> edges{};

    int size() const
    {
        return begin.empty() ? 0 : static_cast<int>(begin.size()) - 1;
    }

    std::span<const RouteEdge> of(const int node) const
    {
        if (node < 0 || node >= size()) {
            return {};
        }
        return {edges.data() + begin[node], static_cast<std::size_t>(begin[node + 1] - begin[node])};
    }
};

// The traversable links of the waypoint graph with their costs, built once per graph version. The persistent part is
// rebuilt only when persistent nodes change; temporary nodes and the links that touch them are overlaid, so their
// churn does not invalidate what was computed on the rest.
struct RouteGraph
{
    int graph_version = -1;
    int persistent_version = -1;
    // Bumped only when the persistent links really change, so a persistent bump that leaves the links alone keeps
    // every destination field.
    int base_revision = 0;
    int waypoint_total = 0;
    std::vector<rf::Vector3> pos{};
    std::vector<uint8_t> has_pos{};
    std::vector<uint8_t> temporary_flag{};
    RouteAdjacency base_out{};
    RouteAdjacency base_in{};
    // Index into the override lists for a node whose current links differ from the base, else -1.
    std::vector<int> out_override{};
    std::vector<int> in_override{};
    std::vector<std::vector<RouteEdge>> override_out_lists{};
    std::vector<std::vector<RouteEdge>> override_in_lists{};
    // The node's links as waypoints_get_links gave them (count, then links), for each override out list.
    std::vector<std::array<int, kMaxWaypointLinks + 1>> override_out_links{};
    int override_out_count = 0;
    int override_in_count = 0;
    std::vector<int> temporary{};
    // Base links whose slot a temporary node borrowed, as (from, to).
    std::vector<std::pair<int, int>> lent_links{};
    // The overlay as of the previous refresh, to reuse its lists and to tell what changed.
    int overlay_total = 0;
    std::vector<int> previous_out_override{};
    std::vector<std::vector<RouteEdge>> previous_out_lists{};
    std::vector<std::array<int, kMaxWaypointLinks + 1>> previous_out_links{};
    std::vector<int> previous_temporary{};
    // Since overlay_epoch, no node was removed or renumbered, and these logs hold every overlay change by graph
    // version: the nodes that gained out-links and the links that went away.
    int overlay_epoch = -1;
    std::vector<std::pair<int, int>> overlay_gained{};
    std::vector<std::pair<int, std::pair<int, int>>> overlay_lost{};

    std::span<const RouteEdge> out_edges(const int node) const
    {
        if (node >= 0 && node < static_cast<int>(out_override.size()) && out_override[node] >= 0) {
            return override_out_lists[out_override[node]];
        }
        return base_out.of(node);
    }

    std::span<const RouteEdge> in_edges(const int node) const
    {
        if (node >= 0 && node < static_cast<int>(in_override.size()) && in_override[node] >= 0) {
            return override_in_lists[in_override[node]];
        }
        return base_in.of(node);
    }

    bool is_temporary(const int node) const
    {
        return node >= 0 && node < static_cast<int>(temporary_flag.size()) && temporary_flag[node];
    }
};

struct FieldPatchEntry
{
    int node = 0;
    float dist = 0.0f;
    int next = -1;
};

// Shortest route length from every waypoint to one goal, and the next waypoint on that route.
struct DestinationField
{
    int goal = 0;
    // The arrays cover the base graph as of base_revision (a temporary goal enters it through its persistent
    // in-links), or the whole graph as of some graph version when arrays_cover_graph is set.
    int base_revision = -1;
    bool arrays_cover_graph = false;
    std::vector<RouteEdge> goal_in_links{};
    // The patched arrays are exact for this graph version.
    int graph_version = -1;
    uint64_t last_use = 0;
    std::vector<float> dist{};
    std::vector<int> next{};
    // Where the current graph's routes differ from the arrays, sorted by node.
    std::vector<FieldPatchEntry> patch{};

    const FieldPatchEntry* find_patch(const int node) const
    {
        const auto it = std::lower_bound(patch.begin(), patch.end(), node, [](const FieldPatchEntry& entry, int n) {
            return entry.node < n;
        });
        return it != patch.end() && it->node == node ? &*it : nullptr;
    }

    float dist_of(const int node) const
    {
        if (const FieldPatchEntry* entry = find_patch(node)) {
            return entry->dist;
        }
        return node >= 0 && node < static_cast<int>(dist.size()) ? dist[node] : kInfinity;
    }

    int next_of(const int node) const
    {
        if (const FieldPatchEntry* entry = find_patch(node)) {
            return entry->next;
        }
        return node >= 0 && node < static_cast<int>(next.size()) ? next[node] : -1;
    }

    std::size_t bytes() const
    {
        return dist.size() * sizeof(float) + next.size() * sizeof(int) + patch.size() * sizeof(FieldPatchEntry);
    }
};

struct ComponentGraph
{
    std::vector<int> component{};
    int component_count = 0;
    RouteAdjacency dag{};
};

// Strongly connected components of the base, with the temporary nodes as extra vertices of its DAG. When a borrowed
// slot is not bridged by its temporary node, the base components may not hold, and the whole graph is used instead.
struct StrongComponents
{
    int base_revision = -1;
    ComponentGraph base{};
    int graph_version = -1;
    bool use_full = false;
    ComponentGraph full{};
    std::vector<int> temporary_vertex{};
    // (component, temporary vertex) for every link from a persistent node to a temporary one.
    std::vector<std::pair<int, int>> links_to_temporary{};
    std::vector<int> visit_generation{};
    int visit_counter = 0;
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

struct CachedDistanceField
{
    int graph_version = -1;
    uint64_t last_use = 0;
    BotWaypointDistanceField field{};
};

RouteGraph g_route_graph{};
// Fields depend only on the graph, so all bots share them.
std::array<CachedDistanceField, 8> g_distance_fields{};
uint64_t g_distance_field_use_counter = 0;
std::vector<std::unique_ptr<DestinationField>> g_destination_fields{};
uint64_t g_destination_field_use_counter = 0;
// Destination field updates: the values of the nodes an update set, stamped per update.
std::vector<int> g_field_stamp{};
std::vector<float> g_field_dist{};
std::vector<int> g_field_next{};
std::vector<int> g_field_touched{};
int g_field_stamp_counter = 0;
// Whether a persistent node is beside a vehicle road (-1 not known yet). Persistent nodes keep their index and position
// while the persistent version holds, and the roads come from the vehicle layer.
std::vector<int8_t> g_road_node_state{};
int g_road_node_state_persistent_version = -1;
int g_road_node_state_road_version = -1;
std::array<int, kRecentRouteDestinations> g_recent_route_destinations{};
int g_recent_route_destination_next = 0;
int g_route_source = 0;
std::vector<int> g_route_source_avoidset{};
int g_route_source_run = 0;
StrongComponents g_strong_components{};

std::vector<int> g_route_search_generation{};
std::vector<int> g_route_closed_generation{};
std::vector<int> g_route_prev{};
std::vector<float> g_route_cur_score{};
int g_route_generation_counter = 1;

std::vector<int> g_waypoint_components{};
int g_waypoint_components_graph_version = 0;
bool g_waypoint_components_dirty = true;

void collect_route_edges(
    const RouteGraph& graph,
    const int from,
    const std::array<int, kMaxWaypointLinks>& links,
    const int num_links,
    std::vector<RouteEdge>& out)
{
    if (!graph.has_pos[from]) {
        return;
    }
    for (int i = 0; i < num_links; ++i) {
        const int to = links[i];
        if (to <= 0 || to >= graph.waypoint_total || !graph.has_pos[to] || !bot_nav_link_allows_route(from, to)) {
            continue;
        }
        out.push_back({to, (graph.pos[to] - graph.pos[from]).len()});
    }
}

void transpose_adjacency(const RouteAdjacency& out, RouteAdjacency& in)
{
    const int size = out.size();
    in.begin.assign(static_cast<std::size_t>(size) + 1, 0);
    for (const RouteEdge& edge : out.edges) {
        ++in.begin[edge.node + 1];
    }
    for (int i = 0; i < size; ++i) {
        in.begin[i + 1] += in.begin[i];
    }
    in.edges.resize(out.edges.size());
    std::vector<int> fill(in.begin.begin(), in.begin.end() - 1);
    for (int from = 0; from < size; ++from) {
        for (const RouteEdge& edge : out.of(from)) {
            in.edges[fill[edge.node]++] = {from, edge.cost};
        }
    }
}

void rebuild_route_graph_base(RouteGraph& graph)
{
    const int total = graph.waypoint_total;
    graph.pos.assign(total, {});
    graph.has_pos.assign(total, 0);
    int base_total = 1;
    for (int i = 1; i < total; ++i) {
        graph.has_pos[i] = waypoints_get_pos(i, graph.pos[i]);
        bool temporary = false;
        if (graph.has_pos[i] && waypoints_get_temporary(i, temporary) && !temporary) {
            base_total = i + 1;
        }
    }

    RouteAdjacency base_out{};
    base_out.begin.reserve(static_cast<std::size_t>(base_total) + 1);
    std::array<int, kMaxWaypointLinks> links{};
    for (int i = 0; i < base_total; ++i) {
        base_out.begin.push_back(static_cast<int>(base_out.edges.size()));
        if (i > 0) {
            collect_route_edges(graph, i, links, waypoints_get_persistent_links(i, links), base_out.edges);
        }
    }
    base_out.begin.push_back(static_cast<int>(base_out.edges.size()));

    if (base_out.begin != graph.base_out.begin || base_out.edges != graph.base_out.edges) {
        ++graph.base_revision;
        graph.base_out = std::move(base_out);
        transpose_adjacency(graph.base_out, graph.base_in);
    }
}

void refresh_route_graph_overlay(RouteGraph& graph, const bool base_rebuilt)
{
    const int total = graph.waypoint_total;
    const int previous_total = graph.overlay_total;
    graph.overlay_total = total;
    std::swap(graph.out_override, graph.previous_out_override);
    std::swap(graph.override_out_lists, graph.previous_out_lists);
    std::swap(graph.override_out_links, graph.previous_out_links);
    std::swap(graph.temporary, graph.previous_temporary);
    const int previous_out_count = graph.override_out_count;
    std::vector<rf::Vector3> previous_temporary_pos{};
    for (const int node : graph.previous_temporary) {
        previous_temporary_pos.push_back(node < static_cast<int>(graph.pos.size()) ? graph.pos[node] : rf::Vector3{});
    }

    std::vector<int> linking_persistent{};
    waypoints_get_temporary_overlay(graph.temporary, linking_persistent);

    graph.pos.resize(total);
    graph.has_pos.resize(total, 0);
    graph.temporary_flag.assign(total, 0);
    graph.out_override.assign(total, -1);
    graph.in_override.assign(total, -1);
    graph.override_out_count = 0;
    graph.override_in_count = 0;
    graph.lent_links.clear();
    for (const int node : graph.temporary) {
        graph.temporary_flag[node] = 1;
        graph.has_pos[node] = waypoints_get_pos(node, graph.pos[node]);
    }

    // No node removed or renumbered: the same index is the same node, so unchanged links keep their route edges. A
    // temporary removed and re-added at the same spot between two refreshes passes as the same node; the re-adds that
    // happen (a hull's boarding node) put the same kind of node back.
    bool stable = !base_rebuilt && total >= previous_total;
    for (std::size_t i = 0; stable && i < graph.previous_temporary.size(); ++i) {
        const int node = graph.previous_temporary[i];
        const rf::Vector3& pos = graph.pos[node];
        stable = graph.is_temporary(node) && pos.x == previous_temporary_pos[i].x
            && pos.y == previous_temporary_pos[i].y && pos.z == previous_temporary_pos[i].z;
    }
    const auto previous_index = [&](const int node) {
        return node < static_cast<int>(graph.previous_out_override.size())
                && graph.previous_out_override[node] < previous_out_count
            ? graph.previous_out_override[node]
            : -1;
    };

    std::array<int, kMaxWaypointLinks> links{};
    const auto override_out = [&](const int node) {
        const int index = graph.override_out_count++;
        if (index == static_cast<int>(graph.override_out_lists.size())) {
            graph.override_out_lists.emplace_back();
            graph.override_out_links.emplace_back();
        }
        graph.out_override[node] = index;
        std::vector<RouteEdge>& list = graph.override_out_lists[index];
        std::array<int, kMaxWaypointLinks + 1>& raw = graph.override_out_links[index];
        raw[0] = waypoints_get_links(node, links);
        std::copy(links.begin(), links.end(), raw.begin() + 1);
        const int previous = stable ? previous_index(node) : -1;
        list.clear();
        if (previous >= 0 && graph.previous_out_links[previous] == raw) {
            list = graph.previous_out_lists[previous];
        }
        else {
            collect_route_edges(graph, node, links, raw[0], list);
        }
        return std::span<const RouteEdge>{list};
    };
    // Current links with a temporary end, as (to, {from, cost}).
    std::vector<std::pair<int, RouteEdge>> added_in{};
    for (const int node : graph.temporary) {
        for (const RouteEdge& edge : override_out(node)) {
            added_in.push_back({edge.node, {node, edge.cost}});
        }
    }
    for (const int node : linking_persistent) {
        const std::span<const RouteEdge> current = override_out(node);
        for (const RouteEdge& edge : current) {
            if (graph.is_temporary(edge.node)) {
                added_in.push_back({edge.node, {node, edge.cost}});
            }
        }
        for (const RouteEdge& edge : graph.base_out.of(node)) {
            if (std::none_of(current.begin(), current.end(), [&](const RouteEdge& e) { return e.node == edge.node; })) {
                graph.lent_links.push_back({node, edge.node});
            }
        }
    }

    std::vector<int> receivers{};
    for (const auto& [to, edge] : added_in) {
        receivers.push_back(to);
    }
    for (const auto& [from, to] : graph.lent_links) {
        receivers.push_back(to);
    }
    std::sort(receivers.begin(), receivers.end());
    receivers.erase(std::unique(receivers.begin(), receivers.end()), receivers.end());
    std::stable_sort(added_in.begin(), added_in.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.first < rhs.first;
    });
    std::vector<std::pair<int, int>> sorted_lent = graph.lent_links;
    std::sort(sorted_lent.begin(), sorted_lent.end());
    for (const int node : receivers) {
        graph.in_override[node] = graph.override_in_count;
        if (graph.override_in_count == static_cast<int>(graph.override_in_lists.size())) {
            graph.override_in_lists.emplace_back();
        }
        std::vector<RouteEdge>& list = graph.override_in_lists[graph.override_in_count++];
        list.clear();
        for (const RouteEdge& edge : graph.base_in.of(node)) {
            if (!std::binary_search(sorted_lent.begin(), sorted_lent.end(), std::pair<int, int>{edge.node, node})) {
                list.push_back(edge);
            }
        }
        auto it = std::lower_bound(added_in.begin(), added_in.end(), node, [](const auto& entry, int n) {
            return entry.first < n;
        });
        for (; it != added_in.end() && it->first == node; ++it) {
            list.push_back(it->second);
        }
    }

    if (!stable || graph.overlay_gained.size() + graph.overlay_lost.size() > kMaxOverlayChanges) {
        graph.overlay_epoch = graph.graph_version;
        graph.overlay_gained.clear();
        graph.overlay_lost.clear();
        return;
    }
    const auto previous_out = [&](const int node) -> std::span<const RouteEdge> {
        const int previous = previous_index(node);
        return previous >= 0 ? graph.previous_out_lists[previous] : graph.base_out.of(node);
    };
    const auto contains = [](std::span<const RouteEdge> list, const RouteEdge& edge) {
        return std::find(list.begin(), list.end(), edge) != list.end();
    };
    const auto log_changes = [&](const int node) {
        const std::span<const RouteEdge> current = graph.out_edges(node);
        const std::span<const RouteEdge> previous = previous_out(node);
        if (std::any_of(current.begin(), current.end(), [&](const RouteEdge& e) { return !contains(previous, e); })) {
            graph.overlay_gained.push_back({graph.graph_version, node});
        }
        for (const RouteEdge& edge : previous) {
            if (!contains(current, edge)) {
                graph.overlay_lost.push_back({graph.graph_version, {node, edge.node}});
            }
        }
    };
    for (int node = 0; node < total; ++node) {
        if (graph.out_override[node] >= 0 || previous_index(node) >= 0) {
            log_changes(node);
        }
    }
}

const RouteGraph& route_graph()
{
    RouteGraph& graph = g_route_graph;
    const int graph_version = waypoints_graph_version();
    const int total = waypoints_count();
    if (graph.graph_version == graph_version && graph.waypoint_total == total) {
        return graph;
    }
    graph.graph_version = graph_version;
    graph.waypoint_total = total;
    const int persistent_version = waypoints_persistent_graph_version();
    const bool base_rebuilt = graph.persistent_version != persistent_version;
    if (base_rebuilt) {
        graph.persistent_version = persistent_version;
        const int base_revision = graph.base_revision;
        rebuild_route_graph_base(graph);
        // Every field depends on the base, and a new level starts a new one.
        if (graph.base_revision != base_revision) {
            g_destination_fields.clear();
        }
    }
    refresh_route_graph_overlay(graph, base_rebuilt);
    return graph;
}

void ensure_route_search_buffers(const std::size_t waypoint_count)
{
    if (g_route_search_generation.size() != waypoint_count) {
        g_route_search_generation.assign(waypoint_count, 0);
        g_route_closed_generation.assign(waypoint_count, 0);
        g_route_prev.assign(waypoint_count, -1);
        g_route_cur_score.assign(waypoint_count, kInfinity);
        g_route_generation_counter = 1;
    }
}

void reset_route_search_generations()
{
    std::fill(g_route_search_generation.begin(), g_route_search_generation.end(), 0);
    std::fill(g_route_closed_generation.begin(), g_route_closed_generation.end(), 0);
    g_route_generation_counter = 1;
}

bool node_near_vehicle_road(const RouteGraph& graph, const int node)
{
    if (graph.is_temporary(node)) {
        return bot_vehicle_foot_node_near_road(graph.pos[node]);
    }
    const int road_version = bot_vehicle_nav_road_version();
    if (g_road_node_state_persistent_version != graph.persistent_version
        || g_road_node_state_road_version != road_version) {
        g_road_node_state.clear();
        g_road_node_state_persistent_version = graph.persistent_version;
        g_road_node_state_road_version = road_version;
    }
    if (node >= static_cast<int>(g_road_node_state.size())) {
        g_road_node_state.resize(graph.waypoint_total, -1);
    }
    int8_t& state = g_road_node_state[node];
    if (state < 0) {
        state = bot_vehicle_foot_node_near_road(graph.pos[node]) ? 1 : 0;
    }
    return state > 0;
}

// A*. The heuristic is the straight-line distance, or with a destination field the goal distance ignoring the avoid
// set (exact without one). Neither drops by more than a link's cost, and avoid_roads only adds cost, so a closed node
// is final and routes stay shortest.
bool search_waypoint_route(
    const RouteGraph& graph,
    const int from,
    const int to,
    const std::vector<int>& avoidset,
    const DestinationField* field,
    const bool avoid_roads,
    std::vector<int>& out_path)
{
    out_path.clear();
    const rf::Vector3& to_pos = graph.pos[to];
    const auto heuristic = [&](const int node) {
        return field ? field->dist_of(node) : (graph.pos[node] - to_pos).len();
    };

    ensure_route_search_buffers(static_cast<std::size_t>(graph.waypoint_total));
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
        g_route_cur_score[index] = kInfinity;
    };

    std::priority_queue<NodeEntry> open{};
    ensure_node_initialized(from);
    g_route_cur_score[from] = 0.0f;
    open.push({from, heuristic(from)});

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

        for (const RouteEdge& edge : graph.out_edges(current)) {
            const int neighbor = edge.node;
            if (std::binary_search(avoidset.begin(), avoidset.end(), neighbor)) {
                continue;
            }
            if (g_route_closed_generation[neighbor] == generation) {
                continue;
            }

            ensure_node_initialized(neighbor);
            float tentative = current_score + edge.cost;
            if (avoid_roads && node_near_vehicle_road(graph, neighbor)) {
                tentative += kVehicleRoadNodeRouteCost;
            }
            if (tentative < g_route_cur_score[neighbor]) {
                const float remaining = heuristic(neighbor);
                if (!std::isfinite(remaining)) {
                    continue;
                }
                g_route_prev[neighbor] = current;
                g_route_cur_score[neighbor] = tentative;
                open.push({neighbor, tentative + remaining});
            }
        }
    }

    if (g_route_search_generation[to] != generation || g_route_prev[to] == -1) {
        return false;
    }

    int current = to;
    int safety_counter = graph.waypoint_total + 1;
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

// Reverse Dijkstra from the seeds already in open, dist and next, over in_edges_of(node).
template<typename InEdges>
void run_reverse_dijkstra(
    std::priority_queue<NodeEntry>& open, std::vector<float>& dist, std::vector<int>& next, InEdges in_edges_of)
{
    while (!open.empty()) {
        const NodeEntry entry = open.top();
        open.pop();
        if (entry.score > dist[entry.index]) {
            continue;
        }
        for (const RouteEdge& edge : in_edges_of(entry.index)) {
            const float tentative = entry.score + edge.cost;
            if (tentative < dist[edge.node]) {
                dist[edge.node] = tentative;
                next[edge.node] = entry.index;
                open.push({edge.node, tentative});
            }
        }
    }
}

// Reverse Dijkstra over the base, from the goal or from a temporary goal's persistent in-links.
void compute_goal_distances(const RouteGraph& graph, DestinationField& field)
{
    field.base_revision = graph.base_revision;
    field.arrays_cover_graph = false;
    field.graph_version = -1;
    field.patch.clear();
    const int size = graph.base_in.size();
    std::vector<float>& dist = field.dist;
    std::vector<int>& next = field.next;
    dist.assign(static_cast<std::size_t>(size), kInfinity);
    next.assign(static_cast<std::size_t>(size), -1);
    std::priority_queue<NodeEntry> open{};
    if (field.goal_in_links.empty()) {
        if (field.goal <= 0 || field.goal >= size) {
            return;
        }
        dist[field.goal] = 0.0f;
        open.push({field.goal, 0.0f});
    }
    for (const RouteEdge& link : field.goal_in_links) {
        if (link.node < size && link.cost < dist[link.node]) {
            dist[link.node] = link.cost;
            next[link.node] = field.goal;
            open.push({link.node, link.cost});
        }
    }
    run_reverse_dijkstra(open, dist, next, [&graph](const int node) { return graph.base_in.of(node); });
}

// Reverse Dijkstra over the whole current graph: the fallback when patching the base would cost more.
void compute_current_goal_distances(const RouteGraph& graph, DestinationField& field)
{
    const int size = graph.waypoint_total;
    field.base_revision = graph.base_revision;
    field.arrays_cover_graph = true;
    field.goal_in_links.clear();
    field.graph_version = graph.graph_version;
    field.patch.clear();
    field.dist.assign(static_cast<std::size_t>(size), kInfinity);
    field.next.assign(static_cast<std::size_t>(size), -1);
    if (field.goal <= 0 || field.goal >= size) {
        return;
    }
    std::priority_queue<NodeEntry> open{};
    field.dist[field.goal] = 0.0f;
    open.push({field.goal, 0.0f});
    run_reverse_dijkstra(open, field.dist, field.next, [&graph](const int node) { return graph.in_edges(node); });
}

// Brings a field to the current graph. Nodes whose route uses one of the removed links are reset, then the seeds and
// the reset nodes are re-evaluated from their out-links and every decrease is passed on over in-links, so the search
// only visits nodes whose value differs from where it starts; finding the reset nodes walks every node's route.
// from_base starts over from the base arrays, with every temporary node a seed. Returns false, leaving the field
// stale, once the work passes a share of a fresh field.
bool update_destination_field(
    const RouteGraph& graph,
    DestinationField& field,
    std::vector<int> seeds,
    const std::vector<std::pair<int, int>>& removed_links,
    const bool from_base,
    const int max_settled)
{
    const int total = graph.waypoint_total;
    if (static_cast<int>(g_field_stamp.size()) != total || g_field_stamp_counter == std::numeric_limits<int>::max()) {
        g_field_stamp.assign(total, 0);
        g_field_dist.resize(total);
        g_field_next.resize(total);
        g_field_stamp_counter = 0;
    }
    const int current_stamp = ++g_field_stamp_counter;
    g_field_touched.clear();
    if (from_base) {
        field.patch.clear();
    }
    const auto value = [&](const int node) {
        return g_field_stamp[node] == current_stamp ? g_field_dist[node] : field.dist_of(node);
    };
    const auto set_value = [&](const int node, const float dist, const int next) {
        if (g_field_stamp[node] != current_stamp) {
            g_field_stamp[node] = current_stamp;
            g_field_touched.push_back(node);
        }
        g_field_dist[node] = dist;
        g_field_next[node] = next;
    };

    if (from_base) {
        if (graph.is_temporary(field.goal)) {
            set_value(field.goal, 0.0f, -1);
        }
        seeds.insert(seeds.end(), graph.temporary.begin(), graph.temporary.end());
    }
    const auto routes_through = [&](const std::pair<int, int>& link) {
        return link.first < total && field.next_of(link.first) == link.second;
    };
    if (std::any_of(removed_links.begin(), removed_links.end(), routes_through)) {
        // 1 = reaches the goal through a removed link, 2 = does not.
        std::vector<uint8_t> state(static_cast<std::size_t>(total), 0);
        for (const auto& link : removed_links) {
            if (routes_through(link)) {
                state[link.first] = 1;
            }
        }
        std::vector<int> chain{};
        for (int node = 1; node < total; ++node) {
            int current = node;
            while (current >= 0 && current < total && state[current] == 0 && std::cmp_less(chain.size(), total)) {
                chain.push_back(current);
                current = field.next_of(current);
            }
            // A cycle in the next links (a corrupt field) cannot be walked out; recompute what it holds.
            const bool cycle = std::cmp_greater_equal(chain.size(), total);
            const uint8_t result = cycle ? 1 : (current >= 0 && current < total ? state[current] : 2);
            for (const int chained : chain) {
                state[chained] = result;
            }
            chain.clear();
        }
        for (int node = 1; node < total; ++node) {
            if (state[node] == 1) {
                set_value(node, kInfinity, -1);
                seeds.push_back(node);
            }
        }
    }

    std::priority_queue<NodeEntry> open{};
    for (const int node : seeds) {
        float best = kInfinity;
        int best_next = -1;
        for (const RouteEdge& edge : graph.out_edges(node)) {
            const float candidate = value(edge.node) + edge.cost;
            if (candidate < best) {
                best = candidate;
                best_next = edge.node;
            }
        }
        if (best < value(node)) {
            set_value(node, best, best_next);
            open.push({node, best});
        }
    }
    int settled = 0;
    while (!open.empty()) {
        const NodeEntry entry = open.top();
        open.pop();
        if (entry.score > value(entry.index)) {
            continue;
        }
        if (++settled > max_settled) {
            return false;
        }
        for (const RouteEdge& edge : graph.in_edges(entry.index)) {
            const float tentative = entry.score + edge.cost;
            if (tentative < value(edge.node)) {
                set_value(edge.node, tentative, entry.index);
                open.push({edge.node, tentative});
            }
        }
    }

    std::sort(g_field_touched.begin(), g_field_touched.end());
    std::vector<FieldPatchEntry> patch{};
    patch.reserve(field.patch.size() + g_field_touched.size());
    auto old_entry = field.patch.begin();
    for (const int node : g_field_touched) {
        for (; old_entry != field.patch.end() && old_entry->node < node; ++old_entry) {
            patch.push_back(*old_entry);
        }
        if (old_entry != field.patch.end() && old_entry->node == node) {
            ++old_entry;
        }
        patch.push_back({node, g_field_dist[node], g_field_next[node]});
    }
    patch.insert(patch.end(), old_entry, field.patch.end());
    field.patch = std::move(patch);
    field.graph_version = graph.graph_version;

    // A large patch costs more to search and to store than plain arrays.
    if (field.patch.size() > static_cast<std::size_t>(total / 8)) {
        field.dist.resize(total, kInfinity);
        field.next.resize(total, -1);
        for (const FieldPatchEntry& entry : field.patch) {
            field.dist[entry.node] = entry.dist;
            field.next[entry.node] = entry.next;
        }
        field.patch.clear();
        field.arrays_cover_graph = true;
        field.goal_in_links.clear();
    }
    return true;
}

// A field is advanced through the logged overlay changes while no node is removed or renumbered (craters, borrowed
// slots). It is re-patched from the base once the log no longer reaches the field's version (a removal, a renumbering,
// a log reset), and computed afresh over the whole graph once patching costs more than that; a field already in that
// form is recomputed afresh then, until the base changes.
void refresh_destination_field(const RouteGraph& graph, DestinationField& field)
{
    if (field.base_revision == graph.base_revision && field.graph_version == graph.graph_version) {
        return;
    }
    const int max_settled = std::max(graph.waypoint_total / 4, 64);
    if (field.base_revision == graph.base_revision && field.graph_version >= graph.overlay_epoch) {
        std::vector<int> seeds{};
        for (const auto& [graph_version, node] : graph.overlay_gained) {
            if (graph_version > field.graph_version) {
                seeds.push_back(node);
            }
        }
        std::vector<std::pair<int, int>> removed_links{};
        for (const auto& [graph_version, link] : graph.overlay_lost) {
            if (graph_version > field.graph_version) {
                removed_links.push_back(link);
            }
        }
        if (update_destination_field(graph, field, std::move(seeds), removed_links, false, max_settled)) {
            return;
        }
    }
    else if (field.base_revision != graph.base_revision || !field.arrays_cover_graph) {
        std::vector<RouteEdge> goal_in_links{};
        if (graph.is_temporary(field.goal)) {
            for (const RouteEdge& edge : graph.in_edges(field.goal)) {
                if (!graph.is_temporary(edge.node)) {
                    goal_in_links.push_back(edge);
                }
            }
        }
        if (field.base_revision != graph.base_revision || field.arrays_cover_graph
            || field.goal_in_links != goal_in_links) {
            field.goal_in_links = std::move(goal_in_links);
            compute_goal_distances(graph, field);
        }
        if (update_destination_field(graph, field, {}, graph.lent_links, true, max_settled)) {
            return;
        }
    }
    compute_current_goal_distances(graph, field);
}

bool note_route_destination(const int goal)
{
    const bool recent = std::find(g_recent_route_destinations.begin(), g_recent_route_destinations.end(), goal)
        != g_recent_route_destinations.end();
    g_recent_route_destinations[g_recent_route_destination_next] = goal;
    g_recent_route_destination_next = (g_recent_route_destination_next + 1) % kRecentRouteDestinations;
    return recent;
}

bool note_route_source(const int from, const std::vector<int>& avoidset)
{
    if (from == g_route_source && avoidset == g_route_source_avoidset) {
        return ++g_route_source_run >= kRouteSourceRunForField;
    }
    g_route_source = from;
    g_route_source_avoidset = avoidset;
    g_route_source_run = 1;
    return false;
}

// Goals are few next to the start waypoints, which change with every step, so the fields bots share are keyed on
// the goal. The least recently used are dropped to keep them, patches included, within the memory budget.
const DestinationField* find_destination_field(const RouteGraph& graph, const int goal, const bool create)
{
    DestinationField* field = nullptr;
    for (const auto& entry : g_destination_fields) {
        if (entry->goal == goal) {
            field = entry.get();
            break;
        }
    }
    if (!field) {
        if (!create) {
            return nullptr;
        }
        field = g_destination_fields.emplace_back(std::make_unique<DestinationField>()).get();
        field->goal = goal;
    }
    refresh_destination_field(graph, *field);
    field->last_use = ++g_destination_field_use_counter;

    std::size_t bytes = 0;
    for (const auto& entry : g_destination_fields) {
        bytes += entry->bytes();
    }
    while (g_destination_fields.size() > 1
        && (bytes > kDestinationFieldBudgetBytes || g_destination_fields.size() > kMaxDestinationFields)) {
        auto victim = g_destination_fields.end();
        for (auto it = g_destination_fields.begin(); it != g_destination_fields.end(); ++it) {
            if (it->get() != field && (victim == g_destination_fields.end() || (*it)->last_use < (*victim)->last_use)) {
                victim = it;
            }
        }
        bytes -= (*victim)->bytes();
        g_destination_fields.erase(victim);
    }
    return field;
}

bool destination_field_path(const DestinationField& field, const int from, const int total, std::vector<int>& out_path)
{
    out_path.clear();
    int current = from;
    for (int guard = 0; guard <= total && current > 0; ++guard) {
        out_path.push_back(current);
        if (current == field.goal) {
            return true;
        }
        current = field.next_of(current);
    }
    out_path.clear();
    return false;
}

const BotWaypointDistanceField* find_cached_distance_field(const int source, const std::vector<int>& avoidset)
{
    const int graph_version = waypoints_graph_version();
    for (CachedDistanceField& entry : g_distance_fields) {
        if (entry.graph_version == graph_version && entry.field.source == source && entry.field.avoidset == avoidset) {
            entry.last_use = ++g_distance_field_use_counter;
            return &entry.field;
        }
    }
    return nullptr;
}

bool find_waypoint_route(const int from, const int to, const std::vector<int>& avoidset, std::vector<int>& out_path)
{
    out_path.clear();

    const RouteGraph& graph = route_graph();
    if (from <= 0 || to <= 0 || from >= graph.waypoint_total || to >= graph.waypoint_total) {
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
    if (!graph.has_pos[from] || !graph.has_pos[to]) {
        return false;
    }
    // The road cost is this bot's alone, so the shared fields cannot answer.
    const rf::Timestamp& road_avoid_timer = bot_state().vehicle_road_avoid_timer;
    if (road_avoid_timer.valid() && !road_avoid_timer.elapsed()) {
        return search_waypoint_route(graph, from, to, avoidset, find_destination_field(graph, to, false), true,
            out_path);
    }
    // A bot routes from where it just evaluated its goals, so the field from its start is often at hand.
    if (const BotWaypointDistanceField* start_field = find_cached_distance_field(from, avoidset)) {
        return bot_waypoint_distance_field_path(*start_field, to, out_path);
    }

    const bool hot_destination = note_route_destination(to);
    const bool source_run = note_route_source(from, avoidset);
    const DestinationField* field = find_destination_field(graph, to, false);
    if (!field && source_run) {
        return bot_waypoint_distance_field_path(bot_waypoint_distance_field(from, avoidset), to, out_path);
    }
    if (!field) {
        field = find_destination_field(graph, to, hot_destination);
    }
    if (!field) {
        return search_waypoint_route(graph, from, to, avoidset, nullptr, false, out_path);
    }
    if (!std::isfinite(field->dist_of(from))) {
        return false;
    }
    if (destination_field_path(*field, from, graph.waypoint_total, out_path)
        && std::none_of(out_path.begin(), out_path.end(), [&](int node) {
               return std::binary_search(avoidset.begin(), avoidset.end(), node);
           })) {
        return true;
    }
    return search_waypoint_route(graph, from, to, avoidset, field, false, out_path);
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

// Tarjan's algorithm, iterative, plus the edges between components.
template<typename OutEdges>
void compute_strong_components(const int size, const std::vector<uint8_t>& has_pos, const OutEdges& out_edges,
    ComponentGraph& out)
{
    out.component.assign(size, -1);
    std::vector<int> order(size, -1);
    std::vector<int> low(size, 0);
    std::vector<uint8_t> on_stack(size, 0);
    std::vector<int> stack{};
    std::vector<std::pair<int, int>> calls{};
    int counter = 0;
    int component_count = 0;
    for (int root = 1; root < size; ++root) {
        if (order[root] != -1 || !has_pos[root]) {
            continue;
        }
        order[root] = low[root] = counter++;
        stack.push_back(root);
        on_stack[root] = 1;
        calls.push_back({root, 0});
        while (!calls.empty()) {
            const int node = calls.back().first;
            const std::span<const RouteEdge> edges = out_edges(node);
            const int edge_index = calls.back().second;
            if (edge_index < static_cast<int>(edges.size())) {
                ++calls.back().second;
                const int neighbor = edges[edge_index].node;
                if (order[neighbor] == -1) {
                    order[neighbor] = low[neighbor] = counter++;
                    stack.push_back(neighbor);
                    on_stack[neighbor] = 1;
                    calls.push_back({neighbor, 0});
                }
                else if (on_stack[neighbor]) {
                    low[node] = std::min(low[node], order[neighbor]);
                }
                continue;
            }
            if (low[node] == order[node]) {
                int member = 0;
                do {
                    member = stack.back();
                    stack.pop_back();
                    on_stack[member] = 0;
                    out.component[member] = component_count;
                } while (member != node);
                ++component_count;
            }
            calls.pop_back();
            if (!calls.empty()) {
                const int parent = calls.back().first;
                low[parent] = std::min(low[parent], low[node]);
            }
        }
    }

    std::vector<std::pair<int, int>> dag_links{};
    for (int node = 1; node < size; ++node) {
        for (const RouteEdge& edge : out_edges(node)) {
            if (out.component[node] != out.component[edge.node]) {
                dag_links.push_back({out.component[node], out.component[edge.node]});
            }
        }
    }
    std::sort(dag_links.begin(), dag_links.end());
    dag_links.erase(std::unique(dag_links.begin(), dag_links.end()), dag_links.end());
    out.component_count = component_count;
    out.dag.begin.assign(static_cast<std::size_t>(component_count) + 1, 0);
    out.dag.edges.clear();
    for (const auto& [from, to] : dag_links) {
        ++out.dag.begin[from + 1];
        out.dag.edges.push_back({to, 0.0f});
    }
    for (int i = 0; i < component_count; ++i) {
        out.dag.begin[i + 1] += out.dag.begin[i];
    }
}

bool lent_link_bridged(const RouteGraph& graph, const std::pair<int, int>& link)
{
    for (const RouteEdge& edge : graph.out_edges(link.first)) {
        if (!graph.is_temporary(edge.node)) {
            continue;
        }
        const std::span<const RouteEdge> onward = graph.out_edges(edge.node);
        if (std::any_of(onward.begin(), onward.end(), [&](const RouteEdge& e) { return e.node == link.second; })) {
            return true;
        }
    }
    return false;
}

StrongComponents& strong_components(const RouteGraph& graph)
{
    StrongComponents& components = g_strong_components;
    if (components.base_revision != graph.base_revision) {
        components.base_revision = graph.base_revision;
        components.graph_version = -1;
        compute_strong_components(graph.base_out.size(), graph.has_pos,
            [&](int node) { return graph.base_out.of(node); }, components.base);
    }
    if (components.graph_version == graph.graph_version) {
        return components;
    }
    components.graph_version = graph.graph_version;
    components.use_full = !std::all_of(graph.lent_links.begin(), graph.lent_links.end(),
        [&](const std::pair<int, int>& link) { return lent_link_bridged(graph, link); });
    components.links_to_temporary.clear();
    components.temporary_vertex.assign(graph.waypoint_total, -1);
    int vertex_count = components.base.component_count;
    if (components.use_full) {
        compute_strong_components(graph.waypoint_total, graph.has_pos,
            [&](int node) { return graph.out_edges(node); }, components.full);
        vertex_count = components.full.component_count;
    }
    else {
        for (const int node : graph.temporary) {
            components.temporary_vertex[node] = vertex_count++;
        }
        for (int node = 0; node < static_cast<int>(graph.out_override.size()); ++node) {
            if (graph.out_override[node] < 0 || graph.is_temporary(node) || node >= graph.base_out.size()) {
                continue;
            }
            for (const RouteEdge& edge : graph.out_edges(node)) {
                if (graph.is_temporary(edge.node) && components.base.component[node] >= 0) {
                    components.links_to_temporary.push_back(
                        {components.base.component[node], components.temporary_vertex[edge.node]});
                }
            }
        }
        std::sort(components.links_to_temporary.begin(), components.links_to_temporary.end());
    }
    components.visit_generation.assign(static_cast<std::size_t>(vertex_count), 0);
    components.visit_counter = 0;
    return components;
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
        return find_waypoint_route(from, to, avoidset, out_path);
    }
    const uint64_t avoidset_hash = hash_avoidset(avoidset);
    if (is_route_search_backed_off(from, to, avoidset_hash)) {
        out_path.clear();
        return false;
    }
    const bool found = find_waypoint_route(from, to, avoidset, out_path);
    note_route_search_result(from, to, avoidset_hash, found);
    return found;
}

const BotWaypointDistanceField& bot_waypoint_distance_field(const int source_waypoint, const std::vector<int>& avoidset)
{
    if (const BotWaypointDistanceField* field = find_cached_distance_field(source_waypoint, avoidset)) {
        return *field;
    }
    CachedDistanceField& slot = *std::min_element(g_distance_fields.begin(), g_distance_fields.end(),
        [](const CachedDistanceField& lhs, const CachedDistanceField& rhs) { return lhs.last_use < rhs.last_use; });
    bot_waypoint_compute_distance_field(source_waypoint, avoidset, slot.field);
    slot.graph_version = waypoints_graph_version();
    slot.last_use = ++g_distance_field_use_counter;
    return slot.field;
}

void bot_waypoint_compute_distance_field(
    const int from,
    const std::vector<int>& avoidset,
    BotWaypointDistanceField& out)
{
    const RouteGraph& graph = route_graph();
    const int waypoint_total = graph.waypoint_total;
    out.source = from;
    out.avoidset = avoidset;
    out.dist.assign(static_cast<std::size_t>(waypoint_total), kInfinity);
    out.prev.assign(static_cast<std::size_t>(waypoint_total), -1);

    if (from <= 0 || from >= waypoint_total
        || std::binary_search(avoidset.begin(), avoidset.end(), from)
        || !graph.has_pos[from]) {
        return;
    }

    std::priority_queue<NodeEntry> open{};
    out.dist[from] = 0.0f;
    open.push({from, 0.0f});

    while (!open.empty()) {
        const NodeEntry entry = open.top();
        open.pop();
        const int current = entry.index;
        if (entry.score > out.dist[current]) {
            continue;
        }

        for (const RouteEdge& edge : graph.out_edges(current)) {
            const int neighbor = edge.node;
            if (std::binary_search(avoidset.begin(), avoidset.end(), neighbor)) {
                continue;
            }
            const float tentative = entry.score + edge.cost;
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

float bot_waypoint_destination_distance(const int from, const int goal)
{
    const RouteGraph& graph = route_graph();
    if (from <= 0 || goal <= 0 || from >= graph.waypoint_total || goal >= graph.waypoint_total
        || !graph.has_pos[from] || !graph.has_pos[goal]) {
        return kInfinity;
    }
    if (from == goal) {
        return 0.0f;
    }
    // A goal seen once (a moving hull's nearest node) costs one A*; a field is shared once the goal repeats. Goals that
    // have one stay out of the recent list, which route goals share.
    const DestinationField* field = find_destination_field(graph, goal, false);
    if (!field && note_route_destination(goal)) {
        field = find_destination_field(graph, goal, true);
    }
    if (field) {
        return field->dist_of(from);
    }
    std::vector<int> path{};
    return search_waypoint_route(graph, from, goal, {}, nullptr, false, path) ? g_route_cur_score[goal] : kInfinity;
}

bool bot_waypoint_reachable(const int from, const int to)
{
    if (from == to) {
        return true;
    }
    const RouteGraph& graph = route_graph();
    if (from <= 0 || to <= 0 || from >= graph.waypoint_total || to >= graph.waypoint_total
        || !graph.has_pos[from] || !graph.has_pos[to]) {
        return false;
    }
    StrongComponents& components = strong_components(graph);
    const ComponentGraph& component_graph = components.use_full ? components.full : components.base;
    const auto vertex = [&](const int node) {
        if (components.use_full || !graph.is_temporary(node)) {
            return node < static_cast<int>(component_graph.component.size()) ? component_graph.component[node] : -1;
        }
        return components.temporary_vertex[node];
    };
    const int from_vertex = vertex(from);
    const int to_vertex = vertex(to);
    if (from_vertex < 0 || to_vertex < 0) {
        return false;
    }
    if (from_vertex == to_vertex) {
        return true;
    }

    if (++components.visit_counter == std::numeric_limits<int>::max()) {
        std::fill(components.visit_generation.begin(), components.visit_generation.end(), 0);
        components.visit_counter = 1;
    }
    const int visit = components.visit_counter;
    std::vector<int> open{from_vertex};
    components.visit_generation[from_vertex] = visit;
    const auto reach = [&](const int next) {
        if (next < 0 || components.visit_generation[next] == visit) {
            return false;
        }
        components.visit_generation[next] = visit;
        open.push_back(next);
        return next == to_vertex;
    };
    while (!open.empty()) {
        const int current = open.back();
        open.pop_back();
        if (current >= component_graph.component_count) {
            const int node = graph.temporary[current - component_graph.component_count];
            for (const RouteEdge& edge : graph.out_edges(node)) {
                if (reach(vertex(edge.node))) {
                    return true;
                }
            }
            continue;
        }
        for (const RouteEdge& edge : component_graph.dag.of(current)) {
            if (reach(edge.node)) {
                return true;
            }
        }
        const auto [first, last] = std::equal_range(components.links_to_temporary.begin(),
            components.links_to_temporary.end(), std::pair<int, int>{current, 0},
            [](const std::pair<int, int>& lhs, const std::pair<int, int>& rhs) { return lhs.first < rhs.first; });
        for (auto it = first; it != last; ++it) {
            if (reach(it->second)) {
                return true;
            }
        }
    }
    return false;
}

void bot_waypoint_invalidate_components()
{
    g_waypoint_components_dirty = true;
}

static void bot_waypoint_compute_components()
{
    const RouteGraph& graph = route_graph();
    const int waypoint_total = graph.waypoint_total;
    g_waypoint_components.assign(static_cast<std::size_t>(waypoint_total), 0);
    g_waypoint_components_graph_version = graph.graph_version;
    g_waypoint_components_dirty = false;

    if (waypoint_total <= 1) {
        return;
    }

    int next_component = 1;
    std::queue<int> bfs_queue{};

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

            for (const RouteEdge& edge : graph.out_edges(current)) {
                if (g_waypoint_components[edge.node] != 0) {
                    continue;
                }
                g_waypoint_components[edge.node] = component_id;
                bfs_queue.push(edge.node);
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
