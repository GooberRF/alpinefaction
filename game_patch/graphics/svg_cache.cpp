#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <xlog/xlog.h>
#include <common/utils/string-utils.h>
#include <lunasvg.h>
#include "svg_cache.h"
#include "../bmpman/bmpman.h"
#include "../misc/misc.h"
#include "../os/os.h"
#include "../rf/bmpman.h"
#include "../rf/file/file.h"
#include "../rf/gr/gr.h"

namespace
{
    // 4 MB is far above any chrome asset; a bigger .svg is a sign of something that is not ours.
    constexpr int max_svg_file_size = 4 * 1024 * 1024;
    constexpr int max_svg_bitmap_dim = 4096;

    struct SizedKeyView
    {
        std::string_view name;
        int w;
        int h;

        bool operator==(const SizedKeyView& other) const = default;
    };

    struct SizedKey
    {
        std::string name;
        int w;
        int h;

        operator SizedKeyView() const
        {
            return {name, w, h};
        }
    };

    // Transparent so the per-frame cache hit never allocates a key string.
    struct SizedKeyHash
    {
        using is_transparent = void;

        std::size_t operator()(SizedKeyView key) const
        {
            std::size_t h = std::hash<std::string_view>{}(key.name);
            h ^= std::hash<int>{}(key.w) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<int>{}(key.h) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };

    struct SizedKeyEqual
    {
        using is_transparent = void;

        bool operator()(SizedKeyView a, SizedKeyView b) const
        {
            return a == b;
        }
    };

    // Both caches keep negative results (null document / -1 handle) so a missing or malformed
    // asset is parsed and reported once instead of every frame.
    std::unordered_map<std::string, std::unique_ptr<lunasvg::Document>> g_documents;
    struct CachedBitmap
    {
        int bm_handle;
        int64_t last_used_ms;
    };
    std::unordered_map<SizedKey, CachedBitmap, SizedKeyHash, SizedKeyEqual> g_bitmaps;
    int64_t g_last_sweep_ms = 0;
    // Every size is keyed exactly, so a resolution change or a resized panel leaves the old
    // rasters behind; anything not drawn for this long is released.
    constexpr int64_t bitmap_idle_ms = 10000;

    void sweep_idle_bitmaps(int64_t now_ms)
    {
        if (now_ms - g_last_sweep_ms < bitmap_idle_ms) {
            return;
        }
        g_last_sweep_ms = now_ms;
        for (auto it = g_bitmaps.begin(); it != g_bitmaps.end();) {
            if (now_ms - it->second.last_used_ms < bitmap_idle_ms) {
                ++it;
                continue;
            }
            if (it->second.bm_handle != -1) {
                // Dropped from the renderer first, so a recycled slot cannot inherit the texture.
                rf::gr::tcache_remove_ref(it->second.bm_handle);
                rf::gr::mark_texture_dirty(it->second.bm_handle);
                rf::bm::release(it->second.bm_handle);
            }
            it = g_bitmaps.erase(it);
        }
    }

    std::string svg_filename(std::string_view name)
    {
        std::string filename{name};
        if (!string_iends_with(name, ".svg")) {
            filename += ".svg";
        }
        return filename;
    }

    bool read_svg_file(const char* filename, std::vector<char>& out)
    {
        rf::File file;
        if (file.open(filename) != 0) {
            return false;
        }
        const int file_size = file.size();
        if (file_size <= 0 || file_size > max_svg_file_size) {
            file.close();
            return false;
        }
        out.resize(static_cast<std::size_t>(file_size));
        const int bytes_read = file.read(out.data(), file_size);
        file.close();
        return bytes_read == file_size;
    }

    lunasvg::Document* get_document(std::string_view name)
    {
        std::string filename = svg_filename(name);
        auto it = g_documents.find(filename);
        if (it != g_documents.end()) {
            return it->second.get();
        }

        std::unique_ptr<lunasvg::Document> doc;
        // The SVG parser is an untrusted-input surface with a known crash history, so chrome is
        // only ever parsed from Alpine's own packfile - never from user_maps or client_mods.
        if (!file_loaded_from_alpinefaction_vpp(filename.c_str())) {
            xlog::error("[SVG] {} rejected: not from alpinefaction.vpp", filename);
        }
        else {
            std::vector<char> data;
            if (!read_svg_file(filename.c_str(), data)) {
                xlog::error("[SVG] Failed to read SVG {}", filename);
            }
            // LunaSVG resolves any <image> href that is not a data: URI as a file path (local or
            // UNC). Chrome never references anything, so a document that does is refused outright.
            else if (std::string_view text{data.data(), data.size()};
                     text.find("href") != std::string_view::npos || text.find("<image") != std::string_view::npos) {
                xlog::error("[SVG] {} rejected: external references are not allowed", filename);
            }
            else {
                doc = lunasvg::Document::loadFromData(data.data(), data.size());
                if (!doc) {
                    xlog::error("[SVG] Failed to parse SVG {}", filename);
                }
            }
        }
        return g_documents.emplace(std::move(filename), std::move(doc)).first->second.get();
    }

    // LunaSVG hands back premultiplied ARGB32, which on x86 is B,G,R,A in memory - the byte order
    // RF calls FORMAT_8888_ARGB. Neither backend has a premultiplied blend mode, so undo it here.
    void unpremultiply(uint8_t* data, int w, int h, int stride)
    {
        for (int y = 0; y < h; ++y) {
            uint8_t* row = data + static_cast<std::ptrdiff_t>(y) * stride;
            for (int x = 0; x < w; ++x) {
                uint8_t* px = row + x * 4;
                const unsigned a = px[3];
                if (a == 0 || a == 255) {
                    continue;
                }
                for (int c = 0; c < 3; ++c) {
                    px[c] = static_cast<uint8_t>(std::min(255u, (px[c] * 255u + a / 2) / a));
                }
            }
        }
    }

    int rasterize(lunasvg::Document& doc, const std::string& name, int w, int h)
    {
        lunasvg::Bitmap bitmap = doc.renderToBitmap(w, h);
        if (bitmap.isNull() || bitmap.width() != w || bitmap.height() != h) {
            xlog::error("[SVG] Failed to rasterize SVG {} at {}x{}", name, w, h);
            return -1;
        }
        unpremultiply(bitmap.data(), bitmap.width(), bitmap.height(), bitmap.stride());

        int bm_handle = rf::bm::create(rf::bm::FORMAT_8888_ARGB, w, h);
        if (bm_handle == -1) {
            xlog::error("[SVG] bm_create failed for SVG {}", name);
            return -1;
        }
        if (!bm_write_argb(bm_handle, bitmap.data(), w, h, bitmap.stride())) {
            xlog::error("[SVG] Failed to upload SVG {}", name);
            rf::gr::mark_texture_dirty(bm_handle);
            rf::bm::release(bm_handle);
            return -1;
        }
        // Survives the force=0 texture cache flush done on every level transition.
        rf::gr::tcache_add_ref(bm_handle);
        return bm_handle;
    }
}

int svg_bitmap(std::string_view name, int w, int h)
{
    if (w <= 0 || h <= 0 || w > max_svg_bitmap_dim || h > max_svg_bitmap_dim) {
        return -1;
    }
    const int64_t now_ms = timer::get_i64(1000);
    sweep_idle_bitmaps(now_ms);
    std::string_view stem = name;
    if (string_iends_with(stem, ".svg")) {
        stem.remove_suffix(4);
    }
    auto it = g_bitmaps.find(SizedKeyView{stem, w, h});
    if (it != g_bitmaps.end()) {
        it->second.last_used_ms = now_ms;
        return it->second.bm_handle;
    }
    lunasvg::Document* doc = get_document(name);
    int bm_handle = doc ? rasterize(*doc, svg_filename(name), w, h) : -1;
    g_bitmaps.emplace(SizedKey{std::string{stem}, w, h}, CachedBitmap{bm_handle, now_ms});
    return bm_handle;
}
