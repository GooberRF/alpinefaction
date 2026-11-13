#pragma once

#include <cstdint>
#include <gr_d3d11_engine.h>

namespace gr::d3d11
{
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
        int32_t cx;
        int32_t cz;
        float floor_y;
        int32_t pad = 0;
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
        uint32_t wrap_base = 0;
        uint32_t salt = 0;
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
        int32_t column_origin_x = 0;
        int32_t column_origin_z = 0;
    };
}
