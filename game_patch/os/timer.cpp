#include <windows.h>
#include <patch_common/FunHook.h>
#include <patch_common/AsmWriter.h>
#include "../rf/os/timer.h"
#include "os.h"

int64_t timer::get_i64(const int scale) {
    LARGE_INTEGER current_value{};
    // QPC is monotonic.
    QueryPerformanceCounter(&current_value);
    // Guard against QPC going backward (e.g. buggy hypervisor during VM live-migration)
    if (current_value.QuadPart < rf::timer::last_value) {
        current_value.QuadPart = rf::timer::last_value;
    }
    const int64_t elapsed = current_value.QuadPart - rf::timer::base;
    rf::timer::last_value = current_value.QuadPart;
    const int64_t freq = g_qpc_frequency.QuadPart;
    // Avoid overflow for large elapsed values.
    return (elapsed / freq) * scale + (elapsed % freq) * scale / freq;
}

FunHook<int(int)> timer_get_hook{
    0x00504AB0,
    [] (const int scale) {
        // Note: sign of result does not matter because it is used only for deltas
        return static_cast<int>(timer::get_i64(scale));
    },
};

void timer_apply_patch()
{
    // Remove Sleep calls in timer_init
    AsmWriter(0x00504A67, 0x00504A82).nop();

    // Fix timer_get handling of frequency greater than 2 GHz (sign bit is set in 32 bit dword)
    QueryPerformanceFrequency(&g_qpc_frequency);
    timer_get_hook.install();
}
