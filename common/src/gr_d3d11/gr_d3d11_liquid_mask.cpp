#include <algorithm>
#include <bit>
#include <cmath>
#include <numeric>
#include <tuple>
#include <xlog/xlog.h>
#include <common/gr_d3d11/gr_d3d11_util.h>
#include <common/gr_d3d11/gr_d3d11_liquid_mask.h>

namespace gr::d3d11
{
    namespace
    {
        constexpr UINT mask_srv_slot = 35;
        constexpr UINT mask_sampler_slot = 9;

        constexpr float min_cell_size = 0.5f;
        // All rooms share one cell size, grown until the cells of all fit, except that a room whose cells alone
        // would exceed its share of them gets coarser ones
        constexpr uint64_t max_total_cells = uint64_t{4} << 20;
        constexpr uint64_t min_room_cells = uint64_t{1} << 18;
        constexpr uint64_t max_room_cells = uint64_t{1} << 21;
        constexpr float cell_growth = 1.25f;
        constexpr int max_fit_attempts = 64;
        // Packing the atlas grows with the square of this
        constexpr size_t max_plans = 256;
        constexpr int max_atlas_dim = D3D11_REQ_TEXTURE3D_U_V_OR_W_DIMENSION;
        constexpr uint64_t max_atlas_voxels = uint64_t{16} << 20;
        // Bounds the work and the stored crossings a room's faces cost, which crafted geometry could otherwise
        // blow up
        constexpr size_t max_room_steps = size_t{1} << 24;
        constexpr size_t max_room_crossings = size_t{1} << 21;
        // Bounds the whole build, which crafted levels can spread over many rooms
        constexpr size_t max_build_steps = size_t{1} << 29;
        // Keeps the rows of cell centres off the edges and vertices level geometry tends to line up on
        constexpr float row_jitter_y = 0.00137f;
        constexpr float row_jitter_z = 0.00291f;

        enum class CellState : uint8_t
        {
            unknown,
            inside,
            outside,
        };

        struct Box
        {
            rf::Vector3 min;
            rf::Vector3 max;
        };

        struct Plan
        {
            const rf::GRoom* room;
            Box domain;
            // The domain's cells may reach one cell above the surface, but not past the room
            float top;
            // Rooms whose space counts as this room's, and the rooms it is told apart from
            std::vector<const rf::GRoom*> inside_rooms;
            std::vector<const rf::GRoom*> outside_rooms;
            float cell;
            std::array<int, 3> dims;
            std::array<int, 3> atlas_offset;
            // False when its rooms could not be resolved, so it keeps its box
            bool masked = true;
        };

        bool is_liquid_room(const rf::GRoom* room)
        {
            return room && !room->is_detail && room->uid >= 0 && room->contains_liquid && room->liquid_type > 0;
        }

        float surface_y(const rf::GRoom* room)
        {
            return room->bbox_min.y + room->liquid_depth;
        }

        // A neighbouring room of the same liquid is the same body of it, unless its surface is lower and
        // leaves air under this room's surface
        bool same_liquid(const rf::GRoom* room, const rf::GRoom* other)
        {
            return is_liquid_room(other) && other->liquid_type == room->liquid_type
                && surface_y(other) >= surface_y(room);
        }

        // Places the plans in order, each in the smallest free box that holds it, and splits what that box has
        // left into three
        bool pack_atlas(std::vector<Plan>& plans, const std::vector<size_t>& order, const std::array<int, 3>& size,
                        uint64_t max_voxels, std::array<int, 3>& used)
        {
            struct FreeBox
            {
                std::array<int, 3> pos;
                std::array<int, 3> size;
            };
            std::vector<FreeBox> free_boxes{{{0, 0, 0}, size}};
            used = {1, 1, 1};
            for (size_t i : order) {
                Plan& plan = plans[i];
                const std::array<int, 3>& dims = plan.dims;
                auto best = free_boxes.end();
                uint64_t best_volume = 0;
                for (auto it = free_boxes.begin(); it != free_boxes.end(); ++it) {
                    const uint64_t volume = uint64_t(it->size[0]) * uint64_t(it->size[1]) * uint64_t(it->size[2]);
                    if (dims[0] <= it->size[0] && dims[1] <= it->size[1] && dims[2] <= it->size[2]
                        && (best == free_boxes.end() || volume < best_volume)) {
                        best = it;
                        best_volume = volume;
                    }
                }
                if (best == free_boxes.end()) {
                    return false;
                }
                const FreeBox box = *best;
                free_boxes.erase(best);
                plan.atlas_offset = box.pos;
                for (int axis = 0; axis < 3; ++axis) {
                    used[axis] = std::max(used[axis], box.pos[axis] + dims[axis]);
                }
                if (uint64_t(used[0]) * uint64_t(used[1]) * uint64_t(used[2]) > max_voxels) {
                    return false;
                }
                const FreeBox rest[] = {
                    {{box.pos[0] + dims[0], box.pos[1], box.pos[2]}, {box.size[0] - dims[0], box.size[1], box.size[2]}},
                    {{box.pos[0], box.pos[1] + dims[1], box.pos[2]}, {dims[0], box.size[1] - dims[1], box.size[2]}},
                    {{box.pos[0], box.pos[1], box.pos[2] + dims[2]}, {dims[0], dims[1], box.size[2] - dims[2]}},
                };
                for (const FreeBox& r : rest) {
                    if (r.size[0] > 0 && r.size[1] > 0 && r.size[2] > 0) {
                        free_boxes.push_back(r);
                    }
                }
            }
            return true;
        }

        bool fit_atlas(std::vector<Plan>& plans, float shared_cell, std::array<int, 3>& atlas_dims)
        {
            const float room_cells = float(std::clamp(max_total_cells / plans.size(), min_room_cells, max_room_cells));
            std::array<int, 3> largest{1, 1, 1};
            uint64_t total_cells = 0;
            for (Plan& plan : plans) {
                float ext[3] = {
                    plan.domain.max.x - plan.domain.min.x,
                    plan.domain.max.y - plan.domain.min.y,
                    plan.domain.max.z - plan.domain.min.z,
                };
                plan.cell = std::max(shared_cell, std::cbrt(ext[0] * ext[1] * ext[2] / room_cells));
                if (!std::isfinite(plan.cell)) {
                    return false;
                }
                // The caustics test looks half a cell off a face, which may be just under the surface
                ext[1] = std::min(plan.domain.max.y + plan.cell, plan.top) - plan.domain.min.y;
                for (int axis = 0; axis < 3; ++axis) {
                    const float n = std::ceil(ext[axis] / plan.cell);
                    if (!(n <= max_atlas_dim)) {
                        return false;
                    }
                    plan.dims[axis] = std::max(1, static_cast<int>(n));
                    largest[axis] = std::max(largest[axis], plan.dims[axis]);
                }
                total_cells += uint64_t(plan.dims[0]) * uint64_t(plan.dims[1]) * uint64_t(plan.dims[2]);
            }
            if (total_cells > max_total_cells) {
                return false;
            }
            std::vector<size_t> order(plans.size());
            std::iota(order.begin(), order.end(), size_t{0});
            std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
                return std::tie(plans[a].dims[0], plans[a].dims[2]) > std::tie(plans[b].dims[0], plans[b].dims[2]);
            });

            // Tries atlas sizes from the largest plan's up and keeps the one that leaves the least unused
            constexpr float size_steps[] = {1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 6.0f};
            uint64_t best_voxels = max_atlas_voxels + 1;
            std::array<int, 3> best_size{};
            std::array<int, 3> used{};
            for (float step_x : size_steps) {
                for (float step_y : size_steps) {
                    for (float step_z : size_steps) {
                        const std::array<int, 3> size{
                            std::min(static_cast<int>(largest[0] * step_x), max_atlas_dim),
                            std::min(static_cast<int>(largest[1] * step_y), max_atlas_dim),
                            std::min(static_cast<int>(largest[2] * step_z), max_atlas_dim),
                        };
                        if (pack_atlas(plans, order, size, best_voxels - 1, used)) {
                            const uint64_t voxels = uint64_t(used[0]) * uint64_t(used[1]) * uint64_t(used[2]);
                            if (voxels < best_voxels) {
                                best_voxels = voxels;
                                best_size = size;
                            }
                        }
                    }
                }
            }
            return best_voxels <= max_atlas_voxels && pack_atlas(plans, order, best_size, best_voxels, atlas_dims);
        }

        bool is_finite(const rf::Vector3& v)
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }

        // Index of the first cell whose centre is at or past pos along an axis, clamped to [0, n]
        int first_cell_from(float pos, float origin, float cell, float jitter, int n)
        {
            const float i = std::ceil((pos - origin - jitter) / cell - 0.5f);
            return i > 0.0f ? static_cast<int>(std::min(i, float(n))) : 0;
        }

        // Gives the cells whose centres lie in the room the state. The room's faces, its portals among them,
        // enclose it, so a row of cells along x is in the room past an odd number of them. False when its faces
        // take too many steps or crossings to resolve, or the build has run out of steps.
        bool mark_room(const Plan& plan, const rf::GRoom* room, CellState state, std::vector<CellState>& states,
                       size_t& build_steps)
        {
            const auto [nx, ny, nz] = plan.dims;
            const rf::Vector3& origin = plan.domain.min;
            // No cell centre is in a room the cells' box misses
            const rf::Vector3 cells_end{origin.x + nx * plan.cell, origin.y + ny * plan.cell,
                                        origin.z + nz * plan.cell};
            if (room->bbox_max.x < origin.x || room->bbox_min.x > cells_end.x || room->bbox_max.y < origin.y
                || room->bbox_min.y > cells_end.y || room->bbox_max.z < origin.z || room->bbox_min.z > cells_end.z) {
                return true;
            }
            // Every room also walks every row once
            build_steps += size_t(ny) * nz;
            if (build_steps > max_build_steps) {
                return false;
            }
            const size_t max_steps = build_steps + std::min(max_room_steps, max_build_steps - build_steps);
            const float first_centre_x = origin.x + 0.5f * plan.cell;
            const float last_centre_x = origin.x + (nx - 0.5f) * plan.cell;
            // Crossings before the first cell centre only flip the side a row starts on; the others up to the last
            // centre are kept as row (z * ny + y) and x
            std::vector<uint8_t> row_starts_inside(size_t(ny) * nz, 0);
            std::vector<std::pair<int, float>> crossings;
            std::vector<rf::Vector3> verts;
            std::vector<float> spans;
            for (const rf::GFace* face = room->face_list.first(); face; face = room->face_list.next(face)) {
                if (++build_steps > max_steps) {
                    return false;
                }
                const rf::Plane& plane = face->plane;
                if (!is_finite(plane.normal) || !std::isfinite(plane.offset)) {
                    continue;
                }
                // Parallel to the rows
                if (plane.normal.x == 0.0f) {
                    continue;
                }
                verts.clear();
                bool valid = true;
                for (const rf::GFaceVertex* fvert = face->edge_loop; fvert;) {
                    if (!fvert->vertex || !is_finite(fvert->vertex->pos)) {
                        valid = false;
                        break;
                    }
                    verts.push_back(fvert->vertex->pos);
                    fvert = fvert->next;
                    if (fvert == face->edge_loop || verts.size() > size_t(rf::max_face_vertices)) {
                        break;
                    }
                }
                build_steps += verts.size();
                if (!valid || verts.size() < 3 || verts.size() > size_t(rf::max_face_vertices)) {
                    continue;
                }
                rf::Vector3 face_min = verts[0];
                rf::Vector3 face_max = verts[0];
                for (const rf::Vector3& v : verts) {
                    face_min = {std::min(face_min.x, v.x), std::min(face_min.y, v.y), std::min(face_min.z, v.z)};
                    face_max = {std::max(face_max.x, v.x), std::max(face_max.y, v.y), std::max(face_max.z, v.z)};
                }
                // Crossed only at or after the last cell centre, or by no row
                if (face_min.x > last_centre_x || face_max.y < origin.y || face_min.y > cells_end.y
                    || face_max.z < origin.z || face_min.z > cells_end.z) {
                    continue;
                }
                // A row more on each side, so rows level with a vertex are left to the edge test alone
                const int k_begin =
                    std::max(first_cell_from(face_min.z, origin.z, plan.cell, row_jitter_z, nz) - 1, 0);
                const int k_end = std::min(first_cell_from(face_max.z, origin.z, plan.cell, row_jitter_z, nz) + 1, nz);
                // Every row walks every edge, even a row the face crosses at no cell centre
                build_steps += size_t(k_end - k_begin) * verts.size();
                if (build_steps > max_steps) {
                    return false;
                }
                for (int k = k_begin; k < k_end; ++k) {
                    const float z = origin.z + (k + 0.5f) * plan.cell + row_jitter_z;
                    spans.clear();
                    for (size_t i = 0; i < verts.size(); ++i) {
                        // Lower end first, so a face and its neighbour put their shared edge at the same y
                        const rf::Vector3* a = &verts[i];
                        const rf::Vector3* b = &verts[(i + 1) % verts.size()];
                        if (a->z > b->z) {
                            std::swap(a, b);
                        }
                        if (a->z <= z && z < b->z) {
                            spans.push_back(a->y + (z - a->z) * (b->y - a->y) / (b->z - a->z));
                        }
                    }
                    if (!std::all_of(spans.begin(), spans.end(), [](float y) { return std::isfinite(y); })) {
                        continue;
                    }
                    if (spans.size() > 2) {
                        build_steps += spans.size() * std::bit_width(spans.size());
                        if (build_steps > max_steps) {
                            return false;
                        }
                    }
                    std::sort(spans.begin(), spans.end());
                    for (size_t i = 0; i + 1 < spans.size(); i += 2) {
                        const int j_end = first_cell_from(spans[i + 1], origin.y, plan.cell, row_jitter_y, ny);
                        for (int j = first_cell_from(spans[i], origin.y, plan.cell, row_jitter_y, ny); j < j_end; ++j) {
                            const float y = origin.y + (j + 0.5f) * plan.cell + row_jitter_y;
                            if (++build_steps > max_steps) {
                                return false;
                            }
                            // Kept within the face, which a nearly parallel one would overshoot
                            const float x = std::fmax(face_min.x, std::fmin(face_max.x,
                                -(plane.normal.y * y + plane.normal.z * z + plane.offset) / plane.normal.x));
                            const int row = k * ny + j;
                            if (x < first_centre_x) {
                                row_starts_inside[row] ^= 1;
                            }
                            else if (x < last_centre_x) {
                                if (crossings.size() == max_room_crossings) {
                                    return false;
                                }
                                crossings.emplace_back(row, x);
                            }
                        }
                    }
                }
            }
            build_steps += crossings.size() * std::bit_width(crossings.size());
            if (build_steps > max_build_steps) {
                return false;
            }
            std::sort(crossings.begin(), crossings.end());

            size_t next = 0;
            for (int row = 0; row < ny * nz; ++row) {
                bool in_room = row_starts_inside[row] != 0;
                if (!in_room && (next == crossings.size() || crossings[next].first != row)) {
                    continue;
                }
                build_steps += nx;
                if (build_steps > max_build_steps) {
                    return false;
                }
                for (int x = 0; x < nx; ++x) {
                    const float centre_x = origin.x + (x + 0.5f) * plan.cell;
                    for (; next < crossings.size() && crossings[next].first == row
                           && crossings[next].second < centre_x; ++next) {
                        in_room = !in_room;
                    }
                    CellState& cell_state = states[size_t(row) * nx + x];
                    if (in_room && (state == CellState::inside || cell_state == CellState::unknown)) {
                        cell_state = state;
                    }
                }
                while (next < crossings.size() && crossings[next].first == row) {
                    ++next;
                }
            }
            return true;
        }
    }

    LiquidVolumeMask::LiquidVolumeMask(ID3D11Device* device) : device_{device}
    {
        CD3D11_SAMPLER_DESC desc{D3D11_DEFAULT};
        desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        DF_GR_D3D11_CHECK_HR(device_->CreateSamplerState(&desc, &sampler_));
    }

    void LiquidVolumeMask::update(ID3D11DeviceContext* device_context, bool force_rebuild)
    {
        rf::GSolid* solid = engine::level_solid();
        if (engine::underwater_fx() < 1 || !solid) {
            if (force_rebuild) {
                built_solid_ = nullptr;
                regions_.clear();
                srv_.release();
                texture_.release();
                bind(device_context);
            }
            return;
        }
        // Not rebuilt when rooms change mid-level (geomod): the stall would land in a frame
        if (force_rebuild || solid != built_solid_) {
            built_solid_ = solid;
            build();
        }
        bind(device_context);
    }

    LiquidMaskGPUData LiquidVolumeMask::region(const rf::GRoom* room) const
    {
        for (const Region& region : regions_) {
            if (region.room == room && region.uid == room->uid) {
                return region.data;
            }
        }
        return {};
    }

    void LiquidVolumeMask::bind(ID3D11DeviceContext* device_context)
    {
        ID3D11ShaderResourceView* srv = srv_;
        device_context->PSSetShaderResources(mask_srv_slot, 1, &srv);
        ID3D11SamplerState* sampler = sampler_;
        device_context->PSSetSamplers(mask_sampler_slot, 1, &sampler);
    }

    void LiquidVolumeMask::build()
    {
        regions_.clear();
        srv_.release();
        texture_.release();

        auto& all_rooms = engine::level_solid()->all_rooms;

        std::vector<Plan> plans;
        for (rf::GRoom* room : all_rooms) {
            if (!is_liquid_room(room)) {
                continue;
            }
            // Only the liquid under the surface is fogged or casts caustics, so only rooms overlapping the box under
            // it count. The box is unexpanded, so a room that merely touches it is no overlap.
            const float surface = surface_y(room);
            Plan plan{room, {room->bbox_max, room->bbox_min}, room->bbox_max.y, {room}, {}, 0.0f, {}, {}};
            for (rf::GRoom* other : all_rooms) {
                if (!other || other == room || other->is_detail) {
                    continue;
                }
                const Box overlap{
                    {std::max(room->bbox_min.x, other->bbox_min.x), std::max(room->bbox_min.y, other->bbox_min.y),
                     std::max(room->bbox_min.z, other->bbox_min.z)},
                    {std::min(room->bbox_max.x, other->bbox_max.x), std::min(room->bbox_max.y, other->bbox_max.y),
                     std::min(room->bbox_max.z, other->bbox_max.z)},
                };
                if (!(overlap.min.x < overlap.max.x && overlap.min.y < std::min(overlap.max.y, surface)
                      && overlap.min.z < overlap.max.z)) {
                    continue;
                }
                if (same_liquid(room, other)) {
                    plan.inside_rooms.push_back(other);
                    continue;
                }
                plan.outside_rooms.push_back(other);
                plan.domain.min = {std::min(plan.domain.min.x, overlap.min.x),
                                   std::min(plan.domain.min.y, overlap.min.y),
                                   std::min(plan.domain.min.z, overlap.min.z)};
                plan.domain.max = {std::max(plan.domain.max.x, overlap.max.x),
                                   std::max(plan.domain.max.y, std::min(overlap.max.y, surface)),
                                   std::max(plan.domain.max.z, overlap.max.z)};
            }
            if (!plan.outside_rooms.empty()) {
                plans.push_back(std::move(plan));
            }
            if (plans.size() > max_plans) {
                xlog::warn("[Liquid] over {} liquid rooms for a volume mask; they keep their boxes", max_plans);
                return;
            }
        }
        if (plans.empty()) {
            return;
        }

        float shared_cell = min_cell_size;
        std::array<int, 3> atlas_dims{};
        for (int attempt = 0; !fit_atlas(plans, shared_cell, atlas_dims); ++attempt) {
            if (attempt == max_fit_attempts) {
                xlog::warn("[Liquid] volume mask of {} liquid rooms does not fit; they keep their boxes", plans.size());
                return;
            }
            shared_cell *= cell_growth;
        }

        const size_t row_pitch = size_t(atlas_dims[0]);
        const size_t slice_pitch = row_pitch * size_t(atlas_dims[1]);
        std::vector<uint8_t> atlas(slice_pitch * size_t(atlas_dims[2]), 255);
        std::vector<CellState> states;
        std::vector<CellState> resolved;
        float max_cell = 0.0f;
        size_t build_steps = 0;
        for (Plan& plan : plans) {
            const auto [nx, ny, nz] = plan.dims;
            auto index = [nx, ny](int x, int y, int z) { return (size_t(z) * ny + y) * nx + x; };
            max_cell = std::max(max_cell, plan.cell);
            if (build_steps > max_build_steps) {
                plan.masked = false;
                continue;
            }

            // A cell centre in no room is in solid
            states.assign(size_t(nx) * ny * nz, CellState::unknown);
            auto mark_rooms = [&](const std::vector<const rf::GRoom*>& rooms, CellState state) {
                for (const rf::GRoom* room : rooms) {
                    if (!mark_room(plan, room, state, states, build_steps)) {
                        if (build_steps > max_build_steps) {
                            xlog::warn("[Liquid] volume mask build hit its work limit at liquid room {}; it and the "
                                       "rest keep their boxes", plan.room->uid);
                        }
                        else {
                            xlog::warn("[Liquid] room {} crosses the volume mask too often; liquid room {} keeps "
                                       "its box", room->uid, plan.room->uid);
                        }
                        return false;
                    }
                }
                return true;
            };
            plan.masked = mark_rooms(plan.inside_rooms, CellState::inside)
                && mark_rooms(plan.outside_rooms, CellState::outside);
            if (!plan.masked) {
                continue;
            }

            auto count_neighbours = [&](const std::vector<CellState>& cells, int x, int y, int z, CellState state) {
                int count = 0;
                for (int dz = std::max(z - 1, 0); dz <= std::min(z + 1, nz - 1); ++dz) {
                    for (int dy = std::max(y - 1, 0); dy <= std::min(y + 1, ny - 1); ++dy) {
                        for (int dx = std::max(x - 1, 0); dx <= std::min(x + 1, nx - 1); ++dx) {
                            count += cells[index(dx, dy, dz)] == state;
                        }
                    }
                }
                return count;
            };

            // A cell centre in solid takes the side most of its neighbours are on (this room on a tie), so
            // filtering at a wall blends the two rooms
            resolved.resize(states.size());
            for (int z = 0; z < nz; ++z) {
                for (int y = 0; y < ny; ++y) {
                    for (int x = 0; x < nx; ++x) {
                        CellState state = states[index(x, y, z)];
                        if (state == CellState::unknown) {
                            state = count_neighbours(states, x, y, z, CellState::outside)
                                > count_neighbours(states, x, y, z, CellState::inside)
                                ? CellState::outside : CellState::inside;
                        }
                        resolved[index(x, y, z)] = state;
                    }
                }
            }

            // Cells next to another room are marked half inside: the fog ignores them, so filtering never
            // carries it into that room, while caustics still reach the wall
            const auto [ox, oy, oz] = plan.atlas_offset;
            for (int z = 0; z < nz; ++z) {
                for (int y = 0; y < ny; ++y) {
                    for (int x = 0; x < nx; ++x) {
                        uint8_t value = 0;
                        if (resolved[index(x, y, z)] == CellState::inside) {
                            value = count_neighbours(resolved, x, y, z, CellState::outside) ? 128 : 255;
                        }
                        atlas[size_t(oz + z) * slice_pitch + size_t(oy + y) * row_pitch + size_t(ox + x)] = value;
                    }
                }
            }
        }

        const auto num_masked = std::count_if(plans.begin(), plans.end(), [](const Plan& plan) { return plan.masked; });
        if (num_masked == 0) {
            return;
        }

        D3D11_TEXTURE3D_DESC desc{};
        desc.Width = atlas_dims[0];
        desc.Height = atlas_dims[1];
        desc.Depth = atlas_dims[2];
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8_UNORM;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init_data{atlas.data(), static_cast<UINT>(row_pitch), static_cast<UINT>(slice_pitch)};
        if (FAILED(device_->CreateTexture3D(&desc, &init_data, &texture_))
            || FAILED(device_->CreateShaderResourceView(texture_, nullptr, &srv_))) {
            xlog::warn("[Liquid] volume mask texture unavailable; liquid rooms keep their boxes");
            srv_.release();
            texture_.release();
            return;
        }

        for (const Plan& plan : plans) {
            if (!plan.masked) {
                continue;
            }
            regions_.push_back({plan.room, plan.room->uid, {
                {plan.domain.min.x, plan.domain.min.y, plan.domain.min.z}, 1.0f / plan.cell,
                {float(plan.dims[0]), float(plan.dims[1]), float(plan.dims[2])}, 1.0f,
                {float(plan.atlas_offset[0]), float(plan.atlas_offset[1]), float(plan.atlas_offset[2])}, 0.0f,
                {1.0f / atlas_dims[0], 1.0f / atlas_dims[1], 1.0f / atlas_dims[2]}, 0.0f,
            }});
        }

        xlog::debug("[Liquid] volume mask: {} rooms, cells {:.2f} to {:.2f}, atlas {}x{}x{}, {} steps", num_masked,
                    shared_cell, max_cell, atlas_dims[0], atlas_dims[1], atlas_dims[2], build_steps);
    }
}
