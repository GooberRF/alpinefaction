#include <windows.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <new>
#ifdef _MSC_VER
#include <new.h>
#endif
#include <patch_common/FunHook.h>
#include <xlog/xlog.h>
#include "bake_progress.h"
#include "headless_bake.h"
#include "level.h"
#include "memory_guard.h"

namespace
{

constexpr std::size_t reserve_size = 64u << 20;
// left free beside a new reserve, so holding it does not itself starve RED
constexpr std::uint64_t rearm_min_free_block = 2ull * reserve_size;
constexpr DWORD rearm_interval_ms = 1000;
// headless_bake.h lists it with the bake's other exit codes
constexpr UINT fatal_exit_code = 7;
constexpr const char* caption = "Out of Memory";

std::atomic<void*> g_reserve{nullptr};
std::atomic<bool> g_tripped{false};
DWORD g_last_rearm_attempt = 0;
int g_quiet_scopes = 0;

void* reserve_block()
{
    // top down, away from the low address space the heaps grow into
    return VirtualAlloc(nullptr, reserve_size, MEM_RESERVE | MEM_TOP_DOWN, PAGE_NOACCESS);
}

// Called from inside a failed allocation: no allocation, lock or logging. A request the reserve could not cover never
// touches it.
bool fits_reserve(std::uint64_t bytes)
{
    return bytes != 0 && bytes <= reserve_size;
}

// The reserve is given back once, however many threads fail at the same time; true for the call that gave it back.
bool give_back_reserve()
{
    void* block = g_reserve.exchange(nullptr);
    if (!block) {
        return false;
    }
    VirtualFree(block, 0, MEM_RELEASE);
    g_tripped = true;
    return true;
}

// For RED's allocators, which retry once: worth it whichever thread gave the reserve back.
bool release_reserve_for(std::uint64_t bytes)
{
    if (!fits_reserve(bytes)) {
        return false;
    }
    give_back_reserve();
    return true;
}

#ifdef _MSC_VER
// AF's own operator new retries for as long as this returns nonzero, so only the call that gave the reserve back does.
int __cdecl af_new_handler(std::size_t size)
{
    return fits_reserve(size) && give_back_reserve() ? 1 : 0;
}
#endif

// RED's static CRT: malloc (0x0051A75F) and operator new (0x0052EE74) both allocate through __nh_malloc (cdecl,
// size and new_mode); calloc and realloc reach the heap on their own. RED's heap grows by VirtualAlloc'd segments,
// so the released reserve serves the retry.
void* __cdecl red_nh_malloc_hooked(std::size_t size, int new_mode);
FunHook<decltype(red_nh_malloc_hooked)> red_nh_malloc_hook{0x0051A771, red_nh_malloc_hooked};
void* __cdecl red_nh_malloc_hooked(std::size_t size, int new_mode)
{
    void* block = red_nh_malloc_hook.call_target(size, new_mode);
    if (!block && release_reserve_for(size)) {
        block = red_nh_malloc_hook.call_target(size, new_mode);
    }
    return block;
}

void* __cdecl red_calloc_hooked(std::size_t count, std::size_t size);
FunHook<decltype(red_calloc_hooked)> red_calloc_hook{0x0051E3EE, red_calloc_hooked};
void* __cdecl red_calloc_hooked(std::size_t count, std::size_t size)
{
    void* block = red_calloc_hook.call_target(count, size);
    if (!block && release_reserve_for(static_cast<std::uint64_t>(count) * size)) {
        block = red_calloc_hook.call_target(count, size);
    }
    return block;
}

// A failed realloc leaves the old block as it was, and size 0 frees it and returns null.
void* __cdecl red_realloc_hooked(void* old_block, std::size_t size);
FunHook<decltype(red_realloc_hooked)> red_realloc_hook{0x0052142C, red_realloc_hooked};
void* __cdecl red_realloc_hooked(void* old_block, std::size_t size)
{
    void* block = red_realloc_hook.call_target(old_block, size);
    if (!block && release_reserve_for(size)) {
        block = red_realloc_hook.call_target(old_block, size);
    }
    return block;
}

} // namespace

const char* memory_guard_advice()
{
    return editor_large_address_aware()
        ? "RED can only use 4 GB; turn off High-resolution lightmaps or simplify the level, then save and restart RED."
        : "RED can only use 2 GB; turn off High-resolution lightmaps or simplify the level, then save and restart RED.";
}

bool memory_guard_take_tripped()
{
    // polled by the bake's progress pump: a plain load first
    return g_tripped.load(std::memory_order_relaxed) && g_tripped.exchange(false);
}

MemoryGuardQuietScope::MemoryGuardQuietScope()
{
    g_quiet_scopes++;
}

MemoryGuardQuietScope::~MemoryGuardQuietScope()
{
    g_quiet_scopes--;
}

bool memory_guard_tripped()
{
    return g_tripped.load(std::memory_order_relaxed);
}

bool memory_guard_rearm()
{
    if (g_reserve.load()) {
        return true;
    }
    std::uint64_t largest = 0;
    std::uint64_t total = 0;
    editor_address_space_free(largest, total);
    if (largest < rearm_min_free_block) {
        return false;
    }
    void* block = reserve_block();
    if (!block) {
        return false;
    }
    void* expected = nullptr;
    if (!g_reserve.compare_exchange_strong(expected, block)) {
        VirtualFree(block, 0, MEM_RELEASE);
    }
    return true;
}

bool memory_guard_ready()
{
    return !memory_guard_take_tripped() && memory_guard_rearm();
}

void memory_guard_idle()
{
    const CDedLevel* level = CDedLevel::Get();
    // not in the middle of something: a modal dialog or stock command that disabled the main frame waits too
    HWND frame = GetMainFrameHandle();
    if ((level && level->build_running) || bake_progress_active() || g_quiet_scopes > 0 ||
        (frame && !IsWindowEnabled(frame))) {
        return;
    }
    if (memory_guard_take_tripped()) {
        editor_report_blocking("Memory", caption, memory_guard_warning);
    }
    if (g_reserve.load()) {
        return;
    }
    const DWORD now = GetTickCount();
    if (now - g_last_rearm_attempt < rearm_interval_ms) {
        return;
    }
    g_last_rearm_attempt = now;
    if (memory_guard_rearm()) {
        xlog::info("[Memory] emergency reserve held again");
    }
}

void memory_guard_fatal(const char* operation, bool level_file_untouched)
{
    // nothing here may allocate from a heap that just ran dry
    char msg[512];
    std::snprintf(msg, sizeof(msg),
                  "RED ran out of memory (or hit an internal error) part-way through %s and cannot continue safely. "
                  "%sRED will now close.\n\nRED can only use %s; turn off High-resolution lightmaps or simplify the "
                  "level.",
                  operation, level_file_untouched ? "The level file on disk has not been changed. " : "",
                  editor_large_address_aware() ? "4 GB" : "2 GB");
    // whose exception, and whether the reserve was still there to serve it; given back now, for the lines and box below
    void* const reserve = g_reserve.exchange(nullptr);
    if (reserve) {
        VirtualFree(reserve, 0, MEM_RELEASE);
    }
    const char* kind = "RED or MFC";
    const char* what = "";
    try {
        throw;
    }
    catch (const std::bad_alloc&) {
        kind = "AF std::bad_alloc";
    }
    catch (const std::exception& e) {
        kind = "AF std::exception";
        what = e.what();
    }
    catch (...) {
    }
    char diag[192];
    std::snprintf(diag, sizeof(diag), "fatal exception: %s, emergency reserve %s%s%s", kind,
                  reserve ? "held" : "spent", *what ? ": " : "", what);
    if (headless_bake_active()) {
        // best effort: the bake log lines (which also go to the AlpineEditor log) matter most
        try {
            headless_bake_note(msg);
            headless_bake_note(diag);
        }
        catch (...) {
        }
    }
    else {
        try {
            xlog::error("[Memory] {}", msg);
            xlog::error("[Memory] {}", diag);
        }
        catch (...) {
        }
        // Shown by the system: no message loop here that would dispatch and repaint into the broken state, and no new
        // thread, whose start-up can block on locks held while memory is exhausted.
        MessageBoxA(nullptr, msg, caption, MB_OK | MB_ICONERROR | MB_SERVICE_NOTIFICATION);
    }
    TerminateProcess(GetCurrentProcess(), fatal_exit_code);
    ExitProcess(fatal_exit_code);
}

void ApplyMemoryGuardPatches()
{
    g_reserve = reserve_block();
    if (!g_reserve.load()) {
        xlog::warn("[Memory] could not reserve the emergency block ({})", GetLastError());
    }
    red_nh_malloc_hook.install();
    red_calloc_hook.install();
    red_realloc_hook.install();
#ifdef _MSC_VER
    // AF's allocations inside RED's build and bake: the size lets a request too big for the reserve fail as before
    _set_new_handler(af_new_handler);
#endif
}
