#include "hud_internal.h"
#include "../rf/bmpman.h"
#include "../rf/hud.h"
#include "../rf/player/player.h"
#include "../rf/entity.h"
#include "../rf/gameseq.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../rf/localize.h"
#include "../rf/multi.h"
#include "../misc/alpine_settings.h"
#include "../multi/vehicles/vehicle.h"
#include <patch_common/AsmWriter.h>
#include <patch_common/FunHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/MemUtils.h>
#include <algorithm>
#include <cmath>

bool g_big_health_armor_hud = false;

void hud_vehicle_bar_fill_color(float frac, rf::ubyte& r, rf::ubyte& g, rf::ubyte& b, rf::ubyte& a)
{
    a = 200;
    if (frac > 0.60f) {
        r = 0; g = 190; b = 40;
        return;
    }
    if (frac > 0.35f) {
        r = 215; g = 200; b = 0;
        return;
    }
    if (frac > 0.15f) {
        r = 230; g = 130; b = 0;
        return;
    }
    r = 200; g = 30; b = 30;
}

namespace
{
    // unscaled hud.tbl units
    constexpr int hud_vehicle_panel_gap = 6;
    constexpr int hud_vehicle_bar_height = 24;

    int hud_bitmap_height(int bmh)
    {
        int w = 0;
        int h = 0;
        if (bmh >= 0) {
            rf::bm::get_dimensions(bmh, &w, &h);
        }
        return h;
    }

    // Where the health/armor cluster ends, in unscaled hud.tbl units.
    int hud_status_cluster_bottom(rf::Entity* entity)
    {
        const int health_idx = std::clamp(static_cast<int>(entity->life * 0.1f), 0, 10);
        const float max_armor = entity->info ? entity->info->max_armor : 0.0f;
        const int enviro_idx = max_armor > 0.0f
            ? std::clamp(static_cast<int>(entity->armor / max_armor * 10.0f), 0, 10)
            : 0;
        int bottom = rf::hud_coords[rf::hud_health].y + hud_bitmap_height(rf::hud_health_bitmaps[health_idx]);
        bottom = std::max(bottom,
                          rf::hud_coords[rf::hud_envirosuit].y + hud_bitmap_height(rf::hud_enviro_bitmaps[enviro_idx]));
        bottom = std::max(bottom, rf::hud_coords[rf::hud_health_value_ul_corner].y
                                      + rf::hud_coords[rf::hud_health_value_width_and_height].y);
        bottom = std::max(bottom, rf::hud_coords[rf::hud_envirosuit_value_ul_corner].y
                                      + rf::hud_coords[rf::hud_envirosuit_value_width_and_height].y);
        return bottom;
    }
} // namespace

// Drawn under the rider's own health/armor cluster, not instead of it.
static void hud_draw_vehicle_status(rf::Entity* entity, float scale)
{
    rf::Entity* vehicle = vehicle_ridden_hull(entity);
    if (!vehicle) {
        return;
    }

    const int bar_x_unscaled = rf::hud_coords[rf::hud_envirosuit].x;
    const int bar_y_unscaled = hud_status_cluster_bottom(entity) + hud_vehicle_panel_gap;
    const int bar_w_unscaled = rf::hud_coords[rf::hud_envirosuit_value_ul_corner].x
        + rf::hud_coords[rf::hud_envirosuit_value_width_and_height].x - bar_x_unscaled;

    rf::HudPoint pos = hud_scale_coords(rf::HudPoint{bar_x_unscaled, bar_y_unscaled}, scale);
    const int w = std::max(8, static_cast<int>(bar_w_unscaled * scale));
    const int h = std::max(6, static_cast<int>(hud_vehicle_bar_height * scale));

    // Keep the whole bar on screen whatever hud.tbl and the scale produce.
    pos.x -= std::max(0, pos.x + w - rf::gr::clip_width());
    pos.y -= std::max(0, pos.y + h - rf::gr::clip_height());

    const float max_life = vehicle_hud_max_life(vehicle);
    const float life = vehicle_hud_life(vehicle);
    const float frac = max_life > 0.0f ? std::clamp(life / max_life, 0.0f, 1.0f) : 1.0f;

    const int border = std::max(1, static_cast<int>(std::lround(scale)));
    const int inner_x = pos.x + border;
    const int inner_y = pos.y + border;
    const int inner_w = std::max(1, w - 2 * border);
    const int inner_h = std::max(1, h - 2 * border);

    rf::gr::set_color(0, 0, 0, 150);
    rf::gr::rect(pos.x, pos.y, w, h);

    if (frac > 0.0f) {
        rf::ubyte r, g, b, a;
        hud_vehicle_bar_fill_color(frac, r, g, b, a);
        rf::gr::set_color(r, g, b, a);
        // Rounded rather than truncated, so a full hull reaches the far edge.
        rf::gr::rect(inner_x, inner_y, std::clamp(static_cast<int>(std::lround(inner_w * frac)), 1, inner_w),
                     inner_h);
    }

    rf::gr::set_color(150, 150, 150, 170);
    hud_rect_border(pos.x, pos.y, w, h, border);

    // No ascent metric exists - both engine helpers return the whole LINE height - so th/6
    // approximates the empty descender space and pulls the digits off the visual high side.
    const int font_id = rf::hud_status_font;
    auto life_str = std::to_string(std::max(static_cast<int>(life), 0));
    const int th = rf::gr::get_string_size(life_str, font_id).second;
    const int text_x = inner_x + inner_w / 2;
    const int text_y = inner_y + (inner_h - th + 1) / 2 + th / 6;

    rf::gr::set_color(0, 0, 0, 220);
    rf::gr::string_aligned(rf::gr::ALIGN_CENTER, text_x + border, text_y + border, life_str.c_str(), font_id);
    rf::gr::set_color(255, 255, 255, 255);
    rf::gr::string_aligned(rf::gr::ALIGN_CENTER, text_x, text_y, life_str.c_str(), font_id);
}

// Retargets hud_status_render's entity_in_vehicle call (0x00439DA0). Answering false lands on
// 0x00439F58 with the registers the on-foot path expects, clear of the FunHook on 0x00439D80.
static bool __cdecl hud_status_rider_hides_own_status(rf::Entity* ep)
{
    if (rf::is_multi && vehicle_ridden_hull(ep)) {
        return false;
    }
    return rf::entity_in_vehicle(ep);
}

// A rider whose vehicle widget this module draws for itself, or null.
static rf::Entity* hud_status_widget_rider(rf::Player* player)
{
    rf::Entity* rider = rf::entity_from_handle(player->entity_handle);
    if (!rf::is_multi || !rf::gameseq_in_gameplay()) {
        return nullptr;
    }
    return vehicle_ridden_hull(rider) ? rider : nullptr;
}

FunHook<void(rf::Player*)> hud_status_render_hook{
    0x00439D80,
    [](rf::Player *player) {
        rf::Entity* widget_rider = hud_status_widget_rider(player);

        if (!g_big_health_armor_hud) {
            // The retarget has already sent this man down the stock on-foot path.
            hud_status_render_hook.call_target(player);
            if (widget_rider) {
                hud_draw_vehicle_status(widget_rider, 1.0f);
            }
            return;
        }

        rf::Entity* entity = rf::entity_from_handle(player->entity_handle);
        if (!entity) {
            return;
        }

        if (!rf::gameseq_in_gameplay()) {
            return;
        }

        int font_id = rf::hud_status_font;
        // Note: 2x scale does not look good because bigfont is not exactly 2x version of smallfont
        float scale = 1.875f;

        // The stock vehicle arm is left for the classes this module does not own.
        if (!widget_rider && rf::entity_in_vehicle(entity)) {
            rf::hud_draw_damage_indicators(player);
            rf::Entity* vehicle = rf::entity_from_handle(entity->host_handle);
            if (rf::entity_is_jeep_driver(entity) || rf::entity_is_jeep_gunner(entity)) {
                rf::gr::set_color(255, 255, 255, 120);
                auto [jeep_x, jeep_y] = hud_scale_coords(rf::hud_coords[rf::hud_jeep], scale);
                hud_scaled_bitmap(rf::hud_health_jeep_bmh, jeep_x, jeep_y, scale);
                auto [jeep_frame_x, jeep_frame_y] = hud_scale_coords(rf::hud_coords[rf::hud_jeep_frame], scale);
                hud_scaled_bitmap(rf::hud_health_veh_frame_bmh, jeep_frame_x, jeep_frame_y, scale);
                auto veh_life = std::max(static_cast<int>(vehicle->life), 1);
                auto veh_life_str = std::to_string(veh_life);
                rf::gr::set_color(rf::hud_full_color);
                auto [jeep_value_x, jeep_value_y] = hud_scale_coords(rf::hud_coords[rf::hud_jeep_value], scale);
                rf::gr::string(jeep_value_x, jeep_value_y, veh_life_str.c_str(), font_id);
            }
            else if (rf::entity_is_driller(vehicle)) {
                rf::gr::set_color(255, 255, 255, 120);
                auto [driller_x, driller_y] = hud_scale_coords(rf::hud_coords[rf::hud_driller], scale);
                hud_scaled_bitmap(rf::hud_health_driller_bmh, driller_x, driller_y, scale);
                auto [driller_frame_x, driller_frame_y] = hud_scale_coords(rf::hud_coords[rf::hud_driller_frame], scale);
                hud_scaled_bitmap(rf::hud_health_veh_frame_bmh, driller_frame_x, driller_frame_y, scale);
                auto veh_life = std::max(static_cast<int>(vehicle->life), 1);
                auto veh_life_str = std::to_string(veh_life);
                rf::gr::set_color(rf::hud_full_color);
                auto [driller_value_x, driller_value_y] = hud_scale_coords(rf::hud_coords[rf::hud_driller_value], scale);
                rf::gr::string(driller_value_x, driller_value_y, veh_life_str.c_str(), font_id);
            }
        }
        else {
            rf::gr::set_color(255, 255, 255, 120);
            int health_tex_idx = static_cast<int>(entity->life * 0.1f);
            health_tex_idx = std::clamp(health_tex_idx, 0, 10);
            int health_bmh = rf::hud_health_bitmaps[health_tex_idx];
            auto [health_x, health_y] = hud_scale_coords(rf::hud_coords[rf::hud_health], scale);
            hud_scaled_bitmap(health_bmh, health_x, health_y, scale);
            int enviro_tex_idx = static_cast<int>(entity->armor / entity->info->max_armor * 10.0f);
            enviro_tex_idx = std::clamp(enviro_tex_idx, 0, 10);
            int enviro_bmh = rf::hud_enviro_bitmaps[enviro_tex_idx];
            auto [envirosuit_x, envirosuit_y] = hud_scale_coords(rf::hud_coords[rf::hud_envirosuit], scale);
            hud_scaled_bitmap(enviro_bmh, envirosuit_x, envirosuit_y, scale);
            rf::gr::set_color(rf::hud_full_color);
            int health = static_cast<int>(std::max(entity->life, 1.0f));
            auto health_str = std::to_string(health);
            auto [text_w, text_h] = rf::gr::get_string_size(health_str, font_id);
            auto [health_value_x, health_value_y] = hud_scale_coords(rf::hud_coords[rf::hud_health_value_ul_corner], scale);
            auto health_value_w = hud_scale_coords(rf::hud_coords[rf::hud_health_value_width_and_height], scale).x;
            rf::gr::string(health_value_x + (health_value_w - text_w) / 2, health_value_y, health_str.c_str(), font_id);
            rf::gr::set_color(rf::hud_mid_color);
            auto armor_str = std::to_string(static_cast<int>(std::lround(entity->armor * (g_alpine_game_config.real_armor_values ? 2.0f : 1.0f))));
            std::tie(text_w, text_h) = rf::gr::get_string_size(armor_str, font_id);
            auto [envirosuit_value_x, envirosuit_value_y] = hud_scale_coords(rf::hud_coords[rf::hud_envirosuit_value_ul_corner], scale);
            auto envirosuit_value_w = hud_scale_coords(rf::hud_coords[rf::hud_envirosuit_value_width_and_height], scale).x;
            rf::gr::string(envirosuit_value_x + (envirosuit_value_w - text_w) / 2, envirosuit_value_y, armor_str.c_str(), font_id);

            rf::hud_draw_damage_indicators(player);

            if (rf::entity_is_carrying_corpse(entity)) {
                rf::gr::set_color(255, 255, 255, 255);
                static const rf::gr::Mode state{
                    rf::gr::TEXTURE_SOURCE_WRAP,
                    rf::gr::COLOR_SOURCE_VERTEX_TIMES_TEXTURE,
                    rf::gr::ALPHA_SOURCE_VERTEX_TIMES_TEXTURE,
                    rf::gr::ALPHA_BLEND_ADDITIVE,
                    rf::gr::ZBUFFER_TYPE_NONE,
                    rf::gr::FOG_NOT_ALLOWED,
                };
                auto [corpse_icon_x, corpse_icon_y] = hud_scale_coords(rf::hud_coords[rf::hud_corpse_icon], scale);
                hud_scaled_bitmap(rf::hud_body_indicator_bmh, corpse_icon_x, corpse_icon_y, scale, state);
                rf::gr::set_color(rf::hud_body_color);
                auto [corpse_text_x, corpse_text_y] = hud_scale_coords(rf::hud_coords[rf::hud_corpse_text], scale);
                rf::gr::string(corpse_text_x, corpse_text_y, rf::strings::array[5], font_id);
            }

            if (widget_rider) {
                hud_draw_vehicle_status(widget_rider, scale);
            }
        }
    },
};

CodeInjection hud_print_armor_patch{
    0x0043A09E,
    [](auto& regs) {
        if (g_alpine_game_config.real_armor_values) {
            rf::Entity* entity = regs.esi;
            if (entity) {
                regs.eax = static_cast<int>(std::lround(entity->armor * 2.0f));
                regs.esp += 0x38;
                regs.eip = 0x0043A0A9;
            }
        }
    },
};

void hud_status_apply_patches()
{
    // Support straight 1:1 armor:effective health display
    // Only applies to non-Big HUD. Big HUD uses logic in hud_status_render_hook
    hud_print_armor_patch.install();

    // Support BigHUD
    hud_status_render_hook.install();

    // A rider of a synced vehicle keeps his own health and armor.
    AsmWriter{0x00439DA0}.call(&hud_status_rider_hides_own_status);
}

void hud_status_set_big(bool is_big)
{
    g_big_health_armor_hud = is_big;
    rf::hud_status_font = rf::gr::load_font(is_big ? "bigfont.vf" : "smallfont.vf");
    static bool big_bitmaps_preloaded = false;
    if (is_big && !big_bitmaps_preloaded) {
        for (int i = 0; i <= 10; ++i) {
            hud_preload_scaled_bitmap(rf::hud_health_bitmaps[i]);
        }
        for (int i = 0; i <= 10; ++i) {
            hud_preload_scaled_bitmap(rf::hud_enviro_bitmaps[i]);
        }
        big_bitmaps_preloaded = true;
    }
}
