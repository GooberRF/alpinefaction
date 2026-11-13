#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <vector>

// A bake's alpine lightmap pages (192 KB RGB, 64 KB BC7 each) are all smaller than the blocks the Windows heap gives
// a region of their own, so hundreds of them grow the process heap, which keeps that address space reserved after
// they are freed. They come from a private heap instead, created with the first page and destroyed with the last.
void* lightmap_page_heap_alloc(std::size_t bytes);
void lightmap_page_heap_free(void* p);

template<typename T>
struct LightmapPageAllocator
{
    using value_type = T;

    LightmapPageAllocator() = default;
    template<typename U>
    LightmapPageAllocator(const LightmapPageAllocator<U>&)
    {
    }

    T* allocate(std::size_t n)
    {
        if (n > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            throw std::bad_array_new_length();
        }
        void* p = lightmap_page_heap_alloc(n * sizeof(T));
        if (!p) {
            throw std::bad_alloc();
        }
        return static_cast<T*>(p);
    }

    void deallocate(T* p, std::size_t)
    {
        lightmap_page_heap_free(p);
    }

    template<typename U>
    bool operator==(const LightmapPageAllocator<U>&) const
    {
        return true;
    }
};

using LightmapPageBuffer = std::vector<std::uint8_t, LightmapPageAllocator<std::uint8_t>>;
using LightmapPages = std::vector<LightmapPageBuffer>;
