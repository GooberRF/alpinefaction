#include <algorithm>
#include <cmath>
#include "ui_draw.h"
#include "d3d11/gr_d3d11_hooks.h"
#include <common/gr_d3d11/gr_d3d11_dynamic_geometry.h>
#include "../misc/alpine_settings.h"

namespace
{
    constexpr rf::gr::Mode ui_sdf_mode{
        rf::gr::TEXTURE_SOURCE_NONE,
        rf::gr::COLOR_SOURCE_VERTEX,
        rf::gr::ALPHA_SOURCE_VERTEX,
        rf::gr::ALPHA_BLEND_ALPHA,
        rf::gr::ZBUFFER_TYPE_NONE,
        rf::gr::FOG_NOT_ALLOWED,
    };

    // Slack around the shape so the smoothed edge is never clipped by the quad.
    constexpr float sdf_padding = 2.0f;

    int round_to_int(float v)
    {
        return static_cast<int>(std::lround(v));
    }

    void sdf_rect(float x, float y, float w, float h, float radius, float thickness, rf::Color color)
    {
        const float half_w = w * 0.5f;
        const float half_h = h * 0.5f;
        const float cx = x + half_w;
        const float cy = y + half_h;
        const float qw = half_w + sdf_padding;
        const float qh = half_h + sdf_padding;
        const float local_x[4] = {-qw, qw, qw, -qw};
        const float local_y[4] = {-qh, -qh, qh, qh};

        gr::d3d11::UiSdfDraw draw{};
        for (int i = 0; i < 4; ++i) {
            draw.pos_x[i] = cx + local_x[i];
            draw.pos_y[i] = cy + local_y[i];
            draw.uv_x[i] = local_x[i];
            draw.uv_y[i] = local_y[i];
        }
        draw.params[0] = half_w;
        draw.params[1] = half_h;
        draw.params[2] = std::clamp(radius, 0.0f, std::min(half_w, half_h));
        draw.params[3] = thickness;
        draw.type = gr::d3d11::UiSdfShape::rounded_rect;

        rf::gr::set_color(color);
        gr::d3d11::ui_sdf(draw, ui_sdf_mode);
    }

    void legacy_rect(float x, float y, float w, float h, rf::Color color)
    {
        if (w <= 0.0f || h <= 0.0f) {
            return;
        }
        rf::gr::set_color(color);
        rf::gr::rect(round_to_int(x), round_to_int(y), round_to_int(w), round_to_int(h));
    }
}

void ui_rounded_rect(float x, float y, float w, float h, float radius, rf::Color color)
{
    if (w <= 0.0f || h <= 0.0f) {
        return;
    }
    if (is_d3d11()) {
        sdf_rect(x, y, w, h, radius, 0.0f, color);
    }
    else {
        legacy_rect(x, y, w, h, color);
    }
}

void ui_rounded_rect_border(float x, float y, float w, float h, float radius, float thickness, rf::Color color)
{
    if (w <= 0.0f || h <= 0.0f || thickness <= 0.0f) {
        return;
    }
    if (is_d3d11()) {
        sdf_rect(x, y, w, h, radius, std::min(thickness, std::min(w, h) * 0.5f), color);
    }
    else {
        const float t = std::min(thickness, std::min(w, h) * 0.5f);
        legacy_rect(x, y, w, t, color);
        legacy_rect(x, y + h - t, w, t, color);
        legacy_rect(x, y + t, t, h - 2.0f * t, color);
        legacy_rect(x + w - t, y + t, t, h - 2.0f * t, color);
    }
}

void ui_aa_line(float x0, float y0, float x1, float y1, float width, rf::Color color)
{
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 0.001f || width <= 0.0f) {
        return;
    }

    if (is_d3d11()) {
        const float dir_x = dx / len;
        const float dir_y = dy / len;
        const float half_len = len * 0.5f;
        const float half_width = width * 0.5f;
        const float cx = (x0 + x1) * 0.5f;
        const float cy = (y0 + y1) * 0.5f;
        const float qh = half_len + half_width + sdf_padding;
        const float qw = half_width + sdf_padding;
        const float local_x[4] = {-qh, qh, qh, -qh};
        const float local_y[4] = {-qw, -qw, qw, qw};

        gr::d3d11::UiSdfDraw draw{};
        for (int i = 0; i < 4; ++i) {
            draw.pos_x[i] = cx + dir_x * local_x[i] - dir_y * local_y[i];
            draw.pos_y[i] = cy + dir_y * local_x[i] + dir_x * local_y[i];
            draw.uv_x[i] = local_x[i];
            draw.uv_y[i] = local_y[i];
        }
        draw.params[0] = half_len;
        draw.params[1] = half_width;
        draw.type = gr::d3d11::UiSdfShape::capsule;

        rf::gr::set_color(color);
        gr::d3d11::ui_sdf(draw, ui_sdf_mode);
        return;
    }

    const float perp_x = -dy / len;
    const float perp_y = dx / len;
    const int num_lines = std::max(1, round_to_int(width));
    rf::gr::set_color(color);
    for (int i = 0; i < num_lines; ++i) {
        const float offset = static_cast<float>(i) - (num_lines - 1) * 0.5f;
        rf::gr::line(x0 + perp_x * offset, y0 + perp_y * offset, x1 + perp_x * offset,
                     y1 + perp_y * offset);
    }
}
