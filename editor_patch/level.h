#pragma once

#include <map>
#include <vector>
#include <string>
#include "vtypes.h"
#include "mfc_types.h"
#include "resources.h"

constexpr std::size_t stock_cdedlevel_size = 0x608;
constexpr int alpine_props_chunk_id = 0x0AFBA5ED;

// should match structure in game_patch\misc\level.h
struct AlpineLevelProperties
{
    // defaults for new levels
    // v1
    bool legacy_cyclic_timers = false;
    // v2
    bool legacy_movers = false;
    bool starts_with_headlamp = true;

    static constexpr std::uint32_t current_alpine_chunk_version = 2u;

    // defaults for existing levels, overwritten for maps with these fields in their alpine level props chunk
    // relevant for maps without alpine level props and maps with older alpine level props versions
    // should always match stock game behaviour
    void LoadDefaults()
    {
        legacy_cyclic_timers = true;
        legacy_movers = true;
        starts_with_headlamp = true;
    }

    void Serialize(rf::File& file) const
    {
        file.write<std::uint32_t>(current_alpine_chunk_version);

        // v1
        file.write<std::uint8_t>(legacy_cyclic_timers ? 1u : 0u);
        // v2
        file.write<std::uint8_t>(legacy_movers ? 1u : 0u);
        file.write<std::uint8_t>(starts_with_headlamp ? 1u : 0u);
    }

    void Deserialize(rf::File& file, std::size_t chunk_len)
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

struct CDedLevel
{
    char padding_before_selection[0x298];
    VArray<DedObject*> selection;
    char padding_after_selection[0x608 - (0x298 + 0xC)];

    std::size_t BeginRflSection(rf::File& file, int chunk_id)
    {
        return AddrCaller{0x00430B60}.this_call<std::size_t>(this, &file, chunk_id);
    }

    void EndRflSection(rf::File& file, std::size_t start_pos)
    {
        return AddrCaller{0x00430B90}.this_call(this, &file, start_pos);
    }

    AlpineLevelProperties& GetAlpineLevelProperties()
    {
        return struct_field_ref<AlpineLevelProperties>(this, stock_cdedlevel_size);
    }

    static CDedLevel* Get()
    {
        return AddrCaller{0x004835F0}.c_call<CDedLevel*>();
    }
};
static_assert(sizeof(CDedLevel) == 0x608);
