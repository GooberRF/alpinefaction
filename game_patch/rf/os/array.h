#pragma once

#include <cstdint>
#include <cstring>
//#include "string.h"

namespace rf
{
    template<typename T = char>
    class VArray
    {
    private:
        int num = 0;
        int capacity = 0;
        T *elements = nullptr;

    public:
        [[nodiscard]] int size() const
        {
            return num;
        }

        [[nodiscard]] bool empty() const
        {
            return num == 0;
        }

        [[nodiscard]] T& operator[](int index)
        {
            return elements[index];
        }

        [[nodiscard]] const T& operator[](int index) const
        {
            return elements[index];
        }

        [[nodiscard]] T& get(int index) const
        {
            return elements[index];
        }

        [[nodiscard]] T* begin()
        {
            return &elements[0];
        }

        [[nodiscard]] const T* begin() const
        {
            return &elements[0];
        }

        [[nodiscard]] T* end()
        {
            return &elements[num];
        }

        [[nodiscard]] const T* end() const
        {
            return &elements[num];
        }

        void add(T element)
        {
            AddrCaller{0x0045EC40}.this_call(this, element);
        }

        void clear()
        {
            num = 0;
        }

        void erase(int index)
        {
            if (index < 0 || index >= num) {
                return; // Invalid index, do nothing
            }

            // Shift elements to the left to overwrite the erased element
            for (int i = index; i < num - 1; ++i) {
                elements[i] = elements[i + 1];
            }

            --num; // Reduce the size
        }

        template<typename Predicate>
        void erase_if(Predicate pred)
        {
            int new_size = 0;
            for (int i = 0; i < num; ++i) {
                if (!pred(elements[i])) {
                    elements[new_size++] = elements[i];
                }
            }
            num = new_size;
        }

        template<typename Predicate>
        requires std::is_invocable_r_v<bool, Predicate, T>
        [[nodiscard]] bool contains(Predicate pred) const
        {
            for (int i = 0; i < num; ++i) {
                if (pred(elements[i])) {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] bool contains(const T& value) const
        {
            for (int i = 0; i < num; ++i) {
                if (elements[i] == value) {
                    return true;
                }
            }
            return false;
        }
    };
    static_assert(sizeof(VArray<>) == 0xC);

    template<typename T = char>
    struct VArray_String
    {
        int num = 0;
        int capacity = 0;
        T* data = nullptr;

    public:
        [[nodiscard]] int size() const
        {
            return num;
        }

        [[nodiscard]] bool empty() const
        {
            return num == 0;
        }

        void clear()
        {
            num = 0;
        }

        void add(T element)
        {
            // 0x00447060 takes the 8-byte String BY VALUE on the stack and destroys it
            // (MSVC x86 callee-destroys ABI). GCC/MinGW passes non-trivial classes by
            // hidden reference instead, so forwarding `element` through this_call pushed
            // a pointer where the engine expects the raw struct — storing garbage in the
            // array and making the engine free() a garbage pointer (this poisoned RF's
            // CRT heap and crashed MinGW dedicated servers at launch). Pass the raw
            // 8 bytes explicitly — identical layout under both compilers — then zero
            // `element` so no destructor re-frees the buffer the engine now owns.
            static_assert(sizeof(T) == 8);
            uint32_t raw[2];
            std::memcpy(raw, &element, sizeof(raw));
            AddrCaller{0x00447060}.this_call(this, raw[0], raw[1]);
            std::memset(&element, 0, sizeof(raw));
        }

        T& operator[](int index)
        {
            return data[index];
        }

        T* begin()
        {
            return data;
        }

        T* end()
        {
            return data + num;
        }

        // No destructor on purpose: `data` is owned by the RF engine (allocated and
        // grown by the stock VArray::add at 0x00447060), so it must never be freed
        // here. C++ delete[] on an engine-allocated buffer is an allocator/heap
        // mismatch — tolerated under MSVC (AF shares RF's MSVCRT heap) but corrupts
        // on MinGW builds (libstdc++ delete[] expects a C++ array cookie / different
        // heap), which crashed dedicated servers via rf::netgame.levels.
    };
    static_assert(sizeof(VArray_String<>) == 0xC);

#pragma pack(push, 1)
    struct BitSet
    {
        void* buf;
        int size_in_bytes;
        bool is_buffer_allocated;

        void set(int index, int value)
        {
            AddrCaller{0x0050EA00}.this_call(this, index, value);
        }
    };
#pragma pack(pop)
    static_assert(sizeof(BitSet) == 0x9);

    template<typename T, int N>
    class FArray
    {
        int num;
        T elements[N];

    public:
        [[nodiscard]] int size() const
        {
            return num;
        }

        [[nodiscard]] T& operator[](int index)
        {
            return elements[index];
        }

        [[nodiscard]] const T& operator[](int index) const
        {
            return elements[index];
        }

        [[nodiscard]] T& get(int index) const
        {
            return elements[index];
        }

        [[nodiscard]] T* begin()
        {
            return &elements[0];
        }

        [[nodiscard]] const T* begin() const
        {
            return &elements[0];
        }

        [[nodiscard]] T* end()
        {
            return &elements[num];
        }

        [[nodiscard]] const T* end() const
        {
            return &elements[num];
        }
    };
}
