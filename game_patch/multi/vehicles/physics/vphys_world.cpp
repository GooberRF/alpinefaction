#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <xlog/xlog.h>
#include <common/utils/list-utils.h>
#include "vphys_internal.h"
#include "../vehicle_physics.h"
#include "../vehicle.h"
#include "../../../misc/alpine_terrain.h"
#include "../../../os/os.h"
#include "../../../rf/clutter.h"
#include "../../../rf/entity.h"
#include "../../../rf/geometry.h"
#include "../../../rf/level.h"
#include "../../../rf/mover.h"
#include "../../../rf/object.h"
#include "../../../rf/physics.h"

bool vphys_class_is_automobile(int cls)
{
    return cls == VPHYS_CLASS_JEEP || cls == VPHYS_CLASS_APC || cls == VPHYS_CLASS_DRILLER;
}

namespace
{
    VehiclePhysicsParams g_params[VPHYS_CLASS_COUNT];

    void params_init_defaults()
    {
        VehiclePhysicsParams fighter{};
        // Wheelless: the size floor is the only gate, since the height exemption is wheeled-only.
        fighter.obstacle_min_size = 0.35f;
        fighter.deadstick_lift = 0.55f;
        fighter.parked_cylinder = 1.0f;
        fighter.cam_enable = 1.0f;
        g_params[VPHYS_CLASS_FIGHTER] = fighter;

        VehiclePhysicsParams sub{};
        sub.obstacle_min_size = 0.35f;
        sub.mass_scale = 2.5f;
        sub.gravity_scale = 0.0f;
        sub.hover_gain = 0.0f;
        sub.thrust_accel = 22.0f;
        sub.strafe_accel = 12.0f;
        sub.lift_accel = 16.0f;
        sub.max_speed = 15.0f;
        sub.speed_drag_accel = 45.0f;
        sub.linear_damping = 0.55f;
        sub.angular_damping = 0.20f;
        sub.pitch_rate = 3.8f;
        sub.yaw_rate = 3.8f;
        sub.rot_servo = 16.0f;
        sub.bank_gain = 0.18f;
        sub.bank_max = 0.25f;
        sub.bank_servo = 3.0f;
        sub.restitution = 0.05f;
        sub.hull_radius = 0.0f;
        sub.cam_enable = 1.0f;

        g_params[VPHYS_CLASS_SUB] = sub;

        VehiclePhysicsParams jeep{};
        jeep.mass_scale = 1.0f;
        jeep.restitution = 0.35f;
        jeep.gravity_scale = 1.30f;
        jeep.downforce_accel = 7.0f;
        jeep.air_gravity = 1.2f;
        jeep.engine_force = 8.5f;
        jeep.brake_force = 17.0f;
        jeep.idle_brake_force = 11.0f;
        jeep.throttle_ramp = 12.0f;
        jeep.throttle_release_ramp = 16.0f;
        jeep.brake_ramp = 30.0f;
        jeep.air_angular_damp = 5.0f;
        jeep.car_max_speed = 18.0f;
        jeep.steer_angle_max = 0.62f;
        jeep.steer_input_gain = 8.0f;
        jeep.steer_return_rate = 9.0f;
        jeep.steer_rate = 14.0f;
        jeep.steer_speed_falloff = 0.06f;
        jeep.suspension_stiffness = 16.0f;
        jeep.suspension_damping = 2.40f;
        jeep.suspension_compression = 6.40f;
        jeep.suspension_rest_length = 0.85f;
        jeep.suspension_max_travel_cm = 85.0f; // = rest, so Bullet's hard stop is at length 0
        jeep.suspension_max_force = 12000.0f;
        jeep.high_centre_assist = 3.0f;
        jeep.wheel_radius = 0.0f;
        jeep.wheel_friction = 2.4f;
        jeep.wheel_friction_rear = 1.9f;
        jeep.wheel_friction_rear_high = 2.8f;
        jeep.yaw_damp = 5.0f;
        jeep.roll_influence = 0.06f;
        jeep.car_angular_damping = 0.25f;
        jeep.upright_gain = 3.0f;
        jeep.upright_air_scale = 0.50f;
        jeep.tilt_damp = 2.5f;
        jeep.chassis_trim_side = 0.20f;
        jeep.chassis_clearance = 0.10f;
        jeep.step_climb_height = 0.60f;
        jeep.cam_enable = 1.0f;
        jeep.cam_dist = 7.0f;
        jeep.cam_height = 2.6f;
        g_params[VPHYS_CLASS_JEEP] = jeep;

        VehiclePhysicsParams apc{};
        apc.mass_scale = 1.0f;
        apc.restitution = 0.08f;
        apc.gravity_scale = 1.90f;
        apc.downforce_accel = 10.0f;
        apc.air_gravity = 1.3f;
        apc.engine_force = 14.0f;
        apc.brake_force = 20.0f;
        apc.idle_brake_force = 10.0f;
        apc.handbrake_force = 16.0f;
        apc.throttle_ramp = 1.6f;
        apc.throttle_release_ramp = 6.0f;
        apc.brake_ramp = 6.0f;
        apc.tread_wheels_per_side = 4.0f;
        apc.air_angular_damp = 4.0f;
        apc.car_max_speed = 10.5f;
        apc.steer_angle_max = 0.50f;
        apc.steer_input_gain = 4.5f;
        apc.steer_return_rate = 5.0f;
        apc.steer_rate = 11.0f;
        apc.steer_speed_falloff = 0.05f;
        apc.suspension_stiffness = 28.0f;
        apc.suspension_damping = 4.23f;
        apc.suspension_compression = 7.41f;
        apc.suspension_rest_length = 0.45f;
        apc.suspension_max_travel_cm = 45.0f;
        apc.high_centre_assist = 4.0f;
        apc.wheel_radius = 0.0f;
        apc.wheel_friction = 3.2f;
        apc.roll_influence = 0.03f;
        apc.car_angular_damping = 0.45f;
        apc.yaw_damp = 3.0f;
        apc.upright_gain = 4.5f;
        apc.upright_servo = 10.0f;
        apc.upright_air_scale = 0.50f;
        apc.tilt_damp = 4.5f;
        apc.chassis_trim_side = 0.20f;
        apc.chassis_clearance = 0.10f;
        apc.step_climb_height = 1.00f;
        apc.cam_enable = 1.0f;
        apc.cam_dist = 10.0f;
        apc.cam_height = 3.4f;
        g_params[VPHYS_CLASS_APC] = apc;

        VehiclePhysicsParams driller{};
        driller.mass_scale = 1.0f;
        driller.restitution = 0.08f;
        driller.gravity_scale = 1.95f;
        driller.downforce_accel = 11.0f;
        driller.air_gravity = 1.35f;
        driller.engine_force = 18.0f;
        driller.brake_force = 20.0f;
        driller.idle_brake_force = 10.0f;
        driller.handbrake_force = 16.0f;
        driller.throttle_ramp = 1.3f;
        driller.throttle_release_ramp = 5.0f;
        driller.brake_ramp = 6.0f;
        driller.tread_wheels_per_side = 5.0f;
        driller.air_angular_damp = 4.0f;
        driller.car_max_speed = 8.5f;
        driller.steer_angle_max = 0.34f;
        driller.steer_input_gain = 3.5f;
        driller.steer_return_rate = 3.5f;
        driller.steer_rate = 10.0f;
        driller.steer_speed_falloff = 0.08f;
        driller.suspension_stiffness = 24.0f;
        driller.suspension_damping = 3.92f;
        driller.suspension_compression = 6.86f;
        driller.suspension_rest_length = 0.42f;
        driller.suspension_max_travel_cm = 42.0f;
        driller.high_centre_assist = 6.0f;
        driller.wheel_radius = 0.0f;
        driller.wheel_friction = 3.4f;
        driller.roll_influence = 0.03f;
        driller.car_angular_damping = 0.50f;
        driller.yaw_damp = 3.0f;
        driller.upright_gain = 5.0f;
        driller.upright_servo = 10.0f;
        driller.upright_air_scale = 0.50f;
        driller.tilt_damp = 5.0f;
        driller.drill_speed_scale = 0.67f;
        // The front trim is NEGATIVE: the contact box must reach a wall before the drill tips do.
        driller.chassis_trim_front = -1.30f;
        driller.chassis_trim_rear = 0.20f;
        driller.chassis_trim_side = 0.40f;
        driller.chassis_trim_top = 0.55f;
        driller.chassis_clearance = 0.10f;
        driller.step_climb_height = 1.20f;
        driller.drill_probe_reach = 8.97f;
        driller.cam_enable = 1.0f;
        driller.cam_dist = 15.0f;
        driller.cam_height = 4.5f;
        g_params[VPHYS_CLASS_DRILLER] = driller;
    }

    // How many nearby clutter objects ONE hull may mirror into the world...
    constexpr int vphys_max_obstacles = 16;
    // ...and the ceiling on the whole registry, across every simulated hull at once.
    constexpr int vphys_max_obstacles_world = 64;
    // Ceiling on how bouncy an obstacle BOX may be, whatever the class's wall restitution.
    constexpr float obstacle_max_restitution = 0.05f;
} // namespace

VehiclePhysicsWorld g_vphys;

namespace
{
    // The container's only two mutators, so bodies and by_handle cannot drift apart.
    VehicleSimBody* sim_body_add(std::unique_ptr<VehicleSimBody> body)
    {
        VehicleSimBody* raw = body.get();
        g_vphys.bodies.push_back(std::move(body));
        g_vphys.by_handle[raw->vehicle_handle] = raw;
        return raw;
    }

    void sim_body_erase(VehicleSimBody* body)
    {
        if (!body) {
            return;
        }
        g_vphys.by_handle.erase(body->vehicle_handle);
        if (g_vphys.driven == body) {
            g_vphys.driven = nullptr;
        }
        g_vphys.bodies.erase(std::remove_if(g_vphys.bodies.begin(), g_vphys.bodies.end(),
                                            [body](const std::unique_ptr<VehicleSimBody>& b) {
                                                return b.get() == body;
                                            }),
                             g_vphys.bodies.end());
    }
} // namespace

VehicleSimBody* sim_body_from_handle(int handle)
{
    if (handle == -1) {
        return nullptr;
    }
    const auto it = g_vphys.by_handle.find(handle);
    return it != g_vphys.by_handle.end() ? it->second : nullptr;
}

const VehiclePhysicsParams& params_for_class(int cls)
{
    return g_params[cls >= 0 && cls < VPHYS_CLASS_COUNT ? cls : VPHYS_CLASS_FIGHTER];
}

namespace
{
    void obstacles_destroy_all()
    {
        for (auto& [handle, obstacle] : g_vphys.obstacles) {
            if (g_vphys.world && obstacle.body) {
                g_vphys.world->removeRigidBody(obstacle.body);
                g_vphys.manifolds_dirty = true;
            }
            delete obstacle.body;
            delete obstacle.motion_state;
            delete obstacle.shape;
        }
        g_vphys.obstacles.clear();
    }

    void sim_body_free(VehicleSimBody& b)
    {
        car_teardown(b);
        if (b.body) {
            if (g_vphys.world) {
                g_vphys.world->removeRigidBody(b.body);
                g_vphys.manifolds_dirty = true;
            }
            delete b.body;
            b.body = nullptr;
        }
        delete b.motion_state;
        b.motion_state = nullptr;
        delete b.parked_shape;
        b.parked_shape = nullptr;
        delete b.shape;
        b.shape = nullptr;
        b.liquid_surface_valid = false;
        // vehicle_handle is deliberately left alone: sim_body_erase removes the body by it.
    }

    void sim_bodies_free_all()
    {
        for (auto& b : g_vphys.bodies) {
            sim_body_free(*b);
        }
        g_vphys.bodies.clear();
        g_vphys.by_handle.clear();
        g_vphys.driven = nullptr;
    }

    // The LEVEL MESH: RF's solid geometry mirrored into Bullet, one static body per (room, cell).
    struct LevelMeshCell
    {
        int x = 0;
        int y = 0;
        int z = 0;
        bool operator==(const LevelMeshCell& o) const
        {
            return x == o.x && y == o.y && z == o.z;
        }
    };

    struct LevelMeshCellHash
    {
        size_t operator()(const LevelMeshCell& c) const
        {
            // Unsigned throughout: a cell index past +-29 overflows a signed multiply, which is UB.
            return static_cast<size_t>(static_cast<uint32_t>(c.x) * 73856093u)
                 ^ static_cast<size_t>(static_cast<uint32_t>(c.y) * 19349663u)
                 ^ static_cast<size_t>(static_cast<uint32_t>(c.z) * 83492791u);
        }
    };

    struct LevelMeshChunk
    {
        std::vector<float> verts;   // owned copy - Bullet must never point at engine memory
        std::vector<int> indices;
        btTriangleIndexVertexArray* iva = nullptr;
        btBvhTriangleMeshShape* shape = nullptr;
        btRigidBody* body = nullptr;
        LevelMeshCell cell{};
        int tris = 0;
        size_t bytes = 0;
        // The chunk's ACTUAL triangle bounds, not the cell's box: the overlay and the remesh wake.
        rf::Vector3 aabb_min{};
        rf::Vector3 aabb_max{};
        // Which rebuild pass last produced this body; 0 for one the level build made.
        uint32_t last_rebuild_serial = 0;
    };

    // A room under vphys_mesh_chunk_min_tris, or a terrain chunk room, is a SINGLE chunk at cell (0,0,0);
    // the mode never changes.
    struct LevelMeshRoomChunks
    {
        bool single = false;
        std::vector<std::unique_ptr<LevelMeshChunk>> chunks;
    };
    std::unordered_map<rf::GRoom*, LevelMeshRoomChunks> g_level_mesh;

    struct MoverWakeBox
    {
        btVector3 lo{0.0f, 0.0f, 0.0f};
        btVector3 hi{0.0f, 0.0f, 0.0f};
    };
    constexpr size_t mover_wake_box_max = 16;

    // One KINEMATIC body per mover brush: the brush GSolid is BRUSH-LOCAL, posed by mb.pos/mb.orient.
    struct MoverMeshBody
    {
        int brush_handle = -1;
        int brush_uid = -1;         // handles are recycled; the uid is not
        std::vector<float> verts;   // owned copy - Bullet must never point at engine memory
        std::vector<int> indices;
        btTriangleIndexVertexArray* iva = nullptr;
        btBvhTriangleMeshShape* shape = nullptr;
        btDefaultMotionState* motion_state = nullptr;
        btRigidBody* body = nullptr;
        rf::Vector3 last_pos{};
        rf::Matrix3 last_orient{};
        // The pose Bullet's m_interpolationWorldTransform holds, plus engine time since it was taken.
        rf::Vector3 carry_pos{};
        rf::Matrix3 carry_orient{};
        float engine_dt_accum = 0.0f;
        std::array<MoverWakeBox, mover_wake_box_max> pending_wake{};
        int pending_wake_count = 0;
        int tris = 0;
        size_t bytes = 0;
        bool in_world = false;
    };
    std::vector<std::unique_ptr<MoverMeshBody>> g_mover_mesh;

    void mover_wake_bank(MoverMeshBody& m, const btVector3& lo, const btVector3& hi)
    {
        if (m.pending_wake_count < static_cast<int>(mover_wake_box_max)) {
            m.pending_wake[m.pending_wake_count++] = MoverWakeBox{lo, hi};
            return;
        }
        MoverWakeBox& last = m.pending_wake[mover_wake_box_max - 1];
        last.lo.setMin(lo);
        last.hi.setMax(hi);
    }

    void wake_sleeping_bodies_in(const btVector3& box_lo, const btVector3& box_hi)
    {
        // getAabb is the TIGHT shape AABB, and a resting manifold holds contacts at a positive gap.
        constexpr float wake_pad = 0.10f;
        constexpr float body_pad = 0.25f;
        const btVector3 pad(wake_pad, wake_pad, wake_pad);
        const btVector3 bpad(body_pad, body_pad, body_pad);
        const btVector3 lo = box_lo - pad;
        const btVector3 hi = box_hi + pad;
        for (auto& owned : g_vphys.bodies) {
            if (!owned->body) {
                continue;
            }
            // WANTS_DEACTIVATION too: buildIslands puts such a body to sleep on the next substep.
            const int state = owned->body->getActivationState();
            if (state != ISLAND_SLEEPING && state != WANTS_DEACTIVATION) {
                continue;
            }
            btVector3 blo;
            btVector3 bhi;
            owned->body->getAabb(blo, bhi);
            // The hull side reaches down to the WHEEL line: a car's shape bottom is above it.
            blo.setY(blo.y() - (owned->wheel_reach + 0.5f));
            blo -= bpad;
            bhi += bpad;
            if (blo.x() <= hi.x() && bhi.x() >= lo.x() && blo.y() <= hi.y()
                && bhi.y() >= lo.y() && blo.z() <= hi.z() && bhi.z() >= lo.z()) {
                owned->body->activate(true);
            }
        }
    }

    // Must also run before the mover leaves the world: those wakes were already owed.
    void mover_wake_flush(MoverMeshBody& m)
    {
        for (int i = 0; i < m.pending_wake_count; ++i) {
            wake_sleeping_bodies_in(m.pending_wake[i].lo, m.pending_wake[i].hi);
        }
        m.pending_wake_count = 0;
    }
} // namespace

// Cleared at multi_stop as well as level init: level-init-only state goes stale across a switch.
bool g_level_has_bullet_vehicles = false;

// A vehicle carries a player, so the PLAYER standard applies: OF_WEAPON_ONLY_COLLIDE only.
bool mover_brush_is_solid(const rf::Object& mb)
{
    return (mb.obj_flags & rf::OF_WEAPON_ONLY_COLLIDE) == 0;
}

namespace
{
    // Also the "this level has been tried" latch, so a room-less level is attempted once.
    bool g_level_mesh_built = false;
    size_t g_level_mesh_bytes = 0;
    int g_level_mesh_tris = 0;
    // Bumped per rebuild pass and stamped on every chunk it finishes; 0 = no remesh on this level yet.
    uint32_t g_level_mesh_rebuild_serial = 0;
    // A room with no dirty cell and all==false is still a KEY: the dead-room check needs to see it.
    struct RemeshRequest
    {
        bool all = false;
        std::unordered_set<LevelMeshCell, LevelMeshCellHash> cells;
    };
    std::unordered_map<rf::GRoom*, RemeshRequest> g_remesh_pending;
    // timer::get_i64(1000) instant the oldest pending mark was queued; 0 = nothing pending.
    int64_t g_remesh_pending_since_ms = 0;

    constexpr float vphys_mesh_cell = 8.0f;        // world units per chunk edge
    constexpr int vphys_mesh_chunk_min_tris = 256; // below this a room is one chunk
    constexpr int64_t vphys_remesh_max_ms = 3000;

    // NaN or an astronomical float makes static_cast<int> UB; clamp to a garbage but CONSISTENT key.
    int level_mesh_cell_axis(float v)
    {
        const float c = std::floor(v / vphys_mesh_cell);
        if (!(c > -1.0e6f && c < 1.0e6f)) { // false for NaN too
            return 0;
        }
        return static_cast<int>(c);
    }

    LevelMeshCell level_mesh_cell_of(const rf::Vector3& p)
    {
        return {level_mesh_cell_axis(p.x), level_mesh_cell_axis(p.y), level_mesh_cell_axis(p.z)};
    }

    void remesh_mark_cell(RemeshRequest& req, const LevelMeshCell& cell)
    {
        if (req.all) {
            return;
        }
        req.cells.insert(cell);
    }

    bool remesh_cell_is_dirty(const RemeshRequest& req, const LevelMeshCell& cell)
    {
        return req.all || req.cells.count(cell) != 0;
    }

    using LevelMeshRoomSet = std::unordered_set<const rf::GRoom*>;

    // Skybox decoration: detail rooms listed under the sky room. Stock collision reaches a detail room
    // only through a parent, so one a non-sky room also lists stays solid.
    LevelMeshRoomSet level_mesh_sky_detail_rooms(rf::GSolid* solid)
    {
        LevelMeshRoomSet out;
        if (!solid) {
            return out;
        }
        // detail_rooms is only read on a non-detail room: the engine can leave a detail room's corrupt.
        for (rf::GRoom* room : solid->all_rooms) {
            if (room && room->is_sky && !room->is_detail) {
                for (rf::GRoom* detail : room->detail_rooms) {
                    if (detail) {
                        out.insert(detail);
                    }
                }
            }
        }
        if (out.empty()) {
            return out;
        }
        for (rf::GRoom* room : solid->all_rooms) {
            if (room && !room->is_sky && !room->is_detail) {
                for (rf::GRoom* detail : room->detail_rooms) {
                    out.erase(detail);
                }
            }
        }
        return out;
    }

    bool level_mesh_room_is_skipped(const rf::GRoom* room, const LevelMeshRoomSet& sky_detail)
    {
        return !room || room->is_sky || sky_detail.count(room) != 0;
    }

    bool level_mesh_face_is_solid(const rf::GFace& face)
    {
        const uint32_t flags = face.attributes.flags;
        if (flags & (rf::FACE_LIQUID | rf::FACE_SHOW_SKY)) {
            return false;
        }
        if (face.attributes.portal_id != 0) {
            return false; // portals are not solid
        }
        return true;
    }

    // The verts/indices survive, so a chunk being REBUILT in place can be freed and finished again.
    void level_mesh_free_chunk(LevelMeshChunk& m)
    {
        if (m.body) {
            if (g_vphys.world) {
                g_vphys.world->removeRigidBody(m.body);
                g_vphys.manifolds_dirty = true;
            }
            delete m.body;
            m.body = nullptr;
        }
        delete m.shape;
        m.shape = nullptr;
        delete m.iva;
        m.iva = nullptr;
        g_level_mesh_tris -= m.tris;
        g_level_mesh_bytes -= m.bytes;
        m.tris = 0;
        m.bytes = 0;
    }

    // Faces are convex n-gons in a circular edge loop, so a fan from the first vertex is exact.
    int level_mesh_append_face(std::vector<float>& verts, std::vector<int>& indices,
                               const rf::GFace& face)
    {
        rf::GFaceVertex* fv = face.edge_loop;
        if (!fv) {
            return 0;
        }
        const int base = static_cast<int>(verts.size() / 3);
        int n = 0;
        rf::GFaceVertex* it = fv;
        while (it) {
            verts.push_back(it->vertex->pos.x);
            verts.push_back(it->vertex->pos.y);
            verts.push_back(it->vertex->pos.z);
            ++n;
            it = it->next;
            if (it == fv || n > rf::max_face_vertices) {
                break;
            }
        }
        if (n < 3) {
            verts.resize(static_cast<size_t>(base) * 3);
            return 0;
        }
        int tris = 0;
        for (int i = 1; i + 1 < n; ++i) {
            indices.push_back(base);
            indices.push_back(base + i);
            indices.push_back(base + i + 1);
            ++tris;
        }
        return tris;
    }

    // False for exactly the faces level_mesh_append_face declines, so the two agree on chunk content.
    bool level_mesh_face_bounds(const rf::GFace& face, rf::Vector3& centroid, rf::Vector3& lo,
                                rf::Vector3& hi)
    {
        rf::GFaceVertex* fv = face.edge_loop;
        if (!fv) {
            return false;
        }
        int n = 0;
        rf::GFaceVertex* it = fv;
        centroid = rf::Vector3{0.0f, 0.0f, 0.0f};
        while (it) {
            const rf::Vector3& p = it->vertex->pos;
            if (n == 0) {
                lo = p;
                hi = p;
            }
            else {
                lo.x = std::min(lo.x, p.x);
                lo.y = std::min(lo.y, p.y);
                lo.z = std::min(lo.z, p.z);
                hi.x = std::max(hi.x, p.x);
                hi.y = std::max(hi.y, p.y);
                hi.z = std::max(hi.z, p.z);
            }
            centroid += p;
            ++n;
            it = it->next;
            if (it == fv || n > rf::max_face_vertices) {
                break;
            }
        }
        if (n < 3) {
            return false;
        }
        centroid /= static_cast<float>(n);
        return true;
    }

    // The WHOLE-room form, for the mover brushes: a mover is one body and is never chunked.
    int level_mesh_append_room(std::vector<float>& verts, std::vector<int>& indices, rf::GRoom* room)
    {
        int tris = 0;
        for (rf::GFace& face : room->face_list) {
            if (!level_mesh_face_is_solid(face)) {
                continue;
            }
            tris += level_mesh_append_face(verts, indices, face);
        }
        return tris;
    }

    struct ChunkBuild
    {
        std::vector<float> verts;
        std::vector<int> indices;
        int tris = 0;
        bool consumed = false;
    };
    using ChunkBuckets = std::unordered_map<LevelMeshCell, ChunkBuild, LevelMeshCellHash>;

    // Every bucket carries its geometry, so a drifted chunk is rebuilt without a second walk.
    int level_mesh_bucket_room(rf::GRoom* room, ChunkBuckets& out, bool single)
    {
        int tris = 0;
        for (rf::GFace& face : room->face_list) {
            if (!level_mesh_face_is_solid(face)) {
                continue;
            }
            LevelMeshCell cell{};
            if (!single) {
                rf::Vector3 centroid;
                rf::Vector3 lo;
                rf::Vector3 hi;
                if (!level_mesh_face_bounds(face, centroid, lo, hi)) {
                    continue;
                }
                cell = level_mesh_cell_of(centroid);
            }
            ChunkBuild& b = out[cell];
            const int n = level_mesh_append_face(b.verts, b.indices, face);
            b.tris += n;
            tris += n;
        }
        return tris;
    }

    bool level_mesh_chunk_finish(LevelMeshChunk& m)
    {
        if (m.tris == 0 || !g_vphys.world) {
            return false;
        }
        m.iva = new btTriangleIndexVertexArray(m.tris, m.indices.data(), 3 * sizeof(int),
                                               static_cast<int>(m.verts.size() / 3), m.verts.data(),
                                               3 * sizeof(float));
        m.shape = new btBvhTriangleMeshShape(m.iva, true);
        btTransform t;
        t.setIdentity();
        btRigidBody::btRigidBodyConstructionInfo ci(0.0f, nullptr, m.shape, btVector3(0, 0, 0));
        ci.m_startWorldTransform = t;
        // Bullet's restitution combine rule is a PRODUCT, so this zero makes every hull's own inert.
        ci.m_restitution = 0.0f;
        ci.m_friction = 0.6f;
        m.body = new btRigidBody(ci);
        g_vphys.world->addRigidBody(m.body, vphys_group_level, vphys_group_hull);
        m.bytes = m.verts.size() * sizeof(float) + m.indices.size() * sizeof(int);
        m.aabb_min = rf::Vector3{m.verts[0], m.verts[1], m.verts[2]};
        m.aabb_max = m.aabb_min;
        for (size_t i = 3; i + 2 < m.verts.size(); i += 3) {
            m.aabb_min.x = std::min(m.aabb_min.x, m.verts[i]);
            m.aabb_min.y = std::min(m.aabb_min.y, m.verts[i + 1]);
            m.aabb_min.z = std::min(m.aabb_min.z, m.verts[i + 2]);
            m.aabb_max.x = std::max(m.aabb_max.x, m.verts[i]);
            m.aabb_max.y = std::max(m.aabb_max.y, m.verts[i + 1]);
            m.aabb_max.z = std::max(m.aabb_max.z, m.verts[i + 2]);
        }
        m.last_rebuild_serial = g_level_mesh_rebuild_serial;
        g_level_mesh_tris += m.tris;
        g_level_mesh_bytes += m.bytes;
        return true;
    }

    int level_mesh_build_room(rf::GRoom* room)
    {
        // RF's own GSolid::collide skips a sky room outright; callers also skip its detail rooms.
        if (!room || room->is_sky) {
            return 0;
        }
        const bool terrain = alpine_terrain_is_chunk_room(room);
        ChunkBuckets buckets;
        const int tris = level_mesh_bucket_room(room, buckets, terrain);
        if (tris == 0) {
            return 0;
        }
        LevelMeshRoomChunks entry;
        entry.single = terrain || tris < vphys_mesh_chunk_min_tris;
        if (entry.single && !terrain) {
            // Re-bucket: a single-chunk room's chunk must sit at cell (0,0,0), where rebuilds look.
            buckets.clear();
            level_mesh_bucket_room(room, buckets, true);
        }
        int built = 0;
        for (auto& [cell, b] : buckets) {
            auto m = std::make_unique<LevelMeshChunk>();
            m->cell = cell;
            m->verts = std::move(b.verts);
            m->indices = std::move(b.indices);
            m->tris = b.tris;
            if (!level_mesh_chunk_finish(*m)) {
                continue;
            }
            built += m->tris;
            entry.chunks.push_back(std::move(m));
        }
        if (entry.chunks.empty()) {
            return 0;
        }
        // Overwriting an entry that holds chunks would free their bodies without removeRigidBody.
        auto [pos, inserted] = g_level_mesh.try_emplace(room);
        if (!inserted) {
            for (auto& m : pos->second.chunks) {
                level_mesh_free_chunk(*m);
            }
        }
        pos->second = std::move(entry);
        return built;
    }

    size_t level_mesh_chunk_count()
    {
        size_t n = 0;
        for (const auto& kv : g_level_mesh) {
            n += kv.second.chunks.size();
        }
        return n;
    }

    void mover_mesh_free(MoverMeshBody& m)
    {
        if (m.body) {
            if (g_vphys.world && m.in_world) {
                g_vphys.world->removeRigidBody(m.body);
                g_vphys.manifolds_dirty = true;
            }
            m.in_world = false;
            delete m.body;
            m.body = nullptr;
        }
        delete m.motion_state;
        m.motion_state = nullptr;
        delete m.shape;
        m.shape = nullptr;
        delete m.iva;
        m.iva = nullptr;
    }

    void mover_mesh_destroy()
    {
        for (auto& m : g_mover_mesh) {
            mover_mesh_free(*m);
        }
        g_mover_mesh.clear();
    }

    // Only ever the numerator of BOTH sides of a ratio, so monotone and consistent is enough.
    float mover_orient_delta_angle(const rf::Matrix3& a, const rf::Matrix3& b)
    {
        const float df = std::clamp(a.fvec.dot_prod(b.fvec), -1.0f, 1.0f);
        const float du = std::clamp(a.uvec.dot_prod(b.uvec), -1.0f, 1.0f);
        return std::max(std::acos(df), std::acos(du));
    }

    int mover_mesh_build()
    {
        if (!g_vphys.world) {
            return 0;
        }
        int brushes = 0;
        for (auto& mb : DoublyLinkedList{rf::mover_brush_list}) {
            ++brushes;
            if (!mb.geometry) {
                continue;
            }
            if (!mover_brush_is_solid(mb)) {
                continue;
            }
            auto m = std::make_unique<MoverMeshBody>();
            // A brush solid keeps faces DIRECTLY in GSolid::face_list; only the LEVEL solid is split.
            for (rf::GFace& face : mb.geometry->face_list) {
                if (!level_mesh_face_is_solid(face)) {
                    continue;
                }
                m->tris += level_mesh_append_face(m->verts, m->indices, face);
            }
            // Fallback only, for a brush that somehow carries rooms instead.
            if (m->tris == 0) {
                for (rf::GRoom* room : mb.geometry->all_rooms) {
                    if (room && !room->is_sky) {
                        m->tris += level_mesh_append_room(m->verts, m->indices, room);
                    }
                }
            }
            if (m->tris == 0) {
                continue;
            }
            m->brush_handle = mb.handle;
            m->brush_uid = mb.uid;
            m->last_pos = mb.pos;
            m->last_orient = mb.orient;
            m->carry_pos = mb.pos;
            m->carry_orient = mb.orient;
            m->iva = new btTriangleIndexVertexArray(m->tris, m->indices.data(), 3 * sizeof(int),
                                                    static_cast<int>(m->verts.size() / 3),
                                                    m->verts.data(), 3 * sizeof(float));
            m->shape = new btBvhTriangleMeshShape(m->iva, true);
            btTransform t;
            t.setBasis(to_bt(mb.orient));
            t.setOrigin(to_bt(mb.pos));
            m->motion_state = new btDefaultMotionState(t);
            btRigidBody::btRigidBodyConstructionInfo ci(0.0f, m->motion_state, m->shape,
                                                        btVector3(0, 0, 0));
            ci.m_restitution = 0.0f;
            ci.m_friction = 0.6f;
            m->body = new btRigidBody(ci);
            // setMassProps ORs in CF_STATIC_OBJECT for mass 0, so the kinematic flag must REPLACE it;
            // DISABLE_DEACTIVATION must stay - updateAabbs and saveKinematicState skip a sleeping body.
            m->body->setCollisionFlags((m->body->getCollisionFlags()
                                        & ~btCollisionObject::CF_STATIC_OBJECT)
                                       | btCollisionObject::CF_KINEMATIC_OBJECT);
            m->body->setActivationState(DISABLE_DEACTIVATION);
            g_vphys.world->addRigidBody(m->body, vphys_group_level, vphys_group_hull);
            m->in_world = true;
            m->bytes = m->verts.size() * sizeof(float) + m->indices.size() * sizeof(int);
            g_level_mesh_tris += m->tris;
            g_level_mesh_bytes += m->bytes;
            g_mover_mesh.push_back(std::move(m));
        }
        return brushes;
    }

    void level_mesh_destroy()
    {
        mover_mesh_destroy();
        for (auto& kv : g_level_mesh) {
            for (auto& m : kv.second.chunks) {
                level_mesh_free_chunk(*m);
            }
        }
        g_level_mesh.clear();
        g_remesh_pending.clear();
        g_remesh_pending_since_ms = 0;
        g_level_mesh_built = false;
        g_level_mesh_bytes = 0;
        g_level_mesh_tris = 0;
        g_level_mesh_rebuild_serial = 0;
    }
} // namespace

void level_mesh_build()
{
    level_mesh_destroy();
    if (!g_level_has_bullet_vehicles || !g_vphys.world) {
        return;
    }
    g_level_mesh_built = true;
    int rooms = 0;
    // A null level solid is not a reason to skip the movers, so only the room pass is guarded.
    if (rf::GSolid* solid = rf::level.geometry) {
        const LevelMeshRoomSet sky_detail = level_mesh_sky_detail_rooms(solid);
        for (rf::GRoom* room : solid->all_rooms) {
            if (level_mesh_room_is_skipped(room, sky_detail)) {
                continue;
            }
            if (level_mesh_build_room(room) == 0) {
                continue;
            }
            ++rooms;
        }
    }
    const int mover_brushes = mover_mesh_build();
    xlog::info("[vphys-mesh] built rooms={} chunks={} mover_brushes={} movers={} tris={} data={} KB",
               rooms, static_cast<int>(level_mesh_chunk_count()), mover_brushes,
               static_cast<int>(g_mover_mesh.size()), g_level_mesh_tris, g_level_mesh_bytes / 1024);
    if (!level_mesh_active()) {
        xlog::error("[vphys-mesh] level mesh unavailable - vehicles on this level have no world "
                    "collision");
    }
}

// step_will_run is what makes idle frames safe: a kinematic body's velocity comes from the pose
// delta against m_interpolationWorldTransform, which saveKinematicState owns on a stepping frame.
void movers_update_all(bool step_will_run, float step_time, float engine_dt)
{
    if (g_mover_mesh.empty() || !g_vphys.world) {
        return;
    }
    // The frames Bullet consumes the pending delta on: a step with a substep, or the no-step case.
    const bool consumed = step_time > 0.0f || !step_will_run;
    for (auto it = g_mover_mesh.begin(); it != g_mover_mesh.end();) {
        MoverMeshBody& m = **it;
        rf::Object* op = rf::obj_from_handle(m.brush_handle);
        // The uid compare is what makes a RECYCLED brush handle safe.
        if (!op || op->type != rf::OT_MOVER_BRUSH || op->uid != m.brush_uid) {
            mover_wake_flush(m);
            g_level_mesh_tris -= m.tris;
            g_level_mesh_bytes -= m.bytes;
            mover_mesh_free(m);
            it = g_mover_mesh.erase(it);
            continue;
        }
        // Insurance against build ordering, not a live signal: the flag is stamped at level init.
        const bool solid = mover_brush_is_solid(*op);
        if (solid != m.in_world) {
            if (solid) {
                g_vphys.world->addRigidBody(m.body, vphys_group_level, vphys_group_hull);
            }
            else {
                g_vphys.world->removeRigidBody(m.body);
                g_vphys.manifolds_dirty = true;
            }
            m.in_world = solid;
        }
        if (!solid) {
            mover_wake_flush(m);
            ++it;
            continue;
        }
        m.engine_dt_accum += engine_dt;
        constexpr float pose_epsilon_sq = 1e-8f;
        const bool moved = (op->pos - m.last_pos).len_sq() > pose_epsilon_sq
                        || (op->orient.rvec - m.last_orient.rvec).len_sq() > pose_epsilon_sq
                        || (op->orient.uvec - m.last_orient.uvec).len_sq() > pose_epsilon_sq
                        || (op->orient.fvec - m.last_orient.fvec).len_sq() > pose_epsilon_sq;
        bool moved_box = false;
        btVector3 moved_lo(0.0f, 0.0f, 0.0f);
        btVector3 moved_hi(0.0f, 0.0f, 0.0f);
        if (moved) {
            btVector3 old_lo;
            btVector3 old_hi;
            m.body->getAabb(old_lo, old_hi);
            btTransform t;
            t.setBasis(to_bt(op->orient));
            t.setOrigin(to_bt(op->pos));
            // From the CARRY anchor, not last_pos: a written-but-unconsumed pose spans several frames.
            bool teleport = false;
            if (step_time > 0.0f && m.engine_dt_accum > 1e-4f) {
                const float dist = (op->pos - m.carry_pos).len();
                const float ang = mover_orient_delta_angle(m.carry_orient, op->orient);
                const float v_true = dist / m.engine_dt_accum;
                const float v_implied = dist / step_time;
                const float w_true = ang / m.engine_dt_accum;
                const float w_implied = ang / step_time;
                teleport = v_implied > 2.5f * v_true + 1.0f || w_implied > 2.5f * w_true + 0.5f;
            }
            m.motion_state->setWorldTransform(t);
            m.body->setWorldTransform(t);
            // A teleport takes the anchor with it; otherwise saveKinematicState derives the velocity.
            if (!step_will_run || teleport) {
                m.body->setInterpolationWorldTransform(t);
            }
            m.last_pos = op->pos;
            m.last_orient = op->orient;
            m.body->getAabb(moved_lo, moved_hi);
            moved_lo.setMin(old_lo);
            moved_hi.setMax(old_hi);
            moved_box = true;
        }
        if (moved_box) {
            mover_wake_bank(m, moved_lo, moved_hi);
        }
        // Only ahead of a substep: a sleeping hull woken on a skipped frame would just idle awake.
        if (consumed) {
            mover_wake_flush(m);
        }
        // Outside the `moved` branch: Bullet re-anchors on every saveKinematicState, moved or not.
        if (consumed) {
            m.engine_dt_accum = 0.0f;
            m.carry_pos = op->pos;
            m.carry_orient = op->orient;
        }
        ++it;
    }
}

// Rebuild exactly the CHUNKS a carve touched, once the geomod machine reports settled. ASSUMPTION:
// the boolean only touches faces in the crater; the triangle-count drift test below is the backstop.
void level_mesh_rebuild_pending()
{
    if (g_remesh_pending.empty() || !g_level_mesh_built) {
        g_remesh_pending.clear();
        return;
    }
    ++g_level_mesh_rebuild_serial;
    rf::GSolid* solid = rf::level.geometry;
    const LevelMeshRoomSet sky_detail = level_mesh_sky_detail_rooms(solid);
    // Old and new bounds of every chunk freed or built: a sleeping body there may have lost its ground.
    bool changed = false;
    btVector3 changed_lo(0.0f, 0.0f, 0.0f);
    btVector3 changed_hi(0.0f, 0.0f, 0.0f);
    const auto mark_changed = [&](const LevelMeshChunk& m) {
        if (!changed) {
            changed_lo = to_bt(m.aabb_min);
            changed_hi = to_bt(m.aabb_max);
            changed = true;
            return;
        }
        changed_lo.setMin(to_bt(m.aabb_min));
        changed_hi.setMax(to_bt(m.aabb_max));
    };
    for (auto& pending : g_remesh_pending) {
        rf::GRoom* room = pending.first;
        const RemeshRequest& req = pending.second;
        // The boolean can DELETE a room, so validate the pointer against all_rooms - never deref it.
        const bool alive =
            solid
            && std::any_of(solid->all_rooms.begin(), solid->all_rooms.end(),
                           [room](rf::GRoom* r) { return r == room; });
        auto it = g_level_mesh.find(room);
        if (!alive) {
            if (it != g_level_mesh.end()) {
                for (auto& m : it->second.chunks) {
                    mark_changed(*m);
                    level_mesh_free_chunk(*m);
                }
                g_level_mesh.erase(it);
            }
            continue;
        }
        if (it == g_level_mesh.end()) {
            if (!level_mesh_room_is_skipped(room, sky_detail) && level_mesh_build_room(room) > 0) {
                for (const auto& m : g_level_mesh[room].chunks) {
                    mark_changed(*m);
                }
            }
            continue;
        }
        LevelMeshRoomChunks& entry = it->second;
        ChunkBuckets buckets;
        level_mesh_bucket_room(room, buckets, entry.single);
        for (auto cit = entry.chunks.begin(); cit != entry.chunks.end();) {
            LevelMeshChunk& m = **cit;
            auto b = buckets.find(m.cell);
            const int now = b == buckets.end() ? 0 : b->second.tris;
            bool dirty = remesh_cell_is_dirty(req, m.cell);
            if (!dirty && now != m.tris) {
                dirty = true; // drift: the boolean touched a cell the marking did not name
            }
            if (b != buckets.end()) {
                b->second.consumed = true;
            }
            if (!dirty) {
                ++cit;
                continue;
            }
            mark_changed(m);
            level_mesh_free_chunk(m);
            if (now == 0) {
                cit = entry.chunks.erase(cit);
                continue;
            }
            m.verts = std::move(b->second.verts);
            m.indices = std::move(b->second.indices);
            m.tris = now;
            if (!level_mesh_chunk_finish(m)) {
                cit = entry.chunks.erase(cit);
                continue;
            }
            mark_changed(m);
            ++cit;
        }
        // Cells that gained their first face - the crater walls the carve just cut.
        for (auto& bucket : buckets) {
            ChunkBuild& b = bucket.second;
            if (b.consumed || b.tris == 0) {
                continue;
            }
            auto m = std::make_unique<LevelMeshChunk>();
            m->cell = bucket.first;
            m->verts = std::move(b.verts);
            m->indices = std::move(b.indices);
            m->tris = b.tris;
            if (!level_mesh_chunk_finish(*m)) {
                continue;
            }
            mark_changed(*m);
            entry.chunks.push_back(std::move(m));
        }
        if (entry.chunks.empty()) {
            g_level_mesh.erase(it);
        }
    }
    g_remesh_pending.clear();
    g_remesh_pending_since_ms = 0;
    // Nothing else wakes a sleeping body whose ground was just carved out.
    if (changed) {
        wake_sleeping_bodies_in(changed_lo, changed_hi);
    }
}

// Both halves: the busy flag (0x006371EC) spans the carve, the pending list the frame before it.
bool level_mesh_geomod_settled()
{
    const rf::GeomodEvent& queue = rf::g_geomod_pending_list;
    return !rf::g_geomod_processing && queue.next == &queue;
}

// Runs strictly BEFORE the engine's geomod_per_frame (0x004b2da9), so frame-N craters are pending.
void level_mesh_poll_remesh()
{
    if (g_remesh_pending.empty()) {
        g_remesh_pending_since_ms = 0;
        return;
    }
    const int64_t now_ms = timer::get_i64(1000);
    if (g_remesh_pending_since_ms == 0) {
        g_remesh_pending_since_ms = now_ms;
    }
    if (!level_mesh_geomod_settled()) {
        // Never force while processing == 1 - that is the boolean mid-mutation; queued marks are lost.
        if (rf::g_geomod_processing || now_ms - g_remesh_pending_since_ms < vphys_remesh_max_ms) {
            return;
        }
        for (auto& pending : g_remesh_pending) {
            pending.second.all = true;
        }
    }
    level_mesh_rebuild_pending();
}

// The ONLY world-collision law a vehicle hull has.
bool level_mesh_active()
{
    return g_level_mesh_built && !(level_mesh_chunk_count() == 0 && g_mover_mesh.empty());
}

void level_mesh_debug_collect(const rf::Vector3& center, float radius,
                              std::vector<LevelMeshDebugChunk>& out)
{
    out.clear();
    const rf::Vector3 lo{center.x - radius, center.y - radius, center.z - radius};
    const rf::Vector3 hi{center.x + radius, center.y + radius, center.z + radius};
    for (const auto& kv : g_level_mesh) {
        for (const auto& m : kv.second.chunks) {
            if (!m->body || m->aabb_min.x > hi.x || m->aabb_max.x < lo.x || m->aabb_min.y > hi.y
                || m->aabb_max.y < lo.y || m->aabb_min.z > hi.z || m->aabb_max.z < lo.z) {
                continue;
            }
            out.push_back({m->aabb_min, m->aabb_max,
                           g_level_mesh_rebuild_serial != 0
                               && m->last_rebuild_serial == g_level_mesh_rebuild_serial});
        }
    }
}

float level_mesh_debug_cell_size()
{
    return vphys_mesh_cell;
}

// Through the bucketing's own NaN-safe axis derivation, so the overlay draws the real grid.
rf::Vector3 level_mesh_debug_cell_origin(const rf::Vector3& p)
{
    const LevelMeshCell c = level_mesh_cell_of(p);
    return rf::Vector3{static_cast<float>(c.x) * vphys_mesh_cell,
                       static_cast<float>(c.y) * vphys_mesh_cell,
                       static_cast<float>(c.z) * vphys_mesh_cell};
}

namespace
{
    // LAZY BUILD: a level-placed vehicle can appear on a level whose factory list is empty.
    void level_mesh_ensure_for_body()
    {
        if (!g_vphys.world) {
            return;
        }
        g_level_has_bullet_vehicles = true;
        if (!g_level_mesh_built) {
            level_mesh_build();
        }
    }

    // Before the solve: the wheel impulses of the last substep are discarded, never integrated.
    void vphys_pin_pre_tick(btDynamicsWorld*, btScalar)
    {
        for (const auto& owned : g_vphys.bodies) {
            if (owned->handbrake_pin && owned->body) {
                owned->body->setLinearVelocity(btVector3(0.0f, 0.0f, 0.0f));
                owned->body->setAngularVelocity(btVector3(0.0f, 0.0f, 0.0f));
                owned->body->clearForces();
            }
        }
    }
} // namespace

void world_destroy()
{
    level_mesh_destroy();
    obstacles_destroy_all();
    sim_bodies_free_all();
    delete g_vphys.world;
    delete g_vphys.solver;
    delete g_vphys.broadphase;
    delete g_vphys.dispatcher;
    delete g_vphys.config;
    g_vphys = VehiclePhysicsWorld{};
    vphys_prev_frame_dt_reset();
}

void world_create()
{
    vphys_prev_frame_dt_reset();
    g_vphys.config = new btDefaultCollisionConfiguration();
    g_vphys.dispatcher = new btCollisionDispatcher(g_vphys.config);
    g_vphys.broadphase = new btDbvtBroadphase();
    g_vphys.solver = new btSequentialImpulseConstraintSolver();
    g_vphys.world =
        new VphysDynamicsWorld(g_vphys.dispatcher, g_vphys.broadphase, g_vphys.solver, g_vphys.config);
    // NEUTRAL world gravity and it must stay so: setGravity/addRigidBody overwrite every body's.
    g_vphys.world->setGravity(btVector3(0.0f, 0.0f, 0.0f));
    g_vphys.world->setInternalTickCallback(vphys_pin_pre_tick, nullptr, true);
    // Active bodies only: a static or sleeping body must never be moved in place, only removed and re-added.
    g_vphys.world->setForceUpdateAllAabbs(false);
}

void sim_body_destroy(VehicleSimBody* b)
{
    if (!b) {
        return;
    }
    sim_body_free(*b);
    sim_body_erase(b);
}

// The obstacle set is the driven hull's own neighbourhood, so it goes with it.
void driven_body_destroy()
{
    obstacles_destroy_all();
    if (!g_vphys.driven) {
        return;
    }
    sim_body_destroy(g_vphys.driven);
}

namespace
{
    // One row per entity.tbl CLASS the extents were measured from, so a class with no row of its
    // own - masako_fighter, any mod hull - is measured below rather than handed another's box.
    struct VehicleHullBoxOverride
    {
        const char* entity_class;
        float half_x, half_y, half_z;
        float center_x, center_y, center_z;
    };

    constexpr VehicleHullBoxOverride vehicle_hull_box_overrides[] = {
        // class                    half x      y          z           centre x     y           z
        {"Jeep01",              1.163767f, 1.549674f, 2.244788f,  -0.004758f, 0.840514f,  0.323186f},
        {"APC",                 2.078801f, 1.538801f, 4.038801f,   0.000000f, 0.416000f,  0.000000f},
        // Driller front face 4.629 local: behind the drill mounts at 5.472 and the tips at 6.572.
        {"Driller01",           3.231721f, 1.931721f, 5.705470f,   0.000000f, 0.674000f, -1.076251f},
        {"Fighter01",           1.737500f, 1.737500f, 2.843253f,   0.029611f, -0.079548f, -0.205088f},
        {"sub",                 2.000000f, 2.000000f, 2.000000f,   0.000000f, 0.000000f,  0.000000f},
        {"Stationary Turret",   0.705964f, 0.705964f, 0.705964f,   0.000000f, -0.459000f, 0.000000f},
        {"Stationary Turret_Plain",
                                0.705964f, 0.705964f, 0.705964f,   0.000000f, -0.459000f, 0.000000f},
    };

    constexpr int vehicle_hull_box_override_count =
        static_cast<int>(std::size(vehicle_hull_box_overrides));

    // entity.tbl is parsed once per process, so what a class name resolves to never moves.
    int vehicle_hull_box_override_type(int row)
    {
        static int types[vehicle_hull_box_override_count] = {};
        static bool resolved = false;
        if (!resolved) {
            for (int i = 0; i < vehicle_hull_box_override_count; ++i) {
                types[i] = rf::entity_lookup_type(vehicle_hull_box_overrides[i].entity_class);
            }
            resolved = true;
        }
        return types[row];
    }

    const VehicleHullBoxOverride* vehicle_hull_box_override(const rf::Object* op)
    {
        if (!op || op->type != rf::OT_ENTITY) {
            return nullptr;
        }
        const int type = static_cast<const rf::Entity*>(op)->info_index;
        if (type < 0) {
            return nullptr;
        }
        for (int i = 0; i < vehicle_hull_box_override_count; ++i) {
            if (vehicle_hull_box_override_type(i) == type) {
                return &vehicle_hull_box_overrides[i];
            }
        }
        return nullptr;
    }

    // ONE derivation for every mesh-measured box. False means no mesh, or a degenerate/unloaded one.
    bool mesh_local_box(const rf::Object* op, HullBox* out)
    {
        if (!op->vmesh) {
            return false;
        }
        rf::Vector3 lo{};
        rf::Vector3 hi{};
        rf::vmesh_get_bbox(op->vmesh, &lo, &hi);
        if (!(hi.x > lo.x) || !(hi.y > lo.y) || !(hi.z > lo.z)) {
            return false;
        }
        out->half = btVector3(std::max((hi.x - lo.x) * 0.5f, 0.05f),
                              std::max((hi.y - lo.y) * 0.5f, 0.05f),
                              std::max((hi.z - lo.z) * 0.5f, 0.05f));
        out->center = btVector3((hi.x + lo.x) * 0.5f, (hi.y + lo.y) * 0.5f, (hi.z + lo.z) * 0.5f);
        return true;
    }

    // A PROP is measured from its live mesh - stock clutter collision is mesh-based (0x004991C0).
    HullBox prop_local_box(const rf::Object* op)
    {
        HullBox box;
        if (mesh_local_box(op, &box)) {
            return box;
        }
        return hull_local_box(op);
    }
} // namespace

// The hull's TRUE local box: min/max over the cspheres, NOT folded about the origin.
HullBox hull_local_box(const rf::Object* op)
{
    if (const VehicleHullBoxOverride* ov = vehicle_hull_box_override(op)) {
        HullBox box;
        box.half = btVector3(std::max(ov->half_x, 0.05f), std::max(ov->half_y, 0.05f),
                             std::max(ov->half_z, 0.05f));
        box.center = btVector3(ov->center_x, ov->center_y, ov->center_z);
        return box;
    }
    // No row of its own: the live mesh is the measurement, as a clutter prop's box already is.
    HullBox mesh_box;
    if (mesh_local_box(op, &mesh_box)) {
        return mesh_box;
    }
    rf::Vector3 lo{1e30f, 1e30f, 1e30f};
    rf::Vector3 hi{-1e30f, -1e30f, -1e30f};
    int count = 0;
    for (int i = 0; i < op->p_data.cspheres.size(); ++i) {
        const rf::PCollisionSphere& s = op->p_data.cspheres[i];
        lo.x = std::min(lo.x, s.center.x - s.radius);
        lo.y = std::min(lo.y, s.center.y - s.radius);
        lo.z = std::min(lo.z, s.center.z - s.radius);
        hi.x = std::max(hi.x, s.center.x + s.radius);
        hi.y = std::max(hi.y, s.center.y + s.radius);
        hi.z = std::max(hi.z, s.center.z + s.radius);
        ++count;
    }
    HullBox box;
    if (count == 0) {
        const float fallback = op->p_data.radius > 0.0f ? op->p_data.radius : op->radius;
        const float e = std::max(fallback * 0.25f, 0.25f);
        box.half = btVector3(e, e, e);
        box.center = btVector3(0.0f, 0.0f, 0.0f);
        return box;
    }
    box.half = btVector3(std::max((hi.x - lo.x) * 0.5f, 0.05f), std::max((hi.y - lo.y) * 0.5f, 0.05f),
                         std::max((hi.z - lo.z) * 0.5f, 0.05f));
    box.center = btVector3((hi.x + lo.x) * 0.5f, (hi.y + lo.y) * 0.5f, (hi.z + lo.z) * 0.5f);
    return box;
}

VehicleSimBody* body_create(rf::Entity* ep, int cls)
{
    if (!g_vphys.world) {
        world_create();
    }
    // Every class, not just the automobiles: the level mesh is the only world collision a hull has.
    level_mesh_ensure_for_body();
    auto owned = std::make_unique<VehicleSimBody>();
    VehicleSimBody& b = *owned;
    if (vphys_class_is_automobile(cls)) {
        car_body_create(b, ep, cls);
        return sim_body_add(std::move(owned));
    }

    const VehiclePhysicsParams& p = params_for_class(cls);
    const float radius = hull_standoff(p, ep);
    b.shape = new btSphereShape(radius);
    b.body_radius = radius;

    b.motion_state = new btDefaultMotionState();
    btRigidBody::btRigidBodyConstructionInfo ci(1.0f, b.motion_state, b.shape,
                                                btVector3(1, 1, 1));
    ci.m_restitution = p.restitution;
    ci.m_friction = std::max(p.hull_friction, 0.0f);
    b.body = new btRigidBody(ci);
    b.body->setUserPointer(&b);
    // A hovering hull would otherwise be slept by the island manager and stop answering input.
    b.body->setActivationState(DISABLE_DEACTIVATION);
    // CCD: a hull moving further than its own radius in a step is swept, not teleported.
    b.body->setCcdMotionThreshold(radius);
    b.body->setCcdSweptSphereRadius(radius * 0.5f);
    g_vphys.world->addRigidBody(b.body, vphys_group_hull, vphys_mask_no_hull);
    b.vehicle_handle = ep->handle;
    b.vehicle_class = cls;
    b.body_mass = 0.0f;
    body_apply_mass(b, p, ep);

    body_seed_from_entity(b, ep);
    return sim_body_add(std::move(owned));
}

namespace
{
    // Nearby CLUTTER only, as static boxes posed once.

    // Dead in the sense that stock collision has already stopped honouring it.
    bool obstacle_is_dead(const rf::Object* op)
    {
        // obj_flag_dead (0x0048AB40) sets OF_DELAYED_DELETE; the stock clutter walk skips OF_HIDDEN.
        if ((op->obj_flags & (rf::OF_DELAYED_DELETE | rf::OF_HIDDEN)) != 0) {
            return true;
        }
        // The CLASS life, not the instance's: every corpse and indestructible class ships $Life -1.
        const float class_life =
            op->type == rf::OT_CLUTTER && static_cast<const rf::Clutter*>(op)->info
                ? static_cast<const rf::Clutter*>(op)->info->life
                : 1.0f;
        return class_life > 0.0f && op->life <= 0.0f;
    }

    // Asked about the HULL on every path: a server's answer must not depend on who is local. Called
    // THROUGH AF's own FunHook on the stock filter deliberately.
    bool obstacle_is_pass_through(rf::Object* op, rf::Entity* self)
    {
        if (!self) {
            return false;
        }
        unsigned pair_flags = 0;
        return rf::obj_pair_should_skip(self, op, &pair_flags);
    }

    // restitution travels with the entry: the value used is the FIRST body's that wanted the object.
    struct WantedObstacle
    {
        rf::Object* op;
        int info_index;
        float restitution;
    };

    constexpr float obstacle_box_epsilon = 0.02f; // below this a rebuild is not worth the churn

    void obstacles_gather(VehicleSimBody& b, rf::Entity* self, const VehiclePhysicsParams& p,
                          std::vector<WantedObstacle>& wanted)
    {
        b.obstacle_handles.clear();
        const float self_radius = std::max({self->p_data.radius, self->radius, 1.0f});
        const float range = std::max(p.obstacle_range, 0.0f) * self_radius;
        const float restitution = std::min(p.restitution, obstacle_max_restitution);
        // vphys_max_obstacles is this HULL's budget; a shared one lets the first hull starve the rest.
        const int budget = std::min(static_cast<int>(wanted.size()) + vphys_max_obstacles,
                                    vphys_max_obstacles_world);

        const auto already_wanted = [&wanted](const rf::Object* op) {
            return std::any_of(wanted.begin(), wanted.end(),
                               [op](const WantedObstacle& w) { return w.op == op; });
        };
        // Hysteresis, not cosmetic: each flap on the boundary destroys and recreates a body and proxy.
        constexpr float obstacle_range_hysteresis = 1.25f;
        const auto in_range = [&](const rf::Object* op) {
            const bool held = g_vphys.obstacles.find(op->handle) != g_vphys.obstacles.end();
            const float r = range * (held ? obstacle_range_hysteresis : 1.0f);
            const float reach = r + std::max(op->p_data.radius, op->radius);
            return (op->pos - self->pos).len_sq() <= reach * reach;
        };

        if (range > 0.0f) {
            const float min_size = std::max(p.obstacle_min_size, 0.0f);
            // WHEELED ONLY: for a wheelless hull the reference degenerates to its own altitude.
            const bool has_wheels = rf::entity_is_automobile(self);
            const float ground_y = self->pos.y - b.wheel_reach;
            const float climb = std::max(p.step_climb_height, 0.0f);
            for (rf::Clutter& clutter : DoublyLinkedList{rf::clutter_list}) {
                if (static_cast<int>(wanted.size()) >= budget) {
                    break;
                }
                if (obstacle_is_dead(&clutter)) {
                    continue;
                }
                if (std::max(clutter.p_data.radius, clutter.radius) < min_size || !in_range(&clutter)) {
                    continue;
                }
                if (obstacle_is_pass_through(&clutter, self)) {
                    continue;
                }
                // The size floor screens on RADIUS, which says nothing about height; wheel rays are
                // level-geometry only, so a sub-tyre prop would meet the hull as a hard wall.
                if (has_wheels) {
                    const HullBox cbox = prop_local_box(&clutter);
                    const float top = clutter.pos.y + cbox.center.y() + cbox.half.y();
                    if (top < ground_y + climb) {
                        continue;
                    }
                }
                // Recorded before the dedup, so obstacle_handles keeps entries another hull claimed.
                b.obstacle_handles.push_back(clutter.handle);
                if (already_wanted(&clutter)) {
                    continue;
                }
                wanted.push_back({&clutter, clutter.info_index, restitution});
            }
        }
    }

    // What a SLEEPING body still holds, re-asked out of the registry rather than re-gathered.
    void obstacles_carry(const VehicleSimBody& b, rf::Entity* self, const VehiclePhysicsParams& p,
                         std::vector<WantedObstacle>& wanted)
    {
        const float restitution = std::min(p.restitution, obstacle_max_restitution);
        for (int handle : b.obstacle_handles) {
            auto it = g_vphys.obstacles.find(handle);
            if (it == g_vphys.obstacles.end()) {
                continue;
            }
            rf::Object* op = rf::obj_from_handle(handle);
            if (!op) {
                continue;
            }
            // Held is not the same as still valid while the body holding it sleeps.
            if (obstacle_is_dead(op) || obstacle_is_pass_through(op, self)) {
                continue;
            }
            const bool have = std::any_of(wanted.begin(), wanted.end(),
                                          [op](const WantedObstacle& w) { return w.op == op; });
            if (!have) {
                wanted.push_back({op, it->second.info_index, restitution});
            }
        }
    }

    void obstacles_sync(const std::vector<WantedObstacle>& wanted)
    {
        const auto is_wanted = [&wanted](int handle) {
            return std::any_of(wanted.begin(), wanted.end(),
                               [handle](const WantedObstacle& w) { return w.op->handle == handle; });
        };
        for (auto it = g_vphys.obstacles.begin(); it != g_vphys.obstacles.end();) {
            if (is_wanted(it->first)) {
                ++it;
                continue;
            }
            // A hull asleep on the prop would otherwise hover where it stood.
            btVector3 lo;
            btVector3 hi;
            it->second.body->getAabb(lo, hi);
            wake_sleeping_bodies_in(lo, hi);
            g_vphys.world->removeRigidBody(it->second.body);
            delete it->second.body;
            delete it->second.motion_state;
            delete it->second.shape;
            it = g_vphys.obstacles.erase(it);
        }

        for (const WantedObstacle& w : wanted) {
            VehiclePhysicsObstacle& obstacle = g_vphys.obstacles[w.op->handle];
            const HullBox fresh = prop_local_box(w.op);
            const bool mesh_changed = obstacle.body && obstacle.vmesh != w.op->vmesh;
            const bool bounds_changed =
                obstacle.body
                && (fresh.half - obstacle.built_half).length() > obstacle_box_epsilon;
            const bool box_changed = mesh_changed || bounds_changed;
            if (obstacle.body && (obstacle.info_index != w.info_index || box_changed)) {
                btVector3 lo;
                btVector3 hi;
                obstacle.body->getAabb(lo, hi);
                wake_sleeping_bodies_in(lo, hi);
                g_vphys.world->removeRigidBody(obstacle.body);
                delete obstacle.body;
                delete obstacle.motion_state;
                delete obstacle.shape;
                obstacle = VehiclePhysicsObstacle{};
            }
            // btBoxShape is centred on the body origin, so the centre offset rides in the transform.
            const HullBox& obox = fresh;
            btTransform t;
            t.setBasis(to_bt(w.op->orient));
            t.setOrigin(to_bt(w.op->pos) + to_bt(w.op->orient) * obox.center);
            if (!obstacle.body) {
                obstacle.shape = new btBoxShape(obox.half);
                obstacle.motion_state = new btDefaultMotionState(t);
                btRigidBody::btRigidBodyConstructionInfo ci(0.0f, obstacle.motion_state,
                                                            obstacle.shape, btVector3(0, 0, 0));
                // Restitution belongs to WALLS: a class's wall bounce on a prop throws the car back.
                ci.m_restitution = w.restitution;
                obstacle.body = new btRigidBody(ci);
                obstacle.info_index = w.info_index;
                obstacle.vmesh = w.op->vmesh;
                obstacle.built_half = obox.half;
                // A prop keeps the default static group/mask addRigidBody gives it.
                g_vphys.world->addRigidBody(obstacle.body);
            }
        }
    }
} // namespace

// The union is gathered first and reconciled once: a per-body reconcile would drop the rest.
void obstacles_update_all()
{
    std::vector<WantedObstacle> wanted;
    for (const auto& owned : g_vphys.bodies) {
        rf::Entity* self = rf::entity_from_handle(owned->vehicle_handle);
        if (!self) {
            continue;
        }
        const VehiclePhysicsParams& p = params_for_class(owned->vehicle_class);
        // A parked body cannot move, so the neighbourhood it gathered last is still the one it has.
        if (owned->body && owned->body->getActivationState() == ISLAND_SLEEPING) {
            obstacles_carry(*owned, self, p, wanted);
            continue;
        }
        obstacles_gather(*owned, self, p, wanted);
    }
    obstacles_sync(wanted);
}

void vehicle_physics_notify_geomod(const rf::Vector3& pos, float radius)
{
    // The queue add only ENQUEUES the carve, so extracting now would re-read pre-carve faces.
    if (!g_level_mesh_built || !rf::level.geometry) {
        return;
    }
    const float r = std::max(radius, 0.0f) + 1.0f; // a margin, since the crater shape is scaled
    const rf::Vector3 lo{pos.x - r, pos.y - r, pos.z - r};
    const rf::Vector3 hi{pos.x + r, pos.y + r, pos.z + r};
    const LevelMeshRoomSet sky_detail = level_mesh_sky_detail_rooms(rf::level.geometry);
    for (rf::GRoom* room : rf::level.geometry->all_rooms) {
        if (level_mesh_room_is_skipped(room, sky_detail)) {
            continue; // never gets a body, so never mark one
        }
        if (hi.x < room->bbox_min.x || lo.x > room->bbox_max.x || hi.y < room->bbox_min.y
            || lo.y > room->bbox_max.y || hi.z < room->bbox_min.z || lo.z > room->bbox_max.z) {
            continue;
        }
        RemeshRequest& req = g_remesh_pending[room];
        auto it = g_level_mesh.find(room);
        if (it == g_level_mesh.end() || it->second.single) {
            req.all = true; // one chunk, or a room we hold nothing for yet
            continue;
        }
        // Marked HERE, at enqueue, while the faces are still PRE-carve: (a) every cell the crater
        // AABB covers - the span is bounded first, since `radius` reaches a client unchecked.
        const LevelMeshCell c0 = level_mesh_cell_of(lo);
        const LevelMeshCell c1 = level_mesh_cell_of(hi);
        const int64_t span = (int64_t{c1.x} - c0.x + 1) * (int64_t{c1.y} - c0.y + 1)
                           * (int64_t{c1.z} - c0.z + 1);
        if (span <= 0 || span > 4096) {
            req.all = true; // absurd radius: the whole room, cheaply bounded
            continue;
        }
        for (int x = c0.x; x <= c1.x; ++x) {
            for (int y = c0.y; y <= c1.y; ++y) {
                for (int z = c0.z; z <= c1.z; ++z) {
                    remesh_mark_cell(req, LevelMeshCell{x, y, z});
                }
            }
        }
        // (b) every face the crater touches, marked in the cell owning its CENTROID - possibly far away.
        for (rf::GFace& face : room->face_list) {
            if (!level_mesh_face_is_solid(face)) {
                continue;
            }
            rf::Vector3 centroid;
            rf::Vector3 flo;
            rf::Vector3 fhi;
            if (!level_mesh_face_bounds(face, centroid, flo, fhi)) {
                continue;
            }
            if (fhi.x < lo.x || flo.x > hi.x || fhi.y < lo.y || flo.y > hi.y || fhi.z < lo.z
                || flo.z > hi.z) {
                continue;
            }
            remesh_mark_cell(req, level_mesh_cell_of(centroid));
        }
    }
}

void vehicle_physics_notify_room_geometry_changed(rf::GRoom* room)
{
    // A detail brush is its own GRoom; process_destroy leaves it in all_rooms with an EMPTY face_list.
    if (!g_level_mesh_built || !room) {
        return;
    }
    g_remesh_pending[room].all = true;
}

void vphys_world_install_patches()
{
    params_init_defaults();
}
