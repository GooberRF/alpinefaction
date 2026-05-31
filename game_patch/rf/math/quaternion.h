#pragma once

namespace rf
{
    //forward declaration
    struct Quaternion;

    struct ShortQuat
    {
        int16_t x, y, z, w;

        // Engine routine writes the compressed quat into *this; it does not return a
        // meaningful value (EAX is not set), so this is void. All callers ignore the result.
        void from_quat(Quaternion* a2)
        {
            AddrCaller{0x0051A540}.this_call<void>(this, a2);
        }
    };
    static_assert(sizeof(ShortQuat) == 0x8);

    struct Quaternion
    {
        float x, y, z, w;

        // Engine routine decompresses pCompressed into *this; it does not return a
        // meaningful value (EAX is not set), so this is void. All callers ignore the result.
        void unpack(const ShortQuat* pCompressed)
        {
            AddrCaller{0x00417E90}.this_call<void>(this, pCompressed);
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
