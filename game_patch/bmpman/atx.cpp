#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <toml++/toml.hpp>
#include <xlog/xlog.h>

#include "atx.h"
#include "bmpman.h"
#include "../graphics/gr.h"
#include "../rf/bmpman.h"
#include "../rf/file/file.h"
#include "../rf/gr/gr.h"
#include "../rf/os/frametime.h"

namespace
{
    enum class AtxAnimationMode : int
    {
        Static = 0,    // No auto playback. Frames change only via events.
        PingPong = 1,  // Forward to last, then backward to first, repeating.
        Loop = 2,      // Forward; wrap to 0 after last.
        PlayOnce = 3,  // Forward; stop and hold on the last frame.
    };

    struct AtxFrame
    {
        std::string filename;
        int bm_handle = -1;
        int frame_time_ms = -1; // -1 means inherit base_frame_time_ms
        uint8_t* locked_pixels = nullptr;
        uint8_t* locked_palette = nullptr;
        // Owned per-frame buffer used when the ATX applies format coercion or alpha-masking.
        // When non-null, locked_pixels points into this buffer and bm_handle has already been
        // released. When null, locked_pixels points into the source child's locked pages.
        std::unique_ptr<uint8_t[]> owned_buffer;
        // Per-frame material override (e.g. frame is metal even though the ATX is wood).
        // Falls back to the controller's header_material if unset.
        std::optional<uint8_t> material_override;
    };

    struct AtxController
    {
        std::string handle_str; // lowercase basename (filename without extension or path)

        std::vector<AtxFrame> frames;
        AtxAnimationMode animation_mode = AtxAnimationMode::Loop;
        int base_frame_time_ms = 100;
        bool initially_on = true;

        // Cached metadata (matches frame[0])
        int width = 0;
        int height = 0;
        rf::bm::Format format = rf::bm::FORMAT_NONE;
        int num_levels = 1;

        // Runtime state
        int current_frame = 0;
        int direction = 1; // ping-pong only: +1 or -1
        bool playing = true;
        float time_in_frame_s = 0.0f;
        int atx_bm_handle = -1; // populated on first lock

        // ATX-wide material override (applied when a frame doesn't have its own).
        std::optional<uint8_t> header_material;
        // True if any override (header or per-frame) is configured. Used as a fast early-out
        // in the material-getter hook so non-override ATXes pay zero overhead.
        bool has_material_override = false;
    };

    // Single registry keyed by lowercase basename. With ATX supercede in effect a caller may
    // load the same file under any extension (foo.tga, foo.dds, foo.atx, foo) — they all
    // resolve to the same controller, so basename is the right key.
    std::unordered_map<std::string, std::unique_ptr<AtxController>> g_controllers;

    // Basenames whose .atx file has been seen this level and failed to parse/validate. Cached so
    // we don't re-run the failing parse on every subsequent load of any extension that supercedes
    // to this name (each retry would log spam and could re-disturb the bm cache).
    std::unordered_set<std::string> g_failed;

    bool g_loading_child = false;
    struct ChildLoadGuard {
        ChildLoadGuard() { g_loading_child = true; }
        ~ChildLoadGuard() { g_loading_child = false; }
    };

    std::string to_lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    bool ends_with_icase(const std::string& s, const char* suffix)
    {
        size_t suf_len = std::strlen(suffix);
        if (s.size() < suf_len) return false;
        for (size_t i = 0; i < suf_len; ++i) {
            if (std::tolower(static_cast<unsigned char>(s[s.size() - suf_len + i]))
                != std::tolower(static_cast<unsigned char>(suffix[i]))) {
                return false;
            }
        }
        return true;
    }

    std::string handle_from_filename(const char* filename)
    {
        std::string s = filename;
        auto last_slash = s.find_last_of("/\\");
        if (last_slash != std::string::npos) s = s.substr(last_slash + 1);
        auto dot = s.find_last_of('.');
        if (dot != std::string::npos) s.resize(dot);
        return to_lower(std::move(s));
    }

    bool read_atx_text(const char* filename, std::string& content_out)
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
        content_out.assign(static_cast<size_t>(file_size), '\0');
        const int bytes_read = file.read(content_out.data(), file_size);
        file.close();
        return bytes_read == file_size;
    }

    // Drop the ATX's exclusive lock on each loaded child, but leave the bm-load reference in place.
    // Used by parse-failure paths: dropping the ref there has caused stock RF to mis-handle later
    // standalone loads of the same .tga, so we accept a small per-failure ref leak (goes away on
    // level transition) in exchange for not destabilising the bm cache.
    void unlock_children(AtxController& c)
    {
        for (auto& f : c.frames) {
            if (f.bm_handle >= 0 && (f.locked_pixels || f.locked_palette)) {
                rf::bm::unlock(f.bm_handle);
                f.locked_pixels = nullptr;
                f.locked_palette = nullptr;
            }
        }
    }

    // Full teardown: unlock then release. Only call when the ATX itself is being destroyed
    // (atx_free, atx_level_reset).
    void release_children(AtxController& c)
    {
        for (auto& f : c.frames) {
            if (f.bm_handle >= 0) {
                if (f.locked_pixels || f.locked_palette) {
                    rf::bm::unlock(f.bm_handle);
                    f.locked_pixels = nullptr;
                    f.locked_palette = nullptr;
                }
                rf::bm::release(f.bm_handle);
                f.bm_handle = -1;
            }
        }
    }

    int frame_time_for(const AtxController& c, int idx)
    {
        if (idx < 0 || idx >= static_cast<int>(c.frames.size())) {
            return c.base_frame_time_ms;
        }
        const int ft = c.frames[idx].frame_time_ms;
        return ft > 0 ? ft : c.base_frame_time_ms;
    }

    void dirty_atx(AtxController& c)
    {
        if (c.atx_bm_handle >= 0) {
            rf::gr::mark_texture_dirty(c.atx_bm_handle);
        }
    }

    AtxController* get_by_handle(const std::string& handle)
    {
        auto it = g_controllers.find(to_lower(handle));
        return it == g_controllers.end() ? nullptr : it->second.get();
    }

    // Map a bm_entry name (which may carry any extension or none, depending on what the caller
    // originally requested) to the controller key.
    std::string key_from_bm_name(const char* bm_name)
    {
        return handle_from_filename(bm_name);
    }

    // Stock RF material indices (array at 0x0059CB10). Names are case-insensitive in the TOML;
    // we normalise to lowercase before lookup.
    std::optional<uint8_t> parse_material_name(std::string s)
    {
        s = to_lower(std::move(s));
        if (s == "default") return 0;
        if (s == "rock")    return 1;
        if (s == "metal")   return 2;
        if (s == "flesh")   return 3;
        if (s == "water")   return 4;
        if (s == "lava")    return 5;
        if (s == "solid")   return 6;
        if (s == "sand")    return 7;
        if (s == "ice")     return 8;
        if (s == "glass")   return 9;
        return std::nullopt;
    }

    // Parse a TOML format string ("8888_argb", "4444", etc.) into the matching bm Format enum.
    std::optional<rf::bm::Format> parse_format_string(std::string s)
    {
        s = to_lower(std::move(s));
        if (s == "565" || s == "565_rgb") return rf::bm::FORMAT_565_RGB;
        if (s == "4444" || s == "4444_argb") return rf::bm::FORMAT_4444_ARGB;
        if (s == "1555" || s == "1555_argb") return rf::bm::FORMAT_1555_ARGB;
        if (s == "888" || s == "888_rgb") return rf::bm::FORMAT_888_RGB;
        if (s == "8888" || s == "8888_argb") return rf::bm::FORMAT_8888_ARGB;
        return std::nullopt;
    }

    // Formats that the ATX transform pass can read or write. Compressed (DXT*) and paletted
    // formats are excluded — we'd need a real codec to round-trip them.
    bool is_supported_for_transform(rf::bm::Format f)
    {
        switch (f) {
            case rf::bm::FORMAT_565_RGB:
            case rf::bm::FORMAT_4444_ARGB:
            case rf::bm::FORMAT_1555_ARGB:
            case rf::bm::FORMAT_888_RGB:
            case rf::bm::FORMAT_8888_ARGB:
                return true;
            default:
                return false;
        }
    }

    // If `f` lacks an alpha channel, return an analogous format with one. Used to auto-promote
    // when the mapper supplies an alpha mask without explicitly choosing a target format.
    rf::bm::Format promote_to_alpha(rf::bm::Format f)
    {
        switch (f) {
            case rf::bm::FORMAT_565_RGB: return rf::bm::FORMAT_4444_ARGB;
            case rf::bm::FORMAT_888_RGB: return rf::bm::FORMAT_8888_ARGB;
            default: return f;
        }
    }

    // Total bytes for a full mip chain at (w, h) in `fmt` over `num_levels`.
    size_t mip_chain_bytes(int w, int h, rf::bm::Format fmt, int num_levels)
    {
        size_t total = 0;
        int mw = w, mh = h;
        for (int i = 0; i < num_levels; ++i) {
            total += bm_calculate_total_bytes(mw, mh, fmt);
            mw = std::max(mw / 2, 1);
            mh = std::max(mh / 2, 1);
        }
        return total;
    }

    // Overlay an 8-bit greyscale mask as the alpha channel of a destination buffer in `dst_fmt`.
    // Caller guarantees dst_fmt is one of the alpha-bearing supported formats.
    void overlay_alpha(uint8_t* dst, rf::bm::Format dst_fmt, const uint8_t* mask, int num_pixels)
    {
        switch (dst_fmt) {
            case rf::bm::FORMAT_8888_ARGB:
                // Memory order is BGRA on little-endian; alpha is the 4th byte per pixel.
                for (int i = 0; i < num_pixels; ++i) {
                    dst[i * 4 + 3] = mask[i];
                }
                break;
            case rf::bm::FORMAT_4444_ARGB:
                // Two bytes per pixel little-endian; alpha lives in the high nibble of byte 1.
                for (int i = 0; i < num_pixels; ++i) {
                    dst[i * 2 + 1] = static_cast<uint8_t>((dst[i * 2 + 1] & 0x0F) | (mask[i] & 0xF0));
                }
                break;
            case rf::bm::FORMAT_1555_ARGB:
                // Single alpha bit at the top of byte 1; threshold the mask at 0x80.
                for (int i = 0; i < num_pixels; ++i) {
                    if (mask[i] >= 128) dst[i * 2 + 1] |= 0x80;
                    else                dst[i * 2 + 1] &= 0x7F;
                }
                break;
            default:
                break;
        }
    }

    std::unique_ptr<AtxController> parse_and_load(const char* filename)
    {
        std::string content;
        if (!read_atx_text(filename, content)) {
            xlog::warn("ATX: failed to open '{}'", filename);
            return nullptr;
        }

        toml::table tbl;
        try {
            tbl = toml::parse(content, std::string_view{filename});
        }
        catch (const toml::parse_error& e) {
            xlog::warn("ATX: parse error in '{}': {}", filename, e.description());
            return nullptr;
        }

        auto c = std::make_unique<AtxController>();
        c->handle_str = handle_from_filename(filename);

        std::optional<rf::bm::Format> target_format;
        std::optional<std::string> alpha_mask_filename;

        if (auto* hdr = tbl.get_as<toml::table>("header")) {
            // `frame_time` in [header] is the default for any [[frame]] that doesn't specify its own.
            if (auto v = (*hdr)["frame_time"].value<int64_t>()) {
                c->base_frame_time_ms = std::max<int>(1, static_cast<int>(*v));
            }
            if (auto v = (*hdr)["initially_on"].value<bool>()) {
                c->initially_on = *v;
            }
            if (auto v = (*hdr)["animation_mode"].value<int64_t>()) {
                int mode = static_cast<int>(*v);
                if (mode < 0 || mode > 3) {
                    xlog::warn("ATX '{}': animation_mode {} out of range, defaulting to Loop", filename, mode);
                    mode = static_cast<int>(AtxAnimationMode::Loop);
                }
                c->animation_mode = static_cast<AtxAnimationMode>(mode);
            }
            if (auto v = (*hdr)["format"].value<std::string>()) {
                target_format = parse_format_string(*v);
                if (!target_format) {
                    xlog::error("ATX '{}': unrecognized format '{}'", filename, *v);
                    return nullptr;
                }
            }
            if (auto v = (*hdr)["alpha_mask"].value<std::string>()) {
                alpha_mask_filename = *v;
            }
            if (auto v = (*hdr)["material"].value<std::string>()) {
                auto mat = parse_material_name(*v);
                if (!mat) {
                    xlog::error("ATX '{}': unrecognized material '{}' in [header]", filename, *v);
                    return nullptr;
                }
                c->header_material = mat;
                c->has_material_override = true;
            }
        }

        const auto* frames_arr = tbl.get_as<toml::array>("frame");
        if (!frames_arr || frames_arr->empty()) {
            xlog::warn("ATX '{}': no [[frame]] entries", filename);
            return nullptr;
        }

        c->frames.reserve(frames_arr->size());
        for (auto&& node : *frames_arr) {
            const auto* frame_tbl = node.as_table();
            if (!frame_tbl) {
                xlog::warn("ATX '{}': non-table frame entry", filename);
                return nullptr;
            }
            AtxFrame f;
            if (auto v = (*frame_tbl)["file"].value<std::string>()) {
                f.filename = *v;
            }
            if (f.filename.empty()) {
                xlog::warn("ATX '{}': frame missing 'file'", filename);
                return nullptr;
            }
            if (ends_with_icase(f.filename, ".atx")) {
                xlog::warn("ATX '{}': nested .atx not allowed (frame '{}')", filename, f.filename);
                return nullptr;
            }
            if (auto v = (*frame_tbl)["frame_time"].value<int64_t>()) {
                f.frame_time_ms = std::max<int>(1, static_cast<int>(*v));
            }
            if (auto v = (*frame_tbl)["material"].value<std::string>()) {
                auto mat = parse_material_name(*v);
                if (!mat) {
                    xlog::error("ATX '{}': unrecognized material '{}' in [[frame]]", filename, *v);
                    return nullptr;
                }
                f.material_override = mat;
                c->has_material_override = true;
            }
            c->frames.push_back(std::move(f));
        }

        for (size_t i = 0; i < c->frames.size(); ++i) {
            auto& f = c->frames[i];
            {
                ChildLoadGuard guard;
                f.bm_handle = rf::bm::load(f.filename.c_str(), -1, true);
            }
            if (f.bm_handle < 0) {
                xlog::error("ATX '{}': failed to load child '{}'", filename, f.filename);
                unlock_children(*c);
                return nullptr;
            }

            int w = 0, h = 0, num_pixels = 0, num_levels = 0;
            rf::bm::get_mipmap_info(f.bm_handle, &w, &h, &num_pixels, &num_levels);
            const rf::bm::Format fmt = rf::bm::get_format(f.bm_handle);

            if (i == 0) {
                c->width = w;
                c->height = h;
                c->format = fmt;
                c->num_levels = num_levels;
            }
            else if (w != c->width || h != c->height || fmt != c->format || num_levels != c->num_levels) {
                xlog::error("ATX '{}': frame {} '{}' mismatch {}x{} fmt={} mips={}, expected {}x{} fmt={} mips={}",
                    filename, i, f.filename, w, h, static_cast<int>(fmt), num_levels,
                    c->width, c->height, static_cast<int>(c->format), c->num_levels);
                unlock_children(*c);
                return nullptr;
            }

            // Lock the child once and keep it resident for fast frame swaps.
            uint8_t* px = nullptr;
            uint8_t* pal = nullptr;
            const rf::bm::Format locked_fmt = rf::bm::lock(f.bm_handle, &px, &pal);
            if (locked_fmt == rf::bm::FORMAT_NONE || px == nullptr) {
                xlog::error("ATX '{}': failed to lock child '{}'", filename, f.filename);
                unlock_children(*c);
                return nullptr;
            }
            f.locked_pixels = px;
            f.locked_palette = pal;
        }

        // Optional transform pass — format coercion and/or alpha mask overlay.
        // Skipped entirely (zero overhead) when both `format` and `alpha_mask` are absent.
        // Also skipped if `format` is set but matches the source and there's no mask.
        bool format_change = target_format && *target_format != c->format;
        if (format_change || alpha_mask_filename) {
            rf::bm::Format effective = target_format.value_or(c->format);
            if (alpha_mask_filename) {
                effective = promote_to_alpha(effective);
            }

            if (!is_supported_for_transform(c->format)) {
                xlog::error("ATX '{}': source format {} cannot be transformed (uncompressed RGB/RGBA only)",
                    filename, static_cast<int>(c->format));
                unlock_children(*c);
                return nullptr;
            }
            if (!is_supported_for_transform(effective)) {
                xlog::error("ATX '{}': target format {} not supported (uncompressed RGB/RGBA only)",
                    filename, static_cast<int>(effective));
                unlock_children(*c);
                return nullptr;
            }

            // Load and validate the alpha mask if one was specified.
            int mask_handle = -1;
            uint8_t* mask_pixels = nullptr;
            rf::bm::Format mask_fmt = rf::bm::FORMAT_NONE;
            if (alpha_mask_filename) {
                {
                    ChildLoadGuard guard;
                    mask_handle = rf::bm::load(alpha_mask_filename->c_str(), -1, true);
                }
                if (mask_handle < 0) {
                    xlog::error("ATX '{}': failed to load alpha mask '{}'", filename, *alpha_mask_filename);
                    unlock_children(*c);
                    return nullptr;
                }
                int mw = 0, mh = 0, mnp = 0, mml = 0;
                rf::bm::get_mipmap_info(mask_handle, &mw, &mh, &mnp, &mml);
                if (mw != c->width || mh != c->height || mml != c->num_levels) {
                    xlog::error("ATX '{}': alpha mask '{}' dimensions/mips don't match frames "
                                "(got {}x{} mips={}, expected {}x{} mips={})",
                        filename, *alpha_mask_filename, mw, mh, mml,
                        c->width, c->height, c->num_levels);
                    rf::bm::release(mask_handle);
                    unlock_children(*c);
                    return nullptr;
                }
                mask_fmt = rf::bm::get_format(mask_handle);
                if (mask_fmt != rf::bm::FORMAT_8_PALETTED && mask_fmt != rf::bm::FORMAT_8_ALPHA) {
                    xlog::error("ATX '{}': alpha mask '{}' must be 8-bit greyscale (got format {})",
                        filename, *alpha_mask_filename, static_cast<int>(mask_fmt));
                    rf::bm::release(mask_handle);
                    unlock_children(*c);
                    return nullptr;
                }
                uint8_t* dummy_pal = nullptr;
                rf::bm::lock(mask_handle, &mask_pixels, &dummy_pal);
                if (!mask_pixels) {
                    xlog::error("ATX '{}': failed to lock alpha mask '{}'", filename, *alpha_mask_filename);
                    rf::bm::release(mask_handle);
                    unlock_children(*c);
                    return nullptr;
                }
            }

            // Per-frame transform: allocate owned buffer, convert + overlay alpha for each mip,
            // then drop the source bm reference (the owned buffer is self-contained).
            const size_t total_bytes = mip_chain_bytes(c->width, c->height, effective, c->num_levels);
            for (auto& f : c->frames) {
                f.owned_buffer = std::make_unique<uint8_t[]>(total_bytes);

                const uint8_t* src = f.locked_pixels;
                uint8_t* dst = f.owned_buffer.get();
                const uint8_t* mask = mask_pixels;

                int mw = c->width, mh = c->height;
                for (int mip = 0; mip < c->num_levels; ++mip) {
                    const int n = mw * mh;
                    if (effective == c->format) {
                        std::memcpy(dst, src, bm_calculate_total_bytes(mw, mh, effective));
                    }
                    else {
                        rf::bm::convert_format(dst, effective, src, c->format, n);
                    }
                    if (mask) {
                        overlay_alpha(dst, effective, mask, n);
                    }
                    src += bm_calculate_total_bytes(mw, mh, c->format);
                    dst += bm_calculate_total_bytes(mw, mh, effective);
                    if (mask) mask += bm_calculate_total_bytes(mw, mh, mask_fmt);
                    mw = std::max(mw / 2, 1);
                    mh = std::max(mh / 2, 1);
                }

                // Source pixels are no longer needed — unlock and drop our ref. Other consumers
                // (direct references, other ATXes) keep the source alive via their own refs.
                rf::bm::unlock(f.bm_handle);
                rf::bm::release(f.bm_handle);
                f.bm_handle = -1;
                f.locked_pixels = f.owned_buffer.get();
                f.locked_palette = nullptr;
            }

            if (mask_handle >= 0) {
                rf::bm::unlock(mask_handle);
                rf::bm::release(mask_handle);
            }

            c->format = effective;
        }

        c->playing = (c->animation_mode != AtxAnimationMode::Static) && c->initially_on;
        c->current_frame = 0;
        c->direction = 1;
        c->time_in_frame_s = 0.0f;
        return c;
    }
} // namespace

rf::bm::Type read_atx_header(const char* atx_filename, int* width_out, int* height_out,
    rf::bm::Format* format_out, int* num_levels_out, int* num_frames_out)
{
    const std::string key = handle_from_filename(atx_filename);
    if (g_failed.contains(key)) {
        return rf::bm::TYPE_NONE;
    }
    AtxController* controller = nullptr;
    if (auto it = g_controllers.find(key); it != g_controllers.end()) {
        controller = it->second.get();
    }
    else {
        auto c = parse_and_load(atx_filename);
        if (!c) {
            g_failed.insert(key);
            return rf::bm::TYPE_NONE;
        }
        controller = c.get();
        g_controllers.emplace(key, std::move(c));
    }

    *width_out = controller->width;
    *height_out = controller->height;
    *format_out = controller->format;
    *num_levels_out = controller->num_levels;
    *num_frames_out = 1; // ATX bitmap surfaces a single GPU texture; frames are virtualized.
    return rf::bm::TYPE_ATX;
}

bool atx_is_loading_child()
{
    return g_loading_child;
}

rf::bm::Format lock_atx_bitmap(rf::bm::BitmapEntry& bm_entry, void** pixels_out, void** palette_out)
{
    *pixels_out = nullptr;
    *palette_out = nullptr;

    auto it = g_controllers.find(key_from_bm_name(bm_entry.name));
    if (it == g_controllers.end()) {
        xlog::warn("ATX: lock for unknown '{}'", bm_entry.name);
        return rf::bm::FORMAT_NONE;
    }
    AtxController& c = *it->second;
    c.atx_bm_handle = bm_entry.handle;

    int idx = c.current_frame;
    if (idx < 0 || idx >= static_cast<int>(c.frames.size())) {
        idx = 0;
    }
    *pixels_out = c.frames[idx].locked_pixels;
    *palette_out = c.frames[idx].locked_palette;
    return bm_entry.format;
}

void unlock_atx_bitmap(rf::bm::BitmapEntry& /*bm_entry*/)
{
    // No-op: children remain locked for the controller's lifetime so frame swaps are O(1).
}

std::optional<uint8_t> atx_material_override(const rf::bm::BitmapEntry& bm_entry)
{
    auto it = g_controllers.find(key_from_bm_name(bm_entry.name));
    if (it == g_controllers.end()) {
        return std::nullopt;
    }
    const AtxController& c = *it->second;
    if (!c.has_material_override) {
        return std::nullopt;
    }
    int idx = c.current_frame;
    if (idx >= 0 && idx < static_cast<int>(c.frames.size())) {
        if (auto& over = c.frames[idx].material_override) {
            return *over;
        }
    }
    return c.header_material; // may be nullopt if only some frames override and this isn't one
}

void atx_free(rf::bm::BitmapEntry& bm_entry)
{
    auto it = g_controllers.find(key_from_bm_name(bm_entry.name));
    if (it == g_controllers.end()) {
        return;
    }
    release_children(*it->second);
    g_controllers.erase(it);
}

void atx_do_frame()
{
    if (g_controllers.empty()) return;
    const float dt_s = rf::frametime;
    if (dt_s <= 0.0f) return;

    for (auto& [key, cptr] : g_controllers) {
        AtxController& c = *cptr;
        if (c.animation_mode == AtxAnimationMode::Static) continue;
        if (!c.playing) continue;
        if (c.frames.size() < 2) continue;

        c.time_in_frame_s += dt_s;
        const int prev = c.current_frame;
        const int n = static_cast<int>(c.frames.size());

        float ft_s = frame_time_for(c, c.current_frame) / 1000.0f;
        // Guard against a malicious 0 — frame_time_for already clamps to >=1ms via parse-time validation,
        // but defensively skip if it ever drops to 0 to avoid an infinite loop.
        if (ft_s <= 0.0f) continue;

        bool stop_advancing = false;
        while (c.time_in_frame_s >= ft_s) {
            c.time_in_frame_s -= ft_s;
            switch (c.animation_mode) {
                case AtxAnimationMode::Loop:
                    c.current_frame = (c.current_frame + 1) % n;
                    break;
                case AtxAnimationMode::PingPong:
                    if (n == 1) break;
                    c.current_frame += c.direction;
                    if (c.current_frame >= n - 1) {
                        c.current_frame = n - 1;
                        c.direction = -1;
                    }
                    else if (c.current_frame <= 0) {
                        c.current_frame = 0;
                        c.direction = 1;
                    }
                    break;
                case AtxAnimationMode::PlayOnce:
                    if (c.current_frame < n - 1) {
                        ++c.current_frame;
                    }
                    else {
                        // Reached and held the last frame; stop the per-frame timer.
                        c.playing = false;
                        c.time_in_frame_s = 0.0f;
                        stop_advancing = true;
                    }
                    break;
                case AtxAnimationMode::Static:
                    break;
            }
            if (stop_advancing) break;
            ft_s = frame_time_for(c, c.current_frame) / 1000.0f;
            if (ft_s <= 0.0f) break;
        }

        if (c.current_frame != prev) {
            dirty_atx(c);
        }
    }
}

void atx_level_reset()
{
    for (auto& [key, cptr] : g_controllers) {
        release_children(*cptr);
    }
    g_controllers.clear();
    g_failed.clear();
}

bool atx_set_frame(const std::string& handle, int frame_index)
{
    AtxController* c = get_by_handle(handle);
    if (!c) {
        rf::bm::load((to_lower(handle) + ".atx").c_str(), -1, true);
        c = get_by_handle(handle);
        if (!c) return false;
    }
    const int n = static_cast<int>(c->frames.size());
    if (frame_index < 0 || frame_index >= n) {
        xlog::warn("ATX '{}': set_frame {} out of range [0,{})", handle, frame_index, n);
        return false;
    }
    if (c->current_frame != frame_index) {
        c->current_frame = frame_index;
        c->time_in_frame_s = 0.0f;
        dirty_atx(*c);
    }
    return true;
}

bool atx_play(const std::string& handle)
{
    AtxController* c = get_by_handle(handle);
    if (!c) {
        rf::bm::load((to_lower(handle) + ".atx").c_str(), -1, true);
        c = get_by_handle(handle);
        if (!c) return false;
    }
    c->playing = true;
    return true;
}

bool atx_pause(const std::string& handle)
{
    AtxController* c = get_by_handle(handle);
    if (!c) return false;
    c->playing = false;
    return true;
}

bool atx_set_frame_time(const std::string& handle, int frame_time_ms)
{
    AtxController* c = get_by_handle(handle);
    if (!c) {
        rf::bm::load((to_lower(handle) + ".atx").c_str(), -1, true);
        c = get_by_handle(handle);
        if (!c) return false;
    }
    c->base_frame_time_ms = std::max(1, frame_time_ms);
    return true;
}
