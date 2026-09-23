#include <algorithm>
#include "vphys_internal.h"
#include "../vehicle_physics.h"
#include "../vehicle.h"
#include "../../../rf/ai.h"
#include "../../../rf/collide.h"
#include "../../../rf/entity.h"
#include "../../../rf/gameseq.h"
#include "../../../rf/multi.h"
#include "../../../rf/os/frametime.h"
#include "../../../rf/physics.h"
#include "../../../rf/player/camera.h"
#include "../../../rf/player/player.h"

float vphys_cam_distance(const VehiclePhysicsParams& p, const HullBox& box)
{
    return p.cam_dist > 0.0f ? p.cam_dist : 2.0f * box.half.z() + 2.0f;
}

float vphys_cam_height(const VehiclePhysicsParams& p, const HullBox& box)
{
    return p.cam_height > 0.0f ? p.cam_height : 1.3f * box.half.y() + 0.9f;
}

// Posed from AF's camera_do_frame hook (0x0040D850, misc/camera.cpp).
VehicleChaseCamera g_vcam;

namespace
{
    rf::Entity* vphys_chase_camera_target(int* out_class, rf::Entity** out_rider)
    {
        if (!rf::is_multi || rf::is_dedicated_server) {
            return nullptr;
        }
        // entity_die frees a rider's seat only at the end of the death animation, not at death.
        rf::Entity* vehicle = vehicle_fp_view_passenger_vehicle(out_rider);
        if (!vehicle) {
            return nullptr;
        }
        const int cls = vphys_class_for(vehicle);
        if (cls < 0 || params_for_class(cls).cam_enable == 0.0f) {
            return nullptr;
        }
        *out_class = cls;
        return vehicle;
    }

    void vphys_chase_camera_release(rf::Camera* camera)
    {
        if (!g_vcam.active) {
            return;
        }
        // 0x0040DDF0 returns 0 and writes NOTHING when the rider's handle is dead, so stamping the
        // mode ourselves would leave first person looking out of an unmoved chase pose.
        if (camera && camera->mode == rf::CAMERA_THIRD_PERSON
            && g_vcam.saved_mode != rf::CAMERA_THIRD_PERSON
            && !rf::camera_enter_first_person(camera)) {
            return; // still ours; retry next frame
        }
        g_vcam.active = false;
        g_vcam.vehicle_handle = -1;
        g_vcam.rider_handle = -1;
    }
} // namespace

bool vphys_chase_camera_do_frame(rf::Camera* camera)
{
    if (!rf::local_player || !camera || camera != rf::local_player->cam || !camera->camera_entity) {
        return false;
    }
    int cls = VPHYS_CLASS_JEEP;
    rf::Entity* rider = nullptr;
    rf::Entity* vehicle = vphys_chase_camera_target(&cls, &rider);
    if (!vehicle || !rider || rf::gameseq_get_state() != rf::GS_GAMEPLAY) {
        vphys_chase_camera_release(camera);
        return false;
    }
    if (g_vcam.active && camera->mode != rf::CAMERA_THIRD_PERSON) {
        g_vcam.active = false;
        g_vcam.vehicle_handle = -1;
        g_vcam.rider_handle = -1;
        return false;
    }

    const VehiclePhysicsParams& p = params_for_class(cls);
    const HullBox box = hull_local_box(vehicle);
    const float want_dist = vphys_cam_distance(p, box);
    const float height = vphys_cam_height(p, box);

    if (!g_vcam.active || g_vcam.vehicle_handle != vehicle->handle
        || g_vcam.rider_handle != rider->handle) {
        const rf::CameraMode prev_mode = camera->mode;
        // 0x0040DE80 sets the mode itself on success and refuses on a dead handle, having seeded
        // nothing; claiming the view anyway would pose a camera the engine never entered.
        if (prev_mode != rf::CAMERA_THIRD_PERSON && !rf::camera_enter_third_person(camera)) {
            return false;
        }
        if (!g_vcam.active) {
            g_vcam.saved_mode = prev_mode;
        }
        g_vcam.dist = want_dist;
        g_vcam.active = true;
        g_vcam.vehicle_handle = vehicle->handle;
        g_vcam.rider_handle = rider->handle;
    }

    // The rider's own eye frame: no second integrator here, or his view and a spectator's diverge.
    const float dt = std::max(rf::frametime, 0.0f);
    const rf::Vector3 look_dir = rider->eye_orient.fvec;

    // A seated rider is placed on his interface point each frame, so his pos IS the seat.
    rf::Vector3 focus = rider->pos;
    focus.y += height;

    rf::Vector3 probe_a = focus;
    rf::Vector3 probe_b = focus - look_dir * want_dist;
    rf::PCollisionOut probe{};
    float allowed = want_dist;
    const bool probe_hit = vphys_collide_solid_segment(probe_a, probe_b, probe);
    if (probe_hit) {
        allowed = std::clamp((probe.hit_point - focus).len() - std::max(p.cam_collide_margin, 0.0f),
                             0.0f, want_dist);
    }
    if (allowed < g_vcam.dist) {
        g_vcam.dist = std::max(allowed, g_vcam.dist - std::max(p.cam_pull_rate, 0.0f) * dt);
    }
    else {
        g_vcam.dist = std::min(allowed, g_vcam.dist + std::max(p.cam_extend_rate, 0.0f) * dt);
    }

    const rf::Vector3 cam_pos = focus - look_dir * g_vcam.dist;
    rf::Matrix3 orient;
    orient.make_quick(look_dir);

    rf::Entity* ce = camera->camera_entity;
    ce->pos = cam_pos;
    ce->orient = orient;
    ce->eye_pos = cam_pos;
    ce->eye_orient = orient;
    ce->set_room(nullptr);
    ce->update_room();
    return true;
}
