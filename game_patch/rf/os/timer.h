#pragma once

#include <patch_common/MemUtils.h>

namespace rf::timer {
    static const auto& get = addr_as_ref<int(int frequency)>(0x00504AB0);
    static const auto& add_delta_time = addr_as_ref<int(int delta_ms)>(0x004FA2D0);
    static auto& inc_game_paused = addr_as_ref<void()>(0x004FA320);
    static auto& dec_game_paused = addr_as_ref<void()>(0x004FA330);

    static auto& base = addr_as_ref<int64_t>(0x01751BF8);
    static auto& last_value = addr_as_ref<int64_t>(0x01751BD0);
    [[deprecated]]
    static auto& freq = addr_as_ref<int32_t>(0x01751C04);
}
