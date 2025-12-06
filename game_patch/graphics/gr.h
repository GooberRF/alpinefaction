#pragma once

#include "../rf/bmpman.h"
#include "../rf/gr/gr.h"

void gr_apply_patch();
void evaluate_lightmaps_only();
int gr_font_get_default();
void gr_font_set_default(int font_id);
bool gr_set_render_target(int bm_handle);
bool gr_is_texture_format_supported(rf::bm::Format format);
void gr_bitmap_scaled_float(int bitmap_handle, float x, float y, float w, float h, float sx, float sy, float sw, float sh, bool flip_x, bool flip_y, rf::gr::Mode mode);
float gr_scale_fov_hor_plus(float horizontal_fov);
bool gr_3d_bitmap_oriented_wh(const rf::Vector3* pnt, const rf::Matrix3* M, float half_w, float half_h, rf::gr::Mode mode);
int gr_fit_string(std::string& text, int max_width, int font_id = -1, std::string_view suffix = "-");

inline constexpr rf::gr::Mode overdraw_colorized_3d_bitmap{
        rf::gr::TEXTURE_SOURCE_WRAP,
        rf::gr::COLOR_SOURCE_VERTEX_PLUS_TEXTURE,
        rf::gr::ALPHA_SOURCE_TEXTURE,
        rf::gr::ALPHA_BLEND_ALPHA,
        rf::gr::ZBUFFER_TYPE_NONE,
        rf::gr::FOG_NOT_ALLOWED
    };

template<typename F>
void gr_font_run_with_default(int font_id, F fun)
{
    int old_font = gr_font_get_default();
    gr_font_set_default(font_id);
    fun();
    gr_font_set_default(old_font);
}
