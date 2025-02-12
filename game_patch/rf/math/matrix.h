#pragma once

#include "vector.h"
#include <patch_common/MemUtils.h>

namespace rf
{
    struct Matrix3
    {
        Vector3 rvec;
        Vector3 uvec;
        Vector3 fvec;

        bool operator==(const Matrix3& other) const = default;

        void make_identity()
        {
            AddrCaller{0x004FCE70}.this_call(this);
        }

        Matrix3* copy_transpose(Matrix3* out)
        {
            Matrix3* result = nullptr;
            AddrCaller{0x004FC8A0}.this_call(this, out);
            return result;
        }

        void make_quick(const Vector3& forward_vector)
        {
            AddrCaller{0x004FCFA0}.this_call(this, &forward_vector);
        }

        void rand_quick()
        {
            Vector3 fvec;

            fvec.rand_quick();

            make_quick(fvec);
        }

        void extract_angles(float* pitch, float* roll, float* yaw)
        {
            AddrCaller{0x004FC060}.this_call(this, pitch, roll, yaw);
        }
    };
    static_assert(sizeof(Matrix3) == 0x24);

    struct Matrix43
    {
        Matrix3 orient;
        Vector3 origin;

        bool operator==(const Matrix43& other) const = default;

        Matrix43 operator*(const Matrix43& other) const
        {
            Matrix43 result;
            AddrCaller{0x0051C620}.this_call(this, &result, &other);
            return result;
        }
    };

    static auto& identity_matrix = addr_as_ref<Matrix3>(0x0173C388);
    static auto& file_default_matrix = *reinterpret_cast<Matrix3*>(0x01BDB278);
    }
