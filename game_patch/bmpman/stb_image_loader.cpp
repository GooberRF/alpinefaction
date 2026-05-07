#include <cstdint>
#include <cstring>
#include <vector>

#include <stb_image.h>
#include <xlog/xlog.h>
#include <common/utils/string-utils.h>
#include <common/bitmap/formats.h>

#include "stb_image_loader.h"
#include "../rf/bmpman.h"
#include "../rf/crt.h"
#include "../rf/math/vector.h"
#include "../rf/math/matrix.h"
#include "../rf/file/file.h"
#include "../misc/vpackfile.h"

namespace
{
    // Slurp an entire file via rf::File (handles packfile + disk via the standard lookup).
    bool read_file_to_vector(const char* filename, std::vector<uint8_t>& out)
    {
        rf::File file;
        if (file.open(filename) != 0) {
            return false;
        }
        const int file_size = file.size();
        if (file_size <= 0) {
            file.close();
            return false;
        }
        out.resize(static_cast<size_t>(file_size));
        const int bytes_read = file.read(out.data(), file_size);
        file.close();
        return bytes_read == file_size;
    }

    // Probe for a PNG/JPG/JPEG sibling of `requested_filename` (any extension stripped). For
    // each candidate extension, the supercede policy gate (vpackfile_supercede_allowed) is
    // checked first — same toggle that gates user_maps overriding stock for any other asset
    // type. The first allowed-and-existing sibling wins; if none qualify, returns false.
    bool read_stb_sibling(const char* requested_filename, std::vector<uint8_t>& out)
    {
        std::string base{get_filename_without_ext(requested_filename)};
        for (const char* ext : {".png", ".jpg", ".jpeg"}) {
            auto candidate = base + ext;
            if (!vpackfile_supercede_allowed(requested_filename, candidate.c_str())) {
                continue;
            }
            if (read_file_to_vector(candidate.c_str(), out)) {
                return true;
            }
        }
        return false;
    }

    // Channel-count → bm format mapping lives in common/bitmap/formats.h so the editor uses
    // the same rule (drift-proof). Thin wrapper provides the rf::bm::Format cast at this site.
    rf::bm::Format format_for_channels(int channels)
    {
        return static_cast<rf::bm::Format>(bm_format_from_stb_channels(channels));
    }

    // stb returns RGBA (or RGB) in memory order; RF's 8888_ARGB / 888_RGB store BGRA / BGR.
    // Swap channels 0 and 2 in place.
    void swizzle_rb(uint8_t* pixels, int num_pixels, int bytes_per_pixel)
    {
        for (int i = 0; i < num_pixels; ++i) {
            uint8_t* p = pixels + i * bytes_per_pixel;
            std::swap(p[0], p[2]);
        }
    }
}

rf::bm::Type read_stb_header(const char* filename, int* width_out, int* height_out,
    rf::bm::Format* format_out, int* num_levels_out)
{
    // Probe for any PNG/JPG/JPEG sibling of the requested name. Returning TYPE_NONE here is
    // not an error — it just means no sibling exists, and the caller falls through to stock.
    std::vector<uint8_t> file_bytes;
    if (!read_stb_sibling(filename, file_bytes)) {
        return rf::bm::TYPE_NONE;
    }

    int w = 0, h = 0, channels = 0;
    if (!stbi_info_from_memory(file_bytes.data(), static_cast<int>(file_bytes.size()),
                               &w, &h, &channels)) {
        xlog::warn("stb_image: failed to read header for '{}': {}", filename, stbi_failure_reason());
        return rf::bm::TYPE_NONE;
    }
    if (w <= 0 || h <= 0 || channels < 1 || channels > 4) {
        return rf::bm::TYPE_NONE;
    }

    *width_out = w;
    *height_out = h;
    *format_out = format_for_channels(channels);
    *num_levels_out = 1; // No mip chain in PNG/JPEG; engine handles auto-mip if enabled.
    return rf::bm::TYPE_STB;
}

int lock_stb_bitmap(rf::bm::BitmapEntry& bm_entry)
{
    // Defensively null both fields up front. If any failure path returns -1 below, the bm_lock
    // hook reads these fields directly, so leaving stale pointers from a prior load could be a
    // use-after-free. Stock unlock frees locked_data and presumably nulls it, but explicit null
    // here is cheap insurance.
    bm_entry.locked_data = nullptr;
    bm_entry.locked_palette = nullptr;

    // Probe siblings the same way read_stb_header did. The bm system stored the originally
    // requested name (which might be foo.tga even though we superceded with foo.png), so we
    // can't just reopen by name — we have to re-resolve.
    std::vector<uint8_t> file_bytes;
    if (!read_stb_sibling(bm_entry.name, file_bytes)) {
        xlog::error("stb_image: no PNG/JPG sibling found for '{}' during lock", bm_entry.name);
        return -1;
    }

    // Choose 4-channel output if we declared 8888 in read_header, else 3-channel.
    const int desired_channels = (bm_entry.format == rf::bm::FORMAT_8888_ARGB) ? 4 : 3;

    int w = 0, h = 0, source_channels = 0;
    uint8_t* pixels = stbi_load_from_memory(file_bytes.data(), static_cast<int>(file_bytes.size()),
                                            &w, &h, &source_channels, desired_channels);
    if (!pixels) {
        xlog::error("stb_image: decode failed for '{}': {}", bm_entry.name, stbi_failure_reason());
        return -1;
    }
    if (w != bm_entry.orig_width || h != bm_entry.orig_height) {
        xlog::error("stb_image: '{}' dimensions changed between header read and decode", bm_entry.name);
        stbi_image_free(pixels);
        return -1;
    }

    const int num_pixels = w * h;
    const int bytes_per_pixel = desired_channels;
    const size_t total_bytes = static_cast<size_t>(num_pixels) * bytes_per_pixel;

    // Allocate via rf::rf_malloc so stock unlock can free this buffer with its matching free.
    void* dst = rf::rf_malloc(static_cast<int>(total_bytes));
    if (!dst) {
        xlog::error("stb_image: rf_malloc failed for {} bytes ('{}')", total_bytes, bm_entry.name);
        stbi_image_free(pixels);
        return -1;
    }

    std::memcpy(dst, pixels, total_bytes);
    swizzle_rb(static_cast<uint8_t*>(dst), num_pixels, bytes_per_pixel);

    bm_entry.locked_data = dst;
    bm_entry.locked_palette = nullptr;
    stbi_image_free(pixels);
    return 0;
}
