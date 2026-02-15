#pragma once

#include "math/vector.h"
#include "geometry.h"
#include "object.h"

namespace rf
{
    struct LevelCollisionOut
    {
        rf::Vector3 hit_point;
        float distance;
        int obj_handle;
        void* face;
    };

    // used by collide_sphereline_world and V3dCollisionInput::flags
    enum V3dCollisionFlags
    {
        V3D_CF_ANY_HIT = 0x1,
        V3D_CF_MESH_SPACE = 0x2,
    };

    // Aliases matching stock naming
    enum CollideFlagAliases
    {
        CF_SKIP_SEE_THRU_FACES = CF_SKIP_SEE_THRU,
        CF_SKIP_SHOOT_THRU_FACES = CF_SKIP_SHOOT_THRU,
        CF_SEE_THRU_FACES_ALPHA_TEST = CF_SEE_THRU_ALPHA_TEST,
        CF_SHOOT_THRU_FACES_ALPHA_TEST = CF_SHOOT_THRU_ALPHA_TEST,
        CF_SKIP_GLASS_FACES = CF_SKIP_DESTRUCTIBLE_GLASS,
    };

    struct LineSegment
    {
        Vector3 start{};
        Vector3 dir{};
    };

    static auto& collide_linesegment_backface = addr_as_ref<bool(
        rf::Vector3* start,
        const rf::Vector3* dir,
        rf::Plane* p,
        float* fraction)>(0x00498740);
    static auto& collide_find_world_thickness = addr_as_ref<float(
        rf::Vector3* pos,
        rf::Vector3* dir,
        rf::PCollisionOut* collision)>(0x004987B0);
    static auto& collide_linesegment_room_backfaces = addr_as_ref<bool(
        rf::GBBox* box,
        rf::LineSegment* line,
        rf::Vector3* end_point,
        rf::GCollisionOutput* out)>(0x00498BD0);
    static auto& collide_linesegment_solid_backfaces = addr_as_ref<bool(
        rf::GSolid* solid,
        rf::LineSegment* line,
        rf::Vector3* end_point,
        rf::GCollisionOutput* out)>(0x00498D70);
    static auto& collide_linesegment_level_for_multi =
        addr_as_ref<bool(rf::Vector3& p0, rf::Vector3& p1, rf::Object *ignore1, rf::Object *ignore2,
        LevelCollisionOut *out, float collision_radius, bool use_mesh_collide, float bbox_size_factor)>(0x0049C690);
    static auto& collide_linesegment_level_solid = addr_as_ref<bool(
        rf::Vector3& p0, rf::Vector3& p1, int flags, rf::GCollisionOutput* collision)>(0x0049C5C0);
    static auto& collide_linesegment_world = addr_as_ref<bool(
        rf::Vector3& p0, rf::Vector3& p1, int flags, rf::PCollisionOut* collision)>(0x00498E80);
    static auto& collide_sphereline_world = addr_as_ref<bool(
        rf::Vector3* p0,
        rf::Vector3* p1,
        float min_radius,
        int flags,
        rf::Object* ignored1,
        rf::Object* ignored2,
        rf::PCollisionOut* collision)>(0x004991C0);
    static auto& collide_cf_to_gcf_flags = addr_as_ref<int(int cf_flags)>(0x00499190);
    static auto& collide_linesegment_boundingbox = addr_as_ref<bool(
        rf::Vector3* bbox_min,
        rf::Vector3* bbox_max,
        rf::Vector3* p1,
        rf::Vector3* p2,
        rf::Vector3* intercept)>(0x00508B70);
}
