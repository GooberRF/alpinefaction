#include <windows.h>
#include <timeapi.h>
#include <cstdlib>
#include <xlog/xlog.h>
#include <patch_common/FunHook.h>
#include <patch_common/AsmWriter.h>
#include "../rf/os/timer.h"
#include "../rf/multi.h"
#include "console.h"

void wait_for(const float ms) {
    if (ms <= .0f) {
        return;
    }

    // Should be a resolution of 500 us.
    thread_local const HANDLE timer = CreateWaitableTimerExA(
        nullptr,
        nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_MODIFY_STATE | SYNCHRONIZE
    );

    if (!timer) {
        ERR_ONCE("CreateWaitableTimerExA in wait_for failed ({})", GetLastError());
    SLEEP:
        static const MMRESULT res = timeBeginPeriod(1);
        if (res != TIMERR_NOERROR) {
            ERR_ONCE(
                "The frame rate may be unstable, because timeBeginPeriod failed ({})",
                res
            );
        }
        Sleep(static_cast<DWORD>(ms));
    } else {
        // `SetWaitableTimer` requires 100-nanosecond intervals.
        // Negative values indicate relative time.
        LARGE_INTEGER dur{
            .QuadPart = -static_cast<LONGLONG>(static_cast<double>(ms) * 10'000.)
        };

        if (!SetWaitableTimer(timer, &dur, 0, nullptr, nullptr, FALSE)) {
            ERR_ONCE("SetWaitableTimer in wait_for failed ({})", GetLastError());
            goto SLEEP;
        }

        if (WaitForSingleObject(timer, INFINITE) != WAIT_OBJECT_0) {
            ERR_ONCE("WaitForSingleObject in wait_for failed ({})", GetLastError());
            goto SLEEP;
        }
    }
}

static LARGE_INTEGER g_qpc_frequency;

// Debug offset applied to int32 result of timer_get, in seconds.
// Adding to the int32 output (rather than shifting the int64 base) preserves all
// frame-to-frame deltas while shifting absolute values toward the overflow point.
static int64_t g_timer_debug_offset_s = 0;

FunHook<int(int)> timer_get_hook{
    0x00504AB0,
    [](int scale) {
        // get QPC current value
        LARGE_INTEGER current_qpc_value;
        QueryPerformanceCounter(&current_qpc_value);
        // make sure time never goes backward
        if (current_qpc_value.QuadPart < rf::timer_last_value) {
            current_qpc_value.QuadPart = rf::timer_last_value;
        }
        rf::timer_last_value = current_qpc_value.QuadPart;
        // Make sure we count from game start
        int64_t elapsed = current_qpc_value.QuadPart - rf::timer_base;
        int64_t freq = g_qpc_frequency.QuadPart;
        // Avoid int64 overflow: (elapsed * scale) can overflow for large elapsed values
        // when scale is 1000000 (microseconds). Split into quotient and remainder.
        int64_t result = (elapsed / freq) * scale + (elapsed % freq) * scale / freq;
        // Apply debug time offset (int64 arithmetic, then truncate to int32).
        // Since the offset is constant, all deltas (new - old) are preserved.
        result += g_timer_debug_offset_s * scale;
        // Note: sign of result does not matter because it is used only for deltas
        return static_cast<int>(result);
    },
};

// Game loop stores its previous timer_get(1000000) return value here for delta computation
static auto& game_loop_prev_timer_us = addr_as_ref<int>(0x01754614);

ConsoleCommand2 dbg_timer_shift_cmd{
    "dbg_timershift",
    [](std::optional<float> days) {
        if (!rf::is_dedicated_server) {
            rf::console::print("This command is only available on dedicated servers.");
            return;
        }
        if (!days || *days <= 0.0f) {
            int uptime_ms = rf::timer_get(1000);
            float uptime_days = static_cast<float>(uptime_ms) / (1000.0f * 86400.0f);
            rf::console::print("Current timer_get(1000) = {} ({:.2f} days)", uptime_ms, uptime_days);
            rf::console::print("Usage: dbg_timershift <days>");
            rf::console::print("  Best used at startup via dedicated_server.txt or $AF_DBG_TIMERSHIFT env var.");
            return;
        }
        if (rf::multi_num_players() > 0) {
            rf::console::print("Cannot shift timer while players are connected.");
            rf::console::print("Run this command before players join, or set the AF_DBG_TIMERSHIFT");
            rf::console::print("environment variable (in days) before starting the server.");
            return;
        }
        // Capture timer values before and after applying the offset so we can
        // adjust the game loop's stored "previous frame" value and prevent a
        // one-frame delta discontinuity that would feed a bogus value into
        // timer_add_delta_time and corrupt the Timestamp system.
        int old_us = rf::timer_get(1000000);
        g_timer_debug_offset_s += static_cast<int64_t>(*days * 86400.0);
        int new_us = rf::timer_get(1000000);
        game_loop_prev_timer_us += (new_us - old_us);

        int new_ms = rf::timer_get(1000);
        float total_days = static_cast<float>(g_timer_debug_offset_s) / 86400.0f;
        rf::console::print("Shifted timer by {:.1f} days (total offset: {:.1f} days). timer_get(1000) = {}",
            *days, total_days, new_ms);
    },
    "Shift the game timer forward by N days to test timer overflow behavior",
    "dbg_timershift <days>",
};

void timer_apply_patch()
{
    // Remove Sleep calls in timer_init
    AsmWriter(0x00504A67, 0x00504A82).nop();

    // Fix timer_get handling of frequency greater than 2MHz (sign bit is set in 32 bit dword)
    QueryPerformanceFrequency(&g_qpc_frequency);
    timer_get_hook.install();

    // Check for debug timer shift via environment variable.
    // Applied here (before any timer_get calls) so there is no discontinuity.
    // Usage: set AF_DBG_TIMERSHIFT=24.6 (days) before starting the server.
    const char* shift_env = std::getenv("AF_DBG_TIMERSHIFT");
    if (shift_env) {
        char* end = nullptr;
        double shift_days = std::strtod(shift_env, &end);
        if (end != shift_env && shift_days > 0.0) {
            g_timer_debug_offset_s = static_cast<int64_t>(shift_days * 86400.0);
            xlog::info("Debug timer shift: {:.1f} days ({} seconds offset)", shift_days, g_timer_debug_offset_s);
        }
    }

    dbg_timer_shift_cmd.register_cmd();
}
