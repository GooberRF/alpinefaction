#pragma once

#include <optional>

namespace rf
{
    struct Vector3;
}

struct MinimapRect
{
    int x;
    int y;
    int size;
};

// The corner panel's reserved screen rect, else nullopt. It depends only on stable state (level,
// cl_minimap, HUD shown), so layout below it does not move while an overlay or the big map hides the panel.
std::optional<MinimapRect> minimap_panel_rect();
void minimap_render();
void minimap_notify_geomod(const rf::Vector3& pos, float radius);
void minimap_level_init_post();
void minimap_level_reset();
void minimap_apply_patches();
