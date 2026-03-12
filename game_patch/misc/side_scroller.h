#pragma once

void side_scroller_do_patch();
void side_scroller_on_level_load();
bool is_side_scroller_mode();

// Called by hud_weapons to get reticle offset for side-scroller mode
bool side_scroller_get_reticle_offset(int& offset_x, int& offset_y);

// Side-scroller occlusion parameters for dithered transparency.
// Read by the D3D11 render pipeline to fade geometry between camera and player.
struct SsOcclusionParams {
    bool active = false;
    float player_x = 0.0f;
    float player_y = 0.0f;
    float player_z = 0.0f;
    float camera_x = 0.0f;
    float fade_strength = 0.0f; // 0.0 = fully opaque, 0.5 = 50% dithered
    float radius = 2.5f;        // cylinder radius in YZ plane
};
const SsOcclusionParams& get_ss_occlusion_params();
