#pragma once

#include <d3d11.h>

namespace gr::d3d11
{
    class RenderContext;

    // Terrain shader slots: cbuffer b7, SRVs t7-t24, samplers s6-s7 (standard_ps.hlsl, -DTERRAIN).

    // Creates terrain `index`'s weight, height and overlay coverage maps and premultiplied overlay
    // textures on `device` and loads its layer bitmaps, once per level and device. False when the
    // terrain cannot draw with the terrain shader; its faces then take the ordinary path.
    bool terrain_gpu_prepare(ID3D11Device* device, int index);

    // Binds what the terrain shader reads for terrain `index`; false when terrain_gpu_prepare did not
    // accept it.
    bool terrain_gpu_bind(ID3D11DeviceContext* context, RenderContext& render_context, int index);

    // Binds bitmap `bm` as the texture of the crater faces drawn next.
    void terrain_gpu_bind_crater(ID3D11DeviceContext* context, RenderContext& render_context, int bm);

    // Clears the SRVs terrain_gpu_bind and terrain_gpu_bind_crater set, so no pass keeps a terrain's
    // textures alive.
    void terrain_gpu_unbind(ID3D11DeviceContext* context);

    void terrain_gpu_release();

    void terrain_register_commands();
}
