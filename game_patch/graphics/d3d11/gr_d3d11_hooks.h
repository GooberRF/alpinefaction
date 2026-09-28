#pragma once

#include <cstdint>
#include <vector>
#include "../../rf/gr/gr.h"

namespace rf
{
    struct GRoom;
}

namespace alpine_lightmap
{
    struct ReadResult;
}

namespace gr::d3d11 {
    bool set_render_target(int bm_handle);
    void invalidate_texture_cache();
    int render_target_generation();
    void update_window_mode();
    void bitmap_float(int bitmap_handle, float x, float y, float w, float h,
                      float sx, float sy, float sw, float sh,
                      bool flip_x, bool flip_y, rf::gr::Mode mode);
    void update_texture_filtering();
    void texture_flush_non_user_cache();
    void set_pow2_tex_active(bool active);
    bool is_antialiasing_err();
    bool supports_sample_count(uint32_t sample_count);
    void flush_frame_buffers();
    void flush_outlines_before_fpgun();
    bool trigger_damage_vignette(unsigned dir_mask);
    // Frees a carved detail room's render cache; the room builds a new one when next drawn.
    void release_detail_room_render_cache(rf::GRoom* room);
    // Frees every terrain's textures, which the next render cache build creates again.
    void release_terrain_gpu();
    // Replaces the Alpine Lightmaps atlas; false, with none held, when there is no renderer or the
    // upload fails.
    bool upload_af_lightmap_atlas(const alpine_lightmap::ReadResult& section, const std::vector<std::uint8_t>& blocks);
    void release_af_lightmap_atlas();
    bool af_lightmap_atlas_live();
}
