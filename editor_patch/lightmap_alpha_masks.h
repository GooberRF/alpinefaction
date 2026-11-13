#pragma once

#include <cstdint>
#include <vector>

// A texture's alpha channel and its box filtered mips, for the bake's alpha-tested occluders.
struct LightmapAlphaMask
{
    struct Level
    {
        int w = 1;
        int h = 1;
        std::vector<std::uint8_t> a;
    };
    std::vector<Level> levels;

    // A triangle's texture area within one repeat, in [0, 1]: what an atlas part may sample.
    struct Rect
    {
        float u0, v0, u1, v1;
    };

    // Opacity in [0, 1] averaged over a square around (u, v), in texture repeats, exp2(lod) levels[0] texels
    // wide. Taps read only texels inside `rect`, or wrap without one.
    float coverage(float u, float v, float lod, const Rect* rect) const;
};

// The mask of a texture with an alpha channel, decoded on first use; nullptr for a texture without one or
// one that cannot be read. Main thread only. The masks live until lightmap_alpha_masks_release.
const LightmapAlphaMask* lightmap_alpha_mask(int bm_handle);
void lightmap_alpha_masks_release();
