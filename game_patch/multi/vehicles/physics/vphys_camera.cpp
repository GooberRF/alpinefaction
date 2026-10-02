#include <algorithm>
#include <cmath>
#include <optional>
#include <string_view>
#include "vphys_internal.h"
#include "../vehicle_physics.h"
#include "../vehicle.h"
#include "../vehicle_internal.h"
#include "../../multi.h"
#include "../../../graphics/gr.h"
#include "../../../hud/multi_spectate.h"
#include "../../../input/mouse.h"
#include "../../../misc/alpine_settings.h"
#include "../../../os/console.h"
#include "../../../rf/ai.h"
#include "../../../rf/collide.h"
#include "../../../rf/entity.h"
#include "../../../rf/gameseq.h"
#include "../../../rf/multi.h"
#include "../../../rf/os/console.h"
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

// Posed from AF's camera_do_frame hook (0x0040D850).
VehicleChaseCamera g_vcam;

namespace
{
    constexpr float vcam_pitch_limit = 1.4f;          // ~80 degrees, as the spectate orbit
    constexpr float vcam_default_pitch = -0.12f;      // what drift settles to, relative to the reference
    constexpr float vcam_follow_pitch_limit = 0.52f;  // the most hull pitch the reference follows
    constexpr float vcam_min_heading_len = 0.2f;
    constexpr float vcam_heading_rate = 8.0f;         // rad/s outer bound on the reference heading
    constexpr float vcam_rigid_s = 0.001f;            // smoothing times below this are rigid
    constexpr float vcam_track_smooth_s = 0.12f;      // time constant of the hull velocity/yaw rate estimate
    constexpr float vcam_max_heading_step = 1.0f;     // rad in one frame: a flip through vertical, not a turn
    constexpr float vcam_velocity_ramp_floor = 1.0f;  // m/s: the travel-heading ramp's start when min speed is 0
    constexpr float vcam_velocity_yaw_cap = 0.7f;     // ~40 degrees of slide the heading follows
    constexpr float vcam_snap_dist = 3.0f;            // a focus jump beyond this plus vcam_snap_speed*dt
    constexpr float vcam_snap_speed = 60.0f;
    constexpr float vcam_lag_max_above = 1.5f;
    constexpr float vcam_lag_max_below = 0.75f;
    constexpr float vcam_lag_max_horizontal = 1.0f;
    constexpr float vcam_focus_probe_min = 0.01f;
    constexpr float vcam_focus_margin = 0.1f;
    constexpr float vcam_drift_delay_s = 2.5f;
    constexpr float vcam_drift_rate = 3.0f;           // 1/s: 95% of the way back after one second
    constexpr float vcam_aim_far = 1000.0f;
    constexpr float vcam_aim_min_ahead = 1.0f;        // past the hull's front half length
    constexpr float vcam_reticle_max_dist = 100.0f;   // from the camera: a detached reticle's cut once faded off P
    constexpr float vcam_reticle_project_dist = 10.0f;
    constexpr float vcam_aim_fade_band = 0.1745f;     // 10 degrees past an aim limit: convergence fully faded
    constexpr float apc_aim_yaw_cap = 0.2618f;        // 15 degrees either side of hull forward
    constexpr int vcam_ray_pass_through = 4;
    constexpr int vcam_probe_flags = 0;               // the boom passes invisible faces, which the view sees through

    rf::Vector3 dir_from_yaw_pitch(float yaw, float pitch)
    {
        const float cp = std::cos(pitch);
        return rf::Vector3{cp * std::sin(yaw), std::sin(pitch), cp * std::cos(yaw)};
    }

    // The heading and eye pitch physics_make_orient (0x004A0D70) turns into this forward vector: it
    // builds fvec from normalize((1 - |sin p|) sin h, sin p, (1 - |sin p|) cos h).
    void engine_angles_from_dir(const rf::Vector3& d, float* heading, float* pitch)
    {
        const float hlen = std::sqrt(d.x * d.x + d.z * d.z);
        *heading = std::atan2(d.x, d.z);
        const float denom = hlen + std::abs(d.y);
        *pitch = denom > 0.0f ? std::asin(std::clamp(d.y / denom, -1.0f, 1.0f)) : 0.0f;
    }

    // The hull-local pitch a first-person eye_phb.x produces, through the same body-relative rotation
    // vehicle_rebuild_eye_orient applies (0x004FD240), so no sign convention is assumed.
    float eye_pitch_to_local(float eye_pitch)
    {
        rf::Matrix3 m;
        m.make_identity();
        m.rotate_about_local_x(eye_pitch);
        return std::asin(std::clamp(m.fvec.y, -1.0f, 1.0f));
    }

    // The aim's weight on P: 1 up to an aim limit, falling to 0 `band` past it.
    float vcam_fade_weight(float excess, float band)
    {
        return excess > 0.0f ? 1.0f - std::min(excess / std::max(band, 0.001f), 1.0f) : 1.0f;
    }

    // Past an aim limit the convergence offset (gun origin -> P against the camera ray) fades out, so
    // the gun follows the camera's look and never P's distance, which jumps as the centre ray hits or misses.
    void vcam_fade_convergence(float w, float look_yaw, float look_pitch, float& yaw, float& pitch)
    {
        if (w < 1.0f) {
            yaw = vehicle_wrap_pi(look_yaw + w * vehicle_wrap_pi(yaw - look_yaw));
            pitch = look_pitch + w * (pitch - look_pitch);
        }
    }

    // The elevation of the forward vector physics_make_orient builds for eye pitch p.
    float engine_pitch_elevation(float p)
    {
        const float s = std::sin(p);
        return std::atan2(s, 1.0f - std::abs(s));
    }

    // The jeep gunner's aim weight on P, from how far the look's elevation is past his stock pitch
    // limits; upward the band is cut to the camera's remaining pitch so it still reaches 0.
    float vcam_gunner_aim_weight(const rf::Entity* rider)
    {
        const float e = std::asin(std::clamp(g_vcam.look.y, -1.0f, 1.0f));
        const float lo = engine_pitch_elevation(rider->min_rel_eye_phb.x);
        const float hi = engine_pitch_elevation(rider->max_rel_eye_phb.x);
        if (e < lo) {
            return vcam_fade_weight(lo - e, std::min(vcam_aim_fade_band, lo + vcam_pitch_limit));
        }
        return vcam_fade_weight(e - hi, std::min(vcam_aim_fade_band, vcam_pitch_limit - hi));
    }

    // How far past vcam_reticle_max_dist the reticle's cut reaches: out to P while the aim is on it.
    float vcam_reticle_reach(float w)
    {
        return w * std::max((g_vcam.aim_point - g_vcam.pos).len() - vcam_reticle_max_dist, 0.0f);
    }

    // A local seat whose view is the player's choice: jeep/APC/driller driver, jeep gunner.
    VehicleOrbitSeat vcam_local_choice_seat(bool require_setting)
    {
        if (!rf::is_multi || rf::is_dedicated_server || is_headless_mode()
            || multi_spectate_is_spectating()) {
            return VehicleOrbitSeat::none;
        }
        rf::Entity* rider = rf::local_player_entity;
        if (!rider || rf::entity_is_dying(rider)) {
            return VehicleOrbitSeat::none;
        }
        rf::Entity* vehicle = vehicle_ridden_live_hull(rider);
        const int cls = vehicle ? vphys_class_for(vehicle) : -1;
        if (cls < 0 || params_for_class(cls).cam_enable == 0.0f) {
            return VehicleOrbitSeat::none;
        }
        if (vehicle_driver_entity(vehicle) == rider) {
            // A mouse-steered class would lose its steering to the orbit.
            if (!vphys_class_is_automobile(cls) || params_for_class(cls).steer_from_mouse != 0.0f) {
                return VehicleOrbitSeat::none;
            }
            return !require_setting || g_alpine_game_config.vehicle_driver_third_person
                ? VehicleOrbitSeat::driver : VehicleOrbitSeat::none;
        }
        if (rf::entity_is_jeep_gunner(rider)) {
            return !require_setting || g_alpine_game_config.vehicle_gunner_third_person
                ? VehicleOrbitSeat::gunner : VehicleOrbitSeat::none;
        }
        return VehicleOrbitSeat::none;
    }

    struct VcamTarget
    {
        rf::Entity* vehicle = nullptr;
        rf::Entity* rider = nullptr;
        int cls = -1;
        VehicleOrbitSeat seat = VehicleOrbitSeat::none;
        bool local = false;
    };

    bool vphys_chase_camera_target(VcamTarget& t)
    {
        if (!rf::is_multi || rf::is_dedicated_server) {
            return false;
        }
        // entity_die frees a rider's seat only at the end of the death animation, not at death.
        rf::Entity* rider = nullptr;
        if (rf::Entity* vehicle = vehicle_fp_view_passenger_vehicle(&rider)) {
            const int cls = vphys_class_for(vehicle);
            if (cls < 0 || params_for_class(cls).cam_enable == 0.0f || !rider) {
                return false;
            }
            // Headless bots aim by writing their own angles, so a bot passenger keeps the legacy view.
            const bool local = rider == rf::local_player_entity && !multi_spectate_is_spectating()
                && !is_headless_mode();
            t = VcamTarget{vehicle, rider, cls, VehicleOrbitSeat::passenger, local};
            return true;
        }
        const VehicleOrbitSeat seat = vcam_local_choice_seat(true);
        if (seat == VehicleOrbitSeat::none) {
            return false;
        }
        rf::Entity* vehicle = vehicle_ridden_live_hull(rf::local_player_entity);
        t = VcamTarget{vehicle, rf::local_player_entity, vphys_class_for(vehicle), seat, true};
        return true;
    }

    // to_first_person: still seated and toggled to first person, whatever the view was on foot.
    void vphys_chase_camera_release(rf::Camera* camera, bool to_first_person)
    {
        if (!g_vcam.active) {
            return;
        }
        // 0x0040DDF0 returns 0 and writes NOTHING when the rider's handle is dead, so stamping the
        // mode ourselves would leave first person looking out of an unmoved chase pose.
        if (camera && camera->mode == rf::CAMERA_THIRD_PERSON
            && (to_first_person || g_vcam.saved_mode != rf::CAMERA_THIRD_PERSON)
            && !rf::camera_enter_first_person(camera)) {
            return; // still ours; retry next frame
        }
        g_vcam = VehicleChaseCamera{};
    }

    void vcam_clamp_rel_pitch()
    {
        g_vcam.rel_pitch = std::clamp(g_vcam.rel_pitch, -vcam_pitch_limit - g_vcam.ref_pitch,
                                      vcam_pitch_limit - g_vcam.ref_pitch);
    }

    // Critically damped spring, exact for a constant target over any dt. Returns the new offset from
    // the target; a non-finite result collapses onto the target.
    float vcam_spring_step(float delta, float& vel, float smooth_s, float dt)
    {
        if (!(smooth_s >= vcam_rigid_s) || !std::isfinite(delta) || !std::isfinite(vel)) {
            vel = 0.0f;
            return 0.0f;
        }
        const float w = 2.0f / smooth_s;
        const float temp = (vel + w * delta) * dt;
        const float e = std::exp(-w * dt);
        const float next_vel = (vel - w * temp) * e;
        const float next = (delta + temp) * e;
        if (!std::isfinite(next) || !std::isfinite(next_vel)) {
            vel = 0.0f;
            return 0.0f;
        }
        vel = next_vel;
        return next;
    }

    void vcam_spring(float& x, float& vel, float target, float smooth_s, float dt)
    {
        if (std::isfinite(target)) {
            x = target + vcam_spring_step(x - target, vel, smooth_s, dt);
        }
    }

    void vcam_spring_angle(float& x, float& vel, float target, float smooth_s, float dt)
    {
        if (std::isfinite(target)) {
            x = vehicle_wrap_pi(target + vcam_spring_step(vehicle_wrap_pi(x - target), vel, smooth_s, dt));
        }
    }

    // The hull's velocity and yaw rate from its pose each frame, so a watched hull (whose p_data.vel
    // this machine does not own) is measured exactly as the one this machine drives.
    void vcam_track_hull(const rf::Entity* vehicle, float dt, bool snap)
    {
        const rf::Vector3& f = vehicle->orient.fvec;
        const bool has_heading = std::sqrt(f.x * f.x + f.z * f.z) >= vcam_min_heading_len;
        const float heading = has_heading ? std::atan2(f.x, f.z) : g_vcam.hull_yaw;
        if (!std::isfinite(heading)) {
            return;
        }
        if (snap) {
            g_vcam.hull_vel = rf::Vector3{};
            g_vcam.hull_yaw_rate = 0.0f;
        }
        else if (dt > 0.0f) {
            const float k = 1.0f - std::exp(-dt / vcam_track_smooth_s);
            const rf::Vector3 raw = (vehicle->pos - g_vcam.hull_pos) * (1.0f / dt);
            g_vcam.hull_vel += (raw - g_vcam.hull_vel) * k;
            const float step = has_heading ? vehicle_wrap_pi(heading - g_vcam.hull_yaw) : 0.0f;
            const float rate = std::abs(step) <= vcam_max_heading_step ? step / dt : 0.0f;
            g_vcam.hull_yaw_rate += (rate - g_vcam.hull_yaw_rate) * k;
            if (!vehicle_vector_is_finite(g_vcam.hull_vel) || !std::isfinite(g_vcam.hull_yaw_rate)) {
                g_vcam.hull_vel = rf::Vector3{};
                g_vcam.hull_yaw_rate = 0.0f;
            }
        }
        g_vcam.hull_pos = vehicle->pos;
        g_vcam.hull_yaw = heading;
    }

    // The heading is held while slow, reversing or near vertical; pitch follows only a share of the hull's.
    void vcam_update_reference(const rf::Entity* vehicle, float dt, bool snap)
    {
        vcam_track_hull(vehicle, dt, snap);
        const AlpineGameSettings& cfg = g_alpine_game_config;
        const rf::Vector3& f = vehicle->orient.fvec;
        const float hlen = std::sqrt(f.x * f.x + f.z * f.z);
        float lookahead = 0.0f;
        if (hlen >= vcam_min_heading_len) {
            const rf::Vector3& v = g_vcam.hull_vel;
            const float speed = std::sqrt(v.x * v.x + v.z * v.z);
            const float fwd = (v.x * f.x + v.z * f.z) / hlen;
            const float min_speed = cfg.vehicle_cam_min_speed;
            if (snap || min_speed <= 0.0f || (speed >= min_speed && fwd > 0.0f)) {
                float target = g_vcam.hull_yaw;
                if (fwd > 0.0f) {
                    // One speed ramp fades both in from the hold threshold, so neither steps there.
                    const float ramp = std::max(min_speed, vcam_velocity_ramp_floor);
                    const float fade = std::clamp((speed - ramp) / (2.0f * ramp), 0.0f, 1.0f);
                    const float slip = std::clamp(vehicle_wrap_pi(std::atan2(v.x, v.z) - target),
                                                  -vcam_velocity_yaw_cap, vcam_velocity_yaw_cap);
                    target += slip * fade * cfg.vehicle_cam_velocity;
                    const float max_ahead = cfg.vehicle_cam_lookahead_max * vehicle_deg_to_rad;
                    lookahead = fade * std::clamp(g_vcam.hull_yaw_rate * cfg.vehicle_cam_lookahead,
                                                  -max_ahead, max_ahead);
                }
                if (std::isfinite(target) && std::isfinite(lookahead)) {
                    g_vcam.target_yaw = vehicle_wrap_pi(target);
                }
                else {
                    lookahead = 0.0f;
                }
            }
        }
        const float yaw_target = vehicle_wrap_pi(g_vcam.target_yaw + lookahead);
        const float pitch_target = std::clamp(std::asin(std::clamp(f.y, -1.0f, 1.0f)) * cfg.vehicle_cam_pitch_follow,
                                              -vcam_follow_pitch_limit, vcam_follow_pitch_limit);
        if (snap || !std::isfinite(g_vcam.ref_yaw) || !std::isfinite(g_vcam.ref_pitch)) {
            g_vcam.ref_yaw = std::isfinite(yaw_target) ? yaw_target : 0.0f;
            g_vcam.ref_pitch = std::isfinite(pitch_target) ? pitch_target : 0.0f;
            g_vcam.ref_yaw_vel = 0.0f;
            g_vcam.ref_pitch_vel = 0.0f;
            return;
        }
        // The spring's own peak rate stays under the bound at the default follow; the bound is what
        // keeps a rigid (0) follow from turning half round in one frame when a hold releases.
        const float prev_yaw = g_vcam.ref_yaw;
        vcam_spring_angle(g_vcam.ref_yaw, g_vcam.ref_yaw_vel, yaw_target, cfg.vehicle_cam_follow, dt);
        const float max_step = vcam_heading_rate * dt;
        const float step = vehicle_wrap_pi(g_vcam.ref_yaw - prev_yaw);
        if (dt > 0.0f && std::abs(step) > max_step) {
            g_vcam.ref_yaw = vehicle_wrap_pi(prev_yaw + std::copysign(max_step, step));
            g_vcam.ref_yaw_vel = std::copysign(vcam_heading_rate, step);
        }
        vcam_spring(g_vcam.ref_pitch, g_vcam.ref_pitch_vel, pitch_target, cfg.vehicle_cam_pitch_smooth, dt);
        g_vcam.ref_pitch = std::clamp(g_vcam.ref_pitch, -vcam_follow_pitch_limit, vcam_follow_pitch_limit);
    }

    // Vertical lag is held tighter below the seat than above it, and the lagged point is pulled back
    // to the rider's side of any surface, so the camera probe never starts inside the ground or a wall.
    rf::Vector3 vcam_update_focus(const rf::Vector3& target, float dt, bool snap)
    {
        const AlpineGameSettings& cfg = g_alpine_game_config;
        rf::Vector3& p = g_vcam.focus;
        rf::Vector3& v = g_vcam.focus_vel;
        if (!vehicle_vector_is_finite(target)) {
            return vehicle_vector_is_finite(p) ? p : target;
        }
        if (snap || !vehicle_vector_is_finite(p) || !vehicle_vector_is_finite(v)) {
            p = target;
            v = rf::Vector3{};
            return p;
        }
        vcam_spring(p.x, v.x, target.x, cfg.vehicle_cam_lag, dt);
        vcam_spring(p.z, v.z, target.z, cfg.vehicle_cam_lag, dt);
        vcam_spring(p.y, v.y, target.y, cfg.vehicle_cam_bounce, dt);

        const float dy = p.y - target.y;
        if (dy > vcam_lag_max_above) {
            p.y = target.y + vcam_lag_max_above;
            v.y = std::min(v.y, 0.0f);
        }
        else if (dy < -vcam_lag_max_below) {
            p.y = target.y - vcam_lag_max_below;
            v.y = std::max(v.y, 0.0f);
        }
        const float ox = p.x - target.x;
        const float oz = p.z - target.z;
        const float olen = std::sqrt(ox * ox + oz * oz);
        if (olen > vcam_lag_max_horizontal) {
            const float nx = ox / olen;
            const float nz = oz / olen;
            p.x = target.x + nx * vcam_lag_max_horizontal;
            p.z = target.z + nz * vcam_lag_max_horizontal;
            const float out = v.x * nx + v.z * nz;
            if (out > 0.0f) {
                v.x -= nx * out;
                v.z -= nz * out;
            }
        }

        const rf::Vector3 lag = p - target;
        const float lag_len = lag.len();
        if (lag_len > vcam_focus_probe_min) {
            rf::PCollisionOut hit{};
            if (vphys_collide_solid_segment(target, p, vcam_probe_flags, hit)) {
                const float keep = std::max((hit.hit_point - target).len() - vcam_focus_margin, 0.0f);
                p = target + lag * (std::min(keep, lag_len) / lag_len);
                v = rf::Vector3{};
            }
        }
        return p;
    }

    // `to`, or `margin` short of the first solid hit on the way from `from`. The segment test is
    // one-sided, so `from` must already be in open space.
    rf::Vector3 vcam_clear_point(const rf::Vector3& from, const rf::Vector3& to, float margin)
    {
        rf::PCollisionOut hit{};
        if (!vphys_collide_solid_segment(from, to, vcam_probe_flags, hit)) {
            return to;
        }
        const rf::Vector3 seg = to - from;
        const float len = seg.len();
        const float keep = std::max((hit.hit_point - from).len() - margin, 0.0f);
        return from + seg * (std::min(keep, len) / len);
    }

    void vcam_seed(const rf::Matrix3& view, const rf::Entity* vehicle)
    {
        const rf::Vector3& f = view.fvec;
        // What a hull with no usable heading (near vertical) holds until it has one again.
        const float view_yaw = std::atan2(f.x, f.z);
        g_vcam.hull_yaw = std::isfinite(view_yaw) ? view_yaw : 0.0f;
        g_vcam.target_yaw = g_vcam.hull_yaw;
        vcam_update_reference(vehicle, 0.0f, true);
        g_vcam.rel_yaw = vehicle_wrap_pi(std::atan2(f.x, f.z) - g_vcam.ref_yaw);
        g_vcam.rel_pitch = std::asin(std::clamp(f.y, -1.0f, 1.0f)) - g_vcam.ref_pitch;
        vcam_clamp_rel_pitch();
        g_vcam.idle_s = 0.0f;
    }

    void vcam_add_look(float pitch, float yaw)
    {
        if (pitch == 0.0f && yaw == 0.0f) {
            return;
        }
        g_vcam.rel_yaw = vehicle_wrap_pi(g_vcam.rel_yaw + yaw);
        g_vcam.rel_pitch += pitch;
        vcam_clamp_rel_pitch();
        g_vcam.idle_s = 0.0f;
    }

    rf::Vector3 vcam_look_dir()
    {
        const float pitch = std::clamp(g_vcam.ref_pitch + g_vcam.rel_pitch, -vcam_pitch_limit, vcam_pitch_limit);
        return dir_from_yaw_pitch(g_vcam.ref_yaw + g_vcam.rel_yaw, pitch);
    }

    // Nearest hit that is not this hull, one of its riders or the local player. 0x0049C690 takes two
    // objects to skip, so the other riders are stepped past one at a time.
    bool vcam_raycast(const rf::Vector3& from, const rf::Vector3& to, const rf::Entity* vehicle,
                      rf::Vector3* out_hit)
    {
        const rf::Vector3 seg = to - from;
        const float total = seg.len();
        if (total < 0.0001f) {
            return false;
        }
        const rf::Vector3 dir = seg * (1.0f / total);
        rf::Vector3 start = from;
        for (int i = 0; i <= vcam_ray_pass_through; ++i) {
            rf::Vector3 p0 = start;
            rf::Vector3 p1 = to;
            rf::LevelCollisionOut col{};
            col.obj_handle = -1;
            col.face = nullptr;
            if (!rf::collide_linesegment_level_for_multi(p0, p1, const_cast<rf::Entity*>(vehicle),
                                                          rf::local_player_entity, &col, 0.1f, false,
                                                          1.0f)) {
                return false;
            }
            const rf::Object* op = col.obj_handle >= 0 ? rf::obj_from_handle(col.obj_handle) : nullptr;
            if (!op || op->type != rf::OT_ENTITY || op->host_handle != vehicle->handle) {
                *out_hit = col.hit_point;
                return true;
            }
            start = col.hit_point + dir * 0.05f;
            if ((start - from).dot_prod(dir) >= total) {
                return false;
            }
        }
        return false;
    }

    void vcam_update_aim_point(const rf::Entity* vehicle, const rf::Vector3& focus)
    {
        const bool aims = g_vcam.seat == VehicleOrbitSeat::gunner
            || (g_vcam.seat == VehicleOrbitSeat::driver && g_vcam.cls == VPHYS_CLASS_APC);
        if (!aims) {
            g_vcam.aim_valid = false;
            return;
        }
        const rf::Vector3& from = g_vcam.pos;
        const rf::Vector3& look = g_vcam.look;
        const float min_t = std::max((focus - from).dot_prod(look), 0.0f)
            + hull_local_box(vehicle).half.z() + vcam_aim_min_ahead;
        float t = vcam_aim_far;
        rf::Vector3 hit{};
        if (vcam_raycast(from, from + look * vcam_aim_far, vehicle, &hit)) {
            t = (hit - from).dot_prod(look);
        }
        g_vcam.aim_point = from + look * std::max(t, min_t);
        g_vcam.aim_valid = true;
    }

    // Toward the camera's aim point from the minigun's origin (the driver's eye); the rockets fire
    // parallel. Capped to the hull: +-15 degrees of yaw and the pitch first person allows; past the cap
    // it follows the camera's look.
    bool vcam_apc_aim(const rf::Entity* vehicle, const rf::Entity* driver, rf::Vector3& out_dir,
                      bool& out_capped, float& out_weight)
    {
        if (!g_vcam.active || !g_vcam.orbit || !g_vcam.aim_valid
            || g_vcam.seat != VehicleOrbitSeat::driver || g_vcam.cls != VPHYS_CLASS_APC
            || !vehicle || !driver || vehicle->handle != g_vcam.vehicle_handle
            || driver->handle != g_vcam.rider_handle || driver != rf::local_player_entity) {
            return false;
        }
        const rf::Matrix3& hull = vehicle->orient;
        const auto hull_angles = [&hull](const rf::Vector3& v, float& yaw, float& pitch) {
            yaw = std::atan2(v.dot_prod(hull.rvec), v.dot_prod(hull.fvec));
            pitch = std::asin(std::clamp(v.dot_prod(hull.uvec), -1.0f, 1.0f));
        };
        rf::Vector3 d = g_vcam.aim_point - driver->eye_pos;
        const float len = d.len();
        d = len > 0.01f ? d * (1.0f / len) : hull.fvec;
        float yaw = 0.0f;
        float pitch = 0.0f;
        hull_angles(d, yaw, pitch);
        float look_yaw = 0.0f;
        float look_pitch = 0.0f;
        hull_angles(g_vcam.look, look_yaw, look_pitch);
        const float a = eye_pitch_to_local(vehicle->min_rel_eye_phb.x);
        const float b = eye_pitch_to_local(vehicle->max_rel_eye_phb.x);
        const float min_pitch = std::min(a, b);
        const float max_pitch = std::max(a, b);
        const float excess = std::max({std::abs(look_yaw) - apc_aim_yaw_cap, min_pitch - look_pitch,
                                       look_pitch - max_pitch, 0.0f});
        out_weight = vcam_fade_weight(excess, vcam_aim_fade_band);
        vcam_fade_convergence(out_weight, look_yaw, look_pitch, yaw, pitch);
        const float capped_yaw = std::clamp(yaw, -apc_aim_yaw_cap, apc_aim_yaw_cap);
        const float capped_pitch = std::clamp(pitch, min_pitch, max_pitch);
        const rf::Vector3 local = dir_from_yaw_pitch(capped_yaw, capped_pitch);
        out_dir = hull.rvec * local.x + hull.uvec * local.y + hull.fvec * local.z;
        out_capped = excess > 0.0f || capped_yaw != yaw || capped_pitch != pitch;
        return true;
    }

    void vcam_setting_cmd(std::optional<float> value, void (AlpineGameSettings::*setter)(float),
                          float AlpineGameSettings::*field, float def, std::string_view what,
                          std::string_view unit)
    {
        if (value) {
            (g_alpine_game_config.*setter)(*value);
        }
        rf::console::print("{} is {:.2f}{} (default {:.2f})", what, g_alpine_game_config.*field, unit, def);
    }

    ConsoleCommand2 vehiclecam_follow_cmd{
        "cl_vehiclecam_follow",
        [](std::optional<float> value) {
            vcam_setting_cmd(value, &AlpineGameSettings::set_vehicle_cam_follow,
                             &AlpineGameSettings::vehicle_cam_follow,
                             AlpineGameSettings::default_vehicle_cam_follow,
                             "Vehicle camera heading smoothing", " s (0 = rigid)");
        },
        "Set how long the third-person vehicle camera takes to swing round behind the vehicle",
        "cl_vehiclecam_follow [0.0-2.0]",
    };

    ConsoleCommand2 vehiclecam_velocity_cmd{
        "cl_vehiclecam_velocity",
        [](std::optional<float> value) {
            vcam_setting_cmd(value, &AlpineGameSettings::set_vehicle_cam_velocity,
                             &AlpineGameSettings::vehicle_cam_velocity,
                             AlpineGameSettings::default_vehicle_cam_velocity,
                             "Vehicle camera travel-direction follow", " (0 = hull heading only)");
        },
        "Set how far the vehicle camera turns toward a land vehicle's direction of travel at speed",
        "cl_vehiclecam_velocity [0.0-1.0]",
    };

    ConsoleCommand2 vehiclecam_min_speed_cmd{
        "cl_vehiclecam_min_speed",
        [](std::optional<float> value) {
            vcam_setting_cmd(value, &AlpineGameSettings::set_vehicle_cam_min_speed,
                             &AlpineGameSettings::vehicle_cam_min_speed,
                             AlpineGameSettings::default_vehicle_cam_min_speed,
                             "Vehicle camera minimum follow speed", " m/s (0 = never hold)");
        },
        "Set the speed below which, or while reversing, the vehicle camera holds its heading",
        "cl_vehiclecam_min_speed [0.0-10.0]",
    };

    ConsoleCommand2 vehiclecam_lookahead_cmd{
        "cl_vehiclecam_lookahead",
        [](std::optional<float> value) {
            vcam_setting_cmd(value, &AlpineGameSettings::set_vehicle_cam_lookahead,
                             &AlpineGameSettings::vehicle_cam_lookahead,
                             AlpineGameSettings::default_vehicle_cam_lookahead,
                             "Vehicle camera turn look-ahead", " s of turn rate (0 = off)");
        },
        "Set how far the vehicle camera looks into a turn, in seconds of the vehicle's turn rate",
        "cl_vehiclecam_lookahead [0.0-1.0]",
    };

    ConsoleCommand2 vehiclecam_lookahead_max_cmd{
        "cl_vehiclecam_lookahead_max",
        [](std::optional<float> value) {
            vcam_setting_cmd(value, &AlpineGameSettings::set_vehicle_cam_lookahead_max,
                             &AlpineGameSettings::vehicle_cam_lookahead_max,
                             AlpineGameSettings::default_vehicle_cam_lookahead_max,
                             "Vehicle camera look-ahead limit", " degrees");
        },
        "Set the most the vehicle camera looks into a turn, in degrees",
        "cl_vehiclecam_lookahead_max [0-30]",
    };

    ConsoleCommand2 vehiclecam_pitch_follow_cmd{
        "cl_vehiclecam_pitch_follow",
        [](std::optional<float> value) {
            vcam_setting_cmd(value, &AlpineGameSettings::set_vehicle_cam_pitch_follow,
                             &AlpineGameSettings::vehicle_cam_pitch_follow,
                             AlpineGameSettings::default_vehicle_cam_pitch_follow,
                             "Vehicle camera pitch follow", " (share of the vehicle's pitch, 0 = level)");
        },
        "Set how much of the vehicle's pitch the vehicle camera tilts with",
        "cl_vehiclecam_pitch_follow [0.0-1.0]",
    };

    ConsoleCommand2 vehiclecam_pitch_smooth_cmd{
        "cl_vehiclecam_pitch_smooth",
        [](std::optional<float> value) {
            vcam_setting_cmd(value, &AlpineGameSettings::set_vehicle_cam_pitch_smooth,
                             &AlpineGameSettings::vehicle_cam_pitch_smooth,
                             AlpineGameSettings::default_vehicle_cam_pitch_smooth,
                             "Vehicle camera pitch smoothing", " s (0 = rigid)");
        },
        "Set how slowly the vehicle camera follows the vehicle's pitch",
        "cl_vehiclecam_pitch_smooth [0.0-3.0]",
    };

    ConsoleCommand2 vehiclecam_bounce_cmd{
        "cl_vehiclecam_bounce",
        [](std::optional<float> value) {
            vcam_setting_cmd(value, &AlpineGameSettings::set_vehicle_cam_bounce,
                             &AlpineGameSettings::vehicle_cam_bounce,
                             AlpineGameSettings::default_vehicle_cam_bounce,
                             "Vehicle camera vertical smoothing", " s (0 = rigid)");
        },
        "Set how much the vehicle camera smooths out the vehicle's bouncing",
        "cl_vehiclecam_bounce [0.0-1.0]",
    };

    ConsoleCommand2 vehiclecam_lag_cmd{
        "cl_vehiclecam_lag",
        [](std::optional<float> value) {
            vcam_setting_cmd(value, &AlpineGameSettings::set_vehicle_cam_lag,
                             &AlpineGameSettings::vehicle_cam_lag,
                             AlpineGameSettings::default_vehicle_cam_lag,
                             "Vehicle camera horizontal smoothing", " s (0 = rigid)");
        },
        "Set how much the vehicle camera smooths out the vehicle's sideways and fore-aft jolts",
        "cl_vehiclecam_lag [0.0-0.5]",
    };
} // namespace

bool vphys_chase_camera_do_frame(rf::Camera* camera)
{
    if (!rf::local_player || !camera || camera != rf::local_player->cam || !camera->camera_entity) {
        return false;
    }
    VcamTarget t{};
    const bool targeted = vphys_chase_camera_target(t);
    if (!targeted || rf::gameseq_get_state() != rf::GS_GAMEPLAY) {
        if (g_vcam.active) {
            vphys_chase_camera_release(camera, !targeted && vcam_local_choice_seat(false) != VehicleOrbitSeat::none);
        }
        return false;
    }
    if (g_vcam.active && camera->mode != rf::CAMERA_THIRD_PERSON) {
        g_vcam = VehicleChaseCamera{};
        return false;
    }
    rf::Entity* vehicle = t.vehicle;
    rf::Entity* rider = t.rider;

    const VehiclePhysicsParams& p = params_for_class(t.cls);
    const HullBox box = hull_local_box(vehicle);
    const float want_dist = vphys_cam_distance(p, box);
    const float height = vphys_cam_height(p, box);
    const float dt = std::isfinite(rf::frametime) ? std::max(rf::frametime, 0.0f) : 0.0f;

    const bool fresh = !g_vcam.active || g_vcam.vehicle_handle != vehicle->handle
        || g_vcam.rider_handle != rider->handle || g_vcam.orbit != t.local;
    if (fresh) {
        // Read before the mode change: the orbit starts from what is on screen, so nothing snaps.
        const rf::Matrix3 view = rf::camera_get_orient(camera);
        const rf::CameraMode prev_mode = camera->mode;
        // 0x0040DE80 sets the mode itself on success and refuses on a dead handle, having seeded
        // nothing; claiming the view anyway would pose a camera the engine never entered.
        if (prev_mode != rf::CAMERA_THIRD_PERSON && !rf::camera_enter_third_person(camera)) {
            return false;
        }
        const rf::CameraMode saved_mode = g_vcam.active ? g_vcam.saved_mode : prev_mode;
        g_vcam = VehicleChaseCamera{};
        g_vcam.saved_mode = saved_mode;
        g_vcam.dist = want_dist;
        g_vcam.active = true;
        g_vcam.vehicle_handle = vehicle->handle;
        g_vcam.rider_handle = rider->handle;
        g_vcam.orbit = t.local;
        if (t.local) {
            vcam_seed(view, vehicle);
        }
    }
    if (g_vcam.seat != t.seat) {
        const VehicleChaseCamera reset{};
        g_vcam.gun_weight = reset.gun_weight;
        g_vcam.gun_reach = reset.gun_reach;
        g_vcam.focus_lift = reset.focus_lift;
    }
    else if (g_vcam.orbit && !fresh && rider->host_tag_handle != g_vcam.seat_tag) {
        g_vcam.focus_lift = VehicleChaseCamera{}.focus_lift;
    }
    g_vcam.seat = t.seat;
    g_vcam.cls = t.cls;

    // A seated rider is placed on his interface point each frame, so his pos IS the seat. It is
    // reached from the hull origin, which the chassis keeps out of solid; a seat tag need not be.
    const float margin = std::max(p.cam_collide_margin, 0.0f);
    const rf::Vector3 seat = vcam_clear_point(vehicle->pos, rider->pos, margin);
    // Any lift up to the clip is open space; easing it back up keeps a roof's end from popping the view.
    const float clip_lift = vcam_clear_point(seat, seat + rf::Vector3{0.0f, height, 0.0f}, margin).y - seat.y;
    g_vcam.focus_lift = std::min(clip_lift, g_vcam.focus_lift + std::max(p.cam_extend_rate, 0.0f) * dt);
    rf::Vector3 focus = seat + rf::Vector3{0.0f, g_vcam.focus_lift, 0.0f};
    // A teleport moves the hull further in one frame than any class can drive. Measured on the hull,
    // not the seat, so a seat swap (up to ~4 m on the APC) is not mistaken for one.
    const bool jumped = g_vcam.orbit && !fresh
        && !((vehicle->pos - g_vcam.hull_pos).len() <= vcam_snap_dist + vcam_snap_speed * dt);

    rf::Vector3 look_dir;
    if (g_vcam.orbit) {
        const bool gunner = g_vcam.seat == VehicleOrbitSeat::gunner;
        if (!fresh) {
            const float prev_ref_yaw = g_vcam.ref_yaw;
            const float prev_ref_pitch = g_vcam.ref_pitch;
            vcam_update_reference(vehicle, dt, jumped);
            if (gunner) {
                // His view is his aim: it holds its world direction as the hull turns, as in first person.
                g_vcam.rel_yaw = vehicle_wrap_pi(g_vcam.rel_yaw - vehicle_wrap_pi(g_vcam.ref_yaw - prev_ref_yaw));
                g_vcam.rel_pitch -= g_vcam.ref_pitch - prev_ref_pitch;
            }
        }
        if (g_vcam.seat == VehicleOrbitSeat::driver) {
            float pitch = 0.0f;
            float yaw = 0.0f;
            consume_vehicle_orbit_mouse_deltas(pitch, yaw);
            // His look keys land in the hull's ControlInfo; scaled as 0x0049DE50 scales a passenger's.
            const float key_scale = rider->info->rot_acceleration * dt;
            vcam_add_look(pitch + vehicle->ai.ci.rot.x * key_scale, yaw + vehicle->ai.ci.rot.y * key_scale);
        }
        g_vcam.idle_s += dt;
        if (!gunner && g_vcam.idle_s >= vcam_drift_delay_s) {
            const float k = 1.0f - std::exp(-vcam_drift_rate * dt);
            g_vcam.rel_yaw -= g_vcam.rel_yaw * k;
            g_vcam.rel_pitch += (vcam_default_pitch - g_vcam.rel_pitch) * k;
        }
        vcam_clamp_rel_pitch();
        look_dir = vcam_look_dir();

        const bool snap_focus = fresh || jumped || rider->host_tag_handle != g_vcam.seat_tag;
        g_vcam.seat_tag = rider->host_tag_handle;
        focus = vcam_update_focus(focus, dt, snap_focus);
    }
    else {
        // A spectated passenger: his own eye frame, which his orbit steers on his machine.
        look_dir = rider->eye_orient.fvec;
    }

    rf::Vector3 probe_a = focus;
    rf::Vector3 probe_b = focus - look_dir * (want_dist + margin);
    rf::PCollisionOut probe{};
    float allowed = want_dist;
    const bool probe_hit = vphys_collide_solid_segment(probe_a, probe_b, vcam_probe_flags, probe);
    if (probe_hit) {
        allowed = std::clamp((probe.hit_point - focus).len() - margin, 0.0f, want_dist);
    }
    if (allowed < g_vcam.dist) {
        g_vcam.dist = allowed;
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

    g_vcam.pos = cam_pos;
    g_vcam.look = look_dir;
    if (g_vcam.orbit) {
        vcam_update_aim_point(vehicle, focus);
    }
    return true;
}

bool vehicle_physics_camera_toggle_view()
{
    switch (vcam_local_choice_seat(false)) {
        case VehicleOrbitSeat::driver:
            g_alpine_game_config.vehicle_driver_third_person = !g_alpine_game_config.vehicle_driver_third_person;
            return true;
        case VehicleOrbitSeat::gunner:
            g_alpine_game_config.vehicle_gunner_third_person = !g_alpine_game_config.vehicle_gunner_third_person;
            return true;
        default:
            return false;
    }
}

bool vehicle_physics_camera_local_seat_is_orbit()
{
    rf::Entity* rider = rf::local_player_entity;
    if (rf::Entity* vehicle = vehicle_passenger_vehicle(rider)) {
        const int cls = vphys_class_for(vehicle);
        return rf::is_multi && cls >= 0 && params_for_class(cls).cam_enable != 0.0f;
    }
    return vcam_local_choice_seat(true) != VehicleOrbitSeat::none;
}

bool vehicle_physics_camera_owns_driver_look()
{
    return g_vcam.active && g_vcam.orbit && g_vcam.seat == VehicleOrbitSeat::driver
        && rf::local_player_entity && g_vcam.rider_handle == rf::local_player_entity->handle
        && rf::local_player_entity->host_handle == g_vcam.vehicle_handle;
}

bool vehicle_physics_camera_take_rider_look(rf::Entity* ep, float& pitch_delta, float& yaw_delta)
{
    if (!ep || ep != rf::local_player_entity || !g_vcam.active || !g_vcam.orbit
        || g_vcam.rider_handle != ep->handle || ep->host_handle != g_vcam.vehicle_handle
        || (g_vcam.seat != VehicleOrbitSeat::passenger && g_vcam.seat != VehicleOrbitSeat::gunner)) {
        return false;
    }
    vcam_add_look(pitch_delta, yaw_delta);
    pitch_delta = 0.0f;
    yaw_delta = 0.0f;

    // The gunner's eye ray (his muzzle is eye + 0.2 fvec) goes through the aim point; a passenger's
    // eye looks where his camera looks, so his body and a spectator's view of him agree with it.
    rf::Vector3 dir;
    if (g_vcam.seat == VehicleOrbitSeat::gunner) {
        if (!g_vcam.aim_valid) {
            return true;
        }
        dir = g_vcam.aim_point - ep->eye_pos;
        const float len = dir.len();
        if (len < 0.01f) {
            return true;
        }
        dir *= 1.0f / len;
    }
    else {
        dir = vcam_look_dir();
    }
    float heading = 0.0f;
    float pitch = 0.0f;
    engine_angles_from_dir(dir, &heading, &pitch);
    if (g_vcam.seat == VehicleOrbitSeat::gunner) {
        float look_heading = 0.0f;
        float look_pitch = 0.0f;
        engine_angles_from_dir(g_vcam.look, &look_heading, &look_pitch);
        g_vcam.gun_weight = vcam_gunner_aim_weight(ep);
        g_vcam.gun_reach = vcam_reticle_reach(g_vcam.gun_weight);
        vcam_fade_convergence(g_vcam.gun_weight, look_heading, look_pitch, heading, pitch);
    }
    // 0x0049DE50 zeroes phb.x/z before building the eye from phb + eye_phb, so eye_phb.x is all of it.
    const rf::EntityControlData& cd = ep->control_data;
    yaw_delta = vehicle_wrap_pi(heading - cd.phb.y);
    pitch_delta = pitch - cd.eye_phb.x;
    return true;
}

bool vehicle_physics_camera_driver_aim(const rf::Entity* vehicle, const rf::Entity* driver,
                                       rf::Vector3* out_dir, bool* out_capped)
{
    bool capped = false;
    float weight = 1.0f;
    if (!vcam_apc_aim(vehicle, driver, *out_dir, capped, weight)) {
        return false;
    }
    if (out_capped) {
        *out_capped = capped;
    }
    return true;
}

bool vehicle_physics_camera_reticle_offset(float* out_dx, float* out_dy)
{
    *out_dx = 0.0f;
    *out_dy = 0.0f;
    rf::Entity* rider = rf::local_player_entity;
    rf::Entity* vehicle = g_vcam.active ? rf::entity_from_handle(g_vcam.vehicle_handle) : nullptr;
    rf::Vector3 dir{};
    float reach = 0.0f;
    if (g_vcam.active && g_vcam.orbit && g_vcam.seat == VehicleOrbitSeat::gunner) {
        if (!vehicle || !rider || !g_vcam.aim_valid || rider->handle != g_vcam.rider_handle
            || rider->host_handle != g_vcam.vehicle_handle) {
            return true;
        }
        // 0x0049CF40 stores the bound itself, so equality means the seat's pitch limit holds his gun;
        // a look past that limit has faded his aim off P.
        const float eye_pitch = rider->control_data.eye_phb.x;
        if (eye_pitch > rider->min_rel_eye_phb.x && eye_pitch < rider->max_rel_eye_phb.x
            && !(g_vcam.gun_weight < 1.0f)) {
            return true;
        }
        dir = rider->eye_orient.fvec;
        reach = g_vcam.gun_reach;
    }
    else {
        bool capped = false;
        float weight = 1.0f;
        if (!vcam_apc_aim(vehicle, rider, dir, capped, weight) || !capped) {
            return true;
        }
        reach = vcam_reticle_reach(weight);
    }
    // The gun's ray, cut where it leaves R of the camera: |o + t*dir| = R, o = from - cam. R reaches out
    // to P while the aim still converges on it, so the reticle leaves the crosshair without a snap.
    const float r = vcam_reticle_max_dist + reach;
    const rf::Vector3 from = rider->eye_pos;
    const rf::Vector3 o = from - g_vcam.pos;
    const float b = o.dot_prod(dir);
    const float c = o.dot_prod(o) - r * r;
    const float t = c <= 0.0f ? std::sqrt(b * b - c) - b : r;
    const rf::Vector3 end = from + dir * t;
    rf::Vector3 point = end;
    vcam_raycast(from, end, vehicle, &point);
    // Projected at a fixed depth on the same sight line, so the far plane (0x005475D0) cannot cull it.
    const rf::Vector3 rel = point - rf::gr::view_pos;
    const float rel_len = rel.len();
    if (!(rel_len > 0.01f)) {
        return true;
    }
    float hx = 0.0f;
    float hy = 0.0f;
    float cx = 0.0f;
    float cy = 0.0f;
    if (!gr_project_world_to_screen(rf::gr::view_pos + rel * (vcam_reticle_project_dist / rel_len), hx, hy)) {
        return false;
    }
    if (!gr_project_world_to_screen(g_vcam.pos + g_vcam.look * vcam_reticle_project_dist, cx, cy)) {
        return true;
    }
    *out_dx = hx - cx;
    *out_dy = hy - cy;
    return true;
}

void vphys_camera_install_patches()
{
    vehiclecam_follow_cmd.register_cmd();
    vehiclecam_velocity_cmd.register_cmd();
    vehiclecam_min_speed_cmd.register_cmd();
    vehiclecam_lookahead_cmd.register_cmd();
    vehiclecam_lookahead_max_cmd.register_cmd();
    vehiclecam_pitch_follow_cmd.register_cmd();
    vehiclecam_pitch_smooth_cmd.register_cmd();
    vehiclecam_bounce_cmd.register_cmd();
    vehiclecam_lag_cmd.register_cmd();
}
