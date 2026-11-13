#include "hud_internal.h"
#include "hud.h"
#include "../rf/hud.h"
#include "../rf/player/player.h"
#include "../rf/entity.h"
#include "../rf/gameseq.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../rf/localize.h"
#include "../rf/multi.h"
#include "../rf/os/frametime.h"
#include "../misc/alpine_settings.h"
#include "../os/os.h"
#include "../multi/vehicles/vehicle.h"
#include <patch_common/AsmWriter.h>
#include <patch_common/FunHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/MemUtils.h>
#include <algorithm>
#include <cmath>
#include <format>

bool g_big_health_armor_hud = false;

void hud_vehicle_bar_fill_color(float frac, rf::ubyte& r, rf::ubyte& g, rf::ubyte& b, rf::ubyte& a)
{
    a = 200;
    if (frac > 0.60f) {
        r = 0;
        g = 190;
        b = 40;
        return;
    }
    if (frac > 0.35f) {
        r = 215;
        g = 200;
        b = 0;
        return;
    }
    if (frac > 0.15f) {
        r = 230;
        g = 130;
        b = 0;
        return;
    }
    r = 200;
    g = 30;
    b = 30;
}

namespace
{
    constexpr int hud_side_gauge_text_gap = 4;

    float hud_vehicle_bar_alpha(const HudVehicleBarFade& bar, int64_t now)
    {
        const float fade = bar.full_since_ms < 0
            ? 1.0f
            : std::clamp(1.0f - static_cast<float>(now - bar.full_since_ms) / hud_vehicle_bar_fade_ms, 0.0f, 1.0f);
        const float t = std::clamp(static_cast<float>(now - bar.rise_since_ms) / hud_vehicle_bar_rise_ms, 0.0f, 1.0f);
        return std::min(bar.rise_from + (1.0f - bar.rise_from) * t, fade);
    }
} // namespace

float HudVehicleBarFade::update(int hull, bool showing)
{
    if (hull_handle != hull || last_frame != rf::frame_count - 1) {
        *this = HudVehicleBarFade{hull};
    }
    last_frame = rf::frame_count;

    const int64_t now = timer::get_i64(1000);
    if (showing) {
        if (!active || full_since_ms >= 0) {
            rise_from = active ? hud_vehicle_bar_alpha(*this, now) : 0.0f;
            rise_since_ms = now;
            full_since_ms = -1;
            active = true;
        }
    }
    else {
        if (!active) {
            return 0.0f;
        }
        if (full_since_ms < 0) {
            full_since_ms = now;
        }
        if (now - full_since_ms >= hud_vehicle_bar_fade_ms) {
            active = false;
            return 0.0f;
        }
    }
    return hud_vehicle_bar_alpha(*this, now);
}

void hud_draw_shadowed_text(int x, int y, const char* text, int font, const rf::Color& color, rf::ubyte shadow_alpha,
                            int shadow_offset)
{
    rf::gr::set_color(0, 0, 0, shadow_alpha);
    rf::gr::string_aligned(rf::gr::ALIGN_CENTER, x + shadow_offset, y + shadow_offset, text, font);
    rf::gr::set_color(color);
    rf::gr::string_aligned(rf::gr::ALIGN_CENTER, x, y, text, font);
}

HudSideGaugeRect hud_side_gauge_rect(bool left)
{
    const bool big = g_alpine_game_config.big_hud;
    const int w = big ? 16 : 10;
    const int h = big ? 180 : 120;
    const int margin = big ? 60 : 40;
    return {left ? margin : rf::gr::clip_width() - margin - w, (rf::gr::clip_height() - h) / 2, w, h};
}

int hud_side_gauge_label_y(const HudSideGaugeRect& rect)
{
    return rect.y - rf::gr::get_font_height(hud_get_small_font()) - hud_side_gauge_text_gap;
}

void hud_draw_side_gauge(bool left, float frac, const rf::Color& fill, const char* label, const char* value,
                         float alpha)
{
    const auto scaled = [alpha](int a) { return static_cast<rf::ubyte>(a * alpha); };
    const HudSideGaugeRect rect = hud_side_gauge_rect(left);
    const auto [x, y, w, h] = rect;
    const int border = g_alpine_game_config.big_hud ? 2 : 1;

    rf::gr::set_color(0, 0, 0, scaled(128));
    rf::gr::rect(x, y, w, h);
    rf::gr::set_color(255, 255, 255, scaled(170));
    hud_rect_border(x, y, w, h, border);

    const int inner_h = h - 2 * border;
    const int fill_h = static_cast<int>(inner_h * std::clamp(frac, 0.0f, 1.0f));
    if (fill_h > 0) {
        rf::gr::set_color(fill.red, fill.green, fill.blue, scaled(220));
        rf::gr::rect(x + border, y + border + (inner_h - fill_h), w - 2 * border, fill_h);
    }

    const int font = hud_get_small_font();
    const rf::Color text_color{255, 255, 255, scaled(hud_shadowed_text_alpha)};
    const rf::ubyte shadow_alpha = scaled(hud_shadowed_text_shadow_alpha);
    hud_draw_shadowed_text(x + w / 2, hud_side_gauge_label_y(rect), label, font, text_color, shadow_alpha,
                           hud_shadowed_text_shadow_offset_px);
    if (value) {
        hud_draw_shadowed_text(x + w / 2, y + h + hud_side_gauge_text_gap, value, font, text_color, shadow_alpha,
                               hud_shadowed_text_shadow_offset_px);
    }
}

namespace
{
    // Below the reticle, clear of the notification slots above it: amber through the countdown, then a red pulse
    // while the hull drains.
    void hud_draw_vehicle_hazard_warning(const VehicleHazardView& hazard)
    {
        static float pulse_phase = 0.0f;
        const bool draining = hazard.warn_left <= 0.0f;
        pulse_phase = draining ? pulse_phase + rf::frametime * 12.0f : 0.0f;
        const auto alpha = static_cast<rf::ubyte>(draining ? 150.0f + 105.0f * std::cos(pulse_phase) : 255.0f);
        const rf::Color color = draining ? rf::Color{255, 60, 40, alpha} : rf::Color{255, 190, 0, alpha};

        const int font_id = hud_get_large_font();
        const int x = rf::gr::clip_width() / 2;
        const int y = rf::gr::clip_height() * 5 / 8;
        const std::string detail = draining ? "BAIL OUT!" : std::format("BAIL OUT  {:.1f}", hazard.warn_left);
        hud_draw_shadowed_text(x, y, hazard.is_sub ? "OUT OF WATER" : "FLOODING", font_id, color, alpha, 2);
        hud_draw_shadowed_text(x, y + rf::gr::get_font_height(font_id), detail.c_str(), font_id, color, alpha, 2);
    }

    // The jetpack fuel gauge's twin on the left, with the hull's life below; only while the hull is below full.
    void hud_draw_vehicle_hull_gauge(rf::Entity* vehicle)
    {
        static HudVehicleBarFade fade;
        const float max_life = vehicle_hud_max_life(vehicle);
        const float life = vehicle_hud_life(vehicle);
        const float frac = max_life > 0.0f ? std::clamp(life / max_life, 0.0f, 1.0f) : 1.0f;
        const float alpha = fade.update(vehicle->handle, frac < 1.0f);
        if (alpha <= 0.0f) {
            return;
        }
        rf::ubyte r, g, b, a;
        hud_vehicle_bar_fill_color(frac, r, g, b, a);
        // Rounded up, so a hull still alive never reads 0.
        const auto life_str = std::to_string(std::max(static_cast<int>(std::ceil(life)), 0));
        hud_draw_side_gauge(true, frac, {r, g, b}, "HULL", life_str.c_str(), alpha);
    }

    // The ridden hull's gauge, and its drown or strand warning.
    void hud_draw_vehicle_status(rf::Entity* entity)
    {
        rf::Entity* vehicle = vehicle_ridden_hull(entity);
        if (!vehicle) {
            return;
        }
        hud_draw_vehicle_hull_gauge(vehicle);

        VehicleHazardView hazard;
        if (vehicle_hazard_view(vehicle, hazard)) {
            hud_draw_vehicle_hazard_warning(hazard);
        }
    }

    // Retargets hud_status_render's entity_in_vehicle call (0x00439DA0). Answering false lands on
    // 0x00439F58 with the registers the on-foot path expects, clear of the FunHook on 0x00439D80.
    bool __cdecl hud_status_rider_hides_own_status(rf::Entity* ep)
    {
        if (rf::is_multi && vehicle_ridden_hull(ep)) {
            return false;
        }
        return rf::entity_in_vehicle(ep);
    }

    // A rider whose vehicle widget this module draws for itself, or null.
    rf::Entity* hud_status_widget_rider(rf::Player* player)
    {
        rf::Entity* rider = rf::entity_from_handle(player->entity_handle);
        if (!rf::is_multi || !rf::gameseq_in_gameplay()) {
            return nullptr;
        }
        return vehicle_ridden_hull(rider) ? rider : nullptr;
    }
} // namespace

FunHook<void(rf::Player*)> hud_status_render_hook{
    0x00439D80,
    [](rf::Player *player) {
        rf::Entity* widget_rider = hud_status_widget_rider(player);

        if (!g_big_health_armor_hud) {
            // The retarget has already sent this man down the stock on-foot path.
            hud_status_render_hook.call_target(player);
            if (widget_rider) {
                hud_draw_vehicle_status(widget_rider);
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
                hud_draw_vehicle_status(widget_rider);
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
