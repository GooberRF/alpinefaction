#include <algorithm>
#include <cmath>
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
#include "../multi/vehicles/vehicle.h"
#include "../multi/vehicles/vehicle_physics.h"
#include "hud.h"
#include "hud_internal.h"
#include "multi_spectate.h"

float g_hud_ammo_scale = 1.0f;
bool g_displaying_custom_reticle = false;

// The Machine Pistol Special's row under the main widget (0x0043B1D5).
constexpr int hud_second_ammo_slot_offset_y = 45;

// The APC this rider drives; the ammo widgets show its primary and secondary.
static rf::Entity* hud_ammo_driven_apc(rf::Entity* rider)
{
    if (!rf::is_multi) {
        return nullptr;
    }
    rf::Entity* hull = vehicle_ridden_hull(rider);
    if (!hull || vehicle_damage_class(hull) != VDC_APC || vehicle_firing_seat_occupant(hull) != rider) {
        return nullptr;
    }
    return hull;
}

static bool hud_ammo_hull_has_secondary(const rf::Entity* hull)
{
    return vehicle_weapon_ammo(hull, hull->ai.current_secondary_weapon) >= 0;
}

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
        float aim_dx = 0.0f;
        float aim_dy = 0.0f;
        if (!vehicle_physics_camera_reticle_offset(&aim_dx, &aim_dy)) {
            return;
        }
        float base_scale = g_alpine_game_config.big_hud ? 2.0f : 1.0f;
        float scale = base_scale * g_alpine_game_config.get_reticle_scale();
        int clip_w = rf::gr::clip_width();
        int clip_h = rf::gr::clip_height();

        x = static_cast<int>((x - clip_w / 2.0F) * scale + clip_w / 2.0F + aim_dx);
        y = static_cast<int>((y - clip_h / 2.0F) * scale + clip_h / 2.0F + aim_dy);

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

// A weapon with no FP mesh is confusing, render the weapon's display name
// so at least you know what gun you have out.
bool weapon_has_first_person_mesh(int weapon_type)
{
    if (weapon_type < 0 || weapon_type >= rf::num_weapon_types) {
        return true;
    }
    const char* filename = rf::weapon_types[weapon_type].first_person_vmesh_filename;
    return filename && *filename;
}

void hud_render_weapon_name_label(int weapon_type)
{
    // Hidden with the first person weapon itself.
    if (!rf::local_player || !rf::local_player->settings.render_fpgun) {
        return;
    }
    if (weapon_has_first_person_mesh(weapon_type)) {
        return;
    }
    const char* name = rf::weapon_types[weapon_type].display_name;
    if (!name || !*name) {
        name = rf::weapon_types[weapon_type].name;   // fall back to the class name
    }
    if (!name || !*name) {
        return;
    }

    const int font_id = rf::hud_text_font_num;
    const auto [text_w, text_h] = rf::gr::get_string_size(name, font_id);

    const int pad = std::max(4, static_cast<int>(6 * g_hud_ammo_scale));
    const int box_w = text_w + pad * 2;
    const int box_h = text_h + pad;
    const int margin_x = std::max(8, static_cast<int>(12 * g_hud_ammo_scale));
    const int margin_y = std::max(40, static_cast<int>(64 * g_hud_ammo_scale));
    const int box_x = rf::gr::screen_width() - box_w - margin_x;
    const int box_y = rf::gr::screen_height() - box_h - margin_y;

    rf::gr::set_color(0, 0, 0, 140);
    rf::gr::rect(box_x, box_y, box_w, box_h);
    rf::gr::set_color(rf::hud_full_color);
    hud_rect_border(box_x, box_y, box_w, box_h, 1);
    rf::gr::string(box_x + pad, box_y + pad / 2, name, font_id);
}

FunHook<void(rf::Entity*, int, int, bool)> hud_render_ammo_hook{
    0x0043A510,
    [](rf::Entity *entity, int weapon_type, int offset_y, bool is_inactive) {
        offset_y = static_cast<int>(offset_y * g_hud_ammo_scale);
        hud_render_ammo_hook.call_target(entity, weapon_type, offset_y, is_inactive);
        if (!is_inactive) {
            hud_render_weapon_name_label(weapon_type);
        }
    },
};

FunHook<void(rf::Entity*, int)> hud_render_ammo_no_clip_hook{
    0x0043ADD0,
    [](rf::Entity* entity, int weapon_type) {
        hud_render_ammo_no_clip_hook.call_target(entity, weapon_type);
        // Only the APC driver's path hands this widget an APC hull.
        if (vehicle_damage_class(entity) == VDC_APC) {
            if (hud_ammo_hull_has_secondary(entity)) {
                constexpr rf::HudItem no_clip_items[] = {
                    rf::hud_ammo_bar_position_no_clip,
                    rf::hud_ammo_signal_position_no_clip,
                    rf::hud_ammo_icon_position_no_clip,
                    rf::hud_ammo_in_inv_ul_region_coord_no_clip,
                };
                const int dy = static_cast<int>(hud_second_ammo_slot_offset_y * g_hud_ammo_scale);
                for (auto item : no_clip_items) {
                    rf::hud_coords[item].y += dy;
                }
                hud_render_ammo_no_clip_hook.call_target(entity, entity->ai.current_secondary_weapon);
                for (auto item : no_clip_items) {
                    rf::hud_coords[item].y -= dy;
                }
            }
            return;
        }
        hud_render_weapon_name_label(weapon_type);
    },
};

CallHook<bool(rf::Entity*)> hud_weapons_render_jeep_gunner_hook{
    {
        0x0043B0E6u,
        0x0043B128u,
    },
    [](rf::Entity* ep) {
        return hud_weapons_render_jeep_gunner_hook.call_target(ep) || hud_ammo_driven_apc(ep) != nullptr;
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


static void hud_ammo_bitmap_size(int bmh, int& w, int& h)
{
    w = 0;
    h = 0;
    if (bmh >= 0) {
        rf::bm::get_dimensions(bmh, &w, &h);
    }
    w = static_cast<int>(std::round(w * g_hud_ammo_scale));
    h = static_cast<int>(std::round(h * g_hud_ammo_scale));
}

// Union of the clip, power and no-clip layouts, so the result does not change with the weapon; plus the
// second row while one is drawn.
int hud_ammo_counter_bottom_y()
{
    auto bm_bottom = [](rf::HudItem item, int bmh) {
        int w, h;
        hud_ammo_bitmap_size(bmh, w, h);
        return rf::hud_coords[item].y + h;
    };
    auto text_bottom = [](rf::HudItem ul, rf::HudItem wh) {
        return rf::hud_coords[ul].y + rf::hud_coords[wh].y;
    };
    return std::max({
        bm_bottom(rf::hud_ammo_bar, rf::hud_ammo_bar_bmh),
        bm_bottom(rf::hud_ammo_bar, rf::hud_ammo_bar_power_bmh),
        bm_bottom(rf::hud_ammo_signal, rf::hud_ammo_signal_green_bmh),
        bm_bottom(rf::hud_ammo_bar_position_no_clip, rf::hud_noclip_ammo_bar_bmh),
        bm_bottom(rf::hud_ammo_signal_position_no_clip, rf::hud_ammo_signal_green_bmh),
        text_bottom(rf::hud_ammo_in_clip_text_ul_region_coord, rf::hud_ammo_in_clip_text_width_and_height),
        text_bottom(rf::hud_ammo_in_inv_text_ul_region_coord, rf::hud_ammo_in_inv_text_width_and_height),
        text_bottom(rf::hud_ammo_in_inv_ul_region_coord_no_clip, rf::hud_ammo_in_inv_text_width_and_height_no_clip),
        text_bottom(rf::hud_ammo_in_clip_ul_coord, rf::hud_ammo_in_clip_width_and_height),
    }) + (hud_weapons_is_double_ammo() ? static_cast<int>(hud_second_ammo_slot_offset_y * g_hud_ammo_scale) : 0);
}

int hud_ammo_counter_right_x()
{
    int bar_w, bar_h, noclip_w, noclip_h;
    hud_ammo_bitmap_size(rf::hud_ammo_bar_bmh, bar_w, bar_h);
    hud_ammo_bitmap_size(rf::hud_noclip_ammo_bar_bmh, noclip_w, noclip_h);
    return std::max(rf::hud_coords[rf::hud_ammo_bar].x + bar_w,
                    rf::hud_coords[rf::hud_ammo_bar_position_no_clip].x + noclip_w);
}

bool hud_weapons_is_double_ammo()
{
    if (rf::is_multi) {
        rf::Player* pp = multi_spectate_is_following_player() ? multi_spectate_get_target_player() : rf::local_player;
        rf::Entity* hull = pp ? hud_ammo_driven_apc(rf::entity_from_handle(pp->entity_handle)) : nullptr;
        return hull && hud_ammo_hull_has_secondary(hull);
    }
    rf::Entity* entity = rf::entity_from_handle(rf::local_player->entity_handle);
    if (!entity) {
        return false;
    }
    auto weapon_type = entity->ai.current_primary_weapon;
    return weapon_type == rf::machine_pistol_weapon_type || weapon_type == rf::machine_pistol_special_weapon_type;
}

static bool is_mouse_wheel_down() {
    static bool was_mouse_3_down = false;
    static HighResTimer mouse_3_up_cool_down_timer{};
    constexpr int MOUSE_BUTTON_3 = 2;
    const bool is_mouse_3_down = rf::mouse_button_is_down(MOUSE_BUTTON_3);
    if (!is_mouse_3_down && was_mouse_3_down) {
        // Use a cool down after key up to avoid accidental
        // weapon cycle selection.
        constexpr uint64_t COOL_DOWN_MS = 64;
        mouse_3_up_cool_down_timer.set_ms(COOL_DOWN_MS);
    } else if (is_mouse_3_down) {
        mouse_3_up_cool_down_timer.invalidate();
    }
    was_mouse_3_down = is_mouse_3_down;
    return is_mouse_3_down || (mouse_3_up_cool_down_timer.valid()
        && !mouse_3_up_cool_down_timer.elapsed());
}

FunHook<void(rf::Player*, int, bool)> player_select_next_primary_hook{
    0x004A3770,
    [] (rf::Player* const player, const int a2, const bool play_sound) {
        if (!is_mouse_wheel_down() || rf::hud_render_weapon_cycle) {
            player_select_next_primary_hook.call_target(player, a2, play_sound);
        }
    },
};

FunHook<void(rf::Player*, int, bool)> player_select_prev_primary_hook{
    0x004A3BE0,
    [] (rf::Player* const player, const int a2, const bool play_sound) {
        if (!is_mouse_wheel_down() || rf::hud_render_weapon_cycle) {
            player_select_prev_primary_hook.call_target(player, a2, play_sound);
        }
    },
};

void hud_weapons_apply_patches()
{
    // Big HUD support for ammo display
    hud_render_ammo_gr_bitmap_hook.install();
    hud_render_ammo_hook.install();
    hud_render_ammo_no_clip_hook.install();
    hud_weapons_render_jeep_gunner_hook.install();

    // reticle color and scale
    render_reticle_gr_bitmap_hook.install();
    render_reticle_set_color_hook.install();
    render_reticle_locked_set_color_hook.install();
    render_reticle_check_custom_injection.install();

    // Disable weapon cycle selection, if `Mouse 3` is pressed.
    player_select_next_primary_hook.install();
    player_select_prev_primary_hook.install();
}
