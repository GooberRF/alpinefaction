#pragma once

#include <cstdint>

namespace rf
{
    struct Entity;
    struct ObjInterp;
}

// Remote-entity interpolation: a slewing playout clock and a keyframe history deeper than the engine ring
void obj_interp_history_init();
void obj_interp_history_clear_all();
// Oldest tick evaluable from the engine ring or the history; requires ep->obj_interp with >= 2 keyframes
uint16_t obj_interp_oldest_tick(const rf::Entity* ep);

// Delay the playout clock keeps behind the newest keyframe for this arrival interval and jitter (ms)
float obj_interp_target_delay_ms(float interval_ms, float jitter_ms);
// Same, measured from the ring's own arrival gaps
float obj_interp_target_delay_ms(const rf::ObjInterp* interp);
