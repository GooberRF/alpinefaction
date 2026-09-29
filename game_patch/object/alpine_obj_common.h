#pragma once

#include "../rf/math/vector.h"
#include "../rf/math/matrix.h"

namespace rf
{
    struct Clutter;
}

// Stock's effects.tbl parser multiplies cone angle degrees by 0.5 before storing it in GlareInfo,
// and the render code reads the halved value back, so every AF glare author path has to halve too.
constexpr float alpine_glare_cone_angle_factor = 0.5f;

// Rejects a serialized orientation that is not finite and roughly unit-length on each axis. Cheap
// enough to run per record at load, and it only has to reject garbage, not validate handedness.
bool alpine_orient_is_sane(const rf::Matrix3& orient);

// Sentinel field init plus clutter list splice for a clutter object that was created with a
// type specific ObjectCreateInfo. Leaves Clutter::info alone; the caller owns that choice.
void alpine_init_anchor_clutter(rf::Clutter* clutter);

// Creates the invisible, mesh-less OT_CLUTTER an alpine object type anchors itself to, so it owns a
// real object handle and uid and stock event->object link resolution can find it. Returns nullptr
// if the object could not be created. name may be null or empty.
rf::Clutter* alpine_create_anchor_clutter(const rf::Vector3& pos, const rf::Matrix3& orient, int uid,
                                          const char* name);
