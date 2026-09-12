#pragma once

#include <cstdint>

namespace rf
{
    struct Entity;
    struct ObjInterp;
}

// Remote-entity interpolation on top of the engine's 20-frame ObjInterp ring: a deeper per-entity
// keyframe history (~1 s of hitscan rewind / povcomp bias at any netfps), a slewing sub-ms playout
// clock in place of the engine's snap-and-freeze one, and cubic Hermite through the replicated
// velocities instead of the overshooting Catmull-Rom spline.
void obj_interp_history_init();
void obj_interp_history_clear_all();
// Oldest tick evaluable for this entity (engine ring or history, whichever reaches further back).
// Requires ep->obj_interp with at least 2 keyframes.
uint16_t obj_interp_oldest_tick(const rf::Entity* ep);

// Delay the playout clock keeps behind the newest keyframe for a ring with this arrival interval
// and jitter (ms). The stock engine anchored 2.2 x interval behind.
float obj_interp_target_delay_ms(float interval_ms, float jitter_ms);
// Same, measured from the ring's own arrival gaps
float obj_interp_target_delay_ms(const rf::ObjInterp* interp);

// Marks the per-frame pose evaluation; inside it the tick evaluators add the entity's sub-ms clock fraction
struct ObjInterpFrameEval
{
    ObjInterpFrameEval();
    ~ObjInterpFrameEval();
};
