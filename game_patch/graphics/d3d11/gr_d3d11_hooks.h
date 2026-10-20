#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "../../rf/gr/gr.h"
#include <common/gr_d3d11/gr_d3d11_weather.h>

namespace rf
{
    struct GRoom;
    struct Particle;
    struct ParticleEmitter;
}

namespace alpine_lightmap
{
    struct ReadResult;
}

namespace gr::d3d11 {
    // Cache the level's mesh lighting settings for the core (gr_d3d11_mesh.h) at level load
    void evaluate_mesh_lighting(const std::string& level_filename);
    void evaluate_pixel_light_overbright(const std::string& level_filename);
    void evaluate_alpha_test_threshold(const std::string& level_filename);
    bool set_render_target(int bm_handle);
    void invalidate_texture_cache();
    int render_target_generation();
    void update_window_mode();
    void bitmap_float(int bitmap_handle, float x, float y, float w, float h,
                      float sx, float sy, float sw, float sh,
                      bool flip_x, bool flip_y, rf::gr::Mode mode);
    void poly_2d(int bitmap_handle, int nv, const rf::gr::Vertex* vertices, rf::gr::Mode mode);
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
    bool render_particle_emitter(rf::ParticleEmitter* emitter, float interp_dt, int* out_drawn, int* out_draws);
    bool batched_billboard(const rf::Vector3& pos, float angle, float radius, rf::gr::Mode mode);
    // Batches a particle drawn by its own room render item; false sends it down the stock path.
    bool batched_particle(const rf::Particle& particle);
    void flush_batched_quads();
    bool upload_weather_seeds(const std::vector<WeatherSeed>& seeds);
    // Writes cells [first, first + cells.size()) of a region's ceiling cache. A cache of a different
    // size is only replaced by a full upload. False when the GPU copy could not be written.
    bool update_weather_columns(int slot, int num_cells, int first, const std::vector<WeatherColumn>& cells);
    void release_weather_columns();
    // Draws one region's weather in place; false when it has to go through the CPU path instead.
    bool draw_weather(const WeatherDraw& draw);
}
