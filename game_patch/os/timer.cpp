#include <windows.h>
#include <patch_common/FunHook.h>
#include <patch_common/AsmWriter.h>
#include "../rf/os/timer.h"
#include "os.h"
#include <patch_common/CodeInjection.h>

int64_t timer::get_i64(const int scale) {
    LARGE_INTEGER current_value{};
    // QPC is monotonic.
    QueryPerformanceCounter(&current_value);
    const int64_t current = current_value.QuadPart;
    const int64_t delta = current - rf::timer::last_value;
    rf::timer::last_value = current;
    const int64_t freq = g_qpc_frequency.QuadPart;
    constexpr int64_t MAX_JUMP_SECONDS = 32LL;
    // In theory, if a hypervisor is faulty, live migration may cause QPC to go backwards
    // or jump too far forwards,
    if (delta < 0) {
        xlog::trace("Time went backwards");
        rf::timer::base += delta;
    } else if (delta > freq * MAX_JUMP_SECONDS) {
        xlog::trace("Time jumped too far forwards");
        rf::timer::base += delta;
    }
    // Count from start-up.
    const int64_t elapsed = current - rf::timer::base;
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

CodeInjection timer_init_patch{
    0x00504A50,
    [] (auto& regs) {
        // `rf::timer::freq` can overflow, because it is not a `LARGE_INTEGER`.
        QueryPerformanceFrequency(&g_qpc_frequency);

        // Prevent a near impossible edge case in which QPC jumps backwards or too far
        // forwards.
        rf::timer::last_value = rf::timer::base;

        regs.eip = 0x00504A57;
    },
};

void timer_apply_patch() {
    // Remove `Sleep` calls in `timer_init`.
    AsmWriter{0x00504A67, 0x00504A82}.nop();
    timer_get_hook.install();
    timer_init_patch.install();
}
