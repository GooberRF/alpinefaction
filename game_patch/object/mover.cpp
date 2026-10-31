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
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <common/mover_path.h>
#include <xlog/xlog.h>
#include "mover.h"
#include "../misc/level.h"
#include "../misc/misc.h"
#include "../rf/os/frametime.h"
#include "../rf/event.h"
#include "../rf/mover.h"
#include "../rf/object.h"
#include "../rf/parse.h"
#include "../rf/level.h"
#include "../rf/sound/sound.h"

namespace mpath = mover_path;

// Forward declarations (used by hold open init before definitions)
static inline rf::MoverKeyframe* KF(const rf::Mover* m, int i);
static inline int count_keyframes(const rf::Mover* m);

// Hold Open runtime tracking — populated after level load from first-keyframe UIDs
static std::unordered_set<int> g_hold_open_handles;
static bool g_hold_open_needs_init = false;

void alpine_mover_clear_hold_open()
{
    g_hold_open_handles.clear();
    g_hold_open_needs_init = true;
}

static void alpine_mover_do_hold_open_init()
{
    g_hold_open_needs_init = false;
    g_hold_open_handles.clear();
    const auto& kf_uids = AlpineLevelProperties::instance().hold_open_keyframe_uids;
    if (kf_uids.empty())
        return;

    // Hold Open data can only exist in v304+ levels with an Alpine props chunk
    if (!rfl_version_minimum(304))
        return;

    // Resolve keyframe UIDs to mover handles by matching against each mover's first keyframe UID
    std::unordered_set<int32_t> target_uids(kf_uids.begin(), kf_uids.end());
    for (rf::Object* obj = rf::object_list.next_obj; obj != &rf::object_list; obj = obj->next_obj) {
        if (obj->type != rf::OT_MOVER)
            continue;

        auto* mover = static_cast<rf::Mover*>(obj);
        if (count_keyframes(mover) <= 0)
            continue;
        auto* first_kf = KF(mover, 0);
        if (!first_kf)
            continue;

        if (target_uids.count(first_kf->uid)) {
            g_hold_open_handles.insert(mover->handle);
        }
    }
}


bool alpine_mover_holds_open(const rf::Mover* mp)
{
    return mp && g_hold_open_handles.count(mp->handle) > 0;
}

static std::unordered_map<int32_t, mpath::KeyframeData> g_mover_path_keyframes;

namespace
{
struct MoverPathEntry
{
    mpath::Curve curve;
    bool curved = false;
    uint8_t ease = 0;
};

// By first keyframe uid. reference is the pose in which the mover's members are where the level placed them.
struct MoverOrientEntry
{
    mpath::OrientMode mode = mpath::OrientMode::none;
    mpath::Mat3 reference;
};

struct MoverSegmentPath
{
    const mpath::Curve* curve = nullptr;
    bool reversed = false;
};
} // namespace

static std::unordered_map<int32_t, MoverPathEntry> g_mover_path_entries;

static std::unordered_map<int32_t, uint8_t> g_mover_path_orient_modes;
static std::unordered_map<int32_t, MoverOrientEntry> g_mover_orient_entries;
static bool g_mover_path_needs_init = false;

void alpine_mover_path_clear_state()
{
    g_mover_path_keyframes.clear();
    g_mover_path_entries.clear();
    g_mover_path_orient_modes.clear();
    g_mover_orient_entries.clear();
    g_mover_path_needs_init = true;
}

void alpine_mover_path_load_chunk(rf::File& file, std::size_t chunk_len)
{
    std::size_t remaining = chunk_len;
    rf::File::ChunkGuard chunk_guard{file, remaining};
    AlpineChunkReader reader{file, remaining};

    const bool complete = mpath::read_chunk(
        reader, [](int32_t uid, uint8_t orient_mode) { g_mover_path_orient_modes[uid] = orient_mode; },
        [](int32_t uid, const mpath::KeyframeData& data) { g_mover_path_keyframes[uid] = data; });
    if (!complete) {
        xlog::warn("[Mover] Mover path chunk is damaged (len={})", chunk_len);
    }
}

static mpath::Mat3 mover_keyframe_orient(const rf::MoverKeyframe* kf)
{
    const rf::Matrix3& m = kf->orient;
    return {{m.rvec.x, m.rvec.y, m.rvec.z}, {m.uvec.x, m.uvec.y, m.uvec.z}, {m.fvec.x, m.fvec.y, m.fvec.z}};
}

static mpath::Vec3 mover_path_direction(const rf::Mover* mp, const mpath::PathSpot& spot, int from, int to)
{
    const mpath::Curve* curve = nullptr;
    if (spot.owner >= 0) {
        if (const auto it = g_mover_path_entries.find(KF(mp, spot.owner)->uid);
            it != g_mover_path_entries.end() && it->second.curved) {
            curve = &it->second.curve;
        }
    }
    return mpath::follow_direction(spot, from, to, count_keyframes(mp), curve, [mp](int i) {
        const rf::Vector3& pos = KF(mp, i)->pos;
        return mpath::Vec3{pos.x, pos.y, pos.z};
    });
}

static void alpine_mover_do_path_init()
{
    g_mover_path_needs_init = false;
    if ((g_mover_path_keyframes.empty() && g_mover_path_orient_modes.empty())
        || AlpineLevelProperties::instance().legacy_movers) {
        return;
    }

    for (rf::Object* obj = rf::object_list.next_obj; obj != &rf::object_list; obj = obj->next_obj) {
        if (obj->type != rf::OT_MOVER) {
            continue;
        }
        auto* mover = static_cast<rf::Mover*>(obj);
        const int count = count_keyframes(mover);
        std::vector<mpath::Key> keys(count);
        for (int i = 0; i < count; i++) {
            const rf::MoverKeyframe* kf = KF(mover, i);
            keys[i].pos = {kf->pos.x, kf->pos.y, kf->pos.z};
            if (const auto it = g_mover_path_keyframes.find(kf->uid); it != g_mover_path_keyframes.end()) {
                keys[i].data = it->second;
            }
        }

        const auto type = static_cast<mpath::MoveType>(mover->keyframe_move_type);
        const bool closing_segment = mpath::has_closing_segment(type, count);
        const bool paths = !rf::mover_rotates_in_place(mover);
        mpath::resolve_handles(keys, closing_segment);
        for (int i = 0; i < count; i++) {
            const bool has_segment = i < count - 1 || closing_segment;
            const bool curved = paths && has_segment
                && keys[i].data.segment_mode == static_cast<uint8_t>(mpath::SegmentMode::curved);
            if (curved || keys[i].data.ease != static_cast<uint8_t>(mpath::Ease::none)) {
                MoverPathEntry& entry = g_mover_path_entries[KF(mover, i)->uid];
                entry.curved = curved;
                entry.ease = keys[i].data.ease;
                if (curved) {
                    entry.curve = mpath::make_segment_curve(keys, i);
                }
            }
        }

        const auto orient_mode = count > 1 ? g_mover_path_orient_modes.find(KF(mover, 0)->uid)
                                           : g_mover_path_orient_modes.end();
        if (paths && orient_mode != g_mover_path_orient_modes.end()
            && orient_mode->second != static_cast<uint8_t>(mpath::OrientMode::none)) {
            MoverOrientEntry entry;
            entry.mode = static_cast<mpath::OrientMode>(orient_mode->second);
            entry.reference = entry.mode == mpath::OrientMode::follow_path
                ? mpath::facing(mover_path_direction(mover, {0, 0.0f}, 0, 0))
                : mover_keyframe_orient(KF(mover, 0));
            g_mover_orient_entries[KF(mover, 0)->uid] = entry;
        }
    }
}

static MoverSegmentPath mover_segment_path(const rf::Mover* mp, int from, int to)
{
    if (g_mover_path_entries.empty()) {
        return {};
    }
    const int count = count_keyframes(mp);
    const auto type = static_cast<mpath::MoveType>(mp->keyframe_move_type);
    const int owner = mpath::segment_owner(from, to, count, mpath::has_closing_segment(type, count));
    if (owner < 0) {
        return {};
    }
    const auto it = g_mover_path_entries.find(KF(mp, owner)->uid);
    if (it == g_mover_path_entries.end() || !it->second.curved) {
        return {};
    }
    return {&it->second.curve, owner != from};
}

static uint8_t mover_keyframe_ease(const rf::MoverKeyframe* kf)
{
    if (g_mover_path_entries.empty()) {
        return 0;
    }
    const auto it = g_mover_path_entries.find(kf->uid);
    return it != g_mover_path_entries.end() ? it->second.ease : 0;
}

static float mover_segment_length(const MoverSegmentPath& path, const rf::MoverKeyframe* kfA,
                                  const rf::MoverKeyframe* kfB)
{
    return path.curve ? path.curve->length : rf::vec_dist(&kfA->pos, &kfB->pos);
}

rf::Mover* mover_find_by_mover_brush(const rf::MoverBrush* mover_brush)
{
    if (!mover_brush) {
        return nullptr;
    }

    const int target_brush_handle = mover_brush->handle;
    if (target_brush_handle < 0) {
        return nullptr;
    }

    for (rf::Object* obj = rf::object_list.next_obj; obj != &rf::object_list; obj = obj->next_obj) {
        if (obj->type != rf::OT_MOVER) {
            continue;
        }

        auto* mover = static_cast<rf::Mover*>(obj);
        if (std::any_of(mover->brush_handles.begin(), mover->brush_handles.end(),
                        [target_brush_handle](int brush_handle) { return brush_handle == target_brush_handle; })) {
            return mover;
        }
    }

    return nullptr;
}

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
    float& travel_param, float& travel, rf::Vector3& delta, float& dist, MoverSegmentPath& path)
{
    if (!mp)
        return false;

    const int count = count_keyframes(mp);
    if (count < 2)
        return false;

    // Must be a door (or Hold Open), must be obstructed by an entity
    if ((!rf::mover_is_door(mp) && !alpine_mover_holds_open(mp)) ||
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

    const bool time_is_velocity = (mp->mover_flags & rf::MoverFlags::MF_USE_TRAV_TIME_AS_SPD) != 0;
    const uint8_t old_ease = mover_keyframe_ease(kfA);
    float old_accel = kfA->ramp_up_time_seconds;
    float old_decel = kfA->ramp_down_time_seconds;
    mpath::fit_ramps(old_T, time_is_velocity, old_accel, old_decel);

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
    path = mover_segment_path(mp, from, to);
    dist = mover_segment_length(path, kfA, kfB);

    const bool dir_forward = (mp->mover_flags & rf::MoverFlags::MF_DIR_FORWARD) != 0;

    if (dir_forward) {
        travel_param = kfA->forward_time_seconds;
    }
    else {
        travel_param = kfA->reverse_time_seconds;
    }

    travel = mpath::travel_seconds(travel_param, dist, time_is_velocity);

    // Put us on the mirrored segment
    mp->travel_time_seconds = mirrored_frac * travel;

    // Mirrored time only lands on the same spot if both directions share one speed profile
    const uint8_t new_ease = mover_keyframe_ease(kfA);
    if (old_ease || new_ease) {
        const float s = mpath::segment_progress(old_t, old_T, old_accel, old_decel, old_ease);

        float accel = kfA->ramp_up_time_seconds;
        float decel = kfA->ramp_down_time_seconds;
        mpath::fit_ramps(travel, time_is_velocity, accel, decel);
        mp->travel_time_seconds = mpath::time_at_progress(1.0f - s, travel, accel, decel, new_ease);
    }

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

    const auto type = static_cast<mpath::MoveType>(mp->keyframe_move_type);
    if (type < mpath::MoveType::one_way || type > mpath::MoveType::lift)
        return;

    const mpath::NextStep step = mpath::next_keyframe(type, count, from, cur, mover_forward(mp));
    if (step.forward) {
        mover_set_forward(mp);
    }
    else {
        mover_set_backward(mp);
    }
    if (step.clear_stop_completely) {
        mp->stop_completely_at_keyframe = -1;
    }

    mp->stop_at_keyframe = step.next;
    mover_pause_at_kf(mp, pause);
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
    MoverSegmentPath path = mover_segment_path(mp, from, to);
    float dist = mover_segment_length(path, kfA, kfB);

    const bool use_travel_time_as_velocity = (mp->mover_flags & rf::MoverFlags::MF_USE_TRAV_TIME_AS_SPD) != 0;
    float travel = mpath::travel_seconds(travel_param, dist, use_travel_time_as_velocity);

    bool bounced_this_frame =
        alpine_mover_try_bounce_door_mid_segment(mp, from, to, kfA, kfB, travel_param, travel, delta, dist, path);

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

    mpath::fit_ramps(travel, use_travel_time_as_velocity, accel, decel);

    // Calculate and set mover properties for this frame
    float s = 0.0f;
    float v_norm = 0.0f;
    mpath::trapezoid(mp->travel_time_seconds, travel, accel, decel, s, v_norm);
    mpath::apply_ease(mover_keyframe_ease(kfA), s, v_norm);
    mp->dist_travelled = dist * s; // s = path fraction
    mp->cur_vel = dist * v_norm;
    if (path.curve) {
        const mpath::Vec3 p = mpath::sample(*path.curve, mpath::directed_param(*path.curve, s, path.reversed));
        mp->p_data.next_pos = rf::Vector3{p.x, p.y, p.z};
    }
    else {
        const rf::Vector3 pos{kfA->pos.x + delta.x * s, kfA->pos.y + delta.y * s, kfA->pos.z + delta.z * s};
        mp->p_data.next_pos = pos;
    }

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
            // zero velocity so mover_interpolate_objects doesn't impart stale velocity to riders
            mp->cur_vel = 0.0f;
            mp->p_data.vel = rf::Vector3{0.0f, 0.0f, 0.0f};
            if (!mover_paused_at_keyframe(mp)) {
                rf::mover_play_stop_sound(mp);
            }
        }

        rf::mover_update_item_status(mp);
    }
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
        if (mp->start_at_keyframe == 0 && mp->stop_at_keyframe == -1
            && !alpine_mover_holds_open(mp))
            door_room[0x40] = 1;
    }

    const auto mover_flags = mp->mover_flags;

    if (mover_flags & rf::MoverFlags::MF_PAUSED) {
        // treat mover as active with zero velocity
        // allows mover_interpolate_objects to ensure child brushes/objects also have their vel zeroed when paused
        // otherwise they would keep their last frame vel and players standing on them would be pushed
        mp->mover_flags = static_cast<rf::MoverFlags>(mover_flags | (rf::MoverFlags::MF_PROCESSED_THIS_FRAME | rf::MoverFlags::MF_UNK_4000));
        mp->obj_flags = mp->obj_flags | rf::OF_WAS_TELEPORTED;

        const int loop_instance = mp->sound_instances[1];
        if (loop_instance != -1) {
            rf::snd_change_3d(loop_instance, mp->pos, rf::zero_vector, 1.0f);
        }

        mp->p_data.vel = rf::Vector3{0.0f, 0.0f, 0.0f};
        mp->cur_vel = 0.0f;

        return;
    }

    if (!mover_is_moving(mp)) {
        // same treatment as MF_PAUSED: set flags so mover_interpolate_objects
        // processes this mover with zero velocity, preventing riders from
        // retaining stale velocity from the last moving frame
        mp->mover_flags = static_cast<rf::MoverFlags>(mover_flags | (rf::MoverFlags::MF_PROCESSED_THIS_FRAME | rf::MoverFlags::MF_UNK_4000));
        mp->obj_flags = mp->obj_flags | rf::OF_WAS_TELEPORTED;
        mp->cur_vel = 0.0f;
        // pin position to current keyframe so next_pos doesn't remain stale
        const int cur = mp->start_at_keyframe;
        if (cur >= 0 && cur < count)
            mp->p_data.next_pos = KF(mp, cur)->pos;
        return;
    }

    // normal movement
    mp->mover_flags = static_cast<rf::MoverFlags>(mover_flags | (rf::MoverFlags::MF_PROCESSED_THIS_FRAME | rf::MoverFlags::MF_UNK_4000));
    mp->obj_flags = mp->obj_flags | rf::OF_WAS_TELEPORTED;

    const int loop_instance = mp->sound_instances[1];
    if (loop_instance != -1) {
        rf::snd_change_3d(loop_instance, mp->pos, rf::zero_vector, 1.0f);
    }

    // handle pause time
    if (mover_paused_at_keyframe(mp)) {
        const bool blocked = ((rf::mover_is_door(mp) || alpine_mover_holds_open(mp))
                               && rf::mover_is_obstructed_by_entity(mp));

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

        // Deferred hold-open init runs once after level load, regardless of legacy mode
        if (g_hold_open_needs_init) {
            alpine_mover_do_hold_open_init();
        }
        if (g_mover_path_needs_init) {
            alpine_mover_do_path_init();
        }

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
        mpath::trapezoid(t, T, accel, decel, s, v_norm);
        mpath::apply_ease(mover_keyframe_ease(kf0), s, v_norm);

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

// These modes turn the whole mover about its first keyframe, so its members stay together.
CodeInjection mover_interpolate_objects_path_orient_patch{
    0x0046BF2E,
    [](auto& regs) {
        if (g_mover_orient_entries.empty()) {
            return;
        }
        rf::Mover* mp = regs.esi;
        rf::Object* child = regs.edi;
        const int count = count_keyframes(mp);
        const auto it = count > 1 ? g_mover_orient_entries.find(KF(mp, 0)->uid) : g_mover_orient_entries.end();
        const int from = mp->start_at_keyframe;
        const int to = mp->stop_at_keyframe;
        if (it == g_mover_orient_entries.end() || from < 0 || from >= count || to >= count) {
            return;
        }

        const MoverOrientEntry& entry = it->second;
        const MoverSegmentPath path = to >= 0 ? mover_segment_path(mp, from, to) : MoverSegmentPath{};
        // From the clock, as a segment between coincident keyframes has no distance to measure
        float s = 0.0f;
        if (to >= 0) {
            const rf::MoverKeyframe* kfA = KF(mp, from);
            const bool time_is_velocity = (mp->mover_flags & rf::MoverFlags::MF_USE_TRAV_TIME_AS_SPD) != 0;
            const float travel_param = mover_forward(mp) ? kfA->forward_time_seconds : kfA->reverse_time_seconds;
            const float dist = mover_segment_length(path, kfA, KF(mp, to));
            const float travel = mpath::travel_seconds(travel_param, dist, time_is_velocity);
            float accel = kfA->ramp_up_time_seconds;
            float decel = kfA->ramp_down_time_seconds;
            mpath::fit_ramps(travel, time_is_velocity, accel, decel);
            s = mpath::segment_progress(mp->travel_time_seconds, travel, accel, decel, mover_keyframe_ease(kfA));
        }

        mpath::Mat3 pose;
        if (entry.mode == mpath::OrientMode::follow_path) {
            const auto type = static_cast<mpath::MoveType>(mp->keyframe_move_type);
            const bool closing_segment = mpath::has_closing_segment(type, count);
            const mpath::PathSpot spot = mpath::path_spot(from, to, s, count, closing_segment);
            pose = mpath::facing(mover_path_direction(mp, spot, from, to));
        }
        else {
            pose = to >= 0 ? mpath::slerp(mover_keyframe_orient(KF(mp, from)), mover_keyframe_orient(KF(mp, to)), s)
                           : mover_keyframe_orient(KF(mp, from));
        }

        const rf::Vector3& kf0_pos = KF(mp, 0)->pos;
        const mpath::Vec3 rest{child->start_pos.x - kf0_pos.x, child->start_pos.y - kf0_pos.y,
                               child->start_pos.z - kf0_pos.z};
        const mpath::Vec3 turned = mpath::carry(entry.reference, pose, rest) - rest;
        auto& target = addr_as_ref<rf::Vector3>(regs.esp + 0x24);
        target.x += turned.x;
        target.y += turned.y;
        target.z += turned.z;

        const rf::Matrix3& start = child->start_orient;
        const mpath::Mat3 orient = mpath::carry(entry.reference, pose, mpath::Mat3{
            {start.rvec.x, start.rvec.y, start.rvec.z},
            {start.uvec.x, start.uvec.y, start.uvec.z},
            {start.fvec.x, start.fvec.y, start.fvec.z}});
        auto& out = addr_as_ref<rf::Matrix3>(regs.esp + 0x64);
        out.rvec = {orient.rvec.x, orient.rvec.y, orient.rvec.z};
        out.uvec = {orient.uvec.x, orient.uvec.y, orient.uvec.z};
        out.fvec = {orient.fvec.x, orient.fvec.y, orient.fvec.z};
        regs.eip = 0x0046BF89;
    },
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

// Stock mover_process_pre: after mover_is_door() returns false at 0x00469B99,
// also check alpine_mover_holds_open() to allow bounce-back for Hold Open movers.
// Stock code: CALL mover_is_door; TEST AL,AL; JZ skip_bounce
// We inject at the JZ to override: if mover_is_door was false but holds_open is true, don't skip.
// Stock mover_process_pre door bounce: the sequence at 0x00469B8E is:
//   PUSH ESI; CALL mover_is_door; ADD ESP,4; TEST AL,AL; JZ skip_bounce
// The short JZ (0x74) can't be hooked directly, so we inject at the PUSH ESI.
// If alpine_mover_holds_open is true, skip straight to the bounce checks (0x469B9B).
// Otherwise, fall through to the trampoline which runs the stock mover_is_door path.
CodeInjection mover_stock_door_bounce_hold_open_patch{
    0x00469B8E,
    [](auto& regs) {
        rf::Mover* mp = regs.esi;
        if (alpine_mover_holds_open(mp)) {
            regs.eip = 0x00469B9B; // skip mover_is_door check, proceed with bounce
        }
        // otherwise: trampoline runs stock PUSH ESI + CALL mover_is_door normally
    },
};

// Stock mover_is_obstructed: at 0x0046A297 checks mover_is_door before allowing the
// hold-open obstruction check. Skip the is_door gate for Hold Open movers so they
// stay paused when obstructed (prevents stop sound playing every loop in legacy mode).
// Injection covers PUSH ESI (1 byte) + CALL mover_is_door (5 bytes) = 6 bytes for trampoline.
CodeInjection mover_stock_obstructed_hold_open_patch{
    0x0046A297,
    [](auto& regs) {
        rf::Mover* mp = regs.esi;
        if (alpine_mover_holds_open(mp)) {
            regs.eip = 0x0046A2A8; // skip mover_is_door check, proceed with obstruction logic
        }
    },
};

// Stock mover_play_start_sound: at 0x0046A127 checks mover_is_door to decide close vs open sound.
// For Hold Open movers heading to keyframe 0 (closing), set play_close = true and skip the
// mover_is_door call. Injection covers PUSH ESI (1) + XOR BL,BL (2) + CALL (5) = 8 bytes.
CodeInjection mover_stock_start_sound_hold_open_patch{
    0x0046A127,
    [](auto& regs) {
        rf::Mover* mp = regs.esi;
        if (alpine_mover_holds_open(mp) && mp->stop_at_keyframe == 0) {
            regs.bl = static_cast<int8_t>(1); // play_close = true
            regs.eip = 0x0046A142;  // skip to rotates_in_place check
        }
    },
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

    mover_interpolate_objects_path_orient_patch.install();

    // Hold Open: patch stock door bounce check to also trigger for Hold Open movers
    mover_stock_door_bounce_hold_open_patch.install();

    // Hold Open: allow obstruction check for Hold Open movers in legacy mode
    mover_stock_obstructed_hold_open_patch.install();

    // Hold Open: play close sound when closing, just like doors
    mover_stock_start_sound_hold_open_patch.install();
}
