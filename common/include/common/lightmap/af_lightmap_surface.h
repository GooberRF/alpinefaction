#pragma once

// Alpine Lightmaps: a geometry surface's place in the atlas, for the binaries' lightmap runtimes. The surfaces come
// from level files, so every field is checked before it addresses anything. Surface and Solid are the binary's
// GSurface and GSolid, with the engine's field names.

#include <cstdint>
#include "af_lightmap_runtime.h"
#include "alpine_lightmap.h"

namespace alpine_lightmap
{

// Whether surface `s` lies within a page x page lightmap page.
template<typename Surface>
bool surface_fits_page(const Surface& s, uint32_t page)
{
    const auto p = static_cast<int64_t>(page);
    return s.xstart >= 0 && s.ystart >= 0 && s.width >= 0 && s.height >= 0
        && static_cast<int64_t>(s.xstart) + s.width <= p
        && static_cast<int64_t>(s.ystart) + s.height <= p;
}

// Whether every stock page the surfaces of `solid` point at is edge x edge.
template<typename Solid>
bool stock_pages_are(const Solid* solid, uint32_t edge)
{
    for (int i = 0; solid && i < solid->surfaces.size(); i++) {
        const auto* s = solid->surfaces[i];
        if (s && s->lightmap
            && (static_cast<uint32_t>(s->lightmap->w) != edge
                || static_cast<uint32_t>(s->lightmap->h) != edge)) {
            return false;
        }
    }
    return true;
}

// The placement of the faces of surface `s` in chart record `chart`, refined by `k`. `bake_page` is the page edge
// the surface's uv_scale/uv_add normalize against, 0 for the page the surface points at. False leaves out zeroed.
template<typename Surface>
bool surface_chart_face(const Surface& s, uint32_t chart, const Chart& k, uint32_t bake_page,
                        AfLightmapFace& out)
{
    out = AfLightmapFace{};
    if (!s.lightmap) {
        return false;
    }
    if (s.u_coefficient < 0 || s.u_coefficient > 2 || s.v_coefficient < 0 || s.v_coefficient > 2
        || s.lightmap->w <= 0 || s.lightmap->h <= 0 || s.xstart < 0 || s.ystart < 0) {
        return false;
    }
    const uint32_t lm_w = bake_page ? bake_page : static_cast<uint32_t>(s.lightmap->w);
    const uint32_t lm_h = bake_page ? bake_page : static_cast<uint32_t>(s.lightmap->h);
    out.chart = static_cast<int>(chart);
    out.axis_u = s.u_coefficient;
    out.axis_v = s.v_coefficient;
    out.scale_u = s.uv_scale.x;
    out.scale_v = s.uv_scale.y;
    out.add_u = s.uv_add.x;
    out.add_v = s.uv_add.y;
    out.lm_w = lm_w;
    out.lm_h = lm_h;
    out.surf_x = static_cast<uint32_t>(s.xstart);
    out.surf_y = static_cast<uint32_t>(s.ystart);
    out.k_u = k.k_u;
    out.k_v = k.k_v;
    return true;
}

// The chart texel of `pos` (x, y, z) on a face placed by surface_chart_face.
inline ChartTexel face_chart_texel(const AfLightmapFace& face, const float* pos)
{
    return surface_chart_texel(pos, face.axis_u, face.axis_v, face.scale_u, face.scale_v, face.add_u, face.add_v,
                               face.lm_w, face.lm_h, face.surf_x, face.surf_y, face.k_u, face.k_v);
}

}
