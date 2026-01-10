#include <patch_common/CallHook.h>
#include <patch_common/FunHook.h>
#include <patch_common/CodeInjection.h>
#include "xlog/xlog.h"
#include "../rf/gr/gr.h"
#include "../rf/hud.h"
#include "../rf/entity.h"
#include "../rf/weapon.h"
#include "../rf/gr/gr_font.h"
#include "../rf/player/player.h"
#include "../rf/multi.h"
#include "../graphics/gr.h"
#include "../main/main.h"
#include "../misc/alpine_settings.h"
#include "../misc/misc.h"
#include "hud_internal.h"

float g_hud_ammo_scale = 1.0f;
bool g_displaying_custom_reticle = false;

CallHook<void(int, int, int, rf::gr::Mode)> hud_render_ammo_gr_bitmap_hook{
    {
        // hud_render_ammo_clip
        0x0043A5E9u,
        0x0043A637u,
        0x0043A680u,
        // hud_render_ammo_power
        0x0043A988u,
        0x0043A9DDu,
        0x0043AA24u,
        // hud_render_ammo_no_clip
        0x0043AE80u,
        0x0043AEC3u,
        0x0043AF0Au,
    },
    [](int bm_handle, int x, int y, rf::gr::Mode mode) {
        hud_scaled_bitmap(bm_handle, x, y, g_hud_ammo_scale, mode);
    },
};

CallHook<void(int, int, int, rf::gr::Mode)> render_reticle_gr_bitmap_hook{
    {
        0x0043A499,
        0x0043A4FE,
    },
    [](int bm_handle, int x, int y, rf::gr::Mode mode) {
        float base_scale = g_alpine_game_config.big_hud ? 2.0f : 1.0f;
        float scale = base_scale * g_alpine_game_config.get_reticle_scale();
        int clip_w = rf::gr::clip_width();
        int clip_h = rf::gr::clip_height();

        x = static_cast<int>((x - clip_w / 2.0F) * scale + clip_w / 2.0F);
        y = static_cast<int>((y - clip_h / 2.0F) * scale + clip_h / 2.0F);

        hud_scaled_bitmap(bm_handle, x, y, scale, mode);
    },
};

CallHook<void(int, int, int, int)> render_reticle_set_color_hook{
    0x0043A4D7,
    [](int r, int g, int b, int a) {
        rf::Color clr{};

        if (g_displaying_custom_reticle && !g_alpine_game_config.colorize_custom_reticles) {
            clr = {255, 255, 255, 255}; // white
        }
        else if (g_alpine_game_config.reticle_color_override) {
            clr = rf::Color::from_hex(*g_alpine_game_config.reticle_color_override);
        }
        else {
            clr = g_displaying_custom_reticle ?
                rf::Color{255, 255, 255, 255} : // white
                rf::Color{0, 255, 0, 255}; // green
        }

        render_reticle_set_color_hook.call_target(clr.red, clr.green, clr.blue, clr.alpha);
    },
};

CallHook<void(int, int, int, int)> render_reticle_locked_set_color_hook{
    0x0043A472,
    [](int r, int g, int b, int a) {
        rf::Color clr{};

        if (g_displaying_custom_reticle && !g_alpine_game_config.colorize_custom_reticles) {
            clr = {255, 255, 255, 255}; // white
        }
        else if (g_alpine_game_config.reticle_locked_color_override) {
            clr = rf::Color::from_hex(*g_alpine_game_config.reticle_locked_color_override);
        }
        else {
            clr = g_displaying_custom_reticle ?
                rf::Color{255, 255, 255, 255} : // white
                rf::Color{255, 0, 0, 255}; // red
        }

        render_reticle_locked_set_color_hook.call_target(clr.red, clr.green, clr.blue, clr.alpha);
    },
};

CodeInjection render_reticle_check_custom_injection{
    0x0043A3B1,
    [](auto& regs) {
        int weap_slot = regs.eax;

        if (weap_slot >= 0) {
            float base_scale = g_alpine_game_config.big_hud ? 2.0f : 1.0f;
            float scale = base_scale * g_alpine_game_config.get_reticle_scale();
            bool big_reticle = scale > 1.0f; // reticle is using _1 variant
            g_displaying_custom_reticle = weapon_reticle_is_customized(weap_slot, big_reticle);

            // special case for rocket lock on reticle
            if (weap_slot == rf::rocket_launcher_weapon_type) {
                rf::Player* pp = regs.edi;
                if (rf::player_fpgun_locked_on(pp)) {
                    g_displaying_custom_reticle = rocket_locked_reticle_is_customized(big_reticle);
                }
            }
        }
        else {
            g_displaying_custom_reticle = false;
        }
        //xlog::warn("player {}, weap {}, big? {}, custom? {}", pp->name, weap_slot, big_reticle, g_displaying_custom_reticle);
    },
};

FunHook<void(rf::Entity*, int, int, bool)> hud_render_ammo_hook{
    0x0043A510,
    [](rf::Entity *entity, int weapon_type, int offset_y, bool is_inactive) {
        offset_y = static_cast<int>(offset_y * g_hud_ammo_scale);
        hud_render_ammo_hook.call_target(entity, weapon_type, offset_y, is_inactive);
    },
};

void hud_weapons_set_big(bool is_big)
{
    rf::HudItem ammo_hud_items[] = {
        rf::hud_ammo_bar,
        rf::hud_ammo_signal,
        rf::hud_ammo_icon,
        rf::hud_ammo_in_clip_text_ul_region_coord,
        rf::hud_ammo_in_clip_text_width_and_height,
        rf::hud_ammo_in_inv_text_ul_region_coord,
        rf::hud_ammo_in_inv_text_width_and_height,
        rf::hud_ammo_bar_position_no_clip,
        rf::hud_ammo_signal_position_no_clip,
        rf::hud_ammo_icon_position_no_clip,
        rf::hud_ammo_in_inv_ul_region_coord_no_clip,
        rf::hud_ammo_in_inv_text_width_and_height_no_clip,
        rf::hud_ammo_in_clip_ul_coord,
        rf::hud_ammo_in_clip_width_and_height,
    };
    g_hud_ammo_scale = is_big ? 1.875f : 1.0f;
    for (auto item_num : ammo_hud_items) {
        rf::hud_coords[item_num] = hud_scale_coords(rf::hud_coords[item_num], g_hud_ammo_scale);
    }
    rf::hud_ammo_font = rf::gr::load_font(is_big ? "biggerfont.vf" : "bigfont.vf");
}


bool hud_weapons_is_double_ammo()
{
    if (rf::is_multi) {
        return false;
    }
    rf::Entity* entity = rf::entity_from_handle(rf::local_player->entity_handle);
    if (!entity) {
        return false;
    }
    auto weapon_type = entity->ai.current_primary_weapon;
    return weapon_type == rf::machine_pistol_weapon_type || weapon_type == rf::machine_pistol_special_weapon_type;
}

void hud_weapons_apply_patches()
{
    // Big HUD support for ammo display
    hud_render_ammo_gr_bitmap_hook.install();
    hud_render_ammo_hook.install();

    // reticle color and scale
    render_reticle_gr_bitmap_hook.install();
    render_reticle_set_color_hook.install();
    render_reticle_locked_set_color_hook.install();
    render_reticle_check_custom_injection.install();
}
