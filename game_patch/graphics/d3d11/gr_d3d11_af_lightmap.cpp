#include <utility>
#include <vector>

#include <d3d11.h>

#include <common/ComPtr.h>
#include <common/lightmap/alpine_lightmap.h>
#include <common/lightmap/alpine_lightmap_reader.h>
#include <xlog/xlog.h>

#include "gr_d3d11_af_lightmap.h"

namespace alm = alpine_lightmap;

namespace
{
    // Pixel shader slots of the atlas (t4), its GPU index (t5) and its sampler (s5)
    constexpr UINT pages_srv_slot = 4;
    constexpr UINT num_srvs = 2;
    constexpr UINT sampler_slot = 5;
}

namespace gr::d3d11
{
    AfLightmapRenderer::AfLightmapRenderer(ComPtr<ID3D11Device> device, ComPtr<ID3D11DeviceContext> context) :
        device_{std::move(device)}, context_{std::move(context)}
    {}

    bool AfLightmapRenderer::create_resources(const alm::ReadResult& section, const std::vector<std::uint8_t>& payload,
                                              const std::vector<std::uint32_t>& index)
    {
        const std::uint32_t p = section.page_size;
        const std::uint32_t pages = section.head.num_pages;
        const bool raw = section.layer.codec == static_cast<std::uint16_t>(alm::Codec::raw_rgb8);
        const DXGI_FORMAT format = raw ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_BC7_UNORM;
        // BC7 is feature level 11 only, and the renderer accepts devices down to 9_1.
        UINT support = 0;
        if (FAILED(device_->CheckFormatSupport(format, &support)) || !(support & D3D11_FORMAT_SUPPORT_TEXTURE2D)) {
            xlog::warn("[AlpineLightmaps] this device cannot sample DXGI format {}, the level "
                       "falls back to its stock lightmaps",
                       static_cast<int>(format));
            return false;
        }
        const UINT row_pitch = raw ? p * 4 : (p / 4) * 16;
        const UINT slice_pitch = row_pitch * (raw ? p : p / 4);

        // D3D11 has no three channel 8 bit format, so the debug codec's pages are widened.
        std::vector<std::uint8_t> rgba;
        if (raw) {
            rgba.resize(static_cast<std::size_t>(pages) * slice_pitch, 0xff);
            const std::size_t texels = static_cast<std::size_t>(pages) * p * p;
            for (std::size_t i = 0; i < texels; i++) {
                rgba[i * 4 + 0] = payload[i * 3 + 0];
                rgba[i * 4 + 1] = payload[i * 3 + 1];
                rgba[i * 4 + 2] = payload[i * 3 + 2];
            }
        }
        const std::uint8_t* src = raw ? rgba.data() : payload.data();

        std::vector<D3D11_SUBRESOURCE_DATA> init(pages);
        for (std::uint32_t i = 0; i < pages; i++) {
            init[i].pSysMem = src + static_cast<std::size_t>(i) * slice_pitch;
            init[i].SysMemPitch = row_pitch;
            init[i].SysMemSlicePitch = slice_pitch;
        }

        D3D11_TEXTURE2D_DESC tex_desc{};
        tex_desc.Width = p;
        tex_desc.Height = p;
        tex_desc.MipLevels = 1;
        tex_desc.ArraySize = pages;
        tex_desc.Format = format;
        tex_desc.SampleDesc.Count = 1;
        tex_desc.Usage = D3D11_USAGE_IMMUTABLE;
        tex_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device_->CreateTexture2D(&tex_desc, init.data(), &pages_tex_))) {
            xlog::warn("[AlpineLightmaps] could not create a {}x{} array of {} pages", p, p, pages);
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
        srv_desc.Format = format;
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        srv_desc.Texture2DArray.MipLevels = 1;
        srv_desc.Texture2DArray.ArraySize = pages;
        if (FAILED(device_->CreateShaderResourceView(pages_tex_, &srv_desc, &pages_srv_))) {
            return false;
        }

        // A typed buffer, not a StructuredBuffer: the latter is shader model 5 and both pixel
        // shaders are ps_4_0.
        D3D11_BUFFER_DESC buf_desc{};
        buf_desc.ByteWidth = static_cast<UINT>(index.size() * sizeof(std::uint32_t));
        buf_desc.Usage = D3D11_USAGE_IMMUTABLE;
        buf_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA buf_data{index.data(), 0, 0};
        if (FAILED(device_->CreateBuffer(&buf_desc, &buf_data, &index_buf_))) {
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC buf_srv{};
        buf_srv.Format = DXGI_FORMAT_R32G32B32A32_UINT;
        buf_srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        buf_srv.Buffer.NumElements = static_cast<UINT>(index.size() / 4);
        if (FAILED(device_->CreateShaderResourceView(index_buf_, &buf_srv, &index_srv_))) {
            return false;
        }

        D3D11_SAMPLER_DESC samp_desc{};
        samp_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samp_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samp_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samp_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samp_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        samp_desc.MaxLOD = D3D11_FLOAT32_MAX;
        return SUCCEEDED(device_->CreateSamplerState(&samp_desc, &sampler_));
    }

    bool AfLightmapRenderer::upload(const alm::ReadResult& section, const std::vector<std::uint8_t>& blocks)
    {
        release();
        const std::vector<std::uint32_t> index = alm::build_gpu_index(section);
        if (!create_resources(section, blocks, index)) {
            release();
            return false;
        }
        live_ = true;
        return true;
    }

    void AfLightmapRenderer::release()
    {
        // bind() leaves the slots bound, which would keep the atlas alive past its ComPtrs.
        if (live_) {
            ID3D11ShaderResourceView* null_srvs[num_srvs] = {};
            context_->PSSetShaderResources(pages_srv_slot, num_srvs, null_srvs);
            ID3D11SamplerState* null_sampler = nullptr;
            context_->PSSetSamplers(sampler_slot, 1, &null_sampler);
        }
        live_ = false;
        pages_srv_.release();
        pages_tex_.release();
        index_srv_.release();
        index_buf_.release();
        sampler_.release();
    }

    void AfLightmapRenderer::bind()
    {
        if (!live_) {
            return;
        }
        ID3D11ShaderResourceView* srvs[num_srvs] = {pages_srv_, index_srv_};
        context_->PSSetShaderResources(pages_srv_slot, num_srvs, srvs);
        ID3D11SamplerState* samp = sampler_;
        context_->PSSetSamplers(sampler_slot, 1, &samp);
    }
}
