#include <algorithm>
#include <cmath>
#include <numbers>
#include "vphys_internal.h"
#include "../vehicle_physics.h"
#include "../vehicle.h"
#include "../../../misc/level.h"
#include "../../../rf/ai.h"
#include "../../../rf/entity.h"
#include "../../../rf/geometry.h"
#include "../../../rf/level.h"
#include "../../../rf/multi.h"
#include "../../../rf/object.h"
#include "../../../rf/physics.h"
#include "../../../rf/player/camera.h"

// Surface = bbox_min.y + liquid_depth (point_in_liquid, 0x004CE080); cached for a breached sub.
bool liquid_surface_for(VehicleSimBody& b, rf::Entity* ep, float& out)
{
    rf::GRoom* room = ep->room;
    if (room && room->contains_liquid) {
        out = room->bbox_min.y + room->liquid_depth;
        b.liquid_surface = out;
        b.liquid_surface_valid = true;
        return true;
    }
    if (b.liquid_surface_valid) {
        out = b.liquid_surface;
        return true;
    }
    return false;
}

bool vphys_hull_submerged(const rf::Entity* ep)
{
    if (!ep || !ep->room || !ep->room->contains_liquid
        || rf::entity_is_sub(const_cast<rf::Entity*>(ep))) {
        return false;
    }
    return ep->pos.y < ep->room->bbox_min.y + ep->room->liquid_depth;
}

void apply_flight_model(VehicleSimBody& b, rf::Entity* ep, const VehiclePhysicsParams& p, int cls)
{
    btRigidBody* body = b.body;
    const btMatrix3x3& basis = body->getWorldTransform().getBasis();
    const btVector3 right = basis.getColumn(0);
    const btVector3 up = basis.getColumn(1);
    const btVector3 forward = basis.getColumn(2);
    const btVector3 world_up(0, 1, 0);
    const float mass = 1.0f / std::max(body->getInvMass(), 0.0001f);

    // The engine scales the pitch axis by 0.5 (0x00430BE8); undo it so pitch_rate and yaw_rate match.
    constexpr float ci_pitch_norm = 2.0f;
    // ep->ai.ci still holds the departed driver's input; blind a COPY - the camera/aim path reads it.
    rf::ControlInfo ci = ep->ai.ci;
    if (b.server_owned || vphys_hull_submerged(ep)) {
        ci.rot = rf::Vector3{};
        ci.move = rf::Vector3{};
    }
    // Sub: the share of the hull below the surface; its propellers and planes only work in water.
    // With no surface ever seen (a dry room), it is simply out of the water.
    const bool is_sub = cls == VPHYS_CLASS_SUB;
    float sub_surface = 0.0f;
    const bool sub_has_surface = is_sub && liquid_surface_for(b, ep, sub_surface);
    const float sub_reach = is_sub ? std::max(hull_radius_for(p, ep), 0.1f) : 0.0f;
    const float sub_freeboard = std::clamp(p.sub_freeboard, 0.0f, sub_reach);
    float water_frac = is_sub ? 0.0f : 1.0f;
    float above_float = 2.0f * sub_reach;
    if (sub_has_surface) {
        water_frac = std::clamp((sub_surface - (ep->pos.y - sub_reach)) / (2.0f * sub_reach), 0.0f, 1.0f);
        above_float = (ep->pos.y + sub_reach) - (sub_surface + sub_freeboard);
    }

    const float pitch_in = std::clamp(ci.rot.x * ci_pitch_norm, -1.0f, 1.0f) * p.pitch_sign * water_frac;
    const float yaw_in = std::clamp(ci.rot.y, -1.0f, 1.0f) * p.yaw_sign * water_frac;
    const float thrust_in = std::clamp(ci.move.z, -1.0f, 1.0f) * p.thrust_sign * water_frac;
    const float strafe_in = std::clamp(ci.move.x, -1.0f, 1.0f) * p.strafe_sign * water_frac;
    const float lift_in = std::clamp(ci.move.y, -1.0f, 1.0f) * p.lift_sign * water_frac;

    btVector3 thrust = forward * (thrust_in * p.thrust_accel) + right * (strafe_in * p.strafe_accel)
                     + world_up * (lift_in * p.lift_accel);
    // Propellers cannot push a hull up out of the water: a held ascend floats it at the float line.
    if (is_sub && thrust.y() > 0.0f) {
        constexpr float sub_lift_fade = 0.3f; // ramped, or a held ascend chatters at the float line
        thrust.setY(thrust.y() * std::clamp(-above_float / sub_lift_fade, 0.0f, 1.0f));
    }

    const auto& props = AlpineLevelProperties::instance();
    const bool ceiling_active = cls != VPHYS_CLASS_SUB && props.vehicle_flight_ceiling_enabled
                             && p.ceiling_band > 0.0f;
    float ceiling_depth = 0.0f;
    if (ceiling_active) {
        ceiling_depth = std::clamp(
            (ep->pos.y - (props.vehicle_flight_ceiling - p.ceiling_band)) / p.ceiling_band, 0.0f, 1.0f);
        if (ceiling_depth > 0.0f && thrust.y() > 0.0f) {
            thrust.setY(thrust.y() * (1.0f - ceiling_depth));
        }
    }

    const float lift_scale = b.server_owned ? std::max(p.deadstick_lift, 0.0f) : 1.0f;
    btVector3 accel = thrust + world_up * (rf::gravity * p.gravity_scale * p.hover_gain * lift_scale);

    const btVector3 velocity = body->getLinearVelocity();
    const float speed = velocity.length();
    if (p.max_speed > 0.0f && speed > 0.001f) {
        const float soft = p.max_speed * std::clamp(p.speed_soft_frac, 0.0f, 0.99f);
        if (speed > soft) {
            const float over = std::clamp((speed - soft) / std::max(p.max_speed - soft, 0.001f), 0.0f, 2.0f);
            accel -= (velocity / speed) * (p.speed_drag_accel * over * over);
        }
    }

    // Sub: neutral below the float line, weight for what rises above it, and drag while the hull spans
    // the surface.
    if (is_sub) {
        if (above_float > 0.0f) {
            const float exposed =
                std::clamp(above_float / std::max(2.0f * sub_reach - sub_freeboard, 0.1f), 0.0f, 1.0f);
            accel -= world_up * (rf::gravity * exposed);
        }
        if (water_frac > 0.0f && water_frac < 1.0f) {
            accel -= world_up * (velocity.y() * std::max(p.sub_surface_drag, 0.0f));
        }
    }

    body->applyCentralForce(accel * mass);

    // Nose-up is torque about -rvec, yaw-right +uvec, bank-right -fvec (RF/Bullet identity basis).
    const btVector3 omega = body->getAngularVelocity();
    const float nose_up_now = -omega.dot(right);
    const float yaw_right_now = omega.dot(up);
    const float bank_right_now = -omega.dot(forward);

    const float nose_up = (pitch_in * p.pitch_rate - nose_up_now) * p.rot_servo;

    const float yaw_right = (yaw_in * p.yaw_rate - yaw_right_now) * p.rot_servo;

    const float bank_now = std::asin(std::clamp(-right.y(), -1.0f, 1.0f));
    const float bank_limit = std::fabs(p.bank_max);
    const float bank_target = std::clamp(yaw_in * p.bank_gain * p.bank_sign, -bank_limit, bank_limit);

    constexpr float deg_to_rad = std::numbers::pi_v<float> / 180.0f;
    constexpr float bank_fade_band_deg = 35.0f;
    const float horiz = std::sqrt(std::max(1.0f - forward.y() * forward.y(), 0.0f));
    const float fade_start_deg = std::clamp(p.bank_fade_deg, 0.0f, 89.0f);
    const float fade_end_deg = std::min(fade_start_deg + bank_fade_band_deg, 90.0f);
    const float horiz_full = std::cos(fade_start_deg * deg_to_rad);
    const float horiz_zero = std::cos(fade_end_deg * deg_to_rad);
    float bank_fade = 1.0f;
    if (horiz_full - horiz_zero > 0.0001f) {
        const float t = std::clamp((horiz - horiz_zero) / (horiz_full - horiz_zero), 0.0f, 1.0f);
        const float smooth = t * t * (3.0f - 2.0f * t);
        bank_fade = smooth * smooth;
    }

    const float bank_rate_target = (bank_target - bank_now) * p.bank_servo * bank_fade;
    const float bank_right = (bank_rate_target - bank_right_now) * p.rot_servo;

    const btVector3 alpha = right * (-nose_up) + up * yaw_right + forward * (-bank_right);
    // Down on its skid the hull settles onto the slope like a crate; the bank servo would hold it on an edge.
    if (!b.skid_compound || !b.chassis_ground_contact) {
        body->applyTorque(body->getInvInertiaTensorWorld().inverse() * alpha);
    }

    if (ceiling_active && ep->pos.y >= props.vehicle_flight_ceiling
        && velocity.y() > -p.ceiling_sink) {
        btVector3 v = velocity;
        v.setY(-p.ceiling_sink);
        body->setLinearVelocity(v);
    }
}
