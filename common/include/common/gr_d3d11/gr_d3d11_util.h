#pragma once

#include <array>
#include <concepts>
#include <source_location>
#include <d3d11.h>
#include <xlog/xlog.h>
#include <gr_d3d11_engine.h>

namespace gr::d3d11
{
    void init_error(ID3D11Device* device);
    void fatal_gr_error(
        HRESULT hr,
        const std::source_location& loc = std::source_location::current()
    );

    template <std::invocable F>
    inline void check_hr(
        const HRESULT hr,
        F&& what,
        const std::source_location& loc = std::source_location::current()
    ) {
        if (FAILED(hr)) {
            what();
            fatal_gr_error(hr, loc);
        }
    }

    #define DF_GR_D3D11_CHECK_HR(code) { \
        const char* const func_name = __func__; \
        ::gr::d3d11::check_hr( \
            code, \
            [=] { \
                ::xlog::error( \
                    "D3D11 call failed: {} (function {} in line {})", \
                    #code, \
                    func_name, \
                    __LINE__ \
                ); \
            } \
        ); \
    }

    // Viewport origin in render-target pixels. Single source for RenderContext::set_clip() and
    // every screen-space reconstruction, which must agree on it.
    static inline std::array<float, 2> viewport_origin()
    {
        return {
            static_cast<float>(rf::gr::screen.clip_left + rf::gr::screen.offset_x),
            static_cast<float>(rf::gr::screen.clip_top + rf::gr::screen.offset_y),
        };
    }

    static inline int pack_color(const rf::Color& color)
    {
        // assume little endian
        return (color.alpha << 24) | (color.blue << 16) | (color.green << 8) | color.red;
    }

    static inline bool mode_uses_vertex_color(rf::gr::Mode mode)
    {
        if (mode.get_texture_source() == rf::gr::TEXTURE_SOURCE_NONE) {
            return true;
        }
        return mode.get_color_source() != rf::gr::COLOR_SOURCE_TEXTURE;
    }

    static inline bool mode_uses_vertex_alpha(rf::gr::Mode mode)
    {
        if (mode.get_texture_source() == rf::gr::TEXTURE_SOURCE_NONE) {
            return true;
        }
        return mode.get_alpha_source() != rf::gr::ALPHA_SOURCE_TEXTURE;
    }

    // White, with src's colour and alpha wherever the mode takes them from the vertex.
    static inline rf::Color vertex_color_for_mode(rf::gr::Mode mode, const rf::Color& src)
    {
        rf::Color color{255, 255, 255, 255};
        if (mode_uses_vertex_color(mode)) {
            color.red = src.red;
            color.green = src.green;
            color.blue = src.blue;
        }
        if (mode_uses_vertex_alpha(mode)) {
            color.alpha = src.alpha;
        }
        return color;
    }

    static inline std::array<int, 2> normalize_texture_handles_for_mode(rf::gr::Mode mode, const std::array<int, 2>& textures)
    {
        switch (mode.get_texture_source()) {
            case rf::gr::TEXTURE_SOURCE_NONE:
                return {-1, -1};
            case rf::gr::TEXTURE_SOURCE_WRAP:
            case rf::gr::TEXTURE_SOURCE_CLAMP:
            case rf::gr::TEXTURE_SOURCE_CLAMP_NO_FILTERING:
                return {textures[0], -1};
            default:
                return textures;
        }
    }
}
