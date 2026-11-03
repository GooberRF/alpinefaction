#pragma once

#include "d3d11/engine/rf_gr.h"

// What Alpine editor code needs from the renderer beyond RED's public gr functions, on either renderer.

// The session's renderer is Direct3D 11 (editor_session_renderer).
bool editor_gr_is_d3d11();

// The x87 precision the lightmap bake runs at on the calling thread (its work pool copies it): single, as Direct3D 8
// and 9 set it on the thread that created their device. Direct3D 11 never sets it, so it is set here.
void editor_gr_set_bake_precision();

// Draws whatever the renderer holds queued, so that what follows is drawn after it.
void editor_gr_flush();

// Whether primitives are queued and not yet drawn, as they are inside a view's frame.
bool editor_gr_batch_open();

// `mode` blended additively (source alpha onto the target) without depth writes. Direct3D 9 leaves `mode` as it is:
// flush_additive (alpine_obj.h) blends its batch additively through the device instead.
rf::gr::Mode editor_gr_additive_mode(rf::gr::Mode mode);

// How the paint in progress draws the level: the Direct3D 11 perspective view draws only its edges with View > Show
// Wireframe, its edges over the fills with Wireframe Overlay, and translucent portal faces with Draw See-thru Portal
// Faces. Every other paint fills it without edges.
struct EditorLevelStyle
{
    bool fills = true;
    bool edges = false;
    bool portal_planes = false;
};
EditorLevelStyle editor_gr_level_style();

// Makes user bitmap `bm`'s texture, which gr_lock then writes; false when it cannot be made.
bool editor_texture_prepare(int bm);

// Before bm_release(bm): drops the bitmap's Direct3D 9 texture, which RED's bm_release leaves behind. Direct3D 11
// drops it in bm_release itself.
void editor_texture_free(int bm);
