#pragma once

#include <cstdint>
#include <vector>
#include <d3d11.h>
#include <common/ComPtr.h>

namespace alpine_lightmap
{
    struct ReadResult;
}

namespace gr::d3d11
{
    // Batch key slot used instead of the stock lightmap bm handle for faces that carry an alpine
    // chart. Negative so the texture manager resolves it to a null SRV, but not -1 so the alpha
    // render mode a face gets still depends on whether it had a stock lightmap.
    constexpr int af_lightmap_batch_key = -2;

    // The Alpine Lightmaps atlas (t4), its GPU chart index (t5) and their sampler (s5).
    class AfLightmapRenderer
    {
    public:
        AfLightmapRenderer(ComPtr<ID3D11Device> device, ComPtr<ID3D11DeviceContext> context);

        // Replaces the atlas with `section`'s pages, decompressed to `blocks`; false leaves none.
        bool upload(const alpine_lightmap::ReadResult& section, const std::vector<std::uint8_t>& blocks);
        void release();
        void bind();

        // Charts are handed out only while this is true, so a cache rebuilt after release() never
        // addresses an atlas that is not bound.
        bool live() const
        {
            return live_;
        }

    private:
        bool create_resources(const alpine_lightmap::ReadResult& section, const std::vector<std::uint8_t>& payload,
                              const std::vector<std::uint32_t>& index);

        ComPtr<ID3D11Device> device_;
        ComPtr<ID3D11DeviceContext> context_;
        ComPtr<ID3D11Texture2D> pages_tex_;
        ComPtr<ID3D11ShaderResourceView> pages_srv_;
        ComPtr<ID3D11Buffer> index_buf_;
        ComPtr<ID3D11ShaderResourceView> index_srv_;
        ComPtr<ID3D11SamplerState> sampler_;
        bool live_ = false;
    };
}
