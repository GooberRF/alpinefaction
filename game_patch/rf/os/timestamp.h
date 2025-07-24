#pragma once

#include <patch_common/MemUtils.h>

namespace rf
{
    struct Timestamp
    {
        int value = -1;

        [[nodiscard]] bool elapsed() const
        {
            return AddrCaller{0x004FA3F0}.this_call<bool>(this);
        }

        int set(int value_ms)
        {
            return AddrCaller{0x004FA360}.this_call<int>(this, value_ms);
        }

        [[nodiscard]] bool valid() const
        {
            return value >= 0;
        }

        [[nodiscard]] int time_until() const
        {
            return AddrCaller{0x004FA420}.this_call<int>(this);
        }

        [[nodiscard]] int time_since() const
        {
            return AddrCaller{0x004FA460}.this_call<int>(this);
        }

        [[nodiscard]] bool is_set() const
        {
            return AddrCaller{0x0040A0D0}.this_call<bool>(this);
        }

        void invalidate()
        {
            AddrCaller{0x004FA3E0}.this_call(this);
        }
    };
    static_assert(sizeof(Timestamp) == 0x4);

    struct TimestampRealtime
    {
        int value = -1;

        [[nodiscard]] bool elapsed() const
        {
            return AddrCaller{0x004FA560}.this_call<bool>(this);
        }

        void set(int value_ms)
        {
            AddrCaller{0x004FA4D0}.this_call(this, value_ms);
        }

        [[nodiscard]] int time_until() const
        {
            return AddrCaller{0x004FA590}.this_call<int>(this);
        }

        [[nodiscard]] bool valid() const
        {
            return AddrCaller{0x004FA5E0}.this_call<bool>(this);
        }

        void invalidate()
        {
            AddrCaller{0x004FA550}.this_call(this);
        }
    };
    static_assert(sizeof(TimestampRealtime) == 0x4);
}
