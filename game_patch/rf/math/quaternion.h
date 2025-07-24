#pragma once

namespace rf
{
    struct Quaternion
    {
        float x, y, z, w;
    };
    static_assert(sizeof(Quaternion) == 0x10);

    struct ShortQuat
    {
        int16_t x, y, z, w;
    };
    static_assert(sizeof(ShortQuat) == 0x8);
    }
