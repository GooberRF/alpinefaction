#pragma once

// Include after the binary's rf engine types (rf::Vector3), as alpine_sun_state.h needs them.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <common/lighting/alpine_sun_state.h>
#include <common/lightmap/af_lightmap_runtime.h>
#include <common/terrain/alpine_terrain.h>

namespace gr::d3d11
{
    // A terrain as the terrain shader draws it, whatever holds it (TerrainRenderer). The pointers and spans only have
    // to outlive the call the source is passed to.
    struct TerrainSource
    {
        struct Layer
        {
            const char* texture = "";
            float uv_scale = alpine_terrain::default_uv_scale;
            bool triplanar = false;
            bool break_tiling = false; // overlays only
        };

        alpine_terrain::Header header{};
        std::span<const uint16_t> heights;
        std::span<const uint8_t> weights;          // weight map 0 then weight map 1, as in the blob
        std::span<const uint8_t> overlay_coverage; // empty without overlays
        Layer layers[alpine_terrain::max_layers];
        std::size_t num_layers = 0;
        Layer overlays[alpine_terrain::max_overlays];
        std::size_t num_overlays = 0;
        const char* underside_texture = "";
        bool fullbright = false;
        // A keyed draw's light: its chart, and the sun in place of the level's
        std::optional<AfTerrainChart> chart;
        std::optional<SunLightState> sun;
    };
}
