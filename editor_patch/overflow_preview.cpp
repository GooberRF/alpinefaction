#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <patch_common/CodeInjection.h>
#include <common/bitmap/formats.h>

#include "overflow_charts.h"
#include "headless_bake.h"
#include "level.h"
#include "terrain_preview.h"
#include "textures.h"
#include "vtypes.h"

namespace
{

// RED's room caches (FUN_0049b550) draw a face without a surface unlit. Overflow faces batch instead under a preview
// page holding each chart's mean in a 2x2 block; solid_write never saves lightmap UVs of such faces.

constexpr std::uint16_t preview_batch_key = 0xFFFE; // a lightmap index no page has
constexpr std::uint32_t preview_max_edge = 1024;

int g_preview_bm = -1;
std::uint32_t g_preview_edge = 0;
std::vector<std::uint8_t> g_preview_rgb;

bool preview_has(const GFace* face)
{
    const auto& faces = overflow_preview_faces();
    return g_preview_bm >= 0 && !faces.empty() && faces.find(face) != faces.end();
}

bool preview_texture_ready()
{
    return g_preview_bm >= 0 && !g_preview_rgb.empty()
        && (editor_bitmap_texture_live(g_preview_bm)
            || editor_bitmap_upload_rgb(g_preview_bm, g_preview_edge, g_preview_edge, g_preview_rgb.data()));
}

// Replaces "MOV word ptr [ESI+0x2c],0" (6 bytes), the batch key of a face without a surface (ESI).
CodeInjection preview_batch_key_injection{
    0x0049b972,
    [](auto& regs) {
        auto* face = reinterpret_cast<GFace*>(static_cast<std::uintptr_t>(regs.esi));
        *reinterpret_cast<std::uint16_t*>(&face->group_id) = preview_has(face) ? preview_batch_key : 0;
        regs.eip = 0x0049b978;
    },
    false, // no trampoline: the injection fully replaces the store
};

// Replaces "MOV EAX,EBX; MOV [ESI+0x44],EAX" (5 bytes): the single pass batch (ESI) of a face without a surface
// (EDI, its first face) takes no lightmap. EAX is reloaded at 0x0049bef3.
CodeInjection preview_batch_lightmap_injection{
    0x0049bebb,
    [](auto& regs) {
        auto& batch = *reinterpret_cast<RedCacheBatch*>(static_cast<std::uintptr_t>(regs.esi));
        const auto* face = reinterpret_cast<const GFace*>(static_cast<std::uintptr_t>(regs.edi));
        batch.lightmap = preview_has(face) && preview_texture_ready() ? g_preview_bm : -1;
        regs.eip = 0x0049bef3;
    },
    false, // no trampoline: the injection fully replaces both instructions
};

// Replaces "MOV [ESI+0x40],EAX; MOV [ESI+0x44],EBX" (6 bytes), where the lightmap pass takes its page (EAX, -1
// for a face without a surface) as its texture.
CodeInjection preview_batch_lightmap_pass_injection{
    0x0049beed,
    [](auto& regs) {
        auto& batch = *reinterpret_cast<RedCacheBatch*>(static_cast<std::uintptr_t>(regs.esi));
        const auto* face = reinterpret_cast<const GFace*>(static_cast<std::uintptr_t>(regs.edi));
        int page = static_cast<int>(regs.eax);
        if (page == -1 && preview_has(face) && preview_texture_ready()) {
            page = g_preview_bm;
        }
        batch.texture = page;
        batch.lightmap = static_cast<int>(regs.ebx);
        regs.eip = 0x0049bef3;
    },
    false, // no trampoline: the injection fully replaces both instructions
};

// Lays the chart means out in the preview page and points the preview faces' lightmap UVs at them.
void preview_apply()
{
    const auto& faces = overflow_preview_faces();
    if (faces.empty()) {
        return;
    }
    const auto key_of = [](const std::array<std::uint8_t, 3>& c, bool quantized) {
        const std::uint32_t mask = quantized ? 0xF8u : 0xFFu;
        return (c[0] & mask) | ((c[1] & mask) << 8) | ((c[2] & mask) << 16);
    };
    constexpr std::size_t capacity = (preview_max_edge / 2) * (preview_max_edge / 2);
    std::unordered_map<std::uint32_t, std::uint32_t> block_of;
    bool quantized = false;
    for (int pass = 0; pass < 2; pass++) {
        block_of.clear();
        for (const auto& [face, rgb] : faces) {
            block_of.try_emplace(key_of(rgb, quantized), static_cast<std::uint32_t>(block_of.size()));
        }
        if (block_of.size() <= capacity) {
            break;
        }
        quantized = true;
    }
    std::uint32_t edge = 16;
    while ((edge / 2) * (edge / 2) < block_of.size() && edge < preview_max_edge) {
        edge *= 2;
    }
    g_preview_rgb.assign(static_cast<std::size_t>(edge) * edge * 3, 0);
    const std::uint32_t per_row = edge / 2;
    for (const auto& [key, block] : block_of) {
        const std::uint32_t bx = (block % per_row) * 2, by = (block / per_row) * 2;
        for (std::uint32_t dy = 0; dy < 2; dy++) {
            for (std::uint32_t dx = 0; dx < 2; dx++) {
                std::uint8_t* t = &g_preview_rgb[(static_cast<std::size_t>(by + dy) * edge + bx + dx) * 3];
                t[0] = static_cast<std::uint8_t>(key & 0xFF);
                t[1] = static_cast<std::uint8_t>((key >> 8) & 0xFF);
                t[2] = static_cast<std::uint8_t>((key >> 16) & 0xFF);
            }
        }
    }
    if (g_preview_bm >= 0 && g_preview_edge != edge) {
        // bm_release leaves the bitmap's texture behind
        if (GrTextureSlot* slot = gr_texture_slot_of(g_preview_bm); slot && slot->bm_handle == g_preview_bm) {
            gr_texture_free(slot);
        }
        bm_release(g_preview_bm);
        g_preview_bm = -1;
    }
    if (g_preview_bm < 0) {
        g_preview_bm = bm_create(BM_FORMAT_565_RGB, static_cast<int>(edge), static_cast<int>(edge));
    }
    g_preview_edge = edge;
    if (g_preview_bm < 0) {
        return;
    }
    editor_bitmap_upload_rgb(g_preview_bm, edge, edge, g_preview_rgb.data());
    for (const auto& [face, rgb] : faces) {
        const std::uint32_t block = block_of[key_of(rgb, quantized)];
        const float u = static_cast<float>((block % per_row) * 2 + 1) / static_cast<float>(edge);
        const float v = static_cast<float>((block / per_row) * 2 + 1) / static_cast<float>(edge);
        const GFaceVertex* start = face->edge_loop;
        for (GFaceVertex* fv = face->edge_loop; fv;) {
            fv->lm_u = u;
            fv->lm_v = v;
            fv = fv->next;
            if (fv == start) {
                break;
            }
        }
    }
}

} // namespace

void overflow_preview_changed(bool after_bake)
{
    // a headless bake draws nothing
    if (headless_bake_active()) {
        return;
    }
    preview_apply();
    if (after_bake) {
        // the rooms were cached before their faces had the preview page; a level being loaded caches its rooms
        // when first drawn, after this
        geo_cache_flush_all();
        editor_views_mark_repaint_all();
    }
}

void overflow_preview_install()
{
    preview_batch_key_injection.install();
    preview_batch_lightmap_injection.install();
    preview_batch_lightmap_pass_injection.install();
}
