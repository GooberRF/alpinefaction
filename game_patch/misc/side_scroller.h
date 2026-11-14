#pragma once

void side_scroller_do_patch();
void side_scroller_on_level_load();
bool is_side_scroller_mode();

// Called by hud_weapons to get reticle offset for side-scroller mode
bool side_scroller_get_reticle_offset(int& offset_x, int& offset_y);

// Side-scroller occlusion parameters for dithered transparency.
// Read by the D3D11 render pipeline to fade geometry between camera and player.
static constexpr int max_ss_occlusion_entities = 16;
struct SsOcclusionParams {
    bool active = false;
    float fade_strength = 0.0f; // 0.0 = fully opaque, 0.5 = 50% dithered
    float radius = 2.5f;        // cylinder radius perpendicular to entity-camera axis
    int num_entities = 0;
    struct { float x, y, z; } entity_pos[max_ss_occlusion_entities];
    struct { float x, y, z; } camera_pos = {0.0f, 0.0f, 0.0f};
};
const SsOcclusionParams& get_ss_occlusion_params();
