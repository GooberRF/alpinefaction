#pragma once

#include "../math/vector.h"
#include "../math/matrix.h"

namespace rf
{
    struct Entity;
    struct Player;

    enum CameraMode
    {
        CAMERA_FIRST_PERSON  = 0x0,
        CAMERA_THIRD_PERSON  = 0x1,
        CAMERA_FREELOOK      = 0x2,
        CAMERA_DESCENT_FLY   = 0x3,
        CAMERA_DEAD_VIEW     = 0x4,
        CAMERA_CUTSCENE      = 0x5,
        CAMERA_FIXED_VIEW    = 0x6,
        CAMERA_ORBIT         = 0x7,
    };

    struct Camera
    {
        Entity *camera_entity;
        Player *player;
        CameraMode mode;
    };
    static_assert(sizeof(Camera) == 0xC);

    static auto& camera_enter_first_person = addr_as_ref<bool(Camera *camera)>(0x0040DDF0);
    static auto& camera_enter_third_person = addr_as_ref<bool(Camera* camera)>(0x0040DE80);
    static auto& camera_enter_freelook = addr_as_ref<bool(Camera *camera)>(0x0040DCF0);
    static auto& camera_enter_fixed = addr_as_ref<void(Camera *camera)>(0x0040DF70);
    static auto& camera_enter_random_fixed_pos = addr_as_ref<void()>(0x0040E070);

    inline Vector3 camera_get_pos(Camera *camera)
    {
        Vector3 result;
        AddrCaller{0x0040D760}.c_call(&result, camera);
        return result;
    }

    inline Matrix3 camera_get_orient(Camera *camera)
    {
        Matrix3 result;
        AddrCaller{0x0040D780}.c_call(&result, camera);
        return result;
    }

    static auto& camera_get_mode = addr_as_ref<CameraMode(const rf::Camera&)>(0x0040D740);
    static auto& camera_shake = addr_as_ref<void(Camera* camera, float amplitude, float time_seconds)>(0x0040E0B0);

    static auto& fixed_camera_count = addr_as_ref<int>(0x005AFB38);
    static auto& fixed_camera_index = addr_as_ref<int>(0x005AFB6C);
    static auto& fixed_camera_look_target_handle = addr_as_ref<int>(0x007C7190); // -1 if none

    // Parallel position/orientation arrays for fixed cameras.
    inline Vector3* fixed_camera_get_pos(int index)
    {
        return &addr_as_ref<Vector3*>(0x005AFB40)[index];
    }
    inline Matrix3* fixed_camera_get_orient(int index)
    {
        return &addr_as_ref<Matrix3*>(0x005AFB68)[index];
    }
}
