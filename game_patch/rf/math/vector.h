#pragma once

#include <cmath>
#include <numbers>
#include <patch_common/MemUtils.h>
#include "../os/os.h"
#include "../../main/main.h"

namespace rf
{
    struct Vector3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;

        Vector3() = default;
        Vector3(float x, float y, float z) : x(x), y(y), z(z) {}

        bool operator==(const Vector3& other) const = default;

        void get_sum(Vector3* out_vec, const Vector3* other)
        {
            AddrCaller{0x0040A030}.this_call(this, out_vec, other);
        }

        void get_scaled(Vector3* out_result, float scale)
        {
            AddrCaller{0x0040A070}.this_call(this, out_result, scale);
        }

        void get_substracted(Vector3* out_result, const Vector3* other)
        {
            AddrCaller{0x00409FA0}.this_call(this, out_result, other);
        }

        [[nodiscard]] Vector3 cross(const Vector3& other) const
        {
            return Vector3{
                y * other.z - z * other.y,
                z * other.x - x * other.z,
                x * other.y - y * other.x
            };
        }

        [[nodiscard]] Vector3 cross_prod(const Vector3& other) const
        {
            return cross(other);
        }

        Vector3& operator+=(const Vector3& other)
        {
            x += other.x;
            y += other.y;
            z += other.z;
            return *this;
        }

        Vector3& operator*=(const Vector3& other)
        {
            x *= other.x;
            y *= other.y;
            z *= other.z;
            return *this;
        }

        Vector3& operator/=(const Vector3& other)
        {
            x /= other.x;
            y /= other.y;
            z /= other.z;
            return *this;
        }

        Vector3& operator+=(float scalar)
        {
            x += scalar;
            y += scalar;
            z += scalar;
            return *this;
        }

        Vector3& operator*=(float scale)
        {
            x *= scale;
            y *= scale;
            z *= scale;
            return *this;
        }

        Vector3& operator/=(float scale)
        {
            *this *= 1.0f / scale;
            return *this;
        }

        Vector3 operator-() const
        {
            return Vector3{-x, -y, -z};
        }

        Vector3& operator-=(const Vector3& other)
        {
            return (*this += -other);
        }

        Vector3& operator-=(float scalar)
        {
            return (*this += -scalar);
        }

        [[nodiscard]] Vector3 operator+(const Vector3& other) const
        {
            Vector3 tmp = *this;
            tmp += other;
            return tmp;
        }

        [[nodiscard]] Vector3 operator-(const Vector3& other) const
        {
            Vector3 tmp = *this;
            tmp -= other;
            return tmp;
        }

        [[nodiscard]] Vector3 operator*(const Vector3& other) const
        {
            Vector3 tmp = *this;
            tmp *= other;
            return tmp;
        }

        [[nodiscard]] Vector3 operator/(const Vector3& other) const
        {
            Vector3 tmp = *this;
            tmp /= other;
            return tmp;
        }

        [[nodiscard]] Vector3 operator+(float scalar) const
        {
            Vector3 tmp = *this;
            tmp += scalar;
            return tmp;
        }

        [[nodiscard]] Vector3 operator-(float scalar) const
        {
            Vector3 tmp = *this;
            tmp -= scalar;
            return tmp;
        }

        [[nodiscard]] Vector3 operator*(float scale) const
        {
            return {x * scale, y * scale, z * scale};
        }

        [[nodiscard]] Vector3 operator/(const float scale) const {
            return *this * (1.f / scale);
        }

        void zero()
        {
            x = 0.0f;
            y = 0.0f;
            z = 0.0f;
        }

        void set(float x, float y, float z)
        {
            this->x = x;
            this->y = y;
            this->z = z;
        }

        [[nodiscard]] float dot_prod(const Vector3& other) const
        {
            return other.x * x + other.y * y + other.z * z;
        }

        [[nodiscard]] float len() const
        {
            return std::sqrt(len_sq());
        }

        [[nodiscard]] float len_sq() const
        {
            return x * x + y * y + z * z;
        }

        float distance_to(const Vector3& other) const
        {
            float dx = other.x - x;
            float dy = other.y - y;
            float dz = other.z - z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        void normalize()
        {
            *this /= len();
        }

        void normalize_safe()
        {
            float magnitude = len();
            if (magnitude > 0.0f) {
                *this /= magnitude;
            }
            else {
                x = 1.0f;
                y = 0.0f;
                z = 0.0f;
            }
        }

        void rand_quick()
        {
            constexpr float TWO_PI = 6.2831855f;

            std::uniform_real_distribution<float> z_dist(-1.0f, 1.0f);
            z = z_dist(g_rng);

            std::uniform_real_distribution<float> angle_dist(0.0f, TWO_PI);
            float angle = angle_dist(g_rng);

            float scale = std::sqrt(1.0f - z * z);

            x = std::cos(angle) * scale;
            y = std::sin(angle) * scale;
        }

        // bell-curve bullet spread with more probable distribution of shots near the reticle center
        // replacement for bullet spread RNG logic, uses gaussian spread with spherical coordinate transformation
        void rand_around_dir_gaussian(const Vector3& dir, float sigma)
        {
            float maxAngle = std::acos(sigma); // reverse cos operation applied at 0x00426627 before this is called
            float effectiveSigma = maxAngle * 0.5f; // reduce max angle to align more closely with stock behaviour

            // Compute an orthonormal basis for the space with 'dir' as the z–axis.
            Vector3 right, up;

            // Choose a helper vector that is not parallel to dir.
            Vector3 helper = (fabs(dir.z) < 0.999f) ? Vector3(0, 0, 1) : Vector3(1, 0, 0);

            // right is perpendicular to both dir and helper.
            right = dir.cross(helper);
            right.normalize();

            // up is perpendicular to both dir and right.
            up = right.cross(dir);
            up.normalize();

            // Create distributions:
            // - For the angular deviation theta (from the central direction), we use a normal distribution.
            std::normal_distribution<float> normalDist(0.0f, effectiveSigma);
            // - For the azimuthal angle phi, we use a uniform distribution over 0 to 2π.
            std::uniform_real_distribution<float> uniformDist(0.0f, 6.283185307f); // 2π

            // Sample the angular deviation from the central direction.
            float theta = normalDist(g_rng);
            // Sample the azimuth angle uniformly.
            float phi = uniformDist(g_rng);

            // Compute sine and cosine values.
            float sinTheta = std::sin(theta);
            float cosTheta = std::cos(theta);
            float sinPhi = std::sin(phi);
            float cosPhi = std::cos(phi);

            // In the coordinate system where 'dir' is the z–axis, the random vector is:
            // (sinθ*cosφ, sinθ*sinφ, cosθ)
            // Now transform this vector to world space using the computed basis.
            Vector3 result = right * (sinTheta * cosPhi) + up * (sinTheta * sinPhi) + dir * (cosTheta);

            // Set this vector to the output (the 'this' vector).
            *this = result;
        }
    };
    static_assert(sizeof(Vector3) == 0xC);

    static auto& zero_vector = addr_as_ref<Vector3>(0x0173C378);
    static auto& file_default_vector = *reinterpret_cast<Vector3*>(0x01BDB238);

    struct Vector2
    {
        float x = 0.0f;
        float y = 0.0f;

        bool operator==(const Vector2& other) const = default;
    };

    static auto& vec2_zero_vector = addr_as_ref<Vector2>(0x0173C370);
    static auto& vec_dist = addr_as_ref<float(const rf::Vector3*, const rf::Vector3*)>(0x004FAED0);
    static auto& vec_dist_squared = addr_as_ref<float(const rf::Vector3*, const rf::Vector3*)>(0x004FAF00);
    static auto& vec_dist_approx = addr_as_ref<float(const rf::Vector3*, const rf::Vector3*)>(0x004FAF30);
    }
