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
#include <common/bitmap/formats.h>
#include <common/atx/parse.h>
#include <common/utils/string-utils.h>

#include "atx.h"
#include "bmpman.h"
#include "../graphics/gr.h"
#include "../rf/bmpman.h"
#include "../rf/file/file.h"
#include "../rf/gr/gr.h"
#include "../rf/os/frametime.h"

namespace
{
    // Alias to the shared schema enum so editor and game can't drift on values.
    using AtxAnimationMode = AtxSpec::AnimationMode;

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

    // Hot-path lookup: bm_handle → controller. Populated lazily on first lock/material query so
    // bm_get_material_idx_hook (called for every footstep/impact/decal — many per frame) can
    // avoid a `to_lower` heap allocation + name-keyed map lookup per call. Cleared on
    // atx_level_reset and on per-entry atx_free.
    std::unordered_map<int, AtxController*> g_by_handle;

    bool g_loading_child = false;
    struct ChildLoadGuard {
        ChildLoadGuard() { g_loading_child = true; }
        ~ChildLoadGuard() { g_loading_child = false; }
    };

    // String helpers come from common/utils/string-utils.h (string_to_lower, string_iends_with).

    std::string handle_from_filename(const char* filename)
    {
        std::string s = filename;
        auto last_slash = s.find_last_of("/\\");
        if (last_slash != std::string::npos) s = s.substr(last_slash + 1);
        auto dot = s.find_last_of('.');
        if (dot != std::string::npos) s.resize(dot);
        return string_to_lower(s);
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
        auto it = g_controllers.find(string_to_lower(handle));
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
        s = string_to_lower(s);
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

    // Total bytes for a full mip chain at (w, h) in `fmt` over `num_levels`. Stays game-side
    // because bm_calculate_total_bytes is a game_patch helper not yet promoted to common.
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

    std::unique_ptr<AtxController> parse_and_load(const char* filename)
    {
        std::string content;
        if (!read_atx_text(filename, content)) {
            xlog::warn("ATX: failed to open '{}'", filename);
            return nullptr;
        }

        // Parse the TOML and validate format-internal constraints via the shared parser.
        // Engine-specific concerns (material name → material index, format token → bm::Format,
        // and child texture loading) are still handled below.
        auto spec = parse_atx(content, filename);
        if (!spec) {
            return nullptr; // parser already logged the cause
        }

        auto c = std::make_unique<AtxController>();
        c->handle_str = handle_from_filename(filename);
        c->base_frame_time_ms = spec->header.frame_time_ms;
        c->initially_on = spec->header.initially_on;
        c->animation_mode = spec->header.animation_mode;

        std::optional<rf::bm::Format> target_format;
        std::optional<std::string> alpha_mask_filename = spec->header.alpha_mask;

        if (spec->header.format) {
            if (auto fmt = atx_parse_format_token(*spec->header.format)) {
                target_format = static_cast<rf::bm::Format>(*fmt);
            }
            else {
                xlog::error("ATX '{}': unrecognized format '{}'", filename, *spec->header.format);
                return nullptr;
            }
        }
        if (spec->header.material) {
            auto mat = parse_material_name(*spec->header.material);
            if (!mat) {
                xlog::error("ATX '{}': unrecognized material '{}' in [header]",
                            filename, *spec->header.material);
                return nullptr;
            }
            c->header_material = mat;
            c->has_material_override = true;
        }

        c->frames.reserve(spec->frames.size());
        for (auto& src : spec->frames) {
            AtxFrame f;
            f.filename = std::move(src.filename);
            if (src.frame_time_ms) {
                f.frame_time_ms = *src.frame_time_ms;
            }
            if (src.material) {
                auto mat = parse_material_name(*src.material);
                if (!mat) {
                    xlog::error("ATX '{}': unrecognized material '{}' in [[frame]]",
                                filename, *src.material);
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
                effective = static_cast<rf::bm::Format>(bm_promote_to_alpha(effective));
            }

            if (!bm_format_is_uncompressed_rgb(c->format)) {
                xlog::error("ATX '{}': source format {} cannot be transformed (uncompressed RGB/RGBA only)",
                    filename, static_cast<int>(c->format));
                unlock_children(*c);
                return nullptr;
            }
            if (!bm_format_is_uncompressed_rgb(effective)) {
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
                        bm_overlay_alpha_mask(dst, effective, mask, n);
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
    if (c.atx_bm_handle != bm_entry.handle) {
        c.atx_bm_handle = bm_entry.handle;
        // Populate the hot-path lookup so bm_get_material_idx_hook can find the controller in
        // O(1) by handle without re-hashing the entry name.
        g_by_handle[bm_entry.handle] = &c;
    }

    int idx = c.current_frame;
    if (idx < 0 || idx >= static_cast<int>(c.frames.size())) {
        idx = 0;
    }
    auto* px = c.frames[idx].locked_pixels;
    if (!px) {
        // Shouldn't happen — parse_and_load locks every frame and only succeeds if every lock
        // succeeds. Defensive: report FORMAT_NONE so the caller doesn't dereference nullptr.
        return rf::bm::FORMAT_NONE;
    }
    *pixels_out = px;
    *palette_out = c.frames[idx].locked_palette;
    return bm_entry.format;
}

void unlock_atx_bitmap(rf::bm::BitmapEntry& /*bm_entry*/)
{
    // No-op: children remain locked for the controller's lifetime so frame swaps are O(1).
}

std::optional<uint8_t> atx_material_override(const rf::bm::BitmapEntry& bm_entry)
{
    // Hot path — called from bm_get_material_idx_hook for every footstep/bullet impact/decal.
    // Try the handle-keyed map first (no string allocation). Fall back to the name-keyed map
    // for the rare case where a material query happens before lock_atx_bitmap has populated
    // g_by_handle (e.g. very first query at level start).
    const AtxController* c = nullptr;
    if (auto it = g_by_handle.find(bm_entry.handle); it != g_by_handle.end()) {
        c = it->second;
    }
    else {
        auto it2 = g_controllers.find(key_from_bm_name(bm_entry.name));
        if (it2 == g_controllers.end()) return std::nullopt;
        c = it2->second.get();
    }
    if (!c->has_material_override) {
        return std::nullopt;
    }
    int idx = c->current_frame;
    if (idx >= 0 && idx < static_cast<int>(c->frames.size())) {
        if (auto& over = c->frames[idx].material_override) {
            return *over;
        }
    }
    return c->header_material; // may be nullopt if only some frames override and this isn't one
}

void atx_free(rf::bm::BitmapEntry& bm_entry)
{
    auto it = g_controllers.find(key_from_bm_name(bm_entry.name));
    if (it == g_controllers.end()) {
        return;
    }
    if (it->second->atx_bm_handle >= 0) {
        g_by_handle.erase(it->second->atx_bm_handle);
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

        // Cap inner-loop iterations to two full cycles. After a long stall (level transition,
        // alt-tab) `dt_s` could be huge, and a tight `frame_time_ms` would otherwise loop
        // thousands of times here. Two cycles is enough to land on the right frame for any
        // mode (Loop wraps once, PingPong needs at most 2*(n-1) advances to return to phase).
        // If we hit the cap, drop the leftover accumulated time so we don't carry the deficit.
        const int max_advances = std::max(2, n * 2);
        int advances = 0;
        bool stop_advancing = false;
        while (c.time_in_frame_s >= ft_s) {
            if (advances++ >= max_advances) {
                c.time_in_frame_s = 0.0f;
                break;
            }
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
    g_by_handle.clear();
    g_failed.clear();
}

bool atx_set_frame(const std::string& handle, int frame_index)
{
    AtxController* c = get_by_handle(handle);
    if (!c) {
        rf::bm::load((string_to_lower(handle) + ".atx").c_str(), -1, true);
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
        rf::bm::load((string_to_lower(handle) + ".atx").c_str(), -1, true);
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
        rf::bm::load((string_to_lower(handle) + ".atx").c_str(), -1, true);
        c = get_by_handle(handle);
        if (!c) return false;
    }
    c->base_frame_time_ms = std::max(1, frame_time_ms);
    return true;
}
