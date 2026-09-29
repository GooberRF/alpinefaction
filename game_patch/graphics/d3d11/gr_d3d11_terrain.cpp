#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>
#include <d3d11.h>
#include <common/ComPtr.h>
#include <common/scope_guard.h>
#include <xlog/xlog.h>
#include "../../rf/bmpman.h"
#include "../../rf/gr/gr.h"
#include "../../bmpman/bmpman.h"
#include "../../os/console.h"
#include "../../misc/alpine_terrain.h"
#include "../af_lightmap.h"
#include "../gr.h"
#include "gr_d3d11.h"
#include "gr_d3d11_context.h"
#include "gr_d3d11_terrain.h"

namespace at = alpine_terrain;

namespace
{
    // Mirror of TerrainBuffer (b7) in standard_ps.hlsl
    struct alignas(16) TerrainBufferData
    {
        std::array<float, 3> origin;
        float cell_size;
        std::array<float, 2> extent;
        float height_min;
        float height_range;
        std::array<float, 2> grid_size;
        float layer_count;
        float underside_uv_scale;
        std::array<float, 3> sun_travel_dir;
        float debug;
        std::array<float, 3> sun_color;
        float lm_chart;
        std::array<float, at::max_layers> layer_uv_scale;
        std::array<float, at::max_layers> layer_triplanar;
        std::array<float, 2> lm_origin;
        float lm_texel_size;
        float overlay_count;
        std::array<float, at::max_overlays> overlay_uv_scale;
        std::array<float, at::max_overlays> overlay_triplanar;
        std::array<float, at::max_overlays> overlay_break_tiling;
        // 0 for an overlay whose texture did not load, which then draws nothing
        std::array<float, at::max_overlays> overlay_enabled;
        // 1 where the bound texture is create_premultiplied's copy
        std::array<float, at::max_overlays> overlay_premultiplied;
        float fullbright;
        std::array<float, 3> pad;
    };
    static_assert(offsetof(TerrainBufferData, extent) == 16);
    static_assert(offsetof(TerrainBufferData, grid_size) == 32);
    static_assert(offsetof(TerrainBufferData, sun_travel_dir) == 48);
    static_assert(offsetof(TerrainBufferData, debug) == 60);
    static_assert(offsetof(TerrainBufferData, sun_color) == 64);
    static_assert(offsetof(TerrainBufferData, layer_uv_scale) == 80);
    static_assert(offsetof(TerrainBufferData, lm_chart) == 76);
    static_assert(offsetof(TerrainBufferData, layer_triplanar) == 112);
    static_assert(offsetof(TerrainBufferData, lm_origin) == 144);
    static_assert(offsetof(TerrainBufferData, overlay_count) == 156);
    static_assert(offsetof(TerrainBufferData, overlay_uv_scale) == 160);
    static_assert(offsetof(TerrainBufferData, overlay_triplanar) == 176);
    static_assert(offsetof(TerrainBufferData, overlay_break_tiling) == 192);
    static_assert(offsetof(TerrainBufferData, overlay_enabled) == 208);
    static_assert(offsetof(TerrainBufferData, overlay_premultiplied) == 224);
    static_assert(offsetof(TerrainBufferData, fullbright) == 240);
    static_assert(sizeof(TerrainBufferData) == 256);
    static_assert(at::max_overlays == 4, "the shader holds the overlays in float4s");

    constexpr UINT first_srv_slot = 7;
    constexpr UINT first_sampler_slot = 6;
    constexpr UINT cbuffer_slot = 7;
    // weights 0 and 1, height, 8 layers, underside, crater; then the overlays and their coverage map
    constexpr UINT num_base_srvs = 3 + at::max_layers + 2;
    constexpr UINT crater_srv_slot = first_srv_slot + num_base_srvs - 1;
    constexpr UINT overlay_srv_slot = first_srv_slot + num_base_srvs;
    constexpr UINT num_overlay_srvs = at::max_overlays + 1;
    constexpr UINT num_srvs = num_base_srvs + num_overlay_srvs;
    static_assert(overlay_srv_slot == 20 && first_srv_slot + num_srvs - 1 == 24);

    bool g_tint_batches = false;

    ComPtr<ID3D11ShaderResourceView> create_map(ID3D11Device* device, DXGI_FORMAT format, UINT w, UINT h,
                                                const void* data, UINT row_pitch)
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{data, row_pitch, 0};
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11ShaderResourceView> srv;
        if (FAILED(device->CreateTexture2D(&desc, &init, &tex)) ||
            FAILED(device->CreateShaderResourceView(tex, nullptr, &srv))) {
            return {};
        }
        return srv;
    }

    int load_bitmap(const std::string& name)
    {
        return name.empty() ? -1 : rf::bm::load(name.c_str(), -1, true);
    }

    // Largest overlay texture given a premultiplied copy: its mip chain is built in memory first.
    constexpr int max_premultiplied_texels = 2048 * 2048;

    // Animated, ATX and dynamic bitmaps change after load, which a copy made at load would not follow.
    bool bitmap_can_change(int bm)
    {
        const int slot = rf::bm::handle_to_index(bm);
        if (!rf::bm::bitmaps || slot < 0 || slot >= rf::bm::num_cache_slots) return true;
        const rf::bm::BitmapEntry& e = rf::bm::bitmaps[slot];
        return e.bm_type == rf::bm::TYPE_ATX || e.num_frames > 1 || e.dynamic;
    }

    // Top level of bitmap `bm` as B8G8R8A8 with colour premultiplied by alpha. DXT1/3/5 go through the
    // bitmap manager's block decoder; DXT2/4, user and changing bitmaps are refused.
    bool read_premultiplied(int bm, int w, int h, std::vector<std::uint8_t>& out)
    {
        if (rf::bm::get_type(bm) == rf::bm::TYPE_USER || bitmap_can_change(bm)) return false;
        rf::ubyte* bits = nullptr;
        rf::ubyte* pal = nullptr;
        const rf::bm::Format fmt = rf::bm::lock(bm, &bits, &pal);
        if (fmt == rf::bm::FORMAT_NONE || !bits) return false;
        ScopeGuard unlock{[bm] { rf::bm::unlock(bm); }};

        out.resize(static_cast<std::size_t>(w) * h * 4);
        const int pitch = bm_calculate_pitch(w, fmt);
        if (pitch <= 0) return false;
        if (fmt == rf::bm::FORMAT_DXT1 || fmt == rf::bm::FORMAT_DXT3 || fmt == rf::bm::FORMAT_DXT5) {
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    const rf::gr::Color c = bm_get_pixel(bits, fmt, pitch, x, y);
                    std::uint8_t* p = &out[(static_cast<std::size_t>(y) * w + x) * 4];
                    p[0] = c.blue;
                    p[1] = c.green;
                    p[2] = c.red;
                    p[3] = c.alpha;
                }
            }
        }
        else if (bm_is_compressed_format(fmt) ||
                 !bm_convert_format(out.data(), rf::bm::FORMAT_8888_ARGB, bits, fmt, w, h, w * 4, pitch, pal)) {
            return false;
        }
        for (std::size_t i = 0; i < out.size(); i += 4) {
            const unsigned a = out[i + 3];
            for (int c = 0; c < 3; c++) out[i + c] = static_cast<std::uint8_t>((out[i + c] * a + 127) / 255);
        }
        return true;
    }

    // Overlay bitmap `bm` premultiplied per texel, with a 2x2 box-filtered mip chain of the premultiplied
    // texels, so filtering never mixes in the colour of transparent ones. Null when the bitmap cannot be
    // read (see read_premultiplied), is over max_premultiplied_texels, or memory runs out; the overlay then
    // samples the bitmap's own texture and premultiplies after filtering.
    ComPtr<ID3D11ShaderResourceView> create_premultiplied(ID3D11Device* device, int bm)
    {
        int w = 0, h = 0, num_pixels = 0, levels = 0;
        rf::bm::get_mipmap_info(bm, &w, &h, &num_pixels, &levels);
        if (w <= 0 || h <= 0 || static_cast<std::int64_t>(w) * h > max_premultiplied_texels) return {};
        UINT support = 0;
        if (FAILED(device->CheckFormatSupport(DXGI_FORMAT_B8G8R8A8_UNORM, &support)) ||
            !(support & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE) || !(support & D3D11_FORMAT_SUPPORT_MIP)) {
            return {};
        }
        try {
            std::vector<std::vector<std::uint8_t>> mips(1);
            if (!read_premultiplied(bm, w, h, mips[0])) return {};
            std::vector<D3D11_SUBRESOURCE_DATA> init{{mips[0].data(), static_cast<UINT>(w * 4), 0}};
            for (int mw = w, mh = h; mw > 1 || mh > 1;) {
                const int nw = std::max(mw / 2, 1), nh = std::max(mh / 2, 1);
                const std::vector<std::uint8_t>& src = mips.back();
                std::vector<std::uint8_t> dst(static_cast<std::size_t>(nw) * nh * 4);
                for (int y = 0; y < nh; y++) {
                    const int y0 = std::min(2 * y, mh - 1), y1 = std::min(2 * y + 1, mh - 1);
                    for (int x = 0; x < nw; x++) {
                        const int x0 = std::min(2 * x, mw - 1), x1 = std::min(2 * x + 1, mw - 1);
                        for (int c = 0; c < 4; c++) {
                            const unsigned sum = src[(static_cast<std::size_t>(y0) * mw + x0) * 4 + c] +
                                                 src[(static_cast<std::size_t>(y0) * mw + x1) * 4 + c] +
                                                 src[(static_cast<std::size_t>(y1) * mw + x0) * 4 + c] +
                                                 src[(static_cast<std::size_t>(y1) * mw + x1) * 4 + c];
                            dst[(static_cast<std::size_t>(y) * nw + x) * 4 + c] =
                                static_cast<std::uint8_t>((sum + 2) / 4);
                        }
                    }
                }
                mips.push_back(std::move(dst));
                mw = nw;
                mh = nh;
            }
            for (std::size_t m = 1; m < mips.size(); m++) {
                init.push_back({mips[m].data(), static_cast<UINT>(std::max(w >> m, 1) * 4), 0});
            }
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = static_cast<UINT>(w);
            desc.Height = static_cast<UINT>(h);
            desc.MipLevels = static_cast<UINT>(mips.size());
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_IMMUTABLE;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            ComPtr<ID3D11Texture2D> tex;
            ComPtr<ID3D11ShaderResourceView> srv;
            if (FAILED(device->CreateTexture2D(&desc, init.data(), &tex)) ||
                FAILED(device->CreateShaderResourceView(tex, nullptr, &srv))) {
                return {};
            }
            return srv;
        }
        catch (const std::bad_alloc&) {
            return {};
        }
    }
}

namespace gr::d3d11
{
    struct TerrainRenderer::TerrainGpu
    {
        enum class State
        {
            unknown,
            ready,
            failed,
        };

        State state = State::unknown;
        ComPtr<ID3D11ShaderResourceView> weights[2];
        ComPtr<ID3D11ShaderResourceView> height;
        ComPtr<ID3D11ShaderResourceView> overlay_coverage;
        // create_premultiplied, or null where the overlay samples its bitmap's own texture
        ComPtr<ID3D11ShaderResourceView> overlay_premultiplied[at::max_overlays];
        int layer_bm[at::max_layers] = {-1, -1, -1, -1, -1, -1, -1, -1};
        int underside_bm = -1;
        int overlay_bm[at::max_overlays] = {-1, -1, -1, -1};

        bool create(ID3D11Device* device, const AlpineTerrain& t);
    };

    bool TerrainRenderer::TerrainGpu::create(ID3D11Device* device, const AlpineTerrain& t)
    {
        const at::Header& h = t.header;
        // Every texture format the shader samples has to filter, or the blend and normals step.
        for (DXGI_FORMAT format : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16_UNORM}) {
            UINT support = 0;
            if (FAILED(device->CheckFormatSupport(format, &support)) ||
                !(support & D3D11_FORMAT_SUPPORT_TEXTURE2D) || !(support & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE)) {
                return false;
            }
        }
        const UINT ww = at::weight_width(h.nx, h.weight_res_mul);
        const UINT wh = at::weight_height(h.nz, h.weight_res_mul);
        const std::size_t map_bytes = at::weight_map_bytes(h.nx, h.nz, h.weight_res_mul);
        if (t.weights.size() != map_bytes * 2) return false;
        for (int m = 0; m < 2; m++) {
            weights[m] =
                create_map(device, DXGI_FORMAT_R8G8B8A8_UNORM, ww, wh, t.weights.data() + m * map_bytes, ww * 4);
            if (!weights[m]) return false;
        }
        height = create_map(device, DXGI_FORMAT_R16_UNORM, h.nx, h.nz, t.heights.data(), h.nx * 2);
        if (!height) return false;
        if (!t.overlays.empty() && t.overlay_coverage.size() == at::overlay_map_bytes(h.nx, h.nz, h.weight_res_mul)) {
            overlay_coverage =
                create_map(device, DXGI_FORMAT_R8G8B8A8_UNORM, ww, wh, t.overlay_coverage.data(), ww * 4);
            if (!overlay_coverage) return false;
            for (std::size_t o = 0; o < t.overlays.size() && o < at::max_overlays; o++) {
                overlay_bm[o] = load_bitmap(t.overlays[o].texture);
                if (overlay_bm[o] >= 0) {
                    overlay_premultiplied[o] = create_premultiplied(device, overlay_bm[o]);
                }
            }
        }

        for (std::size_t l = 0; l < t.layers.size() && l < at::max_layers; l++) {
            layer_bm[l] = load_bitmap(t.layers[l].texture);
        }
        underside_bm = load_bitmap(t.underside_texture);
        return true;
    }

    TerrainRenderer::TerrainRenderer(ComPtr<ID3D11Device> device) : device_{std::move(device)} {}

    TerrainRenderer::~TerrainRenderer() = default;

    bool TerrainRenderer::create_shared()
    {
        if (!cbuffer_) {
            CD3D11_BUFFER_DESC desc{
                sizeof(TerrainBufferData),
                D3D11_BIND_CONSTANT_BUFFER,
                D3D11_USAGE_DYNAMIC,
                D3D11_CPU_ACCESS_WRITE,
            };
            if (FAILED(device_->CreateBuffer(&desc, nullptr, &cbuffer_))) {
                return false;
            }
        }
        if (!map_sampler_) {
            CD3D11_SAMPLER_DESC desc{CD3D11_DEFAULT()};
            desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
            desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
            if (FAILED(device_->CreateSamplerState(&desc, &map_sampler_))) {
                return false;
            }
        }
        return true;
    }

    bool TerrainRenderer::prepare(int index)
    {
        const auto& terrains = alpine_terrain_get_all();
        if (index < 0 || static_cast<std::size_t>(index) >= terrains.size()) {
            return false;
        }
        if (gpu_.size() != terrains.size()) {
            gpu_.resize(terrains.size());
        }
        TerrainGpu& gpu = gpu_[index];
        if (gpu.state == TerrainGpu::State::unknown) {
            const bool ok = create_shared() && gpu.create(device_, terrains[index]);
            gpu.state = ok ? TerrainGpu::State::ready : TerrainGpu::State::failed;
            if (!ok) {
                xlog::warn("[AlpineTerrain] Terrain {} renders as plain geometry: its textures could not be created",
                           terrains[index].uid);
            }
        }
        return gpu.state == TerrainGpu::State::ready;
    }

    bool TerrainRenderer::bind(ID3D11DeviceContext* context, RenderContext& render_context, int index)
    {
        const auto& terrains = alpine_terrain_get_all();
        if (index < 0 || static_cast<std::size_t>(index) >= terrains.size() ||
            static_cast<std::size_t>(index) >= gpu_.size() || gpu_[index].state != TerrainGpu::State::ready) {
            return false;
        }
        const AlpineTerrain& t = terrains[index];
        const at::Header& h = t.header;
        const TerrainGpu& gpu = gpu_[index];

        TerrainBufferData data{};
        std::memcpy(data.origin.data(), h.origin, sizeof(data.origin));
        data.cell_size = h.cell_size;
        data.extent[0] = at::extent(h.nx, h.cell_size);
        data.extent[1] = at::extent(h.nz, h.cell_size);
        data.height_min = h.height_min;
        data.height_range = h.height_range;
        data.grid_size[0] = static_cast<float>(h.nx);
        data.grid_size[1] = static_cast<float>(h.nz);
        data.layer_count = static_cast<float>(h.layer_count);
        const SunLightState sun = gr_get_sun_state();
        data.sun_travel_dir[0] = sun.travel_dir.x;
        data.sun_travel_dir[1] = sun.travel_dir.y;
        data.sun_travel_dir[2] = sun.travel_dir.z;
        std::memcpy(data.sun_color.data(), sun.color, sizeof(data.sun_color));
        data.debug = g_tint_batches ? 1.0f : 0.0f;
        data.fullbright = t.fullbright() ? 1.0f : 0.0f;
        for (std::size_t l = 0; l < at::max_layers; l++) {
            const bool has = l < t.layers.size();
            data.layer_uv_scale[l] = has ? t.layers[l].uv_scale : at::default_uv_scale;
            data.layer_triplanar[l] = has && t.layers[l].triplanar ? 1.0f : 0.0f;
        }
        data.underside_uv_scale = data.layer_uv_scale[0];
        ID3D11ShaderResourceView* overlay_srvs[num_overlay_srvs] = {};
        if (gpu.overlay_coverage) {
            data.overlay_count = static_cast<float>(std::min<std::size_t>(t.overlays.size(), at::max_overlays));
            overlay_srvs[at::max_overlays] = gpu.overlay_coverage;
        }
        for (std::size_t o = 0; o < at::max_overlays; o++) {
            const bool has = o < t.overlays.size();
            data.overlay_uv_scale[o] = has ? t.overlays[o].uv_scale : at::default_uv_scale;
            data.overlay_triplanar[o] = has && t.overlays[o].triplanar ? 1.0f : 0.0f;
            data.overlay_break_tiling[o] = has && t.overlays[o].break_tiling ? 1.0f : 0.0f;
            data.overlay_premultiplied[o] = gpu.overlay_premultiplied[o] ? 1.0f : 0.0f;
            overlay_srvs[o] = gpu.overlay_premultiplied[o] ? gpu.overlay_premultiplied[o].get()
                              : gpu.overlay_bm[o] >= 0 ? render_context.texture_view(gpu.overlay_bm[o])
                                                       : nullptr;
            data.overlay_enabled[o] = static_cast<float>(o) < data.overlay_count && overlay_srvs[o] ? 1.0f : 0.0f;
        }
        AfTerrainChart chart;
        if (af_lightmap_terrain_chart(index, chart)) {
            data.lm_chart = static_cast<float>(chart.chart);
            data.lm_origin[0] = chart.origin_x;
            data.lm_origin[1] = chart.origin_z;
            data.lm_texel_size = chart.texel_size;
        }
        else {
            data.lm_chart = -1.0f;
            data.lm_texel_size = 1.0f;
        }

        D3D11_MAPPED_SUBRESOURCE mapped;
        DF_GR_D3D11_CHECK_HR(context->Map(cbuffer_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
        std::memcpy(mapped.pData, &data, sizeof(data));
        context->Unmap(cbuffer_, 0);

        // A missing layer shows layer 0, and a missing layer 0 the neutral white
        auto view = [&](int bm) -> ID3D11ShaderResourceView* {
            ID3D11ShaderResourceView* v = bm >= 0 ? render_context.texture_view(bm) : nullptr;
            if (!v && gpu.layer_bm[0] >= 0) v = render_context.texture_view(gpu.layer_bm[0]);
            return v ? v : render_context.texture_view(-1);
        };
        ID3D11ShaderResourceView* srvs[num_base_srvs] = {gpu.weights[0], gpu.weights[1], gpu.height};
        for (std::size_t l = 0; l < at::max_layers; l++) {
            srvs[3 + l] = view(gpu.layer_bm[l]);
        }
        srvs[3 + at::max_layers] = view(gpu.underside_bm);
        context->PSSetShaderResources(first_srv_slot, num_base_srvs - 1, srvs);
        context->PSSetShaderResources(overlay_srv_slot, num_overlay_srvs, overlay_srvs);

        ID3D11SamplerState* samplers[] = {render_context.wrap_sampler_state(), map_sampler_};
        context->PSSetSamplers(first_sampler_slot, 2, samplers);
        ID3D11Buffer* cbuffer = cbuffer_;
        context->PSSetConstantBuffers(cbuffer_slot, 1, &cbuffer);
        return true;
    }

    void TerrainRenderer::bind_crater(ID3D11DeviceContext* context, RenderContext& render_context, int bm)
    {
        ID3D11ShaderResourceView* srv = render_context.texture_view(bm);
        if (!srv) srv = render_context.texture_view(-1);
        context->PSSetShaderResources(crater_srv_slot, 1, &srv);
    }

    void TerrainRenderer::unbind(ID3D11DeviceContext* context)
    {
        ID3D11ShaderResourceView* srvs[num_srvs] = {};
        context->PSSetShaderResources(first_srv_slot, num_srvs, srvs);
    }

    void TerrainRenderer::release()
    {
        gpu_.clear();
        cbuffer_.release();
        map_sampler_.release();
    }

    ConsoleCommand2 dbg_terrain_batches_cmd{
        "dbg_terrain_batches",
        [](std::optional<int> value) {
            g_tint_batches = value ? value.value() != 0 : !g_tint_batches;
            rf::console::print("Terrain batch tint is {} (green: surface, red: underside, blue: crater)",
                               g_tint_batches ? "on" : "off");
        },
        "Tints terrain faces by the batch that draws them (Direct3D 11 renderer only)",
        "dbg_terrain_batches [0|1]",
    };

    void terrain_register_commands()
    {
        dbg_terrain_batches_cmd.register_cmd();
    }
}
