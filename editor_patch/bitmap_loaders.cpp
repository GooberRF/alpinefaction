#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <dds.h>
#include <stb_image.h>
#include <toml++/toml.hpp>
#include <xlog/xlog.h>
#include <patch_common/FunHook.h>
#include <common/utils/string-utils.h>
#include <common/bitmap/formats.h>
#include <common/atx/parse.h>
#include "bitmap_loaders.h"
#include "vtypes.h"
#include "textures.h"
#include "level.h"

namespace
{
    enum class DdsKind { Unsupported, RGBA32, RGB24, DXT1, DXT3, DXT5 };

    // Editor's renderer can't consume DXT directly, so DXT-format DDS files decompress for editor view
    DdsKind classify_dds(const DDS_PIXELFORMAT& p)
    {
        switch (bm_format_from_dds(p)) {
            case BM_FORMAT_8888_ARGB: return DdsKind::RGBA32;
            case BM_FORMAT_888_RGB:   return DdsKind::RGB24;
            case BM_FORMAT_DXT1:      return DdsKind::DXT1;
            case BM_FORMAT_DXT3:      return DdsKind::DXT3;
            case BM_FORMAT_DXT5:      return DdsKind::DXT5;
            default:                   return DdsKind::Unsupported;
        }
    }

    // DXT block decoders output 4×4 RGBA pixels in BGRA byte order

    inline void unpack_565(uint16_t v, uint8_t* rgba)
    {
        uint8_t r = static_cast<uint8_t>((v >> 11) & 0x1F);
        uint8_t g = static_cast<uint8_t>((v >> 5)  & 0x3F);
        uint8_t b = static_cast<uint8_t>( v        & 0x1F);
        rgba[0] = static_cast<uint8_t>((r << 3) | (r >> 2)); // R
        rgba[1] = static_cast<uint8_t>((g << 2) | (g >> 4)); // G
        rgba[2] = static_cast<uint8_t>((b << 3) | (b >> 2)); // B
        rgba[3] = 255;                                       // A
    }

    inline uint16_t read_u16le(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
    inline uint32_t read_u32le(const uint8_t* p)
    {
        return static_cast<uint32_t>(p[0])
             | (static_cast<uint32_t>(p[1]) << 8)
             | (static_cast<uint32_t>(p[2]) << 16)
             | (static_cast<uint32_t>(p[3]) << 24);
    }

    void decode_dxt_color(const uint8_t* block, uint8_t out_palette[4][4], bool dxt1)
    {
        uint16_t c0 = read_u16le(block);
        uint16_t c1 = read_u16le(block + 2);
        unpack_565(c0, out_palette[0]);
        unpack_565(c1, out_palette[1]);

        if (!dxt1 || c0 > c1) {
            for (int i = 0; i < 3; ++i) {
                out_palette[2][i] = static_cast<uint8_t>((2 * out_palette[0][i] + out_palette[1][i]) / 3);
                out_palette[3][i] = static_cast<uint8_t>((out_palette[0][i] + 2 * out_palette[1][i]) / 3);
            }
            out_palette[2][3] = 255;
            out_palette[3][3] = 255;
        }
        else {
            for (int i = 0; i < 3; ++i) {
                out_palette[2][i] = static_cast<uint8_t>((out_palette[0][i] + out_palette[1][i]) / 2);
                out_palette[3][i] = 0;
            }
            out_palette[2][3] = 255;
            out_palette[3][3] = 0; // 1-bit alpha: index 3 = transparent black for DXT1
        }
    }

    void emit_dxt_color_block(const uint8_t* block, uint8_t* out, int stride, int max_w, int max_h, bool dxt1)
    {
        uint8_t pal[4][4];
        decode_dxt_color(block, pal, dxt1);
        uint32_t lookup = read_u32le(block + 4);
        for (int y = 0; y < 4 && y < max_h; ++y) {
            uint8_t* row = out + y * stride;
            for (int x = 0; x < 4 && x < max_w; ++x) {
                int idx = (lookup >> ((y * 4 + x) * 2)) & 0x3;
                row[x * 4 + 0] = pal[idx][2]; // B
                row[x * 4 + 1] = pal[idx][1]; // G
                row[x * 4 + 2] = pal[idx][0]; // R
                row[x * 4 + 3] = pal[idx][3]; // A (color-block alpha; overridden for DXT3/5 below)
            }
        }
    }

    void overlay_dxt3_alpha(const uint8_t* alpha_block, uint8_t* out, int stride, int max_w, int max_h)
    {
        // 4 bits per pixel, 16 pixels = 64 bits = 8 bytes.
        for (int y = 0; y < 4 && y < max_h; ++y) {
            uint16_t row_bits = read_u16le(alpha_block + y * 2);
            uint8_t* row = out + y * stride;
            for (int x = 0; x < 4 && x < max_w; ++x) {
                uint8_t a4 = static_cast<uint8_t>((row_bits >> (x * 4)) & 0xF);
                row[x * 4 + 3] = static_cast<uint8_t>((a4 << 4) | a4);
            }
        }
    }

    void overlay_dxt5_alpha(const uint8_t* alpha_block, uint8_t* out, int stride, int max_w, int max_h)
    {
        uint8_t a0 = alpha_block[0];
        uint8_t a1 = alpha_block[1];
        uint8_t a[8];
        a[0] = a0;
        a[1] = a1;
        if (a0 > a1) {
            for (int i = 1; i < 7; ++i) {
                a[i + 1] = static_cast<uint8_t>(((7 - i) * a0 + i * a1) / 7);
            }
        }
        else {
            for (int i = 1; i < 5; ++i) {
                a[i + 1] = static_cast<uint8_t>(((5 - i) * a0 + i * a1) / 5);
            }
            a[6] = 0;
            a[7] = 255;
        }
        // 3 bits per pixel × 16 pixels = 48 bits packed in 6 bytes after the two endpoints.
        uint64_t indices = 0;
        for (int i = 0; i < 6; ++i) {
            indices |= static_cast<uint64_t>(alpha_block[2 + i]) << (i * 8);
        }
        for (int y = 0; y < 4 && y < max_h; ++y) {
            uint8_t* row = out + y * stride;
            for (int x = 0; x < 4 && x < max_w; ++x) {
                int idx = static_cast<int>((indices >> ((y * 4 + x) * 3)) & 0x7);
                row[x * 4 + 3] = a[idx];
            }
        }
    }

    bool decompress_dxt(DdsKind kind, const uint8_t* blocks, int width, int height, uint8_t* out_bgra)
    {
        const int blk_w = (width + 3) / 4;
        const int blk_h = (height + 3) / 4;
        const int stride = width * 4;
        const bool is_dxt1 = (kind == DdsKind::DXT1);
        const int block_bytes = is_dxt1 ? 8 : 16;

        for (int by = 0; by < blk_h; ++by) {
            for (int bx = 0; bx < blk_w; ++bx) {
                const uint8_t* block = blocks + (by * blk_w + bx) * block_bytes;
                uint8_t* out = out_bgra + (by * 4) * stride + (bx * 4) * 4;
                int max_w = std::min(4, width - bx * 4);
                int max_h = std::min(4, height - by * 4);

                if (is_dxt1) {
                    emit_dxt_color_block(block, out, stride, max_w, max_h, true);
                }
                else if (kind == DdsKind::DXT3) {
                    emit_dxt_color_block(block + 8, out, stride, max_w, max_h, false);
                    overlay_dxt3_alpha(block, out, stride, max_w, max_h);
                }
                else { // DXT5
                    emit_dxt_color_block(block + 8, out, stride, max_w, max_h, false);
                    overlay_dxt5_alpha(block, out, stride, max_w, max_h);
                }
            }
        }
        return true;
    }

    using BmReadHeaderFn = int(__cdecl*)(const char* name, int* w, int* h, int* format,
        int* num_levels, void* pal_unused, int* num_frames, int* fps, int* total_bytes,
        void* vbm_ver_unused, const char* default_path);
    using BmLockFn = int(__cdecl*)(int handle, void** pixels_out, void** palette_out);

    // Extension predicates live at global scope in common/bitmap/formats.h (shared with game side).

    bool read_file_to_vector(const char* filename, std::vector<uint8_t>& out)
    {
        // Editor's rf::File::open() is a *find* — open_mode() does the real fopen. close() on a
        // not-actually-opened File crashes with NULL fclose, so always go through open_mode.
        rf::File file;
        if (file.open_mode(filename) != 0) {
            return false;
        }
        const int file_size = file.get_size();
        if (file_size <= 0) {
            file.close();
            return false;
        }
        out.resize(static_cast<size_t>(file_size));
        const int bytes_read = file.read(out.data(), file_size);
        file.close();
        return bytes_read == file_size;
    }

    bool read_file_to_string(const char* filename, std::string& out)
    {
        rf::File file;
        if (file.open_mode(filename) != 0) return false;
        int file_size = file.get_size();
        if (file_size <= 0) { file.close(); return false; }
        out.assign(static_cast<size_t>(file_size), '\0');
        int n = file.read(out.data(), file_size);
        file.close();
        return n == file_size;
    }

    // PNG/JPEG via stb_image

    bool fill_stb_header(const char* filename, int* w, int* h, int* format, int* num_levels,
                        int* num_frames, int* fps, int* total_bytes)
    {
        std::vector<uint8_t> file_bytes;
        if (!read_file_to_vector(filename, file_bytes)) return false;
        int sw = 0, sh = 0, channels = 0;
        if (!stbi_info_from_memory(file_bytes.data(), static_cast<int>(file_bytes.size()),
                                   &sw, &sh, &channels)) {
            xlog::warn("editor stb_image: failed to read header for '{}': {}",
                       filename, stbi_failure_reason());
            return false;
        }
        if (sw <= 0 || sh <= 0) return false;
        // Reject oversized images so the lock path's int arithmetic can't overflow.
        if (sw > BM_STB_MAX_DIMENSION || sh > BM_STB_MAX_DIMENSION) {
            xlog::warn("editor stb_image: '{}' rejected ({}x{} exceeds {} px ceiling)",
                       filename, sw, sh, BM_STB_MAX_DIMENSION);
            return false;
        }

        // Channel-count → format mapping shared with game side via common/bitmap/formats.h
        // (1/3 channel → 24-bit RGB, 2/4 channel → 32-bit RGBA).
        *w = sw;
        *h = sh;
        *format = bm_format_from_stb_channels(channels);
        *num_levels = 1;
        *num_frames = 1;
        *fps = 0;
        *total_bytes = -1;
        return true;
    }

    // Per-binary file redirect map for STB/DDS. Populated at read_header time when a sibling
    std::unordered_map<std::string, std::string> g_file_redirects;

    // Resolve the filename to read for a given bm_entry. Returns the redirect-stored sibling
    // filename if one was recorded at header time, otherwise the entry's literal name.
    std::string resolve_locked_filename(const BitmapEntry& entry)
    {
        auto it = g_file_redirects.find(string_to_lower(entry.name));
        if (it != g_file_redirects.end()) return it->second;
        return entry.name;
    }

    bool fill_stb_locked_data(BitmapEntry& entry)
    {
        // Defensively null out before any failure path so a re-lock-after-unlock can't expose
        // the previous (already-freed) pointer if the new lock fails partway through.
        entry.locked_data = nullptr;
        entry.locked_palette = nullptr;

        const std::string filename = resolve_locked_filename(entry);
        std::vector<uint8_t> file_bytes;
        if (!read_file_to_vector(filename.c_str(), file_bytes)) {
            xlog::error("editor stb_image: failed to reopen '{}' during lock", filename);
            return false;
        }
        // Output channels match the format we declared at read_header time.
        const int desired = (entry.format == EDITOR_BM_FORMAT_8888_ARGB) ? 4 : 3;
        int w = 0, h = 0, source_channels = 0;
        uint8_t* pixels = stbi_load_from_memory(file_bytes.data(),
            static_cast<int>(file_bytes.size()), &w, &h, &source_channels, desired);
        if (!pixels) {
            xlog::error("editor stb_image: decode failed for '{}': {}",
                        entry.name, stbi_failure_reason());
            return false;
        }
        if (w != entry.orig_width || h != entry.orig_height) {
            xlog::error("editor stb_image: '{}' dims changed between header and decode", entry.name);
            stbi_image_free(pixels);
            return false;
        }
        // dims pre-validated against BM_STB_MAX_DIMENSION at header time, so w * h * desired
        // stays in int range here.
        const int num_pixels = w * h;
        const size_t total = static_cast<size_t>(num_pixels) * desired;
        void* dst = rf_alloc(total);
        if (!dst) { stbi_image_free(pixels); return false; }
        std::memcpy(dst, pixels, total);
        // stb returns RGB(A) byte order; both FORMAT_888_RGB and FORMAT_8888_ARGB use BGR(A)
        // memory layout (R at the highest channel position). Swap channel 0 and 2 per pixel.
        auto* p = static_cast<uint8_t*>(dst);
        for (int i = 0; i < num_pixels; ++i) std::swap(p[i * desired + 0], p[i * desired + 2]);
        entry.locked_data = dst;
        entry.locked_palette = nullptr;
        stbi_image_free(pixels);
        return true;
    }

    // DDS

    // Parse just the header so we can fill read_header outputs without decoding pixels.
    bool fill_dds_header(const char* filename, int* w, int* h, int* format, int* num_levels,
                         int* num_frames, int* fps, int* total_bytes)
    {
        std::vector<uint8_t> file_bytes;
        if (!read_file_to_vector(filename, file_bytes)) return false;
        if (file_bytes.size() < 4 + sizeof(DDS_HEADER)) return false;

        if (read_u32le(file_bytes.data()) != DDS_MAGIC) {
            xlog::warn("editor DDS: '{}' has wrong magic", filename);
            return false;
        }
        DDS_HEADER hdr;
        std::memcpy(&hdr, file_bytes.data() + 4, sizeof(hdr));
        if (hdr.size != sizeof(DDS_HEADER)) {
            xlog::warn("editor DDS: '{}' header size {} unexpected", filename, hdr.size);
            return false;
        }

        DdsKind kind = classify_dds(hdr.ddspf);
        if (kind == DdsKind::Unsupported) {
            xlog::warn("editor DDS: '{}' uses an unsupported pixel format (flags=0x{:x} "
                       "fourCC=0x{:x} bits={})",
                       filename, hdr.ddspf.flags, hdr.ddspf.fourCC, hdr.ddspf.RGBBitCount);
            return false;
        }

        *w = static_cast<int>(hdr.width);
        *h = static_cast<int>(hdr.height);
        *format = EDITOR_BM_FORMAT_8888_ARGB; // we always decode to RGBA8
        *num_levels = 1;                      // editor uses only base mip; saves DXT mip work
        *num_frames = 1;
        *fps = 0;
        *total_bytes = -1;
        return true;
    }

    bool fill_dds_locked_data(BitmapEntry& entry)
    {
        // Same defensive nulling as fill_stb_locked_data — protects against any path that
        // reads entry.locked_data after a re-lock that fails before the success store.
        entry.locked_data = nullptr;
        entry.locked_palette = nullptr;

        const std::string filename = resolve_locked_filename(entry);
        std::vector<uint8_t> file_bytes;
        if (!read_file_to_vector(filename.c_str(), file_bytes)) return false;
        if (file_bytes.size() < 4 + sizeof(DDS_HEADER)) return false;
        if (read_u32le(file_bytes.data()) != DDS_MAGIC) return false;

        DDS_HEADER hdr;
        std::memcpy(&hdr, file_bytes.data() + 4, sizeof(hdr));
        DdsKind kind = classify_dds(hdr.ddspf);
        const uint8_t* data = file_bytes.data() + 4 + sizeof(DDS_HEADER);
        const size_t data_bytes = file_bytes.size() - 4 - sizeof(DDS_HEADER);
        const int w = static_cast<int>(hdr.width);
        const int h = static_cast<int>(hdr.height);

        const size_t out_bytes = static_cast<size_t>(w) * h * 4;
        void* dst = rf_alloc(out_bytes);
        if (!dst) return false;
        auto* out = static_cast<uint8_t*>(dst);

        if (kind == DdsKind::RGBA32) {
            const size_t need = static_cast<size_t>(w) * h * 4;
            if (data_bytes < need) { editor_free(dst); return false; }
            std::memcpy(out, data, need);
            // Source is typically BGRA already (D3DFMT_A8R8G8B8), which is what we want — no swap.
        }
        else if (kind == DdsKind::RGB24) {
            const size_t need = static_cast<size_t>(w) * h * 3;
            if (data_bytes < need) { editor_free(dst); return false; }
            // Expand BGR → BGRA, A=255.
            for (int i = 0; i < w * h; ++i) {
                out[i * 4 + 0] = data[i * 3 + 0];
                out[i * 4 + 1] = data[i * 3 + 1];
                out[i * 4 + 2] = data[i * 3 + 2];
                out[i * 4 + 3] = 255;
            }
        }
        else { // DXT1 / DXT3 / DXT5
            const int blk_w = (w + 3) / 4;
            const int blk_h = (h + 3) / 4;
            const int block_bytes = (kind == DdsKind::DXT1) ? 8 : 16;
            const size_t need = static_cast<size_t>(blk_w) * blk_h * block_bytes;
            if (data_bytes < need) {
                xlog::error("editor DDS: '{}' truncated (need {} bytes for blocks, have {})",
                            entry.name, need, data_bytes);
                editor_free(dst);
                return false;
            }
            decompress_dxt(kind, data, w, h, out);
        }

        entry.locked_data = dst;
        entry.locked_palette = nullptr;
        return true;
    }

    // ATX (frame-0 preview)
    // Editor doesn't run the game tick, so animation is irrelevant
    // Keyed by lowercased bm_entry.name (the originally requested filename — which may be a
    // legacy .tga/.vbm name superceded to .atx). The value is the resolved frame[0] filename.
    std::unordered_map<std::string, std::string> g_atx_redirects;

    // Re-entrancy guard in case anything goes weird while loading a child texture.
    bool g_loading_atx_child = false;

    std::optional<std::string> get_atx_frame0_filename(const char* atx_filename)
    {
        std::string content;
        if (!read_file_to_string(atx_filename, content)) return std::nullopt;
        auto spec = parse_atx(content, atx_filename);
        if (!spec || spec->frames.empty()) return std::nullopt;
        return spec->frames[0].filename;
    }

    // Returns true if the editor's VFS can find the named file (either loose or in a packfile).
    // Cheap probe used to decide whether a sibling exists before committing to decode it.
    bool sibling_file_exists(const char* filename)
    {
        rf::File file;
        if (file.open_mode(filename) != 0) return false;
        file.close();
        return true;
    }

    // Probe the supercede chain (.atx > .dds > .png > .jpg > .jpeg) for a sibling of
    // `requested_name`. Returns the first existing sibling that is not the requested file
    // itself, or empty string if none qualify. Mirrors the game-side chain so the editor
    // previews the same file the game will actually load.
    constexpr std::array<std::string_view, 5> editor_sibling_supercede_extensions = {
        ".atx", ".dds", ".png", ".jpg", ".jpeg"
    };

    std::string find_supercede_sibling(const char* requested_name)
    {
        std::string base{get_filename_without_ext(requested_name)};
        for (auto ext : editor_sibling_supercede_extensions) {
            std::string candidate = base + std::string{ext};
            // Don't resolve to ourselves — the caller already handles its own extension via
            // the direct dispatch below. This also avoids infinite recursion when the
            // requested file happens to be one of the new formats but doesn't exist on disk.
            if (string_iequals(candidate, requested_name)) continue;
            if (sibling_file_exists(candidate.c_str())) {
                return candidate;
            }
        }
        return {};
    }

    bool fill_atx_header(const char* atx_filename, const char* original_name,
                         int* w, int* h, int* format, int* num_levels,
                         int* num_frames, int* fps, int* total_bytes)
    {
        auto frame0 = get_atx_frame0_filename(atx_filename);
        if (!frame0) return false;

        // Recursively load the child via the editor's bm system. With g_loading_atx_child set,
        // our hook refuses to recurse on .atx names.
        int child_handle = -1;
        {
            g_loading_atx_child = true;
            child_handle = BitmapEntry::load(frame0->c_str(), 0);
            g_loading_atx_child = false;
        }
        if (child_handle < 0) {
            xlog::warn("editor ATX: '{}' could not load frame[0] '{}'", atx_filename, *frame0);
            return false;
        }
        const int idx = BitmapEntry::handle_to_index(child_handle);
        if (idx < 0) return false;
        BitmapEntry& child = BitmapEntry::entries[idx];

        *w = child.orig_width ? child.orig_width : child.width;
        *h = child.orig_height ? child.orig_height : child.height;
        *format = child.format;
        *num_levels = child.num_levels ? child.num_levels : 1;
        *num_frames = 1;
        *fps = 0;
        *total_bytes = -1;

        // Index the redirect under the originally-requested name (matches what the bm system
        // will store in entry.name) AND under the .atx filename, so direct .atx loads still
        // work alongside supercede resolutions.
        g_atx_redirects[string_to_lower(original_name)] = *frame0;
        if (!string_iequals(original_name, atx_filename)) {
            g_atx_redirects[string_to_lower(atx_filename)] = *frame0;
        }
        return true;
    }

    FunHook<int(const char*, int*, int*, int*, int*, void*, int*, int*, int*, void*, const char*)>
    editor_bm_read_header_hook{
        0x004BC200,
        [](const char* name, int* w, int* h, int* format, int* num_levels, void* pal_buf,
           int* num_frames, int* fps, int* total_bytes, void* vbm_ver_buf, const char* default_path)
        {
            if (is_stb_filename(name)) {
                if (fill_stb_header(name, w, h, format, num_levels, num_frames, fps, total_bytes)) {
                    return EDITOR_BM_TYPE_STB;
                }
                return 0;
            }
            if (is_dds_filename(name)) {
                if (fill_dds_header(name, w, h, format, num_levels, num_frames, fps, total_bytes)) {
                    return EDITOR_BM_TYPE_DDS;
                }
                return 0; // unsupported DDS subformat → placeholder, no error dialog
            }
            if (is_atx_filename(name)) {
                if (g_loading_atx_child) return 0; // refuse recursive .atx load
                if (fill_atx_header(name, name, w, h, format, num_levels, num_frames, fps, total_bytes)) {
                    return EDITOR_BM_TYPE_ATX;
                }
                return 0;
            }
            // Legacy-named — probe the supercede chain so the editor preview
            // matches what the game side resolves at runtime. WYSIWYG with the in-game render.
            if (!g_loading_atx_child) {
                std::string sibling = find_supercede_sibling(name);
                if (!sibling.empty()) {
                    if (is_atx_filename(sibling)) {
                        if (fill_atx_header(sibling.c_str(), name, w, h, format, num_levels,
                                            num_frames, fps, total_bytes)) {
                            return EDITOR_BM_TYPE_ATX;
                        }
                    }
                    else if (is_dds_filename(sibling)) {
                        if (fill_dds_header(sibling.c_str(), w, h, format, num_levels,
                                            num_frames, fps, total_bytes)) {
                            g_file_redirects[string_to_lower(name)] = sibling;
                            return EDITOR_BM_TYPE_DDS;
                        }
                    }
                    else if (is_stb_filename(sibling)) {
                        if (fill_stb_header(sibling.c_str(), w, h, format, num_levels,
                                            num_frames, fps, total_bytes)) {
                            g_file_redirects[string_to_lower(name)] = sibling;
                            return EDITOR_BM_TYPE_STB;
                        }
                    }
                    // sibling probe failed; fall through to stock dispatch below
                }
            }
            return editor_bm_read_header_hook.call_target(name, w, h, format, num_levels, pal_buf,
                num_frames, fps, total_bytes, vbm_ver_buf, default_path);
        }
    };

    // Lock an entry by its bm_type. For custom types (STB/DDS), invoke the editor-side decoder
    // directly. For stock types, invoke the trampoline so RED's original dispatch runs.
    //
    // RED's original bm_lock at 0x004BCCD0 contains a default-case `do { assert } while(true)`
    // which hangs the editor on unknown bm_type, so we must NEVER let a custom bm_type
    // (0x10/0x11/0x12) reach the trampoline.
    int dispatch_lock(BitmapEntry& entry, int handle, void** pixels_out, void** palette_out);

    FunHook<int(int, void**, void**)> editor_bm_lock_hook{
        0x004BCCD0,
        [](int handle, void** pixels_out, void** palette_out) -> int {
            const int idx = BitmapEntry::handle_to_index(handle);
            if (idx < 0) {
                *pixels_out = nullptr;
                *palette_out = nullptr;
                return 0;
            }
            BitmapEntry& entry = BitmapEntry::entries[idx];
            return dispatch_lock(entry, handle, pixels_out, palette_out);
        }
    };

    int dispatch_lock(BitmapEntry& entry, int handle, void** pixels_out, void** palette_out)
    {
        if (entry.bm_type == EDITOR_BM_TYPE_STB) {
            if (fill_stb_locked_data(entry)) {
                *pixels_out = entry.locked_data;
                *palette_out = entry.locked_palette;
                return entry.format;
            }
            *pixels_out = nullptr; *palette_out = nullptr; return 0;
        }
        if (entry.bm_type == EDITOR_BM_TYPE_DDS) {
            if (fill_dds_locked_data(entry)) {
                *pixels_out = entry.locked_data;
                *palette_out = entry.locked_palette;
                return entry.format;
            }
            *pixels_out = nullptr; *palette_out = nullptr; return 0;
        }
        if (entry.bm_type == EDITOR_BM_TYPE_ATX) {
            auto it = g_atx_redirects.find(string_to_lower(entry.name));
            if (it == g_atx_redirects.end()) {
                *pixels_out = nullptr; *palette_out = nullptr; return 0;
            }
            // Re-resolve frame[0]'s bm_handle by filename. BitmapEntry::load is name-cached
            // (returns the existing handle if already loaded) so this is cheap, and it's
            // robust against the original child handle having been freed and replaced.
            int child_handle = -1;
            {
                g_loading_atx_child = true;
                child_handle = BitmapEntry::load(it->second.c_str(), 0);
                g_loading_atx_child = false;
            }
            if (child_handle < 0) {
                *pixels_out = nullptr; *palette_out = nullptr; return 0;
            }
            const int child_idx = BitmapEntry::handle_to_index(child_handle);
            if (child_idx < 0) {
                *pixels_out = nullptr; *palette_out = nullptr; return 0;
            }
            BitmapEntry& child = BitmapEntry::entries[child_idx];
            // Re-dispatch on the child's bm_type. This routes custom-type children (DDS/STB)
            // through our decoders rather than RED's original lock, which would hang on those
            // types. Stock-type children (TGA/VBM/etc.) hit the call_target branch below.
            return dispatch_lock(child, child_handle, pixels_out, palette_out);
        }
        // Stock bm_type — safe to invoke RED's original lock.
        return editor_bm_lock_hook.call_target(handle, pixels_out, palette_out);
    }
}

void install_editor_bitmap_loader_hooks()
{
    editor_bm_read_header_hook.install();
    editor_bm_lock_hook.install();
    xlog::info("Editor bitmap loader hooks installed (PNG/JPG, DDS, ATX preview)");
}

void clear_editor_bitmap_redirects()
{
    g_atx_redirects.clear();
    g_file_redirects.clear();
}

std::vector<std::string> parse_atx_dependencies(const char* atx_filename)
{
    std::vector<std::string> deps;
    std::string content;
    if (!read_file_to_string(atx_filename, content)) {
        return deps;
    }

    auto spec = parse_atx(content, atx_filename);
    if (!spec) return deps;

    // [header].alpha_mask (the parser already filtered out empty strings).
    if (spec->header.alpha_mask && !string_iends_with(*spec->header.alpha_mask, ".atx")) {
        deps.push_back(*spec->header.alpha_mask);
    }
    // Each [[frame]].file — parser already enforces non-empty and non-.atx.
    for (auto& f : spec->frames) {
        deps.push_back(std::move(f.filename));
    }
    return deps;
}
