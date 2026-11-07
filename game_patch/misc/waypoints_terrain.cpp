#include "waypoints_terrain.h"
#include "waypoints_internal.h"
#include "alpine_terrain.h"
#include "../multi/gametype.h"
#include "../rf/collide.h"
#include "../rf/event.h"
#include "../rf/geometry.h"
#include "../rf/level.h"
#include "../rf/mover.h"
#include "../rf/object.h"
#include "../rf/trigger.h"
#include <common/utils/list-utils.h>
#include <xlog/xlog.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>

namespace at = alpine_terrain;

namespace
{
constexpr float kMaskCellTargetSize = 1.0f;
// Across all terrains; a mask cell takes 14 bytes.
constexpr std::size_t kMaxMaskCells = std::size_t{1} << 22;
constexpr float kMinSpacing = 4.0f;
constexpr float kMaxSpacing = 16.0f;
// How far build_masks widens the spacing to keep within the node cap; joins to the fine graph span about this much.
constexpr float kMaxCappedSpacing = 32.0f;
constexpr float kSpacingStep = 0.25f;
constexpr int kLatticeNodeBudget = 8000;
constexpr int kMaxLatticeNodes = 2 * kLatticeNodeBudget;
// A hex link over a ridge or a gully is retried as a chain of up to this many links through surface points between.
constexpr int kMaxLinkParts = 4;
// Geometry rising above a mask cell's lowest surface point, up to this far over its highest, is in the way of anyone
// walking it; anything lower is buried under the surface or flush with it.
constexpr float kSurfaceBandAbove = kWaypointGenerateStandingHeadroom + 0.5f;
constexpr float kBuriedTolerance = 0.05f;
constexpr float kSqrt3 = std::numbers::sqrt3_v<float>;
// tan(60 deg): steeper triangles are cliffs. Gentler ones are crossed only along links within the walk incline.
constexpr float kMaxClimbableSlopeRise = kSqrt3;
// How far the lattice keeps from anything that is not clean: the generator's wall and ground-edge clearances, with
// some slack.
constexpr float kLatticeMargin = kWaypointGenerateWallClearance + kWaypointGenerateEdgeClearance + 0.5f;
constexpr float kCoverTolerance = 0.5f;
constexpr float kSurfaceNodeTolerance = 0.1f;
// The straight segment between two lattice nodes must stay above the terrain (the load-time link trace tests it
// against the chunk faces) and close enough over dips for the route midpoint support probe.
constexpr float kLinkMinClearance = 0.1f;
constexpr float kLinkMaxClearance = 2.0f;
// An 8-neighbour chamfer distance overestimates the Euclidean one by at most 1 / cos(22.5 deg).
constexpr float kChamferOverestimate = 1.0824f;
// Where a mask cell's surface range is sampled, in mask cells: the corners and the centre.
constexpr std::array<std::array<float, 2>, 5> kSurfaceSamples{
    {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}, {0.5f, 0.5f}}};

// Directions E, NE, NW, W, SW, SE; odd rows sit half a spacing east, so their diagonal neighbours are one column
// further east.
constexpr std::array<int, 6> kHexStepI{1, 0, -1, -1, -1, 0};
constexpr std::array<int, 6> kHexStepJ{0, 1, 1, 0, -1, -1};

void hex_neighbor(const int i, const int j, const int dir, int& out_i, int& out_j)
{
    out_i = i + kHexStepI[dir] + (kHexStepJ[dir] != 0 ? (j & 1) : 0);
    out_j = j + kHexStepJ[dir];
}

bool cell_climbable(const at::GridView& g, const std::uint32_t x, const std::uint32_t z)
{
    const float h00 = at::top_y(g, x, z), h10 = at::top_y(g, x + 1, z);
    const float h01 = at::top_y(g, x, z + 1), h11 = at::top_y(g, x + 1, z + 1);
    const float limit = kMaxClimbableSlopeRise * g.cell_size;
    auto steep = [limit](const float dx, const float dz) { return dx * dx + dz * dz > limit * limit; };
    if (!at::get_cell_bit(g.diag, at::cells(g.nx), x, z)) {
        return !steep(h10 - h00, h11 - h10) && !steep(h11 - h01, h01 - h00);
    }
    return !steep(h10 - h00, h01 - h00) && !steep(h11 - h01, h11 - h10);
}

// A world box around what a waypoint zone covers.
bool zone_bounds(const WaypointZoneDefinition& zone, rf::Vector3& out_min, rf::Vector3& out_max, bool& out_liquid_top)
{
    out_liquid_top = false;
    switch (resolve_waypoint_zone_source(zone)) {
    case WaypointZoneSource::trigger_uid: {
        const rf::Object* obj = rf::obj_lookup_from_uid(zone.trigger_uid);
        if (!obj || obj->type != rf::OT_TRIGGER) {
            return false;
        }
        const auto& trigger = *static_cast<const rf::Trigger*>(obj);
        const float reach = trigger_bounding_radius(trigger);
        out_min = trigger.pos - rf::Vector3{reach, reach, reach};
        out_max = trigger.pos + rf::Vector3{reach, reach, reach};
        return true;
    }
    case WaypointZoneSource::room_uid: {
        const rf::GRoom* room = rf::level_room_from_uid(zone.room_uid);
        if (!room) {
            return false;
        }
        out_min = room->bbox_min;
        out_max = room->bbox_max;
        // The hazard is the liquid; the room above its surface is often a map's open ground.
        if (zone.type == WaypointZoneType::damaging_liquid_room && room->contains_liquid) {
            out_max.y = std::min(out_max.y, room->bbox_min.y + room->liquid_depth);
            out_liquid_top = true;
        }
        return true;
    }
    case WaypointZoneSource::box_extents:
        out_min = zone.box_min;
        out_max = zone.box_max;
        return true;
    default:
        return false;
    }
}
} // namespace

bool waypoints_level_has_terrain()
{
    const auto& terrains = alpine_terrain_get_all();
    return std::any_of(terrains.begin(), terrains.end(), [](const AlpineTerrain& t) { return t.resolved; });
}

void for_each_waypoint_generation_obstacle(
    const std::function<void(const rf::Vector3& box_min, const rf::Vector3& box_max, bool liquid_top)>& visit)
{
    auto visit_sphere = [&visit](const rf::Vector3& center, const float radius) {
        const rf::Vector3 reach{radius, radius, radius};
        visit(center - reach, center + reach, false);
    };
    for (rf::Object* obj = rf::object_list.next_obj; obj != &rf::object_list; obj = obj->next_obj) {
        if (obj->type == rf::OT_CLUTTER) {
            visit_sphere(obj->pos, obj->radius);
        }
        else if (obj->type == rf::OT_MOVER) {
            const auto& mover = *static_cast<const rf::Mover*>(obj);
            for (int b = 0; b < mover.brush_handles.size(); ++b) {
                const rf::Object* brush = rf::obj_from_handle(mover.brush_handles[b]);
                if (!brush || mover_brush_passes_through(*brush)) {
                    continue;
                }
                const auto* solid = static_cast<const rf::MoverBrush*>(brush)->geometry;
                const float radius = std::max(
                    brush->radius, solid ? (solid->bbox_max - solid->bbox_min).len() * 0.5f : 0.0f);
                const rf::Vector3 offset = brush->pos - mover.pos;
                for (int k = 0; k < mover.keyframes.size(); ++k) {
                    if (mover.keyframes[k]) {
                        visit_sphere(mover.keyframes[k]->pos + offset, radius);
                    }
                    // The brush sweeps between keyframes, so the box between consecutive ones is in the way too.
                    if (k > 0 && mover.keyframes[k] && mover.keyframes[k - 1]) {
                        const rf::Vector3 reach{radius, radius, radius};
                        const rf::Vector3 p0 = mover.keyframes[k - 1]->pos + offset;
                        const rf::Vector3 p1 = mover.keyframes[k]->pos + offset;
                        visit(point_min(p0, p1) - reach, point_max(p0, p1) + reach, false);
                    }
                }
            }
        }
    }
    for (rf::MoverBrush& brush : DoublyLinkedList{rf::mover_brush_list}) {
        if (mover_brush_passes_through(brush)) {
            continue;
        }
        const float radius = std::max(
            brush.radius, brush.geometry ? (brush.geometry->bbox_max - brush.geometry->bbox_min).len() * 0.5f : 0.0f);
        visit_sphere(brush.pos, radius);
    }
    for (int i = 0; i < rf::level.pushers.size(); ++i) {
        if (const rf::PushRegion* push = rf::level.pushers[i]) {
            visit(push->aabb_min, push->aabb_max, false);
        }
    }
    for (int i = 0; i < rf::level.ladders.size(); ++i) {
        if (const rf::ClimbRegion* climb = rf::level.ladders[i]) {
            const rf::Matrix3& o = climb->orient;
            const rf::Vector3& e = climb->extents;
            const rf::Vector3 reach{
                std::fabs(o.rvec.x) * e.x + std::fabs(o.uvec.x) * e.y + std::fabs(o.fvec.x) * e.z,
                std::fabs(o.rvec.y) * e.x + std::fabs(o.uvec.y) * e.y + std::fabs(o.fvec.y) * e.z,
                std::fabs(o.rvec.z) * e.x + std::fabs(o.uvec.z) * e.y + std::fabs(o.fvec.z) * e.z,
            };
            visit(climb->pos - reach, climb->pos + reach, false);
        }
    }
    for (const auto& zone : g_waypoint_zones) {
        rf::Vector3 zone_min{};
        rf::Vector3 zone_max{};
        bool liquid_top = false;
        if (zone_bounds(zone, zone_min, zone_max, liquid_top)) {
            visit(zone_min, zone_max, liquid_top);
        }
    }
    // Control-point zones exist only in hill game types, but one AWP serves every game type.
    for (rf::Event* handler : rf::find_all_events_by_type(rf::EventType::Capture_Point_Handler)) {
        if (const rf::Trigger* trigger = koth_capture_point_handler_trigger(*handler)) {
            visit_sphere(trigger->pos, trigger_bounding_radius(*trigger));
        }
    }
}

std::uint64_t WaypointTerrainLattice::lattice_key(const int field, const int i, const int j)
{
    constexpr std::uint64_t kCoordMask = (std::uint64_t{1} << 24) - 1;
    constexpr int kCoordBias = 1 << 23;
    return (static_cast<std::uint64_t>(field) << 48)
        | ((static_cast<std::uint64_t>(i + kCoordBias) & kCoordMask) << 24)
        | (static_cast<std::uint64_t>(j + kCoordBias) & kCoordMask);
}

// Where terrains overlap, the one whose node height is nearest pos.
const WaypointTerrainLattice::Field* WaypointTerrainLattice::field_at(const rf::Vector3& pos, int& mask_cell) const
{
    const Field* best = nullptr;
    float best_dist = std::numeric_limits<float>::max();
    for (const auto& f : fields_) {
        const float fx = std::floor((pos.x - f.grid.origin[0]) / f.res);
        const float fz = std::floor((pos.z - f.grid.origin[2]) / f.res);
        if (!(fx >= 0.0f && fz >= 0.0f && fx < static_cast<float>(f.mask_x) && fz < static_cast<float>(f.mask_z))) {
            continue;
        }
        const float dist =
            std::fabs(pos.y - at::height_at(f.grid, pos.x, pos.z) - kWaypointGenerateGroundOffset);
        if (dist < best_dist) {
            best = &f;
            best_dist = dist;
            mask_cell = static_cast<int>(fz) * f.mask_x + static_cast<int>(fx);
        }
    }
    return best;
}

bool WaypointTerrainLattice::solid_at(const Field& f, const float x, const float z) const
{
    const float fx = std::floor((x - f.grid.origin[0]) / f.grid.cell_size);
    const float fz = std::floor((z - f.grid.origin[2]) / f.grid.cell_size);
    return at::cell_solid(f.grid, static_cast<std::int64_t>(fx), static_cast<std::int64_t>(fz));
}

bool WaypointTerrainLattice::open(const Field& f, const int mask_cell) const
{
    return f.clean[mask_cell] && f.clearance[mask_cell] >= kLatticeMargin;
}

bool WaypointTerrainLattice::open_at(const Field& f, const float x, const float z) const
{
    const float fx = std::floor((x - f.grid.origin[0]) / f.res);
    const float fz = std::floor((z - f.grid.origin[2]) / f.res);
    if (!(fx >= 0.0f && fz >= 0.0f && fx < static_cast<float>(f.mask_x) && fz < static_cast<float>(f.mask_z))) {
        return false;
    }
    return open(f, static_cast<int>(fz) * f.mask_x + static_cast<int>(fx));
}

bool WaypointTerrainLattice::on_field_surface(
    const rf::Vector3& pos, const Field*& out_field, int& out_mask_cell) const
{
    out_field = field_at(pos, out_mask_cell);
    return out_field
        && std::fabs(pos.y - at::height_at(out_field->grid, pos.x, pos.z) - kWaypointGenerateGroundOffset)
            <= kSurfaceNodeTolerance;
}

float WaypointTerrainLattice::lattice_x(const Field& f, const int i, const int j) const
{
    return f.grid.origin[0] + (static_cast<float>(i) + ((j & 1) ? 0.5f : 0.0f)) * spacing_;
}

float WaypointTerrainLattice::lattice_z(const Field& f, const int j) const
{
    return f.grid.origin[2] + static_cast<float>(j) * spacing_ * kSqrt3 * 0.5f;
}

rf::Vector3 WaypointTerrainLattice::lattice_pos(const Field& f, const int i, const int j) const
{
    const float x = lattice_x(f, i, j);
    const float z = lattice_z(f, j);
    return {x, at::height_at(f.grid, x, z) + kWaypointGenerateGroundOffset, z};
}

// The parameters along a to b where it crosses a grid line or a cell diagonal of either family: the surface under
// the segment is linear between them. Sorted, with 0 and 1.
void WaypointTerrainLattice::surface_breaks(
    const Field& f, const rf::Vector3& a, const rf::Vector3& b, std::vector<float>& ts)
{
    const float cell = f.grid.cell_size;
    const float ax = (a.x - f.grid.origin[0]) / cell;
    const float az = (a.z - f.grid.origin[2]) / cell;
    const float dx = (b.x - f.grid.origin[0]) / cell - ax;
    const float dz = (b.z - f.grid.origin[2]) / cell - az;
    ts.assign({0.0f, 1.0f});
    auto add_crossings = [&ts](const float start, const float delta) {
        if (std::fabs(delta) < 1e-6f) {
            return;
        }
        const float lo = std::min(start, start + delta);
        const float hi = std::max(start, start + delta);
        for (float k = std::ceil(lo); k <= hi; k += 1.0f) {
            const float t = (k - start) / delta;
            if (t > 0.0f && t < 1.0f) {
                ts.push_back(t);
            }
        }
    };
    add_crossings(ax, dx);
    add_crossings(az, dz);
    add_crossings(ax - az, dx - dz);
    add_crossings(ax + az, dx + dz);
    std::sort(ts.begin(), ts.end());
}

// Whether the surface rises within the walk incline between consecutive samples at ts (sorted, along a to b), and
// sample(x, z, height, t) holds at each.
template<typename Sample>
bool WaypointTerrainLattice::surface_within_incline(
    const Field& f, const rf::Vector3& a, const rf::Vector3& b, const std::vector<float>& ts, Sample&& sample)
{
    const float length = std::hypot(b.x - a.x, b.z - a.z);
    float prev_h = 0.0f;
    for (std::size_t n = 0; n < ts.size(); ++n) {
        const float x = a.x + (b.x - a.x) * ts[n];
        const float z = a.z + (b.z - a.z) * ts[n];
        const float h = at::height_at(f.grid, x, z);
        if (!sample(x, z, h, ts[n])) {
            return false;
        }
        if (n > 0 && std::fabs(h - prev_h) > (ts[n] - ts[n - 1]) * length * kWaypointGenerateMaxInclineRise + 0.01f) {
            return false;
        }
        prev_h = h;
    }
    return true;
}

// Exact for the surface, plus the open mask at half-mask-cell steps.
bool WaypointTerrainLattice::walkable_link(const Field& f, const rf::Vector3& a, const rf::Vector3& b) const
{
    std::vector<float> ts;
    surface_breaks(f, a, b, ts);
    const float length = std::hypot(b.x - a.x, b.z - a.z);
    const int mask_steps = std::max(1, static_cast<int>(std::ceil(length / (f.res * 0.5f))));
    for (int step = 1; step < mask_steps; ++step) {
        ts.push_back(static_cast<float>(step) / static_cast<float>(mask_steps));
    }
    std::sort(ts.begin(), ts.end());
    return surface_within_incline(f, a, b, ts, [&](const float x, const float z, const float h, const float t) {
        const float clearance = a.y + (b.y - a.y) * t - h;
        return open_at(f, x, z) && clearance >= kLinkMinClearance && clearance <= kLinkMaxClearance;
    });
}

bool WaypointTerrainLattice::build_fields()
{
    const auto& terrains = alpine_terrain_get_all();
    for (int t = 0; t < static_cast<int>(terrains.size()); ++t) {
        if (!terrains[t].resolved) {
            continue;
        }
        Field f{};
        f.terrain = t;
        f.grid = alpine_terrain_grid(terrains[t]);
        f.cells_x = static_cast<int>(at::cells(f.grid.nx));
        f.cells_z = static_cast<int>(at::cells(f.grid.nz));
        if (f.cells_x <= 0 || f.cells_z <= 0 || !(f.grid.cell_size > 0.0f)) {
            continue;
        }
        f.split = std::max(1, static_cast<int>(std::ceil(f.grid.cell_size / kMaskCellTargetSize)));
        fields_.push_back(f);
    }
    // One budget for every terrain, coarsening the finest masks first; one mask cell per terrain cell always fits.
    static_assert(static_cast<std::uint64_t>(at::max_terrains) * at::cells(at::max_verts) * at::cells(at::max_verts)
                  <= kMaxMaskCells);
    auto total_cells = [this] {
        std::uint64_t total = 0;
        for (const auto& f : fields_) {
            total += static_cast<std::uint64_t>(f.cells_x) * static_cast<std::uint64_t>(f.cells_z)
                * static_cast<std::uint64_t>(f.split) * static_cast<std::uint64_t>(f.split);
        }
        return total;
    };
    while (total_cells() > kMaxMaskCells) {
        auto finest = std::max_element(
            fields_.begin(), fields_.end(), [](const Field& a, const Field& b) { return a.split < b.split; });
        if (finest->split <= 1) {
            break;
        }
        --finest->split;
    }

    for (auto& f : fields_) {
        f.res = f.grid.cell_size / static_cast<float>(f.split);
        f.mask_x = f.cells_x * f.split;
        f.mask_z = f.cells_z * f.split;
        std::vector<std::uint8_t> cell_solid(static_cast<std::size_t>(f.cells_x) * f.cells_z);
        std::vector<std::uint8_t> cell_clean(cell_solid.size());
        for (int z = 0; z < f.cells_z; ++z) {
            for (int x = 0; x < f.cells_x; ++x) {
                const std::size_t c = static_cast<std::size_t>(z) * f.cells_x + x;
                cell_solid[c] = at::cell_solid(f.grid, x, z);
                cell_clean[c] = cell_solid[c]
                    && cell_climbable(f.grid, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(z));
            }
        }
        const std::size_t mask_count = static_cast<std::size_t>(f.mask_x) * f.mask_z;
        f.low.resize(mask_count);
        f.high.resize(mask_count);
        f.bare.resize(mask_count);
        f.clean.resize(mask_count);
        for (int mz = 0; mz < f.mask_z; ++mz) {
            for (int mx = 0; mx < f.mask_x; ++mx) {
                const std::size_t m = static_cast<std::size_t>(mz) * f.mask_x + mx;
                const std::size_t c = static_cast<std::size_t>(mz / f.split) * f.cells_x + mx / f.split;
                f.bare[m] = cell_solid[c];
                f.clean[m] = cell_clean[c];
                // The surface is planar either side of the terrain cell's diagonal, so the samples come close.
                const float x0 = f.grid.origin[0] + static_cast<float>(mx) * f.res;
                const float z0 = f.grid.origin[2] + static_cast<float>(mz) * f.res;
                float lo = std::numeric_limits<float>::max();
                float hi = std::numeric_limits<float>::lowest();
                for (const auto& [u, v] : kSurfaceSamples) {
                    const float h = at::height_at(f.grid, x0 + u * f.res, z0 + v * f.res);
                    lo = std::min(lo, h);
                    hi = std::max(hi, h);
                }
                f.low[m] = lo;
                f.high[m] = hi;
            }
        }
    }
    return !fields_.empty();
}

// mark_all_nodes: every node in the graph is a seed (a fresh run, which has only seeds yet). Otherwise the seeds are
// re-derived as a fresh run makes them: the persistent special nodes and the control-point floor positions.
bool WaypointTerrainLattice::mark_occupied(const bool mark_all_nodes, const OutOfTime& out_of_time)
{
    // Clears clean (and bare, for an obstacle) on the mask cells under a world box whose y range rises above the
    // cell's surface but not past its band. `plane` (may be null) narrows the y range to where a non-vertical face's
    // plane runs over the cell.
    bool stopped = false;
    int progress_done = 0;
    int progress_total = 0;
    auto mark = [&](const rf::Vector3& box_min, const rf::Vector3& box_max, const rf::Plane* plane,
                    const int skip_terrain, const bool obstacle) {
        for (auto& f : fields_) {
            if (f.terrain == skip_terrain) {
                continue;
            }
            auto mask_coord = [&f](const float world, const int axis) {
                float v = std::floor((world - f.grid.origin[axis]) / f.res);
                // Written so a NaN lands on -1 too.
                if (!(v >= -1.0f)) {
                    v = -1.0f;
                }
                return static_cast<int>(std::min(v, static_cast<float>(std::max(f.mask_x, f.mask_z))));
            };
            const int x0 = std::max(0, mask_coord(box_min.x, 0));
            const int z0 = std::max(0, mask_coord(box_min.z, 2));
            const int x1 = std::min(f.mask_x - 1, mask_coord(box_max.x, 0));
            const int z1 = std::min(f.mask_z - 1, mask_coord(box_max.z, 2));
            for (int mz = z0; mz <= z1 && !stopped; ++mz) {
                stopped = out_of_time(progress_done, progress_total);
                for (int mx = x0; mx <= x1; ++mx) {
                    const std::size_t m = static_cast<std::size_t>(mz) * f.mask_x + mx;
                    if (!f.clean[m] && (!obstacle || !f.bare[m])) {
                        continue;
                    }
                    float y_lo = box_min.y;
                    float y_hi = box_max.y;
                    if (plane) {
                        const float cx0 = std::max(box_min.x, f.grid.origin[0] + static_cast<float>(mx) * f.res);
                        const float cx1 = std::min(box_max.x, f.grid.origin[0] + static_cast<float>(mx + 1) * f.res);
                        const float cz0 = std::max(box_min.z, f.grid.origin[2] + static_cast<float>(mz) * f.res);
                        const float cz1 = std::min(box_max.z, f.grid.origin[2] + static_cast<float>(mz + 1) * f.res);
                        float plane_lo = std::numeric_limits<float>::max();
                        float plane_hi = std::numeric_limits<float>::lowest();
                        for (const float px : {cx0, cx1}) {
                            for (const float pz : {cz0, cz1}) {
                                const float py =
                                    (-plane->offset - plane->normal.x * px - plane->normal.z * pz) / plane->normal.y;
                                plane_lo = std::min(plane_lo, py);
                                plane_hi = std::max(plane_hi, py);
                            }
                        }
                        y_lo = std::max(y_lo, plane_lo);
                        y_hi = std::min(y_hi, plane_hi);
                    }
                    if (y_hi > f.low[m] + kBuriedTolerance && y_lo <= f.high[m] + kSurfaceBandAbove) {
                        f.clean[m] = 0;
                        if (obstacle) {
                            f.bare[m] = 0;
                        }
                    }
                }
            }
        }
    };

    if (rf::level.geometry) {
        const int room_count = rf::level.geometry->all_rooms.size();
        for (int r = 0; r < room_count; ++r) {
            progress_done = r;
            progress_total = room_count;
            if (stopped || out_of_time(r, room_count)) {
                return false;
            }
            rf::GRoom* room = rf::level.geometry->all_rooms[r];
            if (!room) {
                continue;
            }
            const AlpineTerrainRoomRef* chunk = alpine_terrain_find_room(room);
            const int skip_terrain = chunk ? chunk->terrain : -1;
            for (rf::GFace& face : room->face_list) {
                if (face.attributes.is_portal() || face.attributes.is_liquid()) {
                    continue;
                }
                constexpr float kVerticalFaceNormalY = 0.05f;
                const bool sloped = std::fabs(face.plane.normal.y) > kVerticalFaceNormalY;
                mark(face.bounding_box_min, face.bounding_box_max, sloped ? &face.plane : nullptr, skip_terrain,
                     true);
            }
            // Water too shallow to cover a standing node's position is waded like dry ground; other liquids hurt.
            if (room->contains_liquid) {
                const bool water = room->liquid_type == rf::LIQUID_WATER;
                const float wade_depth = water ? kWaypointGenerateGroundOffset : 0.0f;
                const rf::Vector3 wade_max{
                    room->bbox_max.x, room->bbox_min.y + room->liquid_depth - wade_depth, room->bbox_max.z};
                mark(room->bbox_min, wade_max, nullptr, -1, !water);
            }
        }
    }
    for_each_waypoint_generation_obstacle([&mark](const rf::Vector3& box_min, const rf::Vector3& box_max, bool) {
        mark(box_min, box_max, nullptr, -1, true);
    });
    // A seeded node gets a patch of fine nodes within link reach, whatever height above the terrain it stands at.
    auto mark_seed = [&mark](const rf::Vector3& pos) {
        const rf::Vector3 reach{kWaypointLinkRadius, kWaypointLinkRadius, kWaypointLinkRadius};
        const rf::Vector3 below{0.0f, kWaypointGenerateGroundOffset + kBridgeWaypointMaxGroundDistance, 0.0f};
        mark(pos - reach - below, pos + reach, nullptr, -1, true);
    };
    const int node_count = static_cast<int>(g_waypoints.size());
    for (int i = 1; i < node_count && !stopped; ++i) {
        const auto& node = g_waypoints[i];
        progress_done = i;
        progress_total = node_count;
        if (node.valid && (mark_all_nodes || (waypoint_type_is_special(node.type) && !node.temporary))) {
            mark_seed(node.pos);
        }
    }
    if (!mark_all_nodes) {
        for (const rf::Vector3& pos : generation_control_point_seed_positions()) {
            mark_seed(pos);
        }
    }
    return !stopped;
}

// Distance to the nearest cell that is not clean, with everything past the grid edge not clean.
void WaypointTerrainLattice::measure_clearance()
{
    for (auto& f : fields_) {
        const float diag = f.res * std::numbers::sqrt2_v<float>;
        std::vector<float>& d = f.clearance;
        d.assign(f.clean.size(), 0.0f);
        for (std::size_t m = 0; m < d.size(); ++m) {
            d[m] = f.clean[m] ? std::numeric_limits<float>::max() : 0.0f;
        }
        auto at_cell = [&f, &d](const int x, const int z) {
            const bool inside = x >= 0 && z >= 0 && x < f.mask_x && z < f.mask_z;
            return inside ? d[static_cast<std::size_t>(z) * f.mask_x + x] : 0.0f;
        };
        for (int z = 0; z < f.mask_z; ++z) {
            for (int x = 0; x < f.mask_x; ++x) {
                float& v = d[static_cast<std::size_t>(z) * f.mask_x + x];
                v = std::min({v, at_cell(x - 1, z) + f.res, at_cell(x, z - 1) + f.res, at_cell(x - 1, z - 1) + diag,
                              at_cell(x + 1, z - 1) + diag});
            }
        }
        for (int z = f.mask_z - 1; z >= 0; --z) {
            for (int x = f.mask_x - 1; x >= 0; --x) {
                float& v = d[static_cast<std::size_t>(z) * f.mask_x + x];
                v = std::min({v, at_cell(x + 1, z) + f.res, at_cell(x, z + 1) + f.res, at_cell(x + 1, z + 1) + diag,
                              at_cell(x - 1, z + 1) + diag});
            }
        }
        // Centre to centre, then any point of either cell.
        for (float& v : d) {
            v = v / kChamferOverestimate - diag;
        }
    }
}

// The smallest spacing (in whole steps), no finer than a terrain cell, whose hex lattice, 2 / (sqrt(3) s^2) nodes per
// unit area, keeps within budget, clamped to [kMinSpacing, kMaxSpacing]; build_masks widens it further when the
// lattice would pass the node cap. False when nothing is open.
bool WaypointTerrainLattice::choose_spacing()
{
    float open_area = 0.0f;
    float min_spacing = kMinSpacing;
    for (const auto& f : fields_) {
        min_spacing = std::max(min_spacing, f.grid.cell_size);
        for (std::size_t m = 0; m < f.clean.size(); ++m) {
            open_area += open(f, static_cast<int>(m)) ? f.res * f.res : 0.0f;
        }
    }
    const float budget_spacing = std::sqrt(open_area * 2.0f / (kSqrt3 * static_cast<float>(kLatticeNodeBudget)));
    spacing_ = std::clamp(
        std::ceil(std::max(min_spacing, budget_spacing) / kSpacingStep) * kSpacingStep, kMinSpacing, kMaxSpacing);
    xlog::info("[Waypoints] terrain lattice: spacing {:.2f} u over {:.0f} u2 of open terrain", spacing_, open_area);
    return open_area > 0.0f;
}

// The open lattice positions, counting no further than `limit`; -1 once out of time.
int WaypointTerrainLattice::count_lattice_positions(const int limit, const OutOfTime& out_of_time) const
{
    int count = 0;
    for (const auto& f : fields_) {
        const float extent_x = static_cast<float>(f.cells_x) * f.grid.cell_size;
        const float extent_z = static_cast<float>(f.cells_z) * f.grid.cell_size;
        for (int j = 0; lattice_z(f, j) - f.grid.origin[2] <= extent_z; ++j) {
            if (out_of_time(count, limit)) {
                return -1;
            }
            for (int i = 0; static_cast<float>(i) * spacing_ <= extent_x; ++i) {
                count += open_at(f, lattice_x(f, i, j), lattice_z(f, j)) ? 1 : 0;
                if (count >= limit) {
                    return limit;
                }
            }
        }
    }
    return count;
}

bool WaypointTerrainLattice::place_nodes(const OutOfTime& out_of_time)
{
    for (int fi = 0; fi < static_cast<int>(fields_.size()); ++fi) {
        const Field& f = fields_[fi];
        const float extent_x = static_cast<float>(f.cells_x) * f.grid.cell_size;
        const float extent_z = static_cast<float>(f.cells_z) * f.grid.cell_size;
        const int rows = static_cast<int>(extent_z / (spacing_ * kSqrt3 * 0.5f)) + 1;
        for (int j = 0; lattice_z(f, j) - f.grid.origin[2] <= extent_z; ++j) {
            if (out_of_time(j, rows)) {
                return false;
            }
            for (int i = 0; static_cast<float>(i) * spacing_ <= extent_x; ++i) {
                if (!open_at(f, lattice_x(f, i, j), lattice_z(f, j))) {
                    continue;
                }
                node_by_key_[lattice_key(fi, i, j)] = static_cast<int>(g_waypoints.size());
                nodes_.push_back({fi, i, j, 0});
                add_waypoint(lattice_pos(f, i, j), WaypointType::std_new, 0, false, true);
            }
        }
    }
    end_ = static_cast<int>(g_waypoints.size());
    return true;
}

// Chains `from` to `to` through evenly spaced surface points when the straight link crosses a ridge or a gully.
// The new nodes are appended after the lattice and count as lattice nodes.
bool WaypointTerrainLattice::split_link(const int field, const int from, const int to)
{
    const Field& f = fields_[field];
    const rf::Vector3 a = g_waypoints[from].pos;
    const rf::Vector3 b = g_waypoints[to].pos;
    std::array<rf::Vector3, kMaxLinkParts + 1> points{};
    for (int parts = 2; parts <= kMaxLinkParts; ++parts) {
        if (static_cast<int>(nodes_.size()) + parts - 1 > kMaxLatticeNodes) {
            return false;
        }
        points[0] = a;
        points[parts] = b;
        bool ok = true;
        for (int k = 1; k < parts && ok; ++k) {
            const float t = static_cast<float>(k) / static_cast<float>(parts);
            const float x = a.x + (b.x - a.x) * t;
            const float z = a.z + (b.z - a.z) * t;
            ok = open_at(f, x, z);
            points[k] = {x, at::height_at(f.grid, x, z) + kWaypointGenerateGroundOffset, z};
        }
        for (int k = 0; k < parts && ok; ++k) {
            ok = walkable_link(f, points[k], points[k + 1]);
        }
        if (!ok) {
            continue;
        }
        int prev = from;
        for (int k = 1; k <= parts; ++k) {
            int next = to;
            if (k < parts) {
                next = add_waypoint(points[k], WaypointType::std_new, 0, false, true);
                nodes_.push_back({field, 0, 0, 0x3F});
                ++split_link_nodes_;
            }
            link_waypoint(prev, next);
            link_waypoint(next, prev);
            prev = next;
        }
        return true;
    }
    return false;
}

int WaypointTerrainLattice::link_nodes(const OutOfTime& out_of_time)
{
    const int placed_end = end_;
    int links = 0;
    for (int index = first_; index < placed_end; ++index) {
        if (out_of_time(index - first_, placed_end - first_)) {
            break;
        }
        const LatticeNode node = nodes_[index - first_];
        const Field& f = fields_[node.field];
        for (int dir = 0; dir < 3; ++dir) {
            int ni = 0;
            int nj = 0;
            hex_neighbor(node.i, node.j, dir, ni, nj);
            const auto it = node_by_key_.find(lattice_key(node.field, ni, nj));
            if (it == node_by_key_.end()) {
                continue;
            }
            if (walkable_link(f, g_waypoints[index].pos, g_waypoints[it->second].pos)) {
                link_waypoint(index, it->second);
                link_waypoint(it->second, index);
            }
            else if (!split_link(node.field, index, it->second)) {
                continue;
            }
            nodes_[index - first_].linked |= static_cast<std::uint8_t>(1u << dir);
            nodes_[it->second - first_].linked |= static_cast<std::uint8_t>(1u << ((dir + 3) % 6));
            links += 2;
        }
    }
    end_ = static_cast<int>(g_waypoints.size());
    for (int index = first_; index < placed_end; ++index) {
        if (std::popcount(static_cast<unsigned>(nodes_[index - first_].linked)) < 6) {
            edge_nodes_.push_back(index);
        }
    }
    return links;
}

bool WaypointTerrainLattice::build_masks(const bool mark_all_nodes, const OutOfTime& out_of_time)
{
    first_ = end_ = static_cast<int>(g_waypoints.size());
    if (!build_fields() || !mark_occupied(mark_all_nodes, out_of_time)) {
        return false;
    }
    measure_clearance();
    if (!choose_spacing()) {
        return false;
    }
    // A probe places no nodes, so its spacing is never used.
    if (!mark_all_nodes) {
        return true;
    }
    // Past the cap the lattice would leave open terrain that no node covers, so it spreads out until it fits, each
    // time by the square root of the overshoot (counted up to 4x the cap), and by at least one step.
    constexpr int kCountLimit = 4 * kMaxLatticeNodes;
    int positions = count_lattice_positions(kCountLimit, out_of_time);
    while (positions > kMaxLatticeNodes && spacing_ < kMaxCappedSpacing) {
        const float jump = std::sqrt(static_cast<float>(positions) / static_cast<float>(kMaxLatticeNodes));
        const float widened = std::ceil(spacing_ * jump / kSpacingStep) * kSpacingStep;
        spacing_ = std::min(std::max(widened, spacing_ + kSpacingStep), kMaxCappedSpacing);
        positions = count_lattice_positions(kCountLimit, out_of_time);
    }
    if (positions < 0) {
        return false;
    }
    if (positions > kMaxLatticeNodes) {
        xlog::warn(
            "[Waypoints] terrain lattice needs over {} nodes even at {:.0f} u spacing; probing the terrain instead, "
            "which covers only what the node cap allows",
            kMaxLatticeNodes, spacing_);
        fields_.clear();
        return false;
    }
    return positions > 0;
}

void WaypointTerrainLattice::build(const OutOfTime& out_of_time)
{
    if (!build_masks(true, out_of_time) || !place_nodes(out_of_time)) {
        return;
    }
    const int links = link_nodes(out_of_time);
    covering_ = active();
    xlog::info(
        "[Waypoints] terrain lattice: {} nodes ({} on its edge, {} splitting ridge and gully links), {} links",
        node_count(), edge_nodes_.size(), split_link_nodes_, links);
}

void WaypointTerrainLattice::build_for_existing_lattice(const OutOfTime& out_of_time)
{
    covering_ = std::any_of(g_waypoints.begin(), g_waypoints.end(), [](const WaypointNode& node) {
        return node.valid && node.terrain;
    }) && build_masks(false, out_of_time);
}

int WaypointTerrainLattice::unlinked_neighbor_offsets(const int index, std::array<rf::Vector3, 6>& out) const
{
    if (!is_coarse(index)) {
        return 0;
    }
    const LatticeNode& node = nodes_[index - first_];
    const Field& f = fields_[node.field];
    const rf::Vector3& from = g_waypoints[index].pos;
    const float extent_x = static_cast<float>(f.cells_x) * f.grid.cell_size;
    const float extent_z = static_cast<float>(f.cells_z) * f.grid.cell_size;
    int count = 0;
    for (int dir = 0; dir < 6; ++dir) {
        if (node.linked & (1u << dir)) {
            continue;
        }
        int ni = 0;
        int nj = 0;
        hex_neighbor(node.i, node.j, dir, ni, nj);
        const rf::Vector3 to = lattice_pos(f, ni, nj);
        const float lx = to.x - f.grid.origin[0];
        const float lz = to.z - f.grid.origin[2];
        if (lx < 0.0f || lz < 0.0f || lx > extent_x || lz > extent_z) {
            continue;
        }
        out[count++] = to - from;
    }
    return count;
}

bool WaypointTerrainLattice::covers(const rf::Vector3& pos) const
{
    int mask_cell = 0;
    const Field* f = covering_ ? field_at(pos, mask_cell) : nullptr;
    return f && open(*f, mask_cell)
        && std::fabs(pos.y - at::height_at(f->grid, pos.x, pos.z) - kWaypointGenerateGroundOffset) <= kCoverTolerance;
}

bool WaypointTerrainLattice::on_bare_terrain(const rf::Vector3& pos) const
{
    const Field* f = nullptr;
    int mask_cell = 0;
    return covering_ && on_field_surface(pos, f, mask_cell) && f->bare[mask_cell];
}

bool WaypointTerrainLattice::ground_walkable(const rf::Vector3& a, const rf::Vector3& b) const
{
    const Field* fa = nullptr;
    const Field* fb = nullptr;
    int mask_a = 0;
    int mask_b = 0;
    if (!covering_ || !on_field_surface(a, fa, mask_a) || !on_field_surface(b, fb, mask_b)) {
        return true;
    }
    if (fa != fb) {
        return false;
    }
    std::vector<float> ts;
    surface_breaks(*fa, a, b, ts);
    return surface_within_incline(*fa, a, b, ts, [&](const float x, const float z, float, float) {
        return solid_at(*fa, x, z);
    });
}

bool WaypointTerrainLattice::join_walkable(const rf::Vector3& a, const rf::Vector3& b) const
{
    const Field* fa = nullptr;
    const Field* fb = nullptr;
    int mask_a = 0;
    int mask_b = 0;
    if (!covering_ || !on_field_surface(a, fa, mask_a) || !on_field_surface(b, fb, mask_b) || fa != fb) {
        return true;
    }
    // No incline test: a join too steep to climb is still a way down, which the caller links one way.
    std::vector<float> ts;
    surface_breaks(*fa, a, b, ts);
    return std::all_of(ts.begin(), ts.end(), [&](const float t) {
        const float x = a.x + (b.x - a.x) * t;
        const float z = a.z + (b.z - a.z) * t;
        const float clearance = a.y + (b.y - a.y) * t - at::height_at(fa->grid, x, z);
        return solid_at(*fa, x, z) && clearance >= kLinkMinClearance && clearance <= kLinkMaxClearance;
    });
}

bool WaypointTerrainLattice::under_terrain(const rf::Vector3& pos) const
{
    int mask_cell = 0;
    const Field* f = covering_ ? field_at(pos, mask_cell) : nullptr;
    if (!f || !solid_at(*f, pos.x, pos.z)) {
        return false;
    }
    const float surface = at::height_at(f->grid, pos.x, pos.z);
    if (pos.y >= surface || ((f->grid.flags & at::flag_geoable) && pos.y <= at::bottom_y(f->grid))) {
        return false;
    }
    // A cave under the terrain has its own ceiling between the point and the surface.
    rf::Vector3 p0 = pos;
    rf::Vector3 p1{pos.x, surface + 0.1f, pos.z};
    rf::GCollisionOutput collision{};
    if (!rf::collide_linesegment_level_solid(p0, p1, rf::CF_PROCESS_INVISIBLE_FACES, &collision) || !collision.face) {
        return true;
    }
    const AlpineTerrainRoomRef* chunk = alpine_terrain_find_room(collision.face->which_room);
    return chunk && chunk->terrain == f->terrain;
}

bool WaypointTerrainLattice::overlaps_terrain(const rf::Vector3& box_min, const rf::Vector3& box_max) const
{
    if (!covering_) {
        return false;
    }
    return std::any_of(fields_.begin(), fields_.end(), [&](const Field& f) {
        const float x1 = f.grid.origin[0] + static_cast<float>(f.cells_x) * f.grid.cell_size;
        const float z1 = f.grid.origin[2] + static_cast<float>(f.cells_z) * f.grid.cell_size;
        return box_max.x >= f.grid.origin[0] && box_min.x <= x1 && box_max.z >= f.grid.origin[2] && box_min.z <= z1;
    });
}

void WaypointTerrainLattice::lift_probe_to_surface(rf::Vector3& pos) const
{
    int mask_cell = 0;
    const Field* f = covering_ ? field_at(pos, mask_cell) : nullptr;
    if (f && solid_at(*f, pos.x, pos.z)) {
        pos.y = std::max(pos.y, at::height_at(f->grid, pos.x, pos.z) + kWaypointGenerateGroundOffset);
    }
}
