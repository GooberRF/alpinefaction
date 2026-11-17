#pragma once

// Level camera far clip limits, shared by the game runtime and the editor.

#include <algorithm>
#include <cmath>

namespace alpine_camera_far_clip
{

inline constexpr float max_value = 100000.0f;

// gr_set_far_clip (0x00518060) turns the far clip off at 1 or less, so such a value means none (0)
inline float sanitize(float far_clip)
{
    return std::isfinite(far_clip) && far_clip > 1.0f ? std::min(far_clip, max_value) : 0.0f;
}

} // namespace alpine_camera_far_clip
