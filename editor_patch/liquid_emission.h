#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include "mfc_types.h"

struct CDedLevel;
struct GRoom;
struct GSolid;
namespace rf
{
    struct File;
}

// The light a Liquid Room room effect bakes from its liquid surface. RF.exe never reads room effects (it skips
// chunk 0xC00), so this is editor data, kept per object and saved in its own chunk.
enum class LiquidEmitMode : std::uint8_t
{
    off = 0,
    up = 1,          // lights what is above the surface
    up_and_down = 2, // and what is below it
};

struct LiquidEmission
{
    LiquidEmitMode mode = LiquidEmitMode::off;
    std::uint8_t color_r = 255;
    std::uint8_t color_g = 96;
    std::uint8_t color_b = 32;
    float intensity = 1.0f;
    float distance = 8.0f; // how far from the surface the light reaches, falling off linearly to it

    bool operator==(const LiquidEmission&) const = default;
};

inline constexpr float liquid_emission_max_intensity = 100.0f;
inline constexpr float liquid_emission_max_distance = 1000.0f;

LiquidEmission liquid_emission_of(const DedRoomEffect& effect);

// A level keys the records by uid. They are held between the load begin and end calls, as the level reader meets the
// chunk before the room effects.
void liquid_emission_serialize_chunk(CDedLevel& level, rf::File& file);
void liquid_emission_level_load_begin();
void liquid_emission_deserialize_chunk(rf::File& file, std::size_t chunk_len);
void liquid_emission_level_load_end(const CDedLevel& level);

// A .rfg keys the records by the order its room effects were written in, as stock renumbers colliding uids on
// import; the stock serializer reports each one it writes or reads between the begin call and the chunk.
void liquid_emission_group_save_begin();
void liquid_emission_group_serialize_chunk(rf::File& file);
void liquid_emission_group_load_begin();
void liquid_emission_group_deserialize_chunk(rf::File& file, std::size_t chunk_len);

void ApplyLiquidEmissionPatches();

// A liquid room's surface as the bake sees it: the room's horizontal liquid faces, all at one height.
class LiquidSurface
{
public:
    // False when the room has no horizontal liquid face, as before its first Build Geometry.
    bool build(const GSolid& solid, const GRoom& room);

    float height() const { return height_; }
    std::size_t polygon_count() const { return polygons_.size(); }
    float min_x() const { return min_x_; }
    float max_x() const { return max_x_; }
    float min_z() const { return min_z_; }
    float max_z() const { return max_z_; }

    bool contains(float x, float z) const;
    // The surface point nearest p, and its distance.
    Vector3 nearest_point(const Vector3& p) const;
    float distance(const Vector3& p) const;
    // The share of a diffuse receiver's hemisphere the unoccluded surface covers, cosine weighted (1 under an
    // endless surface); 0 for a receiver in the surface's plane.
    float form_factor(const Vector3& p, const Vector3& n) const;

private:
    struct Point
    {
        float x, z;
    };
    struct Edge
    {
        Point a, b;
    };

    float nearest_edge_distance_xz(float x, float z, Point* nearest) const;
    void build_grid();

    float height_ = 0.0f;
    float min_x_ = 0.0f, max_x_ = 0.0f, min_z_ = 0.0f, max_z_ = 0.0f;
    // Counter-clockwise in x, z; they tile the surface without overlapping.
    std::vector<std::vector<Point>> polygons_;
    // The polygons' edges with every shared one cancelled out: the outline, plus any T-junction leftovers inside it.
    std::vector<Edge> outline_;
    // +1 or -1: what turns the outline's circulation into a form factor seen from above.
    float winding_sign_ = 1.0f;
    // Polygons by grid cell over the bounds, for contains().
    int grid_ = 1;
    float cell_x_ = 1.0f, cell_z_ = 1.0f;
    std::vector<std::vector<int>> cells_;
};
