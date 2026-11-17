#pragma once

#include <cstdint>
#include <optional>
#include "../rf/gr/gr.h"
#include "../rf/math/vector.h"

inline constexpr float world_medallion_radius = 0.6f;

struct WorldMedallionRing
{
    float progress = 0.0f; // 0-1, clockwise from the top
    rf::Color track_color{};
    rf::Color fill_color{};
};

// The ring's outer edge, from the medallion's centre
float world_medallion_ring_reach();
// A disc of `face_bm` spinning about the vertical through `center`, its size times `scale`. A ring, if given, is
// notched in eighths and faces the camera, so it reads at any point in the spin.
void world_medallion_draw(const rf::Vector3& center, float scale, int face_bm, const rf::Color& color, int64_t now_ms,
                          bool through_walls, const std::optional<WorldMedallionRing>& ring = std::nullopt);
