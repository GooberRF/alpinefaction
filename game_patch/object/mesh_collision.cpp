#include <algorithm>
#include <patch_common/CallHook.h>
#include <patch_common/FunHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/AsmWriter.h>
#include <xlog/xlog.h>
#include "../rf/vmesh.h"
#include "../rf/physics.h"
#include "../rf/object.h"

// Mesh collision in the RF engine uses per-triangle intersection tests (vmesh_collide)
// rather than BSP-based solid collision (GSolid::collide). This makes it fundamentally
// less robust: triangle soup has gaps at edges/seams, no concept of a closed solid
// volume, and limited multi-hit resolution. These patches improve mesh collision
// reliability to reduce "falling through" and "getting stuck" on mesh surfaces.

// Small margin added to the collision sphere radius when testing physics collision
// against mesh triangles. This closes gaps at triangle edges/seams by making the
// effective collision volume slightly larger, preventing the sphere from slipping
// between adjacent triangles. The margin is small enough to not noticeably affect
// gameplay feel.
static constexpr float mesh_radius_margin = 0.02f;

// Small amount subtracted from hit_time after mesh collision detection. This ensures
// the object stops slightly before the mesh surface, providing clearance that prevents
// the object from getting embedded in the mesh on the next frame. This reduces the
// oscillation/"stuck" behavior that occurs when conflicting normals from adjacent
// triangles push the player back and forth.
static constexpr float mesh_hit_time_safety = 0.005f;

// Hook vmesh_collide (0x005031F0) at its call sites in the physics collision paths.
// This inflates the collision radius for the duration of the mesh test only, making
// the swept sphere slightly larger and closing edge gaps between mesh triangles.
//
// Call sites:
//   0x00499BD2 - collide_spheres_mesh: ground contact and stick-to-ground tests
//   0x0049B332 - collide_object_object_mesh: object-pair mesh collision
//
// Other vmesh_collide callers (weapon raycasts, line-of-sight, glare occlusion)
// are intentionally not hooked as they need precise triangle-level accuracy.
CallHook<bool(rf::VMesh*, rf::VMeshCollisionInput*, rf::VMeshCollisionOutput*, bool)>
mesh_physics_collide_radius_hook{
    {
        0x00499BD2, // in collide_spheres_mesh
        0x0049B332, // in collide_object_object_mesh
    },
    [](rf::VMesh* vmesh, rf::VMeshCollisionInput* in, rf::VMeshCollisionOutput* out, bool clear) {
        float original_radius = in->radius;
        in->radius += mesh_radius_margin;
        bool result = mesh_physics_collide_radius_hook.call_target(vmesh, in, out, clear);
        in->radius = original_radius;
        return result;
    },
};

// Hook collide_object_object_mesh (0x0049AFE0) to apply a safety margin to the
// collision hit_time. When a mesh collision is detected, the object is stopped at
// (hit_time - margin) instead of exactly at hit_time. This small clearance prevents
// the object from being positioned right on the mesh surface where floating-point
// imprecision and conflicting triangle normals can cause it to get stuck or embedded.
FunHook<bool(rf::Object*, rf::Object*)> collide_object_object_mesh_hook{
    0x0049AFE0,
    [](rf::Object* sphere_obj, rf::Object* mesh_obj) {
        bool result = collide_object_object_mesh_hook.call_target(sphere_obj, mesh_obj);
        if (result) {
            float& hit_time = sphere_obj->p_data.collide_out.hit_time;
            if (hit_time > mesh_hit_time_safety) {
                hit_time -= mesh_hit_time_safety;
            }
        }
        return result;
    },
};

void mesh_collision_apply_patches()
{
    mesh_physics_collide_radius_hook.install();
    collide_object_object_mesh_hook.install();
}
