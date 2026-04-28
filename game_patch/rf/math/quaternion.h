#pragma once

namespace rf
{
    //forward declaration
    struct Quaternion;

    struct ShortQuat
    {
        int16_t x, y, z, w;

        ShortQuat* from_quat(Quaternion* a2)
        {
            return AddrCaller{0x0051A540}.this_call<ShortQuat*>(this, a2);
        }
    };
    static_assert(sizeof(ShortQuat) == 0x8);

    struct Quaternion
    {
        float x, y, z, w;

        int unpack(const ShortQuat* pCompressed)
        {
            return AddrCaller{0x00417E90}.this_call<int>(this, pCompressed);
        }

        void extract_matrix(Matrix3* mat)
        {
            AddrCaller{0x005194C0}.this_call(this, mat);
        }

        Quaternion* from_matrix(Matrix3* mat)
        {
            return AddrCaller{0x00518F90}.this_call<Quaternion*>(this, mat);
        }
    };
    static_assert(sizeof(Quaternion) == 0x10);
}
