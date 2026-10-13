#include <patch_common/AsmWriter.h>
#include <patch_common/CallHook.h>
#include <patch_common/FunHook.h>
#include <common/utils/int-utils.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <unordered_map>
#include "obj_interp_history.h"
#include "demo/demo.h"
#include "../rf/entity.h"
#include "../rf/object.h"
#include "../rf/multi.h"
#include "../rf/player/player.h"
#include "../rf/os/timer.h"
#include "../os/os.h"
#include "../rf/math/matrix.h"
#include "../rf/math/vector.h"

// ObjInterp keeps 20 keyframes (500 ms at 40 netfps, 167 ms at 120). All past-tick evaluation of remote
// entities goes through entity_interp_pos_at_tick (0x00484010) / entity_interp_orient_at_tick (0x004842E0):
// the server's hitscan rewind and the client's per-frame interpolation. The hooks below use the ring when
// it covers the tick and, for per-frame evaluation, a deeper per-entity keyframe history otherwise; server
// hitscan rewind keeps the ring's reach (older ticks get the live pose, as in stock).
//
// The engine's playout clock (ObjInterp::frame_advance 0x00483BE0) stepped whole ms at wall-clock rate,
// froze when it ran dry and only recovered via set_next_pos_orient's hard re-anchor. The frame_advance
// hook replaces it with one that slews within +-10% of wall time toward a jitter-derived target, carries
// a sub-ms fraction and extrapolates up to one interval. The insert-time re-anchor is disabled in
// obj_interp_history_init (the hook snaps when far behind instead); the initial anchor is kept and moved
// to the hook's target delay.

constexpr int history_frames = 128; // >= 1 s at 120 netfps

constexpr float clock_max_slew = 0.1f;       // playout clock stays within +-10% of wall time
constexpr float headroom_min = 0.3f;         // x interval: floor on the jitter buffer
constexpr float headroom_per_jitter = 3.0f;  // x measured mean abs arrival deviation
constexpr float headroom_max = 1.0f;         // x interval: cap on the jitter buffer
constexpr float reanchor_margin = 2.2f;      // x interval past the target: snap instead of slewing
constexpr float elapsed_max_ms = 250.0f;     // a hitch longer than this is not played back

float obj_interp_target_delay_ms(float interval_ms, float jitter_ms)
{
    float headroom = std::max(headroom_min * interval_ms, headroom_per_jitter * jitter_ms);
    return interval_ms + std::clamp(headroom, 0.0f, headroom_max * interval_ms);
}

namespace
{

struct RingStats
{
    float interval; // keyframe arrival interval, in ticks
    float jitter;   // mean abs deviation of the arrival gaps, in ticks
};

// In keyframe ticks: arrival gaps are wall-clock, which demo playback scales against sender time
RingStats ring_stats(const rf::ObjInterp* interp)
{
    // Until the ring fills after a Clear(), slot 0 is the first sample, whose gap is always 0; the engine's
    // own average includes it and reads short for the first ~20 keyframes after every spawn
    const int n = interp->num_frames();
    const int first = n > 1 && n < rf::ObjInterp::max_keyframes ? 1 : 0;
    float sum = 0.0f;
    for (int i = first; i < n; ++i)
        sum += static_cast<float>(interp->arrive_time_diff[i]);
    const int count = n - first;
    const float arrival = count > 0 ? sum / static_cast<float>(count) : interp->arrive_time_avg_diff;
    float dev = 0.0f;
    for (int i = first; i < n; ++i)
        dev += std::fabs(static_cast<float>(interp->arrive_time_diff[i]) - arrival);
    const float scale = demo_playback_sim_time_scale();
    float interval = arrival * scale;
    // Right after a spawn the ring holds a few arrival gaps only, and two keyframes landing in the same
    // frame read as a ~0 interval (anchor on top of the newest keyframe, then holds). Until the ring fills,
    // don't go below the sender's keyframe spacing (distinct ticks: stock servers can relay duplicates).
    if (n < rf::ObjInterp::max_keyframes) {
        int distinct = 1;
        for (int i = 1; i < n; ++i)
            distinct += interp->time_array[i] != interp->time_array[i - 1];
        const int span = n > 1 ? wrapped_diff16(interp->time_array[n - 1], interp->time_array[0]) : 0;
        if (distinct > 1 && span > 0)
            interval = std::max(interval, static_cast<float>(span) / static_cast<float>(distinct - 1));
    }
    return {std::clamp(interval, 1.0f, 250.0f), count > 0 ? dev / static_cast<float>(count) * scale : 0.0f};
}

struct KeyFrame
{
    rf::Vector3 pos;
    rf::Vector3 phb;
    rf::Vector3 eye_phb;
    rf::Vector3 vel;
    uint16_t tick;
    // Anim-state inputs the engine applied on arrival (a full playout delay ahead of the pose);
    // carried per keyframe and applied at the evaluated tick instead
    rf::Vector3 move; // sender's movement input; zero = idle anim
    bool crouch;
};

rf::Vector3 lerp_phb(const rf::Vector3& a, const rf::Vector3& b, float t)
{
    rf::Vector3 diff = b - a;
    // Heading wraps at +-pi
    constexpr float pi = std::numbers::pi_v<float>;
    if (diff.y > pi)
        diff.y -= 2 * pi;
    else if (diff.y < -pi)
        diff.y += 2 * pi;
    return a + diff * t;
}

// Cubic Hermite from p0 (t = 0) to p1 (t = 1); m0/m1 are tangents already scaled to the span
rf::Vector3 hermite(const rf::Vector3& p0, const rf::Vector3& m0, const rf::Vector3& p1, const rf::Vector3& m1,
                    float t)
{
    const float t2 = t * t;
    const float t3 = t2 * t;
    return p0 * (2 * t3 - 3 * t2 + 1) + m0 * (t3 - 2 * t2 + t) + p1 * (-2 * t3 + 3 * t2) + m1 * (t3 - t2);
}

// Pose at tick + frac ms from n oldest-first keyframes; false if older than the span. Past the newest
// keyframe: linear extrapolation along its velocity up to max_extrap_ms, orientation held. crouch_of is
// only asked about the keyframe whose discrete state is used.
template<typename FrameAt, typename CrouchOf>
bool eval_frames(FrameAt&& frame_at, CrouchOf&& crouch_of, int n, uint16_t tick, float frac,
                 float max_extrap_ms, KeyFrame& out)
{
    if (n < 2)
        return false;
    const KeyFrame newest = frame_at(n - 1);
    const float past_newest = static_cast<float>(wrapped_diff16(tick, newest.tick)) + frac;
    if (past_newest >= 0.0f) {
        out = newest;
        out.pos = newest.pos + newest.vel * (std::min(past_newest, max_extrap_ms) / 1000.0f);
        out.crouch = crouch_of(newest);
        out.tick = tick;
        return true;
    }
    if (wrapped_diff16(tick, frame_at(0).tick) < 0)
        return false;
    int i = n - 2;
    while (i > 0 && wrapped_diff16(tick, frame_at(i).tick) < 0)
        --i;
    const KeyFrame older = frame_at(i);
    const KeyFrame newer = frame_at(i + 1);
    const float span = static_cast<float>(wrapped_diff16(newer.tick, older.tick));
    const float into = static_cast<float>(wrapped_diff16(tick, older.tick)) + frac;
    const float t = span > 0.0f ? std::clamp(into / span, 0.0f, 1.0f) : 0.0f;
    const float span_s = span / 1000.0f;
    out.pos = hermite(older.pos, older.vel * span_s, newer.pos, newer.vel * span_s, t);
    out.vel = older.vel + (newer.vel - older.vel) * t;
    out.phb = lerp_phb(older.phb, newer.phb, t);
    out.eye_phb = lerp_phb(older.eye_phb, newer.eye_phb, t);
    const KeyFrame& state = t < 0.5f ? older : newer; // discrete: switch at the midpoint
    out.move = state.move;
    out.crouch = crouch_of(state);
    out.tick = tick;
    return true;
}

struct KeyFrameHistory
{
    std::array<KeyFrame, history_frames> frames;
    int head = 0; // next write index
    int count = 0;
    float clock_frac = 0.0f; // sub-millisecond part of the ring's interp_time, [0, 1)
    bool crouch = false;     // crouch bit of the newest replicated state byte, attached to the next keyframe

    const KeyFrame& from_newest(int i) const
    {
        return frames[(head - 1 - i + 2 * history_frames) % history_frames];
    }

    // Keeps crouch: the state byte of the record whose keyframe follows was already applied
    void reset()
    {
        head = count = 0;
        clock_frac = 0.0f;
    }

    void push(const KeyFrame& frame)
    {
        if (count > 0) {
            const int step = wrapped_diff16(frame.tick, from_newest(0).tick);
            if (step <= 0 && step > -5000)
                return; // re-sent (stock server above the client's send rate) or reordered keyframe
            if (step < 0 || step > 5000)
                count = head = 0; // time jumped (demo seek, ring re-anchor): start over
        }
        frames[head] = frame;
        head = (head + 1) % history_frames;
        count = std::min(count + 1, history_frames);
    }

    bool lookup(uint16_t tick, float frac, KeyFrame& out) const
    {
        return eval_frames([this](int i) { return from_newest(count - 1 - i); },
                           [](const KeyFrame& frame) { return frame.crouch; }, count, tick, frac, 0.0f, out);
    }

    // Crouch bit of the keyframe received at this tick (the engine ring has none); newest state if no match
    bool crouch_at(uint16_t tick) const
    {
        for (int i = 0; i < std::min(count, rf::ObjInterp::max_keyframes); ++i)
            if (from_newest(i).tick == tick)
                return from_newest(i).crouch;
        return crouch;
    }
};

std::unordered_map<int, KeyFrameHistory> g_history; // object handle -> received keyframes

// Entities respawn with fresh handles; drop histories of entities that no longer exist
void sweep_dead_entities()
{
    static int pushes_since_sweep = 0;
    if (++pushes_since_sweep < 4096)
        return;
    pushes_since_sweep = 0;
    std::erase_if(g_history, [](const auto& kv) { return rf::entity_from_handle(kv.first) == nullptr; });
}
} // namespace

float obj_interp_target_delay_ms(const rf::ObjInterp* interp)
{
    const auto [interval, jitter] = ring_stats(interp);
    return obj_interp_target_delay_ms(interval, jitter);
}

// ObjInterp::set_next_pos_orient (__thiscall): fed every replicated keyframe on client and server
FunHook<void __fastcall(rf::ObjInterp*, int, rf::Entity*, rf::Vector3*, rf::Vector3*, rf::Vector3*, rf::Vector3*,
                        rf::Vector3*, uint16_t, float)>
    obj_interp_set_next_pos_orient_hook{
        0x00483360,
        [](rf::ObjInterp* self, int edx, rf::Entity* ep, rf::Vector3* pos, rf::Vector3* phb, rf::Vector3* eye_phb,
           rf::Vector3* vel, rf::Vector3* move, uint16_t tick, float always_0) FASTCALL_LAMBDA {
            const uint32_t num_before = self->num;
            const bool anchoring = (self->flags & 1) != 0;
            const uint16_t interp_time_before = self->interp_time;
            obj_interp_set_next_pos_orient_hook.call_target(self, edx, ep, pos, phb, eye_phb, vel, move, tick,
                                                            always_0);
            // Mirror only keyframes the ring took: the vehicle hook inside this one rejects bad rows
            if (self->num == num_before && (self->num == 0 || self->newest_frame_time() != tick))
                return;
            auto& history = g_history[ep->handle];
            // An emptied ring (Clear(): teleport, save load, vehicle hand-off) starts a new timeline, which the
            // deeper history must not bridge
            if (num_before == 0)
                history.reset();
            // The inner vehicle hook can also force a re-anchor, invisible in the flags sampled above
            const bool anchored = (anchoring && (self->flags & 1) == 0) || self->interp_time != interp_time_before;
            if (anchored) {
                // The engine anchored 2.2 x interval behind this keyframe; start at frame_advance's target
                self->interp_time = static_cast<uint16_t>(tick - static_cast<int>(obj_interp_target_delay_ms(self)));
                history.clock_frac = 0.0f;
            }
            history.push({*pos, *phb, *eye_phb, *vel, tick, *move, history.crouch});
            sweep_dead_entities();
        },
    };

// Client only: remote anim-state inputs are deferred to the evaluated tick (server crouch feeds hitboxes)
static bool defers_anim_state(const rf::Entity* ep)
{
    return !rf::is_server && ep != rf::local_player_entity && ep->obj_interp;
}

// get_set_entity_state (0x00475930): the replicated state byte is applied on arrival. Record the
// crouch bit (4) for the keyframe and neutralise it; the other bits stay immediate.
FunHook<void(rf::Entity*, uint8_t*, bool)> get_set_entity_state_hook{
    0x00475930,
    [](rf::Entity* ep, uint8_t* state, bool get) {
        if (!get && defers_anim_state(ep)) {
            g_history[ep->handle].crouch = (*state & 4) != 0;
            *state = static_cast<uint8_t>((*state & ~4) | (rf::entity_is_crouching(ep) ? 4 : 0));
        }
        get_set_entity_state_hook.call_target(ep, state, get);
    },
};

static void apply_anim_state(rf::Entity* ep, const KeyFrame& frame)
{
    ep->ai.ci.move = frame.move;
    if (frame.crouch != rf::entity_is_crouching(ep)) {
        if (frame.crouch)
            rf::entity_crouch(ep);
        else
            rf::entity_maybe_stop_crouching(ep);
    }
}

// ObjInterp::frame_advance (__thiscall, RET 4): per-frame playout clock step for every object with a ring
FunHook<void __fastcall(rf::ObjInterp*, int, rf::Object*)> obj_interp_frame_advance_hook{
    0x00483BE0,
    [](rf::ObjInterp* self, int, rf::Object* obj) FASTCALL_LAMBDA {
        const auto now_us = static_cast<uint32_t>(timer::get_i64(1000000));
        // In keyframe ticks, like ring_stats; 0 while a demo is paused
        const float time_scale = demo_playback_sim_time_scale();
        const float elapsed_ms = std::clamp(
            static_cast<float>(static_cast<int32_t>(now_us - self->frame_time_us)) / 1000.0f, 0.0f, elapsed_max_ms)
            * time_scale;
        self->frame_time_us = now_us;
        // Bit 0: waiting for the next insert to re-anchor
        if ((self->flags & 1) != 0 || self->num_frames() < 2 || time_scale <= 0.0f)
            return;

        float& frac = g_history[obj->handle].clock_frac;
        const uint16_t newest = self->newest_frame_time();
        const auto [interval, jitter] = ring_stats(self);
        const float target = obj_interp_target_delay_ms(interval, jitter);
        // Regulate the post-advance depth (what the evaluators see), not the pre-advance one
        float depth = static_cast<float>(wrapped_diff16(newest, self->interp_time)) - frac;
        const float rate =
            1.0f + std::clamp((depth - elapsed_ms - target) / target, -1.0f, 1.0f) * clock_max_slew;
        const float advanced = frac + elapsed_ms * rate;
        const float whole = std::floor(advanced);
        frac = advanced - whole;
        self->interp_time += static_cast<uint16_t>(static_cast<int>(whole));

        depth = static_cast<float>(wrapped_diff16(newest, self->interp_time)) - frac;
        if (depth < -interval) {
            // The feed stopped: hold at the extrapolation limit
            self->interp_time = static_cast<uint16_t>(newest + static_cast<int>(interval));
            frac = 0.0f;
            if (rf::Player* pp = rf::player_from_entity_handle(obj->handle); pp && pp->net_data)
                ++pp->net_data->stats.obj_update_too_fast;
        }
        else if (depth > target + reanchor_margin * interval) {
            // Far behind (post-stall burst): snap instead of slewing for seconds
            self->interp_time = static_cast<uint16_t>(newest - static_cast<int>(target));
            frac = 0.0f;
        }
    },
};

// frame_eval: the per-frame evaluation of a remote entity (multi_obj_interp_pos/orient, 0x00484770 /
// 0x00484650), which plays the sub-ms clock fraction and the deeper history and applies deferred anim
// state. The other callers are the server's hitscan rewind (0x0046F7xx / 0x0046FDxx).
static float eval_frac(const rf::Entity* ep, bool frame_eval)
{
    if (!frame_eval)
        return 0.0f;
    auto it = g_history.find(ep->handle);
    return it != g_history.end() ? it->second.clock_frac : 0.0f;
}

// Pose at tick from the engine ring when it covers the tick, else from the deeper history
static bool pose_at_tick(const rf::Entity* ep, uint16_t tick, bool frame_eval, KeyFrame& out)
{
    const rf::ObjInterp* interp = ep->obj_interp;
    if (!interp || interp->num_frames() < 2)
        return false;
    auto it = g_history.find(ep->handle);
    const KeyFrameHistory* history = it != g_history.end() ? &it->second : nullptr;
    if (wrapped_diff16(tick, interp->time_array[0]) >= 0) {
        const auto ring_frame = [interp](int i) {
            return KeyFrame{interp->pos_array[i], interp->phb_array[i],  interp->eye_phb_array[i],
                            interp->vel_array[i], interp->time_array[i], interp->move_array[i], false};
        };
        // The engine ring has no crouch bit; the history recorded it per received keyframe
        const auto ring_crouch = [history](const KeyFrame& frame) {
            return history && history->crouch_at(frame.tick);
        };
        return eval_frames(ring_frame, ring_crouch, interp->num_frames(), tick, eval_frac(ep, frame_eval),
                           ring_stats(interp).interval, out);
    }
    // Server hitscan rewind (ticks reported by the shooter) keeps the ring's reach, like stock
    if (rf::is_server && !frame_eval)
        return false;
    return history && history->lookup(tick, eval_frac(ep, frame_eval), out);
}

static void pos_at_tick(rf::Entity* ep, uint16_t tick, rf::Vector3* out_pos, rf::Vector3* out_vel, bool frame_eval);
static void orient_at_tick(rf::Entity* ep, uint16_t tick, rf::Matrix3* out_orient, rf::Vector3* out_phb,
                           rf::Vector3* out_eye_phb, bool frame_eval);

FunHook<void __stdcall(rf::Entity*, uint16_t, rf::Vector3*, rf::Vector3*)> entity_interp_pos_at_tick_hook{
    0x00484010,
    [](rf::Entity* ep, uint16_t tick, rf::Vector3* out_pos, rf::Vector3* out_vel) STDCALL_LAMBDA {
        pos_at_tick(ep, tick, out_pos, out_vel, false);
    },
};

CallHook<void __stdcall(rf::Entity*, uint16_t, rf::Vector3*, rf::Vector3*)> frame_interp_pos_at_tick_hook{
    0x004847A7,
    [](rf::Entity* ep, uint16_t tick, rf::Vector3* out_pos, rf::Vector3* out_vel) STDCALL_LAMBDA {
        pos_at_tick(ep, tick, out_pos, out_vel, true);
    },
};

FunHook<void __stdcall(rf::Entity*, uint16_t, rf::Matrix3*, rf::Vector3*, rf::Vector3*)>
    entity_interp_orient_at_tick_hook{
        0x004842E0,
        [](rf::Entity* ep, uint16_t tick, rf::Matrix3* out_orient, rf::Vector3* out_phb,
           rf::Vector3* out_eye_phb) STDCALL_LAMBDA {
            orient_at_tick(ep, tick, out_orient, out_phb, out_eye_phb, false);
        },
    };

CallHook<void __stdcall(rf::Entity*, uint16_t, rf::Matrix3*, rf::Vector3*, rf::Vector3*)>
    frame_interp_orient_at_tick_hook{
        0x004846BC,
        [](rf::Entity* ep, uint16_t tick, rf::Matrix3* out_orient, rf::Vector3* out_phb,
           rf::Vector3* out_eye_phb) STDCALL_LAMBDA {
            orient_at_tick(ep, tick, out_orient, out_phb, out_eye_phb, true);
        },
    };

static void pos_at_tick(rf::Entity* ep, uint16_t tick, rf::Vector3* out_pos, rf::Vector3* out_vel, bool frame_eval)
{
    KeyFrame frame;
    if (pose_at_tick(ep, tick, frame_eval, frame)) {
        *out_pos = frame.pos;
        *out_vel = frame.vel;
        if (frame_eval && defers_anim_state(ep))
            apply_anim_state(ep, frame); // never the hitscan rewind
        return;
    }
    entity_interp_pos_at_tick_hook.call_target(ep, tick, out_pos, out_vel);
}

static void orient_at_tick(rf::Entity* ep, uint16_t tick, rf::Matrix3* out_orient, rf::Vector3* out_phb,
                           rf::Vector3* out_eye_phb, bool frame_eval)
{
    KeyFrame frame;
    if (pose_at_tick(ep, tick, frame_eval, frame)) {
        // phb = (pitch, heading, bank); set_from_angles takes (pitch, bank, heading)
        out_orient->set_from_angles(frame.phb.x, frame.phb.z, frame.phb.y);
        *out_phb = frame.phb;
        *out_eye_phb = frame.eye_phb;
        return;
    }
    entity_interp_orient_at_tick_hook.call_target(ep, tick, out_orient, out_phb, out_eye_phb);
}

uint16_t obj_interp_oldest_tick(const rf::Entity* ep)
{
    uint16_t oldest = ep->obj_interp->time_array[0];
    auto it = g_history.find(ep->handle);
    if (it != g_history.end() && it->second.count >= 2) {
        const uint16_t history_oldest = it->second.from_newest(it->second.count - 1).tick;
        if (wrapped_diff16(history_oldest, oldest) < 0)
            oldest = history_oldest;
    }
    return oldest;
}

void obj_interp_history_init()
{
    // Disable set_next_pos_orient's insert-time re-anchor (it compared the pre-advance clock and
    // tripped every insert at 60 fps); frame_advance re-anchors instead. The skipped JZ lands on the
    // flags bit 0 test, so the initial anchor still runs and the FPU stack stays balanced either way.
    AsmWriter{0x00483719}.nop(2);
    obj_interp_set_next_pos_orient_hook.install();
    get_set_entity_state_hook.install();
    obj_interp_frame_advance_hook.install();
    entity_interp_pos_at_tick_hook.install();
    entity_interp_orient_at_tick_hook.install();
    frame_interp_pos_at_tick_hook.install();
    frame_interp_orient_at_tick_hook.install();
}

void obj_interp_history_clear_all()
{
    g_history.clear();
}
