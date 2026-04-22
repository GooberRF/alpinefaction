#include <cstring>
#include <cassert>
#include "gr_d3d11.h"
#include "gr_d3d11_texture.h"
#include "../gr.h"
#include "../../bmpman/bmpman.h"
#include "../../main/main.h"
#include "../../misc/alpine_settings.h"
// Access p2t flag directly to avoid pulling in D3D8 types from gr_direct3d.h
#include <patch_common/MemUtils.h>

namespace rf::gr::d3d {
    static auto& p2t = addr_as_ref<int>(0x01CFCC18);
}

namespace gr::d3d11
{
    static int next_power_of_2(int v)
    {
        v--;
        v |= v >> 1;
        v |= v >> 2;
        v |= v >> 4;
        v |= v >> 8;
        v |= v >> 16;
        return v + 1;
    }

    static bool is_block_compressed_format(rf::bm::Format fmt)
    {
        return fmt == rf::bm::FORMAT_DXT1 || fmt == rf::bm::FORMAT_DXT3 || fmt == rf::bm::FORMAT_DXT5;
    }

    TextureManager::TextureManager(ComPtr<ID3D11Device> device, ComPtr<ID3D11DeviceContext> device_context) :
        device_{std::move(device)}, device_context_{std::move(device_context)}
    {
        texture_cache_.reserve(512);
        white_texture_view_ = create_solid_color_texture(1.0f, 1.0f, 1.0f, 1.0f);
        gray_texture_view_ = create_solid_color_texture(0.5f, 0.5f, 0.5f, 1.0f);
        black_texture_view_ = create_solid_color_texture(0.0f, 0.0f, 0.0f, 1.0f);
    }

    TextureManager::Texture TextureManager::create_texture(int bm_handle, rf::bm::Format fmt, int w, int h, rf::ubyte* bits, rf::ubyte* pal, int mip_levels, bool staging, int src_w, int src_h)
    {
        // src_w/src_h are the original bitmap dimensions before pow2 padding
        // When no padding, they equal w and h (or are 0 to indicate no padding)
        bool needs_padding = (src_w > 0 && src_h > 0) && (src_w != w || src_h != h);

        auto [dxgi_format, supported_fmt] = get_supported_texture_format(fmt);
        CD3D11_TEXTURE2D_DESC desc{
            dxgi_format,
            static_cast<UINT>(w),
            static_cast<UINT>(h),
            1, // arraySize
            static_cast<UINT>(mip_levels),
            staging ? 0u : static_cast<UINT>(D3D11_BIND_SHADER_RESOURCE),
            staging ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT,
            staging ? D3D11_CPU_ACCESS_READ|D3D11_CPU_ACCESS_WRITE : 0u,
        };

        std::vector<std::unique_ptr<rf::ubyte[]>> converted_bits_vec;
        std::vector<std::unique_ptr<rf::ubyte[]>> padded_bits_vec;
        std::vector<D3D11_SUBRESOURCE_DATA> subres_data_vec;
        if (bits) {
            // Track both source (bitmap) and destination (texture) dimensions per mip level
            int cur_src_w = needs_padding ? src_w : w;
            int cur_src_h = needs_padding ? src_h : h;
            int cur_dst_w = w;
            int cur_dst_h = h;

            for (int i = 0; i < mip_levels; ++i) {
                int src_pitch = bm_calculate_pitch(cur_src_w, fmt);
                auto src_rows = bm_calculate_rows(cur_src_h, fmt);
                subres_data_vec.emplace_back();
                D3D11_SUBRESOURCE_DATA& subres_data = subres_data_vec.back();

                rf::ubyte* src_bits = bits;
                rf::bm::Format src_fmt = fmt;

                if (supported_fmt != fmt) {
                    xlog::trace("Converting texture {} -> {}", fmt, supported_fmt);
                    int converted_pitch = bm_calculate_pitch(cur_src_w, supported_fmt);
                    converted_bits_vec.push_back(std::make_unique<rf::ubyte[]>(converted_pitch * cur_src_h));
                    rf::ubyte* converted_bits = converted_bits_vec.back().get();
                    ::bm_convert_format(converted_bits, supported_fmt, bits, fmt, cur_src_w, cur_src_h,
                        converted_pitch, src_pitch, pal);
                    src_bits = converted_bits;
                    src_pitch = converted_pitch;
                    src_fmt = supported_fmt;
                }
                else {
                    xlog::trace("Creating texture without conversion: format {}", fmt);
                }

                if (needs_padding) {
                    // Create a zero-filled buffer at the padded dimensions and copy source rows into it
                    int dst_pitch = bm_calculate_pitch(cur_dst_w, src_fmt);
                    int dst_rows = bm_calculate_rows(cur_dst_h, src_fmt);
                    padded_bits_vec.push_back(std::make_unique<rf::ubyte[]>(dst_pitch * dst_rows));
                    rf::ubyte* padded = padded_bits_vec.back().get();
                    std::memset(padded, 0, dst_pitch * dst_rows);

                    int copy_rows = std::min(src_rows, dst_rows);
                    int copy_bytes_per_row = std::min(src_pitch, dst_pitch);
                    for (int row = 0; row < copy_rows; ++row) {
                        std::memcpy(padded + row * dst_pitch, src_bits + row * src_pitch, copy_bytes_per_row);
                    }
                    subres_data.pSysMem = padded;
                    subres_data.SysMemPitch = dst_pitch;
                }
                else {
                    subres_data.pSysMem = src_bits;
                    subres_data.SysMemPitch = src_pitch;
                }

                bits += bm_calculate_pitch(cur_src_w, fmt) * bm_calculate_rows(cur_src_h, fmt);
                cur_src_w = std::max(1, cur_src_w / 2);
                cur_src_h = std::max(1, cur_src_h / 2);
                cur_dst_w = std::max(1, cur_dst_w / 2);
                cur_dst_h = std::max(1, cur_dst_h / 2);
            }
        }
        else {
            xlog::trace("Creating uninitialized texture");
        }

        ComPtr<ID3D11Texture2D> d3d_texture;
        D3D11_SUBRESOURCE_DATA* subres_data_ptr = subres_data_vec.empty() ? nullptr : subres_data_vec.data();
        check_hr(
            device_->CreateTexture2D(&desc, subres_data_ptr, &d3d_texture),
            [&]() { xlog::error("Failed to create texture: format {} dimensions {}x{}, mip levels {}", static_cast<int>(desc.Format), desc.Width, desc.Height, desc.MipLevels); }
        );

        if (staging) {
            return {bm_handle, desc.Format, d3d_texture};
        }
        else {
            Texture texture{bm_handle, desc.Format, {}};
            texture.gpu_texture = d3d_texture;
            return texture;
        }
    }

    TextureManager::Texture TextureManager::create_texture_auto_mips(int bm_handle, rf::bm::Format fmt, int w, int h, rf::ubyte* bits, rf::ubyte* pal)
    {
        // Use the same narrower-format selection as the stock path (e.g. B5G6R5 for
        // 565 sources) so 16bpp content stays 16bpp in VRAM. Then probe whether the
        // chosen DXGI format supports RENDER_TARGET + MIP_AUTOGEN — both are required
        // for GenerateMips. If the narrower format lacks that combination on this
        // driver, fall back to 8888 so we still get the mip chain.
        auto [dxgi_format, supported_fmt] = get_supported_texture_format(fmt);
        UINT format_support = 0;
        device_->CheckFormatSupport(dxgi_format, &format_support);
        constexpr UINT required_support =
            D3D11_FORMAT_SUPPORT_RENDER_TARGET | D3D11_FORMAT_SUPPORT_MIP_AUTOGEN;
        if ((format_support & required_support) != required_support) {
            bool has_alpha = dxgi_format == DXGI_FORMAT_B5G5R5A1_UNORM
                || dxgi_format == DXGI_FORMAT_B4G4R4A4_UNORM
                || dxgi_format == DXGI_FORMAT_B8G8R8A8_UNORM;
            dxgi_format = has_alpha ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_B8G8R8X8_UNORM;
            supported_fmt = rf::bm::FORMAT_8888_ARGB;
        }

        CD3D11_TEXTURE2D_DESC desc{
            dxgi_format,
            static_cast<UINT>(w),
            static_cast<UINT>(h),
            1, // arraySize
            0, // mipLevels = full chain
            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
            D3D11_USAGE_DEFAULT,
            0,
        };
        desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;

        ComPtr<ID3D11Texture2D> d3d_texture;
        HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &d3d_texture);
        if (FAILED(hr)) {
            xlog::warn("Auto-mip texture creation failed ({}x{}), falling back to single mip", w, h);
            return create_texture(bm_handle, fmt, w, h, bits, pal, 1, false, w, h);
        }

        std::unique_ptr<rf::ubyte[]> converted_bits;
        rf::ubyte* upload_bits = bits;
        int upload_pitch = bm_calculate_pitch(w, fmt);
        if (fmt != supported_fmt) {
            int dst_pitch = bm_calculate_pitch(w, supported_fmt);
            converted_bits = std::make_unique<rf::ubyte[]>(dst_pitch * h);
            ::bm_convert_format(converted_bits.get(), supported_fmt, bits, fmt, w, h, dst_pitch, upload_pitch, pal);
            upload_bits = converted_bits.get();
            upload_pitch = dst_pitch;
        }

        device_context_->UpdateSubresource(d3d_texture, 0, nullptr, upload_bits, upload_pitch, 0);

        ComPtr<ID3D11ShaderResourceView> srv;
        hr = device_->CreateShaderResourceView(d3d_texture, nullptr, &srv);
        if (FAILED(hr)) {
            xlog::warn("Auto-mip SRV creation failed ({}x{}), falling back to single mip", w, h);
            return create_texture(bm_handle, fmt, w, h, bits, pal, 1, false, w, h);
        }
        device_context_->GenerateMips(srv);

        Texture texture{bm_handle, dxgi_format, {}};
        texture.gpu_texture = std::move(d3d_texture);
        texture.shader_resource_view = std::move(srv);
        return texture;
    }

    TextureManager::Texture TextureManager::create_render_target(int bm_handle, int w, int h)
    {
        ComPtr<ID3D11Texture2D> gpu_ss_texture{};
        ComPtr<ID3D11Texture2D> gpu_ms_texture{};
        ComPtr<ID3D11RenderTargetView> render_target_view{};

        CD3D11_TEXTURE2D_DESC tex_desc{
            DXGI_FORMAT_R8G8B8A8_UNORM,
            static_cast<UINT>(w),
            static_cast<UINT>(h),
            1, // arraySize
            1, // mipLevels
        };

        const bool use_msaa = g_antialiasing
            && g_game_config.msaa_level >= 2
            && g_game_config.msaa_level <= 8;
        if (use_msaa) {
            DF_GR_D3D11_CHECK_HR(
                device_->CreateTexture2D(&tex_desc, nullptr, &gpu_ss_texture)
            );

            tex_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
            tex_desc.SampleDesc.Count = g_game_config.msaa_level;
            DF_GR_D3D11_CHECK_HR(
                device_->CreateTexture2D(&tex_desc, nullptr, &gpu_ms_texture)
            );

            DF_GR_D3D11_CHECK_HR(
                device_->CreateRenderTargetView(gpu_ms_texture, nullptr, &render_target_view)
            );
        } else {
            tex_desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
            DF_GR_D3D11_CHECK_HR(
                device_->CreateTexture2D(&tex_desc, nullptr, &gpu_ss_texture)
            );

            DF_GR_D3D11_CHECK_HR(
                device_->CreateRenderTargetView(gpu_ss_texture, nullptr, &render_target_view)
            );
        }

        Texture texture{
            bm_handle,
            tex_desc.Format,
            std::move(gpu_ss_texture),
            std::move(render_target_view)
        };
        texture.gpu_ms_texture = std::move(gpu_ms_texture);

        return texture;
    }

    TextureManager::Texture TextureManager::load_texture(int bm_handle, bool staging)
    {
        xlog::trace("Creating texture for bitmap {} handle {} format {}", rf::bm::get_filename(bm_handle), bm_handle, std::to_underlying(rf::bm::get_format(bm_handle)));

        int w, h, num_pixels, mip_levels;
        rf::bm::get_mipmap_info(bm_handle, &w, &h, &num_pixels, &mip_levels);
        if (w <= 0 || h <= 0) {
            xlog::warn("Bad bitmap dimensions: handle {} filename {} dimensions {}x{}", bm_handle, rf::bm::get_filename(bm_handle), w, h);
            return {};
        }

        // D3D11 expects number of mip levels to depend on the shorter dimension and RF uses longer dimension so
        // in case of non-squere textures there is a conflict
        int max_mip_levels = 1 + static_cast<int>(std::floor(std::log2(std::min(w, h))));
        if (mip_levels > max_mip_levels) {
            xlog::trace("Bad number of mip levels for {}x{} texture: expected {} but got {}", w, h, max_mip_levels, mip_levels);
            mip_levels = max_mip_levels;
        }

        rf::bm::Format fmt = rf::bm::get_format(bm_handle);
        if (fmt == rf::bm::FORMAT_RENDER_TARGET) {
            xlog::info("Creating render target: handle {}", bm_handle);
            return create_render_target(bm_handle, w, h);
        }

        if (rf::bm::get_type(bm_handle) == rf::bm::TYPE_USER) {
            xlog::trace("Creating user bitmap texture: handle {}", bm_handle);
            auto texture = create_texture(bm_handle, fmt, w, h, nullptr, nullptr, 1, staging);
            return texture;
        }

        rf::ubyte* bm_bits = nullptr;
        rf::ubyte* bm_pal = nullptr;
        xlog::trace("Locking bitmap");
        fmt = rf::bm::lock(bm_handle, &bm_bits, &bm_pal);
        if (fmt == rf::bm::FORMAT_NONE || bm_bits == nullptr) {
            xlog::warn("Bitmap lock failed or unsupported bitmap");
            return {};
        }

        // Pow2 texture padding when p2t is active
        int tex_w = w;
        int tex_h = h;
        float u_scale = 1.0f;
        float v_scale = 1.0f;

        if (rf::gr::d3d::p2t != 0 && !is_block_compressed_format(fmt)) {
            int pow2_w = next_power_of_2(w);
            int pow2_h = next_power_of_2(h);
            if (pow2_w != w || pow2_h != h) {
                u_scale = static_cast<float>(w) / static_cast<float>(pow2_w);
                v_scale = static_cast<float>(h) / static_cast<float>(pow2_h);
                tex_w = pow2_w;
                tex_h = pow2_h;
                xlog::trace("Pow2 padding texture {} {}x{} -> {}x{} (u_scale={}, v_scale={})",
                    rf::bm::get_filename(bm_handle), w, h, pow2_w, pow2_h, u_scale, v_scale);
            }
        }

        // When r_picmip > 1, auto-generate a full mip chain for every eligible texture so
        // the geometry sampler's MinLOD clamp has lower mips to sample from. Sprites/UI are
        // unaffected because they're drawn with a separate sampler that forces mip 0.
        // Skip: pow2-padded (zero-bleed), BC-compressed (can't be render target), tiny dims,
        // staging use, and textures that already have a mip chain.
        bool auto_mips = g_alpine_game_config.picmip > 1
            && !staging
            && mip_levels == 1
            && u_scale == 1.0f && v_scale == 1.0f
            && !is_block_compressed_format(fmt)
            && std::min(tex_w, tex_h) >= 4;

        xlog::trace("Creating normal texture: handle {} auto_mips {}", bm_handle, auto_mips);
        auto texture = auto_mips
            ? create_texture_auto_mips(bm_handle, fmt, tex_w, tex_h, bm_bits, bm_pal)
            : create_texture(bm_handle, fmt, tex_w, tex_h, bm_bits, bm_pal, mip_levels, staging, w, h);
        texture.u_scale = u_scale;
        texture.v_scale = v_scale;

        xlog::trace("Unlocking bitmap");
        rf::bm::unlock(bm_handle);

        xlog::trace("Texture created");
        return texture;
    }

    void TextureManager::finish_render_target(int bm_handle)
    {
        if (bm_handle < 0) {
            return;
        }
        Texture& texture = get_or_load_texture(bm_handle, false);
        if (texture.gpu_ms_texture && texture.gpu_texture) {
            device_context_->ResolveSubresource(texture.gpu_texture, 0, texture.gpu_ms_texture, 0, texture.format);
        }
    }

    void TextureManager::save_cache()
    {
        for (auto& e : texture_cache_) {
            Texture& texture = e.second;
            ++texture.save_cache_count;
        }
    }

    void TextureManager::flush_cache(bool force)
    {
        xlog::trace("Flushing texture cache");
        if (force) {
            texture_cache_.clear();
        }
        else {
            auto it = texture_cache_.begin();
            while (it != texture_cache_.end()) {
                Texture& texture = it->second;
                if (texture.save_cache_count > 0) {
                    --texture.save_cache_count;
                }
                else if (texture.ref_count <= 0) {
                    xlog::trace("Flushing texture: handle {}", texture.bm_handle);
                    it = texture_cache_.erase(it);
                    continue;
                }
                ++it;
            }
        }
    }

    void TextureManager::flush_non_user_cache()
    {
        // Used by the r_picmip transition flush to force non-USER textures to re-upload
        // with (or without) auto-generated mip chains. Two classes of entries are kept:
        //   1. TYPE_USER bitmaps (font glyph atlases, HUD overlays, other dynamically-
        //      written textures) — their pixel data is written INTO the D3D11 texture
        //      by external code via gr::lock; destroying the texture would leave the
        //      next lookup with an empty SRV and the writer has no way to know it must
        //      re-upload.
        //   2. Entries with ref_count > 0 — the engine holds a logical ref via
        //      gr_d3d_texture_add_ref. Erasing would desync refcount bookkeeping: a
        //      later remove_ref against a freshly re-loaded entry (ref_count=0) would
        //      underflow. Those textures keep their current mip state until released
        //      and re-added, at which point the new picmip setting takes effect.
        auto it = texture_cache_.begin();
        while (it != texture_cache_.end()) {
            const Texture& texture = it->second;
            bool is_user = texture.bm_handle >= 0
                && rf::bm::get_type(texture.bm_handle) == rf::bm::TYPE_USER;
            if (is_user || texture.ref_count > 0) {
                ++it;
            }
            else {
                it = texture_cache_.erase(it);
            }
        }
    }

    void TextureManager::add_ref(int bm_handle)
    {
        page_in(bm_handle);
        int bm_index = rf::bm::get_cache_slot(bm_handle);
        auto it = texture_cache_.find(bm_index);
        if (it != texture_cache_.end()) {
            Texture& texture = it->second;
            ++texture.ref_count;
        }
    }

    void TextureManager::remove_ref(int bm_handle)
    {
        int bm_index = rf::bm::get_cache_slot(bm_handle);
        auto it = texture_cache_.find(bm_index);
        if (it != texture_cache_.end()) {
            Texture& texture = it->second;
            assert(texture.ref_count > 0);
            --texture.ref_count;
            if (texture.ref_count <= 0) {
                xlog::trace("Flushing texture after ref removal: handle {}", texture.bm_handle);
                texture_cache_.erase(it);
            }
        }
    }

    void TextureManager::mark_dirty(int bm_handle)
    {
        int bm_index = rf::bm::get_cache_slot(bm_handle);
        texture_cache_.erase(bm_index);
    }

    std::pair<DXGI_FORMAT, rf::bm::Format> TextureManager::determine_supported_texture_format(rf::bm::Format fmt)
    {
        std::vector<std::pair<DXGI_FORMAT, rf::bm::Format>> candidates;
        switch (fmt) {
            case rf::bm::FORMAT_8_PALETTED: return {DXGI_FORMAT_B8G8R8X8_UNORM, rf::bm::FORMAT_8888_ARGB};
            case rf::bm::FORMAT_565_RGB:
                candidates.emplace_back(DXGI_FORMAT_B5G6R5_UNORM, rf::bm::FORMAT_565_RGB);
                candidates.emplace_back(DXGI_FORMAT_B8G8R8X8_UNORM, rf::bm::FORMAT_8888_ARGB);
                break;
            case rf::bm::FORMAT_1555_ARGB:
                candidates.emplace_back(DXGI_FORMAT_B5G5R5A1_UNORM, rf::bm::FORMAT_1555_ARGB);
                candidates.emplace_back(DXGI_FORMAT_B8G8R8A8_UNORM, rf::bm::FORMAT_8888_ARGB);
                break;
            case rf::bm::FORMAT_4444_ARGB:
                candidates.emplace_back(DXGI_FORMAT_B4G4R4A4_UNORM, rf::bm::FORMAT_4444_ARGB);
                candidates.emplace_back(DXGI_FORMAT_B8G8R8A8_UNORM, rf::bm::FORMAT_8888_ARGB);
                break;
            // Formats below should always be supported
            case rf::bm::FORMAT_888_RGB: return {DXGI_FORMAT_B8G8R8X8_UNORM, rf::bm::FORMAT_8888_ARGB};
            case rf::bm::FORMAT_8888_ARGB: return {DXGI_FORMAT_B8G8R8A8_UNORM, fmt};
            case rf::bm::FORMAT_DXT1: return {DXGI_FORMAT_BC1_UNORM, fmt};
            case rf::bm::FORMAT_DXT3: return {DXGI_FORMAT_BC2_UNORM, fmt};
            case rf::bm::FORMAT_DXT5: return {DXGI_FORMAT_BC3_UNORM, fmt};
            // Other formats
            default: return {DXGI_FORMAT_B8G8R8A8_UNORM, rf::bm::FORMAT_8888_ARGB};
        }

        unsigned format_support = 0;
        for (auto& p: candidates) {
            DF_GR_D3D11_CHECK_HR(
                device_->CheckFormatSupport(p.first, &format_support)
            );
            if (format_support & D3D11_FORMAT_SUPPORT_TEXTURE2D) {
                return p;
            }
        }
        // We shouldn't get here (last candidate should always be supported)
        assert(false);
        return {DXGI_FORMAT_B8G8R8A8_UNORM, rf::bm::FORMAT_8888_ARGB};
    }

    std::pair<DXGI_FORMAT, rf::bm::Format> TextureManager::get_supported_texture_format(rf::bm::Format fmt)
    {
        auto it = supported_texture_format_cache_.find(fmt);
        if (it != supported_texture_format_cache_.end()) {
            return it->second;
        }
        std::pair<DXGI_FORMAT, rf::bm::Format> p = determine_supported_texture_format(fmt);
        supported_texture_format_cache_.insert({fmt, p});
        return p;
    }

    rf::bm::Format TextureManager::get_bm_format(DXGI_FORMAT dxgi_fmt)
    {
        switch (dxgi_fmt) {
            case DXGI_FORMAT_B5G6R5_UNORM: return rf::bm::FORMAT_565_RGB;
            case DXGI_FORMAT_B5G5R5A1_UNORM: return rf::bm::FORMAT_1555_ARGB;
            case DXGI_FORMAT_B4G4R4A4_UNORM: return rf::bm::FORMAT_4444_ARGB;
            case DXGI_FORMAT_B8G8R8A8_UNORM: return rf::bm::FORMAT_8888_ARGB;
            case DXGI_FORMAT_B8G8R8X8_UNORM: return rf::bm::FORMAT_8888_ARGB;
            case DXGI_FORMAT_BC1_UNORM: return rf::bm::FORMAT_DXT1;
            case DXGI_FORMAT_BC2_UNORM: return rf::bm::FORMAT_DXT3;
            case DXGI_FORMAT_BC3_UNORM: return rf::bm::FORMAT_DXT5;
            default: return rf::bm::FORMAT_NONE;
        }
    }

    static D3D11_MAP convert_lock_mode_to_map_type(rf::gr::LockMode mode)
    {
        switch (mode) {
            case rf::gr::LOCK_READ_ONLY:
                return D3D11_MAP_READ;
            case rf::gr::LOCK_READ_ONLY_WRITE:
                return D3D11_MAP_READ_WRITE;
            case rf::gr::LOCK_WRITE_ONLY:
                return D3D11_MAP_WRITE;
            default:
                xlog::warn("Invalid lock mode: {}", static_cast<int>(mode));
                assert(false);
                return D3D11_MAP_READ_WRITE;
        }
    }

    bool TextureManager::lock(int bm_handle, int section, rf::gr::LockInfo *lock)
    {
        if (section != 0) {
            return false;
        }

        int w, h;
        rf::bm::get_dimensions(bm_handle, &w, &h);
        if (w == 0 || h == 0) {
            return false;
        }

        Texture& texture = get_or_load_texture(bm_handle, true);
        ID3D11Texture2D* staging_texture = texture.get_or_create_staging_texture(device_, device_context_,
            lock->mode != rf::gr::LOCK_WRITE_ONLY);
        if (!staging_texture) {
            xlog::warn("Attempted to lock texture without staging resource {}", texture.bm_handle);
            return false;
        }

        D3D11_TEXTURE2D_DESC desc;
        staging_texture->GetDesc(&desc);

        D3D11_MAPPED_SUBRESOURCE mapped_texture;
        D3D11_MAP map_type = convert_lock_mode_to_map_type(lock->mode);
        DF_GR_D3D11_CHECK_HR(
            device_context_->Map(staging_texture, 0, map_type, 0, &mapped_texture)
        );

        lock->bm_handle = bm_handle;
        lock->section = section;
        lock->format = get_bm_format(desc.Format);
        lock->data = reinterpret_cast<rf::ubyte*>(mapped_texture.pData);
        lock->w = w;
        lock->h = h;
        lock->stride_in_bytes = mapped_texture.RowPitch;
        // Note: lock->mode is set by gr_lock
        xlog::trace("locked texture: handle {} format {} size {}x{} data {}", lock->bm_handle, lock->format, lock->w, lock->h, lock->data);
        return true;
    }

    void TextureManager::unlock(rf::gr::LockInfo *lock)
    {
        xlog::trace("unlocking texture: handle {} format {} size {}x{} data {}", lock->bm_handle, lock->format, lock->w, lock->h, lock->data);
        Texture& texture = get_or_load_texture(lock->bm_handle, true);
        if (texture.cpu_texture) {
            device_context_->Unmap(texture.cpu_texture, 0);
            if (lock->mode != rf::gr::LOCK_READ_ONLY && texture.gpu_texture) {
                device_context_->CopySubresourceRegion(texture.gpu_texture, 0, 0, 0, 0, texture.cpu_texture, 0, nullptr);
            }
        }
    }

    void TextureManager::get_texel(
        [[maybe_unused]] int bm_handle,
        [[maybe_unused]] float u,
        [[maybe_unused]] float v,
        rf::gr::Color *clr)
    {
        // This function is only used when shooting at a texture with alpha
        // Note: original function has out of bounds error - it reads color for 16bpp and 32bpp (two addresses)
        // before actually checking the format
        rf::gr::LockInfo lock_info;
        lock_info.mode = rf::gr::LOCK_READ_ONLY;
        if (lock(bm_handle, 0, &lock_info)) {
            // Make sure u and v are in [0, 1] range
            // Assume wrap addressing mode
            u = std::fmod(u, 1.0f);
            v = std::fmod(v, 1.0f);
            if (u < 0.0f) {
                u += 1.0f;
            }
            if (v < 0.0f) {
                v += 1.0f;
            }
            int x = std::lround(u * lock_info.w);
            int y = std::lround(v * lock_info.h);
            x = std::clamp(x, 0, lock_info.w - 1);
            y = std::clamp(y, 0, lock_info.h - 1);
            *clr = bm_get_pixel(lock_info.data, lock_info.format, lock_info.stride_in_bytes, x, y);

            unlock(&lock_info);
        }
        else {
            clr->set(255, 255, 255, 255);
        }
    }

    ComPtr<ID3D11ShaderResourceView> TextureManager::create_solid_color_texture(float r, float g, float b, float a)
    {
        CD3D11_TEXTURE2D_DESC desc{
            DXGI_FORMAT_R32G32B32A32_FLOAT,
            1, // width
            1, // height
        };
        float data[] = {r, g, b, a};
        D3D11_SUBRESOURCE_DATA subres_data{&data, sizeof(data), 0};
        ComPtr<ID3D11Texture2D> gpu_texture;
        DF_GR_D3D11_CHECK_HR(
            device_->CreateTexture2D(&desc, &subres_data, &gpu_texture)
        );
        ComPtr<ID3D11ShaderResourceView> shader_resource_view;
        DF_GR_D3D11_CHECK_HR(
            device_->CreateShaderResourceView(gpu_texture, nullptr, &shader_resource_view)
        );
        return shader_resource_view;
    }

    void TextureManager::Texture::init_shader_resource_view(ID3D11Device* device, ID3D11DeviceContext* device_context)
    {
        if (!gpu_texture) {
            init_gpu_texture(device, device_context);
        }

        if (gpu_texture) {
            DF_GR_D3D11_CHECK_HR(
                device->CreateShaderResourceView(gpu_texture, nullptr, &shader_resource_view)
            );
        }
    }

    void TextureManager::Texture::init_gpu_texture(ID3D11Device* device, ID3D11DeviceContext* device_context)
    {
        xlog::trace("Creating GPU texture for {}", rf::bm::get_filename(bm_handle));

        if (!cpu_texture) {
            xlog::warn("Both GPU and CPU textures are missing");
            return;
        }

        D3D11_TEXTURE2D_DESC cpu_desc;
        cpu_texture->GetDesc(&cpu_desc);

        CD3D11_TEXTURE2D_DESC desc{
            cpu_desc.Format,
            cpu_desc.Width,
            cpu_desc.Height,
            cpu_desc.ArraySize,
            cpu_desc.MipLevels,
        };
        DF_GR_D3D11_CHECK_HR(
            device->CreateTexture2D(&desc, nullptr, &gpu_texture)
        );

        // Copy only first level
        // TODO: mapmaps?
        device_context->CopyResource(gpu_texture, cpu_texture);
        xlog::trace("Created GPU texture");
    }

    void TextureManager::Texture::init_cpu_texture(ID3D11Device* device, ID3D11DeviceContext* device_context, bool copy_from_gpu)
    {
        if (!gpu_texture) {
            xlog::warn("Both GPU and CPU textures are missing");
            return;
        }

        D3D11_TEXTURE2D_DESC gpu_desc;
        gpu_texture->GetDesc(&gpu_desc);

        CD3D11_TEXTURE2D_DESC desc{
            gpu_desc.Format,
            gpu_desc.Width,
            gpu_desc.Height,
            gpu_desc.ArraySize,
            gpu_desc.MipLevels,
            0u,
            D3D11_USAGE_STAGING,
            D3D11_CPU_ACCESS_READ|D3D11_CPU_ACCESS_WRITE,
        };
        DF_GR_D3D11_CHECK_HR(
            device->CreateTexture2D(&desc, nullptr, &cpu_texture)
        );

        if (copy_from_gpu) {
            device_context->CopyResource(cpu_texture, gpu_texture);
        }
    }

    rf::bm::Format TextureManager::read_back_buffer(ID3D11Texture2D* back_buffer, int x, int y, int w, int h, rf::ubyte* data)
    {
        D3D11_TEXTURE2D_DESC gpu_desc;
        back_buffer->GetDesc(&gpu_desc);
        rf::bm::Format bm_format = get_bm_format(gpu_desc.Format);
        if (data) {
            // TODO: use ResolveSubresource if multi-sampling is enabled
            if (!back_buffer_staging_texture_) {
                CD3D11_TEXTURE2D_DESC cpu_desc{
                    gpu_desc.Format,
                    gpu_desc.Width,
                    gpu_desc.Height,
                    1, // arraySize
                    1, // mipLevels
                    0, // bindFlags
                    D3D11_USAGE_STAGING,
                    D3D11_CPU_ACCESS_READ,
                };
                DF_GR_D3D11_CHECK_HR(
                    device_->CreateTexture2D(&cpu_desc, nullptr, &back_buffer_staging_texture_)
                );
            }
            device_context_->CopySubresourceRegion(back_buffer_staging_texture_, 0, 0, 0, 0, back_buffer, 0, nullptr);
            D3D11_MAPPED_SUBRESOURCE mapped_res;
            DF_GR_D3D11_CHECK_HR(
                device_context_->Map(back_buffer_staging_texture_, 0, D3D11_MAP_READ, 0, &mapped_res)
            );
            rf::ubyte* src_ptr = reinterpret_cast<rf::ubyte*>(mapped_res.pData);
            rf::ubyte* dst_ptr = data;

            src_ptr += y * mapped_res.RowPitch;
            int bytes_per_pixel = bm_bytes_per_pixel(bm_format);
            int rows_left = h;
            while (rows_left > 0) {
                int bytes_to_copy = w * bytes_per_pixel;
                std::memcpy(dst_ptr, src_ptr + x * bytes_per_pixel, bytes_to_copy);
                dst_ptr += bytes_to_copy;
                src_ptr += mapped_res.RowPitch;
                --rows_left;
            }
            device_context_->Unmap(back_buffer_staging_texture_, 0);
        }
        return bm_format;
    }
}
