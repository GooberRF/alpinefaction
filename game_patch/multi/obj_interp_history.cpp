#include <patch_common/FunHook.h>
#include <patch_common/MemUtils.h>
#include <xlog/xlog.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <unordered_map>
#include "obj_interp_history.h"
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
// it covers the tick and a deeper per-entity keyframe history otherwise (the engine returned the live pose).
//
// The engine's playout clock (ObjInterp::frame_advance 0x00483BE0) stepped whole ms at wall-clock rate,
// froze when it ran dry and only recovered via set_next_pos_orient's hard re-anchor. The frame_advance
// hook replaces it with one that slews within +-10% of wall time toward a jitter-derived target, carries
// a sub-ms fraction and extrapolates up to one interval. The insert-time re-anchor is disabled in
// obj_interp_history_init; the hook snaps when far behind instead.

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

struct RingStats
{
    float interval; // average local arrival gap, ms
    float jitter;   // mean abs deviation of the arrival gaps, ms
};

static RingStats ring_stats(const rf::ObjInterp* interp)
{
    const int n = interp->num_frames();
    const float interval = std::clamp(interp->arrive_time_avg_diff, 1.0f, 250.0f);
    float dev = 0.0f;
    for (int i = 0; i < n; ++i)
        dev += std::fabs(static_cast<float>(interp->arrive_time_diff[i]) - interval);
    return {interval, n > 0 ? dev / static_cast<float>(n) : 0.0f};
}

float obj_interp_target_delay_ms(const rf::ObjInterp* interp)
{
    const auto [interval, jitter] = ring_stats(interp);
    return obj_interp_target_delay_ms(interval, jitter);
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

// 16-bit sender ms ticks; differences valid within +-32 s
static int tick_delta(uint16_t a, uint16_t b)
{
    return static_cast<int16_t>(static_cast<uint16_t>(a - b));
}

static rf::Vector3 lerp_phb(const rf::Vector3& a, const rf::Vector3& b, float t)
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
static rf::Vector3 hermite(const rf::Vector3& p0, const rf::Vector3& m0, const rf::Vector3& p1, const rf::Vector3& m1,
                           float t)
{
    const float t2 = t * t;
    const float t3 = t2 * t;
    return p0 * (2 * t3 - 3 * t2 + 1) + m0 * (t3 - 2 * t2 + t) + p1 * (-2 * t3 + 3 * t2) + m1 * (t3 - t2);
}

// Pose at tick + frac ms from n oldest-first keyframes; false if older than the span. Past the newest
// keyframe: linear extrapolation along its velocity up to max_extrap_ms, orientation held.
template<typename FrameAt>
static bool eval_frames(FrameAt&& frame_at, int n, uint16_t tick, float frac, float max_extrap_ms, KeyFrame& out)
{
    if (n < 2)
        return false;
    const KeyFrame newest = frame_at(n - 1);
    const float past_newest = static_cast<float>(tick_delta(tick, newest.tick)) + frac;
    if (past_newest >= 0.0f) {
        out = newest;
        out.pos = newest.pos + newest.vel * (std::min(past_newest, max_extrap_ms) / 1000.0f);
        out.tick = tick;
        return true;
    }
    if (tick_delta(tick, frame_at(0).tick) < 0)
        return false;
    int i = n - 2;
    while (i > 0 && tick_delta(tick, frame_at(i).tick) < 0)
        --i;
    const KeyFrame older = frame_at(i);
    const KeyFrame newer = frame_at(i + 1);
    const float span = static_cast<float>(tick_delta(newer.tick, older.tick));
    const float t =
        span > 0.0f ? std::clamp((static_cast<float>(tick_delta(tick, older.tick)) + frac) / span, 0.0f, 1.0f) : 0.0f;
    const float span_s = span / 1000.0f;
    out.pos = hermite(older.pos, older.vel * span_s, newer.pos, newer.vel * span_s, t);
    out.vel = older.vel + (newer.vel - older.vel) * t;
    out.phb = lerp_phb(older.phb, newer.phb, t);
    out.eye_phb = lerp_phb(older.eye_phb, newer.eye_phb, t);
    const KeyFrame& state = t < 0.5f ? older : newer; // discrete: switch at the midpoint
    out.move = state.move;
    out.crouch = state.crouch;
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

    void push(const KeyFrame& frame)
    {
        if (count > 0) {
            const int step = tick_delta(frame.tick, from_newest(0).tick);
            if (step == 0)
                return; // re-sent keyframe (stock server above the client's send rate)
            if (step < 0 || step > 5000)
                count = head = 0; // time jumped (demo seek, ring re-anchor): start over
        }
        frames[head] = frame;
        head = (head + 1) % history_frames;
        count = std::min(count + 1, history_frames);
    }

    bool lookup(uint16_t tick, KeyFrame& out) const
    {
        return eval_frames([this](int i) { return from_newest(count - 1 - i); }, count, tick, 0.0f, 0.0f, out);
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

static std::unordered_map<int, KeyFrameHistory> g_history; // object handle -> received keyframes

// Entities respawn with fresh handles; drop histories of entities that no longer exist
static void sweep_dead_entities()
{
    static int pushes_since_sweep = 0;
    if (++pushes_since_sweep < 4096)
        return;
    pushes_since_sweep = 0;
    std::erase_if(g_history, [](const auto& kv) { return rf::entity_from_handle(kv.first) == nullptr; });
}

// ObjInterp::set_next_pos_orient (__thiscall): fed every replicated keyframe on client and server
FunHook<void __fastcall(rf::ObjInterp*, int, rf::Entity*, rf::Vector3*, rf::Vector3*, rf::Vector3*, rf::Vector3*,
                        rf::Vector3*, uint16_t, int)>
    obj_interp_set_next_pos_orient_hook{
        0x00483360,
        [](rf::ObjInterp* self, int edx, rf::Entity* ep, rf::Vector3* pos, rf::Vector3* phb, rf::Vector3* eye_phb,
           rf::Vector3* vel, rf::Vector3* move, uint16_t tick, int always_0) {
            obj_interp_set_next_pos_orient_hook.call_target(self, edx, ep, pos, phb, eye_phb, vel, move, tick,
                                                            always_0);
            auto& history = g_history[ep->handle];
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
    [](rf::ObjInterp* self, int, rf::Object* obj) {
        const int now_us = static_cast<int>(timer::get_i64(1000000));
        const float elapsed_ms = std::clamp(
            static_cast<float>(now_us - static_cast<int>(self->frame_time_us)) / 1000.0f, 0.0f, elapsed_max_ms);
        self->frame_time_us = static_cast<uint32_t>(now_us);
        // Bit 0: waiting for the next insert to re-anchor
        if ((self->flags & 1) != 0 || self->num_frames() < 2)
            return;

        float& frac = g_history[obj->handle].clock_frac;
        const uint16_t newest = self->newest_frame_time();
        const auto [interval, jitter] = ring_stats(self);
        const float target = obj_interp_target_delay_ms(interval, jitter);
        // Regulate the post-advance depth (what the evaluators see), not the pre-advance one
        float depth = static_cast<float>(tick_delta(newest, self->interp_time)) - frac;
        const float rate =
            1.0f + std::clamp((depth - elapsed_ms - target) / target, -1.0f, 1.0f) * clock_max_slew;
        const float advanced = frac + elapsed_ms * rate;
        const float whole = std::floor(advanced);
        frac = advanced - whole;
        self->interp_time += static_cast<uint16_t>(static_cast<int>(whole));

        depth = static_cast<float>(tick_delta(newest, self->interp_time)) - frac;
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

static int g_frame_eval_depth = 0;

ObjInterpFrameEval::ObjInterpFrameEval()
{
    ++g_frame_eval_depth;
}

ObjInterpFrameEval::~ObjInterpFrameEval()
{
    --g_frame_eval_depth;
}

static float eval_frac(const rf::Entity* ep)
{
    if (g_frame_eval_depth == 0)
        return 0.0f;
    auto it = g_history.find(ep->handle);
    return it != g_history.end() ? it->second.clock_frac : 0.0f;
}

// Pose at tick from the engine ring when it covers the tick, else from the deeper history
static bool pose_at_tick(const rf::Entity* ep, uint16_t tick, KeyFrame& out)
{
    const rf::ObjInterp* interp = ep->obj_interp;
    if (!interp || interp->num_frames() < 2)
        return false;
    auto it = g_history.find(ep->handle);
    const KeyFrameHistory* history = it != g_history.end() ? &it->second : nullptr;
    if (tick_delta(tick, interp->time_array[0]) >= 0) {
        const auto ring_frame = [interp, history](int i) {
            return KeyFrame{interp->pos_array[i],  interp->phb_array[i],  interp->eye_phb_array[i],
                            interp->vel_array[i],  interp->time_array[i], interp->move_array[i],
                            history && history->crouch_at(interp->time_array[i])};
        };
        return eval_frames(ring_frame, interp->num_frames(), tick, eval_frac(ep), ring_stats(interp).interval, out);
    }
    return history && history->lookup(tick, out);
}

FunHook<void __stdcall(rf::Entity*, uint16_t, rf::Vector3*, rf::Vector3*)> entity_interp_pos_at_tick_hook{
    0x00484010,
    [](rf::Entity* ep, uint16_t tick, rf::Vector3* out_pos, rf::Vector3* out_vel) {
        KeyFrame frame;
        if (pose_at_tick(ep, tick, frame)) {
            *out_pos = frame.pos;
            *out_vel = frame.vel;
            if (g_frame_eval_depth > 0 && defers_anim_state(ep))
                apply_anim_state(ep, frame); // per-frame evaluation only, never the hitscan rewind
            return;
        }
        entity_interp_pos_at_tick_hook.call_target(ep, tick, out_pos, out_vel);
    },
};

FunHook<void __stdcall(rf::Entity*, uint16_t, rf::Matrix3*, rf::Vector3*, rf::Vector3*)>
    entity_interp_orient_at_tick_hook{
        0x004842E0,
        [](rf::Entity* ep, uint16_t tick, rf::Matrix3* out_orient, rf::Vector3* out_phb, rf::Vector3* out_eye_phb) {
            KeyFrame frame;
            if (pose_at_tick(ep, tick, frame)) {
                // phb = (pitch, heading, bank); set_from_angles takes (pitch, bank, heading)
                out_orient->set_from_angles(frame.phb.x, frame.phb.z, frame.phb.y);
                *out_phb = frame.phb;
                *out_eye_phb = frame.eye_phb;
                return;
            }
            entity_interp_orient_at_tick_hook.call_target(ep, tick, out_orient, out_phb, out_eye_phb);
        },
    };

uint16_t obj_interp_oldest_tick(const rf::Entity* ep)
{
    uint16_t oldest = ep->obj_interp->time_array[0];
    auto it = g_history.find(ep->handle);
    if (it != g_history.end() && it->second.count >= 2) {
        const uint16_t history_oldest = it->second.from_newest(it->second.count - 1).tick;
        if (tick_delta(history_oldest, oldest) < 0)
            oldest = history_oldest;
    }
    return oldest;
}

void obj_interp_history_init()
{
    // Disable set_next_pos_orient's insert-time re-anchor (it compared the pre-advance clock and
    // tripped every insert at 60 fps); frame_advance re-anchors instead. Initial anchor unaffected.
    write_mem<float>(0x006FED1C, 1.0e9f);
    obj_interp_set_next_pos_orient_hook.install();
    get_set_entity_state_hook.install();
    obj_interp_frame_advance_hook.install();
    entity_interp_pos_at_tick_hook.install();
    entity_interp_orient_at_tick_hook.install();
}

void obj_interp_history_clear_all()
{
    g_history.clear();
}
