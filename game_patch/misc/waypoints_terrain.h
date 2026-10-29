#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>
#include <common/terrain/alpine_terrain.h>
#include "../rf/math/vector.h"

// Whether the level has Alpine terrain a generation run can lay a lattice over.
bool waypoints_level_has_terrain();

// Calls visit(box_min, box_max, liquid_top) with a world box around each level volume a generated graph builds around:
// clutter, mover brushes players collide with and their keyframe sweeps, push regions, climb regions, waypoint zones
// and capture-point triggers. liquid_top: the box ends at the surface of a damaging liquid.
void for_each_waypoint_generation_obstacle(
    const std::function<void(const rf::Vector3& box_min, const rf::Vector3& box_max, bool liquid_top)>& visit);

// The coarse hex lattice one waypoint generation run lays over open Alpine terrain, placed and linked from the
// heightfield and the occupancy masks below, without traces, with a spacing that keeps it within a node budget. Bare
// terrain is solid terrain nothing else occupies: no other level geometry, mover players collide with, clutter, push
// or climb region, waypoint zone, capture-point trigger or seeded node near its surface. Open terrain is bare,
// climbable, not under water and a couple of units from anything that is not.
class WaypointTerrainLattice
{
public:
    // Polled through the long loops with (done, total); true stops the build.
    using OutOfTime = std::function<bool(int done, int total)>;

    // Builds the masks, then appends the lattice nodes to g_waypoints, contiguously, and links them.
    void build(const OutOfTime& out_of_time);
    // Builds only the masks, for adding to a graph an earlier run laid a lattice in: open terrain counts as
    // covered by that lattice.
    void build_for_existing_lattice(const OutOfTime& out_of_time);
    // The lattice covers open terrain in this run (nodes placed now, or by the run that made the graph).
    bool covering() const
    {
        return covering_;
    }
    bool active() const
    {
        return end_ > first_;
    }
    bool is_coarse(int index) const
    {
        return index >= first_ && index < end_;
    }
    int node_count() const
    {
        return end_ - first_;
    }
    float spacing() const
    {
        return spacing_;
    }
    // Lattice nodes missing a link to a hex neighbour; the fine graph joins the lattice through them.
    const std::vector<int>& edge_nodes() const
    {
        return edge_nodes_;
    }
    // From lattice node `index` to the positions of its unlinked hex neighbours, one ground offset up. Returns how
    // many.
    int unlinked_neighbor_offsets(int index, std::array<rf::Vector3, 6>& out) const;
    // pos stands on the open terrain the lattice covers, where a fine node would only duplicate it.
    bool covers(const rf::Vector3& pos) const;
    // pos stands on bare terrain next to a covering lattice.
    bool on_bare_terrain(const rf::Vector3& pos) const;
    // When both points stand on a terrain surface: whether it is the same terrain and the surface between them is
    // solid and stays within the walk incline. True when either does not, since there the generator's traces decide.
    bool ground_walkable(const rf::Vector3& a, const rf::Vector3& b) const;
    // For a link joining the lattice to the fine graph: whether the surface between them is solid and the straight
    // segment stays as close above it as a lattice link does. True unless both points stand on one terrain surface.
    bool join_walkable(const rf::Vector3& a, const rf::Vector3& b) const;
    // pos lies inside solid terrain, below its surface with nothing but terrain above it.
    bool under_terrain(const rf::Vector3& pos) const;
    // Whether a world box reaches over a covering lattice's terrain.
    bool overlaps_terrain(const rf::Vector3& box_min, const rf::Vector3& box_max) const;
    // Moves a probe over bare terrain to one ground offset above the surface, so a probe uphill does not start
    // inside the terrain.
    void lift_probe_to_surface(rf::Vector3& pos) const;

private:
    struct Field
    {
        int terrain = -1;
        alpine_terrain::GridView grid{};
        int cells_x = 0;
        int cells_z = 0;
        // Mask cells per terrain cell along each axis.
        int split = 1;
        float res = 1.0f;
        int mask_x = 0;
        int mask_z = 0;
        // The surface's range over each mask cell.
        std::vector<float> low;
        std::vector<float> high;
        std::vector<std::uint8_t> bare;
        std::vector<std::uint8_t> clean;
        // Lower bound of the distance from any point of the mask cell to any cell that is not clean.
        std::vector<float> clearance;
    };

    struct LatticeNode
    {
        int field = 0;
        int i = 0;
        int j = 0;
        std::uint8_t linked = 0;
    };

    static std::uint64_t lattice_key(int field, int i, int j);
    const Field* field_at(const rf::Vector3& pos, int& mask_cell) const;
    bool solid_at(const Field& f, float x, float z) const;
    bool open(const Field& f, int mask_cell) const;
    bool open_at(const Field& f, float x, float z) const;
    bool on_field_surface(const rf::Vector3& pos, const Field*& out_field, int& out_mask_cell) const;
    float lattice_x(const Field& f, int i, int j) const;
    float lattice_z(const Field& f, int j) const;
    rf::Vector3 lattice_pos(const Field& f, int i, int j) const;
    static void surface_breaks(const Field& f, const rf::Vector3& a, const rf::Vector3& b, std::vector<float>& ts);
    template<typename Sample>
    static bool surface_within_incline(
        const Field& f, const rf::Vector3& a, const rf::Vector3& b, const std::vector<float>& ts, Sample&& sample);
    bool walkable_link(const Field& f, const rf::Vector3& a, const rf::Vector3& b) const;
    bool build_fields();
    bool mark_occupied(bool mark_all_nodes, const OutOfTime& out_of_time);
    void measure_clearance();
    bool choose_spacing();
    int count_lattice_positions(int limit, const OutOfTime& out_of_time) const;
    bool place_nodes(const OutOfTime& out_of_time);
    bool split_link(int field, int from, int to);
    int link_nodes(const OutOfTime& out_of_time);
    bool build_masks(bool mark_all_nodes, const OutOfTime& out_of_time);

    std::vector<Field> fields_;
    std::vector<LatticeNode> nodes_;
    std::unordered_map<std::uint64_t, int> node_by_key_;
    std::vector<int> edge_nodes_;
    float spacing_ = 0.0f;
    int first_ = 0;
    int end_ = 0;
    int split_link_nodes_ = 0;
    bool covering_ = false;
};
