#pragma once

// ATX format core types — describes what an .atx file declares, independent of use.
// Runtime side (loading frames, dispatching locks, animating, etc.) lives per-consumer.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <common/bitmap/formats.h>      // BM_FORMAT_*
#include <common/utils/string-utils.h>  // string_to_lower

// ─── TOML schema constants ────────────────────────────────────────────────────

// [header] keys
inline constexpr std::string_view ATX_KEY_FRAME_TIME     = "frame_time";
inline constexpr std::string_view ATX_KEY_INITIALLY_ON   = "initially_on";
inline constexpr std::string_view ATX_KEY_ANIMATION_MODE = "animation_mode";
inline constexpr std::string_view ATX_KEY_FORMAT         = "format";
inline constexpr std::string_view ATX_KEY_ALPHA_MASK     = "alpha_mask";
inline constexpr std::string_view ATX_KEY_MATERIAL       = "material";

// [[frame]] entry keys
inline constexpr std::string_view ATX_KEY_FRAME_FILE     = "file";
inline constexpr std::string_view ATX_KEY_FRAME_TIME_OVR = "frame_time"; // per-frame override
inline constexpr std::string_view ATX_KEY_FRAME_MATERIAL = "material";   // per-frame override

// TOML structure
inline constexpr std::string_view ATX_TABLE_HEADER       = "header";
inline constexpr std::string_view ATX_ARRAY_FRAME        = "frame";

// ─── Default values & limits ──────────────────────────────────────────────────

inline constexpr int  ATX_MIN_FRAME_TIME_MS     = 1;   // floor enforced by parser
inline constexpr int  ATX_DEFAULT_FRAME_TIME_MS = 100;
inline constexpr bool ATX_DEFAULT_INITIALLY_ON  = true;

// ─── Parsed structures ────────────────────────────────────────────────────────

// All ATX format types are nested under AtxSpec to keep the global namespace clean and
// avoid collisions with other Frame/Header-named types in consumer code.
struct AtxSpec
{
    enum class AnimationMode : int
    {
        Static   = 0, // No anim playback. Frames change only manually (default).
        PingPong = 1, // Forward to last, then backward to first, repeating.
        Loop     = 2, // Forward; wrap to 0 after last, repeating.
        PlayOnce = 3, // Forward; stop and hold on the last frame.
    };

    struct Frame
    {
        std::string filename;                  // required; never empty after parse, never .atx
        std::optional<int> frame_time_ms;      // per-frame override
        std::optional<std::string> material;   // per-frame material override (string token)
    };

    struct Header
    {
        int frame_time_ms          = ATX_DEFAULT_FRAME_TIME_MS;
        bool initially_on          = ATX_DEFAULT_INITIALLY_ON;
        AnimationMode animation_mode = AnimationMode::Static;
        std::optional<std::string> format;     // target format token, e.g. "8888_argb"
        std::optional<std::string> alpha_mask; // alpha-mask filename
        std::optional<std::string> material;   // ATX-wide material override token
    };

    Header header;
    std::vector<Frame> frames; // always non-empty after a successful parse
};

// ─── ATX schema vocabulary parsers ────────────────────────────────────────────

// Maps an ATX [header].format string token like "8888_argb" or "565" to a BM_FORMAT_*
// integer, or std::nullopt if the token isn't a recognised ATX format name.
// The accepted token set is part of the ATX format specification, hence its home here.
inline std::optional<int> atx_parse_format_token(std::string_view token)
{
    std::string s = string_to_lower(token);
    if (s == "565"  || s == "565_rgb")    return BM_FORMAT_565_RGB;
    if (s == "4444" || s == "4444_argb")  return BM_FORMAT_4444_ARGB;
    if (s == "1555" || s == "1555_argb")  return BM_FORMAT_1555_ARGB;
    if (s == "888"  || s == "888_rgb")    return BM_FORMAT_888_RGB;
    if (s == "8888" || s == "8888_argb")  return BM_FORMAT_8888_ARGB;
    return std::nullopt;
}
