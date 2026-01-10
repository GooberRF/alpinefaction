#include <patch_common/AsmOpcodes.h>
#include <patch_common/AsmWriter.h>
#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/FunHook.h>
#include <cstring>
#include <iostream>
#include <fstream>
#include <string>
#include <algorithm>
#include <sstream>
#include "mover.h"
#include "../misc/level.h"
#include "../rf/os/frametime.h"
#include "../rf/event.h"
#include "../rf/mover.h"
#include "../rf/parse.h"
#include "../rf/level.h"

CodeInjection mover_rotating_keyframe_oob_crashfix{
    0x0046A559,
    [](auto& regs) {
        float& unk_time = *reinterpret_cast<float*>(regs.esi + 0x308);
        unk_time = 0;
        regs.eip = 0x0046A89D;
    }
};

static inline rf::MoverKeyframe* KF(const rf::Mover* m, int i)
{
    return m->keyframes[i];
}

static inline int count_keyframes(const rf::Mover* m)
{
    return m->keyframes.size();
}

static inline bool mover_forward(const rf::Mover* m)
{
    return (m->mover_flags & rf::MoverFlags::MF_DIR_FORWARD) != 0;
}

static inline void mover_set_forward(rf::Mover* m)
{
    m->mover_flags = static_cast<rf::MoverFlags>(
        m->mover_flags | rf::MoverFlags::MF_DIR_FORWARD
    );
}

static inline void mover_set_backward(rf::Mover* m)
{
    m->mover_flags = static_cast<rf::MoverFlags>(
        m->mover_flags & ~rf::MoverFlags::MF_DIR_FORWARD
    );
}

static inline bool mover_paused_at_keyframe(const rf::Mover* m)
{
    return (m->mover_flags & rf::MoverFlags::MF_PAUSED_AT_KEYFRAME) != 0;
}

static inline bool mover_paused_with_flag(const rf::Mover* m)
{
    return (m->mover_flags & rf::MoverFlags::MF_PAUSED) != 0;
}

static inline void mover_pause_at_kf(rf::Mover* m, float seconds)
{
    if (!m)
        return;

    if (seconds <= 0.0f) {
        m->mover_flags = static_cast<rf::MoverFlags>(m->mover_flags & ~rf::MoverFlags::MF_PAUSED_AT_KEYFRAME);
        m->wait_timestamp.invalidate();
        return;
    }

    const int ms = static_cast<int>(seconds * 1000.0f + 0.5f);
    m->wait_timestamp.set(ms);
    m->mover_flags = static_cast<rf::MoverFlags>(m->mover_flags | rf::MoverFlags::MF_PAUSED_AT_KEYFRAME);
    m->cur_vel = 0.0f;

    rf::mover_play_stop_sound(m);
}

static inline void mover_clear_pause(rf::Mover* m)
{
    if (!m)
        return;

    m->mover_flags = static_cast<rf::MoverFlags>(m->mover_flags & ~rf::MoverFlags::MF_PAUSED_AT_KEYFRAME);
    m->wait_timestamp.invalidate();
}

static inline bool mover_is_moving(const rf::Mover* m)
{
    return (m->stop_at_keyframe != -1);
}

static bool alpine_mover_try_bounce_door_mid_segment(
    rf::Mover* mp, int& from, int& to, rf::MoverKeyframe*& kfA, rf::MoverKeyframe*& kfB,
    float& travel_param, float& travel, rf::Vector3& delta, float& dist_sq, float& dist)
{
    if (!mp)
        return false;

    const int count = count_keyframes(mp);
    if (count < 2)
        return false;

    // Must be a door, must be obstructed by an entity
    if (!rf::mover_is_door(mp) ||
        mp->keyframe_move_type == rf::MoverKeyframeMoveType::MKMT_ONE_WAY ||
        to != 0 ||
        !rf::mover_is_obstructed_by_entity(mp))
    {
        return false;
    }

    // Ensure current movement segment is valid
    const float old_T = travel;
    if (old_T <= 0.0f)
        return false;

    // Where are we along the segment?
    const float old_t = mp->travel_time_seconds;
    const float frac = std::clamp(old_t / old_T, 0.0f, 1.0f);

    // Mirror the progress when bouncing
    const float mirrored_frac = 1.0f - frac;

    // Go back towards door open state
    std::swap(mp->start_at_keyframe, mp->stop_at_keyframe);
    mover_set_forward(mp);

    from = mp->start_at_keyframe;
    to = mp->stop_at_keyframe;

    if (from < 0 || to < 0 || from >= count || to >= count || from == to)
        return false;

    kfA = KF(mp, from);
    kfB = KF(mp, to);
    if (!kfA || !kfB)
        return false;

    // Rebuild segment geometry
    delta = kfB->pos - kfA->pos;
    dist_sq = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
    dist = (dist_sq > 0.0f) ? std::sqrt(dist_sq) : 0.0f;

    const bool dir_forward = (mp->mover_flags & rf::MoverFlags::MF_DIR_FORWARD) != 0;

    if (dir_forward) {
        travel_param = kfA->forward_time_seconds;
    }
    else {
        travel_param = kfA->reverse_time_seconds;
    }

    travel = 0.001f; // fallback value

    if (travel_param > 0.0f) {
        if (mp->mover_flags & rf::MoverFlags::MF_USE_TRAV_TIME_AS_SPD) {
            if (dist > 0.0f)
                travel = dist / travel_param; // travel time treated as velocity
        }
        else {
            travel = travel_param;
        }
    }

    // Put us on the mirrored segment
    mp->travel_time_seconds = mirrored_frac * travel;

    return true;
}

// t = elapsed sec along this segment
// T = total travel time (sec) for this segment
static float alpine_trans_progress_trapezoid(float t, float T, float accel, float decel)
{
    if (T <= 0.0f)
        return 1.0f;
    if (t <= 0.0f)
        return 0.0f;
    if (t >= T)
        return 1.0f;

    if (accel <= 0.0f && decel <= 0.0f)
        return t / T;

    const float v_max_denom = T - 0.5f * (accel + decel);

    // If accel+decel is too large, fall back to linear
    if (v_max_denom <= 0.0f)
        return t / T;

    const float v_max = 1.0f / v_max_denom;
    const float cruise_end = T - decel;

    if (t < accel) {
        // quadratic accelerate
        return 0.5f * v_max * (t * t / accel);
    }

    if (t < cruise_end) {
        // cruise: full speed, constant slope
        const float area_accel = 0.5f * v_max * accel;
        return area_accel + v_max * (t - accel);
    }

    // quadratic decelerate
    const float u = T - t;
    const float remaining = 0.5f * v_max * (u * u / decel);
    return 1.0f - remaining;
}

// out_s = [0-1] progress
// out_v_norm = normalized speed, integral over [0,T] == 1
static void alpine_trans_trapezoid(float t, float T, float accel, float decel, float& out_s, float& out_v_norm)
{
    if (T <= 0.0f) {
        out_s = 1.0f;
        out_v_norm = 0.0f;
        return;
    }
    if (t <= 0.0f) {
        out_s = 0.0f;
        out_v_norm = 0.0f;
        return;
    }
    if (t >= T) {
        out_s = 1.0f;
        out_v_norm = 0.0f;
        return;
    }

    if (accel <= 0.0f && decel <= 0.0f) {
        // linear case
        out_s = t / T;
        out_v_norm = 1.0f / T; // constant speed
        return;
    }

    const float v_max_denom = T - 0.5f * (accel + decel);

    // If accel+decel is too large, fall back to linear
    if (v_max_denom <= 0.0f) {
        out_s = t / T;
        out_v_norm = 1.0f / T;
        return;
    }

    const float v_max = 1.0f / v_max_denom; // normalized speed
    const float cruise_end = T - decel;
    const float area_accel = 0.5f * v_max * accel;

    // accelerating
    if (t < accel) {
        const float a = v_max / accel; // normalized acceleration
        out_v_norm = a * t;
        out_s      = 0.5f * a * t * t;
        return;
    }
    // cruise
    if (t < cruise_end) {
        out_v_norm = v_max;
        out_s      = area_accel + v_max * (t - accel);
        return;
    }

    // decelerating
    const float u = T - t;
    const float a = v_max / decel; // normalized deceleration
    out_v_norm = a * u;

    const float area_tail = 0.5f * v_max * (u * u / decel);
    out_s = 1.0f - area_tail;
}


void alpine_mover_reached_keyframe(rf::Mover* mp)
{
    if (!mp)
        return;

    const int count = count_keyframes(mp);
    if (count < 2)
        return;

    const int from = mp->start_at_keyframe; // segment we just finished
    const int cur  = mp->stop_at_keyframe; // arrival keyframe

    if (cur < 0 || cur >= count || from < 0 || from >= count) {
        mp->stop_at_keyframe = -1;
        return;
    }

    const int first = 0;
    const int last  = count - 1;

    // Trigger event by UID
    rf::MoverKeyframe* kf_cur = KF(mp, cur);
    if (auto ev = rf::event_lookup_from_uid(kf_cur->event_uid)) {
        rf::event_activate_from_trigger(ev->handle, mp->handle, -1);
    }

    // Snap to arrival state
    mp->start_at_keyframe   = cur;
    mp->travel_time_seconds = 0.0f;
    mp->dist_travelled      = 0.0f;

    const float pause = kf_cur->pause_time_seconds;

    // stay open while player is in trigger region
    if (mp->stop_completely_at_keyframe >= 0 && cur == mp->stop_completely_at_keyframe) {
        const int count = count_keyframes(mp);
        const int last = count - 1;

        mp->start_at_keyframe = cur;
        mp->stop_at_keyframe = -1;
        mp->travel_time_seconds = 0.0f;
        mp->dist_travelled = 0.0f;

        rf::mover_play_stop_sound(mp);

        if (cur == last) {
            mover_set_backward(mp); // clear forward
        }
        else if (cur == 0) {
            mover_set_forward(mp); // set forward
        }

        return;
    }

    int next = -1; // default: stop until re-activated

    switch (mp->keyframe_move_type)
    {
        // ONE WAY
        case rf::MoverKeyframeMoveType::MKMT_ONE_WAY:
        {
            const bool forward = mover_forward(mp);

            if (forward) {
                if (cur < last) {
                    // still heading toward last
                    next = cur + 1;
                } else {
                    // reached last, stop; next activation goes backward
                    mover_set_backward(mp);
                    next = -1;
                }
            } else { // backward
                if (cur > first) {
                    next = cur - 1;
                } else {
                    // reached first, stop; next activation goes forward
                    mover_set_forward(mp);
                    next = -1;
                }
            }

            mp->stop_at_keyframe = next;
            mover_pause_at_kf(mp, pause);
            return;
        }

        // PING PONG ONCE
        case rf::MoverKeyframeMoveType::MKMT_PING_PONG_ONCE:
        {
            bool forward = mover_forward(mp);
            const int first = 0;
            const int last = count - 1;

            if (forward) {
                if (cur < last) {
                    // Still on the forward leg: go to next keyframe
                    next = cur + 1;
                }
                else {
                    // Reached last while going forward – start the backward leg
                    mover_set_backward(mp);
                    forward = false;
                    if (count > 1)
                        next = cur - 1;
                }
            }
            else { // backward
                if (cur > first) {
                    // Still going back toward first
                    next = cur - 1;
                }
                else {
                    // complete, stop here
                    next = -1;

                    // reset state so next activation is clean forward
                    mover_set_forward(mp);
                    mp->stop_completely_at_keyframe = -1;
                }
            }

            mp->stop_at_keyframe = next;
            mover_pause_at_kf(mp, pause);
            return;
        }

        // PING PONG INFINITE
        case rf::MoverKeyframeMoveType::MKMT_PING_PONG_INFINITE:
        {
            bool forward = mover_forward(mp);

            if (forward) {
                if (cur < last) {
                    next = cur + 1;
                } else {
                    // reached last while going forward; bounce
                    mover_set_backward(mp);
                    forward = false;
                    if (count > 1)
                        next = cur - 1;
                }
            } else { // backward
                if (cur > first) {
                    next = cur - 1;
                } else {
                    // reached first while going backward; bounce again
                    mover_set_forward(mp);
                    forward = true;
                    if (count > 1)
                        next = cur + 1;
                }
            }

            mp->stop_at_keyframe = next;
            mover_pause_at_kf(mp, pause);
            return;
        }

        // LOOP ONCE
        case rf::MoverKeyframeMoveType::MKMT_LOOP_ONCE:
        {
            const bool forward = mover_forward(mp);

            if (forward) {
                if (from == last && cur == first) {
                    next = -1;
                }
                else {
                    next = (cur < last) ? (cur + 1) : first;
                }
            }
            else { // backward
                if (from == first && cur == last) {
                    next = -1;
                }
                else {
                    next = (cur > first) ? (cur - 1) : last;
                }
            }

            mp->stop_at_keyframe = next;
            mover_pause_at_kf(mp, pause);
            return;
        }

        // LOOP INFINITE
        case rf::MoverKeyframeMoveType::MKMT_LOOP_INFINITE:
        {
            const bool forward = mover_forward(mp);

            if (forward) {
                next = (cur < last) ? (cur + 1) : first;
            }
            else { // backward
                next = (cur > first) ? (cur - 1) : last;
            }

            mp->stop_at_keyframe = next;
            mover_pause_at_kf(mp, pause);
            return;
        }

        // LIFT
        case rf::MoverKeyframeMoveType::MKMT_LIFT:
        {
            // what should the next activation do?
            if (cur == last)
                mover_set_backward(mp);
            else if (cur == first)
                mover_set_forward(mp);

            // finished one segment
            next = -1;

            mp->stop_at_keyframe = next;
            mover_pause_at_kf(mp, pause);
            return;
        }
    }
}

void alpine_mover_process_linear(rf::Mover* mp)
{
    if (!mp)
        return;

    const int count = count_keyframes(mp);
    if (count < 2)
        return;

    // keep current position pinned if not moving
    if (!mover_is_moving(mp)) {
        const int cur = mp->start_at_keyframe;
        if (cur >= 0 && cur < count)
            mp->p_data.next_pos = KF(mp, cur)->pos;
        return;
    }

    int from = mp->start_at_keyframe;
    int to = mp->stop_at_keyframe;

    if (from < 0 || to < 0 || from >= count || to >= count || from == to)
        return;

    rf::MoverKeyframe* kfA = KF(mp, from);
    rf::MoverKeyframe* kfB = KF(mp, to);
    if (!kfA || !kfB)
        return;

    const bool dir_forward = (mp->mover_flags & rf::MoverFlags::MF_DIR_FORWARD) != 0;

    float travel_param = 0.0f;
    if (dir_forward) {
        travel_param = kfA->forward_time_seconds;
    }
    else { // backwards
        travel_param = kfA->reverse_time_seconds;
    }

    // Compute segment distance
    rf::Vector3 delta = kfB->pos - kfA->pos;
    float dist_sq = rf::vec_dist_squared(&kfA->pos, &kfB->pos);
    float dist = rf::vec_dist(&kfA->pos, &kfB->pos);

    float travel = 0.001f; // fallback

    if (travel_param > 0.0f) {
        if (mp->mover_flags & rf::MoverFlags::MF_USE_TRAV_TIME_AS_SPD) {
            // travel_param as velocity
            if (dist > 0.0f)
                travel = dist / travel_param;
        }
        else {
            // travel_param as time
            travel = travel_param;
        }
    }

    bool bounced_this_frame =
        alpine_mover_try_bounce_door_mid_segment(mp, from, to, kfA, kfB, travel_param, travel, delta, dist_sq, dist);

    // Obstructed and has not bounced this frame
    if (!bounced_this_frame && rf::mover_is_obstructed(mp)) {
        const float obstruct_pause = kfB->pause_time_seconds;
        mover_pause_at_kf(mp, obstruct_pause);
        return;
    }

    // Advance elapsed time
    mp->travel_time_seconds += rf::frametime;
    if (mp->travel_time_seconds > travel)
        mp->travel_time_seconds = travel;

    float accel = kfA->ramp_up_time_seconds;
    float decel = kfA->ramp_down_time_seconds;

    // For "Travel Time as Velocity" mode, ensure accel/decel fit within travel
    const bool use_travel_time_as_velocity = (mp->mover_flags & rf::MoverFlags::MF_USE_TRAV_TIME_AS_SPD) != 0;
    if (use_travel_time_as_velocity && travel > 0.0f) {
        const float ramp_sum = accel + decel;
        if (ramp_sum > 0.0f) {
            const float max_ramp_sum = 0.9f * travel;
            if (ramp_sum > max_ramp_sum) {
                const float scale = max_ramp_sum / ramp_sum;
                accel *= scale;
                decel *= scale;
            }
        }
    }

    // Calculate and set mover properties for this frame
    float s = 0.0f;
    float v_norm = 0.0f;
    alpine_trans_trapezoid(mp->travel_time_seconds, travel, accel, decel, s, v_norm);
    mp->dist_travelled = dist * s; // s = path fraction
    mp->cur_vel = dist * v_norm;
    const rf::Vector3 pos{kfA->pos.x + delta.x * s, kfA->pos.y + delta.y * s, kfA->pos.z + delta.z * s};
    mp->p_data.next_pos = pos;

    // Did we reach the end of this segment?
    if (mp->travel_time_seconds >= travel - 1e-4f) {
        // Snap cleanly to destination
        mp->p_data.next_pos = kfB->pos;
        mp->travel_time_seconds = 0.0f;
        mp->dist_travelled = 0.0f;

        const int old_stop = mp->stop_at_keyframe;

        alpine_mover_reached_keyframe(mp);

        // no longer have a next keyframe, fully stop
        if (!mover_is_moving(mp)) {
            if (!mover_paused_at_keyframe(mp)) {
                rf::mover_play_stop_sound(mp);
            }
        }

        rf::mover_update_item_status(mp);
    }
    /* xlog::warn("mover {} seg {} -> {}, dir_fwd={}, use_trav_as_spd={}, "
               "fwdA={}, revA={}, travel_param={}, travel={}, frametime={}",
               mp->handle, from, to, dir_forward, (mp->mover_flags & rf::MoverFlags::MF_USE_TRAV_TIME_AS_SPD) != 0,
               kfA->forward_time_seconds, kfA->reverse_time_seconds, travel_param, travel, rf::frametime);*/
}

static void alpine_mover_process_pre(rf::Mover* mp)
{
    if (!mp)
        return;

    mp->p_data.vel = rf::Vector3{0.0f, 0.0f, 0.0f};

    if (rf::mover_rotates_in_place(mp)) {
        rf::mover_rotating_process_pre(mp);
        return;
    }

    const int count = count_keyframes(mp);

    if (mp->door_room) {
        auto* door_room = static_cast<std::uint8_t*>(mp->door_room);
        door_room[0x40] = 0;
        if (mp->start_at_keyframe == 0 && mp->stop_at_keyframe == -1)
            door_room[0x40] = 1;
    }

    const auto mover_flags = mp->mover_flags;

    if (mover_flags & rf::MoverFlags::MF_PAUSED) {
        // treat mover as active with zero velocity
        // allows mover_interpolate_objects to ensure child brushes/objects also have their vel zeroed when paused
        // otherwise they would keep their last frame vel and players standing on them would be pushed
        mp->mover_flags = static_cast<rf::MoverFlags>(mover_flags | (rf::MoverFlags::MF_UNK_8 | rf::MoverFlags::MF_UNK_4000));

        mp->obj_flags = static_cast<rf::ObjectFlags>(static_cast<int>(mp->obj_flags) | 0x04000000);

        const int loop_instance = mp->sound_instances[1];
        if (loop_instance != -1) {
            rf::snd_change_3d(loop_instance, mp->p_data.pos, rf::zero_vector, 1.0f);
        }

        mp->p_data.vel = rf::Vector3{0.0f, 0.0f, 0.0f};
        mp->cur_vel = 0.0f;

        return;
    }

    if (!mover_is_moving(mp)) {
        return;
    }

    // normal movement
    mp->mover_flags = static_cast<rf::MoverFlags>(mover_flags | (rf::MoverFlags::MF_UNK_8 | rf::MoverFlags::MF_UNK_4000));
    mp->obj_flags = static_cast<rf::ObjectFlags>(static_cast<int>(mp->obj_flags) | 0x04000000);

    const int loop_instance = mp->sound_instances[1];
    if (loop_instance != -1) {
        rf::snd_change_3d(loop_instance, mp->p_data.pos, rf::zero_vector, 1.0f);
    }

    // handle pause time
    if (mover_paused_at_keyframe(mp)) {
        const bool blocked = (rf::mover_is_door(mp) && rf::mover_is_obstructed_by_entity(mp));

        if (blocked || !mp->wait_timestamp.elapsed()) {
            const int cur = mp->start_at_keyframe;
            if (cur >= 0 && cur < count) {
                mp->p_data.next_pos = KF(mp, cur)->pos;
            }
            mp->p_data.vel = rf::Vector3{0.0f, 0.0f, 0.0f};
            mp->cur_vel = 0.0f;
            return;
        }

        mover_clear_pause(mp);
        rf::mover_play_start_sound(mp);
    }

    alpine_mover_process_linear(mp);
}

FunHook<void(rf::Mover*)> mover_process_pre_hook{
    0x00469800,
    [](rf::Mover* mp) {
        if (!mp)
            return;

        if (AlpineLevelProperties::instance().legacy_movers) {
            mover_process_pre_hook.call_target(mp);
            return;
        }

        alpine_mover_process_pre(mp);

        //auto* brush = rf::obj_lookup_from_uid(6711);
        //xlog::warn("flags {:x},{:x}, vel {},{},{}, next_pos {},{},{}", static_cast<uint32_t>(mp->mover_flags), static_cast<uint32_t>(mp->obj_flags),
        //    brush->p_data.vel.x, brush->p_data.vel.y, brush->p_data.vel.z,
        //    brush->p_data.next_pos.x, brush->p_data.next_pos.y, brush->p_data.next_pos.z);
    },
};

CodeInjection mover_rotating_process_pre_accel_patch{
    0x0046A5D8,
    [](auto& regs) {
        if (AlpineLevelProperties::instance().legacy_movers) {
            return; // legacy behaviour
        }

        rf::Mover* mp = regs.esi;
        rf::MoverKeyframe* kf0 = regs.ebp;

        if (!mp || !kf0 || mp->stop_at_keyframe == -1) {
            return;
        }

        const bool forward =
            (mp->mover_flags & rf::MoverFlags::MF_DIR_FORWARD) != 0;

        const float T = forward
            ? kf0->forward_time_seconds
            : kf0->reverse_time_seconds;

        if (T <= 0.0f) {
            return;
        }

        const float t = mp->travel_time_seconds;

        if (t < 0.0f || t >= T) {
            return;
        }

        float accel = std::max(kf0->ramp_up_time_seconds, 0.0f);
        float decel = std::max(kf0->ramp_down_time_seconds, 0.0f);

        float s = 0.0f;
        float v_norm = 0.0f;
        alpine_trans_trapezoid(t, T, accel, decel, s, v_norm);

        s = std::clamp(s, 0.0f, 1.0f);

        const float A = kf0->rotation_angle; // radians for this segment

        // find absolute angle along the arc
        float theta;
        if (forward) {
            theta = s * A;
        } else {
            theta = (1.0f - s) * A;
        }

        mp->rot_cur_pos = theta; // set current angle
    }
};

// rotating mover reached the end of its run
CodeInjection mover_rotating_process_pre_no_snap_patch{
    0x0046A6D9,
    [](auto& regs) {
        if (AlpineLevelProperties::instance().legacy_movers) {
            return; // legacy behaviour
        }

        regs.esp += 0x8;
        regs.eip = 0x0046A708; // skip the snap back
    }
};

CodeInjection mover_rotating_process_pre_ping_pong_patch{
    0x0046A84F,
    [](auto& regs) {
        if (AlpineLevelProperties::instance().legacy_movers) {
            return; // legacy behaviour
        }

        regs.eip = 0x0046A855; // skip original code which erroneously removes MF_DIR_FORWARD flag
    }
};

CodeInjection mover_interpolate_objects_force_orient_trans_patch{
    0x0046BF39, // after check for translation movers with "Force Orient" flag
    [](auto& regs) {
        if (AlpineLevelProperties::instance().legacy_movers) {
            return; // legacy behaviour
        }

        rf::Mover* mp = regs.esi;
        // for translating movers with "Force Orient", stock game code crashes on Vector3::get_substracted for stop_at_keyframe
        // because stop_at_keyframe is -1 before movement begins - we don't need to care about orient here
        // because it is not important until movement starts, so skip it to avoid the crash
        if (mp->stop_at_keyframe < 0 || mp->start_at_keyframe < 0) {
            regs.eip = 0x0046BF89;
            return;
        }
    }
};

CodeInjection mover_interpolate_objects_force_orient_rot_patch{
    0x0046BECA,
    [](auto& regs) {
        if (AlpineLevelProperties::instance().legacy_movers) {
            return; // legacy behaviour
        }

        rf::Mover* mp = regs.esi;
        // rotating movers force the orientation to follow the rotation path by default, so use "Force Orient"
        // to reverse that behavior and force the orientation to stay fixed throughout the movement
        if ((mp->mover_flags & rf::MoverFlags::MF_FORCE_ORIENT) != 0) {
            rf::Matrix3 orient;
            orient.make_identity();
            regs.ecx = reinterpret_cast<int32_t>(&orient);
        }
    }
};

void mover_do_patch()
{
    // Fix crash when skipping cutscene after robot kill in L7S4
    mover_rotating_keyframe_oob_crashfix.install();

    // Alpine translation movers
    mover_process_pre_hook.install();

    // Fix accel/decel and "Loop" mode behaviour for rotating movers
    mover_rotating_process_pre_accel_patch.install();
    mover_rotating_process_pre_no_snap_patch.install();

    // Fix "Ping Pong Infinite" mode behaviour for rotating movers
    mover_rotating_process_pre_ping_pong_patch.install();

    // Make "Force Orient" mover flag work properly
    mover_interpolate_objects_force_orient_trans_patch.install();
    mover_interpolate_objects_force_orient_rot_patch.install();
}
