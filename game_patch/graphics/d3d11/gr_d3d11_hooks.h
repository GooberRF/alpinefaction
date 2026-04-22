#pragma once

#include "../../rf/gr/gr.h"

namespace gr::d3d11 {
    bool set_render_target(int bm_handle);
    void update_window_mode();
    void bitmap_float(int bitmap_handle, float x, float y, float w, float h,
                      float sx, float sy, float sw, float sh,
                      bool flip_x, bool flip_y, rf::gr::Mode mode);
    void update_texture_filtering();
    void texture_flush_non_user_cache();
    void set_pow2_tex_active(bool active);
}

