#include <vector>
#include "ribbon_trail.h"
#include "gr.h"

bool ribbon_trail_view(const rf::Vector3& anchor, const RibbonTrailStyle& style, float& fade, float& half_width)
{
    const float d = (anchor - rf::gr::eye_pos).len();
    if (d >= level_projection_far()) {
        return false;
    }
    fade = gr_fog_fade(d);
    if (!(fade > 0.0f)) {
        return false;
    }
    half_width = gr_floored_half_width(gr_pixels_per_meter(anchor), style.min_half_width, style.min_half_px);
    return true;
}

void ribbon_trail_stroke(int n, const rf::Vector3* pos, const float* alpha, float half_width, float band,
                         const rf::gr::Color& color)
{
    static std::vector<rf::Vector3> side;
    side.resize(n);
    gr_camera_facing_sides(n, pos, half_width, side.data());
    gr_band_strip(n, pos, side.data(), alpha, band, color, additive_vertex_color_3d);
}
