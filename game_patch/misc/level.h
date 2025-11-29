#pragma once

#include <xlog/xlog.h>
#include "../rf/file/file.h"

constexpr int alpine_props_chunk_id = 0x0AFBA5ED;
constexpr int dash_level_props_chunk_id = 0xDA58FA00;

// should match structure in editor_patch\level.h
struct AlpineLevelProperties
{
    // default values if not set by level file
    // v1
    bool legacy_cyclic_timers = true;
    // v2
    bool legacy_movers = true;
    bool starts_with_headlamp = true;

    static AlpineLevelProperties& instance()
    {
        static AlpineLevelProperties instance;
        return instance;
    }

    void deserialize(rf::File& file, std::size_t chunk_len)
    {
        std::size_t remaining = chunk_len;

        // scope-exit: always skip any unread tail (forward compatibility for unknown newer fields)
        struct Tail
        {
            rf::File& f;
            std::size_t& rem;
            bool active = true;
            ~Tail()
            {
                if (active && rem) {
                    f.seek(static_cast<int>(rem), rf::File::seek_cur);
                }
            }
            void dismiss()
            {
                active = false;
            }
        } tail{file, remaining};

        auto read_bytes = [&](void* dst, std::size_t n) -> bool {
            if (remaining < n)
                return false;
            int got = file.read(dst, n);
            if (got != static_cast<int>(n) || file.error())
                return false;
            remaining -= n;
            return true;
        };

        // version
        std::uint32_t version = 0;
        if (!read_bytes(&version, sizeof(version))) {
            xlog::warn("[AlpineLevelProps] chunk too small for version header (len={})", chunk_len);
            return;
        }
        if (version < 1) {
            xlog::warn("[AlpineLevelProps] unexpected version {} (chunk_len={})", version, chunk_len);
            return;
        }
        xlog::debug("[AlpineLevelProps] version {}", version);

        if (version >= 1) {
            std::uint8_t u8 = 0;
            if (!read_bytes(&u8, sizeof(u8)))
                return;
            legacy_cyclic_timers = (u8 != 0);
            xlog::debug("[AlpineLevelProps] legacy_cyclic_timers {}", legacy_cyclic_timers);
        }

        if (version >= 2) {
            std::uint8_t u8 = 0;
            if (!read_bytes(&u8, sizeof(u8)))
                return;
            legacy_movers = (u8 != 0);
            xlog::debug("[AlpineLevelProps] legacy_movers {}", legacy_movers);
            if (!read_bytes(&u8, sizeof(u8)))
                return;
            starts_with_headlamp = (u8 != 0);
            xlog::debug("[AlpineLevelProps] starts_with_headlamp {}", starts_with_headlamp);
        }
    }
};

struct DashLevelProps
{
    // default values for if not set
    bool lightmaps_full_depth = false; // since DashLevelProps v1

    static DashLevelProps& instance()
    {
        static DashLevelProps instance;
        return instance;
    }

    void deserialize(rf::File& file)
    {
        lightmaps_full_depth = file.read<std::uint8_t>();
        xlog::debug("[DashLevelProps] lightmaps_full_depth {}", lightmaps_full_depth);
    }
};
