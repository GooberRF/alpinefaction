#include <cmath>
#include "gr_d3d11.h"
#include "gr_d3d11_state.h"
#include "../../main/main.h"
#include "../../misc/alpine_settings.h"

using namespace rf;

namespace df::gr::d3d11
{
    StateManager::StateManager(ComPtr<ID3D11Device> device) :
        device_{std::move(device)}
    {
    }

    ComPtr<ID3D11RasterizerState> StateManager::create_rasterizer_state(D3D11_CULL_MODE cull_mode, int depth_bias, bool depth_clip_enable)
    {
        CD3D11_RASTERIZER_DESC desc{CD3D11_DEFAULT{}};
        desc.CullMode = cull_mode;
        desc.DepthBias = depth_bias;
        if (g_game_config.msaa) {
            desc.MultisampleEnable = TRUE;
        }
        desc.DepthClipEnable = depth_clip_enable;

        ComPtr<ID3D11RasterizerState> rasterizer_state;
        check_hr(
            device_->CreateRasterizerState(&desc, &rasterizer_state),
            [=]() { xlog::error("Failed to create rasterizer state {} {}", static_cast<int>(cull_mode), depth_bias); }
        );
        return rasterizer_state;
    }

    ComPtr<ID3D11SamplerState> StateManager::create_sampler_state(rf::gr::TextureSource ts)
    {
        CD3D11_SAMPLER_DESC desc{CD3D11_DEFAULT()};
        switch (ts) {
            case gr::TEXTURE_SOURCE_WRAP:
            case gr::TEXTURE_SOURCE_CLAMP_1_WRAP_0:
            case gr::TEXTURE_SOURCE_CLAMP_1_WRAP_0_MOD2X:
            case gr::TEXTURE_SOURCE_MT_WRAP_TRILIN: // WRAP_1_WRAP_0
                desc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
                desc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
                break;
            case gr::TEXTURE_SOURCE_CLAMP:
            case gr::TEXTURE_SOURCE_CLAMP_1_CLAMP_0:
            case gr::TEXTURE_SOURCE_MT_CLAMP_TRILIN: // WRAP_1_CLAMP_0
                desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
                desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
                break;
            case gr::TEXTURE_SOURCE_CLAMP_NO_FILTERING:
                desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
                desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
                desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
                break;
            case gr::TEXTURE_SOURCE_MT_U_WRAP_V_CLAMP:
                desc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
                desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
                break;
            case gr::TEXTURE_SOURCE_MT_U_CLAMP_V_WRAP:
                desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
                desc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
                break;
            default:
                break;
        }

        desc.MipLODBias = 0.0f;
        int divisor = g_alpine_game_config.picmip;
        if (divisor < 1) {
            divisor = 1;
        }
        desc.MinLOD = divisor > 1 ? std::log2(static_cast<float>(divisor)) : 0.0f;
        if (g_alpine_game_config.nearest_texture_filtering) {
            desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        }
        else if (g_game_config.anisotropic_filtering && desc.Filter != D3D11_FILTER_MIN_MAG_MIP_POINT) {
            desc.Filter = D3D11_FILTER_ANISOTROPIC;
            desc.MaxAnisotropy = 16;
        }

        ComPtr<ID3D11SamplerState> sampler_state;
        check_hr(
            device_->CreateSamplerState(&desc, &sampler_state),
            [=]() { xlog::error("Failed to create sampler state {}", static_cast<int>(ts)); }
        );
        return sampler_state;
    }

    ComPtr<ID3D11BlendState> StateManager::create_blend_state(const gr::AlphaBlend ab) {
        CD3D11_BLEND_DESC desc{D3D11_DEFAULT};

        // Do not write alpha.
        // Fixes semi-transparent infrared scanners.
        // Emulates `D3DFMT_X8R8G8B8`.
        desc.RenderTarget[0].RenderTargetWriteMask =
            D3D11_COLOR_WRITE_ENABLE_RED
                | D3D11_COLOR_WRITE_ENABLE_GREEN
                | D3D11_COLOR_WRITE_ENABLE_BLUE;

        switch (ab) {
            case gr::ALPHA_BLEND_NONE: {
                desc.RenderTarget[0].BlendEnable = FALSE;
                break;
            }
            case gr::ALPHA_BLEND_ADDITIVE: {
                desc.RenderTarget[0].BlendEnable = TRUE;
                desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
                desc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
                break;
            }
            case gr::ALPHA_BLEND_ALPHA_ADDITIVE:
            case gr::ALPHA_BLEND_SRC_COLOR: {
                desc.RenderTarget[0].BlendEnable = TRUE;
                desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
                desc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
                break;
            }
            case gr::ALPHA_BLEND_ALPHA: {
                desc.RenderTarget[0].BlendEnable = TRUE;
                desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
                desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
                break;
            }
            case gr::ALPHA_BLEND_LIGHTMAP: {
                desc.RenderTarget[0].BlendEnable = TRUE;
                desc.RenderTarget[0].SrcBlend = D3D11_BLEND_DEST_COLOR;
                desc.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
                break;
            }
            case gr::ALPHA_BLEND_SUBTRACTIVE: {
                desc.RenderTarget[0].BlendEnable = TRUE;
                desc.RenderTarget[0].SrcBlend = D3D11_BLEND_INV_DEST_COLOR;
                desc.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
                break;
            }
            case gr::ALPHA_BLEND_SWAPPED_SRC_DEST_COLOR: {
                desc.RenderTarget[0].BlendEnable = TRUE;
                desc.RenderTarget[0].SrcBlend = D3D11_BLEND_DEST_COLOR;
                desc.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_COLOR;
                break;
            }
        }

        ComPtr<ID3D11BlendState> blend_state{};
        check_hr(
            device_->CreateBlendState(&desc, &blend_state),
            [=] { xlog::error("Failed to create blend state {}", static_cast<int>(ab)); }
        );
        return blend_state;
    }

    ID3D11DepthStencilState* StateManager::get_outline_stencil_mark_state()
    {
        if (!outline_stencil_mark_state_) {
            D3D11_DEPTH_STENCIL_DESC desc{};
            desc.DepthEnable = TRUE;
            desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            desc.DepthFunc = (gr::screen.depthbuffer_type == gr::DEPTHBUFFER_Z)
                ? D3D11_COMPARISON_GREATER_EQUAL : D3D11_COMPARISON_LESS_EQUAL;
            desc.StencilEnable = TRUE;
            desc.StencilReadMask = 0xFF;
            desc.StencilWriteMask = 0xFF;
            desc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
            desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
            desc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
            desc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
            desc.BackFace = desc.FrontFace;
            check_hr(
                device_->CreateDepthStencilState(&desc, &outline_stencil_mark_state_),
                []() { xlog::error("Failed to create outline stencil mark state"); }
            );
        }
        return outline_stencil_mark_state_;
    }

    ID3D11DepthStencilState* StateManager::get_outline_stencil_mark_xray_state()
    {
        if (!outline_stencil_mark_xray_state_) {
            D3D11_DEPTH_STENCIL_DESC desc{};
            desc.DepthEnable = FALSE;
            desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            desc.StencilEnable = TRUE;
            desc.StencilReadMask = 0xFF;
            desc.StencilWriteMask = 0xFF;
            desc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
            desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
            desc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
            desc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
            desc.BackFace = desc.FrontFace;
            check_hr(
                device_->CreateDepthStencilState(&desc, &outline_stencil_mark_xray_state_),
                []() { xlog::error("Failed to create outline stencil mark xray state"); }
            );
        }
        return outline_stencil_mark_xray_state_;
    }

    ID3D11DepthStencilState* StateManager::get_outline_depth_test_state()
    {
        if (!outline_depth_test_state_) {
            D3D11_DEPTH_STENCIL_DESC desc{};
            desc.DepthEnable = TRUE;
            desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            desc.DepthFunc = (gr::screen.depthbuffer_type == gr::DEPTHBUFFER_Z)
                ? D3D11_COMPARISON_GREATER_EQUAL : D3D11_COMPARISON_LESS_EQUAL;
            desc.StencilEnable = TRUE;
            desc.StencilReadMask = 0xFF;
            desc.StencilWriteMask = 0x00;
            desc.FrontFace.StencilFunc = D3D11_COMPARISON_NOT_EQUAL;
            desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
            desc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
            desc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
            desc.BackFace = desc.FrontFace;
            check_hr(
                device_->CreateDepthStencilState(&desc, &outline_depth_test_state_),
                []() { xlog::error("Failed to create outline depth test state"); }
            );
        }
        return outline_depth_test_state_;
    }

    ID3D11DepthStencilState* StateManager::get_outline_xray_state()
    {
        if (!outline_xray_state_) {
            D3D11_DEPTH_STENCIL_DESC desc{};
            desc.DepthEnable = FALSE;
            desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            desc.StencilEnable = TRUE;
            desc.StencilReadMask = 0xFF;
            desc.StencilWriteMask = 0x00;
            desc.FrontFace.StencilFunc = D3D11_COMPARISON_NOT_EQUAL;
            desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
            desc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
            desc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
            desc.BackFace = desc.FrontFace;
            check_hr(
                device_->CreateDepthStencilState(&desc, &outline_xray_state_),
                []() { xlog::error("Failed to create outline xray state"); }
            );
        }
        return outline_xray_state_;
    }

    ID3D11BlendState* StateManager::get_no_color_write_blend_state()
    {
        if (!no_color_write_blend_state_) {
            CD3D11_BLEND_DESC desc{D3D11_DEFAULT};
            desc.RenderTarget[0].RenderTargetWriteMask = 0;
            check_hr(
                device_->CreateBlendState(&desc, &no_color_write_blend_state_),
                []() { xlog::error("Failed to create no-color-write blend state"); }
            );
        }
        return no_color_write_blend_state_;
    }

    ComPtr<ID3D11DepthStencilState> StateManager::create_depth_stencil_state(gr::ZbufferType zbt)
    {
        CD3D11_DEPTH_STENCIL_DESC desc{CD3D11_DEFAULT()};

        if (gr::screen.depthbuffer_type == gr::DEPTHBUFFER_Z) {
            desc.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
        }
        else {
            desc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        }
        switch (zbt) {
        case gr::ZBUFFER_TYPE_NONE:
            desc.DepthEnable = FALSE;
            desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            break;
        case gr::ZBUFFER_TYPE_READ:
            desc.DepthEnable = TRUE;
            desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            break;
        case gr::ZBUFFER_TYPE_READ_EQUAL:
            desc.DepthEnable = TRUE;
            desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            desc.DepthFunc = D3D11_COMPARISON_EQUAL;
            break;
        case gr::ZBUFFER_TYPE_WRITE:
            desc.DepthEnable = TRUE;
            desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
            desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
            break;
        case gr::ZBUFFER_TYPE_FULL:
        case gr::ZBUFFER_TYPE_FULL_ALPHA_TEST:
            desc.DepthEnable = TRUE;
            desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
            break;
        }

        ComPtr<ID3D11DepthStencilState> depth_stencil_state;

        check_hr(
            device_->CreateDepthStencilState(&desc, &depth_stencil_state),
            [=]() { xlog::warn("Failed to create depth stencil state {}", static_cast<int>(zbt)); }
        );
        return depth_stencil_state;
    }
}
