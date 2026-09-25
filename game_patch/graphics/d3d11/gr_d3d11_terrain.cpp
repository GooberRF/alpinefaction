#include <algorithm>
#include <cstddef>
#include <cstring>
#include <optional>
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
#include "../gr.h"
#include "gr_d3d11.h"
#include "gr_d3d11_af_lightmap.h"
#include "gr_d3d11_context.h"
#include "gr_d3d11_terrain.h"

namespace at = alpine_terrain;

namespace
{
    // Mirror of TerrainBuffer (b7) in standard_ps.hlsl
    struct alignas(16) TerrainBufferData
    {
        float origin[3];
        float cell_size;
        float extent[2];
        float height_min;
        float height_range;
        float grid_size[2];
        float layer_count;
        float underside_uv_scale;
        float sun_travel_dir[3];
        float debug;
        float sun_color[3];
        float lm_chart;
        float layer_uv_scale[at::max_layers];
        float layer_triplanar[at::max_layers];
        float lm_origin[2];
        float lm_texel_size;
        float overlay_count;
        float overlay_uv_scale[at::max_overlays];
        float overlay_triplanar[at::max_overlays];
        float overlay_break_tiling[at::max_overlays];
        // 0 for an overlay whose texture did not load, which then draws nothing
        float overlay_enabled[at::max_overlays];
        // 1 where the bound texture is create_premultiplied's copy
        float overlay_premultiplied[at::max_overlays];
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
    static_assert(sizeof(TerrainBufferData) == 240);
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

    enum class GpuState
    {
        unknown,
        ready,
        failed,
    };

    struct TerrainGpu
    {
        GpuState state = GpuState::unknown;
        ComPtr<ID3D11ShaderResourceView> weights[2];
        ComPtr<ID3D11ShaderResourceView> height;
        ComPtr<ID3D11ShaderResourceView> overlay_coverage;
        // create_premultiplied, or null where the overlay samples its bitmap's own texture
        ComPtr<ID3D11ShaderResourceView> overlay_premultiplied[at::max_overlays];
        int layer_bm[at::max_layers] = {-1, -1, -1, -1, -1, -1, -1, -1};
        int underside_bm = -1;
        int overlay_bm[at::max_overlays] = {-1, -1, -1, -1};
    };

    std::vector<TerrainGpu> g_gpu;
    ID3D11Device* g_device = nullptr;
    ComPtr<ID3D11Buffer> g_cbuffer;
    ComPtr<ID3D11SamplerState> g_map_sampler;
    bool g_debug = false;

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

    bool create_shared(ID3D11Device* device)
    {
        if (!g_cbuffer) {
            CD3D11_BUFFER_DESC desc{
                sizeof(TerrainBufferData),
                D3D11_BIND_CONSTANT_BUFFER,
                D3D11_USAGE_DYNAMIC,
                D3D11_CPU_ACCESS_WRITE,
            };
            if (FAILED(device->CreateBuffer(&desc, nullptr, &g_cbuffer))) {
                return false;
            }
        }
        if (!g_map_sampler) {
            CD3D11_SAMPLER_DESC desc{CD3D11_DEFAULT()};
            desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
            desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
            if (FAILED(device->CreateSamplerState(&desc, &g_map_sampler))) {
                return false;
            }
        }
        return true;
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
                            dst[(static_cast<std::size_t>(y) * nw + x) * 4 + c] = static_cast<std::uint8_t>((sum + 2) / 4);
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

    bool create_terrain(ID3D11Device* device, const AlpineTerrain& t, TerrainGpu& gpu)
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
            gpu.weights[m] = create_map(device, DXGI_FORMAT_R8G8B8A8_UNORM, ww, wh, t.weights.data() + m * map_bytes,
                                        ww * 4);
            if (!gpu.weights[m]) return false;
        }
        gpu.height = create_map(device, DXGI_FORMAT_R16_UNORM, h.nx, h.nz, t.heights.data(), h.nx * 2);
        if (!gpu.height) return false;
        if (!t.overlays.empty() && t.overlay_coverage.size() == at::overlay_map_bytes(h.nx, h.nz, h.weight_res_mul)) {
            gpu.overlay_coverage =
                create_map(device, DXGI_FORMAT_R8G8B8A8_UNORM, ww, wh, t.overlay_coverage.data(), ww * 4);
            if (!gpu.overlay_coverage) return false;
            for (std::size_t o = 0; o < t.overlays.size() && o < at::max_overlays; o++) {
                gpu.overlay_bm[o] = load_bitmap(t.overlays[o].texture);
                if (gpu.overlay_bm[o] >= 0) gpu.overlay_premultiplied[o] = create_premultiplied(device, gpu.overlay_bm[o]);
            }
        }

        for (std::size_t l = 0; l < t.layers.size() && l < at::max_layers; l++) {
            gpu.layer_bm[l] = load_bitmap(t.layers[l].texture);
        }
        gpu.underside_bm = load_bitmap(t.underside_texture);
        return true;
    }

    void release_all()
    {
        g_gpu.clear();
        g_cbuffer.release();
        g_map_sampler.release();
        g_device = nullptr;
    }
}

namespace gr::d3d11
{
    bool terrain_gpu_prepare(ID3D11Device* device, int index)
    {
        const auto& terrains = alpine_terrain_get_all();
        if (!device || index < 0 || static_cast<std::size_t>(index) >= terrains.size()) {
            return false;
        }
        if (device != g_device) {
            release_all();
            g_device = device;
        }
        if (g_gpu.size() != terrains.size()) {
            g_gpu.resize(terrains.size());
        }
        TerrainGpu& gpu = g_gpu[index];
        if (gpu.state == GpuState::unknown) {
            const bool ok = create_shared(device) && create_terrain(device, terrains[index], gpu);
            gpu.state = ok ? GpuState::ready : GpuState::failed;
            if (!ok) {
                xlog::warn("[AlpineTerrain] Terrain {} renders as plain geometry: its textures could not be created",
                           terrains[index].uid);
            }
        }
        return gpu.state == GpuState::ready;
    }

    bool terrain_gpu_bind(ID3D11DeviceContext* context, RenderContext& render_context, int index)
    {
        const auto& terrains = alpine_terrain_get_all();
        if (index < 0 || static_cast<std::size_t>(index) >= terrains.size() ||
            static_cast<std::size_t>(index) >= g_gpu.size() || g_gpu[index].state != GpuState::ready) {
            return false;
        }
        const AlpineTerrain& t = terrains[index];
        const at::Header& h = t.header;
        const TerrainGpu& gpu = g_gpu[index];

        TerrainBufferData data{};
        std::memcpy(data.origin, h.origin, sizeof(data.origin));
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
        std::memcpy(data.sun_color, sun.color, sizeof(data.sun_color));
        data.debug = g_debug ? 1.0f : 0.0f;
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
        DF_GR_D3D11_CHECK_HR(context->Map(g_cbuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
        std::memcpy(mapped.pData, &data, sizeof(data));
        context->Unmap(g_cbuffer, 0);

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

        ID3D11SamplerState* samplers[] = {render_context.wrap_sampler_state(), g_map_sampler};
        context->PSSetSamplers(first_sampler_slot, 2, samplers);
        ID3D11Buffer* cbuffer = g_cbuffer;
        context->PSSetConstantBuffers(cbuffer_slot, 1, &cbuffer);
        return true;
    }

    void terrain_gpu_bind_crater(ID3D11DeviceContext* context, RenderContext& render_context, int bm)
    {
        ID3D11ShaderResourceView* srv = render_context.texture_view(bm);
        if (!srv) srv = render_context.texture_view(-1);
        context->PSSetShaderResources(crater_srv_slot, 1, &srv);
    }

    void terrain_gpu_unbind(ID3D11DeviceContext* context)
    {
        ID3D11ShaderResourceView* srvs[num_srvs] = {};
        context->PSSetShaderResources(first_srv_slot, num_srvs, srvs);
    }

    void terrain_gpu_release()
    {
        release_all();
    }

    ConsoleCommand2 r_terrain_debug_cmd{
        "r_terrain_debug",
        [](std::optional<int> value) {
            g_debug = value ? value.value() != 0 : !g_debug;
            rf::console::print("Terrain batch tint is {} (green: surface, red: underside, blue: crater)", g_debug ? "on" : "off");
        },
        "Tints terrain faces by the batch that draws them (Direct3D 11 renderer only)",
        "r_terrain_debug [0|1]",
    };

    void terrain_register_commands()
    {
        r_terrain_debug_cmd.register_cmd();
    }
}
