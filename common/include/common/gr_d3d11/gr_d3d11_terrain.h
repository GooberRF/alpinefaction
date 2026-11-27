#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>
#include <d3d11.h>
#include <common/ComPtr.h>
#include <gr_d3d11_engine.h>
#include "gr_d3d11_terrain_source.h"

namespace gr::d3d11
{
    class RenderContext;

    // Terrain shader slots: cbuffer b7, SRVs t7-t33, samplers s6-s7 (standard_ps.hlsl, -DTERRAIN); crater detail
    // uses RenderContext's t34/s8/b2.
    class TerrainRenderer
    {
    public:
        explicit TerrainRenderer(ComPtr<ID3D11Device> device);
        ~TerrainRenderer();

        // Creates terrain `index`'s weight, height and overlay coverage maps and premultiplied overlay
        // textures and loads its layer bitmaps, once per level. False when the terrain cannot draw with
        // the terrain shader; its faces then take the ordinary path.
        bool prepare(int index);

        // Binds what the terrain shader reads for terrain `index`; false when prepare did not accept it.
        bool bind(ID3D11DeviceContext* context, RenderContext& render_context, int index);

        // The same for a terrain the caller holds, by a key of its choosing. Its maps can be updated in place, so
        // it is prepared once per shape: a source of other dimensions, layers or overlays is prepared afresh.
        bool prepare(const void* key, const TerrainSource& source);
        bool bind(ID3D11DeviceContext* context, RenderContext& render_context, const void* key,
                  const TerrainSource& source);
        // Copy the heights of vertices [x0, x1) x [z0, z1), or the weight and overlay texels of cells
        // [x0, x1) x [z0, z1), from `source` into the key's maps.
        void update_heights(ID3D11DeviceContext* context, const void* key, const TerrainSource& source,
                            uint32_t x0, uint32_t z0, uint32_t x1, uint32_t z1);
        void update_maps(ID3D11DeviceContext* context, const void* key, const TerrainSource& source, uint32_t x0,
                         uint32_t z0, uint32_t x1, uint32_t z1);
        void release(const void* key);

        // Binds bitmap `bm` as the texture of the crater faces drawn next.
        void bind_crater(ID3D11DeviceContext* context, RenderContext& render_context, int bm);

        // Clears the SRVs bind and bind_crater set, so no pass keeps a terrain's textures alive.
        void unbind(ID3D11DeviceContext* context);

        // Frees every terrain's resources; indices refer to the next level's terrains after this.
        void release();

    private:
        struct TerrainGpu;

        bool create_shared();
        // The key's maps when `source` still fits them
        const TerrainGpu* updatable(const void* key, const TerrainSource& source) const;
        bool bind_gpu(ID3D11DeviceContext* context, RenderContext& render_context, const TerrainGpu& gpu,
                      const TerrainSource& source, const std::optional<AfTerrainChart>& chart,
                      const SunLightState& sun);

        ComPtr<ID3D11Device> device_;
        std::vector<TerrainGpu> gpu_;
        std::unordered_map<const void*, std::unique_ptr<TerrainGpu>> keyed_;
        ComPtr<ID3D11Buffer> cbuffer_;
        ComPtr<ID3D11SamplerState> map_sampler_;
    };

    // Tints terrain faces by the batch that draws them
    extern bool g_terrain_tint_batches;
}
