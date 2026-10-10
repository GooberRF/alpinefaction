#include <algorithm>
#include <cmath>
#include <vector>
#include <patch_common/CallHook.h>
#include <patch_common/FunHook.h>
#include <common/utils/list-utils.h>
#include "vphys_internal.h"
#include "../vehicle_physics.h"
#include "../vehicle.h"
#include "../vehicle_crash_tuning.h"
#include "../../../misc/destruction.h"
#include "../../../misc/level.h"
#include "../../../os/os.h"
#include "../../../rf/entity.h"
#include "../../../rf/geometry.h"
#include "../../../rf/multi.h"
#include "../../../rf/os/frametime.h"
#include "../../../rf/physics.h"
#include "../../../rf/player/camera.h"
#include "../../../rf/player/player.h"

namespace
{
    // The whole taxonomy, in one place: VPHYS class of a WIRE-FROZEN VehicleDamageClass row.
    int vphys_class_from_vdc(int vdc)
    {
        switch (vdc) {
        case VDC_JEEP:
            return VPHYS_CLASS_JEEP;
        case VDC_APC:
            return VPHYS_CLASS_APC;
        case VDC_DRILLER:
            return VPHYS_CLASS_DRILLER;
        case VDC_FIGHTER:
            return VPHYS_CLASS_FIGHTER;
        case VDC_SUB:
            return VPHYS_CLASS_SUB;
        default:
            return -1; // VDC_TURRET has no motion to author; -1 is not a synced vehicle at all
        }
    }

    // The speed the model drives the class at: the car's engine cutoff, the flyer's drag hold.
    float vphys_class_top_speed(int cls)
    {
        const VehiclePhysicsParams& p = params_for_class(cls);
        const float model_max = vphys_class_is_automobile(cls) ? p.car_max_speed : p.max_speed;
        return model_max > 0.0f ? model_max : 0.0f;
    }

    // The one derivation of a class's speed ceiling: the post-step cap and the wire clamp share it.
    float vphys_class_speed_cap(int cls)
    {
        return vphys_class_top_speed(cls) * (vphys_class_is_automobile(cls) ? 1.5f : 1.25f);
    }

    // A car's cap is on GROUND speed; fall speed has its own. One cap on the whole vector made a falling
    // hull trade its forward speed for fall speed in mid-air.
    constexpr float vphys_car_vertical_speed_cap = 30.0f;

    bool vphys_clamp_class_velocity(int cls, rf::Vector3& vel)
    {
        const float cap = vphys_class_speed_cap(cls);
        if (cap <= 0.0f) {
            return false;
        }
        if (!vphys_class_is_automobile(cls)) {
            const float speed = vel.len();
            if (speed <= cap) {
                return false;
            }
            vel *= cap / speed;
            return true;
        }
        bool clamped = false;
        const float ground = std::hypot(vel.x, vel.z);
        if (ground > cap) {
            vel.x *= cap / ground;
            vel.z *= cap / ground;
            clamped = true;
        }
        if (std::fabs(vel.y) > vphys_car_vertical_speed_cap) {
            vel.y = std::copysign(vphys_car_vertical_speed_cap, vel.y);
            clamped = true;
        }
        return clamped;
    }

    rf::Entity* vphys_target(int* out_class)
    {
        if (!rf::is_multi) {
            return nullptr;
        }
        rf::Entity* vehicle = vehicle_local_driven_vehicle();
        if (!vehicle || rf::entity_is_dying(vehicle)) {
            return nullptr;
        }
        const int cls = vphys_class_for(vehicle);
        if (cls < 0) {
            return nullptr;
        }
        *out_class = cls;
        return vehicle;
    }

    // The PREVIOUS frame's rf::frametime: movers_update_all runs ahead of obj_move_all.
    float g_vphys_prev_frame_dt = 0.0f;

    // How hard the cushion pushes (u/s^2) and the separation speed it stops pushing at (u/s).
    constexpr float vphys_pair_push_accel = 8.0f;
    constexpr float vphys_pair_push_max = 5.0f;
    // How far below horizontal the separation direction may dip, as a fraction of its horizontal reach.
    constexpr float vphys_pair_max_dip = 0.15f;
    constexpr float vphys_pair_min_dir_len = 0.05f;

    // POSE from our own body when we hold one, the ENTITY otherwise; SIZE is always hull_local_box.
    VehicleHullObb pair_hull_obb(rf::Entity* ep)
    {
        const HullBox box = hull_local_box(ep);
        rf::Vector3 origin = ep->pos;
        rf::Matrix3 orient = ep->orient;
        const VehicleSimBody* sim = sim_body_from_handle(ep->handle);
        if (sim && sim->body) {
            // The body origin IS the hull origin for every class, hence interchangeable with ep->pos.
            const btTransform& t = sim->body->getWorldTransform();
            origin = from_bt(t.getOrigin());
            orient = from_bt(t.getBasis());
        }
        VehicleHullObb obb;
        obb.orient = orient;
        obb.half = from_bt(box.half);
        obb.center = origin + orient.transform_vector(from_bt(box.center));
        return obb;
    }

    // Walks the ENTITY list, not g_vphys.bodies: nobody simulates a turret, so that is its only way in.
    // True when it pushed the body.
    bool vphys_apply_pair_response(VehicleSimBody& b, rf::Entity* self, float dt)
    {
        if (!b.body || dt <= 0.0f) {
            return false;
        }
        bool pushed = false;
        const VehicleHullObb mine = pair_hull_obb(self);
        const float my_reach = mine.half.len();

        for (rf::Entity& other : DoublyLinkedList{rf::entity_list}) {
            if (&other == self || !vehicle_is_synced_entity_type(&other) || other.life <= 0.0f
                || rf::entity_is_dying(&other)) {
                continue;
            }
            if (other.host_handle == self->handle || self->host_handle == other.handle) {
                continue;
            }
            const VehicleHullObb theirs = pair_hull_obb(&other);
            const rf::Vector3 delta = mine.center - theirs.center; // them -> us
            const float reach = my_reach + theirs.half.len() + 2.0f * vphys_pair_contact_margin;
            if (delta.len_sq() > reach * reach) {
                continue;
            }
            if (!vehicle_hull_obb_overlap(mine, theirs, vphys_pair_contact_margin)) {
                continue;
            }

            const float dist = delta.len();
            if (!(dist >= 0.0001f)) {
                continue;
            }
            rf::Vector3 n = delta / dist;

            // Fraction of the HORIZONTAL reach, so a hull squarely underneath gets no response at all.
            const float horiz = std::sqrt(n.x * n.x + n.z * n.z);
            n.y = std::max(n.y, -vphys_pair_max_dip * horiz);
            const float n_len = n.len();
            if (n_len < vphys_pair_min_dir_len) {
                continue;
            }
            n /= n_len;

            const btVector3 axis = to_bt(n);
            const btVector3 v = b.body->getLinearVelocity();
            const float vn = v.dot(axis); // > 0 is already separating from the other hull

            float target = std::max(vn, 0.0f);
            if (target < vphys_pair_push_max) {
                target = std::min(vphys_pair_push_max, target + vphys_pair_push_accel * dt);
            }
            if (target <= vn) {
                continue;
            }
            b.body->setLinearVelocity(v + axis * (target - vn));
            b.body->activate(true);
            pushed = true;
        }
        return pushed;
    }

    // The order is load-bearing: world pre-step, per-body pre, ONE world step, per-body post.
    struct BodyStepScratch
    {
        VehicleSimBody* b = nullptr;
        rf::Entity* ep = nullptr;
    };

    // Discards an open crash window unreported.
    void vphys_impact_window_close(VehicleSimBody& b)
    {
        b.impact_window_s = 0.0f;
        b.impact_sum = 0.0f;
        b.impact_peak = 0.0f;
        b.impact_pos_sum = 0.0f;
        b.impact_up_sum = 0.0f;
        b.impact_approach = 0.0f;
        b.impact_approach_ground = false;
        b.impact_landing = false;
        b.impact_land_stopped = false;
        b.impact_land_ext = false;
    }

    void vphys_world_pre_step()
    {
        obstacles_update_all();
        // The mover pass is not here - it runs ahead of the empty-world return, on every frame.
    }

    void vphys_body_pre_step(BodyStepScratch& s, float dt, float step_time)
    {
        VehicleSimBody& b = *s.b;
        rf::Entity* ep = s.ep;
        btRigidBody* body = b.body;
        const int cls = b.vehicle_class;
        const bool is_car = vphys_class_is_automobile(cls);
        const VehiclePhysicsParams& p = params_for_class(cls);
        b.impact_armed = false;
        b.handbrake_pin = false;
        bool reseeded = false;

        // A teleport or correction wins - but NOT for a server body: the interp echo is our own output.
        if (!b.server_owned && (ep->pos - b.written_pos).len() > 0.05f) {
            reseeded = true;
            vphys_impact_window_close(b);
            body_seed_from_entity(b, ep);
            b.pre_step_dt = 0.0f;
            if (is_car && b.raycast_vehicle) {
                b.raycast_vehicle->resetSuspension();
            }
        }

        // Nothing here may be a single-frame impulse: a frame with no substep discards it unintegrated.
        const bool stepping = step_time > 0.0f;
        if (stepping) {
            if (is_car) {
                b.car_box = hull_contact_box(b.car_box_base, p, hull_bottom_raise(p, ep, &b));
                car_apply_box_shape(b, p, ep, b.car_box);
            }
            else {
                body_apply_shape(b, p, ep);
            }
            body_apply_mass(b, p, ep);
            body->setRestitution(std::clamp(p.restitution, 0.0f, 0.95f));
            if (!is_car) {
                body->setFriction(std::max(b.parked_shape ? p.parked_friction : p.hull_friction, 0.0f));
            }
            if (is_car) {
                body->setDamping(std::clamp(p.car_linear_damping, 0.0f, 0.99f),
                                 std::clamp(p.car_angular_damping, 0.0f, 0.99f));
            }
            else {
                body->setDamping(std::clamp(p.linear_damping, 0.0f, 0.99f),
                                 std::clamp(p.angular_damping, 0.0f, 0.99f));
            }
            // PER-BODY gravity, re-applied AFTER body_apply_mass: addRigidBody stamps the world's.
            body->setGravity(btVector3(0.0f, -rf::gravity * p.gravity_scale, 0.0f));
        }

        // updateActions is NOT gated on activation state, so a sleeping server body's wheels detach.
        if (b.server_owned && is_car && b.raycast_vehicle) {
            const bool asleep = body->getActivationState() == ISLAND_SLEEPING;
            if (asleep && b.car_action_in_world) {
                g_vphys.world->removeVehicle(b.raycast_vehicle);
                b.car_action_in_world = false;
            }
            else if (!asleep && !b.car_action_in_world) {
                g_vphys.world->addVehicle(b.raycast_vehicle);
                b.car_action_in_world = true;
                b.raycast_vehicle->resetSuspension();
                // Whatever woke it may have changed the ground, and resetSuspension leaves the old contacts
                // standing: the handbrake would pin on them before this step's casts land.
                for (int i = 0; i < b.raycast_vehicle->getNumWheels(); ++i) {
                    btWheelInfo::RaycastInfo& ri = b.raycast_vehicle->getWheelInfo(i).m_raycastInfo;
                    ri.m_isInContact = false;
                    ri.m_groundObject = nullptr;
                }
                b.wheel_casts.clear();
            }
        }
        if (b.server_owned && body->getActivationState() == ISLAND_SLEEPING) {
            b.pre_step_dt = 0.0f;
            vphys_impact_window_close(b);
            return;
        }
        b.pre_step_dt += dt;

        if (is_car) {
            const bool car_ok = apply_car_controls(b, ep, p, dt);
            if (car_ok && stepping) {
                apply_car_model(b, p, b.pre_step_dt, step_time);
            }
        }
        else {
            apply_flight_model(b, ep, p, cls, step_time);
        }

        // After the model on purpose: the ceiling's sink floor writes velocity directly.
        if (stepping) {
            const bool pushed = vphys_apply_pair_response(b, ep, b.pre_step_dt);
            b.handbrake_pin = b.handbrake_pin && !pushed; // the pin would erase the push
            b.pre_step_dt = 0.0f;
            // Last, so every velocity write above is the step's input and never reads as an impact.
            b.impact_pre_vel = body->getLinearVelocity();
            b.impact_armed = true;
            b.impact_skip = reseeded || pushed || timer::get_i64(1000) < b.shove_brake_until_ms;
            b.impact_drilling = is_car && car_drills_on(ep, p);
        }
    }

    void vphys_world_step(float dt)
    {
        g_vphys.world->stepSimulation(dt, vphys_max_substeps, vphys_fixed_timestep);
    }

    VehicleSimBody* car_body_of(const btCollisionObject* obj)
    {
        if (!obj) {
            return nullptr;
        }
        VehicleSimBody* b = static_cast<VehicleSimBody*>(obj->getUserPointer());
        if (!b || b->body != obj || !vphys_class_is_automobile(b->vehicle_class)) {
            return nullptr;
        }
        return b;
    }

    // ONE walk of the world's manifolds for every car, bucketed by body. Manifold contents do not
    // change between here and the post-step read.
    void vphys_ground_contact_pass()
    {
        for (const auto& owned : g_vphys.bodies) {
            owned->chassis_ground_contact_pass = false;
            owned->paired_with_kinematic = false;
        }
        const auto awake_kinematic = [](const btCollisionObject* obj) {
            return obj->isKinematicObject() && obj->getActivationState() != ISLAND_SLEEPING;
        };
        const int nm = g_vphys.dispatcher->getNumManifolds();
        for (int m = 0; m < nm; ++m) {
            const btPersistentManifold* pm = g_vphys.dispatcher->getManifoldByIndexInternal(m);
            VehicleSimBody* car0 = car_body_of(pm->getBody0());
            VehicleSimBody* car1 = car_body_of(pm->getBody1());
            // Contacts or none, as Bullet's island manager has it.
            if (car0 && awake_kinematic(pm->getBody1())) {
                car0->paired_with_kinematic = true;
            }
            if (car1 && awake_kinematic(pm->getBody0())) {
                car1->paired_with_kinematic = true;
            }
            // getBody1 is the flipped side: its ground normal is the negated m_normalWorldOnB.
            if ((!car0 || car0->chassis_ground_contact_pass)
                && (!car1 || car1->chassis_ground_contact_pass)) {
                continue;
            }
            for (int c = 0; c < pm->getNumContacts(); ++c) {
                const btManifoldPoint& cp = pm->getContactPoint(c);
                if (cp.getDistance() > 0.02f) {
                    continue;
                }
                const float ny = cp.m_normalWorldOnB.y();
                if (car0 && ny > 0.5f) {
                    car0->chassis_ground_contact_pass = true;
                }
                if (car1 && -ny > 0.5f) {
                    car1->chassis_ground_contact_pass = true;
                }
            }
        }
    }

    VehicleSimBody* impact_body_of(const btCollisionObject* obj)
    {
        if (!obj) {
            return nullptr;
        }
        VehicleSimBody* b = static_cast<VehicleSimBody*>(obj->getUserPointer());
        return b && b->body == obj && b->impact_armed ? b : nullptr;
    }

    bool impact_is_level_object(const btCollisionObject* obj)
    {
        const btBroadphaseProxy* proxy = obj ? obj->getBroadphaseHandle() : nullptr;
        return proxy && proxy->m_collisionFilterGroup == vphys_group_level;
    }

    // The level mesh and movers only: another hull's box forms no pair, and hull on hull is the ram's.
    void vphys_impact_contact_pass()
    {
        for (const auto& owned : g_vphys.bodies) {
            owned->impact_contact = false;
        }
        const int nm = g_vphys.dispatcher->getNumManifolds();
        for (int m = 0; m < nm; ++m) {
            const btPersistentManifold* pm = g_vphys.dispatcher->getManifoldByIndexInternal(m);
            VehicleSimBody* b = impact_body_of(pm->getBody0());
            float side = 1.0f; // m_normalWorldOnB points at body 0
            if (!b || !impact_is_level_object(pm->getBody1())) {
                b = impact_body_of(pm->getBody1());
                side = -1.0f;
                if (!b || !impact_is_level_object(pm->getBody0())) {
                    continue;
                }
            }
            const btVector3 dv = b->body->getLinearVelocity() - b->impact_pre_vel;
            const btVector3 fwd = b->body->getWorldTransform().getBasis().getColumn(2);
            // A mover's own motion counts toward the speed into it; the level mesh has none.
            const btRigidBody* other = btRigidBody::upcast(side > 0.0f ? pm->getBody1() : pm->getBody0());
            for (int c = 0; c < pm->getNumContacts(); ++c) {
                const btManifoldPoint& cp = pm->getContactPoint(c);
                if (cp.getDistance() > 0.02f) {
                    continue;
                }
                const btVector3 n = cp.m_normalWorldOnB * side;
                if (b->impact_drilling && n.y() < vehicle_crash_ground_ny
                    && n.dot(fwd) < vehicle_crash_drill_face_dot) {
                    continue;
                }
                const float dvn = dv.dot(n);
                if (!b->impact_contact || dvn > b->impact_contact_dvn) {
                    btVector3 rel_pre = b->impact_pre_vel;
                    if (other) {
                        const btVector3& p = side > 0.0f ? cp.getPositionWorldOnB() : cp.getPositionWorldOnA();
                        rel_pre -= other->getVelocityInLocalPoint(p - other->getCenterOfMassPosition());
                    }
                    b->impact_contact = true;
                    b->impact_contact_dvn = dvn;
                    b->impact_contact_ny = n.y();
                    b->impact_contact_approach = std::max(-rel_pre.dot(n), 0.0f);
                    b->impact_contact_n = n;
                }
            }
        }
    }

    struct VehicleImpact
    {
        int vehicle_handle;
        float impact_dv;
        bool ground;
    };

    void vphys_impact_measure(VehicleSimBody& b, bool speed_clamped, float step_time,
                              std::vector<VehicleImpact>& out)
    {
        if (!b.impact_armed) {
            return;
        }
        b.impact_armed = false;
        const btVector3 vel = b.body->getLinearVelocity();
        const btVector3 dv = vel - b.impact_pre_vel;
        bool contact = b.impact_contact;
        float dvn = b.impact_contact_dvn;
        float ny = b.impact_contact_ny;
        float approach = b.impact_contact_approach;
        btVector3 n = b.impact_contact_n;
        if (b.raycast_vehicle) {
            for (int i = 0; i < b.raycast_vehicle->getNumWheels(); ++i) {
                const btWheelInfo& w = b.raycast_vehicle->getWheelInfo(i);
                if (!w.m_raycastInfo.m_isInContact) {
                    continue;
                }
                const btVector3& wn = w.m_raycastInfo.m_contactNormalWS;
                const float len = wn.length();
                if (len < 0.0001f) {
                    continue;
                }
                const float d = dv.dot(wn) / len;
                if (!contact || d > dvn) {
                    contact = true;
                    dvn = d;
                    ny = 1.0f; // a wheel is always a landing: its edge normal leans back on any step
                    n = wn / len;
                    approach = std::max(-b.impact_pre_vel.dot(n), 0.0f);
                }
            }
        }
        const bool after_air = b.impact_grounded_s < vehicle_crash_land_air_grace_s;
        b.impact_grounded_s = contact ? std::min(b.impact_grounded_s + step_time, 1.0f) : 0.0f;
        if (b.impact_skip || speed_clamped) {
            vphys_impact_window_close(b);
            return;
        }
        if (!contact) {
            dvn = 0.0f;
            approach = 0.0f;
        }
        if (b.impact_window_s <= 0.0f) {
            if (dvn < vehicle_crash_open_accel * step_time) {
                return;
            }
            b.impact_window_s = vehicle_crash_window_s;
            b.impact_landing = after_air;
        }
        b.impact_sum += dvn;
        b.impact_peak = std::max(b.impact_peak, b.impact_sum);
        if (dvn > 0.0f) {
            b.impact_pos_sum += dvn;
            b.impact_up_sum += dvn * ny;
        }
        if (approach > b.impact_approach) {
            b.impact_approach = approach;
            b.impact_approach_n = n;
            b.impact_approach_vel = b.impact_pre_vel;
            b.impact_approach_ground = ny >= vehicle_crash_ground_ny;
            b.impact_land_stopped = false; // a new largest approach must confirm its own stop
        }
        // Net, gravity and drive included: a touchdown kills its normal speed, a graze does not.
        if (b.impact_approach > 0.0f
            && (vel - b.impact_approach_vel).dot(b.impact_approach_n)
                   >= vehicle_crash_land_stop_frac * b.impact_approach) {
            b.impact_land_stopped = true;
        }
        b.impact_window_s -= step_time;
        if (b.impact_window_s > 0.0f && !(b.impact_land_ext && b.impact_land_stopped)) {
            return;
        }
        // Inside an extended landing a wall needs both the largest approach and the vote to say wall.
        const bool vote_ground = b.impact_up_sum >= vehicle_crash_ground_ny * b.impact_pos_sum;
        const bool ground = b.impact_land_ext ? (b.impact_approach_ground || vote_ground) : vote_ground;
        // Never more than the speed into the surface: a pinned drivetrain keeps adding velocity change.
        float impact_dv = std::min(b.impact_peak, b.impact_approach);
        if (ground && b.impact_landing && b.impact_approach_ground) {
            if (b.impact_land_stopped) {
                impact_dv = b.impact_approach; // suspension spreads the stop past any summing window
            }
            else if (!b.impact_land_ext) {
                b.impact_land_ext = true;
                b.impact_window_s += vehicle_crash_land_window_s - vehicle_crash_window_s;
                return;
            }
        }
        const VehicleImpact impact{b.vehicle_handle, impact_dv, ground};
        vphys_impact_window_close(b);
        if (std::isfinite(impact.impact_dv) && impact.impact_dv > 0.0f) {
            out.push_back(impact);
        }
    }

    // A never-entered hull pinned on its handbrake holds still until something moves it, so it need not
    // wait out Bullet's 2 s of deactivation time. A hull a mover keeps awake would only flicker.
    bool vphys_parked_hull_can_sleep(const VehicleSimBody& b)
    {
        return b.server_owned && b.handbrake_pin && b.auto_handbrake && !b.paired_with_kinematic;
    }

    // At the pose the entity was just given, so neither it nor a later wake moves the hull. The wheels come
    // off here rather than at the next pre-step, so a wake in between takes the same re-attach path.
    void vphys_body_sleep_now(VehicleSimBody& b)
    {
        if (b.car_action_in_world) {
            g_vphys.world->removeVehicle(b.raycast_vehicle);
            b.car_action_in_world = false;
        }
        btRigidBody* body = b.body;
        btTransform t;
        b.motion_state->getWorldTransform(t);
        body->setWorldTransform(t);
        body->setInterpolationWorldTransform(t);
        const btVector3 zero(0.0f, 0.0f, 0.0f);
        body->setLinearVelocity(zero);
        body->setAngularVelocity(zero);
        body->setInterpolationLinearVelocity(zero);
        body->setInterpolationAngularVelocity(zero);
        body->clearForces();
        body->setActivationState(ISLAND_SLEEPING);
    }

    void vphys_body_post_step(BodyStepScratch& s, float step_time, std::vector<VehicleImpact>& impacts)
    {
        VehicleSimBody& b = *s.b;
        rf::Entity* ep = s.ep;
        btRigidBody* body = b.body;
        const int cls = b.vehicle_class;
        const bool is_car = vphys_class_is_automobile(cls);

        // A sleeping server body integrated nothing; the entity already wears the last awake pose.
        if (b.server_owned && body->getActivationState() == ISLAND_SLEEPING) {
            return;
        }

        // The motion state carries the pose Bullet INTERPOLATED to the end of the frame, not the body's.
        btTransform interp;
        b.motion_state->getWorldTransform(interp);
        rf::Vector3 pos = from_bt(interp.getOrigin());
        rf::Vector3 vel = from_bt(body->getLinearVelocity());
        const rf::Matrix3 orient = from_bt(interp.getBasis());

        // The one place a non-finite solve can leave the world: the entity pose is broadcast from it.
        if (!vehicle_vector_is_finite(pos) || !vehicle_vector_is_finite(vel) || !vphys_matrix_is_finite(orient)) {
            vphys_impact_window_close(b);
            body_seed_from_entity(b, ep);
            body->setLinearVelocity(btVector3(0.0f, 0.0f, 0.0f));
            body->setInterpolationLinearVelocity(btVector3(0.0f, 0.0f, 0.0f));
            return;
        }

        rf::Vector3 capped_vel = vel;
        const bool speed_clamped = vphys_clamp_class_velocity(cls, capped_vel);
        vphys_impact_measure(b, speed_clamped, step_time, impacts);
        if (speed_clamped) {
            vel = capped_vel;
            body->setLinearVelocity(to_bt(vel));
        }

        // ground_material for the airborne-exit gate: grounded => 0, all airborne => -1 (stock semantics).
        bool wheels_all_airborne = true;
        if (is_car && b.raycast_vehicle) {
            for (int i = 0; i < b.raycast_vehicle->getNumWheels(); ++i) {
                const btWheelInfo& w = b.raycast_vehicle->getWheelInfo(i);
                if (w.m_raycastInfo.m_isInContact) {
                    wheels_all_airborne = false;
                }
            }
        }

        if (is_car) {
            if (step_time > 0.0f || g_vphys.manifolds_dirty) {
                b.chassis_ground_contact = b.chassis_ground_contact_pass;
            }
            ep->ground_material = (wheels_all_airborne && !b.chassis_ground_contact) ? -1 : 0;
        }

        // After the cap, both: a 0-substep frame extrapolates from the interpolation velocity.
        body->setLinearVelocity(to_bt(vel));
        body->setInterpolationLinearVelocity(to_bt(vel));

        // pos/next_pos share a value so the stock batch tail is a no-op; phb is what it rebuilds from.
        ep->p_data.pos = pos;
        ep->p_data.next_pos = pos;
        ep->p_data.orient = orient;
        ep->p_data.next_orient = orient;
        ep->orient = orient;
        // A server hull's REPLICATED velocity must be zeroed HERE: obj_update packs it later this frame.
        ep->p_data.vel = b.server_owned ? rf::Vector3{} : vel;
        ep->p_data.rotvel = rf::Vector3{};
        ep->p_data.ang_momentum = rf::Vector3{};
        ep->control_data.phb = vehicle_make_orient_phb(orient);
        ep->move(&pos);
        // A server body has no stock re-room follow-up, and the drown/liquid tests are room-keyed.
        if (b.server_owned) {
            ep->update_room();
        }
        // Seeded here so the fire report, which runs before the physics batch, sees this frame's hull.
        vehicle_rebuild_eye_orient(ep, orient);
        vehicle_refresh_aim_orient(ep); // the aim outranks this hull-derived seed
        b.written_pos = ep->pos;
        vehicle_note_observed_pose(ep->handle, pos, std::atan2(orient.fvec.x, orient.fvec.z));

        if (vphys_parked_hull_can_sleep(b)) {
            vphys_body_sleep_now(b);
        }
    }

    void vphys_step_frame()
    {
        vphys_reconcile();
        // Ahead of the empty-world return, so the step_will_run answer below is exact.
        std::vector<VehicleSimBody*> stale;
        for (const auto& owned : g_vphys.bodies) {
            if (!rf::entity_from_handle(owned->vehicle_handle)) {
                stale.push_back(owned.get());
            }
        }
        for (VehicleSimBody* dead : stale) {
            if (dead == g_vphys.driven) {
                driven_body_destroy();
            }
            else {
                sim_body_destroy(dead);
            }
        }
        const float dt = rf::frametime;
        const bool step_will_run = !g_vphys.bodies.empty() && dt > 0.0f;
        // Must advance on exactly the frames vphys_world_step is reached: it is Bullet's own
        // m_localTime accumulator, re-derived.
        const float step_time = step_will_run ? vphys_frame_step_time(dt) : 0.0f;
        const float moved_dt = g_vphys_prev_frame_dt;
        g_vphys_prev_frame_dt = dt;
        // Every frame the world exists, not just the ones that step.
        if (g_vphys.world) {
            movers_update_all(step_will_run, step_time, moved_dt);
        }
        if (!step_will_run) {
            return;
        }

        std::vector<BodyStepScratch> step;
        step.reserve(g_vphys.bodies.size());
        for (const auto& owned : g_vphys.bodies) {
            BodyStepScratch s;
            s.b = owned.get();
            s.ep = rf::entity_from_handle(owned->vehicle_handle);
            step.push_back(s);
        }

        if (step_time > 0.0f) {
            vphys_world_pre_step();
        }
        for (BodyStepScratch& s : step) {
            vphys_body_pre_step(s, dt, step_time);
        }
        vphys_world_step(dt);
        if (step_time > 0.0f || g_vphys.manifolds_dirty) {
            vphys_ground_contact_pass();
        }
        if (step_time > 0.0f) {
            vphys_impact_contact_pass();
        }
        std::vector<VehicleImpact> impacts;
        for (BodyStepScratch& s : step) {
            vphys_body_post_step(s, step_time, impacts);
        }
        g_vphys.manifolds_dirty = false;
        // After the walk: the damage can destroy a hull, and its body with it.
        for (const VehicleImpact& impact : impacts) {
            if (rf::Entity* ep = rf::entity_from_handle(impact.vehicle_handle)) {
                vehicle_crash_impact(ep, impact.impact_dv, impact.ground);
            }
        }
    }

    // The CALL to player_process_controls: this frame's input is in ai.ci and nothing has consumed it.
    CallHook<void(rf::Player*)> player_process_controls_vphys_hook{
        0x00433633,
        [](rf::Player* pp) {
            player_process_controls_vphys_hook.call_target(pp);
            vphys_step_frame();
        },
    };

    // The physics batch's per-entity integration CALL: skipping it removes stock movement only.
    CallHook<void(rf::Entity*)> physics_simulate_entity_vphys_hook{
        0x004877C9,
        [](rf::Entity* ep) {
            // The hottest per-entity path in the engine: no hash lookup on a level with no hull.
            if (!g_vphys.by_handle.empty() && vehicle_physics_drives(ep)) {
                return;
            }
            physics_simulate_entity_vphys_hook.call_target(ep);
            // A simulated rider's body was just rebuilt upright, and the pair walk after this batch
            // collides against it.
            vehicle_pin_rider_body(ep);
        },
    };

    void notify_geomod_params(const rf::GeomodParams* params)
    {
        // No factory means no level mesh, so the shape lookup below would be paid for nothing.
        if (!params || !g_level_has_bullet_vehicles) {
            return;
        }
        const float radius = geomod_crater_radius(*params).value_or(0.0f);
        vehicle_physics_notify_geomod(params->pos, radius > 0.0f ? radius : 0.0f);
    }

    // Live craters: geomod_create and the client's 0x29 handler both queue here; geomod_create returns
    // early on a multiplayer client.
    FunHook<void(rf::GeomodParams*)> geomod_queue_add_vphys_hook{
        0x00437230,
        [](rf::GeomodParams* params) {
            geomod_queue_add_vphys_hook.call_target(params);
            notify_geomod_params(params);
        },
    };

    // A joiner's replayed craters (pregame boolean 0x11, handler 0x004766B0) carve synchronously through
    // geomod_init without queueing, after this client's level mesh was built. Marked before the carve.
    CallHook<void(rf::GeomodParams*)> pregame_boolean_geomod_init_vphys_hook{
        0x00476787,
        [](rf::GeomodParams* params) {
            notify_geomod_params(params);
            pregame_boolean_geomod_init_vphys_hook.call_target(params);
        },
    };
} // namespace

int vphys_class_for(const rf::Entity* ep)
{
    return vphys_class_from_vdc(vehicle_damage_class(ep));
}

void vphys_reconcile()
{
    int cls = VPHYS_CLASS_FIGHTER;
    rf::Entity* target = vphys_target(&cls);
    const int want = target ? target->handle : -1;
    const VehicleSimBody* have_body = g_vphys.driven;
    const int have = have_body ? have_body->vehicle_handle : -1;
    if (want == have && (!target || (have_body && cls == have_body->vehicle_class))) {
        return;
    }
    driven_body_destroy();
    if (target) {
        // A listen host's boarded hull may still carry its server body; one handle can hold only one.
        sim_body_destroy(sim_body_from_handle(target->handle));
        g_vphys.driven = body_create(target, cls);
    }
}

void vphys_prev_frame_dt_reset()
{
    g_vphys_prev_frame_dt = 0.0f;
}

// btDiscreteDynamicsWorld::stepSimulation, on ITS accumulator and read before the step: the residual
// drains UNCLAMPED while only the CLAMPED count integrates. This is the exact window the same call
// then hands saveKinematicState, so the mover kinematic velocity matches.
float vphys_frame_step_time(float dt)
{
    if (!g_vphys.world) {
        return 0.0f;
    }
    const btScalar local = g_vphys.world->local_time() + dt;
    int num = 0;
    if (local >= vphys_fixed_timestep) {
        num = static_cast<int>(local / vphys_fixed_timestep);
    }
    const int clamped = num > vphys_max_substeps ? vphys_max_substeps : num;
    return clamped * vphys_fixed_timestep;
}

void vehicle_physics_geomod_queue_add_raw(rf::GeomodParams* params)
{
    geomod_queue_add_vphys_hook.call_target(params);
}

bool vehicle_physics_drives(const rf::Entity* ep)
{
    if (!ep) {
        return false;
    }
    const VehicleSimBody* b = sim_body_from_handle(ep->handle);
    return b != nullptr && b->body != nullptr;
}

bool vehicle_physics_driven_velocity(int vehicle_handle, rf::Vector3* out)
{
    const VehicleSimBody* b = g_vphys.driven;
    if (!b || !b->body || b->vehicle_handle != vehicle_handle) {
        return false;
    }
    if (out) {
        *out = from_bt(b->body->getLinearVelocity());
    }
    return true;
}

bool vehicle_physics_driven_steer_angle(int vehicle_handle, float* out)
{
    const VehicleSimBody* b = g_vphys.driven;
    if (!b || !b->body || b->vehicle_handle != vehicle_handle) {
        return false;
    }
    if (out) {
        *out = b->steer_current;
    }
    return true;
}

void vehicle_physics_on_seat_change()
{
    vphys_reconcile();
}

void vehicle_physics_do_frame()
{
    // The remesh belongs HERE: it must not queue behind the step's bodies.empty() return.
    level_mesh_poll_remesh();

    vphys_reconcile();

    // A dedicated server never reaches that hook, so it steps here; exactly one of the two per frame.
    if (rf::is_dedicated_server) {
        vphys_step_frame();
    }
}

bool vehicle_physics_server_ensure(rf::Entity* ep, const rf::Vector3* seed_vel)
{
    if (!ep || !rf::is_multi || !rf::is_server) {
        return false;
    }
    // Already ours: do NOT reseed - the body is mid-simulation.
    if (VehicleSimBody* held = sim_body_from_handle(ep->handle)) {
        return held->server_owned;
    }
    if (rf::entity_is_dying(ep) || ep->life <= 0.0f) {
        return false;
    }
    // A TURRET answers -1: the only exclusion, and one by construction.
    const int cls = vphys_class_for(ep);
    if (cls < 0) {
        return false;
    }

    VehicleSimBody* b = body_create(ep, cls);
    if (!b || !b->body) {
        return false;
    }
    b->server_owned = true;
    if (!vphys_class_is_automobile(cls)) {
        body_apply_shape(*b, params_for_class(cls), ep); // the parked cylinder keys off server_owned
    }
    if (seed_vel) {
        b->body->setLinearVelocity(to_bt(*seed_vel));
        b->body->setInterpolationLinearVelocity(to_bt(*seed_vel));
    }
    // forceActivationState because body_create stamped DISABLE_DEACTIVATION; thresholds < Bullet's.
    b->body->forceActivationState(ACTIVE_TAG);
    b->body->setDeactivationTime(0.0f);
    b->body->setSleepingThresholds(0.25f, 0.25f);
    return true;
}

void vehicle_physics_server_release(int vehicle_handle)
{
    VehicleSimBody* b = sim_body_from_handle(vehicle_handle);
    if (!b || !b->server_owned) {
        return; // no body, or the LOCAL player's driven body, which vphys_reconcile owns
    }
    sim_body_destroy(b);
}

bool vehicle_physics_server_state(int vehicle_handle, rf::Vector3* out_pos, float* out_linear_speed,
                                  float* out_angular_speed, bool* out_asleep)
{
    const VehicleSimBody* b = sim_body_from_handle(vehicle_handle);
    if (!b || !b->server_owned || !b->body) {
        return false;
    }
    if (out_pos) {
        // The INTERPOLATED pose, i.e. the same one the post-step wrote into the entity.
        btTransform interp;
        b->motion_state->getWorldTransform(interp);
        *out_pos = from_bt(interp.getOrigin());
    }
    if (out_linear_speed) {
        *out_linear_speed = b->body->getLinearVelocity().length();
    }
    if (out_angular_speed) {
        *out_angular_speed = b->body->getAngularVelocity().length();
    }
    if (out_asleep) {
        *out_asleep = b->body->getActivationState() == ISLAND_SLEEPING;
    }
    return true;
}

bool vehicle_physics_server_owns(int vehicle_handle)
{
    const VehicleSimBody* b = sim_body_from_handle(vehicle_handle);
    return b != nullptr && b->server_owned && b->body != nullptr;
}

void vehicle_physics_entity_hull_box(const rf::Entity* ep, rf::Vector3* out_half,
                                     rf::Vector3* out_center)
{
    if (!ep) {
        return;
    }
    const HullBox box = hull_local_box(ep);
    if (out_half) {
        *out_half = from_bt(box.half);
    }
    if (out_center) {
        *out_center = from_bt(box.center);
    }
}

bool vehicle_physics_server_velocity(int vehicle_handle, rf::Vector3* out)
{
    const VehicleSimBody* b = sim_body_from_handle(vehicle_handle);
    if (!b || !b->server_owned || !b->body) {
        return false;
    }
    if (out) {
        *out = from_bt(b->body->getLinearVelocity());
    }
    return true;
}

void vehicle_physics_server_wake(int vehicle_handle)
{
    VehicleSimBody* b = sim_body_from_handle(vehicle_handle);
    if (!b || !b->server_owned || !b->body) {
        return;
    }
    // Unconditional: activate() also resets the deactivation TIMER, keeping the hull awake.
    b->body->activate(true);
}

bool vehicle_physics_server_shove(int vehicle_handle, const rf::Vector3& dir, float delta_v)
{
    VehicleSimBody* b = sim_body_from_handle(vehicle_handle);
    if (!b || !b->server_owned || !b->body || delta_v <= 0.0f) {
        return false;
    }
    btVector3 d = to_bt(dir);
    const float len = d.length();
    if (len < 0.0001f) {
        return false;
    }
    d /= len;
    const float inv_mass = b->body->getInvMass();
    if (inv_mass <= 0.0f) {
        return false;
    }
    // Wake FIRST: updateActivationState zeroes a sleeping body's velocity, erasing the impulse.
    b->body->activate(true);
    b->body->applyCentralImpulse(d * (delta_v / inv_mass));
    b->shove_brake_until_ms = timer::get_i64(1000) + vehicle_shove_brake_grace_ms;
    return true;
}

bool vehicle_physics_camera_do_frame(rf::Camera* camera)
{
    return vphys_chase_camera_do_frame(camera);
}

void vehicle_physics_render_debug()
{
    vphys_render_debug();
}

bool vehicle_physics_class_syncs_driver_aim(const rf::Entity* vehicle)
{
    // Spelled out rather than read off cam_enable: a wire gate must not move when a camera is tuned.
    const int cls = vphys_class_for(vehicle);
    return cls == VPHYS_CLASS_JEEP || cls == VPHYS_CLASS_APC;
}

float vehicle_physics_class_max_speed(int vdc_class)
{
    const int cls = vphys_class_from_vdc(vdc_class);
    return cls < 0 ? 0.0f : vphys_class_speed_cap(cls);
}

float vehicle_physics_class_top_speed(int vdc_class)
{
    const int cls = vphys_class_from_vdc(vdc_class);
    return cls < 0 ? 0.0f : vphys_class_top_speed(cls);
}

float vehicle_physics_class_bounce_gain(int vdc_class)
{
    const int cls = vphys_class_from_vdc(vdc_class);
    if (cls < 0) {
        return 1.0f;
    }
    // The same clamp the pre-step gives the body.
    return 1.0f + std::clamp(params_for_class(cls).restitution, 0.0f, 0.95f);
}

void vehicle_physics_clamp_class_velocity(int vdc_class, rf::Vector3& vel)
{
    const int cls = vphys_class_from_vdc(vdc_class);
    if (cls < 0 || vphys_class_speed_cap(cls) <= 0.0f || !vehicle_vector_is_finite(vel)) {
        vel = rf::Vector3{};
        return;
    }
    vphys_clamp_class_velocity(cls, vel);
}

rf::Vector3 vehicle_physics_cap_class_speed(int vdc_class, const rf::Vector3& vel)
{
    const float limit = vehicle_physics_class_max_speed(vdc_class);
    const float speed = vel.len();
    if (speed <= limit) {
        return vel;
    }
    return speed > 0.0f && std::isfinite(speed) ? vel * (limit / speed) : rf::Vector3{};
}

float vehicle_physics_class_max_impact_speed(int vdc_class)
{
    const int cls = vphys_class_from_vdc(vdc_class);
    float speed = vehicle_physics_class_max_speed(vdc_class);
    if (cls >= 0 && vphys_class_is_automobile(cls)) {
        speed = std::hypot(speed, vphys_car_vertical_speed_cap);
    }
    return speed * vehicle_physics_class_bounce_gain(vdc_class);
}

void vehicle_physics_level_reset()
{
    // The EARLY hook: teardown only - this fires before the RFL parse, so factory records are empty.
    world_destroy();
    g_vcam = VehicleChaseCamera{};
    g_level_has_bullet_vehicles = false;
}

void vehicle_physics_level_init_post()
{
    // After the stock level_init_post, hence the whole RFL parse; a dedicated server runs it too.
    g_level_has_bullet_vehicles = vehicle_level_has_factories();
    if (!g_level_has_bullet_vehicles) {
        return;
    }
    if (!g_vphys.world) {
        world_create();
    }
    level_mesh_build();
}

void vehicle_physics_on_multi_shutdown()
{
    world_destroy();
    g_vcam = VehicleChaseCamera{};
    g_level_has_bullet_vehicles = false;
}

void vehicle_physics_apply_patches()
{
    physics_simulate_entity_vphys_hook.install();
    player_process_controls_vphys_hook.install();
    geomod_queue_add_vphys_hook.install();
    pregame_boolean_geomod_init_vphys_hook.install();
    vphys_world_install_patches();
    vphys_camera_install_patches();
    vphys_debug_install_patches();
}
