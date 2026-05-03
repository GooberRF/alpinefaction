#include <windows.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/MemUtils.h>
#include "object_private.h"

static int wall_clock_seconds_of_12h_cycle()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    return (st.wHour % 12) * 3600 + st.wMinute * 60 + st.wSecond;
}

CodeInjection clock_time_source_injection{
    0x00411253,
    [](auto& regs) {
        regs.eax = wall_clock_seconds_of_12h_cycle();
        regs.eip = 0x0041125E;
    },
};

CodeInjection clock_180deg_offset_injection{
    0x0041129A,
    [](auto& regs) {
        const auto esp = static_cast<uintptr_t>(regs.esp);
        int* sec_slot = reinterpret_cast<int*>(esp + 0x14);
        int* min_slot = reinterpret_cast<int*>(esp + 0xEC);
        int* hour_slot = reinterpret_cast<int*>(esp + 0xF0);

        *sec_slot = (*sec_slot + 30) % 60;
        *min_slot = (*min_slot + 1800) % 3600;
        *hour_slot = (*hour_slot + 21600) % 216000;

        regs.edx = static_cast<int32_t>(esp + 0xC4);
        regs.eip = 0x004112A1;
    },
};

void clock_do_patch()
{
    write_mem<float>(0x005894A4, 1.0f / 43200.0f);
    clock_time_source_injection.install();
    clock_180deg_offset_injection.install();
}
