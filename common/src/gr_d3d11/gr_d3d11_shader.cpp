#include <xlog/xlog.h>
#include <common/gr_d3d11/gr_d3d11_shader.h>
#include <common/gr_d3d11/gr_d3d11_util.h>
#include <gr_d3d11_engine.h>

namespace gr::d3d11
{
    ShaderManager::ShaderManager(ComPtr<ID3D11Device> device) : device_{device}
    {
    }

    VertexShaderAndLayout
    ShaderManager::load_vertex_shader(const char* filename, VertexLayout vertex_layout)
    {
        std::vector<D3D11_INPUT_ELEMENT_DESC> desc = get_vertex_layout_desc(vertex_layout);
        return load_vertex_shader(filename, desc.data(), desc.size());
    }

    VertexShaderAndLayout
    ShaderManager::load_vertex_shader(const char* filename, const D3D11_INPUT_ELEMENT_DESC input_elements[], std::size_t num_input_elements)
    {
        std::vector<std::byte> shader_data;
        if (!engine::read_file(filename, shader_data)) {
            xlog::error("Cannot open vertex shader file {}", filename);
            return {{}, {}};
        }
        const std::size_t size = shader_data.size();

        xlog::debug("Loading vertex shader {} size {}", filename, size);
        ComPtr<ID3D11VertexShader> vertex_shader;
        DF_GR_D3D11_CHECK_HR(
            device_->CreateVertexShader(shader_data.data(), size, nullptr, &vertex_shader)
        );

        ComPtr<ID3D11InputLayout> input_layout;
        DF_GR_D3D11_CHECK_HR(
            device_->CreateInputLayout(
                input_elements,
                num_input_elements,
                shader_data.data(),
                size,
                &input_layout
            )
        );

        return {std::move(vertex_shader), std::move(input_layout)};
    }

    ComPtr<ID3D11VertexShader> ShaderManager::load_vertex_shader_only(const char* filename)
    {
        std::vector<std::byte> shader_data;
        if (!engine::read_file(filename, shader_data)) {
            xlog::error("Cannot open vertex shader file {}", filename);
            return {};
        }
        const std::size_t size = shader_data.size();

        xlog::debug("Loading vertex shader (no layout) {} size {}", filename, size);
        ComPtr<ID3D11VertexShader> vertex_shader;
        DF_GR_D3D11_CHECK_HR(
            device_->CreateVertexShader(shader_data.data(), size, nullptr, &vertex_shader)
        );
        return vertex_shader;
    }

    ComPtr<ID3D11PixelShader> ShaderManager::load_pixel_shader(const char* filename)
    {
        std::vector<std::byte> shader_data;
        if (!engine::read_file(filename, shader_data)) {
            xlog::error("Cannot open pixel shader file");
            return {};
        }
        const std::size_t size = shader_data.size();

        xlog::debug("Loading pixel shader {} size {}", filename, size);
        ComPtr<ID3D11PixelShader> pixel_shader;
        DF_GR_D3D11_CHECK_HR(
            device_->CreatePixelShader(shader_data.data(), size, nullptr, &pixel_shader)
        );
        return pixel_shader;
    }
}
