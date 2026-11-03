#include <float.h>
#include "../editor_settings.h"
#include "../textures.h"
#include "../vtypes.h"
#include "d3d11/gr_d3d11_editor_hooks.h"
#include "editor_gr.h"

bool editor_gr_is_d3d11()
{
    return editor_session_renderer() == EditorRendererKind::d3d11;
}

void editor_gr_set_bake_precision()
{
    if (editor_gr_is_d3d11()) {
        _controlfp(_PC_24, _MCW_PC);
    }
}

void editor_gr_flush()
{
    if (editor_gr_is_d3d11()) {
        gr::d3d11::flush();
    }
    else if (gr_batch_open) {
        gr_flush_batch();
    }
}

bool editor_gr_batch_open()
{
    return editor_gr_is_d3d11() ? gr::d3d11::batch_pending() : gr_batch_open;
}

rf::gr::Mode editor_gr_additive_mode(rf::gr::Mode mode)
{
    if (!editor_gr_is_d3d11()) {
        return mode;
    }
    mode.set_alpha_blend(rf::gr::ALPHA_BLEND_ALPHA_ADDITIVE);
    switch (mode.get_zbuffer_type()) {
    case rf::gr::ZBUFFER_TYPE_WRITE:
        mode.set_zbuffer_type(rf::gr::ZBUFFER_TYPE_NONE);
        break;
    case rf::gr::ZBUFFER_TYPE_FULL:
    case rf::gr::ZBUFFER_TYPE_FULL_ALPHA_TEST:
        mode.set_zbuffer_type(rf::gr::ZBUFFER_TYPE_READ);
        break;
    default:
        break;
    }
    return mode;
}

EditorLevelStyle editor_gr_level_style()
{
    return editor_gr_is_d3d11() ? gr::d3d11::level_style() : EditorLevelStyle{};
}

bool editor_texture_prepare(int bm)
{
    // Direct3D 11 makes the texture in gr_lock
    if (editor_gr_is_d3d11()) {
        return bm >= 0;
    }
    // gr_lock's texture setup, done first so a failed CreateTexture never reaches gr_lock. A failure frees the slot,
    // so the bitmap is tried again next time.
    GrTextureSlot* slot = gr_texture_slot_of(bm);
    if (!slot) {
        return false;
    }
    const bool created = (slot->section_count > 0 && slot->bm_handle == bm) || gr_texture_create(bm, slot);
    if (created && slot->section_count == 1 && slot->sections && slot->sections[0].texture) {
        return true;
    }
    gr_texture_free(slot);
    return false;
}

void editor_texture_free(int bm)
{
    if (editor_gr_is_d3d11()) {
        return;
    }
    if (GrTextureSlot* slot = gr_texture_slot_of(bm); slot && slot->bm_handle == bm) {
        gr_texture_free(slot);
    }
}
