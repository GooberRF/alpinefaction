#include "hud_internal.h"
#include "hud.h"
#include "../rf/hud.h"
#include "../rf/player/player.h"
#include "../rf/entity.h"
#include "../rf/gameseq.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../rf/localize.h"
#include "../misc/alpine_settings.h"
#include <patch_common/FunHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/MemUtils.h>
#include <patch_common/CallHook.h>
#include <algorithm>

// Hook to ensure health/status font uses our dynamic font system
CallHook<void(int, int, const char*, int)> hud_status_font_hook{
    {
        // Hook gr::string calls in health/status rendering
        0x00439F3A, // Health text rendering
        0x00439F7A, // Armor text rendering
    },
    [](int x, int y, const char* text, int font_id) {
        // Replace font ID with our dynamic font
        if (font_id == rf::hud_status_font) {
            font_id = hud_get_health_font();
        }
        rf::gr::string(x, y, text, font_id);
    },
};

bool g_big_health_armor_hud = false;
float g_hud_health_scale = 1.0f;

// Hook health bitmap rendering to apply scaling
CallHook<void(int, int, int, rf::gr::Mode)> hud_render_health_gr_bitmap_hook{
    {
        // Health icon rendering
        0x00439EE7,  // health bitmap render
        0x00439F58,  // envirosuit bitmap render
    },
    [](int bm_handle, int x, int y, rf::gr::Mode mode) {
        float scale = g_hud_health_scale;
        if (g_big_health_armor_hud) {
            scale *= 1.875f;
        }
        hud_scaled_bitmap(bm_handle, x, y, scale, mode);
    },
};

FunHook<void(rf::Player*)> hud_status_render_hook{
    0x00439D80,
    [](rf::Player *player) {
        if (!g_big_health_armor_hud && g_hud_health_scale == 1.0f) {
            hud_status_render_hook.call_target(player);
            return;
        }

        rf::Entity* entity = rf::entity_from_handle(player->entity_handle);
        if (!entity) {
            return;
        }

        if (!rf::gameseq_in_gameplay()) {
            return;
        }

        int font_id = hud_get_health_font();
        // Note: 2x scale does not look good because bigfont is not exactly 2x version of smallfont
        float base_scale = g_big_health_armor_hud ? 1.875f : 1.0f;
        float scale = base_scale * g_hud_health_scale;

        if (rf::entity_in_vehicle(entity)) {
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
            int text_w, text_h;
            rf::gr::get_string_size(&text_w, &text_h, health_str.c_str(), -1, font_id);
            auto [health_value_x, health_value_y] = hud_scale_coords(rf::hud_coords[rf::hud_health_value_ul_corner], scale);
            auto health_value_w = hud_scale_coords(rf::hud_coords[rf::hud_health_value_width_and_height], scale).x;
            rf::gr::string(health_value_x + (health_value_w - text_w) / 2, health_value_y, health_str.c_str(), font_id);
            rf::gr::set_color(rf::hud_mid_color);
            auto armor_str = std::to_string(static_cast<int>(std::lround(entity->armor * (g_alpine_game_config.real_armor_values ? 2.0f : 1.0f))));
            rf::gr::get_string_size(&text_w, &text_h, armor_str.c_str(), -1, font_id);
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
    
    // Support health scaling regardless of big HUD mode
    hud_render_health_gr_bitmap_hook.install();
    
    // Hook font usage to use dynamic font system
    hud_status_font_hook.install();
}

void hud_status_set_big(bool is_big)
{
    g_big_health_armor_hud = is_big;
    g_hud_health_scale = g_alpine_game_config.health_hud_scale;
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

void hud_status_update_scale()
{
    g_hud_health_scale = g_alpine_game_config.health_hud_scale;
}
