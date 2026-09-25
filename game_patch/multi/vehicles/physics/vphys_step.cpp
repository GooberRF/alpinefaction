#include <algorithm>
#include <cmath>
#include <vector>
#include <patch_common/CallHook.h>
#include <patch_common/FunHook.h>
#include <common/utils/list-utils.h>
#include "vphys_internal.h"
#include "../vehicle_physics.h"
#include "../vehicle.h"
#include "../../../misc/destruction.h"
#include "../../../misc/level.h"
#include "../../../os/console.h"
#include "../../../os/os.h"
#include "../../../rf/ai.h"
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
} // namespace

int vphys_class_for(const rf::Entity* ep)
{
    return vphys_class_from_vdc(vehicle_damage_class(ep));
}

namespace
{
    // The one derivation of a class's speed ceiling: the post-step cap and the wire clamp share it.
    float vphys_class_speed_cap(int cls)
    {
        const VehiclePhysicsParams& p = params_for_class(cls);
        const bool is_car = vphys_class_is_automobile(cls);
        const float model_max = is_car ? p.car_max_speed : p.max_speed;
        return model_max > 0.0f ? model_max * (is_car ? 1.5f : 1.25f) : 0.0f;
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
} // namespace

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

namespace
{
    // The PREVIOUS frame's rf::frametime: movers_update_all runs ahead of obj_move_all.
    float g_vphys_prev_frame_dt = 0.0f;
} // namespace

void vphys_prev_frame_dt_reset()
{
    g_vphys_prev_frame_dt = 0.0f;
}

// btDiscreteDynamicsWorld::stepSimulation lines 386-412, on ITS accumulator and read before the
// step: the residual drains UNCLAMPED while only the CLAMPED count integrates. This is the exact
// window the same call then hands saveKinematicState, so the mover kinematic velocity matches.
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

namespace
{
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
    void vphys_apply_pair_response(VehicleSimBody& b, rf::Entity* self, float dt)
    {
        if (!b.body || dt <= 0.0f) {
            return;
        }
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
            if (dist < 0.0001f) {
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
        }
    }

    // The order is load-bearing: world pre-step, per-body pre, ONE world step, per-body post.
    struct BodyStepScratch
    {
        VehicleSimBody* b = nullptr;
        rf::Entity* ep = nullptr;
    };

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

        // A teleport or correction wins - but NOT for a server body: the interp echo is our own output.
        if (!b.server_owned && (ep->pos - b.written_pos).len() > 0.05f) {
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
                body->setFriction(std::max(p.hull_friction, 0.0f));
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
            }
        }
        if (b.server_owned && body->getActivationState() == ISLAND_SLEEPING) {
            b.pre_step_dt = 0.0f;
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
            apply_flight_model(b, ep, p, cls);
        }

        // After the model on purpose: the ceiling's sink floor writes velocity directly.
        if (stepping) {
            vphys_apply_pair_response(b, ep, b.pre_step_dt);
            b.pre_step_dt = 0.0f;
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

    // ONE walk of the world's manifolds for every car, bucketed by body: the same test run per car
    // was O(cars x manifolds). Manifold contents do not change between here and the post-step read.
    void vphys_ground_contact_pass()
    {
        for (const auto& owned : g_vphys.bodies) {
            owned->chassis_ground_contact_pass = false;
        }
        const int nm = g_vphys.dispatcher->getNumManifolds();
        for (int m = 0; m < nm; ++m) {
            const btPersistentManifold* pm = g_vphys.dispatcher->getManifoldByIndexInternal(m);
            VehicleSimBody* car0 = car_body_of(pm->getBody0());
            VehicleSimBody* car1 = car_body_of(pm->getBody1());
            // getBody1 is the flipped side: its ground normal is the negated m_normalWorldOnB.
            if ((!car0 || car0->chassis_ground_contact_pass) &&
                (!car1 || car1->chassis_ground_contact_pass)) {
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

    void vphys_body_post_step(BodyStepScratch& s, float step_time)
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
        const rf::Vector3 pre_contact_pos = pos;
        rf::Vector3 vel = from_bt(body->getLinearVelocity());
        const rf::Matrix3 orient = from_bt(interp.getBasis());

        // The one place a non-finite solve can leave the world: the entity pose is broadcast from it.
        if (!vphys_finite(pos) || !vphys_finite(vel) || !vphys_finite(orient)) {
            body_seed_from_entity(b, ep);
            body->setLinearVelocity(btVector3(0.0f, 0.0f, 0.0f));
            body->setInterpolationLinearVelocity(btVector3(0.0f, 0.0f, 0.0f));
            return;
        }

        const float speed = vel.len();
        const float speed_cap = vphys_class_speed_cap(cls);
        if (speed_cap > 0.0f && speed > speed_cap) {
            vel *= speed_cap / speed;
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

        ep->orient = orient;
        if (is_car) {
            if (step_time > 0.0f || g_vphys.manifolds_dirty) {
                b.chassis_ground_contact = b.chassis_ground_contact_pass;
            }
            ep->ground_material = (wheels_all_airborne && !b.chassis_ground_contact) ? -1 : 0;
        }

        // The correction must reach BOTH transforms: the motion-state pose is built from the interp one.
        const rf::Vector3 correction = pos - pre_contact_pos;
        if (correction.len() > 0.0001f) {
            const btVector3 delta = to_bt(correction);
            btTransform t = body->getWorldTransform();
            t.setOrigin(t.getOrigin() + delta);
            body->setWorldTransform(t);
            btTransform ti = body->getInterpolationWorldTransform();
            ti.setOrigin(ti.getOrigin() + delta);
            body->setInterpolationWorldTransform(ti);
            interp.setOrigin(to_bt(pos));
            b.motion_state->setWorldTransform(interp);
        }
        // Same for the velocity: a 0-substep frame would otherwise extrapolate the PRE-contact one.
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
        for (BodyStepScratch& s : step) {
            vphys_body_post_step(s, step_time);
        }
        g_vphys.manifolds_dirty = false;
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

void vehicle_physics_level_init()
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
}
