#pragma once

#include <cstdint>
#include <vector>
#include "../../rf/gr/gr.h"

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
    // GPU weather: one record of fixed randoms per particle, shared by every region's segment
    struct WeatherSeed
    {
        // Position in the unit box
        float x;
        float y;
        float z;
        float phase;
        float angle;
        float spin;
        float drift_x;
        float drift_z;
    };

    // One ceiling cache cell as the GPU reads it: the world column the slot holds and the floor under it
    struct WeatherColumn
    {
        std::int32_t cx;
        std::int32_t cz;
        float floor_y;
        std::int32_t pad = 0;
    };

    // One weather region's particles for one frame. The GPU places each one from its seed, so the box
    // carries the uniform motion folded in: box_shift is the box-relative offset the lattice has moved
    // by, wrap_base the whole vertical laps it has made.
    struct WeatherDraw
    {
        int count = 0;
        bool snow = false;
        rf::gr::Mode mode;
        int bm_handle = -1;
        rf::Color color{255, 255, 255, 255};
        rf::Vector3 box_min;
        rf::Vector3 box_size;
        rf::Vector3 box_shift;
        std::uint32_t wrap_base = 0;
        std::uint32_t salt = 0;
        float time = 0.0f;
        rf::Vector3 camera_pos;
        rf::Matrix3 camera_orient;
        // Rain streak half width, or snow sprite radius
        float half_width = 0.0f;
        rf::Vector3 fall_dir;
        float streak_half_len = 0.0f;
        float sway_amplitude = 0.0f;
        float sway_speed = 0.0f;
        float sway_phase = 0.0f;
        bool sphere = false;
        rf::Vector3 region_center;
        rf::Matrix3 region_orient;
        rf::Vector3 region_half_extents;
        float region_radius = 0.0f;
        bool has_surface = false;
        float surface_y = 0.0f;
        // Ceiling cache uploaded through update_weather_columns, -1 when the region has none
        int columns_slot = -1;
        float column_cell = 0.0f;
        int cols_x = 0;
        int cols_z = 0;
        // A multiple of the grid size at or below every column the box covers
        std::int32_t column_origin_x = 0;
        std::int32_t column_origin_z = 0;
    };

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
