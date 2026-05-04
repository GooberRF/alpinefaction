#pragma once

// Pure ATX parser — takes TOML bytes, returns a structurally-validated AtxSpec, or nullopt.
//
// What this DOES validate:
//   - Valid TOML syntax (toml++ parses it)
//   - At least one [[frame]]
//   - Each frame has a non-empty `file` that doesn't end in .atx (no nesting)
//   - animation_mode is in range (0–3); out-of-range warns and falls back to default
//   - Numeric fields clamped to ATX_MIN_FRAME_TIME_MS
//
// What this does NOT do (caller's responsibility):
//   - File IO (caller supplies the bytes)
//   - Loading or decoding frame textures
//   - Cross-frame validation (dimensions/format/mip-count must match)
//   - Material name → engine material index lookup
//   - Format string → engine format enum lookup
//
// Header-only and engine-agnostic. Each consumer just needs toml++ on the include path.

#include <optional>
#include <string>
#include <string_view>

#include <toml++/toml.hpp>
#include <xlog/xlog.h>
#include <common/utils/string-utils.h>

#include "spec.h"

// Returns the parsed AtxSpec on success, or nullopt on TOML parse error or schema violation.
// `source_name` is included in log messages to identify the file in question (typically
// the .atx filename).
inline std::optional<AtxSpec> parse_atx(std::string_view toml_bytes, std::string_view source_name = {})
{
    toml::table tbl;
    try {
        tbl = toml::parse(toml_bytes, source_name);
    }
    catch (const toml::parse_error& e) {
        xlog::warn("ATX: parse error in '{}': {}", source_name, e.description());
        return std::nullopt;
    }
    catch (...) {
        xlog::warn("ATX: unexpected exception while parsing '{}'", source_name);
        return std::nullopt;
    }

    AtxSpec spec;

    // [header] is optional — defaults apply if absent.
    if (const auto* hdr = tbl.get_as<toml::table>(ATX_TABLE_HEADER)) {
        if (auto v = (*hdr)[ATX_KEY_FRAME_TIME].value<int64_t>()) {
            spec.header.frame_time_ms = std::max<int>(ATX_MIN_FRAME_TIME_MS, static_cast<int>(*v));
        }
        if (auto v = (*hdr)[ATX_KEY_INITIALLY_ON].value<bool>()) {
            spec.header.initially_on = *v;
        }
        if (auto v = (*hdr)[ATX_KEY_ANIMATION_MODE].value<int64_t>()) {
            int mode = static_cast<int>(*v);
            if (mode < 0 || mode > 3) {
                xlog::warn("ATX '{}': animation_mode {} out of range, defaulting to static",
                           source_name, mode);
                mode = static_cast<int>(AtxSpec::AnimationMode::Static);
            }
            spec.header.animation_mode = static_cast<AtxSpec::AnimationMode>(mode);
        }
        if (auto v = (*hdr)[ATX_KEY_FORMAT].value<std::string>()) {
            if (!v->empty()) spec.header.format = *v;
        }
        if (auto v = (*hdr)[ATX_KEY_ALPHA_MASK].value<std::string>()) {
            if (!v->empty()) spec.header.alpha_mask = *v;
        }
        if (auto v = (*hdr)[ATX_KEY_MATERIAL].value<std::string>()) {
            if (!v->empty()) spec.header.material = *v;
        }
    }

    const auto* frames_arr = tbl.get_as<toml::array>(ATX_ARRAY_FRAME);
    if (!frames_arr || frames_arr->empty()) {
        xlog::warn("ATX '{}': no [[frame]] entries", source_name);
        return std::nullopt;
    }

    spec.frames.reserve(frames_arr->size());
    for (auto&& node : *frames_arr) {
        const auto* frame_tbl = node.as_table();
        if (!frame_tbl) {
            xlog::warn("ATX '{}': non-table frame entry", source_name);
            return std::nullopt;
        }
        AtxSpec::Frame f;
        if (auto v = (*frame_tbl)[ATX_KEY_FRAME_FILE].value<std::string>()) {
            f.filename = *v;
        }
        if (f.filename.empty()) {
            xlog::warn("ATX '{}': frame missing 'file'", source_name);
            return std::nullopt;
        }
        if (string_iends_with(f.filename, ".atx")) {
            xlog::warn("ATX '{}': nested .atx is not allowed (frame '{}')",
                       source_name, f.filename);
            return std::nullopt;
        }
        if (auto v = (*frame_tbl)[ATX_KEY_FRAME_TIME_OVR].value<int64_t>()) {
            f.frame_time_ms = std::max<int>(ATX_MIN_FRAME_TIME_MS, static_cast<int>(*v));
        }
        if (auto v = (*frame_tbl)[ATX_KEY_FRAME_MATERIAL].value<std::string>()) {
            if (!v->empty()) f.material = *v;
        }
        spec.frames.push_back(std::move(f));
    }

    return spec;
}
