#pragma once

#include "../rf/gr/gr.h"
#include "../rf/math/vector.h"
#include "../rf/math/matrix.h"

namespace rf
{
    struct VMesh;
}

namespace gr
{
    // D3D11 only; returns false (draws nothing) on other renderers. alpha_below/above in 0..1;
    // fill_y is world Y. Draws with depth write off, so call after opaque geometry.
    bool render_ghost_mesh(rf::VMesh* mesh, const rf::Vector3& pos, const rf::Matrix3& orient,
                           float alpha_below, float alpha_above, float fill_y, const rf::Color* tint);
}
