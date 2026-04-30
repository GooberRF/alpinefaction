#include <cassert>
#include <patch_common/FunHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/AsmWriter.h>
#include <xlog/xlog.h>
#include <common/utils/string-utils.h>
#include "../graphics/gr.h"
#include "../rf/file/file.h"
#include "../misc/vpackfile.h"
#include "atx.h"
#include "dds.h"

int bm_calculate_pitch(int w, rf::bm::Format format)
{
    switch (format) {
        case rf::bm::FORMAT_8888_ARGB:
            return w * 4;
        case rf::bm::FORMAT_888_RGB:
            return w * 3;
        case rf::bm::FORMAT_565_RGB:
        case rf::bm::FORMAT_1555_ARGB:
        case rf::bm::FORMAT_4444_ARGB:
            return w * 2;
        case rf::bm::FORMAT_8_ALPHA:
        case rf::bm::FORMAT_8_PALETTED:
            return w;
        case rf::bm::FORMAT_DXT1:
            // 4x4 pixels per block, 64 bits per block
            return (w + 3) / 4 * 64 / 8;
        case rf::bm::FORMAT_DXT2:
        case rf::bm::FORMAT_DXT3:
        case rf::bm::FORMAT_DXT4:
        case rf::bm::FORMAT_DXT5:
            // 4x4 pixels per block, 128 bits per block
            return (w + 3) / 4 * 128 / 8;
        default:
            xlog::warn("Unknown format {} in bm_calculate_pitch", static_cast<int>(format));
            return -1;
    }
}

int bm_calculate_rows(int h, rf::bm::Format format)
{
    switch (format) {
        case rf::bm::FORMAT_DXT1:
        case rf::bm::FORMAT_DXT2:
        case rf::bm::FORMAT_DXT3:
        case rf::bm::FORMAT_DXT4:
        case rf::bm::FORMAT_DXT5:
            // 4x4 pixels per block
            return (h + 3) / 4;
        default:
            return h;
    }
}

size_t bm_calculate_total_bytes(int w, int h, rf::bm::Format format)
{
    return bm_calculate_pitch(w, format) * bm_calculate_rows(h, format);
}

bool bm_is_compressed_format(rf::bm::Format format)
{
    switch (format) {
        case rf::bm::FORMAT_DXT1:
        case rf::bm::FORMAT_DXT2:
        case rf::bm::FORMAT_DXT3:
        case rf::bm::FORMAT_DXT4:
        case rf::bm::FORMAT_DXT5:
            return true;
        default:
            return false;
    }
}

FunHook<rf::bm::Type(const char*, int*, int*, rf::bm::Format*, int*, int*, int*, int*, int*, int*, int)>
bm_read_header_hook{
    0x0050FCB0,
    [](const char* filename, int* width_out, int* height_out, rf::bm::Format *pixel_fmt_out, int *num_levels_out,
    int *num_levels_external_mips_out, int *num_frames_out, int *fps_out, int *total_bytes_m2v_out,
    int *vbm_ver_out, int a11) {

        *num_levels_external_mips_out = 1;
        *num_levels_out = 1;
        *fps_out = 0;
        *num_frames_out = 1;
        *total_bytes_m2v_out = -1;
        *vbm_ver_out = 1;

        std::string filename_without_ext{get_filename_without_ext(filename)};

        // ATX: text-based proxy format that wraps existing texture files
        if (!atx_is_loading_child()) {
            auto atx_filename = filename_without_ext + ".atx";
            rf::File atx_file;
            if (atx_file.open(atx_filename.c_str()) == 0) {
                atx_file.close();
                xlog::trace("Loading ATX {} (resolved from {})", atx_filename, filename);
                auto bm_type = read_atx_header(atx_filename.c_str(), width_out, height_out,
                    pixel_fmt_out, num_levels_out, num_frames_out);
                if (bm_type != rf::bm::TYPE_NONE) {
                    return bm_type;
                }
            }
        }

        rf::File dds_file;
        auto dds_filename = filename_without_ext + ".dds";
        if (dds_file.open(dds_filename.c_str()) == 0) {
            xlog::trace("Loading {}", dds_filename);
            auto bm_type = read_dds_header(dds_file, width_out, height_out, pixel_fmt_out, num_levels_out);
            if (bm_type != rf::bm::TYPE_NONE) {
                return bm_type;
            }
        }

        xlog::trace("Loading bitmap header for '{}'", filename);
        auto bm_type = bm_read_header_hook.call_target(filename, width_out, height_out, pixel_fmt_out, num_levels_out,
            num_levels_external_mips_out, num_frames_out, fps_out, total_bytes_m2v_out, vbm_ver_out, a11);
        xlog::trace("Bitmap header for '{}': type {} size {}x{} pixel_fmt {} levels {} frames {}",
            filename, bm_type, *width_out, *height_out, *pixel_fmt_out, *num_levels_out, *num_frames_out);

        // Sanity checks
        // Prevents heap corruption when width = 0 or height = 0
        if (*width_out <= 0 || *height_out <= 0 || *pixel_fmt_out == rf::bm::FORMAT_NONE || *num_levels_out < 1 || *num_frames_out < 1) {
            bm_type = rf::bm::TYPE_NONE;
        }

        if (bm_type == rf::bm::TYPE_NONE && !is_known_missing_stock_asset(filename)) {
            xlog::warn("Failed to load bitmap header for '{}'", filename);
        }

        return bm_type;
    },
};

FunHook<rf::bm::Format(int, void**, void**)> bm_lock_hook{
    0x00510780,
    [](int bmh, void** pixels_out, void** palette_out) {
        auto& bm_entry = rf::bm::bitmaps[rf::bm::get_cache_slot(bmh)];
        if (bm_entry.bm_type == rf::bm::TYPE_DDS) {
            lock_dds_bitmap(bm_entry);
            *pixels_out = bm_entry.locked_data;
            *palette_out = bm_entry.locked_palette;
            return bm_entry.format;
        }
        if (bm_entry.bm_type == rf::bm::TYPE_ATX) {
            return lock_atx_bitmap(bm_entry, pixels_out, palette_out);
        }
        auto pixel_fmt = bm_lock_hook.call_target(bmh, pixels_out, palette_out);
        if (pixel_fmt == rf::bm::FORMAT_NONE) {
            *pixels_out = nullptr;
            *palette_out = nullptr;
            xlog::warn("bm_lock failed");
        }
        return pixel_fmt;
    },
};

FunHook<bool(int)> bm_has_alpha_hook{
    0x00510710,
    [](int bm_handle) {
        auto format = rf::bm::get_format(bm_handle);
        switch (format) {
            case rf::bm::FORMAT_4444_ARGB:
            case rf::bm::FORMAT_1555_ARGB:
            case rf::bm::FORMAT_8888_ARGB:
            case rf::bm::FORMAT_DXT1:
            case rf::bm::FORMAT_DXT2:
            case rf::bm::FORMAT_DXT3:
            case rf::bm::FORMAT_DXT4:
            case rf::bm::FORMAT_DXT5:
                return true;
            default:
                return false;
        }
    },
};

FunHook<uint8_t(int)> bm_get_material_idx_hook{
    0x00511780,
    [](int bmh) -> uint8_t {
        auto& bm_entry = rf::bm::bitmaps[rf::bm::get_cache_slot(bmh)];
        if (bm_entry.bm_type == rf::bm::TYPE_ATX) {
            if (auto over = atx_material_override(bm_entry)) {
                return *over;
            }
        }
        return bm_get_material_idx_hook.call_target(bmh);
    },
};

FunHook<void(int)> bm_unlock_hook{
    0x00511700,
    [](int bmh) {
        auto& bm_entry = rf::bm::bitmaps[rf::bm::get_cache_slot(bmh)];
        if (bm_entry.bm_type == rf::bm::TYPE_ATX) {
            unlock_atx_bitmap(bm_entry);
            return;
        }
        bm_unlock_hook.call_target(bmh);
    },
};

FunHook<void(int)> bm_free_entry_hook{
    0x0050F240,
    [](int bm_index) {
        auto& bm_entry = rf::bm::bitmaps[bm_index];
        if (bm_entry.bm_type == rf::bm::TYPE_ATX) {
            atx_free(bm_entry);
        }
        bm_entry.dynamic = false;
        bm_free_entry_hook.call_target(bm_index);
    },
};

// Fix greyscale TGA files (image types 3 and 11) not loading.
// The pixel-loading dispatch only handles types 1/2 (uncompressed) and 9/10 (RLE);
// the validation accepts types 3/11 but they fall through the dispatch with no pixel
// copy performed. Additionally, 8-bit greyscale is classified as FORMAT_8_PALETTED
// but the loader never generates a palette for it.
CodeInjection tga_greyscale_fix{
    0x0055A95E,
    [](auto& regs) {
        uint8_t image_type = regs.bl;
        if (image_type != 3 && image_type != 11) {
            return;
        }
        if (addr_as_ref<uint8_t>(regs.esp + 0x2e) != 8) {
            return;
        }
        auto* palette = addr_as_ref<uint8_t*>(regs.esp + 0x36c);
        if (palette) {
            for (int i = 0; i < 256; ++i) {
                palette[i * 3] = palette[i * 3 + 1] = palette[i * 3 + 2] =
                    static_cast<uint8_t>(i);
            }
        }
        regs.bl = static_cast<int8_t>((image_type == 3) ? 2 : 10);
    },
};

CodeInjection tga_greyscale_fix_mipmap{
    0x0055AEAE,
    [](auto& regs) {
        uint8_t image_type = regs.bl;
        if (image_type != 3 && image_type != 11) {
            return;
        }
        if (addr_as_ref<uint8_t>(regs.esp + 0x3e) != 8) {
            return;
        }
        auto* palette = addr_as_ref<uint8_t*>(regs.esp + 0x37c);
        if (palette) {
            for (int i = 0; i < 256; ++i) {
                palette[i * 3] = palette[i * 3 + 1] = palette[i * 3 + 2] =
                    static_cast<uint8_t>(i);
            }
        }
        regs.bl = static_cast<int8_t>((image_type == 3) ? 2 : 10);
    },
};

CodeInjection load_tga_alloc_fail_fix{
    0x0051095D,
    [](auto& regs) {
        if (regs.eax == 0) {
            regs.esp += 4;
            unsigned bpp = regs.esi;
            auto num_total_pixels = addr_as_ref<size_t>(regs.ebp + 0x30);
            auto num_bytes = num_total_pixels * bpp;
            xlog::warn("Failed to allocate buffer for a bitmap: {} bytes!", num_bytes);
            regs.eip = 0x00510944;
        }
    },
};

void bm_set_dynamic(int bm_handle, bool dynamic)
{
    int bm_index = rf::bm::get_cache_slot(bm_handle);
    rf::bm::bitmaps[bm_index].dynamic = dynamic;
}

bool bm_is_dynamic(int bm_handle)
{
    int bm_index = rf::bm::get_cache_slot(bm_handle);
    return rf::bm::bitmaps[bm_index].dynamic;
}

void bm_change_format(int bm_handle, rf::bm::Format format)
{
    int bm_idx = rf::bm::get_cache_slot(bm_handle);
    auto& bm = rf::bm::bitmaps[bm_idx];
    assert(bm.bm_type == rf::bm::TYPE_USER);
    if (bm.format != format) {
        rf::gr::mark_texture_dirty(bm_handle);
        bm.format = format;
    }
}

void bm_apply_patch()
{
    bm_read_header_hook.install();
    bm_lock_hook.install();
    bm_unlock_hook.install();
    bm_get_material_idx_hook.install();
    bm_has_alpha_hook.install();
    bm_free_entry_hook.install();

    // Fix greyscale TGA files not loading (types 3 and 11)
    tga_greyscale_fix.install();
    tga_greyscale_fix_mipmap.install();

    // Fix crash when loading very big TGA files
    load_tga_alloc_fail_fix.install();

    // Enable mip-mapping for textures bigger than 256x256 in bm_read_header
    AsmWriter(0x0050FEDA, 0x0050FEE9).nop();
}
