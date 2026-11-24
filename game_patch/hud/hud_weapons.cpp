#include <algorithm>
#include <cmath>
#include <cstring>
#include <patch_common/CallHook.h>
#include <patch_common/FunHook.h>
#include <patch_common/CodeInjection.h>
#include "xlog/xlog.h"
#include "../rf/gr/gr.h"
#include "../rf/bmpman.h"
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
#include "../os/console.h"
#include "../os/os.h"
#include "../rf/os/frametime.h"
#include "../multi/vehicles/vehicle.h"
#include "../multi/vehicles/vehicle_physics.h"
#include "hud.h"
#include "hud_internal.h"
#include "multi_spectate.h"
#include <common/utils/string-utils.h>

float g_hud_ammo_scale = 1.0f;
bool g_displaying_custom_reticle = false;
static bool g_reticle_locked = false;
static int g_reticle_weapon_slot = -1; // weapon whose reticle is being drawn this frame

static float hud_reticle_scale()
{
    return (g_alpine_game_config.big_hud ? 2.0f : 1.0f) * g_alpine_game_config.get_reticle_scale();
}

static ReticleConfig reticle_active_config()
{
    if (g_reticle_weapon_slot >= 0 && g_reticle_weapon_slot < rf::num_weapon_types) {
        const char* name = rf::weapon_types[g_reticle_weapon_slot].name;
        return g_alpine_game_config.reticle_for_weapon(name ? name : "");
    }
    return g_alpine_game_config.default_reticle();
}

constexpr rf::gr::Mode reticle_aa_mode{
    rf::gr::TEXTURE_SOURCE_NONE,
    rf::gr::COLOR_SOURCE_VERTEX,
    rf::gr::ALPHA_SOURCE_VERTEX,
    rf::gr::ALPHA_BLEND_ALPHA,
    rf::gr::ZBUFFER_TYPE_NONE,
    rf::gr::FOG_NOT_ALLOWED,
};

// Fills the band between loop(s_in) and loop(s_out), built from n quads via tmapper, same as gr_rect does internally.
// loop(s, i, x, y) gives vertex i of a closed n-point outline (clip-relative) pushed out by edge offset s.
// Each real edge gets a 1 px alpha ramp centered on it (anti-aliasing): a pixel center half a pixel inside gets full alpha,
// one half a pixel outside none, so pixel-aligned edges stay crisp. inner_edge: loop(s_in) is an edge (a ring's hole).
template<typename Loop>
static void reticle_draw_band(Loop loop, int n, float s_in, float s_out, bool inner_edge, rf::Color clr)
{
    rf::gr::set_texture(-1, -1);
    constexpr auto flags = static_cast<rf::gr::TMapperFlags>(rf::gr::TMAP_FLAG_RGB | rf::gr::TMAP_FLAG_ALPHA);
    // quads between loop(s0) at alpha f0 and loop(s1) at alpha f1, s1 > s0
    auto band = [&](float s0, float f0, float s1, float f1) {
        for (int i = 0; i < n; ++i) {
            rf::gr::Vertex v[4]{};
            rf::gr::Vertex* vp[4] = {&v[0], &v[1], &v[2], &v[3]};
            auto set = [&](int idx, float s, int j, float f) {
                loop(s, j % n, v[idx].sx, v[idx].sy);
                v[idx].sx += rf::gr::screen.offset_x;
                v[idx].sy += rf::gr::screen.offset_y;
                v[idx].sw = 1.0f;
                v[idx].r = clr.red;
                v[idx].g = clr.green;
                v[idx].b = clr.blue;
                v[idx].a = static_cast<rf::ubyte>(clr.alpha * f + 0.5f);
            };
            set(0, s1, i, f1);
            set(1, s1, i + 1, f1);
            set(2, s0, i + 1, f0);
            set(3, s0, i, f0);
            rf::gr::tmapper(4, vp, flags, reticle_aa_mode);
        }
    };
    constexpr float h = 0.5f;
    float core_in = s_in;
    if (inner_edge) {
        band(std::max(0.0f, s_in - h), 0.0f, s_in + h, 1.0f);
        core_in = s_in + h;
    }
    if (s_out - h > core_in) {
        band(core_in, 1.0f, s_out - h, 1.0f);
    }
    band(s_out - h, 1.0f, s_out + h, 0.0f);
}

// Hollow regular n-gon (ring) centered at cx,cy (clip-relative, float) with inradius r and line thickness t.
// a0 = angle of first vertex.
static void hud_draw_ngon_ring(float cx, float cy, int n, float r, float t, float a0, rf::Color clr)
{
    constexpr float pi = 3.14159265f;
    const float k = 1.0f / std::cos(pi / n); // inradius -> circumradius
    auto loop = [=](float s, int i, float& x, float& y) {
        const float ang = a0 + 2 * pi * i / n;
        x = cx + s * k * std::cos(ang);
        y = cy + s * k * std::sin(ang);
    };
    const float ri = std::max(0.0f, r - t * 0.5f);
    reticle_draw_band(loop, n, ri, r + t * 0.5f, ri > 0.0f, clr);
}

// Filled axis-aligned rect: a band out from its center line.
static void hud_draw_reticle_rect(int x, int y, int w, int h, rf::Color clr)
{
    const float cx = x + w * 0.5f;
    const float cy = y + h * 0.5f;
    const float hw = w * 0.5f;
    const float hh = h * 0.5f;
    auto loop = [=](float s, int i, float& px, float& py) {
        // clockwise from the top left, like the ring
        px = cx + (i == 0 || i == 3 ? -1.0f : 1.0f) * (hw + s);
        py = cy + (i < 2 ? -1.0f : 1.0f) * (hh + s);
    };
    reticle_draw_band(loop, 4, -std::min(hw, hh), 0.0f, false, clr);
}

// Reticle tint, as the stock render path's set_color hooks apply it. A mod-supplied bitmap stays white unless the
// player asked to tint those too.
static rf::Color reticle_tint(const ReticleConfig& cfg, bool locked, bool custom_bitmap)
{
    const auto color_override = locked ? cfg.locked_color : cfg.color;
    if (custom_bitmap && !g_alpine_game_config.colorize_custom_reticles) {
        return {255, 255, 255, 255};
    }
    if (color_override) {
        return rf::Color::from_hex(*color_override);
    }
    if (custom_bitmap) {
        return {255, 255, 255, 255};
    }
    return locked ? rf::Color{255, 0, 0, 255} : rf::Color{0, 255, 0, 255};
}

// Vector reticle: any mix of dot, crosshair, square, circle, triangle centered on clip-relative cx, cy.
// Rects for dot/cross, n-gon rings for the outline shapes, all with anti-aliased edges.
static void hud_render_vector_reticle(const ReticleConfig& cfg, int cx, int cy, bool locked, float scale)
{
    constexpr float pi = 3.14159265f;
    const int style = cfg.style;
    auto px = [scale](int v) { return static_cast<int>(v * scale + 0.5f); };
    const int d = std::max(1, px(cfg.dot_size));
    const int l = std::max(1, px(cfg.cross_length));
    const int t = std::max(1, px(cfg.cross_thickness));
    const int g = px(cfg.cross_gap);
    const int o = px(cfg.outline);
    auto deg = [](int v) { return v * pi / 180.0f; };

    // shape = the style bit each primitive belongs to, used to pick which get an outline
    struct R { int shape, x, y, w, h; };
    R rects[5];
    int n = 0;
    if (style & 1) {
        rects[n++] = {1, cx - d / 2, cy - d / 2, d, d};
    }
    if (style & 2) {
        rects[n++] = {2, cx - g - l, cy - t / 2, l, t}; // left
        rects[n++] = {2, cx + g, cy - t / 2, l, t};     // right
        rects[n++] = {2, cx - t / 2, cy - g - l, t, l}; // top
        rects[n++] = {2, cx - t / 2, cy + g, t, l};     // bottom
    }

    // ring shapes: {shape, sides, inradius, line thickness, first vertex angle}
    struct Ring { int shape, sides; float r; float t; float a0; };
    Ring rings[3];
    int nr = 0;
    if (style & 4) {
        rings[nr++] = {4, 4, static_cast<float>(px(cfg.square_size)),
                       static_cast<float>(std::max(1, px(cfg.square_thickness))),
                       -0.75f * pi + deg(cfg.square_angle)};
    }
    if (style & 8) {
        const float r = static_cast<float>(px(cfg.circle_size));
        rings[nr++] = {8, std::clamp(static_cast<int>(r * 2), 24, 128), r,
                       static_cast<float>(std::max(1, px(cfg.circle_thickness))), 0.0f};
    }
    if (style & 16) {
        rings[nr++] = {16, 3, px(cfg.triangle_size) * 0.5f, // inradius = circumradius/2
                       static_cast<float>(std::max(1, px(cfg.triangle_thickness))),
                       -0.5f * pi + deg(cfg.triangle_angle)}; // 0 deg = tip up
    }

    // ring center on the pixel center of (cx, cy), matching the rect convention
    const float fcx = cx + 0.5f;
    const float fcy = cy + 0.5f;

    const rf::Color clr = reticle_tint(cfg, locked, false);

    if (o > 0) {
        const int om = cfg.outline_shapes;
        const rf::Color oclr = rf::Color::from_hex(cfg.outline_color);
        for (int i = 0; i < n; ++i) {
            if (rects[i].shape & om) {
                hud_draw_reticle_rect(rects[i].x - o, rects[i].y - o, rects[i].w + 2 * o, rects[i].h + 2 * o, oclr);
            }
        }
        for (int i = 0; i < nr; ++i) {
            if (rings[i].shape & om) {
                hud_draw_ngon_ring(fcx, fcy, rings[i].sides, rings[i].r, rings[i].t + 2 * o, rings[i].a0, oclr);
            }
        }
    }
    for (int i = 0; i < n; ++i) {
        hud_draw_reticle_rect(rects[i].x, rects[i].y, rects[i].w, rects[i].h, clr);
    }
    for (int i = 0; i < nr; ++i) {
        hud_draw_ngon_ring(fcx, fcy, rings[i].sides, rings[i].r, rings[i].t, rings[i].a0, clr);
    }
}

// The Machine Pistol Special's row under the main widget (0x0043B1D5).
constexpr int hud_second_ammo_slot_offset_y = 45;

// Unscaled pixels, like the ammo widget's own bitmaps.
constexpr int hud_refill_bar_width = 36;
constexpr int hud_refill_bar_height = 6;
constexpr int hud_refill_bar_gap = 6;
// The vehicle health bars' look (vehicle_markers.cpp).
constexpr float hud_refill_bar_back_alpha = 0.45f;
constexpr float hud_refill_bar_fill_alpha = 0.75f;

namespace
{
    // Per weapon slot; only the viewed hull's bars are ever drawn.
    HudVehicleBarFade g_hud_refill_bars[2];
} // namespace

static void hud_ammo_bitmap_size(int bmh, int& w, int& h);

static bool hud_ammo_is_driven_class(const rf::Entity* hull)
{
    const int vdc = vehicle_damage_class(hull);
    return vdc == VDC_APC || vdc == VDC_FIGHTER || vdc == VDC_SUB;
}

// The APC, fighter or sub this rider drives; the ammo widgets show its primary and any secondary.
static rf::Entity* hud_ammo_driven_hull(rf::Entity* rider)
{
    if (!rf::is_multi) {
        return nullptr;
    }
    rf::Entity* hull = vehicle_ridden_hull(rider);
    if (!hull || !hud_ammo_is_driven_class(hull) || vehicle_firing_seat_occupant(hull) != rider) {
        return nullptr;
    }
    return hull;
}

static bool hud_ammo_hull_has_secondary(const rf::Entity* hull)
{
    return vehicle_weapon_ammo(hull, hull->ai.current_secondary_weapon) >= 0;
}

// Left of the no-clip row, centred on its signal light; dy is the row's offset.
static void hud_render_vehicle_refill_bar(const rf::Entity* hull, int slot, int dy)
{
    float progress = 1.0f;
    const bool charging = vehicle_hud_ammo_refill_progress(hull, slot, progress);
    const float alpha = g_hud_refill_bars[slot].update(hull->handle, charging);
    if (alpha <= 0.0f) {
        return;
    }
    if (!charging) {
        progress = 1.0f;
    }

    int signal_w = 0;
    int signal_h = 0;
    hud_ammo_bitmap_size(rf::hud_ammo_signal_green_bmh, signal_w, signal_h);
    const int w = std::max(8, static_cast<int>(std::lround(hud_refill_bar_width * g_hud_ammo_scale)));
    const int h = std::max(3, static_cast<int>(std::lround(hud_refill_bar_height * g_hud_ammo_scale)));
    const int gap = static_cast<int>(std::lround(hud_refill_bar_gap * g_hud_ammo_scale));
    const int row_left = std::min({rf::hud_coords[rf::hud_ammo_bar_position_no_clip].x,
                                   rf::hud_coords[rf::hud_ammo_signal_position_no_clip].x,
                                   rf::hud_coords[rf::hud_ammo_icon_position_no_clip].x});
    const int x = std::max(0, row_left - gap - w);
    const int y = rf::hud_coords[rf::hud_ammo_signal_position_no_clip].y + dy + (signal_h - h) / 2;

    rf::ubyte r, g, b, a;
    hud_vehicle_bar_fill_color(progress, r, g, b, a);
    const int fill_w = std::clamp(static_cast<int>(std::lround(w * progress)), 0, w);
    if (fill_w > 0) {
        rf::gr::set_color(r, g, b, static_cast<rf::ubyte>(255.0f * hud_refill_bar_fill_alpha * alpha));
        rf::gr::rect(x, y, fill_w, h);
    }
    if (fill_w < w) {
        rf::gr::set_color(0, 0, 0, static_cast<rf::ubyte>(255.0f * hud_refill_bar_back_alpha * alpha));
        rf::gr::rect(x + fill_w, y, w - fill_w, h);
    }
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
        const ReticleConfig cfg = reticle_active_config();
        if (cfg.style) {
            hud_render_vector_reticle(cfg, rf::gr::clip_width() / 2 + static_cast<int>(aim_dx),
                                      rf::gr::clip_height() / 2 + static_cast<int>(aim_dy), g_reticle_locked,
                                      hud_reticle_scale());
            return;
        }
        float scale = hud_reticle_scale();
        int clip_w = rf::gr::clip_width();
        int clip_h = rf::gr::clip_height();

        x = static_cast<int>((x - clip_w / 2.0F) * scale + clip_w / 2.0F + aim_dx);
        y = static_cast<int>((y - clip_h / 2.0F) * scale + clip_h / 2.0F + aim_dy);

        hud_scaled_bitmap(bm_handle, x, y, scale, mode);
    },
};

int g_reticle_menu_slot = -1;

void hud_draw_reticle_preview(int weapon_slot, bool locked, int cx, int cy)
{
    const char* name = weapon_slot >= 0 && weapon_slot < rf::num_weapon_types
        ? static_cast<const char*>(rf::weapon_types[weapon_slot].name) : nullptr;
    const ReticleConfig cfg = name ? g_alpine_game_config.reticle_for_weapon(name) : g_alpine_game_config.default_reticle();
    if (cfg.style) {
        hud_render_vector_reticle(cfg, cx, cy, locked, hud_reticle_scale());
        return;
    }

    // Stock bitmap. The default shows reticle_0, the one most weapons share; borrow a weapon's handle to it (weapon
    // init loads them) rather than loading one the level code might free.
    int bmh = -1;
    bool custom = false;
    for (int i = 0; i < rf::num_weapon_types && bmh <= 0; ++i) {
        const int handle = rf::weapon_types[i].hud_reticle_bitmap;
        const char* file = handle > 0 ? rf::bm::get_filename(handle) : nullptr;
        if (file && string_iequals(file, "reticle_0.tga")) {
            bmh = rf::weapon_types[i].hud_reticle_bitmap;
            custom = weapon_reticle_is_customized(i, hud_reticle_scale() > 1.0f);
        }
    }
    const float scale = hud_reticle_scale();
    if (name) {
        const rf::WeaponInfo& info = rf::weapon_types[weapon_slot];
        const bool use_locked = locked && weapon_slot == rf::rocket_launcher_weapon_type && info.hud_locked_reticle_bitmap > 0;
        bmh = use_locked ? info.hud_locked_reticle_bitmap : info.hud_reticle_bitmap;
        custom = use_locked ? rocket_locked_reticle_is_customized(hud_reticle_scale() > 1.0f)
                            : weapon_reticle_is_customized(weapon_slot, hud_reticle_scale() > 1.0f);
    }
    if (bmh <= 0) {
        return;
    }
    int w = 0;
    int h = 0;
    rf::bm::get_dimensions(bmh, &w, &h);
    rf::gr::set_color(reticle_tint(cfg, locked, custom));
    hud_scaled_bitmap(bmh, cx - static_cast<int>(w * scale / 2.0f), cy - static_cast<int>(h * scale / 2.0f), scale);
}

CallHook<void(int, int, int, int)> render_reticle_set_color_hook{
    0x0043A4D7,
    [](int r, int g, int b, int a) {
        const rf::Color clr = reticle_tint(reticle_active_config(), false, g_displaying_custom_reticle);
        render_reticle_set_color_hook.call_target(clr.red, clr.green, clr.blue, clr.alpha);
    },
};

CallHook<void(int, int, int, int)> render_reticle_locked_set_color_hook{
    0x0043A472,
    [](int r, int g, int b, int a) {
        const rf::Color clr = reticle_tint(reticle_active_config(), true, g_displaying_custom_reticle);
        render_reticle_locked_set_color_hook.call_target(clr.red, clr.green, clr.blue, clr.alpha);
    },
};

CodeInjection render_reticle_check_custom_injection{
    0x0043A3B1,
    [](auto& regs) {
        int weap_slot = regs.eax;
        g_reticle_locked = false;
        g_reticle_weapon_slot = weap_slot;

        if (weap_slot >= 0) {
            bool big_reticle = hud_reticle_scale() > 1.0f; // reticle is using _1 variant
            g_displaying_custom_reticle = weapon_reticle_is_customized(weap_slot, big_reticle);

            // special case for rocket lock on reticle
            if (weap_slot == rf::rocket_launcher_weapon_type) {
                rf::Player* pp = regs.edi;
                if (rf::player_fpgun_locked_on(pp)) {
                    g_reticle_locked = true;
                    g_displaying_custom_reticle = rocket_locked_reticle_is_customized(big_reticle);
                }
            }
        }
        else {
            g_displaying_custom_reticle = false;
        }
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
        // Only the firing seat's paths hand this widget a hull: the jeep gunner's, a turret's, and ours.
        if (rf::is_multi && vehicle_damage_class(entity) >= 0) {
            hud_render_vehicle_refill_bar(entity, 0, 0);
        }
        if (rf::is_multi && hud_ammo_is_driven_class(entity)) {
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
                hud_render_vehicle_refill_bar(entity, 1, dy);
            }
            return;
        }
        hud_render_weapon_name_label(weapon_type);
    },
};

CallHook<bool(rf::Entity*)> hud_weapons_render_jeep_gunner_hook{
    {
        0x0043B0E6,
        0x0043B128,
    },
    [](rf::Entity* ep) {
        return hud_weapons_render_jeep_gunner_hook.call_target(ep) || hud_ammo_driven_hull(ep) != nullptr;
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
        rf::Entity* hull = pp ? hud_ammo_driven_hull(rf::entity_from_handle(pp->entity_handle)) : nullptr;
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

// ---- reticle console commands ----
// All ui_reticle_* / ui_color_reticle* commands edit one target: the default config, or one weapon's override.
static std::string g_reticle_edit_weapon; // canonical weapon class name, empty = default

ReticleConfig& reticle_edit_target()
{
    if (!g_reticle_edit_weapon.empty()) {
        auto it = g_alpine_game_config.weapon_reticles.find(g_reticle_edit_weapon);
        if (it != g_alpine_game_config.weapon_reticles.end()) {
            return it->second;
        }
        g_reticle_edit_weapon.clear(); // override was removed
    }
    return g_alpine_game_config.reticle;
}

std::optional<uint32_t>& reticle_edit_color(bool locked)
{
    ReticleConfig& cfg = reticle_edit_target();
    if (&cfg == &g_alpine_game_config.reticle) {
        return locked ? g_alpine_game_config.reticle_locked_color_override : g_alpine_game_config.reticle_color_override;
    }
    return locked ? cfg.locked_color : cfg.color;
}

bool reticle_editing_default()
{
    return &reticle_edit_target() == &g_alpine_game_config.reticle;
}

std::string reticle_edit_target_label()
{
    return g_reticle_edit_weapon.empty() ? "default" : "weapon: " + g_reticle_edit_weapon;
}

static const char* reticle_shape_names[] = {"dot", "cross", "square", "circle", "triangle"}; // bit i = 1 << i

static std::string reticle_style_to_string(int style)
{
    std::string s;
    for (int i = 0; i < 5; ++i) {
        if (style & (1 << i)) {
            s += (s.empty() ? "" : "+") + std::string(reticle_shape_names[i]);
        }
    }
    return s.empty() ? "default" : s;
}

// "dot+circle", "dot,cross", "cross circle" (quoted), "both", "all", "default", or a numeric bitmask
static std::optional<int> reticle_style_from_string(std::string s)
{
    s = string_to_lower(s);
    if (s == "default" || s == "off" || s == "none") {
        return 0;
    }
    if (s == "both") {
        return 3;
    }
    if (s == "all") {
        return 31;
    }
    int style = 0;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find_first_of("+, ", pos);
        if (end == std::string::npos) {
            end = s.size();
        }
        if (end > pos) {
            std::string tok = s.substr(pos, end - pos);
            int bit = -1;
            for (int i = 0; i < 5; ++i) {
                if (tok == reticle_shape_names[i]) {
                    bit = 1 << i;
                }
            }
            if (bit < 0) {
                if (tok.find_first_not_of("0123456789") != std::string::npos) {
                    return {};
                }
                bit = std::atoi(tok.c_str());
            }
            style |= bit;
        }
        pos = end + 1;
    }
    return style;
}

static const ReticleIntField& reticle_field(const char* cmd)
{
    for (const auto& f : reticle_int_fields) {
        if (std::strcmp(f.cmd, cmd) == 0) {
            return f;
        }
    }
    return reticle_int_fields[0];
}

static void handle_reticle_int_cmd(std::optional<int> v, const ReticleIntField& f)
{
    ReticleConfig& cfg = reticle_edit_target();
    if (v) {
        reticle_set_int(cfg, f, *v);
    }
    rf::console::print("{} is {} {} ({})", f.label, cfg.*f.member, f.wrap ? "degrees" : "px", reticle_edit_target_label());
}

// "current" = held weapon, otherwise a weapon class name (case-insensitive). Returns slot or -1.
static int reticle_resolve_weapon(const std::string& s)
{
    if (string_to_lower(s) == "current") {
        rf::Entity* entity = rf::local_player ? rf::entity_from_handle(rf::local_player->entity_handle) : nullptr;
        return entity ? entity->ai.current_primary_weapon : -1;
    }
    return rf::weapon_lookup_type(s.c_str());
}

static std::string reticle_weapon_name(int slot)
{
    const char* name = (slot >= 0 && slot < rf::num_weapon_types) ? rf::weapon_types[slot].name : nullptr;
    return name ? name : "";
}

ConsoleCommand2 reticle_style_cmd{
    "ui_reticle_style",
    [](std::optional<std::string> style_opt) {
        ReticleConfig& cfg = reticle_edit_target();
        if (style_opt) {
            auto style = reticle_style_from_string(*style_opt);
            if (!style) {
                rf::console::print("Invalid style. Combine dot, cross, square, circle, triangle with '+' (e.g. dot+circle), or use default.");
                return;
            }
            reticle_set_int(cfg, reticle_field("style"), *style);
        }
        rf::console::print("Reticle style is {} ({})", reticle_style_to_string(cfg.style), reticle_edit_target_label());
    },
    "Select reticle shapes: default (weapon bitmap) or any '+'-joined mix of dot, cross, square, circle, triangle. Color: ui_color_reticle / ui_color_reticle_locked. Scale: ui_scale_reticle. Per-weapon: ui_reticle_weapon.",
    "ui_reticle_style <default|dot|cross|square|circle|triangle|a+b+c>",
};

ConsoleCommand2 reticle_outline_shapes_cmd{
    "ui_reticle_outline_shapes",
    [](std::optional<std::string> shapes_opt) {
        ReticleConfig& cfg = reticle_edit_target();
        if (shapes_opt) {
            auto mask = reticle_style_from_string(*shapes_opt);
            if (!mask) {
                rf::console::print("Invalid input. Use all, none, or a '+'-joined mix of dot, cross, square, circle, triangle.");
                return;
            }
            reticle_set_int(cfg, reticle_field("outline_shapes"), *mask);
        }
        const int m = cfg.outline_shapes;
        rf::console::print("Reticle outline applies to {} ({})", m == 31 ? "all shapes" : m == 0 ? "no shapes" : reticle_style_to_string(m), reticle_edit_target_label());
    },
    "Select which reticle shapes get the outline (thickness via ui_reticle_outline, color via ui_color_reticle_outline).",
    "ui_reticle_outline_shapes <all|none|dot|cross|square|circle|triangle|a+b+c>",
};

ConsoleCommand2 reticle_weapon_cmd{
    "ui_reticle_weapon",
    [](std::optional<std::string> arg) {
        if (arg) {
            if (string_to_lower(*arg) == "default") {
                g_reticle_edit_weapon.clear();
            }
            else {
                const std::string name = reticle_weapon_name(reticle_resolve_weapon(*arg));
                if (name.empty()) {
                    rf::console::print("Unknown weapon '{}'. Use default, current, or a weapon class name (quote names with spaces).", *arg);
                    return;
                }
                if (!g_alpine_game_config.weapon_reticles.count(name)) {
                    g_alpine_game_config.weapon_reticles[name] = g_alpine_game_config.default_reticle();
                    rf::console::print("Created reticle override for {} (copied from default).", name);
                }
                g_reticle_edit_weapon = name;
            }
        }
        rf::console::print("Reticle commands now edit: {}", reticle_edit_target_label());
        if (!arg) {
            if (g_alpine_game_config.weapon_reticles.empty()) {
                rf::console::print("No per-weapon reticle overrides.");
            }
            for (const auto& [name, cfg] : g_alpine_game_config.weapon_reticles) {
                rf::console::print("  {}: {}", name, reticle_style_to_string(cfg.style));
            }
        }
    },
    "Select which reticle the ui_reticle_* and ui_color_reticle* commands edit: default, current (held weapon), or a weapon class name. Creates the per-weapon override if missing. No argument lists overrides.",
    "ui_reticle_weapon [default|current|<weapon name>]",
};

ConsoleCommand2 reticle_weapon_reset_cmd{
    "ui_reticle_weapon_reset",
    [](std::optional<std::string> arg) {
        const std::string name = arg ? reticle_weapon_name(reticle_resolve_weapon(*arg)) : g_reticle_edit_weapon;
        if (name.empty()) {
            rf::console::print("Specify a weapon, or select one first with ui_reticle_weapon.");
            return;
        }
        if (g_alpine_game_config.weapon_reticles.erase(name)) {
            rf::console::print("Removed reticle override for {}; it now uses the default reticle.", name);
        }
        else {
            rf::console::print("{} has no reticle override.", name);
        }
        if (g_reticle_edit_weapon == name) {
            g_reticle_edit_weapon.clear();
        }
    },
    "Remove a per-weapon reticle override (the selected weapon, or the named one) so that weapon uses the default reticle.",
    "ui_reticle_weapon_reset [current|<weapon name>]",
};

// Copy a reticle config between targets. Source: default, current, or a weapon (its effective config,
// so a weapon without an override copies the default). Destination: default, current, or a weapon (override created).
ConsoleCommand2 reticle_weapon_copy_cmd{
    "ui_reticle_weapon_copy",
    [](std::string from, std::string to) {
        auto resolve = [](const std::string& s, std::string& name) {
            if (string_to_lower(s) == "default") {
                return true; // name stays empty = default
            }
            name = reticle_weapon_name(reticle_resolve_weapon(s));
            if (name.empty()) {
                rf::console::print("Unknown weapon '{}'. Use default, current, or a weapon class name (quote names with spaces).", s);
                return false;
            }
            return true;
        };
        std::string from_name, to_name;
        if (!resolve(from, from_name) || !resolve(to, to_name)) {
            return;
        }
        const ReticleConfig src = g_alpine_game_config.reticle_for_weapon(from_name); // copy: dest may alias
        if (to_name.empty()) {
            g_alpine_game_config.set_default_reticle(src);
        }
        else {
            g_alpine_game_config.weapon_reticles[to_name] = src;
        }
        rf::console::print("Copied reticle from {} to {}.", from_name.empty() ? "default" : from_name, to_name.empty() ? "default" : to_name);
    },
    "Copy a reticle config: from/to are default, current (held weapon), or a weapon class name. Copying to a weapon creates its override.",
    "ui_reticle_weapon_copy <from> <to>",
};

void hud_weapons_apply_patches()
{
    // Big HUD support for ammo display
    hud_render_ammo_gr_bitmap_hook.install();
    hud_render_ammo_hook.install();
    hud_render_ammo_no_clip_hook.install();
    hud_weapons_render_jeep_gunner_hook.install();

    // reticle color, scale and vector reticle
    render_reticle_gr_bitmap_hook.install();
    render_reticle_set_color_hook.install();
    render_reticle_locked_set_color_hook.install();
    render_reticle_check_custom_injection.install();

    reticle_style_cmd.register_cmd();
    reticle_outline_shapes_cmd.register_cmd();
    reticle_weapon_cmd.register_cmd();
    reticle_weapon_reset_cmd.register_cmd();
    reticle_weapon_copy_cmd.register_cmd();
    // one ui_reticle_<field> <value> command per int field (style/outline_shapes have string forms above)
    for (const auto& f : reticle_int_fields) {
        if (std::strcmp(f.cmd, "style") == 0 || std::strcmp(f.cmd, "outline_shapes") == 0) {
            continue;
        }
        const char* unit = f.wrap ? "degrees" : "px";
        auto* name = new std::string("ui_reticle_" + std::string(f.cmd));
        auto* desc = new std::string(std::string(f.label) + " in " + unit + " (edits the target chosen with ui_reticle_weapon).");
        auto* usage = new std::string(*name + " <" + unit + ">");
        auto* cmd = new ConsoleCommand2{
            name->c_str(),
            [&f](std::optional<int> v) { handle_reticle_int_cmd(v, f); },
            desc->c_str(),
            usage->c_str(),
        };
        cmd->register_cmd();
    }

    // Disable weapon cycle selection, if `Mouse 3` is pressed.
    player_select_next_primary_hook.install();
    player_select_prev_primary_hook.install();
}
