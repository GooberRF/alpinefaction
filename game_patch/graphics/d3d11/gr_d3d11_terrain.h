#pragma once

#include <vector>
#include <d3d11.h>
#include <common/ComPtr.h>

namespace gr::d3d11
{
    class RenderContext;

    // Terrain shader slots: cbuffer b7, SRVs t7-t24, samplers s6-s7 (standard_ps.hlsl, -DTERRAIN).
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

        // Binds bitmap `bm` as the texture of the crater faces drawn next.
        void bind_crater(ID3D11DeviceContext* context, RenderContext& render_context, int bm);

        // Clears the SRVs bind and bind_crater set, so no pass keeps a terrain's textures alive.
        void unbind(ID3D11DeviceContext* context);

        // Frees every terrain's resources; indices refer to the next level's terrains after this.
        void release();

    private:
        struct TerrainGpu;

        bool create_shared();

        ComPtr<ID3D11Device> device_;
        std::vector<TerrainGpu> gpu_;
        ComPtr<ID3D11Buffer> cbuffer_;
        ComPtr<ID3D11SamplerState> map_sampler_;
    };

    void terrain_register_commands();
}
