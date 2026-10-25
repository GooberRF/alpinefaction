#pragma once

// RED's engine value types under the game's rf:: names and members (game_patch/rf/math, rf/os), for the shared
// Direct3D 11 core. Layout-identical to the editor's own Vector3, Matrix3, Plane, Color and VArray.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <patch_common/MemUtils.h>

namespace rf
{
    using uint = unsigned int;
    using ushort = unsigned short;
    using ubyte = unsigned char;

    struct Vector3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;

        Vector3() = default;
        Vector3(float x, float y, float z) : x(x), y(y), z(z) {}

        bool operator==(const Vector3& other) const = default;

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

        [[nodiscard]] Vector3 operator/(const float scale) const
        {
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
    };
    static_assert(sizeof(Vector3) == 0xC);

    struct Vector2
    {
        float x = 0.0f;
        float y = 0.0f;

        bool operator==(const Vector2& other) const = default;
    };
    static_assert(sizeof(Vector2) == 0x8);

    struct Matrix3
    {
        Vector3 rvec;
        Vector3 uvec;
        Vector3 fvec;

        bool operator==(const Matrix3& other) const = default;

        // FUN_004b1e40 (RF 0x004FCCF0)
        void inverse()
        {
            AddrCaller{0x004B1E40}.this_call(this);
        }

        // FUN_00401e20 (RF 0x0040EA80)
        void mul(const Matrix3& other)
        {
            AddrCaller{0x00401E20}.this_call(this, this, &other);
        }

        Vector3 transform_vector(const Vector3& v) const
        {
            return Vector3{
                rvec.x * v.x + uvec.x * v.y + fvec.x * v.z,
                rvec.y * v.x + uvec.y * v.y + fvec.y * v.z,
                rvec.z * v.x + uvec.z * v.y + fvec.z * v.z,
            };
        }
    };
    static_assert(sizeof(Matrix3) == 0x24);

    struct Matrix43
    {
        Matrix3 orient;
        Vector3 origin;

        bool operator==(const Matrix43& other) const = default;
    };
    static_assert(sizeof(Matrix43) == 0x30);

    struct Plane
    {
        Vector3 normal;
        float offset;

        [[nodiscard]] float distance_to_point(const Vector3& pt) const
        {
            return pt.dot_prod(normal) + offset;
        }
    };
    static_assert(sizeof(Plane) == 0x10);

    namespace gr
    {
        struct Color
        {
            ubyte red;
            ubyte green;
            ubyte blue;
            ubyte alpha;

            constexpr Color() : red(255), green(255), blue(255), alpha(255) {}

            constexpr Color(ubyte r, ubyte g, ubyte b, ubyte a = 255) :
                red(r), green(g), blue(b), alpha(a) {}

            void set(ubyte r, ubyte g, ubyte b, ubyte a)
            {
                this->red = r;
                this->green = g;
                this->blue = b;
                this->alpha = a;
            }

            bool operator==(const Color& other) const = default;

            static Color from_hex(uint32_t hex_color, bool has_alpha = true)
            {
                ubyte r = (hex_color >> 24) & 0xFF;
                ubyte g = (hex_color >> 16) & 0xFF;
                ubyte b = (hex_color >> 8) & 0xFF;
                ubyte a = has_alpha ? (hex_color & 0xFF) : 255;

                return Color(r, g, b, a);
            }
        };
        static_assert(sizeof(Color) == 0x4);
    }

    using Color = gr::Color;

    // An engine-owned array, read only: RED's allocator owns its storage.
    template<typename T>
    class VArray
    {
        int size_ = 0;
        int capacity_ = 0;
        T* elements_ = nullptr;

    public:
        VArray() = default;
        VArray(const VArray&) = delete;
        VArray& operator=(const VArray&) = delete;

        [[nodiscard]] int size() const noexcept
        {
            return size_;
        }

        [[nodiscard]] int capacity() const noexcept
        {
            return capacity_;
        }

        [[nodiscard]] bool empty() const noexcept
        {
            return size_ == 0;
        }

        [[nodiscard]] T& operator[](const size_t index)
        {
            return at(index);
        }

        [[nodiscard]] const T& operator[](const size_t index) const
        {
            return at(index);
        }

        [[nodiscard]] T& at(const size_t index)
        {
            if (std::cmp_greater_equal(index, size_)) {
                throw std::out_of_range{"index is out of range"};
            }
            return elements_[index];
        }

        [[nodiscard]] const T& at(const size_t index) const
        {
            if (std::cmp_greater_equal(index, size_)) {
                throw std::out_of_range{"index is out of range"};
            }
            return elements_[index];
        }

        [[nodiscard]] T& at_unchecked(const size_t index) noexcept
        {
            return elements_[index];
        }

        [[nodiscard]] const T& at_unchecked(const size_t index) const noexcept
        {
            return elements_[index];
        }

        [[nodiscard]] T* begin() noexcept
        {
            return elements_;
        }

        [[nodiscard]] const T* begin() const noexcept
        {
            return elements_;
        }

        [[nodiscard]] T* end() noexcept
        {
            return elements_ + size_;
        }

        [[nodiscard]] const T* end() const noexcept
        {
            return elements_ + size_;
        }
    };
    static_assert(sizeof(VArray<char>) == 0xC);

    // An engine-owned fixed array, read only
    template<typename T, int N>
    class FArray
    {
        int size_ = 0;
        T elements_[N] = {};

    public:
        [[nodiscard]] int size() const noexcept
        {
            return size_;
        }

        [[nodiscard]] const T& operator[](const size_t index) const
        {
            if (std::cmp_greater_equal(index, size_)) {
                throw std::out_of_range{"index is out of range"};
            }
            return elements_[index];
        }

        [[nodiscard]] const T* begin() const noexcept
        {
            return elements_;
        }

        [[nodiscard]] const T* end() const noexcept
        {
            return elements_ + size_;
        }
    };
    static_assert(sizeof(FArray<char, 8>) == 0xC);

    // An engine linked list threaded through T::next[I], read only
    template<typename T, int I = 0>
    class VList
    {
        T* head;
        int num_elements;

    public:
        class Iterator
        {
            T* current_;

        public:
            Iterator(T* ptr) : current_(ptr) {}

            T& operator*() const
            {
                return *current_;
            }

            Iterator& operator++()
            {
                current_ = current_->next[I];
                return *this;
            }

            Iterator operator++(int)
            {
                Iterator copy = *this;
                current_ = current_->next[I];
                return copy;
            }

            bool operator==(const Iterator& other) const = default;
        };

        class ConstIterator
        {
            const T* current_;

        public:
            ConstIterator(const T* ptr) : current_(ptr) {}

            const T& operator*() const
            {
                return *current_;
            }

            ConstIterator& operator++()
            {
                current_ = current_->next[I];
                return *this;
            }

            ConstIterator operator++(int)
            {
                ConstIterator copy = *this;
                current_ = current_->next[I];
                return copy;
            }

            bool operator==(const ConstIterator& other) const = default;
        };

        [[nodiscard]] T* first()
        {
            return head;
        }

        [[nodiscard]] const T* first() const
        {
            return head;
        }

        [[nodiscard]] T* next(T* elem)
        {
            return elem->next[I];
        }

        [[nodiscard]] const T* next(const T* elem) const
        {
            return elem->next[I];
        }

        [[nodiscard]] int size() const
        {
            return num_elements;
        }

        [[nodiscard]] bool empty() const
        {
            return num_elements == 0;
        }

        [[nodiscard]] Iterator begin()
        {
            return Iterator{head};
        }

        [[nodiscard]] ConstIterator begin() const
        {
            return ConstIterator{head};
        }

        [[nodiscard]] Iterator end()
        {
            return Iterator{nullptr};
        }

        [[nodiscard]] ConstIterator end() const
        {
            return ConstIterator{nullptr};
        }
    };
    static_assert(sizeof(VList<int>) == 0x8);
}
