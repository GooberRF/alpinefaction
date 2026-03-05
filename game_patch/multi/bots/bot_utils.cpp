#include "bot_utils.h"

#include "bot_internal.h"
#include "../../misc/waypoints.h"
#include "../../rf/collide.h"

bool bot_has_unobstructed_level_los(
    const rf::Vector3& from,
    const rf::Vector3& to,
    const rf::Object* ignore1,
    const rf::Object* ignore2)
{
    rf::Vector3 p0 = from;
    rf::Vector3 p1 = to;
    rf::LevelCollisionOut collision{};
    collision.obj_handle = -1;
    collision.face = nullptr;
    const bool hit = rf::collide_linesegment_level_for_multi(
        p0,
        p1,
        const_cast<rf::Object*>(ignore1),
        const_cast<rf::Object*>(ignore2),
        &collision,
        0.05f,
        false,
        1.0f
    );
    return !hit;
}

int bot_find_closest_waypoint_with_fallback(const rf::Vector3& pos)
{
    float radius = kWaypointSearchRadius;
    for (int pass = 0; pass < 6; ++pass) {
        if (const int waypoint = waypoints_closest(pos, radius); waypoint > 0) {
            return waypoint;
        }
        radius *= 2.0f;
    }
    return 0;
}
