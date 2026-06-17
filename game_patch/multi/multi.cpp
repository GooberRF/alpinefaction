#include <cmath>
#include <algorithm>
#include <unordered_map>
#include <regex>
#include <xlog/xlog.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <shellapi.h>
#include <patch_common/FunHook.h>
#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/AsmWriter.h>
#include <common/version/version.h>
#include <common/utils/list-utils.h>
#include "multi.h"
#include "endgame_votes.h"
#include "multi_private.h"
#include "alpine_packets.h"
#include "server_internal.h"
#include "gametype.h"
#include "bots/bot_chat_manager.h"
#include "../hud/hud.h"
#include "../hud/multi_spectate.h"
#include "../rf/file/file.h"
#include "../rf/level.h"
#include "../os/os.h"
#include "../os/console.h"
#include "../misc/misc.h"
#include "../misc/alpine_settings.h"
#include "../misc/waypoints.h"
#include "../rf/os/os.h"
#include "../rf/event.h"
#include "../rf/gameseq.h"
#include "../rf/misc.h"
#include "../rf/os/timer.h"
#include "../rf/player/camera.h"
#include "../rf/multi.h"
#include "../rf/os/console.h"
#include "../rf/weapon.h"
#include "../rf/entity.h"
#include "../rf/localize.h"
#include "../rf/ai.h"
#include "../rf/item.h"
#include "../rf/gameseq.h"
#include "../rf/gr/gr_font.h"
#include "../rf/ui.h"
#include "../rf/sound/sound.h"
#include "../rf/vmesh.h"
#include "../main/main.h"
#include "../graphics/gr.h"

// --- Improved hitboxes: split capsule collision with head tracking ---

// Lag compensation: when true, capsule collision uses rewound physics data
static bool s_in_lag_comp_raycast = false;

void set_lag_comp_flag(bool active) {
    s_in_lag_comp_raycast = active;
}

// Entity pointer captured from the collision loop
static rf::Entity* s_current_collide_entity = nullptr;

// Cache: VMesh::mesh pointer -> head csphere index (per-model, stable across instances)
static std::unordered_map<void*, int> s_head_csphere_cache;

// Capture ESI (entity pointer) at start of bbox computation in collide_linesegment_level_for_multi
static CodeInjection capture_entity_injection{
    0x0049C7D5,
    [](auto& regs) {
        s_current_collide_entity = static_cast<rf::Entity*>(regs.esi);
    },
};

// Look up historical eye forward vector from ObjInterp during lag compensation.
// Matches the rewound p_data.pos against ObjInterp.pos_array to find the temporal index,
// then reads the corresponding eye_phb_array entry to reconstruct the eye orientation.
static rf::Vector3 get_historical_eye_fvec(const rf::Entity* entity)
{
    // Fallback: current eye orientation
    const rf::Vector3& current_fvec = entity->eye_orient.fvec;

    const rf::ObjInterp* interp = entity->obj_interp;
    if (!interp)
        return current_fvec;

    // ObjInterp layout (from Ghidra analysis):
    //   +0x000: pos_array[20] — Vector3[20] (240 bytes)
    //   +0x1E0: eye_phb_array[20] — Vector3[20] (240 bytes)
    //   +0x534: num — int (number of stored keyframes)
    const char* base = interp->data_;
    const auto* pos_array = reinterpret_cast<const rf::Vector3*>(base + 0x000);
    const auto* eye_phb_array = reinterpret_cast<const rf::Vector3*>(base + 0x1E0);
    int num_frames = *reinterpret_cast<const int*>(base + 0x534);

    if (num_frames < 1)
        return current_fvec;

    int count = std::min(num_frames, 20);

    // Find the pos_array entry closest to the rewound p_data.pos
    const rf::Vector3& rewound_pos = entity->p_data.pos;
    float best_dist_sq = 1e30f;
    int best_index = -1;

    for (int i = 0; i < count; i++) {
        float dx = pos_array[i].x - rewound_pos.x;
        float dy = pos_array[i].y - rewound_pos.y;
        float dz = pos_array[i].z - rewound_pos.z;
        float dist_sq = dx * dx + dy * dy + dz * dz;
        if (dist_sq < best_dist_sq) {
            best_dist_sq = dist_sq;
            best_index = i;
        }
    }

    if (best_index < 0)
        return current_fvec;

    // eye_phb is (pitch, heading, bank) in radians
    const rf::Vector3& eye_phb = eye_phb_array[best_index];
    rf::Matrix3 mat;
    // set_from_angles(pitch, roll, yaw) — eye_phb.x=pitch, eye_phb.z=bank(roll), eye_phb.y=heading(yaw)
    mat.set_from_angles(eye_phb.x, eye_phb.z, eye_phb.y);
    return mat.fvec;
}

// Find the world position of the head collision sphere.
// Uses highest world-space Y to identify head csphere, with per-model caching
// to avoid oscillation at extreme downward pitch where head/torso Y values converge.
static bool find_head_csphere_pos(const rf::Entity* entity, rf::Vector3* out_pos,
                                   float* out_radius = nullptr)
{
    if (!entity->vmesh)
        return false;

    int num = rf::vmesh_get_num_cspheres(entity->vmesh);
    if (num <= 0)
        return false;

    // During lag comp, use rewound p_data pos/orient; otherwise use live entity pos/orient
    const rf::Vector3& transform_pos = s_in_lag_comp_raycast ? entity->p_data.pos : entity->pos;
    const rf::Matrix3& transform_orient = s_in_lag_comp_raycast ? entity->p_data.orient : entity->orient;

    // Find the csphere with highest world-space Y
    int highest_y_index = -1;
    float highest_y = -1e30f;
    rf::Vector3 highest_y_pos;

    for (int i = 0; i < num; i++) {
        rf::Vector3 pos;
        if (rf::vmesh_get_csphere_pos(entity->vmesh, i, &pos,
                const_cast<rf::Vector3*>(&transform_pos),
                const_cast<rf::Matrix3*>(&transform_orient))) {
            if (pos.y > highest_y) {
                highest_y = pos.y;
                highest_y_index = i;
                highest_y_pos = pos;
            }
        }
    }

    if (highest_y_index < 0)
        return false;

    // Helper to retrieve csphere radius for a given index (reduced by 10% for tighter head hitbox)
    auto get_csphere_radius = [&](int index, float* radius) {
        if (radius) {
            rf::Vector3 local_pos;
            float r;
            if (rf::vmesh_get_csphere(entity->vmesh, index, &local_pos, &r)) {
                *radius = r * 1.0f;
            }
        }
    };

    // At non-extreme pitch, highest-Y is reliable — update cache
    // During lag comp, use historical eye forward vector for pitch check
    float sin_pitch = s_in_lag_comp_raycast
        ? get_historical_eye_fvec(entity).y
        : entity->eye_orient.fvec.y;
    bool extreme_pitch = std::abs(sin_pitch) > 0.5f;
    void* mesh_key = entity->vmesh->mesh;

    if (!extreme_pitch) {
        s_head_csphere_cache[mesh_key] = highest_y_index;
        *out_pos = highest_y_pos;
        get_csphere_radius(highest_y_index, out_radius);
        return true;
    }

    // At extreme pitch, use cached index to avoid oscillation
    auto it = s_head_csphere_cache.find(mesh_key);
    if (it != s_head_csphere_cache.end()) {
        int cached_index = it->second;
        if (cached_index >= 0 && cached_index < num) {
            rf::Vector3 cached_pos;
            if (rf::vmesh_get_csphere_pos(entity->vmesh, cached_index, &cached_pos,
                    const_cast<rf::Vector3*>(&transform_pos),
                    const_cast<rf::Matrix3*>(&transform_orient))) {
                *out_pos = cached_pos;
                get_csphere_radius(cached_index, out_radius);
                return true;
            }
        }
    }

    // No cache yet — fall through to highest-Y
    *out_pos = highest_y_pos;
    get_csphere_radius(highest_y_index, out_radius);
    return true;
}

// Pitch-based tilt axis fallback when head csphere is unavailable
static rf::Vector3 compute_tilt_axis_from_pitch(const rf::Entity* entity)
{
    // During lag comp, use historical eye forward vector instead of live eye_orient
    const rf::Vector3 fvec = s_in_lag_comp_raycast
        ? get_historical_eye_fvec(entity)
        : entity->eye_orient.fvec;

    constexpr float threshold = 0.01f;
    float cos_pitch_raw = std::sqrt(fvec.x * fvec.x + fvec.z * fvec.z);

    rf::Vector3 fwd_xz;
    if (cos_pitch_raw > threshold) {
        float inv_cos = 1.0f / cos_pitch_raw;
        fwd_xz = {fvec.x * inv_cos, 0.0f, fvec.z * inv_cos};
    }
    else {
        // During lag comp, use rewound p_data.orient for body direction fallback
        const rf::Vector3& body_fvec = s_in_lag_comp_raycast
            ? entity->p_data.orient.fvec
            : entity->orient.fvec;
        float body_len = std::sqrt(body_fvec.x * body_fvec.x + body_fvec.z * body_fvec.z);
        if (body_len > threshold) {
            float inv_len = 1.0f / body_len;
            fwd_xz = {body_fvec.x * inv_len, 0.0f, body_fvec.z * inv_len};
        }
        else {
            fwd_xz = {1.0f, 0.0f, 0.0f};
        }
    }

    float raw_pitch = std::asin(std::clamp(fvec.y, -1.0f, 1.0f));
    float fraction = (raw_pitch >= 0.0f) ? (40.0f / 90.0f) : (43.0f / 90.0f);
    float pitch = raw_pitch * fraction;
    float sin_pitch = std::sin(pitch);
    float cos_pitch = std::cos(pitch);

    return rf::Vector3{
        -fwd_xz.x * sin_pitch,
        cos_pitch,
        -fwd_xz.z * sin_pitch
    };
}

rf::Vector3 compute_tilt_axis(const rf::Entity* entity, const rf::Vector3& split_point,
                              float* head_dist_out,
                              float* head_radius_out,
                              rf::Vector3* head_pos_out)
{
    rf::Vector3 pitch_axis = compute_tilt_axis_from_pitch(entity);

    if (head_dist_out)
        *head_dist_out = -1.0f;
    if (head_radius_out)
        *head_radius_out = -1.0f;

    rf::Vector3 head_pos;
    float head_radius;
    if (find_head_csphere_pos(entity, &head_pos, &head_radius)) {
        if (head_pos_out) {
            // Offset head hitbox backward (opposite look direction)
            // During lag comp, use historical eye forward vector
            const rf::Vector3 fwd = s_in_lag_comp_raycast
                ? get_historical_eye_fvec(entity)
                : entity->eye_orient.fvec;
            *head_pos_out = head_pos - fwd * (head_radius * 0.30f);
        }
        if (head_radius_out)
            *head_radius_out = head_radius;
        rf::Vector3 dir = head_pos - split_point;
        float len = dir.len();
        if (len > 0.01f) {
            if (head_dist_out)
                *head_dist_out = len;
            return dir * (1.0f / len);
        }
    }

    return pitch_axis;
}

// Test ray against a sphere centered at `center` with given `radius`.
// Returns true if hit found with t in [t_min, t_max], writes nearest t to `t_out`.
static bool ix_ray_sphere(const rf::Vector3& ray_origin, const rf::Vector3& ray_dir,
                          const rf::Vector3& center, float radius,
                          float t_min, float t_max, float* t_out)
{
    float ox = ray_origin.x - center.x;
    float oy = ray_origin.y - center.y;
    float oz = ray_origin.z - center.z;

    float a = ray_dir.x * ray_dir.x + ray_dir.y * ray_dir.y + ray_dir.z * ray_dir.z;
    float b = 2.0f * (ox * ray_dir.x + oy * ray_dir.y + oz * ray_dir.z);
    float c = ox * ox + oy * oy + oz * oz - radius * radius;

    float disc = b * b - 4.0f * a * c;
    if (disc < 0.0f)
        return false;

    float sqrt_disc = std::sqrt(disc);
    float inv_2a = 1.0f / (2.0f * a);

    float t0 = (-b - sqrt_disc) * inv_2a;
    if (t0 >= t_min && t0 <= t_max) {
        *t_out = t0;
        return true;
    }

    float t1 = (-b + sqrt_disc) * inv_2a;
    if (t1 >= t_min && t1 <= t_max) {
        *t_out = t1;
        return true;
    }

    return false;
}

// Vertical-axis ray-capsule intersection (optimized for Y-aligned capsules).
// cap_a and cap_b are hemisphere centers sharing the same X and Z.
// Updates *best_t if a closer hit is found. Returns true if any hit found.
static bool ix_ray_capsule_vertical(const rf::Vector3& cap_a, const rf::Vector3& cap_b,
                                     float radius,
                                     const rf::Vector3& ray_origin, const rf::Vector3& ray_dir,
                                     float* best_t)
{
    float cx = cap_a.x;
    float cz = cap_a.z;
    float y_bot = std::min(cap_a.y, cap_b.y);
    float y_top = std::max(cap_a.y, cap_b.y);

    // Degenerate: capsule too short, use single sphere
    if (y_bot >= y_top) {
        rf::Vector3 center{cx, y_bot, cz};
        float t;
        if (ix_ray_sphere(ray_origin, ray_dir, center, radius, 0.0f, 1.0f, &t) && t < *best_t) {
            *best_t = t;
            return true;
        }
        return false;
    }

    bool found = false;
    constexpr float epsilon = 1e-12f;

    // --- Cylinder body (infinite cylinder in XZ, clamped to Y slab) ---
    float ox = ray_origin.x - cx;
    float oz = ray_origin.z - cz;
    float a = ray_dir.x * ray_dir.x + ray_dir.z * ray_dir.z;
    float b_cyl = 2.0f * (ox * ray_dir.x + oz * ray_dir.z);
    float c_cyl = ox * ox + oz * oz - radius * radius;

    if (a >= epsilon) {
        float disc = b_cyl * b_cyl - 4.0f * a * c_cyl;
        if (disc >= 0.0f) {
            float sqrt_disc = std::sqrt(disc);
            float inv_2a = 1.0f / (2.0f * a);
            float t0 = (-b_cyl - sqrt_disc) * inv_2a;
            float t1 = (-b_cyl + sqrt_disc) * inv_2a;

            for (float t : {t0, t1}) {
                if (t >= 0.0f && t <= 1.0f && t < *best_t) {
                    float hit_y = ray_origin.y + ray_dir.y * t;
                    if (hit_y >= y_bot && hit_y <= y_top) {
                        *best_t = t;
                        found = true;
                        break;
                    }
                }
            }
        }
    }
    else {
        // Ray nearly vertical - check if inside XZ circle
        if (c_cyl <= 0.0f) {
            if (std::abs(ray_dir.y) >= epsilon) {
                float inv_dy = 1.0f / ray_dir.y;
                float ty0 = (y_bot - ray_origin.y) * inv_dy;
                float ty1 = (y_top - ray_origin.y) * inv_dy;
                if (ty0 > ty1) std::swap(ty0, ty1);
                float t_enter = std::max(ty0, 0.0f);
                if (t_enter <= ty1 && t_enter <= 1.0f && t_enter < *best_t) {
                    *best_t = t_enter;
                    found = true;
                }
            }
            else if (ray_origin.y >= y_bot && ray_origin.y <= y_top && 0.0f < *best_t) {
                *best_t = 0.0f;
                found = true;
            }
        }
    }

    // --- Bottom hemisphere ---
    {
        rf::Vector3 cap_center{cx, y_bot, cz};
        float t;
        if (ix_ray_sphere(ray_origin, ray_dir, cap_center, radius, 0.0f, 1.0f, &t)) {
            float hit_y = ray_origin.y + ray_dir.y * t;
            if (hit_y <= y_bot && t < *best_t) {
                *best_t = t;
                found = true;
            }
        }
    }

    // --- Top hemisphere ---
    {
        rf::Vector3 cap_center{cx, y_top, cz};
        float t;
        if (ix_ray_sphere(ray_origin, ray_dir, cap_center, radius, 0.0f, 1.0f, &t)) {
            float hit_y = ray_origin.y + ray_dir.y * t;
            if (hit_y >= y_top && t < *best_t) {
                *best_t = t;
                found = true;
            }
        }
    }

    return found;
}

// General-axis ray-capsule intersection for capsule from cap_a to cap_b with given radius.
// Updates *best_t if a closer hit is found. Returns true if any hit found.
static bool ix_ray_capsule_general(const rf::Vector3& cap_a, const rf::Vector3& cap_b,
                                    float radius,
                                    const rf::Vector3& ray_origin, const rf::Vector3& ray_dir,
                                    float* best_t)
{
    rf::Vector3 v = cap_b - cap_a;
    float v_dot_v = v.dot_prod(v);

    constexpr float epsilon = 1e-12f;

    // Degenerate: A == B, test single sphere
    if (v_dot_v < epsilon) {
        float t;
        if (ix_ray_sphere(ray_origin, ray_dir, cap_a, radius, 0.0f, 1.0f, &t) && t < *best_t) {
            *best_t = t;
            return true;
        }
        return false;
    }

    float inv_v_dot_v = 1.0f / v_dot_v;
    rf::Vector3 w = ray_origin - cap_a;

    // Project ray_dir and w perpendicular to capsule axis V
    float d_dot_v = ray_dir.dot_prod(v);
    float w_dot_v = w.dot_prod(v);

    rf::Vector3 d_perp = ray_dir - v * (d_dot_v * inv_v_dot_v);
    rf::Vector3 w_perp = w - v * (w_dot_v * inv_v_dot_v);

    float a = d_perp.dot_prod(d_perp);
    float b = 2.0f * w_perp.dot_prod(d_perp);
    float c = w_perp.dot_prod(w_perp) - radius * radius;

    bool found = false;

    // --- Cylinder body ---
    if (a >= epsilon) {
        float disc = b * b - 4.0f * a * c;
        if (disc >= 0.0f) {
            float sqrt_disc = std::sqrt(disc);
            float inv_2a = 1.0f / (2.0f * a);
            float t0 = (-b - sqrt_disc) * inv_2a;
            float t1 = (-b + sqrt_disc) * inv_2a;

            for (float t : {t0, t1}) {
                if (t >= 0.0f && t <= 1.0f && t < *best_t) {
                    // Check projection along capsule axis: s in [0, 1] means cylinder body
                    float s = (w_dot_v + d_dot_v * t) * inv_v_dot_v;
                    if (s >= 0.0f && s <= 1.0f) {
                        *best_t = t;
                        found = true;
                        break;
                    }
                }
            }
        }
    }

    // --- Hemisphere at A (accept if projection s <= 0) ---
    {
        float t;
        if (ix_ray_sphere(ray_origin, ray_dir, cap_a, radius, 0.0f, 1.0f, &t) && t < *best_t) {
            float s = (w_dot_v + d_dot_v * t) * inv_v_dot_v;
            if (s <= 0.0f) {
                *best_t = t;
                found = true;
            }
        }
    }

    // --- Hemisphere at B (accept if projection s >= 1) ---
    {
        float t;
        if (ix_ray_sphere(ray_origin, ray_dir, cap_b, radius, 0.0f, 1.0f, &t) && t < *best_t) {
            float s = (w_dot_v + d_dot_v * t) * inv_v_dot_v;
            if (s >= 1.0f) {
                *best_t = t;
                found = true;
            }
        }
    }

    return found;
}

// Vertical-axis ray-cylinder intersection (flat disc caps, Y-aligned).
// cyl_bot and cyl_top are the flat disc centers sharing the same X and Z.
// Updates *best_t if a closer hit is found. Returns true if any hit found.
static bool ix_ray_cylinder_vertical(const rf::Vector3& cyl_bot, const rf::Vector3& cyl_top,
                                      float radius,
                                      const rf::Vector3& ray_origin, const rf::Vector3& ray_dir,
                                      float* best_t)
{
    float cx = cyl_bot.x;
    float cz = cyl_bot.z;
    float y_bot = std::min(cyl_bot.y, cyl_top.y);
    float y_top = std::max(cyl_bot.y, cyl_top.y);

    if (y_bot >= y_top)
        return false;

    bool found = false;
    constexpr float epsilon = 1e-12f;

    // --- Cylinder side wall (infinite cylinder in XZ, clamped to Y slab) ---
    float ox = ray_origin.x - cx;
    float oz = ray_origin.z - cz;
    float a = ray_dir.x * ray_dir.x + ray_dir.z * ray_dir.z;
    float b_cyl = 2.0f * (ox * ray_dir.x + oz * ray_dir.z);
    float c_cyl = ox * ox + oz * oz - radius * radius;

    if (a >= epsilon) {
        float disc = b_cyl * b_cyl - 4.0f * a * c_cyl;
        if (disc >= 0.0f) {
            float sqrt_disc = std::sqrt(disc);
            float inv_2a = 1.0f / (2.0f * a);
            float t0 = (-b_cyl - sqrt_disc) * inv_2a;
            float t1 = (-b_cyl + sqrt_disc) * inv_2a;

            for (float t : {t0, t1}) {
                if (t >= 0.0f && t <= 1.0f && t < *best_t) {
                    float hit_y = ray_origin.y + ray_dir.y * t;
                    if (hit_y >= y_bot && hit_y <= y_top) {
                        *best_t = t;
                        found = true;
                        break;
                    }
                }
            }
        }
    }
    else {
        // Ray nearly vertical - check if inside XZ circle
        if (c_cyl <= 0.0f) {
            if (std::abs(ray_dir.y) >= epsilon) {
                float inv_dy = 1.0f / ray_dir.y;
                float ty0 = (y_bot - ray_origin.y) * inv_dy;
                float ty1 = (y_top - ray_origin.y) * inv_dy;
                if (ty0 > ty1) std::swap(ty0, ty1);
                float t_enter = std::max(ty0, 0.0f);
                if (t_enter <= ty1 && t_enter <= 1.0f && t_enter < *best_t) {
                    *best_t = t_enter;
                    found = true;
                }
            }
            else if (ray_origin.y >= y_bot && ray_origin.y <= y_top && 0.0f < *best_t) {
                *best_t = 0.0f;
                found = true;
            }
        }
    }

    // --- Bottom disc cap (y = y_bot) ---
    if (std::abs(ray_dir.y) >= epsilon) {
        float t = (y_bot - ray_origin.y) / ray_dir.y;
        if (t >= 0.0f && t <= 1.0f && t < *best_t) {
            float hx = ray_origin.x + ray_dir.x * t - cx;
            float hz = ray_origin.z + ray_dir.z * t - cz;
            if (hx * hx + hz * hz <= radius * radius) {
                *best_t = t;
                found = true;
            }
        }
    }

    // --- Top disc cap (y = y_top) ---
    if (std::abs(ray_dir.y) >= epsilon) {
        float t = (y_top - ray_origin.y) / ray_dir.y;
        if (t >= 0.0f && t <= 1.0f && t < *best_t) {
            float hx = ray_origin.x + ray_dir.x * t - cx;
            float hz = ray_origin.z + ray_dir.z * t - cz;
            if (hx * hx + hz * hz <= radius * radius) {
                *best_t = t;
                found = true;
            }
        }
    }

    return found;
}

// General-axis ray-cylinder intersection (flat disc caps) for cylinder from cyl_a to cyl_b.
// Updates *best_t if a closer hit is found. Returns true if any hit found.
static bool ix_ray_cylinder_general(const rf::Vector3& cyl_a, const rf::Vector3& cyl_b,
                                     float radius,
                                     const rf::Vector3& ray_origin, const rf::Vector3& ray_dir,
                                     float* best_t)
{
    rf::Vector3 v = cyl_b - cyl_a;
    float v_dot_v = v.dot_prod(v);

    constexpr float epsilon = 1e-12f;

    // Degenerate: A == B, no volume
    if (v_dot_v < epsilon)
        return false;

    float inv_v_dot_v = 1.0f / v_dot_v;
    rf::Vector3 w = ray_origin - cyl_a;

    // Project ray_dir and w perpendicular to cylinder axis V
    float d_dot_v = ray_dir.dot_prod(v);
    float w_dot_v = w.dot_prod(v);

    rf::Vector3 d_perp = ray_dir - v * (d_dot_v * inv_v_dot_v);
    rf::Vector3 w_perp = w - v * (w_dot_v * inv_v_dot_v);

    float a = d_perp.dot_prod(d_perp);
    float b = 2.0f * w_perp.dot_prod(d_perp);
    float c = w_perp.dot_prod(w_perp) - radius * radius;

    bool found = false;

    // --- Cylinder body ---
    if (a >= epsilon) {
        float disc = b * b - 4.0f * a * c;
        if (disc >= 0.0f) {
            float sqrt_disc = std::sqrt(disc);
            float inv_2a = 1.0f / (2.0f * a);
            float t0 = (-b - sqrt_disc) * inv_2a;
            float t1 = (-b + sqrt_disc) * inv_2a;

            for (float t : {t0, t1}) {
                if (t >= 0.0f && t <= 1.0f && t < *best_t) {
                    float s = (w_dot_v + d_dot_v * t) * inv_v_dot_v;
                    if (s >= 0.0f && s <= 1.0f) {
                        *best_t = t;
                        found = true;
                        break;
                    }
                }
            }
        }
    }

    // --- Disc cap at A (plane: dot(p - A, v) = 0) ---
    if (std::abs(d_dot_v) >= epsilon) {
        float t = -w_dot_v / d_dot_v;
        if (t >= 0.0f && t <= 1.0f && t < *best_t) {
            // Hit point relative to A; on this plane, axial component is 0, so |h|² = radial distance²
            rf::Vector3 h = w + ray_dir * t;
            if (h.dot_prod(h) <= radius * radius) {
                *best_t = t;
                found = true;
            }
        }
    }

    // --- Disc cap at B (plane: dot(p - A, v) = |v|²) ---
    if (std::abs(d_dot_v) >= epsilon) {
        float t = (v_dot_v - w_dot_v) / d_dot_v;
        if (t >= 0.0f && t <= 1.0f && t < *best_t) {
            // Hit point relative to B; on this plane, axial component is 0, so |h|² = radial distance²
            rf::Vector3 h = w + ray_dir * t - v;
            if (h.dot_prod(h) <= radius * radius) {
                *best_t = t;
                found = true;
            }
        }
    }

    return found;
}

// Hook the ix_linesegment_boundingbox call at 0x0049C862 (multi entity collision path)
// Replaces AABB intersection with split capsule (lower vertical + upper tilted toward head)
static CallHook<bool(const rf::Vector3&, const rf::Vector3&, const rf::Vector3&, const rf::Vector3&, rf::Vector3*)>
ix_linesegment_capsule_hook{
    0x0049C862,
    [](const rf::Vector3& bbox_min, const rf::Vector3& bbox_max,
       const rf::Vector3& p0, const rf::Vector3& p1, rf::Vector3* hit_point) {

        if (g_alpine_server_config.legacy_hitboxes)
            return ix_linesegment_capsule_hook.call_target(bbox_min, bbox_max, p0, p1, hit_point);

        rf::Entity* entity = s_current_collide_entity;

        float cx = (bbox_min.x + bbox_max.x) * 0.5f;
        float cz = (bbox_min.z + bbox_max.z) * 0.5f;
        float radius = (bbox_max.x - bbox_min.x) * 0.5f;

        // When crouching, the engine lowers bbox_max.y by half_size.y * 0.5 (only the top).
        // Some character models (e.g. miner1 with non-pistol weapons) don't crouch that low - extend upward
        bool crouching = entity && rf::entity_is_crouching(entity);
        float effective_max_y = bbox_max.y;
        if (crouching) {
            float bbox_height = bbox_max.y - bbox_min.y;
            effective_max_y += bbox_height * 0.3f;
        }

        float split_ratio = crouching ? 0.35f : 0.55f;
        float bbox_height = effective_max_y - bbox_min.y;
        float split_y = bbox_min.y + bbox_height * split_ratio;
        float upper_height = effective_max_y - split_y;

        rf::Vector3 dir = p1 - p0;
        float best_t = 2.0f;

        // Split into lower (vertical) + upper (tilted) if entity available and bbox tall enough
        if (entity && upper_height > radius) {
            // Lower capsule (vertical): inset bottom so hemisphere doesn't extend below bbox
            rf::Vector3 lower_bot{cx, bbox_min.y + radius, cz};
            rf::Vector3 lower_top{cx, split_y, cz};
            ix_ray_capsule_vertical(lower_bot, lower_top, radius, p0, dir, &best_t);

            // Torso cylinder (tilted): starts at split point, shortened at top for head sphere
            rf::Vector3 upper_a{cx, split_y, cz};
            float head_dist;
            float head_radius;
            rf::Vector3 head_world_pos;
            rf::Vector3 tilt_axis = compute_tilt_axis(entity, upper_a, &head_dist, &head_radius, &head_world_pos);
            float upper_len = upper_height;
            if (head_dist > 0.0f)
                upper_len = std::min(upper_len, head_dist);

            // Shorten so flat top stops just before head sphere
            if (head_radius > 0.0f && head_dist > 0.0f) {
                float max_endpoint = head_dist - head_radius;
                upper_len = std::min(upper_len, max_endpoint);
            }
            upper_len = std::max(upper_len, 0.0f);
            upper_len *= 1.05f;

            rf::Vector3 upper_b = upper_a + tilt_axis * upper_len;
            ix_ray_cylinder_general(upper_a, upper_b, radius, p0, dir, &best_t);

            // Head sphere intersection
            if (head_radius > 0.0f && head_dist > 0.0f) {
                float head_t;
                if (ix_ray_sphere(p0, dir, head_world_pos, head_radius, 0.0f, 1.0f, &head_t) && head_t < best_t) {
                    best_t = head_t;
                }
            }
        }
        else {
            // Fallback: single vertical capsule
            rf::Vector3 bot{cx, bbox_min.y, cz};
            rf::Vector3 top{cx, bbox_max.y, cz};
            ix_ray_capsule_vertical(bot, top, radius, p0, dir, &best_t);
        }

        if (best_t > 1.0f)
            return false;

        if (hit_point) {
            hit_point->x = p0.x + dir.x * best_t;
            hit_point->y = p0.y + dir.y * best_t;
            hit_point->z = p0.z + dir.z * best_t;
        }
        return true;
    },
};

// Note: this must be called from DLL init function
// Note: we can't use global variable because that would lead to crash when launcher loads this DLL to check dependencies
static rf::CmdLineParam& get_url_cmd_line_param()
{
    static rf::CmdLineParam url_param{"-url", "", true};
    return url_param;
}

static rf::CmdLineParam& get_levelm_cmd_line_param()
{
    static rf::CmdLineParam levelm_param{"-levelm", "", true};
    return levelm_param;
}

static rf::CmdLineParam& get_bot_cmd_line_param()
{
    static rf::CmdLineParam bot_param{"-bot", "", true};
    return bot_param;
}

static rf::CmdLineParam& get_debugbot_cmd_line_param()
{
    static rf::CmdLineParam debugbot_param{"-debugbot", "", true};
    return debugbot_param;
}

static rf::CmdLineParam& get_noquit_cmd_line_param()
{
    static rf::CmdLineParam noquit_param{"-noquit", "", false};
    return noquit_param;
}

static rf::CmdLineParam& get_awpgen_cmd_line_param()
{
    static rf::CmdLineParam awpgen_param{"-awpgen", "", true};
    return awpgen_param;
}

static bool g_client_bot_launch_enabled = false;
static bool g_client_bot_debug_render_enabled = false;
static bool g_awpgen_mode = false;

bool client_bot_launch_enabled()
{
    return g_client_bot_launch_enabled
        || is_client_bot_requested_from_cmdline()
        || is_client_debugbot_requested_from_cmdline();
}

bool client_bot_headless_enabled()
{
    return client_bot_launch_enabled()
        && !(g_client_bot_debug_render_enabled || is_client_debugbot_requested_from_cmdline());
}

bool is_awpgen_active()
{
    return g_awpgen_mode || awpgen_requested_from_raw_cmdline();
}

bool is_headless_mode()
{
    return client_bot_headless_enabled() || g_awpgen_mode || awpgen_requested_from_raw_cmdline();
}

// Returns false if bot launch validation failed and the process should quit.
static bool handle_bot_cmd_line_params()
{
    if (rf::is_dedicated_server) {
        g_client_bot_launch_enabled = false;
        g_client_bot_debug_render_enabled = false;
        return true;
    }

    const bool has_bot_switch = is_client_bot_requested_from_cmdline();
    const bool has_debugbot_switch = is_client_debugbot_requested_from_cmdline();
    g_client_bot_launch_enabled = has_bot_switch || has_debugbot_switch;
    g_client_bot_debug_render_enabled = has_debugbot_switch;

    // Parse shared secret from -bot or -debugbot argument
    auto parse_secret = [](rf::CmdLineParam& param) -> uint32_t {
        if (!param.found()) return 0;
        const char* arg = param.get_arg();
        if (!arg || arg[0] == '\0') return 0;
        try { return std::stoul(arg); }
        catch (...) { return 0; }
    };
    if (has_bot_switch) {
        g_alpine_game_config.bot_shared_secret = parse_secret(get_bot_cmd_line_param());
    }
    if (has_debugbot_switch && g_alpine_game_config.bot_shared_secret == 0) {
        g_alpine_game_config.bot_shared_secret = parse_secret(get_debugbot_cmd_line_param());
    }

    // Parse -noquit flag
    if (get_noquit_cmd_line_param().found() && client_bot_launch_enabled()) {
        g_alpine_game_config.bot_quit_when_disconnected = false;
    }

    // Validate bot launch requirements
    if (g_client_bot_launch_enabled) {
        const bool has_url = get_url_cmd_line_param().found();
        const bool has_secret = g_alpine_game_config.bot_shared_secret != 0;
        if (!has_url || !has_secret) {
            rf::console::print(
                "Bot launch requires a server (-url <rf://IP:PORT>) and the correct shared secret (-bot <secret> or -debugbot <secret>)."
            );
            g_client_bot_launch_enabled = false;
            g_alpine_game_config.rendering_enabled = false;
            rf::sound_enabled = false;
            rf::gameseq_set_state(rf::GS_QUITING, false);
            return false;
        }
    }

    if (client_bot_launch_enabled()) {
        const bool headless = client_bot_headless_enabled();
        g_alpine_game_config.rendering_enabled = !headless;
        if (headless) {
            rf::sound_enabled = false;
        }
        rf::console::print(
            "Client bot mode enabled ({}).",
            headless ? "headless" : "debug render"
        );

        // Set a consistent initial name for bot clients.
        // The server will assign the real identity after connection.
        if (rf::local_player) {
            std::strncpy(rf::local_player->settings.name, "af_bot", sizeof(rf::local_player->settings.name) - 1);
            rf::local_player->settings.name[sizeof(rf::local_player->settings.name) - 1] = '\0';
            rf::local_player->name = "af_bot";
        }
    }
    return true;
}

void handle_url_param()
{
    if (!get_url_cmd_line_param().found()) {
        return;
    }

    const char* const url = get_url_cmd_line_param().get_arg();
    std::regex e{R"(^rf://([\w\.-]+):(\d+)/?(?:\?password=(.*))?$)"};
    std::cmatch cm{};
    if (!std::regex_match(url, cm, e)) {
        xlog::warn("Unsupported URL: {}", url);
        return;
    }

    const std::string host_name = cm[1].str();
    const uint16_t port = static_cast<uint16_t>(std::stoi(cm[2].str()));
    const std::string password = cm[3].str();

    rf::console::print("Connecting to {}:{}...", host_name, port);

    const addrinfo hints{ .ai_family = AF_INET };
    addrinfo* host_addr = nullptr;
    if (getaddrinfo(host_name.c_str(), nullptr, &hints, &host_addr) != 0
        || !host_addr) {
        xlog::warn("URL host lookup failed");
        return;
    }

    const sockaddr_in* const sock =
        reinterpret_cast<const sockaddr_in*>(host_addr->ai_addr);

    const rf::NetAddr addr{
        .ip_addr = ntohl(sock->sin_addr.S_un.S_addr), 
        .port = port
    };
    start_join_multi_game_sequence(addr, password);

    freeaddrinfo(host_addr);
}

void handle_levelm_param()
{
    // do nothing unless -levelm is specified
    if (!get_levelm_cmd_line_param().found()) {
        return;
    }

    std::string level_filename = get_levelm_cmd_line_param().get_arg();

    auto [is_valid, valid_filename] = is_level_name_valid(level_filename);
    if (!is_valid) {
        xlog::warn("levelm: level '{}' is not available", level_filename);
        return;
    }
    start_levelm_load_sequence(valid_filename);
}

// Returns true if -awpgen was handled (caller should skip -levelm).
bool handle_awpgen_param()
{
    if (!get_awpgen_cmd_line_param().found()) {
        return false;
    }

    const char* arg = get_awpgen_cmd_line_param().get_arg();
    if (!arg || arg[0] == '\0') {
        xlog::error("-awpgen: missing level filename, quitting");
        rf::gameseq_set_state(rf::GS_QUITING, false);
        return true;
    }

    std::string level_filename = arg;

    // Normalize .rfl extension
    if (!string_iends_with(level_filename, ".rfl")) {
        level_filename += ".rfl";
    }

    // Validate level file is installed
    if (rf::get_file_checksum(level_filename.c_str()) == 0) {
        xlog::error("-awpgen: unknown level {}, quitting", level_filename);
        rf::gameseq_set_state(rf::GS_QUITING, false);
        return true;
    }

    waypoints_start_awpgen(level_filename);
    start_levelm_load_sequence(level_filename);
    return true;
}

FunHook<void()> multi_limbo_init{
    0x0047C280,
    [] {
        rf::activate_all_events_of_type(rf::EventType::When_Round_Ends, -1, -1, true);

        int limbo_time = 10000;

        if (rf::is_server) {
            server_on_limbo_state_enter();
            multi_player_set_can_endgame_vote(false); // servers can't endgame vote

            if (g_match_info.match_active) {
                af_broadcast_automated_chat_msg("\xA6 Match complete!");
                g_match_info.reset();
            }
            else if (g_match_info.pre_match_active && g_match_info.everyone_ready) {
                limbo_time = 5000; // reduce limbo time to 5 sec on match start (will always be a restart of the current map)
                g_match_info.match_active = g_match_info.everyone_ready;
                g_match_info.everyone_ready = false;
                g_match_info.pre_match_active = false;
            }
            else if (g_match_info.pre_match_active) {
                cancel_match(); // cancel match if map forcefully ends during pre-match phase
            }
        }

        // don't let clients vote if the map has been played for less than 1 min
        else if(rf::level.time >= 60.0f) {
            multi_player_set_can_endgame_vote(true);
        }

        // purge any vote or ready notifications on level end
        if (!rf::is_server && !rf::is_dedicated_server) {
            remove_hud_vote_notification();
            set_local_pre_match_active(false);
        }

        if (!rf::player_list) {
            xlog::trace("Wait between levels shortened because server is empty");
            limbo_time = 100;
        }

        rf::multi_limbo_timer.set(limbo_time);
        waypoints_on_limbo_enter();

        if (!rf::local_player)
            return;

        bot_chat_manager_on_limbo_enter(*rf::local_player);

        rf::camera_enter_random_fixed_pos();
        rf::camera_enter_fixed(rf::local_player->cam);
        rf::local_screen_flash(rf::local_player, 0xFF, 0xFF, 0xFF, 0x01);

        const auto gt = rf::multi_get_game_type();
        bool we_win = false;

        if (gt == rf::NG_TYPE_DM) {
            // need at least 2 players to possibly win
            if (rf::multi_num_players() >= 2) {
                int my_score = 0;
                int max_score = 0;

                if (rf::local_player->stats)
                    my_score = rf::local_player->stats->score;

                for (auto& p : SinglyLinkedList{rf::player_list}) {
                    if (p.stats && p.stats->score > max_score)
                        max_score = p.stats->score;
                }

                we_win = (my_score >= max_score); // count it as a win if tied for win
            }
        }
        else if (gt != rf::NG_TYPE_RUN) { // no winner in run
            int red = 0, blue = 0;
            switch (gt) {
            case rf::NG_TYPE_CTF: {
                red = rf::multi_ctf_get_red_team_score();
                blue = rf::multi_ctf_get_blue_team_score();
                break;
            }
            case rf::NG_TYPE_TEAMDM: {
                red = rf::multi_tdm_get_red_team_score();
                blue = rf::multi_tdm_get_blue_team_score();
                break;
            }
            case rf::NG_TYPE_DC:
            case rf::NG_TYPE_KOTH: {
                red = multi_koth_get_red_team_score();
                blue = multi_koth_get_blue_team_score();
                break;
            }
            case rf::NG_TYPE_REV: {
                red = static_cast<int>(rev_all_points_permalocked());
                blue = static_cast<int>(!rev_all_points_permalocked());
                break;
            }
            case rf::NG_TYPE_ESC: {
                for (const auto& hill : g_koth_info.hills) {
                    if (hill.ownership == HillOwner::HO_Red) {
                        red++;
                    }
                    else if (hill.ownership == HillOwner::HO_Blue) {
                        blue++;
                    }
                }
                break;
            }
            default:
                break;
            }
            if (rf::local_player->team == 0)
                we_win = (red > blue);
            else if (rf::local_player->team == 1)
                we_win = (blue > red);
        }

        rf::snd_play(we_win ? 80 : 78, 0, 0.0f, 1.0f);
    },
};

CodeInjection multi_start_injection{
    0x0046D5B0,
    []() {
        void debug_multi_init();
        debug_multi_init();
        void reset_restricted_cmds_on_init_multi();
        reset_restricted_cmds_on_init_multi();
        if (g_alpine_game_config.try_disable_textures) {
            evaluate_lightmaps_only();
        }        
    },
};

CodeInjection ctf_flag_return_fix{
    0x0047381D,
    [](auto& regs) {
        auto stack_frame = regs.esp + 0x1C;
        bool red = addr_as_ref<bool>(stack_frame + 4);
        if (red) {
            regs.eip = 0x00473827;
        }
        else {
            regs.eip = 0x00473822;
        }
    },
};

// Vanilla multi_ctf_is_red/blue_flag_in_base functions crash if the ctf_initialized flag is set
// but the corresponding flag item pointer is NULL. This can happen if a level has only one CTF flag
// item (e.g. blue but not red). Return true (in base) when the pointer is NULL, matching the vanilla
// behavior when CTF is uninitialized.
// Same vulnerability exists in the CTF flag capture function (0x00473BA0) which directly
// dereferences ctf_red/blue_flag_item pointers without NULL checks.
CodeInjection ctf_flag_capture_red_null_check{
    0x00473C17,
    [](auto& regs) {
        if (!rf::ctf_red_flag_item) {
            regs.eip = 0x00473D2E;
        }
    },
};

CodeInjection ctf_flag_capture_blue_null_check{
    0x00473CC2,
    [](auto& regs) {
        if (!rf::ctf_blue_flag_item) {
            regs.eip = 0x00473D2E;
        }
    },
};

FunHook<bool()> multi_ctf_is_red_flag_in_base_hook{
    0x00474E80,
    []() -> bool {
        if (!rf::ctf_red_flag_item) return true;
        return multi_ctf_is_red_flag_in_base_hook.call_target();
    },
};

FunHook<bool()> multi_ctf_is_blue_flag_in_base_hook{
    0x00474EA0,
    []() -> bool {
        if (!rf::ctf_blue_flag_item) return true;
        return multi_ctf_is_blue_flag_in_base_hook.call_target();
    },
};

FunHook<void()> multi_ctf_level_init_hook{
    0x00472E30,
    []() {
        multi_ctf_level_init_hook.call_target();
        // Make sure CTF flag does not spin in new level if it was dropped in the previous level
        int info_index = rf::item_lookup_type("flag_red");
        if (info_index >= 0) {
            rf::item_info[info_index].flags &= ~rf::IIF_SPINS_IN_MULTI;
        }
        info_index = rf::item_lookup_type("flag_blue");
        if (info_index >= 0) {
            rf::item_info[info_index].flags &= ~rf::IIF_SPINS_IN_MULTI;
        }
    },
};

rf::Timestamp g_select_weapon_done_timestamp[rf::multi_max_player_id];

bool multi_is_selecting_weapon(rf::Player* pp)
{
    auto& done_timestamp = g_select_weapon_done_timestamp[pp->net_data->player_id];
    return done_timestamp.valid() && !done_timestamp.elapsed();
}

void server_set_player_weapon(rf::Player* pp, rf::Entity* ep, int weapon_type)
{
    rf::player_make_weapon_current_selection(pp, weapon_type);
    ep->ai.current_primary_weapon = weapon_type;
    g_select_weapon_done_timestamp[pp->net_data->player_id].set(300);
}

static bool multi_is_rail_gun_on_cooldown(rf::Player* pp, rf::Entity* ep)
{
    if (ep->ai.current_primary_weapon != rf::rail_gun_weapon_type) {
        return false;
    }
    bool fire_cooldown = ep->ai.next_fire_primary.valid() && !ep->ai.next_fire_primary.elapsed();
    bool reloading = pp->rail_gun_reload_timer.valid() && !pp->rail_gun_reload_timer.elapsed();
    return fire_cooldown || reloading;
}

FunHook<void(rf::Player*, rf::Entity*, int)> multi_select_weapon_server_side_hook{
    0x004858D0,
    [](rf::Player *pp, rf::Entity *ep, int weapon_type) {
        if (weapon_type == -1 || ep->ai.current_primary_weapon == weapon_type) {
            // Nothing to do
            return;
        }
        if (g_alpine_server_config_active_rules.gungame.enabled &&
            !((ep->ai.current_primary_weapon == 1 && weapon_type == 0) || (ep->ai.current_primary_weapon == 0 && weapon_type == 1))) {
            // af_send_automated_chat_msg("Weapon switch denied. In GunGame, you get new weapons by getting frags.", pp);
            return;
        }
        bool has_weapon;
        if (weapon_type == rf::remote_charge_det_weapon_type) {
            has_weapon = rf::ai_has_weapon(&ep->ai, rf::remote_charge_weapon_type);
        }
        else {
            has_weapon = rf::ai_has_weapon(&ep->ai, weapon_type);
        }
        if (!has_weapon) {
            xlog::debug("Player {} attempted to select an unpossesed weapon {}", pp->name, weapon_type);
        }
        else if (multi_is_selecting_weapon(pp)) {
            xlog::debug("Player {} attempted to select weapon {} while selecting weapon {}",
                pp->name, weapon_type, ep->ai.current_primary_weapon);
        }
        else if (rf::entity_is_reloading(ep)) {
            xlog::debug("Player {} attempted to select weapon {} while reloading weapon {}",
                pp->name, weapon_type, ep->ai.current_primary_weapon);
        }
        else if (g_alpine_server_config_active_rules.force_rail_reload &&
            multi_is_rail_gun_on_cooldown(pp, ep)) {
            xlog::debug("Player {} attempted to switch from rail_gun while on cooldown", pp->name);
        }
        else {
            rf::player_make_weapon_current_selection(pp, weapon_type);
            ep->ai.current_primary_weapon = weapon_type;
            g_select_weapon_done_timestamp[pp->net_data->player_id].set(300);
        }
    },
};

void multi_reload_weapon_server_side(rf::Player* pp, int weapon_type)
{
    rf::Entity* ep = rf::entity_from_handle(pp->entity_handle);
    if (!ep) {
        // Entity is dead
    }
    else if (ep->ai.current_primary_weapon != weapon_type) {
        xlog::debug("Player {} attempted to reload unselected weapon {}", pp->name, weapon_type);
    }
    else if (multi_is_selecting_weapon(pp)) {
        xlog::debug("Player {} attempted to reload weapon {} while selecting it", pp->name, weapon_type);
    }
    else if (rf::entity_is_reloading(ep)) {
        xlog::debug("Player {} attempted to reload weapon {} while reloading it", pp->name, weapon_type);
    }
    else {
        // action the reload
        bool reloaded = rf::entity_reload_current_primary(ep, false, false);
        if (reloaded && weapon_type == rf::rail_gun_weapon_type) {
            float reload_secs = rf::weapon_types[weapon_type].clip_reload_time_secs;

            // default rail reload time is 3 seconds, but the animation time isn't that long
            // without this fix, the player is blocked from switching away from the rail for
            // 500ms after the reload finishes
            if (reload_secs == 3.0f) {
                reload_secs = 2.5f;
            }

            pp->rail_gun_reload_timer.set(static_cast<int>(reload_secs * 1000.0f));
        }
    }
}

void multi_ensure_ammo_is_not_empty(rf::Entity* ep)
{
    int weapon_type = ep->ai.current_primary_weapon;
    auto& wi = rf::weapon_types[weapon_type];
    // at least ammo for 100 milliseconds
    int min_ammo = std::max(static_cast<int>(0.1f / wi.fire_wait), 1);
    if (rf::weapon_is_melee(weapon_type)) {
        return;
    }
    if (rf::weapon_uses_clip(weapon_type)) {
        auto& clip_ammo = ep->ai.clip_ammo[weapon_type];
        clip_ammo = std::max(clip_ammo, min_ammo);
    }
    else {
        auto& ammo = ep->ai.ammo[wi.ammo_type];
        ammo = std::max(ammo, min_ammo);
    }
}

void multi_turn_weapon_on(rf::Entity* ep, rf::Player* pp, bool alt_fire)
{
    // Note: pp is always null client-side
    auto weapon_type = ep->ai.current_primary_weapon;
    if (!rf::weapon_is_on_off_weapon(weapon_type, alt_fire)) {
        xlog::debug("Player {} attempted to turn on weapon {} which has no continous fire flag", ep->name, weapon_type);
    }
    else if (rf::is_server && multi_is_selecting_weapon(pp)) {
        xlog::debug("Player {} attempted to turn on weapon {} while selecting it", ep->name, weapon_type);
    }
    else if (rf::is_server && rf::entity_is_reloading(ep)) {
        xlog::debug("Player {} attempted to turn on weapon {} while reloading it", ep->name, weapon_type);
    }
    else {
        if (!rf::is_server) {
            // Make sure client-side ammo is not empty when we know that the player is currently shooting
            // It can be empty if a reload packet was lost or if it got desynced because of network delays
            multi_ensure_ammo_is_not_empty(ep);
        }
        rf::entity_turn_weapon_on(ep->handle, weapon_type, alt_fire);
    }
}

void multi_turn_weapon_off(rf::Entity* ep)
{
    auto current_primary_weapon = ep->ai.current_primary_weapon;
    if (rf::weapon_is_on_off_weapon(current_primary_weapon, false)
        || rf::weapon_is_on_off_weapon(current_primary_weapon, true)) {

        rf::entity_turn_weapon_off(ep->handle, current_primary_weapon);
    }
}

bool weapon_uses_ammo(int weapon_type, bool alt_fire)
{
    if (rf::weapon_is_detonator(weapon_type)) {
         return false;
    }
    if (rf::weapon_is_riot_stick(weapon_type) && alt_fire) {
        return true;
    }
    rf::WeaponInfo* winfo = &rf::weapon_types[weapon_type];
    return !(winfo->flags & rf::WTF_MELEE);
}

bool is_entity_out_of_ammo(rf::Entity *entity, int weapon_type, bool alt_fire)
{
    if (!weapon_uses_ammo(weapon_type, alt_fire)) {
        return false;
    }
    rf::WeaponInfo* winfo = &rf::weapon_types[weapon_type];
    if (winfo->clip_size == 0) {
        auto ammo = entity->ai.ammo[winfo->ammo_type];
        return ammo == 0;
    }
    auto clip_ammo = entity->ai.clip_ammo[weapon_type];
    return clip_ammo == 0;
}
void send_private_message_for_cancelled_shot(rf::Player* player, const std::string& reason)
{
    auto message = std::format("\xA6 Shot canceled: {}", reason);
    af_send_automated_chat_msg(message, player);
}

bool multi_is_player_firing_too_fast(const rf::Player* const pp, const int weapon_type) {
    // do not consider melee weapons for click limiter
    if (rf::weapon_is_melee(weapon_type)) {
        return false;
    }

    int fire_wait_ms = 0;
    if (rf::weapon_is_semi_automatic(weapon_type)) {
        // if semi auto click limit is on
        if (get_af_server_info().has_value() && get_af_server_info()->click_limit) {
            // use override value for pistol/PR in stock game
            // stock game pistol has alt fire wait of 200ms, so can't use normal min logic for it
            if (rf::weapon_get_fire_wait_ms(weapon_type, 0) == 500) {
                fire_wait_ms = get_semi_auto_fire_wait_override();
            } else {
                // otherwise use the minimum fire wait between both modes in weapons.tbl
                fire_wait_ms = std::min(
                    rf::weapon_get_fire_wait_ms(weapon_type, 0), // primary
                    rf::weapon_get_fire_wait_ms(weapon_type, 1) // alt
                );
            }
        } else {
            // otherwise, don't enforce a limit for semi autos
            fire_wait_ms = 0;
        }
    } else {
        // for automatic weapons, use the minimum fire wait between both modes in weapons.tbl
        fire_wait_ms = std::min(
            rf::weapon_get_fire_wait_ms(weapon_type, 0), // primary
            rf::weapon_get_fire_wait_ms(weapon_type, 1) // alt
        );
    }

    static std::vector<int> last_weapon_id(rf::multi_max_player_id, 0);
    static std::vector<int64_t> last_weapon_fire(rf::multi_max_player_id, 0);

    const int player_id = pp->net_data->player_id;
    const int64_t now = timer::get_i64(1000);

    // If weapon changed, skip interval check.
    if (last_weapon_id[player_id] != weapon_type) {
        last_weapon_id[player_id] = weapon_type;
    } else {
        const int64_t time_since_last_shot = now - last_weapon_fire[player_id];
        // Calculate server-enforced cooldown from weapon fire wait, half of ping,
        // and 50 ms jitter tolerance.
        const int adjusted_ping = std::max(0, pp->net_data->ping);
        const int cooldown_threshold =
            std::max(0, fire_wait_ms - (adjusted_ping / 2) - 50);
        if (time_since_last_shot < cooldown_threshold) {
            // Send notification to player for firing too fast.
            // send_private_message_for_cancelled_shot(pp, "You are firing too fast!");
            return true;
        }
    }

    // We fired.
    last_weapon_fire[player_id] = now;
    return false;
}

bool multi_is_weapon_fire_allowed_server_side(rf::Entity *ep, int weapon_type, bool alt_fire)
{
    rf::Player* pp = rf::player_from_entity_handle(ep->handle);
    if (ep->ai.current_primary_weapon != weapon_type) {
        xlog::debug("Player {} attempted to fire unselected weapon {}", pp->name, weapon_type);
    }
    else if (is_entity_out_of_ammo(ep, weapon_type, alt_fire)) {
        xlog::debug("Player {} attempted to fire weapon {} without ammunition", pp->name, weapon_type);
    }
    else if (rf::weapon_is_on_off_weapon(weapon_type, alt_fire)) {
        xlog::debug("Player {} attempted to fire a single bullet from on/off weapon {}", pp->name, weapon_type);
    }
    else if (multi_is_selecting_weapon(pp)) {
        xlog::debug("Player {} attempted to fire weapon {} while selecting it", pp->name, weapon_type);
    }
    // causes shots fired immediately after reloading to be cancelled (especially noticable with shotgun)
    // is because entity_is_reloading looks at anim length and some anims are longer than the actual reload time
    // todo: make new entity_is_reloading function that calculates based on reload start and duration
    //else if (rf::entity_is_reloading(ep)) {
    //    xlog::debug("Player {} attempted to fire weapon {} while reloading it", pp->name, weapon_type);
    //}
    else if (!multi_is_player_firing_too_fast(pp, weapon_type)) {
        return true;
    }
    return false;
}

FunHook<void(rf::Entity*, int, rf::Vector3&, rf::Matrix3&, bool)> multi_process_remote_weapon_fire_hook{
    0x0047D220,
    [](rf::Entity *ep, int weapon_type, rf::Vector3& pos, rf::Matrix3& orient, bool alt_fire) {
        if (rf::is_server) {
            // Do some checks server-side to prevent cheating
            if (!multi_is_weapon_fire_allowed_server_side(ep, weapon_type, alt_fire)) {
                return;
            }
        }
        multi_process_remote_weapon_fire_hook.call_target(ep, weapon_type, pos, orient, alt_fire);

        // Notify spectate system of weapon fire so the fpgun fire animation is triggered.
        // Skip thrown projectile weapons (grenade, C4, flamethrower canister alt-fire) because
        // their animation is driven earlier and at the correct time by entity_play_attack_anim_spectate_hook.
        if (!rf::is_server) {
            bool is_thrown = (weapon_type == rf::grenade_weapon_type)
                || (weapon_type == rf::remote_charge_weapon_type)
                || (rf::weapon_is_flamethrower(weapon_type) && alt_fire);
            if (!is_thrown) {
                multi_spectate_on_obj_update_fire(ep, alt_fire);
            }
        }
    },
};

void multi_init_player(rf::Player* player)
{
    multi_kill_init_player(player);
}

std::string_view multi_game_type_name(const rf::NetGameType game_type) {
    if (game_type == rf::NG_TYPE_DM) {
        return std::string_view{"Deathmatch"};
    } else if (game_type == rf::NG_TYPE_CTF) {
        return std::string_view{"Capture the Flag"};
    } else if (game_type == rf::NG_TYPE_KOTH) {
        return std::string_view{"King of the Hill"};
    } else if (game_type == rf::NG_TYPE_DC) {
        return std::string_view{"Damage Control"};
    } else if (game_type == rf::NG_TYPE_REV) {
        return std::string_view{"Revolt"};
    } else if (game_type == rf::NG_TYPE_RUN) {
        return std::string_view{"Run"};
    } else if (game_type == rf::NG_TYPE_ESC) {
        return std::string_view{"Escalation"};
    } else if (game_type == rf::NG_TYPE_BAG) {
        return std::string_view{"Bagman"};
    } else if (game_type == rf::NG_TYPE_TBAG) {
        return std::string_view{"Team Bagman"};
    } else if (game_type == rf::NG_TYPE_UNK) {
        return std::string_view{"Unknown"};
    } else {
        if (game_type != rf::NG_TYPE_TEAMDM) {
            xlog::warn("{} is an invalid `NetGameType`", static_cast<int>(game_type));
        }
        return std::string_view{"Team Deathmatch"};
    }
}

std::string_view multi_game_type_name_upper(const rf::NetGameType game_type) {
    if (game_type == rf::NG_TYPE_DM) {
        return std::string_view{rf::strings::deathmatch};
    } else if (game_type == rf::NG_TYPE_CTF) {
        return std::string_view{rf::strings::capture_the_flag};
    } else if (game_type == rf::NG_TYPE_KOTH) {
        return std::string_view{"KING OF THE HILL"};
    } else if (game_type == rf::NG_TYPE_DC) {
        return std::string_view{"DAMAGE CONTROL"};
    } else if (game_type == rf::NG_TYPE_REV) {
        return std::string_view{"REVOLT"};
    } else if (game_type == rf::NG_TYPE_RUN) {
        return std::string_view{"RUN"};
    } else if (game_type == rf::NG_TYPE_ESC) {
        return std::string_view{"ESCALATION"};
    } else if (game_type == rf::NG_TYPE_BAG) {
        return std::string_view{"BAGMAN"};
    } else if (game_type == rf::NG_TYPE_TBAG) {
        return std::string_view{"TEAM BAGMAN"};
    } else if (game_type == rf::NG_TYPE_UNK) {
        return std::string_view{"UNKNOWN"};
    } else {
        if (game_type != rf::NG_TYPE_TEAMDM) {
            xlog::warn("{} is an invalid `NetGameType`", static_cast<int>(game_type));
        }
        return std::string_view{rf::strings::team_deathmatch};
    }
}

std::string_view multi_game_type_name_short(const rf::NetGameType game_type) {
    if (game_type == rf::NG_TYPE_DM) {
        return std::string_view{"DM"};
    } else if (game_type == rf::NG_TYPE_CTF) {
        return std::string_view{"CTF"};
    } else if (game_type == rf::NG_TYPE_KOTH) {
        return std::string_view{"KOTH"};
    } else if (game_type == rf::NG_TYPE_DC) {
        return std::string_view{"DC"};
    } else if (game_type == rf::NG_TYPE_REV) {
        return std::string_view{"REV"};
    } else if (game_type == rf::NG_TYPE_RUN) {
        return std::string_view{"RUN"};
    } else if (game_type == rf::NG_TYPE_ESC) {
        return std::string_view{"ESC"};
    } else if (game_type == rf::NG_TYPE_BAG) {
        return std::string_view{"BAG"};
    } else if (game_type == rf::NG_TYPE_TBAG) {
        return std::string_view{"TBAG"};
    } else if (game_type == rf::NG_TYPE_UNK) {
        return std::string_view{"UNK"};
    } else {
        if (game_type != rf::NG_TYPE_TEAMDM) {
            xlog::warn("{} is an invalid `NetGameType`", static_cast<int>(game_type));
        }
        return std::string_view{"TDM"};
    }
}

std::string_view multi_game_type_prefix(const rf::NetGameType game_type) {
    if (game_type == rf::NG_TYPE_DM) {
        return std::string_view{"dm"};
    } else if (game_type == rf::NG_TYPE_TEAMDM) {
        return std::string_view{"dm"};
    } else if (game_type == rf::NG_TYPE_CTF) {
        return std::string_view{"ctf"};
    } else if (game_type == rf::NG_TYPE_KOTH) {
        return std::string_view{"koth"};
    } else if (game_type == rf::NG_TYPE_DC) {
        return std::string_view{"dc"};
    } else if (game_type == rf::NG_TYPE_REV) {
        return std::string_view{"rev"};
    } else if (game_type == rf::NG_TYPE_RUN) {
        return std::string_view{"run"};
    } else if (game_type == rf::NG_TYPE_ESC) {
        return std::string_view{"esc"};
    } else if (game_type == rf::NG_TYPE_BAG) {
        return std::string_view{"bag"};
    } else if (game_type == rf::NG_TYPE_TBAG) {
        return std::string_view{"tbag"};
    } else if (game_type == rf::NG_TYPE_UNK) {
        // No real level-name prefix for unknown game types; "dm" is the safest fallback.
        return std::string_view{"dm"};
    } else {
        if (game_type != rf::NG_TYPE_TEAMDM) {
            xlog::warn("{} is an invalid `NetGameType`", static_cast<int>(game_type));
        }
        return std::string_view{"dm"};
    }
}

int multi_num_spawned_players() {
    return std::ranges::count_if(SinglyLinkedList{rf::player_list}, [] (const auto& p) {
        return !rf::player_is_dead(&p) && !rf::player_is_dying(&p);
    });
}

void configure_custom_gametype_listen_server_settings() {
    // reset to defaults
    g_alpine_server_config = AlpineServerConfig{};
    g_alpine_server_config_active_rules = AlpineServerConfigRules{};
    set_upcoming_game_type(rf::netgame.type);

    auto& rules = g_alpine_server_config_active_rules;
    rules.game_type = rf::netgame.type;
    apply_defaults_for_game_type(rules.game_type, rules);
    rules.set_koth_score_limit(3600);
    rules.set_dc_score_limit(3600);

    // Enable on listen servers based on the host's local config
    if (rf::game_get_gore_level() >= 2) {
        rules.gibbing.enabled = true;
    }

    // Always allow these visual options on listen servers
    g_alpine_server_config.allow_fullbright_meshes = true;
    g_alpine_server_config.allow_lightmaps_only = true;
    g_alpine_server_config.allow_unlimited_fps = true;
    g_alpine_server_config.allow_outlines = true;
}

void start_level_in_multi(std::string filename) {

    auto [is_valid, valid_filename] = is_level_name_valid(filename);

    if (is_valid) {
        // Clean up any previous game state so the level loader doesn't call game_shutdown
        // after multi_start has already set up the new multiplayer session
        rf::game_shutdown();

        rf::netgame.levels.clear();
        rf::netgame.levels.add(valid_filename.c_str());
        rf::netgame.max_time_seconds = 3600.0f;
        rf::netgame.max_kills = 30;
        rf::netgame.geomod_limit = 64;
        rf::netgame.max_captures = 5;
        rf::netgame.flags = 0; // no broadcast to tracker
        rf::netgame.type = string_istarts_with(filename, "ctf") ? rf::NetGameType::NG_TYPE_CTF
            : string_istarts_with(filename, "koth") ? rf::NetGameType::NG_TYPE_KOTH
            : string_istarts_with(filename, "dc") ? rf::NetGameType::NG_TYPE_DC
            : string_istarts_with(filename, "rev") ? rf::NetGameType::NG_TYPE_REV
            : string_istarts_with(filename, "run") ? rf::NetGameType::NG_TYPE_RUN
            : string_istarts_with(filename, "esc") ? rf::NetGameType::NG_TYPE_ESC
            : rf::NetGameType::NG_TYPE_DM;
        rf::netgame.name = "Alpine Faction Test Server";
        rf::netgame.password = "password";

        configure_custom_gametype_listen_server_settings();

        rf::set_in_mp_flag();
        rf::multi_start(0, 0);
        rf::multi_hud_clear_chat();
        rf::multi_load_next_level();
        rf::multi_init_server();
    }
}

CodeInjection multi_customize_listen_server_settings_patch {
    0x0044E485,
    [](auto& regs) {
        configure_custom_gametype_listen_server_settings();
    },
};

ConsoleCommand2 levelm_cmd{
    "levelm",
    [](std::string filename) {
        if (rf::is_dedicated_server) {
            rf::console::print("This command is not available on dedicated servers");
            return;
        }
        auto [is_valid, valid_filename] = is_level_name_valid(filename);
        if (!is_valid) {
            rf::console::print("Level '{}' is not available!", filename);
            return;
        }
        start_levelm_load_sequence(valid_filename);
        rf::gameseq_set_state(rf::GS_MAIN_MENU, true);
        rf::console::print("Starting local multiplayer game on {}", valid_filename);
    },
    "Start a new local multiplayer game on the specified level",
    "levelm <filename>",
};

DcCommandAlias mapm_cmd{
    "mapm",
    levelm_cmd,
};

ConsoleCommand2 mapver_cmd{
    "dbg_mapver",
    [](std::string filename) {
        // Append .rfl if missing
        if (filename.find(".rfl") == std::string::npos) {
            filename += ".rfl";
        }

        const std::expected map_ver = get_level_file_version(filename);
        if (!map_ver) {
            rf::console::print("Level {} not found.", filename);
        } else {
            std::string version_text;

            if (*map_ver == 175) {
                version_text = "Official - PS2 retail";
            } else if (*map_ver == 180) {
                version_text = "Official - PC retail";
            } else if (*map_ver == 200) {
                version_text = "Community - RF/PF/DF";
            } else if (*map_ver > 0 && *map_ver < 200) {
                version_text = "Official - Internal";
            } else if (*map_ver >= 300) {
                version_text = "Community - Alpine";
            } else {
                version_text = "Unsupported";
            }

            rf::console::print(
                "RFL version for level {} is {} ({}). You can {}load this map.",
                filename,
                *map_ver,
                version_text,
                *map_ver > MAXIMUM_RFL_VERSION ? " NOT" : ""
            );
        }
    },
    "Check the version of a specific level file",
    "dbg_mapver <filename>",
};

ConsoleCommand2 dbg_bot_cmd{
    "dbg_bot",
    [](std::optional<int> enabled) {
        if (enabled.has_value()) {
            g_alpine_game_config.dbg_bot = enabled.value() != 0;
        }
        rf::console::print(
            "Bot debug console logging is {}.",
            g_alpine_game_config.dbg_bot ? "enabled" : "disabled"
        );
    },
    "Toggle bot debug console logging",
    "dbg_bot [0|1]",
};

void mp_send_handicap_request(bool force) {
    if (force || g_alpine_game_config.desired_handicap > 0) {
        af_send_handicap_request(static_cast<uint8_t>(g_alpine_game_config.desired_handicap));
    }
}

ConsoleCommand2 set_handicap_cmd{
    "mp_handicap",
    [](std::optional<int> new_handicap) {
        if (new_handicap) {
            g_alpine_game_config.set_desired_handicap(new_handicap.value());
            mp_send_handicap_request(true);
        }
        rf::console::print("Your desired damage reduction handicap is {}. It will only be applied in servers that support this feature.", g_alpine_game_config.desired_handicap);
    },
    "Set desired multiplayer damage reduction handicap",
};


CallHook<float(int, float, int, int, int, rf::PCollisionOut*, int, bool)> obj_apply_damage_lava_hook{
    {
        0x004212A1,
        0x004212D4
    },
    [](int obj_handle, float damage, int killer_handle, int a4, int damage_type, rf::PCollisionOut* collide_out, int resp_ent_uid, bool force) {
        // use obj_handle for killer_handle on servers so players kill themselves in lava and acid instead of
        // "killed mysteriously" or a random player getting credit for the kill
        // on clients, use killer_handle as passed (-1) so players visually ignite in lava for clients
        int killer_handle_new = rf::is_dedicated_server || rf::is_server ? obj_handle : killer_handle;
        return obj_apply_damage_lava_hook.call_target(obj_handle, damage, killer_handle_new, a4, damage_type, collide_out, resp_ent_uid, force);
    }
};

CallHook<void(const char* filename)> level_cmd_multi_change_level_hook{
    0x00435108,
    [](const char* filename) {
        if (rf::is_multi)
            set_manually_loaded_level(true); // "level" console command
        level_cmd_multi_change_level_hook.call_target(filename);
    }
};

void multi_limbo_just_joined_handle_input(const int key) {
    if (!key) {
        return;
    }
    if (rf::multi_chat_is_say_visible()) {
        rf::multi_chat_say_handle_key(key);
    } else if (key == rf::KEY_ESC) {
        rf::gameseq_push_state(rf::GS_MAIN_MENU, false, false);
    }
}

bool g_multi_limbo_just_joined_req_leave = false;

void multi_limbo_just_joined_do_frame() {
    rf::game_poll(multi_limbo_just_joined_handle_input);

    const int scr_w = rf::gr::screen.max_w;
    const int scr_h = rf::gr::screen.max_h;

    static const int bg_bm = rf::bm::load("demo-gameover.tga", -1, false);

    int bm_w = 0, bm_h = 0;
    rf::bm::get_dimensions(bg_bm, &bm_w, &bm_h);

    rf::gr::set_color(255, 255, 255, 255);
    rf::gr::bitmap_scaled(bg_bm, 0, 0, scr_w, scr_h, 0, 0, bm_w, bm_h);

    rf::multi_hud_render_chat();

    rf::ControlConfig& controls = rf::local_player->settings.controls;
    if (rf::control_config_check_pressed(&controls, rf::CC_ACTION_CHAT, nullptr)) {
        rf::multi_chat_say_show(rf::CHAT_SAY_GLOBAL);
    }

    if (rf::multi_chat_is_say_visible()) {
        rf::multi_chat_say_render();
    }

    const std::string_view text = g_multi_limbo_just_joined_req_leave
        ? "LOADING..."
        : "BETWEEN LEVELS...";
    const auto [text_w, text_h] = rf::gr::get_string_size(text, rf::ui::large_font);

    const int unscaled_text_w = static_cast<int>(text_w / rf::ui::scale_x);
    const int unscaled_text_h = static_cast<int>(text_h / rf::ui::scale_y);

    const int x = (640 - unscaled_text_w) / 2;
    const int y = (480 - unscaled_text_h) / 2 - 64;

    rf::gr::set_color(255, 255, 255, 255);
    rf::gr::string_aligned(
        rf::gr::ALIGN_LEFT,
        static_cast<int>(x * rf::ui::scale_x)
            + static_cast<int>(1.f * rf::ui::scale_x),
        static_cast<int>(y * rf::ui::scale_y),
        text.data(),
        rf::ui::large_font
    );

    if (rf::control_config_check_pressed(&controls, rf::CC_ACTION_MP_STATS, nullptr)) {
        rf::scoreboard_render_internal(true);
    }

    if (g_multi_limbo_just_joined_req_leave) {
        if (!multi_next_level_exists()) {
            rf::gameseq_set_state(rf::GS_MULTI_LEVEL_DOWNLOAD, false);
            multi_level_download_manager_start(rf::level.next_level_filename);
        } else {
            rf::gameseq_set_state(rf::GS_NEW_LEVEL, false);
        }
        g_multi_limbo_just_joined_req_leave = false;
    }
}

CodeInjection rf_do_frame_dim_screen_patch{
    0x004B2E26,
    [] (auto& regs) {
        const rf::GameState state = rf::gameseq_get_state();
        if (state == rf::GS_MULTI_LIMBO_JUST_JOINED) {
            regs.eip = 0x004B2E3F;
        }
    },
};

void multi_do_patch()
{
    rf_do_frame_dim_screen_patch.install();
    multi_limbo_init.install();
    multi_start_injection.install();

    // Fix CTF flag not returning to the base if the other flag was returned when the first one was waiting
    ctf_flag_return_fix.install();

    // Weapon select server-side handling
    multi_select_weapon_server_side_hook.install();

    // Check ammo server-side when handling weapon fire packets
    multi_process_remote_weapon_fire_hook.install();

    // Prevent crash when CTF flag item pointers are NULL
    multi_ctf_is_red_flag_in_base_hook.install();
    multi_ctf_is_blue_flag_in_base_hook.install();
    ctf_flag_capture_red_null_check.install();
    ctf_flag_capture_blue_null_check.install();

    // Make sure CTF flag does not spin in new level if it was dropped in the previous level
    multi_ctf_level_init_hook.install();

    // Set custom listen server settings based on gametype
    multi_customize_listen_server_settings_patch.install();

    multi_kill_do_patch();
    faction_files_do_patch();
    level_download_do_patch();
    network_init();
    multi_tdm_apply_patch();

    level_download_init();
    multi_ban_apply_patch();
    // Improved hitboxes: split capsule with head tracking
    capture_entity_injection.install();
    ix_linesegment_capsule_hook.install();

    // Fix lava damage sometimes being attributed to a player
    obj_apply_damage_lava_hook.install();

    // Flag manually loaded levels from "level" command
    level_cmd_multi_change_level_hook.install();

    // Init cmd line param
    get_url_cmd_line_param();
    get_levelm_cmd_line_param();
    get_bot_cmd_line_param();
    get_debugbot_cmd_line_param();
    get_noquit_cmd_line_param();
    get_awpgen_cmd_line_param();
    g_client_bot_launch_enabled = is_client_bot_requested_from_cmdline() || is_client_debugbot_requested_from_cmdline();
    g_client_bot_debug_render_enabled = is_client_debugbot_requested_from_cmdline();
    g_awpgen_mode = get_awpgen_cmd_line_param().found();
    if (is_headless_mode()) {
        g_alpine_game_config.rendering_enabled = false;
        rf::sound_enabled = false;
    }
    else if (g_client_bot_launch_enabled) {
        g_alpine_game_config.rendering_enabled = true;
    }

    // console commands
    levelm_cmd.register_cmd();
    mapver_cmd.register_cmd();
    mapm_cmd.register_cmd();
    set_handicap_cmd.register_cmd();
    dbg_bot_cmd.register_cmd();
}

void multi_after_full_game_init()
{
    populate_gametype_table();
    if (!handle_bot_cmd_line_params()) {
        return; // bot launch validation failed, process is quitting
    }
    handle_url_param();
    if (!handle_awpgen_param()) {
        handle_levelm_param();
    }
}
