#pragma once

#include <vector>
#include <unordered_set>
#include <xlog/xlog.h>
#include "../rf/geometry.h"
#include "../rf/file/file.h"

constexpr int alpine_props_chunk_id = 0x0AFBA5ED;
constexpr int dash_level_props_chunk_id = 0xDA58FA00;

// should match structure in editor_patch\level.h
struct AlpineLevelProperties
{
    uint32_t chunk_version;
    // default values if not set by level file
    // v1
    bool legacy_cyclic_timers = true;
    // v2
    bool legacy_movers = true;
    bool starts_with_headlamp = true;
    // v3
    bool override_static_mesh_ambient_light_modifier = false;
    float static_mesh_ambient_light_modifier = 2.0f;
    // v4
    bool rf2_style_geomod = false;
    // std::vector<int32_t> geoable_brush_uids; // unnecessary in game
    std::vector<int32_t> geoable_room_uids;
    // std::vector<int32_t> breakable_brush_uids; // unnecessary in game
    std::vector<int32_t> breakable_room_uids;
    std::vector<uint8_t> breakable_materials;

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
        chunk_version = version;
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

        if (version >= 3) {
            std::uint8_t u8 = 0;
            if (!read_bytes(&u8, sizeof(u8)))
                return;
            override_static_mesh_ambient_light_modifier = (u8 != 0);
            xlog::debug("[AlpineLevelProps] override_static_mesh_ambient_light_modifier {}", override_static_mesh_ambient_light_modifier);
            if (!read_bytes(&static_mesh_ambient_light_modifier, sizeof(static_mesh_ambient_light_modifier)))
                return;
            xlog::debug("[AlpineLevelProps] static_mesh_ambient_light_modifier {}", static_mesh_ambient_light_modifier);
        }

        if (version >= 4) {
            std::uint8_t u8 = 0;
            if (!read_bytes(&u8, sizeof(u8)))
                return;
            rf2_style_geomod = (u8 != 0);
            xlog::debug("[AlpineLevelProps] rf2_style_geomod {}", rf2_style_geomod);

            // Geoable entries as (brush_uid, room_uid) pairs
            uint32_t count = 0;
            if (!read_bytes(&count, sizeof(count)))
                return;
            if (count > 10000) count = 10000;
            geoable_room_uids.resize(count);
            for (uint32_t i = 0; i < count; i++) {
                int32_t brush_uid = 0; // editor-only, skip
                if (!read_bytes(&brush_uid, sizeof(brush_uid)))
                    return;
                int32_t room_uid = 0;
                if (!read_bytes(&room_uid, sizeof(room_uid)))
                    return;
                geoable_room_uids[i] = room_uid;
                xlog::debug("[AlpineLevelProps] geoable entry: brush_uid={} room_uid={}", brush_uid, room_uid);
            }
            xlog::debug("[AlpineLevelProps] geoable_room_uids count={}", count);

            // Breakable material entries as (brush_uid, room_uid, material) triples
            uint32_t bcount = 0;
            if (!read_bytes(&bcount, sizeof(bcount))) {
                xlog::warn("[AlpineLevelProps] GAME: failed to read breakable count (remaining={})", remaining);
                return;
            }
            xlog::trace("[AlpineLevelProps] GAME: breakable count raw={}", bcount);
            if (bcount > 10000) bcount = 10000;
            breakable_room_uids.resize(bcount);
            breakable_materials.resize(bcount);
            for (uint32_t i = 0; i < bcount; i++) {
                int32_t brush_uid = 0; // editor-only, skip
                if (!read_bytes(&brush_uid, sizeof(brush_uid)))
                    return;
                int32_t room_uid = 0;
                if (!read_bytes(&room_uid, sizeof(room_uid)))
                    return;
                breakable_room_uids[i] = room_uid;
                uint8_t mat = 0;
                if (!read_bytes(&mat, sizeof(mat)))
                    return;
                breakable_materials[i] = mat;
                xlog::trace("[AlpineLevelProps] GAME: breakable[{}] brush_uid={} room_uid={} material={}", i, brush_uid, room_uid, mat);
            }
            xlog::trace("[AlpineLevelProps] GAME: total breakable entries loaded={}", bcount);
        }
    }
};

struct DashLevelProps
{
    uint32_t chunk_version;
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
        chunk_version = 1u; // latest supported version
    }
};

// used by RF2-style geomod
struct RF2AnchorInfo {
    rf::GRoom* room;
    std::unordered_set<rf::GFace*> anchor_faces;
};
