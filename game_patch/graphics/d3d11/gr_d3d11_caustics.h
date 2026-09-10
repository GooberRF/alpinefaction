#pragma once

#include <d3d11.h>
#include <common/ComPtr.h>

namespace gr::d3d11
{
    class CausticsRenderer
    {
    public:
        static constexpr int max_caustic_volumes = 16;
        static constexpr int num_frames = 16;
        static constexpr int frame_size = 256;
        static constexpr int mip_levels = 9;

        explicit CausticsRenderer(ID3D11Device* device);

        void update(ID3D11DeviceContext* device_context);

        operator ID3D11Buffer*() const
        {
            return buffer_;
        }

    private:
        bool build_texture();
        void write_disabled(ID3D11DeviceContext* device_context);

        ComPtr<ID3D11Device> device_;
        ComPtr<ID3D11Buffer> buffer_;
        ComPtr<ID3D11Texture2D> texture_;
        ComPtr<ID3D11ShaderResourceView> srv_;
        ComPtr<ID3D11SamplerState> sampler_;
        bool build_attempted_ = false;
        bool build_failed_ = false;
    };
}
