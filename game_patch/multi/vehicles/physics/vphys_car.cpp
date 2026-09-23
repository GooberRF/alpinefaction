#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>
#include "vphys_internal.h"
#include "../vehicle_physics.h"
#include "../vehicle.h"
#include "../../../misc/level.h"
#include "../../../os/os.h"
#include "../../../rf/ai.h"
#include "../../../rf/clutter.h"
#include "../../../rf/collide.h"
#include "../../../rf/entity.h"
#include "../../../rf/os/timer.h"
#include "../../../rf/multi.h"
#include "../../../rf/object.h"
#include "../../../rf/physics.h"
#include "../../../rf/player/camera.h"

// Wheels are cast with RF's collide_linesegment_world (0x00498E80); null means "airborne".
// The arc's angular reach: forward for climbing, a little rearward for descending symmetry.
constexpr float wheel_arc_fwd_deg = 55.0f;
constexpr float wheel_arc_back_deg = 25.0f;

// 0x00498E80 walks the mover-brush list with no solidity test at all, so a brush the Bullet chassis
// passes through (mover_brush_is_solid, vphys_world.cpp) would still stop a probe. Re-cast from just
// past each such hit rather than discarding it: the engine returns only the nearest hit, so the real
// surface behind a see-through mover is otherwise lost.
bool vphys_collide_solid_segment(const rf::Vector3& a, const rf::Vector3& b, rf::PCollisionOut& out)
{
    constexpr int max_pass_through = 4;
    // Small enough that a solid surface hugging the back of a see-through mover face is not stepped over.
    constexpr float nudge = 0.001f;
    const rf::Vector3 seg = b - a;
    const float total = seg.len();
    if (total < 0.0001f) {
        return false;
    }
    const rf::Vector3 dir = seg * (1.0f / total);
    rf::Vector3 from = a;
    for (int i = 0; i <= max_pass_through; ++i) {
        rf::Vector3 to = b; // 0x00498E80 takes both endpoints by non-const reference
        out = rf::PCollisionOut{};
        out.obj_handle = -1;
        if (!rf::collide_linesegment_world(from, to, 0, &out)) {
            return false;
        }
        // -1 is level geometry; the only object the segment test ever reports is a mover brush.
        const rf::Object* op = out.obj_handle >= 0 ? rf::obj_from_handle(out.obj_handle) : nullptr;
        if (!op || op->type != rf::OT_MOVER_BRUSH || mover_brush_is_solid(*op)) {
            return true;
        }
        from = out.hit_point + dir * nudge;
        if ((from - a).dot_prod(dir) >= total) {
            return false;
        }
    }
    return false;
}

struct RfVehicleRaycaster : public btVehicleRaycaster
{
    // btRaycastVehicle::rayCast hands castRay no user context; this back-pointer is the only way.
    VehicleSimBody* owner = nullptr;

    explicit RfVehicleRaycaster(VehicleSimBody* owning_body) : owner(owning_body) {}

    void* castRay(const btVector3& from, const btVector3& to,
                  btVehicleRaycasterResult& result) override
    {
        rf::Vector3 a = from_bt(from);
        rf::Vector3 b = from_bt(to);
        const rf::Vector3 seg = b - a;
        const float raylen = seg.len();
        if (raylen < 0.0001f) {
            return nullptr;
        }
        const rf::Vector3 dir = seg * (1.0f / raylen); // suspension axis, ~down
        const rf::Vector3 up = dir * -1.0f;
        const float radius = std::max(raylen - std::max(owner->wheel_rest_len, 0.05f), 0.05f);
        const float rest = raylen - radius;

        rf::Vector3 contact{};
        rf::Vector3 surf_normal{0.0f, 1.0f, 0.0f};
        float best_L = 1e30f;
        float best_theta = 0.0f;
        float L_straight = 1e30f; // the L the straight-down sample implies; the seat's floor

        if (owner->wheel_cylinder) {
            rf::Vector3 fwd = owner->wheel_fwd;
            fwd -= dir * fwd.dot_prod(dir);
            fwd.normalize_safe();
            const int n = std::clamp(owner->wheel_arc_samples, 3, 12);
            constexpr float deg2rad = std::numbers::pi_v<float> / 180.0f;
            const float th_lo = -wheel_arc_back_deg * deg2rad;
            const float th_hi = wheel_arc_fwd_deg * deg2rad;
            const float probe_len = raylen + radius + 0.5f;
            // theta == 0 is sampled unconditionally (the extra i == n pass): the fall-through guarantee.
            for (int i = 0; i <= n; ++i) {
                const float theta = i == n ? 0.0f
                                           : th_lo + (th_hi - th_lo) * static_cast<float>(i)
                                                         / static_cast<float>(n - 1);
                const float st = std::sin(theta);
                const float ct = std::cos(theta);
                rf::Vector3 pa = a + fwd * (radius * st);
                rf::Vector3 pb = pa + dir * probe_len;
                rf::PCollisionOut o{};
                if (!vphys_collide_solid_segment(pa, pb, o)) {
                    continue;
                }
                const float d_hit = (o.hit_point - pa).dot_prod(dir);
                const float L = d_hit - radius * ct; // suspension length this arc point implies
                if (theta == 0.0f) {
                    L_straight = L;
                }
                if (L < best_L) {
                    best_L = L;
                    best_theta = theta;
                    contact = o.hit_point;
                    surf_normal = o.hit_normal;
                }
            }
            if (best_L > 1e29f) {
                return nullptr;
            }
            // Scale the extra lift a forward edge asks for by cos(theta); never below the straight-down floor.
            if (best_theta > 0.15f && L_straight < 1e29f && best_L < L_straight) {
                best_L = L_straight - (L_straight - best_L) * std::cos(best_theta);
            }
        }
        else {
            rf::PCollisionOut o{};
            if (!vphys_collide_solid_segment(a, b, o)) {
                return nullptr;
            }
            best_L = (o.hit_point - a).dot_prod(dir) - radius;
            contact = o.hit_point;
            surf_normal = o.hit_normal;
        }

        if (best_L > rest + 0.001f) {
            return nullptr;
        }
        const float L = std::clamp(best_L, 0.0f, rest);

        // Surface normal straight down, the radial edge->axle normal on an edge, so the wheel climbs.
        rf::Vector3 normal = surf_normal;
        const bool edge = std::fabs(best_theta) > 0.15f; // ~8.6 degrees
        if (edge && owner->wheel_cylinder) {
            rf::Vector3 fwd = owner->wheel_fwd;
            fwd -= dir * fwd.dot_prod(dir);
            fwd.normalize_safe();
            rf::Vector3 radial = up * std::cos(best_theta) - fwd * std::sin(best_theta);
            radial.normalize_safe();
            // The suspension force is applied along the normal, so cap how far back an edge one leans.
            const float min_y = std::clamp(owner->wheel_edge_min_normal_y, 0.0f, 0.99f);
            if (radial.y < min_y) {
                const float horiz = std::sqrt(std::max(1.0f - min_y * min_y, 0.0f));
                rf::Vector3 h{radial.x, 0.0f, radial.z};
                h.normalize_safe();
                radial = h * horiz + rf::Vector3{0.0f, min_y, 0.0f};
                radial.normalize_safe();
            }
            if (radial.y > 0.1f) {
                normal = radial;
            }
        }

        result.m_hitPointInWorld = to_bt(contact);
        result.m_hitNormalInWorld = to_bt(normal);
        result.m_distFraction = std::clamp((L + radius) / raylen, 0.0f, 1.0f);
        // Any non-null pointer means "hit"; btRaycastVehicle never dereferences what we return.
        return this;
    }
};

void car_teardown(VehicleSimBody& b)
{
    if (b.raycast_vehicle) {
        if (g_vphys.world && b.car_action_in_world) {
            g_vphys.world->removeVehicle(b.raycast_vehicle);
        }
        delete b.raycast_vehicle;
        b.raycast_vehicle = nullptr;
    }
    b.car_action_in_world = false;
    delete b.raycaster;
    b.raycaster = nullptr;
    // The compound owns no child memory; the order only keeps the body from seeing a freed shape.
    delete b.car_compound;
    b.car_compound = nullptr;
    delete b.car_shape;
    b.car_shape = nullptr;
    b.car_box = HullBox{};
    b.car_box_base = HullBox{};
    b.car_shape_box = HullBox{};
    b.wheel_center_y.clear();
    b.num_wheels = 0;
    b.wheel_ref_count = 0;
    b.wheel_reach = 0.0f;
    b.auto_bottom_raise = 0.0f;
    b.steer_current = 0.0f;
    b.steer_input = 0.0f;
    b.engine_accel = 0.0f;
    b.brake_accel = 0.0f;
    b.engine_cmd = 0.0f;
    b.upright_recover_timer = 0.0f;
    b.upright_prop_timer = 0.0f;
    b.upright_recovering = false;
    b.chassis_ground_contact = false;
}

// The csphere box with the class's per-face trims taken off; no trim can invert a face.
HullBox hull_contact_box(const HullBox& base, const VehiclePhysicsParams& p, float bottom_raise)
{
    float lo_x = base.center.x() - base.half.x();
    float hi_x = base.center.x() + base.half.x();
    float lo_y = base.center.y() - base.half.y();
    float hi_y = base.center.y() + base.half.y();
    float lo_z = base.center.z() - base.half.z();
    float hi_z = base.center.z() + base.half.z();

    // A NEGATIVE trim is an expansion; the clamps only stop a face crossing the midline.
    constexpr float min_half = 0.05f;
    const auto shrink = [](float& lo, float& hi, float from_lo, float from_hi) {
        const float mid = (lo + hi) * 0.5f;
        lo = std::min(lo + from_lo, mid - min_half);
        hi = std::max(hi - from_hi, mid + min_half);
    };
    shrink(lo_x, hi_x, p.chassis_trim_side, p.chassis_trim_side);
    // The bottom face is the box/wheel jurisdiction line; the contact line itself is wheel_reach.
    shrink(lo_y, hi_y, std::max(bottom_raise, 0.0f), p.chassis_trim_top);
    shrink(lo_z, hi_z, p.chassis_trim_rear, p.chassis_trim_front);

    HullBox box;
    box.half = btVector3((hi_x - lo_x) * 0.5f, (hi_y - lo_y) * 0.5f, (hi_z - lo_z) * 0.5f);
    box.center = btVector3((hi_x + lo_x) * 0.5f, (hi_y + lo_y) * 0.5f, (hi_z + lo_z) * 0.5f);
    return box;
}

// The one statement of the rule; `b` hands back the radius measured when the body was built.
float hull_bottom_raise(const VehiclePhysicsParams& p, const rf::Object* op, const VehicleSimBody* b)
{
    if (p.chassis_bottom_raise > 0.0f) {
        return p.chassis_bottom_raise;
    }
    return b ? b->auto_bottom_raise : hull_wheel_radius(op, p);
}

namespace
{
    float hull_auto_radius(const rf::Object* op)
    {
        float radius = 0.0f;
        for (int i = 0; i < op->p_data.cspheres.size(); ++i) {
            const rf::PCollisionSphere& s = op->p_data.cspheres[i];
            radius = std::max(radius, s.center.len() + s.radius);
        }
        // FALLBACK only, never a floor under the sphere set: on a long hull it is the nose length.
        if (radius <= 0.0f) {
            radius = op->p_data.radius > 0.0f ? op->p_data.radius : op->radius;
        }
        return std::max(radius, 0.25f);
    }
} // namespace

float hull_radius_for(const VehiclePhysicsParams& p, const rf::Object* op)
{
    return p.hull_radius > 0.0f ? p.hull_radius : hull_auto_radius(op);
}

// The sphere a flyer's/sub's bt body wears: how far the hull origin stays from any surface.
float hull_standoff(const VehiclePhysicsParams& p, const rf::Object* op)
{
    return hull_radius_for(p, op) + std::max(p.ground_clearance, 0.0f);
}

void body_seed_from_entity(VehicleSimBody& b, rf::Entity* ep)
{
    btTransform t;
    t.setBasis(to_bt(ep->orient));
    t.setOrigin(to_bt(ep->pos));
    b.body->setWorldTransform(t);
    // The interpolation anchors too, or the first frame after a seed lerps out of the old pose.
    b.body->setInterpolationWorldTransform(t);
    b.motion_state->setWorldTransform(t);
    b.body->setLinearVelocity(to_bt(ep->p_data.vel));
    b.body->setInterpolationLinearVelocity(to_bt(ep->p_data.vel));
    b.body->setAngularVelocity(btVector3(0, 0, 0));
    b.body->setInterpolationAngularVelocity(btVector3(0, 0, 0));
    b.body->clearForces();
    b.written_pos = ep->pos;
}

void body_apply_mass(VehicleSimBody& b, const VehiclePhysicsParams& p, const rf::Entity* ep)
{
    const float mass = std::max(ep->p_data.mass, 1.0f) * std::max(p.mass_scale, 0.01f);
    if (std::fabs(mass - b.body_mass) < 0.01f) {
        return;
    }
    btVector3 inertia(0, 0, 0);
    b.body->getCollisionShape()->calculateLocalInertia(mass, inertia);
    b.body->setMassProps(mass, inertia);
    b.body->updateInertiaTensor();
    b.body_mass = mass;
}

// The body leaves the world for the swap: the broadphase caches the proxy AABB from the old shape.
void body_apply_shape(VehicleSimBody& b, const VehiclePhysicsParams& p, rf::Entity* ep)
{
    const float radius = hull_standoff(p, ep);
    if (std::fabs(radius - b.body_radius) < 0.001f) {
        return;
    }
    g_vphys.world->removeRigidBody(b.body);
    btSphereShape* shape = new btSphereShape(radius);
    b.body->setCollisionShape(shape);
    delete b.shape;
    b.shape = shape;
    b.body_radius = radius;
    b.body->setCcdMotionThreshold(radius);
    b.body->setCcdSweptSphereRadius(radius * 0.5f);
    b.body_mass = 0.0f; // force the inertia tensor to be rebuilt against the new shape
    body_apply_mass(b, p, ep);
    g_vphys.world->addRigidBody(b.body, vphys_group_hull, vphys_mask_no_hull);
}

namespace
{
    btRaycastVehicle::btVehicleTuning car_tuning(const VehiclePhysicsParams& p)
    {
        btRaycastVehicle::btVehicleTuning t;
        t.m_suspensionStiffness = p.suspension_stiffness;
        t.m_suspensionCompression = p.suspension_compression;
        t.m_suspensionDamping = p.suspension_damping;
        t.m_maxSuspensionTravelCm = p.suspension_max_travel_cm;
        t.m_frictionSlip = p.wheel_friction;
        t.m_maxSuspensionForce = p.suspension_max_force;
        return t;
    }

    // Every per-wheel quantity Bullet SUMS is divided by this, so a tread row changes nothing else.
    float wheel_rate_scale(const VehicleSimBody& b)
    {
        const int n = b.num_wheels;
        if (b.wheel_ref_count <= 0 || n <= 0) {
            return 1.0f;
        }
        return static_cast<float>(b.wheel_ref_count) / static_cast<float>(n);
    }

    // The count the suspension behaves as if it had, i.e. the one the sag is set by.
    int wheel_effective_count(const VehicleSimBody& b, int num_wheels)
    {
        return b.wheel_ref_count > 0 ? b.wheel_ref_count : std::max(num_wheels, 1);
    }
} // namespace

// The hull's tyre radius: the mean of its spring cspheres.
float hull_wheel_radius(const rf::Object* op, const VehiclePhysicsParams& p)
{
    if (p.wheel_radius > 0.0f) {
        return p.wheel_radius;
    }
    float sum = 0.0f;
    int n = 0;
    for (int i = 0; i < op->p_data.cspheres.size(); ++i) {
        if (op->p_data.cspheres[i].spring_const > 0.0f) {
            sum += op->p_data.cspheres[i].radius;
            ++n;
        }
    }
    return n > 0 ? sum / static_cast<float>(n) : 0.0f;
}

float wheel_sag(const VehicleSimBody& b, const VehiclePhysicsParams& p, int num_wheels)
{
    const float k = std::max(p.suspension_stiffness, 0.01f);
    const float n = static_cast<float>(wheel_effective_count(b, num_wheels));
    // air_gravity is deliberately not here: it only acts where there is no spring load.
    const float g = rf::gravity * std::max(p.gravity_scale, 0.0f);
    return std::min(g / (n * k), std::max(p.suspension_rest_length, 0.05f) * 0.7f);
}

namespace
{
    // The SPRING cspheres ARE the wheels in RF's own suspension, so centre and radius are verbatim.
    void car_add_wheels(VehicleSimBody& b, rf::Entity* ep, const VehiclePhysicsParams& p,
                        const HullBox& box)
    {
        const btVector3 wheel_dir(0.0f, -1.0f, 0.0f); // straight down, RF/Bullet identity basis
        const btVector3 wheel_axle(-1.0f, 0.0f, 0.0f); // +/-X, the standard Bullet steering axle
        const btRaycastVehicle::btVehicleTuning tuning = car_tuning(p);
        const float rest = std::max(p.suspension_rest_length, 0.05f);

        struct WheelPos
        {
            float x, y, z, radius;
            bool is_front;
        };
        std::vector<WheelPos> wheels;

        float min_z = 1e30f;
        float max_z = -1e30f;
        int spring_count = 0;
        for (int i = 0; i < ep->p_data.cspheres.size(); ++i) {
            if (ep->p_data.cspheres[i].spring_const > 0.0f) {
                const float z = ep->p_data.cspheres[i].center.z;
                min_z = std::min(min_z, z);
                max_z = std::max(max_z, z);
                ++spring_count;
            }
        }
        // Three spring spheres AND a real front/rear spread, or the steered/driven split is meaningless.
        const int tread_per_side = static_cast<int>(std::lround(std::max(p.tread_wheels_per_side, 0.0f)));
        if (spring_count >= 3 && (max_z - min_z) > 0.2f) {
            const float front_line = max_z - (max_z - min_z) * 0.25f;
            if (tread_per_side >= 2) {
                // A TREAD ROW per side: sides split on the sign of x, laid evenly between their ends.
                for (int sign = -1; sign <= 1; sign += 2) {
                    float sx = 0.0f;
                    float sy = 0.0f;
                    float sr = 0.0f;
                    float lo = 1e30f;
                    float hi = -1e30f;
                    int count = 0;
                    for (int i = 0; i < ep->p_data.cspheres.size(); ++i) {
                        const rf::PCollisionSphere& s = ep->p_data.cspheres[i];
                        if (s.spring_const <= 0.0f || (s.center.x < 0.0f ? -1 : 1) != sign) {
                            continue;
                        }
                        sx += s.center.x;
                        sy += s.center.y;
                        sr += s.radius;
                        lo = std::min(lo, s.center.z);
                        hi = std::max(hi, s.center.z);
                        ++count;
                    }
                    if (count == 0) {
                        continue;
                    }
                    sx /= count;
                    sy /= count;
                    sr /= count;
                    const float r = p.wheel_radius > 0.0f ? p.wheel_radius : std::max(sr, 0.1f);
                    for (int i = 0; i < tread_per_side; ++i) {
                        const float t = static_cast<float>(i) / static_cast<float>(tread_per_side - 1);
                        const float z = lo + (hi - lo) * t;
                        wheels.push_back({sx, sy, z, r, z >= front_line});
                    }
                }
            }
            if (wheels.empty()) {
                for (int i = 0; i < ep->p_data.cspheres.size(); ++i) {
                    const rf::PCollisionSphere& s = ep->p_data.cspheres[i];
                    if (s.spring_const <= 0.0f) {
                        continue;
                    }
                    const float r = p.wheel_radius > 0.0f ? p.wheel_radius : std::max(s.radius, 0.1f);
                    wheels.push_back({s.center.x, s.center.y, s.center.z, r, s.center.z >= front_line});
                }
            }
        }
        else {
            const float ix = box.half.x() * std::clamp(p.wheel_inset, 0.1f, 1.0f);
            const float iz = box.half.z() * std::clamp(p.wheel_inset, 0.1f, 1.0f);
            const float r = p.wheel_radius > 0.0f ? p.wheel_radius : std::max(box.half.y() * 0.5f, 0.2f);
            // Corner fallback: the wheel CENTRE goes one radius above the box bottom, so contact = bottom.
            const float cy = box.center.y() - box.half.y() + r;
            wheels = {{ix, cy, iz, r, true},
                      {-ix, cy, iz, r, true},
                      {ix, cy, -iz, r, false},
                      {-ix, cy, -iz, r, false}};
        }

        // Connection at (sphere y + rest - sag): a loaded wheel rests one sag short of full extension.
        b.wheel_ref_count = spring_count >= 3 ? spring_count : 0;
        b.num_wheels = static_cast<int>(wheels.size());
        const float sag = wheel_sag(b, p, static_cast<int>(wheels.size()));
        b.wheel_center_y.clear();
        for (const WheelPos& w : wheels) {
            const btVector3 conn(w.x, w.y + rest - sag, w.z);
            btWheelInfo& info =
                b.raycast_vehicle->addWheel(conn, wheel_dir, wheel_axle, rest, w.radius, tuning,
                                            w.is_front);
            info.m_rollInfluence = p.roll_influence;
            b.wheel_center_y.push_back(w.y);
        }
        b.num_wheels = b.raycast_vehicle->getNumWheels();
    }
} // namespace

void car_body_create(VehicleSimBody& b, rf::Entity* ep, int cls)
{
    const VehiclePhysicsParams& p = params_for_class(cls);
    const HullBox base = hull_local_box(ep);
    // wheel_reach is the BASE box's bottom and stays the ground reference for every height test.
    b.wheel_reach = std::max(-(base.center.y() - base.half.y()), 0.0f);
    b.auto_bottom_raise = hull_wheel_radius(ep, p);
    const HullBox box = hull_contact_box(base, p, hull_bottom_raise(p, ep, &b));
    b.car_box_base = base;
    b.car_box = box;
    b.car_shape_box = box;
    // btBoxShape centres on the body origin, so the compound carries the offset (body origin = pos).
    b.car_shape = new btBoxShape(box.half);
    b.car_compound = new btCompoundShape();
    btTransform child;
    child.setIdentity();
    child.setOrigin(box.center);
    b.car_compound->addChildShape(child, b.car_shape);

    b.motion_state = new btDefaultMotionState();
    btRigidBody::btRigidBodyConstructionInfo ci(1.0f, b.motion_state, b.car_compound,
                                                btVector3(1, 1, 1));
    ci.m_restitution = p.restitution;
    // Traction is btRaycastVehicle's; Bullet's default 0.5 let a landing's normal impulse eat speed.
    ci.m_friction = 0.2f;
    b.body = new btRigidBody(ci);
    b.body->setUserPointer(&b);
    // A car at rest would otherwise be slept by the island manager and stop answering input.
    b.body->setActivationState(DISABLE_DEACTIVATION);
    // CCD: threshold is half the box's smallest dimension, swept proxy half of that (Bullet's guidance).
    {
        const float min_half =
            std::min({box.half.x(), box.half.y(), box.half.z()});
        b.body->setCcdMotionThreshold(min_half);
        b.body->setCcdSweptSphereRadius(min_half * 0.5f);
    }
    g_vphys.world->addRigidBody(b.body, vphys_group_hull, vphys_mask_no_hull);
    b.vehicle_handle = ep->handle;
    b.vehicle_class = cls;
    b.body_mass = 0.0f;
    body_apply_mass(b, p, ep);

    b.raycaster = new RfVehicleRaycaster(&b);
    b.raycast_vehicle = new btRaycastVehicle(car_tuning(p), b.body, b.raycaster);
    // The btRaycastVehicle ctor defaults to up=Z/forward=Y; without this the wheel rays fire sideways.
    b.raycast_vehicle->setCoordinateSystem(0, 1, 2);
    g_vphys.world->addVehicle(b.raycast_vehicle);
    b.car_action_in_world = true;
    // The corner-wheel fallback gets the UNTRIMMED box: trims describe the shell, not the wheels.
    car_add_wheels(b, ep, p, base);

    body_seed_from_entity(b, ep);
    b.raycast_vehicle->resetSuspension();
}

void car_apply_box_shape(VehicleSimBody& b, const VehiclePhysicsParams& p, rf::Entity* ep,
                         const HullBox& box)
{
    const HullBox& cur = b.car_shape_box;
    if ((box.half - cur.half).length() < 0.005f && (box.center - cur.center).length() < 0.005f) {
        return;
    }
    g_vphys.world->removeRigidBody(b.body);
    btBoxShape* shape = new btBoxShape(box.half);
    btCompoundShape* compound = new btCompoundShape();
    btTransform child;
    child.setIdentity();
    child.setOrigin(box.center);
    compound->addChildShape(child, shape);
    b.body->setCollisionShape(compound);
    delete b.car_compound;
    delete b.car_shape;
    b.car_compound = compound;
    b.car_shape = shape;
    b.car_shape_box = box;
    b.body_mass = 0.0f; // force the inertia tensor to be rebuilt against the new box
    body_apply_mass(b, p, ep);
    g_vphys.world->addRigidBody(b.body, vphys_group_hull, vphys_mask_no_hull);
}

bool apply_car_controls(VehicleSimBody& b, rf::Entity* ep, const VehiclePhysicsParams& p, float dt)
{
    btRaycastVehicle* veh = b.raycast_vehicle;
    if (!veh || veh->getNumWheels() == 0) {
        return false;
    }
    btRigidBody* body = b.body;

    // ep->ai.ci still holds the departed driver's input; blind a COPY - the camera/aim path reads it.
    rf::ControlInfo ci = ep->ai.ci;
    if (b.server_owned || vphys_hull_submerged(ep)) {
        ci.rot = rf::Vector3{};
        ci.move = rf::Vector3{};
    }
    const btVector3 forward = body->getWorldTransform().getBasis().getColumn(2);
    const btVector3 side = body->getWorldTransform().getBasis().getColumn(0);
    const float fwd_speed = body->getLinearVelocity().dot(forward);
    const float lateral_speed = body->getLinearVelocity().dot(side);

    const float steer_raw = p.steer_from_mouse != 0.0f ? ci.rot.y : ci.move.x;
    const float steer_in = std::clamp(steer_raw, -1.0f, 1.0f) * p.yaw_sign;
    if (std::fabs(steer_in) > 0.001f) {
        b.steer_input += steer_in * std::max(p.steer_input_gain, 0.0f) * dt;
    }
    else {
        const float unwind = std::max(p.steer_return_rate, 0.0f) * dt;
        b.steer_input -= std::clamp(b.steer_input, -unwind, unwind);
    }
    b.steer_input = std::clamp(b.steer_input, -1.0f, 1.0f);

    // Off ground_speed, not fwd_speed: keyed off the nose component a slide feeds itself full lock.
    const float ground_speed = std::sqrt(fwd_speed * fwd_speed + lateral_speed * lateral_speed);
    const float falloff = 1.0f / (1.0f + std::max(p.steer_speed_falloff, 0.0f) * ground_speed);
    const float steer_target = b.steer_input * p.steer_angle_max * falloff;
    const float max_step = std::max(p.steer_rate, 0.0f) * dt;
    b.steer_current += std::clamp(steer_target - b.steer_current, -max_step, max_step);

    const bool drills_on = p.drill_speed_scale < 1.0f && rf::entity_is_driller(ep)
                        && ep->ai.current_primary_weapon >= 0
                        && rf::entity_weapon_is_on(ep->handle, ep->ai.current_primary_weapon);
    const float drill_scale = drills_on ? std::clamp(p.drill_speed_scale, 0.1f, 1.0f) : 1.0f;
    const float max_speed_now = p.car_max_speed * drill_scale;
    const float engine_force_now = std::max(p.engine_force, 0.0f) * drill_scale;

    const float throttle = std::clamp(ci.move.z, -1.0f, 1.0f) * p.thrust_sign;
    // ACCELERATIONS, before the mass/wheel split, so a class's wind-up is wheel-count independent.
    float engine_cmd = 0.0f;
    float brake_cmd = 0.0f;
    if (std::fabs(throttle) > 0.05f) {
        const bool braking = throttle * fwd_speed < -0.5f && std::fabs(fwd_speed) > 1.0f;
        if (braking) {
            brake_cmd = std::max(p.brake_force, 0.0f);
        }
        else {
            const bool over = (throttle > 0.0f && fwd_speed > max_speed_now)
                           || (throttle < 0.0f && fwd_speed < -max_speed_now * 0.5f);
            if (!over) {
                engine_cmd = throttle * engine_force_now;
            }
        }
    }
    else {
        // The idle brake would eat the whole capped shove inside a second, so a grace suppresses it.
        const bool shove_grace =
            b.shove_brake_until_ms != 0 && timer::get_i64(1000) < b.shove_brake_until_ms;
        brake_cmd = shove_grace ? 0.0f : std::max(p.idle_brake_force, 0.0f);
    }
    b.engine_cmd = engine_cmd;

    // A REVERSAL counts as a release: the drivetrain must unwind before it can wind the other way.
    const bool reversing = engine_cmd * b.engine_accel < 0.0f;
    const bool releasing = reversing || std::fabs(engine_cmd) < std::fabs(b.engine_accel);
    const bool stalled = std::fabs(fwd_speed) < std::max(p.throttle_stall_speed, 0.0f);
    const float build_rate = stalled ? std::max(p.throttle_stall_ramp, p.throttle_ramp) : p.throttle_ramp;
    const float throttle_rate = releasing ? p.throttle_release_ramp : build_rate;
    // Exponential, not clamp(rate*dt): the clamp form's time constant moves with fps.
    const float throttle_step = 1.0f - std::exp(-std::max(throttle_rate, 0.0f) * dt);
    const float brake_step = 1.0f - std::exp(-std::max(p.brake_ramp, 0.0f) * dt);
    b.engine_accel += (engine_cmd - b.engine_accel) * throttle_step;
    b.brake_accel += (brake_cmd - b.brake_accel) * brake_step;
    return true;
}

// engine_force/brake_force are ACCELERATIONS, multiplied by mass and split across the wheels.
void apply_car_model(VehicleSimBody& b, const VehiclePhysicsParams& p, float dt, float step_time)
{
    btRaycastVehicle* veh = b.raycast_vehicle;
    btRigidBody* body = b.body;
    const int nwheels = veh->getNumWheels();
    if (nwheels == 0) {
        return;
    }

    const float mass = 1.0f / std::max(body->getInvMass(), 0.0001f);
    const btVector3 forward = body->getWorldTransform().getBasis().getColumn(2); // RF forward = +Z
    const btVector3 side = body->getWorldTransform().getBasis().getColumn(0);    // RF right = +X
    const float fwd_speed = body->getLinearVelocity().dot(forward); // signed world u/s
    const float lateral_speed = body->getLinearVelocity().dot(side); // signed, across the nose

    float rear_grip = p.wheel_friction_rear;
    if (p.wheel_friction_rear > 0.0f && p.wheel_friction_rear_high > 0.0f && p.car_max_speed > 0.0f) {
        const float t = std::clamp(std::fabs(fwd_speed) / p.car_max_speed, 0.0f, 1.0f);
        rear_grip = p.wheel_friction_rear + (p.wheel_friction_rear_high - p.wheel_friction_rear) * t;
    }

    const float sag = wheel_sag(b, p, nwheels);
    const float rest_len = std::max(p.suspension_rest_length, 0.05f);
    const float rate = wheel_rate_scale(b);
    b.wheel_fwd = from_bt(forward);                 // arc plane forward axis for castRay
    b.wheel_cylinder = p.wheel_cylinder_cast != 0.0f;
    b.wheel_arc_samples = static_cast<int>(std::lround(p.wheel_arc_samples));
    b.wheel_rest_len = std::max(p.suspension_rest_length, 0.05f);
    b.wheel_edge_min_normal_y = p.wheel_edge_min_normal_y;
    // Pushed in each frame; the connection point comes too - its sag is a function of live stiffness.
    for (int i = 0; i < nwheels; ++i) {
        btWheelInfo& w = veh->getWheelInfo(i);
        if (i < static_cast<int>(b.wheel_center_y.size())) {
            w.m_suspensionRestLength1 = rest_len;
            w.m_chassisConnectionPointCS.setY(b.wheel_center_y[i] + rest_len - sag);
        }
        w.m_suspensionStiffness = p.suspension_stiffness * rate;
        w.m_wheelsDampingRelaxation = p.suspension_damping * rate;
        w.m_wheelsDampingCompression = p.suspension_compression * rate;
        // NOT scaled: the cap is per wheel's own load share, which already sums to the hull's weight.
        w.m_frictionSlip = (!w.m_bIsFrontWheel && p.wheel_friction_rear > 0.0f)
                               ? rear_grip
                               : p.wheel_friction;
        w.m_rollInfluence = p.roll_influence;
        w.m_maxSuspensionForce = p.suspension_max_force * rate;
        w.m_maxSuspensionTravelCm = p.suspension_max_travel_cm;
    }

    const float ground_speed = std::sqrt(fwd_speed * fwd_speed + lateral_speed * lateral_speed);

    const int dw = static_cast<int>(std::lround(std::clamp(p.drive_wheels, 0.0f, 2.0f)));
    const auto is_driven = [dw](bool front) {
        return dw == 2 || (dw == 1 && front) || (dw == 0 && !front);
    };
    int num_driven = 0;
    for (int i = 0; i < nwheels; ++i) {
        if (is_driven(veh->getWheelInfo(i).m_bIsFrontWheel)) {
            ++num_driven;
        }
    }
    num_driven = std::max(num_driven, 1);

    const float engine_each = b.engine_accel * mass / num_driven;
    // btRaycastVehicle's two drivetrain inputs are NOT in the same units: updateFriction scales the
    // engine force by the timestep but hands m_brake to calcRollingFriction as an UNSCALED impulse.
    const float brake_each = b.brake_accel * mass / nwheels * vphys_fixed_timestep;

    for (int i = 0; i < nwheels; ++i) {
        btWheelInfo& w = veh->getWheelInfo(i);
        veh->applyEngineForce(is_driven(w.m_bIsFrontWheel) ? engine_each : 0.0f, i);
        veh->setBrake(brake_each, i);
        if (w.m_bIsFrontWheel) {
            veh->setSteeringValue(b.steer_current, i);
        }
    }

    // Bullet's wheel brake delivers a fraction of what is asked, so apply the deceleration here.
    int grounded_here = 0;
    float deepest_compress = 0.0f;
    for (int i = 0; i < nwheels; ++i) {
        const btWheelInfo& w = veh->getWheelInfo(i);
        if (!w.m_raycastInfo.m_isInContact) {
            continue;
        }
        ++grounded_here;
        deepest_compress = std::max(deepest_compress,
                                    1.0f - w.m_raycastInfo.m_suspensionLength
                                               / std::max(w.getSuspensionRestLength(), 0.001f));
    }

    float brake_applied_accel = 0.0f;
    if (grounded_here > 0 && b.brake_accel > 0.0f && std::fabs(fwd_speed) > 0.05f
        && p.brake_chassis_scale > 0.0f) {
        const float grounded_frac =
            static_cast<float>(grounded_here) / static_cast<float>(std::max(nwheels, 1));
        brake_applied_accel = b.brake_accel * std::clamp(p.brake_chassis_scale, 0.0f, 2.0f)
                            * grounded_frac;
        // Bullet clears forces only AFTER its substep loop, so this one acts over step_time, not dt.
        brake_applied_accel =
            std::min(brake_applied_accel, std::fabs(fwd_speed) / std::max(step_time, 0.0001f));
        body->applyCentralForce(forward * (-(fwd_speed > 0.0f ? 1.0f : -1.0f) * brake_applied_accel * mass));
    }

    // Two thirds of the wheels, not all: one wheel over a rim is the normal shape of this.
    const bool high_centred = grounded_here * 3 >= nwheels * 2 && nwheels > 0 && deepest_compress > 0.85f
                           && std::fabs(b.engine_cmd) > 1.0f && std::fabs(fwd_speed) < 0.75f;
    if (high_centred && p.high_centre_assist > 0.0f) {
        const float dir_sign = b.engine_cmd > 0.0f ? 1.0f : -1.0f;
        body->applyCentralForce(forward * (dir_sign * p.high_centre_assist * mass));
    }

    // Nothing in btRaycastVehicle opposes roll or pitch, so an impact's angular velocity persists.
    int grounded_wheels = 0;
    btVector3 normal_sum(0.0f, 0.0f, 0.0f);
    for (int i = 0; i < nwheels; ++i) {
        const btWheelInfo& w = veh->getWheelInfo(i);
        if (w.m_raycastInfo.m_isInContact) {
            ++grounded_wheels;
            normal_sum += w.m_raycastInfo.m_contactNormalWS;
        }
    }
    const btMatrix3x3& basis = body->getWorldTransform().getBasis();
    const btVector3 right = basis.getColumn(0);
    const btVector3 up = basis.getColumn(1);
    const btVector3 world_up(0.0f, 1.0f, 0.0f);

    // The averaged in-contact wheel normals ARE the surface, so a ramp asks for no correction.
    btVector3 ref_up = world_up;
    float agree = 0.0f;
    // Three contacts describe a plane; with one wheel down `agree` is trivially 1.0 and meaningless.
    const int ref_min_wheels = std::min(3, std::max(nwheels, 1));
    if (grounded_wheels >= ref_min_wheels) {
        agree = normal_sum.length() / static_cast<float>(grounded_wheels);
        if (agree >= std::clamp(p.upright_normal_agree, 0.0f, 1.0f)) {
            const btVector3 ground_up = normal_sum.normalized();
            const float blend = std::clamp(p.upright_ground_blend, 0.0f, 1.0f);
            btVector3 mixed = world_up * (1.0f - blend) + ground_up * blend;
            if (mixed.length2() > 0.0001f) {
                ref_up = mixed.normalized();
            }
        }
    }
    // Cap the reference's own lean, or a hull against a near-vertical face adopts that face as up.
    const float ref_max = std::clamp(p.upright_ref_max_deg, 0.0f, 89.0f) * std::numbers::pi_v<float> / 180.0f;
    const float ref_cos = std::clamp(ref_up.dot(world_up), -1.0f, 1.0f);
    if (std::acos(ref_cos) > ref_max) {
        btVector3 perp = ref_up - world_up * ref_cos;
        if (perp.length2() > 0.0001f) {
            ref_up = world_up * std::cos(ref_max) + perp.normalized() * std::sin(ref_max);
        }
        else {
            ref_up = world_up;
        }
    }
    b.upright_ref = from_bt(ref_up); // for the overlay, later in the same frame

    const btVector3 tilt_axis = up.cross(ref_up);
    const float sin_tilt = tilt_axis.length();
    const float tilt = std::atan2(sin_tilt, up.dot(ref_up));
    btVector3 rate_target(0.0f, 0.0f, 0.0f);
    if (sin_tilt > 0.0001f) {
        rate_target = tilt_axis * (tilt * std::max(p.upright_gain, 0.0f) / sin_tilt);
    }
    else if (up.dot(ref_up) < 0.0f) {
        // Exactly inverted: the cross names no axis, so roll about forward - the shorter way round.
        rate_target = forward * (tilt * std::max(p.upright_gain, 0.0f));
    }
    const btVector3 omega = body->getAngularVelocity();

    const float body_speed = body->getLinearVelocity().length();
    // "At rest, off its wheels" - the condition that is wrong at ANY attitude.
    const bool at_rest_airborne = grounded_wheels == 0
                               && body_speed < std::max(p.upright_recover_speed, 0.0f)
                               && omega.length() < std::max(p.upright_recover_omega, 0.0f);
    const bool stranded_now = at_rest_airborne && up.y() < p.upright_recover_up_max;
    if (!at_rest_airborne) {
        b.upright_recover_timer = 0.0f;
        b.upright_prop_timer = 0.0f;
    }
    else {
        b.upright_prop_timer += dt;
        if (stranded_now) {
            b.upright_recover_timer += dt;
        }
        else {
            b.upright_recover_timer = 0.0f;
        }
    }
    const bool recovering =
        grounded_wheels == 0
        && (b.upright_recover_timer >= std::max(p.upright_recover_delay, 0.0f)
            || b.upright_prop_timer >= std::max(p.upright_recover_any_delay, 0.0f));
    b.upright_recovering = recovering;

    // PARTIAL contact is not grounded: full authority there holds a pitched hull up as it lands.
    const bool partial_contact = grounded_wheels > 0 && grounded_wheels < ref_min_wheels;
    const float grounded_authority =
        partial_contact ? std::clamp(p.upright_partial_scale, 0.0f, 1.0f) : 1.0f;
    const float authority = grounded_wheels > 0
                                ? grounded_authority
                                : (recovering ? std::clamp(p.upright_air_scale, 0.0f, 1.0f) : 0.0f);
    const float damp = grounded_wheels > 0 ? std::max(p.tilt_damp, 0.0f) * grounded_authority : 0.0f;
    const float servo = std::max(p.upright_servo, 0.0f);
    const auto leg = [&](const btVector3& axis) {
        const float w = omega.dot(axis);
        return (rate_target.dot(axis) - w) * servo - w * damp;
    };
    btVector3 alpha = (right * leg(right) + forward * leg(forward)) * authority;

    // `slip` is 0 down the nose and 1 across it, so a clean turn is never resisted.
    float slip = 0.0f;
    if (p.yaw_damp > 0.0f && ground_speed > 1.0f) {
        slip = std::clamp(std::fabs(lateral_speed) / ground_speed, 0.0f, 1.0f);
        alpha -= up * (omega.dot(up) * p.yaw_damp * slip);
    }

    // Pure damping, no reference direction: the hull stops turning without being turned.
    if (grounded_wheels == 0 && !recovering && p.air_angular_damp > 0.0f) {
        alpha -= omega * p.air_angular_damp;
    }
    if (alpha.length2() > 0.0f) {
        body->applyTorque(body->getInvInertiaTensorWorld().inverse() * alpha);
    }

    // air_gravity is a SIGNED correction of TOTAL gravity, blended by airborne wheel fraction.
    const float grounded_frac =
        nwheels > 0 ? static_cast<float>(grounded_wheels) / static_cast<float>(nwheels) : 0.0f;
    const float air_frac = 1.0f - grounded_frac;
    float extra_down = 0.0f;
    if (p.downforce_accel > 0.0f && p.car_max_speed > 0.0f) {
        const float frac = std::clamp(std::fabs(fwd_speed) / p.car_max_speed, 0.0f, 1.5f);
        extra_down += p.downforce_accel * frac * frac * grounded_frac;
    }
    extra_down += rf::gravity * (p.air_gravity - p.gravity_scale) * air_frac;
    if (std::fabs(extra_down) > 0.001f) {
        body->applyCentralForce(btVector3(0.0f, -extra_down * mass, 0.0f));
    }
}
