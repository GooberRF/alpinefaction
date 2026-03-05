#include <windows.h>
#include <timeapi.h>
#include <xlog/xlog.h>
#include <patch_common/FunHook.h>
#include <patch_common/AsmWriter.h>
#include "../rf/os/timer.h"

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
        current_qpc_value.QuadPart -= rf::timer_base;
        // Multiply with unit scale (eg. ms/us) before division
        current_qpc_value.QuadPart *= scale;
        // Divide by frequency using 64 bits and then cast to 32 bits
        // Note: sign of result does not matter because it is used only for deltas
        return static_cast<int>(current_qpc_value.QuadPart / g_qpc_frequency.QuadPart);
    },
};

void timer_apply_patch()
{
    // Remove Sleep calls in timer_init
    AsmWriter(0x00504A67, 0x00504A82).nop();

    // Fix timer_get handling of frequency greater than 2MHz (sign bit is set in 32 bit dword)
    QueryPerformanceFrequency(&g_qpc_frequency);
    timer_get_hook.install();
}
