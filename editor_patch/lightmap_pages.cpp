#include <windows.h>
#include <cstddef>
#include <mutex>
#ifdef _MSC_VER
#include <new.h>
#endif
#include "lightmap_pages.h"

namespace
{

std::mutex g_mutex;
HANDLE g_heap = nullptr;
std::size_t g_blocks = 0;

void* try_alloc(std::size_t bytes)
{
    const std::lock_guard lock{g_mutex};
    if (!g_heap) {
        g_heap = HeapCreate(0, 0, 0);
        if (!g_heap) {
            return nullptr;
        }
    }
    void* p = HeapAlloc(g_heap, 0, bytes ? bytes : 1);
    if (p) {
        g_blocks++;
    }
    else if (g_blocks == 0) {
        HeapDestroy(g_heap);
        g_heap = nullptr;
    }
    return p;
}

// As AF's operator new does, so the memory guard's reserve serves a page as well.
bool call_new_handler([[maybe_unused]] std::size_t bytes)
{
#ifdef _MSC_VER
    return _callnewh(bytes) != 0;
#else
    return false;
#endif
}

} // namespace

void* lightmap_page_heap_alloc(std::size_t bytes)
{
    void* p;
    while (!(p = try_alloc(bytes))) {
        if (!call_new_handler(bytes)) {
            return nullptr;
        }
    }
    return p;
}

void lightmap_page_heap_free(void* p)
{
    if (!p) {
        return;
    }
    const std::lock_guard lock{g_mutex};
    HeapFree(g_heap, 0, p);
    if (--g_blocks == 0) {
        HeapDestroy(g_heap);
        g_heap = nullptr;
    }
}
